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

#include "fyai_transport_boot.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>
#include <unistd.h>

#include "fyai.h"
#include "fyai_claude_sdk.h"
#include "fyai_remote.h"
#include "fyai_config.h"
#include "fyai_storage.h"
#include "commands.h"
#include "fyai_crash.h"
#include "fyai_terminal.h"
#include "fyai_prof.h"
#include "fyai_app.h"

/*
 * libcurl initialises itself, and OpenSSL with it, when the first handle is
 * made. A verb that makes no request thus never pays for it. Handles are made
 * on the thread of the event loop, so the implicit initialisation is safe.
 */
int main(int argc, char **argv)
{
	struct fyai_app app;
	struct fyai_cfg *cfg = &app.cfg;
	char **sdk_argv, *attach, *next, *attach_dir, *attach_return;
	bool attach_watch;
	int rc, ret = EXIT_FAILURE;

	fyai_prof_init();

	/* Before anything else can fault: report a backtrace, then die. */
	fyai_crash_install();

	/* 256MB of stack */
	raise_stack(256LU << 20, argv);
	fyai_reserve_arena_ranges();

	/*
	 * A Claude Agent SDK runs this program as its CLI. Its command line is
	 * the argument list of the claude-sdk command, whose grammar is not
	 * that of the global options. A program that runs this one again takes
	 * the command line with the command.
	 */
	sdk_argv = fyai_claude_sdk_args(argc, argv);
	if (sdk_argv) {
		argc++;
		argv = sdk_argv;
	}

	rc = fyai_app_init(&app);
	if (rc) {
		/* No sink exists yet, so the report is written at once. */
		fyai_diagf(NULL, FYAIET_ERROR, FYAIEM_INIT, __FILE__, __LINE__,
			   __func__, "cannot make the diagnostics of the program");
		free(sdk_argv);
		return EXIT_FAILURE;
	}
	rc = fyai_config_setup(cfg, argc, argv);
	/*
	 * Report what the option parsing raised before the verb runs - it has
	 * no context, so this is the only boundary its diagnostics reach. A
	 * failed setup released cfg itself; the diagnostics stay with the app.
	 */
	if (rc) {
		fyai_diag_drain(&app.diag);
		fyai_app_cleanup(&app);
		free(sdk_argv);
		return rc > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
	}
	cfg->argv = argv;

	/*
	 * With isolation on, this image is the bootstrap: it starts the
	 * transport and executes itself again without a credential. That does
	 * not return. There is no fallback to a direct connection. The next
	 * image runs the same setup and raises what it raised, so the setup is
	 * reported after the bootstrap: once, by the image that runs the verb,
	 * or with the cause when the bootstrap stops.
	 */
	if (fyai_transport_bootstrap(cfg, argc, argv)) {
		fyai_diag_drain(&app.diag);
		fyai_app_cleanup(&app);
		curl_global_cleanup();
		free(sdk_argv);
		return EXIT_FAILURE;
	}
	fyai_diag_drain(&app.diag);

	rc = fyai_run(&app);
	ret = rc ? EXIT_FAILURE : cfg->exit_status;
	if (!rc && cfg->reload_branch) {
		ret = fyai_config_reload_exec(cfg) ? EXIT_FAILURE : EXIT_SUCCESS;
		fyai_diag_drain(&app.diag);
	}

	attach = cfg->attach_branch;
	attach_dir = cfg->attach_dir;
	attach_return = cfg->attach_return;
	attach_watch = cfg->attach_watch;
	next = cfg->next_session;
	cfg->attach_branch = cfg->next_session = NULL;
	cfg->attach_dir = cfg->attach_return = NULL;
	if (rc) {
		free(attach);
		free(next);
		attach = next = NULL;
	}
	fyai_app_cleanup(&app);
	curl_global_cleanup();
	fyai_prof_report();
	/* Exec-attach to a live server session; returns only on failure. */
	if (attach) {
		(void)fyai_remote_attach_exec(argv[0], attach_dir, attach, attach_watch,
					      attach_return);
		perror("fyai: cannot start the client of the live session");
		free(attach);
		ret = EXIT_FAILURE;
	}
	free(attach_dir);
	free(attach_return);
	/* Exec follow-up session after handoff; returns only on error. */
	if (next) {
		(void)fyai_remote_next_exec(argv[0], next);
		perror("fyai: cannot start the next session");
		free(next);
		ret = EXIT_FAILURE;
	}
	free(sdk_argv);
	return ret;
}
