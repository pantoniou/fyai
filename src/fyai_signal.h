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

#include "fyai_event.h"

/*
 * Install the interactive-session handlers: SIGINT sets a flag (the request
 * path polls it and aborts the in-flight call), SIGWINCH tells linenoise to
 * reflow the prompt. Installed without SA_RESTART so blocking reads/polls
 * see EINTR promptly. Batch runs never call this - default dispositions
 * (^C exits) stay in force there.
 *
 * These stay installed for the whole session. A wait that can act on a signal
 * borrows it as an event source for the duration of the wait; the event layer
 * restores the disposition installed here when the source is removed.
 */
void fyai_signals_install(void);

/* What a borrower does with a resize, beyond marking the width stale. */
struct fyai_signal_winch {
	enum fyai_event_action (*fn)(void *user);
	void *user;
};

/* Deliver SIGWINCH to @el synchronously for as long as *@srcp lives, instead
 * of to the handler above. */
int fyai_signals_attach_winch(struct fyai_event_loop *el,
			      struct fyai_signal_winch *w,
			      struct fyai_event_source **srcp);

/* True when a SIGINT arrived since the last check (does not clear). */
bool fyai_sig_intr_pending(void);

/* Return-and-clear the pending SIGINT flag. */
bool fyai_sig_intr_check(void);

#endif
