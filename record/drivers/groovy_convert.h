/* SPDX-License-Identifier: GPL-3.0-or-later */
/* groovy_convert.h -- the three input converters of the bridge (BGR24,
 * XRGB8888, RGB565 -> packed RGB888), with no dependency on RetroArch.
 * Header-only, on the same model as groovy_downsample.h: record_groovy.c
 * includes it to run, and the test bridge libgm/test/groovy_test_bridge.c
 * includes it to exercise it outside RetroArch.
 *
 * The three *_box functions are the converters
 * record_groovy.c used before, body unchanged: a ratio box filter that
 * computes, for EVERY output pixel, its source box with 64-bit divisions
 * and divides the three sums by the pixel count. At 1:1 the box is one
 * pixel and the count is 1 -- the result is a plain copy -- yet the cost
 * stays: measured in the fork at 23.6 ms per frame for BGR24 1280x960,
 * enough to starve the audio buffer.
 *
 * The *_direct functions do that 1:1 case with no division: a byte swap
 * (BGR24, XRGB8888) or the same 5/6/5 bit replication (RGB565). The
 * dispatchers bgr24_to_rgb888(), xrgb8888_to_rgb888() and
 * rgb565_to_rgb888() -- the names record_groovy.c calls -- take the direct
 * path when source and destination dimensions are equal, and the box
 * otherwise. Byte for byte, direct == box at 1:1 (dividing by 1 changes
 * nothing): emitter/test/test_groovy_convert.py proves it.
 */

#ifndef GROOVY_CONVERT_H
#define GROOVY_CONVERT_H

#include <stdint.h>
#include <stddef.h>

/* ---------------------------------------------------------------------------
 * bgr24_to_rgb888 — in-place B↔R byte swap (Pitfall 2 mitigation).
 *
 * glcore PBO readback produces BGR24; daemon expects RGB888. Swap is
 * mandatory for correct color on the CRT (STRIDE T-3-02).
 *
 * Handles pitch != width*3 (e.g., GL row alignment padding): reads from
 * pitch-strided source rows, writes to tightly-packed destination rows.
 * dst and src MAY alias only if dst == src AND pitch == width*3 (caller
 * MUST use swap_buf, not vid->data, to avoid aliasing in the non-trivial
 * pitch case).
 *
 * dst: contiguous output buffer, at least width*height*3 bytes.
 * src: source buffer, pitch bytes per row, height rows.
 * ------------------------------------------------------------------------- */
static inline void groovy_bgr24_to_rgb888_box(uint8_t       *dst,
                             const uint8_t *src,
                             unsigned       dst_w,
                             unsigned       dst_h,
                             unsigned       src_w,
                             unsigned       src_h,
                             unsigned       src_pitch)
{
    /* Box-filter downscale + BGR → RGB (Bug E v2: text legibility fix).
     * Same algorithm as xrgb8888_to_rgb888 but reads 3-byte BGR pixels.
     * When src == dst dims, box is 1×1 and this is a 1:1 copy with swap. */
    for (unsigned dy = 0; dy < dst_h; ++dy) {
        unsigned sy0 = (unsigned)((size_t)dy * src_h / dst_h);
        unsigned sy1 = (unsigned)((size_t)(dy + 1u) * src_h / dst_h);
        if (sy1 <= sy0) sy1 = sy0 + 1u;
        if (sy1 > src_h) sy1 = src_h;

        uint8_t *dst_row = dst + (size_t)dy * dst_w * 3u;
        for (unsigned dx = 0; dx < dst_w; ++dx) {
            unsigned sx0 = (unsigned)((size_t)dx * src_w / dst_w);
            unsigned sx1 = (unsigned)((size_t)(dx + 1u) * src_w / dst_w);
            if (sx1 <= sx0) sx1 = sx0 + 1u;
            if (sx1 > src_w) sx1 = src_w;

            uint32_t r_acc = 0u, g_acc = 0u, b_acc = 0u;
            uint32_t n = 0u;
            for (unsigned sy = sy0; sy < sy1; ++sy) {
                const uint8_t *src_row = src + (size_t)sy * src_pitch;
                for (unsigned sx = sx0; sx < sx1; ++sx) {
                    /* BGR24: B=src[0], G=src[1], R=src[2] */
                    b_acc += src_row[3u * sx + 0u];
                    g_acc += src_row[3u * sx + 1u];
                    r_acc += src_row[3u * sx + 2u];
                    ++n;
                }
            }
            dst_row[3u * dx + 0u] = (uint8_t)(r_acc / n);
            dst_row[3u * dx + 1u] = (uint8_t)(g_acc / n);
            dst_row[3u * dx + 2u] = (uint8_t)(b_acc / n);
        }
    }
}

/* ---------------------------------------------------------------------------
 * xrgb8888_to_rgb888 — convert 32-bit XRGB8888 (RetroArch FFEMU_PIX_ARGB8888,
 * 4 bytes/pixel little-endian) to packed RGB888 (3 bytes/pixel) — Bug D fix.
 *
 * RetroArch's RETRO_PIXEL_FORMAT_XRGB8888 stores each pixel as a 32-bit value
 * 0x00RRGGBB in native byte order. On x86 (little-endian), the byte layout is:
 *   src[0]=B, src[1]=G, src[2]=R, src[3]=X (alpha/padding, ignored)
 * Daemon expects packed RGB888: dst[0]=R, dst[1]=G, dst[2]=B.
 *
 * Same dst/src layout assumptions as bgr24_to_rgb888: dst contiguous
 * width*height*3, src pitch-strided height rows.
 * ------------------------------------------------------------------------- */
static inline void groovy_xrgb8888_to_rgb888_box(uint8_t       *dst,
                                const uint8_t *src,
                                unsigned       dst_w,
                                unsigned       dst_h,
                                unsigned       src_w,
                                unsigned       src_h,
                                unsigned       src_pitch)
{
    /* Box-filter downscale + XRGB8888 → RGB888 (Bug E v2: quality fix).
     *
     * For each dst pixel, average all source pixels in the box it covers.
     * For 700→320 horizontal (ratio 2.19) and 576→240 vertical (ratio 2.4),
     * each dst pixel averages ~2x2 to ~3x3 src pixels. This eliminates the
     * "missing lines" / illegible text artifact of nearest-neighbor.
     *
     * Cost: ~5x nearest-neighbor (76800 dst px × ~6 src reads + accumulate
     * + divide). Trivial at 60 Hz on any modern CPU.
     *
     * If src dims == dst dims, the box is 1×1 and this is a 1:1 copy with
     * format swap — same path serves the no-scale case. */
    for (unsigned dy = 0; dy < dst_h; ++dy) {
        /* Compute the source row range covered by this dst row. */
        unsigned sy0 = (unsigned)((size_t)dy * src_h / dst_h);
        unsigned sy1 = (unsigned)((size_t)(dy + 1u) * src_h / dst_h);
        if (sy1 <= sy0) sy1 = sy0 + 1u;
        if (sy1 > src_h) sy1 = src_h;

        uint8_t *dst_row = dst + (size_t)dy * dst_w * 3u;
        for (unsigned dx = 0; dx < dst_w; ++dx) {
            unsigned sx0 = (unsigned)((size_t)dx * src_w / dst_w);
            unsigned sx1 = (unsigned)((size_t)(dx + 1u) * src_w / dst_w);
            if (sx1 <= sx0) sx1 = sx0 + 1u;
            if (sx1 > src_w) sx1 = src_w;

            uint32_t r_acc = 0u, g_acc = 0u, b_acc = 0u;
            uint32_t n = 0u;
            for (unsigned sy = sy0; sy < sy1; ++sy) {
                const uint8_t *src_row = src + (size_t)sy * src_pitch;
                for (unsigned sx = sx0; sx < sx1; ++sx) {
                    /* XRGB8888 little-endian: B=src[0], G=src[1], R=src[2] */
                    b_acc += src_row[4u * sx + 0u];
                    g_acc += src_row[4u * sx + 1u];
                    r_acc += src_row[4u * sx + 2u];
                    ++n;
                }
            }
            /* n is at least 1 by clamp above */
            dst_row[3u * dx + 0u] = (uint8_t)(r_acc / n);  /* R */
            dst_row[3u * dx + 1u] = (uint8_t)(g_acc / n);  /* G */
            dst_row[3u * dx + 2u] = (uint8_t)(b_acc / n);  /* B */
        }
    }
}

/* ---------------------------------------------------------------------------
 * rgb565_to_rgb888 — convert 16-bit RGB565 (RetroArch RETRO_PIXEL_FORMAT_RGB565
 * / FFEMU_PIX_RGB565, 2 bytes/pixel, native little-endian uint16) to packed
 * RGB888 (3 bytes/pixel).
 *
 * Bit layout: bits 15..11 = R (5 bits), bits 10..5 = G (6 bits), bits 4..0 =
 * B (5 bits). Normative source for the 5/6/5 -> 8/8/8 expansion:
 * libretro-common/gfx/scaler/pixconv.c::conv_rgb565_argb8888 (same fork,
 * same pin) — bit replication of the high bits, so 0x1f -> 255 and
 * 0x00 -> 0 (full-scale expansion, not a left-shift-only truncation).
 *
 * src_pitch is in bytes and is typically larger than src_w*2 — RetroArch
 * cores commonly allocate a wider framebuffer than the active area (e.g.
 * Genesis Plus GX: pitch=1440 for 320 active px = 720px-wide bitmap).
 *
 * Same dst/src layout assumptions as bgr24_to_rgb888 / xrgb8888_to_rgb888:
 * dst contiguous width*height*3, src pitch-strided height rows.
 * ------------------------------------------------------------------------- */
static inline void groovy_rgb565_to_rgb888_box(uint8_t       *dst,
                              const uint8_t *src,
                              unsigned       dst_w,
                              unsigned       dst_h,
                              unsigned       src_w,
                              unsigned       src_h,
                              unsigned       src_pitch)
{
    /* Box-filter downscale + RGB565 → RGB888 (same algorithm as
     * bgr24_to_rgb888 / xrgb8888_to_rgb888). When src == dst dims, the box
     * is 1×1 and this is a 1:1 copy with format expansion. */
    for (unsigned dy = 0; dy < dst_h; ++dy) {
        unsigned sy0 = (unsigned)((size_t)dy * src_h / dst_h);
        unsigned sy1 = (unsigned)((size_t)(dy + 1u) * src_h / dst_h);
        if (sy1 <= sy0) sy1 = sy0 + 1u;
        if (sy1 > src_h) sy1 = src_h;

        uint8_t *dst_row = dst + (size_t)dy * dst_w * 3u;
        for (unsigned dx = 0; dx < dst_w; ++dx) {
            unsigned sx0 = (unsigned)((size_t)dx * src_w / dst_w);
            unsigned sx1 = (unsigned)((size_t)(dx + 1u) * src_w / dst_w);
            if (sx1 <= sx0) sx1 = sx0 + 1u;
            if (sx1 > src_w) sx1 = src_w;

            uint32_t r_acc = 0u, g_acc = 0u, b_acc = 0u;
            uint32_t n = 0u;
            for (unsigned sy = sy0; sy < sy1; ++sy) {
                const uint8_t *src_row = src + (size_t)sy * src_pitch;
                for (unsigned sx = sx0; sx < sx1; ++sx) {
                    /* Native LE uint16 RGB565: recomposed byte-by-byte so it
                     * never depends on any alignment of the source pitch. */
                    const uint8_t *px = src_row + 2u * sx;
                    uint32_t col = (uint32_t)px[0] | ((uint32_t)px[1] << 8);
                    uint32_t r5 = (col >> 11) & 0x1fu;
                    uint32_t g6 = (col >>  5) & 0x3fu;
                    uint32_t b5 = (col >>  0) & 0x1fu;
                    /* 5/6/5 -> 8/8/8 expansion by high-bit replication
                     * (identical to conv_rgb565_argb8888): 0x1f -> 255. */
                    r_acc += (r5 << 3) | (r5 >> 2);
                    g_acc += (g6 << 2) | (g6 >> 4);
                    b_acc += (b5 << 3) | (b5 >> 2);
                    ++n;
                }
            }
            dst_row[3u * dx + 0u] = (uint8_t)(r_acc / n);
            dst_row[3u * dx + 1u] = (uint8_t)(g_acc / n);
            dst_row[3u * dx + 2u] = (uint8_t)(b_acc / n);
        }
    }
}

/* ---------------------------------------------------------------------------
 * The 1:1 paths. Same layouts as the box versions: src pitch-strided,
 * dst tightly packed w*3 bytes per row; dst[0]=R, dst[1]=G, dst[2]=B.
 * ------------------------------------------------------------------------- */
static inline void groovy_bgr24_to_rgb888_direct(uint8_t *dst,
        const uint8_t *src, unsigned w, unsigned h, unsigned src_pitch)
{
    for (unsigned y = 0; y < h; ++y) {
        const uint8_t *s = src + (size_t)y * src_pitch;
        uint8_t       *d = dst + (size_t)y * w * 3u;
        for (unsigned x = 0; x < w; ++x, s += 3, d += 3) {
            /* Read the whole pixel before writing: dst may alias src
             * (same buffer, pitch == w*3), as the box version allows. */
            const uint8_t b = s[0], g = s[1], r = s[2];
            d[0] = r;
            d[1] = g;
            d[2] = b;
        }
    }
}

static inline void groovy_xrgb8888_to_rgb888_direct(uint8_t *dst,
        const uint8_t *src, unsigned w, unsigned h, unsigned src_pitch)
{
    for (unsigned y = 0; y < h; ++y) {
        const uint8_t *s = src + (size_t)y * src_pitch;
        uint8_t       *d = dst + (size_t)y * w * 3u;
        for (unsigned x = 0; x < w; ++x, s += 4, d += 3) {
            d[0] = s[2];   /* R */
            d[1] = s[1];   /* G */
            d[2] = s[0];   /* B */
        }
    }
}

static inline void groovy_rgb565_to_rgb888_direct(uint8_t *dst,
        const uint8_t *src, unsigned w, unsigned h, unsigned src_pitch)
{
    for (unsigned y = 0; y < h; ++y) {
        const uint8_t *s = src + (size_t)y * src_pitch;
        uint8_t       *d = dst + (size_t)y * w * 3u;
        for (unsigned x = 0; x < w; ++x, s += 2, d += 3) {
            uint32_t col = (uint32_t)s[0] | ((uint32_t)s[1] << 8);
            uint32_t r5  = (col >> 11) & 0x1fu;
            uint32_t g6  = (col >>  5) & 0x3fu;
            uint32_t b5  = (col >>  0) & 0x1fu;
            d[0] = (uint8_t)((r5 << 3) | (r5 >> 2));
            d[1] = (uint8_t)((g6 << 2) | (g6 >> 4));
            d[2] = (uint8_t)((b5 << 3) | (b5 >> 2));
        }
    }
}

/* ---------------------------------------------------------------------------
 * The dispatchers record_groovy.c calls: direct at 1:1, box otherwise.
 * ------------------------------------------------------------------------- */
static inline void bgr24_to_rgb888(uint8_t *dst, const uint8_t *src,
        unsigned dst_w, unsigned dst_h, unsigned src_w, unsigned src_h,
        unsigned src_pitch)
{
    if (dst_w == src_w && dst_h == src_h)
        groovy_bgr24_to_rgb888_direct(dst, src, dst_w, dst_h, src_pitch);
    else
        groovy_bgr24_to_rgb888_box(dst, src, dst_w, dst_h,
                                   src_w, src_h, src_pitch);
}

static inline void xrgb8888_to_rgb888(uint8_t *dst, const uint8_t *src,
        unsigned dst_w, unsigned dst_h, unsigned src_w, unsigned src_h,
        unsigned src_pitch)
{
    if (dst_w == src_w && dst_h == src_h)
        groovy_xrgb8888_to_rgb888_direct(dst, src, dst_w, dst_h, src_pitch);
    else
        groovy_xrgb8888_to_rgb888_box(dst, src, dst_w, dst_h,
                                      src_w, src_h, src_pitch);
}

static inline void rgb565_to_rgb888(uint8_t *dst, const uint8_t *src,
        unsigned dst_w, unsigned dst_h, unsigned src_w, unsigned src_h,
        unsigned src_pitch)
{
    if (dst_w == src_w && dst_h == src_h)
        groovy_rgb565_to_rgb888_direct(dst, src, dst_w, dst_h, src_pitch);
    else
        groovy_rgb565_to_rgb888_box(dst, src, dst_w, dst_h,
                                    src_w, src_h, src_pitch);
}

#endif /* GROOVY_CONVERT_H */
