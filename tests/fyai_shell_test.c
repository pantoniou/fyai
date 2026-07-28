/*
 * fyai_shell_test.c - tests for the shell-capture options
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_UNKNOWN

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fyai.h"
#include "fyai_event.h"
#include "utils.h"
#include "fyai_test.h"

#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(shell, timeout_stops_command, shell_timeout_stops_command)
FYAI_TEST_ENTRY(shell, timeout_spares_quick, shell_timeout_spares_quick)
FYAI_TEST_ENTRY(shell, workdir_changes_dir, shell_workdir_changes_directory)
FYAI_TEST_ENTRY(shell, bad_workdir_reports_125, shell_bad_workdir_reports_125)

static struct fyai_cfg test_cfg;
static struct fyai_ctx test_ctx = { .cfg = &test_cfg };

static int shell_run(const char *command,
		     const struct shell_command_opts *opts,
		     struct shell_command_result *result)
{
	return run_shell_command_capture_cb(&test_ctx, command, result,
					    NULL, NULL, NULL, opts);
}

static void shell_teardown(void)
{
	if (test_ctx.el) {
		fyai_event_loop_destroy(test_ctx.el);
		test_ctx.el = NULL;
	}
	fyai_event_pool_drain(&test_ctx);
}

int shell_timeout_stops_command(void)
{
	struct shell_command_opts opts = { .timeout_ms = 200 };
	struct shell_command_result result = {};
	int rc;

	rc = shell_run("sleep 30", &opts, &result);
	FYAI_TCHECK(rc == 0);
	FYAI_TCHECK(result.timed_out);
	FYAI_TCHECK(result.signaled);

	shell_command_result_cleanup(&result);
	shell_teardown();
	printf("ok - a shell time limit stops the command\n");
	return 0;
}

int shell_timeout_spares_quick(void)
{
	struct shell_command_opts opts = { .timeout_ms = 30000 };
	struct shell_command_result result = {};
	int rc;

	rc = shell_run("echo quick", &opts, &result);
	FYAI_TCHECK(rc == 0);
	FYAI_TCHECK(!result.timed_out);
	FYAI_TCHECK(!result.signaled);
	FYAI_TCHECK(result.exit_code == 0);
	FYAI_TCHECK(!strcmp(result.stdout_data, "quick\n"));

	shell_command_result_cleanup(&result);
	shell_teardown();
	printf("ok - a shell time limit spares a quick command\n");
	return 0;
}

int shell_workdir_changes_directory(void)
{
	struct shell_command_opts opts = { .workdir = "/" };
	struct shell_command_result result = {};
	int rc;

	rc = shell_run("pwd", &opts, &result);
	FYAI_TCHECK(rc == 0);
	FYAI_TCHECK(result.exit_code == 0);
	FYAI_TCHECK(!strcmp(result.stdout_data, "/\n"));

	shell_command_result_cleanup(&result);
	shell_teardown();
	printf("ok - a shell workdir changes the directory\n");
	return 0;
}

int shell_bad_workdir_reports_125(void)
{
	struct shell_command_opts opts = {
		.workdir = "/nonexistent-fyai-shell-test",
	};
	struct shell_command_result result = {};
	int rc;

	rc = shell_run("pwd", &opts, &result);
	FYAI_TCHECK(rc == 0);
	FYAI_TCHECK(!result.signaled);
	FYAI_TCHECK(result.exit_code == 125);
	FYAI_TCHECK(!*result.stdout_data);

	shell_command_result_cleanup(&result);
	shell_teardown();
	printf("ok - an unusable shell workdir is reported\n");
	return 0;
}
