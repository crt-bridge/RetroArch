/* SPDX-License-Identifier: GPL-3.0-or-later */
/* groovy_native.h -- the native-width contract (CMD_NATIVE_SCALE, 0x09), the
 * emitter's side.
 *
 * Instead of repeating every source pixel kx times across the line (and every
 * line ky times) before it leaves, the emitter can send the NATIVE picture and
 * announce the factors; the receiver does the repetition. Only the native
 * picture then travels, at 1/(kx*ky) of the bytes.
 *
 * The contract applies only when the composition would be a PURE repetition
 * that fills the announced mode exactly: send_w * kx == mode_w and
 * send_h * ky == mode_h. Any other layout (black borders, a cut at the bottom
 * or on the right) is left to the composition, as before.
 *
 * Pure: no allocation, no I/O. Everything is `static inline`. */

#ifndef GROOVY_NATIVE_H
#define GROOVY_NATIVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "groovy_compose.h"

/* The factor bound the wire accepts (one byte each, 1..8). */
#define GROOVY_NATIVE_MAX_FACTOR 8u

/* GROOVY_NATIVE_SCALE: "on" / "1" asks for the contract, anything else (and an
 * absent variable) leaves it off. Off is the default: the wire is then bit for
 * bit what it was. */
static inline bool groovy_native_wanted(const char *env)
{
    if (!env) return false;
    return (env[0] == '1' && env[1] == '\0')
        || (env[0] == 'o' && env[1] == 'n' && env[2] == '\0');
}

/* Can the picture go out native AND PLACED (CMD_NATIVE_PLACE, 0x0A)? The
 * enlarged picture (send_w * kx by send_h * ky) must fit the mode; the
 * receiver centres it with the same floor as the composition (black borders
 * around). It must also be worth it: fewer pixels on the wire than the mode.
 * `fields` is true when the wire carries fields (480i): ky stays 1 and the
 * height is even (the receiver refuses the rest, and so do we). */
static inline bool groovy_native_fits(unsigned send_w, unsigned send_h,
                                      unsigned mode_w, unsigned mode_h,
                                      unsigned kx, unsigned ky, bool fields)
{
    if (kx < 1u || kx > GROOVY_NATIVE_MAX_FACTOR) return false;
    if (ky < 1u || ky > GROOVY_NATIVE_MAX_FACTOR) return false;
    if (send_w == 0u || send_h == 0u) return false;
    if (send_w > 0xFFFFu || send_h > 0xFFFFu) return false;   /* u16 on the wire */
    if (fields && (ky > 1u || (send_h & 1u))) return false;
    if ((uint64_t)send_w * kx > (uint64_t)mode_w) return false;
    if ((uint64_t)send_h * ky > (uint64_t)mode_h) return false;
    /* nothing to gain when the picture already is the mode */
    if ((uint64_t)send_w * send_h >= (uint64_t)mode_w * mode_h) return false;
    return true;
}

/* Can the picture go out native? `fields` is true when the wire carries
 * fields (480i): a vertical factor above 1 would repeat lines and break the
 * field order, so the receiver refuses it and so do we. */
static inline bool groovy_native_exact(unsigned send_w, unsigned send_h,
                                       unsigned mode_w, unsigned mode_h,
                                       unsigned kx, unsigned ky, bool fields)
{
    if (kx < 1u || kx > GROOVY_NATIVE_MAX_FACTOR) return false;
    if (ky < 1u || ky > GROOVY_NATIVE_MAX_FACTOR) return false;
    if (kx == 1u && ky == 1u) return false;   /* nothing to announce */
    if (send_w == 0u || send_h == 0u) return false;
    if (fields && ky > 1u) return false;
    if ((uint64_t)send_w * kx != (uint64_t)mode_w) return false;
    if ((uint64_t)send_h * ky != (uint64_t)mode_h) return false;
    return true;
}

/* What a receiver that cannot take the native picture (a MiSTer, a receiver
 * in a classic session, one older than the contract) is sent while another
 * receiver takes it: the composed picture, exactly what the emitter sent
 * before the contract existed. `kind` is how the wire carries the picture:
 *   GROOVY_NATIVE_KIND_FULL    one progressive frame       -> out0 = whole composed frame
 *   GROOVY_NATIVE_KIND_HALF    one field of parity `field` -> out0 = that field
 *   GROOVY_NATIVE_KIND_FIELDS  both fields                 -> out0 = rows 0, 2, ..., out1 = rows 1, 3, ...
 * `comp` is the scratch for the whole composed frame (mode_w * mode_h * 3
 * bytes); the field buffers hold (mode_w * (mode_h / 2) * 3) bytes each.
 * Returns true when the outputs were written. Pure: no allocation, no I/O. */
#define GROOVY_NATIVE_KIND_FULL   0
#define GROOVY_NATIVE_KIND_HALF   1
#define GROOVY_NATIVE_KIND_FIELDS 2

static inline bool groovy_native_composed_variant(int kind, unsigned field,
        uint8_t *comp, unsigned mode_w, unsigned mode_h,
        const uint8_t *native, unsigned w, unsigned h, unsigned kx, unsigned ky,
        uint8_t *out0, uint8_t *out1)
{
    if (!comp || !native || mode_w == 0u || mode_h == 0u) return false;
    if (!groovy_compose_frame(comp, mode_w, mode_h, native, w, h, kx, ky)) return false;
    if (kind == GROOVY_NATIVE_KIND_FULL) {
        if (!out0) return false;
        memcpy(out0, comp, (size_t)mode_w * (size_t)mode_h * 3u);
        return true;
    }
    {
        const size_t row = (size_t)mode_w * 3u;
        const size_t half = (size_t)(mode_h / 2u);
        size_t r;
        if (kind == GROOVY_NATIVE_KIND_HALF) {
            if (!out0 || field > 1u) return false;
            for (r = 0; r < half; ++r)
                memcpy(out0 + r * row, comp + (2u * r + (size_t)field) * row, row);
            return true;
        }
        if (kind == GROOVY_NATIVE_KIND_FIELDS) {
            if (!out0 || !out1) return false;
            for (r = 0; r < half; ++r) {
                memcpy(out0 + r * row, comp + (2u * r) * row, row);
                memcpy(out1 + r * row, comp + (2u * r + 1u) * row, row);
            }
            return true;
        }
    }
    return false;
}

/* Which receivers take the native picture: bit i of the result is set when
 * takes[i] is true. The master (index 0) is not special: a master that cannot
 * take it is composed for, and is still served first. */
static inline unsigned groovy_native_receiver_mask(const bool *takes, unsigned n)
{
    unsigned mask = 0u, i;
    for (i = 0u; i < n && i < 32u; i++)
        if (takes[i]) mask |= 1u << i;
    return mask;
}

#endif /* GROOVY_NATIVE_H */
