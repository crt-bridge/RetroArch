/* SPDX-License-Identifier: GPL-3.0-or-later */
/* groovy_nvenc.h -- H.264 4:4:4 encoding of the native picture through NVENC, loaded at run time
 * (phase 38, slice 3 of the codec phase).
 *
 * NVENC is reached through the driver's own nvEncodeAPI64.dll and nvcuda.dll, loaded with
 * LoadLibrary: nothing is linked, nothing new is built, nothing is shipped. When the DLL or the card is
 * missing, groovy_nvenc_open returns NULL and says why; the caller falls back to LZ4. The header of the
 * API (record/drivers/nvenc/nvEncodeAPI.h, MIT, see SOURCE.md there) is compiled in only to know the
 * structures of the version of the API this file was written against.
 *
 * One encoder = one session = one picture size. A new size is a new encoder (the caller closes and
 * reopens: the first picture of a session is a keyframe anyway).
 *
 * Not thread-safe: one thread calls open, encode and close (the codec thread of record_groovy). */
#ifndef GROOVY_NVENC_H
#define GROOVY_NVENC_H

#include <stddef.h>
#include <stdint.h>

typedef struct groovy_nvenc groovy_nvenc;

typedef struct {
    unsigned width, height;        /* the native picture */
    unsigned fps_num, fps_den;     /* the rate it arrives at (the VBV buffer is one frame of it) */
    unsigned bitrate_kbps;         /* constant bitrate, 20000 by default (spike 019: 39.7 dB on Tekken 3) */
    int      intra_refresh;        /* 1 = gradual refresh over `refresh_period` frames instead of periodic keyframes */
    unsigned refresh_period;       /* 60 by default */
} groovy_nvenc_cfg;

/* Fills the defaults for a picture size. */
void groovy_nvenc_cfg_defaults(groovy_nvenc_cfg *cfg, unsigned width, unsigned height);

/* NULL when NVENC cannot be used; *why then points to a static sentence. */
groovy_nvenc *groovy_nvenc_open(const groovy_nvenc_cfg *cfg, const char **why);
void          groovy_nvenc_close(groovy_nvenc *e);

/* One picture in (bgr888 = 3 bytes per pixel, B first, the order of the wire and of the fork's frames),
 * one access unit out (Annex B, valid until the next call). force_key = 1 asks for an IDR with its
 * parameter sets. *is_key tells whether the unit starts a decodable sequence. *us = encode time in
 * microseconds (conversion to the input format included). Returns 0, or -1 on an encoder error. */
int groovy_nvenc_encode(groovy_nvenc *e, const uint8_t *bgr888, int force_key,
                        const uint8_t **out, size_t *len, int *is_key, double *us);

#endif /* GROOVY_NVENC_H */
