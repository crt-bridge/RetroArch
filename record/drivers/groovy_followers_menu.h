/* SPDX-License-Identifier: GPL-3.0-or-later */
/* groovy_followers_menu.h -- compose the GROOVY_FOLLOWERS-grammar string
 * from the four "Additional Follower N" menu toggles, added after a
 * bench validation session.
 *
 * Header-only, same pattern as groovy_menu_precedence.h: record_groovy.c
 * includes it to run, the test bridge libgm/test/groovy_test_bridge.c
 * includes it to exercise it outside RetroArch. No dependency on gm.h --
 * this file knows nothing about the ip[:port],key=value;... grammar, it
 * only builds the "ip1;ip2;..." text that groovy_followers_parse
 * (groovy_followers.h) will read afterward.
 *
 * THREE-TIER PRECEDENCE. The caller (groovy_new, record_groovy.c) must
 * check, IN THIS ORDER, before calling this function:
 *   1. GROOVY_FOLLOWERS (environment variable) -- always wins, same
 *      discipline as the five other settings resolved in the same
 *      block;
 *   2. the groovy_followers text key of retroarch.cfg, if non-empty --
 *      the expert path, the only one carrying per-follower options
 *      (mtu, compression, audio, inputs, forme, pad) and left OUTSIDE
 *      the menu by this feature (it was impossible to clear from there:
 *      the Start button, the only upstream exit, is mapped to Enter on
 *      this bench, which is already used to confirm -- a typo would
 *      lock the bridge with no way out);
 *   3. ONLY IF the previous two are absent: the four menu toggles and
 *      addresses, composed HERE.
 *
 * EXPLICIT REFUSAL, NO SILENT FALLBACK (same spirit as
 * groovy_followers_parse): a toggle that's on with an empty address is
 * a refusal, never a dropped entry -- a follower believed active that
 * never starts would be worse than a refusal that says why.
 */

#ifndef GROOVY_FOLLOWERS_MENU_H
#define GROOVY_FOLLOWERS_MENU_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define GROOVY_FOLLOWERS_MENU_N 4u

/* groovy_followers_menu_compose -- returns 0 on success (out can be
 * empty: all toggles off, unchanged behavior, no follower), -1 on
 * refusal (err names the offending follower).
 *
 * enabled[i] / address[i] for i in 0..3 correspond to "Additional
 * Follower (i+1)". address[i] can be NULL or empty when enabled[i] is
 * false -- only an ACTIVE toggle with an empty address is a refusal.
 *
 * out is always zero-terminated on return (empty string on refusal or
 * if no toggle is active). No allocation, no dependency on gm.h -- pure
 * function, testable via ctypes outside RetroArch. */
static inline int groovy_followers_menu_compose(const int *enabled,
                                                  const char *const *address,
                                                  char *out, size_t out_cap,
                                                  char *err, size_t err_cap)
{
    size_t   pos = 0u;
    unsigned i;

    if (out && out_cap)
        out[0] = '\0';

    if (!enabled || !address)
        return 0;

    for (i = 0u; i < GROOVY_FOLLOWERS_MENU_N; i++) {
        size_t alen, need;

        if (!enabled[i])
            continue;

        if (!address[i] || address[i][0] == '\0') {
            if (err && err_cap)
                snprintf(err, err_cap,
                          "Additional Follower %u is on but its address is "
                          "empty -- set an address or turn it off", i + 1u);
            if (out && out_cap)
                out[0] = '\0';
            return -1;
        }

        alen = strlen(address[i]);
        need = alen + (pos > 0u ? 1u : 0u);
        if (!out || pos + need >= out_cap) {
            if (err && err_cap)
                snprintf(err, err_cap,
                          "composed follower list too long (Additional "
                          "Follower %u)", i + 1u);
            if (out && out_cap)
                out[0] = '\0';
            return -1;
        }

        if (pos > 0u)
            out[pos++] = ';';
        memcpy(out + pos, address[i], alen);
        pos += alen;
        out[pos] = '\0';
    }

    return 0;
}

#endif /* GROOVY_FOLLOWERS_MENU_H */
