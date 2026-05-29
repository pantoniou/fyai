/*
 * fyai_signal.h - interactive-session signal handling
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_SIGNAL_H
#define FYAI_SIGNAL_H

#include <stdbool.h>

/*
 * Install the interactive-session handlers: SIGINT sets a flag (the request
 * path polls it and aborts the in-flight call), SIGWINCH tells linenoise to
 * reflow the prompt. Installed without SA_RESTART so blocking reads/polls
 * see EINTR promptly. Batch runs never call this - default dispositions
 * (^C exits) stay in force there.
 */
void fyai_signals_install(void);

/* True when a SIGINT arrived since the last check (does not clear). */
bool fyai_sig_intr_pending(void);

/* Return-and-clear the pending SIGINT flag. */
bool fyai_sig_intr_check(void);

#endif
