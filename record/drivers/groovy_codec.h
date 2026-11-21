/* SPDX-License-Identifier: GPL-3.0-or-later */
/* groovy_codec.h -- the codec of ONE follower: a thread next to groovy_fan_out that encodes the
 * picture handed to it (NVENC, groovy_nvenc.h) and sends it on the follower's handle
 * (gm_send_codec_frame). The master is never in this path: the runloop only copies a picture into
 * a mailbox and goes on.
 *
 * Off by default: GROOVY_CODEC names the follower, the rate and the parity, e.g. "1:h264:20000:10" = the
 * first follower, H.264 4:4:4, 20000 kbit/s, 10 % Reed-Solomon parity (the burst-loss table of
 * libgm/test/test_shard_burst.c is why it is that low: under Wi-Fi bursts, parity buys little). (A key of GROOVY_FOLLOWERS will take over once the codec is
 * proven; for one follower an environment variable is enough.)
 *
 * The mailbox keeps the LATEST picture only: a codec that falls behind drops pictures, it never
 * queues them (a queue is latency). */
#ifndef GROOVY_CODEC_H
#define GROOVY_CODEC_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "groovy_nvenc.h"
#include "groovy_ffenc.h"

/* --- GROOVY_CODEC = <follower>:<codec>[:<kbit/s>[:<parity %>]] -------------------------------------
 * follower: 1..4 (1 = the first follower of GROOVY_FOLLOWERS); codec: "h264"; kbit/s: 500..200000
 * (default 20000); parity: 0..100 (default 10). Returns 1 and fills the outputs when well formed, 0
 * when absent or empty, -1 when malformed (the caller says so and does NOT start a codec). Pure. */
static inline int groovy_codec_parse(const char *s, unsigned *follower, unsigned *kbps, unsigned *parity)
{
    unsigned f = 0u, k = 20000u, par = 10u;
    const char *p = s;
    if (!s || s[0] == '\0') return 0;
    if (*p < '1' || *p > '4' || p[1] != ':') return -1;
    f = (unsigned)(*p - '0');
    p += 2;
    if (strncmp(p, "h264", 4) != 0) return -1;
    p += 4;
    if (*p == ':') {
        unsigned long v = 0ul;
        p++;
        if (*p < '0' || *p > '9') return -1;
        while (*p >= '0' && *p <= '9') { v = v * 10ul + (unsigned long)(*p - '0'); if (v > 1000000ul) return -1; p++; }
        if (v < 500ul || v > 200000ul) return -1;
        k = (unsigned)v;
        if (*p == ':') {
            unsigned long q = 0ul;
            p++;
            if (*p < '0' || *p > '9') return -1;
            while (*p >= '0' && *p <= '9') { q = q * 10ul + (unsigned long)(*p - '0'); if (q > 1000ul) return -1; p++; }
            if (q > 100ul) return -1;
            par = (unsigned)q;
        }
    }
    if (*p != '\0') return -1;
    if (follower) *follower = f;
    if (kbps) *kbps = k;
    if (parity) *parity = par;
    return 1;
}

typedef struct groovy_codec groovy_codec;
struct gm_handle_s;   /* gm_handle, opaque */

typedef struct {
    unsigned long long submitted;     /* pictures handed in */
    unsigned long long encoded;       /* pictures encoded and sent */
    unsigned long long overwritten;   /* pictures replaced in the mailbox before the thread took them */
    unsigned long long errors;
    double             last_us;       /* encode time of the last picture, microseconds */
    double             worst_us;
} groovy_codec_stats;

/* Cheap check, at the opening of the session: can an encoder be used at all here? 1 or 0 (*why says why).
 * GROOVY_ENCODER = auto (default: NVENC, then libavcodec/libx264) | nvenc | ff picks the encoder. */
int groovy_codec_available(const char **why);
/* The encoder groovy_codec_available chose: "NVENC", "libx264", or "none". */
const char *groovy_codec_backend(void);

/* Starts the thread for one follower handle (gm = a gm_handle *). NULL on failure. */
groovy_codec *groovy_codec_start(void *gm, unsigned kbps, unsigned fps_num, unsigned fps_den);
void          groovy_codec_stop(groovy_codec *c);

/* Hands a picture (bgr888, w x h) to the thread; copies it. force_key asks for a keyframe (a new
 * mode, a new session). Never blocks on the encoder. Returns 0, or -1. */
int groovy_codec_submit(groovy_codec *c, const uint8_t *bgr888, unsigned w, unsigned h,
                        uint32_t frame_id, uint16_t vsync, int force_key);

/* 1 once the encoder has failed for good (no NVENC, a refused configuration, an encoder error): the
 * caller takes the follower back to LZ4. */
int groovy_codec_failed(const groovy_codec *c, const char **why);
void groovy_codec_get_stats(const groovy_codec *c, groovy_codec_stats *out);

#endif /* GROOVY_CODEC_H */
