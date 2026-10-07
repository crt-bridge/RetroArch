/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef GROOVY_OPEN_KNOCK_H
#define GROOVY_OPEN_KNOCK_H

/* Open knock -- a pure decision: when to re-send CMD_INIT + CMD_SWITCHRES
 * to a receiver that has not opened its session.
 *
 * A receiver started AFTER the emitter never saw the session opening go by.
 * The status-driven re-announce (groovy_reannounce.h) is only evaluated when
 * a status arrives, and a receiver with no session sends none: without this
 * decision, it stays dark until the emitter restarts. This one is evaluated
 * on EVERY video turn, per receiver, whether or not a status came in.
 *
 * A synchronized flag, a last-event instant and a clock as input, one
 * boolean as output. No dependency on RetroArch or the network: the clock is
 * a number supplied by the caller, which is what keeps the function
 * testable over hours of simulated time.
 *
 * Schedule: the interval between knocks doubles from
 * GROOVY_OPEN_KNOCK_FIRST_MS and is capped at GROOVY_OPEN_KNOCK_CAP_MS --
 * 1, 2, 4, 8, 10, 10, ... seconds. A receiver that stays silent by choice
 * is bothered by one CMD_INIT + CMD_SWITCHRES pair every ten seconds at
 * most.
 *
 * Why only silence is knocked. A receiver whose statuses keep flowing
 * (a software client with no geometry yet knocks every 250 ms with
 * frame_echo == 0) is already served by the status-driven re-announce,
 * which sees every one of those statuses. Knocking it as well would add a
 * second, independent cadence on top of that one. So the reference instant
 * is the LATEST event for this receiver -- the last status received, or the
 * last announce sent -- and a knock only fires once a full interval has
 * passed since it. A synchronized receiver is never knocked, and its
 * synchronization resets the schedule to the first interval.
 *
 * Why the caller must pass an event refreshed by EVERY announce. The
 * session opening, the status-driven re-announce, a geometry change and
 * the knock itself all send CMD_INIT or CMD_SWITCHRES. One shared timer,
 * restarted by any of them, keeps the knock from firing right after
 * another announce went out. groovy_open_knock_last_event_ms() below
 * combines the two instants.
 *
 * Why a separate counter. The status-driven re-announce keeps its own
 * burst budget, which must never drain on a receiver that sends nothing:
 * draining it here would bring back a known failure -- a budget emptied in
 * three seconds, then a black screen with no recourse. This header never
 * reads nor writes that
 * state; it owns `struct groovy_open_knock` and nothing else.
 *
 * Why the number of knocks is never capped. A receiver absent for minutes
 * or hours (powered on late, rebooted, woken on LAN) must still be served
 * when it arrives. Only the interval is capped; `attempt` saturates and
 * never wraps. Storm bound: one knock per interval and per receiver, at
 * least GROOVY_OPEN_KNOCK_FIRST_MS apart, held by `last_fire_ms` even if
 * the caller passes a stale event. */

#include <stdint.h>

#define GROOVY_OPEN_KNOCK_FIRST_MS 1000u
#define GROOVY_OPEN_KNOCK_CAP_MS   10000u

struct groovy_open_knock {
    unsigned attempt;       /* knocks since the receiver was last synchronized */
    uint64_t last_fire_ms;  /* instant of the last knock                        */
};

/* Interval before knock number `attempt` (0-based): FIRST_MS shifted left by
 * min(attempt, 4) bits, capped at CAP_MS. */
static inline unsigned groovy_open_knock_interval_ms(unsigned attempt)
{
    const unsigned shift = attempt > 4u ? 4u : attempt;
    const uint64_t v     = (uint64_t)GROOVY_OPEN_KNOCK_FIRST_MS << shift;

    return v > GROOVY_OPEN_KNOCK_CAP_MS ? GROOVY_OPEN_KNOCK_CAP_MS : (unsigned)v;
}

/* The reference instant for the knock: the later of the last announce sent
 * to this receiver and the last status received from it. `has_status` at 0
 * (no status ever received) ignores `last_status_ms`. */
static inline uint64_t groovy_open_knock_last_event_ms(uint64_t last_announce_ms, int has_status,
                                                       uint64_t last_status_ms)
{
    if (has_status && last_status_ms > last_announce_ms)
        return last_status_ms;
    return last_announce_ms;
}

/* Returns 1 if CMD_INIT then CMD_SWITCHRES must be sent now, 0 otherwise.
 * `synced`: the receiver is echoing frames. `last_event_ms`: the latest
 * announce or status for this receiver (groovy_open_knock_last_event_ms).
 * `now_ms`: monotonic clock in milliseconds; a clock behind the reference
 * never fires. Updates *k; no side effect outside *k. */
static inline int groovy_open_knock_should_fire(struct groovy_open_knock *k, int synced,
                                                uint64_t last_event_ms, uint64_t now_ms)
{
    uint64_t ref;

    if (!k)
        return 0;
    if (synced) {
        k->attempt = 0u;
        return 0;
    }
    ref = last_event_ms > k->last_fire_ms ? last_event_ms : k->last_fire_ms;
    if (now_ms < ref)
        return 0;
    if (now_ms - ref < (uint64_t)groovy_open_knock_interval_ms(k->attempt))
        return 0;
    k->last_fire_ms = now_ms;
    if (k->attempt < ~0u)
        k->attempt++;
    return 1;
}

#endif /* GROOVY_OPEN_KNOCK_H */
