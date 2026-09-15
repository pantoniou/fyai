/*
 * fyai_crash.c - fatal signal backtrace
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#ifdef HAVE_EXECINFO
#include <execinfo.h>
#endif

#include "fyai_crash.h"

/* Frames held for the symbolizer, which writes them itself. */
#define FYAI_CRASH_FRAMES 64
/* Alternate stack so a stack overflow still reaches the handler. */
#define FYAI_CRASH_ALT_STACK (64 * 1024)

static const int crash_signals[] = {
	SIGSEGV,
	SIGABRT,
	SIGBUS,
	SIGILL,
	SIGFPE,
#ifdef SIGSYS
	SIGSYS,
#endif
};
#define FYAI_CRASH_NSIGNS \
	(sizeof(crash_signals) / sizeof(crash_signals[0]))

/* crash_installed holds one bit per entry. */
_Static_assert(FYAI_CRASH_NSIGNS <= 8 * sizeof(unsigned int),
	       "crash signal set exceeds the installed bitmap");

/* Output of a later crash; standard error until fyai_crash_set_fd() aims it
 * at the preserved diagnostic descriptor past the UI stdio spool. */
static volatile sig_atomic_t crash_fd = STDERR_FILENO;
/* Whether crash_fd names a terminal, probed outside signal context. */
static volatile sig_atomic_t crash_is_tty;
/* Set on entry so a fault inside the handler re-raises at once. */
static volatile sig_atomic_t crash_active;
static struct sigaction crash_prev[FYAI_CRASH_NSIGNS];
/* One bit per crash_signals[] entry this process installed. */
static unsigned int crash_installed;
static char crash_altstack[FYAI_CRASH_ALT_STACK];
static bool crash_ready;

/* Write all of @n or give up; never block a dying process on a full pipe. */
static void crash_write(int fd, const char *s, size_t n)
{
	size_t off = 0;
	ssize_t w;

	while (off < n) {
		w = write(fd, s + off, n - off);
		if (w > 0) {
			off += (size_t)w;
			continue;
		}
		if (w < 0 && errno == EINTR)
			continue;
		break;
	}
}

static void crash_str(int fd, const char *s)
{
	if (s)
		crash_write(fd, s, strlen(s));
}

static void crash_nl(int fd)
{
	/* Plain NL: the restore above left a terminal cooked, and a pipe
	 * needs no carriage return. */
	crash_write(fd, "\n", 1);
}

static void crash_num(int fd, long long v)
{
	char t[24];
	size_t i = sizeof(t);
	unsigned long long u;

	if (v < 0) {
		crash_write(fd, "-", 1);
		u = (unsigned long long)(-(v + 1)) + 1ULL;
	} else {
		u = (unsigned long long)v;
	}
	do {
		t[--i] = (char)('0' + (u % 10));
		u /= 10;
	} while (u);
	crash_write(fd, t + i, sizeof(t) - i);
}

static const char *crash_signame(int signo)
{
	switch (signo) {
	case SIGSEGV:
		return "SIGSEGV";
	case SIGABRT:
		return "SIGABRT";
	case SIGBUS:
		return "SIGBUS";
	case SIGILL:
		return "SIGILL";
	case SIGFPE:
		return "SIGFPE";
#ifdef SIGSYS
	case SIGSYS:
		return "SIGSYS";
#endif
	default:
		return "UNKNOWN";
	}
}

/* The index of @signo in crash_signals[], or -1. */
static int crash_index(int signo)
{
	size_t i;

	for (i = 0; i < FYAI_CRASH_NSIGNS; i++) {
		if (crash_signals[i] == signo)
			return (int)i;
	}
	return -1;
}

/* Restore the default disposition and die of @signo, keeping the exit status
 * and any core dump the signal carries. */
static void crash_reraise(int signo)
{
	struct sigaction dfl;
	sigset_t unblock;
	int idx;

	idx = crash_index(signo);
	if (idx >= 0 && (crash_installed & (1u << idx)))
		(void)sigaction(signo, &crash_prev[idx], NULL);
	else {
		memset(&dfl, 0, sizeof(dfl));
		dfl.sa_handler = SIG_DFL;
		sigemptyset(&dfl.sa_mask);
		(void)sigaction(signo, &dfl, NULL);
	}
	sigemptyset(&unblock);
	sigaddset(&unblock, signo);
	(void)sigprocmask(SIG_UNBLOCK, &unblock, NULL);
	(void)raise(signo);
	_exit(128 + (signo & 0x7f));
}

/*
 * Best-effort terminal repair before the report. An interactive session
 * leaves the terminal in raw mode, possibly on the alternate screen; without
 * this the backtrace prints stair-stepped and the shell returns to a bricked
 * terminal. Bounded direct writes and one cooked-mode tcsetattr each; every
 * step may fail, and the report follows regardless. The first of standard
 * input and the report descriptor that answers keeps the cooked settings.
 */
static void crash_term_restore(int fd)
{
	struct termios tio;
	int tfds[2];
	unsigned int i;

	crash_str(fd, "\x1b[?1049l\x1b[?25h\x1b[0m");
	tfds[0] = STDIN_FILENO;
	tfds[1] = fd;
	for (i = 0; i < 2; i++) {
		if (tcgetattr(tfds[i], &tio))
			continue;
		tio.c_iflag |= ICRNL | IXON;
		tio.c_oflag |= OPOST;
		tio.c_lflag |= ECHO | ECHONL | ICANON | ISIG | IEXTEN;
		(void)tcsetattr(tfds[i], TCSANOW, &tio);
		break;
	}
}

static void crash_handler(int signo)
{
	int fd, flags;
#ifdef HAVE_EXECINFO
	void *addrs[FYAI_CRASH_FRAMES];
	int frames;
#endif

	if (crash_active)
		crash_reraise(signo);
	crash_active = 1;

	fd = (int)crash_fd;
	/* A pipe with no reader must truncate the report, not hang the exit. */
	flags = fcntl(fd, F_GETFL);
	if (flags >= 0)
		(void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);

	if (crash_is_tty)
		crash_term_restore(fd);

	crash_str(fd, "fyai: fatal signal ");
	crash_num(fd, signo);
	crash_str(fd, " (");
	crash_str(fd, crash_signame(signo));
	crash_str(fd, "), pid ");
	crash_num(fd, (long long)getpid());
	crash_nl(fd);
#ifdef HAVE_EXECINFO
	frames = backtrace(addrs, FYAI_CRASH_FRAMES);
	if (frames > 0) {
		crash_str(fd, "fyai: backtrace (most recent call first):");
		crash_nl(fd);
		backtrace_symbols_fd(addrs, frames, fd);
	} else {
		crash_str(fd, "fyai: no backtrace frames captured");
		crash_nl(fd);
	}
#else
	crash_str(fd, "fyai: no backtrace available on this platform");
	crash_nl(fd);
#endif
	crash_reraise(signo);
}

void fyai_crash_install(void)
{
	struct sigaction sa, old;
	sigset_t unblock;
	stack_t ss;
	size_t i;

	if (crash_ready)
		return;

	/* Without an alternate stack a stack overflow never reaches us. */
	ss.ss_sp = crash_altstack;
	ss.ss_size = sizeof(crash_altstack);
	ss.ss_flags = 0;
	(void)sigaltstack(&ss, NULL);

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = crash_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_ONSTACK;

	crash_is_tty = isatty((int)crash_fd) == 1;
	sigemptyset(&unblock);
	for (i = 0; i < FYAI_CRASH_NSIGNS; i++) {
		if (sigaction(crash_signals[i], NULL, &old))
			continue;
		/* Leave a handler its owner installed, such as a sanitizer. */
		if (old.sa_handler != SIG_DFL)
			continue;
		if (sigaction(crash_signals[i], &sa, &crash_prev[i]))
			continue;
		crash_installed |= 1u << i;
		sigaddset(&unblock, crash_signals[i]);
	}
	(void)sigprocmask(SIG_UNBLOCK, &unblock, NULL);
	crash_ready = true;
}

void fyai_crash_set_fd(int fd)
{
	if (fd >= 0) {
		crash_fd = fd;
		crash_is_tty = isatty(fd) == 1;
	}
}

bool fyai_crash_armed(int signo)
{
	int idx;

	idx = crash_index(signo);
	return idx >= 0 && (crash_installed & (1u << idx)) != 0;
}

void fyai_crash_uninstall(void)
{
	stack_t ss;
	size_t i;

	for (i = 0; i < FYAI_CRASH_NSIGNS; i++) {
		if (!(crash_installed & (1u << i)))
			continue;
		(void)sigaction(crash_signals[i], &crash_prev[i], NULL);
	}
	crash_installed = 0;
	crash_ready = false;
	crash_active = 0;

	ss.ss_flags = SS_DISABLE;
	ss.ss_sp = NULL;
	ss.ss_size = 0;
	(void)sigaltstack(&ss, NULL);
}
