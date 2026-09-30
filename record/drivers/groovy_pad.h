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

#endif /* GROOVY_PAD_H */
