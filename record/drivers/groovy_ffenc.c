/* SPDX-License-Identifier: GPL-3.0-or-later */
/* groovy_ffenc.c -- see groovy_ffenc.h. libavcodec loaded at run time, prototypes by hand.
 *
 * Everything is set through av_opt_set (names, never struct offsets), except three places where the
 * public API has no function and the layout is read directly: AVFrame width/height/format (as in the
 * decoder), AVFrame pts, and AVPacket data/size. Each one is checked at run time (the default pts must
 * read back as "unset"; the first packet must start with an Annex B start code), so a layout that
 * moved refuses to open instead of encoding garbage. */
#include "groovy_ffenc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <errno.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <dlfcn.h>
#  include <time.h>
#endif

/* The libraries accepted: libavcodec major, libavutil major (FFmpeg 7.1, FFmpeg 8). */
static const struct { int codec, util; } ffenc_ok[] = { { 63, 61 }, { 61, 59 } };

typedef struct {   /* the prefix of AVFrame shared by FFmpeg 7 and 8, 64-bit */
    uint8_t *data[8];
    int      linesize[8];
    uint8_t **extended_data;
    int      width, height;
    int      nb_samples;
    int      format;
} ffenc_frame;
typedef struct {   /* the prefix of AVPacket */
    void    *buf;
    int64_t  pts, dts;
    uint8_t *data;
    int      size;
} ffenc_packet;
#if UINTPTR_MAX > 0xffffffffu
_Static_assert(offsetof(ffenc_frame, width) == 104 && offsetof(ffenc_frame, format) == 116, "AVFrame prefix layout");
_Static_assert(offsetof(ffenc_packet, data) == 24 && offsetof(ffenc_packet, size) == 32, "AVPacket prefix layout");
#endif
#define FFENC_PTS_OFFSET 136   /* AVFrame.pts: the same in FFmpeg 7 and 8 */
#define FFENC_OPT_CHILDREN 1   /* AV_OPT_SEARCH_CHILDREN: the private options of libx264 */

static struct {
    void *hcodec, *hutil;
    unsigned (*avcodec_version)(void);
    unsigned (*avutil_version)(void);
    const void *(*find_encoder_by_name)(const char *);
    void *(*alloc_context3)(const void *);
    void (*free_context)(void **);
    int  (*open2)(void *, const void *, void **);
    int  (*send_frame)(void *, const void *);
    int  (*receive_packet)(void *, void *);
    void *(*packet_alloc)(void);
    void (*packet_free)(void **);
    void (*packet_unref)(void *);
    void *(*frame_alloc)(void);
    void (*frame_free)(void **);
    int  (*frame_get_buffer)(void *, int);
    int  (*frame_make_writable)(void *);
    int  (*opt_set)(void *, const char *, const char *, int);
    int  (*get_pix_fmt)(const char *);
    void (*log_set_level)(int);
} ff;
static int  ff_state;          /* 0 not probed, 1 loaded, -1 failed */
static char ff_err[160];

#ifdef _WIN32
static void *ff_dlopen(const char *dir, const char *name)
{
    wchar_t wname[96], wdir[1024], joined[1100], full[1100];
    size_t i;
    for (i = 0; name[i] && i < 95; i++) wname[i] = (wchar_t)(unsigned char)name[i];
    wname[i] = 0;
    if (!dir) return (void *)LoadLibraryExW(wname, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (MultiByteToWideChar(CP_UTF8, 0, dir, -1, wdir, 1024) <= 0) return NULL;
    if (_snwprintf(joined, 1100, L"%ls\\%ls", wdir, wname) < 0) return NULL;
    if (!GetFullPathNameW(joined, 1100, full, NULL)) return NULL;
    return (void *)LoadLibraryExW(full, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
}
static void *ff_dlsym(void *h, const char *n) { return (void *)GetProcAddress((HMODULE)h, n); }
static void  ff_dlclose(void *h) { if (h) FreeLibrary((HMODULE)h); }
#  define FF_NAME_CODEC "avcodec-%d.dll"
#  define FF_NAME_UTIL  "avutil-%d.dll"
static double ff_now_us(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1e6 / (double)f.QuadPart;
}
#else
static void *ff_dlopen(const char *dir, const char *name)
{
    char path[1100], real[4096];
    if (!dir) return dlopen(name, RTLD_NOW | RTLD_LOCAL);
    if (snprintf(path, sizeof path, "%s/%s", dir, name) >= (int)sizeof path) return NULL;
    if (!realpath(path, real)) return NULL;
    return dlopen(real, RTLD_NOW | RTLD_LOCAL);
}
static void *ff_dlsym(void *h, const char *n) { return dlsym(h, n); }
static void  ff_dlclose(void *h) { if (h) dlclose(h); }
#  ifdef __APPLE__
#    define FF_NAME_CODEC "libavcodec.%d.dylib"
#    define FF_NAME_UTIL  "libavutil.%d.dylib"
#  else
#    define FF_NAME_CODEC "libavcodec.so.%d"
#    define FF_NAME_UTIL  "libavutil.so.%d"
#  endif
static double ff_now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e6 + (double)t.tv_nsec / 1e3;
}
#endif

static void ff_fail(const char *what, const char *detail)
{
    snprintf(ff_err, sizeof ff_err, "%s%s%s", what, detail && *detail ? ": " : "", detail ? detail : "");
}

#define FF_SYM(h, field, sym) do { \
        *(void **)&ff.field = ff_dlsym((h), sym); \
        if (!ff.field) { ff_fail("missing symbol", sym); return -1; } \
    } while (0)

static int ff_bind(int codec_major, int util_major)
{
    void *hc = ff.hcodec, *hu = ff.hutil;
    FF_SYM(hc, avcodec_version, "avcodec_version");
    FF_SYM(hu, avutil_version, "avutil_version");
    if ((int)(ff.avcodec_version() >> 16) != codec_major || (int)(ff.avutil_version() >> 16) != util_major) {
        char d[64];
        snprintf(d, sizeof d, "avcodec %u, avutil %u", ff.avcodec_version() >> 16, ff.avutil_version() >> 16);
        ff_fail("library major not accepted", d);
        return -1;
    }
    FF_SYM(hc, find_encoder_by_name, "avcodec_find_encoder_by_name");
    FF_SYM(hc, alloc_context3, "avcodec_alloc_context3");
    FF_SYM(hc, free_context, "avcodec_free_context");
    FF_SYM(hc, open2, "avcodec_open2");
    FF_SYM(hc, send_frame, "avcodec_send_frame");
    FF_SYM(hc, receive_packet, "avcodec_receive_packet");
    FF_SYM(hc, packet_alloc, "av_packet_alloc");
    FF_SYM(hc, packet_free, "av_packet_free");
    FF_SYM(hc, packet_unref, "av_packet_unref");
    FF_SYM(hu, frame_alloc, "av_frame_alloc");
    FF_SYM(hu, frame_free, "av_frame_free");
    FF_SYM(hu, frame_get_buffer, "av_frame_get_buffer");
    FF_SYM(hu, frame_make_writable, "av_frame_make_writable");
    FF_SYM(hu, opt_set, "av_opt_set");
    FF_SYM(hu, get_pix_fmt, "av_get_pix_fmt");
    FF_SYM(hu, log_set_level, "av_log_set_level");
    ff.log_set_level(-8);
    return 0;
}

static int ff_try_source(const char *dir)
{
    size_t k;
    for (k = 0; k < sizeof ffenc_ok / sizeof ffenc_ok[0]; k++) {
        char nc[64], nu[64];
        snprintf(nc, sizeof nc, FF_NAME_CODEC, ffenc_ok[k].codec);
        snprintf(nu, sizeof nu, FF_NAME_UTIL, ffenc_ok[k].util);
        memset(&ff, 0, sizeof ff);
        ff.hutil = ff_dlopen(dir, nu);
        if (!ff.hutil) { ff_fail("library not found", nu); continue; }
        ff.hcodec = ff_dlopen(dir, nc);
        if (!ff.hcodec) { ff_fail("library not found", nc); ff_dlclose(ff.hutil); continue; }
        if (ff_bind(ffenc_ok[k].codec, ffenc_ok[k].util) == 0) return 1;
        ff_dlclose(ff.hcodec); ff_dlclose(ff.hutil);
    }
    memset(&ff, 0, sizeof ff);
    return 0;
}

static int ff_load(void)
{
    const char *dir;
    if (ff_state) return ff_state > 0;
    ff_err[0] = 0;
    dir = getenv("GROOVY_FFMPEG_DIR");
    if (dir && *dir && ff_try_source(dir)) { ff_state = 1; return 1; }
    if (ff_try_source(NULL)) { ff_state = 1; ff_err[0] = 0; return 1; }
#ifdef __APPLE__
    {
        static const char *const brew[2] = { "/opt/homebrew/lib", "/usr/local/lib" };
        unsigned b;
        for (b = 0; b < 2; b++) if (ff_try_source(brew[b])) { ff_state = 1; ff_err[0] = 0; return 1; }
    }
#endif
    ff_state = -1;
    return 0;
}

struct groovy_ffenc {
    groovy_nvenc_cfg cfg;
    void  *ctx, *frame, *pkt;
    int64_t pts;
    uint8_t *out;          /* the last access unit, copied: valid until the next call */
    size_t   out_cap;
    int      packet_checked;
};

static int ff_set(void *ctx, const char *k, const char *v)
{
    return ff.opt_set(ctx, k, v, FFENC_OPT_CHILDREN);
}

/* Builds the codec context and the frame for the configured size. 0, or -1 with ff_err set. */
static int ffenc_start(groovy_ffenc *e)
{
    char v[96];
    int fmt;
    const void *codec = ff.find_encoder_by_name("libx264");
    const char *preset = getenv("GROOVY_FFENC_PRESET");
    const char *threads = getenv("GROOVY_FFENC_THREADS");
    const unsigned fn = e->cfg.fps_num ? e->cfg.fps_num : 60u, fd = e->cfg.fps_den ? e->cfg.fps_den : 1u;
    ffenc_frame *f;
    if (!codec) { ff_fail("this FFmpeg has no libx264", ""); return -1; }
    e->ctx = ff.alloc_context3(codec);
    e->frame = ff.frame_alloc();
    e->pkt = ff.packet_alloc();
    if (!e->ctx || !e->frame || !e->pkt) { ff_fail("out of memory", ""); return -1; }
    fmt = ff.get_pix_fmt("yuv444p");
    if (fmt < 0) { ff_fail("unknown pixel format", "yuv444p"); return -1; }
    snprintf(v, sizeof v, "%ux%u", e->cfg.width, e->cfg.height);
    if (ff_set(e->ctx, "video_size", v) < 0) { ff_fail("option refused", "video_size"); return -1; }
    if (ff_set(e->ctx, "pix_fmt", "yuv444p") < 0 && ff_set(e->ctx, "pixel_format", "yuv444p") < 0) { ff_fail("option refused", "pix_fmt"); return -1; }
    snprintf(v, sizeof v, "%u/%u", fd, fn);
    if (ff_set(e->ctx, "time_base", v) < 0) { ff_fail("option refused", "time_base"); return -1; }
    snprintf(v, sizeof v, "%u", e->cfg.bitrate_kbps * 1000u);
    if (ff_set(e->ctx, "b", v) < 0 || ff_set(e->ctx, "maxrate", v) < 0) { ff_fail("option refused", "bitrate"); return -1; }
    snprintf(v, sizeof v, "%u", (unsigned)((unsigned long long)e->cfg.bitrate_kbps * 1000ull * fd / fn));   /* one picture of VBV, like NVENC */
    if (ff_set(e->ctx, "bufsize", v) < 0) { ff_fail("option refused", "bufsize"); return -1; }
    if (ff_set(e->ctx, "bf", "0") < 0 || ff_set(e->ctx, "g", "2000000000") < 0) { ff_fail("option refused", "gop"); return -1; }
    ff_set(e->ctx, "colorspace", "bt470bg");   /* BT.601, limited range: what the decoder assumes without a label */
    ff_set(e->ctx, "color_range", "tv");
    if (ff_set(e->ctx, "threads", threads && *threads ? threads : "2") < 0) { ff_fail("option refused", "threads"); return -1; }
    if (ff_set(e->ctx, "preset", preset && *preset ? preset : "veryfast") < 0) { ff_fail("option refused", "preset"); return -1; }
    ff_set(e->ctx, "tune", "zerolatency");
    ff_set(e->ctx, "profile", "high444");
    ff_set(e->ctx, "x264-params", "keyint=infinite:min-keyint=1:scenecut=0:repeat-headers=1:aud=0");
    if (ff.open2(e->ctx, codec, NULL) < 0) { ff_fail("libx264 refused the configuration", v); return -1; }
    f = (ffenc_frame *)e->frame;
    f->width = (int)e->cfg.width; f->height = (int)e->cfg.height; f->format = fmt;
    if (ff.frame_get_buffer(e->frame, 0) < 0) { ff_fail("cannot allocate the picture", ""); return -1; }
    if (*(int64_t *)((unsigned char *)e->frame + FFENC_PTS_OFFSET) != INT64_MIN) { ff_fail("AVFrame layout not recognised", "pts"); return -1; }
    e->pts = 0;
    e->packet_checked = 0;
    return 0;
}

static void ffenc_stop(groovy_ffenc *e)
{
    if (e->pkt) ff.packet_free(&e->pkt);
    if (e->frame) ff.frame_free(&e->frame);
    if (e->ctx) ff.free_context(&e->ctx);
    e->ctx = e->frame = e->pkt = NULL;
}

groovy_ffenc *groovy_ffenc_open(const groovy_nvenc_cfg *cfg, const char **why)
{
#if !(UINTPTR_MAX > 0xffffffffu)
    /* The hand-written AVFrame/AVPacket prefixes are checked for 64-bit builds only. */
    if (why) *why = "the libx264 path is built for 64-bit only";
    return NULL;
#endif
    groovy_ffenc *e;
    if (why) *why = "";
    if (!cfg || cfg->width < 16u || cfg->height < 16u || cfg->width > 4096u || cfg->height > 2048u) { if (why) *why = "bad picture size"; return NULL; }
    if (!ff_load()) { if (why) *why = ff_err; return NULL; }
    e = (groovy_ffenc *)calloc(1u, sizeof *e);
    if (!e) { if (why) *why = "out of memory"; return NULL; }
    e->cfg = *cfg;
    if (ffenc_start(e) != 0) {
        if (why) *why = ff_err;
        ffenc_stop(e);
        free(e);
        return NULL;
    }
    return e;
}

void groovy_ffenc_close(groovy_ffenc *e)
{
    if (!e) return;
    ffenc_stop(e);
    free(e->out);
    free(e);
}

static int ffenc_has_idr(const uint8_t *p, size_t n)
{
    size_t i;
    for (i = 0; i + 3 < n; i++)
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1 && (p[i + 3] & 31u) == 5u) return 1;
    return 0;
}

/* BGR888 to planar YUV 4:4:4, BT.601 limited range (integer, rounded). */
static void ffenc_convert(const ffenc_frame *f, const uint8_t *bgr, unsigned w, unsigned h)
{
    unsigned x, y;
    for (y = 0; y < h; y++) {
        uint8_t *py = f->data[0] + (size_t)y * (size_t)f->linesize[0];
        uint8_t *pu = f->data[1] + (size_t)y * (size_t)f->linesize[1];
        uint8_t *pv = f->data[2] + (size_t)y * (size_t)f->linesize[2];
        const uint8_t *s = bgr + (size_t)y * w * 3u;
        for (x = 0; x < w; x++, s += 3) {
            const int b = s[0], g = s[1], r = s[2];
            py[x] = (uint8_t)(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
            pu[x] = (uint8_t)(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
            pv[x] = (uint8_t)(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
        }
    }
}

int groovy_ffenc_encode(groovy_ffenc *e, const uint8_t *bgr888, int force_key,
                        const uint8_t **out, size_t *len, int *is_key, double *us)
{
    const double t0 = ff_now_us();
    ffenc_packet *pk;
    int r;
    if (!e || !bgr888 || !out || !len) return -1;
    if (force_key && e->pts > 0) {   /* a new encoder: its first picture is an IDR with its parameter sets */
        ffenc_stop(e);
        if (ffenc_start(e) != 0) return -1;
    }
    if (ff.frame_make_writable(e->frame) < 0) return -1;
    ffenc_convert((const ffenc_frame *)e->frame, bgr888, e->cfg.width, e->cfg.height);
    *(int64_t *)((unsigned char *)e->frame + FFENC_PTS_OFFSET) = e->pts++;
    if (ff.send_frame(e->ctx, e->frame) < 0) return -1;
    ff.packet_unref(e->pkt);
    r = ff.receive_packet(e->ctx, e->pkt);
    if (r < 0) return -1;   /* zerolatency: one picture in, one unit out; anything else is an error here */
    pk = (ffenc_packet *)e->pkt;
    if (!pk->data || pk->size <= 4) return -1;
    if (!e->packet_checked) {
        if (!(pk->data[0] == 0 && pk->data[1] == 0 && (pk->data[2] == 1 || (pk->data[2] == 0 && pk->data[3] == 1)))) return -1;   /* AVPacket layout, or not Annex B */
        e->packet_checked = 1;
    }
    if ((size_t)pk->size > e->out_cap) {
        uint8_t *g = (uint8_t *)realloc(e->out, (size_t)pk->size);
        if (!g) return -1;
        e->out = g; e->out_cap = (size_t)pk->size;
    }
    memcpy(e->out, pk->data, (size_t)pk->size);
    *out = e->out; *len = (size_t)pk->size;
    if (is_key) *is_key = ffenc_has_idr(e->out, *len);
    if (us) *us = ff_now_us() - t0;
    return 0;
}
