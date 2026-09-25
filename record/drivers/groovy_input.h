/* SPDX-License-Identifier: GPL-3.0-or-later */
/* groovy_input.h -- setting and latency histogram for the Groovy inputs
 * channel, no dependency on RetroArch. Header-only, same pattern as
 * groovy_audio.h: record_groovy.c includes it to run, and the test bridge
 * libgm/test/groovy_test_bridge.c includes it to exercise it outside
 * RetroArch.
 *
 * Three things live here, and nothing else:
 *
 *   1. the GROOVY_INPUT setting, which GUARDS the OPENING of the inputs
 *      channel;
 *   2. the latency histogram for input k, self-contained and testable;
 *   3. its periodic trace formatting.
 */

#ifndef GROOVY_INPUT_H
#define GROOVY_INPUT_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * GROOVY_INPUT -- the RUNTIME guard for the inputs channel.
 *
 * The nuance that sets GROOVY_INPUT apart from GROOVY_AUDIO: audio and
 * compression are settings internal to a driver that's ALREADY active.
 * Inputs, on the other hand, also depend on a driver SELECTED AT STARTUP
 * by a static configuration key (input_joypad_driver = "mister").
 * GROOVY_INPUT therefore isn't what registers the driver -- it's the
 * RUNTIME guard that decides whether gm_bind_inputs() is actually called.
 * The driver stays registered and inert when the variable is absent. It
 * mirrors upstream's video_mister_enable (mister_joypad.c), replaced by a
 * getenv consistent with the rest of the project.
 *
 * OFF is the default, same discipline as GROOVY_AUDIO: change nothing on
 * first launch. Only the exact string "on" turns it on -- strict
 * comparison, same as audio.
 * ------------------------------------------------------------------------- */
enum groovy_input_mode {
    GROOVY_INPUT_OFF = 0,
    GROOVY_INPUT_ON  = 1
};

/* Parses the GROOVY_INPUT value. NULL, empty or unknown -> OFF. */
static inline enum groovy_input_mode groovy_input_mode_parse(const char *s)
{
    if (!s || s[0] == '\0')
        return GROOVY_INPUT_OFF;
    if (!strcmp(s, "on"))
        return GROOVY_INPUT_ON;
    return GROOVY_INPUT_OFF;
}

static inline const char *groovy_input_mode_name(enum groovy_input_mode m)
{
    switch (m) {
        case GROOVY_INPUT_ON:  return "on";
        case GROOVY_INPUT_OFF:
        default:               return "off";
    }
}

/* ---------------------------------------------------------------------------
 * Latency histogram for input k.
 *
 * Explicit structure rather than file-scope variables, to stay callable
 * from the test bridge. Same percentile logic as record_groovy.c's
 * lat_hist_percentiles(): walk the buckets, accumulate, no sorting, no
 * allocation.
 * ------------------------------------------------------------------------- */
#define GROOVY_INPUT_LAT_BUCKETS 64u

struct groovy_input_lat {
    uint16_t hist[GROOVY_INPUT_LAT_BUCKETS];
    unsigned n;
    uint64_t win_start_ns;
};

/* Clears the count and the buckets. Does NOT touch win_start_ns: reset()
 * empties the window's content, not its clock -- should_report re-arms
 * it, once every elapsed second. */
static inline void groovy_input_lat_reset(struct groovy_input_lat *l)
{
    if (!l) return;
    memset(l->hist, 0, sizeof(l->hist));
    l->n = 0u;
}

/* Files k into its bucket. Any value >= GROOVY_INPUT_LAT_BUCKETS - 1 is
 * filed into the last bucket -- never an out-of-bounds write, whatever
 * the value of k. An aberrant k (receiver restart, a counter that wraps
 * back to zero) must produce a saturated value visible in the trace, not
 * a buffer overrun. */
static inline void groovy_input_lat_record(struct groovy_input_lat *l, unsigned k)
{
    unsigned b;
    if (!l) return;
    b = (k >= GROOVY_INPUT_LAT_BUCKETS) ? (GROOVY_INPUT_LAT_BUCKETS - 1u) : k;
    l->hist[b]++;
    l->n++;
}

/* Reads the min/p50/p95/max percentiles. An empty histogram returns
 * percentiles of zero without reading uninitialized memory -- walking
 * the 64 buckets only touches hist[], never a value derived from n=0. */
static inline void groovy_input_lat_percentiles(const struct groovy_input_lat *l,
                                                 unsigned *min, unsigned *p50,
                                                 unsigned *p95, unsigned *max)
{
    unsigned cum, p50_thresh, p95_thresh, b;
    int have_min, have_p50, have_p95;

    *min = 0u;
    *p50 = 0u;
    *p95 = 0u;
    *max = 0u;
    if (!l || l->n == 0u)
        return;

    cum        = 0u;
    p50_thresh = (l->n + 1u) / 2u;
    p95_thresh = (l->n * 95u + 99u) / 100u;
    have_min = 0;
    have_p50 = 0;
    have_p95 = 0;
    if (p95_thresh == 0u)  p95_thresh = 1u;
    if (p95_thresh > l->n) p95_thresh = l->n;

    for (b = 0u; b < GROOVY_INPUT_LAT_BUCKETS; ++b) {
        if (l->hist[b] == 0u) continue;
        if (!have_min) { *min = b; have_min = 1; }
        cum += l->hist[b];
        if (!have_p50 && cum >= p50_thresh) { *p50 = b; have_p50 = 1; }
        if (!have_p95 && cum >= p95_thresh) { *p95 = b; have_p95 = 1; }
        *max = b;   /* last non-empty bucket visited wins */
    }
}

/* Returns 1 if a full second has elapsed AND n > 0, re-arming the window
 * in every case once a second has passed -- otherwise an idle bridge
 * would accumulate an ever-wider window and never close it. Silent
 * (returns 0) as long as nothing has happened -- same discipline as
 * depth_max on the receiver side and as audio_out_report: an idle bridge
 * says nothing. */
static inline int groovy_input_lat_should_report(struct groovy_input_lat *l,
                                                  uint64_t now_ns)
{
    int should;
    if (!l) return 0;
    if (l->win_start_ns == 0u) {
        l->win_start_ns = now_ns;
        return 0;
    }
    if (now_ns - l->win_start_ns < 1000000000ull)
        return 0;

    should = (l->n > 0u) ? 1 : 0;
    l->win_start_ns = now_ns;
    return should;
}

#endif /* GROOVY_INPUT_H */
