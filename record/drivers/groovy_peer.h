/* SPDX-License-Identifier: GPL-3.0-or-later */
/* groovy_peer.h -- what the master is, and the settings that follow from it.
 * Header-only, no dependency on RetroArch; record_groovy.c includes it.
 *
 * The emitter speaks the original Groovy protocol by default. The crt-bridge
 * receiver is the peer that has extensions (the pinned pair of modes of its
 * tube, two fields per frame in 0x07, a padded session), and it says so: it
 * answers CMD_GET_VERSION with its 13-byte status. A MiSTer core answers with
 * one byte (the original core) or with `01 43 42 <rev>` (the core forked to
 * take padded headers). So the master is asked once, at opening, before the
 * first CMD_INIT, and put in one of four profiles. ONE table below gives each
 * profile its settings; nothing else in the driver tests what the peer is.
 *
 * Precedence, the same as everywhere in the driver: a variable that is set
 * wins on its own line (GROOVY_MODE_FALLBACK, GROOVY_FIELD_PER_FRAME,
 * GROOVY_NATIVE_HOLD_HFREQ, GROOVY_AUDIO_RATE, GROOVY_PAD). The profile only
 * gives the defaults.
 *
 * When the peer is asked (groovy_peer_wants_probe):
 *   GROOVY_PEER set           never: the profile it names is used
 *   GROOVY_PAD=probe          always
 *   GROOVY_PAD=off|on|headers never: an explicit choice, the wire of before
 *                             (headers: the forked MiSTer core; off, on: the
 *                             crt-bridge receiver)
 *   the menu's padded session never: crt-bridge receiver, padded
 *   nothing set               always
 */

#ifndef GROOVY_PEER_H
#define GROOVY_PEER_H

#include <stdbool.h>
#include <string.h>

#include "groovy_pad.h"

enum groovy_peer {
    GROOVY_PEER_STANDARD     = 0,  /* no known answer: the original Groovy protocol */
    GROOVY_PEER_CRTBRIDGE    = 1,  /* the crt-bridge receiver (13-byte status) */
    GROOVY_PEER_MISTER       = 2,  /* the MiSTer core forked for padded headers */
    GROOVY_PEER_MISTER_STOCK = 3   /* the original MiSTer core (1-byte answer) */
};
#define GROOVY_PEER_COUNT 4

struct groovy_peer_profile {
    const char *name;
    bool native_fallback;  /* a picture outside the session follows the core's geometry,
                            * never the receiver's pinned table */
    bool hold_hfreq;       /* native announce: the line held at the standard rate (colour
                            * of the FPGA composite encoder) */
    bool field_per_frame;  /* 480i: one half-height 0x06 per frame (GroovyMAME path) */
    int  pad_mode;         /* GROOVY_PAD_MODE_*; -1 = keep what the menu or GROOVY_PAD gave */
    bool wire_48k;         /* a 44100 Hz core goes out at 48000 Hz (the peer resamples crudely) */
    bool cut_audio;        /* no audio: the 3-byte AUDIO header dies on a Wi-Fi link and the
                            * orphaned sound datagrams are read as commands */
    bool servo_ff;         /* clock servo in feed-forward: the emulator runs at the announced
                            * cadence (59.83 Hz with the held line), not 0.15 % faster */
    bool audio_ratio;      /* the wire audio resampled by the known ratio (goes with servo_ff) */
};

/* The table. The crt-bridge line keeps the receiver's pinned table, the
 * padding left to the menu or the variable, and no servo; in 480i it now takes
 * ONE field per frame, as a MiSTer does: a receiver whose field gate waits for
 * the right field before each flip shows it smoothly (a receiver without that
 * gate shows the fields in the wrong order: GROOVY_FIELD_PER_FRAME=off keeps
 * two fields per frame there). The servo is on for the forked MiSTer core only:
 * the one heard and judged smooth with it; the original core has not been
 * tried. */
static const struct groovy_peer_profile groovy_peer_profiles[GROOVY_PEER_COUNT] = {
    /* name             fallback hold   field  pad                       48k    cut    servo  ratio */
    { "standard",       true,    false, true,  GROOVY_PAD_MODE_OFF,      false, false, false, false },
    { "crt-bridge",     false,   false, true,  -1,                       false, false, false, false },
    { "mister",         true,    true,  true,  GROOVY_PAD_MODE_HEADERS,  true,  false, true,  true  },
    { "mister-stock",   true,    true,  true,  GROOVY_PAD_MODE_OFF,      false, true,  false, false },
};

static inline const struct groovy_peer_profile *groovy_peer_profile(enum groovy_peer p)
{
    if ((unsigned)p >= GROOVY_PEER_COUNT)
        p = GROOVY_PEER_STANDARD;
    return &groovy_peer_profiles[(unsigned)p];
}

static inline const char *groovy_peer_name(enum groovy_peer p)
{
    return groovy_peer_profile(p)->name;
}

/* Parses GROOVY_PEER: one of the four names, exact. Anything else (NULL,
 * empty, wrong case) -> -1, the caller says it and asks the peer. */
static inline int groovy_peer_parse(const char *s)
{
    unsigned i;

    if (!s || s[0] == '\0')
        return -1;
    for (i = 0u; i < GROOVY_PEER_COUNT; i++)
        if (!strcmp(s, groovy_peer_profiles[i].name))
            return (int)i;
    return -1;
}

/* The answer of gm_probe_peer (GM_PEER_* of gm.h, passed as int so this
 * header does not need gm.h): 0 unknown, 1 stock, 2 padded, 3 crt-bridge. */
static inline enum groovy_peer groovy_peer_from_probe(int gm_peer)
{
    switch (gm_peer) {
    case 1:  return GROOVY_PEER_MISTER_STOCK;
    case 2:  return GROOVY_PEER_MISTER;
    case 3:  return GROOVY_PEER_CRTBRIDGE;
    default: return GROOVY_PEER_STANDARD;
    }
}

/* Is the peer asked, when GROOVY_PEER names no profile? pad_master is the value
 * resolved from GROOVY_PAD or the menu, pad_from_env says which. See the table
 * at the top of this file. */
static inline bool groovy_peer_wants_probe(int pad_master, bool pad_from_env)
{
    if (pad_master == GROOVY_PAD_MODE_PROBE)
        return true;
    if (pad_from_env)
        return false;
    return pad_master == GROOVY_PAD_MODE_OFF;   /* the menu's padded session is a choice */
}

/* The profile of an explicit padding choice, when the peer is not asked. */
static inline enum groovy_peer groovy_peer_of_pad(int pad_master)
{
    return (pad_master == GROOVY_PAD_MODE_HEADERS) ? GROOVY_PEER_MISTER : GROOVY_PEER_CRTBRIDGE;
}

/* The padding of the session: GROOVY_PAD wins when it names a form; otherwise
 * the profile's, or the resolved value when the profile keeps it. "probe" is
 * never a form: it falls back to the profile, and to off. */
static inline int groovy_peer_pad_mode(enum groovy_peer p, int pad_master, bool pad_from_env)
{
    const int prof = groovy_peer_profile(p)->pad_mode;

    if (pad_from_env && pad_master != GROOVY_PAD_MODE_PROBE)
        return pad_master;
    if (prof >= 0)
        return prof;
    return (pad_master == GROOVY_PAD_MODE_PROBE) ? GROOVY_PAD_MODE_OFF : pad_master;
}

#endif /* GROOVY_PEER_H */
