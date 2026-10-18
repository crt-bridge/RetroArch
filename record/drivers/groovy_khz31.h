/* SPDX-License-Identifier: GPL-3.0-or-later */
/* groovy_khz31.h -- "the receiver takes 31 kHz" setting for the crt-bridge
 * emitter, no dependency on RetroArch. Header-only, same pattern as
 * groovy_pad.h: record_groovy.c includes it to run. No test-bridge include
 * here -- emitter/test/test_groovy_khz31.py compiles its own small harness.
 *
 * Only one thing lives here: reading the GROOVY_31KHZ setting, which tells
 * the emitter that the MASTER receiver's tube has the 31 kHz band. When it
 * is on, an image taller than 280 lines is announced as a progressive 480p
 * mode instead of an interlaced 480i one (groovy_mode.h).
 *
 * OFF is the default, so nothing changes on first launch: a 15 kHz-only tube
 * cannot show a 31 kHz mode, and the protocol has no way to ask the receiver
 * what its tube takes -- it is a deliberate, off-by-default choice made here.
 *
 * `groovy_khz31_parse` accepts ONLY "on" and "off" -- nothing else,
 * deliberately, same discipline as groovy_pad_parse: a plausible-looking
 * value like "1" or "true" must not silently turn a 31 kHz mode on.
 */

#ifndef GROOVY_KHZ31_H
#define GROOVY_KHZ31_H

#include <string.h>

/* Parses the GROOVY_31KHZ value. "on" -> 1, "off" -> 0, anything else (NULL,
 * empty, wrong case, any other word) -> -1: the caller decides the
 * fallback and logs it -- this function never guesses silently. */
static inline int groovy_khz31_parse(const char *s)
{
    if (!s)
        return -1;
    if (!strcmp(s, "on"))
        return 1;
    if (!strcmp(s, "off"))
        return 0;
    return -1;
}

static inline const char *groovy_khz31_name(int on)
{
    return on ? "on" : "off";
}

#endif /* GROOVY_KHZ31_H */
