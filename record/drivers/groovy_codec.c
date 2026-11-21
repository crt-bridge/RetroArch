/* SPDX-License-Identifier: GPL-3.0-or-later */
/* groovy_codec.c -- see groovy_codec.h. */
#include "groovy_codec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gm.h"

/* ---- the little that differs between Windows and POSIX: a lock, a wake-up event, a thread, a flag ---- */
#ifdef _WIN32
#  include <windows.h>
typedef volatile LONG cx_flag;
static void cx_flag_set(cx_flag *f) { InterlockedExchange(f, 1); }
typedef struct { CRITICAL_SECTION cs; HANDLE ev, th; } cx_sync;
static int  cx_init(cx_sync *s) { InitializeCriticalSection(&s->cs); s->ev = CreateEventA(NULL, FALSE, FALSE, NULL); s->th = NULL; if (s->ev) return 0; DeleteCriticalSection(&s->cs); return -1; }
static void cx_lock(cx_sync *s) { EnterCriticalSection(&s->cs); }
static void cx_unlock(cx_sync *s) { LeaveCriticalSection(&s->cs); }
static void cx_signal(cx_sync *s) { SetEvent(s->ev); }
static void cx_wait(cx_sync *s, unsigned ms) { WaitForSingleObject(s->ev, ms); }
typedef struct { void *(*fn)(void *); void *arg; } cx_boot;
static DWORD WINAPI cx_entry(LPVOID p) { cx_boot b = *(cx_boot *)p; free(p); b.fn(b.arg); return 0; }
static int  cx_spawn(cx_sync *s, void *(*fn)(void *), void *arg)
{
    cx_boot *b = (cx_boot *)malloc(sizeof *b);
    if (!b) return -1;
    b->fn = fn; b->arg = arg;
    s->th = CreateThread(NULL, 0, cx_entry, b, 0, NULL);
    if (!s->th) { free(b); return -1; }
    return 0;
}
static void cx_join(cx_sync *s) { WaitForSingleObject(s->th, 5000); CloseHandle(s->th); }
static void cx_destroy(cx_sync *s) { CloseHandle(s->ev); DeleteCriticalSection(&s->cs); }
#else
#  include <pthread.h>
#  include <errno.h>
#  include <time.h>
typedef volatile int cx_flag;
static void cx_flag_set(cx_flag *f) { __atomic_store_n(f, 1, __ATOMIC_SEQ_CST); }
typedef struct { pthread_mutex_t mu; pthread_cond_t cv; int signaled; pthread_t th; } cx_sync;
static int  cx_init(cx_sync *s)
{
    s->signaled = 0;
    if (pthread_mutex_init(&s->mu, NULL) != 0) return -1;
    if (pthread_cond_init(&s->cv, NULL) != 0) { pthread_mutex_destroy(&s->mu); return -1; }
    return 0;
}
static void cx_lock(cx_sync *s) { pthread_mutex_lock(&s->mu); }
static void cx_unlock(cx_sync *s) { pthread_mutex_unlock(&s->mu); }
static void cx_signal(cx_sync *s) { pthread_mutex_lock(&s->mu); s->signaled = 1; pthread_cond_signal(&s->cv); pthread_mutex_unlock(&s->mu); }
static void cx_wait(cx_sync *s, unsigned ms)   /* auto-reset, like the Windows event */
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_sec += (time_t)(ms / 1000u); t.tv_nsec += (long)(ms % 1000u) * 1000000L;
    if (t.tv_nsec >= 1000000000L) { t.tv_sec++; t.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&s->mu);
    while (!s->signaled && pthread_cond_timedwait(&s->cv, &s->mu, &t) != ETIMEDOUT) { }
    s->signaled = 0;
    pthread_mutex_unlock(&s->mu);
}
static int  cx_spawn(cx_sync *s, void *(*fn)(void *), void *arg) { return pthread_create(&s->th, NULL, fn, arg) == 0 ? 0 : -1; }
static void cx_join(cx_sync *s) { pthread_join(s->th, NULL); }
static void cx_destroy(cx_sync *s) { pthread_cond_destroy(&s->cv); pthread_mutex_destroy(&s->mu); }
#endif

struct groovy_codec {
    gm_handle        *gm;
    unsigned          kbps, fps_num, fps_den;
    cx_sync           sync;
    cx_flag           stop;
    cx_flag           failed;
    const char       *why;
    /* mailbox: the latest picture only */
    uint8_t          *box;
    size_t            box_cap;
    unsigned          box_w, box_h;
    uint32_t          box_id;
    uint16_t          box_vsync;
    int               box_key, box_full;
    groovy_codec_stats st;
};

/* One encoder, whichever: NVENC (NVIDIA, driver; Windows) or libavcodec/libx264 (anywhere FFmpeg is). */
typedef struct { groovy_nvenc *nv; groovy_ffenc *ff; } codec_enc;

static const char *g_backend = "none";

/* GROOVY_ENCODER: 0 = auto (NVENC first, then libx264), 1 = nvenc only, 2 = libx264 only; -1 = unknown word. */
static int codec_encoder_choice(void)
{
    const char *s = getenv("GROOVY_ENCODER");
    if (!s || !*s || !strcmp(s, "auto")) return 0;
    if (!strcmp(s, "nvenc")) return 1;
    if (!strcmp(s, "ff")) return 2;
    return -1;
}

static int codec_enc_open(codec_enc *e, const groovy_nvenc_cfg *cfg, const char **why)
{
    const int ch = codec_encoder_choice();
    const char *w1 = NULL, *w2 = NULL;
    static char both[240];
    e->nv = NULL; e->ff = NULL;
    if (ch < 0) { *why = "GROOVY_ENCODER must be auto, nvenc or ff"; return -1; }
    if (ch != 2) e->nv = groovy_nvenc_open(cfg, &w1);
    if (e->nv) { g_backend = "NVENC"; return 0; }
    if (ch != 1) e->ff = groovy_ffenc_open(cfg, &w2);
    if (e->ff) { g_backend = "libx264"; return 0; }
    if (ch == 1) snprintf(both, sizeof both, "NVENC: %s", w1 ? w1 : "unavailable");
    else if (ch == 2) snprintf(both, sizeof both, "libx264: %s", w2 ? w2 : "unavailable");
    else snprintf(both, sizeof both, "NVENC: %s; libx264: %s", w1 ? w1 : "unavailable", w2 ? w2 : "unavailable");
    *why = both;
    return -1;
}

static void codec_enc_close(codec_enc *e)
{
    if (e->nv) groovy_nvenc_close(e->nv);
    if (e->ff) groovy_ffenc_close(e->ff);
    e->nv = NULL; e->ff = NULL;
}

static int codec_enc_encode(codec_enc *e, const uint8_t *bgr, int key, const uint8_t **out, size_t *len, int *is_key, double *us)
{
    if (e->nv) return groovy_nvenc_encode(e->nv, bgr, key, out, len, is_key, us);
    return groovy_ffenc_encode(e->ff, bgr, key, out, len, is_key, us);
}

const char *groovy_codec_backend(void) { return g_backend; }

int groovy_codec_available(const char **why)
{
    groovy_nvenc_cfg cfg;
    codec_enc e;
    const char *w = NULL;
    groovy_nvenc_cfg_defaults(&cfg, 320u, 240u);   /* NVENC refuses H.264 4:4:4 at 128x128: ask for a size that exists */
    if (codec_enc_open(&e, &cfg, &w) != 0) { if (why) *why = w ? w : "no encoder"; g_backend = "none"; return 0; }
    codec_enc_close(&e);
    return 1;
}

static void *codec_thread(void *arg)
{
    groovy_codec *c = (groovy_codec *)arg;
    codec_enc enc = { NULL, NULL };
    int enc_open = 0;
    unsigned ew = 0u, eh = 0u;
    uint8_t *pic = NULL;
    size_t pic_cap = 0u;

    while (!c->stop) {
        unsigned w, h;
        uint32_t id;
        uint16_t vs;
        int key;
        cx_wait(&c->sync, 100u);
        if (c->stop) break;
        cx_lock(&c->sync);
        if (!c->box_full) { cx_unlock(&c->sync); continue; }
        w = c->box_w; h = c->box_h; id = c->box_id; vs = c->box_vsync; key = c->box_key;
        if ((size_t)w * h * 3u > pic_cap) {
            uint8_t *g = (uint8_t *)realloc(pic, (size_t)w * h * 3u);
            if (!g) { c->box_full = 0; cx_unlock(&c->sync); cx_flag_set(&c->failed); c->why = "out of memory"; break; }
            pic = g; pic_cap = (size_t)w * h * 3u;
        }
        memcpy(pic, c->box, (size_t)w * h * 3u);
        c->box_full = 0;
        c->box_key = 0;
        cx_unlock(&c->sync);

        if (enc_open && (w != ew || h != eh)) { codec_enc_close(&enc); enc_open = 0; key = 1; }   /* a new size is a new session */
        if (!enc_open) {
            groovy_nvenc_cfg cfg;
            const char *why = NULL;
            groovy_nvenc_cfg_defaults(&cfg, w, h);
            cfg.fps_num = c->fps_num; cfg.fps_den = c->fps_den; cfg.bitrate_kbps = c->kbps;
            if (codec_enc_open(&enc, &cfg, &why) != 0) { c->why = why ? why : "encoder refused"; cx_flag_set(&c->failed); break; }
            enc_open = 1;
            ew = w; eh = h; key = 1;
        }
        {
            const uint8_t *au = NULL;
            size_t len = 0u;
            int is_key = 0;
            double us = 0.0;
            if (codec_enc_encode(&enc, pic, key, &au, &len, &is_key, &us) != 0) {
                c->st.errors++;
                c->why = "encoder error";
                cx_flag_set(&c->failed);
                break;
            }
            c->st.last_us = us;
            if (us > c->st.worst_us) c->st.worst_us = us;
            if (gm_send_codec_frame(c->gm, id, vs, au, len, is_key) == 0) c->st.encoded++;
            else c->st.errors++;
        }
    }
    if (enc_open) codec_enc_close(&enc);
    free(pic);
    return NULL;
}

groovy_codec *groovy_codec_start(void *gm, unsigned kbps, unsigned fps_num, unsigned fps_den)
{
    groovy_codec *c;
    if (!gm) return NULL;
    c = (groovy_codec *)calloc(1u, sizeof *c);
    if (!c) return NULL;
    c->gm = (gm_handle *)gm;
    c->kbps = kbps; c->fps_num = fps_num ? fps_num : 60u; c->fps_den = fps_den ? fps_den : 1u;
    if (cx_init(&c->sync) != 0) { free(c); return NULL; }
    if (cx_spawn(&c->sync, codec_thread, c) != 0) {
        cx_destroy(&c->sync);
        free(c);
        return NULL;
    }
    return c;
}

void groovy_codec_stop(groovy_codec *c)
{
    if (!c) return;
    cx_flag_set(&c->stop);
    cx_signal(&c->sync);
    cx_join(&c->sync);
    cx_destroy(&c->sync);
    free(c->box);
    free(c);
}

int groovy_codec_submit(groovy_codec *c, const uint8_t *bgr888, unsigned w, unsigned h,
                        uint32_t frame_id, uint16_t vsync, int force_key)
{
    const size_t n = (size_t)w * h * 3u;
    if (!c || !bgr888 || w == 0u || h == 0u || c->failed) return -1;
    cx_lock(&c->sync);
    if (n > c->box_cap) {
        uint8_t *g = (uint8_t *)realloc(c->box, n);
        if (!g) { cx_unlock(&c->sync); return -1; }
        c->box = g; c->box_cap = n;
    }
    memcpy(c->box, bgr888, n);
    if (c->box_full) c->st.overwritten++;
    c->box_w = w; c->box_h = h; c->box_id = frame_id; c->box_vsync = vsync;
    c->box_key = c->box_key || force_key;
    c->box_full = 1;
    c->st.submitted++;
    cx_unlock(&c->sync);
    cx_signal(&c->sync);
    return 0;
}

int groovy_codec_failed(const groovy_codec *c, const char **why)
{
    if (!c) return 1;
    if (c->failed && why) *why = c->why ? c->why : "failed";
    return c->failed ? 1 : 0;
}

void groovy_codec_get_stats(const groovy_codec *c, groovy_codec_stats *out)
{
    if (c && out) *out = c->st;
}
