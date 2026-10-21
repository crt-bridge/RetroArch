/* SPDX-License-Identifier: GPL-3.0-or-later */
/* groovy_bench.h -- switch for the bench-only diagnostics of the groovy
 * record driver.
 *
 * WHAT IT DOES. The bench tools (fault injection, deliberate frame skip,
 * measurement log lines) are compiled in only when the build passes
 * HAVE_GROOVY_BENCH=1 to make, which defines HAVE_GROOVY_BENCH. The
 * release build never sets it, so its binary carries none of their
 * environment variable names nor of their log formats.
 *
 * Under HAVE_GROOVY_BENCH, GROOVY_BENCH_LOG and GROOVY_BENCH_WARN are the
 * ordinary RARCH_LOG and RARCH_WARN. Without it, they expand to nothing:
 * the preprocessor drops their arguments, and with them the format string
 * literal, so no trace of the line reaches the object file. A guard around
 * the computation is not enough for that, because a literal left in a call
 * that is merely never reached stays in the binary.
 *
 * Without HAVE_GROOVY_BENCH, the functions of groovy_inject.h and
 * groovy_skip.h are also replaced by inert macros. This header MUST be
 * included after every groovy_*.h header: the macros shadow the functions
 * at each call site, and the headers themselves stay whole, so the pure
 * tests that include them outside RetroArch are unaffected. The arguments
 * of these macros are never evaluated, so none of them may carry a side
 * effect.
 */

#ifndef GROOVY_BENCH_H
#define GROOVY_BENCH_H

#ifdef HAVE_GROOVY_BENCH

#define GROOVY_BENCH_LOG(...)   RARCH_LOG(__VA_ARGS__)
#define GROOVY_BENCH_WARN(...)  RARCH_WARN(__VA_ARGS__)

#else

#define GROOVY_BENCH_LOG(...)   ((void)0)
#define GROOVY_BENCH_WARN(...)  ((void)0)

/* groovy_skip.h: no variable is read, nothing is ever skipped. */
#define groovy_skip_every_parse(raw)                                  (0u)

/* groovy_inject.h: no variable is read, no fault is ever due. */
#define groovy_inject_open(st)                                        ((void)0)
#define groovy_inject_pending(items, n, kind, elapsed_ns)             (-1)
#define groovy_inject_note_fired(st, it, frame_id, t_ns)              ((void)0)
#define groovy_inject_busy_wait_ns(ns)                                ((void)0)
#define groovy_inject_elapsed_ns(st, now_ns)                          (0u)
#define groovy_inject_window_active(start_ns, dur_ns, now_ns)         (0)
#define groovy_inject_parity_observe(p, is_480i_fpf, frame_id)        ((void)0)
#define groovy_inject_parity_eligible(p, is_480i_fpf, frame_id)       (0)
#define groovy_inject_parity_fired(p)                                 ((void)0)

#endif /* HAVE_GROOVY_BENCH */

#endif /* GROOVY_BENCH_H */
