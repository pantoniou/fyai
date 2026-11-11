/*
 * fyai_httpd_test.c - unit tests for the network layer, the HTTP server and
 * the WebSocket client
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 *
 * The server runs on an event loop that the test steps. A raw client is a
 * blocking socket that sends its request before the server runs, which the
 * kernel allows, and reads after the server has answered and closed. A test of
 * a connection that stays open reads with srv_read_until(), which runs the
 * loop between two reads. The
 * WebSocket client blocks by design, so it runs in a thread of its own.
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
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
#include "fyai_httpd.h"
#include "fyai_net.h"
#include "fyai_remote_tls.h"
#include "fyai_test.h"
#include "fyai_ws_client.h"

#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(net, url_parse, net_url_parse)
FYAI_TEST_ENTRY(httpd, parse_a_request, httpd_parse)
FYAI_TEST_ENTRY(httpd, answers_and_ends, httpd_respond)
FYAI_TEST_ENTRY(httpd, refuses_a_long_head, httpd_long_head)
FYAI_TEST_ENTRY(httpd, refuses_a_request_that_is_not_http, httpd_not_http)
FYAI_TEST_ENTRY(httpd, websocket_client_and_server, httpd_websocket)
FYAI_TEST_ENTRY(httpd, lists_the_headers, httpd_headers)
FYAI_TEST_ENTRY(wsc, echoes_a_message, wsc_echo)
FYAI_TEST_ENTRY(wsc, reports_a_refusal_and_a_refused_connect, wsc_failures)
FYAI_TEST_ENTRY(wsc, checks_the_pin_over_tls, wsc_tls)
FYAI_TEST_ENTRY(httpd, reads_a_body_and_keeps_the_connection, httpd_body)
FYAI_TEST_ENTRY(httpd, limits_a_body, httpd_body_limits)
FYAI_TEST_ENTRY(httpd, answers_expect_continue, httpd_expect_continue)
FYAI_TEST_ENTRY(httpd, ends_on_close_and_http10, httpd_close_and_http10)

/* A step of the loop in which nothing happens ends a wait that cannot end. */
#define TEST_STEPS	400

static struct fyai_cfg test_cfg;
static struct fyai_ctx test_ctx;

static int httpd_run(void (*testfn)(void))
{
	sigset_t pipe_mask;
	int rc;

	/* The program keeps SIGPIPE blocked; a write to a client that left is an error. */
	sigemptyset(&pipe_mask);
	sigaddset(&pipe_mask, SIGPIPE);
	pthread_sigmask(SIG_BLOCK, &pipe_mask, NULL);

	memset(&test_cfg, 0, sizeof(test_cfg));
	memset(&test_ctx, 0, sizeof(test_ctx));
	test_ctx.cfg = &test_cfg;
	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);
	curl_global_init(CURL_GLOBAL_DEFAULT);
	test_ctx.curl = curl_easy_init();
	FYAI_TCHECK(test_ctx.curl);

	testfn();

	curl_easy_cleanup(test_ctx.curl);
	curl_global_cleanup();
	fyai_event_pool_drain(&test_ctx);
	return 0;
}

/* ---- net ------------------------------------------------------------------ */

static void test_url_parse(void)
{
	struct fyai_net_url u;

	FYAI_TCHECK(!fyai_net_url_parse(&test_ctx, "http://example.org", &u));
	FYAI_TCHECK(!u.tls && !strcmp(u.host, "example.org") && !strcmp(u.port, "80"));
	FYAI_TCHECK(!strcmp(u.authority, "example.org"));

	FYAI_TCHECK(!fyai_net_url_parse(&test_ctx, "https://example.org/a/b?c", &u));
	FYAI_TCHECK(u.tls && !strcmp(u.port, "443") && !strcmp(u.authority, "example.org"));

	FYAI_TCHECK(!fyai_net_url_parse(&test_ctx, "http://127.0.0.1:8080/x", &u));
	FYAI_TCHECK(!strcmp(u.host, "127.0.0.1") && !strcmp(u.port, "8080"));
	FYAI_TCHECK(!strcmp(u.authority, "127.0.0.1:8080"));

	/* The socket takes an address with no brackets; the Host header keeps them. */
	FYAI_TCHECK(!fyai_net_url_parse(&test_ctx, "http://[::1]:9/", &u));
	FYAI_TCHECK(!strcmp(u.host, "::1") && !strcmp(u.port, "9"));
	FYAI_TCHECK(!strcmp(u.authority, "[::1]:9"));
	FYAI_TCHECK(!fyai_net_url_parse(&test_ctx, "https://[::1]", &u));
	FYAI_TCHECK(!strcmp(u.host, "::1") && !strcmp(u.port, "443"));

	FYAI_TCHECK(fyai_net_url_parse(&test_ctx, "ftp://example.org/", &u));
	FYAI_TCHECK(fyai_net_url_parse(&test_ctx, "example.org", &u));
	FYAI_TCHECK(fyai_net_url_parse(&test_ctx, "http://", &u));
	FYAI_TCHECK(fyai_net_url_parse(&test_ctx, "http://example.org:port/", &u));
	FYAI_TCHECK(fyai_net_url_parse(&test_ctx, "http://user:secret@example.org/", &u));
	fyai_diag_reset(&test_cfg.diag);
}

/* ---- the request head ---------------------------------------------------- */

static int parse_text(const char *text, struct fyai_http_request *req)
{
	return fyai_http_parse(text, strlen(text), 0, req);
}

static void test_parse(void)
{
	struct fyai_http_request req;
	int rc;

	rc = parse_text("GET /a?b=c HTTP/1.1\r\nHost: h:1\r\nOrigin: http://h:1\r\n"
			"Authorization: Bearer tok\r\nUpgrade: websocket\r\n"
			"Connection: keep-alive, Upgrade\r\nSec-WebSocket-Version: 13\r\n"
			"Sec-WebSocket-Key: k\r\n\r\n", &req);
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(!strcmp(req.method, "GET") && !strcmp(req.target, "/a?b=c"));
	FYAI_TCHECK(!strcmp(req.host, "h:1") && !strcmp(req.bearer, "tok"));
	FYAI_TCHECK(req.upgrade && req.connection_upgrade && req.ws_version == 13);
	FYAI_TCHECK(fyai_http_origin_ok(&req, false) && !fyai_http_origin_ok(&req, true));
	fyai_http_request_release(&req);

	/* A header that is not a bearer is a bearer that nothing matches. */
	rc = parse_text("GET / HTTP/1.1\r\nHost: h\r\nAuthorization: Basic x\r\n\r\n", &req);
	FYAI_TCHECK(!rc && !strcmp(req.bearer, ""));
	fyai_http_request_release(&req);

	FYAI_TCHECK(parse_text("GET / HTTP/1.1\r\n\r\n", &req) == 400);
	FYAI_TCHECK(parse_text("GET / HTTP/1.0\r\nHost: h\r\n\r\n", &req) == 505);
	FYAI_TCHECK(parse_text("GET / HTTP/1.2\r\nHost: h\r\n\r\n", &req) == 505);
	FYAI_TCHECK(parse_text("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", &req) == 400);
	FYAI_TCHECK(parse_text("GET x HTTP/1.1\r\nHost: h\r\n\r\n", &req) == 400);
	FYAI_TCHECK(parse_text("GET / HTTP/1.1\nHost: h\n\n", &req) == 400);
	FYAI_TCHECK(parse_text("GET / HTTP/1.1\r\nHost: h\r\n folded\r\n\r\n", &req) == 400);
	FYAI_TCHECK(parse_text("GET / HTTP/1.1\r\nHost : h\r\n\r\n", &req) == 400);
	FYAI_TCHECK(parse_text("GET / HTTP/1.1\r\nHost: h\r\nSec-WebSocket-Version: x\r\n\r\n",
			       &req) == 400);
}

static void test_headers(void)
{
	struct fyai_http_request req;
	const char *head = "POST /p HTTP/1.1\r\nHost: h\r\nX-One: 1\r\n"
			   "x-two:  2 \r\nConnection: keep-alive\r\n"
			   "Connection: close\r\n\r\n";

	FYAI_TCHECK(!fyai_http_parse(head, strlen(head), 0, &req));
	FYAI_TCHECK(req.nheaders == 5 && !req.http10);
	FYAI_TCHECK(!strcmp(fyai_http_header(&req, "x-one"), "1"));
	FYAI_TCHECK(!strcmp(fyai_http_header(&req, "X-TWO"), "2"));
	FYAI_TCHECK(!strcmp(fyai_http_header(&req, "connection"), "keep-alive"));
	FYAI_TCHECK(!fyai_http_header(&req, "X-Three"));
	fyai_http_request_release(&req);

	/* HTTP/1.0 is taken on request, and needs no Host. */
	head = "GET / HTTP/1.0\r\n\r\n";
	FYAI_TCHECK(fyai_http_parse(head, strlen(head), 0, &req) == 505);
	FYAI_TCHECK(!fyai_http_parse(head, strlen(head), FYAI_HTTP_PARSE_HTTP10, &req));
	FYAI_TCHECK(req.http10 && !req.host);
	FYAI_TCHECK(!fyai_http_origin_ok(&req, false) || !req.origin);
	fyai_http_request_release(&req);
	head = "GET / HTTP/1.1\r\n\r\n";
	FYAI_TCHECK(fyai_http_parse(head, strlen(head), FYAI_HTTP_PARSE_HTTP10, &req) == 400);
}

/* ---- the server ----------------------------------------------------------- */

struct server {
	struct fyai_event_loop *el;
	struct fyai_httpd *h;
	char bound[64];
	int closed;
	char log[256];
};

static void srv_request(void *data, struct fyai_httpd_conn *c,
			const struct fyai_http_request *req)
{
	char head[128];
	int n;

	(void)data;
	if (!strcmp(req->target, "/ok")) {
		fyai_httpd_respond(c, 200, NULL, "Content-Type: text/plain\r\n", "hello", 5);
		return;
	}
	if (!strcmp(req->target, "/ws")) {
		if (fyai_httpd_ws_check(c, req))
			return;
		fyai_httpd_ws_accept(c, req);
		return;
	}
	if (!strcmp(req->target, "/echo")) {
		/* An answer of the handler's own, with the connection kept. */
		n = snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\n"
			     "Content-Length: %zu\r\n\r\n", req->body_len);
		FYAI_TCHECK(!fyai_httpd_conn_write(c, head, (size_t)n));
		if (req->body_len)
			FYAI_TCHECK(!fyai_httpd_conn_write(c, req->body, req->body_len));
		fyai_httpd_conn_done(c, false);
		return;
	}
	fyai_httpd_respond(c, 404, "no such path", NULL, NULL, 0);
}

/* The echo of a message, with a prefix, so that the client knows who answered. */
static void srv_message(void *data, struct fyai_httpd_conn *c,
			const struct fyai_ws_message *msg)
{
	char *text;

	(void)data;
	text = malloc(msg->size + 6);
	FYAI_TCHECK(text);
	memcpy(text, "echo:", 5);
	memcpy(text + 5, msg->data, msg->size);
	FYAI_TCHECK(!fyai_httpd_ws_send(c, msg->opcode, text, msg->size + 5));
	free(text);
	fyai_httpd_kick(c);
}

static void srv_closed(void *data, struct fyai_httpd_conn *c)
{
	struct server *s = data;

	(void)c;
	s->closed++;
}

static void srv_log(void *data, const char *line)
{
	struct server *s = data;

	snprintf(s->log, sizeof(s->log), "%s", line);
}

static void srv_start_full(struct server *s, size_t max_body, bool keepalive,
			   bool http10, SSL_CTX *tls)
{
	struct fyai_httpd_opts opts = { .max_conns = 8, .max_message = 1u << 16,
		.max_output = 1u << 20, .head_ms = 60000, .max_body = max_body,
		.keepalive = keepalive, .http10 = http10 };
	struct fyai_httpd_ops ops = { .request = srv_request, .ws_message = srv_message,
		.closed = srv_closed, .log = srv_log, .data = s };

	memset(s, 0, sizeof(*s));
	s->el = fyai_event_loop_create(&test_ctx);
	FYAI_TCHECK(s->el);
	opts.loop = s->el;
	s->h = fyai_httpd_new(&test_ctx, &opts, &ops);
	FYAI_TCHECK(s->h);
	FYAI_TCHECK(!fyai_httpd_listen(s->h, "127.0.0.1:0", tls, s->bound, sizeof(s->bound)));
}

static void srv_start_with(struct server *s, size_t max_body, bool keepalive,
			   bool http10)
{
	srv_start_full(s, max_body, keepalive, http10, NULL);
}

static void srv_start(struct server *s)
{
	srv_start_with(s, 0, false, false);
}

static void srv_stop(struct server *s)
{
	fyai_httpd_close(s->h);
	fyai_event_loop_destroy(s->el);
}

/* A blocking client socket connected to the server. */
static int raw_connect(const struct server *s)
{
	struct sockaddr_in sa = { .sin_family = AF_INET };
	int fd = socket(AF_INET, SOCK_STREAM, 0);

	FYAI_TCHECK(fd >= 0);
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sa.sin_port = htons((unsigned short)atoi(strrchr(s->bound, ':') + 1));
	FYAI_TCHECK(!connect(fd, (struct sockaddr *)&sa, sizeof(sa)));
	return fd;
}

static void raw_send(int fd, const void *data, size_t n)
{
	const unsigned char *p = data;
	ssize_t put;

	while (n) {
		put = write(fd, p, n);
		FYAI_TCHECK(put > 0);
		p += put;
		n -= (size_t)put;
	}
}

static void raw_send_text(int fd, const char *text)
{
	raw_send(fd, text, strlen(text));
}

/* Step the loop until the server has ended @count connections. */
static void srv_wait_closed(struct server *s, int count)
{
	int i;

	for (i = 0; i < TEST_STEPS && s->closed < count; i++)
		fyai_event_loop_step(s->el, 50);
	FYAI_TCHECK(s->closed >= count);
}

/*
 * Read one whole answer: its head, and the body that Content-Length counts.
 * The descriptor of an ended connection closes at the next turn of the loop,
 * so the client does not wait for the end of the stream.
 */
static size_t raw_read_all(int fd, char *out, size_t size)
{
	size_t n = 0, want = 0;
	const char *mark, *len;
	ssize_t got;

	while (n + 1 < size && (!want || n < want)) {
		got = read(fd, out + n, size - 1 - n);
		if (got <= 0)
			break;
		n += (size_t)got;
		out[n] = '\0';
		mark = strstr(out, "\r\n\r\n");
		len = strstr(out, "Content-Length: ");
		if (mark && len)
			want = (size_t)(mark + 4 - out) + (size_t)atoi(len + 16);
	}
	out[n] = '\0';
	return n;
}

static void test_respond(void)
{
	struct server s;
	char text[512];
	int fd;

	srv_start(&s);
	fd = raw_connect(&s);
	raw_send_text(fd, "GET /ok HTTP/1.1\r\nHost: h\r\n\r\n");
	srv_wait_closed(&s, 1);
	raw_read_all(fd, text, sizeof(text));
	close(fd);
	FYAI_TCHECK(!strncmp(text, "HTTP/1.1 200 OK\r\n", 17));
	FYAI_TCHECK(strstr(text, "Content-Length: 5\r\n"));
	FYAI_TCHECK(strstr(text, "Connection: close\r\n"));
	FYAI_TCHECK(strstr(text, "Content-Type: text/plain\r\n"));
	FYAI_TCHECK(!strcmp(text + strlen(text) - 9, "\r\n\r\nhello"));

	fd = raw_connect(&s);
	raw_send_text(fd, "GET /nowhere HTTP/1.1\r\nHost: h\r\n\r\n");
	srv_wait_closed(&s, 2);
	raw_read_all(fd, text, sizeof(text));
	close(fd);
	FYAI_TCHECK(!strncmp(text, "HTTP/1.1 404 Not Found\r\n", 24));
	FYAI_TCHECK(strstr(s.log, "no such path"));
	srv_stop(&s);
}

static void test_long_head(void)
{
	struct server s;
	char text[512], filler[FYAI_HTTP_HEAD_MAX + 64];
	int fd;

	srv_start(&s);
	fd = raw_connect(&s);
	memset(filler, 'a', sizeof(filler));
	raw_send(fd, "GET / HTTP/1.1\r\nX: ", 19);
	raw_send(fd, filler, sizeof(filler));
	srv_wait_closed(&s, 1);
	raw_read_all(fd, text, sizeof(text));
	close(fd);
	FYAI_TCHECK(!strncmp(text, "HTTP/1.1 431 ", 13));
	srv_stop(&s);
}

static void test_not_http(void)
{
	struct server s;
	char text[512];
	int fd;

	srv_start(&s);
	fd = raw_connect(&s);
	raw_send_text(fd, "HELLO\r\n\r\n");
	srv_wait_closed(&s, 1);
	raw_read_all(fd, text, sizeof(text));
	close(fd);
	FYAI_TCHECK(!strncmp(text, "HTTP/1.1 400 ", 13));

	fd = raw_connect(&s);
	raw_send_text(fd, "GET / HTTP/1.0\r\nHost: h\r\n\r\n");
	srv_wait_closed(&s, 2);
	raw_read_all(fd, text, sizeof(text));
	close(fd);
	FYAI_TCHECK(!strncmp(text, "HTTP/1.1 505 ", 13));
	srv_stop(&s);
}

/* ---- the body and the kept connection ------------------------------------ */

/*
 * Run the server until the text @needle is in what the client @fd has read,
 * and return the text. The client socket does not block, so the loop of the
 * server runs between two reads.
 */
static const char *srv_read_until(struct server *s, int fd, char *out, size_t size,
				  const char *needle)
{
	size_t n = strlen(out);
	ssize_t got;
	int i;

	FYAI_TCHECK(!fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK));
	for (i = 0; i < TEST_STEPS && !strstr(out, needle); i++) {
		fyai_event_loop_step(s->el, 20);
		got = read(fd, out + n, size - 1 - n);
		if (got > 0) {
			n += (size_t)got;
			out[n] = '\0';
		}
	}
	FYAI_TCHECK(strstr(out, needle));
	return out;
}

static void test_body(void)
{
	struct server s;
	char text[1024] = "";
	int fd;

	srv_start_with(&s, 64, true, false);
	fd = raw_connect(&s);

	/* A body that comes in two parts, then a second request on the same
	 * connection, which the answer did not end. The expect test holds the
	 * case where the head is read before its body. */
	raw_send_text(fd, "POST /echo HTTP/1.1\r\nHost: h\r\nContent-Length: 10\r\n\r\nabcde");
	raw_send_text(fd, "fghij");
	srv_read_until(&s, fd, text, sizeof(text), "\r\n\r\nabcdefghij");
	FYAI_TCHECK(strstr(text, "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n"));
	FYAI_TCHECK(!s.closed);

	text[0] = '\0';
	raw_send_text(fd, "POST /echo HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n"
			  "3\r\nabc\r\n4\r\ndefg\r\n0\r\n\r\n");
	srv_read_until(&s, fd, text, sizeof(text), "\r\n\r\nabcdefg");
	FYAI_TCHECK(strstr(text, "Content-Length: 7\r\n"));

	/* Two requests that came together are answered in order. */
	text[0] = '\0';
	raw_send_text(fd, "POST /echo HTTP/1.1\r\nHost: h\r\nContent-Length: 3\r\n\r\none"
			  "POST /echo HTTP/1.1\r\nHost: h\r\nContent-Length: 3\r\n\r\ntwo");
	srv_read_until(&s, fd, text, sizeof(text), "two");
	FYAI_TCHECK(strstr(text, "one") && strstr(text, "one") < strstr(text, "two"));

	/* A request without a body has none. */
	text[0] = '\0';
	raw_send_text(fd, "POST /echo HTTP/1.1\r\nHost: h\r\n\r\n");
	srv_read_until(&s, fd, text, sizeof(text), "Content-Length: 0\r\n");
	FYAI_TCHECK(!s.closed);
	close(fd);
	srv_wait_closed(&s, 1);
	srv_stop(&s);
}

static void test_body_limits(void)
{
	struct server s;
	char text[512] = "";
	int fd;

	srv_start_with(&s, 8, true, false);

	/* A body that is too long is refused before it is read. */
	fd = raw_connect(&s);
	raw_send_text(fd, "POST /echo HTTP/1.1\r\nHost: h\r\nContent-Length: 100\r\n\r\n");
	srv_wait_closed(&s, 1);
	raw_read_all(fd, text, sizeof(text));
	close(fd);
	FYAI_TCHECK(!strncmp(text, "HTTP/1.1 413 ", 13));

	/* So is a chunked body that grows past the limit. */
	text[0] = '\0';
	fd = raw_connect(&s);
	raw_send_text(fd, "POST /echo HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n"
			  "5\r\nabcde\r\n5\r\nfghij\r\n");
	srv_wait_closed(&s, 2);
	raw_read_all(fd, text, sizeof(text));
	close(fd);
	FYAI_TCHECK(!strncmp(text, "HTTP/1.1 413 ", 13));

	/* A length that is not a number is not a request. */
	text[0] = '\0';
	fd = raw_connect(&s);
	raw_send_text(fd, "POST /echo HTTP/1.1\r\nHost: h\r\nContent-Length: x\r\n\r\n");
	srv_wait_closed(&s, 3);
	raw_read_all(fd, text, sizeof(text));
	close(fd);
	FYAI_TCHECK(!strncmp(text, "HTTP/1.1 400 ", 13));
	srv_stop(&s);

	/* A server that takes no body refuses the bytes of one. */
	srv_start_with(&s, 0, true, false);
	text[0] = '\0';
	fd = raw_connect(&s);
	raw_send_text(fd, "POST /echo HTTP/1.1\r\nHost: h\r\nContent-Length: 1\r\n\r\nx");
	srv_wait_closed(&s, 1);
	raw_read_all(fd, text, sizeof(text));
	close(fd);
	FYAI_TCHECK(!strncmp(text, "HTTP/1.1 400 ", 13));
	srv_stop(&s);
}

static void test_expect_continue(void)
{
	struct server s;
	char text[512] = "";
	int fd;

	srv_start_with(&s, 64, true, false);
	fd = raw_connect(&s);
	raw_send_text(fd, "POST /echo HTTP/1.1\r\nHost: h\r\nExpect: 100-continue\r\n"
			  "Content-Length: 4\r\n\r\n");
	srv_read_until(&s, fd, text, sizeof(text), "HTTP/1.1 100 Continue\r\n\r\n");
	raw_send_text(fd, "body");
	srv_read_until(&s, fd, text, sizeof(text), "\r\n\r\nbody");
	/* 100 Continue goes once. */
	FYAI_TCHECK(strstr(text + 10, "100 Continue") == NULL);
	close(fd);
	srv_wait_closed(&s, 1);
	srv_stop(&s);
}

static void test_close_and_http10(void)
{
	struct server s;
	char text[512] = "";
	int fd;

	/* Connection: close ends the connection after the answer. */
	srv_start_with(&s, 64, true, true);
	fd = raw_connect(&s);
	raw_send_text(fd, "POST /echo HTTP/1.1\r\nHost: h\r\nConnection: close\r\n"
			  "Content-Length: 2\r\n\r\nhi");
	srv_wait_closed(&s, 1);
	raw_read_all(fd, text, sizeof(text));
	close(fd);
	FYAI_TCHECK(strstr(text, "\r\n\r\nhi"));

	/* So does HTTP/1.0, when the server takes it. */
	text[0] = '\0';
	fd = raw_connect(&s);
	raw_send_text(fd, "POST /echo HTTP/1.0\r\nContent-Length: 2\r\n\r\nho");
	srv_wait_closed(&s, 2);
	raw_read_all(fd, text, sizeof(text));
	close(fd);
	FYAI_TCHECK(strstr(text, "\r\n\r\nho"));
	srv_stop(&s);

	/* A server without keepalive ends the connection after each answer. */
	srv_start_with(&s, 64, false, false);
	text[0] = '\0';
	fd = raw_connect(&s);
	raw_send_text(fd, "POST /echo HTTP/1.1\r\nHost: h\r\nContent-Length: 2\r\n\r\nyo");
	srv_wait_closed(&s, 1);
	raw_read_all(fd, text, sizeof(text));
	close(fd);
	FYAI_TCHECK(strstr(text, "\r\n\r\nyo"));
	srv_stop(&s);
}

/* ---- the WebSocket client on the loop ------------------------------------- */

struct wsc_run {
	bool opened;		/* open() was called */
	bool closed;		/* closed() was called */
	bool was_opened;	/* the argument of closed() */
	int closes;
	char reply[64];
};

static void wsc_open_cb(void *data, struct fyai_wsc *c)
{
	struct wsc_run *run = data;

	run->opened = true;
	FYAI_TCHECK(!fyai_wsc_send(c, FYAI_WS_TEXT, "hello", 5));
}

static void wsc_message_cb(void *data, struct fyai_wsc *c,
			   const struct fyai_ws_message *msg)
{
	struct wsc_run *run = data;

	FYAI_TCHECK(msg->opcode == FYAI_WS_TEXT && msg->size < sizeof(run->reply));
	memcpy(run->reply, msg->data, msg->size);
	run->reply[msg->size] = '\0';
	fyai_wsc_close(c, 1000);
}

static void wsc_closed_cb(void *data, struct fyai_wsc *c, bool opened)
{
	struct wsc_run *run = data;

	(void)c;
	run->closed = true;
	run->was_opened = opened;
	run->closes++;
}

/* Connect to the server @s at @path, and step the loop until the client ends. */
static void wsc_go(struct server *s, struct wsc_run *run, const char *path,
		   bool tls, const char *pin)
{
	struct fyai_wsc_opts opts = { .ws = { .path = path, .max_message = 1u << 16,
		.bearer = "tok", .pin = pin }, .loop = s->el, .max_output = 1u << 20 };
	struct fyai_wsc_ops ops = { .open = wsc_open_cb, .message = wsc_message_cb,
		.closed = wsc_closed_cb, .data = run };
	struct fyai_wsc *c;
	const char *port = strrchr(s->bound, ':') + 1;
	int i;

	memset(run, 0, sizeof(*run));
	opts.ws.url.tls = tls;
	snprintf(opts.ws.url.host, sizeof(opts.ws.url.host), "127.0.0.1");
	snprintf(opts.ws.url.port, sizeof(opts.ws.url.port), "%s", port);
	snprintf(opts.ws.url.authority, sizeof(opts.ws.url.authority), "127.0.0.1:%s", port);
	c = fyai_wsc_open(&test_ctx, &opts, &ops);
	FYAI_TCHECK(c);
	for (i = 0; i < TEST_STEPS && !run->closed; i++)
		fyai_event_loop_step(s->el, 50);
	FYAI_TCHECK(run->closed && run->closes == 1);
}

/* True when the diagnostics hold @text; they are cleared. */
static bool diag_has(const char *text)
{
	char *why = fyai_diag_string(&test_cfg.diag);
	bool has = why && strstr(why, text);

	free(why);
	fyai_diag_reset(&test_cfg.diag);
	return has;
}

static void test_wsc_echo(void)
{
	struct server s;
	struct wsc_run run;

	srv_start(&s);
	wsc_go(&s, &run, "/ws", false, NULL);
	FYAI_TCHECK(run.opened && run.was_opened);
	FYAI_TCHECK(!strcmp(run.reply, "echo:hello"));
	srv_wait_closed(&s, 1);
	srv_stop(&s);
}

static void test_wsc_failures(void)
{
	struct server s;
	struct wsc_run run;

	/* A path that does not upgrade is an answer, and the client says why. */
	srv_start(&s);
	wsc_go(&s, &run, "/nowhere", false, NULL);
	FYAI_TCHECK(!run.opened && !run.was_opened);
	FYAI_TCHECK(diag_has("did not upgrade"));
	srv_wait_closed(&s, 1);
	srv_stop(&s);

	/* A port that nobody listens on: the connect is refused. */
	srv_start(&s);
	fyai_httpd_stop(s.h, NULL);
	wsc_go(&s, &run, "/ws", false, NULL);
	FYAI_TCHECK(!run.opened && !run.was_opened);
	FYAI_TCHECK(diag_has("cannot connect"));
	srv_stop(&s);
}

static void test_wsc_tls(void)
{
	struct server s;
	struct wsc_run run;
	char dir[] = "/tmp/fyai-wsc-XXXXXX", path[128], fp[FYAI_TLS_FP_SIZE];
	char wrong[FYAI_TLS_FP_SIZE];
	SSL_CTX *tls;

	FYAI_TCHECK(mkdtemp(dir));
	snprintf(path, sizeof(path), "%s/server.pem", dir);
	FYAI_TCHECK(!fyai_tls_generate(&test_ctx, path, NULL));
	tls = fyai_tls_server_ctx(&test_ctx, path, path, NULL, fp);
	FYAI_TCHECK(tls);
	srv_start_full(&s, 0, false, false, tls);

	/* The pin of its key opens the connection. */
	wsc_go(&s, &run, "/ws", true, fp);
	FYAI_TCHECK(run.opened && run.was_opened);
	FYAI_TCHECK(!strcmp(run.reply, "echo:hello"));

	/* Another pin refuses it, before the token is sent. */
	snprintf(wrong, sizeof(wrong), "sha256:%064d", 0);
	wsc_go(&s, &run, "/ws", true, wrong);
	FYAI_TCHECK(!run.opened && !run.was_opened);
	FYAI_TCHECK(diag_has("not the pinned"));

	/* A self-signed certificate is not trusted without a pin. */
	wsc_go(&s, &run, "/ws", true, NULL);
	FYAI_TCHECK(!run.opened);
	FYAI_TCHECK(diag_has("not trusted"));

	srv_stop(&s);
	SSL_CTX_free(tls);
	unlink(path);
	rmdir(dir);
}

/* ---- the WebSocket -------------------------------------------------------- */

struct client_run {
	char port[16];
	const char *path;
	atomic_int done;
	int rc;			/* 0 when the exchange went as it should */
	char reply[64];
	int status_ok;
};

static void *ws_thread(void *arg)
{
	struct client_run *run = arg;
	struct fyai_ws_client_opts opts = { .path = run->path, .max_message = 1u << 16,
		.bearer = "tok" };
	struct fyai_ws_client c;
	struct fyai_ws_message msg;
	int r;

	run->rc = -1;
	snprintf(opts.url.host, sizeof(opts.url.host), "127.0.0.1");
	snprintf(opts.url.port, sizeof(opts.url.port), "%s", run->port);
	snprintf(opts.url.authority, sizeof(opts.url.authority), "127.0.0.1:%s", run->port);
	if (fyai_ws_client_open(&test_ctx, &opts, &c))
		goto out;
	if (fyai_ws_client_send(&c, FYAI_WS_TEXT, "hello", 5))
		goto close;
	/* The same bytes may hold more than one message; ask until one is whole. */
	for (r = 0; !(r = fyai_ws_client_recv(&c, &msg, true));)
		;
	if (r != 1 || msg.opcode != FYAI_WS_TEXT || msg.size >= sizeof(run->reply))
		goto close;
	memcpy(run->reply, msg.data, msg.size);
	run->reply[msg.size] = '\0';
	/* A ping is answered with its payload. */
	if (fyai_ws_client_send(&c, FYAI_WS_PING, "p", 1))
		goto close;
	for (r = 0; !(r = fyai_ws_client_recv(&c, &msg, true));)
		;
	if (r != 1 || msg.opcode != FYAI_WS_PONG || msg.size != 1 || msg.data[0] != 'p')
		goto close;
	/* The server echoes the close, and then it ends the connection. */
	if (fyai_ws_client_send(&c, FYAI_WS_CLOSE, "\x03\xe8", 2))
		goto close;
	for (r = 0; !(r = fyai_ws_client_recv(&c, &msg, true));)
		;
	run->rc = r == 1 && msg.opcode == FYAI_WS_CLOSE ? 0 : -1;
close:
	fyai_ws_client_close(&c);
out:
	atomic_store(&run->done, 1);
	return NULL;
}

static void run_client(struct server *s, struct client_run *run, const char *path)
{
	pthread_t th;
	int i;

	memset(run, 0, sizeof(*run));
	run->path = path;
	snprintf(run->port, sizeof(run->port), "%s", strrchr(s->bound, ':') + 1);
	FYAI_TCHECK(!pthread_create(&th, NULL, ws_thread, run));
	for (i = 0; i < TEST_STEPS && !atomic_load(&run->done); i++)
		fyai_event_loop_step(s->el, 50);
	FYAI_TCHECK(atomic_load(&run->done));
	FYAI_TCHECK(!pthread_join(th, NULL));
}

static void test_websocket(void)
{
	struct server s;
	struct client_run run;
	char *why;

	srv_start(&s);
	run_client(&s, &run, "/ws");
	FYAI_TCHECK(!run.rc);
	FYAI_TCHECK(!strcmp(run.reply, "echo:hello"));
	srv_wait_closed(&s, 1);

	/* A path that does not upgrade is an answer, and the client says why. */
	run_client(&s, &run, "/nowhere");
	FYAI_TCHECK(run.rc);
	why = fyai_diag_string(&test_cfg.diag);
	FYAI_TCHECK(why && strstr(why, "did not upgrade"));
	free(why);
	srv_wait_closed(&s, 2);
	srv_stop(&s);
	fyai_diag_reset(&test_cfg.diag);
}

int net_url_parse(void)
{
	return httpd_run(test_url_parse);
}

int httpd_parse(void)
{
	return httpd_run(test_parse);
}

int wsc_echo(void)
{
	return httpd_run(test_wsc_echo);
}

int wsc_failures(void)
{
	return httpd_run(test_wsc_failures);
}

int wsc_tls(void)
{
	return httpd_run(test_wsc_tls);
}

int httpd_headers(void)
{
	return httpd_run(test_headers);
}

int httpd_body(void)
{
	return httpd_run(test_body);
}

int httpd_body_limits(void)
{
	return httpd_run(test_body_limits);
}

int httpd_expect_continue(void)
{
	return httpd_run(test_expect_continue);
}

int httpd_close_and_http10(void)
{
	return httpd_run(test_close_and_http10);
}

int httpd_respond(void)
{
	return httpd_run(test_respond);
}

int httpd_long_head(void)
{
	return httpd_run(test_long_head);
}

int httpd_not_http(void)
{
	return httpd_run(test_not_http);
}

int httpd_websocket(void)
{
	return httpd_run(test_websocket);
}
