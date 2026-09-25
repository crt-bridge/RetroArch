/* SPDX-License-Identifier: GPL-3.0-or-later */
/* groovy_skip.h -- diagnostic hook: deliberately skips sending one frame
 * in N. Header-only, pattern from groovy_dump.h -- NOT the one from
 * groovy_mtu.h: this setting has NO counterpart on the protocol side nor
 * on the libgm side, so it includes no libgm header and carries no
 * inclusion-order guard.
 *
 * WHAT THIS HOOK IS FOR. A deliberately missing blit is tolerated by the
 * emitter's ack regulator and by the receivers. Without this hook, the
 * gap in the frame_id sequence would be neither exact nor reproducible,
 * and the proof would rest on an incidental network loss.
 *
 * WHAT IT NEVER TOUCHES. frame_id -- the gap in the sequence IS the very
 * subject of the proof, not a side effect to hide. frame_id advances
 * exactly as it would for any other frame.
 *
 * THE VERY FIRST FRAME IS SKIPPED, AND THAT IS INTENTIONAL. frame_id
 * starts at 0 and (0 % every) == 0: with the hook active, frame 0 of the
 * session is the first one skipped, then one in every `every` (every,
 * 2*every, ...). That's not what "one frame in N" suggests at first
 * glance, but it's the formula that test_groovy_skip.py locks down
 * (n % 10 == 0 for n in 0..99) and that bench runs have already
 * measured. Replacing it with (frame_id % every) == every - 1 would move
 * the gap by one frame without proving anything more: it's a measurement
 * instrument, described as-is, never retouched.
 *
 * THE SIGNAL EXPECTED ON THE BENCH. A rise in the net= percentile of the
 * [groovy-lat] line (the sent - echo gap), NOT a rise in anomalies --
 * that counter only moves past a gap of 1000 frames (s_lat_anomalies, in
 * record_groovy.c's [groovy-lat] report), which a reasonable hook never
 * reaches.
 *
 * The setting lives in an environment variable and NOT in retroarch.cfg:
 * RetroArch rewrites that file on exit from its own settings list, so a
 * key added by hand would disappear. Same pattern as GROOVY_AUDIO,
 * GROOVY_COMPRESSION, GROOVY_DOWNSAMPLE, GROOVY_INPUT, GROOVY_MTU,
 * GROOVY_DUMP_FRAME_PATH.
 *
 * DO NOT name this variable GROOVY_DUMP_FRAME_PATH: it already exists
 * and serves the frame witness, a different role. The name chosen here
 * is GROOVY_SKIP_FRAME_EVERY.
 *
 * DELIBERATE DIFFERENCE from groovy_dump_interval_parse: here the
 * default IS zero, and zero means "nothing skips" -- this isn't a
 * division by zero because groovy_skip_should_skip tests every > 0u
 * BEFORE any modulo. GROOVY_SKIP_EVERY_MIN = 2 exists so that a
 * GROOVY_SKIP_FRAME_EVERY=1 (which would skip EVERY frame) falls back to
 * 0 instead of cutting the wire.
 */

#ifndef GROOVY_SKIP_H
#define GROOVY_SKIP_H

#include <stdlib.h>
#include <stdint.h>

#define GROOVY_SKIP_EVERY_DEFAULT     0u   /* absent or invalid: nothing skips */
#define GROOVY_SKIP_EVERY_MIN         2u   /* 1 would skip EVERY frame: refused */
#define GROOVY_SKIP_EVERY_MAX     99999u

/* Parses GROOVY_SKIP_FRAME_EVERY. Verbatim discipline from
 * groovy_dump_interval_parse: strtoul, no suffix tolerated
 * (*fin != '\0'), bounds [GROOVY_SKIP_EVERY_MIN, GROOVY_SKIP_EVERY_MAX].
 * NULL, empty, not entirely numeric, or out of bounds -> the default,
 * i.e. zero, i.e. "hook off". */
static inline unsigned groovy_skip_every_parse(const char *s)
{
    char         *fin = NULL;
    unsigned long v;

    if (!s || s[0] == '\0')
        return GROOVY_SKIP_EVERY_DEFAULT;
    v = strtoul(s, &fin, 10);
    if (!fin || *fin != '\0')
        return GROOVY_SKIP_EVERY_DEFAULT;
    if (v < GROOVY_SKIP_EVERY_MIN || v > GROOVY_SKIP_EVERY_MAX)
        return GROOVY_SKIP_EVERY_DEFAULT;
    return (unsigned)v;
}

/* Decides whether frame frame_id should be skipped. every == 0u -> never
 * (guard BEFORE any modulo, no division by zero possible). Otherwise,
 * one frame in `every`, exactly -- frame 0 included (see file header). */
static inline int groovy_skip_should_skip(unsigned every, uint32_t frame_id)
{
    if (every == 0u)
        return 0;
    return (frame_id % (uint32_t)every) == 0u ? 1 : 0;
}

#endif /* GROOVY_SKIP_H */
