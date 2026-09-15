/*
 * fyai_crash_test.c - unit tests for the fatal signal backtrace
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fyai_crash.h"

#include "fyai_test_registry.h"
#include "fyai_test.h"

FYAI_TEST_ENTRY(crash, install, crash_install_run)
FYAI_TEST_ENTRY(crash, report, crash_report_run)

/*
 * Installing twice keeps one handling; uninstall restores the default. A
 * signal already owned (a sanitizer under test, the watchdog in fyai)
 * stays with its owner: armed then only names what this installed.
 */
int crash_install_run(void)
{
	struct sigaction sa, before_segv, before_abrt;

	FYAI_TCHECK(sigaction(SIGSEGV, NULL, &before_segv) == 0);
	FYAI_TCHECK(sigaction(SIGABRT, NULL, &before_abrt) == 0);

	fyai_crash_install();
	FYAI_TCHECK(fyai_crash_armed(SIGSEGV) ==
		    (before_segv.sa_handler == SIG_DFL));
	FYAI_TCHECK(fyai_crash_armed(SIGABRT) ==
		    (before_abrt.sa_handler == SIG_DFL));
	FYAI_TCHECK(!fyai_crash_armed(SIGUSR2));
	/* A second install must not stack handlers. */
	fyai_crash_install();
	FYAI_TCHECK(fyai_crash_armed(SIGSEGV) ==
		    (before_segv.sa_handler == SIG_DFL));

	fyai_crash_uninstall();
	FYAI_TCHECK(!fyai_crash_armed(SIGSEGV));
	FYAI_TCHECK(sigaction(SIGSEGV, NULL, &sa) == 0);
	FYAI_TCHECK(sa.sa_handler == before_segv.sa_handler);
	FYAI_TCHECK(sigaction(SIGABRT, NULL, &sa) == 0);
	FYAI_TCHECK(sa.sa_handler == before_abrt.sa_handler);

	printf("ok - the crash handler installs and uninstalls\n");
	return 0;
}

/* Read @fd until EOF into a NUL-terminated buffer. The caller frees it. */
static char *read_all(int fd)
{
	size_t cap = 4096, len = 0;
	char *buf = malloc(cap);
	char *grown;
	ssize_t r;

	if (!buf)
		return NULL;
	for (;;) {
		if (len + 1 >= cap) {
			cap *= 2;
			grown = realloc(buf, cap);
			if (!grown) {
				free(buf);
				return NULL;
			}
			buf = grown;
		}
		r = read(fd, buf + len, cap - len - 1);
		if (r < 0)
			break;
		if (r == 0)
			break;
		len += (size_t)r;
	}
	buf[len] = '\0';
	return buf;
}

/*
 * A faulted child must report the signal and a symbolized backtrace, then
 * die of the same signal. The test parent owns this pipe (not the harness):
 * the child writes the report and re-raises, and the pipe EOFs on death.
 */
int crash_report_run(void)
{
	char *report;
	pid_t pid;
	int p[2], status;

#if defined(__SANITIZE_ADDRESS__)
	/* The sanitizer owns SIGSEGV here and reports the fault itself. */
	printf("crash/report: skipped under address sanitizer\n");
	return 0;
#endif

	if (pipe(p)) {
		fprintf(stderr, "pipe() failed\n");
		return 1;
	}
	pid = fork();
	if (pid < 0) {
		fprintf(stderr, "fork() failed\n");
		return 1;
	}
	if (!pid) {
		volatile int *crashme;

		close(p[0]);
		fyai_crash_install();
		fyai_crash_set_fd(p[1]);
		/* Fault below the handler, never in it: the fault address
		 * must not be mistaken for handler state. The qualifier is
		 * on the pointee, so the store is observable and no
		 * optimization level deletes the fault itself. */
		crashme = NULL;
		*crashme = 42;
		_exit(99);
	}
	close(p[1]);
	report = read_all(p[0]);
	close(p[0]);
	if (waitpid(pid, &status, 0) < 0) {
		fprintf(stderr, "waitpid() failed\n");
		free(report);
		return 1;
	}
	if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGSEGV) {
		fprintf(stderr, "child did not die of SIGSEGV (status %#x)\n",
			(unsigned int)status);
		free(report);
		return 1;
	}
	if (!report || !strstr(report, "fyai: fatal signal") ||
	    !strstr(report, "SIGSEGV")) {
		fprintf(stderr, "child reported no fatal signal line: %s\n",
			report ? report : "(null)");
		free(report);
		return 1;
	}
#ifdef HAVE_EXECINFO
	if (!strstr(report, "fyai: backtrace")) {
		fprintf(stderr, "child reported no backtrace:\n%s\n", report);
		free(report);
		return 1;
	}
#endif
	free(report);
	printf("ok - a fault reports the signal and dies of it\n");
	return 0;
}
