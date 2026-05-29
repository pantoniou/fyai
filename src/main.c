/*
 * main.c - fyai command-line entry point
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * Grammar: fyai [global-options] [verb] [verb-args]
 *          fyai [global-options] <prompt...>
 *          fyai [global-options] -          (read the prompt from stdin)
 *          fyai [global-options]            (no verb/prompt: interactive)
 *
 * Global options are parsed first (getopt stops at the first non-option via
 * the leading '+'). The first non-option token is dispatched as a verb when
 * it matches a known verb; otherwise the remaining arguments are run as a
 * prompt. A lone "-" reads the prompt from stdin; with nothing left at all,
 * fyai enters interactive mode.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>
#include <unistd.h>

#include "fyai.h"
#include "fyai_config.h"
#include "fyai_storage.h"
#include "commands.h"
#include "fyai_terminal.h"
#include "fyai_prof.h"

int main(int argc, char **argv)
{
	struct fyai_cfg cfg = { };
	int rc, ret = EXIT_FAILURE;

	fyai_prof_init();

	/* 256MB of stack */
	raise_stack(256LU << 20, argv);
	fyai_reserve_arena_ranges();
	curl_global_init(CURL_GLOBAL_DEFAULT);

	rc = fyai_config_setup(&cfg, argc, argv);
	if (rc)
		return rc > 0 ? EXIT_SUCCESS : EXIT_FAILURE;

	rc = fyai_run(&cfg);
	ret = rc ? EXIT_FAILURE : EXIT_SUCCESS;

	fyai_config_cleanup(&cfg);
	curl_global_cleanup();
	fyai_prof_report();
	return ret;
}
