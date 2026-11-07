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

#endif /* GROOVY_NATIVE_H */
