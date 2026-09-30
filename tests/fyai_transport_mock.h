/*
 * fyai_transport_mock.h - a loopback HTTP provider for the transport tests
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis@konsulko.com>
 * SPDX-License-Identifier: MIT
 *
 * The provider is a forked process that listens on a loopback port. It reports
 * the request it read on a pipe, so a test can check what reached the wire.
 */

#ifndef FYAI_TRANSPORT_MOCK_H
#define FYAI_TRANSPORT_MOCK_H

#include <stddef.h>
#include <stdlib.h>
#include <sys/types.h>

#define BIG_BODY	(1024 * 1024)

/*
 * How long a test waits for a state before it gives up. It is a bound on a
 * failure, never a delay: a test that passes does not wait for it. A runner can
 * stall for minutes for no reason, so it is generous, and it grows with
 * FYAI_TIMEOUT_SCALE.
 */
static inline long tmock_bound_ms(void)
{
	const char *scale = getenv("FYAI_TIMEOUT_SCALE");
	long n = scale ? atol(scale) : 1;

	return 120000L * (n > 0 ? n : 1);
}

enum tmock_mode {
	TMOCK_SMALL,		/* 200 with "hello" */
	TMOCK_BIG,		/* 200 with BIG_BODY bytes */
	TMOCK_STALL,		/* headers and one byte, then wait */
	TMOCK_LIMITED,		/* 429 with rate-limit headers and a body */
};

struct tmock {
	pid_t pid;
	int port;
	int report;		/* the request head and body */
};

/* Serve one connection, and close the report. */
void tmock_start(struct tmock *m, enum tmock_mode mode);
/* Serve connections until stopped; the report stays open. */
void tmock_start_many(struct tmock *m, enum tmock_mode mode);
void tmock_stop(struct tmock *m);
/* Read the report to its end. */
size_t tmock_report(struct tmock *m, char *buf, size_t size);
/* Read what was reported so far, without waiting. */
size_t tmock_report_nonblock(struct tmock *m, char *buf, size_t size);

#endif
