/*
 * fyai_oauth.c - provider-agnostic OAuth 2.0 authorization-code mechanics
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_AUTH

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/rand.h>
#include <openssl/sha.h>

#include <libfyaml/libfyaml-util.h>

#include "fyai.h"
#include "fyai_auth_util.h"
#include "fyai_event.h"
#include "fyai_oauth.h"

char *fyai_oauth_query_value(CURL *curl, const char *request, const char *key)
{
	const char *q, *end, *p;
	size_t klen;
	const char *v, *amp;
	char *decoded;
	char *result;
	int n;

	klen = strlen(key);
	q = strchr(request, '?');
	if (!q)
		return NULL;
	q++;
	end = strchr(q, ' ');
	if (!end)
		return NULL;

	/* Walk the &-separated parameters. */
	v = NULL;
	for (p = q; p < end; ) {
		if ((size_t)(end - p) > klen && !strncmp(p, key, klen) &&
		    p[klen] == '=') {
			v = p + klen + 1;
			break;
		}
		amp = memchr(p, '&', (size_t)(end - p));
		if (!amp)
			break;
		p = amp + 1;
	}
	if (!v)
		return NULL;

	/* find the next & or end */
	amp = memchr(v, '&', (size_t)(end - v));
	if (amp)
		end = amp;

	/* decode */
	decoded = curl_easy_unescape(curl, v, (size_t)(end - v), &n);
	if (!decoded)
		return NULL;

	result = strndup(decoded, (size_t)n);
	curl_free(decoded);

	return result;
}

int fyai_oauth_pkce_generate(struct fyai_oauth_pkce *p)
{
	unsigned char verifier_random[48], state_random[32];
	unsigned char digest[SHA256_DIGEST_LENGTH];

	memset(p, 0, sizeof(*p));

	if (RAND_bytes(verifier_random, sizeof(verifier_random)) != 1 ||
	    RAND_bytes(state_random, sizeof(state_random)) != 1)
		return -1;

	p->verifier = fyai_base64url_encode(verifier_random,
					    sizeof(verifier_random));
	p->state = fyai_base64url_encode(state_random, sizeof(state_random));
	if (!p->verifier || !p->state)
		goto err_out;

	SHA256((unsigned char *)p->verifier, strlen(p->verifier), digest);
	p->challenge = fyai_base64url_encode(digest, sizeof(digest));
	if (!p->challenge)
		goto err_out;

	return 0;

err_out:
	fyai_oauth_pkce_cleanup(p);
	return -1;
}

void fyai_oauth_pkce_cleanup(struct fyai_oauth_pkce *p)
{
	if (!p)
		return;
	free(p->verifier);
	free(p->challenge);
	free(p->state);
	memset(p, 0, sizeof(*p));
}

void fyai_oauth_open_browser(const char *url)
{
	extern char **environ;
	pid_t pid;
#ifdef __APPLE__
	char *const argv[] = { "open", (char *)url, NULL };
	const char *program = "open";
#else
	char *const argv[] = { "xdg-open", (char *)url, NULL };
	const char *program = "xdg-open";
#endif

	/* Deliberately not reaped: the helper detaches immediately and the
	 * login flow is short-lived, so the zombie is collected at exit. */
	if (!posix_spawnp(&pid, program, NULL, NULL, argv, environ))
		(void)pid;
}


/* The redirect receiver. */
#define OAUTH_CB_MAX_CONNS	8

/* Per-connection state. A connection is only interesting until its request
 * headers are complete; after that it is either the winner or closed. */
enum oauth_conn_state {
	OAUTH_CONN_FREE = 0,		/* slot unused */
	OAUTH_CONN_READING,		/* accumulating request headers */
	OAUTH_CONN_HELD,		/* carried the code; owed a response */
};

struct oauth_conn {
	struct fyai_oauth_flow *flow;
	struct fyai_event_source *src;
	enum oauth_conn_state state;
	int fd;
	size_t len;
	char buf[8192];
};

struct fyai_oauth_flow {
	struct fyai_ctx *ctx;
	struct fyai_event_loop *el;		/* borrowed */
	struct fyai_event_source *listen_src;
	struct fyai_event_source *timer_src;

	enum fyai_oauth_state state;
	bool settled;				/* completion already reported */

	int listen_fd;
	unsigned short port;
	const char *path;
	const char *expect_state;

	char *code;
	int winner;				/* conn index owed a response */

	fyai_oauth_done_cb done_cb;
	void *done_user;

	struct oauth_conn conns[OAUTH_CB_MAX_CONNS];
};

static const char oauth_resp_ok[] =
	"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n"
	"Login complete. You may close this window.\n";
static const char oauth_resp_bad[] =
	"HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n"
	"Login failed.\n";
static const char oauth_resp_notfound[] =
	"HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n"
	"Not found.\n";

bool fyai_oauth_state_is_terminal(enum fyai_oauth_state st)
{
	return st != FYAI_OAUTH_LISTENING;
}

const char *fyai_oauth_state_string(enum fyai_oauth_state st)
{
	switch (st) {
	case FYAI_OAUTH_LISTENING:	return "listening";
	case FYAI_OAUTH_GOT_CODE:	return "got-code";
	case FYAI_OAUTH_TIMED_OUT:	return "timed-out";
	case FYAI_OAUTH_BAD_STATE:	return "bad-state";
	case FYAI_OAUTH_FAILED:		return "failed";
	}
	return "unknown";
}

/* Best effort: the responses are a couple of hundred bytes into a fresh
 * socket buffer, and the browser is being told to close regardless. */
static void oauth_respond(int fd, const char *resp, size_t len)
{
	ssize_t wrn;

	if (fd < 0)
		return;
	do {
		wrn = write(fd, resp, len);
	} while (wrn == -1 && errno == EINTR);
}

static void oauth_conn_close(struct oauth_conn *c)
{
	if (c->state == OAUTH_CONN_FREE)
		return;
	fyai_event_source_remove(c->src);
	c->src = NULL;
	if (c->fd >= 0)
		close(c->fd);
	c->fd = -1;
	c->len = 0;
	c->state = OAUTH_CONN_FREE;
}

/* Move the flow to a terminal state exactly once. */
static void oauth_settle(struct fyai_oauth_flow *f, enum fyai_oauth_state st)
{
	size_t i;

	if (f->settled)
		return;
	f->settled = true;
	f->state = st;

	fyai_event_source_remove(f->timer_src);
	f->timer_src = NULL;
	fyai_event_source_remove(f->listen_src);
	f->listen_src = NULL;

	/* Drop every connection except the one owed a response. */
	for (i = 0; i < OAUTH_CB_MAX_CONNS; i++) {
		if ((int)i == f->winner && f->conns[i].state == OAUTH_CONN_HELD) {
			/* keep the fd, but stop watching it */
			fyai_event_source_remove(f->conns[i].src);
			f->conns[i].src = NULL;
			continue;
		}
		oauth_conn_close(&f->conns[i]);
	}

	if (f->done_cb)
		f->done_cb(f, f->done_user);
}

/* Consume one connection's request once its headers are complete. */
static void oauth_consume(struct fyai_oauth_flow *f, struct oauth_conn *c)
{
	char *got_state;
	size_t plen;

	plen = strlen(f->path);

	if (strncmp(c->buf, "GET ", 4) || strncmp(c->buf + 4, f->path, plen) ||
	    c->buf[4 + plen] != '?') {
		/* Ignore browser probes and keep waiting for the redirect. */
		oauth_respond(c->fd, oauth_resp_notfound,
			      sizeof(oauth_resp_notfound) - 1);
		oauth_conn_close(c);
		return;
	}

	f->code = fyai_oauth_query_value(f->ctx->curl, c->buf, "code");
	got_state = fyai_oauth_query_value(f->ctx->curl, c->buf, "state");

	/* A redirect with the wrong state is a cross-site request or a stale tab, not
	 * a reason to keep listening. */
	if (!f->code || !got_state || strcmp(got_state, f->expect_state)) {
		free(f->code);
		f->code = NULL;
		free(got_state);
		oauth_respond(c->fd, oauth_resp_bad, sizeof(oauth_resp_bad) - 1);
		oauth_conn_close(c);
		oauth_settle(f, FYAI_OAUTH_BAD_STATE);
		return;
	}

	free(got_state);
	c->state = OAUTH_CONN_HELD;
	f->winner = (int)(c - f->conns);
	oauth_settle(f, FYAI_OAUTH_GOT_CODE);
}

static enum fyai_event_action oauth_on_client(const struct fyai_event *ev)
{
	struct oauth_conn *c = ev->userdata;
	ssize_t n;

	/* EOF arrives with READ while bytes remain, so only a bare EOF or an
	 * error means this connection is finished. */
	if ((ev->events & (FYAIEV_ERROR | FYAIEV_EOF)) &&
	    !(ev->events & FYAIEV_READ)) {
		oauth_conn_close(c);
		return FYAIEA_CONTINUE;
	}

	n = read(c->fd, c->buf + c->len, sizeof(c->buf) - 1 - c->len);
	if (n < 0) {
		if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
			return FYAIEA_CONTINUE;
		oauth_conn_close(c);
		return FYAIEA_CONTINUE;
	}
	if (!n) {			/* closed before a full request */
		oauth_conn_close(c);
		return FYAIEA_CONTINUE;
	}

	c->len += (size_t)n;
	c->buf[c->len] = '\0';

	/* Wait for the blank line ending the headers rather than trusting one read to
	 * deliver them. */
	if (!strstr(c->buf, "\r\n\r\n") && c->len < sizeof(c->buf) - 1)
		return FYAIEA_CONTINUE;

	oauth_consume(c->flow, c);

	return FYAIEA_CONTINUE;
}

static enum fyai_event_action oauth_on_accept(const struct fyai_event *ev)
{
	struct fyai_oauth_flow *f = ev->userdata;
	struct oauth_conn *c = NULL;
	size_t i;
	int fd;
	int rc;

	fd = accept(ev->fd, NULL, NULL);
	if (fd < 0)
		return FYAIEA_CONTINUE;

	for (i = 0; i < OAUTH_CB_MAX_CONNS; i++) {
		if (f->conns[i].state == OAUTH_CONN_FREE) {
			c = &f->conns[i];
			break;
		}
	}
	if (!c) {			/* all slots busy; shed this one */
		close(fd);
		return FYAIEA_CONTINUE;
	}

	(void)fcntl(fd, F_SETFD, FD_CLOEXEC);
	(void)fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

	c->flow = f;
	c->fd = fd;
	c->len = 0;
	c->state = OAUTH_CONN_READING;
	rc = fyai_event_add_fd(f->el, fd, FYAIEV_READ, oauth_on_client, c,
			       &c->src);
	if (rc) {
		close(fd);
		c->fd = -1;
		c->state = OAUTH_CONN_FREE;
		return FYAIEA_CONTINUE;
	}

	return FYAIEA_CONTINUE;
}

static enum fyai_event_action oauth_on_timeout(const struct fyai_event *ev)
{
	struct fyai_oauth_flow *f = ev->userdata;

	oauth_settle(f, FYAI_OAUTH_TIMED_OUT);
	return FYAIEA_CONTINUE;
}

/* Bind the first free port. */
static int oauth_listen(struct fyai_ctx *ctx, const unsigned short *ports,
			size_t nports, unsigned short *portp)
{
	struct sockaddr_in sa;
	socklen_t slen;
	int fd = -1;
	size_t i;
	int one;
	int rc;

	for (i = 0; i < nports; i++) {

		if (fd >= 0)
			close(fd);
		fd = socket(AF_INET, SOCK_STREAM, 0);
		if (fd < 0)
			break;

		fcntl(fd, F_SETFD, FD_CLOEXEC);

		/* Every connection the browser makes leaves this port in TIME_WAIT once
		 * closed. */
		one = 1;
		(void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

		memset(&sa, 0, sizeof(sa));
		sa.sin_family = AF_INET;
		sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		sa.sin_port = htons(ports[i]);
		rc = bind(fd, (struct sockaddr *)&sa, sizeof(sa));
		if (rc)
			continue;

		rc = listen(fd, OAUTH_CB_MAX_CONNS);
		if (rc)
			continue;

		(void)fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

		/* Port 0 asks the kernel to choose; report what it picked. */
		*portp = ports[i];
		if (!ports[i]) {
			slen = sizeof(sa);
			if (!getsockname(fd, (struct sockaddr *)&sa, &slen))
				*portp = ntohs(sa.sin_port);
		}
		return fd;
	}

	if (fd >= 0)
		close(fd);

	fyai_error(ctx, "could not bind a loopback port for the OAuth redirect");
	return -1;
}

int fyai_oauth_flow_start(struct fyai_ctx *ctx, struct fyai_event_loop *el,
			  const struct fyai_oauth_params *params,
			  fyai_oauth_done_cb cb, void *user,
			  struct fyai_oauth_flow **flowp)
{
	struct fyai_oauth_flow *f;
	size_t i;
	int rc;

	if (!ctx || !el || !params || !flowp)
		return -1;

	f = calloc(1, sizeof(*f));
	fyai_error_check(ctx, f, err_out, "out of memory arming the OAuth redirect");

	f->ctx = ctx;
	f->el = el;
	f->state = FYAI_OAUTH_LISTENING;
	f->path = params->path;
	f->expect_state = params->state;
	f->done_cb = cb;
	f->done_user = user;
	f->winner = -1;
	f->listen_fd = -1;
	for (i = 0; i < OAUTH_CB_MAX_CONNS; i++)
		f->conns[i].fd = -1;

	f->listen_fd = oauth_listen(ctx, params->ports, params->nports,
				    &f->port);
	if (f->listen_fd < 0)
		goto err_out;

	rc = fyai_event_add_fd(el, f->listen_fd, FYAIEV_READ, oauth_on_accept,
			       f, &f->listen_src);
	if (rc)
		goto err_out;

	if (params->timeout_ms > 0) {
		rc = fyai_event_add_timer(el, params->timeout_ms, 0,
					  oauth_on_timeout, f, &f->timer_src);
		if (rc)
			goto err_out;
	}

	*flowp = f;
	return 0;

err_out:
	fyai_oauth_flow_destroy(f);
	return -1;
}

enum fyai_oauth_state fyai_oauth_flow_state(const struct fyai_oauth_flow *f)
{
	return f ? f->state : FYAI_OAUTH_FAILED;
}

unsigned short fyai_oauth_flow_port(const struct fyai_oauth_flow *f)
{
	return f ? f->port : 0;
}

const char *fyai_oauth_flow_code(const struct fyai_oauth_flow *f)
{
	return f ? f->code : NULL;
}

void fyai_oauth_flow_finish(struct fyai_oauth_flow *f, bool ok)
{
	struct oauth_conn *c;

	if (!f || f->winner < 0)
		return;

	c = &f->conns[f->winner];
	if (c->state != OAUTH_CONN_HELD)
		return;

	if (ok)
		oauth_respond(c->fd, oauth_resp_ok, sizeof(oauth_resp_ok) - 1);
	else
		oauth_respond(c->fd, oauth_resp_bad, sizeof(oauth_resp_bad) - 1);

	oauth_conn_close(c);
	f->winner = -1;
}

void fyai_oauth_flow_destroy(struct fyai_oauth_flow *f)
{
	size_t i;

	if (!f)
		return;

	fyai_event_source_remove(f->timer_src);
	fyai_event_source_remove(f->listen_src);
	for (i = 0; i < OAUTH_CB_MAX_CONNS; i++)
		oauth_conn_close(&f->conns[i]);
	if (f->listen_fd >= 0)
		close(f->listen_fd);
	free(f->code);
	free(f);
}

/* Completion hook for the synchronous form: end the run it is sitting in. */
static void oauth_sync_done(struct fyai_oauth_flow *f, void *user)
{
	bool *done = user;

	(void)f;
	*done = true;
}

int fyai_oauth_flow_wait(struct fyai_ctx *ctx, struct fyai_oauth_flow *f)
{
	volatile bool done = false;
	enum fyai_oauth_state st;
	int rc;

	if (!f)
		return -1;

	/* Only meaningful for a caller with nothing else on this loop. */
	st = fyai_oauth_flow_state(f);
	if (!fyai_oauth_state_is_terminal(st)) {
		f->done_cb = oauth_sync_done;
		f->done_user = (void *)&done;

		rc = fyai_event_loop_run_until(f->el, &done, -1);
		if (rc)
			return -1;
		st = fyai_oauth_flow_state(f);
	}

	fyai_error_check(ctx, st != FYAI_OAUTH_BAD_STATE, err_out,
			 "the OAuth redirect carried the wrong state - the login "
			 "was not the one started here");
	fyai_error_check(ctx, st != FYAI_OAUTH_TIMED_OUT, err_out,
			 "no authorization code arrived on the redirect");
	fyai_error_check(ctx, st == FYAI_OAUTH_GOT_CODE, err_out,
			 "the OAuth redirect receiver failed");

	return 0;

err_out:
	return -1;
}
