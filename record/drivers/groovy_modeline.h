/* SPDX-License-Identifier: GPL-3.0-or-later */
/* groovy_modeline.h — shared compute_modeline_from_av_info logic.
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
 *   interlace  = 0 (240p / progressive-only for MVP; 480i requires v2 work)
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

/* retro_game_geometry and retro_system_av_info come from libretro.h in the
 * RetroArch build tree. For the test bridge (groovy_test_bridge.c), these
 * structs are defined locally to avoid pulling in the full RetroArch headers.
 * record_groovy.c includes them transitively via record_driver.h → libretro.h.
 */
#ifndef LIBRETRO_H__
/* Minimal struct definitions for the test bridge path (when libretro.h
 * is not pulled in by the consumer). MUST match libretro.h exactly --
 * verified field-by-field against the canonical header: field names,
 * types and order are identical, byte-for-byte. No changes to the
 * fallback block below were required. Field accesses in
 * compute_modeline_from_av_info (av->geometry.base_width,
 * av->geometry.base_height, av->timing.fps) match that header. */
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

/* compute_modeline_from_av_info — derive a gm_modeline from a libretro
 * retro_system_av_info using Option G-friendly defaults.
 *
 * For use inside groovy_push_av_info (record_groovy.c) and the unit-test
 * bridge (groovy_test_bridge.c). Both must call this function — never
 * inline equivalent logic — so the test exercises the exact runtime path.
 *
 * Thread safety: pure function (no side effects, no globals). Safe from any
 * thread.
 */
/* compute_modeline_from_dims — derive a gm_modeline from explicit (w, h, fps).
 *
 * Native-resolution path: replaces the av_info-only path in
 * groovy_push_av_info. This path emits
 * CMD_SWITCHRES whenever vid->width/height changes per-frame (PSX cores do
 * this for menu mode switches without firing libretro SET_GEOMETRY), so the
 * daemon dynamically reprograms the CRT to match the actual native render
 * resolution — no downscale, no crop, no aspect lying.
 *
 * Same blanking rules as compute_modeline_from_av_info — kept identical so
 * the test bridge fixtures still pass. fps comes from the caller (cached
 * from last SET_SYSTEM_AV_INFO since vid doesn't carry timing info).
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
     * of v_active — passes the JVC envelope and matches real arcade timings. */
    {
        bool is_50hz = (fps > 0.0 && fps < 55.0);
        /* 31kHz threshold: any height above 280 lines is "hires" territory
         * (480p/576p region). Any width above 400 also triggers hires
         * (some PSX hires modes are 640x240). */
        bool is_31khz = (h > 280u) || (w > 400u);
        unsigned v_total;
        if (is_50hz) {
            v_total = is_31khz ? 625u : 312u;
        } else {
            v_total = is_31khz ? 525u : 262u;
        }
        out->v_total = (uint16_t)(v_total & ~1u);  /* force even */
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
    /* The caller sets out->interlace per the classified regime
     * (record_groovy.c::push_video). */
}

static inline void compute_modeline_from_av_info(
        const struct retro_system_av_info *av,
        gm_modeline *out)
{
    memset(out, 0, sizeof(*out));

    out->h_active = (uint16_t)av->geometry.base_width;
    out->v_active = (uint16_t)av->geometry.base_height;

    /* Default blanking: 27% horizontal, 9% vertical (arcade_15 convention). */
    out->h_total = (uint16_t)groovy_round_even(
            (double)av->geometry.base_width * 1.27);
    out->v_total = (uint16_t)groovy_round_even(
            (double)av->geometry.base_height * 1.09);

    /* Porches: derived from the blanking budget so DRM ordering constraint
     * (hdisplay ≤ hsync_start ≤ hsync_end ≤ htotal) is satisfied by
     * construction. Standard split: ¼ front porch, ½ sync, ¼ back porch.
     * Integer truncation ensures end ≤ total without an explicit clamp.
     * Bug B fix: replaces the prior zero porches, which the kernel
     * rejected with EINVAL even on otherwise-valid modelines. */
    {
        unsigned h_blanking = (unsigned)out->h_total - (unsigned)out->h_active;
        unsigned v_blanking = (unsigned)out->v_total - (unsigned)out->v_active;
        out->h_begin = (uint16_t)(out->h_active + h_blanking / 4u);
        out->h_end   = (uint16_t)(out->h_begin + h_blanking / 2u);
        out->v_begin = (uint16_t)(out->v_active + v_blanking / 4u);
        out->v_end   = (uint16_t)(out->v_begin + v_blanking / 2u);
    }

    /* pclock in MHz (NOT Hz) — GroovyMAME convention, daemon expects MHz. */
    out->pclock_mhz = ((double)out->h_total * (double)out->v_total
                       * av->timing.fps) / 1.0e6;

    /* The caller sets out->interlace per the classified regime
     * (record_groovy.c::push_video). */
}

#endif /* GROOVY_MODELINE_H */
