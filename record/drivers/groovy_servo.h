/* SPDX-License-Identifier: GPL-3.0-or-later */
/* groovy_servo.h — clock servo for record_groovy: per-mode receiver
 * feed-forward, a ring of send timestamps, sub-frame error, a PI loop
 * bounded to the residual crystal drift alone.
 *
 *   - The term has TWO additive parts, in ppm: a feed-forward applied ON
 *     THE STEP at every receiver mode change (no ramp), and a servo that
 *     only closes on the residual crystal drift (bounded authority,
 *     bounded slope). The large step between 240p and 480i (~1900 ppm)
 *     never crosses the PI loop anymore -- that is exactly what an earlier
 *     revision of this file did, and it is why that revision's own tests
 *     could not pass.
 *   - The error is measured IN TIME, via a ring of send timestamps --
 *     never via the integer difference of frame counters, which quantizes
 *     and manufactures a ripple independent of the gain settings. The
 *     integer difference is now used only for re-anchoring.
 *
 * Header-only, same pattern as groovy_pacing.h: clock and log injectable
 * by macro BEFORE inclusion, calloc-zeroed state that is coherent by
 * construction (zero = without-master, term exactly 1.0), pure decision
 * logic testable from libgm/test/groovy_test_bridge.c without RetroArch
 * and without a socket.
 *
 * Mock clock: #define GROOVY_SERVO_MONO_NS_OVERRIDE 1, then provide your
 * own static inline groovy_servo_mono_ns() BEFORE including this header.
 *
 * Mock log: #define GROOVY_SERVO_LOG_HOOK your_fn (signature
 * void fn(const char *evt)) BEFORE including this header. Without the
 * hook, the default line goes to stderr, prefixed [groovy-servo] --
 * NEVER the prefix of the neighboring groovy_pacing.h file, nor that of
 * groovy-lat, which are distinct log families.
 *
 * Guard: this file consumes groovy_ack_state_t (groovy_pacing.h), which
 * must therefore be included before it. groovy_servo.h does not know
 * about gm.h.
 */

#ifndef GROOVY_SERVO_H
#define GROOVY_SERVO_H

#ifndef GROOVY_PACING_H
#  error "groovy_servo.h requires groovy_pacing.h to be included first"
#endif

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

/* ---------------------------------------------------------------------------
 * Monotonic clock, injectable for tests -- same body as
 * groovy_pacing_mono_ns (groovy_pacing.h).
 * ------------------------------------------------------------------------- */
#ifndef GROOVY_SERVO_MONO_NS_OVERRIDE
#  ifdef _WIN32
#    include <windows.h>
static inline uint64_t groovy_servo_mono_ns(void)
{
    static LARGE_INTEGER freq = { { 0, 0 } };
    LARGE_INTEGER counter;
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (uint64_t)((double)counter.QuadPart * 1.0e9
                      / (double)freq.QuadPart);
}
#  else
#    include <time.h>
static inline uint64_t groovy_servo_mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#  endif
#endif /* GROOVY_SERVO_MONO_NS_OVERRIDE */

/* ---------------------------------------------------------------------------
 * Log -- one line per event. Prefix [groovy-servo], never that of
 * groovy_pacing.h nor of groovy-lat (distinct log families).
 * ------------------------------------------------------------------------- */
static inline void groovy_servo_log(const char *evt)
{
#ifdef GROOVY_SERVO_LOG_HOOK
    GROOVY_SERVO_LOG_HOOK(evt);
#else
    fprintf(stderr, "[groovy-servo] %s\n", evt);
#endif
}

/* ---------------------------------------------------------------------------
 * GROOVY_SERVO setting -- on/off, same pattern as groovy_audio_mode_parse
 * (groovy_audio.h). Any unknown value falls back explicitly to OFF.
 * ------------------------------------------------------------------------- */
enum groovy_servo_mode {
    GROOVY_SERVO_OFF = 0,
    GROOVY_SERVO_ON  = 1,
    GROOVY_SERVO_FF  = 2  /* feed-forward only; the closed loop is disabled */
};

static inline enum groovy_servo_mode groovy_servo_mode_parse(const char *s)
{
    if (!s || s[0] == '\0')
        return GROOVY_SERVO_OFF;
    if (!strcmp(s, "on"))
        return GROOVY_SERVO_ON;
    if (!strcmp(s, "ff"))
        return GROOVY_SERVO_FF;
    return GROOVY_SERVO_OFF;
}

static inline const char *groovy_servo_mode_name(enum groovy_servo_mode m)
{
    switch (m) {
    case GROOVY_SERVO_ON: return "on";
    case GROOVY_SERVO_FF: return "ff";
    default:              return "off";
    }
}

/* Tuning rule.
 * A term of T ppm makes the frame count drift at k = 6e-5 frame/s per ppm
 * (60 frames/s x 1e-6). Ki acts on every ack, ~60 times per second, so Kp
 * compares to Ki x 60, NOT to Ki.
 *   natural frequency : omega = sqrt(6e-5 * 60 * Ki)
 *   damping           : zeta  = 6e-5 * Kp / (2 * omega)
 * TARGET zeta = 0.7.
 *
 * Tuning history, kept because the derivation matters: with Kp 700 /
 * Ki 0.25 (omega 0.0300 rad/s, zeta 0.700), the step scenario at 300 ppm
 * of residual peaks at 0.305 frame -- above the 0.3 threshold. The error
 * term, for a constant residual R, follows the impulse response of a
 * second-order system: its peak is PROPORTIONAL TO 1/omega at fixed zeta
 * (the normalised peak depends only on zeta; the amplitude depends on
 * 1/omega). So at constant zeta, raising omega (Kp and Ki together, Kp by
 * a factor c and Ki by c^2) reduces the peak without changing the shape
 * of the response. Retained: Kp 840 / Ki 0.36 (c = 1.2): omega =
 * 0.0360 rad/s, zeta = 0.700 (unchanged), time constant
 * 1/(zeta*omega) ~ 39.7 s. The ripple under jitter grows proportionally
 * to Kp (benchmark Kp x 1ms/16.69ms) but stays well below the 100 ppm
 * threshold (benchmark ~50 ppm at Kp 840, measured ~14 to 17 ppm in
 * practice). Ki 0.25 (the value before this pass) had already been
 * confirmed once; the rule above is applied a second time for the same
 * reason (zeta 0.7). */

/* DELIBERATE, TRACKED COUPLING: the four constants below are COPIED from
 * the MODELINE_240P_SR_NTSC_* and MODELINE_480I_NTSC_* constants of
 * receiver/modelines.h, which carries the same derivation and points back
 * here. If one changes, change the other; the startup
 * [groovy-servo] settings: ... ff_240p= ff_480i= line shows the values in
 * effect in every session log. */
#define GROOVY_SERVO_PCLOCK_HZ       48480000.0  /* receiver/modelines.h, shared 240p/480i */
#define GROOVY_SERVO_H_TOTAL              3088u  /* same, shared (mode-switch invariant) */
#define GROOVY_SERVO_V_TOTAL_240P          262u
#define GROOVY_SERVO_V_TOTAL_480I          525u  /* lines of the FULL frame (2 fields) */
#define GROOVY_SERVO_CADENCE_240P_HZ  59.921687  /* 48480000/(3088*262) */
#define GROOVY_SERVO_CADENCE_480I_HZ  59.807550  /* 48480000/(3088*525)*2 -- FIELDS */

/* Loop constants. */
#define GROOVY_SERVO_AUTHORITY_PPM       500.0   /* the SERVO only closes on the residual (~300 ppm worst case measured; ~50 ppm typical). 500, not 300: canceling a residual R needs servo = -R, and an authority equal to the expected residual saturates without converging. The step between modes is handled by the feed-forward. */
#define GROOVY_SERVO_SLOPE_MAX_PPM_S    100.0   /* crosses the authority in 5 s (500 ppm / 100 ppm/s) */
#define GROOVY_SERVO_KP_PPM_FRAME       840.0   /* see the tuning-rule block above: zeta 0.7, omega raised to 0.036 rad/s to bring the step peak under 0.3 frame */
#define GROOVY_SERVO_KI_PPM_FRAME         0.36  /* same: omega 0.036 rad/s with Kp 840 for zeta 0.7 */
#define GROOVY_SERVO_SETPOINT_FRAMES      2.25  /* floor measured at 29.156 ms (center of the low
                                           * cluster over a full session) / period 16.68845 ms
                                           * (GROOVY_SERVO_CADENCE_240P_HZ) = 1.7471 frame;
                                           * + 0.5 frame (loop margin) = 2.2471; rounded to 2.25
                                           * (log format %.2ftr, rounding gap 0.0029 frame =
                                           * 0.048 ms, 30x under the dispersion of the
                                           * measurement itself, 3.18 ms of spread over the low
                                           * cluster). Latency cost: +8.34 ms against the
                                           * physical floor (1.747 frame), -4.22 ms against the
                                           * measured average (2.503 frame). Never re-derived by
                                           * threshold since. */
#define GROOVY_SERVO_REANCHOR_FRAMES    30.0   /* beyond this, the counters are no longer comparable */
#define GROOVY_SERVO_FRESH_NS      50000000ull  /* same window as groovy_pacing.h */
#define GROOVY_SERVO_STALE_NS   2000000000ull  /* same threshold */
#define GROOVY_SERVO_RING              128u   /* >= 120 frames = the 2 s stale threshold; 64 would silently wrap a legitimate ack */
#define GROOVY_SERVO_RING_MASK       127u

/* The error fed into the loop is the AVERAGE of the fresh acks' errors
 * over the last second. The smoothing of the TERM (slope, authority)
 * stays -- this filter is additive, it replaces nothing.
 * Reason: the ~10 ms steps from the sensor (0.6 frame) under a Kp of
 * 840 ppm/frame saturate the loop on noise alone. Cost: 0.5 s of group
 * delay, i.e. 1.03 deg of phase margin at omega = 0.036 rad/s out of
 * ~65 deg -- the gains do not move. */
#define GROOVY_SERVO_WINDOW_NS  1000000000ull  /* one second of acks */
#define GROOVY_SERVO_WINDOW           128u     /* >= 60 acks/s, 2x margin */
#define GROOVY_SERVO_WINDOW_MASK    127u

/* ---------------------------------------------------------------------------
 * States and freeze.
 * ------------------------------------------------------------------------- */
enum groovy_servo_state {
    GROOVY_SERVO_STATE_WITHOUT_MASTER = 0,
    GROOVY_SERVO_STATE_WAITING     = 1,
    GROOVY_SERVO_STATE_FRESH       = 2,
    GROOVY_SERVO_STATE_STALE      = 3
};

static inline const char *groovy_servo_state_name(int state)
{
    switch (state) {
    case GROOVY_SERVO_STATE_WITHOUT_MASTER: return "without-master";
    case GROOVY_SERVO_STATE_WAITING:     return "waiting";
    case GROOVY_SERVO_STATE_FRESH:       return "fresh";
    case GROOVY_SERVO_STATE_STALE:      return "stale";
    default:                            return "unknown";
    }
}

enum groovy_servo_freeze {
    GROOVY_SERVO_FREEZE_NONE         = 0,
    GROOVY_SERVO_FREEZE_PAUSE       = 1,
    GROOVY_SERVO_FREEZE_MENU        = 2,
    GROOVY_SERVO_FREEZE_FASTFWD      = 3,
    GROOVY_SERVO_FREEZE_SLOWMO     = 4,
    GROOVY_SERVO_FREEZE_REWIND = 5
};

static inline const char *groovy_servo_freeze_name(int freeze)
{
    switch (freeze) {
    case GROOVY_SERVO_FREEZE_NONE:         return "none";
    case GROOVY_SERVO_FREEZE_PAUSE:       return "pause";
    case GROOVY_SERVO_FREEZE_MENU:        return "menu";
    case GROOVY_SERVO_FREEZE_FASTFWD:      return "fastfwd";
    case GROOVY_SERVO_FREEZE_SLOWMO:     return "slowmo";
    case GROOVY_SERVO_FREEZE_REWIND: return "rewind";
    default:                           return "unknown";
    }
}

/* ---------------------------------------------------------------------------
 * Per-instance state.
 *
 * calloc-allocated in groovy_new (like groovy_ack_state_t), zero is a
 * coherent fallback (term exactly 1.0) -- callers must NOT memset to
 * non-zero.
 *
 * term_ppm is NOT a field: it is ff_ppm + servo_ppm, returned by a
 * function, so that no code can write one while forgetting the other.
 * ------------------------------------------------------------------------- */
typedef struct { uint32_t frame_id; uint64_t send_ns; } groovy_servo_send_t;

/* One sample of the one-second window -- timestamp of the ack (not of
 * the send) and RAW error measured on that pass. */
typedef struct { uint64_t t_ns; double error; } groovy_servo_sample_t;

typedef struct {
    double   ff_ppm;              /* feed-forward of the current mode, applied on the step */
    double   residual_ppm;          /* INTEGRAL part of the servo, bounded to +/- authority */
    double   servo_ppm;           /* applied servo part (integral + proportional, bounded, slope-bounded) */
    double   period_ns;          /* period of the current mode, in ns; 0 = no known mode */
    double   offset_frames;     /* re-anchor offset: subtracted from the error */
    double   error_frames;       /* last FILTERED error, the one fed into the PI */
    double   raw_error_frames; /* last RAW error (before filtering), for the log */
    double   latency_ns;          /* last measured latency, for the log */
    uint64_t last_step_ns;      /* timestamp of the last applied step (slope bound) */
    uint32_t reanchors;
    uint32_t ring_misses;      /* acks with no retrievable timestamp */
    uint32_t toggles;            /* feed-forward changes */
    uint32_t samples;             /* successful passes of branch (8), for the trace */
    uint32_t window_n;           /* number of samples < 1 s in the current window */
    uint32_t window_head;        /* next write index into window[] */
    int      state;                /* enum groovy_servo_state */
    int      freeze;                 /* enum groovy_servo_freeze, as passed by the caller */
    int      reanchor_due;        /* 1 when a freeze or a toggle has just ended */
    int      stale_logged;        /* 1 after the current stale episode has been logged */
    groovy_servo_send_t ring[GROOVY_SERVO_RING];
    groovy_servo_sample_t   window[GROOVY_SERVO_WINDOW]; /* the one-second window */
} groovy_servo_state_t;

/* Cadence of a RECEIVER mode, in fields per second. h_total/v_total zero
 * -> 0.0 (neutral fallback).
 *
 * Proof of the factor of 2 under interlace: v_total under interlace
 * counts the lines of the FULL frame (both fields), not of one field.
 * On the receiver's modelines (pclock_hz=48480000, h_total=3088,
 * shared): 48480000/(3088*525) = 29.9038 Hz -- exactly the FRAME
 * cadence announced by the PCLOCK re-tune banner of
 * receiver/modelines.h ("frame rate 29.9038 Hz"). The FIELD cadence,
 * the one that matters here (one core frame per field), is double
 * that: 59.8076 Hz.
 *
 * GAP BETWEEN DESIGN NOTES AND CODE (flagged, not resolved here): this
 * function applies to the RECEIVER's modelines, COPIED into constants
 * -- NOT to the modeline that the emitter sets in CMD_SWITCHRES. Two
 * reasons, verified in the code:
 *   (1) the receiver only opens the wire's modeline in legacy_modeset;
 *       receiver/blit.c::blit_runtime_modeset delegates to
 *       blit_regime_toggle and applies the two FIXED entries of
 *       receiver/modelines.h on the production path
 *       (legacy_modeset == 0) -- CMD_SWITCHRES only selects the
 *       REGIME;
 *   (2) even if it could, the quotient would yield the CORE's cadence,
 *       not the receiver's: groovy_modeline.h computes
 *       pclock_mhz = h_total * v_total * fps / 1e6 (fps = the core's
 *       native cadence), so pclock/(h_total*v_total) returns that same
 *       fps -- a circular calculation that would cancel RetroArch's
 *       skew instead of locking the core to the tube.
 * Do not "simplify" this later by reading the wire's modeline.
 * ------------------------------------------------------------------------- */
static inline double groovy_servo_cadence_hz(double pclock_hz, unsigned h_total,
                                             unsigned v_total, int interlace)
{
    if (h_total == 0u || v_total == 0u)
        return 0.0;
    return pclock_hz / ((double)h_total * (double)v_total) * (interlace ? 2.0 : 1.0);
}

/* Feed-forward in ppm of core SPEED: how much faster (positive) or
 * slower (negative) the core must run than its nominal cadence to
 * track the receiver's mode.
 * video_refresh_rate_hz <= 0 or cadence_hz <= 0 -> 0.0 (neutral
 * fallback: no known mode, no feed-forward).
 * Written in NEGATIVE form (same idiom as the audio_driver_set_groovy_term
 * and groovy_resample_set_ratio accessors), to also catch NaN -- any
 * comparison against NaN is false, so the earlier positive form
 * (<= 0.0) would have let a NaN video_refresh_rate_hz straight through
 * the division. Masked downstream by isfinite() in groovy_servo_term(),
 * but ff_ppm itself and the log lines would have carried NaN without
 * saying so. */
static inline double groovy_servo_ff_ppm(double cadence_hz, double video_refresh_rate_hz)
{
    if (!(video_refresh_rate_hz > 0.0) || !(cadence_hz > 0.0))
        return 0.0;
    return (cadence_hz / video_refresh_rate_hz - 1.0) * 1e6;
}

/* Total term in ppm (speed), ff + servo. */
static inline double groovy_servo_term_ppm(const groovy_servo_state_t *s)
{
    return s->ff_ppm + s->servo_ppm;
}

/* Factor applied to src_ratio_curr.
 *
 * SPEED convention here (term_ppm), ratio convention on the SRC side --
 * the core's speed varies as 1/T: multiplying src_ratio_curr by T
 * changes the core's speed by 1/T. An inversion here would make the
 * core SPEED UP instead of SLOWING DOWN when the tube is slower -- the
 * most costly bug this file could have, guarded by a named test
 * (test_terme_est_l_inverse_de_la_vitesse), not by this comment alone.
 *
 * Guard: non-finite or zero denominator -> exactly 1.0.
 * ------------------------------------------------------------------------- */
static inline double groovy_servo_term(const groovy_servo_state_t *s)
{
    double denom = 1.0 + groovy_servo_term_ppm(s) * 1e-6;
    if (!isfinite(denom) || denom == 0.0)
        return 1.0;
    return 1.0 / (1.0 + groovy_servo_term_ppm(s) * 1e-6);
}

/* Resets the state to zero (without-master fallback, term 1.0). */
static inline void groovy_servo_reset(groovy_servo_state_t *s)
{
    memset(s, 0, sizeof(*s));
}

/* Timestamps the send of a frame in the ring. Called at the moment the
 * frame leaves for the master. */
static inline void groovy_servo_note_send(groovy_servo_state_t *s, uint32_t frame_id,
                                           uint64_t now_ns)
{
    groovy_servo_send_t *e = &s->ring[frame_id & GROOVY_SERVO_RING_MASK];
    e->frame_id = frame_id;
    e->send_ns = now_ns;
}

/* Applies the feed-forward of the current mode, ON THE STEP (no ramp --
 * that is the point). residual_ppm and servo_ppm are PRESERVED: the
 * crystal residual does not change just because the mode changes.
 * Strict no-op if ff_ppm and period_ns have barely moved (< 0.5 ppm,
 * < 1000 ns): the reannounce paths re-apply the SAME mode on every loss
 * of sync, and must not disturb anything -- re-anchoring on those would
 * make the servo restart from zero for nothing. */
static inline void groovy_servo_set_feed_forward(groovy_servo_state_t *s, double ff_ppm,
                                                  double period_ns)
{
    double d_ff      = ff_ppm - s->ff_ppm;
    double d_period = period_ns - s->period_ns;

    if (d_ff < 0.0) d_ff = -d_ff;
    if (d_period < 0.0) d_period = -d_period;

    if (d_ff < 0.5 && d_period < 1000.0)
        return; /* strict no-op */

    s->ff_ppm       = ff_ppm;
    s->period_ns   = period_ns;
    s->reanchor_due = 1;
    s->toggles++;
    groovy_servo_log("toggle");
}

/* Clears the one-second window. Called on every discontinuity
 * (re-anchor, without-master) -- an un-cleared window would manufacture
 * a one-second transient of a false average after every toggle. */
static inline void groovy_servo_window_clear(groovy_servo_state_t *s)
{
    memset(s->window, 0, sizeof(s->window));
    s->window_head = 0u;
    s->window_n    = 0u;
}

/* Pushes a raw error into the window and returns the average of the
 * samples younger than one second. Samples are written in time order:
 * we walk from the most recent back to the oldest and STOP at the
 * first one that is too old -- everything after it is too, by
 * construction. n is always at least 1 (the sample just written has
 * zero age). */
static inline double groovy_servo_window_push(groovy_servo_state_t *s,
                                                 uint64_t now_ns, double error)
{
    groovy_servo_sample_t *e = &s->window[s->window_head & GROOVY_SERVO_WINDOW_MASK];
    unsigned i, n = 0u;
    double   somme = 0.0;

    e->t_ns   = now_ns;
    e->error = error;
    s->window_head++;

    for (i = 0u; i < GROOVY_SERVO_WINDOW; i++) {
        const groovy_servo_sample_t *v =
            &s->window[(s->window_head - 1u - i) & GROOVY_SERVO_WINDOW_MASK];
        if (v->t_ns == 0ull || now_ns < v->t_ns
            || (now_ns - v->t_ns) >= GROOVY_SERVO_WINDOW_NS)
            break;
        somme += v->error;
        n++;
    }
    s->window_n = n;
    return (n > 0u) ? (somme / (double)n) : error;
}

/* Post-poll loop -- pure decision, called once per processed ack (same
 * call site as compute_vsync_target_from_acks_post_poll; never a
 * follower).
 *
 * `mode` replaces the older boolean `servo_actif` (values of
 * `enum groovy_servo_mode`; an `int` so the bridge's ABI does not move
 * -- GROOVY_SERVO_OFF=0 and GROOVY_SERVO_ON=1 keep their values, so any
 * caller that used to pass 0/1 still behaves identically). In `ff`,
 * branches (9)-(10) (residual/servo) do NOT run: the servo part stays
 * pinned at zero, never a leftover -- see the end of this function.
 * ------------------------------------------------------------------------- */
static inline void groovy_servo_update_post_poll(groovy_servo_state_t *s,
                                                  const groovy_ack_state_t *ack,
                                                  uint32_t emitted, uint64_t now_ns,
                                                  int mode, int freeze)
{
    s->freeze = freeze;

    /* (2) without master: term exactly 1.0. Unchanged for OFF -- setting
     * absent means identical to today. */
    if (mode == GROOVY_SERVO_OFF || ack == NULL) {
        s->state                = GROOVY_SERVO_STATE_WITHOUT_MASTER;
        s->ff_ppm               = 0.0;
        s->residual_ppm           = 0.0;
        s->servo_ppm            = 0.0;
        s->error_frames        = 0.0;
        s->raw_error_frames  = 0.0;
        s->latency_ns           = 0.0;
        s->last_step_ns       = now_ns;
        groovy_servo_window_clear(s);
        return;
    }

    /* (3) master declared, no ack received yet: waiting. ff_ppm is
     * PRESERVED -- a mode can be known before the first ack. */
    if (!ack->has_any_ack) {
        s->state      = GROOVY_SERVO_STATE_WAITING;
        s->servo_ppm = 0.0;
        return;
    }

    /* (4) stale ack (>= 2 s): term frozen, logged once per episode. */
    {
        uint64_t age_ns = now_ns - ack->last_ack_mono_ns;
        if (age_ns >= GROOVY_SERVO_STALE_NS) {
            s->state = GROOVY_SERVO_STATE_STALE;
            if (!s->stale_logged) {
                groovy_servo_log("stale");
                s->stale_logged = 1;
            }
            return;
        }
    }

    /* (5) fresh ack. */
    s->state         = GROOVY_SERVO_STATE_FRESH;
    s->stale_logged = 0;

    /* (6) freeze (pause/menu/fast-forward/slow-motion/rewind):
     * anti-windup, nothing moves, the return will arm a re-anchor. */
    if (freeze != GROOVY_SERVO_FREEZE_NONE) {
        s->reanchor_due = 1;
        return;
    }

    /* (7) re-anchor on the INTEGER gap -- the ONLY remaining use of the
     * integer gap; it never again enters the integral or the
     * proportional term. */
    {
        double gap = (double)(int32_t)(emitted - ack->last_frame);
        if (gap < 0.0)
            gap = -gap;
        if (s->reanchor_due
            || gap > (GROOVY_SERVO_REANCHOR_FRAMES + GROOVY_SERVO_SETPOINT_FRAMES)) {
            s->reanchor_due    = 0;
            s->reanchors++;
            s->offset_frames = 0.0;
            /* Clear BEFORE logging -- a discontinuity makes the errors
             * from before incomparable to those from after; averaging
             * them together would manufacture a one-second transient of
             * a false average after every toggle and after every pause
             * exit. */
            groovy_servo_window_clear(s);
            groovy_servo_log("reanchor");
            return; /* no step this round -- the measurement is not
                      * reliable at the moment of a discontinuity. */
        }
    }

    /* (8) sub-frame measurement, via the ring of send timestamps. */
    {
        groovy_servo_send_t *e = &s->ring[ack->last_frame & GROOVY_SERVO_RING_MASK];
        double raw_error;
        double error;

        if (e->frame_id != ack->last_frame
            || e->send_ns == 0ull
            || now_ns < e->send_ns
            || (now_ns - e->send_ns) >= GROOVY_SERVO_STALE_NS
            || s->period_ns <= 0.0) {
            s->ring_misses++;
            return; /* never an invented latency */
        }

        s->latency_ns = (double)(now_ns - e->send_ns);
        raw_error  = s->latency_ns / s->period_ns
                        - GROOVY_SERVO_SETPOINT_FRAMES - s->offset_frames;
        s->raw_error_frames = raw_error;

        /* The error fed into the PI is the one-second sliding AVERAGE of
         * the raw value -- this counter only moves on a SUCCESSFUL pass
         * of this branch (8), never on a re-anchor, a freeze, or a ring
         * miss. */
        error = groovy_servo_window_push(s, now_ns, raw_error);
        s->error_frames = error;
        s->samples++;

        /* (9)-(10) PI, SPEED convention -- the two parts SUBTRACT:
         * positive error = the core is AHEAD => it must SLOW DOWN =>
         * term_ppm GOES DOWN. ONLY in ON mode: in `ff`, the servo part
         * stays pinned at zero -- the total term IS the feed-forward
         * alone, never a leftover of residual or slope. */
        if (mode == GROOVY_SERVO_ON) {
            double target;

            s->residual_ppm -= GROOVY_SERVO_KI_PPM_FRAME * error;
            if (s->residual_ppm >  GROOVY_SERVO_AUTHORITY_PPM) s->residual_ppm =  GROOVY_SERVO_AUTHORITY_PPM;
            if (s->residual_ppm < -GROOVY_SERVO_AUTHORITY_PPM) s->residual_ppm = -GROOVY_SERVO_AUTHORITY_PPM;

            target = s->residual_ppm - GROOVY_SERVO_KP_PPM_FRAME * error;
            if (target >  GROOVY_SERVO_AUTHORITY_PPM) target =  GROOVY_SERVO_AUTHORITY_PPM;
            if (target < -GROOVY_SERVO_AUTHORITY_PPM) target = -GROOVY_SERVO_AUTHORITY_PPM;

            /* (10) slope bound. */
            {
                int64_t dt_ns = (int64_t)now_ns - (int64_t)s->last_step_ns;
                double  dt_s  = (double)dt_ns * 1e-9;
                double  step_max;
                double  delta;

                if (dt_s < 0.0) dt_s = 0.0;
                if (dt_s > 1.0) dt_s = 1.0;
                step_max = GROOVY_SERVO_SLOPE_MAX_PPM_S * dt_s;

                delta = target - s->servo_ppm;
                if (delta >  step_max) delta =  step_max;
                if (delta < -step_max) delta = -step_max;

                s->servo_ppm += delta;
                if (s->servo_ppm >  GROOVY_SERVO_AUTHORITY_PPM) s->servo_ppm =  GROOVY_SERVO_AUTHORITY_PPM;
                if (s->servo_ppm < -GROOVY_SERVO_AUTHORITY_PPM) s->servo_ppm = -GROOVY_SERVO_AUTHORITY_PPM;
                s->last_step_ns = now_ns;
            }
        } else {
            /* ff mode: the servo part stays pinned at zero, never a
             * leftover from a previous state -- last_step_ns still
             * tracks time so that an eventual return to on mode does not
             * see a huge dt_ns and a slope that would saturate on the
             * very first step. */
            s->residual_ppm     = 0.0;
            s->servo_ppm      = 0.0;
            s->last_step_ns = now_ns;
        }
    }

    /* (11) numeric guard: nothing non-finite reaches src_ratio_curr. */
    if (!isfinite(s->servo_ppm) || !isfinite(s->residual_ppm)) {
        s->servo_ppm  = 0.0;
        s->residual_ppm = 0.0;
        groovy_servo_log("term_not_finite");
    }
}

#endif /* GROOVY_SERVO_H */
