/* SPDX-License-Identifier: GPL-3.0-or-later */
/* groovy_dump.h — the frame witness knows how to number itself.
 * Header-only, exact pattern of groovy_mtu.h: the driver includes it to
 * run, the ctypes test bridge includes it to exercise it outside
 * RetroArch.
 *
 * UNLIKE groovy_mtu.h, this header has NOTHING to take from gm.h: it
 * therefore does not include it and carries no include-order guard
 * (unlike groovy_modeline.h / groovy_pacing.h, which #error if GM_H is
 * absent). This is not an oversight -- there is simply no constant or
 * type shared with libgm here.
 *
 * WHAT THE WITNESS IS. The exact RGB888 frame handed to libgm, after
 * format conversion, after cropping, after compositing -- the only
 * faithful witness of what actually goes out: RetroArch's own window
 * draws with a stale geometry (SwanStation never emits SET_GEOMETRY
 * after loading) and so cannot serve as a referee.
 *
 * THREE VARIABLES, read once by record_groovy.c:
 *   GROOVY_DUMP_FRAME_PATH      file path; absent = witness off.
 *   GROOVY_DUMP_FRAME_INTERVAL  one frame kept out of every N, 60 by default (~1/s).
 *   GROOVY_DUMP_FRAME_SEQ       0/absent = single file overwritten (default,
 *                               the pre-existing behavior); 1 = SEQUENCE
 *                               mode, one numbered file per kept frame.
 *
 * VOLUME WARNING. In sequence mode at interval 1, that is 60 files per
 * second of 230 KB (240p 320x240) to 690 KB (Chrono Cross 512x432), i.e.
 * ~26 MB/s and ~775 MB for 30 s of Sonic 2 in 480i. THAT IS DELIBERATE,
 * meant for a CAPTURE session feeding an offline compression estimator,
 * NOT for playing.
 *
 * WHY THIS MODE EXISTS. To estimate LZ4HC / delta / RGB565 offline on
 * CONSECUTIVE frames. A single-still-image measurement does not predict
 * a live stream -- that lesson has already been paid for once (offline
 * estimates were off by roughly 2x against live measurements).
 *
 * The setting lives in an environment variable and NOT in
 * retroarch.cfg, since RetroArch rewrites that file on exit -- same
 * pattern as GROOVY_AUDIO, GROOVY_COMPRESSION, GROOVY_DOWNSAMPLE,
 * GROOVY_INPUT, GROOVY_MTU.
 */

#ifndef GROOVY_DUMP_H
#define GROOVY_DUMP_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define GROOVY_DUMP_INTERVAL_DEFAULT 60u
#define GROOVY_DUMP_INTERVAL_MIN      1u
#define GROOVY_DUMP_INTERVAL_MAX  99999u

/* Parses GROOVY_DUMP_FRAME_INTERVAL: one frame kept out of every N.
 *
 * NEVER RETURNS 0. The call site computes `idx % interval`: a zero
 * would be a division by zero, not a "keep everything" mode. That is
 * the reason GROOVY_DUMP_INTERVAL_MIN exists, and the test proves it in
 * a loop.
 *
 * NULL, empty, not entirely numeric, or out of bounds -> the default. No
 * suffix is tolerated: strtoul must have consumed the whole string. This
 * is a TIGHTENING relative to the earlier parse (strtol(iv, NULL, 10),
 * which accepted "1abc" and returned 1); the sole caller is a script. */
static inline unsigned groovy_dump_interval_parse(const char *s)
{
    char         *fin = NULL;
    unsigned long v;

    if (!s || s[0] == '\0')
        return GROOVY_DUMP_INTERVAL_DEFAULT;
    v = strtoul(s, &fin, 10);
    if (!fin || *fin != '\0')
        return GROOVY_DUMP_INTERVAL_DEFAULT;
    if (v < GROOVY_DUMP_INTERVAL_MIN || v > GROOVY_DUMP_INTERVAL_MAX)
        return GROOVY_DUMP_INTERVAL_DEFAULT;
    return (unsigned)v;
}

/* Parses GROOVY_DUMP_FRAME_SEQ. 1 -> sequence mode. ANY other value,
 * including "on" and "true", -> 0, i.e. the pre-existing behavior: a
 * single file, overwritten. Deliberately stricter than
 * groovy_input_mode_parse ("on"): the variable is set by
 * launch-emitter.ps1, never typed by hand. */
static inline int groovy_dump_seq_parse(const char *s)
{
    return (s && s[0] == '1' && s[1] == '\0') ? 1 : 0;
}

/* Returns in dst the witness file name for frame `counter`.
 *
 * seq == 0: dst receives path unchanged -- the pre-existing behavior.
 * seq == 1: ".%06u" is inserted BEFORE the extension of the FILE NAME.
 *     C:\cap\frame.ppm , 123  ->  C:\cap\frame.000123.ppm
 *     C:\cap\frame     , 123  ->  C:\cap\frame.000123
 *
 * PITFALL closed here: the extension is looked up in the last segment
 * of the path, after the last '/' AND the last '\\'. A dot in a
 * DIRECTORY name (C:\cap.d\frame) is not one, otherwise the result
 * would be C:\cap.000123.d\frame -- a path into a directory that does
 * not exist.
 *
 * The counter is that of EMITTED frames, not of kept frames: the gap
 * between two numbers says how many frames were skipped. Beyond
 * 999999, printf widens the field (it does not truncate) and name-sort
 * stops being number-sort -- 4 h 37 at 60 frames/s.
 *
 * Returns 1 if the name fits in dst, 0 otherwise (truncation, or a null
 * argument). On failure dst[0] is '\0': never a half-written name. */
static inline int groovy_dump_name(char *dst, size_t cap, const char *path,
                                   int seq, uint32_t counter)
{
    const char *base, *dot, *p;
    int n;

    if (!dst || !cap || !path)
        return 0;
    dst[0] = '\0';

    if (!seq) {
        n = snprintf(dst, cap, "%s", path);
        if (n < 0 || (size_t)n >= cap) { dst[0] = '\0'; return 0; }
        return 1;
    }

    base = path;
    for (p = path; *p; ++p)
        if (*p == '/' || *p == '\\')
            base = p + 1;
    dot = strrchr(base, '.');

    if (dot)
        n = snprintf(dst, cap, "%.*s.%06u%s",
                     (int)(dot - path), path, counter, dot);
    else
        n = snprintf(dst, cap, "%s.%06u", path, counter);

    if (n < 0 || (size_t)n >= cap) { dst[0] = '\0'; return 0; }
    return 1;
}

#endif /* GROOVY_DUMP_H */
