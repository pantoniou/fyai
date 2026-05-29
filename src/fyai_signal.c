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

#include <linenoise.h>

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

	/*
	 * Deliberately no SA_RESTART: linenoise's read() and curl's poll must
	 * come back with EINTR so a resize reflows the prompt immediately and
	 * a ^C aborts the in-flight request at the next progress callback.
	 */
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;

	sa.sa_handler = fyai_sigint_handler;
	sigaction(SIGINT, &sa, NULL);

	sa.sa_handler = fyai_sigwinch_handler;
	sigaction(SIGWINCH, &sa, NULL);
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
