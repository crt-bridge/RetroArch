/* SPDX-License-Identifier: GPL-3.0-or-later */
/* groovy_inject.h -- diagnostic hook: injects known faults at chosen
 * instants of a session. Header-only and pure, pattern from groovy_skip.h:
 * no clock, no getenv, no global state, no libgm include, so the whole
 * grammar and every decision can be loaded and checked outside the driver.
 *
 * WHAT THIS HOOK IS FOR. A measuring instrument can only be trusted if it
 * is shown a fault whose kind, instant and size are known in advance. This
 * hook produces such faults on demand: a frame that is not sent, an
 * interlaced opening field that is not sent, a gap in the audio, a fixed
 * extra delay before every frame, a single busy wait of the run loop. The
 * driver reads GROOVY_INJECT once, asks the pure decisions below what is
 * due at the current elapsed time, and logs what it did.
 *
 * WHAT IT NEVER TOUCHES. frame_id advances exactly as it would for any
 * other frame: a frame that is not emitted still consumes its number, the
 * gap in the sequence is the very subject of the proof. And nothing here
 * decides anything when the variable is absent, empty or refused: the
 * parser then returns zero items and no fault can ever fire.
 *
 * WHY AN ENVIRONMENT VARIABLE AND NOT retroarch.cfg. RetroArch rewrites
 * that file on exit from its own settings list, so a key added by hand
 * would vanish; and a fault must never be switchable from a menu where it
 * could be left on by accident. Same pattern as GROOVY_SKIP_FRAME_EVERY,
 * GROOVY_AUDIO, GROOVY_MTU.
 *
 * GRAMMAR. A comma-separated list, no blanks, lowercase:
 *
 *     kind@S        or        kind@S:P
 *
 * S is whole seconds, 1 to 5 digits, 0 to GROOVY_INJECT_MAX_AT_S, counted
 * from the origin of the session (the caller owns the clock). P is whole
 * milliseconds, 1 to 4 digits, at least 1.
 *
 *   skip@S      no parameter. A progressive frame is not emitted.
 *   parity@S    no parameter. With interlaced video sent one field per
 *               frame, one opening field is not emitted; the opening
 *               parity of the following pairs flips and stays flipped.
 *   audio@S:P   P <= GROOVY_INJECT_AUDIO_MAX_MS. The PCM is not sent for
 *               P milliseconds.
 *   delay@S:P   P <= GROOVY_INJECT_DELAY_MAX_MS. For
 *               GROOVY_INJECT_DELAY_WINDOW_S seconds every emitted frame
 *               waits P milliseconds before it is sent. The wait is a busy
 *               wait on the run-loop thread: once P plus the emulation of
 *               a frame exceeds the frame period (16.7 ms at 60 Hz), the
 *               loop no longer keeps its rate and the fault becomes a
 *               slowdown (repeated frames, starved audio), not only a
 *               delay. Keep P to a few milliseconds for a pure delay.
 *   stall@S:P   P <= GROOVY_INJECT_STALL_MAX_MS. One single busy wait of
 *               P milliseconds in the run loop.
 *
 * At most GROOVY_INJECT_MAX_ITEMS items; the same kind may appear several
 * times and each fires once. ANY error (unknown kind, missing '@', empty or
 * out-of-range second, sign or blank in front of a number, missing
 * parameter where one is required, parameter where none is allowed,
 * out-of-range parameter, empty item, trailing comma, too many items)
 * refuses the WHOLE string: zero items. A good item never survives a bad
 * neighbour, so a typo cannot silently run a half of the intended test.
 *
 * THE "OPPOSITE FIELD" USE OF parity@0. The tracked opening parity is the
 * parity of the first half-frame of the interlaced excursion. A parity
 * fault withholds one opening half-frame, after which the receiver pairs
 * on the other parity. Fired at second 0 it flips the opening parity from
 * the very first pair of the first excursion; fired later it flips it in
 * the middle of an excursion. Both forms are deliberate and both are
 * observable on the receiver side.
 *
 * Each item fires once, so parity@0 flips the FIRST excursion only. And two
 * parity items due at the same time cancel out: after the first fires, the
 * very next half-frame carries the new opening parity and is eligible, so
 * the second fires on it and flips the parity back (two half-frames lost,
 * opening parity unchanged). To flip several excursions, give each item a
 * second after the end of the previous excursion: it then fires on the
 * first opening half-frame of the next one.
 *
 * DO NOT COMBINE with GROOVY_SKIP_FRAME_EVERY: the tracked opening parity
 * assumes that the first half-frame of an excursion is actually sent, and
 * that setting skips frame 0 of the session.
 */

#ifndef GROOVY_INJECT_H
#define GROOVY_INJECT_H

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define GROOVY_INJECT_MAX_ITEMS        8u
#define GROOVY_INJECT_MAX_AT_S         86400u
#define GROOVY_INJECT_AUDIO_MAX_MS     2000u
#define GROOVY_INJECT_DELAY_MAX_MS     50u
#define GROOVY_INJECT_STALL_MAX_MS     500u
#define GROOVY_INJECT_DELAY_WINDOW_S   30u

#define GROOVY_INJECT_AT_DIGITS_MAX    5u
#define GROOVY_INJECT_PARAM_DIGITS_MAX 4u

enum groovy_inject_kind
{
   GROOVY_INJECT_SKIP   = 0,
   GROOVY_INJECT_PARITY = 1,
   GROOVY_INJECT_AUDIO  = 2,
   GROOVY_INJECT_DELAY  = 3,
   GROOVY_INJECT_STALL  = 4
};

/* One parsed item. Field order and widths are fixed: the test harness
 * mirrors this layout. */
struct groovy_inject_item
{
   uint32_t kind;          /* enum groovy_inject_kind */
   uint32_t at_s;          /* seconds from the session origin */
   uint32_t param_ms;      /* 0 for skip and parity */
   uint32_t fired;         /* 0 = pending, 1 = already fired */
   uint32_t fired_frame;   /* frame_id at the instant of firing */
   uint32_t reserved;
   uint64_t fired_ns;      /* elapsed ns at the instant of firing */
};

typedef char groovy_inject_item_size_check
   [(sizeof(struct groovy_inject_item) == 32u) ? 1 : -1];

/* Tracked opening parity of the interlaced excursion. */
struct groovy_inject_parity
{
   uint32_t in_excursion;  /* 1 while interlaced one field per frame */
   uint32_t open_parity;   /* parity of the opening half-frame */
};

static inline const char *groovy_inject_kind_name(uint32_t kind)
{
   switch (kind)
   {
      case GROOVY_INJECT_SKIP:   return "skip";
      case GROOVY_INJECT_PARITY: return "parity";
      case GROOVY_INJECT_AUDIO:  return "audio";
      case GROOVY_INJECT_DELAY:  return "delay";
      case GROOVY_INJECT_STALL:  return "stall";
      default:                   break;
   }
   return "?";
}

/* Reads a run of decimal digits at *pp. The first character must itself be
 * a digit (strtoul would tolerate blanks and a sign), and the run is at
 * most max_digits long. Returns 1 and advances *pp on success. */
static inline int groovy_inject_read_number(const char **pp,
      unsigned max_digits, uint32_t *value)
{
   const char *p = *pp;
   unsigned    digits = 0;
   uint32_t    v = 0;

   while (*p >= '0' && *p <= '9')
   {
      if (++digits > max_digits)
         return 0;
      v = v * 10u + (uint32_t)(*p - '0');
      p++;
   }
   if (digits == 0)
      return 0;
   *value = v;
   *pp    = p;
   return 1;
}

/* Maps the kind word at *pp, which must be followed by '@'. Returns the
 * kind, or -1; advances *pp past the '@' on success. */
static inline int groovy_inject_read_kind(const char **pp)
{
   static const struct { const char *name; size_t len; int kind; } table[] =
   {
      { "skip",   4, GROOVY_INJECT_SKIP   },
      { "parity", 6, GROOVY_INJECT_PARITY },
      { "audio",  5, GROOVY_INJECT_AUDIO  },
      { "delay",  5, GROOVY_INJECT_DELAY  },
      { "stall",  5, GROOVY_INJECT_STALL  }
   };
   unsigned i;

   for (i = 0; i < sizeof(table) / sizeof(table[0]); i++)
   {
      if (   strncmp(*pp, table[i].name, table[i].len) == 0
          && (*pp)[table[i].len] == '@')
      {
         *pp += table[i].len + 1u;
         return table[i].kind;
      }
   }
   return -1;
}

/* Parses GROOVY_INJECT into out[0..cap). Returns the number of items; 0 when
 * s is NULL, empty or refused, and then out[0..cap) is zeroed. */
static inline unsigned groovy_inject_parse(const char *s,
      struct groovy_inject_item *out, unsigned cap)
{
   const char *p = s;
   unsigned    n = 0;

   if (out && cap)
      memset(out, 0, (size_t)cap * sizeof(*out));
   if (!s || s[0] == '\0' || !out)
      return 0;

   for (;;)
   {
      int      kind;
      uint32_t at_s  = 0;
      uint32_t param = 0;

      if (n >= GROOVY_INJECT_MAX_ITEMS || n >= cap)
         goto refuse;
      if ((kind = groovy_inject_read_kind(&p)) < 0)
         goto refuse;
      if (!groovy_inject_read_number(&p, GROOVY_INJECT_AT_DIGITS_MAX, &at_s))
         goto refuse;
      if (at_s > GROOVY_INJECT_MAX_AT_S)
         goto refuse;

      if (kind == GROOVY_INJECT_AUDIO
       || kind == GROOVY_INJECT_DELAY
       || kind == GROOVY_INJECT_STALL)
      {
         uint32_t max_ms = GROOVY_INJECT_AUDIO_MAX_MS;

         if (kind == GROOVY_INJECT_DELAY)
            max_ms = GROOVY_INJECT_DELAY_MAX_MS;
         else if (kind == GROOVY_INJECT_STALL)
            max_ms = GROOVY_INJECT_STALL_MAX_MS;
         if (*p != ':')
            goto refuse;
         p++;
         if (!groovy_inject_read_number(&p, GROOVY_INJECT_PARAM_DIGITS_MAX,
                  &param))
            goto refuse;
         if (param < 1u || param > max_ms)
            goto refuse;
      }

      out[n].kind     = (uint32_t)kind;
      out[n].at_s     = at_s;
      out[n].param_ms = param;
      n++;

      if (*p == '\0')
         return n;
      if (*p != ',')
         goto refuse;
      p++;
   }

refuse:
   memset(out, 0, (size_t)cap * sizeof(*out));
   return 0;
}

/* Index of the first item of this kind that has not fired and whose second
 * has been reached (at_s * 10^9 <= elapsed_ns), else -1. No mutation. */
static inline int groovy_inject_pending(const struct groovy_inject_item *items,
      unsigned n, uint32_t kind, uint64_t elapsed_ns)
{
   unsigned i;

   if (!items)
      return -1;
   for (i = 0; i < n; i++)
   {
      if (   items[i].kind == kind
          && !items[i].fired
          && (uint64_t)items[i].at_s * 1000000000ull <= elapsed_ns)
         return (int)i;
   }
   return -1;
}

/* Marks an item as fired so that it never becomes due again. */
static inline void groovy_inject_fire(struct groovy_inject_item *it,
      uint32_t frame_id, uint64_t now_ns)
{
   if (!it)
      return;
   it->fired       = 1u;
   it->fired_frame = frame_id;
   it->fired_ns    = now_ns;
}

/* Called once per frame with the current video shape. Outside interlaced
 * one-field-per-frame the excursion ends; on the first frame of a new
 * excursion the opening parity is that frame's parity. */
static inline void groovy_inject_parity_observe(struct groovy_inject_parity *p,
      int is_480i_fpf, uint32_t frame_id)
{
   if (!p)
      return;
   if (!is_480i_fpf)
   {
      p->in_excursion = 0u;
      return;
   }
   if (!p->in_excursion)
   {
      p->in_excursion = 1u;
      p->open_parity  = frame_id & 1u;
   }
}

/* True only for an opening half-frame: interlaced one field per frame,
 * inside an excursion, and a frame_id whose parity is the tracked opening
 * parity. */
static inline int groovy_inject_parity_eligible(
      const struct groovy_inject_parity *p, int is_480i_fpf, uint32_t frame_id)
{
   if (!p || !is_480i_fpf || !p->in_excursion)
      return 0;
   return (frame_id & 1u) == p->open_parity;
}

/* A parity fault has fired: the following pairs open on the other parity. */
static inline void groovy_inject_parity_fired(struct groovy_inject_parity *p)
{
   if (p)
      p->open_parity ^= 1u;
}

/* Half-open window: active from start_ns included to start_ns + dur_ns
 * excluded. Written as a subtraction so the sum cannot wrap. */
static inline int groovy_inject_window_active(uint64_t start_ns,
      uint64_t dur_ns, uint64_t now_ns)
{
   return now_ns >= start_ns && (now_ns - start_ns) < dur_ns;
}

#endif /* GROOVY_INJECT_H */
