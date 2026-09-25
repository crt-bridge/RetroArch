/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef GROOVY_REANNOUNCE_H
#define GROOVY_REANNOUNCE_H

/* Re-announce CMD_INIT + CMD_SWITCHRES -- a pure decision.
 *
 * Three integers and a clock as input (source address, the peer's `frame`
 * counter, the peer's `frame_echo`, and a monotonic `now_ms` supplied by the
 * caller), one boolean as output. No dependency on RetroArch or the
 * network: the clock is a number, not a system call -- that is what keeps
 * the function testable.
 *
 * What the peer sends while it has no geometry yet (client/gmclient.c,
 * maybe_status_nogeom): a "knock" every 250 ms, frame_echo = 0, and its own
 * counter in `frame`, incrementing by 1 on every knock. This status, and
 * only this status, is what a client started after the emitter, or a
 * relaunched client, can send -- a client with no geometry assembles
 * nothing, so it never sends any frame status, and nobody polls it.
 *
 * Why these triggers, and why in this order.
 *
 * (0) frame_echo != 0: the peer has already echoed back the id of a frame
 *     it assembled. It has therefore received CMD_INIT and CMD_SWITCHRES --
 *     there is nothing left to re-announce to it. This case, and only this
 *     case, is what keeps the bench receiver entirely out of the mechanism
 *     once it has settled into steady state. It is also what FULLY
 *     replenishes the re-announce budget (fired reset to 0, armed reset to
 *     0): a peer that synchronizes proves that the re-announce served its
 *     purpose. Since a later revision, it is no longer the ONLY thing that
 *     makes a new shot possible -- replenishment OVER TIME (see the
 *     anti-storm bound below) also allows a re-announce beyond the initial
 *     burst, without ever resetting `fired` to zero (it stays saturated at
 *     GROOVY_REANNOUNCE_BURST_MAX).
 *
 * (a) First status ever seen from an unknown source address. A client
 *     started AFTER the emitter never saw the CMD_INIT nor the
 *     CMD_SWITCHRES of the session opening go by: it does not know the
 *     compression in effect nor the geometry, so it assembles nothing, so
 *     its frame_echo is 0 and case (0) does not mask it. Its first status
 *     is the only signal the emitter gets from it.
 *
 * (b) `frame` counter going BACKWARDS for the SAME address. A client
 *     relaunched during the session rebinds the same socket
 *     0.0.0.0:32100, so it comes back with the same address AND the same
 *     port: (a) would not see it. What gives it away is its frame counter,
 *     which restarts near zero. This is a signal DISTINCT from
 *     `s_lat_anomalies` in record_groovy.c: that one only discards
 *     in-flight gaps larger than 1000 frames, never a plain rollback, and
 *     it triggers nothing.
 *
 * (c) Knocks that persist for more than GROOVY_REANNOUNCE_RETRY_MS after
 *     the last re-announce to this address. A re-announce is two UDP
 *     datagrams; if they are lost, the client still has no geometry and
 *     keeps knocking, with a counter that KEEPS RISING: neither (a) nor
 *     (b) fires again. Without (c), the budget of three would serve no
 *     purpose. With (c), a knock received at least one second after the
 *     last re-announce triggers a new one. A client that received the
 *     announcement stops knocking as soon as its geometry is known (even
 *     before its first frame), so knocks that persist do mean the
 *     announcement never arrived. The one-second threshold is worth four
 *     knocks: the emitter does not replay on a simple delay.
 *
 * Anti-storm bound (a security requirement), REDEFINED by a later revision
 * (option 3, form A). A burst of at most GROOVY_REANNOUNCE_BURST_MAX
 * re-announces spaced at least GROOVY_REANNOUNCE_RETRY_MS apart, then at
 * most one re-announce every GROOVY_REANNOUNCE_REPLENISH_MS, without end,
 * for as long as the peer keeps knocking with frame_echo == 0: a RATE
 * bound, no longer a TOTAL bound (this paragraph used to say the opposite
 * -- a TOTAL bound -- which is exactly the reverse of what form A decides:
 * no more permanent silence once the budget is exhausted). A synchronized
 * peer (case 0) is never re-fired at. A frozen clock still never produces
 * more than GROOVY_REANNOUNCE_BURST_MAX shots between two
 * synchronizations. Branch (c) tests `armed`, never `fired > 0`: in form
 * A, the two always hold the same value (armed == (fired > 0), an
 * invariant verified by test_invariant_armed_egale_fired_positif,
 * emitter/test/test_groovy_reannounce.py), but `armed` is the field that
 * CARRIES the intent -- `fired` serves the burst budget, `armed` serves
 * guard (c).
 *
 * Two known limits, property of the per-receiver table work -- not to be
 * fixed here:
 *   1. (LIFTED by a later revision, option 3) The silence used to be
 *      permanent. After three re-announces without a sync to the same
 *      address, only a sync could replenish the budget, and a sync was
 *      impossible without geometry: the client stayed black until the
 *      fork restarted or the address changed. Scenario: a ~2 s link
 *      outage while a starting client is knocking (plausible over
 *      Tailscale, not on a wired LAN). The driver logs the exhaustion
 *      once per EPISODE (re-armed by a sync ALONE, branch (0), armed ==
 *      0; never by a new peer: after (a) or (b), armed is 1 -- the trace
 *      fires on every new episode) so that the symptom is recognized.
 *   2. The bound only holds for ONE peer. State retains only the last
 *      one, and (a) fires without consulting the budget: two addresses
 *      that alternate fire on every packet. Remaining bound: one
 *      re-announce per drain pass (the driver's `|=`), 60/s at most.
 *
 * What is deliberately NOT here: the per-address table and per-RECEIVER
 * announce state. Only one peer and one counter are retained.
 * Generalizing this is separate follow-up work.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define GROOVY_REANNOUNCE_DEFAULT      1
#define GROOVY_REANNOUNCE_BURST_MAX    3u
#define GROOVY_REANNOUNCE_RETRY_MS     1000u   /* (c): four knocks at 250 ms */
#define GROOVY_REANNOUNCE_REPLENISH_MS 5000u   /* option 3: replenishment period, form A */

struct groovy_reannounce_state {
    int      have_peer;    /* 0 until any status has been received       */
    uint32_t peer_ip;      /* sin_addr.s_addr as-is, network order        */
    uint32_t last_frame;   /* last `frame` seen from this peer            */
    unsigned fired;        /* re-announces since the last sync            */
    uint64_t last_fire_ms; /* timestamp of the last re-announce           */
    unsigned armed;        /* 1 once a re-announce has fired since the last sync; 0 after (0) */
};

/* Invariant in form A: `armed == (fired > 0)` at all times -- every write
 * to `fired` (the three sites below, plus the reset in branch (0)) is
 * accompanied by the SAME write to `armed`. Probed by the bridge (3,000
 * random sequences): no divergence found. `armed` is therefore a SECOND
 * source of truth, with no observable effect today -- it does not replace
 * `fired > 0u` with anything different in form A. It remains the field
 * that CARRIES the intent (branch (c) tests `armed`, never `fired > 0u`,
 * in case a future form makes the two diverge), verified by
 * `test_invariant_armed_egale_fired_positif`
 * (emitter/test/test_groovy_reannounce.py). */

/* "off"/"0" -> 0; "on"/"1" -> 1; absent, empty or unknown -> default (1). */
static inline int groovy_reannounce_parse(const char *s)
{
    if (!s || s[0] == '\0')                        return GROOVY_REANNOUNCE_DEFAULT;
    if (!strcmp(s, "off") || !strcmp(s, "0"))      return 0;
    if (!strcmp(s, "on")  || !strcmp(s, "1"))      return 1;
    return GROOVY_REANNOUNCE_DEFAULT;
}

/* CMD_INIT -> CMD_SWITCHRES spacing of a re-announce (from a network
 * capture). Setting GROOVY_REANNOUNCE_SPACING_US, in microseconds.
 *
 * Default (absent or empty): GROOVY_REANNOUNCE_SPACING_US_DEFAULT, the
 * spacing fix -- 100,000 us, the only value tested on the bench, a clear
 * positive result. Explicit "0" (invalid or out-of-range values also fall
 * back to the default): immediate send, the behavior from before this
 * fix -- CMD_SWITCHRES sent within the same call as CMD_INIT, wire
 * unchanged.
 *
 * N > 0: the re-announce's CMD_SWITCHRES is NOT sent within the call; the
 * driver defers it and sends it BEFORE the fan-out of a later frame --
 * never the one that triggered the deferral, even with a very short
 * spacing (a deliberate choice -- path 1). The clock alone is not enough
 * to guarantee "later": the driver keeps a round counter at the moment of
 * the deferral (switchres_deferred_turn, record_groovy.c) and refuses to
 * send until that round has changed, regardless of the result of
 * groovy_reannounce_spacing_due() below -- this lock originally keyed on
 * the driver's frame_id, which only advances if the fan-out to the MASTER
 * succeeds; a master whose send fails for good would freeze that frame_id
 * permanently, and a FOLLOWER's deferred send would then never go out
 * again. The round counter, by contrast, advances on every rendered
 * frame, independently of any send. Before that fix, the send used to go
 * out AFTER the fan-out of the SAME frame -- stuck to the tail of its own
 * burst instead of being separate from the next one.
 * Never a wait inside the runloop thread: a wait would delay the blit of
 * the frame to every receiver, including the master. A nonzero spacing is
 * therefore worth at least one frame; the actual gap can be read from the
 * t_ns values of [groovy-annonce].
 *
 * Covers ONLY the re-announce: the CMD_SWITCHRES of groovy_switchres_all
 * (a geometry or regime change) is never spaced out.
 *
 * Upper bound 200 ms: captures from one test family lost the opening
 * CMD_SWITCHRES 14 and 41 ms after its CMD_INIT; the capture window must
 * be able to clear those gaps comfortably, while staying under
 * GROOVY_REANNOUNCE_RETRY_MS so that a deferred send GENERALLY goes out
 * before the next re-announce -- not always: a new re-announce can
 * OVERWRITE a send not yet issued (branch (b) or (c) above, which can
 * fire at any time), or groovy_free can occur during the window. The
 * first case is logged (record_groovy.c, "[groovy-emit] re-announce:
 * switchres deferred overwritten") and counts toward reannounces_failed
 * if the overwritten CMD_INIT had itself failed; the second has nothing
 * to log, the session simply ends. */
#define GROOVY_REANNOUNCE_SPACING_US_DEFAULT  100000u  /* value tested on the bench (clear positive result) */
#define GROOVY_REANNOUNCE_SPACING_US_MAX      200000u

static inline unsigned groovy_reannounce_spacing_parse(const char *s)
{
    char         *fin = NULL;
    unsigned long v;

    if (!s || s[0] == '\0')
        return GROOVY_REANNOUNCE_SPACING_US_DEFAULT;
    if (s[0] < '0' || s[0] > '9')
        return GROOVY_REANNOUNCE_SPACING_US_DEFAULT;
    v = strtoul(s, &fin, 10);
    if (!fin || *fin != '\0')
        return GROOVY_REANNOUNCE_SPACING_US_DEFAULT;
    if (v > GROOVY_REANNOUNCE_SPACING_US_MAX)
        return GROOVY_REANNOUNCE_SPACING_US_DEFAULT;
    return (unsigned)v;
}

/* 1 when a deferred CMD_SWITCHRES may go out: at least spacing_us
 * microseconds have elapsed since t_init_ns. now_ns < t_init_ns -> 0. */
static inline int groovy_reannounce_spacing_due(uint64_t t_init_ns, unsigned spacing_us,
                                                uint64_t now_ns)
{
    if (now_ns < t_init_ns)
        return 0;
    return (now_ns - t_init_ns) >= (uint64_t)spacing_us * 1000u ? 1 : 0;
}

/* Returns 1 if CMD_INIT then CMD_SWITCHRES must be re-announced, 0
 * otherwise. `now_ms`: monotonic clock in milliseconds, supplied by the
 * caller (the driver passes groovy_pacing_mono_ns() / 1000000u; tests pass
 * a plain integer). Updates *st in every case. No side effect outside
 * *st. */
static inline int groovy_reannounce_should_fire(struct groovy_reannounce_state *st,
                                                uint32_t peer_ip,
                                                uint32_t frame,
                                                uint32_t frame_echo,
                                                uint64_t now_ms)
{
    if (!st) return 0;

    if (frame_echo != 0u) {                /* (0) peer demonstrably synchronized */
        st->have_peer  = 1;
        st->peer_ip    = peer_ip;
        st->last_frame = frame;
        st->fired      = 0u;               /* re-announce budget replenished */
        st->armed      = 0u;               /* nothing left to replenish for this peer */
        return 0;
    }
    /* === everything below only runs for frame_echo == 0 === */

    if (!st->have_peer || st->peer_ip != peer_ip) {
        st->have_peer    = 1;
        st->peer_ip      = peer_ip;
        st->last_frame   = frame;
        st->fired        = 1u;
        st->last_fire_ms = now_ms;
        st->armed        = 1u;
        return 1;                          /* (a) unknown address */
    }

    if (frame < st->last_frame) {          /* (b) the counter goes backwards */
        st->last_frame = frame;
        if (st->fired >= GROOVY_REANNOUNCE_BURST_MAX)
            return 0;                      /* anti-storm bound */
        st->fired++;
        st->last_fire_ms = now_ms;
        st->armed        = 1u;
        return 1;
    }

    st->last_frame = frame;

    if (st->armed &&                       /* (c) knocks that persist after a re-announce */
        now_ms - st->last_fire_ms >= (uint64_t)GROOVY_REANNOUNCE_RETRY_MS) {
        if (st->fired < GROOVY_REANNOUNCE_BURST_MAX) {      /* initial burst, unchanged */
            st->fired++;
            st->last_fire_ms = now_ms;
            return 1;
        }
        if (now_ms - st->last_fire_ms >= (uint64_t)GROOVY_REANNOUNCE_REPLENISH_MS) {
            st->last_fire_ms = now_ms;       /* option 3, form A: one re-announce per period; fired stays saturated */
            return 1;
        }
        return 0;
    }

    return 0;
}

#endif /* GROOVY_REANNOUNCE_H */
