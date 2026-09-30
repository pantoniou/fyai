/*
 * fyai_transport_mock.c - a loopback HTTP provider for the transport tests
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fyai_test.h"
#include "fyai_transport_mock.h"

/* Mock provider. */

/* Serve one connection. With @many the report stays open for the next one. */
static void tmock_serve(int listener, enum tmock_mode mode, int report, bool many)
{
	char req[65536], hdr[512];
	size_t got = 0, need = 0;
	static char chunk[16384];
	ssize_t n;
	int c;
	char *end, *cl;
	size_t left, k;

	c = accept(listener, NULL, NULL);
	if (c < 0)
		_exit(1);
	while (got < sizeof(req) - 1) {
		n = read(c, req + got, sizeof(req) - 1 - got);
		if (n <= 0)
			_exit(2);
		got += n;
		req[got] = '\0';
		end = strstr(req, "\r\n\r\n");
		if (!end)
			continue;
		cl = strcasestr(req, "content-length:");
		need = (end + 4 - req) + (cl ? strtoul(cl + 15, NULL, 10) : 0);
		if (got >= need)
			break;
	}
	if (write(report, req, got) < 0)
		_exit(3);
	if (!many)
		close(report);

	memset(chunk, 'a', sizeof(chunk));
	switch (mode) {
	case TMOCK_SMALL:
		snprintf(hdr, sizeof(hdr),
			 "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
			 "Retry-After: 7\r\nX-RateLimit-Remaining-Requests: 99\r\n"
			 "Anthropic-Ratelimit-Requests-Limit: 50\r\nX-Other: 1\r\n"
			 "Content-Length: 5\r\n\r\nhello");
		if (write(c, hdr, strlen(hdr)) < 0)
			_exit(4);
		break;
	case TMOCK_BIG:
		left = BIG_BODY;

		snprintf(hdr, sizeof(hdr),
			 "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n", BIG_BODY);
		if (write(c, hdr, strlen(hdr)) < 0)
			_exit(4);
		while (left) {
			k = left > sizeof(chunk) ? sizeof(chunk) : left;

			n = write(c, chunk, k);
			if (n <= 0)
				_exit(0);	/* the transport went away */
			left -= n;
		}
		break;
	case TMOCK_LIMITED:
		snprintf(hdr, sizeof(hdr),
			 "HTTP/1.1 429 Too Many Requests\r\nRetry-After: 3\r\n"
			 "X-RateLimit-Remaining-Requests: 0\r\n"
			 "RateLimit-Reset: 12\r\nServer: mock\r\n"
			 "Content-Length: 2\r\n\r\n{}");
		if (write(c, hdr, strlen(hdr)) < 0)
			_exit(4);
		break;
	case TMOCK_STALL:
		snprintf(hdr, sizeof(hdr),
			 "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nx");
		if (write(c, hdr, strlen(hdr)) < 0)
			_exit(4);
		pause();
		break;
	}
	close(c);
}

static void tmock_child(int listener, enum tmock_mode mode, int report, bool many)
{
	do
		tmock_serve(listener, mode, report, many);
	while (many);
	_exit(0);
}

static void tmock_start_opt(struct tmock *m, enum tmock_mode mode, bool many)
{
	struct sockaddr_in sa = { .sin_family = AF_INET };
	socklen_t sl = sizeof(sa);
	int l, rp[2];

	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	l = socket(AF_INET, SOCK_STREAM, 0);
	FYAI_TCHECK(l >= 0);
	FYAI_TCHECK(!bind(l, (struct sockaddr *)&sa, sizeof(sa)));
	FYAI_TCHECK(!listen(l, 4));
	FYAI_TCHECK(!getsockname(l, (struct sockaddr *)&sa, &sl));
	m->port = ntohs(sa.sin_port);
	FYAI_TCHECK(!pipe(rp));
	m->pid = fork();
	FYAI_TCHECK(m->pid >= 0);
	if (!m->pid) {
		close(rp[0]);
		tmock_child(l, mode, rp[1], many);
	}
	close(l);
	close(rp[1]);
	m->report = rp[0];
}

void tmock_start(struct tmock *m, enum tmock_mode mode)
{
	tmock_start_opt(m, mode, false);
}

/* A provider that serves any number of requests until it is stopped. */
void tmock_start_many(struct tmock *m, enum tmock_mode mode)
{
	tmock_start_opt(m, mode, true);
}

void tmock_stop(struct tmock *m)
{
	kill(m->pid, SIGKILL);
	waitpid(m->pid, NULL, 0);
	close(m->report);
}

/* What the mock saw, as one string. */
size_t tmock_report(struct tmock *m, char *buf, size_t size)
{
	size_t got = 0;
	ssize_t n;

	while (got < size - 1 && (n = read(m->report, buf + got, size - 1 - got)) > 0)
		got += n;
	buf[got] = '\0';
	return got;
}

/* Return the bytes the mock reported so far, without waiting for more. */
size_t tmock_report_nonblock(struct tmock *m, char *buf, size_t size)
{
	ssize_t n;

	fcntl(m->report, F_SETFL, fcntl(m->report, F_GETFL) | O_NONBLOCK);
	n = read(m->report, buf, size);
	return n > 0 ? n : 0;
}
