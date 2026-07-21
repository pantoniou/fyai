/*
 * fyai_oauth_test.c - the OAuth loopback redirect receiver
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * The receiver is a state machine on the event loop, so the tests drive both
 * ends from the same loop: the client sockets are registered as sources too,
 * and nothing here blocks. That is deliberate - a test that blocked on the
 * client side would pass even if the receiver had gone back to a synchronous
 * accept()/read(), which is exactly the regression these guard.
 *
 * Every wait is bounded so a hang fails the run instead of hanging CI.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <curl/curl.h>

#include "fyai.h"
#include "fyai_event.h"
#include "fyai_oauth.h"
#include "fyai_test.h"

#define TEST_BOUND_MS	5000
#define TEST_STATE	"s3cr3t-state"
#define TEST_PATH	"/auth/callback"

static struct fyai_cfg test_cfg;
static struct fyai_ctx test_ctx;

/* Port 0: let the kernel pick, so the tests never collide with a real login
 * on 1455 or with each other under a parallel ctest. */
static const unsigned short test_ports[] = { 0 };

static struct fyai_oauth_params test_params(fyai_event_ms_t timeout_ms)
{
	struct fyai_oauth_params p;

	memset(&p, 0, sizeof(p));
	p.path = TEST_PATH;
	p.ports = test_ports;
	p.nports = 1;
	p.state = TEST_STATE;
	p.timeout_ms = timeout_ms;
	return p;
}

/* A client connection driven from the loop. */
struct client {
	int fd;
	struct fyai_event_source *src;
	const char *send;
	size_t sent;
	size_t chunk;
	int nchunks;
	int chunk_done;
	char resp[512];
	size_t resplen;
	bool closed;
};

static int client_connect(struct fyai_event_loop *el, struct client *c,
			  unsigned short port, const char *payload,
			  int nchunks, fyai_event_cb cb);

static enum fyai_event_action client_cb(const struct fyai_event *ev)
{
	struct client *c = ev->userdata;
	size_t remain, n;
	ssize_t r;

	if (ev->events & FYAIEV_WRITE) {
		remain = strlen(c->send) - c->sent;
		if (remain) {
			/* one chunk per wakeup */
			n = c->chunk && remain > c->chunk ? c->chunk : remain;
			r = write(c->fd, c->send + c->sent, n);
			if (r > 0)
				c->sent += (size_t)r;
			if (c->sent >= strlen(c->send))
				fyai_event_fd_modify(c->src, FYAIEV_READ);
			return FYAIEA_CONTINUE;
		}
		fyai_event_fd_modify(c->src, FYAIEV_READ);
	}

	if (ev->events & FYAIEV_READ) {
		r = read(c->fd, c->resp + c->resplen,
			 sizeof(c->resp) - 1 - c->resplen);
		if (r > 0) {
			c->resplen += (size_t)r;
			c->resp[c->resplen] = '\0';
			return FYAIEA_CONTINUE;
		}
		if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return FYAIEA_CONTINUE;
		c->closed = true;
		fyai_event_source_remove(c->src);
		c->src = NULL;
		return FYAIEA_CONTINUE;
	}

	if (ev->events & (FYAIEV_EOF | FYAIEV_ERROR)) {
		c->closed = true;
		fyai_event_source_remove(c->src);
		c->src = NULL;
	}
	return FYAIEA_CONTINUE;
}

static int client_connect(struct fyai_event_loop *el, struct client *c,
			  unsigned short port, const char *payload,
			  int nchunks, fyai_event_cb cb)
{
	struct sockaddr_in sa;
	int fd;
	int rc;

	memset(c, 0, sizeof(*c));
	c->fd = -1;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	FYAI_TCHECK(fd >= 0);
	(void)fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sa.sin_port = htons(port);

	rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
	FYAI_TCHECK(!rc || errno == EINPROGRESS);

	c->fd = fd;
	c->send = payload ? payload : "";
	c->nchunks = nchunks;
	if (payload && nchunks > 1)
		c->chunk = (strlen(payload) + (size_t)nchunks - 1) /
			   (size_t)nchunks;

	if (!payload)		/* a silent preconnect: never writes, never reads */
		return 0;

	rc = fyai_event_add_fd(el, fd, FYAIEV_WRITE, cb ? cb : client_cb, c,
			       &c->src);
	FYAI_TCHECK(!rc);
	return 0;
}

static void client_close(struct client *c)
{
	if (c->src)
		fyai_event_source_remove(c->src);
	c->src = NULL;
	if (c->fd >= 0)
		close(c->fd);
	c->fd = -1;
}

static const char *resp_status(const struct client *c)
{
	static char line[128];
	const char *eol;
	size_t n;

	eol = strstr(c->resp, "\r\n");
	if (!eol)
		return c->resplen ? "(partial)" : "(none)";
	n = (size_t)(eol - c->resp);
	if (n >= sizeof(line))
		n = sizeof(line) - 1;
	memcpy(line, c->resp, n);
	line[n] = '\0';
	return line;
}

static char *redirect_request(const char *code, const char *state)
{
	char *s;
	int rc;

	rc = asprintf(&s, "GET %s?code=%s&state=%s HTTP/1.1\r\n"
		      "Host: localhost\r\n\r\n", TEST_PATH, code, state);
	FYAI_TCHECK(rc > 0);
	return s;
}

/* Run until the flow settles or the bound elapses. */
static void run_until_settled(struct fyai_event_loop *el,
			      struct fyai_oauth_flow *f)
{
	fyai_event_ms_t end = fyai_event_now_ms() + TEST_BOUND_MS;

	while (!fyai_oauth_state_is_terminal(fyai_oauth_flow_state(f)) &&
	       fyai_event_now_ms() < end)
		fyai_event_loop_step(el, 50);
}

static void test_plain_redirect(void)
{
	struct fyai_oauth_params p = test_params(TEST_BOUND_MS);
	struct fyai_event_loop *el;
	struct fyai_oauth_flow *f = NULL;
	struct client c;
	char *req;
	int rc;

	el = fyai_event_loop_create(&test_ctx);
	FYAI_TCHECK(el);
	rc = fyai_oauth_flow_start(&test_ctx, el, &p, NULL, NULL, &f);
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(fyai_oauth_flow_state(f) == FYAI_OAUTH_LISTENING);
	FYAI_TCHECK(fyai_oauth_flow_port(f) != 0);

	req = redirect_request("THECODE", TEST_STATE);
	client_connect(el, &c, fyai_oauth_flow_port(f), req, 1, NULL);

	run_until_settled(el, f);

	FYAI_TCHECK(fyai_oauth_flow_state(f) == FYAI_OAUTH_GOT_CODE);
	FYAI_TCHECK(fyai_oauth_flow_code(f));
	FYAI_TCHECK(!strcmp(fyai_oauth_flow_code(f), "THECODE"));

	/* the winning connection is held open until the verdict is reported */
	fyai_oauth_flow_finish(f, true);

	free(req);
	client_close(&c);
	fyai_oauth_flow_destroy(f);
	fyai_event_loop_destroy(el);
	printf("  plain redirect: ok\n");
}

/* A favicon fetch must not fail the login. */
static void test_favicon_does_not_fail_login(void)
{
	struct fyai_oauth_params p = test_params(TEST_BOUND_MS);
	struct fyai_event_loop *el;
	struct fyai_oauth_flow *f = NULL;
	struct client fav, cb;
	char *req;
	fyai_event_ms_t end;
	int rc;

	el = fyai_event_loop_create(&test_ctx);
	FYAI_TCHECK(el);
	rc = fyai_oauth_flow_start(&test_ctx, el, &p, NULL, NULL, &f);
	FYAI_TCHECK(!rc);

	client_connect(el, &fav, fyai_oauth_flow_port(f),
		       "GET /favicon.ico HTTP/1.1\r\nHost: localhost\r\n\r\n",
		       1, NULL);

	/* let the favicon be served without settling the flow */
	end = fyai_event_now_ms() + 500;
	while (fyai_event_now_ms() < end && !fav.resplen)
		fyai_event_loop_step(el, 20);

	FYAI_TCHECK(!strcmp(resp_status(&fav), "HTTP/1.1 404 Not Found"));
	FYAI_TCHECK(fyai_oauth_flow_state(f) == FYAI_OAUTH_LISTENING);

	/* and the real redirect still wins afterwards */
	req = redirect_request("AFTERFAV", TEST_STATE);
	client_connect(el, &cb, fyai_oauth_flow_port(f), req, 1, NULL);
	run_until_settled(el, f);

	FYAI_TCHECK(fyai_oauth_flow_state(f) == FYAI_OAUTH_GOT_CODE);
	FYAI_TCHECK(!strcmp(fyai_oauth_flow_code(f), "AFTERFAV"));

	free(req);
	client_close(&fav);
	client_close(&cb);
	fyai_oauth_flow_destroy(f);
	fyai_event_loop_destroy(el);
	printf("  favicon does not fail the login: ok\n");
}

/* A connection that opens and sends nothing must not stall the receiver. */
static void test_silent_preconnect(void)
{
	struct fyai_oauth_params p = test_params(TEST_BOUND_MS);
	struct fyai_event_loop *el;
	struct fyai_oauth_flow *f = NULL;
	struct client idle, cb;
	char *req;
	int rc;

	el = fyai_event_loop_create(&test_ctx);
	FYAI_TCHECK(el);
	rc = fyai_oauth_flow_start(&test_ctx, el, &p, NULL, NULL, &f);
	FYAI_TCHECK(!rc);

	/* opens, never speaks */
	client_connect(el, &idle, fyai_oauth_flow_port(f), NULL, 0, NULL);
	fyai_event_loop_step(el, 50);
	FYAI_TCHECK(fyai_oauth_flow_state(f) == FYAI_OAUTH_LISTENING);

	req = redirect_request("PRECONN", TEST_STATE);
	client_connect(el, &cb, fyai_oauth_flow_port(f), req, 1, NULL);
	run_until_settled(el, f);

	FYAI_TCHECK(fyai_oauth_flow_state(f) == FYAI_OAUTH_GOT_CODE);
	FYAI_TCHECK(!strcmp(fyai_oauth_flow_code(f), "PRECONN"));

	free(req);
	client_close(&idle);
	client_close(&cb);
	fyai_oauth_flow_destroy(f);
	fyai_event_loop_destroy(el);
	printf("  silent preconnect: ok\n");
}

/* A request line split across segments must be reassembled. */
static void test_split_request(void)
{
	struct fyai_oauth_params p = test_params(TEST_BOUND_MS);
	struct fyai_event_loop *el;
	struct fyai_oauth_flow *f = NULL;
	struct client c;
	char *req;
	int rc;

	el = fyai_event_loop_create(&test_ctx);
	FYAI_TCHECK(el);
	rc = fyai_oauth_flow_start(&test_ctx, el, &p, NULL, NULL, &f);
	FYAI_TCHECK(!rc);

	req = redirect_request("SPLITCODE", TEST_STATE);
	client_connect(el, &c, fyai_oauth_flow_port(f), req, 6, NULL);

	run_until_settled(el, f);

	FYAI_TCHECK(fyai_oauth_flow_state(f) == FYAI_OAUTH_GOT_CODE);
	FYAI_TCHECK(!strcmp(fyai_oauth_flow_code(f), "SPLITCODE"));

	free(req);
	client_close(&c);
	fyai_oauth_flow_destroy(f);
	fyai_event_loop_destroy(el);
	printf("  split request line: ok\n");
}

static void test_bad_state_rejected(void)
{
	struct fyai_oauth_params p = test_params(TEST_BOUND_MS);
	struct fyai_event_loop *el;
	struct fyai_oauth_flow *f = NULL;
	struct client c;
	char *req;
	int rc;

	el = fyai_event_loop_create(&test_ctx);
	FYAI_TCHECK(el);
	rc = fyai_oauth_flow_start(&test_ctx, el, &p, NULL, NULL, &f);
	FYAI_TCHECK(!rc);

	req = redirect_request("EVILCODE", "not-the-state");
	client_connect(el, &c, fyai_oauth_flow_port(f), req, 1, NULL);

	run_until_settled(el, f);

	FYAI_TCHECK(fyai_oauth_flow_state(f) == FYAI_OAUTH_BAD_STATE);
	FYAI_TCHECK(!fyai_oauth_flow_code(f));	/* never surfaced */

	free(req);
	client_close(&c);
	fyai_oauth_flow_destroy(f);
	fyai_event_loop_destroy(el);
	printf("  wrong state rejected: ok\n");
}

static void test_timeout(void)
{
	struct fyai_oauth_params p = test_params(120);
	struct fyai_event_loop *el;
	struct fyai_oauth_flow *f = NULL;
	int rc;

	el = fyai_event_loop_create(&test_ctx);
	FYAI_TCHECK(el);
	rc = fyai_oauth_flow_start(&test_ctx, el, &p, NULL, NULL, &f);
	FYAI_TCHECK(!rc);

	run_until_settled(el, f);

	FYAI_TCHECK(fyai_oauth_flow_state(f) == FYAI_OAUTH_TIMED_OUT);
	FYAI_TCHECK(!fyai_oauth_flow_code(f));

	fyai_oauth_flow_destroy(f);
	fyai_event_loop_destroy(el);
	printf("  timeout: ok\n");
}

/* The completion callback fires exactly once, on settling. */
static int done_calls;
static enum fyai_oauth_state done_state;

static void on_done(struct fyai_oauth_flow *f, void *user)
{
	(void)user;
	done_calls++;
	done_state = fyai_oauth_flow_state(f);
}

static void test_completion_callback(void)
{
	struct fyai_oauth_params p = test_params(TEST_BOUND_MS);
	struct fyai_event_loop *el;
	struct fyai_oauth_flow *f = NULL;
	struct client c;
	char *req;
	int rc;

	done_calls = 0;
	el = fyai_event_loop_create(&test_ctx);
	FYAI_TCHECK(el);
	rc = fyai_oauth_flow_start(&test_ctx, el, &p, on_done, NULL, &f);
	FYAI_TCHECK(!rc);

	req = redirect_request("CBCODE", TEST_STATE);
	client_connect(el, &c, fyai_oauth_flow_port(f), req, 1, NULL);
	run_until_settled(el, f);

	FYAI_TCHECK(done_calls == 1);
	FYAI_TCHECK(done_state == FYAI_OAUTH_GOT_CODE);

	/* extra loop turns must not re-fire it */
	fyai_event_loop_step(el, 20);
	FYAI_TCHECK(done_calls == 1);

	free(req);
	client_close(&c);
	fyai_oauth_flow_destroy(f);
	fyai_event_loop_destroy(el);
	printf("  completion callback fires once: ok\n");
}

/* The point of the state machine: a flow armed on a loop must not stop that
 * loop doing other work. */
static int tick_count;

static enum fyai_event_action on_tick(const struct fyai_event *ev)
{
	(void)ev;
	tick_count++;
	return FYAIEA_CONTINUE;
}

static void test_shares_the_loop(void)
{
	struct fyai_oauth_params p = test_params(TEST_BOUND_MS);
	struct fyai_event_loop *el;
	struct fyai_event_source *tick = NULL;
	struct fyai_oauth_flow *f = NULL;
	struct client c;
	char *req;
	int rc;

	tick_count = 0;
	el = fyai_event_loop_create(&test_ctx);
	FYAI_TCHECK(el);

	rc = fyai_event_add_timer(el, 10, 10, on_tick, NULL, &tick);
	FYAI_TCHECK(!rc);

	rc = fyai_oauth_flow_start(&test_ctx, el, &p, NULL, NULL, &f);
	FYAI_TCHECK(!rc);

	/* the login is outstanding; the loop keeps serving the timer */
	while (tick_count < 3)
		fyai_event_loop_step(el, 50);
	FYAI_TCHECK(fyai_oauth_flow_state(f) == FYAI_OAUTH_LISTENING);

	req = redirect_request("SHARED", TEST_STATE);
	client_connect(el, &c, fyai_oauth_flow_port(f), req, 1, NULL);
	run_until_settled(el, f);

	FYAI_TCHECK(fyai_oauth_flow_state(f) == FYAI_OAUTH_GOT_CODE);
	FYAI_TCHECK(tick_count >= 3);

	free(req);
	client_close(&c);
	fyai_event_source_remove(tick);
	fyai_oauth_flow_destroy(f);
	fyai_event_loop_destroy(el);
	printf("  flow shares the loop: ok\n");
}

/* More connections than slots: the receiver sheds the excess and still works. */
static void test_connection_flood(void)
{
	struct fyai_oauth_params p = test_params(TEST_BOUND_MS);
	struct fyai_event_loop *el;
	struct fyai_oauth_flow *f = NULL;
	struct client flood[16];
	struct client cb;
	char *req;
	size_t i;
	int rc;

	el = fyai_event_loop_create(&test_ctx);
	FYAI_TCHECK(el);
	rc = fyai_oauth_flow_start(&test_ctx, el, &p, NULL, NULL, &f);
	FYAI_TCHECK(!rc);

	for (i = 0; i < 16; i++)
		client_connect(el, &flood[i], fyai_oauth_flow_port(f), NULL, 0,
			       NULL);
	fyai_event_loop_step(el, 50);
	fyai_event_loop_step(el, 50);
	FYAI_TCHECK(fyai_oauth_flow_state(f) == FYAI_OAUTH_LISTENING);

	/* drop them, then the real redirect must still be served */
	for (i = 0; i < 16; i++)
		client_close(&flood[i]);
	fyai_event_loop_step(el, 50);

	req = redirect_request("FLOOD", TEST_STATE);
	client_connect(el, &cb, fyai_oauth_flow_port(f), req, 1, NULL);
	run_until_settled(el, f);

	FYAI_TCHECK(fyai_oauth_flow_state(f) == FYAI_OAUTH_GOT_CODE);
	FYAI_TCHECK(!strcmp(fyai_oauth_flow_code(f), "FLOOD"));

	free(req);
	client_close(&cb);
	fyai_oauth_flow_destroy(f);
	fyai_event_loop_destroy(el);
	printf("  connection flood: ok\n");
}

/* Destroying a flow that never settled must release the port. */
static void test_destroy_while_listening(void)
{
	struct fyai_oauth_params p = test_params(TEST_BOUND_MS);
	struct fyai_event_loop *el;
	struct fyai_oauth_flow *f = NULL;
	unsigned short port;
	int rc;

	el = fyai_event_loop_create(&test_ctx);
	FYAI_TCHECK(el);
	rc = fyai_oauth_flow_start(&test_ctx, el, &p, NULL, NULL, &f);
	FYAI_TCHECK(!rc);
	port = fyai_oauth_flow_port(f);
	FYAI_TCHECK(port);

	fyai_oauth_flow_destroy(f);

	/* the same port binds again immediately */
	p.ports = &port;
	rc = fyai_oauth_flow_start(&test_ctx, el, &p, NULL, NULL, &f);
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(fyai_oauth_flow_port(f) == port);

	fyai_oauth_flow_destroy(f);
	fyai_event_loop_destroy(el);
	printf("  destroy while listening releases the port: ok\n");
}

static void test_pkce(void)
{
	struct fyai_oauth_pkce a, b;
	int rc;

	rc = fyai_oauth_pkce_generate(&a);
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(a.verifier && a.challenge && a.state);

	/* base64url: no padding, no + or / */
	FYAI_TCHECK(!strchr(a.verifier, '=') && !strchr(a.verifier, '+') &&
	       !strchr(a.verifier, '/'));
	FYAI_TCHECK(!strchr(a.challenge, '=') && !strchr(a.challenge, '+') &&
	       !strchr(a.challenge, '/'));

	/* the challenge is a SHA-256 digest, so always 43 base64url chars */
	FYAI_TCHECK(strlen(a.challenge) == 43);
	FYAI_TCHECK(strcmp(a.verifier, a.challenge));

	rc = fyai_oauth_pkce_generate(&b);
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(strcmp(a.verifier, b.verifier));	/* never reused */
	FYAI_TCHECK(strcmp(a.state, b.state));

	fyai_oauth_pkce_cleanup(&a);
	fyai_oauth_pkce_cleanup(&b);
	FYAI_TCHECK(!a.verifier && !a.challenge && !a.state);
	printf("  pkce generation: ok\n");
}

static void test_query_value(void)
{
	CURL *curl = test_ctx.curl;
	char *v;

	v = fyai_oauth_query_value(curl, "GET /cb?code=abc&state=xyz HTTP/1.1", "code");
	FYAI_TCHECK(v && !strcmp(v, "abc"));
	free(v);

	v = fyai_oauth_query_value(curl, "GET /cb?code=abc&state=xyz HTTP/1.1", "state");
	FYAI_TCHECK(v && !strcmp(v, "xyz"));
	free(v);

	/* percent-decoding */
	v = fyai_oauth_query_value(curl, "GET /cb?code=a%20b HTTP/1.1", "code");
	FYAI_TCHECK(v && !strcmp(v, "a b"));
	free(v);

	/* absent key, and a prefix that must not match */
	v = fyai_oauth_query_value(curl, "GET /cb?code=abc HTTP/1.1", "state");
	FYAI_TCHECK(!v);
	v = fyai_oauth_query_value(curl, "GET /cb?xcode=abc HTTP/1.1", "code");
	FYAI_TCHECK(!v);

	/* no query string at all */
	v = fyai_oauth_query_value(curl, "GET /cb HTTP/1.1", "code");
	FYAI_TCHECK(!v);

	printf("  query value parsing: ok\n");
}

int main(void)
{
	int rc;

	memset(&test_cfg, 0, sizeof(test_cfg));
	memset(&test_ctx, 0, sizeof(test_ctx));
	test_ctx.cfg = &test_cfg;
	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);

	curl_global_init(CURL_GLOBAL_DEFAULT);
	test_ctx.curl = curl_easy_init();
	FYAI_TCHECK(test_ctx.curl);

	printf("fyai_oauth tests\n");

	test_pkce();
	test_query_value();
	test_plain_redirect();
	test_favicon_does_not_fail_login();
	test_silent_preconnect();
	test_split_request();
	test_bad_state_rejected();
	test_timeout();
	test_completion_callback();
	test_shares_the_loop();
	test_connection_flood();
	test_destroy_while_listening();

	curl_easy_cleanup(test_ctx.curl);
	curl_global_cleanup();
	fyai_event_pool_drain(&test_ctx);

	printf("all oauth tests passed\n");
	return 0;
}
