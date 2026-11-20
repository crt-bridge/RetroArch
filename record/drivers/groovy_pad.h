/* SPDX-License-Identifier: GPL-3.0-or-later */
/* groovy_pad.h -- padded-session setting for the crt-bridge emitter, no
 * dependency on RetroArch. Header-only, same pattern as
 * groovy_compression.h: record_groovy.c includes it to run. No test-bridge
 * include here -- libgm/test/groovy_test_bridge.c stays pinned at bec270a
 * for the release and does not exercise this setting;
 * emitter/test/test_groovy_pad.py compiles its own small harness instead.
 *
 * Only one thing lives here: reading the GROOVY_PAD setting, which decides
 * whether the MASTER's session is opened padded -- every payload datagram
 * carried above the size a Wi-Fi link drops (CMD_INIT at 8 bytes instead of
 * 4). See libgm/include/gm.h, gm_set_padding.
 *
 * OFF is the default, so nothing changes on first launch: a MiSTer FPGA
 * only reads a 4- or 5-byte CMD_INIT and a 3-byte CMD_AUDIO. Turning
 * padding on by default would break every MiSTer master, and the protocol
 * has no way to know what is actually at the other end of the wire -- there
 * is no negotiation, only a deliberate, off-by-default choice made here.
 *
 * This setting is for the MASTER only. A follower keeps its own `pad=` key
 * inside GROOVY_FOLLOWERS (groovy_followers.h) -- unrelated to this file,
 * unrelated to GROOVY_PAD.
 *
 * `groovy_pad_parse` accepts ONLY "on" and "off" -- nothing else,
 * deliberately, same discipline as groovy_compression_mode_parse (which
 * accepts only "lz4"/"off"): a plausible-looking value like "1" or "true"
 * must not silently produce a wrong guess.
 */

#ifndef GROOVY_PAD_H
#define GROOVY_PAD_H

#include <string.h>

/* Parses the GROOVY_PAD value. "on" -> 1, "off" -> 0, anything else (NULL,
 * empty, wrong case, any other word) -> -1: the caller decides the
 * fallback and logs it -- this function never guesses silently. */
static inline int groovy_pad_parse(const char *s)
{
    if (!s)
        return -1;
    if (!strcmp(s, "on"))
        return 1;
    if (!strcmp(s, "off"))
        return 0;
    return -1;
}

static inline const char *groovy_pad_name(int on)
{
    return on ? "on" : "off";
}

/* The MASTER's setting is richer than the boolean above with the MiSTer support: two
 * more words for a MiSTer core forked to take padded HEADERS (not the full
 * padded session -- see gm.h, GM_PAD_HEADERS).
 *   off      0  the original forms (default)
 *   on       1  the full padded session (crt-bridge receiver or client)
 *   headers  2  the four short headers padded, classic chunking
 *   probe    3  ask the peer first (CMD_GET_VERSION): headers if it is the
 *               forked core, otherwise the original forms -- and, toward the
 *               original MiSTer core, no audio (its 3-byte header dies on Wi-Fi)
 * Same strictness as groovy_pad_parse: anything else -> -1, the caller names
 * the fallback. groovy_pad_parse itself is unchanged: on/off only. */
#define GROOVY_PAD_MODE_OFF      0
#define GROOVY_PAD_MODE_ON       1
#define GROOVY_PAD_MODE_HEADERS  2
#define GROOVY_PAD_MODE_PROBE    3

static inline int groovy_pad_mode_parse(const char *s)
{
    int p = groovy_pad_parse(s);

    if (p >= 0)
        return p;
    if (s && !strcmp(s, "headers"))
        return GROOVY_PAD_MODE_HEADERS;
    if (s && !strcmp(s, "probe"))
        return GROOVY_PAD_MODE_PROBE;
    return -1;
}

static inline const char *groovy_pad_mode_name(int mode)
{
    switch (mode) {
    case GROOVY_PAD_MODE_ON:      return "on";
    case GROOVY_PAD_MODE_HEADERS: return "headers";
    case GROOVY_PAD_MODE_PROBE:   return "probe";
    default:                      return "off";
    }
}

#endif /* GROOVY_PAD_H */
