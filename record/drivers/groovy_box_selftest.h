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

/* The size of the second, stand-alone test of every factor. It is neither
 * square nor 4:3, so a swap of width and height cannot pass it, and its width
 * and height reduced by 8 (127 x 93) are both odd, which a size that is a
 * multiple of 16 would never show. */
#define GROOVY_BOXTEST_AXIS_W      1016u
#define GROOVY_BOXTEST_AXIS_H      744u

/* The most pixels a test image may have (4096 x 2048): the test allocates
 * seven bytes per pixel, and the size comes from whatever core is running. */
#define GROOVY_BOXTEST_MAX_PIXELS  (4096u * 2048u)

/* 1 for the factors the CPU box has a fixed version of (2, 4, 8), 0 for the
 * rest: the self-test is never run for a factor it cannot judge. */
static inline int groovy_boxtest_factor_ok(unsigned n)
{
    return n == 2u || n == 4u || n == 8u;
}

/* 1 when a w x h image can be tested for the factor n: the factor is admitted,
 * both sides are at least n and divide by it, and the pixel count (computed on
 * 64 bits, so a hostile size cannot wrap) is at most
 * GROOVY_BOXTEST_MAX_PIXELS. */
static inline int groovy_boxtest_fits(unsigned w, unsigned h, unsigned n)
{
    if (!groovy_boxtest_factor_ok(n))
        return 0;
    if (w < n || h < n)
        return 0;
    if ((w % n) != 0u || (h % n) != 0u)
        return 0;
    return ((uint64_t)w * (uint64_t)h) <= (uint64_t)GROOVY_BOXTEST_MAX_PIXELS;
}

/* Fill `rgba` (w * h pixels, rows in order, R G B A in memory) with the test
 * image for the factor n. Writes nothing if the image does not fit
 * (groovy_boxtest_fits). */
static inline void groovy_boxtest_fill_rgba(uint8_t *rgba, unsigned w,
                                            unsigned h, unsigned n)
{
    static const uint8_t levels[8] = { 0, 255, 1, 254, 127, 128, 64, 191 };
    uint32_t seed = 0x1badb002u;
    size_t   npx, i;
    unsigned x, y;

    if (!rgba || !groovy_boxtest_fits(w, h, n))
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
 * when it was computed, 0 (and writes nothing) when the image does not fit
 * (groovy_boxtest_fits). */
static inline int groovy_boxtest_expected(uint8_t *cpu_rgb, const uint8_t *rgb,
                                          unsigned w, unsigned h, unsigned n)
{
    if (!cpu_rgb || !rgb || !groovy_boxtest_fits(w, h, n))
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

/* The most (factor, size) pairs one session remembers as proven. */
#define GROOVY_BOXTEST_TABLE_CAP   16u

struct groovy_boxtest_pair {
    unsigned factor;
    unsigned w;
    unsigned h;
};

/* What a session has proven. Each (factor, size) pair is tested once, before
 * its first reduced frame, and written here when it passed; the table is
 * bounded so a core that changes size every frame cannot make the test run
 * every frame. Past the cap the size is left to the CPU, and the caller says
 * so once. `axis_done` holds, per factor (2, 4, 8), whether the stand-alone
 * test on the axis size already ran. All zero is the initial state: a calloc,
 * or a memset, is enough. */
struct groovy_boxtest_results {
    unsigned                   count;
    unsigned char              axis_done[3];
    struct groovy_boxtest_pair pair[GROOVY_BOXTEST_TABLE_CAP];
};

/* Index of an admitted factor in a per-factor array (2 -> 0, 4 -> 1, 8 -> 2),
 * -1 for any other. */
static inline int groovy_boxtest_factor_index(unsigned n)
{
    switch (n) {
        case 2u: return 0;
        case 4u: return 1;
        case 8u: return 2;
        default: return -1;
    }
}

/* 1 when the pair (n, w, h) was proven earlier in this session. */
static inline int groovy_boxtest_results_has(const struct groovy_boxtest_results *r,
                                             unsigned n, unsigned w, unsigned h)
{
    unsigned i, count;

    if (!r)
        return 0;
    count = r->count;
    if (count > GROOVY_BOXTEST_TABLE_CAP)
        count = GROOVY_BOXTEST_TABLE_CAP;
    for (i = 0; i < count; i++)
        if (r->pair[i].factor == n && r->pair[i].w == w && r->pair[i].h == h)
            return 1;
    return 0;
}

/* Record that the pair (n, w, h) passed. A pair is written once: adding a
 * pair that is already there succeeds without a second entry. Returns 0, and
 * writes nothing, when the table is full or the factor is not admitted. */
static inline int groovy_boxtest_results_add(struct groovy_boxtest_results *r,
                                             unsigned n, unsigned w, unsigned h)
{
    if (!r || !groovy_boxtest_factor_ok(n))
        return 0;
    if (groovy_boxtest_results_has(r, n, w, h))
        return 1;
    if (r->count >= GROOVY_BOXTEST_TABLE_CAP)
        return 0;
    r->pair[r->count].factor = n;
    r->pair[r->count].w      = w;
    r->pair[r->count].h      = h;
    r->count++;
    return 1;
}

/* 1 when the stand-alone test on the axis size is still to run for the factor
 * n: once per factor, and never for a factor that is not admitted. */
static inline int groovy_boxtest_axis_due(const struct groovy_boxtest_results *r,
                                          unsigned n)
{
    int idx = groovy_boxtest_factor_index(n);

    return r && idx >= 0 && !r->axis_done[idx];
}

/* Record that the stand-alone test on the axis size ran for the factor n.
 * A factor that is not admitted changes nothing. */
static inline void groovy_boxtest_axis_done(struct groovy_boxtest_results *r,
                                            unsigned n)
{
    int idx = groovy_boxtest_factor_index(n);

    if (r && idx >= 0)
        r->axis_done[idx] = 1;
}

#endif /* GROOVY_BOX_SELFTEST_H */
