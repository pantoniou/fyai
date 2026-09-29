/*
 * fyai_transport_server_test.c - tests for the transport request engine
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis@konsulko.com>
 * SPDX-License-Identifier: MIT
 *
 * The server runs in this process on the event loop. The agent is the other
 * end of a socketpair in the same process, and the provider is a forked
 * loopback HTTP server. Every wait steps the loop with a deadline; nothing
 * sleeps.
 */

#define FYAI_MODULE FYAIEM_UNKNOWN

#include <errno.h>
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

#include "fyai.h"
#include "fyai_curl.h"
#include "fyai_event.h"
#include "fyai_test.h"
#include "fyai_test_scratch.h"
#include "fyai_redact.h"
#include "fyai_transport_server.h"
#include "utils.h"

#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(transport_server, headers_build, transport_server_headers_build)
FYAI_TEST_ENTRY(transport_server, ratelimit_codec, transport_server_ratelimit_codec)
FYAI_TEST_ENTRY(transport_server, records_ratelimit, transport_server_records_ratelimit)
FYAI_TEST_ENTRY(transport_server, logs_without_credentials, transport_server_logs_without_credentials)
FYAI_TEST_ENTRY(transport_server, enforces_grant, transport_server_enforces_grant)
FYAI_TEST_ENTRY(transport_server, narrows_model, transport_server_narrows_model)
FYAI_TEST_ENTRY(transport_server, injects_credential, transport_server_injects_credential)
FYAI_TEST_ENTRY(transport_server, redacts_resolved_credential, transport_server_redacts_resolved_credential)
FYAI_TEST_ENTRY(transport_server, reload_profiles, transport_server_reload_profiles)
FYAI_TEST_ENTRY(transport_server, streams_response, transport_server_streams_response)
FYAI_TEST_ENTRY(transport_server, no_credential_for_no_auth, transport_server_no_credential_for_no_auth)
FYAI_TEST_ENTRY(transport_server, credit_bounds_stream, transport_server_credit_bounds_stream)
FYAI_TEST_ENTRY(transport_server, cancel_request, transport_server_cancel_request)
FYAI_TEST_ENTRY(transport_server, channel_close_cancels, transport_server_channel_close_cancels)
FYAI_TEST_ENTRY(transport_server, connect_failure_is_transient, transport_server_connect_failure_is_transient)
FYAI_TEST_ENTRY(transport_server, refuses_bad_requests, transport_server_refuses_bad_requests)
FYAI_TEST_ENTRY(transport_server, ignores_copied_descriptor, transport_server_ignores_copied_descriptor)

#define BOUND_MS	10000
#define BIG_BODY	(1024 * 1024)

static struct fyai_cfg test_cfg;
static struct fyai_ctx test_ctx = { .cfg = &test_cfg };

/* Mock provider. */

enum mock_mode {
	MOCK_SMALL,		/* 200 with "hello" */
	MOCK_BIG,		/* 200 with BIG_BODY bytes */
	MOCK_STALL,		/* headers and one byte, then wait */
	MOCK_LIMITED,		/* 429 with rate-limit headers and a body */
};

struct mock {
	pid_t pid;
	int port;
	int report;		/* request head and body, read after the run */
};

/* Serve one connection. With @many the report stays open for the next one. */
static void mock_serve(int listener, enum mock_mode mode, int report, bool many)
{
	char req[65536], hdr[512];
	size_t got = 0, need = 0;
	static char chunk[16384];
	ssize_t n;
	int c;

	c = accept(listener, NULL, NULL);
	if (c < 0)
		_exit(1);
	while (got < sizeof(req) - 1) {
		char *end, *cl;

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
	case MOCK_SMALL:
		snprintf(hdr, sizeof(hdr),
			 "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
			 "Retry-After: 7\r\nX-RateLimit-Remaining-Requests: 99\r\n"
			 "Anthropic-Ratelimit-Requests-Limit: 50\r\nX-Other: 1\r\n"
			 "Content-Length: 5\r\n\r\nhello");
		if (write(c, hdr, strlen(hdr)) < 0)
			_exit(4);
		break;
	case MOCK_BIG: {
		size_t left = BIG_BODY;

		snprintf(hdr, sizeof(hdr),
			 "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n", BIG_BODY);
		if (write(c, hdr, strlen(hdr)) < 0)
			_exit(4);
		while (left) {
			size_t k = left > sizeof(chunk) ? sizeof(chunk) : left;

			n = write(c, chunk, k);
			if (n <= 0)
				_exit(0);	/* the transport went away */
			left -= n;
		}
		break;
	}
	case MOCK_LIMITED:
		snprintf(hdr, sizeof(hdr),
			 "HTTP/1.1 429 Too Many Requests\r\nRetry-After: 3\r\n"
			 "X-RateLimit-Remaining-Requests: 0\r\n"
			 "RateLimit-Reset: 12\r\nServer: mock\r\n"
			 "Content-Length: 2\r\n\r\n{}");
		if (write(c, hdr, strlen(hdr)) < 0)
			_exit(4);
		break;
	case MOCK_STALL:
		snprintf(hdr, sizeof(hdr),
			 "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nx");
		if (write(c, hdr, strlen(hdr)) < 0)
			_exit(4);
		pause();
		break;
	}
	close(c);
}

static void mock_child(int listener, enum mock_mode mode, int report, bool many)
{
	do
		mock_serve(listener, mode, report, many);
	while (many);
	_exit(0);
}

static void mock_start_opt(struct mock *m, enum mock_mode mode, bool many)
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
		mock_child(l, mode, rp[1], many);
	}
	close(l);
	close(rp[1]);
	m->report = rp[0];
}

static void mock_start(struct mock *m, enum mock_mode mode)
{
	mock_start_opt(m, mode, false);
}

/* A provider that serves any number of requests until it is stopped. */
static void mock_start_many(struct mock *m, enum mock_mode mode)
{
	mock_start_opt(m, mode, true);
}

static void mock_stop(struct mock *m)
{
	kill(m->pid, SIGKILL);
	waitpid(m->pid, NULL, 0);
	close(m->report);
}

/* What the mock saw, as one string. */
static size_t mock_report(struct mock *m, char *buf, size_t size)
{
	size_t got = 0;
	ssize_t n;

	while (got < size - 1 && (n = read(m->report, buf + got, size - 1 - got)) > 0)
		got += n;
	buf[got] = '\0';
	return got;
}

/* Return the bytes the mock reported so far, without waiting for more. */
static size_t mock_report_nonblock(struct mock *m, char *buf, size_t size)
{
	ssize_t n;

	fcntl(m->report, F_SETFL, fcntl(m->report, F_GETFL) | O_NONBLOCK);
	n = read(m->report, buf, size);
	return n > 0 ? n : 0;
}

/* Agent side. */

struct frame {
	uint16_t kind;
	uint64_t seq;
	size_t len;
	uint8_t data[1024];	/* copy of the start of the payload */
};

struct agent {
	int fd;
	struct fyai_transport_server *srv;
	struct fyai_transport_registry *reg;
	struct frame frames[512];
	size_t nframes;
	size_t body_bytes;	/* bytes of body frames */
	unsigned int rejected;	/* messages the server logged as rejected */
	unsigned int retired;
	uint64_t next_seq;	/* the sequence the next frame must carry */
	bool seq_ok;
};

static void log_cb(void *ud, uint64_t id, const char *event, const char *detail)
{
	struct agent *a = ud;

	(void)id;
	(void)detail;
	if (!strcmp(event, "rejected"))
		a->rejected++;
	else if (!strcmp(event, "retired"))
		a->retired++;
}

static int cred_cb(void *ud, const struct fyai_transport_profile *pr, char **secret)
{
	(void)ud;
	(void)pr;	/* every credential source resolves to the same value */
	*secret = strdup("sekret");
	return *secret ? 0 : -ENOMEM;
}

/* Profiles for the mock provider at @port; @tag names the profile. */
static void profiles_set(struct fyai_transport_server *srv, int port,
			 const char *tag, bool with_model)
{
	struct fyai_transport_grant grant = { 0 };
	char url[64];
	int rc;

	snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1/x", port);
	rc = fyai_transport_grant_add(&grant, "main", url, tag,
				      with_model ? "m1" : NULL, FYAI_TA_NONE,
				      NULL, NULL);
	FYAI_TCHECK(!rc);
	rc = fyai_transport_grant_add_header(&grant, "main", "X-Version", "2");
	FYAI_TCHECK(!rc);
	rc = fyai_transport_grant_add(&grant, "side", url, "side", NULL,
				      FYAI_TA_NONE, NULL, NULL);
	FYAI_TCHECK(!rc);
	rc = fyai_transport_server_set_profiles(srv, &grant);
	FYAI_TCHECK(!rc);
}

static const struct fyai_transport_allow main_only[] = { { "main", NULL } };

static void agent_open(struct agent *a, int port, bool with_model)
{
	int sv[2], rc;

	memset(a, 0, sizeof(*a));
	a->seq_ok = true;
	FYAI_TCHECK(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
	FYAI_TCHECK(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv));
	a->fd = sv[0];
	a->reg = fyai_transport_registry_create(FYAI_TL_B, NULL);
	FYAI_TCHECK(a->reg);
	a->srv = fyai_transport_server_create(&test_ctx, a->reg, cred_cb, NULL,
					      log_cb, a);
	FYAI_TCHECK(a->srv);
	profiles_set(a->srv, port, "chat", with_model);
	rc = fyai_transport_server_admit(a->srv, 7, 0, getpid(), getuid(), sv[1],
					 main_only, 1, NULL);
	FYAI_TCHECK(!rc);
}

static void agent_close(struct agent *a)
{
	fyai_transport_server_destroy(a->srv);
	fyai_transport_registry_destroy(a->reg);
	close(a->fd);
	fyai_curl_cleanup(&test_ctx);
	if (test_ctx.el) {
		fyai_event_loop_destroy(test_ctx.el);
		test_ctx.el = NULL;
	}
	fyai_event_pool_drain(&test_ctx);
	curl_global_cleanup();
}

/* Move every queued frame from the socket into @a. */
static void agent_drain(struct agent *a)
{
	uint8_t buf[FYAI_TRANSPORT_MAX_FRAME];
	struct fyai_transport_hdr h;
	struct frame *f;
	ssize_t n;

	while ((n = recv(a->fd, buf, sizeof(buf), MSG_DONTWAIT)) > 0) {
		FYAI_TCHECK(!fyai_transport_hdr_decode(buf, n, &h));
		FYAI_TCHECK(h.exec_id == 7 && h.request_id == 1);
		FYAI_TCHECK(a->nframes < sizeof(a->frames) / sizeof(a->frames[0]));
		if (h.seq != a->next_seq)
			a->seq_ok = false;
		a->next_seq = h.seq + 1;
		f = &a->frames[a->nframes++];
		f->kind = h.kind;
		f->seq = h.seq;
		f->len = h.len;
		/* Keep the start of the payload as a string. */
		memcpy(f->data, buf + FYAI_TRANSPORT_HDR_SIZE,
		       h.len < sizeof(f->data) - 1 ? h.len : sizeof(f->data) - 1);
		f->data[h.len < sizeof(f->data) - 1 ? h.len : sizeof(f->data) - 1] = '\0';
		if (h.kind == FYAI_TK_RESP_BODY)
			a->body_bytes += h.len;
	}
}

static bool agent_has(const struct agent *a, uint16_t kind)
{
	size_t i;

	for (i = 0; i < a->nframes; i++)
		if (a->frames[i].kind == kind)
			return true;
	return false;
}

static const struct frame *agent_frame(const struct agent *a, uint16_t kind)
{
	size_t i;

	for (i = 0; i < a->nframes; i++)
		if (a->frames[i].kind == kind)
			return &a->frames[i];
	return NULL;
}

static bool agent_terminal(const struct agent *a)
{
	return agent_has(a, FYAI_TK_RESP_END) || agent_has(a, FYAI_TK_RESP_ERROR);
}

/* Step the loop until @pred holds or the deadline passes. */
static bool agent_wait(struct agent *a, bool (*pred)(const struct agent *))
{
	fyai_event_ms_t end = fyai_event_now_ms() + BOUND_MS;
	struct fyai_event_loop *el = fyai_ctx_loop(&test_ctx);

	agent_drain(a);
	while (!pred(a)) {
		if (fyai_event_now_ms() > end)
			return false;
		FYAI_TCHECK(fyai_event_loop_step(el, 20) >= 0);
		agent_drain(a);
	}
	return true;
}

static void send_request_to(struct agent *a, const char *profile,
			    const char *method, const char *content_type,
			    const char *body)
{
	struct fyai_transport_request rq = {
		.profile = profile, .method = method,
		.content_type = content_type, .accept = "text/event-stream",
		.body = (const uint8_t *)body, .body_len = strlen(body),
	};
	struct fy_generic_builder_cfg cfg = { .flags = FYGBCF_SCOPE_LEADER };
	struct fy_generic_builder *gb = fy_generic_builder_create(&cfg);
	uint8_t *payload;
	size_t len;
	int rc;

	FYAI_TCHECK(gb);
	rc = fyai_transport_request_encode(gb, &rq, &payload, &len);
	FYAI_TCHECK(!rc);
	rc = fyai_transport_send_message(a->fd, FYAI_TK_REQUEST, 7, 1, payload, len);
	FYAI_TCHECK(!rc);
	free(payload);
	fy_generic_builder_destroy(gb);
}

static void send_request(struct agent *a, const char *method,
			 const char *content_type, const char *body)
{
	send_request_to(a, "main", method, content_type, body);
}

static void send_raw_request(struct agent *a, const char *json, const char *body)
{
	size_t hl = strlen(json), bl = strlen(body), i;
	uint8_t *p = malloc(4 + hl + bl);

	FYAI_TCHECK(p);
	for (i = 0; i < 4; i++)
		p[i] = (hl >> (8 * i)) & 0xff;
	memcpy(p + 4, json, hl);
	memcpy(p + 4 + hl, body, bl);
	FYAI_TCHECK(!fyai_transport_send_message(a->fd, FYAI_TK_REQUEST, 7, 1,
						 p, 4 + hl + bl));
	free(p);
}

static void send_credit(struct agent *a, uint32_t n)
{
	uint8_t b[4] = { n, n >> 8, n >> 16, n >> 24 };
	struct fyai_transport_hdr h = {
		.kind = FYAI_TK_CREDIT, .exec_id = 7, .request_id = 1, .len = 4,
	};

	FYAI_TCHECK(!fyai_transport_send_frame(a->fd, &h, b));
}

static bool has_terminal(const struct agent *a)
{
	return agent_terminal(a);
}

static bool has_body(const struct agent *a)
{
	return agent_has(a, FYAI_TK_RESP_BODY);
}

static bool has_body_credit_limit(const struct agent *a)
{
	return a->body_bytes >= FYAI_TRANSPORT_INITIAL_CREDIT - 16384;
}

static long error_code(const struct frame *f)
{
	struct fy_generic_builder_cfg cfg = { .flags = FYGBCF_SCOPE_LEADER };
	struct fy_generic_builder *gb = fy_generic_builder_create(&cfg);
	struct fyai_transport_error er;

	FYAI_TCHECK(gb);
	FYAI_TCHECK(!fyai_transport_error_parse(gb, f->data, f->len, &er));
	fy_generic_builder_destroy(gb);
	return er.code;
}

/* Tests. */

int transport_server_headers_build(void)
{
	struct fyai_transport_grant g = { 0 };
	struct fyai_transport_request rq = {
		.profile = "p", .content_type = "application/json",
		.accept = "text/event-stream",
	};
	struct curl_slist *l, *it;
	bool bearer = false, key = false, ver = false, ct = false, expect = false;

	FYAI_TCHECK(!fyai_transport_grant_add(&g, "b", "https://a.example/", NULL,
					      NULL, FYAI_TA_BEARER, NULL, "env:K"));
	FYAI_TCHECK(!fyai_transport_grant_add(&g, "h", "https://a.example/", NULL,
					      NULL, FYAI_TA_HEADER, "x-api-key", "env:K"));
	FYAI_TCHECK(!fyai_transport_grant_add_header(&g, "h", "anthropic-version", "2023-06-01"));

	FYAI_TCHECK(!fyai_transport_headers_build(fyai_transport_grant_find(&g, "b"),
						  &rq, "sekret", &l));
	for (it = l; it; it = it->next) {
		bearer |= !strcmp(it->data, "Authorization: Bearer sekret");
		ct |= !strcmp(it->data, "Content-Type: application/json");
		expect |= !strcmp(it->data, "Expect:");
	}
	FYAI_TCHECK(bearer && ct && expect);
	curl_slist_free_all(l);

	FYAI_TCHECK(!fyai_transport_headers_build(fyai_transport_grant_find(&g, "h"),
						  &rq, "sekret", &l));
	for (it = l; it; it = it->next) {
		key |= !strcmp(it->data, "x-api-key: sekret");
		ver |= !strcmp(it->data, "anthropic-version: 2023-06-01");
	}
	FYAI_TCHECK(key && ver);
	curl_slist_free_all(l);

	/* A credential cannot carry a second header line. */
	FYAI_TCHECK(fyai_transport_headers_build(fyai_transport_grant_find(&g, "b"),
						 &rq, "a\r\nX: y", &l) == -EINVAL);
	FYAI_TCHECK(fyai_transport_headers_build(fyai_transport_grant_find(&g, "b"),
						 &rq, "", &l) == -EINVAL);
	FYAI_TCHECK(!l);

	/* A fixed header cannot replace the authentication or repeat itself. */
	FYAI_TCHECK(fyai_transport_grant_add_header(&g, "h", "X-Api-Key", "z") == -EINVAL);
	FYAI_TCHECK(fyai_transport_grant_add_header(&g, "h", "Authorization", "z") == -EINVAL);
	FYAI_TCHECK(fyai_transport_grant_add_header(&g, "h", "anthropic-version", "2") == -EINVAL);
	FYAI_TCHECK(fyai_transport_grant_add_header(&g, "h", "X-A", "a\nb") == -EINVAL);
	FYAI_TCHECK(fyai_transport_grant_add_header(&g, "nope", "X-A", "a") == -ENOENT);
	fyai_transport_grant_clear(&g);
	return 0;
}

int transport_server_streams_response(void)
{
	struct mock m;
	struct agent a;
	const struct frame *st;
	struct fy_generic_builder_cfg cfg = { .flags = FYGBCF_SCOPE_LEADER };
	struct fy_generic_builder *gb;
	struct fyai_transport_response rs;
	char seen[8192];

	mock_start(&m, MOCK_SMALL);
	agent_open(&a, m.port, true);
	send_request(&a, "POST", "application/json", "{\"model\":\"m1\",\"x\":1}");
	FYAI_TCHECK(agent_wait(&a, has_terminal));

	FYAI_TCHECK(a.nframes == 3 && a.seq_ok);
	FYAI_TCHECK(a.frames[0].kind == FYAI_TK_RESP_START);
	FYAI_TCHECK(a.frames[1].kind == FYAI_TK_RESP_BODY && a.frames[1].len == 5);
	FYAI_TCHECK(!memcmp(a.frames[1].data, "hello", 5));
	FYAI_TCHECK(a.frames[2].kind == FYAI_TK_RESP_END);
	st = agent_frame(&a, FYAI_TK_RESP_START);
	gb = fy_generic_builder_create(&cfg);
	FYAI_TCHECK(gb);
	FYAI_TCHECK(!fyai_transport_response_parse(gb, st->data, st->len, &rs));
	FYAI_TCHECK(rs.status == 200 && rs.retry_after_s == 7);
	FYAI_TCHECK(!strcmp(rs.content_type, "text/plain"));
	FYAI_TCHECK(!strcmp(rs.tag, "chat") && !strcmp(rs.model, "m1"));
	/* The two rate-limit headers are reported, the other is not. */
	FYAI_TCHECK(rs.rate_limit.count == 2);
	FYAI_TCHECK(!strcmp(rs.rate_limit.item[0].name, "x-ratelimit-remaining-requests"));
	FYAI_TCHECK(!strcmp(rs.rate_limit.item[0].value, "99"));
	FYAI_TCHECK(!strcmp(rs.rate_limit.item[1].name, "anthropic-ratelimit-requests-limit"));
	fy_generic_builder_destroy(gb);
	FYAI_TCHECK(!fyai_transport_server_active(a.srv));

	/* The provider saw the fixed header, the body, and the type. */
	mock_report(&m, seen, sizeof(seen));
	FYAI_TCHECK(strstr(seen, "POST /v1/x HTTP/1.1"));
	FYAI_TCHECK(strcasestr(seen, "X-Version: 2"));
	FYAI_TCHECK(strcasestr(seen, "Content-Type: application/json"));
	FYAI_TCHECK(strcasestr(seen, "Accept: text/event-stream"));
	FYAI_TCHECK(!strcasestr(seen, "Expect:"));
	FYAI_TCHECK(strstr(seen, "{\"model\":\"m1\",\"x\":1}"));
	agent_close(&a);
	mock_stop(&m);
	return 0;
}

/* A profile without authentication sends no credential, whatever the agent does. */
int transport_server_no_credential_for_no_auth(void)
{
	struct mock m;
	struct agent a;
	char seen[8192];

	mock_start(&m, MOCK_SMALL);
	agent_open(&a, m.port, false);
	send_request(&a, "POST", "application/json", "{}");
	FYAI_TCHECK(agent_wait(&a, has_terminal));
	FYAI_TCHECK(agent_has(&a, FYAI_TK_RESP_END));
	mock_report(&m, seen, sizeof(seen));
	FYAI_TCHECK(!strcasestr(seen, "authorization"));
	FYAI_TCHECK(!strstr(seen, "sekret"));
	FYAI_TCHECK(!strcasestr(seen, "proxy"));
	agent_close(&a);
	mock_stop(&m);
	return 0;
}

/* Without credit the transfer pauses; credit resumes it, in order and whole. */
int transport_server_credit_bounds_stream(void)
{
	struct mock m;
	struct agent a;

	mock_start(&m, MOCK_BIG);
	agent_open(&a, m.port, false);
	send_request(&a, "POST", "application/json", "{}");
	FYAI_TCHECK(agent_wait(&a, has_body_credit_limit));

	/* The window is full: the server must not send more than the credit. */
	{
		fyai_event_ms_t end = fyai_event_now_ms() + 300;
		struct fyai_event_loop *el = fyai_ctx_loop(&test_ctx);

		while (fyai_event_now_ms() < end) {
			FYAI_TCHECK(fyai_event_loop_step(el, 20) >= 0);
			agent_drain(&a);
		}
	}
	FYAI_TCHECK(a.body_bytes <= FYAI_TRANSPORT_INITIAL_CREDIT);
	FYAI_TCHECK(!agent_terminal(&a));

	send_credit(&a, BIG_BODY);
	FYAI_TCHECK(agent_wait(&a, has_terminal));
	FYAI_TCHECK(agent_has(&a, FYAI_TK_RESP_END));
	FYAI_TCHECK(a.body_bytes == BIG_BODY && a.seq_ok);
	agent_close(&a);
	mock_stop(&m);
	return 0;
}

int transport_server_cancel_request(void)
{
	struct mock m;
	struct agent a;
	struct fyai_transport_hdr h = {
		.kind = FYAI_TK_CANCEL, .exec_id = 7, .request_id = 1,
	};

	mock_start(&m, MOCK_STALL);
	agent_open(&a, m.port, false);
	send_request(&a, "POST", "application/json", "{}");
	FYAI_TCHECK(agent_wait(&a, has_body));
	FYAI_TCHECK(fyai_transport_server_active(a.srv) == 1);
	FYAI_TCHECK(!fyai_transport_send_frame(a.fd, &h, NULL));
	FYAI_TCHECK(agent_wait(&a, has_terminal));
	FYAI_TCHECK(agent_frame(&a, FYAI_TK_RESP_ERROR));
	FYAI_TCHECK(error_code(agent_frame(&a, FYAI_TK_RESP_ERROR)) == -2);
	FYAI_TCHECK(!fyai_transport_server_active(a.srv));
	agent_close(&a);
	mock_stop(&m);
	return 0;
}

static bool transfer_gone(const struct agent *a)
{
	return !fyai_transport_server_active(a->srv);
}

/* An agent that goes away takes its transfers with it. */
int transport_server_channel_close_cancels(void)
{
	struct mock m;
	struct agent a;

	mock_start(&m, MOCK_STALL);
	agent_open(&a, m.port, false);
	send_request(&a, "POST", "application/json", "{}");
	FYAI_TCHECK(agent_wait(&a, has_body));
	FYAI_TCHECK(fyai_transport_server_active(a.srv) == 1);
	close(a.fd);
	a.fd = -1;
	FYAI_TCHECK(agent_wait(&a, transfer_gone));
	FYAI_TCHECK(a.retired == 1);
	fyai_transport_server_destroy(a.srv);
	fyai_transport_registry_destroy(a.reg);
	fyai_curl_cleanup(&test_ctx);
	fyai_event_loop_destroy(test_ctx.el);
	test_ctx.el = NULL;
	fyai_event_pool_drain(&test_ctx);
	curl_global_cleanup();
	mock_stop(&m);
	return 0;
}

int transport_server_connect_failure_is_transient(void)
{
	struct mock m;
	struct agent a;
	struct fy_generic_builder_cfg cfg = { .flags = FYGBCF_SCOPE_LEADER };
	struct fy_generic_builder *gb;
	struct fyai_transport_error er;
	const struct frame *f;

	/* Nothing listens after the mock exits. */
	mock_start(&m, MOCK_SMALL);
	mock_stop(&m);
	agent_open(&a, m.port, false);
	send_request(&a, "POST", "application/json", "{}");
	FYAI_TCHECK(agent_wait(&a, has_terminal));
	f = agent_frame(&a, FYAI_TK_RESP_ERROR);
	FYAI_TCHECK(f);
	gb = fy_generic_builder_create(&cfg);
	FYAI_TCHECK(gb);
	FYAI_TCHECK(!fyai_transport_error_parse(gb, f->data, f->len, &er));
	FYAI_TCHECK(er.code == CURLE_COULDNT_CONNECT && er.transient);
	fy_generic_builder_destroy(gb);
	agent_close(&a);
	m.report = open("/dev/null", O_RDONLY);
	return 0;
}

/* A request that names anything but a profile is refused, and nothing is sent. */
int transport_server_refuses_bad_requests(void)
{
	struct mock m;
	struct agent a;
	char seen[64];
	static const char *const bad[] = {
		"{\"profile\":\"main\",\"url\":\"http://evil.example/\"}",
		"{\"profile\":\"main\",\"headers\":{\"Authorization\":\"x\"}}",
		"{\"profile\":\"main\",\"proxy\":\"http://p/\"}",
		"{\"profile\":\"main\",\"method\":\"DELETE\"}",
		"{\"profile\":\"main\",\"content_type\":\"text/x\\r\\nX: y\"}",
		"{\"profile\":\"other\"}",
		"{\"method\":\"POST\"}",
		"[1]",
		"not json",
	};
	unsigned int i;

	mock_start(&m, MOCK_SMALL);
	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		agent_open(&a, m.port, false);
		send_raw_request(&a, bad[i], "{}");
		FYAI_TCHECK(agent_wait(&a, has_terminal));
		FYAI_TCHECK(agent_has(&a, FYAI_TK_RESP_ERROR));
		FYAI_TCHECK(!agent_has(&a, FYAI_TK_RESP_START));
		FYAI_TCHECK(error_code(agent_frame(&a, FYAI_TK_RESP_ERROR)) == -1);
		FYAI_TCHECK(!fyai_transport_server_active(a.srv));
		agent_close(&a);
	}

	/* A model other than the granted one is refused. */
	agent_open(&a, m.port, true);
	send_request(&a, "POST", "application/json", "{\"model\":\"m2\"}");
	FYAI_TCHECK(agent_wait(&a, has_terminal));
	FYAI_TCHECK(agent_has(&a, FYAI_TK_RESP_ERROR));
	agent_close(&a);
	agent_open(&a, m.port, true);
	send_request(&a, "POST", "application/json", "{\"nomodel\":1}");
	FYAI_TCHECK(agent_wait(&a, has_terminal));
	FYAI_TCHECK(agent_has(&a, FYAI_TK_RESP_ERROR));
	agent_close(&a);

	/* The provider was never contacted. */
	FYAI_TCHECK(!mock_report_nonblock(&m, seen, sizeof(seen)));
	mock_stop(&m);
	return 0;
}

/* A process with a copy of the descriptor cannot get a request served. */
int transport_server_ignores_copied_descriptor(void)
{
	struct mock m;
	struct agent a;
	struct fyai_transport_request rq = {
		.profile = "main", .content_type = "application/json",
		.body = (const uint8_t *)"{}", .body_len = 2,
	};
	struct fy_generic_builder_cfg cfg = { .flags = FYGBCF_SCOPE_LEADER };
	struct fy_generic_builder *gb;
	struct fyai_event_loop *el;
	fyai_event_ms_t end;
	uint8_t *p;
	size_t n;
	pid_t child;
	int st;

	mock_start(&m, MOCK_SMALL);
	agent_open(&a, m.port, false);
	child = fork();
	FYAI_TCHECK(child >= 0);
	if (!child) {
		gb = fy_generic_builder_create(&cfg);

		if (!gb || fyai_transport_request_encode(gb, &rq, &p, &n))
			_exit(1);
		_exit(fyai_transport_send_message(a.fd, FYAI_TK_REQUEST, 7, 1,
						  p, n) ? 2 : 0);
	}
	FYAI_TCHECK(waitpid(child, &st, 0) == child && WIFEXITED(st) && !WEXITSTATUS(st));

	end = fyai_event_now_ms() + BOUND_MS;
	el = fyai_ctx_loop(&test_ctx);
	while (!a.rejected && fyai_event_now_ms() < end)
		FYAI_TCHECK(fyai_event_loop_step(el, 20) >= 0);
	FYAI_TCHECK(a.rejected == 1);
	agent_drain(&a);
	FYAI_TCHECK(!a.nframes);
	FYAI_TCHECK(!fyai_transport_server_active(a.srv));

	/* The registered agent is still served afterwards. */
	send_request(&a, "POST", "application/json", "{}");
	FYAI_TCHECK(agent_wait(&a, has_terminal));
	FYAI_TCHECK(agent_has(&a, FYAI_TK_RESP_END));
	agent_close(&a);
	mock_stop(&m);
	return 0;
}

#define RL_ADD(rl, lit) fyai_transport_ratelimit_add((rl), (lit), strlen(lit))

int transport_server_ratelimit_codec(void)
{
	struct fyai_transport_ratelimit rl = { 0 };
	struct fyai_transport_response in = { .status = 200, .retry_after_s = -1 }, out;
	struct fy_generic_builder_cfg cfg = { .flags = FYGBCF_SCOPE_LEADER };
	struct fy_generic_builder *gb = fy_generic_builder_create(&cfg);
	const char *json;
	char line[512];
	unsigned int i;

	FYAI_TCHECK(gb);
	FYAI_TCHECK(fyai_transport_ratelimit_header("X-RateLimit-Limit", strlen("X-RateLimit-Limit")));
	FYAI_TCHECK(fyai_transport_ratelimit_header("anthropic-ratelimit-tokens-reset", strlen("anthropic-ratelimit-tokens-reset")));
	FYAI_TCHECK(fyai_transport_ratelimit_header("RateLimit-Policy", strlen("RateLimit-Policy")));
	FYAI_TCHECK(!fyai_transport_ratelimit_header("Retry-After", strlen("Retry-After")));
	FYAI_TCHECK(!fyai_transport_ratelimit_header("X-RateLimit-", strlen("X-RateLimit-")));
	FYAI_TCHECK(!fyai_transport_ratelimit_header("X-Other", strlen("X-Other")));

	/* Names go to lower case; values are trimmed and otherwise unchanged. */
	FYAI_TCHECK(RL_ADD(&rl, "X-RateLimit-Reset:  1s \r\n"));
	FYAI_TCHECK(!strcmp(rl.item[0].name, "x-ratelimit-reset"));
	FYAI_TCHECK(!strcmp(rl.item[0].value, "1s"));
	FYAI_TCHECK(!RL_ADD(&rl, "X-Other: 1\r\n"));
	FYAI_TCHECK(!RL_ADD(&rl, "no colon here"));

	/* An over-long value is dropped, not cut. */
	memset(line, 'v', sizeof(line));
	memcpy(line, "X-RateLimit-Big: ", 17);
	FYAI_TCHECK(!fyai_transport_ratelimit_add(&rl, line, sizeof(line)));
	FYAI_TCHECK(rl.count == 1);

	/* A full table drops the rest. */
	for (i = 1; i < FYAI_TRANSPORT_RL_MAX; i++)
		FYAI_TCHECK(RL_ADD(&rl, "X-RateLimit-N: 1"));
	FYAI_TCHECK(!RL_ADD(&rl, "X-RateLimit-N: 1"));
	FYAI_TCHECK(rl.count == FYAI_TRANSPORT_RL_MAX);

	in.rate_limit = rl;
	json = fyai_transport_response_encode(gb, &in);
	FYAI_TCHECK(json);
	FYAI_TCHECK(!fyai_transport_response_parse(gb, (const uint8_t *)json, strlen(json), &out));
	/* A repeated name is one key in the mapping. */
	FYAI_TCHECK(out.rate_limit.count == 2);
	FYAI_TCHECK(!strcmp(out.rate_limit.item[0].name, "x-ratelimit-reset"));
	fy_generic_builder_destroy(gb);
	return 0;
}

/* The transport records a limit and a 429, and does nothing else about them. */
int transport_server_records_ratelimit(void)
{
	struct fyai_transport_ratelimit_record rec;
	struct fy_generic_builder_cfg cfg = { .flags = FYGBCF_SCOPE_LEADER };
	struct fy_generic_builder *gb;
	struct fyai_transport_response rs;
	char url[64], seen[8192];
	struct mock m;
	struct agent a;

	mock_start(&m, MOCK_LIMITED);
	agent_open(&a, m.port, false);
	snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1/x", m.port);
	FYAI_TCHECK(!fyai_transport_server_ratelimit(a.srv, url, &rec));
	send_request(&a, "POST", "application/json", "{}");
	FYAI_TCHECK(agent_wait(&a, has_terminal));

	/* The 429 is the response: no retry, no error, and the body arrives. */
	FYAI_TCHECK(a.nframes == 3 && a.seq_ok);
	FYAI_TCHECK(a.frames[0].kind == FYAI_TK_RESP_START);
	FYAI_TCHECK(a.frames[1].kind == FYAI_TK_RESP_BODY);
	FYAI_TCHECK(a.frames[2].kind == FYAI_TK_RESP_END);
	gb = fy_generic_builder_create(&cfg);
	FYAI_TCHECK(gb);
	FYAI_TCHECK(!fyai_transport_response_parse(gb, a.frames[0].data,
						   a.frames[0].len, &rs));
	FYAI_TCHECK(rs.status == 429 && rs.retry_after_s == 3);
	FYAI_TCHECK(rs.rate_limit.count == 2);
	fy_generic_builder_destroy(gb);

	FYAI_TCHECK(fyai_transport_server_ratelimit(a.srv, url, &rec));
	FYAI_TCHECK(rec.status == 429 && rec.retry_after_s == 3);
	FYAI_TCHECK(rec.rl.count == 2);
	FYAI_TCHECK(!strcmp(rec.rl.item[0].name, "x-ratelimit-remaining-requests"));
	FYAI_TCHECK(!strcmp(rec.rl.item[1].name, "ratelimit-reset"));
	FYAI_TCHECK(rec.when_ms > 0);
	FYAI_TCHECK(!fyai_transport_server_ratelimit(a.srv, "http://other/", &rec));

	/* The provider saw one request only. */
	mock_report(&m, seen, sizeof(seen));
	FYAI_TCHECK(strstr(seen, "POST /v1/x"));
	agent_close(&a);
	mock_stop(&m);
	return 0;
}

static char *slurp(const char *path)
{
	FILE *fp = fopen(path, "r");
	char *buf;
	long n;

	if (!fp)
		return NULL;
	FYAI_TCHECK(!fseek(fp, 0, SEEK_END));
	n = ftell(fp);
	FYAI_TCHECK(n >= 0 && !fseek(fp, 0, SEEK_SET));
	buf = malloc(n + 1);
	FYAI_TCHECK(buf);
	FYAI_TCHECK(fread(buf, 1, n, fp) == (size_t)n);
	buf[n] = '\0';
	fclose(fp);
	return buf;
}

/* The transport log records events and traffic, and never a credential. */
int transport_server_logs_without_credentials(void)
{
	struct fy_generic_builder_cfg gbcfg = {
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED,
	};
	const char *dir = fyai_test_scratch_dir("transport-log");
	char arena[512], path[512], *log, seen[8192];
	struct mock m;
	struct agent a;

	snprintf(arena, sizeof(arena), "%s/arena", dir);
	snprintf(path, sizeof(path), "%s/logs/transport.yaml", dir);
	test_cfg.gb = fy_generic_builder_create(&gbcfg);
	FYAI_TCHECK(test_cfg.gb);
	test_cfg.arena_dir = arena;

	/* Off by default: nothing is written. */
	mock_start(&m, MOCK_SMALL);
	agent_open(&a, m.port, false);
	send_request(&a, "POST", "application/json", "{}");
	FYAI_TCHECK(agent_wait(&a, has_terminal));
	agent_close(&a);
	mock_stop(&m);
	FYAI_TCHECK(!(log = slurp(path)));

	test_cfg.transport_logging = true;
	mock_start(&m, MOCK_SMALL);
	agent_open(&a, m.port, false);
	send_request(&a, "POST", "application/json",
		     "{\"note\":\"Bearer sekrettoken123\"}");
	FYAI_TCHECK(agent_wait(&a, has_terminal));
	mock_report(&m, seen, sizeof(seen));
	agent_close(&a);
	mock_stop(&m);
	test_cfg.transport_logging = false;

	/* The provider got the body; the log has everything but the token. */
	FYAI_TCHECK(strstr(seen, "sekrettoken123"));
	log = slurp(path);
	FYAI_TCHECK(log);
	FYAI_TCHECK(strstr(log, "event: admitted"));
	FYAI_TCHECK(strstr(log, "event: request"));
	FYAI_TCHECK(strstr(log, "profile main method POST"));
	FYAI_TCHECK(strstr(log, "event: response"));
	FYAI_TCHECK(strstr(log, "status 200"));
	FYAI_TCHECK(strstr(log, "event: end"));
	FYAI_TCHECK(strstr(log, "kind: transport-wire"));
	FYAI_TCHECK(strstr(log, "type: header_out"));
	FYAI_TCHECK(strstr(log, "POST /v1/x"));
	FYAI_TCHECK(strstr(log, "type: data_in"));
	FYAI_TCHECK(strstr(log, FYAI_REDACTED));
	FYAI_TCHECK(!strstr(log, "sekrettoken123"));
	free(log);

	fy_generic_builder_destroy(test_cfg.gb);
	test_cfg.gb = NULL;
	test_cfg.arena_dir = NULL;
	return 0;
}

static void agent_reset(struct agent *a)
{
	a->nframes = 0;
	a->body_bytes = 0;
	a->next_seq = 0;
	a->seq_ok = true;
}

/*
 * A configuration or catalogue change replaces the profiles. A request in
 * flight keeps its snapshot, a later request sees the new endpoint, and a
 * profile that the new set lacks is refused.
 */
int transport_server_reload_profiles(void)
{
	struct fyai_transport_grant other = { 0 };
	struct fyai_transport_hdr cancel = {
		.kind = FYAI_TK_CANCEL, .exec_id = 7, .request_id = 1,
	};
	struct fy_generic_builder_cfg cfg = { .flags = FYGBCF_SCOPE_LEADER };
	struct fy_generic_builder *gb;
	struct fyai_transport_response rs;
	struct mock m1, m2;
	struct agent a;
	char seen[8192];

	mock_start(&m1, MOCK_STALL);
	mock_start(&m2, MOCK_SMALL);
	agent_open(&a, m1.port, false);
	send_request(&a, "POST", "application/json", "{}");
	FYAI_TCHECK(agent_wait(&a, has_body));
	FYAI_TCHECK(fyai_transport_server_active(a.srv) == 1);

	profiles_set(a.srv, m2.port, "second", false);
	FYAI_TCHECK(fyai_transport_server_active(a.srv) == 1);
	FYAI_TCHECK(!fyai_transport_send_frame(a.fd, &cancel, NULL));
	FYAI_TCHECK(agent_wait(&a, has_terminal));
	FYAI_TCHECK(error_code(agent_frame(&a, FYAI_TK_RESP_ERROR)) == -2);
	FYAI_TCHECK(!fyai_transport_server_active(a.srv));

	/* The next request goes to the new endpoint. */
	agent_reset(&a);
	send_request(&a, "POST", "application/json", "{}");
	FYAI_TCHECK(agent_wait(&a, has_terminal));
	FYAI_TCHECK(agent_has(&a, FYAI_TK_RESP_END));
	gb = fy_generic_builder_create(&cfg);
	FYAI_TCHECK(gb);
	FYAI_TCHECK(!fyai_transport_response_parse(gb, a.frames[0].data,
						   a.frames[0].len, &rs));
	FYAI_TCHECK(!strcmp(rs.tag, "second"));
	fy_generic_builder_destroy(gb);
	mock_report(&m2, seen, sizeof(seen));
	FYAI_TCHECK(strstr(seen, "POST /v1/x"));

	/* A set without the profile refuses it. */
	FYAI_TCHECK(!fyai_transport_grant_add(&other, "elsewhere",
					      "https://a.example/", NULL, NULL,
					      FYAI_TA_NONE, NULL, NULL));
	FYAI_TCHECK(!fyai_transport_server_set_profiles(a.srv, &other));
	agent_reset(&a);
	send_request(&a, "POST", "application/json", "{}");
	FYAI_TCHECK(agent_wait(&a, has_terminal));
	FYAI_TCHECK(error_code(agent_frame(&a, FYAI_TK_RESP_ERROR)) == -1);
	agent_close(&a);
	mock_stop(&m1);
	mock_stop(&m2);
	return 0;
}

static void agent_reset(struct agent *a);

/* One request; return true when it was served, false when it was refused. */
static bool served(struct agent *a, const char *profile, const char *body)
{
	bool ok;

	agent_reset(a);
	send_request_to(a, profile, "POST", "application/json", body);
	FYAI_TCHECK(agent_wait(a, has_terminal));
	ok = agent_has(a, FYAI_TK_RESP_END);
	FYAI_TCHECK(ok || agent_has(a, FYAI_TK_RESP_ERROR));
	return ok;
}

/*
 * The profile set has two profiles, and the execution is granted one. It can
 * name only that one, whatever the set holds; a grant change applies to the
 * next request; an empty grant reaches nothing.
 */
int transport_server_enforces_grant(void)
{
	static const struct fyai_transport_allow both[] = {
		{ "main", NULL }, { "side", NULL },
	};
	struct mock m;
	struct agent a;

	mock_start_many(&m, MOCK_SMALL);
	agent_open(&a, m.port, false);
	FYAI_TCHECK(served(&a, "main", "{}"));
	FYAI_TCHECK(!served(&a, "side", "{}"));
	FYAI_TCHECK(error_code(agent_frame(&a, FYAI_TK_RESP_ERROR)) == -1);

	FYAI_TCHECK(!fyai_transport_server_set_grant(a.srv, 7, both, 2));
	FYAI_TCHECK(served(&a, "side", "{}"));
	FYAI_TCHECK(served(&a, "main", "{}"));

	FYAI_TCHECK(!fyai_transport_server_set_grant(a.srv, 7, NULL, 0));
	FYAI_TCHECK(!served(&a, "main", "{}"));
	FYAI_TCHECK(!served(&a, "side", "{}"));
	agent_close(&a);
	mock_stop(&m);
	return 0;
}

/* A grant can narrow the model of a profile; it cannot widen it. */
int transport_server_narrows_model(void)
{
	static const struct fyai_transport_allow narrow[] = { { "main", "m1" } };
	static const struct fyai_transport_allow widen[] = { { "main", "m2" } };
	struct mock m;
	struct agent a;

	/* The profile takes any model; the grant allows m1. */
	mock_start_many(&m, MOCK_SMALL);
	agent_open(&a, m.port, false);
	FYAI_TCHECK(!fyai_transport_server_set_grant(a.srv, 7, narrow, 1));
	FYAI_TCHECK(!served(&a, "main", "{\"model\":\"m2\"}"));
	FYAI_TCHECK(!served(&a, "main", "{\"other\":1}"));
	FYAI_TCHECK(served(&a, "main", "{\"model\":\"m1\"}"));
	agent_close(&a);
	mock_stop(&m);

	/* The profile is pinned to m1; a grant for m2 does not widen it. */
	mock_start_many(&m, MOCK_SMALL);
	agent_open(&a, m.port, true);
	FYAI_TCHECK(served(&a, "main", "{\"model\":\"m1\"}"));
	FYAI_TCHECK(!fyai_transport_server_set_grant(a.srv, 7, widen, 1));
	FYAI_TCHECK(!served(&a, "main", "{\"model\":\"m2\"}"));
	FYAI_TCHECK(!served(&a, "main", "{\"model\":\"m1\"}"));
	agent_close(&a);
	mock_stop(&m);
	return 0;
}

/* Authenticated profiles for the mock at @port: a bearer one and a header one. */
static void profiles_set_auth(struct fyai_transport_server *srv, int port)
{
	struct fyai_transport_grant grant = { 0 };
	char url[64];

	snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1/x", port);
	FYAI_TCHECK(!fyai_transport_grant_add(&grant, "main", url, NULL, NULL,
					      FYAI_TA_BEARER, NULL, "mem:key"));
	FYAI_TCHECK(!fyai_transport_grant_add(&grant, "side", url, NULL, NULL,
					      FYAI_TA_HEADER, "X-Api-Key", "mem:key"));
	FYAI_TCHECK(!fyai_transport_server_set_profiles(srv, &grant));
}

static const struct fyai_transport_allow both_profiles[] = {
	{ "main", NULL }, { "side", NULL },
};

/* The transport adds the credential; the request cannot carry one. */
int transport_server_injects_credential(void)
{
	struct mock m;
	struct agent a;
	char seen[8192];

	mock_start_many(&m, MOCK_SMALL);
	agent_open(&a, m.port, false);
	profiles_set_auth(a.srv, m.port);
	FYAI_TCHECK(!fyai_transport_server_set_grant(a.srv, 7, both_profiles, 2));

	FYAI_TCHECK(served(&a, "main", "{}"));
	mock_report_nonblock(&m, seen, sizeof(seen));
	FYAI_TCHECK(strcasestr(seen, "Authorization: Bearer sekret\r\n"));

	FYAI_TCHECK(served(&a, "side", "{}"));
	mock_report_nonblock(&m, seen, sizeof(seen));
	FYAI_TCHECK(strcasestr(seen, "X-Api-Key: sekret\r\n"));
	FYAI_TCHECK(!strcasestr(seen, "Authorization:"));
	agent_close(&a);
	mock_stop(&m);
	return 0;
}

/* The log has the traffic, and nothing of the key that the provider received. */
int transport_server_redacts_resolved_credential(void)
{
	struct fy_generic_builder_cfg gbcfg = {
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED,
	};
	const char *dir = fyai_test_scratch_dir("transport-cred-log");
	char arena[512], path[512], *log, seen[8192];
	struct mock m;
	struct agent a;

	snprintf(arena, sizeof(arena), "%s/arena", dir);
	snprintf(path, sizeof(path), "%s/logs/transport.yaml", dir);
	test_cfg.gb = fy_generic_builder_create(&gbcfg);
	FYAI_TCHECK(test_cfg.gb);
	test_cfg.arena_dir = arena;
	test_cfg.transport_logging = true;

	mock_start_many(&m, MOCK_SMALL);
	agent_open(&a, m.port, false);
	profiles_set_auth(a.srv, m.port);
	FYAI_TCHECK(!fyai_transport_server_set_grant(a.srv, 7, both_profiles, 2));
	/* The provider echoes what it was sent; the mock echoes nothing, so the
	 * request body carries the key the way an error reply would. */
	FYAI_TCHECK(served(&a, "main", "{\"echo\":\"sekret\"}"));
	FYAI_TCHECK(served(&a, "side", "{}"));
	mock_report_nonblock(&m, seen, sizeof(seen));
	FYAI_TCHECK(strcasestr(seen, "sekret"));
	agent_close(&a);
	mock_stop(&m);
	test_cfg.transport_logging = false;

	log = slurp(path);
	FYAI_TCHECK(log);
	FYAI_TCHECK(strstr(log, "type: header_out"));
	FYAI_TCHECK(strstr(log, FYAI_REDACTED));
	FYAI_TCHECK(!strstr(log, "sekret"));
	free(log);
	fy_generic_builder_destroy(test_cfg.gb);
	test_cfg.gb = NULL;
	test_cfg.arena_dir = NULL;
	return 0;
}
