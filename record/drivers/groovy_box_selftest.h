/* SPDX-License-Identifier: GPL-3.0-or-later */
/* groovy_box_selftest.h -- the self-test of the GPU box reduction, with no
 * dependency on RetroArch and no graphics API. Header-only, on the same model
 * as groovy_downsample.h and groovy_reduction.h: a video driver includes it to
 * run, and a test includes it to exercise it outside RetroArch.
 *
 * Why it exists: the card is trusted to average the supersampled frame only
 * after it has reproduced the CPU box, byte for byte, on a fixed
 * pseudo-random image of the real size. The pattern is built so that it
 * covers every byte value and the rounding of each block; the CPU box
 * (groovy_downsample_box_rgb888_n2/_n4/_n8) stays the oracle, and nothing here
 * averages anything by itself. One copy of the pattern and of the comparison
 * serves every video driver, so the OpenGL and Vulkan paths are judged by the
 * same bytes.
 *
 * The driver's part stays in the driver: upload the image from
 * groovy_boxtest_fill_rgba(), run its reduction, read the result back as RGBA,
 * and hand it to groovy_boxtest_compare() with the expected RGB from
 * groovy_boxtest_expected(). The number of bytes that differ is the verdict:
 * zero, or the card is not used.
 *
 * The image: every RGB byte comes from a fixed-seed linear congruential
 * generator (non-uniform blocks, whose averages truncate), except the first n
 * rows, made of uniform n x n blocks that must come out unchanged (2D
 * invariance), with 0 and 255 in every channel across that row. For n = 4 it
 * is the image the OpenGL driver has always used.
 */

#ifndef GROOVY_BOX_SELFTEST_H
#define GROOVY_BOX_SELFTEST_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "groovy_downsample.h"

/* 1 for the factors the CPU box has a fixed version of (2, 4, 8), 0 for the
 * rest: the self-test is never run for a factor it cannot judge. */
static inline int groovy_boxtest_factor_ok(unsigned n)
{
    return n == 2u || n == 4u || n == 8u;
}

/* Fill `rgba` (w * h pixels, rows in order, R G B A in memory) with the test
 * image for the factor n. Writes nothing if the factor is not admitted or the
 * dimensions do not divide by it. */
static inline void groovy_boxtest_fill_rgba(uint8_t *rgba, unsigned w,
                                            unsigned h, unsigned n)
{
    static const uint8_t levels[8] = { 0, 255, 1, 254, 127, 128, 64, 191 };
    uint32_t seed = 0x1badb002u;
    size_t   npx, i;
    unsigned x, y;

    if (!rgba || !groovy_boxtest_factor_ok(n) || !groovy_downsample_dims_ok(w, h, n))
        return;

    npx = (size_t)w * (size_t)h;
    for (i = 0; i < npx; i++) {
        seed          = seed * 1664525u + 1013904223u;
        rgba[4*i + 0] = (uint8_t)(seed >> 24);
        rgba[4*i + 1] = (uint8_t)(seed >> 16);
        rgba[4*i + 2] = (uint8_t)(seed >>  8);
        rgba[4*i + 3] = 255;
    }
    for (y = 0; y < n; y++)
        for (x = 0; x < w; x++) {
            unsigned b = x / n;
            uint8_t *p = rgba + ((size_t)y * w + x) * 4;
            p[0]       = levels[b % 8];
            p[1]       = (uint8_t)(255 - levels[b % 8]);
            p[2]       = levels[(b + 3) % 8];
        }
}

/* Drop the alpha byte: `npx` RGBA pixels in, `npx` packed RGB888 pixels out. */
static inline void groovy_boxtest_rgba_to_rgb(uint8_t *rgb, const uint8_t *rgba,
                                              size_t npx)
{
    size_t i;

    if (!rgb || !rgba)
        return;
    for (i = 0; i < npx; i++) {
        rgb[3*i + 0] = rgba[4*i + 0];
        rgb[3*i + 1] = rgba[4*i + 1];
        rgb[3*i + 2] = rgba[4*i + 2];
    }
}

/* What the card must produce: the CPU box of the packed RGB888 image `rgb`
 * (w x h), written to `cpu_rgb` as (w / n) x (h / n) packed RGB888. Returns 1
 * when it was computed, 0 (and writes nothing) for a factor that is not
 * admitted or dimensions that do not divide by it. */
static inline int groovy_boxtest_expected(uint8_t *cpu_rgb, const uint8_t *rgb,
                                          unsigned w, unsigned h, unsigned n)
{
    if (!cpu_rgb || !rgb || !groovy_boxtest_factor_ok(n)
            || !groovy_downsample_dims_ok(w, h, n))
        return 0;

    switch (n) {
        case 2u: groovy_downsample_box_rgb888_n2(cpu_rgb, rgb, w, h); break;
        case 4u: groovy_downsample_box_rgb888_n4(cpu_rgb, rgb, w, h); break;
        default: groovy_downsample_box_rgb888_n8(cpu_rgb, rgb, w, h); break;
    }
    return 1;
}

/* Number of R, G and B bytes of the card's output (`gpu_rgba`, out_w x out_h
 * RGBA) that differ from the expected RGB (`cpu_rgb`); the alpha byte is not
 * compared. The total compared is out_w * out_h * 3. A missing buffer is the
 * worst answer, never zero. */
static inline unsigned groovy_boxtest_compare(const uint8_t *gpu_rgba,
                                              const uint8_t *cpu_rgb,
                                              unsigned out_w, unsigned out_h)
{
    size_t   nbox, i;
    unsigned differ = 0u;

    if (!gpu_rgba || !cpu_rgb)
        return 0xFFFFFFFFu;

    nbox = (size_t)out_w * (size_t)out_h;
    for (i = 0; i < nbox; i++) {
        differ += gpu_rgba[4*i + 0] != cpu_rgb[3*i + 0];
        differ += gpu_rgba[4*i + 1] != cpu_rgb[3*i + 1];
        differ += gpu_rgba[4*i + 2] != cpu_rgb[3*i + 2];
    }
    return differ;
}

#endif /* GROOVY_BOX_SELFTEST_H */
