/* SPDX-License-Identifier: GPL-3.0-or-later */
/* groovy_modeline.h -- fall-back definitions of the libretro geometry
 * structures.
 *
 * This header used to compute a modeline from the native size of the picture
 * (a clock, totals and porches derived from the width, the height and the
 * core's frame rate). No modeline is built that way any more: every
 * CMD_SWITCHRES the driver sends comes from groovy_announce_modeline
 * (groovy_announce.h) -- the pinned modeline of the receiver for the regime,
 * or the mode of the session the launcher chose (groovy_session.h). The
 * function and its rounding helper are gone with their last caller.
 *
 * What stays is the part that is not about modelines: minimal copies of the
 * libretro structures for the consumers that do not pull in the full
 * RetroArch headers (libgm/test/groovy_test_bridge.c references
 * retro_system_av_info directly). They are used only when libretro.h has not
 * been included.
 */

#ifndef GROOVY_MODELINE_H
#define GROOVY_MODELINE_H

/* retro_game_geometry and retro_system_av_info come from libretro.h in the
 * RetroArch build tree. Minimal struct definitions for the consumers that
 * don't pull in the full RetroArch headers. MUST match libretro.h exactly --
 * verified field-by-field against the canonical header: field names, types
 * and order are identical, byte-for-byte. */
#ifndef LIBRETRO_H__
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

#endif /* GROOVY_MODELINE_H */
