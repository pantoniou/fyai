/*
 * fyai_prof.h - lightweight, opt-in phase profiling.
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * Enabled only when the FYAI_PROFILE environment variable is set (to any
 * non-empty value). When disabled every call is a cheap no-op / inline test,
 * so instrumentation can stay in the hot path. Timings are wall-clock
 * (CLOCK_MONOTONIC) microseconds accumulated per named label; a summary is
 * emitted to stderr at process exit via fyai_prof_report().
 */

#ifndef FYAI_PROF_H
#define FYAI_PROF_H

#include <stdbool.h>
#include <time.h>

/* Read FYAI_PROFILE and latch the process base time. Call once, early. */
void fyai_prof_init(void);

bool fyai_prof_enabled(void);

/* Sample the monotonic clock into *ts (only meaningful when enabled). */
void fyai_prof_stamp(struct timespec *ts);

/* Accumulate the interval [from, now] under label (count++, sum, min, max). */
void fyai_prof_since(const char *label, const struct timespec *from);

/* One-shot: record now-relative-to-process-base under label, first call wins.
 * Used for "time from process start to first request send". */
void fyai_prof_once_from_base(const char *label);

/* Emit the accumulated table to stderr. */
void fyai_prof_report(void);

#endif /* FYAI_PROF_H */
