/* SPDX-License-Identifier: GPL-3.0-or-later */
/* groovy_mode.h — the single 240p/480i regime decision, shared by both
 * announce points (groovy_push_av_info, groovy_push_video) and by the
 * unit-test bridge (libgm/test/groovy_test_bridge.c).
 *
 * Header-only so the test bridge and the runtime share the same logic —
 * same pattern as groovy_modeline.h, groovy_reannounce.h, groovy_pacing.h.
 * No dependency on RetroArch or on gm.h: pure decision, testable outside
 * RetroArch.
 *
 * The regime is decided on height ALONE. Width used to gate it too
 * (a line-frequency formula, "Bug F", carried over without argument into
 * the 240p/480i split). 240p super-res scans 2560 pixels wide and
 * integer-scales up (640 -> x4, 512 -> x5, 320 -> x8, 256 -> x10): width
 * imposes nothing on the regime.
 * Threshold unchanged, more than 280 lines -> 480i (no 288p regime
 * at the receiver).
 * Core-doubled 240p (256x448, 320x448) stays 480i for now.
 * Detecting the doubling is left for later work.
 */

#ifndef GROOVY_MODE_H
#define GROOVY_MODE_H

#include <stdint.h>
#include <stdbool.h>

enum groovy_mode { GROOVY_MODE_240P_SUPER_RES = 0, GROOVY_MODE_480I = 1 };   /* moved from record_groovy.h */

#define GROOVY_MODE_480I_ABOVE_LINES   280u                 /* 480i threshold */
#define GROOVY_DIM_HYSTERESIS_FRAMES   15u                  /* moved from record_groovy.c */
#define GROOVY_MODE_GUARD_MAX_SWITCHES 2u                   /* oscillation guard: switches allowed per window */
#define GROOVY_MODE_GUARD_WINDOW_NS    1000000000ull        /* oscillation guard: the window, in ns (1 s) */
#define GROOVY_MODE_GUARD_QUIET_NS     1000000000ull        /* oscillation guard: quiet time before it lifts, in ns (1 s) */

typedef struct groovy_av_cache {                           /* moved from record_groovy.c, same fields */
    unsigned width;
    unsigned height;
    double   fps;
    bool     valid;
} groovy_av_cache_t;

enum groovy_mode_action {
    GROOVY_MODE_KEEP        = 0,   /* nothing to announce */
    GROOVY_MODE_ANNOUNCE    = 1,   /* cache updated: announce it now, before this frame's blit */
    GROOVY_MODE_DROP_FROZEN = 2    /* regime switch refused by the guard */
};

enum groovy_mode_reason {          /* why the last ANNOUNCE happened -- for the log line */
    GROOVY_MODE_REASON_NONE     = 0,
    GROOVY_MODE_REASON_FIRST    = 1,   /* nothing announced before */
    GROOVY_MODE_REASON_SWITCH   = 2,   /* regime changed */
    GROOVY_MODE_REASON_DECLARED = 3,   /* size equals the core's declaration, same regime */
    GROOVY_MODE_REASON_SETTLED  = 4    /* undeclared size, stable for the hysteresis */
};

struct groovy_mode_state {
    unsigned pending_w, pending_h, pending_streak;  /* same-regime size hysteresis (moved from groovy_state_t) */
    unsigned hint_w, hint_h;                         /* last geometry declared by the core (a hint, not an authority) */
    bool     have_hint;
    uint64_t switch_ns[GROOVY_MODE_GUARD_MAX_SWITCHES];
    unsigned switch_count;
    enum groovy_mode last_wanted;
    bool     have_wanted;
    uint64_t last_request_ns;
    bool     frozen;
    enum groovy_mode_reason last_reason;
    uint64_t switches;       /* regime switches announced, session total */
    uint32_t freezes;
    uint64_t frozen_drops;   /* frames refused: contrary to the frozen regime */
    uint64_t dims_drops;     /* frames refused: size != announced, same regime */
};
/* A zeroed struct (calloc) is the valid initial state. */

/* groovy_mode_classify — height alone decides the regime. */
static inline enum groovy_mode groovy_mode_classify(unsigned h)
{
    return (h > GROOVY_MODE_480I_ABOVE_LINES) ? GROOVY_MODE_480I : GROOVY_MODE_240P_SUPER_RES;
}

/* groovy_mode_of — the regime of the last announced cache is the
 * ONLY source the field-splitter and the modeline's interlace flag derive
 * from. An invalid cache (nothing announced yet) reads as 240p: the first
 * announce for any session comes from groovy_mode_on_frame's FIRST reason,
 * not from this helper. */
static inline enum groovy_mode groovy_mode_of(const groovy_av_cache_t *c)
{
    return c->valid ? groovy_mode_classify(c->height) : GROOVY_MODE_240P_SUPER_RES;
}

static inline const char *groovy_mode_name(enum groovy_mode m)
{
    return (m == GROOVY_MODE_480I) ? "480i" : "240p_super_res";
}

/* groovy_mode_on_declare — the core's declaration is only a
 * HINT, never an authority. It never announces, never touches the cache,
 * never requests a switch. A late frame at the old size must still be able
 * to leave in the old regime, unannounced, unrefused. */
static inline void groovy_mode_on_declare(struct groovy_mode_state *ms, unsigned w, unsigned h)
{
    ms->hint_w    = w;
    ms->hint_h    = h;
    ms->have_hint = true;
}

/* groovy_mode_note_request — internal. A "request" is a CHANGE of the
 * regime the incoming frames are asking for, not every contrary frame:
 * repeating the same request never touches last_request_ns, so the guard's
 * quiet timer (below) measures time since the demand last changed, not
 * time since the last frame. While frozen, once GROOVY_MODE_GUARD_QUIET_NS
 * has elapsed without the requested regime changing, the freeze lifts and
 * the switch-window ring is cleared -- a fresh start, not a partial one. */
static inline void groovy_mode_note_request(struct groovy_mode_state *ms,
        enum groovy_mode wanted, uint64_t now_ns)
{
    if (!ms->have_wanted || ms->last_wanted != wanted) {
        ms->last_wanted    = wanted;
        ms->have_wanted    = true;
        ms->last_request_ns = now_ns;
    }
    if (ms->frozen && (now_ns - ms->last_request_ns) >= GROOVY_MODE_GUARD_QUIET_NS) {
        ms->frozen       = false;
        ms->switch_count = 0u;
    }
}

/* groovy_mode_guard — internal. Freezes on the last ANNOUNCED regime
 * once 2 switches land inside a 1 s sliding window. Already frozen skips
 * straight to DROP_FROZEN without touching the ring -- the ring only ever
 * records regimes that were actually ANNOUNCEd (real switches), matching
 * groovy_mode_of()'s "last announced" definition of the frozen regime. */
static inline enum groovy_mode_action groovy_mode_guard(struct groovy_mode_state *ms,
        enum groovy_mode announced, enum groovy_mode wanted, uint64_t now_ns)
{
    unsigned i, count;

    if (wanted == announced)
        return GROOVY_MODE_KEEP;
    if (ms->frozen)
        return GROOVY_MODE_DROP_FROZEN;

    count = 0u;
    for (i = 0u; i < ms->switch_count; i++) {
        if ((now_ns - ms->switch_ns[i]) < GROOVY_MODE_GUARD_WINDOW_NS)
            count++;
    }
    if (count >= GROOVY_MODE_GUARD_MAX_SWITCHES) {
        ms->frozen = true;
        ms->freezes++;
        return GROOVY_MODE_DROP_FROZEN;
    }

    if (ms->switch_count < GROOVY_MODE_GUARD_MAX_SWITCHES) {
        ms->switch_ns[ms->switch_count++] = now_ns;
    } else {
        /* Ring full: evict the oldest entry (index 0), shift, append. */
        for (i = 1u; i < GROOVY_MODE_GUARD_MAX_SWITCHES; i++)
            ms->switch_ns[i - 1u] = ms->switch_ns[i];
        ms->switch_ns[GROOVY_MODE_GUARD_MAX_SWITCHES - 1u] = now_ns;
    }
    ms->switches++;
    return GROOVY_MODE_ANNOUNCE;
}

/* groovy_mode_announce — internal. The only place that writes the cache.
 * Consumes the declaration hint exactly when this frame's size matches it:
 * a hint for a DIFFERENT size stays armed for a later frame. */
static inline enum groovy_mode_action groovy_mode_announce(struct groovy_mode_state *ms,
        groovy_av_cache_t *cache, unsigned w, unsigned h, double fps,
        enum groovy_mode_reason reason)
{
    cache->width      = w;
    cache->height     = h;
    cache->fps        = fps;
    cache->valid      = true;
    ms->last_reason   = reason;
    if (ms->have_hint && w == ms->hint_w && h == ms->hint_h)
        ms->have_hint = false;
    return GROOVY_MODE_ANNOUNCE;
}

/* groovy_mode_on_frame — the only place that may ANNOUNCE. Every rendered
 * frame is classified, checked against the announced cache and the
 * declaration hint, and against the oscillation guard, in this
 * order:
 *   1. no cache yet (first frame of the session) -> announce (FIRST) ;
 *   2. the frame's regime contradicts the announced one -> the image
 *      makes the call, through the guard, at the FIRST contrary
 *      frame, no settle wait ;
 *   3. same regime, new size, exactly the last declared size -> announce
 *      (DECLARED) without waiting ;
 *   4. same regime, new size, stable for GROOVY_DIM_HYSTERESIS_FRAMES ->
 *      announce (SETTLED) ;
 *   5. otherwise -> keep.
 * The same-regime size hysteresis (pending_w/h/streak) updates BEFORE
 * these checks, unconditionally -- including on a DROP_FROZEN frame, so
 * the count is already there once the guard lifts. */
static inline enum groovy_mode_action groovy_mode_on_frame(struct groovy_mode_state *ms,
        groovy_av_cache_t *cache, unsigned w, unsigned h, double fps, uint64_t now_ns)
{
    enum groovy_mode wanted = groovy_mode_classify(h);

    groovy_mode_note_request(ms, wanted, now_ns);

    if (w == ms->pending_w && h == ms->pending_h) {
        if (ms->pending_streak < GROOVY_DIM_HYSTERESIS_FRAMES)
            ms->pending_streak++;
    } else {
        ms->pending_w      = w;
        ms->pending_h      = h;
        ms->pending_streak = 0u;
    }

    if (!cache->valid)
        return groovy_mode_announce(ms, cache, w, h, fps, GROOVY_MODE_REASON_FIRST);

    if (wanted != groovy_mode_of(cache)) {
        enum groovy_mode_action a = groovy_mode_guard(ms, groovy_mode_of(cache), wanted, now_ns);
        if (a == GROOVY_MODE_DROP_FROZEN)
            return a;
        return groovy_mode_announce(ms, cache, w, h, fps, GROOVY_MODE_REASON_SWITCH);
    }

    if (w != cache->width || h != cache->height) {
        if (ms->have_hint && w == ms->hint_w && h == ms->hint_h)
            return groovy_mode_announce(ms, cache, w, h, fps, GROOVY_MODE_REASON_DECLARED);
        if (ms->pending_streak >= GROOVY_DIM_HYSTERESIS_FRAMES)
            return groovy_mode_announce(ms, cache, w, h, fps, GROOVY_MODE_REASON_SETTLED);
    }

    return GROOVY_MODE_KEEP;
}

/* groovy_mode_frame_matches — does this frame's size match the last
 * announced cache? Used by the caller to decide whether to emit the frame
 * (whole or split into fields) or refuse it. A mismatch refused by
 * the guard counts as frozen_drops; any other mismatch counts as
 * dims_drops. */
static inline bool groovy_mode_frame_matches(struct groovy_mode_state *ms,
        const groovy_av_cache_t *cache, unsigned w, unsigned h, enum groovy_mode_action a)
{
    bool ok = cache->valid && cache->width == w && cache->height == h;
    if (!ok) {
        if (a == GROOVY_MODE_DROP_FROZEN)
            ms->frozen_drops++;
        else
            ms->dims_drops++;
    }
    return ok;
}

#endif /* GROOVY_MODE_H */
