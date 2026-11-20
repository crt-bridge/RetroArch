/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef GROOVY_MODE_REFRESH_H
#define GROOVY_MODE_REFRESH_H

/* Periodic mode refresh -- a pure decision.
 *
 * Why. An acknowledgement proves neither a mode nor a picture. A stock MiSTer
 * core acknowledges the header of every frame (sendACK runs when the header
 * arrives, before anything is shown), including the frames it throws away
 * with "Skipped no modeline" after it lost its modeline. The emitter then
 * believes the session is alive and never says the mode again. A CMD_INIT
 * lost on the way, a core reloaded under a running emitter, a CMD_SWITCHRES
 * of the opening that died on a Wi-Fi link: every one ends in a black screen
 * with a flowing stream of acknowledgements.
 *
 * What. Another signal is cheap: say the mode again, at a fixed interval,
 * whatever the receiver answers. A CMD_SWITCHRES identical to the one the
 * receiver already holds changes nothing there: the stock core recomputes the
 * same registers, and the crt-bridge receiver publishes a mode only when it
 * differs from the last one served. It is sent between two frames (the sender
 * is one thread), never inside one.
 *
 * Interval. GROOVY_MODE_REFRESH_DEFAULT_MS: long enough to cost nothing (one
 * 26-byte datagram), short enough that a black screen heals before a person
 * walks away. The bench variable GROOVY_MODE_REFRESH_MS changes it; "off" or
 * 0 disables the refresh.
 *
 * Pure: <stdint.h>, <stdlib.h>, <string.h>; the clock is a number.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define GROOVY_MODE_REFRESH_DEFAULT_MS  5000u
#define GROOVY_MODE_REFRESH_MIN_MS      500u      /* below this, a typo rather than a choice */
#define GROOVY_MODE_REFRESH_MAX_MS      600000u

/* "off" or "0" -> 0 (disabled); a whole number of milliseconds in
 * [MIN, MAX] -> that number; absent, empty, or anything else -> the default. */
static inline unsigned groovy_mode_refresh_parse(const char *s)
{
    unsigned long v = 0u;
    size_t        i;

    if (!s || s[0] == '\0')
        return GROOVY_MODE_REFRESH_DEFAULT_MS;
    if (!strcmp(s, "off") || !strcmp(s, "0"))
        return 0u;
    for (i = 0u; s[i] != '\0'; i++) {
        if (s[i] < '0' || s[i] > '9' || i >= 7u)
            return GROOVY_MODE_REFRESH_DEFAULT_MS;
        v = v * 10u + (unsigned long)(s[i] - '0');
    }
    if (v < GROOVY_MODE_REFRESH_MIN_MS || v > GROOVY_MODE_REFRESH_MAX_MS)
        return GROOVY_MODE_REFRESH_DEFAULT_MS;
    return (unsigned)v;
}

/* 1 when the mode must be said again to a receiver now.
 *   interval_ms   : 0 = disabled;
 *   have_mode     : something has been announced (nothing to repeat before);
 *   pending       : a deferred CMD_SWITCHRES of a re-announce is still to go
 *                   -- it is the mode, about to be said, so nothing is added;
 *   last_sent_ms  : the later of the last announce attempted to this
 *                   receiver, any site, and the last refresh (0 = never);
 *   now_ms        : the same clock.
 * A clock that went backwards never fires. */
static inline int groovy_mode_refresh_due(unsigned interval_ms, int have_mode, int pending,
                                          uint64_t last_sent_ms, uint64_t now_ms)
{
    if (interval_ms == 0u || !have_mode || pending)
        return 0;
    if (now_ms < last_sent_ms)
        return 0;
    return (now_ms - last_sent_ms) >= (uint64_t)interval_ms;
}

#endif /* GROOVY_MODE_REFRESH_H */
