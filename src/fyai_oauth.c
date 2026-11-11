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

#include "fyai_host.h"
#include "fyai.h"
#include "fyai_auth_util.h"
#include "fyai_event.h"
#include "fyai_httpd.h"
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
	unsigned char verifier_random[48], state_random[32], nonce_random[32];
	unsigned char digest[SHA256_DIGEST_LENGTH];

	memset(p, 0, sizeof(*p));

	if (RAND_bytes(verifier_random, sizeof(verifier_random)) != 1 ||
	    RAND_bytes(state_random, sizeof(state_random)) != 1 ||
	    RAND_bytes(nonce_random, sizeof(nonce_random)) != 1)
		return -1;

	p->verifier = fyai_base64url_encode(verifier_random,
					    sizeof(verifier_random));
	p->state = fyai_base64url_encode(state_random, sizeof(state_random));
	p->nonce = fyai_base64url_encode(nonce_random, sizeof(nonce_random));
	if (!p->verifier || !p->state || !p->nonce)
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
	free(p->nonce);
	memset(p, 0, sizeof(*p));
}

void fyai_oauth_open_browser(const char *url)
{
	extern char **environ;
	pid_t pid;
	const char *program = fyai_host_open_url_program();
	char *const argv[] = { (char *)program, (char *)url, NULL };

	/* Deliberately not reaped: the helper detaches immediately and the
	 * login flow is short-lived, so the zombie is collected at exit. */
	if (!posix_spawnp(&pid, program, NULL, NULL, argv, environ))
		(void)pid;
}


/* The redirect receiver. */
#define OAUTH_CB_MAX_CONNS	8
/* The time a browser has to send its request; the flow has its own limit. */
#define OAUTH_CB_HEAD_MS	10000

struct fyai_oauth_flow {
	struct fyai_ctx *ctx;
	struct fyai_event_loop *el;		/* borrowed */
	struct fyai_httpd *httpd;
	struct fyai_event_source *timer_src;

	enum fyai_oauth_state state;
	bool settled;				/* completion already reported */

	unsigned short port;
	const char *path;
	const char *expect_state;

	char *code;
	char *client_id;
	char *error;
	struct fyai_httpd_conn *held;		/* the connection owed a response */

	fyai_oauth_done_cb done_cb;
	void *done_user;
};

static const char oauth_body_ok[] = "Login complete. You may close this window.\n";
static const char oauth_body_bad[] = "Login failed.\n";
static const char oauth_body_notfound[] = "Not found.\n";
static const char oauth_text[] = "Content-Type: text/plain\r\n";

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

/* Move the flow to a terminal state exactly once. */
static void oauth_settle(struct fyai_oauth_flow *f, enum fyai_oauth_state st)
{
	if (f->settled)
		return;
	f->settled = true;
	f->state = st;

	fyai_event_source_remove(f->timer_src);
	f->timer_src = NULL;
	/* Drop every connection except the one owed a response. */
	fyai_httpd_stop(f->httpd, f->held);

	if (f->done_cb)
		f->done_cb(f, f->done_user);
}

void fyai_oauth_flow_redirect(struct fyai_oauth_flow *f, const char *request)
{
	char *got_state;

	if (f->settled)
		return;
	got_state = fyai_oauth_query_value(f->ctx->curl, request, "state");
	if (!got_state || strcmp(got_state, f->expect_state)) {
		free(got_state);
		oauth_settle(f, FYAI_OAUTH_BAD_STATE);
		return;
	}
	free(got_state);
	f->error = fyai_oauth_query_value(f->ctx->curl, request, "error");
	if (f->error) {
		oauth_settle(f, FYAI_OAUTH_FAILED);
		return;
	}
	f->code = fyai_oauth_query_value(f->ctx->curl, request, "code");
	f->client_id = fyai_oauth_query_value(f->ctx->curl, request, "client_id");
	oauth_settle(f, f->code && *f->code ?
		     FYAI_OAUTH_GOT_CODE : FYAI_OAUTH_FAILED);
}

const char *fyai_oauth_flow_client_id(const struct fyai_oauth_flow *f)
{
	return f->client_id;
}

const char *fyai_oauth_flow_error(const struct fyai_oauth_flow *f)
{
	return f->error;
}

/* A request head is whole: the redirect, or a probe of the browser. */
static void oauth_request(void *data, struct fyai_httpd_conn *c,
			  const struct fyai_http_request *req)
{
	struct fyai_oauth_flow *f = data;
	size_t plen = strlen(f->path);
	char *line = NULL;
	int rc;

	if (strcmp(req->method, "GET") || strncmp(req->target, f->path, plen) ||
	    req->target[plen] != '?') {
		/* Ignore browser probes and keep waiting for the redirect. */
		fyai_httpd_respond(c, 404, NULL, oauth_text, oauth_body_notfound,
				   sizeof(oauth_body_notfound) - 1);
		return;
	}
	rc = asprintf(&line, "GET %s HTTP/1.1\r\n", req->target);
	if (rc < 0) {
		fyai_httpd_respond(c, 400, NULL, oauth_text, oauth_body_bad,
				   sizeof(oauth_body_bad) - 1);
		return;
	}
	fyai_httpd_conn_hold(c);
	f->held = c;
	fyai_oauth_flow_redirect(f, line);
	free(line);
}

static void oauth_closed(void *data, struct fyai_httpd_conn *c)
{
	struct fyai_oauth_flow *f = data;

	if (f->held == c)
		f->held = NULL;
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
	struct fyai_httpd_opts opts = {
		.loop = el,
		.max_conns = OAUTH_CB_MAX_CONNS,
		.max_message = 1,
		.max_output = 1,
		.head_ms = OAUTH_CB_HEAD_MS,
	};
	struct fyai_httpd_ops ops = {
		.request = oauth_request,
		.closed = oauth_closed,
	};
	int rc, fd;

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
	ops.data = f;
	f->httpd = fyai_httpd_new(ctx, &opts, &ops);
	if (!f->httpd)
		goto err_out;

	fd = oauth_listen(ctx, params->ports, params->nports, &f->port);
	if (fd < 0)
		goto err_out;
	rc = fyai_httpd_adopt(f->httpd, fd, NULL);
	if (rc) {
		close(fd);
		goto err_out;
	}

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
	struct fyai_httpd_conn *c;

	if (!f || !f->held)
		return;
	c = f->held;
	f->held = NULL;
	if (ok)
		fyai_httpd_respond(c, 200, NULL, oauth_text, oauth_body_ok,
				   sizeof(oauth_body_ok) - 1);
	else
		fyai_httpd_respond(c, 400, NULL, oauth_text, oauth_body_bad,
				   sizeof(oauth_body_bad) - 1);
}

void fyai_oauth_flow_destroy(struct fyai_oauth_flow *f)
{
	if (!f)
		return;

	fyai_event_source_remove(f->timer_src);
	fyai_httpd_close(f->httpd);
	free(f->code);
	free(f->client_id);
	free(f->error);
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
