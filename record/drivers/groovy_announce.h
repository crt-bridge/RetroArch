/* SPDX-License-Identifier: GPL-3.0-or-later */
/* groovy_announce.h -- the pinned modelines, and the one function that builds
 * a CMD_SWITCHRES announcement.
 *
 * The receiver keeps three modelines of its own (240p and 480i at 2560
 * pixels wide, which toggle by registers, and 480p at 1280 wide, the 31 kHz
 * reference). The receiver serves the mode it receives as received, so the
 * emitter announces those very modelines, field for field, for its three
 * regimes: what the tube has always been shown is what goes on the wire.
 * Nothing here is derived from the native size of the picture; the size only
 * decides the regime (groovy_mode.h), and the regime decides the modeline.
 *
 * The values are a COPY of the receiver's constants (the public fork does not
 * include anything from the receiver). A test reads both files on every run
 * and requires every field to be equal. Do not change a value here alone.
 *
 * The three announcement sites of the driver (first announce and regime
 * switch, re-announce, deferred re-announce) and the unit-test bridge all go
 * through groovy_announce_modeline(): no site builds a modeline of its own.
 * It gives the pinned modeline of the regime, or, in the regime of the
 * session, the modeline the launcher chose (groovy_session.h).
 *
 * Positions are CUMULATIVE (start of sync, end of sync, total), as on the
 * wire, not widths.
 *
 * Must be included AFTER gm.h (gm_modeline) and groovy_mode.h (the regime).
 */

#ifndef GROOVY_ANNOUNCE_H
#define GROOVY_ANNOUNCE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>   /* memset */

#ifndef GM_H
#error "groovy_announce.h must be included AFTER gm.h"
#endif

#include "groovy_mode.h"

/* 240p, 2560 wide, progressive. */
#define GROOVY_PINNED_240P_H_ACTIVE     2560u
#define GROOVY_PINNED_240P_H_BEGIN      2632u
#define GROOVY_PINNED_240P_H_END        2860u
#define GROOVY_PINNED_240P_H_TOTAL      3088u
#define GROOVY_PINNED_240P_V_ACTIVE      240u
#define GROOVY_PINNED_240P_V_BEGIN       244u
#define GROOVY_PINNED_240P_V_END         247u
#define GROOVY_PINNED_240P_V_TOTAL       262u
#define GROOVY_PINNED_240P_PCLOCK_HZ 48480000u
#define GROOVY_PINNED_240P_INTERLACE       0u

/* 480i, 2560 wide, interlaced: same clock and same horizontal positions as
 * the 240p above (the pair the receiver toggles by registers). */
#define GROOVY_PINNED_480I_H_ACTIVE     2560u
#define GROOVY_PINNED_480I_H_BEGIN      2632u
#define GROOVY_PINNED_480I_H_END        2860u
#define GROOVY_PINNED_480I_H_TOTAL      3088u
#define GROOVY_PINNED_480I_V_ACTIVE      480u
#define GROOVY_PINNED_480I_V_BEGIN       488u
#define GROOVY_PINNED_480I_V_END         494u
#define GROOVY_PINNED_480I_V_TOTAL       525u
#define GROOVY_PINNED_480I_PCLOCK_HZ 48480000u
#define GROOVY_PINNED_480I_INTERLACE       1u

/* 480p, 1280 wide, progressive, 31 kHz. */
#define GROOVY_PINNED_480P_H_ACTIVE     1280u
#define GROOVY_PINNED_480P_H_BEGIN      1312u
#define GROOVY_PINNED_480P_H_END        1504u
#define GROOVY_PINNED_480P_H_TOTAL      1600u
#define GROOVY_PINNED_480P_V_ACTIVE      480u
#define GROOVY_PINNED_480P_V_BEGIN       490u
#define GROOVY_PINNED_480P_V_END         492u
#define GROOVY_PINNED_480P_V_TOTAL       525u
#define GROOVY_PINNED_480P_PCLOCK_HZ 50350000u
#define GROOVY_PINNED_480P_INTERLACE       0u

/* groovy_announce_pinned -- the pinned modeline of the regime `m`.
 *
 * The pixel clock goes on the wire in MHz as a double built from a whole
 * number of kHz (`khz / 1000.0`): the receiver truncates it to the kHz
 * anyway, so this is the value it uses, with no sub-kHz residue to round
 * differently on two machines. Pure function. */
static inline void groovy_announce_pinned(enum groovy_mode m, gm_modeline *out)
{
    uint32_t pclock_hz;
    uint32_t khz;

    memset(out, 0, sizeof(*out));

    switch (m) {
    case GROOVY_MODE_480I:
        out->h_active = (uint16_t)GROOVY_PINNED_480I_H_ACTIVE;
        out->h_begin  = (uint16_t)GROOVY_PINNED_480I_H_BEGIN;
        out->h_end    = (uint16_t)GROOVY_PINNED_480I_H_END;
        out->h_total  = (uint16_t)GROOVY_PINNED_480I_H_TOTAL;
        out->v_active = (uint16_t)GROOVY_PINNED_480I_V_ACTIVE;
        out->v_begin  = (uint16_t)GROOVY_PINNED_480I_V_BEGIN;
        out->v_end    = (uint16_t)GROOVY_PINNED_480I_V_END;
        out->v_total  = (uint16_t)GROOVY_PINNED_480I_V_TOTAL;
        out->interlace = (uint8_t)GROOVY_PINNED_480I_INTERLACE;
        pclock_hz = GROOVY_PINNED_480I_PCLOCK_HZ;
        break;
    case GROOVY_MODE_480P:
        out->h_active = (uint16_t)GROOVY_PINNED_480P_H_ACTIVE;
        out->h_begin  = (uint16_t)GROOVY_PINNED_480P_H_BEGIN;
        out->h_end    = (uint16_t)GROOVY_PINNED_480P_H_END;
        out->h_total  = (uint16_t)GROOVY_PINNED_480P_H_TOTAL;
        out->v_active = (uint16_t)GROOVY_PINNED_480P_V_ACTIVE;
        out->v_begin  = (uint16_t)GROOVY_PINNED_480P_V_BEGIN;
        out->v_end    = (uint16_t)GROOVY_PINNED_480P_V_END;
        out->v_total  = (uint16_t)GROOVY_PINNED_480P_V_TOTAL;
        out->interlace = (uint8_t)GROOVY_PINNED_480P_INTERLACE;
        pclock_hz = GROOVY_PINNED_480P_PCLOCK_HZ;
        break;
    default:   /* GROOVY_MODE_240P_SUPER_RES */
        out->h_active = (uint16_t)GROOVY_PINNED_240P_H_ACTIVE;
        out->h_begin  = (uint16_t)GROOVY_PINNED_240P_H_BEGIN;
        out->h_end    = (uint16_t)GROOVY_PINNED_240P_H_END;
        out->h_total  = (uint16_t)GROOVY_PINNED_240P_H_TOTAL;
        out->v_active = (uint16_t)GROOVY_PINNED_240P_V_ACTIVE;
        out->v_begin  = (uint16_t)GROOVY_PINNED_240P_V_BEGIN;
        out->v_end    = (uint16_t)GROOVY_PINNED_240P_V_END;
        out->v_total  = (uint16_t)GROOVY_PINNED_240P_V_TOTAL;
        out->interlace = (uint8_t)GROOVY_PINNED_240P_INTERLACE;
        pclock_hz = GROOVY_PINNED_240P_PCLOCK_HZ;
        break;
    }

    khz = (pclock_hz + 999u) / 1000u;   /* whole kHz, rounded up */
    out->pclock_mhz = (double)khz / 1000.0;
}

/* groovy_announce_modeline -- THE function that builds a CMD_SWITCHRES
 * announcement, for the regime `m` and the mode of the session `sm`.
 *
 * In the regime of the session (GROOVY_MODE_SESSION, with a valid session)
 * the modeline is the one the launcher gave, copied field for field: clock
 * `khz / 1000.0` MHz, the eight positions, the scan. Any other regime gives
 * the pinned modeline of the receiver (groovy_announce_pinned). Nothing is
 * derived from the native size of the picture, and nothing here judges the
 * ranges of the tube: the receiver's guard does. Pure function. */
static inline void groovy_announce_modeline(enum groovy_mode m,
                                            const struct groovy_session_mode *sm,
                                            gm_modeline *out)
{
    if (m != GROOVY_MODE_SESSION || !sm || !sm->valid) {
        groovy_announce_pinned(m, out);
        return;
    }
    memset(out, 0, sizeof(*out));
    out->h_active  = (uint16_t)sm->h_active;
    out->h_begin   = (uint16_t)sm->h_begin;
    out->h_end     = (uint16_t)sm->h_end;
    out->h_total   = (uint16_t)sm->h_total;
    out->v_active  = (uint16_t)sm->v_active;
    out->v_begin   = (uint16_t)sm->v_begin;
    out->v_end     = (uint16_t)sm->v_end;
    out->v_total   = (uint16_t)sm->v_total;
    out->interlace = (uint8_t)(sm->interlace ? 1u : 0u);
    out->pclock_mhz = (double)sm->khz / 1000.0;
}

/* groovy_announce_cadence_hz -- the rate the tube is given by `m`: frames per
 * second, or FIELDS per second when the modeline is interlaced. Computed on
 * the clock brought back to a whole number of kHz, because that is the clock
 * the receiver programs. Returns 0.0 for a degenerate modeline. */
static inline double groovy_announce_cadence_hz(const gm_modeline *m)
{
    double khz;
    double lines;
    double rate;

    if (!m || m->h_total == 0u || m->v_total == 0u || !(m->pclock_mhz > 0.0))
        return 0.0;

    khz   = (double)(uint32_t)(m->pclock_mhz * 1000.0 + 0.5);
    lines = (double)m->h_total * (double)m->v_total;
    rate  = (khz * 1000.0) / lines;
    return m->interlace ? rate * 2.0 : rate;
}

/* groovy_announce_modelines_equal -- the same modeline field for field (the
 * clock compared as whole kHz, which is how it is programmed). */
static inline bool groovy_announce_modelines_equal(const gm_modeline *a, const gm_modeline *b)
{
    return (uint32_t)(a->pclock_mhz * 1000.0 + 0.5) == (uint32_t)(b->pclock_mhz * 1000.0 + 0.5)
        && a->h_active == b->h_active && a->h_begin == b->h_begin
        && a->h_end    == b->h_end    && a->h_total == b->h_total
        && a->v_active == b->v_active && a->v_begin == b->v_begin
        && a->v_end    == b->v_end    && a->v_total == b->v_total
        && a->interlace == b->interlace;
}

/* groovy_announce_is_repeat -- would announcing `next` only repeat what the
 * receivers were last told?
 *
 * Yes, and nothing needs to go out, when something was announced before
 * (`have_last`), the reason is a size change inside the same regime (a size
 * the core declared, or one that settled) and the modeline is the one
 * already on the wire. That is every size change of the pinned modes: the
 * wire carries the modeline of the regime, not the native size, so the
 * announce would be a copy of the last one. The first announce, a regime
 * switch and both re-announce paths are never repeats. */
static inline bool groovy_announce_is_repeat(bool have_last, const gm_modeline *last,
                                             const gm_modeline *next,
                                             enum groovy_mode_reason reason)
{
    if (!have_last || !last || !next)
        return false;
    if (reason != GROOVY_MODE_REASON_DECLARED && reason != GROOVY_MODE_REASON_SETTLED)
        return false;
    return groovy_announce_modelines_equal(last, next);
}

/* groovy_announce_wire_has_fields -- does the wire carry FIELDS? It does when
 * the modeline last announced is interlaced: two half-height pictures per
 * frame, whatever the regime that announced it. The single predicate for
 * cutting a picture into fields, for the one-field-per-frame lever and for
 * the parity injection; it replaces a comparison to the name of the regime
 * 480i, which stood for it while the 480i was the only interlaced mode. */
static inline bool groovy_announce_wire_has_fields(bool announced_valid,
                                                   const gm_modeline *announced)
{
    return announced_valid && announced && announced->interlace != 0u;
}

#endif /* GROOVY_ANNOUNCE_H */
