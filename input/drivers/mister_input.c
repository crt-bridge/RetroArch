/* SPDX-License-Identifier: GPL-3.0-or-later */
/* mister_input.c — RetroArch full input_driver_t reading keyboard/mouse
 * state from the crt-bridge Groovy PS/2 input channel.
 *
 * Ported from `mister_input.c` in the `mister` branch of
 * https://github.com/antonioginer/RetroArch (fetched 2026-09-01,
 * input/drivers/mister_input.c). Original copyright below, preserved per
 * its license (GPL v3).
 *
 * This port is NOT verbatim — bench probing measured that calling
 * `input_keyboard_event()` from a point OUTSIDE the active input driver
 * triggers no RetroArch shortcut. The only way to keep both the
 * receiver's keyboard AND the PC keyboard working (a hard requirement)
 * is a WRAPPING driver: this file becomes `input_driver` itself
 * (instead of `dinput`), and explicitly delegates to a `dinput` instance
 * it owns internally. Every departure carries a "crt-bridge :" comment
 * at the point where it appears:
 *
 *   a. Data source (mister_input_poll): upstream calls
 *      gmw_pollInputs()/gmw_getPS2Inputs() against its own
 *      `groovymister` library. Here, exactly one call enters the PS/2
 *      API offered by record_groovy (record/drivers/record_groovy.h)
 *      -- this driver never touches a socket or the wire protocol.
 *   b. Activation guard (mister_input_init): upstream gates its socket
 *      open on config_get_ptr()->bools.video_mister_enable, a setting
 *      of an upstream video driver named "mister" never ported into
 *      this fork. Nothing to keep here: GROOVY_INPUT (on the
 *      record_groovy side) decides whether any state ever arrives;
 *      this driver stays registered and inert otherwise (same
 *      discipline as mister_joypad.c).
 *   c. dinput delegation (the ENTIRE file): upstream ENTIRELY REPLACES
 *      `dinput` -- no reference to a PC keyboard driver in its code.
 *      Here, `dinput` is instantiated INTERNALLY at init
 *      (mister_input_init), polled every frame (mister_input_poll),
 *      and its state is OR-combined with the Groovy state for
 *      RETRO_DEVICE_JOYPAD, RETRO_DEVICE_KEYBOARD and
 *      RETRO_DEVICE_MOUSE (mister_input_state) -- exactly the
 *      adaptation the bench probe's verdict requires, and that a
 *      bench session verified explicitly.
 *   d. A LOCAL keyboard mapping table, NOT the global LUT
 *      (mister_rk_from_sdl / mister_sdl_from_rk, and the whole block
 *      that precedes them): see the comment at their declaration --
 *      a real bug that a more literal port would have introduced.
 *
 * BENCH FINDING, OUTSIDE THIS FILE BUT ESSENTIAL READING HERE
 * (diagnosed after "pad responds, keyboard doesn't" was reported):
 * porting this driver is NOT ENOUGH. The Windows context callback of
 * the active video driver (`gfx/drivers_context/w_vk_ctx.c` for
 * Vulkan, `wgl_ctx.c` for glcore -- upstream code, never touched
 * before this fix) SILENTLY OVERWRITES `input_driver_st.current_driver`
 * as soon as the window is ready, hard-coding `dinput`
 * (only the `"raw"` case was handled) -- with no log line reporting
 * the overwrite. `input_driver = "mister"` was correctly SELECTED at
 * startup (`[Input] Found input driver: "mister".`), but never POLLED
 * again afterwards: `mister_input_poll` was simply never called,
 * replaced by raw `dinput_poll`. The pad kept working (a separate
 * GAMEPAD driver, `mister_joypad.c`, untouched by this mechanism),
 * which made the symptom misleading. Both context files now carry a
 * special `"mister"` case, alongside the existing `"raw"` case -- see
 * their own "crt-bridge" comments.
 *
 * The mouse: consumed HERE only to advance it into the format
 * RetroArch already expects (RETRO_DEVICE_MOUSE), because that is the
 * closest form to upstream and needs no extra line to obtain -- but
 * validation is explicitly scoped to the wire proof, not to this
 * consumption. Do not add further application-level processing here
 * (cursor, config, calibration) beyond what upstream already does:
 * that would cross the scope line drawn for this port. There is
 * indeed no equivalent of input_keyboard_event() for
 * RETRO_DEVICE_MOUSE -- position is read on demand from the active
 * driver, which mister_input_state already does.
 *
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2017 - Daniel De Matteis
 *  Copyright (C) 2014-2015 - Higor Euripedes
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

#include <boolean.h>
#include <string/stdstring.h>
#include <retro_miscellaneous.h>
#include <libretro.h>

#include "../../record/drivers/record_groovy.h"

#include "../input_keymaps.h"
#include "../input_driver.h"

#include "../../configuration.h"
#include "../../retroarch.h"
#include "../../verbosity.h"
#include "../../tasks/tasks_internal.h"

typedef struct mister_input
{
   void    *dinput_data;   /* crt-bridge (departure c) : internal dinput instance */
   uint8_t  keys_mister[32];
   uint8_t  mouse_mister;
   /* frame_mister (upstream's per-frame mouse gate) removed -- novelty is
    * now carried by libgm's delta accumulator (take_mouse, consumed-and-
    * cleared), not by a frame number. */
   int      mouse_x;
   int      mouse_y;
   int      mouse_abs_x;
   int      mouse_abs_y;
   int      mouse_l;
   int      mouse_r;
   int      mouse_m;
} mister_input_t;

/* ---------------------------------------------------------------------------
 * crt-bridge (departure d) : a mapping table LOCAL to this driver, NOT the
 * global rarch_keysym_lut[] LUT upstream uses (input_keymaps_init_
 * keyboard_lut(rarch_key_map_mister) then rarch_keysym_lut[key]).
 *
 * That LUT is a GLOBAL singleton, and dinput_init() (input/drivers/
 * dinput.c) already initializes it with rarch_key_map_dinput (index =
 * DirectInput's DIK_*) at the moment this driver instantiates dinput
 * internally (departure c). If mister_input_init then overwrote it with
 * rarch_key_map_mister (index = SDL scancode), the internal dinput would
 * silently lose its own table: dinput_input_state reads
 * rarch_keysym_lut[key] to index di->state[], an array filled by
 * DirectInput and therefore indexed by DIK_*, not by SDL scancode -- any
 * PC keyboard binding verified through dinput would silently break (the
 * port itself breaking the requirement it exists to satisfy).
 *
 * Solution: two small tables built once from rarch_key_map_mister, never
 * touching rarch_keysym_lut. Same idea as
 * input_keymaps_init_keyboard_lut/input_keymaps_translate_keysym_to_rk,
 * just a private copy.
 * ------------------------------------------------------------------------- */
static enum retro_key mister_rk_from_sdl[256];
static unsigned        mister_sdl_from_rk[RETROK_LAST];
static bool             mister_lut_ready = false;

static void mister_init_local_lut(void)
{
   const struct rarch_key_map *map;

   if (mister_lut_ready)
      return;

   memset(mister_rk_from_sdl, 0, sizeof(mister_rk_from_sdl));
   memset(mister_sdl_from_rk, 0, sizeof(mister_sdl_from_rk));

   for (map = rarch_key_map_mister; map->rk != RETROK_UNKNOWN; map++)
   {
      if (map->sym < ARRAY_SIZE(mister_rk_from_sdl))
         mister_rk_from_sdl[map->sym] = map->rk;
      if (map->rk < ARRAY_SIZE(mister_sdl_from_rk))
         mister_sdl_from_rk[map->rk] = map->sym;
   }

   mister_lut_ready = true;
}

static void *mister_input_init(const char *joypad_driver)
{
   mister_input_t *mister = (mister_input_t*)calloc(1, sizeof(mister_input_t));
   if (!mister)
      return NULL;

   /* crt-bridge (departure b) : nothing to keep here -- see the header
    * comment. */
   mister_init_local_lut();

   /* crt-bridge (departure c) : dinput instantiated INTERNALLY. If its
    * init fails, the Groovy (receiver) state keeps working in a
    * degraded mode -- the PC keyboard would be lost, but that is no
    * reason to fail the whole driver and lose the receiver too. Logged
    * loudly either way. */
   mister->dinput_data = input_dinput.init(joypad_driver);
   if (!mister->dinput_data)
      RARCH_ERR("[mister] dinput.init failed -- PC keyboard "
                "unavailable, the receiver's Groovy state stays active.\n");

   return mister;
}

static bool mister_key_pressed(mister_input_t *mister, int key)
{
   unsigned sym;

   if (key < 0 || key >= (int)ARRAY_SIZE(mister_sdl_from_rk))
      return false;

   /* crt-bridge (departure d) : local table, never the global LUT. */
   sym = mister_sdl_from_rk[(enum retro_key)key];
   return (bool)(1 & (mister->keys_mister[sym / 8] >> (sym % 8)));
}

static int16_t mister_input_state(
      void *data,
      const input_device_driver_t *joypad,
      const input_device_driver_t *sec_joypad,
      rarch_joypad_info_t *joypad_info,
      const retro_keybind_set *binds,
      bool keyboard_mapping_blocked,
      unsigned port,
      unsigned device,
      unsigned idx,
      unsigned id)
{
   int16_t         ret_groovy = 0;
   int16_t         ret_dinput = 0;
   mister_input_t *mister     = (mister_input_t*)data;

   switch (device)
   {
      case RETRO_DEVICE_JOYPAD:
         if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
         {
            unsigned i;

            for (i = 0; i < RARCH_FIRST_CUSTOM_BIND; i++)
            {
               if (binds[port][i].valid)
                  if (mister_key_pressed(mister, binds[port][i].key))
                     ret_groovy |= (1 << i);
            }
         }
         else if (id < RARCH_BIND_LIST_END)
         {
            if (binds[port][id].valid)
               if (mister_key_pressed(mister, binds[port][id].key))
                  ret_groovy = 1;
         }
         /* crt-bridge (departure c) : OR-combined with the internal
          * dinput -- same bind request, same parameters. */
         if (mister->dinput_data && input_dinput.input_state)
            ret_dinput = input_dinput.input_state(mister->dinput_data,
                  joypad, sec_joypad, joypad_info, binds,
                  keyboard_mapping_blocked, port, device, idx, id);
         return ret_groovy | ret_dinput;
      case RETRO_DEVICE_ANALOG:
         {
            int id_minus_key      = 0;
            int id_plus_key       = 0;
            unsigned id_minus     = 0;
            unsigned id_plus      = 0;
            bool id_plus_valid    = false;
            bool id_minus_valid   = false;

            input_conv_analog_id_to_bind_id(idx, id, id_minus, id_plus);

            id_minus_valid        = binds[port][id_minus].valid;
            id_plus_valid         = binds[port][id_plus].valid;
            id_minus_key          = binds[port][id_minus].key;
            id_plus_key           = binds[port][id_plus].key;

            if (id_plus_valid && id_plus_key < RETROK_LAST)
            {
               if (mister_key_pressed(mister, id_plus_key))
                  ret_groovy = 0x7fff;
            }
            if (id_minus_valid && id_minus_key < RETROK_LAST)
            {
               if (mister_key_pressed(mister, id_minus_key))
                  ret_groovy += -0x7fff;
            }
         }
         return ret_groovy;
      case RETRO_DEVICE_MOUSE:
      case RARCH_DEVICE_MOUSE_SCREEN:
         if (config_get_ptr()->uints.input_mouse_index[port] == 0)
         {
            switch (id)
            {
               case RETRO_DEVICE_ID_MOUSE_LEFT:
                  ret_groovy = (int16_t)mister->mouse_l;
                  break;
               case RETRO_DEVICE_ID_MOUSE_RIGHT:
                  ret_groovy = (int16_t)mister->mouse_r;
                  break;
               case RETRO_DEVICE_ID_MOUSE_X:
                  ret_groovy = (int16_t)mister->mouse_x;
                  break;
               case RETRO_DEVICE_ID_MOUSE_Y:
                  ret_groovy = (int16_t)mister->mouse_y;
                  break;
               case RETRO_DEVICE_ID_MOUSE_MIDDLE:
                  ret_groovy = (int16_t)mister->mouse_m;
                  break;
               default:
                  break;
            }
         }
         /* crt-bridge (departure c) : OR-combined with the PC mouse (dinput). */
         if (mister->dinput_data && input_dinput.input_state)
            ret_dinput = input_dinput.input_state(mister->dinput_data,
                  joypad, sec_joypad, joypad_info, binds,
                  keyboard_mapping_blocked, port, device, idx, id);
         return ret_groovy | ret_dinput;
      case RETRO_DEVICE_POINTER:
      case RARCH_DEVICE_POINTER_SCREEN:
         if (idx == 0)
         {
            struct video_viewport vp;
            bool screen                 = device ==
               RARCH_DEVICE_POINTER_SCREEN;
            const int edge_detect       = 32700;
            bool inside                 = false;
            int16_t res_x               = 0;
            int16_t res_y               = 0;
            int16_t res_screen_x        = 0;
            int16_t res_screen_y        = 0;

            vp.x                        = 0;
            vp.y                        = 0;
            vp.width                    = 0;
            vp.height                   = 0;
            vp.full_width               = 0;
            vp.full_height              = 0;

            if (video_driver_translate_coord_viewport_wrap(
                        &vp, mister->mouse_abs_x, mister->mouse_abs_y,
                        &res_x, &res_y, &res_screen_x, &res_screen_y))
            {
               if (screen)
               {
                  res_x = res_screen_x;
                  res_y = res_screen_y;
               }

               inside =    (res_x >= -edge_detect)
                  && (res_y >= -edge_detect)
                  && (res_x <= edge_detect)
                  && (res_y <= edge_detect);

               switch (id)
               {
                  case RETRO_DEVICE_ID_POINTER_X:
                     return res_x;
                  case RETRO_DEVICE_ID_POINTER_Y:
                     return res_y;
                  case RETRO_DEVICE_ID_POINTER_PRESSED:
                     return (int16_t)mister->mouse_l;
                  case RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN:
                     return !inside;
               }
            }
         }
         break;
      case RETRO_DEVICE_KEYBOARD:
         ret_groovy = (int16_t)((id < RETROK_LAST) && mister_key_pressed(mister, (int)id));
         /* crt-bridge (departure c) : OR-combined -- the most heavily
          * verified line in this whole file, per the bench probe's own
          * verdict. */
         if (mister->dinput_data && input_dinput.input_state)
            ret_dinput = input_dinput.input_state(mister->dinput_data,
                  joypad, sec_joypad, joypad_info, binds,
                  keyboard_mapping_blocked, port, device, idx, id);
         return ret_groovy | ret_dinput;
      case RETRO_DEVICE_LIGHTGUN:
         switch (id)
         {
            case RETRO_DEVICE_ID_LIGHTGUN_X:
               return (int16_t)mister->mouse_x;
            case RETRO_DEVICE_ID_LIGHTGUN_Y:
               return (int16_t)mister->mouse_y;
            case RETRO_DEVICE_ID_LIGHTGUN_TRIGGER:
               return (int16_t)mister->mouse_l;
            case RETRO_DEVICE_ID_LIGHTGUN_CURSOR:
               return (int16_t)mister->mouse_m;
            case RETRO_DEVICE_ID_LIGHTGUN_TURBO:
               return (int16_t)mister->mouse_r;
            case RETRO_DEVICE_ID_LIGHTGUN_START:
               return (int16_t)(mister->mouse_m && mister->mouse_r);
            case RETRO_DEVICE_ID_LIGHTGUN_PAUSE:
               return (int16_t)(mister->mouse_m && mister->mouse_l);
         }
         break;
   }

   return 0;
}

static void mister_input_free(void *data)
{
   mister_input_t *mister = (mister_input_t*)data;
   if (!mister)
      return;

   /* crt-bridge (departure c) : free the internal dinput too. */
   if (mister->dinput_data && input_dinput.free)
      input_dinput.free(mister->dinput_data);

   free(mister);
}

static void mister_input_poll(void *data)
{
   mister_input_t *mister = (mister_input_t*)data;
   gm_ps2_inputs   ps2;

   if (!mister)
      return;

   /* crt-bridge (departure c) : dinput is polled UNCONDITIONALLY, even
    * when no Groovy state arrives (receiver off, GROOVY_INPUT off) --
    * the PC keyboard must never depend on the receiver. */
   if (mister->dinput_data && input_dinput.poll)
      input_dinput.poll(mister->dinput_data);

   /* crt-bridge (departure a) : the single entry point into
    * record_groovy, no socket/protocol here. Returns -1 when no state
    * is available (no session, GROOVY_INPUT off) -- the cache then
    * stays at its last value, same as mister_joypad_poll. */
   if (groovy_input_take_ps2(&ps2) != 0)
      return;

   {
      int i;
      for (i = 0; i < 256; i++)
      {
         int bit_pos = 1 & (ps2.keys[i / 8] >> (i % 8));
         int bit_pre = 1 & (mister->keys_mister[i / 8] >> (i % 8));
         if (bit_pre != bit_pos)
         {
            uint16_t mod  = 0;
            /* crt-bridge (departure d) : local table, not
             * input_keymaps_translate_keysym_to_rk (the global LUT). */
            unsigned code = (unsigned)mister_rk_from_sdl[i];
            /* crt-bridge : on the wire, bit = 1 means the key is
             * PRESSED (GroovyMAME decoder input_mister.cpp:556-560, and
             * our own encoder groovy_input_set_key_bit). The signature
             * is input_keyboard_event(bool down, ...). Upstream emitted
             * !bit_pos -- an INVERTED polarity (release on press),
             * contradicting the mister_key_pressed state path three
             * lines above, which treats bit = 1 as pressed. Fixed:
             * down = (bit_pos != 0). No documented upstream reason for
             * the inversion in either of the two sources cited above. */
            input_keyboard_event(bit_pos != 0, code, code, mod, RETRO_DEVICE_KEYBOARD);
         }
      }
      memcpy(&mister->keys_mister, &ps2.keys, sizeof(mister->keys_mister));
   }

   if ((ps2.mouse_bits & (1 << 0)) && !(mister->mouse_mister & (1 << 0)))
      mister->mouse_l = 1;

   if ((ps2.mouse_bits & (1 << 1)) && !(mister->mouse_mister & (1 << 1)))
      mister->mouse_r = 1;

   if ((ps2.mouse_bits & (1 << 2)) && !(mister->mouse_mister & (1 << 2)))
      mister->mouse_m = 1;

   if (!(ps2.mouse_bits & (1 << 0)) && (mister->mouse_mister & (1 << 0)))
      mister->mouse_l = 0;

   if (!(ps2.mouse_bits & (1 << 1)) && (mister->mouse_mister & (1 << 1)))
      mister->mouse_r = 0;

   if (!(ps2.mouse_bits & (1 << 2)) && (mister->mouse_mister & (1 << 2)))
      mister->mouse_m = 0;

   mister->mouse_mister = ps2.mouse_bits;

   /* The mouse is only entitled here to the same consumption as
    * upstream (RETRO_DEVICE_MOUSE via mister_input_state) -- no
    * additional application-level processing (cursor, calibration) is
    * added. Validation is scoped to the WIRE, not to this consumption:
    * see the header comment.
    *
    * crt-bridge : the wire's mouse bytes are per-datagram DELTAS (the
    * receiver clears its accumulator after EVERY send), not a state. A
    * verbatim upstream port consumed them "latest state wins" behind a
    * frame-only gate: any datagram with order >= 1 within the same
    * receiver frame, or any datagram overwritten before consumption
    * within the same drain, lost its movement -- and the -255 decoding
    * additionally lost 1 on every negative delta (a -1 delta decoded
    * to 0: slow movement lost). libgm now accumulates the
    * ALREADY-DECODED deltas (signed -256, GroovyMAME's formula) at
    * drain time; take_mouse consumes them and clears the accumulator.
    * The frame_mister gate becomes moot: the accumulator itself IS the
    * novelty flag (0 = no movement drained). */
   {
      int dx = 0, dy = 0, dz = 0;
      if (groovy_input_take_mouse(&dx, &dy, &dz) == 0 && (dx || dy))
      {
         mister->mouse_x     += dx;
         mister->mouse_abs_x += dx;
         mister->mouse_y     -= dy;
         mister->mouse_abs_y -= dy;
      }
      else
      {
         mister->mouse_x     = 0;
         mister->mouse_y     = 0;
         mister->mouse_abs_x = 0;
         mister->mouse_abs_y = 0;
      }
   }
}

static uint64_t mister_get_capabilities(void *data)
{
   mister_input_t *mister = (mister_input_t*)data;
   uint64_t caps =
        (1 << RETRO_DEVICE_JOYPAD)
      | (1 << RETRO_DEVICE_MOUSE)
      | (1 << RETRO_DEVICE_KEYBOARD)
      | (1 << RETRO_DEVICE_LIGHTGUN)
      | (1 << RETRO_DEVICE_POINTER)
      | (1 << RETRO_DEVICE_ANALOG);

   /* crt-bridge (departure c) : relay to dinput too. */
   if (mister && mister->dinput_data && input_dinput.get_capabilities)
      caps |= input_dinput.get_capabilities(mister->dinput_data);

   return caps;
}

input_driver_t input_mister = {
   mister_input_init,
   mister_input_poll,
   mister_input_state,
   mister_input_free,
   NULL,
   NULL,
   mister_get_capabilities,
   "mister",
   NULL,                   /* grab_mouse */
   NULL,
   NULL
};
