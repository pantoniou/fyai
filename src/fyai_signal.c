/*
 * fyai_signal.c - interactive-session signal handling
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <signal.h>
#include <stddef.h>

#include <linenoise.h>

#include "fyai_event.h"
#include "fyai_signal.h"

static volatile sig_atomic_t fyai_sig_intr;

static void fyai_sigint_handler(int sig)
{
	(void)sig;
	fyai_sig_intr = 1;
}

static void fyai_sigwinch_handler(int sig)
{
	(void)sig;
	linenoiseWindowChanged();
}

void fyai_signals_install(void)
{
	struct sigaction sa;

	/* Leave waits interruptible so ^C reaches curl's progress callback. */
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;

	sa.sa_handler = fyai_sigint_handler;
	sigaction(SIGINT, &sa, NULL);

	sa.sa_handler = fyai_sigwinch_handler;
	sigaction(SIGWINCH, &sa, NULL);
}

/* The forwarding shim. */
static enum fyai_event_action fyai_sigwinch_cb(const struct fyai_event *ev)
{
	const struct fyai_signal_winch *w = ev->userdata;

	linenoiseWindowChanged();
	if (!w || !w->fn)
		return FYAIEA_CONTINUE;
	return w->fn(w->user);
}

int fyai_signals_attach_winch(struct fyai_event_loop *el,
			      struct fyai_signal_winch *w,
			      struct fyai_event_source **srcp)
{
	return fyai_event_add_signal(el, SIGWINCH, fyai_sigwinch_cb, w, srcp);
}

bool fyai_sig_intr_pending(void)
{
	return fyai_sig_intr != 0;
}

bool fyai_sig_intr_check(void)
{
	if (!fyai_sig_intr)
		return false;
	fyai_sig_intr = 0;
	return true;
}
