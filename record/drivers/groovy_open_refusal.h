/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef GROOVY_OPEN_REFUSAL_H
#define GROOVY_OPEN_REFUSAL_H

/* Open refusal -- the three messages that say why a receiver did not open.
 *
 * A refused master used to make the driver return NULL without a single
 * line: a black CRT and no reason anywhere. A refused follower used to take
 * the master down with it. The driver now says both, and these are the
 * exact texts it writes:
 *
 *   GROOVY_REFUSAL_MASTER_OPEN    master "<addr>:<port>" refused to open: <cause>
 *   GROOVY_REFUSAL_FOLLOWER_OPEN  follower "<addr>:<port>" refused to open: <cause>;
 *                                 dropped for this session, the master starts
 *   GROOVY_REFUSAL_MASTER_INIT    master "<addr>:<port>" refused the session
 *                                 opening: CMD_INIT send failed (code <code>)
 *
 * The address is quoted and cut at 63 characters: the master's address may
 * be a free host name from the configuration, and it can neither overflow
 * the buffer nor blend into the rest of the message. The cause is meant to
 * come from a static table of libgm (gm_init_error_str), never from outside.
 * A NULL address is written "(none)", a NULL cause "unknown error".
 *
 * Pure: no dependency on RetroArch or libgm, so a gcc harness can build it
 * together with libgm's sources and check the exact text against the real
 * causes. */

#include <stddef.h>
#include <stdio.h>

#define GROOVY_REFUSAL_MASTER_OPEN   0
#define GROOVY_REFUSAL_FOLLOWER_OPEN 1
#define GROOVY_REFUSAL_MASTER_INIT   2

/* Writes the message of `kind` into buf (cap bytes, always NUL-terminated
 * when cap > 0, truncated if needed). An unknown kind writes an empty
 * string. `code` is only read by GROOVY_REFUSAL_MASTER_INIT, `cause` only by
 * the two open kinds. Returns what snprintf returned, or 0 when nothing
 * could be written. */
static inline int groovy_open_refusal_format(char *buf, size_t cap, int kind,
                                             const char *addr, unsigned port,
                                             const char *cause, int code)
{
    const char *a = addr  ? addr  : "(none)";
    const char *c = cause ? cause : "unknown error";

    if (!buf || cap == 0u)
        return 0;
    switch (kind) {
    case GROOVY_REFUSAL_MASTER_OPEN:
        return snprintf(buf, cap, "master \"%.63s:%u\" refused to open: %s",
                        a, port, c);
    case GROOVY_REFUSAL_FOLLOWER_OPEN:
        return snprintf(buf, cap, "follower \"%.63s:%u\" refused to open: %s; "
                        "dropped for this session, the master starts",
                        a, port, c);
    case GROOVY_REFUSAL_MASTER_INIT:
        return snprintf(buf, cap, "master \"%.63s:%u\" refused the session "
                        "opening: CMD_INIT send failed (code %d)",
                        a, port, code);
    default:
        buf[0] = '\0';
        return 0;
    }
}

#endif /* GROOVY_OPEN_REFUSAL_H */
