/* SPDX-License-Identifier: GPL-3.0-or-later */
/* record_groovy.c — RetroArch recording driver shipping post-shader frames
 * via libgm to a crt-bridge daemon over UDP/32100.
 *
 * Cloned structurally from record_ffmpeg.c at RetroArch master commit 4a82976.
 * Sink swapped: FFmpeg encoding → libgm UDP transmission.
 *
 * Configuration: receiver_ip[:port] is read from the RetroArch
 * video_record_config (record_config) retroarch.cfg key. MANDATORY —
 * there is no default receiver IP. If that key is absent or empty,
 * groovy_new refuses to start (returns NULL) and RARCH_ERR logs the
 * MAC-resolution recipe. A wrong default used to hang or silently aim UDP
 * at the wrong machine; an absent one fails in about a second with an
 * exact message. The key
 * "record_config" was chosen over "record_path" because record_path is
 * used by the RetroArch UI to name the output file (string interpreted as
 * filesystem path), while record_config is an opaque configuration blob
 * already forwarded as params->config inside the record_params struct.
 * Confirmed by reading record/record_driver.h at 4a82976 — record_params
 * field `config` maps to the retroarch.cfg `record_config` key.
 *
 * Wire-format reconciliations (CRITICAL — do not "fix"):
 *   - CMD_INIT is 4 bytes on the wire (verified against a live capture:
 *     `02 00 03 02`). Mistglow's 5-byte form with rgb_mode is wrong for
 *     our daemon; receiver/proto.h:43 enforces sizeof == 4.
 *   - pclock is double in MHz (NOT Hz). GroovyMAME 0.287 wire-verified.
 *   - CMD_BLIT_VSYNC opcode 0x06 (progressive 240p), NOT 0x07
 *     (0x07 = CMD_BLIT_FIELD_VSYNC for interlaced — libgm's minimal viable
 *     implementation sends 0x06 only, consistent with the daemon's own
 *     validation).
 *   - BGR24 → RGB888 in-place swap mandatory before gm_send_blit.
 *
 * Audio started as a no-op here (video-only): groovy_push_audio returned
 * true immediately without emitting CMD_AUDIO. Compression started raw and
 * uncompressed only (compression = 0); both grew real support later in
 * this same file (see the audio and compression fields below).
 *
 * Threading: video_threaded=false is mandatory. All gm_send_* calls happen
 * from the RetroArch runloop thread. No additional synchronization is
 * needed here.
 *
 * ACK pacing: groovy_push_video drains every queued status once per frame
 * (gm_recv_status timeout=0, looped) and calls
 * compute_vsync_target_from_acks_post_poll (groovy_pacing.h) on the
 * resulting state. Freshness windows: <50 ms fresh, 50 ms-2 s
 * stale-tolerable, >2 s fall back to local clock (returns 0) and log
 * ack_lost once. State lives in groovy_ack_state_t inside groovy_state_t.
 * Logic is shared with the test bridge via groovy_pacing.h. Unified with
 * the re-announce flush: a single-shot poll ahead of the drain loop would
 * starve the loop of every status whenever the peer sends at or below the
 * emitter's frame rate — see the comment above the drain loop in
 * groovy_push_video for the full story.
 *
 * Geometry: CMD_SWITCHRES is emitted from groovy_push_av_info
 * (registered in the record_driver_t vtable; the hook site lives in
 * runloop.c). Modeline computation is shared with the unit-test bridge via
 * #include "groovy_modeline.h". Dedupe (same geometry → no second
 * CMD_SWITCHRES) and defensive checks (width=0 → reject) are implemented
 * here per STRIDE T-3-12 (bad av_info) and T-3-13 (spam).
 *
 * Option G: the daemon forges modelines from wire data at runtime and does NOT
 * require CMD_SWITCHRES before the first CMD_BLIT_VSYNC. Frames render
 * correctly even if push_av_info fires after the first blit.
 *
 * Build:
 *   This file lives in record/drivers/ alongside RetroArch's other
 *   recording drivers. libgm.a is linked via the HAVE_GROOVY Makefile
 *   toggle.
 *
 * STRIDE compliance:
 *   T-3-02 — BGR24 swap in bgr24_to_rgb888 (mandatory, verifiable via test)
 *   T-3-09 — swap_buf realloc with NULL-check; frame dropped on alloc fail
 *   T-3-10 — parse_receiver_config uses snprintf(out_ip, ip_cap, ...) — safe
 *   T-3-11 — vid->is_dupe honored (skip duplicate frames)
 *   T-3-12 — groovy_push_av_info rejects width=0, height=0, fps≤0
 *   T-3-13 — groovy_push_av_info dedupes identical successive callbacks
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>

/* RetroArch record driver interface. Include path is relative to
 * record/drivers/ where this file lives. */
#include "record_groovy.h"

/* Logging macros (RARCH_LOG). Path relative to record/drivers/. */
#include "../../verbosity.h"

/* libgm public API. Include path relies on DEFINES += -I$(LIBGM_DIR)/include
 * injected by the Makefile.common HAVE_GROOVY block (patch hunk 3). */
#include <gm.h>

/* Shared modeline computation (Plan 03-06). Must follow gm.h so the
 * gm_modeline type is visible. The header is also included by the unit-test
 * bridge (libgm/test/groovy_test_bridge.c) to exercise the same math. */
#include "groovy_modeline.h"

/* The anonymous "[groovy_pacing] ack_lost" line from groovy_pacing.h's
 * default stderr fallback is silenced here -- the driver now prints its
 * own NAMED line instead ("[groovy_pacing] ack_back target=%s role=%s" /
 * "ack_lost target=%s role=%s"), one per receiver, at the call sites of
 * groovy_ack_link_on_status/on_tick (see groovy_drain_statuses and
 * groovy_push_video further below).
 * compute_vsync_target_from_acks_post_poll itself is NOT modified: only
 * its internal call to the log hook (the pacing code's own local
 * fallback, rx[0] only) is silenced here, replaced by the named line from
 * groovy_ack_link_on_tick on that SAME rx[0]. */
static inline void groovy_pacing_log_mute(const char *ev) { (void)ev; }
#define GROOVY_PACING_LOG_HOOK groovy_pacing_log_mute

/* ACK-pacing state machine. Must follow gm.h (for gm_handle and
 * gm_status). Header-only; also included by groovy_test_bridge.c so the test
 * bridge exercises the exact same logic path. */
#include "groovy_pacing.h"

/* Emitter-side audio: output setting, CMD_INIT frequency code, linear
 * resampler. Header-only, shared with the test bridge like
 * groovy_modeline.h and groovy_pacing.h. */
#include "groovy_audio.h"

/* Lever b6: holds back a frame's audio, released stuck to the next
 * frame's video burst. Header-only, shared with the test bridge, same
 * pattern as groovy_audio.h above. */
#include "groovy_audio_hold.h"

/* Emitter-side compression setting: reads GROOVY_COMPRESSION, same
 * pattern as groovy_audio.h. Header-only, shared with the test bridge. */
#include "groovy_compression.h"

/* Per-session send MTU: reads GROOVY_MTU, handed to libgm via
 * gm_set_mtu. Header-only, same pattern as groovy_compression.h. */
#include "groovy_mtu.h"

/* Generic CPU supersampling box: reads GROOVY_DOWNSAMPLE and averages
 * n*n blocks. Same pattern as groovy_compression.h above. */
#include "groovy_downsample.h"

/* Input-channel setting and latency histogram: reads GROOVY_INPUT,
 * measures k. Header-only, shared with the test bridge. */
#include "groovy_input.h"

/* Numbered frame witness: reads GROOVY_DUMP_FRAME_INTERVAL /
 * GROOVY_DUMP_FRAME_SEQ, renders the file name. Header-only, same
 * pattern as groovy_compression.h above. */
#include "groovy_dump.h"
#include "groovy_reannounce.h"

/* Diagnostic frame-skip hook: reads GROOVY_SKIP_FRAME_EVERY and decides
 * whether to skip a frame. Header-only, same pattern as groovy_dump.h
 * above (no dependency on libgm). */
#include "groovy_skip.h"

/* Lever 1 switch: reads GROOVY_FRAME_DUP, handed to libgm via the
 * frame-skip setter (gm.h). Header-only, same pattern as
 * groovy_compression.h above. */
#include "groovy_frame_dup.h"

/* The field(n) formula and lever 2 switch: reads GROOVY_FIELD_PER_FRAME.
 * Depends on groovy_pacing.h (already included above) for
 * groovy_ack_state_t. */
#include "groovy_field.h"

/* Receiver table, provenance filter, follower saturation, and input
 * merge: reads GROOVY_FOLLOWERS. Depends on gm.h (for
 * gm_joy_inputs/gm_ps2_inputs), already included above. */
#include "groovy_followers.h"

#include "groovy_menu_precedence.h"   /* menu/variable precedence */
#include "groovy_followers_menu.h"    /* composes the 4 "Additional Follower N" switches */

/* strlcpy: follower precedence copies st->followers_src from getenv or
 * cfg->arrays.groovy_followers, never a borrowed pointer. Not included
 * elsewhere in this file -- record_ffmpeg.c already includes it for the
 * same need. */
#include <compat/strl.h>

/* Fast-forward guard: runloop fastmotion flag + monotone clock. runloop.h
 * is at the fork root -> relative path from record/drivers/. */
#include "../../runloop.h"            /* runloop_get_flags, RUNLOOP_FLAG_FASTMOTION */
#include <features/features_cpu.h>    /* cpu_features_get_time_usec */

/* Clock servo. audio_driver.h carries the clamped term accessor;
 * state_manager.h and menu_*.h carry the five freeze signals;
 * configuration.h carries video_refresh_rate, never hardcoded.
 * groovy_servo.h MUST follow groovy_pacing.h (already included above):
 * its #error guard fires otherwise. */
#include "../../audio/audio_driver.h"  /* audio_driver_set/get_groovy_term, audio_state_get_ptr */
#include "../../state_manager.h"       /* state_manager_frame_is_reversed (no guard needed: returns false without HAVE_REWIND) */
#ifdef HAVE_MENU
#include "../../menu/menu_driver.h"    /* menu_state_get_ptr */
#include "../../menu/menu_defines.h"   /* MENU_ST_FLAG_ALIVE */
#endif
#include "../../configuration.h"       /* config_get_ptr, settings_t.floats.video_refresh_rate */
#include "groovy_servo.h"              /* apres groovy_pacing.h : il consomme groovy_ack_state_t */

/* ---------------------------------------------------------------------------
 * Per-instance state.
 *
 * Allocated by groovy_new, owned until groovy_free.
 * Static-global swap_buf pattern from RESEARCH.md Pattern 2 sketch is
 * REJECTED here: static globals are not safe when RetroArch unloads+reloads
 * a core without restarting the process (e.g., content switch). Use
 * instance-local allocation instead.
 * ------------------------------------------------------------------------- */
/* groovy_av_cache_t — per-instance geometry dedupe state.
 *
 * Promoted from function-static in groovy_push_av_info to instance field
 * of groovy_state_t (Phase 3.1, HIL-3-08 fix) so that lifetime is explicit
 * and tied to the driver session. The test bridge mirrors this with its own
 * file-scope g_av_cache (groovy_test_bridge.c). */
typedef struct {
    unsigned width;
    unsigned height;
    double   fps;     /* effective fps used for the last emit (may be cached) */
    bool     valid;
} groovy_av_cache_t;

/* ---------------------------------------------------------------------------
 * Per-receiver latency window. Moved here (ahead of struct
 * groovy_receiver) from its former file scope -- the histograms and
 * counters that used to live as `s_lat_*` become a field of each
 * receiver, 384 bytes of histograms each. Defined ahead of
 * GROOVY_RX_MAX/struct groovy_receiver below, which embeds it.
 * ------------------------------------------------------------------------- */
#define GROOVY_LAT_HIST_BUCKETS 64u

struct groovy_lat_window {
    uint16_t hist_inflight[GROOVY_LAT_HIST_BUCKETS];
    uint16_t hist_net[GROOVY_LAT_HIST_BUCKETS];
    uint16_t hist_queue[GROOVY_LAT_HIST_BUCKETS];
    unsigned n;
    unsigned ackq_max;
    unsigned anomalies;
    uint32_t last_sent;
    uint32_t last_echo;
    uint32_t last_shown;
};

/* ---------------------------------------------------------------------------
 * Receiver table. One gm_handle PER receiver: the master at index 0
 * (rx[0].gm == st->gm, never two closes of the same handle -- see
 * groovy_free), the GROOVY_FOLLOWERS followers at indices 1..n_rx-1, in
 * their declaration order.
 *
 * declared_ip is in NETWORK order (same convention as
 * groovy_followers_ip_parse and gm_last_status_peer). declared_ok says
 * whether the declared address is known: at 0, the provenance filter
 * stays inactive for this receiver (groovy_drain_statuses). This flag
 * replaces an older declared_ip == 0 sentinel, which conflated "unknown
 * address" with a follower declared at 0.0.0.0.
 * ------------------------------------------------------------------------- */
#define GROOVY_RX_MAX (1u + GROOVY_FOLLOWERS_MAX)

enum groovy_rx_role {
    GROOVY_ROLE_MASTER  = 0,
    GROOVY_ROLE_FOLLOWER = 1
};

struct groovy_receiver {
    gm_handle *gm;
    int        role;             /* enum groovy_rx_role */
    char       target[24];        /* "ip:port", for the trace lines */
    uint32_t   declared_ip;      /* network order */
    uint16_t   declared_port;
    int        declared_ok;      /* 1 = declared address known, provenance filter active */
    int        compression;      /* enum groovy_compression_mode, specific to this receiver */
    unsigned   mtu;               /* specific to this receiver */
    int        pad;              /* padded-session mode for THIS receiver;
                                   * always 0 for the master */
    int        audio_on;         /* active by default for a declared follower */
    int        inputs;          /* enum groovy_inputs (groovy_followers.h) */
    uint64_t   audio_errors;     /* gm_send_audio refusals, per receiver */
    uint64_t   audio_hold_not_played;  /* held blocks that were NEVER emitted
                                        * to this receiver because it was
                                        * skipped by saturation at fan-out
                                        * time (lever b6) */

    /* --- Per-receiver pacing and re-announce. Only rx[0].ack_state feeds
     * compute_vsync_target_from_acks_post_poll; a follower's own state only
     * feeds its own telemetry and its own budget. calloc already zeroed
     * everything (same convention as the rest of this table). --- */
    groovy_ack_state_t             ack_state;
    struct groovy_ack_link         link;
    struct groovy_reannounce_state reannounce;
    int                            reannounce_exhausted_logged; /* trace emitted once per
                                                                    episode, rearmed when armed == 0 */

    /* --- Outcome of announce sends to THIS receiver: CMD_INIT and
     * CMD_SWITCHRES, at the three call sites (session opening, re-announce,
     * groovy_switchres_all). announces_ok = sends accepted by Winsock;
     * announces_failed = sends refused (the code is in the [groovy-annonce]
     * trace). Cumulative over the session -- never reset by the one-second
     * [groovy-lat] window: an old failure must stay readable. calloc zeroes
     * them. --- */
    uint32_t                       announces_ok;
    uint32_t                       announces_failed;

    /* --- CMD_SWITCHRES deferred from a re-announce. Set by
     * groovy_reannounce_emit when st->reannounce_spacing_us > 0; sent by
     * groovy_reannounce_flush_deferred BEFORE the fan-out of a LATER frame
     * whose instant exceeds switchres_t_init_ns + spacing -- never the
     * frame that set the deferral itself: switchres_deferred_turn retains
     * st->turn_video from that moment, and the flush requires a different
     * st->turn_video (the clock alone isn't enough to guarantee "later"
     * with a very short spacing). This latch used to be pinned on
     * st->frame_id, which only advances if the MASTER's own frame send
     * succeeds; a master whose send fails persistently (route gone,
     * WSAENOBUFS, ...) would then freeze frame_id for good, and a
     * FOLLOWER's deferred send would then never go out again -- a black
     * screen with no recourse, exactly what this design removes. turn_video
     * advances on every pass through groovy_push_video, whatever the
     * fan-out result: the latch no longer depends on any send succeeding.
     * Cancelled if the receiver sent back a sync in the meantime, or
     * OVERWRITTEN (and traced) if a new re-announce happens before the
     * send. calloc zeroes them. --- */
    int                            switchres_deferred;
    uint64_t                       switchres_t_init_ns;
    int                            switchres_rc_init;
    uint32_t                       switchres_deferred_turn;

    /* --- [groovy-lat] telemetry, per receiver. rejects= : statuses
     * rejected by the provenance filter on this receiver, one-second
     * window like lat.* -- reset by the same gesture. --- */
    struct groovy_lat_window lat;
    uint32_t                 rejects;

    /* --- Follower saturation. Unused for the master (i == 0 in
     * groovy_fan_out, never skipped). last_sent_id/has_sent are the last
     * frame_id ACTUALLY sent to THIS receiver -- not the global frame_id,
     * which would freeze the skip as soon as another receiver advances;
     * held while a send is skipped (groovy_fan_out). They now only serve
     * [groovy-lat]: the saturation signal counts frames sent and not yet
     * acked, on the sat ring buffer (groovy_sat_note_sent). saturations= :
     * one-second window, reset the same way as lat.* and rejects. --- */
    struct groovy_sat_state sat;
    uint32_t                last_sent_id;
    int                     has_sent;
    uint32_t                saturations;
};

typedef struct {
    gm_handle          *gm;
    uint8_t            *swap_buf;       /* BGR→RGB conversion scratch buffer */
    size_t              swap_cap;       /* current capacity in bytes */
    uint32_t            frame_id;       /* monotonic frame counter (per driver instance) */
    /* ++ on EVERY pass through the { vsync_target ... } block of
     * groovy_push_video, whatever the fan-out result -- INDEPENDENT of any
     * send, unlike frame_id (which only advances on emit_ok, and emit_ok is
     * only the MASTER's own result, groovy_fan_out). Serves as the "later
     * frame" anchor for groovy_reannounce_flush_deferred
     * (switchres_deferred_turn, struct groovy_receiver): before this field,
     * a master whose send failed persistently would freeze frame_id for
     * good, and a follower's deferred CMD_SWITCHRES would then never go
     * out again. */
    uint32_t            turn_video;
    /* ack_state/reannounce/reannounce_exhausted_logged moved into struct
     * groovy_receiver -- st->rx[0].ack_state etc. Only reannounce_enabled
     * stays here: the GROOVY_REANNOUNCE setting, one per SESSION (the
     * budget itself is now per receiver). */
    int                 reannounce_enabled;  /* GROOVY_REANNOUNCE */
    unsigned reannounce_spacing_us;  /* GROOVY_REANNOUNCE_SPACING_US ; 0 = send immediately */
    unsigned            width;          /* cached from last push_video (Plan 03-06 handoff) */
    unsigned            height;         /* cached from last push_video */
    groovy_av_cache_t   av_cache;       /* geometry dedupe state — Phase 3.1 */
    enum ffemu_pix_format pix_fmt;      /* input pixel format — Phase 3.1 Bug D fix */
    /* Per-frame dim hysteresis (Phase 3.1 v3 native-resolution path):
     * tracks a candidate new (w,h) and how many consecutive frames it has
     * persisted. We only emit CMD_SWITCHRES when the candidate has been stable
     * for HYSTERESIS_FRAMES — prevents the daemon's rate-limiter from coalescing
     * away genuine mode changes when Beetle PSX oscillates between fields. */
    unsigned            pending_w;
    unsigned            pending_h;
    unsigned            pending_streak;
    bool                inited;         /* true after groovy_new succeeded */

    /* Phase 3.2 regime state. */
    enum groovy_mode current_mode;
    enum groovy_mode pending_mode;
    unsigned           mode_streak;

    /* --- Audio to the receiver. Set at session opening, never touched
     * afterwards; push_audio only reads it. --- */
    enum groovy_audio_mode  audio_mode;      /* off by default */
    uint8_t                 audio_rate_code; /* 0 = no audio announced */
    uint8_t                 audio_channels;  /* 1 or 2 */
    struct groovy_resampler audio_rs;        /* inactive when src == dst */
    int16_t                *audio_buf;       /* resampler output */
    size_t                  audio_buf_frames;/* capacity, in pairs */
    uint64_t                audio_blocks;    /* blocks handed to libgm */
    uint64_t                audio_frames;    /* pairs handed to libgm */
    uint64_t                audio_errors;    /* gm_send_audio refusals */

    /* --- Lever b6, sticking a frame's audio to the video burst. Set at
     * session opening; the queue itself carries over from one frame to the
     * next. --- */
    enum groovy_audio_hold_mode audio_hold_mode;   /* off by default, on BOTH sides */
    struct groovy_audio_hold    audio_hold;

    /* --- Compression. Set at session opening, never touched afterwards. --- */
    enum groovy_compression_mode compression_mode;   /* off by default */

    /* --- Lever 1 switch. Set at session opening, never touched
     * afterwards; handed to libgm via the frame-skip setter. dup_count_base
     * is libgm's monotonic counter value at the start of the current
     * [groovy-emit] window -- what is displayed is a DIFFERENCE, never the
     * absolute value. --- */
    enum groovy_frame_dup_mode frame_dup_mode;   /* off by default */
    uint32_t                   dup_count_base;   /* calloc -> 0, consistent with "right at the start" */

    /* --- Lever 2 switch. Set at session opening, never touched
     * afterwards. fields_sent is a WINDOW counter, indexed by the
     * parity of the sent field (0 or 1), reset by the [groovy-emit] block
     * every 600 frames -- a wide gap between the two entries would signal
     * a stuck parity, hence a stale ACK. --- */
    enum groovy_field_mode field_mode;      /* both (0x07) by default */
    uint32_t                fields_sent[2];

    /* --- Send-side MTU. Set at session opening, never touched
     * afterwards; handed to libgm via gm_set_mtu. --- */
    unsigned  mtu;                          /* 1472 by default */

    /* --- Diagnostic frame-skip hook. skip_every set at session opening,
     * never touched afterwards. skipped_frames is a WINDOW counter, reset
     * by the [groovy-emit] block every 600 frames -- same cadence as
     * emit_samples below. --- */
    unsigned  skip_every;                   /* 0 = off, otherwise one frame in N */
    uint32_t  skipped_frames;               /* frames skipped since the last reading */

    /* --- Supersampling CPU box. Set at session opening; 1 = off, and then
     * none of this is touched. --- */
    unsigned  downsample_n;                 /* 1, 2, 4 or 8 */
    uint8_t  *down_buf;                     /* box destination, allocated as needed */
    size_t    down_cap;
    uint64_t  downsample_dims_mismatch;     /* frames rejected, never distorted */
    uint64_t  downsample_ns_total;          /* box cost, read every 600 */
    uint64_t  downsample_ns_max;
    uint32_t  downsample_samples;

    /* --- Input channel, per receiver. Set at session opening, never
     * touched afterwards -- s_groovy_input_h[] (the file-scope set,
     * below) is what the ported drivers actually use; this field is only
     * a trace of the GROOVY_INPUT setting read for this instance (the
     * RUNTIME guard, common to every receiver). --- */
    enum groovy_input_mode  input_mode;      /* off by default */

    /* --- Send-path timer: brackets gm_send_blit / gm_send_blit_field,
     * compression included since it lives inside. Read every 600 frames,
     * then reset. Does NOT measure compression's own cost alone -- see
     * the comment at the call site in groovy_push_video. --- */
    uint64_t                emit_ns_total;   /* sum of send-block durations */
    uint64_t                emit_ns_max;     /* worst case since the last reading */
    uint32_t                emit_samples;    /* number of frames measured */

    /* --- Receiver table. rx[0] = the master (rx[0].gm == gm above),
     * rx[1..n_rx-1] = the GROOVY_FOLLOWERS followers. calloc -> n_rx = 0
     * and before_master = 0 before groovy_new sets them; the table itself
     * never needs a memset, each used field is only read after being
     * written. --- */
    struct groovy_receiver rx[GROOVY_RX_MAX];
    unsigned                n_rx;
    uint32_t                before_master;    /* window counter, [groovy-emit] */

    /* A SINGLE one-second window for every receiver -- formerly
     * s_lat_win_start_ns (file scope), now a session field so each
     * instance has its own. */
    uint64_t                lat_win_start_ns;

    /* --- Clock servo. Both settings are set at session opening, never
     * touched afterwards. servo_log_ns is its OWN window, independent of
     * [groovy-lat]'s (two separate lines, so two separate windows). --- */
    enum groovy_servo_mode servo_mode;        /* off by default */
    int                    audio_ratio_on;    /* 0 by default */
    int                    servo_trace;       /* trace per measured ack, off by default */
    groovy_servo_state_t   servo;             /* calloc -> implicit term of 1.0 */
    uint64_t               servo_log_ns;      /* start of the one-second window */
    uint32_t               servo_acked;     /* fresh acks seen within the window */
    double                 servo_ppm_previous; /* TOTAL term of the previous line, for the slope. NEVER a static */
    int                    servo_vrr_trace;   /* 1 after tracing an unreadable video_refresh_rate */
    int                    ratio_refused_logged; /* 1 after tracing a refused thread report */

    /* The followers value already resolved (menu or variable). COPIED,
     * never borrowed: this buffer depends on neither the environment
     * block nor settings_t. Empty == no follower. 513 =
     * GROOVY_FOLLOWERS_LEN_MAX plus the terminating zero, same as
     * settings->arrays.groovy_followers. */
    char followers_src[513];
} groovy_state_t;

/* Stable-frame threshold before committing a dim change to the daemon.
 * 15 frames @ 60Hz = 250ms — comfortably longer than the daemon's 100ms
 * MIN_MODESET_INTERVAL_NS rate-limit window, so once we emit, the daemon
 * will fire it. Beetle PSX's field-by-field oscillation bursts shorter
 * than this and gets filtered. */
#define GROOVY_DIM_HYSTERESIS_FRAMES 15u

/* Bug A fix (Phase 3.1, 2026-05-10 PM HIL re-run): the last-known-good fps
 * from SET_SYSTEM_AV_INFO is a *session-global* scalar that must survive the
 * RetroArch core-reinit cycle (CMD_EVENT_REINIT triggers groovy_free +
 * groovy_new, wiping any per-instance struct field). Previously this was
 * `groovy_state_t::last_valid_fps`, calloc'd to 0 by groovy_new — so the
 * second instance's first SET_GEOMETRY callback (fps=0) had no cached value
 * and emitted CMD_SWITCHRES with refresh:0/pclock:0 (HIL-3-08 partial
 * regression observed in the 2026-05-10 PM HIL run).
 *
 * RetroArch's record driver is singleton (only one record_driver_t active per
 * process), so file-scope static is safe across re-init cycles and only one
 * instance ever reads/writes it at a time.
 *
 * Note: `av_cache` (geometry dedupe) intentionally stays per-instance — a
 * fresh driver session SHOULD re-emit the same modeline after reinit. Only
 * the fps seed must persist. */
static double s_last_valid_fps = 0.0;

/* ---------------------------------------------------------------------------
 * Latency instrumentation (quick-260828-jue) — Point A: in-flight frame
 * count derived from the ACK the pacing code already reads (compute_vsync_
 * target_from_acks) plus a local ACK-queue drain (see the call site in
 * groovy_push_video). Three 64-bucket histograms (value clamped to 63), one
 * per grandeur, cleared each 1-second window. One bucket increment per
 * frame — zero cost; percentiles read back via a cumulative scan
 * (lat_hist_percentiles), never a sort.
 *
 * What used to live here as file-scope statics (GROOVY_LAT_HIST_BUCKETS,
 * s_lat_hist_*, s_lat_win_n, s_lat_ackq_max, s_lat_anomalies,
 * s_lat_last_*) became `struct groovy_lat_window`, a field PER RECEIVER
 * (`rx->lat`, defined above with struct groovy_receiver) -- one
 * [groovy-lat] line per receiver, still the same window.
 * `GROOVY_LAT_HIST_BUCKETS` and `struct groovy_lat_window` stay defined
 * ahead of `struct groovy_receiver`, so the macro isn't repeated here.
 * `s_lat_win_start_ns` becomes `st->lat_win_start_ns` (a single window
 * shared by every receiver of a given session).
 * ------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------
 * Input channel, per receiver -- file-scope state, a singleton like
 * s_last_valid_fps above.
 *
 * s_groovy_input_h[] : the set of handles carrying an input channel,
 * indexed EXACTLY like st->rx[] (index 0 = the master, always a
 * candidate when GROOVY_INPUT=on; indices 1..n_rx-1 = the followers whose
 * `inputs` key isn't `off`). NULL = this receiver has no input channel.
 * s_groovy_input_attr[] carries the merge attribution (enum
 * groovy_inputs, groovy_followers.h) at the same index --
 * GROOVY_INPUTS_IDENTITY for the master, P1/P2 for a follower.
 * s_groovy_input_acc[] is the `inputs=` counter of [groovy-lat], per
 * receiver, reset to zero on every window. Set at opening (groovy_new),
 * reset to NULL AS A BLOCK by groovy_input_set_clear -- at closing
 * (groovy_free) AND on every failure path of groovy_new, BEFORE any
 * gm_close_inputs/gm_close -- otherwise an in-flight driver call would
 * touch a closed socket (now valid for the WHOLE set, not just the
 * former single handle). Every groovy_input_take_* loops over the entire
 * set and silently ignores any NULL index: as long as no receiver has an
 * input channel hooked up, the ported gamepad driver simply stays inert,
 * as before.
 *
 * s_last_joy_frame : the last frame_id SEEN on the joystick channel of
 * the MASTER ALONE (index 0) -- the k latency measurement stays bounded
 * to the master, even though a follower can now also carry inputs; not
 * to be confused with s_groovy_input_acc[i], which counts the ACCEPTED
 * datagrams of EACH receiver, master and followers included.
 *
 * s_input_lat : k latency histogram, same "silent at rest" discipline as
 * rx->lat (struct groovy_lat_window) but reported through its own trace
 * prefix, so as not to mix two measurements in the same log (see the
 * call site in groovy_push_video).
 * ------------------------------------------------------------------------- */
static gm_handle *s_groovy_input_h[GROOVY_RX_MAX];    /* NULL = no inputs for this receiver */
static int        s_groovy_input_attr[GROOVY_RX_MAX]; /* enum groovy_inputs from rx->inputs */
static uint32_t   s_groovy_input_acc[GROOVY_RX_MAX];  /* inputs= from [groovy-lat], per receiver */
/* Timestamp (groovy_pacing_mono_ns) of the last ACCEPTED input datagram,
 * per receiver -- 0 = none in this session. Set by
 * groovy_input_take_joy, read by groovy_input_is_live. */
static uint64_t   s_groovy_input_last_ns[GROOVY_RX_MAX];
/* s_groovy_input_target[] : the "ip:port" target of each channel, copied
 * from rx->target at hookup time, because groovy_input_take_joy has no
 * access to st->rx[] and needs it to name the follower in its transition
 * trace. s_groovy_input_mute_logged[] : this trace has already gone out
 * for this silent follower -- one line per transition, never one per
 * poll. s_groovy_input_had_source : a live source has already supplied a
 * state, without which the "controllers released" trace would fire right
 * at opening, before any input at all. All three cleared by
 * groovy_input_set_clear. */
static char       s_groovy_input_target[GROOVY_RX_MAX][24];
static int        s_groovy_input_mute_logged[GROOVY_RX_MAX];
static int        s_groovy_input_had_source = 0;
static uint32_t   s_last_joy_frame  = 0u;
/* The `frame` field of the master's last ACCEPTED status (passed the
 * provenance filter) -- the value of st->rx[0].ack_state.last_frame,
 * copied here because groovy_input_take_joy has no access to the session
 * state. Used for the k latency measurement, instead of
 * gm_last_ack_frame, which returns the last status RECEIVED, spoofed or
 * not (libgm fills its cache before the driver filters). 0 = no status
 * accepted in this session. */
static uint32_t   s_groovy_master_ack_frame = 0u;
static struct groovy_input_lat s_input_lat;

/* ---------------------------------------------------------------------------
 * groovy_input_set_clear -- clears the whole set of input channels and
 * its two twin arrays, in a single gesture.
 *
 * The `mister` gamepad driver stays registered with RetroArch no matter
 * what, and calls groovy_input_take_joy on every poll. A pointer left in
 * s_groovy_input_h[] after a gm_close therefore becomes a use-after-free
 * on the next poll. Hence the rule: this function is called BEFORE any
 * gm_close, on EVERY path that closes handles -- both failure paths of
 * groovy_new as well as groovy_free -- and right at the start of
 * groovy_new.
 *
 * s_groovy_input_attr[] and s_groovy_input_acc[] used to survive
 * groovy_free; the next session's first [groovy-lat] line would then
 * carry an inputs= inherited from the previous one.
 *
 * s_groovy_input_last_ns[] follows the same gesture -- a timestamp
 * inherited from a previous session would make a fresh follower look
 * "alive".
 *
 * s_groovy_master_ack_frame too -- a fresh session's k measurement must
 * not start from the previous session's last accepted status.
 *
 * The target of each channel, its traced-transition flag, and the
 * "a source has already spoken" flag as well -- a fresh session doesn't
 * silence its first transition, and doesn't trace a release that no
 * input preceded.
 * ------------------------------------------------------------------------- */
static void groovy_input_set_clear(void)
{
    for (unsigned i = 0u; i < GROOVY_RX_MAX; i++) {
        s_groovy_input_h[i]       = NULL;
        s_groovy_input_attr[i]    = GROOVY_INPUTS_OFF;
        s_groovy_input_acc[i]     = 0u;
        s_groovy_input_last_ns[i] = 0u;
        s_groovy_input_target[i][0]   = '\0';
        s_groovy_input_mute_logged[i] = 0;
    }
    s_groovy_master_ack_frame = 0u;
    s_groovy_input_had_source = 0;
}

/* ---------------------------------------------------------------------------
 * Witness of what goes out on the wire.
 *
 * What the witness is, the three environment variables, sequence-mode
 * file naming, and the volume warning: see groovy_dump.h. Not duplicated
 * here -- a future reader should find only one source of truth.
 * ------------------------------------------------------------------------- */
static const char *s_dump_path     = NULL;
static unsigned    s_dump_interval = 60u;
static bool        s_dump_resolved = false;
static uint32_t    s_dump_counter  = 0u;
static int         s_dump_seq      = 0;

static void groovy_dump_frame_ppm(const uint8_t *rgb888,
                                  unsigned width, unsigned height)
{
    char     out[1024];
    char     tmp[1040];   /* out + ".tmp"; generous, the guard stays snprintf */
    FILE    *f;
    int      n;
    uint32_t idx;

    if (!s_dump_resolved) {
        s_dump_resolved = true;
        s_dump_path     = getenv("GROOVY_DUMP_FRAME_PATH");
        s_dump_interval = groovy_dump_interval_parse(getenv("GROOVY_DUMP_FRAME_INTERVAL"));
        s_dump_seq      = groovy_dump_seq_parse(getenv("GROOVY_DUMP_FRAME_SEQ"));
        if (s_dump_path)
            RARCH_LOG("[groovy] frame witness active: %s (1 in %u, %s)\n",
                      s_dump_path, s_dump_interval,
                      s_dump_seq ? "numbered -- one file per retained frame"
                                 : "single file, overwritten");
    }

    if (!s_dump_path || !width || !height)
        return;
    idx = s_dump_counter++;
    if ((idx % s_dump_interval) != 0u)
        return;

    if (!groovy_dump_name(out, sizeof(out), s_dump_path, s_dump_seq, idx))
        return;
    n = snprintf(tmp, sizeof(tmp), "%s.tmp", out);
    if (n <= 0 || n >= (int)sizeof(tmp))
        return;

    f = fopen(tmp, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%u %u\n255\n", width, height);
    (void)fwrite(rgb888, 1u, (size_t)width * (size_t)height * 3u, f);
    fclose(f);

    /* rename() refuses to overwrite on Windows: remove the target first.
     * Also true in SEQUENCE mode, and that's not an oversight: nothing is
     * overwritten there, but the estimator may be running while a capture
     * writes, and must never read a half-written PPM. The cost is nil next
     * to writing 690 KB. */
    (void)remove(out);
    (void)rename(tmp, out);
}

static inline unsigned lat_hist_bucket(int64_t v)
{
    if (v < 0) return 0u;   /* defensive — callers pre-filter negatives as anomalies */
    return (v >= (int64_t)GROOVY_LAT_HIST_BUCKETS)
         ? (GROOVY_LAT_HIST_BUCKETS - 1u) : (unsigned)v;
}

/* Cumulative-scan percentile read: min/p50/p95/max from a 64-bucket
 * histogram. No sort, no allocation — the entire point of the fixed-bucket
 * design (T-JUE-01 applies here too: this runs once per second, not per
 * frame, but stays allocation-free regardless). */
static void lat_hist_percentiles(const uint16_t *hist, unsigned n,
                                  unsigned *out_min, unsigned *out_p50,
                                  unsigned *out_p95, unsigned *out_max)
{
    *out_min = 0u;
    *out_p50 = 0u;
    *out_p95 = 0u;
    *out_max = 0u;
    if (n == 0u) return;

    unsigned cum        = 0u;
    unsigned p50_thresh = (n + 1u) / 2u;
    unsigned p95_thresh = (n * 95u + 99u) / 100u;
    int      have_min   = 0;
    int      have_p50   = 0;
    int      have_p95   = 0;
    if (p95_thresh == 0u) p95_thresh = 1u;
    if (p95_thresh > n)   p95_thresh = n;

    for (unsigned b = 0u; b < GROOVY_LAT_HIST_BUCKETS; ++b) {
        if (hist[b] == 0u) continue;
        if (!have_min) { *out_min = b; have_min = 1; }
        cum += hist[b];
        if (!have_p50 && cum >= p50_thresh) { *out_p50 = b; have_p50 = 1; }
        if (!have_p95 && cum >= p95_thresh) { *out_p95 = b; have_p95 = 1; }
        *out_max = b;   /* last non-empty bucket visited wins */
    }
}

/* ---------------------------------------------------------------------------
 * Configuration.
 * Set via `record_config = "ip"` or `record_config = "ip:port"` in
 * retroarch.cfg (the `video_record_config` key). The IP has NO default —
 * an earlier revision baked in a stale fallback address (e.g. 192.0.2.39)
 * as a silent default, which an audit later flagged as a contradiction. A
 * stale default sends UDP at an empty machine with no error, no dialog,
 * black tube, apparently-healthy emitter. Port stays optional; defaults to
 * GROOVY_DEFAULT_PORT.
 * ------------------------------------------------------------------------- */
#define GROOVY_DEFAULT_PORT  32100u

/* ---------------------------------------------------------------------------
 * parse_receiver_config — extract IP and port from params->config.
 *
 * Accepts formats:
 *   ""            → defaults
 *   "ip"          → ip + default port
 *   "ip:port"     → explicit ip + port
 *
 * Security (T-3-10): uses snprintf with ip_cap to prevent buffer overflow
 * regardless of how long params->config is. ip is always null-terminated
 * by construction (snprintf guarantees it).
 *
 * Returns true and fills out_ip/out_port when params->config carries an
 * explicit "ip" or "ip:port". Returns false — out_ip left empty — when the
 * key is absent, empty, or malformed (e.g. ":32100" with no IP before the
 * colon). No default IP is ever substituted; the
 * caller (groovy_new) must refuse to start on false.
 * ------------------------------------------------------------------------- */
/* Regime classification.
 * Reuses an earlier formula verbatim — already tested on Chrono Cross
 * intro/menu transitions. No new heuristic.
 *
 * Trade-off accepted: PSX fake-interlaced sources
 * (256×448, 320×448) classify as 480i and render at half vertical resolution
 * per field. They were line-doubled in the core anyway, so the visual
 * result on a 480i CRT is correct.
 */
static inline enum groovy_mode classify_mode(unsigned w, unsigned h)
{
    return (h > 280u || w > 400u) ? GROOVY_MODE_480I
                                   : GROOVY_MODE_240P_SUPER_RES;
}

static bool parse_receiver_config(const struct record_params *params,
                                  char *out_ip, size_t ip_cap,
                                  uint16_t *out_port)
{
    out_ip[0] = '\0';
    *out_port = (uint16_t)GROOVY_DEFAULT_PORT;

    if (!params || !params->config || params->config[0] == '\0')
        return false;

    /* Check for "ip:port" form. */
    const char *colon = strchr(params->config, ':');
    if (colon) {
        /* Extract the IP portion. */
        size_t ip_len = (size_t)(colon - params->config);
        if (ip_len == 0 || ip_len >= ip_cap)
            return false;
        snprintf(out_ip, ip_cap, "%.*s", (int)ip_len, params->config);
        /* Extract the port. strtol handles garbage gracefully. */
        long p = strtol(colon + 1, NULL, 10);
        if (p > 0 && p <= 65535)
            *out_port = (uint16_t)p;
    } else {
        /* No colon — entire string is an IP address. */
        snprintf(out_ip, ip_cap, "%s", params->config);
    }

    return out_ip[0] != '\0';
}

/* ---------------------------------------------------------------------------
 * bgr24_to_rgb888 — in-place B↔R byte swap (Pitfall 2 mitigation).
 *
 * glcore PBO readback produces BGR24; daemon expects RGB888. Swap is
 * mandatory for correct color on the CRT (STRIDE T-3-02).
 *
 * Handles pitch != width*3 (e.g., GL row alignment padding): reads from
 * pitch-strided source rows, writes to tightly-packed destination rows.
 * dst and src MAY alias only if dst == src AND pitch == width*3 (caller
 * MUST use swap_buf, not vid->data, to avoid aliasing in the non-trivial
 * pitch case).
 *
 * dst: contiguous output buffer, at least width*height*3 bytes.
 * src: source buffer, pitch bytes per row, height rows.
 * ------------------------------------------------------------------------- */
static void bgr24_to_rgb888(uint8_t       *dst,
                             const uint8_t *src,
                             unsigned       dst_w,
                             unsigned       dst_h,
                             unsigned       src_w,
                             unsigned       src_h,
                             unsigned       src_pitch)
{
    /* Box-filter downscale + BGR → RGB (Bug E v2: text legibility fix).
     * Same algorithm as xrgb8888_to_rgb888 but reads 3-byte BGR pixels.
     * When src == dst dims, box is 1×1 and this is a 1:1 copy with swap. */
    for (unsigned dy = 0; dy < dst_h; ++dy) {
        unsigned sy0 = (unsigned)((size_t)dy * src_h / dst_h);
        unsigned sy1 = (unsigned)((size_t)(dy + 1u) * src_h / dst_h);
        if (sy1 <= sy0) sy1 = sy0 + 1u;
        if (sy1 > src_h) sy1 = src_h;

        uint8_t *dst_row = dst + (size_t)dy * dst_w * 3u;
        for (unsigned dx = 0; dx < dst_w; ++dx) {
            unsigned sx0 = (unsigned)((size_t)dx * src_w / dst_w);
            unsigned sx1 = (unsigned)((size_t)(dx + 1u) * src_w / dst_w);
            if (sx1 <= sx0) sx1 = sx0 + 1u;
            if (sx1 > src_w) sx1 = src_w;

            uint32_t r_acc = 0u, g_acc = 0u, b_acc = 0u;
            uint32_t n = 0u;
            for (unsigned sy = sy0; sy < sy1; ++sy) {
                const uint8_t *src_row = src + (size_t)sy * src_pitch;
                for (unsigned sx = sx0; sx < sx1; ++sx) {
                    /* BGR24: B=src[0], G=src[1], R=src[2] */
                    b_acc += src_row[3u * sx + 0u];
                    g_acc += src_row[3u * sx + 1u];
                    r_acc += src_row[3u * sx + 2u];
                    ++n;
                }
            }
            dst_row[3u * dx + 0u] = (uint8_t)(r_acc / n);
            dst_row[3u * dx + 1u] = (uint8_t)(g_acc / n);
            dst_row[3u * dx + 2u] = (uint8_t)(b_acc / n);
        }
    }
}

/* ---------------------------------------------------------------------------
 * xrgb8888_to_rgb888 — convert 32-bit XRGB8888 (RetroArch FFEMU_PIX_ARGB8888,
 * 4 bytes/pixel little-endian) to packed RGB888 (3 bytes/pixel) — Bug D fix.
 *
 * RetroArch's RETRO_PIXEL_FORMAT_XRGB8888 stores each pixel as a 32-bit value
 * 0x00RRGGBB in native byte order. On x86 (little-endian), the byte layout is:
 *   src[0]=B, src[1]=G, src[2]=R, src[3]=X (alpha/padding, ignored)
 * Daemon expects packed RGB888: dst[0]=R, dst[1]=G, dst[2]=B.
 *
 * Same dst/src layout assumptions as bgr24_to_rgb888: dst contiguous
 * width*height*3, src pitch-strided height rows.
 * ------------------------------------------------------------------------- */
static void xrgb8888_to_rgb888(uint8_t       *dst,
                                const uint8_t *src,
                                unsigned       dst_w,
                                unsigned       dst_h,
                                unsigned       src_w,
                                unsigned       src_h,
                                unsigned       src_pitch)
{
    /* Box-filter downscale + XRGB8888 → RGB888 (Bug E v2: quality fix).
     *
     * For each dst pixel, average all source pixels in the box it covers.
     * For 700→320 horizontal (ratio 2.19) and 576→240 vertical (ratio 2.4),
     * each dst pixel averages ~2x2 to ~3x3 src pixels. This eliminates the
     * "missing lines" / illegible text artifact of nearest-neighbor.
     *
     * Cost: ~5x nearest-neighbor (76800 dst px × ~6 src reads + accumulate
     * + divide). Trivial at 60 Hz on any modern CPU.
     *
     * If src dims == dst dims, the box is 1×1 and this is a 1:1 copy with
     * format swap — same path serves the no-scale case. */
    for (unsigned dy = 0; dy < dst_h; ++dy) {
        /* Compute the source row range covered by this dst row. */
        unsigned sy0 = (unsigned)((size_t)dy * src_h / dst_h);
        unsigned sy1 = (unsigned)((size_t)(dy + 1u) * src_h / dst_h);
        if (sy1 <= sy0) sy1 = sy0 + 1u;
        if (sy1 > src_h) sy1 = src_h;

        uint8_t *dst_row = dst + (size_t)dy * dst_w * 3u;
        for (unsigned dx = 0; dx < dst_w; ++dx) {
            unsigned sx0 = (unsigned)((size_t)dx * src_w / dst_w);
            unsigned sx1 = (unsigned)((size_t)(dx + 1u) * src_w / dst_w);
            if (sx1 <= sx0) sx1 = sx0 + 1u;
            if (sx1 > src_w) sx1 = src_w;

            uint32_t r_acc = 0u, g_acc = 0u, b_acc = 0u;
            uint32_t n = 0u;
            for (unsigned sy = sy0; sy < sy1; ++sy) {
                const uint8_t *src_row = src + (size_t)sy * src_pitch;
                for (unsigned sx = sx0; sx < sx1; ++sx) {
                    /* XRGB8888 little-endian: B=src[0], G=src[1], R=src[2] */
                    b_acc += src_row[4u * sx + 0u];
                    g_acc += src_row[4u * sx + 1u];
                    r_acc += src_row[4u * sx + 2u];
                    ++n;
                }
            }
            /* n is at least 1 by clamp above */
            dst_row[3u * dx + 0u] = (uint8_t)(r_acc / n);  /* R */
            dst_row[3u * dx + 1u] = (uint8_t)(g_acc / n);  /* G */
            dst_row[3u * dx + 2u] = (uint8_t)(b_acc / n);  /* B */
        }
    }
}

/* ---------------------------------------------------------------------------
 * rgb565_to_rgb888 — convert 16-bit RGB565 (RetroArch RETRO_PIXEL_FORMAT_RGB565
 * / FFEMU_PIX_RGB565, 2 bytes/pixel, native little-endian uint16) to packed
 * RGB888 (3 bytes/pixel).
 *
 * Bit layout: bits 15..11 = R (5 bits), bits 10..5 = G (6 bits), bits 4..0 =
 * B (5 bits). Normative source for the 5/6/5 -> 8/8/8 expansion:
 * libretro-common/gfx/scaler/pixconv.c::conv_rgb565_argb8888 (same fork,
 * same pin) — bit replication of the high bits, so 0x1f -> 255 and
 * 0x00 -> 0 (full-scale expansion, not a left-shift-only truncation).
 *
 * src_pitch is in bytes and is typically larger than src_w*2 — RetroArch
 * cores commonly allocate a wider framebuffer than the active area (e.g.
 * Genesis Plus GX: pitch=1440 for 320 active px = 720px-wide bitmap).
 *
 * Same dst/src layout assumptions as bgr24_to_rgb888 / xrgb8888_to_rgb888:
 * dst contiguous width*height*3, src pitch-strided height rows.
 * ------------------------------------------------------------------------- */
static void rgb565_to_rgb888(uint8_t       *dst,
                              const uint8_t *src,
                              unsigned       dst_w,
                              unsigned       dst_h,
                              unsigned       src_w,
                              unsigned       src_h,
                              unsigned       src_pitch)
{
    /* Box-filter downscale + RGB565 → RGB888 (same algorithm as
     * bgr24_to_rgb888 / xrgb8888_to_rgb888). When src == dst dims, the box
     * is 1×1 and this is a 1:1 copy with format expansion. */
    for (unsigned dy = 0; dy < dst_h; ++dy) {
        unsigned sy0 = (unsigned)((size_t)dy * src_h / dst_h);
        unsigned sy1 = (unsigned)((size_t)(dy + 1u) * src_h / dst_h);
        if (sy1 <= sy0) sy1 = sy0 + 1u;
        if (sy1 > src_h) sy1 = src_h;

        uint8_t *dst_row = dst + (size_t)dy * dst_w * 3u;
        for (unsigned dx = 0; dx < dst_w; ++dx) {
            unsigned sx0 = (unsigned)((size_t)dx * src_w / dst_w);
            unsigned sx1 = (unsigned)((size_t)(dx + 1u) * src_w / dst_w);
            if (sx1 <= sx0) sx1 = sx0 + 1u;
            if (sx1 > src_w) sx1 = src_w;

            uint32_t r_acc = 0u, g_acc = 0u, b_acc = 0u;
            uint32_t n = 0u;
            for (unsigned sy = sy0; sy < sy1; ++sy) {
                const uint8_t *src_row = src + (size_t)sy * src_pitch;
                for (unsigned sx = sx0; sx < sx1; ++sx) {
                    /* Native LE uint16 RGB565: recomposed byte-by-byte so it
                     * never depends on any alignment of the source pitch. */
                    const uint8_t *px = src_row + 2u * sx;
                    uint32_t col = (uint32_t)px[0] | ((uint32_t)px[1] << 8);
                    uint32_t r5 = (col >> 11) & 0x1fu;
                    uint32_t g6 = (col >>  5) & 0x3fu;
                    uint32_t b5 = (col >>  0) & 0x1fu;
                    /* 5/6/5 -> 8/8/8 expansion by high-bit replication
                     * (identical to conv_rgb565_argb8888): 0x1f -> 255. */
                    r_acc += (r5 << 3) | (r5 >> 2);
                    g_acc += (g6 << 2) | (g6 >> 4);
                    b_acc += (b5 << 3) | (b5 >> 2);
                    ++n;
                }
            }
            dst_row[3u * dx + 0u] = (uint8_t)(r_acc / n);
            dst_row[3u * dx + 1u] = (uint8_t)(g_acc / n);
            dst_row[3u * dx + 2u] = (uint8_t)(b_acc / n);
        }
    }
}

/* ---------------------------------------------------------------------------
 * groovy_init_params_for -- builds the CMD_INIT (4 bytes) for ONE receiver.
 * Fixes an earlier bug: `st->audio_rate_code` used to be 0 for the WHOLE
 * session as soon as the master's GROOVY_AUDIO was "off" -- a follower
 * declared with audio on by default could then never receive CMD_AUDIO,
 * whatever its own `audio` key said. Now `st->audio_rate_code` is the
 * SESSION's frequency code (computed once from the core's native
 * frequency, regardless of GROOVY_AUDIO), and it's `rx->audio_on` -- not a
 * global setting -- that decides whether THIS receiver announces audio.
 * For the master (rx == &st->rx[0]), rx->audio_on == (st->audio_mode !=
 * GROOVY_AUDIO_OFF): the 4 bytes rendered here are therefore IDENTICAL to
 * the former manual construction. */
static void groovy_init_params_for(const groovy_state_t *st,
                                   const struct groovy_receiver *rx,
                                   gm_init_params *p)
{
    memset(p, 0, sizeof(*p));
    p->compression = (uint8_t)rx->compression;
    p->sample_rate = rx->audio_on ? st->audio_rate_code : 0u;
    p->channels    = (p->sample_rate != 0u) ? st->audio_channels : 0u;
}

/* ---------------------------------------------------------------------------
 * vtable function: groovy_new
 *
 * Called by RetroArch when the user enables the groovy record driver (e.g.
 * record_driver = "groovy" in retroarch.cfg). Allocates state, opens the
 * UDP socket via gm_init, and sends CMD_INIT.
 *
 * Returns the allocated groovy_state_t* on success, NULL on any failure.
 * RetroArch treats NULL return as "driver unavailable" and falls back.
 * ------------------------------------------------------------------------- */
/* ---------------------------------------------------------------------------
 * Announce trace. One line per CMD_INIT or CMD_SWITCHRES sent to a
 * receiver, with its EXACT outcome:
 *   envoye    -- accepted by Winsock (does NOT prove arrival);
 *   echec     -- refused; wsa = gm_last_announce_error, read by the caller
 *                RIGHT AFTER this send (the next announce on the same
 *                handle would overwrite it; one on another handle would
 *                not);
 *   not_ready -- re-announce with no geometry to carry: nothing went out,
 *                and this is not a send failure (the third mechanism).
 * t_ns = groovy_pacing_mono_ns() taken at send time: the same QPC clock as
 * the client's GMC_TRACE t_ns, so the two are comparable on the same PC.
 * VOLUME BOUNDED by construction: never called per frame. At most one
 * line per receiver at opening, one per receiver per geometry or regime
 * change, two per re-announce -- bounded by GROOVY_REANNOUNCE_BURST_MAX
 * per episode (untouched by this). The caller only logs AFTER its last
 * send: no log write ever interleaves between two datagrams.
 * ------------------------------------------------------------------------- */
enum groovy_announce_cmd  { GROOVY_ANNOUNCE_INIT = 0, GROOVY_ANNOUNCE_SWITCHRES = 1 };
enum groovy_announce_site { GROOVY_SITE_OPENING = 0, GROOVY_SITE_REANNOUNCE = 1,
                           GROOVY_SITE_SWITCHRES_ALL = 2 };
#define GROOVY_ANNOUNCE_NOT_READY 1   /* reserved rc: no geometry to carry */

static const char *groovy_role_name(int r);   /* defined further below, with the named traces */

static void groovy_announce_note(const groovy_state_t *st, struct groovy_receiver *rx,
                                enum groovy_announce_cmd cmd, enum groovy_announce_site site,
                                int rc, int wsa, uint64_t t_ns)
{
    static const char *const cmd_name[]  = { "init", "switchres" };
    static const char *const site_name[] = { "opening", "reannounce", "switchres_all" };
    const char *issue;

    if (rc == GROOVY_ANNOUNCE_NOT_READY) { issue = "not_ready"; wsa = 0; }
    else if (rc == 0)                    { issue = "sent";  rx->announces_ok++; }
    else                                 { issue = "failed";   rx->announces_failed++; }
    RARCH_LOG("[groovy-annonce] target=%s role=%s cmd=%s site=%s issue=%s wsa=%d "
              "frame=%u t_ns=%llu\n",
              rx->target, groovy_role_name(rx->role), cmd_name[cmd], site_name[site],
              issue, wsa, (unsigned)st->frame_id, (unsigned long long)t_ns);
}

/* Servo feed-forward. The 1905 ppm step between the receiver's two modes
 * is NOT servoed: it is APPLIED outright, on the step, at every
 * CMD_SWITCHRES. The servo then only works on the crystal's residual
 * drift (+/-500 ppm authority).
 *
 * WARNING: the source of the timings is NOT the modeline that goes out on
 * the wire. Outside legacy_modeset, the receiver ignores the wire
 * modeline (receiver/blit.c, blit_runtime_modeset delegates to
 * blit_regime_toggle and the timings come from the two fixed entries of
 * receiver/modelines.h), and compute_modeline_from_dims sets pclock =
 * h_total * v_total * the CORE's fps -- dividing back out would recover
 * the core's cadence, not the tube's. The RECEIVER's constants are
 * therefore copied into groovy_servo.h, and only the interlace flag comes
 * from the wire: that one IS faithful to the regime.
 *
 * Placed here (ahead of groovy_new, not just above groovy_switchres_all):
 * groovy_new calls this function right at session opening to apply the
 * initial feed-forward, and a static used ahead of its definition does
 * not compile in C -- the same compilation-ordering fix as the
 * audio_driver accessor moved elsewhere in this codebase for the same
 * reason. The two CALL SITES stay exactly as designed (in
 * groovy_switchres_all and at opening); only the DEFINITION's location
 * changes. */
static void groovy_servo_apply_ff(groovy_state_t *st, int interlace)
{
    settings_t *cfg = config_get_ptr();
    const double vrr = cfg ? (double)cfg->floats.video_refresh_rate : 0.0;
    const double cad = groovy_servo_cadence_hz(GROOVY_SERVO_PCLOCK_HZ,
                                               GROOVY_SERVO_H_TOTAL,
                                               interlace ? GROOVY_SERVO_V_TOTAL_480I
                                                         : GROOVY_SERVO_V_TOTAL_240P,
                                               interlace);
    const double ff  = groovy_servo_ff_ppm(cad, vrr);
    const double per = (cad > 0.0) ? (1.0e9 / cad) : 0.0;
    if (!(vrr > 0.0) && !st->servo_vrr_trace) {
        RARCH_WARN("[groovy-servo] video_refresh_rate unreadable (%.6f): "
                   "feed-forward at 0 ppm, term 1.0. Never a hardcoded value.\n", vrr);
        st->servo_vrr_trace = 1;
    }
    RARCH_LOG("[groovy-servo] feed-forward: mode=%s cadence=%.6fHz vrr=%.6fHz "
              "ff=%+.1fppm period=%.5fms\n",
              interlace ? "480i" : "240p", cad, vrr, ff, per * 1e-6);
    groovy_servo_set_feed_forward(&st->servo, ff, per);
}

/* A forced-setting message goes out on the PC screen AND to the log,
 * never into the image sent to the receiver. A recording driver has no
 * RETRO_ENVIRONMENT_SET_MESSAGE_EXT -- that callback is reserved for
 * libretro cores. runloop_msg_queue_push is the only channel available,
 * and it's the one gfx/video_driver.c already uses for a message of the
 * same kind. ../../runloop.h is already included by this file. */
static void groovy_log_forced(const char *msg)
{
    size_t len;
    if (!msg || msg[0] == '\0')
        return;
    len = strlen(msg);
    runloop_msg_queue_push(msg, len, 2, 360, false, NULL,
          MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);
    RARCH_LOG("[groovy] %s\n", msg);
}

/* Same channel as groovy_log_forced above (runloop_msg_queue_push, PC
 * screen AND log), but for a REFUSAL that prevents the bridge from
 * starting -- ERROR category instead of INFO, and a longer duration (600
 * frames versus 360): a message explaining why the CRT stays black must
 * stay readable longer than a simple forced-setting notice. Before this,
 * a refusal (malformed follower list) only went out via RARCH_ERR -- to
 * the log, which nobody reads while playing. */
static void groovy_log_error(const char *msg)
{
    size_t len;
    if (!msg || msg[0] == '\0')
        return;
    len = strlen(msg);
    runloop_msg_queue_push(msg, len, 2, 600, false, NULL,
          MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_ERROR);
    RARCH_ERR("[groovy] %s\n", msg);
}

static void *groovy_new(const struct record_params *params)
{
    /* Nothing from a previous session's inputs survives the opening of a
     * new one -- see groovy_input_set_clear. */
    groovy_input_set_clear();

    groovy_state_t *st = (groovy_state_t *)calloc(1u, sizeof(*st));
    if (!st) return NULL;
    /* calloc zeroes all fields. Explicit documentation of intent:
     *   st->av_cache.valid = false — no geometry cached yet for this instance
     *
     * NOTE: s_last_valid_fps is file-scope static (Bug A fix), NOT per-instance.
     * It survives groovy_free / groovy_new cycles by design — the seed from
     * the previous core's SET_SYSTEM_AV_INFO carries forward to the next core's
     * first SET_GEOMETRY-only callback. */

    char     ip[64];
    uint16_t port;
    if (!parse_receiver_config(params, ip, sizeof(ip), &port)) {
        /* Refuse to start rather than silently aiming UDP at a stale
         * default. */
        RARCH_ERR("[groovy] video_record_config missing or empty -- no "
                  "default address, the driver refuses to start.\n"
                  "[groovy] Resolve the receiver's current address by its "
                  "hardware address, e.g. `ip neigh` on a host of the same "
                  "network.\n"
                  "[groovy] Then set video_record_config = \"ip[:port]\" "
                  "in retroarch.cfg.\n");
        free(st);
        return NULL;
    }

    /* GROOVY_FOLLOWERS: the master's socket opens BEFORE parsing, so the
     * master's address -- the "follower == master" check, like the
     * provenance filter -- is the one libgm ACTUALLY targets
     * (gm_peer_addr, read via inet_addr), never a second decimal reading
     * of the same text: "192.0.2.020" targets 192.0.2.16 (octal), and an
     * older decimal reading (192.0.2.20) used to make every status from
     * the real master get rejected. gm_init sends nothing: a refused
     * entry closes this socket before a single byte goes out (no partial
     * startup, no silent fallback). shape_master fixes the shape every
     * follower must match in v1 (refused otherwise). */
    st->gm = gm_init(ip, port);
    if (!st->gm) {
        free(st);
        return NULL;
    }
    /* --- Menu / variable precedence -----------------------------------
     * The menu proposes, the variable disposes, and the program SAYS SO.
     *
     * RESOLVED ONCE, HERE. Compression is consumed right after as an
     * argument to groovy_followers_parse, then again further below for
     * st->compression_mode: resolving at each call site would give TWO
     * truths within the same session -- a follower would negotiate its
     * compression against getenv's raw value while the master used the
     * resolved one.
     *
     * No comparison against the menu's value. A variable that is set
     * wins and says so, even if it states the same thing. Full
     * transparency on what governs a session is the point.
     *
     * GROOVY_FIELD_PER_FRAME and GROOVY_FRAME_DUP do NOT appear here:
     * they stay variable-only, so their getenv reads already receive the
     * same value everywhere. Do not "fix" them. */
    {
        settings_t *cfg = config_get_ptr();
        char        msg[GROOVY_FORCED_MSG_CAP];
        const char *env;

        /* compression */
        env = getenv("GROOVY_COMPRESSION");
        if (groovy_precedence_env_wins(env)) {
            st->compression_mode = groovy_compression_mode_parse(env);
            groovy_precedence_msg(msg, sizeof msg, "groovy_compression",
                                  env, "GROOVY_COMPRESSION");
            groovy_log_forced(msg);
        } else {
            st->compression_mode = (cfg && cfg->bools.groovy_compression)
                                 ? GROOVY_COMPRESSION_LZ4 : GROOVY_COMPRESSION_OFF;
        }

        /* son */
        env = getenv("GROOVY_AUDIO");
        if (groovy_precedence_env_wins(env)) {
            st->audio_mode = groovy_audio_mode_parse(env);
            groovy_precedence_msg(msg, sizeof msg, "groovy_audio",
                                  env, "GROOVY_AUDIO");
            groovy_log_forced(msg);
        } else {
            unsigned v = cfg ? cfg->uints.groovy_audio : (unsigned)GROOVY_AUDIO_OFF;
            if (v > (unsigned)GROOVY_AUDIO_BOTH)
                v = (unsigned)GROOVY_AUDIO_OFF;   /* hand-edited key: fall back gracefully, don't crash */
            st->audio_mode = (enum groovy_audio_mode)v;
        }

        /* inputs */
        env = getenv("GROOVY_INPUT");
        if (groovy_precedence_env_wins(env)) {
            st->input_mode = groovy_input_mode_parse(env);
            groovy_precedence_msg(msg, sizeof msg, "groovy_input",
                                  env, "GROOVY_INPUT");
            groovy_log_forced(msg);
        } else {
            st->input_mode = (cfg && cfg->bools.groovy_input)
                           ? GROOVY_INPUT_ON : GROOVY_INPUT_OFF;
        }

        /* MTU -- the menu value goes through the SAME parser as the
         * variable, so there's only one clamping authority. */
        env = getenv("GROOVY_MTU");
        if (groovy_precedence_env_wins(env)) {
            st->mtu = groovy_mtu_parse(env);
            groovy_precedence_msg(msg, sizeof msg, "groovy_mtu",
                                  env, "GROOVY_MTU");
            groovy_log_forced(msg);
        } else {
            char tmp[16];
            snprintf(tmp, sizeof tmp, "%u",
                     cfg ? cfg->uints.groovy_mtu : (unsigned)GM_MTU_DEFAULT);
            st->mtu = groovy_mtu_parse(tmp);
        }

        /* followers -- copies, never borrowed. THREE-TIER PRECEDENCE, in
         * this exact order --
         *   1. GROOVY_FOLLOWERS (variable) always wins, like the five
         *      settings above;
         *   2. the groovy_followers text key of retroarch.cfg, if not
         *      empty -- the expert path, removed from the menu;
         *   3. otherwise, the four "Additional Follower N" switches and
         *      their addresses, composed by groovy_followers_menu_compose
         *      (groovy_followers_menu.h). A switch turned on with an
         *      empty address is an explicit REFUSAL -- same discipline as
         *      groovy_followers_parse further below (never a silent
         *      fallback), said on screen AND in the log. */
        st->followers_src[0] = '\0';
        env = getenv("GROOVY_FOLLOWERS");
        if (groovy_precedence_env_wins(env)) {
            strlcpy(st->followers_src, env, sizeof(st->followers_src));
            groovy_precedence_msg(msg, sizeof msg, "groovy_followers",
                                  env, "GROOVY_FOLLOWERS");
            groovy_log_forced(msg);
        } else if (cfg && cfg->arrays.groovy_followers[0]) {
            strlcpy(st->followers_src, cfg->arrays.groovy_followers,
                    sizeof(st->followers_src));
        } else if (cfg) {
            int         menu_on[GROOVY_FOLLOWERS_MENU_N];
            const char *menu_addr[GROOVY_FOLLOWERS_MENU_N];
            char        compose_err[GROOVY_FOLLOWERS_ERR_CAP];

            menu_on[0] = cfg->bools.groovy_follower_1_enable;
            menu_on[1] = cfg->bools.groovy_follower_2_enable;
            menu_on[2] = cfg->bools.groovy_follower_3_enable;
            menu_on[3] = cfg->bools.groovy_follower_4_enable;
            menu_addr[0] = cfg->arrays.groovy_follower_1_address;
            menu_addr[1] = cfg->arrays.groovy_follower_2_address;
            menu_addr[2] = cfg->arrays.groovy_follower_3_address;
            menu_addr[3] = cfg->arrays.groovy_follower_4_address;

            if (groovy_followers_menu_compose(menu_on, menu_addr,
                                              st->followers_src, sizeof(st->followers_src),
                                              compose_err, sizeof(compose_err)) != 0) {
                char refusal_msg[GROOVY_FORCED_MSG_CAP];
                snprintf(refusal_msg, sizeof refusal_msg,
                          "Additional Follower setting refused -- %s -- the "
                          "bridge refuses to start. Fix it in the menu, then "
                          "restart.",
                          compose_err);
                groovy_log_error(refusal_msg);
                groovy_input_set_clear();   /* discipline shared by every closing path */
                gm_close(st->gm);           /* nothing went out yet, gm_init sends no byte */
                free(st);
                return NULL;
            }
        }
    }

    uint32_t mip    = 0u;
    uint16_t mport  = port;
    int      mip_ok = gm_peer_addr(st->gm, &mip, &mport);
    int      shape_master =
        (groovy_field_mode_parse(getenv("GROOVY_FIELD_PER_FRAME")) == GROOVY_FIELD_PER_FRAME)
            ? GROOVY_SHAPE_FIELD : GROOVY_SHAPE_IMAGE;
    struct groovy_follower_config fcfg[GROOVY_FOLLOWERS_MAX];
    unsigned n_f = 0u;
    char     ferr[GROOVY_FOLLOWERS_ERR_CAP];
    if (groovy_followers_parse(st->followers_src[0] ? st->followers_src : NULL,
                               mip_ok ? mip : 0u, mport,
                               (int)st->compression_mode,
                               shape_master,
                               fcfg, GROOVY_FOLLOWERS_MAX, &n_f, ferr, sizeof(ferr)) != 0) {
        /* Log text partly FIXED: "GROOVY_FOLLOWERS refuse" is read
         * verbatim by test_record_groovy_followers.py -- keep that exact
         * substring even though the rest of the message is translated. */
        RARCH_ERR("[groovy] GROOVY_FOLLOWERS refuse -- %s -- the driver "
                  "refuses to start (no fallback).\n", ferr);
        /* This refusal used to speak ONLY to the log (RARCH_ERR above) --
         * nobody reads it while playing. The same refusal now ALSO goes
         * to the PC screen, without duplicating the log line already
         * written (hence no groovy_log_error here, which would log a
         * second time). */
        {
            char refusal_msg[GROOVY_FORCED_MSG_CAP];
            snprintf(refusal_msg, sizeof refusal_msg,
                      "Follower list refused -- %s -- the bridge refuses to "
                      "start. Fix it in retroarch.cfg or the menu, then "
                      "restart.",
                      ferr);
            runloop_msg_queue_push(refusal_msg, strlen(refusal_msg), 2, 600, false, NULL,
                  MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_ERROR);
        }
        groovy_input_set_clear();   /* discipline shared by every closing path */
        gm_close(st->gm);           /* nothing went out yet, gm_init sends no byte */
        free(st);
        return NULL;
    }

    /* rx[0] = the master. declared_ip/declared_port: the address libgm
     * ACTUALLY targets -- the provenance filter is therefore active for
     * it whatever the text form of video_record_config. inputs =
     * IDENTITY: the master's contribution to the input merge copies its
     * two controllers as-is. */
    st->rx[0].gm           = st->gm;
    st->rx[0].role          = GROOVY_ROLE_MASTER;
    snprintf(st->rx[0].target, sizeof(st->rx[0].target), "%s:%u", ip, (unsigned)port);
    st->rx[0].declared_ip   = mip;
    st->rx[0].declared_port = mport;
    st->rx[0].declared_ok   = mip_ok;    /* 1 as soon as gm_init has succeeded */
    st->rx[0].inputs       = GROOVY_INPUTS_IDENTITY;
    st->rx[0].audio_errors  = 0u;
    {
        /* The log says when the text doesn't read the way libgm targets
         * it (octal, hexadecimal, short form). The old "provenance filter
         * inactive" warning no longer applies. */
        uint32_t dec = 0u;
        if (!groovy_followers_ip_parse(ip, strlen(ip), &dec) || dec != mip)
            RARCH_WARN("[groovy] master address '%s' targeted by libgm as "
                       "%u.%u.%u.%u (inet_addr reading, not decimal): this is "
                       "the one the provenance filter and follower check "
                       "serve\n", ip,
                       (unsigned)(mip & 0xFFu), (unsigned)((mip >> 8) & 0xFFu),
                       (unsigned)((mip >> 16) & 0xFFu), (unsigned)((mip >> 24) & 0xFFu));
    }
    st->n_rx = 1u;

    /* Audio output setting, already resolved above: the groovy_audio key
     * of retroarch.cfg now exists, and it's the GROOVY_AUDIO variable
     * that overrides it when set (see groovy_audio.h for the parser). */

    /* Lever b6, read once here. See groovy_audio_hold.h for why this is
     * an environment variable and why the default is off on both sides
     * (deliberate -- this lever is being measured). */
    st->audio_hold_mode = groovy_audio_hold_mode_parse(getenv("GROOVY_AUDIO_HOLD"));

    /* Clock servo, read once here. Default off when the variable is
     * absent: no setting = same cadence as today, an unmodified
     * GroovyMAME keeps driving the receiver. A single on/off parser
     * (groovy_servo_mode_parse) reused for both settings -- writing a
     * second, identical parser would have added nothing. */
    st->servo_mode     = groovy_servo_mode_parse(getenv("GROOVY_SERVO"));
    st->audio_ratio_on = (groovy_servo_mode_parse(getenv("GROOVY_AUDIO_RATIO")) == GROOVY_SERVO_ON);
    /* Trace per measured ack, not throttled -- read once here, same
     * parser as the servo's other two settings. Off by default: no
     * setting = not one more byte in the log, not one more cycle in the
     * loop. */
    st->servo_trace    = (groovy_servo_mode_parse(getenv("GROOVY_SERVO_TRACE")) == GROOVY_SERVO_ON);
    groovy_servo_reset(&st->servo);
    {
        settings_t  *cfg = config_get_ptr();
        const double vrr = cfg ? (double)cfg->floats.video_refresh_rate : 0.0;

        /* Both feed-forward cadences are copied from receiver/modelines.h;
         * this startup line shows, in every session log, the values
         * REALLY in effect -- like every GROOVY_* setting. trace=%s is
         * APPENDED AT THE END: test_groovy_annonce_source.py's source
         * gates read the first segments word for word, never an
         * insertion in the middle. */
        RARCH_LOG("[groovy-servo] settings: servo=%s audio_ratio=%s vrr=%.6fHz "
                  "ff_240p=%+.2fppm (%.6fHz) ff_480i=%+.2fppm (%.6fHz) "
                  "authority=%.0fppm slope=%.0fppm/s kp=%.0f ki=%.2f setpoint=%.2ftr "
                  "trace=%s\n",
                  groovy_servo_mode_name(st->servo_mode),
                  st->audio_ratio_on ? "on" : "off",
                  vrr,
                  groovy_servo_ff_ppm(GROOVY_SERVO_CADENCE_240P_HZ, vrr), GROOVY_SERVO_CADENCE_240P_HZ,
                  groovy_servo_ff_ppm(GROOVY_SERVO_CADENCE_480I_HZ, vrr), GROOVY_SERVO_CADENCE_480I_HZ,
                  GROOVY_SERVO_AUTHORITY_PPM, GROOVY_SERVO_SLOPE_MAX_PPM_S,
                  GROOVY_SERVO_KP_PPM_FRAME, GROOVY_SERVO_KI_PPM_FRAME,
                  GROOVY_SERVO_SETPOINT_FRAMES,
                  st->servo_trace ? "on" : "off");

        /* This warning covers both ON and FF -- in 480i, in both modes,
         * the core slows down by ~1900 ppm and only the wire ratio
         * keeps audio at 44100 Hz (see also the refusal on the
         * launch-emitter.ps1 side for -Servo ff -AudioRatio off). */
        if (st->servo_mode != GROOVY_SERVO_OFF && !st->audio_ratio_on)
            RARCH_WARN("[groovy-servo] WARNING: GROOVY_SERVO=%s with GROOVY_AUDIO_RATIO=off "
                       "-- in 480i the core slows down by ~1922 ppm and only the wire ratio "
                       "keeps audio at 44100 Hz; audio drift will be WORSE than today on skewed "
                       "content. Combination allowed for measurement, never a default.\n",
                       groovy_servo_mode_name(st->servo_mode));
    }

    /* Input channel, per receiver. GROOVY_INPUT is the RUNTIME guard,
     * common to every receiver: the ported gamepad driver stays
     * registered with RetroArch no matter what, but the hookup below
     * only happens if the variable is exactly "on" (see groovy_input.h
     * for the nuance with GROOVY_AUDIO). The master is ALWAYS a
     * candidate for hookup when GROOVY_INPUT=on -- its attribution
     * (st->rx[0].inputs, already set above) is IDENTITY, never OFF,
     * unlike a follower (further below). Trace the mode in every case --
     * the log must say what's active, as it already does for audio and
     * compression. st->input_mode is already resolved above (menu or
     * variable). */
    if (st->input_mode == GROOVY_INPUT_ON) {
        if (gm_bind_inputs(st->gm, 0) == 0) {    /* 0 -> video port + 1 */
            s_groovy_input_h[0]    = st->gm;     /* visible to groovy_input_take_* */
            s_groovy_input_attr[0] = GROOVY_INPUTS_IDENTITY;
        } else {
            RARCH_WARN("[groovy-input] input channel hookup failed\n");
        }
    }
    RARCH_LOG("[groovy-input] mode=%s\n", groovy_input_mode_name(st->input_mode));

    /* Compression setting, already resolved above: the groovy_compression
     * key of retroarch.cfg now exists, and it's the GROOVY_COMPRESSION
     * variable that overrides it when set (see groovy_compression.h for
     * the only two values the wire byte can carry). */
    RARCH_LOG("[groovy] compression: mode=%s\n",
              groovy_compression_mode_name(st->compression_mode));

    /* CMD_INIT/CMD_SWITCHRES re-announce. See groovy_reannounce.h for the
     * pure decision; the SETTING is PER SESSION, but each receiver now
     * has its own state and its own budget -- st->rx[0].reannounce here
     * for the master, calloc already zeroed everything, this memset
     * documents the intent rather than relying on it silently (same
     * pattern as av_cache). The followers (rx[1..n_rx-1], below) need no
     * equivalent memset: the whole table comes straight out of calloc. */
    memset(&st->rx[0].reannounce, 0, sizeof(st->rx[0].reannounce));
    st->reannounce_enabled              = groovy_reannounce_parse(getenv("GROOVY_REANNOUNCE"));
    st->rx[0].reannounce_exhausted_logged = 0;
    RARCH_LOG("[groovy] re-announce: %s\n", st->reannounce_enabled ? "on" : "off");

    {
        const char *esp = getenv("GROOVY_REANNOUNCE_SPACING_US");
        int         spacing_invalid;

        st->reannounce_spacing_us = groovy_reannounce_spacing_parse(esp);
        /* A non-empty value that still falls back to the DEFAULT (out of
         * bounds, non-numeric, "100ms", "100 000") is distinguished here
         * from a deliberate absence of setting -- otherwise the log just
         * says "(default)" and the session default (the fork then plays
         * spacing as the alternate default) only comes to light when
         * reading the results.
         *
         * This test used to compare st->reannounce_spacing_us to 0u and
         * esp literally to "0" -- exact as long as the default was 0,
         * WRONG since the default became
         * GROOVY_REANNOUNCE_SPACING_US_DEFAULT (100000). Double
         * consequence: a bad value ("abc", "300000", "100ms") now falls
         * back to 100000, never flagged (spacing_invalid stayed 0 because
         * reannounce_spacing_us != 0u); and "00" (valid, worth 0) IS
         * different from "0" by strcmp() and was wrongly flagged.
         * Compare the PARSED value to the DEFAULT (never a hardcoded 0),
         * and confirm with a raw numeric re-read (strtoul) that the
         * original string did NOT explicitly ask for that same value --
         * this finally distinguishes "the user wrote 100000" (deliberate)
         * from "the parser fell back to 100000 for lack of anything
         * better" (a bug). */
        spacing_invalid = (esp && esp[0]
                        && st->reannounce_spacing_us == GROOVY_REANNOUNCE_SPACING_US_DEFAULT
                        && strtoul(esp, NULL, 10) != GROOVY_REANNOUNCE_SPACING_US_DEFAULT);
        RARCH_LOG("[groovy] re-announce: spacing init->switchres=%u us%s%s%s\n",
                  st->reannounce_spacing_us,
                  (esp && esp[0]) ? " (GROOVY_REANNOUNCE_SPACING_US)" : " (default)",
                  (st->reannounce_spacing_us == 0u) ? ", immediate send" : ", switchres deferred until a later frame",
                  spacing_invalid ? ", invalid value ignored" : "");
    }

    /* Unchanged frame skip. The driver decides NOTHING: all the logic --
     * byte-for-byte comparison, full-frame cadence, header shape -- lives
     * in libgm. Here, only the switch. Without the variable, libgm stays
     * inert and the wire is bit-identical to the previous behavior. */
    st->frame_dup_mode = groovy_frame_dup_mode_parse(getenv("GROOVY_FRAME_DUP"));
    /* Dup-proofing BY SETTING, not by sensor. A frame_dup does not toggle
     * the tube: the receiver then only advances the echo, never ack_frame
     * (receiver/net.c:1144-1157), and the servo reads ack_frame
     * (groovy_servo.h, branch (8)). On a half-static scene, 54% of frames
     * went out as dup during an earlier measurement session, and the
     * polled ring could return an OLD toggle timestamp. So dup is cut
     * towards the MASTER, and only towards it; the followers keep
     * GROOVY_FRAME_DUP (follower site, below). st->frame_dup_mode KEEPS
     * the value READ (it drives the followers and gets printed) -- the
     * != 0 fallback must no longer overwrite it: a refusal from libgm on
     * the master says nothing about the followers. */
    if (gm_set_frame_dup(st->gm, 0) != 0) {
        RARCH_WARN("[groovy] frame_dup refused by libgm for the master (forced off); "
                    "the master session stays off\n");
    }
    RARCH_LOG("[groovy] frame_dup: mode=%s (master: forced off; followers: %s)\n",
              groovy_frame_dup_mode_name(st->frame_dup_mode),
              groovy_frame_dup_mode_name(st->frame_dup_mode));

    /* Lever b6: this line is UNCONDITIONAL and that's deliberate -- it's
     * the proof, in every session's log, of the setting REALLY in
     * effect, same discipline as [groovy-servo] settings:. It costs one
     * line per session, not one per frame. Placed AFTER reading
     * GROOVY_FRAME_DUP so that st->frame_dup_mode is already set (the
     * warning below depends on it). */
    RARCH_LOG("[groovy-audio-hold] audio_hold=%s (a frame's audio goes out stuck to "
              "the next frame's video burst; cost: one frame of audio latency, and the "
              "last held block never goes out)\n",
              groovy_audio_hold_mode_name(st->audio_hold_mode));
    if (st->audio_hold_mode == GROOVY_AUDIO_HOLD_ON
        && st->frame_dup_mode == GROOVY_FRAME_DUP_ON)
        RARCH_WARN("[groovy-audio-hold] WARNING: audio_hold=on with frame_dup=on -- "
                   "a skipped frame only emits 9 bytes, so the audio would then stick to an "
                   "EMPTY burst. This lever is measured with -FrameDup off towards the "
                   "follower. Combination allowed, never a default.\n");

    /* Lever 2. In 480i, a single half-frame per rendered frame instead of
     * two (GroovyMAME path); without the variable, the fork keeps the
     * current fld0+fld1 path, bit-identical (guarded by
     * test_pcap_diff_groovy_baseline.py). */
    st->field_mode = groovy_field_mode_parse(getenv("GROOVY_FIELD_PER_FRAME"));
    RARCH_LOG("[groovy] 480i: %s\n",
              (st->field_mode == GROOVY_FIELD_PER_FRAME)
                  ? "one field per frame (0x06 half-height, GroovyMAME path)"
                  : "two fields per frame (0x07, prior behavior)");

    /* Payload datagram size. A peer behind WireGuard cannot receive 1472
     * bytes without IP fragmentation; see groovy_mtu.h for the
     * measurement and the bounds. st->mtu is already resolved above
     * (menu or variable) -- the groovy_mtu key of retroarch.cfg goes
     * through the SAME parser as the variable, a single clamping
     * authority. */
    if (gm_set_mtu(st->gm, st->mtu) != 0) {
        /* Cannot trigger today: groovy_mtu_parse and gm_set_mtu read the
         * SAME constants from gm.h. The guard is here so that a future
         * widening on only one side would be LOUD rather than silent. */
        RARCH_WARN("[groovy] mtu=%u refused by libgm; the session keeps %u\n",
                   st->mtu, (unsigned)GM_MTU_DEFAULT);
        st->mtu = (unsigned)GM_MTU_DEFAULT;
    }
    RARCH_LOG("[groovy] mtu: %u bytes per payload datagram%s\n",
              st->mtu,
              (st->mtu == (unsigned)GM_MTU_DEFAULT) ? " (default)" : " (GROOVY_MTU)");

    /* Padded-session mode: the MASTER stays classic, and that must be
     * VISIBLE in the code. calloc() would already give 0; this line is
     * here so a reader sees that the choice is a choice -- not an
     * oversight.
     *
     * How a master could get it, if ever needed (the "Mac alone, as
     * master, over Wi-Fi" case): an environment variable GROOVY_PAD
     * (off|on, default off), parsed by a groovy_pad.h modeled on
     * groovy_compression.h, read once here next to st->compression_mode,
     * and passed to gm_set_padding(st->gm, ...). Three lines and a
     * header. Not needed today: this design stops here. */
    (void)gm_set_padding(st->gm, 0);

    /* Supersampling box setting, read once here. See groovy_downsample.h
     * for why this is an environment variable. */
    st->downsample_n = groovy_downsample_factor_parse(getenv("GROOVY_DOWNSAMPLE"));
    RARCH_LOG("[groovy] downsample: factor=%s\n",
              groovy_downsample_factor_name(st->downsample_n));

    /* Diagnostic hook: deliberately skip sending one frame in N, to prove
     * on the bench that a missing blit is tolerated. No effect when
     * GROOVY_SKIP_FRAME_EVERY is absent. */
    st->skip_every = groovy_skip_every_parse(getenv("GROOVY_SKIP_FRAME_EVERY"));
    RARCH_LOG("[groovy] skip: one frame in %u%s\n", st->skip_every,
              (st->skip_every == 0u) ? " (off)" : " (GROOVY_SKIP_FRAME_EVERY)");

    /* Channels: record_driver.c always sets 2, but this doesn't assume it. */
    unsigned ch = (params && params->channels > 0u) ? params->channels : 2u;
    if (ch > 2u) ch = 2u;
    st->audio_channels = (uint8_t)ch;

    /* Frequency code, and resampler only if the core's native frequency
     * isn't in the protocol's enumeration. */
    double   native_hz = (params && params->samplerate > 0.0)
                       ? params->samplerate : 0.0;
    uint8_t  code      = groovy_audio_rate_code(native_hz);
    unsigned wire_hz   = groovy_audio_rate_hz(code);
    /* The "GROOVY_AUDIO=off on the master -> code=0 for the whole
     * session" short-circuit is gone from here. st->audio_rate_code is
     * now the SESSION's wire code, computed regardless of GROOVY_AUDIO --
     * it's groovy_init_params_for (rx->audio_on) that decides, PER
     * RECEIVER, whether this code is announced or replaced by 0. For the
     * master alone, the resulting 4 bytes of CMD_INIT don't change (see
     * the comment on groovy_init_params_for). */
    st->audio_rate_code = code;
    groovy_resample_reset(&st->audio_rs,
                          (unsigned)(native_hz + 0.5), wire_hz);

    RARCH_LOG("[groovy] audio: mode=%s native=%.0f Hz -> wire=%u Hz "
              "(code %u, %u channels)%s\n",
              groovy_audio_mode_name(st->audio_mode), native_hz,
              wire_hz, (unsigned)code, (unsigned)st->audio_channels,
              (st->audio_rs.src_hz != st->audio_rs.dst_hz)
                  ? " [resampled]" : "");

    /* rx[0] (the master): compression/mtu/audio_on now known -- set
     * here, right before building its CMD_INIT. */
    st->rx[0].compression = (int)st->compression_mode;
    st->rx[0].mtu         = st->mtu;
    st->rx[0].pad         = 0;   /* the master stays classic */
    st->rx[0].audio_on    = (st->audio_mode != GROOVY_AUDIO_OFF);

    /* Send CMD_INIT: compression now carries the GROOVY_COMPRESSION
     * setting (replacing an earlier hardcoded 0); the two audio bytes
     * carry the frequency and the channels. Built by
     * groovy_init_params_for from rx[0]: the same 4 bytes as before this
     * design. */
    gm_init_params init_p;
    groovy_init_params_for(st, &st->rx[0], &init_p);

    const int      rc_init_m  = gm_send_init(st->gm, &init_p);
    const int      wsa_init_m = gm_last_announce_error(st->gm);
    groovy_announce_note(st, &st->rx[0], GROOVY_ANNOUNCE_INIT, GROOVY_SITE_OPENING,
                        rc_init_m, wsa_init_m, groovy_pacing_mono_ns());
    if (rc_init_m != 0) {
        /* s_groovy_input_h[0] may already point to st->gm (input channel
         * hookup above) -- clear it BEFORE gm_close, otherwise the next
         * gamepad poll reads a freed handle. */
        groovy_input_set_clear();
        gm_close(st->gm);
        free(st);
        return NULL;
    }

    /* Followers: one gm_handle per GROOVY_FOLLOWERS entry, opened AFTER
     * the master's CMD_INIT -- documented ordering. A system failure to
     * open (gm_init returns NULL) refuses the whole startup, cleanly
     * closing whatever was already opened; a CMD_INIT refusal towards a
     * follower, on the other hand, is only a warning -- the re-announce
     * will replay it. */
    RARCH_LOG("[groovy] master: target=%s\n", st->rx[0].target);
    if (n_f == 0u) {
        RARCH_LOG("[groovy] followers: none\n");
    } else {
        for (unsigned i = 0u; i < n_f; i++) {
            struct groovy_receiver *rx = &st->rx[1u + i];

            rx->gm = gm_init(fcfg[i].ip_text, fcfg[i].port);
            if (!rx->gm) {
                RARCH_ERR("[groovy] failed to open follower %s:%u\n",
                          fcfg[i].ip_text, (unsigned)fcfg[i].port);
                /* Index 0 (the master) and indices 1..i (followers hooked
                 * up in previous iterations) may already point to the
                 * handles closed below -- clear the whole set BEFORE any
                 * gm_close. */
                groovy_input_set_clear();
                for (unsigned j = 1u; j < 1u + i; j++)
                    gm_close(st->rx[j].gm);
                gm_close(st->gm);
                free(st);
                return NULL;
            }

            if (gm_set_mtu(rx->gm, fcfg[i].mtu) != 0) {
                /* Loud fallback, same pattern as the master above. */
                RARCH_WARN("[groovy] mtu=%u refused by libgm for follower "
                           "%s:%u; the session keeps %u\n",
                           fcfg[i].mtu, fcfg[i].ip_text, (unsigned)fcfg[i].port,
                           (unsigned)GM_MTU_DEFAULT);
                fcfg[i].mtu = (unsigned)GM_MTU_DEFAULT;
            }
            (void)gm_set_frame_dup(rx->gm,
                                   st->frame_dup_mode == GROOVY_FRAME_DUP_ON ? 1 : 0);
            (void)gm_set_padding(rx->gm, fcfg[i].pad);

            rx->role          = GROOVY_ROLE_FOLLOWER;
            /* %.15s (not %s): ip_text is bounded to 16 bytes by
             * construction (groovy_followers_ip_parse returns at most
             * "255.255.255.255"), but gcc can't deduce that through the
             * runtime indexing fcfg[i] -- the explicit width silences the
             * -Wformat-truncation false positive without changing the
             * actual result. */
            snprintf(rx->target, sizeof(rx->target), "%.15s:%u",
                      fcfg[i].ip_text, (unsigned)fcfg[i].port);
            rx->declared_ip   = fcfg[i].ip;
            rx->declared_port = fcfg[i].port;
            rx->declared_ok   = 1;   /* an accepted follower always has a usable address */
            rx->compression   = fcfg[i].compression;
            rx->mtu           = fcfg[i].mtu;
            rx->pad           = fcfg[i].pad;
            rx->audio_on      = fcfg[i].audio;
            rx->inputs       = fcfg[i].inputs;
            rx->audio_errors  = 0u;

            {
                gm_init_params follower_init_params;
                int            follower_init_rc, follower_init_err;
                groovy_init_params_for(st, rx, &follower_init_params);
                follower_init_rc  = gm_send_init(rx->gm, &follower_init_params);
                follower_init_err = gm_last_announce_error(rx->gm);
                groovy_announce_note(st, rx, GROOVY_ANNOUNCE_INIT, GROOVY_SITE_OPENING,
                                    follower_init_rc, follower_init_err, groovy_pacing_mono_ns());
                if (follower_init_rc != 0)
                    RARCH_WARN("[groovy] CMD_INIT refused towards %s -- the "
                               "re-announce will replay it\n", rx->target);
            }

            RARCH_LOG("[groovy] follower %u: target=%s mtu=%u compression=%s "
                      "audio=%s entrees=%s pad=%s\n",
                      i + 1u, rx->target, rx->mtu,
                      groovy_compression_mode_name((enum groovy_compression_mode)rx->compression),
                      rx->audio_on ? "on" : "off",
                      (rx->inputs == GROOVY_INPUTS_P1) ? "p1" :
                      (rx->inputs == GROOVY_INPUTS_P2) ? "p2" : "off",
                      rx->pad ? "on" : "off");
            if (rx->pad) {
                /* Forbidden on MiSTer: the protocol has no way to detect
                 * what's at the other end of the wire -- the warning is
                 * the most honest thing we can do. */
                RARCH_WARN("[groovy] padded mode active towards %s; this mode "
                           "exists ONLY for a crt-bridge receiver or a "
                           "gmclient client, NEVER for a MiSTer FPGA\n", rx->target);
            }

            /* Per-receiver input channel: this follower is only a
             * candidate for hookup if GROOVY_INPUT=on AND its `inputs`
             * key isn't `off` -- inputs=off opens NO socket, not even to
             * send the hookup byte. The index into the s_groovy_input_h[]
             * set is st->rx[]'s, 1 + i here -- never fcfg[]'s bare index
             * i, which doesn't count the master. */
            if (st->input_mode == GROOVY_INPUT_ON && rx->inputs != GROOVY_INPUTS_OFF) {
                int ok_bind = (gm_bind_inputs(rx->gm, 0) == 0);
                if (ok_bind) {
                    s_groovy_input_h[1u + i]    = rx->gm;
                    s_groovy_input_attr[1u + i] = rx->inputs;
                    /* Named for the transition trace. */
                    snprintf(s_groovy_input_target[1u + i], sizeof(s_groovy_input_target[0]),
                             "%s", rx->target);
                } else {
                    RARCH_WARN("[groovy-input] hookup failed towards %s\n", rx->target);
                }
                RARCH_LOG("[groovy-input] follower %s: entrees=%s (%s)\n",
                          rx->target,
                          (rx->inputs == GROOVY_INPUTS_P1) ? "p1" : "p2",
                          ok_bind ? "hooked" : "not hooked");
            } else if (st->input_mode == GROOVY_INPUT_ON) {
                RARCH_LOG("[groovy-input] follower %s: entrees=off (not hooked)\n", rx->target);
            }
        }
        st->n_rx = 1u + n_f;

        /* In "one field per frame" mode, the field sent to EVERYONE is
         * chosen from the MASTER's FLAG_VGA_F1 alone
         * (groovy_field_for_frame on st->rx[0].ack_state). No effect for
         * a buffered software client; wrong for a tube follower (another
         * crt-bridge-rcv), whose scan parity is independent -- fields
         * would come out reversed. Known limitation, stated in the log
         * right at opening rather than discovered by eye. */
        if (st->field_mode == GROOVY_FIELD_PER_FRAME)
            RARCH_WARN("[groovy] GROOVY_FIELD_PER_FRAME active with %u follower(s): the field "
                       "sent to everyone follows the MASTER's parity -- a tube follower "
                       "(crt-bridge-rcv) would receive reversed fields; known limitation\n", n_f);
    }

    /* CMD_SWITCHRES is NOT sent here. Rationale:
     *   - Option G daemon forges modelines from wire data at runtime, so a
     *     prior CMD_SWITCHRES is not required before the first CMD_BLIT_VSYNC.
     *   - The correct geometry is only known after the core runs its first
     *     retro_run() and fires SET_SYSTEM_AV_INFO. Sending a placeholder
     *     here would cause the daemon to forge a garbage modeline via Option G.
     *   - Plan 03-06 adds push_av_info to the vtable; that hook sends
     *     CMD_SWITCHRES from the actual geometry callback. */

    /* Cache the input pixel format for groovy_push_video dispatch (Bug D fix). */
    st->pix_fmt = params ? params->pix_fmt : FFEMU_PIX_BGR24;

    /* Seed the fps cache from record_params (quick-260712-2b5). Some HW cores
     * (SwanStation GL, HIL-observed) NEVER fire SET_SYSTEM_AV_INFO /
     * SET_GEOMETRY at runtime, so push_av_info never runs and
     * s_last_valid_fps stays 0 — push_video then soft-skips every frame and
     * nothing reaches the wire. record_params.fps is av_info->timing.fps
     * captured by recording_init: same source of truth, available for every
     * core. push_av_info still overrides with fresher values when it fires
     * (Beetle path unchanged). */
    if (params && params->fps > 0.0)
        s_last_valid_fps = params->fps;

    /* Regime state init — matches daemon's boot default. */
    st->current_mode = GROOVY_MODE_240P_SUPER_RES;
    st->pending_mode = GROOVY_MODE_240P_SUPER_RES;
    st->mode_streak  = 0u;
    groovy_servo_apply_ff(st, 0);   /* initial feed-forward, 240p */

    /* Diagnostic: log all record_params dims to understand the FB pipeline.
     * out_width/out_height = desired output resolution, fb_width/fb_height =
     * source framebuffer (what push_video receives in vid->width/height). */
    if (params) {
        RARCH_LOG("[groovy] record_params: out=%ux%u fb=%ux%u pix_fmt=%u "
                  "scale_factor=%u gpu_record=%d\n",
                  params->out_width, params->out_height,
                  params->fb_width, params->fb_height,
                  (unsigned)params->pix_fmt,
                  params->video_record_scale_factor,
                  (int)params->video_gpu_record);
    }

    st->inited = true;
    return st;
}

/* ---------------------------------------------------------------------------
 * vtable function: groovy_free
 *
 * Called by RetroArch when the record session ends. This comment used to
 * say gm_close sends CMD_CLOSE "internally". It never has -- gm_close
 * (libgm/src/gm.c) stops the heartbeat, closes the input channel, closes
 * the socket and frees the handle; it contains no call to gm_send_close.
 * CMD_CLOSE is sent by groovy_finalize's explicit gm_send_close() calls
 * below, on the wire form (classic or padded) already fixed on that
 * handle by gm_set_padding at open. groovy_free only releases local
 * resources.
 * ------------------------------------------------------------------------- */
static void groovy_free(void *data)
{
    groovy_state_t *st = (groovy_state_t *)data;
    if (!st) return;

    /* Reset to 1.0 so RetroArch gets its normal pace back at session close.
     * Without this, a frozen term would survive the driver. */
    audio_driver_set_groovy_term(1.0);

    /* Input channel (per receiver): the WHOLE file-scope set is cleared to
     * NULL BEFORE any gm_close_inputs, and gm_close_inputs for EACH receiver
     * BEFORE its video session is closed (gm_close below) -- in that order,
     * otherwise an in-flight driver call during teardown would touch an
     * already-closed socket, now across the whole set rather than just the
     * former single handle. gm_close_inputs is a no-op if the channel was
     * never opened (input_sock < 0) -- so it has no effect on a receiver
     * that never had inputs (inputs=off, or GROOVY_INPUT=off). */
    groovy_input_set_clear();   /* h, attr and acc, in one gesture */
    for (unsigned i = 0u; i < st->n_rx; i++)
        if (st->rx[i].gm)
            gm_close_inputs(st->rx[i].gm);

    /* Followers: closed BEFORE the master, never the other way around --
     * rx[0].gm == st->gm, so closing it here too, on top of the
     * gm_close(st->gm) that follows, would close the same handle twice. */
    for (unsigned i = 1u; i < st->n_rx; i++)
        if (st->rx[i].gm)
            gm_close(st->rx[i].gm);

    if (st->gm)
        gm_close(st->gm);   /* gm_close does NOT send CMD_CLOSE (see
                              * libgm/src/gm.c) -- groovy_finalize already did
                              * that earlier in the call sequence; gm_close
                              * here only frees the socket and the handle's
                              * memory. */

    free(st->swap_buf);
    free(st->down_buf);
    free(st->audio_buf);
    /* Lever b6: the calloc in groovy_new already zeroed the structure, so no
     * explicit initialization is needed at open time -- only this release,
     * here, is required. */
    groovy_audio_hold_release(&st->audio_hold);
    free(st);
}

/* ---------------------------------------------------------------------------
 * Input channel -- the two functions offered to ported drivers. Declared in
 * record_groovy.h. Neither one tells the caller anything about sockets or
 * protocol: they return an already-decoded structure, or -1.
 *
 * What k measures, and why this computation. The receiver timestamps every
 * datagram with ITS OWN scan counter (ack_frame). That same value comes back
 * sixty times a second in the VIDEO channel's acks (gm_status.frame, cached
 * by gm_recv_status and exposed by the dedicated accessor below). The two
 * timestamps -- the one set on the input datagram, the one from the last
 * video ack -- are therefore on the SAME clock, the receiver's, and their
 * difference is exactly the segment described above: the wire plus polling,
 * expressed in 16.7 ms frames. This is NOT the button-to-photon chain --
 * this measurement is honest about what it covers, and silent about the
 * rest.
 *
 * The k measurement targets the joystick (the device of the latency
 * criterion and the reference bench test); take_ps2 shares the hookup and
 * the null guard but does not double the instrumentation -- a single
 * measurement point for a single channel, keyboard/mouse application-level
 * validation being deferred separately. */
/* ---------------------------------------------------------------------------
 * Bench diagnostic history: "pad and keyboard both drop inputs" was traced
 * to record_groovy owning the input channel socket, with ported drivers
 * meant to ONLY consume it via take_joy/take_ps2 -- never two independent
 * drains of the same stream in the same RetroArch frame. Before the fix,
 * take_joy AND take_ps2 each called gm_poll_inputs(): two drains per frame
 * once a second consumer (mister_input.c) joined the first
 * (mister_joypad.c). Code review: the drain isn't discriminating by
 * datagram type (it handles joystick AND keyboard/mouse in the SAME loop,
 * regardless of caller), so no net cross-theft was found on inspection --
 * but the double drain was still a layering violation and a redundant
 * syscall every frame, and dropped inputs were observed on both pad and
 * keyboard that hadn't existed before. Fixed conservatively: ONE drain per
 * frame, done by take_joy.
 *
 * take_ps2 no longer drains -- it relies on take_joy having already drained
 * the SAME RetroArch frame. This guarantee holds through the FIXED order of
 * input_driver_poll() (input/input_driver.c, upstream, unmodified):
 * joypad->poll() is ALWAYS called before input->poll(), and
 * input_joypad_driver = "mister" is locked for this whole project (see the
 * patch README) -- mister_joypad_poll() (hence take_joy) therefore always
 * runs first, every frame, with no known exception in this configuration.
 * Should that lock ever be lifted (a different gamepad driver, or
 * mister_input active without mister_joypad), take_ps2 would stop
 * receiving fresh state -- worth revisiting if the locked configuration
 * ever changes. */
/* take_joy drains ALL handles in the s_groovy_input_h[] set -- one drain per
 * HANDLE, still always exactly once per RetroArch frame (same guarantee as
 * before, now extended from "the one handle" to "every handle in the set").
 * Each hooked receiver's state is then merged by groovy_input_fold_joy
 * according to that receiver's attribution (s_groovy_input_attr[i]) -- the
 * master in IDENTITY, a follower in P1/P2. The result's frame/order are
 * those of the FIRST state obtained (the master's, if it has one; rx[0] is
 * always polled first).
 *
 * What this returns is decided by groovy_input_take_joy_rc -- -1 with no
 * channel hooked at all; otherwise 0, with an EMPTY merge -- so everything
 * released -- once no source is alive anymore. */
int groovy_input_take_joy(gm_joy_inputs *out)
{
    if (!out) return -1;

    gm_joy_inputs acc;
    memset(&acc, 0, sizeof(acc));
    int have_any = 0;
    int any_hooked = 0;   /* at least one channel hooked */
    const uint64_t now_ns = groovy_pacing_mono_ns();   /* one read per call */

    for (unsigned i = 0u; i < GROOVY_RX_MAX; i++) {
        gm_handle *h = s_groovy_input_h[i];
        if (!h) continue;
        any_hooked = 1;

        int n = gm_poll_inputs(h);           /* ONE drain per handle and per frame */
        if (n > 0) {
            s_groovy_input_acc[i]    += (uint32_t)n;
            s_groovy_input_last_ns[i] = now_ns;   /* last ACCEPTED datagram */
        }

        gm_joy_inputs one;
        if (gm_get_joy_inputs(h, &one) != 0) continue;   /* nothing fresh for THIS receiver */

        /* A follower's gamepad that has been silent for more than 500 ms
         * stops counting. Without this filter, libgm would keep returning
         * its last state forever, and a button held at the moment it
         * disappeared would stay pressed for the player at the tube. The
         * master (i == 0) is never aged out. Only the gamepad is aged out:
         * by the same decision, a follower's keyboard and mouse are left
         * out of the merge. */
        if (!groovy_input_is_live(i, now_ns, s_groovy_input_last_ns[i])) {
            /* The transition is logged, one line per "alive" -> "silent"
             * crossing, never one per poll. */
            if (!s_groovy_input_mute_logged[i]) {
                s_groovy_input_mute_logged[i] = 1;
                RARCH_LOG("[groovy-input] follower %s silent for over 500 ms: "
                          "its gamepad no longer counts\n", s_groovy_input_target[i]);
            }
            continue;
        }
        s_groovy_input_mute_logged[i] = 0;   /* alive: the next transition will be traced */

        if (i == 0u && one.frame != s_last_joy_frame) {  /* k measurement, master only */
            /* The master's last ACCEPTED status, plus gm_last_ack_frame(h) --
             * which reads the last status RECEIVED, spoofed ones included. */
            uint32_t ack = s_groovy_master_ack_frame;
            unsigned k   = (ack >= one.frame) ? (unsigned)(ack - one.frame) : 0u;
            groovy_input_lat_record(&s_input_lat, k);
            s_last_joy_frame = one.frame;
        }

        if (!have_any) {
            acc.frame = one.frame;
            acc.order = one.order;
            have_any  = 1;
        }
        groovy_input_fold_joy(&acc, &one, s_groovy_input_attr[i]);
    }

    /* -1 only with no channel hooked at all. With a channel hooked but no
     * source alive -- the only follower player went quiet, the master never
     * sent anything (no-master fallback) -- the EMPTY merge goes out:
     * everything released. Returning -1 here used to leave the driver on
     * its last state, including a vanished follower's held button. The
     * trace follows the same branch, once per transition, and only if a
     * source had spoken before: no "released" is traced at opening. */
    const int rc = groovy_input_take_joy_rc(any_hooked, have_any);
    if (rc != 0)
        return rc;
    if (have_any) {
        s_groovy_input_had_source = 1;
    } else if (s_groovy_input_had_source) {
        s_groovy_input_had_source = 0;
        RARCH_LOG("[groovy-input] no more live input source: gamepads released\n");
    }
    *out = acc;
    return 0;
}

int groovy_input_take_ps2(gm_ps2_inputs *out)
{
    if (!out) return -1;
    /* No gm_poll_inputs() here, for any handle -- see the comment above
     * groovy_input_take_joy: it has already drained ALL handles this same
     * RetroArch frame. */
    gm_ps2_inputs acc;
    memset(&acc, 0, sizeof(acc));
    int have_any = 0;

    for (unsigned i = 0u; i < GROOVY_RX_MAX; i++) {
        gm_handle *h = s_groovy_input_h[i];
        if (!h) continue;

        gm_ps2_inputs one;
        if (gm_get_ps2_inputs(h, &one) != 0) continue;

        /* The MASTER's keyboard only. A follower's state is read above,
         * then discarded -- it contributes neither keys nor frame/order.
         * The rule now lives only in groovy_input_fold_ps2, which returns 0
         * for a follower: that is what the test harness exercises. */
        if (!groovy_input_fold_ps2(&acc, &one, s_groovy_input_attr[i])) continue;

        if (!have_any) {
            acc.frame = one.frame;
            acc.order = one.order;
            have_any  = 1;
        }
    }

    if (!have_any) return -1;
    *out = acc;
    return 0;
}

int groovy_input_take_mouse(int *dx, int *dy, int *dz)
{
    /* No gm_poll_inputs() here either -- same dependency on take_joy's
     * drain as take_ps2 (see above). Deltas are already decoded and
     * accumulated by libgm at drain time; consume-and-reset, PER HANDLE.
     * Only the MASTER's are summed. Before this, a follower's P1 and P2
     * deltas were summed too, and drove the game's mouse as soon as
     * input_driver = "mister". */
    int have_any = 0;
    int sx = 0, sy = 0, sz = 0;

    for (unsigned i = 0u; i < GROOVY_RX_MAX; i++) {
        gm_handle *h = s_groovy_input_h[i];
        if (!h) continue;

        int x = 0, y = 0, z = 0;
        if (gm_get_mouse_deltas(h, &x, &y, &z) != 0) continue;
        /* A follower's deltas have just been consumed -- so they don't pile
         * up inside libgm -- and are discarded by groovy_input_fold_mouse,
         * which alone carries the "master's mouse only" rule. */
        if (groovy_input_fold_mouse(&sx, &sy, &sz, x, y, z, s_groovy_input_attr[i]))
            have_any = 1;
    }

    if (!have_any) return -1;
    if (dx) *dx = sx;
    if (dy) *dy = sy;
    if (dz) *dz = sz;
    return 0;
}

/* Re-announce counters. File scope -- AGGREGATED across all receivers: the
 * budget and the state are now per receiver (rx->reannounce), but
 * [groovy-emit] stays a single line per session, so these counters remain
 * totals, not a per-receiver breakdown. s_reannounce_sent counts the
 * re-announces whose CMD_SWITCHRES was ACCEPTED by Winsock -- not merely
 * attempted, as before; s_reannounce_failure counts those where at least
 * one send (CMD_INIT or CMD_SWITCHRES) failed. The detail, send by send and
 * including the error code, is in [groovy-annonce]. */
static unsigned s_reannounce_sent      = 0u;
static unsigned s_reannounce_init_only = 0u;
static unsigned s_reannounce_hello     = 0u;
static unsigned s_reannounce_failure     = 0u;
/* Number of resets of the input sequence guard -- one per re-announce when
 * the input channel is hooked. Serves as a BINARY-FRESHNESS witness at the
 * bench: its presence in [groovy-emit] proves that the loaded retroarch.exe
 * carries this fix (the stale-DLL trap, see the LZ4 compression note in the
 * project's build guidance). */
static unsigned s_reannounce_input_reset = 0u;

/* ---------------------------------------------------------------------------
 * groovy_role_name -- for traces named per receiver.
 * ------------------------------------------------------------------------- */
static const char *groovy_role_name(int r)
{
    return (r == GROOVY_ROLE_MASTER) ? "master" : "follower";
}

/* ---------------------------------------------------------------------------
 * groovy_drain_statuses -- ONE drain of all queued statuses for THIS
 * receiver. Sets its pacing state (rx->ack_state), filters the source (IP
 * AND port), names the hookup (ack_back), and evaluates its OWN
 * re-announce decision (*want is OR-ed to 1 if THIS receiver must be
 * re-announced -- never reset to 0 here: the caller sets it to 0 before
 * each call).
 *
 * declared_ok == 0 (address not yet known, see groovy_new) disables the
 * filter for this receiver -- behavior documented at the site where the
 * flag is declared, alongside the declared_ip == 0 sentinel.
 *
 * backlog counts EVERY status read this round -- accepted AND rejected.
 * Necessary, not just a style choice: without this count on the rejected
 * branch, a stream of spoofed statuses would never move `backlog` toward
 * the 256 cap, and the `while` loop would spin forever against an attacker
 * who never stops sending -- starving the runloop thread. The cap therefore
 * protects BOTH branches, not just the normal case. */
static void groovy_drain_statuses(groovy_state_t *st, struct groovy_receiver *rx,
                                  uint64_t now_ms, int *want)
{
    gm_status extra;
    unsigned  backlog = 0u;

    /* The cap is tested BEFORE the read. In the reverse order, the 257th
     * status was pulled off the socket and then dropped without being
     * applied -- and it may have been the master's freshest one. */
    while (backlog < 256u && gm_recv_status(rx->gm, &extra, 0) == 0) {
        uint32_t pip  = 0u;
        uint16_t ppt  = 0u;
        int      have = gm_last_status_peer(rx->gm, &pip, &ppt);

        if (rx->declared_ok
            && (!have || !groovy_status_from_declared(pip, ppt, rx->declared_ip, rx->declared_port))) {
            rx->rejects++;
            backlog++;
            continue;
        }

        rx->ack_state.last_vcount      = extra.vcount;
        rx->ack_state.last_ack_mono_ns = groovy_pacing_mono_ns();
        rx->ack_state.ack_lost_logged  = 0;
        rx->ack_state.has_any_ack      = 1;
        rx->ack_state.last_frame_echo  = extra.frame_echo;
        rx->ack_state.last_frame       = extra.frame;
        rx->ack_state.last_flags       = extra.flags;
        if (rx->role == GROOVY_ROLE_MASTER)
            s_groovy_master_ack_frame = extra.frame;   /* only an ACCEPTED status feeds k */

        if (groovy_ack_link_on_status(&rx->link, extra.frame_echo))
            RARCH_LOG("[groovy_pacing] ack_back target=%s role=%s\n",
                      rx->target, groovy_role_name(rx->role));

        if (st->reannounce_enabled && have) {
            *want |= groovy_reannounce_should_fire(&rx->reannounce, pip, extra.frame,
                                                   extra.frame_echo, now_ms);
            if (rx->reannounce.armed == 0u)
                rx->reannounce_exhausted_logged = 0;          /* re-arm follows a resync ONLY -- see the note below */
            if (extra.frame_echo == 0u
                && rx->reannounce.fired >= GROOVY_REANNOUNCE_BURST_MAX
                && !rx->reannounce_exhausted_logged) {
                /* This trace fires ONCE PER EPISODE (re-armed by a resync,
                 * armed == 0 above -- never by budget replenishment alone,
                 * which doesn't touch armed), and it no longer promises a
                 * final silence: the chosen shape keeps sending a
                 * re-announce at most every GROOVY_REANNOUNCE_REPLENISH_MS,
                 * without end, for as long as this peer stays blind.
                 *
                 * "Re-armed by a resync or a new peer" turned out to be
                 * wrong. After branch (a) (a status arriving from a
                 * DIFFERENT address than the one on file) as after branch
                 * (b) (a counter going backwards at the SAME address, a
                 * restarted client), armed is already 1 before this test:
                 * neither a new peer nor a restarted client re-arms this
                 * trace; only a resync (branch 0, frame_echo != 0) does. If
                 * the PREVIOUS peer had already exhausted its budget
                 * (logged=1) and the next one never resyncs, its own
                 * exhaustion stays silent -- a known, out-of-scope
                 * limitation. */
                RARCH_LOG("[groovy-emit] reannounce: budget exhausted for target=%s role=%s (%u without resync) -- "
                          "initial burst done; one further reannounce at most every %u ms while this receiver "
                          "keeps missing sync (replenished over time)\n",
                          rx->target, groovy_role_name(rx->role),
                          (unsigned)GROOVY_REANNOUNCE_BURST_MAX, (unsigned)GROOVY_REANNOUNCE_REPLENISH_MS);
                rx->reannounce_exhausted_logged = 1;
            }
        }
        backlog++;
    }
    if (backlog > rx->lat.ackq_max) rx->lat.ackq_max = backlog;
}

/* Re-emits CMD_INIT then CMD_SWITCHRES from the session state ALREADY in
 * hand, towards a SINGLE receiver. No new state is invented: compression,
 * audio, dimensions and mode are those the session opening and the last
 * geometry change already established -- only the TARGET (rx->gm) changes
 * from one call to the next. */
static void groovy_reannounce_emit(groovy_state_t *st, struct groovy_receiver *rx)
{
    gm_init_params init_p;
    gm_modeline    mode;
    int            rc_init, wsa_init;
    uint64_t       t_init;

    if (!st || !rx || !rx->gm) return;

    /* groovy_init_params_for(st, rx, ...) returns bytes IDENTICAL to the old
     * manual construction for the master (see the comment on
     * groovy_init_params_for); for a follower, those of ITS OWN opening
     * CMD_INIT (its own compression/audio).
     *
     * Padded mode: gm_send_init below re-sends CMD_INIT on the SAME handle
     * rx->gm, which already had gm_set_padding applied once at opening (a
     * property of the handle, never re-evaluated per datagram -- see
     * gm.h). The re-announce therefore inherits the mode without a single
     * line of code here: nothing to reset. */
    groovy_init_params_for(st, rx, &init_p);
    rc_init  = gm_send_init(rx->gm, &init_p);
    wsa_init = gm_last_announce_error(rx->gm);   /* before CMD_SWITCHRES, which would overwrite it */
    t_init   = groovy_pacing_mono_ns();

    /* Reset the input sequence guard BEFORE sending the hookup byte below.
     * The ORDER IS THE FIX, not just the call itself: the peer cannot send
     * its first input datagram before receiving that byte (have_bound,
     * client/gmclient_input.c), so reset-then-hello leaves no window where
     * a datagram from the new peer could arrive while the guard is still
     * armed. Swapping the two lines would reintroduce a race.
     *
     * No condition on the (a)/(b)/(c) reason from
     * groovy_reannounce_should_fire: want_reannounce is a boolean OR-ed
     * across every status of the same round and doesn't carry the reason.
     * Resetting on (c) too is harmless -- (c) only concerns a peer that is
     * still blind (frame_echo == 0, case (0) covering any synced peer),
     * whose guard is already low or zero. This paragraph used to claim "no
     * new bound, the GROOVY_REANNOUNCE_BURST_MAX one (3 per peer) is
     * inherited unchanged" -- true before the current re-announce shape,
     * false since: resetting this guard now follows the SAME cadence as
     * should_fire itself -- the initial burst (at most 3), THEN at most
     * once every GROOVY_REANNOUNCE_REPLENISH_MS, without end, for as long
     * as this peer stays blind -- never capped at a fixed total.
     *
     * Before the follower plan, the input channel was only bound to the
     * MASTER's handle (s_groovy_input_h == st->gm), so
     * gm_input_reset_peer_guard/gm_send_input_hello on a follower's rx->gm
     * acted on a socket never bound to inputs and returned -1 with no
     * effect. Since then, a follower CAN carry its own channel
     * (s_groovy_input_h[i], when rx->inputs != OFF and GROOVY_INPUT=on) --
     * the two calls then really act on ITS OWN sequence guard and hookup
     * byte, never on the master's nor another follower's (the guard is per
     * handle, therefore per receiver). For a receiver never hooked
     * (inputs=off, or GROOVY_INPUT=off), the documented -1-with-no-effect
     * behavior at gm_bind_inputs's call site is unchanged. s_last_joy_frame
     * (telemetry for the k measurement alone) is only reset for the master
     * (rx == &st->rx[0]) -- a follower is never instrumented for k, only
     * for the inputs= field of [groovy-lat]. */
    if (st->input_mode == GROOVY_INPUT_ON
        && gm_input_reset_peer_guard(rx->gm) == 0) {
        s_reannounce_input_reset++;
        /* Driver-side telemetry, separate from libgm's state:
         * s_last_joy_frame belongs to the PREVIOUS peer. Resetting it at the
         * same event as libgm's state gives it a single owner and a single
         * reset point; without this, if the new peer's first frame happened
         * to land on the old peer's last value, a measurement would be
         * skipped.
         *
         * WHAT THIS LINE DOES NOT DO, and should not be mistaken for: it
         * fixes no k value from [groovy-input-lat]. The guard is an
         * INEQUALITY test (out->frame != s_last_joy_frame), so k is
         * computed the same with or without it. An anomalous k observed
         * once (33/33/33/33 for one client against 0/0/0/0 for another)
         * came from elsewhere: k = gm_last_ack_frame - out->frame subtracts
         * TWO client-side counters -- its status counter (ack_frame in
         * gmclient.c) and its tick counter (the core's runs) -- which drift
         * from each other. For a software client this k is not a latency.
         * Out of scope here: observation recorded, not fixed. */
        if (rx == &st->rx[0])
            s_last_joy_frame = 0u;
    }

    /* Re-send the input channel's hookup byte, FROM THE SAME SOCKET.
     * Without this resend, a client started AFTER the emitter never hooks
     * up: the byte was only sent once, at session opening, and it is the
     * ONLY way for the peer to learn the return address (ephemeral source
     * port). gm_send_input_hello, not gm_bind_inputs: the latter would
     * close and reopen the socket, changing that port every time. */
    if (st->input_mode == GROOVY_INPUT_ON && gm_send_input_hello(rx->gm) == 0)
        s_reannounce_hello++;

    if (!st->av_cache.valid || st->av_cache.fps <= 0.0) {
        /* No geometry has been announced yet: CMD_INIT alone, and it is
         * counted. The next geometry change will do the rest. */
        s_reannounce_init_only++;
        if (rc_init != 0) s_reannounce_failure++;
        groovy_announce_note(st, rx, GROOVY_ANNOUNCE_INIT, GROOVY_SITE_REANNOUNCE,
                            rc_init, wsa_init, t_init);
        groovy_announce_note(st, rx, GROOVY_ANNOUNCE_SWITCHRES, GROOVY_SITE_REANNOUNCE,
                            GROOVY_ANNOUNCE_NOT_READY, 0, groovy_pacing_mono_ns());
        return;
    }

    if (st->reannounce_spacing_us != 0u) {
        /* Deferred, never a wait. CMD_SWITCHRES goes out from
         * groovy_reannounce_flush_deferred, BEFORE the fan-out of a LATER
         * frame -- never this one (see switchres_deferred_turn). The
         * CMD_INIT trace is written now: no send follows within this call.
         *
         * A deferral already pending and not yet sent is OVERWRITTEN here
         * -- silently, before this fix. Now traced, and its possible
         * CMD_INIT failure (never counted until now) is added to
         * s_reannounce_failure before being lost. */
        if (rx->switchres_deferred) {
            if (rx->switchres_rc_init != 0) s_reannounce_failure++;
            RARCH_LOG("[groovy-emit] reannounce: deferred switchres overwritten for target=%s role=%s\n",
                      rx->target, groovy_role_name(rx->role));
        }
        rx->switchres_deferred          = 1;
        rx->switchres_t_init_ns        = t_init;
        rx->switchres_rc_init          = rc_init;
        rx->switchres_deferred_turn     = st->turn_video;
        groovy_announce_note(st, rx, GROOVY_ANNOUNCE_INIT, GROOVY_SITE_REANNOUNCE,
                            rc_init, wsa_init, t_init);
        return;
    }

    compute_modeline_from_dims(st->av_cache.width, st->av_cache.height,
                               st->av_cache.fps, &mode);
    mode.interlace = (st->current_mode == GROOVY_MODE_480I) ? 1u : 0u;
    {
        const int      rc_sw  = gm_send_switchres(rx->gm, &mode);
        const int      wsa_sw = gm_last_announce_error(rx->gm);
        const uint64_t t_sw   = groovy_pacing_mono_ns();
        if (rc_sw == 0) s_reannounce_sent++;
        if (rc_init != 0 || rc_sw != 0) s_reannounce_failure++;
        groovy_announce_note(st, rx, GROOVY_ANNOUNCE_INIT, GROOVY_SITE_REANNOUNCE,
                            rc_init, wsa_init, t_init);
        groovy_announce_note(st, rx, GROOVY_ANNOUNCE_SWITCHRES, GROOVY_SITE_REANNOUNCE,
                            rc_sw, wsa_sw, t_sw);
    }
}

/* Sends a re-announce's deferred CMD_SWITCHRES.
 *
 * Called for each receiver BEFORE the fan-out of a LATER frame -- never the
 * one that set the deferral, even with a very short spacing. The clock
 * alone wasn't enough to guarantee "later": switchres_deferred_turn keeps
 * st->turn_video at the moment of deferral, and this function refuses to
 * fire until st->turn_video has changed, whatever
 * groovy_reannounce_spacing_due() below returns. Before this fix, the send
 * used to happen AFTER the fan-out of the SAME frame -- stuck to the tail
 * of its own burst instead of being separated from the next one.
 *
 * This lock used to be on st->frame_id, which only advances after a
 * successful fan-out and REFLECTS THE MASTER'S RESULT ALONE (emit_ok ==
 * master_ok, groovy_fan_out) -- if gm_send_blit to the master fails every
 * frame (route gone, WSAENOBUFS, ...), frame_id stayed frozen for good and
 * a FOLLOWER's deferred CMD_SWITCHRES would then never go out again: each
 * following re-announce overwrote it with the same frozen frame_id, and the
 * follower kept receiving a CMD_INIT and blits every T seconds, but never
 * its geometry -- a black screen with no recovery, exactly what this
 * design removes. turn_video (groovy_state_t) advances on EVERY pass
 * through the { vsync_target ... } block of groovy_push_video, regardless
 * of the fan-out result that follows: the "never the same turn" lock no
 * longer depends on any send, only on the number of frames rendered.
 *
 * No effect while the spacing hasn't elapsed. Cancelled if the receiver has
 * sent a resync in the meantime: a hooked peer never receives this resend
 * -- this test reads rx->ack_state.last_frame_echo exactly as THIS frame's
 * status drain (groovy_drain_statuses, called earlier in
 * groovy_push_video, before this loop) just left it: the caller must stay
 * placed AFTER that drain, never before. The geometry sent is the one in
 * effect AT SEND TIME.
 *
 * The two silent returns below (cancellation, av_cache guard) used to count
 * a failed CMD_INIT nowhere -- reannounces_failed undercounted in one
 * build. Fixed to increment s_reannounce_failure the same way the
 * immediate path does; the av_cache guard now also traces
 * GROOVY_ANNOUNCE_NOT_READY the way the immediate path already does. */
static void groovy_reannounce_flush_deferred(groovy_state_t *st, struct groovy_receiver *rx, uint64_t now_ns)
{
    gm_modeline mode;

    if (!st || !rx || !rx->gm || !rx->switchres_deferred) return;
    if (st->turn_video == rx->switchres_deferred_turn) return;   /* never the same turn */
    if (!groovy_reannounce_spacing_due(rx->switchres_t_init_ns, st->reannounce_spacing_us, now_ns))
        return;
    rx->switchres_deferred = 0;
    if (rx->ack_state.last_frame_echo != 0u) {
        if (rx->switchres_rc_init != 0) s_reannounce_failure++;
        RARCH_LOG("[groovy-emit] reannounce: deferred switchres cancelled for target=%s role=%s -- resync already back\n",
                  rx->target, groovy_role_name(rx->role));
        return;
    }
    if (!st->av_cache.valid || st->av_cache.fps <= 0.0) {
        if (rx->switchres_rc_init != 0) s_reannounce_failure++;
        groovy_announce_note(st, rx, GROOVY_ANNOUNCE_SWITCHRES, GROOVY_SITE_REANNOUNCE,
                            GROOVY_ANNOUNCE_NOT_READY, 0, groovy_pacing_mono_ns());
        return;
    }
    compute_modeline_from_dims(st->av_cache.width, st->av_cache.height,
                               st->av_cache.fps, &mode);
    mode.interlace = (st->current_mode == GROOVY_MODE_480I) ? 1u : 0u;
    {
        const int      rc_sw  = gm_send_switchres(rx->gm, &mode);
        const int      wsa_sw = gm_last_announce_error(rx->gm);
        const uint64_t t_sw   = groovy_pacing_mono_ns();
        if (rc_sw == 0) s_reannounce_sent++;
        if (rx->switchres_rc_init != 0 || rc_sw != 0) s_reannounce_failure++;
        groovy_announce_note(st, rx, GROOVY_ANNOUNCE_SWITCHRES, GROOVY_SITE_REANNOUNCE,
                            rc_sw, wsa_sw, t_sw);
    }
}

/* ---------------------------------------------------------------------------
 * Fan-out.
 *
 * groovy_switchres_all -- CMD_SWITCHRES to the master FIRST, then to each
 * follower; returns the master's result. A follower's failure stays
 * WITHOUT EFFECT ON THE WIRE -- that receiver keeps its old geometry until
 * the next real change or a future re-announce -- but this failure is now
 * made VISIBLE by [groovy-annonce], instead of being silently dropped.
 *
 * A deferred CMD_SWITCHRES not yet sent is cancelled HERE, before the
 * immediate send below -- otherwise the receiver would get two
 * CMD_SWITCHRES in a row (this one, then the old deferral going out anyway
 * later), and the report (client/bilan-999-33.py) wrongly attributed that
 * second send to the "reannounce" site instead of the "switchres_all" site
 * that actually made it moot.
 * ------------------------------------------------------------------------- */
static bool groovy_switchres_all(groovy_state_t *st, const gm_modeline *m)
{
    int      rc[GROOVY_RX_MAX];
    int      wsa[GROOVY_RX_MAX];
    uint64_t t_env[GROOVY_RX_MAX];

    /* Clock servo: the feed-forward is set RIGHT HERE, before any send -- it
     * must change when the MODE changes, not when the receiver replies.
     * The only other site in this file that sets it is the opening
     * (groovy_new): both re-announce paths (groovy_reannounce_emit,
     * groovy_reannounce_flush_deferred) reapply the SAME mode and never
     * call this setter directly -- adding it there would be a strict no-op
     * (see groovy_servo.h), with no benefit, and would multiply the sites
     * to re-check. */
    groovy_servo_apply_ff(st, (int)m->interlace);

    for (unsigned i = 0u; i < st->n_rx; i++) {
        if (st->rx[i].switchres_deferred) {
            /* This cancellation path counted no failed CMD_INIT in
             * s_reannounce_failure -- an earlier fix had covered the
             * overwrite case, the resync cancellation and the av_cache
             * guard, never this one. Same discipline as the other three:
             * the count precedes clearing the flag, before the trace. */
            if (st->rx[i].switchres_rc_init != 0) s_reannounce_failure++;
            st->rx[i].switchres_deferred = 0;
            RARCH_LOG("[groovy-emit] reannounce: deferred switchres cancelled for target=%s role=%s -- switchres_all\n",
                      st->rx[i].target, groovy_role_name(st->rx[i].role));
        }
    }

    /* Master FIRST, order unchanged. Result read INSIDE the loop, right
     * after THIS send; no trace interleaved between two sends. */
    for (unsigned i = 0u; i < st->n_rx; i++) {
        rc[i]    = gm_send_switchres(st->rx[i].gm, m);
        wsa[i]   = gm_last_announce_error(st->rx[i].gm);
        t_env[i] = groovy_pacing_mono_ns();
    }
    for (unsigned i = 0u; i < st->n_rx; i++)
        groovy_announce_note(st, &st->rx[i], GROOVY_ANNOUNCE_SWITCHRES,
                            GROOVY_SITE_SWITCHRES_ALL, rc[i], wsa[i], t_env[i]);
    return rc[0] == 0;
}

/* The three ways to emit a frame, unified so the receiver table can be
 * looped over without duplicating the split-by-shape logic. */
enum groovy_emit_kind {
    GROOVY_EMIT_FULL   = 0,   /* CMD_BLIT_VSYNC (0x06), full frame -- 240p */
    GROOVY_EMIT_FIELDS = 1,   /* two CMD_BLIT_FIELD_VSYNC (0x07) -- 480i, fld0+fld1 path */
    GROOVY_EMIT_HALF   = 2    /* one CMD_BLIT_VSYNC (0x06), half height -- 480i, "one field per frame" */
};

/* Sends an ALREADY PREPARED payload to a SINGLE handle. `a`/`b`: for FULL
 * and HALF, only `a` is read (`b` ignored); for FIELDS, `a` = field 0, `b` =
 * field 1, sent in that order with the SAME short-circuit as before this
 * change (if `a` fails, `b` is never attempted). Returns 0 if everything
 * went out, -1 otherwise -- same convention as gm_send_blit*. */
static int groovy_send_payload(gm_handle *h, enum groovy_emit_kind k,
                               uint32_t frame_id, uint16_t vsync, uint8_t field,
                               const uint8_t *a, const uint8_t *b, size_t len)
{
    switch (k) {
    case GROOVY_EMIT_FULL:
        return gm_send_blit(h, frame_id, vsync, a, len);
    case GROOVY_EMIT_HALF:
        return gm_send_blit_half(h, frame_id, vsync, field, a, len);
    case GROOVY_EMIT_FIELDS:
        if (gm_send_blit_field(h, frame_id, vsync, 0u, a, len) != 0)
            return -1;
        return gm_send_blit_field(h, frame_id, vsync, 1u, b, len);
    default:
        return -1;
    }
}

/* Fan-out of an already-prepared frame: the master FIRST, then each
 * follower in its declaration order -- its return code is IGNORED (a
 * follower's failure never affects emit_ok nor the master's frame_id
 * advance). `st->before_master` counts, per window, followers served
 * BEFORE the master in their turn -- an ordering violation, evidence on the
 * emission side, independent of network arrival order (local loop latency
 * varies frame to frame).
 *
 * An earlier version of this counter compared a follower send's start
 * timestamp to the end of the master's, read earlier in the same thread --
 * it was 0 by construction, in EVERY order (a follower served before the
 * master still saw the master's end still at 0). Two test assertions could
 * therefore never fail. The master_served boolean counts what a reversed
 * order would actually betray: zero as long as the master goes first, one
 * per follower per turn otherwise.
 *
 * A follower's saturation (BEFORE the send, for i > 0 only -- the master is
 * never skipped): `groovy_sat_should_skip` decides on THIS follower's
 * SIGNAL -- the number of images REALLY sent to it that it hasn't
 * acknowledged yet (its last frame_echo, rx->ack_state.last_frame_echo),
 * counted on the rx->sat ring. This signal lags by at least one round trip:
 * it is a TREND correction, not instantaneous back-pressure.
 * `rx->last_sent_id`/`has_sent` are only updated AFTER a send really
 * attempted (any i, master included) -- never after a skip, which
 * deliberately leaves both fields frozen at the last value really sent.
 * The rx->sat ring follows the same rule (groovy_sat_note_sent): a skip
 * writes nothing to it.
 *
 * st->frame_id is passed into the decision -- the first probe of a skip
 * episode freezes the output reference there, and later probes in the same
 * episode no longer move it (see groovy_sat_should_skip).
 *
 * An earlier version of the gap was rx->last_sent_id - last_frame_echo, a
 * difference of GLOBAL identifiers. After a skip episode, it also counted
 * the SKIPPED images, which this follower never received and no one will
 * ever acknowledge -- against the written definition of the signal
 * (groovy_followers.h), and that is what made a follower fall back into
 * skipping at 6-8 round-trip images. The ring only counts images that
 * REALLY went out. */
/* Delivers the held blocks to ONE receiver, right after its image chunks,
 * in the SAME send gesture -- no call in between (no timestamp, no trace,
 * no syscall other than the sendto calls).
 *
 * WHY AFTER AND NOT BEFORE: "audio before the frame" is explicitly
 * forbidden by this design's negative constraint.
 * WHY NOT IN THE MIDDLE: a CMD_AUDIO header landing between an image
 * header and its last chunk triggers the receiver's reassembler escape path
 * and DROPS the in-progress image (client/gmclient.c, handle_datagram_inner,
 * issue 'E').
 *
 * A send failure only abandons the REST of THIS receiver's audio: the other
 * receivers and the video keep going. */
static void groovy_hold_flush_vers(groovy_state_t *st, struct groovy_receiver *rx)
{
    for (unsigned b = 0u; b < st->audio_hold.n_blocks; b++) {
        size_t nf = 0u;
        const int16_t *p = groovy_audio_hold_block(&st->audio_hold, b, &nf);
        if (!p || nf == 0u) continue;
        size_t sent = 0u;
        while (sent < nf) {
            size_t chunk = nf - sent;
            if (chunk > GROOVY_AUDIO_MAX_FRAMES) chunk = GROOVY_AUDIO_MAX_FRAMES;
            if (gm_send_audio(rx->gm, p + sent * (size_t)st->audio_channels,
                              chunk, st->audio_channels) != 0) {
                if (rx->audio_errors++ == 0u)
                    RARCH_WARN("[groovy-audio-hold] gm_send_audio refused a held block "
                               "towards %s; this receiver's audio will stop where it "
                               "drops.\n", rx->target);
                return;
            }
            sent += chunk;
        }
    }
}

static bool groovy_fan_out(groovy_state_t *st, enum groovy_emit_kind k,
                           uint16_t vsync, uint8_t field,
                           const uint8_t *a, const uint8_t *b, size_t len)
{
    bool master_ok    = false;
    bool master_served = false;   /* has the master already been served this turn? */

    for (unsigned i = 0u; i < st->n_rx; i++) {
        struct groovy_receiver *rx = &st->rx[i];

        if (i > 0u
            && groovy_sat_should_skip(&rx->sat, rx->link.synced,
                                      rx->ack_state.last_frame_echo,
                                      st->frame_id)) {   /* this turn's image */
            rx->saturations++;
            /* Lever b6, cross case: this follower is
             * skipped, but the master will be served and the queue will
             * therefore be flushed at the bottom of groovy_push_video. This
             * audio will NEVER reach it. That is a CHOICE, not an
             * oversight -- deferring the flush would make the master
             * receive these same blocks a SECOND time (duplicated, audible
             * audio), and dumping the backlog onto a follower that
             * saturation is precisely trying to relieve would push more
             * bytes at the worst possible moment. It is counted, and the
             * report states it. */
            if (st->audio_hold_mode == GROOVY_AUDIO_HOLD_ON && rx->audio_on)
                rx->audio_hold_not_played += (uint64_t)st->audio_hold.n_blocks;
            continue;
        }

        int rc = groovy_send_payload(rx->gm, k, st->frame_id, vsync,
                                     field, a, b, len);
        if (i == 0u) {
            master_ok    = (rc == 0);
            master_served = true;
        } else if (!master_served) {
            st->before_master++;   /* follower served BEFORE the master, ordering violation */
        }
        /* A follower's rc is deliberately ignored. */
        rx->last_sent_id = st->frame_id;
        rx->has_sent     = 1;
        groovy_sat_note_sent(&rx->sat, st->frame_id);   /* only images that went OUT count */

        /* Lever b6: audio sticks to THIS receiver's image burst. */
        if (st->audio_hold_mode == GROOVY_AUDIO_HOLD_ON && rx->audio_on)
            groovy_hold_flush_vers(st, rx);
    }
    return master_ok;
}

/* At least one receiver (master or follower) has audio on -- the entry
 * guard for groovy_push_audio, to bail out early without splitting or
 * resampling a block that no one would receive. */
static bool groovy_any_audio_on(const groovy_state_t *st)
{
    for (unsigned i = 0u; i < st->n_rx; i++)
        if (st->rx[i].audio_on)
            return true;
    return false;
}

/* Servo freeze. Five signals, in display priority order. Verified: before
 * this feature, record_groovy.c only read FASTMOTION (two sites) --
 * PAUSED, SLOWMOTION, the menu and rewind are NEW code, not a hook onto
 * something already there. */
static int groovy_servo_freeze_current(void)
{
    const uint32_t rf = runloop_get_flags();
    if (rf & RUNLOOP_FLAG_PAUSED)      return GROOVY_SERVO_FREEZE_PAUSE;
    if (rf & RUNLOOP_FLAG_SLOWMOTION)  return GROOVY_SERVO_FREEZE_SLOWMO;
    if (rf & RUNLOOP_FLAG_FASTMOTION)  return GROOVY_SERVO_FREEZE_FASTFWD;
#ifdef HAVE_MENU
    {
        struct menu_state *ms = menu_state_get_ptr();
        if (ms && (ms->flags & MENU_ST_FLAG_ALIVE))
            return GROOVY_SERVO_FREEZE_MENU;
    }
#endif
    if (state_manager_frame_is_reversed()) return GROOVY_SERVO_FREEZE_REWIND;
    return GROOVY_SERVO_FREEZE_NONE;
}

/* ---------------------------------------------------------------------------
 * vtable function: groovy_push_video
 *
 * Called by RetroArch once per rendered frame with the GPU readback buffer.
 * The buffer is BGR24 from the glcore PBO readback path (Pitfall 2).
 *
 * Protocol:
 *   1. Skip duplicate frames (vid->is_dupe == true — T-3-11 defense).
 *   2. Grow the swap buffer if needed (T-3-09: realloc with NULL-check).
 *   3. Swap BGR→RGB via bgr24_to_rgb888 (T-3-02).
 *   4. Emit CMD_BLIT_VSYNC via gm_send_blit.
 *
 * Returns true on success, false on irrecoverable error. RetroArch
 * does NOT abort the session on false — it just logs a warning.
 * ------------------------------------------------------------------------- */
static bool groovy_push_video(void *data, const struct record_video_data *vid)
{
    groovy_state_t *st = (groovy_state_t *)data;
    if (!st || !st->gm || !vid || !vid->data)
        return false;

    /* Skip duplicate frames per upstream convention (T-3-11). */
    if (vid->is_dupe)
        return true;

    /* Diagnostic: log per-frame vid dims to verify whether vid->width/height
     * is the CURRENT frame size (libretro native, what we want) or the FB max
     * size (700x576). Logged once per ~2 seconds via simple counter to avoid
     * spam. */
    {
        static unsigned diag_counter = 0u;
        if ((diag_counter++ % 120u) == 0u) {
            RARCH_LOG("[groovy] push_video vid: %ux%u pitch=%d (av_cache=%ux%u valid=%d pending=%ux%u streak=%u)\n",
                      vid->width, vid->height, vid->pitch,
                      st->av_cache.width, st->av_cache.height,
                      (int)st->av_cache.valid,
                      st->pending_w, st->pending_h, st->pending_streak);
        }
    }

    /* Phase 3.1 v3 fix (HIL run, "native resolution" goal): use the actual
     * per-frame vid dims as the wire size. PSX cores (Beetle PSX) change resolution mid-stream for menus
     * (320x240 gameplay → 640x478 menu) without firing libretro
     * SET_GEOMETRY, so the av_info-only path missed those transitions.
     * Per-frame detection here triggers a fresh CMD_SWITCHRES on any dim
     * change, the daemon re-programs the CRT modeline, and the wire carries
     * native pixels — no downscale, no crop.
     *
     * Core project value: a native-resolution frame reaches the CRT at the
     * right cadence. This path satisfies it for the first time. */
    /* Captured dims (quick 260901-po3): what the core submitted. Announced
     * dims: what goes out on the wire, divided by the box factor. The
     * division itself happens further down, after the startup guards (fps
     * not known yet, fastmotion) — otherwise the downsample_dims_mismatch
     * counter would get polluted by frames from before the handshake. */
    unsigned cap_w  = vid->width;
    unsigned cap_h  = vid->height;
    unsigned send_w = cap_w;
    unsigned send_h = cap_h;

    /* Detect dim change and emit CMD_SWITCHRES BEFORE the BLIT. Daemon is
     * tolerant of in-stream switchres per Phase 2 design. Reuse the cache
     * comparison: if cache is invalid, also re-emit (first frame after init).
     *
     * Skip the entire frame if no fps cached yet (push_video fired before any
     * SET_SYSTEM_AV_INFO) — sending a BLIT without a matching CMD_SWITCHRES
     * would land on whatever modeline the daemon happens to have (likely the
     * 640x240 super-res startup mode), causing the framing-loss cascade we
     * just fixed. Return true (not false) — soft-skip, not session-broken. */
    if (s_last_valid_fps <= 0.0)
        return true;

    /* Fast-forward guard (quick-260712-aye). In fastmotion, the core runs unbridled and
     * push_video fires much faster than the display refresh. Each emit =
     * readback swap + ~150-300 sendto calls (BLIT chunks); flooding the
     * daemon past ~60 fps corrupts the CRT image AND the sendto cost throttles
     * the fast-forward it should be speeding up. Silent drop if less than 1
     * mode period has elapsed since the last emit — the core keeps its full
     * speed, the CRT receives ~refresh snapshots/s. A no-op at normal speed:
     * the guard is only armed while the fastmotion flag is present (frames
     * then already arrive one period apart — no drop possible during normal
     * play). File-scope-static-safe: record driver singleton, same rationale
     * as s_last_valid_fps. */
    if (runloop_get_flags() & RUNLOOP_FLAG_FASTMOTION) {
        static retro_time_t s_ff_last_emit_usec = 0;
        retro_time_t now         = cpu_features_get_time_usec();
        retro_time_t period_usec = (retro_time_t)(1000000.0 / s_last_valid_fps);
        if (s_ff_last_emit_usec != 0
            && (now - s_ff_last_emit_usec) < period_usec) {
            /* Clock servo: this guard hands control back PRE-EMPTIVELY
             * (soft-drop) -- without this call, the servo would never see
             * the fast-forward while this path is taken, and would read
             * back on return a huge, false advance (anti-windup). */
            groovy_servo_update_post_poll(&st->servo, &st->rx[0].ack_state,
                                          st->frame_id,
                                          groovy_pacing_mono_ns(),
                                          (int)st->servo_mode,
                                          GROOVY_SERVO_FREEZE_FASTFWD);
            audio_driver_set_groovy_term(groovy_servo_term(&st->servo));
            return true;               /* soft-drop, session alive */
        }
        s_ff_last_emit_usec = now;
    }

    /* Supersampling box, CPU side (quick 260901-po3): divide the dims BEFORE mode
     * classification, otherwise an enlarged submission accidentally flips
     * the receiver into 480i. Placed AFTER the two guards above so that
     * downsample_dims_mismatch doesn't count startup frames, from before
     * the fps handshake. */
    if (st->downsample_n > 1u) {
        if (!groovy_downsample_dims_ok(cap_w, cap_h, st->downsample_n)) {
            /* Refuse the frame rather than distort it. Trace limited to
             * dim changes (singleton driver, same rationale as the BLIT
             * guard below). */
            static unsigned s_ds_warn_w = 0u, s_ds_warn_h = 0u;
            st->downsample_dims_mismatch++;
            if (s_ds_warn_w != cap_w || s_ds_warn_h != cap_h) {
                RARCH_WARN("[groovy] downsample_dims_mismatch: %ux%u not divisible "
                           "by %u — frame refused\n",
                           cap_w, cap_h, st->downsample_n);
                s_ds_warn_w = cap_w;
                s_ds_warn_h = cap_h;
            }
            return true;   /* soft-skip, session alive */
        }
        send_w = cap_w / st->downsample_n;
        send_h = cap_h / st->downsample_n;
    }

    /* Per-frame dim-change → emit CMD_SWITCHRES, gated by hysteresis.
     * Daemon's parser updates expected_frame_bytes immediately on receipt
     * (net.c:425), so the BLIT we send right after a switchres will match
     * the new size — no framing loss. The daemon's actual drmModeSetCrtc may
     * rate-limit (100ms window), and its last-write-wins coalescing can drop
     * a real geometry change if the emitter spams switchres on every Beetle
     * PSX field flip. We filter that spam at the source: only commit a new
     * (send_w, send_h) after it has persisted for HYSTERESIS_FRAMES. */
    /* Mode selector. */
    enum groovy_mode new_mode = classify_mode(send_w, send_h);
    if (st->pending_mode == new_mode) {
        if (st->mode_streak < GROOVY_MODE_SETTLE_FRAMES) ++st->mode_streak;
    } else {
        st->pending_mode = new_mode;
        st->mode_streak  = 0u;
    }

    /* Dim hysteresis (preserved) — guards within-mode dim oscillation. */
    if (st->pending_w == send_w && st->pending_h == send_h) {
        if (st->pending_streak < GROOVY_DIM_HYSTERESIS_FRAMES)
            ++st->pending_streak;
    } else {
        st->pending_w      = send_w;
        st->pending_h      = send_h;
        st->pending_streak = 0u;
    }

    /* Emit CMD_SWITCHRES on mode change (rare). */
    if (st->mode_streak >= GROOVY_MODE_SETTLE_FRAMES
        && st->current_mode != new_mode) {
        gm_modeline mode;
        compute_modeline_from_dims(send_w, send_h, s_last_valid_fps, &mode);
        mode.interlace = (new_mode == GROOVY_MODE_480I) ? 1u : 0u;
        (void)groovy_switchres_all(st, &mode);
        st->current_mode  = new_mode;
        st->av_cache.width  = send_w;
        st->av_cache.height = send_h;
        st->av_cache.fps    = s_last_valid_fps;
        st->av_cache.valid  = true;
        /* Telemetry: a structured emitter-side mode_change log entry could go
         * here, but record_groovy already logs CMD_SWITCHRES sends; the daemon
         * emits the mode_change JSON event when it processes the packet. */
    } else if (st->pending_streak >= GROOVY_DIM_HYSTERESIS_FRAMES
               && (!st->av_cache.valid
                   || st->av_cache.width != send_w
                   || st->av_cache.height != send_h)) {
        /* Within-mode dim change — emit CMD_SWITCHRES so the daemon updates
         * h_active_announced for the crop/center/pad math. The daemon's
         * mode resolver treats this as a "no modeset" event since the mode
         * is unchanged. */
        gm_modeline mode;
        compute_modeline_from_dims(send_w, send_h, s_last_valid_fps, &mode);
        mode.interlace = (st->current_mode == GROOVY_MODE_480I) ? 1u : 0u;
        (void)groovy_switchres_all(st, &mode);
        st->av_cache.width  = send_w;
        st->av_cache.height = send_h;
        st->av_cache.fps    = s_last_valid_fps;
        st->av_cache.valid  = true;
    }

    /* Two sizes (quick 260901-po3): need_cap is what the converters write (captured dims,
     * swap_buf); need is what goes out on the wire (announced dims, after
     * any box). At `off`, cap_w==send_w and cap_h==send_h, so need_cap ==
     * need — no change in behavior. */
    size_t need_cap = (size_t)cap_w  * (size_t)cap_h  * 3u;
    size_t need     = (size_t)send_w * (size_t)send_h * 3u;
    if (need == 0u)
        return false;

    /* Grow swap buffer if needed (T-3-09: realloc + NULL check). */
    if (need_cap > st->swap_cap) {
        uint8_t *grown = (uint8_t *)realloc(st->swap_buf, need_cap);
        if (!grown)
            return false;   /* soft error: drop frame, keep session alive */
        st->swap_buf = grown;
        st->swap_cap = need_cap;
    }

    /* CRITICAL — Pitfall 2 + Bug D fix: pixel format depends on RetroArch's
     * record_driver.c:302-394 selection. RGB565 (2 bytes/px), BGR24 (3
     * bytes/px), or ARGB8888 (4 bytes/px) — pre-cached in st->pix_fmt at init.
     *
     * Daemon expects packed RGB888. Each helper handles the source-stride
     * distinction internally; bgr24/xrgb path crop to top-left send_w x send_h
     * (per Bug C fix block above). RGB565 is record_driver.c's default for
     * any core whose libretro pixel format isn't XRGB8888 — i.e. every 16-bit
     * core (Mega Drive, SNES, NES, Master System, PC Engine); handled below.
     *
     * Operate into swap_buf (not vid->data, which is const and has a pitch
     * stride determined by the GL FB layout, not by send_w*3). */
    /* The three converters receive cap_w/cap_h as destination dims — NOT
     * send_w/send_h. They are already ratio-preserving boxes; passing them
     * the divided dims on top of the new box would shrink the frame twice
     * (N^2 instead of N). At `off`, cap == send and these three calls are
     * identical to before, argument for argument. */
    if (st->pix_fmt == FFEMU_PIX_BGR24) {
        bgr24_to_rgb888(st->swap_buf, vid->data,
                        cap_w, cap_h,             /* dst dims (declare, 1:1) */
                        vid->width, vid->height,  /* src dims (RA FB) */
                        (unsigned)vid->pitch);
    } else if (st->pix_fmt == FFEMU_PIX_ARGB8888) {
        xrgb8888_to_rgb888(st->swap_buf, vid->data,
                           cap_w, cap_h,
                           vid->width, vid->height,
                           (unsigned)vid->pitch);
    } else if (st->pix_fmt == FFEMU_PIX_RGB565) {
        rgb565_to_rgb888(st->swap_buf, vid->data,
                         cap_w, cap_h,
                         vid->width, vid->height,
                         (unsigned)vid->pitch);
    } else {
        /* Unknown format — drop frame (T-3-09 soft error). */
        return false;
    }

    /* The box, at the convergence point (quick 260901-po3): a single piece of code for all
     * four backends, software cores covered by construction. Right after
     * format normalization, before caching st->width/height. */
    const uint8_t *frame = st->swap_buf;
    if (st->downsample_n > 1u) {
        if (need > st->down_cap) {
            uint8_t *grown = (uint8_t *)realloc(st->down_buf, need);
            if (!grown) return false;   /* soft error, session alive */
            st->down_buf = grown;
            st->down_cap = need;
        }
        uint64_t box_t0 = groovy_pacing_mono_ns();
        groovy_downsample_box_rgb888(st->down_buf, st->swap_buf,
                                     cap_w, cap_h, st->downsample_n);
        uint64_t box_ns = groovy_pacing_mono_ns() - box_t0;
        st->downsample_ns_total += box_ns;
        if (box_ns > st->downsample_ns_max) st->downsample_ns_max = box_ns;
        if (++st->downsample_samples >= 600u) {
            RARCH_LOG("[groovy-box] factor=%s %ux%u -> %ux%u n=%u "
                      "average=%.1f us worst=%.1f us\n",
                      groovy_downsample_factor_name(st->downsample_n),
                      cap_w, cap_h, send_w, send_h, st->downsample_samples,
                      (double)st->downsample_ns_total / (double)st->downsample_samples / 1000.0,
                      (double)st->downsample_ns_max / 1000.0);
            st->downsample_ns_total = 0u;
            st->downsample_ns_max   = 0u;
            st->downsample_samples  = 0u;
        }
        frame = st->down_buf;
    }

    /* Cache geometry for Plan 03-06 push_av_info / CMD_SWITCHRES fallback.
     * Store the SENT dims (declared), not the FB dims, so subsequent frames
     * are consistent with what the daemon expects. */
    st->width  = send_w;
    st->height = send_h;

    /* ACK-based vsync target (Plan 03-07, EMT-04), MERGED with the status
     * drain (Rule 1 fix). This single drain loop is now ONE PER RECEIVER
     * (`groovy_drain_statuses`, defined above), each setting its OWN
     * `rx->ack_state`, filtering its OWN source, and evaluating its OWN
     * re-announce budget. `vsync_target` is still computed AFTER, by
     * compute_vsync_target_from_acks_post_poll -- but now on
     * `st->rx[0].ack_state` ALONE (a single call in the whole file, checked
     * by grep): a follower can no longer touch the tube, by construction.
     * With no master, this same call returns the local fallback exactly as
     * before this design -- nothing changes for the "no receiver at all"
     * case. */
    uint16_t vsync_target;
    {
        /* Anchor for groovy_reannounce_flush_deferred's "later frame"
         * (switchres_deferred_turn) -- advances on EVERY pass here,
         * whatever the fan-out result further down in this same frame,
         * unlike st->frame_id (which only advances if the MASTER's send
         * lands). */
        st->turn_video++;
        const uint64_t now_ms = groovy_pacing_mono_ns() / 1000000u;  /* (c) */
        int             want_master = 0;

        groovy_drain_statuses(st, &st->rx[0], now_ms, &want_master);
        if (want_master) groovy_reannounce_emit(st, &st->rx[0]);

        /* Freshness decision (groovy_pacing.h), on the state the drain
         * above just refreshed -- the ONLY call to
         * compute_vsync_target_from_acks_post_poll in the whole file, and
         * only rx[0] (the master) is ever passed to it. */
        vsync_target = compute_vsync_target_from_acks_post_poll(&st->rx[0].ack_state);

        /* Clock servo: the MASTER, and only the master. Never a loop over
         * st->rx[] -- a follower carries its own counter in `frame` and
         * paces nothing. st->frame_id is the emitted-frame counter; it
         * increments AFTER the send, so it is one more than the last
         * identifier really sent. This constant offset is absorbed by the
         * setpoint, which is MEASURED: don't "fix" it with a -1, that would
         * count it twice. */
        /* Clock hoisted into a local variable -- one fewer
         * QueryPerformanceCounter call, and above all the SAME value in the
         * trace as in the measurement. samples_before lets the trace know
         * whether a sub-frame measurement really happened THIS turn
         * (branch (8) of groovy_servo_update_post_poll), rather than a
         * reanchor/freeze/miss. */
        const uint64_t servo_now_ns   = groovy_pacing_mono_ns();
        const uint32_t samples_before  = st->servo.samples;
        groovy_servo_update_post_poll(&st->servo, &st->rx[0].ack_state,
                                      st->frame_id,
                                      servo_now_ns,
                                      (int)st->servo_mode,
                                      groovy_servo_freeze_current());
        audio_driver_set_groovy_term(groovy_servo_term(&st->servo));
        if (st->servo.state == GROOVY_SERVO_STATE_FRESH) st->servo_acked++;

        /* One trace PER MEASURED ACK, not throttled -- the [groovy-servo]
         * line is a 1/s sample of a ~60 Hz stream, which cannot
         * distinguish a real plateau from a sub-sampling artifact. Off by
         * default: absent setting = not one extra byte in the log, not one
         * extra cycle in the loop. send_ns is RECOMPUTED instead of being
         * re-read from the ring: the subtraction is EXACT, because
         * latency_ns is the difference of two nanosecond integers stored
         * in a double, and any integer under 2^53 ns (~104 days) fits
         * there without loss. */
        if (st->servo_trace && st->servo.samples != samples_before) {
            const uint64_t send_ns = servo_now_ns - (uint64_t)st->servo.latency_ns;
            RARCH_LOG("[groovy-servo-trace] now_ns=%llu send_ns=%llu frame=%u emitted=%u "
                      "latency_us=%.1f raw_error=%+.4ftr window=%u\n",
                      (unsigned long long)servo_now_ns,
                      (unsigned long long)send_ns,
                      (unsigned)st->rx[0].ack_state.last_frame,
                      (unsigned)st->frame_id,
                      st->servo.latency_ns * 1e-3,
                      st->servo.raw_error_frames,
                      st->servo.window_n);
        }

        /* One line per second, SEPARATE from [groovy-lat]. One concern per
         * line; existing tools don't change. The displayed slope is the
         * total term's variation over the elapsed second -- right at a
         * mode toggle it will legitimately show ~1900 ppm/s, well above
         * GROOVY_SERVO_SLOPE_MAX_PPM_S: this is NOT a defect, the slope
         * bound only applies to servo_ppm, never to the feed-forward. */
        {
            const uint64_t now_log_ns = groovy_pacing_mono_ns();
            if (st->servo_log_ns == 0ull)
                st->servo_log_ns = now_log_ns;
            else if (now_log_ns - st->servo_log_ns >= 1000000000ull)
            {
                RARCH_LOG("[groovy-servo] state=%s freeze=%s "
                          "ff=%+.1fppm servo=%+.1fppm term=%+.1fppm "
                          "latency=%.2fms error=%+.3ftr setpoint=%.2ftr "
                          "slope=%+.0fppm/s reanchors=%u toggles=%u "
                          "misses=%u acked=%u "
                          "raw_error=%+.3ftr window=%u samples=%u "
                          "mode=%s\n",
                          groovy_servo_state_name(st->servo.state),
                          groovy_servo_freeze_name(st->servo.freeze),
                          st->servo.ff_ppm,
                          st->servo.servo_ppm,
                          groovy_servo_term_ppm(&st->servo),
                          st->servo.latency_ns * 1e-6,
                          st->servo.error_frames,
                          (double)GROOVY_SERVO_SETPOINT_FRAMES,
                          (groovy_servo_term_ppm(&st->servo) - st->servo_ppm_previous),
                          st->servo.reanchors,
                          st->servo.toggles,
                          st->servo.ring_misses,
                          st->servo_acked,
                          st->servo.raw_error_frames,
                          st->servo.window_n,
                          st->servo.samples,
                          groovy_servo_mode_name(st->servo_mode));
                st->servo_ppm_previous = groovy_servo_term_ppm(&st->servo);
                st->servo_log_ns        = now_log_ns;
                st->servo_acked       = 0u;
            }
        }

        /* Followers: drain and re-announce EACH ON ITS OWN -- never mixed
         * with the master's above (a budget per receiver), and with no
         * influence on vsync_target (already frozen above). */
        for (unsigned i = 1u; i < st->n_rx; i++) {
            int want_i = 0;
            groovy_drain_statuses(st, &st->rx[i], now_ms, &want_i);
            if (want_i) groovy_reannounce_emit(st, &st->rx[i]);
        }

        /* ack_lost plus a sample, one receiver at a time, in rx[] order --
         * the master first. frame_id is incremented AFTER the send (see the
         * file header): at poll time, the last identifier SENT TO THE
         * MASTER equals frame_id-1 -- a DIFFERENT signal from a follower's,
         * which may have been skipped and whose last really-sent
         * identifier is rx->last_sent_id (maintained by groovy_fan_out).
         * use_shown selects the master's model (frame_echo=reassembled,
         * shown=displayed, both supplied by the real receiver) against a
         * follower's (its status only carries ITS OWN counter -- not
         * comparable to "shown", hence in_queue=0 and in_flight=in_net for
         * it, see groovy_lat_classify). */
        uint64_t now_ns = groovy_pacing_mono_ns();
        for (unsigned i = 0u; i < st->n_rx; i++) {
            struct groovy_receiver *rx = &st->rx[i];

            if (groovy_ack_link_on_tick(&rx->link, &rx->ack_state, now_ns))
                RARCH_LOG("[groovy_pacing] ack_lost target=%s role=%s\n",
                          rx->target, groovy_role_name(rx->role));

            int      has_sent;
            uint32_t sent;
            int      use_shown;
            if (i == 0u) {
                has_sent  = st->frame_id > 0u;
                sent      = st->frame_id - 1u;
                use_shown = 1;
            } else {
                has_sent  = rx->has_sent;
                sent      = rx->last_sent_id;
                use_shown = 0;
            }

            int64_t fl = 0, ne = 0, qu = 0;
            int classification = groovy_lat_classify(&rx->link, has_sent, sent,
                                             rx->ack_state.last_frame_echo,
                                             rx->ack_state.last_frame, use_shown,
                                             &fl, &ne, &qu);
            if (classification == 1) {
                rx->lat.hist_inflight[lat_hist_bucket(fl)]++;
                rx->lat.hist_net[lat_hist_bucket(ne)]++;
                rx->lat.hist_queue[lat_hist_bucket(qu)]++;
                rx->lat.n++;
            } else if (classification == -1) {
                rx->lat.anomalies++;
            }
            if (has_sent) {
                rx->lat.last_sent  = sent;
                rx->lat.last_echo  = rx->ack_state.last_frame_echo;
                rx->lat.last_shown = rx->ack_state.last_frame;
            }
        }

        uint64_t now_mono = now_ns;
        if (st->lat_win_start_ns == 0u) {
            st->lat_win_start_ns = now_mono;
        } else if (now_mono - st->lat_win_start_ns >= 1000000000ull) {
            /* One line PER RECEIVER, in rx[] order (master first), same
             * window for all -- printed even when n=0 (a declared follower
             * that returned nothing says so, rather than staying silent). */
            for (unsigned i = 0u; i < st->n_rx; i++) {
                struct groovy_receiver *rx = &st->rx[i];
                unsigned imin, ip50, ip95, imax;
                unsigned nmin, np50, np95, nmax;
                unsigned qmin, qp50, qp95, qmax;
                lat_hist_percentiles(rx->lat.hist_inflight, rx->lat.n, &imin, &ip50, &ip95, &imax);
                lat_hist_percentiles(rx->lat.hist_net,      rx->lat.n, &nmin, &np50, &np95, &nmax);
                lat_hist_percentiles(rx->lat.hist_queue,    rx->lat.n, &qmin, &qp50, &qp95, &qmax);

                RARCH_LOG("[groovy-lat] target=%s role=%s n=%u inflight(min/p50/p95/max)=%u/%u/%u/%u "
                          "net=%u/%u/%u/%u queue=%u/%u/%u/%u backlog_max=%u "
                          "anomalies=%u sent=%u echo=%u shown=%u rejects=%u saturations=%u "
                          "entrees=%u announces=%u/%u\n",
                          rx->target, groovy_role_name(rx->role),
                          rx->lat.n,
                          imin, ip50, ip95, imax,
                          nmin, np50, np95, nmax,
                          qmin, qp50, qp95, qmax,
                          rx->lat.ackq_max, rx->lat.anomalies,
                          rx->lat.last_sent, rx->lat.last_echo, rx->lat.last_shown,
                          rx->rejects, rx->saturations, s_groovy_input_acc[i],
                          rx->announces_ok, rx->announces_failed);

                memset(rx->lat.hist_inflight, 0, sizeof(rx->lat.hist_inflight));
                memset(rx->lat.hist_net,      0, sizeof(rx->lat.hist_net));
                memset(rx->lat.hist_queue,    0, sizeof(rx->lat.hist_queue));
                rx->lat.n         = 0u;
                rx->lat.ackq_max  = 0u;
                rx->lat.anomalies = 0u;
                rx->rejects        = 0u;
                rx->saturations       = 0u;
                s_groovy_input_acc[i] = 0u;
            }
            st->lat_win_start_ns = now_mono;
        }

        /* Input channel: periodic trace of k, unchanged by this design.
         * Distinct log prefix from the block above, so as not to mix two
         * samples in the same log. Reuses the now_mono already read above
         * -- the same one-second window, not a second clock wakeup. Always
         * on, silent when nothing happened: any future regression is
         * visible, same philosophy as the receiver side's depth_max. */
        if (groovy_input_lat_should_report(&s_input_lat, now_mono)) {
            unsigned kmin, kp50, kp95, kmax;
            groovy_input_lat_percentiles(&s_input_lat, &kmin, &kp50, &kp95, &kmax);
            RARCH_LOG("[groovy-input-lat] n=%u k(min/p50/p95/max)=%u/%u/%u/%u\n",
                      s_input_lat.n, kmin, kp50, kp95, kmax);
            groovy_input_lat_reset(&s_input_lat);
        }
    }

    /* Deferred CMD_SWITCHRES from re-announces, BEFORE the fan-out of a
     * LATER frame -- the same position relative to the burst as the
     * immediate CMD_SWITCHRES above (in the { vsync_target ... } block that
     * just closed): only the gaps with CMD_INIT and with the hookup byte
     * that precedes it vary between the two paths -- never the position
     * relative to the burst, which stays fixed on both sides (IN-14: this
     * comment used to cite only the CMD_INIT gap, as if only one varied;
     * the hookup byte goes out right after CMD_INIT -- groovy_reannounce_emit
     * -- so its gap at re-announce time follows the same deferral as
     * CMD_SWITCHRES's).
     * Placed HERE, outside the if/else 480i/240p chain below, so that BOTH
     * modes flush their deferrals -- and AFTER this frame's status drain
     * (groovy_drain_statuses, in the block that just closed) so that the
     * cancellation test (rx->ack_state.last_frame_echo) reads the freshest
     * state. No effect when GROOVY_REANNOUNCE_SPACING_US is 0 (no send is
     * ever deferred); no effect on the frame that just set a deferral
     * (switchres_deferred_turn, checked in
     * groovy_reannounce_flush_deferred). Before this fix, this call used to
     * live AFTER the frame's fan-out -- stuck to the tail of its own
     * burst. */
    for (unsigned i = 0u; i < st->n_rx; i++)
        groovy_reannounce_flush_deferred(st, &st->rx[i], groovy_pacing_mono_ns());

    /* BLIT-vs-SWITCHRES dims guard (quick-260712-2b5 — defense in depth,
     * blueprint requirement #3, spike 006 desync). NEVER ship a BLIT whose
     * dims contradict the last announced CMD_SWITCHRES: while hysteresis has
     * not yet let a new dim through, av_cache still holds the OLD announced
     * dims and a mismatched BLIT would desync the daemon's
     * expected_frame_bytes (unknown_opcode spam). Soft-skip keeps the session
     * alive; the hysteresis blocks above emit the switchres within
     * GROOVY_DIM_HYSTERESIS_FRAMES and frames flow again. Applies to BOTH
     * the software (Beetle) and HW-native (SwanStation) capture paths.
     * Warn log is throttled to dim changes (singleton driver — file-scope
     * static is safe, same rationale as s_last_valid_fps). */
    if (   !st->av_cache.valid
        || st->av_cache.width  != send_w
        || st->av_cache.height != send_h) {
        static unsigned s_guard_warn_w = 0u, s_guard_warn_h = 0u;
        if (s_guard_warn_w != send_w || s_guard_warn_h != send_h) {
            RARCH_WARN("[groovy] BLIT dims %ux%u != announced %ux%u — skipping frame\n",
                       send_w, send_h,
                       st->av_cache.width, st->av_cache.height);
            s_guard_warn_w = send_w;
            s_guard_warn_h = send_h;
        }
        return true;   /* soft-skip, session alive */
    }

    /* Witness of what actually goes out on the wire — after every guard, so
     * only frames really emitted get sampled. No effect when
     * GROOVY_DUMP_FRAME_PATH is absent. */
    groovy_dump_frame_ppm(frame, send_w, send_h);

    /* Timer for the emission path — brackets the gm_send_blit* calls below,
     * compression included since it lives inside them (gm_send_blit /
     * gm_send_blit_field compress before sending the header, see
     * libgm/src/gm_proto.c). What this measurement answers, and what it
     * doesn't: it gives the time the runloop thread spends in the emission
     * path, compression AND sending combined. The PURE cost of compression
     * is obtained by the difference between a run with it on and a run with
     * it off — don't add a second internal timer inside libgm to isolate
     * it. */
    uint64_t emit_t0 = groovy_pacing_mono_ns();
    bool     emit_ok = true;
    /* Lever b6: the queue is only flushed after a fan-out that REALLY
     * played -- never in the WAN-03 skip branch, never when a 480i malloc
     * failed and groovy_fan_out was therefore never called. See the flush
     * site further down. */
    bool     fan_out_done = false;

    /* WAN-03 diagnostic hook. Placed AFTER every geometry guard and AFTER
     * the frame witness, so the skipped frame is one that would have
     * REALLY gone out. frame_id is NOT touched here: it advances at the
     * bottom of the function like for any other frame, and that is exactly
     * the deliberate gap WAN-03 must prove tolerable. The expected bench
     * signal is a rise in [groovy-lat]'s net= percentile (the sent - echo
     * gap), NOT a rise in anomalies. */
    if (groovy_skip_should_skip(st->skip_every, st->frame_id)) {
        st->skipped_frames++;
    /* Phase 3.2 regime-aware emit. */
    } else if (st->current_mode == GROOVY_MODE_480I
               && st->field_mode == GROOVY_FIELD_PER_FRAME) {
        /* GroovyMAME path: ONE half-frame per rendered frame, as a
         * half-height CMD_BLIT_VSYNC (0x06), under consecutive frame_ids.
         * The receiver already pairs these half-frames by frame_id + 1
         * adjacency (COMPAT-03) and derives parity from frame_id & 1 --
         * nothing to change on the receiver side.
         *
         * frame_id is NOT incremented here: the single site at the bottom
         * of the function advances by exactly 1 per call, and
         * groovy_push_video is called once per rendered frame at 60 Hz.
         * frame_id's cadence is therefore EXACTLY the same as before this
         * feature. Do not add a second increment thinking it would catch
         * up on the two previous fields: that would break the continuity
         * GroovyMAME expects. */
        uint8_t field = groovy_field_for_frame(&st->rx[0].ack_state, st->frame_id);
        size_t row_bytes   = (size_t)send_w * 3U;
        size_t half_h      = (size_t)(send_h / 2U);
        size_t field_bytes = row_bytes * half_h;

        uint8_t *fld = (uint8_t *)malloc(field_bytes);
        bool ok = false;
        if (fld) {
            for (size_t r = 0; r < half_h; ++r)
                memcpy(fld + r * row_bytes,
                       frame + (2 * r + (size_t)field) * row_bytes, row_bytes);
            ok = groovy_fan_out(st, GROOVY_EMIT_HALF, vsync_target, field,
                                fld, NULL, field_bytes);
            fan_out_done = true;
        }
        free(fld);
        /* Only count what really went OUT: a failed malloc or a
         * gm_send_blit_half returning -1 must not inflate fields=%u/%u
         * (IN-06). */
        if (ok)
            st->fields_sent[field]++;
        emit_ok = ok;
    } else if (st->current_mode == GROOVY_MODE_480I) {
        /* Split swap_buf (send_w × send_h × 3 RGB888) into two field buffers.
         * Field 0 = rows 0, 2, 4, ... (h/2 rows). Field 1 = rows 1, 3, 5, ...
         * Per-row size = send_w * 3 bytes. Total per field = send_w * (send_h/2) * 3.
         *
         * Optimisation note: this allocates two small buffers per 480i frame.
         * If push_video shows allocation pressure during HIL, the cleaner path
         * is to reuse a pre-allocated st->field0_buf + st->field1_buf sized to
         * MAX_FRAME at init. Plan-08 HIL evidence will tell. For now, simplicity
         * over micro-optimization — the heap calls land in a single thread
         * (record_groovy is video_threaded=false per EMT-05) and 480i frame
         * dimensions are bounded.
         */
        size_t row_bytes = (size_t)send_w * 3U;
        size_t half_h = (size_t)(send_h / 2U);
        size_t field_bytes = row_bytes * half_h;

        uint8_t *fld0 = (uint8_t *)malloc(field_bytes);
        uint8_t *fld1 = (uint8_t *)malloc(field_bytes);
        bool ok = false;
        if (fld0 && fld1) {
            for (size_t r = 0; r < half_h; ++r) {
                memcpy(fld0 + r * row_bytes, frame + (2 * r)     * row_bytes, row_bytes);
                memcpy(fld1 + r * row_bytes, frame + (2 * r + 1) * row_bytes, row_bytes);
            }
            ok = groovy_fan_out(st, GROOVY_EMIT_FIELDS, vsync_target, 0u,
                                fld0, fld1, field_bytes);
            fan_out_done = true;
        }
        free(fld0);
        free(fld1);
        emit_ok = ok;
    } else {
        /* GROOVY_MODE_240P_SUPER_RES — unchanged progressive path. */
        emit_ok = groovy_fan_out(st, GROOVY_EMIT_FULL, vsync_target, 0u,
                                 frame, NULL, need);
        fan_out_done = true;
    }

    /* Lever b6: the queue is only flushed after a fan-out that REALLY
     * played. A skipped frame (WAN-03), one refused by the dims guard, a
     * duplicate, or one whose 480i split failed, leaves the audio waiting
     * for the NEXT burst -- never silently dropped.
     *
     * Conversely, a fan-out that plays flushes the queue EVEN IF a follower
     * was skipped in it by saturation: that follower will never receive
     * this audio, and rx->audio_hold_not_played says so (see the skip
     * branch). `played` counts blocks OUT OF THE QUEUE, never "received by
     * each receiver" -- the two numbers diverge as soon as a follower is
     * skipped. */
    if (st->audio_hold_mode == GROOVY_AUDIO_HOLD_ON && fan_out_done) {
        st->audio_hold.played += (uint64_t)st->audio_hold.n_blocks;
        groovy_audio_hold_clear(&st->audio_hold);
    }

    /* The deferred-CMD_SWITCHRES flush used to live HERE (AFTER the frame's
     * fan-out). Moved further up, before the if/else 480i/240p chain --
     * see the comment at the new call site. Do not put it back here: it
     * would stick again to the tail of the burst that just went out
     * instead of being separated from the next one. */

    /* Sampled every 600 frames (ten seconds at 60 fps): one readable log
     * line, never one per frame (per-frame telemetry disturbs bench
     * judgment). */
    {
        uint64_t emit_ns = groovy_pacing_mono_ns() - emit_t0;
        st->emit_ns_total += emit_ns;
        if (emit_ns > st->emit_ns_max)
            st->emit_ns_max = emit_ns;
        if (++st->emit_samples >= 600u) {
            /* The average turns slightly optimistic while the WAN-03 hook
             * is active -- a skipped frame measures ~0 on this timer. No
             * consequence for a diagnostic hook. */
            /* dups= is a DIFFERENCE on libgm's monotonic counter: a window
             * with no frame_dup shows 0, never a value inherited from the
             * previous window. */
            uint32_t dups_now = gm_frame_dup_count(st->gm);
            uint32_t dups_fen = dups_now - st->dup_count_base;
            /* fields=%u/%u below: the two parity counters for lever 2. In a
             * settled mode they should stay close -- a wide gap would
             * signal a stuck parity, hence a stale ACK. No effect when
             * field_mode == GROOVY_FIELD_BOTH (both count 0, the branch
             * that increments them is never taken). */
            RARCH_LOG("[groovy-emit] compression=%s frame_dup=%s n=%u average=%.1f us "
                      "worst=%.1f us skipped=%u dups=%u fields=%u/%u "
                      "reannounces=%u/%u reannounces_failed=%u hooked=%u input_resets=%u before_master=%u\n",
                      groovy_compression_mode_name(st->compression_mode),
                      groovy_frame_dup_mode_name(st->frame_dup_mode),
                      st->emit_samples,
                      (double)st->emit_ns_total / (double)st->emit_samples / 1000.0,
                      (double)st->emit_ns_max / 1000.0,
                      st->skipped_frames, dups_fen,
                      st->fields_sent[0], st->fields_sent[1],
                      s_reannounce_sent, s_reannounce_init_only,
                      s_reannounce_failure,
                      s_reannounce_hello, s_reannounce_input_reset,
                      st->before_master);
            st->emit_ns_total = 0u;
            st->emit_ns_max   = 0u;
            st->emit_samples  = 0u;
            st->skipped_frames = 0u;
            st->dup_count_base = dups_now;
            st->fields_sent[0] = 0u;
            st->fields_sent[1] = 0u;
            st->before_master  = 0u;
        }
    }

    if (!emit_ok)
        return false;

    /* The servo's error is SUB-FRAME, so it needs a latency in TIME.
     * [groovy-lat]'s inflight is an integer COUNT (sent - shown), not a
     * duration. This timestamps the frame that just went out to the master
     * -- st->frame_id still carries ITS identifier, the one the ack will
     * echo back in `frame` (offset 6). Timestamping after the increment
     * would shift every latency by a whole frame. No wire change: the ring
     * lives entirely on the emitter side. */
    groovy_servo_note_send(&st->servo, st->frame_id, groovy_pacing_mono_ns());

    st->frame_id++;
    return true;
}

/* ---------------------------------------------------------------------------
 * vtable function: groovy_push_audio  (step 2 of the audio-to-receiver
 * design)
 *
 * RetroArch hands over signed 16-bit interleaved stereo PCM here, at the
 * core's frequency, in blocks of at most 1024 pairs
 * (AUDIO_CHUNK_SIZE_NONBLOCKING >> 1, audio/audio_driver.c). `frames` counts
 * left/right pairs, not bytes.
 *
 * push_audio's three call sites are on the runloop thread, the same one as
 * push_video: video_threaded = false is mandatory for this driver (EMT-05).
 * libgm's contract — a single thread calls gm_send_* — is therefore
 * respected without a lock.
 *
 * That also holds on the wire: gm_send_blit sends the header AND all its
 * chunks before returning, so a video frame goes out as one block and an
 * audio block can only slot in BETWEEN two frames. The receiver's
 * reassembler depends on this.
 *
 * Data volume at 44100 stereo: 176 KB/s, about 2940 bytes per frame at 60
 * Hz, i.e. two datagrams — negligible next to the ~330 KB of video.
 *
 * Record driver return convention: true when there is no error. A block
 * deliberately dropped returns true.
 * ------------------------------------------------------------------------- */
static bool groovy_push_audio(void *data, const struct record_audio_data *audio)
{
    groovy_state_t *st = (groovy_state_t *)data;

    /* No frequency announced, or no receiver with audio on: nothing goes
     * out. The guard no longer compares st->audio_mode -- a declared
     * follower has audio on by default, whatever the master's
     * GROOVY_AUDIO. groovy_any_audio_on() covers the master (rx[0].audio_on,
     * set from st->audio_mode at opening) AND every follower. */
    if (!st || !st->gm || st->audio_rate_code == 0u || !groovy_any_audio_on(st))
        return true;
    if (!audio || !audio->data || audio->frames == 0u)
        return true;

    /* Fast-forward: drop, like the video (quick-260712-aye). The core
     * produces audio much faster than real time and flooding the receiver
     * makes no sense — it has nothing to play it faster with. */
    if (runloop_get_flags() & RUNLOOP_FLAG_FASTMOTION)
        return true;

    const int16_t *pcm    = (const int16_t *)audio->data;
    size_t         frames = audio->frames;

    /* Wire ratio. The wire must carry the exact NATIVE frequency (44100 for
     * a PS1 core) per REAL second, whatever RetroArch's skew does and
     * whatever the servo does.
     *
     * skewed_input = audio_state_get_ptr()->input: the input rate RetroArch
     * has set (44177.71 Hz on Chrono Cross, for instance). This is NOT
     * params->samplerate, which holds the core's native frequency (44100) --
     * the two exist and are not interchangeable.
     *
     * The core's speed varies as 1/term, so the real input rate equals
     * skewed_input / term, and the output/input ratio equals
     *     native * term / skewed_input.
     * An earlier note wrote it as "native / (skewed input x term)": that is
     * the same formula when "term" there denotes the core's SPEED factor,
     * which equals 1/term in the sense used elsewhere in this driver. A
     * single value is stored; its inverse is taken here. The sign is fixed
     * by a test named in test_groovy_audio.py -- do not flip it without
     * re-reading that test. */
    double resample_ratio = 1.0;
    if (st->audio_ratio_on)
    {
        const audio_driver_state_t *ast = audio_state_get_ptr();
        const double skewed_input = ast ? (double)ast->input : 0.0;
        const double native_hz         = (double)groovy_audio_rate_hz(st->audio_rate_code);
        const double term         = audio_driver_get_groovy_term();
        if (skewed_input > 0.0 && native_hz > 0.0)
            resample_ratio = native_hz * term / skewed_input;
    }
    if (!groovy_resample_set_ratio(&st->audio_rs, resample_ratio))
    {
        /* Ratio refused (out of bounds or not finite): the old one is kept.
         * Traced ONCE per session, like ack_lost -- a trace per audio block
         * would flood the log at 60 blocks a second. */
        if (!st->ratio_refused_logged) {
            RARCH_ERR("[groovy-servo] wire ratio refused: %.9f "
                      "(keeping the old one)\n", resample_ratio);
            st->ratio_refused_logged = 1;
        }
    }

    /* Resampling when the core's native frequency isn't in the protocol's
     * enumeration, OR when the fractional ratio differs from 1.0 -- the
     * plain src_hz != dst_hz comparison alone is no longer enough, it is
     * ALWAYS false on this project (PS1 44100 == wire 44100). */
    if (st->audio_rs.src_hz != st->audio_rs.dst_hz || st->audio_rs.ratio != 1.0) {
        size_t need = groovy_resample_capacity_ratio(frames,
                                                    st->audio_rs.src_hz,
                                                    st->audio_rs.dst_hz,
                                                    st->audio_rs.ratio);
        if (need > st->audio_buf_frames) {
            int16_t *grown = (int16_t *)realloc(st->audio_buf,
                                                need * 2u * sizeof(int16_t));
            if (!grown)
                return true;            /* block lost, session alive */
            st->audio_buf        = grown;
            st->audio_buf_frames = need;
        }
        frames = groovy_resample(&st->audio_rs, pcm, frames,
                                 st->audio_buf, st->audio_buf_frames);
        if (frames == 0u)
            return true;                /* nothing ripe to emit this round */
        pcm = st->audio_buf;
    }

    /* The size CMD_AUDIO announces fits a u16: gm_send_audio refuses
     * anything past 65535 bytes. A RetroArch block is at most 1024 pairs,
     * i.e. 4096 bytes, and 2.2 times that after upward resampling — the
     * margin is sevenfold. The splitting below is a safety belt, not an
     * expected path. */
    const size_t max_frames = GROOVY_AUDIO_MAX_FRAMES;    /* 32768 bytes in stereo */

    /* Lever b6: hold instead of send. What is deposited is the buffer
     * ALREADY resampled -- the lever only changes the send INSTANT, never a
     * single byte. Delivery lives in groovy_fan_out (the same gesture as
     * the next frame's image burst). */
    if (st->audio_hold_mode == GROOVY_AUDIO_HOLD_ON) {
        if (!groovy_audio_hold_deposit(&st->audio_hold, pcm, frames, st->audio_channels)
            && st->audio_hold.lost == 1u)
            RARCH_WARN("[groovy-audio-hold] an audio block could not be held "
                       "(queue full, different format, or memory); the end-of-session "
                       "report gives the count\n");
        st->audio_blocks++;
        st->audio_frames += (uint64_t)frames;
        /* Under this mode, this function NEVER goes through the
         * per-receiver loop further down -- so it never reaches the line
         * that refreshes st->audio_errors from rx[0].audio_errors either.
         * Without this mirror here, a gm_send_audio failure at DELIVERY
         * time (groovy_hold_flush_vers, called from groovy_fan_out, never
         * from this function) stays counted in rx[0].audio_errors but never
         * reaches the "[groovy] audio sent: ..." report again
         * (groovy_finalize reads st->audio_errors, not rx[0].audio_errors):
         * the MASTER's failures would become invisible, even though the
         * per-receiver count (rx[i].audio_errors, i>=1) is still reported
         * for each follower. The mirror here restores parity with the path
         * that doesn't use the lever (the line at the bottom of this
         * function), at the cost of a lag of at most one frame -- the same
         * approximation as the rest of this counter. */
        st->audio_errors = st->rx[0].audio_errors;
        return true;
    }

    /* Loop PER RECEIVER: the resampled buffer (pcm/frames) is shared, only
     * the send GATE differs -- nothing in gm_send_audio is
     * receiver-specific. A send failure towards one receiver only abandons
     * the REST of this block FOR THAT RECEIVER ALONE: the other receivers,
     * and the video, keep going normally. */
    for (unsigned i = 0u; i < st->n_rx; i++) {
        if (!st->rx[i].audio_on)
            continue;
        size_t sent = 0u;
        while (sent < frames) {
            size_t chunk = frames - sent;
            if (chunk > max_frames) chunk = max_frames;
            if (gm_send_audio(st->rx[i].gm, pcm + sent * 2u, chunk,
                              st->audio_channels) != 0) {
                if (st->rx[i].audio_errors++ == 0u) {
                    if (i == 0u)
                        RARCH_WARN("[groovy] gm_send_audio refused a block "
                                   "of %u pairs; audio will stop where it "
                                   "drops.\n", (unsigned)chunk);
                    else
                        RARCH_WARN("[groovy] gm_send_audio refused a block "
                                   "towards %s; this follower's audio will "
                                   "stop where it drops.\n",
                                   st->rx[i].target);
                }
                break;   /* abandon the rest of the block for THIS receiver only */
            }
            sent += chunk;
        }
    }
    st->audio_errors = st->rx[0].audio_errors;   /* the master's counter, for groovy_finalize */

    st->audio_blocks++;
    st->audio_frames += (uint64_t)frames;
    return true;
}

/* ---------------------------------------------------------------------------
 * vtable function: groovy_finalize
 *
 * Called by RetroArch just before groovy_free, signalling that the record
 * session is ending cleanly (user stopped recording, content unloaded, etc.).
 * We send CMD_CLOSE here so the daemon gets a clean teardown signal.
 *
 * This comment used to say "gm_close (called in groovy_free) will also
 * attempt gm_send_close" -- false, gm_close never sends CMD_CLOSE
 * (libgm/src/gm.c has no gm_send_close call in it). This function's
 * explicit gm_send_close() calls below are the ONLY place CMD_CLOSE is
 * sent, on the wire form (classic or padded) fixed on each handle at open
 * by gm_set_padding. groovy_finalize -> groovy_free is still the expected
 * calling sequence: finalize signals the remote, free only cleans up local
 * resources (sockets, memory) and sends nothing.
 * ------------------------------------------------------------------------- */
static bool groovy_finalize(void *data)
{
    groovy_state_t *st = (groovy_state_t *)data;
    if (!st || !st->gm)
        return true;

    /* Count of audio bytes sent, to compare against what the receiver
     * counted. This is step 3's objective measure: bytes sent against
     * bytes received, without having to sniff the wire. */
    if (st->audio_blocks > 0u)
        RARCH_LOG("[groovy] audio sent: %llu blocks, %llu pairs, "
                  "%llu bytes, %llu refused\n",
                  (unsigned long long)st->audio_blocks,
                  (unsigned long long)st->audio_frames,
                  (unsigned long long)(st->audio_frames
                                       * st->audio_channels * 2u),
                  (unsigned long long)st->audio_errors);

    /* Box report (quick 260901-po3), on the same model as the audio report
     * above. */
    if (st->downsample_dims_mismatch > 0u)
        RARCH_LOG("[groovy] downsample: %llu frames refused (dims not "
                  "divisible by %u)\n",
                  (unsigned long long)st->downsample_dims_mismatch,
                  st->downsample_n);

    /* Lever b6: closing report, BEFORE any gm_send_close -- it must
     * describe the queue's state at the moment the session ends.
     * at_close: the lever's SECOND named cost -- what remains in the queue
     * is NOT sent here (no send at close time), it is lost and counted. */
    if (st->audio_hold.deposited > 0u || st->audio_hold_mode == GROOVY_AUDIO_HOLD_ON) {
        uint64_t not_played = 0u;
        for (unsigned i = 0u; i < st->n_rx; i++)
            not_played += st->rx[i].audio_hold_not_played;
        RARCH_LOG("[groovy-audio-hold] report: held=%llu played=%llu lost=%llu "
                  "not_played=%llu at_close=%u\n",
                  (unsigned long long)st->audio_hold.deposited,
                  (unsigned long long)st->audio_hold.played,
                  (unsigned long long)st->audio_hold.lost,
                  (unsigned long long)not_played,
                  st->audio_hold.n_blocks);
        for (unsigned i = 0u; i < st->n_rx; i++)
            if (st->rx[i].audio_hold_not_played > 0u)
                RARCH_LOG("[groovy-audio-hold] not delivered to %s: %llu blocks (receiver "
                          "skipped by saturation at fan-out time)\n",
                          st->rx[i].target,
                          (unsigned long long)st->rx[i].audio_hold_not_played);
    }

    gm_send_close(st->gm);

    /* Followers: CMD_CLOSE to each, plus an audio-failure report per
     * follower -- on the same model as the master's audio report above, one
     * line PER follower that saw at least one failure, never one per
     * failure (same discipline as the box report above). */
    for (unsigned i = 1u; i < st->n_rx; i++) {
        if (st->rx[i].gm)
            gm_send_close(st->rx[i].gm);
        if (st->rx[i].audio_errors > 0u)
            RARCH_LOG("[groovy] follower %s audio: %llu refused\n",
                      st->rx[i].target,
                      (unsigned long long)st->rx[i].audio_errors);
    }

    return true;
}

/* ---------------------------------------------------------------------------
 * vtable function: groovy_push_av_info  (Plan 03-06, EMT-03)
 *
 * Called by runloop.c at SET_SYSTEM_AV_INFO and SET_GEOMETRY dispatch sites
 * (hook added by 0001-record-driver-geometry-hook.patch). Computes a
 * gm_modeline from the incoming retro_system_av_info and emits CMD_SWITCHRES
 * via gm_send_switchres.
 *
 * HIL-3-08 root cause (Phase 3.1): libretro convention is that SET_GEOMETRY
 * only updates `geometry`, not `timing`. The runloop.c SET_GEOMETRY hook
 * (patch line 2902) passes `av_info` whose `timing.fps` is zero if no
 * SET_SYSTEM_AV_INFO has fired yet (calloc'd / default-initialised). The
 * previous code returned `false` (rejecting the callback) when fps <= 0,
 * but HIL evidence showed pclock=0/refresh=0 packets reaching the daemon,
 * meaning the bug was also present in callbacks where fps appears valid
 * but timing was stale from a prior session re-init.
 *
 * Fix (Phase 3.1 v1, cache approach): maintain a `last_valid_fps` cache.
 * When fps > 0, update the cache and use it. When fps <= 0 (SET_GEOMETRY-only
 * callback), reuse last_valid_fps if available; otherwise skip emission
 * (return true — do NOT return false, which would signal a broken session
 * to RetroArch).
 *
 * Bug A fix (Phase 3.1 v2, HIL re-run): cache lives in file-scope
 * `s_last_valid_fps`, NOT a groovy_state_t field. That HIL run showed
 * RetroArch fires CMD_EVENT_REINIT (e.g. on disc load) → groovy_free +
 * groovy_new wipes per-instance fields → next SET_GEOMETRY-only callback
 * sees cached=0 and emits malformed CMD_SWITCHRES. File-scope static survives
 * the cycle. RetroArch's record driver is singleton, so this is safe.
 *
 * Option G: daemon forges drm_mode_modeinfo source-exact from wire data —
 *   INCLUDING porches (h_begin → hsync_start, h_end → hsync_end, ...).
 *   Porches are derived from blanking in groovy_modeline.h (Bug B fix); the
 *   prior assumption "daemon tolerates zero porches" was wrong.
 *
 * STRIDE mitigations:
 *   T-3.1-01 (zero-fps from libretro core): fps-cache + skip guard — zero
 *     or negative fps callbacks never produce a CMD_SWITCHRES on the wire
 *   T-3-12 (bad av_info): rejects base_width==0, base_height==0
 *   T-3-13 (spam): dedupe — identical successive callbacks do not emit
 *
 * Thread safety: called from the RetroArch runloop thread only (video_threaded
 *   must be false per EMT-05). av_cache lives in groovy_state_t (instance
 *   scope); s_last_valid_fps is file-scope static — singleton driver, so no
 *   concurrent access.
 * ------------------------------------------------------------------------- */
static bool groovy_push_av_info(void *data, const struct retro_system_av_info *av)
{
    groovy_state_t *st = (groovy_state_t *)data;
    if (!st || !st->gm || !av)
        return false;

    /* Diagnostic log on EVERY call — provides direct HIL evidence for the
     * next run so we can confirm the hypothesis empirically.
     * T-3.1-02: logs only geometry/fps tuples; no PII, no auth tokens.
     *
     * Deliberately DIFFERENT prefix from "[groovy] push_av_info" -- that
     * other prefix is what R_AVINFO (client/bilan-999-33.py's
     * first-announced-geometry check) takes for a geometry REALLY
     * announced. This line traces EVERY call, including the ones the
     * guards further down reject (zero dims, fps=0 with no cache,
     * non-divisible box, duplicate) -- so it must never be able to match
     * R_AVINFO. The line that matches R_AVINFO is further down, right
     * before the real call to groovy_switchres_all. */
    RARCH_LOG("[groovy-diag] push_av_info: %ux%u @ %.6f fps (cached=%.6f)\n",
              av->geometry.base_width, av->geometry.base_height,
              av->timing.fps, s_last_valid_fps);

    /* T-3-12: defensive — reject zero geometry (non-recoverable) */
    if (av->geometry.base_width  == 0u ||
        av->geometry.base_height == 0u)
        return false;

    /* Announced geometry = core geometry divided by the box factor (quick
     * 260901-po3). The two paths — this one and groovy_push_video — MUST
     * announce the same grid, otherwise the BLIT/SWITCHRES guard refuses
     * every frame (design finding 4). */
    unsigned gw = av->geometry.base_width;
    unsigned gh = av->geometry.base_height;
    if (st->downsample_n > 1u) {
        if (!groovy_downsample_dims_ok(gw, gh, st->downsample_n)) {
            RARCH_WARN("[groovy] downsample_dims_mismatch (av_info): %ux%u not "
                       "divisible by %u — no announcement\n",
                       gw, gh, st->downsample_n);
            return true;   /* announce nothing rather than announce something wrong */
        }
        gw /= st->downsample_n;
        gh /= st->downsample_n;
    }

    /* Determine effective fps using per-instance last-known-fps cache.
     *
     * Case 1: fps_in > 0 — SET_SYSTEM_AV_INFO or equivalent. Update cache. */
    double fps_in = av->timing.fps;
    double effective_fps;

    if (fps_in > 0.0) {
        /* Valid fps from this callback — update the cache. */
        s_last_valid_fps = fps_in;
        effective_fps = fps_in;
    } else if (s_last_valid_fps > 0.0) {
        /* fps_in == 0: SET_GEOMETRY-only callback, but we have a cached fps.
         * Build synthetic av_info reusing last-known-good fps. */
        effective_fps = s_last_valid_fps;
    } else {
        /* fps_in == 0 AND no cached fps yet: cannot compute a valid modeline.
         * Skip emission silently. Return true (not false) — this is not a
         * broken session, just a premature callback before any
         * SET_SYSTEM_AV_INFO has established fps. */
        RARCH_LOG("[groovy] push_av_info: skipping — fps=0 and no cached fps yet "
                  "(geometry=%ux%u)\n",
                  av->geometry.base_width, av->geometry.base_height);
        return true;
    }

    /* T-3-13: dedupe — skip CMD_SWITCHRES if (w, h, effective_fps) unchanged.
     * Keyed on effective_fps (what was actually used), not raw fps_in, so a
     * SET_GEOMETRY callback reusing the cached fps doesn't re-emit when the
     * geometry itself is unchanged. Compared on gw/gh (DIVIDED dims, quick
     * 260901-po3) — consistent with what is actually announced and cached
     * below. */
    if (st->av_cache.valid
        && st->av_cache.width  == gw
        && st->av_cache.height == gh
        && st->av_cache.fps    == effective_fps)
        return true;   /* identical — no-op, report success */

    st->av_cache.width  = gw;
    st->av_cache.height = gh;
    st->av_cache.fps    = effective_fps;
    st->av_cache.valid  = true;

    /* Unconditional synthetic copy (quick 260901-po3): carries the DIVIDED
     * dims (gw/gh) and the effective fps. At `off`, gw/gh equal the
     * original dims and effective_fps equals fps_in when fps_in > 0 — the
     * resulting modeline is then identical to before this feature. */
    struct retro_system_av_info av_use;
    memcpy(&av_use, av, sizeof(av_use));
    av_use.geometry.base_width  = gw;
    av_use.geometry.base_height = gh;
    av_use.timing.fps           = effective_fps;

    gm_modeline mode;
    compute_modeline_from_av_info(&av_use, &mode);

    /* THE line that R_AVINFO really reads, moved here -- after the two
     * guards above (zero dims, fps=0 with no cache), after the box
     * division check, and after the dedupe (T-3-13) -- so that it can
     * never match a call that led to no send attempt at all. Not
     * re-emitting on a duplicate doesn't violate the invariant: the
     * first-announced-geometry check only reads the FIRST line, and a
     * duplicate's geometry has already been announced by an earlier call.
     * RAW dims (av->geometry.*), not the divided gw/gh -- drift under
     * supersampling stays out of this fix's scope. */
    RARCH_LOG("[groovy] push_av_info: %ux%u @ %.6f fps (cached=%.6f)\n",
              av->geometry.base_width, av->geometry.base_height,
              av->timing.fps, s_last_valid_fps);

    return groovy_switchres_all(st, &mode);
}

/* ---------------------------------------------------------------------------
 * Vtable registration.
 *
 * Field names use C99 designated initializers. push_av_info is registered
 * as the last field (Plan 03-06), consistent with its append-only position
 * in the record_driver_t struct added by 0001 patch.
 *
 * Field name `ident` is the driver's printable name, used by RetroArch when
 * the user selects `record_driver = "groovy"` in retroarch.cfg and when
 * `retroarch.exe --help` lists available record drivers.
 *
 * NOTE on field name `new_`: Verified at 4a82976 record/record_driver.h —
 * the actual field is named `new_` (trailing underscore) to avoid collision
 * with the C++ keyword `new`. The struct is in a C header but RetroArch is
 * compiled as C++ on some platforms; the trailing underscore is intentional.
 * ------------------------------------------------------------------------- */
const record_driver_t record_groovy = {
    .init         = groovy_new,
    .free         = groovy_free,
    .push_video   = groovy_push_video,
    .push_audio   = groovy_push_audio,
    .finalize     = groovy_finalize,
    .ident        = "groovy",
    .push_av_info = groovy_push_av_info,   /* + Plan 03-06 EMT-03 */
};
