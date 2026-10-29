/* SPDX-License-Identifier: GPL-3.0-or-later */
/* groovy_session.h -- the mode of the session: the modeline the launcher
 * chose, read from the GROOVY_MODE variable, strictly.
 *
 * The launcher picks a mode (a line frequency, a rate, a number of lines)
 * and hands it to the emitter in one environment variable. The emitter does
 * not choose and does not judge the ranges of the tube: it copies the
 * modeline it is given, announces it, and lays each picture that fits into
 * it. The receiver's own guard judges the modeline before the card is
 * touched.
 *
 * GROOVY_MODE is fourteen fields, ONE space between two fields, nothing
 * before the first and nothing after the last:
 *
 *   clock_khz  h_active h_begin h_end h_total  v_active v_begin v_end v_total
 *   p|i  factor_h factor_v  crop_lines crop_cols
 *
 * Every field but the tenth is a decimal whole number, digits only: no sign,
 * no leading zero ("0" alone is allowed), no prefix, no other character. The
 * positions are CUMULATIVE (start of sync, end of sync, total), as on the
 * wire. The tenth field is the letter p or the letter i.
 *
 * The whole value is refused, never half read, when: the text is longer than
 * 200 bytes; it does not have exactly fourteen fields, or carries a space
 * at the head, at the tail or doubled; a field is not made of digits (the
 * tenth: not p or i), has a leading zero, or overflows 32 bits; a position
 * or a crop is above 65 535; the clock is outside 1 000 to 250 000 kHz;
 * h_active or v_active is zero; the positions are not ordered (active <=
 * begin <= end <= total, horizontally and vertically); an interlaced mode
 * has an odd number of active lines; a factor is zero or above 64; the
 * picture of the mode (h_active x v_active x 3 bytes) is larger than
 * GROOVY_SESSION_MAX_FRAME_BYTES. The reason is a short English text.
 *
 * A picture FITS the mode when its lines do not exceed the lines of the mode
 * plus the crop of lines and its columns do not exceed the width of the mode
 * plus the crop of columns. The factors of the session are brought back to
 * the largest whole number that fits, and to 1 for a picture larger than
 * the mode (it is cut by the composition, as the receiver used to cut it).
 *
 * Pure: <stdbool.h>, <stddef.h>, <stdint.h>, <string.h>, nothing else. No
 * RetroArch, no gm.h, no I/O. Everything is static inline.
 */

#ifndef GROOVY_SESSION_H
#define GROOVY_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The biggest picture the receiver accepts, in bytes. It is the capacity the
 * receiver deduces from the file of the tube (8 192 000 pixels x 3). A copy:
 * a test checks it against the receiver's own code. */
#define GROOVY_SESSION_MAX_FRAME_BYTES 24576000u

#define GROOVY_SESSION_MAX_TEXT        200u      /* bytes of the variable */
#define GROOVY_SESSION_FIELDS          14u
#define GROOVY_SESSION_MIN_KHZ         1000u
#define GROOVY_SESSION_MAX_KHZ         250000u
#define GROOVY_SESSION_MAX_POSITION    65535u    /* a position, a crop */
#define GROOVY_SESSION_MAX_FACTOR      64u

struct groovy_session_mode {
    bool     valid;          /* false until a value has been read in full */
    uint32_t khz;            /* pixel clock, whole kHz */
    uint32_t h_active, h_begin, h_end, h_total;
    uint32_t v_active, v_begin, v_end, v_total;
    uint32_t interlace;      /* 1 = interlaced, 0 = progressive */
    uint32_t kx, ky;         /* factors of the engine, >= 1 */
    uint32_t crop_lines;     /* lines a picture may exceed the mode by, and be cut */
    uint32_t crop_cols;      /* same, columns */
};
/* A zeroed struct is the valid "no session" state. */

/* Copies a reason into `why`, truncated, always zero-terminated. */
static inline void groovy_session_say(char *why, size_t why_cap, const char *text)
{
    size_t i;

    if (!why || why_cap == 0u)
        return;
    for (i = 0u; text[i] != '\0' && i + 1u < why_cap; i++)
        why[i] = text[i];
    why[i] = '\0';
}

/* The refusal: `out` is left as a zeroed, invalid session, the reason is
 * written, -1 is returned. Nothing half read ever survives. */
static inline int groovy_session_refuse(struct groovy_session_mode *out,
                                        char *why, size_t why_cap, const char *text)
{
    if (out)
        memset(out, 0, sizeof(*out));
    groovy_session_say(why, why_cap, text);
    return -1;
}

/* The refusal of one field: "field <n> (<name>) <text>". */
static inline int groovy_session_refuse_field(struct groovy_session_mode *out,
                                              char *why, size_t why_cap,
                                              unsigned index, const char *name,
                                              const char *text)
{
    char   msg[96];
    size_t p = 0u;
    size_t i;
    const unsigned n = index + 1u;   /* 1-based, as the contract numbers them */

    if (out)
        memset(out, 0, sizeof(*out));
    memcpy(msg, "field ", 6u);
    p = 6u;
    if (n >= 10u)
        msg[p++] = (char)('0' + (int)(n / 10u));
    msg[p++] = (char)('0' + (int)(n % 10u));
    msg[p++] = ' ';
    msg[p++] = '(';
    for (i = 0u; name[i] != '\0' && p + 2u < sizeof(msg); i++)
        msg[p++] = name[i];
    msg[p++] = ')';
    msg[p++] = ' ';
    for (i = 0u; text[i] != '\0' && p + 1u < sizeof(msg); i++)
        msg[p++] = text[i];
    msg[p] = '\0';
    groovy_session_say(why, why_cap, msg);
    return -1;
}

/* groovy_session_parse -- read GROOVY_MODE.
 *
 *   0  : read in full (out->valid true), OR the variable is absent or empty
 *        (out->valid false, `why` empty: an empty variable is not a variable
 *        that is set, the emitter behaves as without it);
 *   -1 : refused as a whole (out->valid false and zeroed, `why` says why). */
static inline int groovy_session_parse(const char *s, struct groovy_session_mode *out,
                                       char *why, size_t why_cap)
{
    static const char *const names[GROOVY_SESSION_FIELDS] = {
        "clock", "h_active", "h_begin", "h_end", "h_total",
        "v_active", "v_begin", "v_end", "v_total", "scan",
        "factor_h", "factor_v", "crop_lines", "crop_cols"
    };
    struct groovy_session_mode m;
    uint32_t v[GROOVY_SESSION_FIELDS];
    size_t   len = 0u;
    size_t   i;
    unsigned f;

    if (why && why_cap > 0u)
        why[0] = '\0';
    if (!out) {
        groovy_session_say(why, why_cap, "no destination for the session");
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (!s || s[0] == '\0')
        return 0;

    while (s[len] != '\0' && len <= GROOVY_SESSION_MAX_TEXT)
        len++;
    if (len > GROOVY_SESSION_MAX_TEXT)
        return groovy_session_refuse(out, why, why_cap, "longer than 200 bytes");

    memset(v, 0, sizeof(v));
    i = 0u;
    for (f = 0u; f < GROOVY_SESSION_FIELDS; f++) {
        const size_t start = i;
        const size_t k = (size_t)f;
        size_t       n;
        uint64_t     acc = 0u;

        while (s[i] != '\0' && s[i] != ' ')
            i++;
        n = i - start;

        if (n == 0u) {
            if (s[start] == '\0')
                return groovy_session_refuse(out, why, why_cap,
                    f == 0u ? "empty" : "a space at the tail");
            return groovy_session_refuse(out, why, why_cap,
                f == 0u ? "a space at the head" : "two spaces in a row");
        }

        if (f == 9u) {
            if (n != 1u || (s[start] != 'p' && s[start] != 'i'))
                return groovy_session_refuse_field(out, why, why_cap, f,
                    names[k], "is neither p nor i");
            v[f] = (s[start] == 'i') ? 1u : 0u;
        } else {
            size_t d;

            for (d = 0u; d < n; d++) {
                const char c = s[start + d];

                if (c < '0' || c > '9')
                    return groovy_session_refuse_field(out, why, why_cap, f,
                        names[k], "is not made of digits");
                acc = acc * 10u + (uint64_t)(c - '0');
                if (acc > 0xFFFFFFFFull)
                    return groovy_session_refuse_field(out, why, why_cap, f,
                        names[k], "overflows 32 bits");
            }
            if (n > 1u && s[start] == '0')
                return groovy_session_refuse_field(out, why, why_cap, f,
                    names[k], "has a leading zero");
            v[f] = (uint32_t)acc;
        }

        if (f + 1u < GROOVY_SESSION_FIELDS) {
            if (s[i] == '\0')
                return groovy_session_refuse(out, why, why_cap,
                    "fewer than 14 fields");
            i++;   /* the ONE separator */
        }
    }
    if (s[i] != '\0') {
        size_t j = i;

        while (s[j] == ' ')
            j++;
        return groovy_session_refuse(out, why, why_cap,
            s[j] == '\0' ? "a space at the tail" : "more than 14 fields");
    }

    /* Values. The order of the checks only matters for a value that breaks
     * several rules at once; each rule has its own reason. */
    for (f = 1u; f <= 8u; f++)
        if (v[f] > GROOVY_SESSION_MAX_POSITION)
            return groovy_session_refuse(out, why, why_cap,
                "a position is above 65535");
    if (v[12] > GROOVY_SESSION_MAX_POSITION || v[13] > GROOVY_SESSION_MAX_POSITION)
        return groovy_session_refuse(out, why, why_cap, "a crop is above 65535");
    if (v[0] < GROOVY_SESSION_MIN_KHZ || v[0] > GROOVY_SESSION_MAX_KHZ)
        return groovy_session_refuse(out, why, why_cap,
            "clock outside 1000 to 250000 kHz");
    if (v[1] == 0u)
        return groovy_session_refuse(out, why, why_cap, "h_active is zero");
    if (v[5] == 0u)
        return groovy_session_refuse(out, why, why_cap, "v_active is zero");
    if (!(v[1] <= v[2] && v[2] <= v[3] && v[3] <= v[4]))
        return groovy_session_refuse(out, why, why_cap,
            "horizontal positions are not ordered");
    if (!(v[5] <= v[6] && v[6] <= v[7] && v[7] <= v[8]))
        return groovy_session_refuse(out, why, why_cap,
            "vertical positions are not ordered");
    if (v[9] == 1u && (v[5] & 1u) != 0u)
        return groovy_session_refuse(out, why, why_cap,
            "interlaced with an odd number of lines");
    if (v[10] == 0u || v[10] > GROOVY_SESSION_MAX_FACTOR)
        return groovy_session_refuse(out, why, why_cap,
            "factor_h is zero or above 64");
    if (v[11] == 0u || v[11] > GROOVY_SESSION_MAX_FACTOR)
        return groovy_session_refuse(out, why, why_cap,
            "factor_v is zero or above 64");
    if ((uint64_t)v[1] * (uint64_t)v[5] * 3ull > (uint64_t)GROOVY_SESSION_MAX_FRAME_BYTES)
        return groovy_session_refuse(out, why, why_cap,
            "picture larger than the receiver can take");

    memset(&m, 0, sizeof(m));
    m.valid      = true;
    m.khz        = v[0];
    m.h_active   = v[1];
    m.h_begin    = v[2];
    m.h_end      = v[3];
    m.h_total    = v[4];
    m.v_active   = v[5];
    m.v_begin    = v[6];
    m.v_end      = v[7];
    m.v_total    = v[8];
    m.interlace  = v[9];
    m.kx         = v[10];
    m.ky         = v[11];
    m.crop_lines = v[12];
    m.crop_cols  = v[13];
    *out = m;
    return 0;
}

/* groovy_session_fits -- does a picture of w x h take place in the mode? */
static inline bool groovy_session_fits(const struct groovy_session_mode *sm,
                                       unsigned w, unsigned h)
{
    if (!sm || !sm->valid || w == 0u || h == 0u)
        return false;
    return (uint64_t)w <= (uint64_t)sm->h_active + (uint64_t)sm->crop_cols
        && (uint64_t)h <= (uint64_t)sm->v_active + (uint64_t)sm->crop_lines;
}

/* groovy_session_factors -- the factors of the session brought back to what
 * fits: the largest whole number not above the engine's factor such that the
 * scaled picture stays inside the mode; at least 1, and 1 for a picture that
 * is already larger than the mode. */
static inline void groovy_session_factors(const struct groovy_session_mode *sm,
                                          unsigned w, unsigned h,
                                          unsigned *kx, unsigned *ky)
{
    unsigned x = 1u;
    unsigned y = 1u;

    if (sm && sm->valid && w != 0u && h != 0u) {
        const unsigned fit_x = (w <= sm->h_active) ? (sm->h_active / w) : 1u;
        const unsigned fit_y = (h <= sm->v_active) ? (sm->v_active / h) : 1u;

        x = (sm->kx < fit_x) ? sm->kx : fit_x;
        y = (sm->ky < fit_y) ? sm->ky : fit_y;
        if (x < 1u) x = 1u;
        if (y < 1u) y = 1u;
    }
    if (kx) *kx = x;
    if (ky) *ky = y;
}

#endif /* GROOVY_SESSION_H */
