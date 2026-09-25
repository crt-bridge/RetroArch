/* SPDX-License-Identifier: GPL-3.0-or-later */
/* groovy_audio_hold.h -- an audio-hold lever, with no dependency on
 * RetroArch. Header-only, same model as groovy_frame_dup.h: record_groovy.c
 * will include it to run once wired in, and the test bridge
 * libgm/test/groovy_test_bridge.c includes it to exercise it outside
 * RetroArch.
 *
 * WHAT THE LEVER DOES: frame N's audio no longer leaves the moment
 * RetroArch hands it over; it is held and leaves glued to frame N+1's
 * video burst. Named cost: one frame of audio latency.
 *
 * WHAT DOES NOT CHANGE: opcode 0x04 (CMD_AUDIO) stays intact, no byte is
 * added to the wire, the announced size does not move, the BLOCK
 * STRUCTURE is preserved -- one block deposited comes back out as one
 * block, never merged. Only the send TIMING changes. Merging two held
 * blocks into a single CMD_AUDIO would ALSO change the burst length and
 * the announced size, i.e. exactly the variable a prior investigation
 * had already ruled out as a confound ("not a single-variable test"). A
 * lever that changed two things at once would be just as unreadable.
 *
 * OFF BY DEFAULT ON BOTH SIDES, AND THAT IS DELIBERATE -- unlike
 * GROOVY_AUDIO, GROOVY_FRAME_DUP and GROOVY_INPUT, which have two
 * diverging defaults (bench script on, fork off). Here the bench script
 * also leaves it off: this lever measures, it does not fix anything. The
 * actual fix is deliberately withheld pending a human decision, not
 * resolved here. A reviewer who "corrected" this default believing it
 * was an oversight would break that decision.
 *
 * READ TOGETHER WITH: with GROOVY_FRAME_DUP=on, a skipped frame only
 * emits 9 bytes (frame_dup), and the audio would then glue itself to an
 * empty burst. This lever is meant to be exercised with -FrameDup off
 * toward the follower -- a hard requirement, not a detail.
 *
 * The setting lives in the GROOVY_AUDIO_HOLD environment variable, not
 * in retroarch.cfg: RetroArch rewrites that file on exit from its own
 * list of settings, so a hand-added key there would disappear. The fork
 * already uses this pattern for GROOVY_COMPRESSION, GROOVY_MTU,
 * GROOVY_AUDIO, GROOVY_INPUT and GROOVY_FRAME_DUP.
 *
 * No dependency on libgm's public header: this module knows neither
 * opcode nor socket, it holds bytes and returns them intact -- the
 * boundary with the wire (gm_send_audio) is entirely the caller's
 * responsibility.
 */

#ifndef GROOVY_AUDIO_HOLD_H
#define GROOVY_AUDIO_HOLD_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * (a) The switch -- exact tracing of groovy_frame_dup_mode_parse.
 * NULL, empty or any value other than the exact string "on" -> OFF.
 * Exact, case-sensitive comparison, same discipline as
 * groovy_frame_dup_mode_parse / groovy_compression_mode_parse.
 * ------------------------------------------------------------------------- */
enum groovy_audio_hold_mode {
    GROOVY_AUDIO_HOLD_OFF = 0,   /* audio leaves the moment RetroArch hands it over -- the pre-lever behavior */
    GROOVY_AUDIO_HOLD_ON  = 1    /* frame N's audio leaves glued to frame N+1's video burst */
};

static inline enum groovy_audio_hold_mode groovy_audio_hold_mode_parse(const char *s)
{
    if (!s || s[0] == '\0')
        return GROOVY_AUDIO_HOLD_OFF;
    if (!strcmp(s, "on"))
        return GROOVY_AUDIO_HOLD_ON;
    return GROOVY_AUDIO_HOLD_OFF;
}

static inline const char *groovy_audio_hold_mode_name(enum groovy_audio_hold_mode m)
{
    switch (m) {
        case GROOVY_AUDIO_HOLD_ON: return "on";
        case GROOVY_AUDIO_HOLD_OFF:
        default:                   return "off";
    }
}

/* ---------------------------------------------------------------------------
 * (b) Chunking constant, factored out of the body of groovy_push_audio
 * so there is only one source of truth for it.
 *
 * Pairs per CMD_AUDIO at most. gm_send_audio refuses beyond 65535 BYTES;
 * 8192 stereo pairs make 32768 bytes, half of that bound. A belt, not an
 * expected path: a RetroArch block is at most 1024 pairs.
 * ------------------------------------------------------------------------- */
#define GROOVY_AUDIO_MAX_FRAMES ((size_t)8192u)

/* ---------------------------------------------------------------------------
 * (c) The hold buffer -- a flat queue of blocks.
 *
 * One deposit = one block. A block deposited comes back out as one
 * block, same size, same bytes, same order -- this module transforms
 * nothing, it holds.
 *
 * cap_samples is not part of the lever's original interface: it is an
 * internal safety guard against a buffer overrun if two successive
 * bursts (separated by a _clear) use a different channel count --
 * cap_frames alone, measured "in pairs" in the sense of the PREVIOUS
 * group, is not enough to bound a byte allocation once the channel
 * count changes. cap_samples is the truth in actually-allocated int16
 * samples; cap_frames remains the documented field, reported for the
 * current group, and never shrinks unless a reallocation forces it to.
 * ------------------------------------------------------------------------- */
#define GROOVY_AUDIO_HOLD_MAX_BLOCKS 8u

struct groovy_audio_hold {
    int16_t *pcm;            /* flat buffer, interleaved samples */
    size_t   cap_frames;     /* reported capacity, in PAIRS (see cap_samples note above) */
    size_t   cap_samples;    /* actually allocated capacity, in int16 SAMPLES -- internal truth */
    size_t   frames;         /* pairs held, across all blocks */
    size_t   block_frames[GROOVY_AUDIO_HOLD_MAX_BLOCKS];
    unsigned n_blocks;
    unsigned channels;       /* 1 or 2; fixed by the first deposit of a burst */
    uint64_t deposited;        /* SESSION counters, never cleared by _clear */
    uint64_t played;
    uint64_t lost;
};

/* Deposits a block of `frames` pairs (sample-frames: 1 if mono, 2 if
 * stereo, per frame) at `channels` channels. Returns 1 if accepted, 0
 * otherwise. No silent write ever:
 *
 *   - h or pcm null, frames == 0, frames > 32768, channels neither 1 nor
 *     2: returns 0 WITHOUT counting a loss (invalid input, not a full
 *     queue);
 *   - n_blocks == GROOVY_AUDIO_HOLD_MAX_BLOCKS: lost++, returns 0;
 *   - n_blocks > 0 and channels != h->channels: lost++, returns 0
 *     (never mix two formats within the same burst);
 *   - realloc failure: lost++, returns 0, the old buffer stays intact
 *     (same pattern as gm_lz4_compress / gm_dup_after_real in
 *     libgm/src/gm_proto.c: realloc into a temporary variable, never
 *     overwriting the existing pointer on failure).
 */
static inline int groovy_audio_hold_deposit(struct groovy_audio_hold *h,
                                           const int16_t *pcm, size_t frames,
                                           unsigned channels)
{
    if (!h || !pcm || frames == 0u || frames > 32768u ||
        (channels != 1u && channels != 2u))
        return 0;   /* invalid input, never a full-queue loss */

    if (h->n_blocks == GROOVY_AUDIO_HOLD_MAX_BLOCKS) {
        h->lost++;
        return 0;
    }
    if (h->n_blocks > 0u && channels != h->channels) {
        h->lost++;   /* never mix two formats within the same burst */
        return 0;
    }

    {
        size_t needed_frames  = h->frames + frames;
        size_t needed_samples = needed_frames * (size_t)channels;

        if (needed_samples > h->cap_samples) {
            /* Same pattern as gm_lz4_compress / gm_dup_after_real
             * (libgm/src/gm_proto.c): realloc into a temporary variable,
             * never overwriting the existing pointer on failure. */
            int16_t *grown = (int16_t *)realloc(h->pcm, needed_samples * sizeof(int16_t));
            if (!grown) {
                h->lost++;
                return 0;
            }
            h->pcm         = grown;
            h->cap_samples = needed_samples;
        }
        h->cap_frames = h->cap_samples / (size_t)channels;

        memcpy(h->pcm + h->frames * (size_t)channels, pcm,
               frames * (size_t)channels * sizeof(int16_t));
    }

    h->block_frames[h->n_blocks++] = frames;
    h->frames  += frames;
    h->channels = channels;
    h->deposited++;
    return 1;
}

/* Returns the pointer to block `i` (offset = sum of the preceding
 * block_frames, multiplied by channels) and sets *out_frames. Returns
 * NULL if `i` is beyond n_blocks (and sets *out_frames to 0 if non-null). */
static inline const int16_t *groovy_audio_hold_block(const struct groovy_audio_hold *h,
                                                     unsigned i, size_t *out_frames)
{
    size_t offset_frames = 0u;
    unsigned k;

    if (!h || i >= h->n_blocks) {
        if (out_frames) *out_frames = 0u;
        return NULL;
    }

    for (k = 0u; k < i; ++k)
        offset_frames += h->block_frames[k];

    if (out_frames) *out_frames = h->block_frames[i];
    return h->pcm + offset_frames * (size_t)h->channels;
}

/* Clears the held blocks: n_blocks, frames and channels go back to 0.
 * pcm and both capacities (cap_frames, cap_samples) are PRESERVED -- the
 * buffer grows once and lives until session close, exactly like
 * libgm's lz4_scratch (definitely not the malloc/free-per-frame
 * pattern). deposited/played/lost are never touched here: they are
 * session counters. */
static inline void groovy_audio_hold_clear(struct groovy_audio_hold *h)
{
    if (!h) return;
    h->n_blocks  = 0u;
    h->frames   = 0u;
    h->channels = 0u;
}

/* Frees the buffer and zeroes the whole structure, session counters
 * included -- call only at session close. */
static inline void groovy_audio_hold_release(struct groovy_audio_hold *h)
{
    if (!h) return;
    free(h->pcm);
    memset(h, 0, sizeof(*h));
}

#endif /* GROOVY_AUDIO_HOLD_H */
