/* SPDX-License-Identifier: GPL-3.0-or-later */
/* groovy_menu_precedence.h -- the environment variable wins over the menu.
 *
 * Header-only, same model as groovy_compression.h: the driver includes it
 * to run, the test bridge includes it to exercise it outside RetroArch.
 *
 * WHY. Six bridge settings live in retroarch.cfg and can be set from the
 * menu. The GROOVY_* variables keep working AND KEEP WINNING: that is
 * what makes a bench session reproducible, and what lets a session run
 * unattended. The menu proposes, the variable disposes, and the program
 * SAYS ON SCREEN when it is forced, naming the setting, the imposed
 * value and the responsible variable.
 *
 * TWO RULES THAT ARE NOT UP FOR DEBATE:
 *   - a SET variable wins, even if it says the same thing as the menu.
 *     No value comparison happens here: full transparency -- every set
 *     variable speaks, unfiltered;
 *   - an EMPTY variable is not a set variable: the menu decides.
 *
 * NO UPSTREAM PRECEDENT. The antonioginer/RetroArch "mister" branch reads
 * no environment variable at all (zero getenv in gfx/gfx_mister.c):
 * everything there comes from config_get_ptr(). The only model is our
 * own client, already delivered and proven: client/core/groovy_libretro.c.
 */

#ifndef GROOVY_MENU_PRECEDENCE_H
#define GROOVY_MENU_PRECEDENCE_H

#include <stddef.h>
#include <stdio.h>

/* 640 = the 512 characters groovy_followers_parse accepts at most
 * (GROOVY_FOLLOWERS_LEN_MAX) + the setting name + the variable name +
 * the fixed text. A longer value truncates the message, it does not
 * suppress it: the head of the message already names the setting. */
#define GROOVY_FORCED_MSG_CAP 640u

/* 1 if the variable wins (set AND non-empty), 0 otherwise. */
static inline int groovy_precedence_env_wins(const char *env_value)
{
    return (env_value && env_value[0] != '\0') ? 1 : 0;
}

/* Builds the forcing message. Returns:
 *   > 0  the length written (full message, or truncated to cap-1);
 *     0  the variable does not win -- dst is empty, nothing to say;
 *    -1  unusable argument (dst null, cap zero, key or variable null).
 * The format is fixed and copied verbatim from the client. */
static inline int groovy_precedence_msg(char *dst, size_t cap,
                                        const char *key,
                                        const char *env_value,
                                        const char *variable)
{
    int n;

    if (!dst || cap == 0u)
        return -1;
    dst[0] = '\0';

    if (!groovy_precedence_env_wins(env_value))
        return 0;
    if (!key || !variable)
        return -1;

    n = snprintf(dst, cap, "%s: forced to %s by %s, menu value ignored",
                 key, env_value, variable);
    if (n < 0) {
        dst[0] = '\0';
        return -1;
    }
    if ((size_t)n >= cap)
        return (int)(cap - 1u);   /* truncated, always zero-terminated */
    return n;
}

#endif /* GROOVY_MENU_PRECEDENCE_H */
