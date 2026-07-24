/*
 * fyai_jsonrpc.h - asynchronous JSON-RPC 2.0 client over stdio or HTTP
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * A transport-agnostic JSON-RPC client on the context event loop. A connection
 * carries one transport - a child's stdin/stdout pipes (newline-framed JSON) or
 * an HTTP endpoint (POST, JSON or SSE response) - and issues requests that
 * complete through a callback with the parsed `result` generic.
 *
 * The core owns framing, id/response correlation, timeouts, cancellation and
 * generic result/error extraction. Transport policy the core cannot know -
 * per-request HTTP headers (auth, protocol, session) and response-header
 * observation (e.g. capturing a session id) - is supplied by the caller through
 * jsonrpc_http_hooks. Subprocess spawn and teardown stay with the caller; a
 * stdio connection borrows already-open descriptors.
 */
#ifndef FYAI_JSONRPC_H
#define FYAI_JSONRPC_H

#include <stdbool.h>

#include <curl/curl.h>

#include "fyai.h"

struct jsonrpc_conn;
struct jsonrpc_request;

typedef void (*jsonrpc_complete_fn)(struct jsonrpc_request *req, void *userdata);

/* HTTP transport policy owned by the caller. */
struct jsonrpc_http_hooks {
	void *userdata;
	/* Append per-request headers before the POST. Returns 0 on success. */
	int (*add_headers)(void *userdata, struct fyai_ctx *ctx,
			   struct curl_slist **headers);
	/* Observe one response header line (unterminated, @len bytes). */
	void (*response_header)(void *userdata, const char *line, size_t len);
};

/*
 * Create a connection. @name labels it in diagnostics; @log_channel enables
 * per-request logging under that fyai_log channel when non-NULL. @timeout_s is
 * the per-request timeout in seconds. Returned connections are destroyed with
 * jsonrpc_conn_destroy(); that does not touch a stdio connection's descriptors.
 */
struct jsonrpc_conn *
jsonrpc_conn_stdio(struct fyai_ctx *ctx, int stdin_fd, int stdout_fd,
		   int timeout_s, const char *name, const char *log_channel);
struct jsonrpc_conn *
jsonrpc_conn_http(struct fyai_ctx *ctx, const char *endpoint, int timeout_s,
		  const char *name, const char *log_channel,
		  const struct jsonrpc_http_hooks *hooks);
void jsonrpc_conn_destroy(struct jsonrpc_conn *conn);

/* Update a stdio connection's descriptors after a transport restart. */
void jsonrpc_conn_stdio_set_fds(struct jsonrpc_conn *conn, int stdin_fd,
				int stdout_fd);
/* Allocate the next request id on this connection. */
long long jsonrpc_conn_next_id(struct jsonrpc_conn *conn);

/*
 * Submit a request (or, when @notification, a notification with no reply).
 * @id is caller-chosen so a retry can reuse an id; ignored for notifications.
 * Completion is deferred through the loop and fires once. The caller keeps
 * @conn alive until the request is destroyed.
 */
struct jsonrpc_request *
jsonrpc_request_submit(struct jsonrpc_conn *conn, const char *method,
		       fy_generic params, long long id, bool notification,
		       jsonrpc_complete_fn complete, void *userdata);
void jsonrpc_request_cancel(struct jsonrpc_request *req);
bool jsonrpc_request_done(const struct jsonrpc_request *req);
bool jsonrpc_request_ok(const struct jsonrpc_request *req);
/* The parsed JSON-RPC `result` (fy_null for a notification), else fy_invalid. */
fy_generic jsonrpc_request_result(const struct jsonrpc_request *req);
/* HTTP status and curl code of the last transfer (0/CURLE_OK for stdio). */
long jsonrpc_request_http_status(const struct jsonrpc_request *req);
CURLcode jsonrpc_request_curl_code(const struct jsonrpc_request *req);
void jsonrpc_request_destroy(struct jsonrpc_request *req);

#endif
