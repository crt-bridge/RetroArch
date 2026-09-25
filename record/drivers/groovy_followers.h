/* SPDX-License-Identifier: GPL-3.0-or-later */
/* groovy_followers.h -- GROOVY_FOLLOWERS parser, provenance filter,
 * follower saturation, inputs merge.
 * Header-only, same pattern as groovy_reannounce.h: record_groovy.c
 * includes it to run, the test bridge libgm/test/groovy_test_bridge.c
 * includes it to exercise it outside RetroArch.
 *
 * WHAT THIS FILE DOES, AND WHAT IT DOESN'T.
 *
 * The fan-out adds up to GROOVY_FOLLOWERS_MAX buffered followers
 * alongside the clock master (the local tube). This file carries six
 * pure decisions, all with no dependency on RetroArch or the network:
 *
 *   1. groovy_followers_parse -- reads the GROOVY_FOLLOWERS environment
 *      variable and returns a table of struct groovy_follower_config.
 *      Absent or empty: 0 followers, wire unchanged -- that's today's
 *      behavior, to the letter. Any malformed entry REFUSES to start,
 *      never a silent fallback: a half-understood follower list is
 *      worse than a refusal. The error message names the offending
 *      entry -- the caller (groovy_new) logs that message then refuses
 *      to start, same gesture as parse_receiver_config on a missing
 *      video_record_config.
 *
 *   2. groovy_status_from_declared -- the provenance filter: a status
 *      only belongs to a receiver if IP AND port both equal the address
 *      declared for it. Port INCLUDED, unlike the inputs channel (which
 *      compares the IP alone, gm_poll_inputs): the real receiver and the
 *      client answer from their single bound socket (receiver/net.c),
 *      so a status's source port is stable. It's also the only filter
 *      that makes a loopback test discriminating when a master and a
 *      simulated follower share 127.0.0.1.
 *
 *   3. groovy_sat_should_skip -- a follower's saturation policy. Signal:
 *      the number of frames SENT to THIS follower and not yet acked by
 *      it, counted on the ring that groovy_sat_note_sent keeps at every
 *      real send (groovy_sat_in_flight) -- never the global frame_id,
 *      which would freeze the skip forever as soon as ANY OTHER
 *      receiver moves on. Starting thresholds, not calibrated on the
 *      bench, to be confirmed by saturations=: entry at a gap of 8, exit at
 *      a gap of 2, one probe every 4 skipped frames so a follower that
 *      came back within the window is never left completely silent.
 *      The exit is also judged on the episode's FIRST probe, frozen
 *      until its ack.
 *
 *   4. groovy_input_fold_joy / groovy_input_fold_ps2 /
 *      groovy_input_fold_mouse -- the inputs merge by attribution: the
 *      master is in IDENTITY (both its controllers copied as-is), a P1
 *      follower merges its single controller into the receiver's
 *      player 1, P2 into player 2, OFF touches nothing. A follower's
 *      controller 2 is never read: one follower = one player, within
 *      the MAX_USERS_MISTER bound of 2. Keyboard and mouse: the master
 *      alone. Since the rule was tightened, it only lives in fold_ps2
 *      and fold_mouse, which take_ps2 and take_mouse call.
 *
 *   5. groovy_input_is_live -- the aging of a follower's controller:
 *      silent for more than GROOVY_INPUT_STALE_NS, it stops counting in
 *      the merge. The master is never aged.
 *
 *   6. groovy_input_take_joy_rc -- what groovy_input_take_joy returns to
 *      the controller driver: -1 with no channel hooked at all;
 *      otherwise 0, with an EMPTY merge, so everything released, when
 *      no source is alive anymore.
 *
 * REFUSAL WITH NO FALLBACK, THE PATTERN TO FOLLOW (same pattern as
 * parse_receiver_config in record_groovy.c). Bounds: snprintf everywhere
 * for err and ip_text, no malloc, the variable's length bounded to
 * GROOVY_FOLLOWERS_LEN_MAX before even starting to split entries. A
 * fifth follower is a refusal, never a table overrun
 * (GROOVY_FOLLOWERS_MAX, a hard bound).
 *
 * This file depends on gm.h (for gm_joy_inputs, gm_ps2_inputs, GM_MTU_*)
 * -- same inclusion-order guard as groovy_pacing.h and groovy_field.h.
 */

#ifndef GROOVY_FOLLOWERS_H
#define GROOVY_FOLLOWERS_H

#ifndef GM_H
#  error "groovy_followers.h requiert gm.h avant lui"
#endif

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

#define GROOVY_FOLLOWERS_MAX        4u    /* hard bound */
#define GROOVY_FOLLOWERS_LEN_MAX    512u
#define GROOVY_FOLLOWERS_ERR_CAP    192u
#define GROOVY_FOLLOWERS_PORT_DEFAULT 32100u

#define GROOVY_SAT_INPUT  8u   /* gap at which a follower enters skip */
#define GROOVY_SAT_OUTPUT  2u   /* gap at which it exits */
#define GROOVY_SAT_PROBE   4u   /* consecutive skips before a probe frame */
#define GROOVY_SAT_RING   16u   /* IDs of SENT frames kept per follower */

/* A FOLLOWER's controller silent for more than 500 ms -- 30 frames at
 * 60 Hz -- stops counting in the merge. See groovy_input_is_live. */
#define GROOVY_INPUT_STALE_NS (500ull * 1000000ull)

/* GROOVY_INPUTS_IDENTITY is never produced by the parser -- it's the
 * value record_groovy.c passes for the MASTER's own contribution to the
 * merge. Followers only ever carry P1, P2 or OFF on the GROOVY_FOLLOWERS
 * wire. */
enum groovy_inputs {
    GROOVY_INPUTS_OFF      = 0,
    GROOVY_INPUTS_P1       = 1,
    GROOVY_INPUTS_P2       = 2,
    GROOVY_INPUTS_IDENTITY = 3
};

enum groovy_shape {
    GROOVY_SHAPE_IMAGE = 0,
    GROOVY_SHAPE_FIELD = 1
};

/* 48 bytes on Win64 (default alignment, no #pragma pack here -- unlike
 * the wire structs in gm.h, this one never crosses the network).
 * ip_text[16] + ip(4) + port(2) + 2 padding bytes (4-byte alignment for
 * mtu) + mtu(4) + compression(4) + audio(4) + inputs(4) + shape(4) +
 * pad(4). */
struct groovy_follower_config {
    char     ip_text[16];
    uint32_t ip;
    uint16_t port;
    unsigned mtu;
    int      compression;
    int      audio;
    int      inputs;
    int      shape;
    int      pad;
};
_Static_assert(sizeof(struct groovy_follower_config) == 48u,
               "groovy_follower_config must stay 48 bytes (ctypes mirror)");

/* 84 bytes: skipping + run + probe_id + probe_out, then the ring of
 * SENT IDs, sent_ring[GROOVY_SAT_RING] + sent_head. Only 4-byte fields,
 * no padding (ctypes SatState mirror). */
struct groovy_sat_state {
    int      skipping;
    unsigned run;
    uint32_t probe_id;    /* frame_id of the FIRST probe of the skip episode */
    int      probe_out;   /* 1 = that probe went out and its ack hasn't come back */
    uint32_t sent_ring[GROOVY_SAT_RING]; /* frame_id of the last frames SENT to THIS follower */
    unsigned sent_head;                  /* next slot of sent_ring to be written */
};
_Static_assert(sizeof(struct groovy_sat_state) == 84u,
               "groovy_sat_state must stay 84 bytes (ctypes mirror)");
_Static_assert(GROOVY_SAT_RING >= GROOVY_SAT_INPUT,
               "the ring must be able to count up to the entry threshold");

/* ---------------------------------------------------------------------------
 * groovy_followers_ip_parse -- exactly four decimal groups of 1 to 3
 * digits, each <= 255, separated by three dots, NOTHING else (the
 * string [s, s+len) must be entirely consumed). Returns 1 and sets
 * *out_ip in network order on a little-endian host (a | b<<8 | c<<16 |
 * d<<24) -- Windows only, like the rest of the driver. Returns 0 on
 * failure; *out_ip is left untouched.
 * ------------------------------------------------------------------------- */
static inline int groovy_followers_ip_parse(const char *s, size_t len, uint32_t *out_ip)
{
    unsigned parts[4];
    size_t   i = 0u;
    unsigned part_idx = 0u;

    if (!s || !out_ip)
        return 0;

    while (part_idx < 4u) {
        size_t   digits = 0u;
        unsigned v = 0u;

        while (i < len && s[i] >= '0' && s[i] <= '9') {
            if (digits >= 3u)
                return 0;   /* more than three digits in a group */
            v = v * 10u + (unsigned)(s[i] - '0');
            digits++;
            i++;
        }
        if (digits == 0u)
            return 0;       /* empty group */
        if (v > 255u)
            return 0;
        parts[part_idx] = v;
        part_idx++;

        if (part_idx < 4u) {
            if (i >= len || s[i] != '.')
                return 0;
            i++;
        }
    }
    if (i != len)
        return 0;           /* text after the fourth group */

    *out_ip = (uint32_t)parts[0]
            | ((uint32_t)parts[1] << 8)
            | ((uint32_t)parts[2] << 16)
            | ((uint32_t)parts[3] << 24);
    return 1;
}

/* Bounded decimal parsing, generic for mtu and port: only digits over
 * [s, s+len), no sign, no whitespace. Overflow guard before each
 * multiplication -- a pathologically long string returns 0 instead of
 * silently overflowing. */
static inline int groovy_followers_uint_parse(const char *s, size_t len, unsigned long *out)
{
    unsigned long v = 0ul;
    size_t        i;

    if (!s || len == 0u || !out)
        return 0;
    for (i = 0u; i < len; i++) {
        if (s[i] < '0' || s[i] > '9')
            return 0;
        if (v > (ULONG_MAX - 9ul) / 10ul)
            return 0;
        v = v * 10ul + (unsigned long)(s[i] - '0');
    }
    *out = v;
    return 1;
}

/* Formats the error message: "entry N "<substring>": <reason>". The
 * substring is bounded to 64 characters -- enough to identify the
 * offending entry, never enough to overflow an undersized caller
 * buffer. */
static inline void groovy_followers_fail(char *err, size_t err_cap, unsigned index,
                                          const char *entry_start, size_t entry_len,
                                          const char *reason)
{
    size_t quoted_len = (entry_len < 64u) ? entry_len : 64u;

    if (!err || err_cap == 0u)
        return;
    snprintf(err, err_cap, "entry %u \"%.*s\": %s",
              index, (int)quoted_len, entry_start, reason);
}

/* ---------------------------------------------------------------------------
 * groovy_followers_parse -- grammar:
 *   GROOVY_FOLLOWERS = follower { ";" follower }
 *   follower         = ip [ ":" port ] { "," key "=" value }
 *   keys             = mtu | compression | audio | inputs | shape | pad
 *
 * s absent or empty -> *n_out = 0, returns 0 (wire unchanged, today's
 * behavior to the letter). Any malformed entry -> returns -1, *n_out = 0,
 * *err names the offending entry -- NEVER a silent fallback.
 *
 * master_ip == 0 means "no canonical master yet" (a caller that doesn't
 * know the master's address yet, e.g. an isolated test): the
 * "follower == master" check is then skipped rather than refusing a
 * follower at address 0.0.0.0 by accident.
 *
 * No allocation: in-place scan over s, bounds checked at every step.
 *
 * The `pad` key -- padded-session mode, negotiated by this receiver,
 * NEVER case by case:
 *
 *   | key   | values    | default |
 *   |-------|-----------|---------|
 *   | `pad` | `on`/`off`| `off`   |
 *
 * `off` (absent): wire bit-identical to before padded sessions existed,
 * guarded by test_pcap_diff_groovy_baseline.py. `on`: every
 * emitter->receiver command under 8 bytes goes out at exactly 8 bytes,
 * zero-padded at the end -- 8 is the size of the frame header, one byte
 * above the measured 7-byte threshold on the Wi-Fi link that motivated
 * this feature.
 *
 * FORBIDDEN: `pad=on` only makes sense toward a `crt-bridge-rcv`
 * receiver or a `gmclient` client, both of which know the padded-session
 * mode -- NEVER toward a MiSTer FPGA, which can only read a 4- or 5-byte
 * CMD_INIT and a 3-byte CMD_AUDIO. The protocol has no way to detect
 * what's on the other end of the wire: it's on the operator never to set
 * `pad=on` toward a MiSTer. record_groovy.c logs a warning when the key
 * is active, naming the target.
 * ------------------------------------------------------------------------- */
static inline int groovy_followers_parse(const char *s,
                                          uint32_t master_ip, uint16_t master_port,
                                          int master_compression, int master_shape,
                                          struct groovy_follower_config *out, unsigned cap,
                                          unsigned *n_out, char *err, size_t err_cap)
{
    size_t   total_len;
    size_t   pos;
    unsigned index;
    unsigned accepted = 0u;

    if (n_out)
        *n_out = 0u;

    if (!s || s[0] == '\0')
        return 0;

    total_len = strlen(s);
    if (total_len > (size_t)GROOVY_FOLLOWERS_LEN_MAX) {
        size_t quoted_len = (total_len < 64u) ? total_len : 64u;
        groovy_followers_fail(err, err_cap, 1u, s, quoted_len,
                              "variable too long (512 characters max)");
        return -1;
    }

    pos   = 0u;
    index = 0u;
    for (;;) {
        const char *entry_start = s + pos;
        size_t      entry_len   = 0u;
        int         has_more;
        size_t      scan;
        size_t      ip_end;
        size_t      p;
        uint32_t    ip;
        uint16_t    port;
        unsigned    mtu;
        int         compression, audio, inputs, shape, pad;
        int         seen_mtu = 0, seen_compression = 0, seen_audio = 0;
        int         seen_inputs = 0, seen_shape = 0, seen_pad = 0;
        unsigned    dup_i;

        while (pos + entry_len < total_len && s[pos + entry_len] != ';')
            entry_len++;

        index++;
        has_more = (pos + entry_len < total_len) ? 1 : 0;

        /* Hard bound: the fifth follower (or beyond) is a refusal,
         * never a table overrun. */
        if (accepted >= cap) {
            char reason[64];
            snprintf(reason, sizeof(reason), "more than %u followers", cap);
            groovy_followers_fail(err, err_cap, index, entry_start, entry_len, reason);
            return -1;
        }

        if (entry_len == 0u) {
            groovy_followers_fail(err, err_cap, index, entry_start, entry_len,
                                  "empty entry");
            return -1;
        }

        for (scan = 0u; scan < entry_len; scan++) {
            if (entry_start[scan] == ' ') {
                groovy_followers_fail(err, err_cap, index, entry_start, entry_len,
                                      "space not allowed");
                return -1;
            }
        }

        ip_end = entry_len;
        for (scan = 0u; scan < entry_len; scan++) {
            if (entry_start[scan] == ':' || entry_start[scan] == ',') {
                ip_end = scan;
                break;
            }
        }
        if (ip_end == 0u) {
            /* ip missing: either the entry starts with ':' or ',', or
             * (the string ",") it's nothing else at all. */
            groovy_followers_fail(err, err_cap, index, entry_start, entry_len,
                                  "empty entry");
            return -1;
        }

        if (!groovy_followers_ip_parse(entry_start, ip_end, &ip)) {
            groovy_followers_fail(err, err_cap, index, entry_start, entry_len,
                                  "invalid IPv4 address (a.b.c.d expected)");
            return -1;
        }

        /* These addresses pass the grammar but don't name any follower.
         * 0.x.x.x (0.0.0.0 included) silently broke the provenance
         * filter -- 0 served as a sentinel -- and gave an accepted but
         * dead follower (sendto toward 0.0.0.0 fails).
         * 255.255.255.255 equals INADDR_NONE for inet_addr: gm_init used
         * to fail AFTER the master's CMD_INIT, a failure path of its
         * own. First byte >= 224: multicast (224-239), reserved range
         * (240-254) and broadcast. ip is in network order, so its first
         * byte is ip & 0xFF (groovy_followers_ip_parse). */
        if ((ip & 0xFFu) == 0u || (ip & 0xFFu) >= 224u) {
            groovy_followers_fail(err, err_cap, index, entry_start, entry_len,
                                  "address not usable for a follower");
            return -1;
        }

        p    = ip_end;
        port = (uint16_t)GROOVY_FOLLOWERS_PORT_DEFAULT;
        if (p < entry_len && entry_start[p] == ':') {
            size_t        port_start = p + 1u;
            size_t        port_end   = port_start;
            unsigned long v;

            while (port_end < entry_len && entry_start[port_end] != ',')
                port_end++;

            if (!groovy_followers_uint_parse(entry_start + port_start,
                                             port_end - port_start, &v)
                || v < 1ul || v > 65535ul) {
                groovy_followers_fail(err, err_cap, index, entry_start, entry_len,
                                      "invalid port (1-65535)");
                return -1;
            }
            port = (uint16_t)v;
            p = port_end;
        }

        /* Same address AND same port as the master -> refusal. Skipped
         * if the caller doesn't have a canonical master yet
         * (master_ip == 0). */
        if (master_ip != 0u && ip == master_ip && port == master_port) {
            groovy_followers_fail(err, err_cap, index, entry_start, entry_len,
                                  "same address and same port as the master");
            return -1;
        }

        for (dup_i = 0u; dup_i < accepted; dup_i++) {
            if (out[dup_i].ip == ip && out[dup_i].port == port) {
                groovy_followers_fail(err, err_cap, index, entry_start, entry_len,
                                      "duplicate follower");
                return -1;
            }
        }

        /* Defaults: mtu 1472, compression follows the master, audio ON
         * (never an opt-in), inputs P1, shape follows the master, pad
         * OFF -- an absent setting means a bit-identical wire. */
        mtu         = GM_MTU_DEFAULT;
        compression = master_compression;
        audio       = 1;
        inputs     = GROOVY_INPUTS_P1;
        shape       = master_shape;
        pad         = 0;

        while (p < entry_len) {
            size_t      key_start, key_end, val_start, val_end, klen, vlen;
            const char *key;
            const char *val;

            if (entry_start[p] != ',') {
                groovy_followers_fail(err, err_cap, index, entry_start, entry_len,
                                      "unknown key");
                return -1;
            }
            p++;   /* skip the comma */

            key_start = p;
            while (p < entry_len && entry_start[p] != '=' && entry_start[p] != ',')
                p++;
            key_end = p;

            if (p >= entry_len || entry_start[p] != '=') {
                groovy_followers_fail(err, err_cap, index, entry_start, entry_len,
                                      "unknown key");
                return -1;
            }
            p++;   /* skip the '=' */

            val_start = p;
            while (p < entry_len && entry_start[p] != ',')
                p++;
            val_end = p;

            key  = entry_start + key_start;
            val  = entry_start + val_start;
            klen = key_end - key_start;
            vlen = val_end - val_start;

            if (klen == 3u && memcmp(key, "mtu", 3u) == 0) {
                unsigned long v;
                if (seen_mtu) {
                    groovy_followers_fail(err, err_cap, index, entry_start, entry_len,
                                          "duplicate key");
                    return -1;
                }
                seen_mtu = 1;
                if (!groovy_followers_uint_parse(val, vlen, &v)
                    || v < (unsigned long)GM_MTU_MIN
                    || v > (unsigned long)GM_MTU_MAX) {
                    groovy_followers_fail(err, err_cap, index, entry_start, entry_len,
                                          "value out of range for mtu (548-3800)");
                    return -1;
                }
                mtu = (unsigned)v;
            } else if (klen == 11u && memcmp(key, "compression", 11u) == 0) {
                if (seen_compression) {
                    groovy_followers_fail(err, err_cap, index, entry_start, entry_len,
                                          "duplicate key");
                    return -1;
                }
                seen_compression = 1;
                if (vlen == 3u && memcmp(val, "off", 3u) == 0)
                    compression = 0;
                else if (vlen == 3u && memcmp(val, "lz4", 3u) == 0)
                    compression = 1;
                else {
                    groovy_followers_fail(err, err_cap, index, entry_start, entry_len,
                                          "unknown value for compression (off|lz4)");
                    return -1;
                }
            } else if (klen == 5u && memcmp(key, "audio", 5u) == 0) {
                if (seen_audio) {
                    groovy_followers_fail(err, err_cap, index, entry_start, entry_len,
                                          "duplicate key");
                    return -1;
                }
                seen_audio = 1;
                if (vlen == 2u && memcmp(val, "on", 2u) == 0)
                    audio = 1;
                else if (vlen == 3u && memcmp(val, "off", 3u) == 0)
                    audio = 0;
                else {
                    groovy_followers_fail(err, err_cap, index, entry_start, entry_len,
                                          "unknown value for audio (on|off)");
                    return -1;
                }
            } else if ((klen == 6u && memcmp(key, "inputs", 6u) == 0) || (klen == 7u && memcmp(key, "entrees", 7u) == 0)) {
                if (seen_inputs) {
                    groovy_followers_fail(err, err_cap, index, entry_start, entry_len,
                                          "duplicate key");
                    return -1;
                }
                seen_inputs = 1;
                if (vlen == 2u && memcmp(val, "p1", 2u) == 0)
                    inputs = GROOVY_INPUTS_P1;
                else if (vlen == 2u && memcmp(val, "p2", 2u) == 0)
                    inputs = GROOVY_INPUTS_P2;
                else if (vlen == 3u && memcmp(val, "off", 3u) == 0)
                    inputs = GROOVY_INPUTS_OFF;
                else {
                    groovy_followers_fail(err, err_cap, index, entry_start, entry_len,
                                          "unknown value for inputs (p1|p2|off)");
                    return -1;
                }
            } else if ((klen == 5u && memcmp(key, "shape", 5u) == 0) || (klen == 5u && memcmp(key, "forme", 5u) == 0)) {
                int v_shape;
                if (seen_shape) {
                    groovy_followers_fail(err, err_cap, index, entry_start, entry_len,
                                          "duplicate key");
                    return -1;
                }
                seen_shape = 1;
                if (vlen == 5u && memcmp(val, "image", 5u) == 0)
                    v_shape = GROOVY_SHAPE_IMAGE;
                else if ((vlen == 5u && memcmp(val, "field", 5u) == 0) || (vlen == 5u && memcmp(val, "champ", 5u) == 0))
                    v_shape = GROOVY_SHAPE_FIELD;
                else {
                    groovy_followers_fail(err, err_cap, index, entry_start, entry_len,
                                          "unknown value for shape (image|field)");
                    return -1;
                }
                if (v_shape != master_shape) {
                    /* v1: only the master's shape is accepted -- a
                     * per-follower encoding does not exist yet. */
                    groovy_followers_fail(err, err_cap, index, entry_start, entry_len,
                                          "shape different from the master's: a "
                                          "per-follower encoding does not exist "
                                          "yet");
                    return -1;
                }
                shape = v_shape;
            } else if (klen == 3u && memcmp(key, "pad", 3u) == 0) {
                /* Modeled on the audio branch above: same shape, same
                 * message. */
                if (seen_pad) {
                    groovy_followers_fail(err, err_cap, index, entry_start, entry_len,
                                          "duplicate key");
                    return -1;
                }
                seen_pad = 1;
                if (vlen == 2u && memcmp(val, "on", 2u) == 0)
                    pad = 1;
                else if (vlen == 3u && memcmp(val, "off", 3u) == 0)
                    pad = 0;
                else {
                    groovy_followers_fail(err, err_cap, index, entry_start, entry_len,
                                          "unknown value for pad (on|off)");
                    return -1;
                }
            } else {
                groovy_followers_fail(err, err_cap, index, entry_start, entry_len,
                                      "unknown key");
                return -1;
            }
        }

        snprintf(out[accepted].ip_text, sizeof(out[accepted].ip_text),
                  "%u.%u.%u.%u",
                  (unsigned)(ip & 0xFFu),
                  (unsigned)((ip >> 8) & 0xFFu),
                  (unsigned)((ip >> 16) & 0xFFu),
                  (unsigned)((ip >> 24) & 0xFFu));
        out[accepted].ip          = ip;
        out[accepted].port        = port;
        out[accepted].mtu         = mtu;
        out[accepted].compression = compression;
        out[accepted].audio       = audio;
        out[accepted].inputs     = inputs;
        out[accepted].shape       = shape;
        out[accepted].pad         = pad;
        accepted++;

        if (!has_more)
            break;
        pos = pos + entry_len + 1u;   /* skip the ';' */
    }

    if (n_out)
        *n_out = accepted;
    return 0;
}

/* ---------------------------------------------------------------------------
 * groovy_status_from_declared -- a status only belongs to this receiver
 * if IP AND port both equal the address declared for it. Returns 1 if
 * both match, 0 otherwise.
 * ------------------------------------------------------------------------- */
static inline int groovy_status_from_declared(uint32_t peer_ip, uint16_t peer_port,
                                               uint32_t declared_ip, uint16_t declared_port)
{
    return (peer_ip == declared_ip && peer_port == declared_port) ? 1 : 0;
}

/* ---------------------------------------------------------------------------
 * groovy_sat_note_sent / groovy_sat_in_flight -- the gap that the
 * saturation policy compares against its thresholds.
 *
 * groovy_sat_note_sent files the frame_id of a frame ACTUALLY sent to
 * THIS follower into its state's ring. groovy_fan_out calls it at the
 * same spot as rx->last_sent_id, after every attempted send; a skip
 * writes nothing there.
 *
 * groovy_sat_in_flight counts the ring's IDs more recent than
 * last_echo: the frames sent to THIS follower and not yet acked by it.
 * That is, to the letter, the written definition of the signal. A slot
 * never written is worth 0, and 0 is never more recent than an ack: it
 * doesn't count. The count caps at GROOVY_SAT_RING, above the entry
 * threshold.
 * ------------------------------------------------------------------------- */
static inline void groovy_sat_note_sent(struct groovy_sat_state *s, uint32_t frame_id)
{
    if (!s)
        return;
    s->sent_head %= GROOVY_SAT_RING;
    s->sent_ring[s->sent_head] = frame_id;
    s->sent_head = (s->sent_head + 1u) % GROOVY_SAT_RING;
}

static inline uint32_t groovy_sat_in_flight(const struct groovy_sat_state *s, uint32_t last_echo)
{
    uint32_t c = 0u;
    unsigned k;

    if (!s)
        return 0u;
    for (k = 0u; k < GROOVY_SAT_RING; k++)
        if (s->sent_ring[k] > last_echo)
            c++;
    return c;
}

/* ---------------------------------------------------------------------------
 * groovy_sat_should_skip -- saturation policy for a follower.
 *
 * Signal: gap = frames SENT to THIS follower and not yet REASSEMBLED by
 * it, counted by groovy_sat_in_flight on the ring that the caller keeps
 * via groovy_sat_note_sent -- never the global frame_id (which would
 * freeze the skip forever as soon as another receiver moves on).
 *
 * Not hooked (synced == 0) -> reset the state, return 0 (never a skip
 * for a follower that has never answered -- nothing to compare). The
 * ring is NOT cleared: it describes what actually went out, hooked or
 * not. An ack ahead of any send (previous session) leaves no frame in
 * flight: gap of zero.
 *
 * Outside a skip episode: enter skip when gap >= GROOVY_SAT_INPUT.
 * Inside a skip episode: exit when the ack of the episode's FIRST probe
 * has come back (last_echo >= probe_id), or when gap <= GROOVY_SAT_OUTPUT;
 * otherwise, after GROOVY_SAT_PROBE CONSECUTIVE skips, a frame goes out
 * anyway (a probe -- never leave a follower that came back within the
 * window completely silent), then the skip counter restarts from zero
 * and the series resumes. frame_id is the ID of the frame the caller is
 * about to send: it's the one the first probe freezes into probe_id.
 *
 * THE FIX. Before, the only exit condition was the gap, measured
 * against last_sent, which EVERY probe moved. With a probe leaving
 * every five frames, a follower whose round trip exceeded five frames
 * saw the reference move before the previous ack came back: the gap
 * never fell back to 2, and the follower stayed at one frame in five
 * (12 fps) for the whole session, on a link that carried 60. The exit
 * reference is now FROZEN on the episode's first probe, until its ack.
 * Later probes still leave, at the same cadence, but no longer move it:
 * a LOST probe therefore doesn't freeze the follower into silence,
 * since the ack of any more recent probe also exceeds probe_id. (An
 * earlier proposal was to stop probing while a probe is in flight: a
 * single lost probe would then have left the follower silent forever.)
 *
 * THE WRITTEN DEFINITION, FINALLY APPLIED. The gap used to be computed
 * as last_sent - last_echo, a difference of GLOBAL IDs: after a skip
 * episode, it also counted the SKIPPED frames, which this follower
 * never received and which nobody will ever ack. That's what dropped a
 * follower with a 6-8 frame round trip straight back into skip as soon
 * as it exited: 62.5% of frames at 6, 23.1% at 8, in the measured
 * model. The gap is now groovy_sat_in_flight, only the frames actually
 * sent toward THIS follower and not yet acked: at a 6-8 frame round
 * trip, a passing spike no longer leaves anything behind, the follower
 * exits skip and then receives every frame.
 *
 * OPEN QUESTION. In steady state, at r frames of round trip, r - 1
 * frames are in flight at decision time. From 9 frames of round trip
 * (roughly 150 ms) onward, the gap alone reaches the entry threshold:
 * the signal conflates latency and congestion. This isn't a matter of
 * threshold, but of the signal's own definition, which remains open.
 *
 * Thresholds not calibrated on the bench, to be confirmed by the
 * saturations= counter of [groovy-lat].
 * ------------------------------------------------------------------------- */
static inline int groovy_sat_should_skip(struct groovy_sat_state *s, int synced,
                                          uint32_t last_echo, uint32_t frame_id)
{
    uint32_t gap;

    if (!s)
        return 0;

    if (!synced) {
        s->skipping  = 0;
        s->run       = 0u;
        s->probe_id  = 0u;
        s->probe_out = 0;
        return 0;
    }

    gap = groovy_sat_in_flight(s, last_echo);   /* sent AND not yet acked */

    if (!s->skipping) {
        if (gap >= GROOVY_SAT_INPUT) {
            s->skipping  = 1;
            s->run       = 1u;
            s->probe_id  = 0u;
            s->probe_out = 0;
            return 1;
        }
        return 0;
    }

    /* s->skipping == 1: already in a skip episode. Exit: the first
     * probe's ack has come back (frozen reference), or the gap has
     * dropped back down. */
    if ((s->probe_out && last_echo >= s->probe_id) || gap <= GROOVY_SAT_OUTPUT) {
        s->skipping  = 0;
        s->run       = 0u;
        s->probe_id  = 0u;
        s->probe_out = 0;
        return 0;
    }

    if (s->run >= GROOVY_SAT_PROBE) {
        s->run = 0u;   /* probe: one frame goes out, then skips resume */
        if (!s->probe_out) {
            s->probe_out = 1;          /* the FIRST probe freezes the reference */
            s->probe_id  = frame_id;
        }
        return 0;
    }

    s->run++;
    return 1;
}

/* ---------------------------------------------------------------------------
 * groovy_axis_merge -- merges an analog axis: the sample with the
 * larger absolute value wins. On a tie, the value ALREADY accumulated
 * wins (so the master's, always merged first). No addition: adding two
 * opposite movements would saturate or cancel out in a way the player
 * couldn't make sense of.
 * ------------------------------------------------------------------------- */
static inline int8_t groovy_axis_merge(int8_t cur, int8_t add)
{
    int a = (add < 0) ? -(int)add : (int)add;
    int c = (cur < 0) ? -(int)cur : (int)cur;
    return (a > c) ? add : cur;
}

/* ---------------------------------------------------------------------------
 * groovy_input_fold_joy -- merges a controller state according to the
 * attribution:
 *   IDENTITY (the master): both its controllers copied as-is into
 *     joy1/joy2 of the accumulator (buttons OR'd, axes merged);
 *   P1: the follower's controller 1 (src's joy1) merged into the
 *     accumulator's joy1 -- buttons OR'd, joy1_* axes merged;
 *   P2: the follower's controller 1 merged into the accumulator's joy2
 *     -- same source axes (src's joy1_*), but written into the
 *     accumulator's joy2_*;
 *   OFF: nothing.
 * A follower's controller 2 (src's joy2) is NEVER read for P1/P2: one
 * follower = one player, within the MAX_USERS_MISTER bound of 2.
 * ------------------------------------------------------------------------- */
static inline void groovy_input_fold_joy(gm_joy_inputs *acc, const gm_joy_inputs *src,
                                          int inputs)
{
    if (!acc || !src)
        return;

    switch (inputs) {
    case GROOVY_INPUTS_IDENTITY:
        acc->joy1 = (uint16_t)(acc->joy1 | src->joy1);
        acc->joy2 = (uint16_t)(acc->joy2 | src->joy2);
        acc->joy1_lx = groovy_axis_merge(acc->joy1_lx, src->joy1_lx);
        acc->joy1_ly = groovy_axis_merge(acc->joy1_ly, src->joy1_ly);
        acc->joy1_rx = groovy_axis_merge(acc->joy1_rx, src->joy1_rx);
        acc->joy1_ry = groovy_axis_merge(acc->joy1_ry, src->joy1_ry);
        acc->joy2_lx = groovy_axis_merge(acc->joy2_lx, src->joy2_lx);
        acc->joy2_ly = groovy_axis_merge(acc->joy2_ly, src->joy2_ly);
        acc->joy2_rx = groovy_axis_merge(acc->joy2_rx, src->joy2_rx);
        acc->joy2_ry = groovy_axis_merge(acc->joy2_ry, src->joy2_ry);
        break;
    case GROOVY_INPUTS_P1:
        acc->joy1 = (uint16_t)(acc->joy1 | src->joy1);
        acc->joy1_lx = groovy_axis_merge(acc->joy1_lx, src->joy1_lx);
        acc->joy1_ly = groovy_axis_merge(acc->joy1_ly, src->joy1_ly);
        acc->joy1_rx = groovy_axis_merge(acc->joy1_rx, src->joy1_rx);
        acc->joy1_ry = groovy_axis_merge(acc->joy1_ry, src->joy1_ry);
        break;
    case GROOVY_INPUTS_P2:
        acc->joy2 = (uint16_t)(acc->joy2 | src->joy1);
        acc->joy2_lx = groovy_axis_merge(acc->joy2_lx, src->joy1_lx);
        acc->joy2_ly = groovy_axis_merge(acc->joy2_ly, src->joy1_ly);
        acc->joy2_rx = groovy_axis_merge(acc->joy2_rx, src->joy1_rx);
        acc->joy2_ry = groovy_axis_merge(acc->joy2_ry, src->joy1_ry);
        break;
    case GROOVY_INPUTS_OFF:
    default:
        break;
    }
}

/* ---------------------------------------------------------------------------
 * groovy_input_fold_ps2 -- merges keyboard + mouse: MASTER ONLY.
 * IDENTITY (the master): keys OR'd byte by byte, mouse_bits OR'd, mouse
 * deltas copied. P1, P2 or OFF: nothing -- a follower's keyboard and
 * mouse never drive the game. Before this rule, a P1/P2 follower's keys
 * used to be OR'd into the single keyboard, where the P1/P2 attribution
 * makes no sense. groovy_input_take_ps2 still always reads a follower's
 * state, then discards it.
 *
 * Returns 1 if src contributed (IDENTITY), 0 otherwise.
 * groovy_input_take_ps2 uses this to decide whether the state counts,
 * frame and order included, and no longer has a guard of its own: the
 * "master only" rule lives only here, where the test bridge exercises
 * it.
 * ------------------------------------------------------------------------- */
static inline int groovy_input_fold_ps2(gm_ps2_inputs *acc, const gm_ps2_inputs *src,
                                         int inputs)
{
    unsigned i;

    if (!acc || !src)
        return 0;
    if (inputs != GROOVY_INPUTS_IDENTITY)
        return 0;   /* keyboard and mouse: the master alone */

    for (i = 0u; i < 32u; i++)
        acc->keys[i] = (uint8_t)(acc->keys[i] | src->keys[i]);

    acc->mouse_bits = (uint8_t)(acc->mouse_bits | src->mouse_bits);
    acc->mouse_x    = src->mouse_x;
    acc->mouse_y    = src->mouse_y;
    acc->mouse_z    = src->mouse_z;
    return 1;
}

/* ---------------------------------------------------------------------------
 * groovy_input_fold_mouse -- sums mouse deltas: MASTER ONLY (same rule
 * as fold_ps2). IDENTITY: the deltas add to the sums, returns 1. P1, P2
 * or OFF: a follower's deltas are discarded, returns 0 --
 * groovy_input_take_mouse has already consumed them in libgm, so they
 * don't accumulate there. Before this function existed, the rule only
 * lived inline in take_mouse, which doesn't call groovy_input_fold_ps2:
 * no test exercised it.
 * ------------------------------------------------------------------------- */
static inline int groovy_input_fold_mouse(int *sx, int *sy, int *sz,
                                          int dx, int dy, int dz, int inputs)
{
    if (!sx || !sy || !sz)
        return 0;
    if (inputs != GROOVY_INPUTS_IDENTITY)
        return 0;   /* a follower's deltas are discarded */
    *sx += dx;
    *sy += dy;
    *sz += dz;
    return 1;
}

/* ---------------------------------------------------------------------------
 * groovy_input_is_live -- does the inputs contribution at index `idx`
 * of st->rx[] still count in the merge?
 *
 * libgm returns the LAST state received from a peer as long as it has
 * received one, i.e. forever (gm_get_joy_inputs, has_joy): neither
 * ack_lost, nor a re-announce (gm_input_reset_peer_guard keeps has_joy
 * on purpose), nor silence clears it. As long as the peer talks, that's
 * harmless -- a client sends its state every frame. But a follower
 * that disappears while holding a button (crashed client, Wi-Fi cut,
 * client closed) used to leave it held down forever for the player at
 * the tube, through the OR merge: exactly what this rule forbids.
 *
 * Rule:
 *   - idx == 0, the master: always alive, never aged;
 *   - a follower: alive if its last ACCEPTED datagram is at most
 *     GROOVY_INPUT_STALE_NS (500 ms) old; beyond that, its contribution
 *     drops;
 *   - last_accept_ns == 0: no datagram accepted this session, nothing
 *     to merge.
 * A clock that runs backward (now_ns < last_accept_ns) ages nobody:
 * treated as alive.
 * ------------------------------------------------------------------------- */
static inline int groovy_input_is_live(unsigned idx, uint64_t now_ns, uint64_t last_accept_ns)
{
    if (idx == 0u)
        return 1;
    if (last_accept_ns == 0u)
        return 0;
    if (now_ns < last_accept_ns)
        return 1;
    return ((now_ns - last_accept_ns) <= GROOVY_INPUT_STALE_NS) ? 1 : 0;
}

/* ---------------------------------------------------------------------------
 * groovy_input_take_joy_rc -- what groovy_input_take_joy returns to the
 * `mister` controller driver after the merge.
 *
 * The driver (mister_joypad_poll) reads -1 as "nothing available" and
 * then keeps its cache, i.e. the LAST merged state. Rule:
 *   - any_hooked == 0: no inputs channel hooked -- no session, or
 *     GROOVY_INPUT off. -1: the cache stays as-is, as before;
 *   - have_any != 0: at least one live source provided a state. 0: the
 *     driver copies the merge;
 *   - otherwise, channels are hooked but no source is alive. Also 0,
 *     and the caller publishes an EMPTY merge: everything is released.
 *
 * The third case is the fixed bug. The aging rule did skip a silent
 * follower's controller, but if no other live source remained, the
 * function used to return -1, and the driver kept the vanished
 * follower's button held down forever. That's the master-less
 * fallback: the master's channel is hooked with nothing talking on it,
 * and the only player is a follower.
 * ------------------------------------------------------------------------- */
static inline int groovy_input_take_joy_rc(int any_hooked, int have_any)
{
    if (have_any)
        return 0;
    return any_hooked ? 0 : -1;
}

#endif /* GROOVY_FOLLOWERS_H */
