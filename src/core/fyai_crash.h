/*
 * fyai_crash.h - fatal signal backtrace
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_CRASH_H
#define FYAI_CRASH_H

#include <stdbool.h>

/*
 * Catch fatal signals and report a stack backtrace before dying. The
 * handler writes to standard error until fyai_crash_set_fd() aims it at
 * the preserved diagnostic descriptor, then re-raises the signal so the
 * exit status (and any core dump) still names the cause. Safe to call
 * before any context exists; forked children inherit the handling.
 */
void fyai_crash_install(void);

/* Direct later crash output at @fd (the duped stderr of fyai_signals_open). */
void fyai_crash_set_fd(int fd);

/* True when a fatal @signo reaches the crash handler. Unit tests only. */
bool fyai_crash_armed(int signo);

/* Restore the previous dispositions. */
void fyai_crash_uninstall(void);

#endif
