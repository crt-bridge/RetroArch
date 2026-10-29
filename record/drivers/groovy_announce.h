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
 * through groovy_announce_pinned(): no site builds a modeline of its own.
 *
 * Positions are CUMULATIVE (start of sync, end of sync, total), as on the
 * wire, not widths.
 *
 * Must be included AFTER gm.h (gm_modeline) and groovy_mode.h (the regime).
 */

#ifndef GROOVY_ANNOUNCE_H
#define GROOVY_ANNOUNCE_H

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

#endif /* GROOVY_ANNOUNCE_H */
