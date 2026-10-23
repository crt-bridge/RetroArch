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

/* Where a reduction actually ran, as the status line says it. */
enum groovy_reduction_path
{
    GROOVY_REDUCTION_PATH_NONE = 0,  /* factor 1: nothing to reduce        */
    GROOVY_REDUCTION_PATH_CPU,       /* the box filter on the processor    */
    GROOVY_REDUCTION_PATH_GPU,       /* the video driver reduced on the card */
    GROOVY_REDUCTION_PATH_REFUSED    /* the frame was refused              */
};

/* Reads the GROOVY_DOWNSAMPLE value. Only "auto", "off", "2", "4", "8" and
 * "gpu4" are known, spelled exactly: anything else (NULL, empty, another
 * case, another number) returns 0 and leaves both outputs untouched, so the
 * caller can warn naming the value and fall back to Auto. Returns 1 when the
 * value is known. "gpu4" is the 4x setting with *gpu = 1 (the reduction is
 * asked of the video driver). A NULL output pointer is skipped. */
static inline int groovy_reduction_parse(const char *s,
        enum groovy_reduction_mode *mode, int *gpu)
{
    enum groovy_reduction_mode m;
    int g = 0;

    if (!s)
        return 0;
    if (!strcmp(s, "auto"))
        m = GROOVY_REDUCTION_AUTO;
    else if (!strcmp(s, "off"))
        m = GROOVY_REDUCTION_OFF;
    else if (!strcmp(s, "2"))
        m = GROOVY_REDUCTION_X2;
    else if (!strcmp(s, "4"))
        m = GROOVY_REDUCTION_X4;
    else if (!strcmp(s, "8"))
        m = GROOVY_REDUCTION_X8;
    else if (!strcmp(s, "gpu4"))
    {
        m = GROOVY_REDUCTION_X4;
        g = 1;
    }
    else
        return 0;

    if (mode)
        *mode = m;
    if (gpu)
        *gpu = g;
    return 1;
}

/* The name the variable and the "[groovy] downsample: factor=" log line
 * use: auto off 2 4 8 gpu4. An unknown mode is named like Auto. */
static inline const char *groovy_reduction_name(
        enum groovy_reduction_mode mode, int gpu)
{
    switch (mode)
    {
        case GROOVY_REDUCTION_OFF: return "off";
        case GROOVY_REDUCTION_X2:  return "2";
        case GROOVY_REDUCTION_X4:  return gpu ? "gpu4" : "4";
        case GROOVY_REDUCTION_X8:  return "8";
        case GROOVY_REDUCTION_AUTO:
        default:                   return "auto";
    }
}

/* The first word of the status line: Auto Off 2x 4x 8x gpu4. */
static inline const char *groovy_reduction_display_name(
        enum groovy_reduction_mode mode, int gpu)
{
    switch (mode)
    {
        case GROOVY_REDUCTION_OFF: return "Off";
        case GROOVY_REDUCTION_X2:  return "2x";
        case GROOVY_REDUCTION_X4:  return gpu ? "gpu4" : "4x";
        case GROOVY_REDUCTION_X8:  return "8x";
        case GROOVY_REDUCTION_AUTO:
        default:                   return "Auto";
    }
}

/* The mode a stored menu index stands for. An index out of range (a
 * retroarch.cfg edited by hand) reads as Auto. */
static inline enum groovy_reduction_mode groovy_reduction_mode_from_menu(
        unsigned v)
{
    return (v <= (unsigned)GROOVY_REDUCTION_X8)
        ? (enum groovy_reduction_mode)v : GROOVY_REDUCTION_AUTO;
}

/* The words of the menu, plain ASCII (the menu font of the bench is
 * bitmap): Auto Off 2x 4x 8x. An index out of range shows Auto. */
static inline const char *groovy_reduction_menu_name(unsigned v)
{
    return groovy_reduction_display_name(
            groovy_reduction_mode_from_menu(v), 0);
}

/* GPU CPU none REFUSED. */
static inline const char *groovy_reduction_path_name(
        enum groovy_reduction_path path)
{
    switch (path)
    {
        case GROOVY_REDUCTION_PATH_GPU:     return "GPU";
        case GROOVY_REDUCTION_PATH_CPU:     return "CPU";
        case GROOVY_REDUCTION_PATH_REFUSED: return "REFUSED";
        case GROOVY_REDUCTION_PATH_NONE:
        default:                            return "none";
    }
}

/* The decision changes only when the rendered size changes. An open menu is
 * composed at the size of the last core frame, so the rendered size does not
 * move, nothing is recomputed and nothing is announced.
 *
 * The key is the rendered size. `declare` only stores the size the core
 * declared as a candidate; it is adopted as the reference at the next change
 * of rendered size. A declaration that flips while the picture stays the
 * same size therefore cannot move the factor.
 *
 * A zeroed struct is the valid initial state (as a calloc gives it). */
struct groovy_reduction_memo
{
    unsigned key_w, key_h;                /* rendered size of the decision  */
    struct groovy_reduction_decision decision;
    unsigned used_rw, used_rh;            /* reference the decision used    */
    unsigned cand_rw, cand_rh;            /* latest declared, not yet used  */
    bool valid;
};

static inline void groovy_reduction_memo_declare(
        struct groovy_reduction_memo *memo, unsigned rw, unsigned rh)
{
    memo->cand_rw = rw;
    memo->cand_rh = rh;
}

/* The decision for a rendered w x h frame. *changed (if not NULL) is set to
 * 1 when the decision was recomputed (first frame, or a new rendered size),
 * 0 when the remembered one was returned. */
static inline struct groovy_reduction_decision groovy_reduction_memo_frame(
        struct groovy_reduction_memo *memo, enum groovy_reduction_mode mode,
        int hw, unsigned w, unsigned h, int *changed)
{
    if (!memo->valid || memo->key_w != w || memo->key_h != h)
    {
        memo->used_rw = memo->cand_rw;
        memo->used_rh = memo->cand_rh;
        memo->decision = groovy_reduction_decide(mode, hw, w, h,
                memo->used_rw, memo->used_rh);
        memo->key_w = w;
        memo->key_h = h;
        memo->valid = true;
        if (changed)
            *changed = 1;
    }
    else if (changed)
        *changed = 0;
    return memo->decision;
}

#endif /* GROOVY_REDUCTION_H */
