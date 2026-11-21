/* SPDX-License-Identifier: GPL-3.0-or-later */
/* groovy_ffenc.h -- H.264 4:4:4 encoding of the native picture through libavcodec (libx264), for an
 * emitter without NVIDIA (codec milestone, step 2: the codec for everyone).
 *
 * Same shape as groovy_nvenc.h, and the same configuration structure: the codec thread of
 * record_groovy calls either one. libavcodec is NOT linked: it is loaded at run time (LoadLibrary /
 * dlopen) from GROOVY_FFMPEG_DIR, then the system, then (macOS) Homebrew, exactly as the client
 * loads it for decoding (client/codec_ff.c); no FFmpeg header is compiled in, the prototypes are
 * written by hand. Only the majors listed in groovy_ffenc.c are accepted. libx264 must be inside that
 * FFmpeg (a GPL build): when it is not, groovy_ffenc_open returns NULL and says why, and the caller
 * falls back to LZ4.
 *
 * Not thread-safe: one thread calls open, encode and close. */
#ifndef GROOVY_FFENC_H
#define GROOVY_FFENC_H

#include <stddef.h>
#include <stdint.h>

#include "groovy_nvenc.h"   /* groovy_nvenc_cfg: width, height, fps, bitrate_kbps */

typedef struct groovy_ffenc groovy_ffenc;

/* NULL when libavcodec or libx264 cannot be used; *why then points to a sentence (valid until the next call). */
groovy_ffenc *groovy_ffenc_open(const groovy_nvenc_cfg *cfg, const char **why);
void          groovy_ffenc_close(groovy_ffenc *e);

/* One picture in (bgr888, B first), one access unit out (Annex B, valid until the next call). force_key = 1
 * asks for an IDR with its parameter sets (the encoder is reopened: its first picture is an IDR). *is_key
 * tells whether the unit holds an IDR. *us = encode time in microseconds, colour conversion included.
 * Returns 0, or -1 on an encoder error. */
int groovy_ffenc_encode(groovy_ffenc *e, const uint8_t *bgr888, int force_key,
                        const uint8_t **out, size_t *len, int *is_key, double *us);

#endif /* GROOVY_FFENC_H */
