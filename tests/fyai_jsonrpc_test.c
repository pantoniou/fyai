/*
 * fyai_jsonrpc_test.c - unit tests for the async JSON-RPC transport
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 *
 * These drive the stdio transport end to end over a pipe pair on one event
 * loop: the client half is the module under test, the "server" half is the
 * test writing framed responses. The HTTP transport is exercised by the
 * functional MCP cases against the mock provider.
 */

#define FYAI_MODULE FYAIEM_UNKNOWN

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fyai.h"
#include "fyai_event.h"
#include "fyai_jsonrpc.h"
#include "fyai_test.h"

#define TEST_BOUND_MS 5000

static struct fyai_cfg test_cfg;
static struct fyai_ctx test_ctx = { .cfg = &test_cfg };

/* The transport references the logger; these tests run with logging off, so a
 * stub keeps the UI/log objects out of the link. */
int fyai_log_generic(struct fyai_ctx *ctx, const char *name, fy_generic doc)
{
	(void)ctx;
	(void)name;
	(void)doc;
	return 0;
}

/* The transport reaches its scratch builder through this accessor; the test
 * sets test_ctx.transient_gb up front, so just hand it back. */
struct fy_generic_builder *fyai_ctx_transient_gb(struct fyai_ctx *ctx)
{
	return ctx->transient_gb;
}

struct call_result {
	volatile bool done;
	bool ok;
	fy_generic result;
};

static void on_complete(struct jsonrpc_request *req, void *userdata)
{
	struct call_result *cr = userdata;

	FYAI_TCHECK(jsonrpc_request_done(req));
	cr->ok = jsonrpc_request_ok(req);
	cr->result = jsonrpc_request_result(req);
	cr->done = true;
}

/* A client connection whose peer is two pipes the test drives directly. */
struct peer {
	struct jsonrpc_conn *conn;
	int to_client;		/* test writes responses here (client stdout) */
	int from_client;	/* test reads requests here (client stdin) */
};

static void peer_open(struct peer *p)
{
	int reqpipe[2], resppipe[2];

	FYAI_TCHECK(!pipe(reqpipe));
	FYAI_TCHECK(!pipe(resppipe));
	/* client writes requests to reqpipe[1], reads responses from resppipe[0]. */
	p->from_client = reqpipe[0];
	p->to_client = resppipe[1];
	p->conn = jsonrpc_conn_stdio(&test_ctx, reqpipe[1], resppipe[0], 1,
				     "peer", NULL);
	FYAI_TCHECK(p->conn);
}

static void peer_close(struct peer *p)
{
	jsonrpc_conn_destroy(p->conn);
	close(p->from_client);
	close(p->to_client);
}

static void peer_send(struct peer *p, const char *line)
{
	size_t len = strlen(line);

	FYAI_TCHECK(write(p->to_client, line, len) == (ssize_t)len);
	FYAI_TCHECK(write(p->to_client, "\n", 1) == 1);
}

static void setup_builders(void)
{
	struct fy_auto_allocator_cfg acfg;
	struct fy_generic_builder_cfg gbc;

	memset(&acfg, 0, sizeof(acfg));
	acfg.scenario = FYAST_PER_TAG_FREE_DEDUP;
	test_ctx.gb = fy_generic_builder_create(&(struct fy_generic_builder_cfg){
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED |
			 FYGBCF_CREATE_TAG,
		.allocator = fy_allocator_create("auto", &acfg),
	});
	FYAI_TCHECK(test_ctx.gb);

	memset(&acfg, 0, sizeof(acfg));
	acfg.scenario = FYAST_PER_TAG_FREE_DEDUP;
	test_ctx.transient_allocator = fy_allocator_create("auto", &acfg);
	memset(&gbc, 0, sizeof(gbc));
	gbc.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED | FYGBCF_CREATE_TAG;
	gbc.allocator = test_ctx.transient_allocator;
	gbc.parent = test_ctx.gb;
	test_ctx.transient_gb = fy_generic_builder_create(&gbc);
	FYAI_TCHECK(test_ctx.transient_gb);
}

static struct fyai_event_loop *loop(void)
{
	struct fyai_event_loop *el = fyai_ctx_loop(&test_ctx);

	FYAI_TCHECK(el);
	return el;
}

static void test_request_response(void)
{
	struct call_result cr = {};
	struct jsonrpc_request *req;
	struct peer p;

	peer_open(&p);
	req = jsonrpc_request_submit(p.conn, "ping", fy_map_empty, 1, false,
				    on_complete, &cr);
	FYAI_TCHECK(req);
	peer_send(&p, "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"value\":42}}");
	FYAI_TCHECK(!fyai_event_loop_run_until(loop(), &cr.done, TEST_BOUND_MS));

	FYAI_TCHECK(cr.ok);
	FYAI_TCHECK(fy_get(cr.result, "value", 0LL) == 42);
	jsonrpc_request_destroy(req);
	peer_close(&p);
	printf("ok - request completes with its matching response\n");
}

static void test_ignores_other_ids(void)
{
	struct call_result cr = {};
	struct jsonrpc_request *req;
	struct peer p;

	/* A notification (no id) and a mismatched-id response must be skipped;
	 * completion waits for the response whose id matches the request. */
	peer_open(&p);
	req = jsonrpc_request_submit(p.conn, "ping", fy_map_empty, 7, false,
				    on_complete, &cr);
	FYAI_TCHECK(req);
	peer_send(&p, "{\"jsonrpc\":\"2.0\",\"method\":\"note\",\"params\":{}}");
	peer_send(&p, "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"value\":1}}");
	peer_send(&p, "{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{\"value\":9}}");
	FYAI_TCHECK(!fyai_event_loop_run_until(loop(), &cr.done, TEST_BOUND_MS));

	FYAI_TCHECK(cr.ok);
	FYAI_TCHECK(fy_get(cr.result, "value", 0LL) == 9);
	jsonrpc_request_destroy(req);
	peer_close(&p);
	printf("ok - mismatched ids and notifications are ignored\n");
}

static void test_error_response(void)
{
	struct call_result cr = {};
	struct jsonrpc_request *req;
	struct peer p;

	peer_open(&p);
	req = jsonrpc_request_submit(p.conn, "ping", fy_map_empty, 1, false,
				    on_complete, &cr);
	FYAI_TCHECK(req);
	peer_send(&p, "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":"
		     "{\"code\":-1,\"message\":\"nope\"}}");
	FYAI_TCHECK(!fyai_event_loop_run_until(loop(), &cr.done, TEST_BOUND_MS));

	FYAI_TCHECK(!cr.ok);
	jsonrpc_request_destroy(req);
	peer_close(&p);
	printf("ok - an error response fails the request\n");
}

static void test_notification(void)
{
	struct call_result cr = {};
	struct jsonrpc_request *req;
	struct peer p;

	/* A notification completes once written; no response is expected. */
	peer_open(&p);
	req = jsonrpc_request_submit(p.conn, "note", fy_map_empty, 0, true,
				    on_complete, &cr);
	FYAI_TCHECK(req);
	FYAI_TCHECK(!fyai_event_loop_run_until(loop(), &cr.done, TEST_BOUND_MS));

	FYAI_TCHECK(cr.ok);
	jsonrpc_request_destroy(req);
	peer_close(&p);
	printf("ok - a notification completes on write\n");
}

static void test_cancel(void)
{
	struct call_result cr = {};
	struct jsonrpc_request *req;
	struct peer p;

	peer_open(&p);
	req = jsonrpc_request_submit(p.conn, "ping", fy_map_empty, 1, false,
				    on_complete, &cr);
	FYAI_TCHECK(req);
	/* No response ever arrives; cancellation finishes it immediately. */
	jsonrpc_request_cancel(req);
	FYAI_TCHECK(jsonrpc_request_done(req));
	FYAI_TCHECK(cr.done);
	FYAI_TCHECK(!cr.ok);
	jsonrpc_request_destroy(req);
	peer_close(&p);
	printf("ok - a pending request can be cancelled\n");
}


/* Record requests and notifications that the connection serves. */
struct served {
	volatile int requests;
	volatile int notifications;
	char method[64];
	long long arg;
	bool fail_next;
};

static fy_generic on_serve(struct jsonrpc_conn *conn, const char *method,
			   fy_generic params, fy_generic id, void *userdata,
			   fy_generic *errorp)
{
	struct served *sv = userdata;

	(void)conn;
	snprintf(sv->method, sizeof(sv->method), "%s", method ? method : "");
	sv->arg = fy_get(params, "n", 0LL);
	if (!fy_generic_is_valid(id)) {
		sv->notifications++;
		return fy_invalid;
	}
	sv->requests++;
	if (sv->fail_next) {
		*errorp = fy_mapping(test_ctx.transient_gb,
				     "code", -32000LL, "message", "refused");
		return fy_invalid;
	}
	return fy_mapping(test_ctx.transient_gb, "echo", sv->arg);
}

/* Read one framed line the client wrote back to us. */
static char *peer_recv(struct peer *p, char *buf, size_t cap)
{
	size_t used = 0;
	ssize_t n;

	while (used + 1 < cap) {
		n = read(p->from_client, buf + used, 1);
		if (n <= 0)
			break;
		if (buf[used] == '\n')
			break;
		used++;
	}
	buf[used] = '\0';
	return buf;
}

static void test_serve_request(void)
{
	struct served sv = {};
	struct peer p;
	char line[512];

	peer_open(&p);
	FYAI_TCHECK(!jsonrpc_conn_serve(p.conn, on_serve, &sv));

	peer_send(&p, "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"ping\","
		      "\"params\":{\"n\":41}}");
	FYAI_TCHECK(!fyai_event_loop_run_until(loop(), NULL, 300));

	FYAI_TCHECK(sv.requests == 1);
	FYAI_TCHECK(!strcmp(sv.method, "ping"));
	FYAI_TCHECK(sv.arg == 41);

	/* The response must go back with the same id. */
	peer_recv(&p, line, sizeof(line));
	FYAI_TCHECK(strstr(line, "\"id\": 7") || strstr(line, "\"id\":7"));
	FYAI_TCHECK(strstr(line, "echo"));

	peer_close(&p);
	printf("ok - an inbound request is served and answered\n");
}

static void test_serve_notification(void)
{
	struct served sv = {};
	struct peer p;

	peer_open(&p);
	FYAI_TCHECK(!jsonrpc_conn_serve(p.conn, on_serve, &sv));

	peer_send(&p, "{\"jsonrpc\":\"2.0\",\"method\":\"note\",\"params\":{\"n\":5}}");
	FYAI_TCHECK(!fyai_event_loop_run_until(loop(), NULL, 300));

	FYAI_TCHECK(sv.notifications == 1);
	FYAI_TCHECK(sv.requests == 0);
	FYAI_TCHECK(sv.arg == 5);
	peer_close(&p);
	printf("ok - an inbound notification is served without a reply\n");
}

static void test_serve_error(void)
{
	struct served sv = { .fail_next = true };
	struct peer p;
	char line[512];

	peer_open(&p);
	FYAI_TCHECK(!jsonrpc_conn_serve(p.conn, on_serve, &sv));
	peer_send(&p, "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"ping\"}");
	FYAI_TCHECK(!fyai_event_loop_run_until(loop(), NULL, 300));

	peer_recv(&p, line, sizeof(line));
	FYAI_TCHECK(strstr(line, "error"));
	FYAI_TCHECK(strstr(line, "refused"));
	FYAI_TCHECK(!strstr(line, "result"));
	peer_close(&p);
	printf("ok - a handler error becomes a JSON-RPC error response\n");
}

static void test_unserved_request_is_answered(void)
{
	struct peer p;
	char line[512];

	/* Reject a request when no handler is registered. */
	peer_open(&p);
	peer_send(&p, "{\"jsonrpc\":\"2.0\",\"method\":\"note\"}");
	peer_send(&p, "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"ping\"}");
	FYAI_TCHECK(!jsonrpc_conn_serve(p.conn, NULL, NULL));
	/* Arm the reader the way an outstanding call would. */
	{
		struct call_result cr = {};
		struct jsonrpc_request *req;

		req = jsonrpc_request_submit(p.conn, "probe", fy_map_empty, 1,
					     false, on_complete, &cr);
		FYAI_TCHECK(req);
		FYAI_TCHECK(!fyai_event_loop_run_until(loop(), NULL, 300));
		peer_recv(&p, line, sizeof(line));   /* our own probe */
		peer_recv(&p, line, sizeof(line));   /* the refusal */
		FYAI_TCHECK(strstr(line, "-32601"));
		jsonrpc_request_cancel(req);
		jsonrpc_request_destroy(req);
	}
	peer_close(&p);
	printf("ok - a request with no handler is refused, not dropped\n");
}

/* Deliver notifications while a request is pending. */
static void test_notification_during_request(void)
{
	struct call_result cr = {};
	struct served sv = {};
	struct jsonrpc_request *req;
	struct peer p;

	peer_open(&p);
	FYAI_TCHECK(!jsonrpc_conn_serve(p.conn, on_serve, &sv));
	req = jsonrpc_request_submit(p.conn, "run", fy_map_empty, 11, false,
				     on_complete, &cr);
	FYAI_TCHECK(req);

	peer_send(&p, "{\"jsonrpc\":\"2.0\",\"method\":\"progress\",\"params\":{\"n\":1}}");
	peer_send(&p, "{\"jsonrpc\":\"2.0\",\"method\":\"progress\",\"params\":{\"n\":2}}");
	peer_send(&p, "{\"jsonrpc\":\"2.0\",\"id\":11,\"result\":{\"done\":true}}");
	FYAI_TCHECK(!fyai_event_loop_run_until(loop(), &cr.done, TEST_BOUND_MS));

	FYAI_TCHECK(sv.notifications == 2);
	FYAI_TCHECK(sv.arg == 2);
	FYAI_TCHECK(cr.ok);
	jsonrpc_request_destroy(req);
	peer_close(&p);
	printf("ok - notifications arrive while a request is outstanding\n");
}


/* Ignore non-frame lines and continue to process valid frames. */
static void test_skips_non_frames(void)
{
	struct served sv = {};
	struct peer p;

	peer_open(&p);
	FYAI_TCHECK(!jsonrpc_conn_serve(p.conn, on_serve, &sv));

	peer_send(&p, "{\"jsonrpc\":\"2.0\",\"method\":\"note\",\"params\":{\"n\":1}}");
	peer_send(&p, "warning: something wrote to stdout");
	peer_send(&p, "");
	peer_send(&p, "   ");
	peer_send(&p, "{\"jsonrpc\":\"2.0\",\"method\":\"note\",\"params\":{\"n\":2}}");
	FYAI_TCHECK(!fyai_event_loop_run_until(loop(), NULL, 300));

	FYAI_TCHECK(sv.notifications == 2);
	FYAI_TCHECK(sv.arg == 2);
	peer_close(&p);
	printf("ok - a stray line is skipped, not fatal to the stream\n");
}

int main(void)
{
	int rc;

	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);
	setup_builders();

	test_request_response();
	test_ignores_other_ids();
	test_error_response();
	test_notification();
	test_cancel();
	test_serve_request();
	test_serve_notification();
	test_serve_error();
	test_unserved_request_is_answered();
	test_notification_during_request();
	test_skips_non_frames();

	fyai_diag_drain(&test_cfg.diag);
	fyai_diag_cleanup(&test_cfg.diag);
	fyai_event_pool_drain(&test_ctx);

	printf("all jsonrpc tests passed\n");
	return 0;
}
