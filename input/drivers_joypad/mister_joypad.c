/* SPDX-License-Identifier: GPL-3.0-or-later */
/* mister_joypad.c — RetroArch joypad driver reading gamepad state from the
 * crt-bridge Groovy input channel.
 *
 * Ported quasi-verbatim from `mister_joypad.c` in the `mister` branch of
 * https://github.com/antonioginer/RetroArch (fetched 2026-09-01,
 * input/drivers_joypad/mister_joypad.c, commit tip of that branch at fetch
 * time). Original copyright below, preserved per its license (GPL v3).
 *
 * Every departure from the upstream source is marked in place with a
 * "crt-bridge :" comment that says why it exists. There are exactly
 * four (the driver must stay a pure translator, the PC keyboard must
 * keep working, and one honest bug-fix in the axis rescale — see the
 * comment at mister_scale_axis_i8):
 *
 *   a. Data source (mister_joypad_poll): upstream calls gmw_pollInputs() +
 *      gmw_getJoyInputs() against its own `groovymister` library socket.
 *      Here there is exactly one call into record_groovy's input API (see
 *      poll(), below, for the exact identifier) — this driver never
 *      touches a socket or the wire protocol itself.
 *   b. Activation guard (mister_joypad_init): upstream gates its socket
 *      open on config_get_ptr()->bools.video_mister_enable. There is
 *      nothing to gate here — record_groovy (GROOVY_INPUT) decides whether
 *      any data ever arrives; this driver stays registered and inert
 *      otherwise.
 *   c. Button identifiers (mister_pad_get_button): GM_JOY_* masks (gm.h)
 *      replace upstream's GMW_JOY_* masks — same numeric values, same
 *      RETRO_DEVICE_ID_JOYPAD_* mapping, cross-checked bit-for-bit against
 *      receiver/input.c:groovy_input_button_bit on the bench.
 *   d. Axis rescale (mister_scale_axis_i8): see the comment at its
 *      definition — an explicit multiplication replaces upstream's
 *      approximate bit-shift trick.
 *
 * RETRO_DEVICE_ID_JOYPAD_* -> GM_JOY_* table, copied from mister_joypad.c
 * (upstream) line 52-86:
 *   _UP->JOY_UP  _DOWN->JOY_DOWN  _LEFT->JOY_LEFT  _RIGHT->JOY_RIGHT
 *   _B->B1  _A->B2  _Y->B3  _X->B4  _SELECT->B5  _START->B6
 *   _L->B7  _R->B8  _L2->B9  _R2->B10
 * No case for _L3/_R3 — the upstream switch only has 14 entries and falls
 * through to `return 0` for everything else, mirrored here.
 *
 * MAX_USERS_MISTER stays 2, unchanged — an ACCEPTED limit (two pads, ten
 * buttons, four axes). Widening it is scope creep this port does not
 * take on.
 *
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2017 - Daniel De Matteis
 *  Copyright (C) 2014-2017 - Higor Euripedes
 *  Copyright (C)      2023 - Carlo Refice
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <compat/strl.h>

#include "../../record/drivers/record_groovy.h"

#include "../input_driver.h"

#include "../../tasks/tasks_internal.h"
#include "../../verbosity.h"

#define MAX_USERS_MISTER 2

typedef struct _mister_joypad
{
   uint16_t map;
   int8_t   axis[4];
   unsigned num_axes;
   unsigned num_buttons;
   unsigned num_hats;
} mister_joypad_t;

/* TODO/FIXME - static globals */
static mister_joypad_t mister_pads[MAX_USERS_MISTER];

static const char *mister_joypad_name(unsigned pad)
{
   if (pad >= MAX_USERS_MISTER)
      return NULL;

   return "MiSTer";
}

static int32_t mister_pad_get_button(mister_joypad_t *pad, uint16_t joykey)
{
   /* crt-bridge : (button identifiers) table copied from
    * mister_joypad.c:52-86 (antonioginer upstream), GM_JOY_* masks
    * (gm.h) in place of GMW_JOY_* -- same numeric values. Mirrors
    * receiver/input.c:groovy_input_button_bit, verified button by
    * button on the bench. */
   switch (joykey)
   {
      case RETRO_DEVICE_ID_JOYPAD_UP:
         return (int32_t)(pad->map & GM_JOY_UP);
      case RETRO_DEVICE_ID_JOYPAD_DOWN:
         return (int32_t)(pad->map & GM_JOY_DOWN);
      case RETRO_DEVICE_ID_JOYPAD_LEFT:
         return (int32_t)(pad->map & GM_JOY_LEFT);
      case RETRO_DEVICE_ID_JOYPAD_RIGHT:
         return (int32_t)(pad->map & GM_JOY_RIGHT);
      case RETRO_DEVICE_ID_JOYPAD_B:
         return (int32_t)(pad->map & GM_JOY_B1);
      case RETRO_DEVICE_ID_JOYPAD_A:
         return (int32_t)(pad->map & GM_JOY_B2);
      case RETRO_DEVICE_ID_JOYPAD_Y:
         return (int32_t)(pad->map & GM_JOY_B3);
      case RETRO_DEVICE_ID_JOYPAD_X:
         return (int32_t)(pad->map & GM_JOY_B4);
      case RETRO_DEVICE_ID_JOYPAD_SELECT:
         return (int32_t)(pad->map & GM_JOY_B5);
      case RETRO_DEVICE_ID_JOYPAD_START:
         return (int32_t)(pad->map & GM_JOY_B6);
      case RETRO_DEVICE_ID_JOYPAD_L:
         return (int32_t)(pad->map & GM_JOY_B7);
      case RETRO_DEVICE_ID_JOYPAD_R:
         return (int32_t)(pad->map & GM_JOY_B8);
      case RETRO_DEVICE_ID_JOYPAD_L2:
         return (int32_t)(pad->map & GM_JOY_B9);
      case RETRO_DEVICE_ID_JOYPAD_R2:
         return (int32_t)(pad->map & GM_JOY_B10);
   }
   return 0;
}

static void mister_pad_connect(unsigned id)
{
   mister_joypad_t *pad       = (mister_joypad_t*)&mister_pads[id];
   int32_t product            = 0x9999;
   int32_t vendor             = 0x9999;

   input_autoconfigure_connect(
         mister_joypad_name(id),
         NULL, NULL,
         mister_joypad.ident,
         id,
         vendor,
         product);

   pad->num_axes    = 4;
   pad->num_buttons = 10;
   pad->num_hats    = 1;
}

static void mister_pad_disconnect(unsigned id)
{
   input_autoconfigure_disconnect(id, mister_joypad_name(id));
   memset(&mister_pads[id], 0, sizeof(mister_pads[id]));
}

static void mister_joypad_destroy(void)
{
   unsigned i;
   for (i = 0; i < MAX_USERS_MISTER; i++)
      mister_pad_disconnect(i);

   memset(mister_pads, 0, sizeof(mister_pads));
}

static void *mister_joypad_init(void *data)
{
   /* crt-bridge : (activation guard) upstream gates its socket open on
    * config_get_ptr()->bools.video_mister_enable -- an upstream video
    * driver setting named "mister", never ported here. There is
    * nothing to gate: this driver never opens a socket itself,
    * record_groovy (GROOVY_INPUT) decides whether a channel opens.
    * mister_joypad_init just clears the cache and connects the
    * MAX_USERS_MISTER pads -- a registered, inert driver costs
    * nothing as long as no state arrives from record_groovy (poll()
    * below then leaves the cache at zero, buttons released). */
   unsigned i;
   memset(mister_pads, 0, sizeof(mister_pads));

   for (i = 0; i < MAX_USERS_MISTER; i++)
      mister_pad_connect(i);

   return (void*)-1;
}

static int32_t mister_joypad_button_state(
      mister_joypad_t *pad,
      unsigned port, uint16_t joykey)
{
   return mister_pad_get_button(pad, joykey);
}

static int32_t mister_joypad_button(unsigned port, uint16_t joykey)
{
   /* crt-bridge : port bound checked BEFORE forming and dereferencing
    * the pointer -- RetroArch queries drivers up to MAX_USERS (16),
    * mister_pads[] only has MAX_USERS_MISTER (2) entries; upstream
    * read pad->map out of bounds for any port >= 2. */
   mister_joypad_t *pad;
   if (port >= MAX_USERS_MISTER)
      return 0;
   pad = (mister_joypad_t*)&mister_pads[port];
   if (!pad->map)
      return 0;
   return mister_joypad_button_state(pad, port, joykey);
}

static int16_t mister_scale_axis_i8(int8_t v)
{
   /* crt-bridge : (axis rescale) gm_joy_inputs (gm.h) carries axes as
    * int8_t (-128..127) -- the same quantization as
    * receiver/input.c:groovy_input_scale_axis, which produces them
    * from a calibrated evdev source. RetroArch expects an int16_t
    * (-32768..32767) on RETRO_DEVICE_ANALOG. Rescaled by an explicit
    * MULTIPLICATION, not by upstream's approximate bit-shift trick
    * (upstream mister_joypad_axis_state: `(v << 8) + v`, i.e. v*257 --
    * works by accident on an int16_t already close to full scale,
    * wrong here where the source is a narrow int8_t). x256:
    * -128*256 = -32768 (EXACT low bound); 127*256 = 32512 (high bound
    * within 255/32767, no overflow risk). */
   return (int16_t)((int32_t)v * 256);
}

static int16_t mister_joypad_axis_state(
      mister_joypad_t *pad,
      unsigned port, uint32_t joyaxis)
{
   /* crt-bridge : (axis sign filter) upstream returned the scaled value
    * for a NEG bind and a POS bind alike, so RetroArch's
    * abs(plus) - abs(minus) reconstruction (input_joypad_analog_axis,
    * input/input_driver.c) always cancelled to zero -- the stick read as
    * permanently centred no matter how it was held. Filtered on the model
    * of sdl_joypad_axis_state (sdl_joypad.c): a NEG bind only returns a
    * negative value, clamped to -0x7fff (never -0x8000, unsafe once
    * mister_joypad_state's abs() sees it); a POS bind only returns a
    * positive value; anything else falls through to 0. */
   if (AXIS_NEG_GET(joyaxis) < pad->num_axes)
   {
      int16_t val = mister_scale_axis_i8(pad->axis[AXIS_NEG_GET(joyaxis)]);
      if (val < 0)
      {
         if (val < -0x7fff)
            return -0x7fff;
         return val;
      }
   }
   else if (AXIS_POS_GET(joyaxis) < pad->num_axes)
   {
      int16_t val = mister_scale_axis_i8(pad->axis[AXIS_POS_GET(joyaxis)]);
      if (val > 0)
         return val;
   }

   return 0;
}

static int16_t mister_joypad_axis(unsigned port, uint32_t joyaxis)
{
   /* crt-bridge : upstream had NO bound check here at all (the !pad
    * test was always false: the address of a static array element is
    * never NULL) -- out-of-bounds read for any port >= MAX_USERS_MISTER. */
   mister_joypad_t *pad;
   if (port >= MAX_USERS_MISTER)
      return 0;
   pad = (mister_joypad_t*)&mister_pads[port];
   return mister_joypad_axis_state(pad, port, joyaxis);
}

static int16_t mister_joypad_state(
      rarch_joypad_info_t *joypad_info,
      const struct retro_keybind *binds,
      unsigned port)
{
   unsigned i;
   int16_t ret                          = 0;
   uint16_t port_idx                    = joypad_info->joy_idx;
   mister_joypad_t *pad;

   /* crt-bridge : bound checked BEFORE forming the pointer and reading
    * pad->map -- same out-of-bounds defect as mister_joypad_button for
    * any port >= MAX_USERS_MISTER. */
   if (port_idx >= MAX_USERS_MISTER)
      return 0;
   pad = (mister_joypad_t*)&mister_pads[port_idx];
   if (!pad->map)
      return 0;

   for (i = 0; i < RARCH_FIRST_CUSTOM_BIND; i++)
   {
      /* Auto-binds are per joypad, not per user. */
      const uint64_t joykey  = (binds[i].joykey != NO_BTN)
         ? binds[i].joykey  : joypad_info->auto_binds[i].joykey;
      const uint32_t joyaxis = (binds[i].joyaxis != AXIS_NONE)
         ? binds[i].joyaxis : joypad_info->auto_binds[i].joyaxis;
      if (
               (uint16_t)joykey != NO_BTN
            && mister_joypad_button_state(pad, port_idx, (uint16_t)joykey)
         )
         ret |= ( 1 << i);
      else if (joyaxis != AXIS_NONE &&
            ((float)abs(mister_joypad_axis_state(pad, port_idx, joyaxis))
             / 0x8000) > joypad_info->axis_threshold)
         ret |= (1 << i);
   }

   return ret;
}

static void mister_joypad_poll(void)
{
   /* crt-bridge : (data source) upstream queries its `groovymister`
    * library directly (gmw_pollInputs() then gmw_getJoyInputs()). Here
    * there is exactly ONE entry point into record_groovy
    * (record/drivers/record_groovy.h), called once below -- this
    * driver knows neither socket nor protocol,
    * mister_joypad_button/_state/_axis read ONLY the mister_pads[]
    * cache above, never the network. When no state is available (no
    * open Groovy session, or GROOVY_INPUT off), the cache stays
    * unchanged -- on the first call it is zero (mister_joypad_init),
    * so the driver reports "nothing pressed", never noisy. */
   gm_joy_inputs joy;
   mister_joypad_t *pad1;
   mister_joypad_t *pad2;

   if (groovy_input_take_joy(&joy) != 0)
      return;

   pad1 = (mister_joypad_t*)&mister_pads[0];
   pad2 = (mister_joypad_t*)&mister_pads[1];

   pad1->map     = joy.joy1;
   pad1->axis[0] = joy.joy1_lx;
   pad1->axis[1] = joy.joy1_ly;
   pad1->axis[2] = joy.joy1_rx;
   pad1->axis[3] = joy.joy1_ry;
   pad2->map     = joy.joy2;
   pad2->axis[0] = joy.joy2_lx;
   pad2->axis[1] = joy.joy2_ly;
   pad2->axis[2] = joy.joy2_rx;
   pad2->axis[3] = joy.joy2_ry;
}

static bool mister_joypad_query_pad(unsigned pad)
{
   return pad < MAX_USERS_MISTER;
}

input_device_driver_t mister_joypad = {
   mister_joypad_init,
   mister_joypad_query_pad,
   mister_joypad_destroy,
   mister_joypad_button,
   mister_joypad_state,
   NULL,
   mister_joypad_axis,
   mister_joypad_poll,
   NULL,
   NULL,
   NULL,
   NULL,
   mister_joypad_name,
   "mister"
};
