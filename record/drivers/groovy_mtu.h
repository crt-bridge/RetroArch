/* SPDX-License-Identifier: GPL-3.0-or-later */
/* groovy_mtu.h -- payload datagram size sent to ONE receiver. Header-only,
 * same pattern as groovy_compression.h: the driver includes it to run, the
 * test bridge includes it to exercise it outside RetroArch.
 *
 * Only one thing lives here: reading the GROOVY_MTU setting, handed to
 * libgm via gm_set_mtu when the session opens.
 *
 * WHY. A peer joined over WireGuard (Tailscale) presents an MTU of 1280;
 * our 1472 bytes then leave as TWO IP fragments. Measured on one run:
 * 324,332 fragmented datagrams, 648,664 fragments created. On the LAN
 * almost nothing is lost, but on a WAN a single lost fragment loses the
 * whole datagram, and some equipment drops fragments on principle.
 *
 * The setting lives in an environment variable, NOT in retroarch.cfg:
 * RetroArch rewrites that file on exit from its own settings list, so a
 * key added by hand would disappear. Same pattern as GROOVY_AUDIO,
 * GROOVY_COMPRESSION, GROOVY_DOWNSAMPLE and GROOVY_INPUT.
 *
 * 1472 is the default: without GROOVY_MTU, the wire doesn't move by a
 * byte.
 *
 * The bounds and the default come from gm.h -- a single source, so the
 * two sides can't drift apart.
 */

#ifndef GROOVY_MTU_H
#define GROOVY_MTU_H

#include <stdlib.h>
#include <gm.h>   /* GM_MTU_DEFAULT / GM_MTU_MIN / GM_MTU_MAX */

/* Parses the GROOVY_MTU value. NULL, empty, not entirely numeric, or out
 * of [GM_MTU_MIN, GM_MTU_MAX] -> GM_MTU_DEFAULT, i.e. the wire unchanged.
 * No suffix is tolerated: strtoul must have consumed the whole string.
 * Leading whitespace is accepted (strtoul's own behavior) and harmless --
 * the value comes from a script, not from a network. */
static inline unsigned groovy_mtu_parse(const char *s)
{
    char         *fin = NULL;
    unsigned long v;

    if (!s || s[0] == '\0')
        return GM_MTU_DEFAULT;

    v = strtoul(s, &fin, 10);
    if (!fin || *fin != '\0')
        return GM_MTU_DEFAULT;
    if (v < (unsigned long)GM_MTU_MIN || v > (unsigned long)GM_MTU_MAX)
        return GM_MTU_DEFAULT;
    return (unsigned)v;
}

#endif /* GROOVY_MTU_H */
