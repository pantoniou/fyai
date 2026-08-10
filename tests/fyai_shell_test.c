/*
 * fyai_shell_test.c - tests for the shell-capture options
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_UNKNOWN

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "fyai.h"
#include "fyai_event.h"
#include "fyai_tools.h"
#include "utils.h"
#include "fyai_test.h"

#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(shell, timeout_stops_command, shell_timeout_stops_command)
FYAI_TEST_ENTRY(shell, timeout_stops_descendant, shell_timeout_stops_descendant)
FYAI_TEST_ENTRY(shell, completion_ignores_open_descendant, shell_completion_ignores_open_descendant)
FYAI_TEST_ENTRY(shell, timeout_spares_quick, shell_timeout_spares_quick)
FYAI_TEST_ENTRY(shell, workdir_changes_dir, shell_workdir_changes_directory)
FYAI_TEST_ENTRY(shell, bad_workdir_reports_125, shell_bad_workdir_reports_125)
FYAI_TEST_ENTRY(shell, native_timeout_honored, shell_native_timeout_honored)
FYAI_TEST_ENTRY(shell, native_timeout_bounded, shell_native_timeout_bounded)
FYAI_TEST_ENTRY(shell, native_timeout_output, shell_native_timeout_output)

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

int shell_timeout_stops_descendant(void)
{
	struct shell_command_opts opts = { .timeout_ms = 200 };
	struct shell_command_result result = {};
	char *end;
	long child;
	int alive;
	int rc;
	int i;

	rc = shell_run("sh -c 'trap \"\" TERM; sleep 30' & echo $!; wait",
		       &opts, &result);
	FYAI_TCHECK(rc == 0);
	FYAI_TCHECK(result.timed_out);
	child = strtol(result.stdout_data, &end, 10);
	FYAI_TCHECK(child > 0 && end != result.stdout_data);

	alive = 1;
	for (i = 0; i < 200; i++) {
		rc = kill((pid_t)child, 0);
		if (rc < 0 && errno == ESRCH) {
			alive = 0;
			break;
		}
		usleep(10000);
	}
	if (alive)
		(void)kill((pid_t)child, SIGKILL);
	FYAI_TCHECK(!alive);

	shell_command_result_cleanup(&result);
	shell_teardown();
	printf("ok - a shell time limit stops a descendant\n");
	return 0;
}

int shell_completion_ignores_open_descendant(void)
{
	struct shell_command_result result = {};
	int rc;

	rc = shell_run("echo captured; sleep 30 & exit 0", NULL, &result);
	FYAI_TCHECK(rc == 0);
	FYAI_TCHECK(!result.signaled);
	FYAI_TCHECK(result.exit_code == 0);
	FYAI_TCHECK(!strcmp(result.stdout_data, "captured\n"));

	shell_command_result_cleanup(&result);
	shell_teardown();
	printf("ok - shell completion ignores an open descendant\n");
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

/*
 * Test the native Responses `shell_call` path. Read the time limit from
 * `timeout_ms` in the action. Keep the standard result sequence of
 * {stdout, stderr, outcome} after a timeout.
 */
static struct fy_generic_builder *native_setup(long long action_timeout_ms,
					       long long default_timeout_ms,
					       long long max_timeout_ms,
					       const char *command,
					       fy_generic *callp)
{
	struct fy_generic_builder_cfg gb_cfg = {
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED,
	};
	struct fy_generic_builder *gb;

	memset(&test_cfg, 0, sizeof(test_cfg));
	memset(&test_ctx, 0, sizeof(test_ctx));
	test_ctx.cfg = &test_cfg;
	test_cfg.shell_timeout_ms = default_timeout_ms;
	test_cfg.shell_max_timeout_ms = max_timeout_ms;

	gb = fy_generic_builder_create(&gb_cfg);
	if (!gb)
		return NULL;
	test_ctx.transient_gb = gb;
	test_ctx.gb = gb;
	test_ctx.durable_gb = gb;

	*callp = fy_mapping(gb,
		"type", "shell_call",
		"call_id", "call_shell_test",
		"action", fy_mapping(gb,
			"commands", fy_sequence(gb, command),
			"timeout_ms", action_timeout_ms));
	return gb;
}

static void native_teardown(struct fy_generic_builder *gb)
{
	shell_teardown();
	fy_generic_builder_destroy(gb);
	test_ctx.transient_gb = NULL;
	test_ctx.gb = NULL;
	test_ctx.durable_gb = NULL;
}

/* Return the elapsed time of one native shell call in milliseconds. */
static long long native_run(fy_generic call, fy_generic *resultp, bool *okp)
{
	struct timespec t0, t1;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	*resultp = fyai_execute_tool_call(&test_ctx, call, okp);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	return (t1.tv_sec - t0.tv_sec) * 1000 +
	       (t1.tv_nsec - t0.tv_nsec) / 1000000;
}

int shell_native_timeout_honored(void)
{
	struct fy_generic_builder *gb;
	fy_generic call, result, outcome;
	long long elapsed;
	bool ok = true;

	/*
	 * The action requests 200 ms, and the configured default is 10 s.
	 * The action limit must have priority.
	 */
	gb = native_setup(200, 10000, 600000, "sleep 10", &call);
	FYAI_TCHECK(gb);

	elapsed = native_run(call, &result, &ok);
	FYAI_TCHECK(!ok);
	FYAI_TCHECK(elapsed < 3000);

	outcome = fy_get_at_path(result, 0, "outcome");
	FYAI_TCHECK(fy_equal(fy_get(outcome, "type"), "timeout"));
	FYAI_TCHECK(fy_get(outcome, "timeout_ms", 0LL) == 200);
	/* A timeout must not change the result shape. */
	FYAI_TCHECK(fy_generic_is_sequence(result));
	FYAI_TCHECK(fy_generic_is_string(fy_get_at_path(result, 0, "stdout")));
	FYAI_TCHECK(fy_generic_is_string(fy_get_at_path(result, 0, "stderr")));

	native_teardown(gb);
	printf("ok - a native shell call honors its own time limit\n");
	return 0;
}

int shell_native_timeout_bounded(void)
{
	struct fy_generic_builder *gb;
	fy_generic call, result, outcome;
	long long elapsed;
	bool ok = true;

	/* Limit a large request to shell/max_timeout_ms. */
	gb = native_setup(600000, 10000, 200, "sleep 10", &call);
	FYAI_TCHECK(gb);

	elapsed = native_run(call, &result, &ok);
	FYAI_TCHECK(!ok);
	FYAI_TCHECK(elapsed < 3000);

	outcome = fy_get_at_path(result, 0, "outcome");
	FYAI_TCHECK(fy_equal(fy_get(outcome, "type"), "timeout"));
	FYAI_TCHECK(fy_get(outcome, "timeout_ms", 0LL) == 200);

	native_teardown(gb);
	printf("ok - a native shell time limit is bounded by the maximum\n");
	return 0;
}

int shell_native_timeout_output(void)
{
	struct fy_generic_builder *gb;
	fy_generic call, result, out;
	bool ok = true;

	gb = native_setup(400, 10000, 600000, "echo before; sleep 10", &call);
	FYAI_TCHECK(gb);

	(void)native_run(call, &result, &ok);
	FYAI_TCHECK(!ok);

	/* Keep the output that the command produced before the timeout. */
	out = fy_get_at_path(result, 0, "stdout");
	FYAI_TCHECK(fy_generic_is_string(out));
	FYAI_TCHECK(strstr(fy_castp(&out, ""), "before"));
	/*
	 * The outcome identifies the time limit, not the termination signal.
	 * A signal outcome incorrectly indicates a command failure.
	 */
	FYAI_TCHECK(fy_equal(fy_get_at_path(result, 0, "outcome", "type"),
			     "timeout"));

	native_teardown(gb);
	printf("ok - a timed-out native shell call keeps its output\n");
	return 0;
}
