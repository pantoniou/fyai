/*
 * fyai_transport_client_test.c - tests for the agent side of the transport
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis@konsulko.com>
 * SPDX-License-Identifier: MIT
 *
 * The transport server and the client run in this process on one event loop,
 * joined by a socketpair, and the provider is a forked loopback server.
 */

#define FYAI_MODULE FYAIEM_UNKNOWN

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "fyai.h"
#include "fyai_curl.h"
#include "fyai_event.h"
#include "fyai_test.h"
#include "fyai_transport_cfg.h"
#include "fyai_transport_client.h"
#include "fyai_transport_server.h"
#include "fyai_xfer.h"

#include "fyai_test_registry.h"
#include "fyai_transport_mock.h"

FYAI_TEST_ENTRY(transport_client, call_streams, transport_client_call_streams)
FYAI_TEST_ENTRY(transport_client, credit_returns, transport_client_credit_returns)
FYAI_TEST_ENTRY(transport_client, cancel_call, transport_client_cancel_call)
FYAI_TEST_ENTRY(transport_client, refusal_is_reported, transport_client_refusal_is_reported)
FYAI_TEST_ENTRY(transport_client, failure_is_transient, transport_client_failure_is_transient)
FYAI_TEST_ENTRY(transport_client, body_callback_aborts, transport_client_body_callback_aborts)
FYAI_TEST_ENTRY(transport_client, lost_transport, transport_client_lost_transport)
FYAI_TEST_ENTRY(transport_client, concurrent_calls, transport_client_concurrent_calls)
FYAI_TEST_ENTRY(transport_client, xfer_backend, transport_client_xfer_backend)
FYAI_TEST_ENTRY(transport_client, xfer_maps_failures, transport_client_xfer_maps_failures)

#define BOUND_MS tmock_bound_ms()

static struct fyai_cfg test_cfg;
static struct fyai_ctx test_ctx = { .cfg = &test_cfg };

struct rig {
	char name[FYAI_TPC_NAME_MAX];	/* the profile of the configuration */
	struct fyai_transport_registry *reg;
	struct fyai_transport_server *srv;
	struct fyai_tclient *client;
};

static void rig_open(struct rig *r, int port)
{
	struct fyai_transport_grant grant = { 0 };
	struct fyai_transport_allow allow[1];
	char url[64];
	int sv[2], rc;

	FYAI_TCHECK(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
	FYAI_TCHECK(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv));
	r->reg = fyai_transport_registry_create(FYAI_TL_B, NULL);
	FYAI_TCHECK(r->reg);
	r->srv = fyai_transport_server_create(&test_ctx, r->reg, NULL, NULL,
					      NULL, NULL);
	FYAI_TCHECK(r->srv);
	snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1/x", port);
	/* The agent names its profile from its configuration. */
	test_cfg.api_url = strdup(url);
	test_cfg.api_mode = FYAI_API_CHAT_COMPLETIONS;
	test_cfg.no_auth = true;
	FYAI_TCHECK(fyai_transport_profile_name(&test_cfg, FYAI_TPC_MODEL, r->name,
						sizeof(r->name)));
	FYAI_TCHECK(!fyai_transport_grant_add(&grant, r->name, url, "chat", NULL,
					      FYAI_TA_NONE, NULL, NULL));
	allow[0].profile = r->name;
	allow[0].model = NULL;
	FYAI_TCHECK(!fyai_transport_server_set_profiles(r->srv, &grant));
	rc = fyai_transport_server_admit(r->srv, 7, 0, getpid(), getuid(), sv[1],
					 allow, 1, NULL);
	FYAI_TCHECK(!rc);
	r->client = fyai_tclient_open(&test_ctx, sv[0], 7);
	FYAI_TCHECK(r->client);
}

static void rig_close(struct rig *r)
{
	test_ctx.tclient = NULL;
	fyai_tclient_close(r->client);
	free((char *)test_cfg.api_url);
	test_cfg.api_url = NULL;
	fyai_transport_server_destroy(r->srv);
	fyai_transport_registry_destroy(r->reg);
	fyai_curl_cleanup(&test_ctx);
	if (test_ctx.el) {
		fyai_event_loop_destroy(test_ctx.el);
		test_ctx.el = NULL;
	}
	fyai_event_pool_drain(&test_ctx);
	curl_global_cleanup();
}

/* What one call saw. */
struct seen {
	bool ended;
	struct fyai_tcall_result result;
	struct fyai_tcall_start start;
	bool started;
	size_t bytes;
	char head[16];
	size_t stop_after;		/* refuse the body after this many bytes; 0: never */
	bool got_body;
};

static void on_start(struct fyai_tcall *call, const struct fyai_tcall_start *st,
		     void *ud)
{
	struct seen *s = ud;

	(void)call;
	s->started = true;
	s->start = *st;
}

static size_t on_body(const void *data, size_t len, void *ud)
{
	struct seen *s = ud;

	if (s->bytes < sizeof(s->head))
		memcpy(s->head + s->bytes, data,
		       len < sizeof(s->head) - s->bytes ? len : sizeof(s->head) - s->bytes);
	s->bytes += len;
	s->got_body = true;
	if (s->stop_after && s->bytes >= s->stop_after)
		return 0;
	return len;
}

static void on_end(struct fyai_tcall *call, const struct fyai_tcall_result *r,
		   void *ud)
{
	struct seen *s = ud;

	(void)call;
	s->ended = true;
	s->result = *r;
}

static struct fyai_tcall *call_start(struct rig *r, struct seen *s,
				     const char *profile)
{
	struct fyai_transport_request rq = {
		.profile = profile, .method = "POST",
		.content_type = "application/json",
		.body = (const uint8_t *)"{}", .body_len = 2,
	};
	struct fyai_tcall_callbacks cb = {
		.start = on_start, .body = on_body, .end = on_end, .userdata = s,
	};
	struct fyai_tcall *call = fyai_tclient_submit(r->client, &rq, &cb);

	FYAI_TCHECK(call);
	return call;
}

/* Step the loop until @pred holds, or the deadline. */
static bool wait_for(bool (*pred)(void *), void *ud)
{
	fyai_event_ms_t end = fyai_event_now_ms() + BOUND_MS;
	struct fyai_event_loop *el = fyai_ctx_loop(&test_ctx);

	while (!pred(ud)) {
		if (fyai_event_now_ms() > end)
			return false;
		FYAI_TCHECK(fyai_event_loop_step(el, 20) >= 0);
	}
	return true;
}

static bool seen_ended(void *ud)
{
	return ((struct seen *)ud)->ended;
}

static bool seen_body(void *ud)
{
	return ((struct seen *)ud)->got_body;
}

int transport_client_call_streams(void)
{
	struct tmock m;
	struct rig r;
	struct seen s = { 0 };
	struct fyai_tcall *call;
	struct fyai_transport_ratelimit rl;

	tmock_start(&m, TMOCK_SMALL);
	rig_open(&r, m.port);
	FYAI_TCHECK(!fyai_tclient_ratelimit(r.client, &rl));
	call = call_start(&r, &s, r.name);
	FYAI_TCHECK(wait_for(seen_ended, &s));
	FYAI_TCHECK(s.result.end == FYAI_TC_OK);
	FYAI_TCHECK(s.started && s.start.status == 200 && s.start.retry_after_s == 7);
	FYAI_TCHECK(s.bytes == 5 && !memcmp(s.head, "hello", 5));

	/* The rate-limit report is recorded for the caller and acts on nothing. */
	FYAI_TCHECK(fyai_tclient_ratelimit(r.client, &rl) && rl.count == 2);
	FYAI_TCHECK(s.start.rate_limit.count == 2);
	fyai_tcall_destroy(call);
	rig_close(&r);
	tmock_stop(&m);
	return 0;
}

/* A response far larger than the credit window arrives whole. */
int transport_client_credit_returns(void)
{
	struct tmock m;
	struct rig r;
	struct seen s = { 0 };
	struct fyai_tcall *call;

	tmock_start(&m, TMOCK_BIG);
	rig_open(&r, m.port);
	call = call_start(&r, &s, r.name);
	FYAI_TCHECK(wait_for(seen_ended, &s));
	FYAI_TCHECK(s.result.end == FYAI_TC_OK && s.bytes == BIG_BODY);
	fyai_tcall_destroy(call);
	rig_close(&r);
	tmock_stop(&m);
	return 0;
}

int transport_client_cancel_call(void)
{
	struct tmock m;
	struct rig r;
	struct seen s = { 0 };
	struct fyai_tcall *call;

	tmock_start(&m, TMOCK_STALL);
	rig_open(&r, m.port);
	call = call_start(&r, &s, r.name);
	FYAI_TCHECK(wait_for(seen_body, &s));
	fyai_tcall_cancel(call);
	fyai_tcall_cancel(call);	/* a second cancel sends nothing more */
	FYAI_TCHECK(wait_for(seen_ended, &s));
	FYAI_TCHECK(s.result.end == FYAI_TC_CANCELLED);
	FYAI_TCHECK(!fyai_transport_server_active(r.srv));
	fyai_tcall_destroy(call);
	rig_close(&r);
	tmock_stop(&m);
	return 0;
}

int transport_client_refusal_is_reported(void)
{
	struct tmock m;
	struct rig r;
	struct seen s = { 0 };
	struct fyai_tcall *call;

	tmock_start(&m, TMOCK_SMALL);
	rig_open(&r, m.port);
	call = call_start(&r, &s, "other");
	FYAI_TCHECK(wait_for(seen_ended, &s));
	FYAI_TCHECK(s.result.end == FYAI_TC_REFUSED && !s.started);
	FYAI_TCHECK(strstr(s.result.message, "not granted"));
	fyai_tcall_destroy(call);
	rig_close(&r);
	tmock_stop(&m);
	return 0;
}

int transport_client_failure_is_transient(void)
{
	struct tmock m;
	struct rig r;
	struct seen s = { 0 };
	struct fyai_tcall *call;

	tmock_start(&m, TMOCK_SMALL);
	tmock_stop(&m);		/* nothing listens on the port now */
	rig_open(&r, m.port);
	call = call_start(&r, &s, r.name);
	FYAI_TCHECK(wait_for(seen_ended, &s));
	FYAI_TCHECK(s.result.end == FYAI_TC_FAILED);
	FYAI_TCHECK(s.result.code == CURLE_COULDNT_CONNECT && s.result.transient);
	fyai_tcall_destroy(call);
	rig_close(&r);
	return 0;
}

/* A body callback that refuses the data ends the call, and the server's
 * transfer with it. */
int transport_client_body_callback_aborts(void)
{
	struct tmock m;
	struct rig r;
	struct seen s = { .stop_after = 1 };
	struct fyai_tcall *call;
	struct fyai_event_loop *el;
	fyai_event_ms_t end;

	tmock_start(&m, TMOCK_BIG);
	rig_open(&r, m.port);
	call = call_start(&r, &s, r.name);
	FYAI_TCHECK(wait_for(seen_ended, &s));
	FYAI_TCHECK(s.result.end == FYAI_TC_ABORTED && s.started);
	fyai_tcall_destroy(call);
	/* The server saw the cancel and dropped the transfer. */
	end = fyai_event_now_ms() + BOUND_MS;
	el = fyai_ctx_loop(&test_ctx);
	while (fyai_transport_server_active(r.srv) && fyai_event_now_ms() < end)
		FYAI_TCHECK(fyai_event_loop_step(el, 20) >= 0);
	FYAI_TCHECK(!fyai_transport_server_active(r.srv));
	rig_close(&r);
	tmock_stop(&m);
	return 0;
}

/* A transport that goes away ends every call, and no retry follows. */
int transport_client_lost_transport(void)
{
	struct tmock m;
	struct rig r;
	struct seen s = { 0 }, s2 = { 0 };
	struct fyai_tcall *call, *call2;
	struct fyai_transport_request rq = {
		.profile = r.name, .method = "POST", .body = (const uint8_t *)"{}",
		.body_len = 2, .content_type = "application/json",
	};
	struct fyai_tcall_callbacks cb = {
		.body = on_body, .end = on_end, .userdata = &s2,
	};

	tmock_start(&m, TMOCK_STALL);
	rig_open(&r, m.port);
	call = call_start(&r, &s, r.name);
	FYAI_TCHECK(wait_for(seen_body, &s));
	/* Retire the execution: the server closes its end of the channel. */
	FYAI_TCHECK(!fyai_transport_server_retire(r.srv, 7));
	FYAI_TCHECK(wait_for(seen_ended, &s));
	FYAI_TCHECK(s.result.end == FYAI_TC_LOST);
	FYAI_TCHECK(!s.result.transient);
	fyai_tcall_destroy(call);

	/* A new call has nowhere to go. */
	call2 = fyai_tclient_submit(r.client, &rq, &cb);
	FYAI_TCHECK(!call2);
	rig_close(&r);
	tmock_stop(&m);
	return 0;
}

struct pair {
	struct seen a, b;
};

static bool pair_ended(void *ud)
{
	struct pair *p = ud;

	return p->a.ended && p->b.ended;
}

/* Two calls at once do not mix their bytes. */
int transport_client_concurrent_calls(void)
{
	struct tmock m;
	struct rig r;
	struct pair p;
	struct fyai_tcall *ca, *cb;

	memset(&p, 0, sizeof(p));
	tmock_start_many(&m, TMOCK_SMALL);
	rig_open(&r, m.port);
	ca = call_start(&r, &p.a, r.name);
	cb = call_start(&r, &p.b, r.name);
	FYAI_TCHECK(wait_for(pair_ended, &p));
	FYAI_TCHECK(p.a.result.end == FYAI_TC_OK && p.b.result.end == FYAI_TC_OK);
	FYAI_TCHECK(p.a.bytes == 5 && p.b.bytes == 5);
	FYAI_TCHECK(!memcmp(p.a.head, "hello", 5) && !memcmp(p.b.head, "hello", 5));
	fyai_tcall_destroy(ca);
	fyai_tcall_destroy(cb);
	rig_close(&r);
	tmock_stop(&m);
	return 0;
}

/* The transfer interface over the transport, as the stream code uses it. */

struct xf {
	struct fyai_xfer *x;
	bool done;
	size_t bytes;
	size_t refuse_after;
	char head[8];
	CURLcode result;
	long status, retry_after;
	char error[64];
};

static size_t xf_write(void *ptr, size_t size, size_t nmemb, void *ud)
{
	struct xf *f = ud;
	size_t n = size * nmemb;

	if (f->bytes < sizeof(f->head))
		memcpy(f->head + f->bytes, ptr,
		       n < sizeof(f->head) - f->bytes ? n : sizeof(f->head) - f->bytes);
	f->bytes += n;
	return f->refuse_after && f->bytes >= f->refuse_after ? 0 : n;
}

static void xf_done(struct fyai_xfer *x, void *ud)
{
	struct xf *f = ud;

	f->result = fyai_xfer_result(x);
	f->status = fyai_xfer_last_status(&test_ctx);
	f->retry_after = fyai_xfer_last_retry_after(&test_ctx);
	snprintf(f->error, sizeof(f->error), "%s", fyai_xfer_last_error(&test_ctx));
	fyai_xfer_destroy(x);
	f->x = NULL;
	f->done = true;
}

static bool xf_finished(void *ud)
{
	return ((struct xf *)ud)->done;
}

static void xf_run(struct rig *r, struct xf *f, const char *profile)
{
	test_ctx.tclient = r->client;
	fyai_xfer_set_endpoint(&test_ctx, NULL, profile);
	fyai_xfer_set_body(&test_ctx, "{}");
	f->x = fyai_xfer_submit(&test_ctx, xf_write, f, xf_done, f);
	FYAI_TCHECK(f->x);
	/* The completion comes from the loop, not from the submit. */
	FYAI_TCHECK(!f->done);
}

int transport_client_xfer_backend(void)
{
	struct tmock m;
	struct rig r;
	struct xf f = { 0 };

	tmock_start(&m, TMOCK_SMALL);
	rig_open(&r, m.port);
	xf_run(&r, &f, "model");
	FYAI_TCHECK(wait_for(xf_finished, &f));
	FYAI_TCHECK(f.result == CURLE_OK && f.status == 200 && f.retry_after == 7);
	FYAI_TCHECK(f.bytes == 5 && !memcmp(f.head, "hello", 5));
	rig_close(&r);
	tmock_stop(&m);
	return 0;
}

/* The failures come back as the curl results that the stream code tests. */
int transport_client_xfer_maps_failures(void)
{
	struct tmock m;
	struct rig r;
	struct xf f = { 0 };
	struct fyai_event_loop *el;
	fyai_event_ms_t end;

	/* A refusal is not transient and carries the transport's reason. */
	tmock_start(&m, TMOCK_SMALL);
	rig_open(&r, m.port);
	FYAI_TCHECK(!fyai_transport_server_set_grant(r.srv, 7, NULL, 0));
	xf_run(&r, &f, "model");
	FYAI_TCHECK(wait_for(xf_finished, &f));
	FYAI_TCHECK(f.result == CURLE_BAD_FUNCTION_ARGUMENT && !f.status);
	FYAI_TCHECK(strstr(f.error, "not granted"));
	FYAI_TCHECK(!fyai_http_transient(f.result, f.status));
	rig_close(&r);
	tmock_stop(&m);

	/* A body callback that stops the transfer is a write error, with the
	 * status that arrived. */
	memset(&f, 0, sizeof(f));
	f.refuse_after = 1;
	tmock_start(&m, TMOCK_BIG);
	rig_open(&r, m.port);
	xf_run(&r, &f, "model");
	FYAI_TCHECK(wait_for(xf_finished, &f));
	FYAI_TCHECK(f.result == CURLE_WRITE_ERROR && f.status == 200);
	rig_close(&r);
	tmock_stop(&m);

	/* A cancel is an abort by callback. */
	memset(&f, 0, sizeof(f));
	tmock_start(&m, TMOCK_STALL);
	rig_open(&r, m.port);
	xf_run(&r, &f, "model");
	end = fyai_event_now_ms() + BOUND_MS;
	el = fyai_ctx_loop(&test_ctx);
	while (!f.bytes && fyai_event_now_ms() < end)
		FYAI_TCHECK(fyai_event_loop_step(el, 20) >= 0);
	FYAI_TCHECK(f.bytes);
	fyai_xfer_cancel(f.x);
	FYAI_TCHECK(wait_for(xf_finished, &f));
	FYAI_TCHECK(f.result == CURLE_ABORTED_BY_CALLBACK);
	rig_close(&r);
	tmock_stop(&m);

	/* A lost transport is not transient either: nothing retries it. */
	memset(&f, 0, sizeof(f));
	tmock_start(&m, TMOCK_STALL);
	rig_open(&r, m.port);
	xf_run(&r, &f, "model");
	end = fyai_event_now_ms() + BOUND_MS;
	el = fyai_ctx_loop(&test_ctx);
	while (!f.bytes && fyai_event_now_ms() < end)
		FYAI_TCHECK(fyai_event_loop_step(el, 20) >= 0);
	FYAI_TCHECK(!fyai_transport_server_retire(r.srv, 7));
	FYAI_TCHECK(wait_for(xf_finished, &f));
	FYAI_TCHECK(f.result == CURLE_FAILED_INIT);
	FYAI_TCHECK(!fyai_http_transient(f.result, f.status));
	rig_close(&r);
	tmock_stop(&m);
	return 0;
}
