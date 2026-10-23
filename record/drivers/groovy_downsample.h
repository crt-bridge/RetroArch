/* SPDX-License-Identifier: GPL-3.0-or-later */
/* groovy_downsample.h -- the generic CPU box filter for supersampling, with
 * no dependency on RetroArch. Header-only, on the same model as
 * groovy_compression.h: record_groovy.c includes it to run, and the test
 * bridge libgm/test/groovy_test_bridge.c includes it to exercise it
 * outside RetroArch.
 *
 * Only one thing lives here: reading the GROOVY_DOWNSAMPLE setting, and
 * the box filter that averages n*n blocks of packed RGB888 down to one
 * pixel -- the mechanism described in the project's design notes on
 * generic supersampling (the exact box -- the math that governs
 * everything -- and why the v1 strategy stopped at its point of
 * convergence).
 *
 * Three things share that box: groovy_downsample_box_rgb888_generic(), the
 * reference (same body as before this file specialized per factor, and
 * still the path for n <= 1 and for any factor other than 2/4/8);
 * groovy_downsample_box_rgb888_fixed(), the same arithmetic with the
 * factor as a compile-time constant (always inlined, so the compiler
 * turns the divisions and inner loops into something cheap); and
 * groovy_downsample_box_rgb888(), the dispatcher record_groovy.c calls,
 * which picks _n2/_n4/_n8 or falls back to the generic reference.
 *
 * The setting is the "Supersampling reduction" entry of the Recording
 * menu; the GROOVY_DOWNSAMPLE environment variable overrides it when set.
 * record_groovy.c reads both, and groovy_reduction.h decides the factor
 * (including "auto"). The video driver does not read the variable: it asks
 * the recording for the factor with recording_capture_reduction().
 *
 * "gpu4" is an alias of the 4x4 reduction. Reading the
 * full-size frame back from the GPU and averaging it on the CPU costs a
 * large share of the per-frame budget at 1280x960; a video driver can
 * average the 4x4 blocks on the GPU instead and read back only the native
 * size. The video driver reports, for every frame, the factor by which it
 * already reduced the capture; groovy_downsample_cpu_factor() turns that
 * report and the decided factor into the factor the CPU box still has to
 * apply, so a frame is never reduced twice. If the video driver did not reduce a
 * "gpu4" frame, the CPU box does it (the image stays correct, only slower).
 * groovy_downsample_factor_parse() does not know "gpu4" on purpose: it keeps
 * its meaning for every other value, and an older binary reads "gpu4" as
 * "off", which the size check on the wire then reveals.
 *
 * The box filter is **arithmetically** exact on whole blocks: an n*n
 * block of identical pixels reproduces that exact value (2D invariance).
 * On a gradient, each block's average is a **truncated** integer
 * division, like the three format converters in record_groovy.c
 * (bgr24_to_rgb888, xrgb8888_to_rgb888, rgb565_to_rgb888), which already
 * do ratio box filtering.
 */

#ifndef GROOVY_DOWNSAMPLE_H
#define GROOVY_DOWNSAMPLE_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Reduction factor. 1 = off; otherwise 2, 4 or 8. */
static inline unsigned groovy_downsample_factor_parse(const char *s)
{
    if (!s || s[0] == '\0') return 1u;
    if (!strcmp(s, "2")) return 2u;
    if (!strcmp(s, "4")) return 4u;
    if (!strcmp(s, "8")) return 8u;
    return 1u;      /* "off", and any unknown value */
}

static inline const char *groovy_downsample_factor_name(unsigned n)
{
    switch (n) {
        case 2u: return "2";
        case 4u: return "4";
        case 8u: return "8";
        default: return "off";
    }
}

/* The factor the CPU box still has to apply to one frame; 0 = refuse it.
 * `factor` is the decided reduction, `prereduced` is the factor by which the
 * video driver already averaged this frame (1 = not reduced). The answer
 * does not depend on which setting chose the factor.
 *   prereduced == factor -> 1 (the driver did the whole reduction: send as is)
 *   prereduced == 1      -> factor (the driver did not reduce: the CPU does)
 *   anything else        -> 0.
 * A partial reduction is refused and never finished by a second box: two
 * truncating boxes in cascade do not equal the single n*n box, so the frame
 * would differ from the one the CPU path gives for the same setting. */
static inline unsigned groovy_downsample_cpu_factor(unsigned factor,
                                                    unsigned prereduced)
{
    if (prereduced == factor) return 1u;
    if (prereduced == 1u)     return factor;
    return 0u;
}

/* The submitted dims must be exactly divisible by n, both width AND
 * height. Otherwise the frame is refused: distorting it would betray the
 * goal ("always at native resolution"). n <= 1 accepts any nonzero
 * dimension. */
static inline int groovy_downsample_dims_ok(unsigned w, unsigned h, unsigned n)
{
    if (w == 0u || h == 0u) return 0;
    if (n <= 1u)            return 1;
    return ((w % n) == 0u && (h % n) == 0u);
}

/* Size of the destination buffer, in bytes. 0 if the dims don't fit. */
static inline size_t groovy_downsample_dst_bytes(unsigned w, unsigned h, unsigned n)
{
    if (!groovy_downsample_dims_ok(w, h, n)) return 0u;
    if (n <= 1u) return (size_t)w * (size_t)h * 3u;
    return (size_t)(w / n) * (size_t)(h / n) * 3u;
}

/* Average of n*n blocks -- the reference. `src`: PACKED RGB888 (src_w*3
 * bytes per row, no pitch -- this is swap_buf after format normalization,
 * never vid->data). `dst`: packed RGB888 of (src_w/n) x (src_h/n). No
 * allocation. dst and src must not overlap. The caller MUST have checked
 * dims_ok. n is read at runtime: this is the path for n <= 1 (plain copy)
 * and for any factor other than 2/4/8, and it is the byte-for-byte
 * reference that the fixed-factor versions below are proven against. */
static inline void groovy_downsample_box_rgb888_generic(uint8_t       *dst,
                                                         const uint8_t *src,
                                                         unsigned       src_w,
                                                         unsigned       src_h,
                                                         unsigned       n)
{
    if (!dst || !src || src_w == 0u || src_h == 0u) return;
    if (n <= 1u) {
        memcpy(dst, src, (size_t)src_w * (size_t)src_h * 3u);
        return;
    }

    const unsigned dst_w   = src_w / n;
    const unsigned dst_h   = src_h / n;
    const size_t   src_row = (size_t)src_w * 3u;
    const uint32_t divisor = (uint32_t)n * (uint32_t)n;   /* 4, 16 or 64 */

    for (unsigned dy = 0u; dy < dst_h; ++dy) {
        uint8_t *dst_row = dst + (size_t)dy * (size_t)dst_w * 3u;
        for (unsigned dx = 0u; dx < dst_w; ++dx) {
            uint32_t r = 0u, g = 0u, b = 0u;   /* 64 * 255 = 16320, plenty of headroom */
            for (unsigned sy = 0u; sy < n; ++sy) {
                const uint8_t *p = src
                                 + ((size_t)dy * n + sy) * src_row
                                 + (size_t)dx * n * 3u;
                for (unsigned sx = 0u; sx < n; ++sx) {
                    r += p[3u * sx + 0u];
                    g += p[3u * sx + 1u];
                    b += p[3u * sx + 2u];
                }
            }
            dst_row[3u * dx + 0u] = (uint8_t)(r / divisor);
            dst_row[3u * dx + 1u] = (uint8_t)(g / divisor);
            dst_row[3u * dx + 2u] = (uint8_t)(b / divisor);
        }
    }
}

/* Portability: force inlining where the compiler supports it, so the
 * factor really does become a compile-time constant at each call site
 * below. Falls back to a plain inline hint elsewhere. */
#if defined(__GNUC__) || defined(__clang__)
#define GROOVY_DS_ALWAYS_INLINE __attribute__((always_inline)) inline
#else
#define GROOVY_DS_ALWAYS_INLINE inline
#endif

/* Same arithmetic as the generic reference above, for n > 1 only (the
 * dispatcher below handles n <= 1). Always inlined into each of _n2/_n4/
 * _n8, so at each of those call sites `n` is a compile-time constant: the
 * divisions by 4/16/64 become shifts and the inner sy/sx loops unroll
 * (measured on the bench: 0.63 ms instead of 3.5 ms at 1280x960). No
 * branch on n <= 1 here -- that case never reaches this function. */
static GROOVY_DS_ALWAYS_INLINE void groovy_downsample_box_rgb888_fixed(
        uint8_t       *dst,
        const uint8_t *src,
        unsigned       src_w,
        unsigned       src_h,
        const unsigned n)
{
    const unsigned dst_w   = src_w / n;
    const unsigned dst_h   = src_h / n;
    const size_t   src_row = (size_t)src_w * 3u;
    const uint32_t divisor = (uint32_t)n * (uint32_t)n;   /* 4, 16 or 64 */

    for (unsigned dy = 0u; dy < dst_h; ++dy) {
        uint8_t *dst_row = dst + (size_t)dy * (size_t)dst_w * 3u;
        for (unsigned dx = 0u; dx < dst_w; ++dx) {
            uint32_t r = 0u, g = 0u, b = 0u;   /* 64 * 255 = 16320, plenty of headroom */
            for (unsigned sy = 0u; sy < n; ++sy) {
                const uint8_t *p = src
                                 + ((size_t)dy * n + sy) * src_row
                                 + (size_t)dx * n * 3u;
                for (unsigned sx = 0u; sx < n; ++sx) {
                    r += p[3u * sx + 0u];
                    g += p[3u * sx + 1u];
                    b += p[3u * sx + 2u];
                }
            }
            dst_row[3u * dx + 0u] = (uint8_t)(r / divisor);
            dst_row[3u * dx + 1u] = (uint8_t)(g / divisor);
            dst_row[3u * dx + 2u] = (uint8_t)(b / divisor);
        }
    }
}

static inline void groovy_downsample_box_rgb888_n2(uint8_t *dst, const uint8_t *src,
                                                    unsigned src_w, unsigned src_h)
{
    groovy_downsample_box_rgb888_fixed(dst, src, src_w, src_h, 2u);
}

static inline void groovy_downsample_box_rgb888_n4(uint8_t *dst, const uint8_t *src,
                                                    unsigned src_w, unsigned src_h)
{
    groovy_downsample_box_rgb888_fixed(dst, src, src_w, src_h, 4u);
}

static inline void groovy_downsample_box_rgb888_n8(uint8_t *dst, const uint8_t *src,
                                                    unsigned src_w, unsigned src_h)
{
    groovy_downsample_box_rgb888_fixed(dst, src, src_w, src_h, 8u);
}

/* Dispatcher -- what record_groovy.c calls, once per image, name and
 * contract unchanged (the caller has already checked dims_ok). n is chosen
 * at runtime (GROOVY_DOWNSAMPLE), but each factor's own version gives the
 * compiler a constant: 2, 4 and 8 go to the fixed-factor versions above,
 * everything else (including n <= 1) falls back to the generic
 * reference. */
static inline void groovy_downsample_box_rgb888(uint8_t       *dst,
                                                const uint8_t *src,
                                                unsigned       src_w,
                                                unsigned       src_h,
                                                unsigned       n)
{
    if (!dst || !src || src_w == 0u || src_h == 0u) return;
    switch (n) {
        case 2u: groovy_downsample_box_rgb888_n2(dst, src, src_w, src_h); return;
        case 4u: groovy_downsample_box_rgb888_n4(dst, src, src_w, src_h); return;
        case 8u: groovy_downsample_box_rgb888_n8(dst, src, src_w, src_h); return;
        default: groovy_downsample_box_rgb888_generic(dst, src, src_w, src_h, n); return;
    }
}

#endif /* GROOVY_DOWNSAMPLE_H */
