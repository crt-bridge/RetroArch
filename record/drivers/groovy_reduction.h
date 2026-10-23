/* SPDX-License-Identifier: GPL-3.0-or-later */
/* groovy_reduction.h -- the supersampling reduction rule of the crt-bridge
 * emitter, with no dependency on RetroArch. Header-only, same pattern as
 * groovy_khz31.h: record_groovy.c includes it to run, and the test
 * emitter/test/test_groovy_reduction.py compiles its own small harness
 * around it to exercise it outside RetroArch.
 *
 * WHY ONE FILE. The factor by which a rendered frame is reduced before it
 * is sent is decided here and only here. The menu, the environment
 * variable, the status line and the refusal message all take their words
 * and their rule from this header, so none of them can drift from another.
 *
 * THE RULE. Auto reduces a supersampled hardware capture by an integer
 * factor, and only when the ratio between the rendered size and the size
 * the core declared holds on BOTH axes (a ratio that holds on one axis
 * only is a coincidence, not a supersampled image). Otherwise an image
 * that fits the receiver goes out untouched; an image that does not fit is
 * reduced by the smallest factor that divides both axes and makes it fit.
 * A software core sends its native size, so Auto never reduces it. Off
 * never reduces and refuses what the receiver cannot serve; a fixed factor
 * refuses an image it does not divide, or that it does not make fit.
 *
 * THE RECEIVER LIMITS are copied from the receiver's own source, because
 * nothing on the wire carries them: it refuses a mode taller than 480 lines
 * or larger than 1280*480*4 bytes of slot buffer (3 bytes per pixel on the
 * wire). A test reads the receiver header and fails if the copy drifts.
 *
 * WHERE THE REDUCTION RUNS is not decided here. A later change may move a
 * reduction onto the card; the decision stays in this file either way.
 */

#ifndef GROOVY_REDUCTION_H
#define GROOVY_REDUCTION_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

/* Largest frame, in lines, the receiver serves. */
#define GROOVY_REDUCTION_MAX_LINES 480u
/* Slot buffer capacity of the receiver, in bytes (MAX_FRAME_BYTES there). */
#define GROOVY_REDUCTION_MAX_BYTES (1280u * 480u * 4u)
/* Largest integer factor Auto or a fixed setting will use. */
#define GROOVY_REDUCTION_MAX_FACTOR 8u
/* No status line for a frame with a side below this: the start-up images
 * (1x1, 4x4 without content) are not a decision anybody should read. */
#define GROOVY_REDUCTION_STATUS_MIN_SIDE 16u
/* Least time between two refusal messages, in nanoseconds (5 s). */
#define GROOVY_REDUCTION_REFUSAL_GAP_NS UINT64_C(5000000000)

/* The menu index: 0 Auto, 1 Off, 2 = 2x, 3 = 4x, 4 = 8x. */
enum groovy_reduction_mode
{
    GROOVY_REDUCTION_AUTO = 0,
    GROOVY_REDUCTION_OFF = 1,
    GROOVY_REDUCTION_X2 = 2,
    GROOVY_REDUCTION_X4 = 3,
    GROOVY_REDUCTION_X8 = 4
};

enum groovy_reduction_reason
{
    GROOVY_REDUCTION_NATIVE = 0,            /* the frame goes out untouched  */
    GROOVY_REDUCTION_DECLARED_RATIO,        /* integer ratio to the declared size */
    GROOVY_REDUCTION_CEILING,               /* too large: smallest clean factor */
    GROOVY_REDUCTION_SOFTWARE_CORE,         /* no hardware capture: native size */
    GROOVY_REDUCTION_FIXED,                 /* a fixed factor was chosen     */
    GROOVY_REDUCTION_REFUSED_OVERSIZE,      /* too large for the receiver    */
    GROOVY_REDUCTION_REFUSED_NOT_DIVISIBLE, /* a fixed factor does not divide */
    GROOVY_REDUCTION_REFUSED_NO_CLEAN_FACTOR, /* no factor makes it fit      */
    GROOVY_REDUCTION_REFUSED_PREREDUCED     /* the video driver already reduced it */
};

/* factor == 0 means the frame is refused. */
struct groovy_reduction_decision
{
    unsigned factor;
    enum groovy_reduction_reason reason;
};

/* 1 if the receiver can serve a w x h frame, 0 otherwise. The one predicate
 * behind "fits": Off, fixed factors (after reduction), and both Auto rules
 * use it. Computed in 64 bits so hostile dimensions cannot wrap. */
static inline int groovy_reduction_fits(unsigned w, unsigned h)
{
    if (w == 0u || h == 0u || h > GROOVY_REDUCTION_MAX_LINES)
        return 0;
    return ((uint64_t)w * (uint64_t)h * 3u
            <= (uint64_t)GROOVY_REDUCTION_MAX_BYTES) ? 1 : 0;
}

/* The factor a mode asks for: Auto 0 (decided per frame), Off 1, then 2, 4
 * and 8. An unknown value is read as Auto. */
static inline unsigned groovy_reduction_mode_factor(
        enum groovy_reduction_mode mode)
{
    switch (mode)
    {
        case GROOVY_REDUCTION_OFF: return 1u;
        case GROOVY_REDUCTION_X2:  return 2u;
        case GROOVY_REDUCTION_X4:  return 4u;
        case GROOVY_REDUCTION_X8:  return 8u;
        case GROOVY_REDUCTION_AUTO:
        default:                   return 0u;
    }
}

static inline struct groovy_reduction_decision groovy_reduction_make(
        unsigned factor, enum groovy_reduction_reason reason)
{
    struct groovy_reduction_decision d;
    d.factor = factor;
    d.reason = reason;
    return d;
}

/* Decides the factor for a rendered w x h frame. `hw` is non-zero for a
 * hardware capture (a supersampled FBO); rw x rh is the size the core
 * declared, 0 x 0 when it declared nothing. */
static inline struct groovy_reduction_decision groovy_reduction_decide(
        enum groovy_reduction_mode mode, int hw,
        unsigned w, unsigned h, unsigned rw, unsigned rh)
{
    unsigned n, k;

    if (mode == GROOVY_REDUCTION_OFF)
        return groovy_reduction_fits(w, h)
            ? groovy_reduction_make(1u, GROOVY_REDUCTION_NATIVE)
            : groovy_reduction_make(0u, GROOVY_REDUCTION_REFUSED_OVERSIZE);

    n = groovy_reduction_mode_factor(mode);
    if (n >= 2u)
    {
        if (w % n != 0u || h % n != 0u)
            return groovy_reduction_make(0u,
                    GROOVY_REDUCTION_REFUSED_NOT_DIVISIBLE);
        return groovy_reduction_fits(w / n, h / n)
            ? groovy_reduction_make(n, GROOVY_REDUCTION_FIXED)
            : groovy_reduction_make(0u, GROOVY_REDUCTION_REFUSED_OVERSIZE);
    }

    /* Auto. A software core sends its native size. */
    if (!hw)
        return groovy_reduction_fits(w, h)
            ? groovy_reduction_make(1u, GROOVY_REDUCTION_SOFTWARE_CORE)
            : groovy_reduction_make(0u, GROOVY_REDUCTION_REFUSED_OVERSIZE);

    /* Declared ratio: the same integer on both axes. A declared size of 0
     * (a core that declares nothing) never divides anything. */
    if (rw != 0u && rh != 0u && h > rh && h % rh == 0u && w % rw == 0u
            && h / rh == w / rw)
    {
        k = h / rh;
        if (k >= 2u && k <= GROOVY_REDUCTION_MAX_FACTOR
                && groovy_reduction_fits(w / k, h / k))
            return groovy_reduction_make(k, GROOVY_REDUCTION_DECLARED_RATIO);
    }

    /* No such ratio: an image that fits goes out untouched. */
    if (groovy_reduction_fits(w, h))
        return groovy_reduction_make(1u, GROOVY_REDUCTION_NATIVE);

    /* Too large for the receiver: the smallest clean factor. */
    for (k = 2u; k <= GROOVY_REDUCTION_MAX_FACTOR; k++)
        if (w % k == 0u && h % k == 0u && groovy_reduction_fits(w / k, h / k))
            return groovy_reduction_make(k, GROOVY_REDUCTION_CEILING);

    return groovy_reduction_make(0u,
            GROOVY_REDUCTION_REFUSED_NO_CLEAN_FACTOR);
}

#endif /* GROOVY_REDUCTION_H */
