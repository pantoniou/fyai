/*
 * fyai_test_main.c - single main() driving the registered unit tests
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 *
 * Each test runs in a child process. The parent reports the child status.
 *
 * Usage:
 *   fyai_test                 run every registered test, report a summary
 *   fyai_test --list          print every suite/name, one per line
 *   fyai_test <suite>         run one suite
 *   fyai_test <suite>/<name>  run one test
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "fyai_test_registry.h"
#include "fyai_test_scratch.h"

/* Run one test in a child process. */
static int run_one(const struct fyai_test_case *tc)
{
	pid_t pid;
	int status, rc;

	fflush(stdout);
	fflush(stderr);

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return 1;
	}

	if (pid == 0) {
		/* The parent owns its scratch; this child owns only what it
		 * makes from here. */
		fyai_test_scratch_forget();
		/* Do not run inherited exit handlers. */
		rc = tc->run();
		fflush(stdout);
		fflush(stderr);
		fyai_test_scratch_cleanup();
		_exit(rc ? 1 : 0);
	}

	if (waitpid(pid, &status, 0) < 0) {
		perror("waitpid");
		return 1;
	}

	if (WIFEXITED(status))
		return WEXITSTATUS(status) == 0 ? 0 : 1;

	if (WIFSIGNALED(status)) {
		fprintf(stderr, "%s/%s: terminated by signal %d\n",
			tc->suite, tc->name, WTERMSIG(status));
		return 1;
	}

	return 1;
}

static void list_all(void)
{
	int i;
	const struct fyai_test_case *tc;

	for (i = 0; i < fyai_test_count(); i++) {
		tc = fyai_test_at(i);
		printf("%s/%s\n", tc->suite, tc->name);
	}
}

/* Run one test suite. */
static int run_suite(const char *suite, int *ran)
{
	int i, failed = 0;
	const struct fyai_test_case *tc;

	*ran = 0;
	for (i = 0; i < fyai_test_count(); i++) {
		tc = fyai_test_at(i);
		if (strcmp(tc->suite, suite))
			continue;
		(*ran)++;
		if (run_one(tc)) {
			printf("FAIL %s/%s\n", tc->suite, tc->name);
			failed++;
		} else {
			printf("PASS %s/%s\n", tc->suite, tc->name);
		}
	}
	return failed;
}

static int run_all(void)
{
	int i, failed = 0, total = 0;
	const struct fyai_test_case *tc;

	for (i = 0; i < fyai_test_count(); i++) {
		tc = fyai_test_at(i);
		total++;
		if (run_one(tc)) {
			printf("FAIL %s/%s\n", tc->suite, tc->name);
			failed++;
		} else {
			printf("PASS %s/%s\n", tc->suite, tc->name);
		}
	}

	printf("%d/%d tests passed\n", total - failed, total);
	return failed ? 1 : 0;
}

int main(int argc, char *argv[])
{
	const struct fyai_test_case *tc;
	const char *arg, *slash;
	char suite[128], name[128];
	int failed, ran;

	fyai_test_register_all();

	/* One directory holds the scratch of the run, so a test that crashes
	 * leaves nothing outside it. */
	if (fyai_test_scratch_root()) {
		fprintf(stderr, "cannot make the scratch directory\n");
		return 2;
	}

	if (argc > 1 && !strcmp(argv[1], "--list")) {
		list_all();
		return 0;
	}

	if (argc == 1)
		return run_all();

	if (argc != 2) {
		fprintf(stderr,
			"usage: %s [--list | <suite> | <suite>/<name>]\n",
			argv[0]);
		return 2;
	}

	arg = argv[1];
	slash = strchr(arg, '/');
	if (slash) {
		if ((size_t)(slash - arg) >= sizeof(suite) ||
		    strlen(slash + 1) >= sizeof(name)) {
			fprintf(stderr, "test name too long: %s\n", arg);
			return 2;
		}
		memcpy(suite, arg, (size_t)(slash - arg));
		suite[slash - arg] = '\0';
		strcpy(name, slash + 1);

		tc = fyai_test_find(suite, name);
		if (!tc) {
			fprintf(stderr, "no such test: %s/%s\n", suite, name);
			return 2;
		}
		return run_one(tc);
	}

	failed = run_suite(arg, &ran);
	if (!ran) {
		fprintf(stderr, "no such suite: %s\n", arg);
		return 2;
	}
	return failed ? 1 : 0;
}
