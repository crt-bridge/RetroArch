/* SPDX-License-Identifier: GPL-3.0-or-later */
/* groovy_pacing.h — ACK-pacing state machine for record_groovy.
 *
 * Design:
 *   - Heartbeat is in libgm (opt-in, 100 ms idle CMD_GET_STATUS)
 *   - ACK consumption is here in record_groovy (caller-owned)
 *   - 50 ms-fresh window:    use last_vcount as the vsync_target unchanged
 *   - 50 ms-2 s stale:       still use last_vcount, no logging
 *   - >2 s zero ACKs:        fall back to local-clock estimate, log `ack_lost` ONCE
 *
 * Header-only so the test bridge (libgm/test/groovy_test_bridge.c) and the
 * runtime (emitter/retroarch/src/record_groovy.c) share the same logic.
 *
 * Mock clock injection for tests:
 *   Before #include-ing this header, #define GROOVY_PACING_MONO_NS_OVERRIDE 1
 *   and provide a file-scope static inline groovy_pacing_mono_ns() that returns
 *   a uint64_t. The real implementation uses QueryPerformanceCounter under
 *   _WIN32, clock_gettime(CLOCK_MONOTONIC) elsewhere.
 *
 * Log hook injection for tests:
 *   Before #include-ing this header, #define GROOVY_PACING_LOG_HOOK your_fn
 *   where your_fn has signature void fn(const char *event). Without the hook,
 *   the default implementation writes to stderr.
 *
 * STRIDE compliance:
 *   T-3-05 — DoS (daemon stops ACKs): non-blocking gm_recv_status(timeout=0);
 *             freshness windows keep record_groovy from stalling.
 *   T-3-15 — Repudiation (log spam): ack_lost_logged flag fires log ONCE per
 *             timeout-window entry; Test 5 asserts log count == 1.
 *   T-3-16 — Stale vcount across daemon restarts: bounded by 2 s window; new
 *             ACK on restart overwrites immediately. Accepted risk.
 *
 * Guard: this header requires gm.h to be included first (for gm_handle and
 * gm_status). A build error occurs if GM_H is not defined when this is included.
 */

#ifndef GROOVY_PACING_H
#define GROOVY_PACING_H

#ifndef GM_H
#  error "groovy_pacing.h requires gm.h to be included first"
#endif

#include <stdint.h>
#include <stdio.h>   /* fputs / stderr for default log fallback */

/* ---------------------------------------------------------------------------
 * Monotonic clock — nanosecond resolution.
 *
 * Real implementation: Win32 QueryPerformanceCounter, or POSIX
 * clock_gettime(CLOCK_MONOTONIC).
 * Test override: define GROOVY_PACING_MONO_NS_OVERRIDE 1 before including
 * this header, then provide your own groovy_pacing_mono_ns() as a static
 * inline (it will shadow this block).
 * ------------------------------------------------------------------------- */
#ifndef GROOVY_PACING_MONO_NS_OVERRIDE
#  ifdef _WIN32
#    include <windows.h>
static inline uint64_t groovy_pacing_mono_ns(void)
{
    static LARGE_INTEGER freq = { { 0, 0 } };
    LARGE_INTEGER counter;
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    /* Multiply first by 1e9 (as double) then cast — avoids integer overflow
     * for counter values that approach LLONG_MAX at high QPC frequencies. */
    return (uint64_t)((double)counter.QuadPart * 1.0e9
                      / (double)freq.QuadPart);
}
#  else
#    include <time.h>
static inline uint64_t groovy_pacing_mono_ns(void)
{
    /* LITERAL transposition of gm_mono_ns (libgm/src/gm_proto.c), which
     * declares itself a deliberate copy of this same block and runs on
     * the Linux build host. CLOCK_MONOTONIC, never its "_RAW" variant
     * (Linux-specific): the latter would break macOS with nothing to
     * gain over intervals of a few seconds.
     * No feature-test macro is set here: the fork's Makefile already
     * adds -D_GNU_SOURCE and -std=gnu99 to every C translation unit. */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#  endif
#endif /* GROOVY_PACING_MONO_NS_OVERRIDE */

/* ---------------------------------------------------------------------------
 * Log hook — fires at most once per 2 s timeout-window entry.
 *
 * Default: stderr line. Override with GROOVY_PACING_LOG_HOOK your_fn.
 * your_fn signature: void fn(const char *event_name)
 * ------------------------------------------------------------------------- */
static inline void groovy_pacing_log_ack_lost(void)
{
#ifdef GROOVY_PACING_LOG_HOOK
    GROOVY_PACING_LOG_HOOK("ack_lost");
#else
    fputs("[groovy_pacing] ack_lost\n", stderr);
#endif
}

/* ---------------------------------------------------------------------------
 * Local-clock vsync estimate — fallback when daemon is unreachable.
 *
 * Phase 3 MVP: return 0. The daemon tolerates vsync=0 (renders at emulator
 * pace). Phase 4 jitter tuning may upgrade this to a wall-clock-derived
 * raster-line estimate.
 * ------------------------------------------------------------------------- */
static inline uint16_t groovy_pacing_local_clock_estimate(void)
{
    return 0u;
}

/* ---------------------------------------------------------------------------
 * groovy_ack_state_t — per-instance state for the ACK pacing machine.
 *
 * Lifetime: embedded in groovy_state_t (allocated by groovy_new via calloc,
 * so zero-initialised by construction). Callers must NOT memset to non-zero.
 * ------------------------------------------------------------------------- */
typedef struct {
    uint16_t last_vcount;       /* vcount from the most recent valid ACK */
    uint64_t last_ack_mono_ns;  /* mono timestamp of last successful ACK */
    int      ack_lost_logged;   /* 1 after the first >2 s log event fires */
    int      has_any_ack;       /* 1 after the first ACK ever received */

    /* Latency instrumentation -- Point A. Both round-tripped straight
     * from the 13-byte status ACK (gm_status, see gm.h): offset 0 = last
     * REASSEMBLED frame (receiver net thread), offset 6 = last DISPLAYED
     * frame (receiver blit thread). Purely additive — no existing
     * field's write site changes. */
    uint32_t last_frame_echo;   /* ACK offset 0: last REASSEMBLED frame */
    uint32_t last_frame;        /* ACK offset 6: last DISPLAYED frame    */

    uint8_t  last_flags;        /* ACK offset 12: the flag byte, of which FLAG_VGA_F1
                                 * (0x20) is the parity of last_frame. This is the F
                                 * in field(n) = F XOR ((n-G) mod 2).
                                 * Zero before any ACK -- a coherent fallback, not a
                                 * special case: the structure is calloc-allocated in
                                 * groovy_new, and field(0) = 0 XOR (0 mod 2) = 0. */
} groovy_ack_state_t;

/* ---------------------------------------------------------------------------
 * compute_vsync_target_from_acks_post_poll — freshness logic only.
 *
 * Assumes any incoming ACK has already been applied to *st by the caller.
 * Testable without a real gm_handle (test bridge calls this directly after
 * injecting a synthetic ACK via test_pacing_inject_ack).
 *
 * Freshness windows:
 *   age < 50 ms       → fresh: return last_vcount
 *   50 ms ≤ age < 2 s → stale-but-tolerable: return last_vcount
 *   age ≥ 2 s         → lost: log once, return local-clock estimate
 * ------------------------------------------------------------------------- */
static inline uint16_t
compute_vsync_target_from_acks_post_poll(groovy_ack_state_t *st)
{
    if (!st->has_any_ack)
        return groovy_pacing_local_clock_estimate();

    uint64_t now    = groovy_pacing_mono_ns();
    uint64_t age_ns = now - st->last_ack_mono_ns;

    /* Fresh window: 0 – 50 ms */
    if (age_ns < 50ull * 1000000ull)
        return st->last_vcount;

    /* Stale-but-tolerable window: 50 ms – 2 s */
    if (age_ns < 2ull * 1000000000ull)
        return st->last_vcount;

    /* Lost window: >2 s — log once, fall back */
    if (!st->ack_lost_logged) {
        groovy_pacing_log_ack_lost();
        st->ack_lost_logged = 1;
    }
    return groovy_pacing_local_clock_estimate();
}

/* ---------------------------------------------------------------------------
 * compute_vsync_target_from_acks — full per-frame entry point.
 *
 * Polls gm_recv_status(timeout_ms=0) non-blocking once per frame.
 * If a new ACK arrives, updates st and resets the lost-log flag.
 * Then delegates to compute_vsync_target_from_acks_post_poll for the
 * freshness decision.
 *
 * Called from groovy_push_video (record_groovy.c) once per rendered frame.
 * Never blocks — gm_recv_status with timeout=0 is non-blocking by contract.
 * ------------------------------------------------------------------------- */
static inline uint16_t
compute_vsync_target_from_acks(gm_handle *h, groovy_ack_state_t *st)
{
    gm_status s;
    if (gm_recv_status(h, &s, 0) == 0) {
        st->last_vcount      = s.vcount;
        st->last_ack_mono_ns = groovy_pacing_mono_ns();
        st->ack_lost_logged  = 0;
        st->has_any_ack      = 1;
        /* Point A: retain both frame counters from this ACK. */
        st->last_frame_echo  = s.frame_echo;
        st->last_frame       = s.frame;
        /* F and G MUST come from the SAME status packet -- the receiver
         * publishes them in a single atomic gesture (page_flip_handler in
         * receiver/blit.c, "Signal net thread to send an ACK" block).
         * Setting them in this same block isn't a convenience, it's the
         * correctness requirement. */
        st->last_flags       = s.flags;
    }
    return compute_vsync_target_from_acks_post_poll(st);
}

/* ---------------------------------------------------------------------------
 * groovy_ack_link -- per-receiver ack_back/ack_lost transition tracking.
 * Additive: groovy_ack_state_t and the two functions above are unchanged
 * by this block.
 *
 * WAN-05 requires one groovy_ack_state_t per receiver (master + followers).
 * Only the master's feeds compute_vsync_target_from_acks_post_poll (pacing);
 * a follower's ack_state exists purely for its own telemetry ([groovy-lat])
 * and its own reannounce budget. This block adds the event the
 * fixed pacing state machine above does not track: the TRANSITION from
 * "not synced" to "synced", one line per episode (ack_back), and the
 * mirror event when a receiver goes silent (ack_lost, same 2 s threshold
 * as the lost window of compute_vsync_target_from_acks_post_poll).
 * ------------------------------------------------------------------------- */

/* same threshold as the lost window of compute_vsync_target_from_acks_post_poll */
#define GROOVY_ACK_LOST_NS 2000000000ull

/* Minimal per-receiver link state. calloc-zeroed -> "not synced", exactly
 * like groovy_ack_state_t above. */
struct groovy_ack_link {
    int synced;
    int lost_logged;
};

/* groovy_ack_link_on_status -- call once per status drain for THIS
 * receiver, with the frame_echo field of the status just consumed.
 *
 * Trigger: frame_echo != 0u -- the exact condition of branch (0) of
 * groovy_reannounce_should_fire: the branch that reconstitutes the
 * reannounce budget, and the one that never fires for a peer stuck
 * sending frame_echo == 0 "pings" every 250 ms forever without ever
 * recovering its geometry.
 *
 * Returns 1 EXACTLY ONCE per "not synced" -> "synced" transition, including
 * the very first one of the session -- no ack_lost is required beforehand.
 * Reason: ack_lost freshness also counts frame_echo == 0 pings, so a
 * peer stuck in that failure mode (pinging every 250 ms) never enters
 * the >2 s silence that ack_lost requires. Requiring a prior ack_lost
 * would make ack_back silent for exactly the case it exists to surface.
 *
 * frame_echo == 0u always resets synced to 0 (the peer lost its geometry,
 * or never had it) -- same tolerance for the very first frame id as the
 * rest of this file (frame 0 self-reports frame_echo == 0; the sync shows
 * up on the NEXT status).
 *
 * lost_logged is reset unconditionally on every status, synced or not:
 * any status at all proves the peer is talking, so a future genuine
 * silence (via groovy_ack_link_on_tick) is free to log again. */
static inline int groovy_ack_link_on_status(struct groovy_ack_link *l, uint32_t frame_echo)
{
    l->lost_logged = 0;

    if (frame_echo != 0u) {
        if (!l->synced) {
            l->synced = 1;
            return 1;
        }
        return 0;
    }

    l->synced = 0;
    return 0;
}

/* groovy_ack_link_on_tick -- call once per frame (or once per telemetry
 * window) with THIS receiver's groovy_ack_state_t, to detect the silence
 * that groovy_ack_link_on_status alone cannot: no status at all, ever.
 *
 * ack_lost meaning is unchanged from the rest of this file: no status,
 * whatsoever, for >= 2 s. One line per silence episode (lost_logged gates
 * repeats), synced dropped to 0 on the same event. */
static inline int groovy_ack_link_on_tick(struct groovy_ack_link *l,
                                          const groovy_ack_state_t *ack, uint64_t now_ns)
{
    if (!ack->has_any_ack)
        return 0;
    if (now_ns - ack->last_ack_mono_ns < GROOVY_ACK_LOST_NS)
        return 0;
    if (l->lost_logged)
        return 0;

    l->lost_logged = 1;
    l->synced      = 0;
    return 1;
}

/* groovy_lat_classify -- [groovy-lat] samples only count on a
 * synced receiver (l->synced) that has actually sent something this
 * window (has_sent) -- otherwise a receiver that never answers would
 * still produce in_net = sent - 0 and pass for a valid measurement (the
 * exact false-positive this function exists to close).
 *
 * use_shown selects the master's model (frame_echo = reassembled,
 * "shown" = displayed, both tracked by the receiver daemon) versus a
 * follower's (its status carries only its own frame counter in `frame`,
 * WAN-15 -- not comparable to the master's `shown`, hence in_queue = 0
 * and in_flight collapses to in_net for a follower).
 *
 * Returns 0 if not eligible to sample (not synced, or nothing sent this
 * window); -1 if any of the three values is out of the sane range
 * [0, 1000] (anomaly -- same bound as the existing s_lat_anomalies guard
 * in record_groovy.c); 1 otherwise, with *in_net, *in_queue, *in_flight
 * filled. */
static inline int groovy_lat_classify(const struct groovy_ack_link *l, int has_sent,
                                       uint32_t sent, uint32_t echo, uint32_t shown,
                                       int use_shown,
                                       int64_t *in_flight, int64_t *in_net, int64_t *in_queue)
{
    if (!l->synced || !has_sent)
        return 0;

    *in_net = (int64_t)sent - (int64_t)echo;
    if (use_shown) {
        *in_queue  = (int64_t)echo - (int64_t)shown;
        *in_flight = (int64_t)sent - (int64_t)shown;
    } else {
        *in_queue  = 0;
        *in_flight = *in_net;
    }

    if (*in_net    < 0 || *in_net    > 1000
        || *in_queue  < 0 || *in_queue  > 1000
        || *in_flight < 0 || *in_flight > 1000)
        return -1;

    return 1;
}

#endif /* GROOVY_PACING_H */
