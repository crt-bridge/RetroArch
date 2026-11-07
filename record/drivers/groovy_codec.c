/* SPDX-License-Identifier: GPL-3.0-or-later */
/* groovy_codec.c -- see groovy_codec.h. */
#include "groovy_codec.h"

#include <stdlib.h>

#include "gm.h"

#ifndef _WIN32

int groovy_codec_available(const char **why) { if (why) *why = "NVENC is only wired on Windows"; return 0; }
groovy_codec *groovy_codec_start(void *gm, unsigned kbps, unsigned n, unsigned d) { (void)gm; (void)kbps; (void)n; (void)d; return NULL; }
void groovy_codec_stop(groovy_codec *c) { (void)c; }
int groovy_codec_submit(groovy_codec *c, const uint8_t *p, unsigned w, unsigned h, uint32_t f, uint16_t v, int k)
{ (void)c; (void)p; (void)w; (void)h; (void)f; (void)v; (void)k; return -1; }
int groovy_codec_failed(const groovy_codec *c, const char **why) { (void)c; if (why) *why = "unavailable"; return 1; }
void groovy_codec_get_stats(const groovy_codec *c, groovy_codec_stats *out) { (void)c; if (out) memset(out, 0, sizeof *out); }

#else /* _WIN32 */

#include <windows.h>

struct groovy_codec {
    gm_handle        *gm;
    unsigned          kbps, fps_num, fps_den;
    HANDLE            thread, wake;
    CRITICAL_SECTION  lock;
    volatile LONG     stop;
    volatile LONG     failed;
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

int groovy_codec_available(const char **why)
{
    groovy_nvenc_cfg cfg;
    groovy_nvenc *e;
    const char *w = NULL;
    groovy_nvenc_cfg_defaults(&cfg, 320u, 240u);   /* NVENC refuses H.264 4:4:4 at 128x128: ask for a size that exists */
    e = groovy_nvenc_open(&cfg, &w);
    if (!e) { if (why) *why = w ? w : "NVENC unavailable"; return 0; }
    groovy_nvenc_close(e);
    return 1;
}

static DWORD WINAPI codec_thread(LPVOID arg)
{
    groovy_codec *c = (groovy_codec *)arg;
    groovy_nvenc *enc = NULL;
    unsigned ew = 0u, eh = 0u;
    uint8_t *pic = NULL;
    size_t pic_cap = 0u;

    while (!c->stop) {
        unsigned w, h;
        uint32_t id;
        uint16_t vs;
        int key;
        WaitForSingleObject(c->wake, 100);
        if (c->stop) break;
        EnterCriticalSection(&c->lock);
        if (!c->box_full) { LeaveCriticalSection(&c->lock); continue; }
        w = c->box_w; h = c->box_h; id = c->box_id; vs = c->box_vsync; key = c->box_key;
        if ((size_t)w * h * 3u > pic_cap) {
            uint8_t *g = (uint8_t *)realloc(pic, (size_t)w * h * 3u);
            if (!g) { c->box_full = 0; LeaveCriticalSection(&c->lock); InterlockedExchange(&c->failed, 1); c->why = "out of memory"; break; }
            pic = g; pic_cap = (size_t)w * h * 3u;
        }
        memcpy(pic, c->box, (size_t)w * h * 3u);
        c->box_full = 0;
        c->box_key = 0;
        LeaveCriticalSection(&c->lock);

        if (enc && (w != ew || h != eh)) { groovy_nvenc_close(enc); enc = NULL; key = 1; }   /* a new size is a new session */
        if (!enc) {
            groovy_nvenc_cfg cfg;
            const char *why = NULL;
            groovy_nvenc_cfg_defaults(&cfg, w, h);
            cfg.fps_num = c->fps_num; cfg.fps_den = c->fps_den; cfg.bitrate_kbps = c->kbps;
            enc = groovy_nvenc_open(&cfg, &why);
            if (!enc) { c->why = why ? why : "NVENC refused"; InterlockedExchange(&c->failed, 1); break; }
            ew = w; eh = h; key = 1;
        }
        {
            const uint8_t *au = NULL;
            size_t len = 0u;
            int is_key = 0;
            double us = 0.0;
            if (groovy_nvenc_encode(enc, pic, key, &au, &len, &is_key, &us) != 0) {
                c->st.errors++;
                c->why = "NVENC encode error";
                InterlockedExchange(&c->failed, 1);
                break;
            }
            c->st.last_us = us;
            if (us > c->st.worst_us) c->st.worst_us = us;
            if (gm_send_codec_frame(c->gm, id, vs, au, len, is_key) == 0) c->st.encoded++;
            else c->st.errors++;
        }
    }
    if (enc) groovy_nvenc_close(enc);
    free(pic);
    return 0;
}

groovy_codec *groovy_codec_start(void *gm, unsigned kbps, unsigned fps_num, unsigned fps_den)
{
    groovy_codec *c;
    if (!gm) return NULL;
    c = (groovy_codec *)calloc(1u, sizeof *c);
    if (!c) return NULL;
    c->gm = (gm_handle *)gm;
    c->kbps = kbps; c->fps_num = fps_num ? fps_num : 60u; c->fps_den = fps_den ? fps_den : 1u;
    InitializeCriticalSection(&c->lock);
    c->wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    c->thread = c->wake ? CreateThread(NULL, 0, codec_thread, c, 0, NULL) : NULL;
    if (!c->thread) {
        if (c->wake) CloseHandle(c->wake);
        DeleteCriticalSection(&c->lock);
        free(c);
        return NULL;
    }
    return c;
}

void groovy_codec_stop(groovy_codec *c)
{
    if (!c) return;
    InterlockedExchange(&c->stop, 1);
    SetEvent(c->wake);
    WaitForSingleObject(c->thread, 5000);
    CloseHandle(c->thread);
    CloseHandle(c->wake);
    DeleteCriticalSection(&c->lock);
    free(c->box);
    free(c);
}

int groovy_codec_submit(groovy_codec *c, const uint8_t *bgr888, unsigned w, unsigned h,
                        uint32_t frame_id, uint16_t vsync, int force_key)
{
    const size_t n = (size_t)w * h * 3u;
    if (!c || !bgr888 || w == 0u || h == 0u || c->failed) return -1;
    EnterCriticalSection(&c->lock);
    if (n > c->box_cap) {
        uint8_t *g = (uint8_t *)realloc(c->box, n);
        if (!g) { LeaveCriticalSection(&c->lock); return -1; }
        c->box = g; c->box_cap = n;
    }
    memcpy(c->box, bgr888, n);
    if (c->box_full) c->st.overwritten++;
    c->box_w = w; c->box_h = h; c->box_id = frame_id; c->box_vsync = vsync;
    c->box_key = c->box_key || force_key;
    c->box_full = 1;
    c->st.submitted++;
    LeaveCriticalSection(&c->lock);
    SetEvent(c->wake);
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

#endif /* _WIN32 */
