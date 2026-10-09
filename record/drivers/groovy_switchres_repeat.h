/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef GROOVY_SWITCHRES_REPEAT_H
#define GROOVY_SWITCHRES_REPEAT_H

/* Mode-change repeat -- a pure decision: when to send a follower the SAME
 * CMD_SWITCHRES again, after the immediate send of a mode change.
 *
 * Why repeat. A mode change sends one CMD_SWITCHRES per receiver. Over
 * Wi-Fi that single datagram can be lost, and nothing ever repairs it: the
 * follower keeps the old geometry until the next change. When the old and
 * the new geometry carry the same number of bytes, the follower even
 * decodes every frame (wrongly), keeps echoing frames, and looks
 * synchronized to the emitter -- no status-driven re-announce and no open
 * knock will ever fire for it. So the emitter does not wait for a symptom:
 * after the immediate send, it sends the same CMD_SWITCHRES twice more, at
 * least GROOVY_SWITCHRES_REPEAT_SPACING_MS and at least twice that after
 * the original. A receiver that already got the geometry ignores an
 * identical one.
 *
 * Why never cancelled on synchronization. The case this repairs is
 * precisely a follower that echoes frames with the wrong geometry: it looks
 * synchronized. Cancelling the repeats on a synchronized link would remove
 * them exactly where they are needed. This header takes no synchronization
 * state as input, on purpose.
 *
 * Why two and then nothing. Repeats only follow a mode change, and stop:
 * GROOVY_SWITCHRES_REPEAT_COUNT sends per arming, never periodic. Mode
 * changes themselves are already bounded by the regime oscillation guard.
 * A receiver that lost all three sends is left to the other recovery paths.
 *
 * Why a new arming overwrites the old one. A mode change that comes in
 * while repeats are still pending restarts the count from the new send:
 * the caller copies the new geometry, so a stale geometry is never repeated
 * after a newer one went out.
 *
 * Why a turn lock. The caller passes its video turn counter. Nothing fires
 * on the turn that armed, and at most one repeat fires per turn: the locked
 * turn becomes the turn of the last repeat. Two repeats therefore always
 * travel in two different frames, whatever the clock says.
 *
 * A turn and a clock as input, one boolean as output. No dependency on
 * RetroArch or the network: the clock is a number supplied by the caller,
 * which keeps the function testable over hours of simulated time. A zeroed
 * structure (calloc) is idle and never fires. */

#include <stdint.h>

#define GROOVY_SWITCHRES_REPEAT_COUNT      2u
#define GROOVY_SWITCHRES_REPEAT_SPACING_MS 100u

struct groovy_switchres_repeat {
    unsigned left;         /* repeats still owed; 0 = idle                  */
    uint32_t locked_turn;  /* nothing fires on this turn: the arming turn,
                            * then the turn of the last repeat              */
    uint64_t t0_ms;        /* instant of the original send                  */
};

/* Arms (or re-arms, overwriting any pending repeat) after the original
 * CMD_SWITCHRES went out at `t0_ms`, on the caller's turn `turn`. */
static inline void groovy_switchres_repeat_arm(struct groovy_switchres_repeat *r,
                                               uint32_t turn, uint64_t t0_ms)
{
    if (!r)
        return;
    r->left        = GROOVY_SWITCHRES_REPEAT_COUNT;
    r->locked_turn = turn;
    r->t0_ms       = t0_ms;
}

/* Returns 1 if the same CMD_SWITCHRES must be sent again now, 0 otherwise.
 * Repeat number k (1, then 2) is due once `now_ms` is at least
 * k * GROOVY_SWITCHRES_REPEAT_SPACING_MS past the original send, on a turn
 * other than the locked one. A clock behind the original send never fires.
 * Updates *r; no side effect outside *r. */
static inline int groovy_switchres_repeat_due(struct groovy_switchres_repeat *r,
                                              uint32_t turn, uint64_t now_ms)
{
    unsigned k;

    if (!r)
        return 0;
    if (r->left == 0u || r->left > GROOVY_SWITCHRES_REPEAT_COUNT)
        return 0;
    if (turn == r->locked_turn)
        return 0;
    if (now_ms < r->t0_ms)
        return 0;
    k = GROOVY_SWITCHRES_REPEAT_COUNT - r->left + 1u;
    if (now_ms - r->t0_ms < (uint64_t)GROOVY_SWITCHRES_REPEAT_SPACING_MS * k)
        return 0;
    r->left--;
    r->locked_turn = turn;
    return 1;
}

#endif /* GROOVY_SWITCHRES_REPEAT_H */
