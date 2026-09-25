/* SPDX-License-Identifier: GPL-3.0-or-later */
/* groovy_compression.h -- compression setting for the crt-bridge emitter,
 * no dependency on RetroArch. Header-only, same pattern as
 * groovy_audio.h: record_groovy.c includes it to run, and the test
 * bridge libgm/test/groovy_test_bridge.c includes it to exercise it
 * outside RetroArch.
 *
 * Only one thing lives here: reading the GROOVY_COMPRESSION setting,
 * which determines the `compression` byte of CMD_INIT (0x02).
 *
 * The setting lives in the GROOVY_COMPRESSION environment variable and
 * not in retroarch.cfg: RetroArch rewrites that file on exit from its
 * own settings list, so a key added by hand would disappear. The fork
 * already uses this pattern for GROOVY_AUDIO and for the frame witness
 * (GROOVY_DUMP_FRAME_PATH).
 *
 * OFF is the default, so nothing changes on first launch.
 *
 * `groovy_compression_mode_parse` accepts ONLY "lz4" and "off" --
 * nothing else, and that's deliberate. On the wire, the compression
 * byte can only be 0 or 1: groovymister.cpp writes (lz4Frames) ? 1 : 0,
 * and groovy.cpp clamps any value above 1 down to 0 (checked against
 * the upstream source, see libgm/include/gm.h). The modes 0 through 6
 * that psakhis documents are his own emitter's LOCAL setting; accepting
 * them here would give the illusion of a choice that doesn't exist on
 * the wire.
 */

#ifndef GROOVY_COMPRESSION_H
#define GROOVY_COMPRESSION_H

#include <string.h>

/* ---------------------------------------------------------------------------
 * Byte 1 of CMD_INIT (0x02). Read once when the session opens.
 * ------------------------------------------------------------------------- */
enum groovy_compression_mode {
    GROOVY_COMPRESSION_OFF = 0,   /* byte 0 of CMD_INIT: raw */
    GROOVY_COMPRESSION_LZ4 = 1    /* byte 1 of CMD_INIT: LZ4 block */
};

/* Parses the GROOVY_COMPRESSION value. NULL, empty or unknown -> OFF.
 * "off" -> OFF. "lz4" -> LZ4. No other value is accepted. */
static inline enum groovy_compression_mode groovy_compression_mode_parse(const char *s)
{
    if (!s || s[0] == '\0')
        return GROOVY_COMPRESSION_OFF;
    if (!strcmp(s, "lz4"))
        return GROOVY_COMPRESSION_LZ4;
    return GROOVY_COMPRESSION_OFF;
}

static inline const char *groovy_compression_mode_name(enum groovy_compression_mode m)
{
    switch (m) {
        case GROOVY_COMPRESSION_LZ4: return "lz4";
        case GROOVY_COMPRESSION_OFF:
        default:                     return "off";
    }
}

#endif /* GROOVY_COMPRESSION_H */
