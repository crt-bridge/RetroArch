/* SPDX-License-Identifier: GPL-3.0-or-later */
/* groovy_audio.h — emitter-side audio logic for crt-bridge, with no
 * dependency on RetroArch. Header-only, same model as groovy_modeline.h
 * and groovy_pacing.h: record_groovy.c includes it to run, and the test
 * bridge libgm/test/groovy_test_bridge.c includes it to exercise it
 * outside RetroArch.
 *
 * Three things live here, and nothing else:
 *
 *   1. the audio output setting, read from the GROOVY_AUDIO environment
 *      variable;
 *   2. the CMD_INIT frequency code, a four-value enumeration;
 *   3. a linear resampler, armed only when the core's native frequency
 *      is not one of those four values.
 */

#ifndef GROOVY_AUDIO_H
#define GROOVY_AUDIO_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Where the audio must go out. Read once when the session opens.
 *
 * The setting lives in the GROOVY_AUDIO environment variable, not in
 * retroarch.cfg: RetroArch rewrites that file on exit from its own list
 * of settings, so a hand-added key there disappears. The fork already
 * uses this pattern for the frame witness path (GROOVY_DUMP_FRAME_PATH).
 *
 * OFF is the default, so a first launch changes nothing.
 *
 * RECEIVER and BOTH do exactly the same thing on the wire -- the emitter
 * sends. What separates them is the PC's silence, which is achieved with
 * no code at all: `python scripts/ra-command.py MUTE`, or
 * audio_mute_enable in the configuration. Both values are kept because
 * they are the words of the original request, and the log trace says
 * which one is active.
 * ------------------------------------------------------------------------- */
enum groovy_audio_mode {
    GROOVY_AUDIO_OFF      = 0,
    GROOVY_AUDIO_RECEIVER = 1,
    GROOVY_AUDIO_BOTH     = 2
};

/* Parses the value of GROOVY_AUDIO. NULL, empty or unknown -> OFF. */
static inline enum groovy_audio_mode groovy_audio_mode_parse(const char *s)
{
    if (!s || s[0] == '\0')
        return GROOVY_AUDIO_OFF;
    if (!strcmp(s, "receiver"))
        return GROOVY_AUDIO_RECEIVER;
    if (!strcmp(s, "both"))
        return GROOVY_AUDIO_BOTH;
    return GROOVY_AUDIO_OFF;
}

static inline const char *groovy_audio_mode_name(enum groovy_audio_mode m)
{
    switch (m) {
        case GROOVY_AUDIO_RECEIVER: return "receiver";
        case GROOVY_AUDIO_BOTH:     return "both";
        case GROOVY_AUDIO_OFF:
        default:                    return "off";
    }
}

/* ---------------------------------------------------------------------------
 * CMD_INIT frequency code — byte 2 of the 4-byte packet.
 *
 *   0 = muted, 1 = 22050 Hz, 2 = 44100 Hz, 3 = 48000 Hz
 *
 * The enumeration only knows these three frequencies. A core that outputs
 * anything else must be resampled, and the code choice is NOT the closest
 * one in absolute terms: it is the smallest code whose frequency is
 * greater than or equal to the native one, and 3 if the native rate
 * exceeds 48000.
 *
 * Why: stepping 32000 down to 22050 would fold the spectrum, and there is
 * no anti-aliasing filter here -- aliasing is clearly audible. Stepping
 * 32000 up to 44100 by linear interpolation adds nothing audible. The
 * only case where we step down is above 48000, where there is no other
 * choice; no known core reaches that far.
 *
 * The PlayStation outputs at 44100: code 2, no processing.
 * ------------------------------------------------------------------------- */
static inline unsigned groovy_audio_rate_hz(uint8_t code)
{
    switch (code) {
        case 1u: return 22050u;
        case 2u: return 44100u;
        case 3u: return 48000u;
        default: return 0u;
    }
}

static inline uint8_t groovy_audio_rate_code(double native_hz)
{
    if (!(native_hz > 0.0))
        return 0u;
    if (native_hz <= 22050.0) return 1u;
    if (native_hz <= 44100.0) return 2u;
    return 3u;
}

/* ---------------------------------------------------------------------------
 * Linear resampler.
 *
 * State carried from one call to the next, because push_audio arrives in
 * blocks and a fractional read position almost always straddles two
 * blocks. `phase` is expressed in source samples, relative to the start
 * of the current block, and sits between -1 and 0 in steady state: the
 * negative value designates `prev`, the last pair of the previous block.
 *
 * Linear interpolation only. At ratios close to 1 -- the only case that
 * matters here, between 32000 and 48000 -- it is transparent. This is
 * not a studio-quality resampler and that is not the point: the
 * project's normal path (44100) never goes through here.
 *
 * A single thread calls these functions, the runloop thread, as for the
 * whole record_groovy chain.
 * ------------------------------------------------------------------------- */

/* --- Thread-side fractional ratio. -----------------------------------------
 * The ratio can NOT be armed by src_hz != dst_hz: on this project, a PS1
 * core outputs at 44100 Hz and the CMD_INIT frequency code also yields
 * 44100 Hz, so that comparison is ALWAYS false and record_groovy.c's
 * resampling block never runs. The ratio is therefore a SEPARATE field,
 * neutral at 1.0.
 * Bounds: the actual need is ~1762 ppm of RetroArch skew plus 2428 ppm of
 * total term (mode feed-forward + servo at 500 ppm), i.e. under 4200 ppm.
 * A 10000 ppm bound leaves margin and forbids any absurd interpolation
 * step.
 * Proven worst case in this repository (saturated 480i + Chrono Cross,
 * authority 500): native/input = 44100/44177.71 = 0.998242; speed term
 * -2422.4 to -1422.4 ppm; SRC-side factor 1.0024283 to 1.0014244;
 * thread-side ratio 0.999664 to 1.000666 -- more than 9300 ppm from
 * either bound, a margin of 14x. DO NOT tighten this without redoing
 * this calculation. */
#define GROOVY_RESAMPLE_RATIO_MIN 0.99
#define GROOVY_RESAMPLE_RATIO_MAX 1.01

struct groovy_resampler {
    unsigned src_hz;
    unsigned dst_hz;
    double   phase;        /* read position, in source samples */
    int16_t  prev[2];      /* last pair of the previous block */
    int      prev_valid;
    double   ratio;   /* output pairs per input pair, on top of dst/src; 1.0 = neutral */
};

static inline void groovy_resample_reset(struct groovy_resampler *r,
                                         unsigned src_hz, unsigned dst_hz)
{
    if (!r) return;
    r->src_hz     = src_hz;
    r->dst_hz     = dst_hz;
    r->phase      = 0.0;
    r->prev[0]    = 0;
    r->prev[1]    = 0;
    r->prev_valid = 0;
    r->ratio      = 1.0;
}

/* Fractional ratio of the resampler, adjustable mid-session. Bounds
 * [GROOVY_RESAMPLE_RATIO_MIN, GROOVY_RESAMPLE_RATIO_MAX]; NULL,
 * out-of-bounds or non-finite (NaN, +/-inf caught by the negated write)
 * -> refused, the previous ratio is kept, returns 0. Never touches
 * `phase` nor `prev`: changing the ratio mid-session must not produce a
 * click. */
static inline int groovy_resample_set_ratio(struct groovy_resampler *r, double ratio)
{
    if (!r)
        return 0;
    if (!(ratio > GROOVY_RESAMPLE_RATIO_MIN) || !(ratio < GROOVY_RESAMPLE_RATIO_MAX))
        return 0;
    r->ratio = ratio;
    return 1;
}

/* Upper bound on the number of pairs produced for `in_frames` input
 * pairs. The +2 covers the carried-over phase and rounding. */
static inline size_t groovy_resample_capacity(size_t in_frames,
                                              unsigned src_hz, unsigned dst_hz)
{
    if (src_hz == 0u || dst_hz == 0u)
        return in_frames;
    return (size_t)((double)in_frames * (double)dst_hz / (double)src_hz) + 2u;
}

/* Upper bound on the number of pairs produced, fractional ratio included.
 * `groovy_resample_capacity` stays unchanged: other callers read it. An
 * out-of-bounds ratio is treated as 1.0 -- the same fallback as
 * `groovy_resample` via r_eff. */
static inline size_t groovy_resample_capacity_ratio(size_t in_frames,
                                                     unsigned src_hz, unsigned dst_hz,
                                                     double ratio)
{
    double r_eff;
    if (src_hz == 0u || dst_hz == 0u)
        return in_frames + 2u;
    r_eff = (ratio > GROOVY_RESAMPLE_RATIO_MIN && ratio < GROOVY_RESAMPLE_RATIO_MAX)
            ? ratio : 1.0;
    return (size_t)((double)in_frames * (double)dst_hz / (double)src_hz * r_eff) + 2u;
}

/* Integer floor of a possibly-negative double -- (long) truncates
 * toward zero, which is wrong for negatives, and the carried-over phase
 * is negative. */
static inline long groovy_resample_floor(double v)
{
    long i = (long)v;
    return ((double)i > v) ? (i - 1) : i;
}

/* Produces the resampled stereo pairs from `in` into `out`. Returns the
 * number of pairs written. `out_cap` is in pairs. A ratio of 1 (src ==
 * dst) copies through without touching the phase. */
static inline size_t groovy_resample(struct groovy_resampler *r,
                                     const int16_t *in, size_t in_frames,
                                     int16_t *out, size_t out_cap)
{
    if (!r || !in || !out || in_frames == 0u || out_cap == 0u)
        return 0u;
    if (r->src_hz == 0u || r->dst_hz == 0u)
        return 0u;

    if (r->src_hz == r->dst_hz && r->ratio == 1.0) {
        size_t n = (in_frames < out_cap) ? in_frames : out_cap;
        memcpy(out, in, n * 2u * sizeof(int16_t));
        return n;
    }

    /* The carried-over phase sits between -1 and 0 in steady state, and
     * index -1 designates `prev`. Nothing in the loop can push it lower,
     * but an explicit bound costs one comparison and permanently forbids
     * reading before the start of the block. */
    if (r->phase < -1.0)
        r->phase = -1.0;

    /* r_eff: the fractional ratio, folded back to 1.0 if not positive
     * (numeric guard -- step can never become zero, negative or
     * infinite). */
    const double r_eff = (r->ratio > 0.0) ? r->ratio : 1.0;
    const double step  = ((double)r->src_hz / (double)r->dst_hz) / r_eff;
    const double limit = (double)in_frames - 1.0;
    size_t       n     = 0u;

    /* Only emit outputs whose right-hand neighbor is WITHIN this block;
     * the rest is carried over to the next call via the phase. */
    while (r->phase < limit && n < out_cap) {
        long   i    = groovy_resample_floor(r->phase);
        double frac = r->phase - (double)i;

        const int16_t *a;
        if (i < 0)
            a = r->prev_valid ? r->prev : in;   /* very first block: no history */
        else
            a = in + (size_t)i * 2u;
        const int16_t *b = in + (size_t)(i + 1) * 2u;

        for (unsigned c = 0u; c < 2u; ++c) {
            double v = (double)a[c] + ((double)b[c] - (double)a[c]) * frac;
            if (v >  32767.0) v =  32767.0;
            if (v < -32768.0) v = -32768.0;
            out[n * 2u + c] = (int16_t)v;
        }
        ++n;
        r->phase += step;
    }

    /* Carry-over: the phase becomes relative to the start of the NEXT
     * block, and `prev` keeps the last pair of this one. */
    r->prev[0]    = in[(in_frames - 1u) * 2u];
    r->prev[1]    = in[(in_frames - 1u) * 2u + 1u];
    r->prev_valid = 1;
    r->phase     -= (double)in_frames;

    return n;
}

#endif /* GROOVY_AUDIO_H */
