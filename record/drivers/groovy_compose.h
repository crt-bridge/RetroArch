/* SPDX-License-Identifier: GPL-3.0-or-later */
/* groovy_compose.h -- put a native-size picture into the size of the
 * announced mode, the way the receiver used to.
 *
 * The receiver serves the mode it is announced as received and no longer
 * stretches the picture into it. The picture therefore leaves the emitter at
 * the size of that mode (width x lines of the announced modeline), laid out
 * exactly as the receiver laid it out when it did the work itself:
 *
 *   - the horizontal factor is `xscale_pick` of the receiver (copied below):
 *     the largest whole factor that fits, except for a small picture with
 *     square pixels (a handheld, a portrait picture), which gets the factor
 *     that keeps its own proportion;
 *   - each source pixel is repeated that many times across the line, and the
 *     drawn width is centred in the mode;
 *   - the lines of the source are laid in order, the drawn height is centred
 *     in the mode; what does not fit is cut at the bottom and on the right;
 *   - everything else is black.
 *
 * `groovy_compose_factor_pinned` is a COPY of `xscale_pick` (the public fork
 * includes nothing from the receiver). A test compiles the receiver's header
 * on every run and requires both to agree on a whole grid of sizes (with no
 * declared aspect), and to differ only on the portrait cells when the aspect
 * says the screen is not a portrait. Do not change one alone.
 *
 * `groovy_compose_frame` also takes a vertical factor, 1 on the path of the
 * pinned modes; above 1 every source line is repeated that many times.
 *
 * Pure: no allocation, no I/O, only <stdint.h>, <stddef.h> and <string.h>.
 * Everything is `static inline`, so two translation units can include it.
 */

#ifndef GROOVY_COMPOSE_H
#define GROOVY_COMPOSE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* A picture of this many lines or fewer has square pixels. */
#define GROOVY_COMPOSE_SQUARE_PIXEL_MAX_LINES 160u
/* A portrait picture counts as square-pixel only from this width on (an
 * Atari 2600 is taller than wide but has pixels twice as wide as tall). */
#define GROOVY_COMPOSE_PORTRAIT_MIN_WIDTH     200u

/* Largest source the composition accepts, in bytes: the product that does
 * not fit 32 bits is refused (the picture is then left black). */
#define GROOVY_COMPOSE_MAX_BYTES              0xFFFFFFFFull

/* A declared screen proportion is usable when it is a finite number above
 * zero (NaN and infinity fail the test). Anything else counts as absent. */
#define GROOVY_COMPOSE_ASPECT_USABLE(a)       ((a) > 0.0f && (a) < 1.0e6f)

/* groovy_compose_factor_pinned -- the horizontal factor, a whole number >= 1
 * that never goes beyond what fits in `mode_w`. Copy of `xscale_pick`
 * (receiver/xscale.h), which takes signed ints: a size above INT32_MAX reads
 * as negative there and gives 1, so it does here.
 *
 * One deliberate difference from the receiver: `aspect` is the screen
 * proportion the core declares (`geometry.aspect_ratio`). The receiver only
 * has the pixel size and takes every picture taller than wide (200 columns or
 * more) for a portrait screen. A core that declares a screen at least as wide
 * as tall (aspect >= 1: Sonic 2 in 480i, 320x448, declares 1.306) is not a
 * portrait, whatever its pixel size: it keeps the whole factor that fits. A
 * declared portrait (aspect < 1: the DS, 0.667) and an absent, zero,
 * negative, NaN or infinite aspect keep the receiver's rule, so a call with
 * aspect 0 is `xscale_pick` exactly. */
static inline unsigned groovy_compose_factor_pinned(unsigned src_w, unsigned src_h,
                                                    unsigned mode_w, unsigned mode_h,
                                                    float aspect)
{
    unsigned fit;
    double   want;
    unsigned k;
    int      portrait;

    if (src_w == 0u || mode_w == 0u || src_w > 0x7FFFFFFFu || mode_w > 0x7FFFFFFFu)
        return 1u;
    fit = mode_w / src_w;
    if (fit < 1u)
        return 1u;
    if (src_h == 0u || mode_h == 0u || src_h > 0x7FFFFFFFu || mode_h > 0x7FFFFFFFu)
        return fit;
    portrait = src_h > src_w && src_w >= GROOVY_COMPOSE_PORTRAIT_MIN_WIDTH
               && !(GROOVY_COMPOSE_ASPECT_USABLE(aspect) && aspect >= 1.0f);
    if (src_h > GROOVY_COMPOSE_SQUARE_PIXEL_MAX_LINES && !portrait)
        return fit;

    /* square pixels: wanted width = mode_w x (src_h / mode_h) x (src_w / src_h) / (4/3) */
    want = (double)mode_w * ((double)src_w / (double)mode_h) * 0.75;
    k = (unsigned)(int)(want / (double)src_w + 0.5);
    if (k < 1u)
        k = 1u;
    if (k > fit)
        k = fit;
    return k;
}

/* groovy_compose_frame -- lay `src` (src_w x src_h, RGB888, tightly packed)
 * into `dst` (mode_w x mode_h, RGB888, tightly packed, mode_w * mode_h * 3
 * bytes), repeating each source pixel `kx` times across and each source line
 * `ky` times down.
 *
 * Returns 1 when `dst` was written in full (black where nothing is drawn),
 * 0 when the mode itself is unusable (a side of zero, or a size above
 * GROOVY_COMPOSE_MAX_BYTES): nothing is written then. A source that is
 * unusable (a side of zero, a size above GROOVY_COMPOSE_MAX_BYTES, a null
 * pointer) gives a black picture and is never read. Every product is made in
 * 64 bits and nothing is read or written outside src_w x src_h x 3 and
 * mode_w x mode_h x 3 bytes. A factor of zero counts as 1. */
static inline int groovy_compose_frame(uint8_t *dst, unsigned mode_w, unsigned mode_h,
                                       const uint8_t *src, unsigned src_w, unsigned src_h,
                                       unsigned kx, unsigned ky)
{
    uint64_t dst_bytes;
    uint64_t draw_w;
    uint64_t draw_h;
    size_t   x_off;
    size_t   y_off;
    size_t   row_bytes;
    size_t   y;

    if (!dst || mode_w == 0u || mode_h == 0u)
        return 0;
    dst_bytes = (uint64_t)mode_w * (uint64_t)mode_h * 3ull;
    if (dst_bytes > GROOVY_COMPOSE_MAX_BYTES)
        return 0;

    memset(dst, 0, (size_t)dst_bytes);

    if (!src || src_w == 0u || src_h == 0u
        || (uint64_t)src_w * (uint64_t)src_h * 3ull > GROOVY_COMPOSE_MAX_BYTES)
        return 1;

    if (kx == 0u) kx = 1u;
    if (ky == 0u) ky = 1u;

    draw_w = (uint64_t)src_w * (uint64_t)kx;
    if (draw_w > mode_w) draw_w = mode_w;
    draw_h = (uint64_t)src_h * (uint64_t)ky;
    if (draw_h > mode_h) draw_h = mode_h;

    x_off     = (size_t)(((uint64_t)mode_w - draw_w) / 2u);
    y_off     = (size_t)(((uint64_t)mode_h - draw_h) / 2u);
    row_bytes = (size_t)mode_w * 3u;

    for (y = 0; y < (size_t)draw_h; y++) {
        uint8_t *drow = dst + (y_off + y) * row_bytes + x_off * 3u;

        if (y % ky != 0u) {
            /* a repeat of the line above: same bytes, same place */
            memcpy(drow, drow - row_bytes, (size_t)draw_w * 3u);
        } else {
            const uint8_t *srow = src + (y / ky) * (size_t)src_w * 3u;
            size_t         sx;

            if (kx == 1u) {
                memcpy(drow, srow, (size_t)draw_w * 3u);
            } else {
                /* whole blocks only: a block that would pass the drawn
                 * width is not drawn */
                for (sx = 0; sx < src_w; sx++) {
                    uint64_t dx = (uint64_t)sx * kx;
                    unsigned rep;

                    if (dx + kx > draw_w)
                        break;
                    for (rep = 0; rep < kx; rep++) {
                        uint8_t *p = drow + (size_t)(dx + rep) * 3u;
                        p[0] = srow[sx * 3u + 0u];
                        p[1] = srow[sx * 3u + 1u];
                        p[2] = srow[sx * 3u + 2u];
                    }
                }
            }
        }
    }
    return 1;
}

#endif /* GROOVY_COMPOSE_H */
