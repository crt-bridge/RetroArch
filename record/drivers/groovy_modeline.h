/* SPDX-License-Identifier: GPL-3.0-or-later */
/* groovy_modeline.h — shared compute_modeline_from_dims logic.
 *
 * Shared between:
 *   - emitter/retroarch/src/record_groovy.c (runtime path)
 *   - libgm/test/groovy_test_bridge.c (unit-test path)
 *
 * Keeping the math in a header ensures the pytest exercises the EXACT same
 * computation that ships in the RetroArch fork — no divergence risk.
 *
 * Option G semantics (receiver/blit.c):
 *   - The daemon forges a drm_mode_modeinfo source-exact from h_total, v_total,
 *     pclock_mhz, AND porches (h_begin → hsync_start, h_end → hsync_end, etc.).
 *   - DRM kernel constraint: hdisplay ≤ hsync_start ≤ hsync_end ≤ htotal
 *     (and same for vertical). Porches MUST satisfy this ordering or
 *     drmModeSetCrtc returns EINVAL — even when pclock is valid.
 *   - SAFETY-01/02/04 envelope checks remain the final hardware guard, but
 *     they only check hfreq/vfreq, not porch ordering.
 *
 * Bug B fix: the prior "zero porches, daemon tolerates" assumption was
 * wrong — the daemon never tolerated zeros, that comment block was
 * aspirational. Hardware-in-the-loop evidence showed
 * runtime_setcrtc_failed errno=22 on every CMD_SWITCHRES with valid pclock
 * but porches=0. Porches are now derived from the blanking budget per the
 * standard split: front porch ≈ ¼ blanking, sync ≈ ½ blanking, back porch
 * ≈ ¼ blanking. Integer truncation guarantees end ≤ total.
 *
 * Default blanking rules (matching MAME/GroovyMAME arcade_15 conventions):
 *   h_total = round_even(h_active * 1.27)   ≈ 25% horizontal blanking
 *   v_total = round_even(v_active * 1.09)   ≈  9% vertical blanking
 *   pclock_mhz = (h_total * v_total * fps) / 1e6
 *
 * interlace (compute_modeline_from_dims only): posed HERE, by
 * the SAME groovy_mode_classify(h) that decides the regime -- never
 * left to the caller. A caller that still sets out->interlace afterwards is
 * a bug: the two decisions (regime, interlace) must never be able to
 * diverge again.
 *
 * round_even: round to nearest EVEN integer (DRM modeline convention for
 * divisibility by 2 at each blanking boundary).
 */

#ifndef GROOVY_MODELINE_H
#define GROOVY_MODELINE_H

#include <stdint.h>
#include <string.h>   /* memset */
#include <stdbool.h>

/* gm_modeline is defined in libgm/include/gm.h. Both consumers include gm.h
 * before this header. We forward-declare to be self-contained if included
 * in an unexpected order, but callers must still include gm.h for the full
 * definition. */
#ifndef GM_H
#error "groovy_modeline.h must be included AFTER gm.h"
#endif

/* groovy_mode.h: the single 240p/480i regime decision.
 * compute_modeline_from_dims derives v_total AND interlace from the SAME
 * groovy_mode_classify(h) that the driver's frame-by-frame decision uses
 * -- no second copy of the height threshold. */
#include "groovy_mode.h"

/* retro_game_geometry and retro_system_av_info come from libretro.h in the
 * RetroArch build tree. Kept here (no longer used by any
 * function in THIS header) because libgm/test/groovy_test_bridge.c and other
 * consumers still reference retro_system_av_info directly; removing the
 * fallback would be an unrelated breakage outside this plan's scope.
 */
#ifndef LIBRETRO_H__
/* Minimal struct definitions for consumers that don't pull in the full
 * RetroArch headers. MUST match libretro.h exactly -- verified field-by-field
 * against the canonical header: field names, types and order are identical,
 * byte-for-byte. */
struct retro_game_geometry {
    unsigned base_width;
    unsigned base_height;
    unsigned max_width;
    unsigned max_height;
    float    aspect_ratio;
};
struct retro_system_timing {
    double fps;
    double sample_rate;
};
struct retro_system_av_info {
    struct retro_game_geometry geometry;
    struct retro_system_timing timing;
};
#endif /* !LIBRETRO_H__ */

/* round_even: round a double to the nearest even unsigned integer.
 *
 * DRM modeline tools (xrandr, switchres, cvt) conventionally round blanking
 * totals to even values for divisibility at the 2-pixel boundary. Using
 * round_even here ensures our pclock_mhz math is stable: if h_total or
 * v_total differ by ±1 between two calls with the same av_info, the
 * pclock will shift, potentially triggering a duplicate CMD_SWITCHRES.
 * Even-rounding makes the formula deterministic. */
static inline unsigned groovy_round_even(double v)
{
    unsigned u = (unsigned)(v + 0.5);
    return u & ~1u;  /* force even */
}

/* compute_modeline_from_dims — derive a gm_modeline from explicit (w, h, fps).
 *
 * For use inside groovy_push_video (record_groovy.c) and the unit-test
 * bridge (groovy_test_bridge.c). Both must call this function — never
 * inline equivalent logic — so the test exercises the exact runtime path.
 * Now that the announced cache is the only source for the regime, and the
 * core's declaration is treated as a hint rather than an authority, this
 * is the ONLY modeline computation left in this header: groovy_push_av_info
 * no longer announces anything, so the former av_info-only sibling function
 * was retired --
 * every CMD_SWITCHRES this driver sends, at any of its three
 * call sites (groovy_push_video, groovy_reannounce_emit,
 * groovy_reannounce_flush_deferred), goes through this one function, fed by
 * st->av_cache, the single announced-geometry source of truth.
 *
 * Native-resolution path: CMD_SWITCHRES whenever vid->width/height changes
 * per-frame (PSX cores do this for menu mode switches without firing
 * libretro SET_GEOMETRY), so the daemon dynamically reprograms the CRT to
 * match the actual native render resolution — no downscale, no crop, no
 * aspect lying. fps comes from the caller (cached from last
 * SET_SYSTEM_AV_INFO since vid doesn't carry timing info).
 *
 * Thread safety: pure function. */
static inline void compute_modeline_from_dims(unsigned w, unsigned h,
                                               double fps, gm_modeline *out)
{
    memset(out, 0, sizeof(*out));

    out->h_active = (uint16_t)w;
    out->v_active = (uint16_t)h;

    /* Bug F fix: fixed v_total per broadcast class instead of
     * v_active*1.09. The old percentage formula gave too-low hfreq
     * for non-240-line modes (e.g. PSX BIOS 320x216 → v_total=236 → hfreq=14
     * kHz → modeline_rejected H_LOW). Real NTSC/PAL standards use FIXED
     * scanline counts independent of active area:
     *   NTSC 60Hz 15kHz: v_total = 262 (240p single field of 525-line frame)
     *   NTSC 60Hz 31kHz: v_total = 525 (480p)
     *   PAL  50Hz 15kHz: v_total = 312 (288p)
     *   PAL  50Hz 31kHz: v_total = 625 (576p)
     * Result: hfreq = v_total * fps stays ~15.7 kHz or ~31.5 kHz regardless
     * of v_active — passes the JVC envelope and matches real arcade timings.
     *
     * The 31kHz/15kHz split is no longer its own
     * width-aware local boolean -- it is groovy_mode_classify(h), the
     * SAME rule groovy_mode_on_frame uses to decide the regime. Width plays
     * no part: a wide 240p image (640x240) still gets the 15kHz
     * scanline count, matching what the receiver's super-res framebuffer
     * (2560 wide, shared by both regimes) already expected. */
    const enum groovy_mode m = groovy_mode_classify(h);
    {
        bool is_50hz = (fps > 0.0 && fps < 55.0);
        unsigned v_total;
        if (is_50hz) {
            v_total = (m == GROOVY_MODE_480I) ? 625u : 312u;
        } else {
            v_total = (m == GROOVY_MODE_480I) ? 525u : 262u;
        }
        /* Rule 1 fix: NOT force-even here. v_total is a
         * FIXED broadcast-standard scanline count (262/312/525/625), never
         * a rounded percentage -- unlike groovy_round_even's callers, which
         * genuinely need evening for divisibility. The pre-existing
         * "& ~1u" silently truncated the two interlaced totals (525 -> 524,
         * 625 -> 624), corrupting the exact standard field/frame line
         * counts this function must preserve; it was latent because no
         * prior test exercised a 480i-shaped v_total through this
         * function. */
        out->v_total = (uint16_t)v_total;
    }

    /* h_total: keep arcade_15 horizontal blanking convention (~27% blanking).
     * For hires h_active (e.g. 640), 1.27 gives h_total = 813 → hfreq =
     * 525 * 60 ≈ 31.5 kHz. */
    out->h_total = (uint16_t)groovy_round_even((double)w * 1.27);

    /* Porches per blanking-split (Bug B fix). */
    {
        unsigned h_blanking = (unsigned)out->h_total - (unsigned)out->h_active;
        unsigned v_blanking = (unsigned)out->v_total - (unsigned)out->v_active;
        out->h_begin = (uint16_t)(out->h_active + h_blanking / 4u);
        out->h_end   = (uint16_t)(out->h_begin + h_blanking / 2u);
        out->v_begin = (uint16_t)(out->v_active + v_blanking / 4u);
        out->v_end   = (uint16_t)(out->v_begin + v_blanking / 2u);
    }

    out->pclock_mhz = ((double)out->h_total * (double)out->v_total * fps) / 1.0e6;

    /* interlace: posed HERE, by the same classification as v_total above --
     * never left for a caller to set separately. */
    out->interlace = (uint8_t)((m == GROOVY_MODE_480I) ? 1u : 0u);
}

#endif /* GROOVY_MODELINE_H */
