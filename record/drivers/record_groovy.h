/* SPDX-License-Identifier: GPL-3.0-or-later */
/*  RetroArch - A frontend for libretro.
 *  crt-bridge: record_groovy driver header.
 *
 *  Same license as the rest of RetroArch (GPL v3).
 */

#ifndef _RECORD_GROOVY_H
#define _RECORD_GROOVY_H

#include "../record_driver.h"

/* gm_joy_inputs / gm_ps2_inputs -- needed for the prototypes below.
 * Included here (not just in record_groovy.c) because the ported
 * drivers (input/drivers_joypad/mister_joypad.c) include THIS header
 * without necessarily including gm.h themselves. */
#include <gm.h>

/* Regime decision for the dual-mode super-res pivot -- the single decision
 * shared by record_groovy.c::groovy_push_video/groovy_push_av_info and the
 * test bridge. groovy_push_video is the sole authority: it calls
 * groovy_mode_on_frame on every rendered frame, which owns
 * its own same-regime size hysteresis and oscillation guard internally --
 * no additional settle gate lives here any more. */
#include "groovy_mode.h"

extern const record_driver_t record_groovy;

/* ---------------------------------------------------------------------------
 * Input channel. The functions offered to the ported drivers: they speak
 * neither socket nor protocol.
 *
 * Returns 0 and fills *out if a controller/keyboard state is available,
 * -1 otherwise (no Groovy session, or GROOVY_INPUT off, or a null
 * pointer).
 *
 * DRAIN OWNERSHIP: ONLY take_joy drains the input socket. take_ps2 and
 * take_mouse do NOT drain -- they consume the state already drained by
 * take_joy within the same RetroArch frame (an order guaranteed by
 * input_driver_poll(): joypad->poll() before input->poll(),
 * input_joypad_driver = "mister" locked in). Do NOT "fix" take_ps2 by
 * giving it its own drain: two independent drains of the same per-frame
 * stream are exactly the joypad/keyboard drop bug this fixed. The
 * detail and the documented fragility of that ordering guarantee live
 * in the comment block above groovy_input_take_joy (record_groovy.c).
 * ------------------------------------------------------------------------- */
int groovy_input_take_joy(gm_joy_inputs *out);
int groovy_input_take_ps2(gm_ps2_inputs *out);

/* Mouse deltas accumulated by libgm at drain time, already decoded into
 * signed values (see gm_get_mouse_deltas in gm.h). CONSUMES-AND-RESETS:
 * a single consumer per frame. NULL pointers accepted. Returns 0, or -1
 * with no session/channel. */
int groovy_input_take_mouse(int *dx, int *dy, int *dz);

#endif
