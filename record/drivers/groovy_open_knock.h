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
 * 1, 2, 4, 8, 10, 10, ... seconds. Only the interval is capped, never the
 * number of knocks. */

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

/* Returns 1 if CMD_INIT then CMD_SWITCHRES must be sent now, 0 otherwise.
 * `synced`: the receiver is echoing frames. `last_event_ms`: the latest
 * announce or status for this receiver. `now_ms`: monotonic clock in
 * milliseconds. Updates *k; no side effect outside *k. */
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
