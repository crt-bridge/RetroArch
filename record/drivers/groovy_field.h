/* SPDX-License-Identifier: GPL-3.0-or-later */
/* groovy_field.h — the field(n) formula and the switch for lever 2
 * (WAN-02). Header-only, same model as groovy_frame_dup.h: record_groovy.c
 * includes it to run, and the test bridge libgm/test/groovy_test_bridge.c
 * includes it to exercise it outside RetroArch.
 *
 * WHAT THE LEVER DOES. In 480i, the fork today sends BOTH fld0 AND fld1
 * of every frame under the same frame_id (CMD_BLIT_FIELD_VSYNC, 0x07) --
 * double what a 480i tube actually scans (one field per sixtieth of a
 * second). This lever switches emission to ONE field per frame, over
 * CMD_BLIT_VSYNC (0x06) half-height under consecutive frame_ids -- the
 * GroovyMAME path. Exactly halves the bitrate: Tekken 3 over LZ4,
 * 193 -> 97 Mbps (see the compression-levers findings).
 *
 * THE FORMULA, AND ITS DERIVATION -- not just its statement.
 *
 *   field(n) = F XOR ((n - G) mod 2)
 *
 * GroovyMAME's formula (drawnogpu.cpp:400):
 *   F = the FLAG_VGA_F1 bit (0x20) of the last status received;
 *   G = its ack_frame.
 *
 * Under COMPAT-02 (already delivered), the receiver guarantees
 * F = G mod 2 (the flag carries the parity of last_frame). Then:
 *
 *   field(n) = F XOR ((n - G) mod 2)
 *            = (G mod 2) XOR ((n - G) mod 2)
 *            = n mod 2
 *
 * The emitter's choice and the receiver's deduction (field = frame_id & 1
 * for a half-height 0x06, COMPAT-03) therefore agree BY CONSTRUCTION. The
 * full formula is computed anyway: hard-coding `n & 1` would start lying
 * the day a receiver stopped honoring COMPAT-02 -- the formula would then
 * follow what it actually announces, `n & 1` would not.
 *
 * G COMES FROM last_frame, NEVER FROM last_frame_echo. `last_frame` is
 * the last DISPLAYED frame (ACK offset 6); `last_frame_echo` is the last
 * REASSEMBLED frame (offset 0). Using the wrong source would shift the
 * parity INTERMITTENTLY, only under load (when the two counters
 * diverge) -- a bug that would not show up at rest.
 *
 * F AND G MUST COME FROM THE SAME STATUS PACKET. The receiver publishes
 * them in a single atomic gesture (page_flip_handler in
 * receiver/blit.c, the "Signal net thread to send an ACK for the frame
 * just shown" block); groovy_ack_state_t therefore ALWAYS receives them
 * together, at both of its ACK-drain call sites.
 *
 * THE LATENCY MODEL, STATED PLAINLY: every other field displays one
 * period later, exactly like GroovyMAME today. That is the behavior of
 * the project's own oracle, so the same proof holds for it and for us.
 *
 * TWO DIVERGING DEFAULTS, AND THAT IS DELIBERATE: without
 * GROOVY_FIELD_PER_FRAME, the fork keeps the current fld0+fld1 path,
 * bit-identical (the "no variable set" behavior, guarded by
 * test_pcap_diff_groovy_baseline.py). scripts/launch-emitter.ps1 sets the
 * variable active by default, as it already does for GROOVY_FRAME_DUP.
 *
 * DO NOT MEASURE THIS LEVER WITH GROOVY_FRAME_DUP=on AT THE SAME TIME.
 * On the receiver side, a 480i pair on the 0x06 path opens on the first
 * REAL half-frame received after a complete pair. On the libgm side,
 * rule 5 forbids a dup while a pair is open, but nothing forces a SERIES
 * of dups to break on field 0: an identical field 0 (dup) followed by a
 * different field 1 (real) opens a pair on ODD parity -- and the
 * one-second deadline (rule 3) landing on a field 1 does the same. Pair
 * opening parity therefore becomes variable MID-SESSION, depending on
 * content (menus, static screens), while receiver/blit.c's one-time
 * re-phase (phase_locked) assumes it is constant. The logs show it:
 * p0_total and p1_total both non-zero within the same session. Until a
 * pending backlog decision settles this (replay the memorized even
 * field before breaking a series on an odd field, or another approach),
 * any measurement of this lever is done with -FrameDup off.
 *
 * This header depends on gm.h (for FLAG_VGA_F1) AND on groovy_pacing.h
 * (for groovy_ack_state_t): same include-order guard as groovy_pacing.h.
 */

#ifndef GROOVY_FIELD_H
#define GROOVY_FIELD_H

#ifndef GM_H
#  error "groovy_field.h requires gm.h to be included first"
#endif

#include <string.h>
#include <stdint.h>

#include "groovy_pacing.h"

enum groovy_field_mode {
    GROOVY_FIELD_BOTH     = 0,  /* two fields per frame, 0x07 under the same frame_id
                                 * -- the pre-lever behavior (default) */
    GROOVY_FIELD_PER_FRAME = 1  /* one field per frame, half-height 0x06 under
                                 * consecutive frame_ids -- the GroovyMAME path */
};

/* Parses the value of GROOVY_FIELD_PER_FRAME. NULL, empty or unknown ->
 * BOTH. "on" -> PER_FRAME. No other value is accepted -- exact,
 * case-sensitive comparison, same discipline as
 * groovy_frame_dup_mode_parse. */
static inline enum groovy_field_mode groovy_field_mode_parse(const char *s)
{
    if (!s || s[0] == '\0')
        return GROOVY_FIELD_BOTH;
    if (!strcmp(s, "on"))
        return GROOVY_FIELD_PER_FRAME;
    return GROOVY_FIELD_BOTH;
}

static inline const char *groovy_field_mode_name(enum groovy_field_mode m)
{
    switch (m) {
        case GROOVY_FIELD_PER_FRAME: return "on";
        case GROOVY_FIELD_BOTH:
        default:                     return "off";
    }
}

/* field(n) = F XOR ((n - G) mod 2), GroovyMAME's formula (drawnogpu.cpp:400),
 * F = the FLAG_VGA_F1 bit (0x20) of the last status, G = its ack_frame.
 * Returns 0 (even ranks) or 1 (odd ranks). The subtraction is UNSIGNED: an
 * n smaller than G wraps around, and the parity stays correct -- that is
 * deliberate, not an overflow to fix (see the derivation above). */
static inline uint8_t groovy_field_for_frame(const groovy_ack_state_t *st, uint32_t n)
{
    uint8_t  F = (uint8_t)((st->last_flags & FLAG_VGA_F1) ? 1u : 0u);
    uint32_t G = st->last_frame;          /* ACK offset 6, NOT frame_echo */
    return (uint8_t)(F ^ (uint8_t)((n - G) & 1u));
}

#endif /* GROOVY_FIELD_H */
