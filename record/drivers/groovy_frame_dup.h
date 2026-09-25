/* SPDX-License-Identifier: GPL-3.0-or-later */
/* groovy_frame_dup.h -- switch for lever 1, no dependency on RetroArch.
 * Header-only, same pattern as groovy_compression.h: record_groovy.c
 * includes it to run, and the test bridge
 * libgm/test/groovy_test_bridge.c includes it to exercise it outside
 * RetroArch.
 *
 * Only one thing lives here: reading the GROOVY_FRAME_DUP setting, a
 * boolean that crosses the boundary into `libgm` via `gm_set_frame_dup`.
 *
 * WHAT THE LEVER DOES (in libgm, not here): an RGB888 frame strictly
 * identical to the previous one, byte for byte, goes out as a 9-byte
 * `frame_dup` instead of a full frame. Measured on the bench: a game
 * intro dropped from 87 to 22 Mbps. The decision -- comparison,
 * full-frame timer, header shape -- lives entirely in `libgm`; this
 * file carries only the switch, never the wire logic.
 *
 * DEFAULTS UNIFIED ACROSS THE PROJECT: on by default on the
 * `scripts/launch-emitter.ps1` side AND on the fork side with no
 * variable set -- this makes the default playable for a human who
 * opens RetroArch without a script. The wire baseline
 * (`test_pcap_diff_groovy_baseline.py`) stays green: it reads no
 * configuration and never calls `groovy_frame_dup_mode_parse`.
 *
 * The setting lives in the GROOVY_FRAME_DUP environment variable and
 * not in retroarch.cfg: RetroArch rewrites that file on exit from its
 * own settings list, so a key added by hand would disappear. The fork
 * already uses this pattern for GROOVY_COMPRESSION, GROOVY_MTU,
 * GROOVY_AUDIO and GROOVY_INPUT.
 *
 * No dependency on libgm's public header: this setting has no
 * counterpart on the wire itself -- the boolean passes through
 * `gm_set_frame_dup(h, 0|1)`, never as a value that would travel
 * inside a packet.
 */

#ifndef GROOVY_FRAME_DUP_H
#define GROOVY_FRAME_DUP_H

#include <string.h>

enum groovy_frame_dup_mode {
    GROOVY_FRAME_DUP_OFF = 0,   /* every frame goes out whole -- the original behavior */
    GROOVY_FRAME_DUP_ON  = 1    /* a frame identical to the previous one goes out as frame_dup */
};

/* Parses the GROOVY_FRAME_DUP value.
 *
 * DEFAULT IS ON: NULL, empty or unknown -> ON. "off" is now the ONLY
 * way to turn it off, and it must be written out. The file's own rule
 * hasn't changed -- an unknown value always falls back to the default
 * -- it's the DEFAULT itself that moved.
 *
 * WHY HERE AND NOT IN THE MENU. frame_dup is the only one of the
 * playable defaults with no menu setting: it's kept as a variable
 * alone, because its correct value is known and measured and a wrong
 * setting degrades without saying why. Its default can therefore only
 * change in this function. The other three (audio, compression,
 * inputs) keep their parser intact and take their default from the
 * menu. */
static inline enum groovy_frame_dup_mode groovy_frame_dup_mode_parse(const char *s)
{
    if (!s || s[0] == '\0')
        return GROOVY_FRAME_DUP_ON;
    if (!strcmp(s, "off"))
        return GROOVY_FRAME_DUP_OFF;
    return GROOVY_FRAME_DUP_ON;
}

static inline const char *groovy_frame_dup_mode_name(enum groovy_frame_dup_mode m)
{
    switch (m) {
        case GROOVY_FRAME_DUP_ON: return "on";
        case GROOVY_FRAME_DUP_OFF:
        default:                  return "off";
    }
}

#endif /* GROOVY_FRAME_DUP_H */
