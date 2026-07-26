/*
 * fyai_jsonrpc.c - asynchronous JSON-RPC 2.0 client over stdio or HTTP
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_STREAM

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "fyai_curl.h"
#include "fyai_event.h"
#include "fyai_jsonrpc.h"
#include "fyai_log.h"
#include "utils.h"

enum jsonrpc_transport {
	JSONRPC_STDIO,
	JSONRPC_HTTP,
};

struct jsonrpc_conn {
	struct fyai_ctx *ctx;
	enum jsonrpc_transport transport;
	const char *name;
	const char *log_channel;
	int timeout_s;
	long long next_id;

	/* Standard input and output state belongs to the connection. */
	int stdin_fd;
	int stdout_fd;
	struct fyai_event_source *read_src;
	struct fyai_event_source *write_src;
	struct response_buffer rx;
	struct response_buffer tx;
	size_t tx_off;
	struct jsonrpc_request *pending;
	struct jsonrpc_request *flush_wait;	/* notifications awaiting write */
	jsonrpc_serve_fn serve;
	void *serve_userdata;
	bool serve_deferred;	/* handler will answer later */

	/* http */
	const char *endpoint;
	struct jsonrpc_http_hooks hooks;
	bool has_hooks;
};

struct jsonrpc_request {
	struct jsonrpc_conn *conn;
	jsonrpc_complete_fn complete;
	void *userdata;
	const char *method;
	long long id;
	bool notification;
	struct timespec started;

	/* http */
	struct fyai_curl_transfer *transfer;
	CURL *curl;
	struct curl_slist *headers;
	struct curl_slist *response_headers;
	struct response_buffer response;
	char *body;

	/* Standard input and output request state. */
	struct fyai_event_source *timer_src;
	struct jsonrpc_request *next;

	fy_generic result;
	long status;
	CURLcode code;
	bool done;
	bool ok;
	bool cancel_requested;
};

static void jsonrpc_request_free(struct jsonrpc_request *req);
static void jsonrpc_conn_unlink(struct jsonrpc_request *req);

static void jsonrpc_log(struct jsonrpc_request *req, bool success)
{
	struct jsonrpc_conn *conn = req->conn;
	struct timespec end;
	double elapsed_ms;

	if (!conn->log_channel || !conn->ctx->cfg->mcp_logging)
		return;
	clock_gettime(CLOCK_MONOTONIC, &end);
	elapsed_ms = (double)(end.tv_sec - req->started.tv_sec) * 1000.0 +
		(double)(end.tv_nsec - req->started.tv_nsec) / 1000000.0;
	(void)fyai_log_generic(conn->ctx, conn->log_channel,
		fy_null_filtered_mapping(
			"event", "request", "server", conn->name,
			"transport", conn->transport == JSONRPC_STDIO ?
				"stdio" : "http",
			"method", req->method,
			"id", req->notification ? fy_null : fy_value(req->id),
			"notification", req->notification, "success", success,
			"elapsed_ms", elapsed_ms));
}

static void jsonrpc_drop_source(struct fyai_event_source **srcp)
{
	fyai_event_source_remove(*srcp);
	*srcp = NULL;
}

static void jsonrpc_stdio_sources_drop(struct jsonrpc_request *req)
{
	jsonrpc_drop_source(&req->timer_src);
}

/* Latch completion once and hand the result to the owner. */
static void jsonrpc_finish(struct jsonrpc_request *req, bool ok,
			   fy_generic result, long status, CURLcode code)
{
	if (req->done)
		return;
	/* Remove the request before another frame can use its ID. */
	jsonrpc_conn_unlink(req);
	jsonrpc_stdio_sources_drop(req);
	req->result = result;
	req->ok = ok;
	req->status = status;
	req->code = code;
	req->done = true;
	jsonrpc_log(req, ok);
	if (req->complete)
		req->complete(req, req->userdata);
}

/*
 * stdio transport
 */

/* Match one framed line against this request. Returns true once the request is
 * finished (matched response, or a fatal parse/transport error). */
/* Unlink a request from one of the connection's lists. */
static struct jsonrpc_request **
jsonrpc_list_find(struct jsonrpc_request **head, struct jsonrpc_request *req)
{
	struct jsonrpc_request **pp;

	for (pp = head; *pp; pp = &(*pp)->next)
		if (*pp == req)
			return pp;
	return NULL;
}

static void jsonrpc_list_remove(struct jsonrpc_request **head,
				struct jsonrpc_request *req)
{
	struct jsonrpc_request **pp;

	pp = jsonrpc_list_find(head, req);
	if (pp)
		*pp = req->next;
	req->next = NULL;
}

/* Remove a request from its connection list. */
static void jsonrpc_conn_unlink(struct jsonrpc_request *req)
{
	if (!req || !req->conn || req->conn->transport != JSONRPC_STDIO)
		return;
	jsonrpc_list_remove(&req->conn->pending, req);
	jsonrpc_list_remove(&req->conn->flush_wait, req);
}

static struct jsonrpc_request *jsonrpc_pending_take(struct jsonrpc_conn *conn,
						    long long id)
{
	struct jsonrpc_request **pp, *req;

	for (pp = &conn->pending; *pp; pp = &(*pp)->next) {
		if ((*pp)->id != id)
			continue;
		req = *pp;
		*pp = req->next;
		req->next = NULL;
		return req;
	}
	return NULL;
}

static int jsonrpc_conn_arm_write(struct jsonrpc_conn *conn);

/* Queue one output frame. */
static int jsonrpc_conn_send(struct jsonrpc_conn *conn, const char *json)
{
	size_t len = strlen(json);
	int rc;

	rc = response_buffer_reserve(&conn->tx, conn->tx.len + len + 2);
	if (rc)
		return -1;
	memcpy(conn->tx.data + conn->tx.len, json, len);
	conn->tx.len += len;
	conn->tx.data[conn->tx.len++] = '\n';
	conn->tx.data[conn->tx.len] = '\0';
	return jsonrpc_conn_arm_write(conn);
}

/* Answer an inbound request. @error wins when valid, per JSON-RPC. */
static void jsonrpc_conn_reply(struct jsonrpc_conn *conn, fy_generic id,
			       fy_generic result, fy_generic error)
{
	struct fy_generic_builder *gb;
	fy_generic frame;
	const char *body;

	gb = fyai_ctx_transient_gb(conn->ctx);
	if (!gb)
		return;
	if (fy_generic_is_valid(error))
		frame = fy_gb_mapping(gb, "jsonrpc", "2.0", "id", id,
				      "error", error);
	else
		frame = fy_gb_mapping(gb, "jsonrpc", "2.0", "id", id,
				      "result", fy_generic_is_valid(result) ?
						result : fy_null);
	body = emit_json_string(gb, frame);
	if (body)
		(void)jsonrpc_conn_send(conn, body);
}

/* Process one decoded frame. */
static void jsonrpc_conn_frame(struct jsonrpc_conn *conn, fy_generic doc)
{
	struct fyai_ctx *ctx = conn->ctx;
	struct jsonrpc_request *req;
	fy_generic id, method, error, result;
	const char *name;

	id = fy_get(doc, "id", fy_invalid);
	method = fy_get(doc, "method", fy_invalid);

	if (fy_generic_is_valid(method)) {
		name = fy_castp(&method, "");
		if (!conn->serve) {
			/* Reject a request when no handler is available. */
			if (fy_generic_is_valid(id))
				jsonrpc_conn_reply(conn, id, fy_invalid,
					fy_gb_mapping(
					    fyai_ctx_transient_gb(ctx),
					    "code", -32601LL,
					    "message", "method not found"));
			return;
		}
		error = fy_invalid;
		conn->serve_deferred = false;
		result = conn->serve(conn, name, fy_get(doc, "params", fy_null),
				     id, conn->serve_userdata, &error);
		if (fy_generic_is_valid(id) && !conn->serve_deferred)
			jsonrpc_conn_reply(conn, id, result, error);
		conn->serve_deferred = false;
		return;
	}

	if (!fy_generic_is_valid(id))
		return;			/* neither a call nor a reply */
	req = jsonrpc_pending_take(conn, fy_cast(id, -1LL));
	if (!req)
		return;			/* not ours, or already finished */

	error = fy_get(doc, "error", fy_invalid);
	if (fy_generic_is_valid(error)) {
		fyai_error(ctx, "%s %s: %s", conn->name, req->method,
			   fy_get(error, "message", "server error"));
		jsonrpc_finish(req, false, fy_invalid, 0, CURLE_OK);
		return;
	}
	result = fy_get(doc, "result", fy_invalid);
	if (fy_generic_is_invalid(result)) {
		fyai_error(ctx, "%s %s response has no result",
			   conn->name, req->method);
		jsonrpc_finish(req, false, fy_invalid, 0, CURLE_OK);
		return;
	}
	jsonrpc_finish(req, true, result, 0, CURLE_OK);
}

static void jsonrpc_conn_line(struct jsonrpc_conn *conn, const char *line)
{
	struct fy_generic_builder *gb;
	fy_generic doc;

	while (*line == ' ' || *line == '\t' || *line == '\r')
		line++;
	if (!*line)
		return;
	/* Skip and report non-JSON lines without desynchronizing the stream. */
	if (*line != '{') {
		fyai_debug(conn->ctx, "%s skipped a non-frame line: %.60s",
			   conn->name, line);
		return;
	}
	gb = fyai_ctx_transient_gb(conn->ctx);
	if (!gb) {
		fyai_error(conn->ctx, "%s could not acquire transient storage",
			   conn->name);
		return;
	}
	doc = parse_json_string(gb, line);
	if (!fy_generic_is_mapping(doc)) {
		fyai_error(conn->ctx, "%s received invalid stdio JSON",
			   conn->name);
		return;
	}
	jsonrpc_conn_frame(conn, doc);
}

/* Fail all pending requests after the peer closes the connection. */
static void jsonrpc_conn_fail_pending(struct jsonrpc_conn *conn,
				      const char *why)
{
	struct jsonrpc_request *req;

	while ((req = conn->pending)) {
		conn->pending = req->next;
		req->next = NULL;
		fyai_error(conn->ctx, "%s %s %s", conn->name, req->method, why);
		jsonrpc_finish(req, false, fy_invalid, 0, CURLE_OK);
	}
}

static enum fyai_event_action jsonrpc_conn_readable(const struct fyai_event *ev)
{
	struct jsonrpc_conn *conn = ev->userdata;
	char chunk[4096];
	char *newline;
	size_t consumed, remaining;
	ssize_t n;
	int rc;

	for (;;) {
		n = read(ev->fd, chunk, sizeof(chunk));
		if (n > 0) {
			rc = response_buffer_reserve(&conn->rx,
						     conn->rx.len +
						     (size_t)n + 1);
			if (rc) {
				fyai_error(conn->ctx, "%s frame is too large",
					   conn->name);
				jsonrpc_conn_fail_pending(conn, "overflowed");
				return FYAIEA_CONTINUE;
			}
			memcpy(conn->rx.data + conn->rx.len, chunk, (size_t)n);
			conn->rx.len += (size_t)n;
			conn->rx.data[conn->rx.len] = '\0';
			while ((newline = memchr(conn->rx.data, '\n',
						 conn->rx.len))) {
				*newline = '\0';
				consumed = (size_t)(newline - conn->rx.data) + 1;
				jsonrpc_conn_line(conn, conn->rx.data);
				remaining = conn->rx.len - consumed;
				memmove(conn->rx.data,
					conn->rx.data + consumed, remaining);
				conn->rx.len = remaining;
				conn->rx.data[remaining] = '\0';
			}
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		break;
	}
	if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
		return FYAIEA_CONTINUE;

	jsonrpc_drop_source(&conn->read_src);
	jsonrpc_conn_fail_pending(conn, "stdio peer closed");
	return FYAIEA_CONTINUE;
}

static enum fyai_event_action jsonrpc_conn_writable(const struct fyai_event *ev)
{
	struct jsonrpc_conn *conn = ev->userdata;
	struct jsonrpc_request *req;
	ssize_t n;

	while (conn->tx_off < conn->tx.len) {
		n = write(ev->fd, conn->tx.data + conn->tx_off,
			  conn->tx.len - conn->tx_off);
		if (n > 0) {
			conn->tx_off += (size_t)n;
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return FYAIEA_CONTINUE;
		fyai_error(conn->ctx, "%s stdio write failed", conn->name);
		jsonrpc_drop_source(&conn->write_src);
		jsonrpc_conn_fail_pending(conn, "could not be written");
		return FYAIEA_CONTINUE;
	}
	conn->tx.len = 0;
	conn->tx_off = 0;
	jsonrpc_drop_source(&conn->write_src);

	/* Complete notifications after the write queue is empty. */
	while ((req = conn->flush_wait)) {
		conn->flush_wait = req->next;
		req->next = NULL;
		jsonrpc_finish(req, true, fy_null, 0, CURLE_OK);
	}
	return FYAIEA_CONTINUE;
}

static int jsonrpc_conn_arm_write(struct jsonrpc_conn *conn)
{
	struct fyai_event_loop *el;

	if (conn->write_src || conn->tx_off >= conn->tx.len)
		return 0;
	el = fyai_ctx_loop(conn->ctx);
	if (!el)
		return -1;
	return fyai_event_add_fd(el, conn->stdin_fd, FYAIEV_WRITE,
				 jsonrpc_conn_writable, conn,
				 &conn->write_src);
}

static int jsonrpc_conn_arm_read(struct jsonrpc_conn *conn)
{
	struct fyai_event_loop *el;
	int flags;

	if (conn->read_src)
		return 0;
	el = fyai_ctx_loop(conn->ctx);
	if (!el)
		return -1;
	flags = fcntl(conn->stdout_fd, F_GETFL, 0);
	if (flags >= 0)
		(void)fcntl(conn->stdout_fd, F_SETFL, flags | O_NONBLOCK);
	return fyai_event_add_fd(el, conn->stdout_fd, FYAIEV_READ,
				 jsonrpc_conn_readable, conn, &conn->read_src);
}

bool jsonrpc_conn_has_output(const struct jsonrpc_conn *conn)
{
	return conn && conn->transport == JSONRPC_STDIO &&
	       conn->tx_off < conn->tx.len;
}

void jsonrpc_conn_defer(struct jsonrpc_conn *conn)
{
	if (conn)
		conn->serve_deferred = true;
}

int jsonrpc_conn_respond(struct jsonrpc_conn *conn, fy_generic id,
			 fy_generic result, fy_generic error)
{
	if (!conn || conn->transport != JSONRPC_STDIO)
		return -1;
	jsonrpc_conn_reply(conn, id, result, error);
	return 0;
}

int jsonrpc_conn_serve(struct jsonrpc_conn *conn, jsonrpc_serve_fn fn,
		       void *userdata)
{
	if (!conn || conn->transport != JSONRPC_STDIO)
		return -1;
	conn->serve = fn;
	conn->serve_userdata = userdata;
	/* A server must listen before anyone calls it. */
	return fn ? jsonrpc_conn_arm_read(conn) : 0;
}

static enum fyai_event_action jsonrpc_stdio_timeout(const struct fyai_event *ev)
{
	struct jsonrpc_request *req = ev->userdata;

	req->timer_src = NULL;
	fyai_error(req->conn->ctx, "%s %s stdio response timed out",
		   req->conn->name, req->method);
	jsonrpc_finish(req, false, fy_invalid, 0, CURLE_OK);
	return FYAIEA_CONTINUE;
}

static int jsonrpc_stdio_submit(struct jsonrpc_request *req, fy_generic params)
{
	struct jsonrpc_conn *conn = req->conn;
	struct fyai_ctx *ctx = conn->ctx;
	struct fy_generic_builder *gb;
	struct fyai_event_loop *el;
	fy_generic rpc;
	const char *body;
	int flags, rc;

	el = fyai_ctx_loop(ctx);
	fyai_error_check(ctx, el, err_out,
			 "could not acquire the application event loop");
	gb = fyai_ctx_transient_gb(ctx);
	fyai_error_check(ctx, gb, err_out,
			 "could not acquire transient storage");
	rpc = fy_mapping(gb, "jsonrpc", "2.0",
			 "method", req->method, "params", params);
	if (!req->notification)
		rpc = fy_assoc(gb, rpc, "id", req->id);
	body = emit_request_body(gb, rpc);
	fyai_error_check(ctx, body, err_out,
			 "could not encode JSON-RPC stdio request");

	flags = fcntl(conn->stdin_fd, F_GETFL);
	fyai_error_check(ctx, flags >= 0, err_out,
			 "could not inspect stdin: %s", strerror(errno));
	rc = fcntl(conn->stdin_fd, F_SETFL, flags | O_NONBLOCK);
	fyai_error_check(ctx, !rc, err_out,
			 "could not make stdin non-blocking: %s",
			 strerror(errno));

	/* Register the request before the peer can reply. */
	if (req->notification) {
		req->next = conn->flush_wait;
		conn->flush_wait = req;
	} else {
		req->next = conn->pending;
		conn->pending = req;
	}

	rc = jsonrpc_conn_send(conn, body);
	fyai_error_check(ctx, !rc, err_dequeue, "could not queue the request");
	if (req->notification)
		return 0;

	rc = jsonrpc_conn_arm_read(conn);
	fyai_error_check(ctx, !rc, err_dequeue, "could not watch stdout");
	/* A nonpositive timeout disables the timer. */
	if (conn->timeout_s > 0) {
		rc = fyai_event_add_timer(el, conn->timeout_s * 1000, 0,
					  jsonrpc_stdio_timeout, req,
					  &req->timer_src);
		fyai_error_check(ctx, !rc, err_dequeue,
				 "could not arm response timeout");
	}
	return 0;

err_dequeue:
	jsonrpc_list_remove(&conn->pending, req);
	jsonrpc_list_remove(&conn->flush_wait, req);
err_out:
	jsonrpc_stdio_sources_drop(req);
	return -1;
}

/*
 * HTTP transport
 */

static size_t jsonrpc_http_header(void *ptr, size_t size, size_t nmemb,
				  void *userdata)
{
	struct jsonrpc_request *req = userdata;
	struct curl_slist *headers;
	const char *data = ptr;
	char *line;
	size_t raw_len = size * nmemb;
	size_t len = raw_len;

	while (len && (data[len - 1] == '\r' || data[len - 1] == '\n'))
		len--;
	line = len ? strndup(data, len) : NULL;
	if (line) {
		if (!strncasecmp(line, "HTTP/", 5)) {
			curl_slist_free_all(req->response_headers);
			req->response_headers = NULL;
		}
		headers = curl_slist_append(req->response_headers, line);
		free(line);
		if (headers)
			req->response_headers = headers;
	}
	if (req->conn->has_hooks && req->conn->hooks.response_header)
		req->conn->hooks.response_header(req->conn->hooks.userdata,
						 ptr, raw_len);
	return raw_len;
}

/* Parse the JSON-RPC result out of an HTTP response body (JSON or SSE). */
static fy_generic jsonrpc_http_result(struct jsonrpc_request *req)
{
	struct fyai_ctx *ctx = req->conn->ctx;
	struct fy_generic_builder *gb;
	fy_generic doc, error, out;
	const char *json, *data, *end;
	char *sse_json;

	if (req->code != CURLE_OK || req->status < 200 || req->status >= 300)
		return fy_invalid;
	if (req->notification)
		return fy_null;
	gb = fyai_ctx_transient_gb(ctx);
	fyai_error_check(ctx, gb, err_out, "%s %s could not acquire "
			 "transient storage", req->conn->name, req->method);
	json = req->response.data ? req->response.data : "";
	sse_json = NULL;
	if (!strncmp(json, "event:", 6) || !strncmp(json, "data:", 5)) {
		data = strstr(json, "data:");
		if (data) {
			data += 5;
			while (*data == ' ' || *data == '\t')
				data++;
			end = strchr(data, '\n');
			sse_json = strndup(data,
				end ? (size_t)(end - data) : strlen(data));
			if (sse_json)
				json = sse_json;
		}
	}
	doc = parse_json_string(gb, json);
	free(sse_json);
	fyai_error_check(ctx, fy_generic_is_valid(doc), err_out,
			 "%s %s returned invalid JSON", req->conn->name,
			 req->method);
	error = fy_get(doc, "error");
	fyai_error_check(ctx, fy_generic_is_invalid(error), err_out,
			 "%s %s: %s", req->conn->name, req->method,
			 fy_get(error, "message", "server error"));
	out = fy_get(doc, "result", fy_invalid);
	fyai_error_check(ctx, fy_generic_is_valid(out), err_out,
			 "%s %s response has no result", req->conn->name,
			 req->method);
	return out;

err_out:
	return fy_invalid;
}

static void jsonrpc_http_complete(struct fyai_curl_transfer *transfer,
				  void *userdata)
{
	struct jsonrpc_request *req = userdata;
	fy_generic result;

	req->code = fyai_curl_collect(transfer);
	curl_easy_getinfo(req->curl, CURLINFO_RESPONSE_CODE, &req->status);
	fyai_curl_transfer_destroy(transfer);
	req->transfer = NULL;
	if (req->cancel_requested) {
		jsonrpc_finish(req, false, fy_invalid, req->status, req->code);
		return;
	}
	result = jsonrpc_http_result(req);
	jsonrpc_finish(req, fy_generic_is_valid(result), result, req->status,
		       req->code);
}

static int jsonrpc_http_submit(struct jsonrpc_request *req, fy_generic params)
{
	struct jsonrpc_conn *conn = req->conn;
	struct fyai_ctx *ctx = conn->ctx;
	struct fy_generic_builder *gb;
	fy_generic rpc;
	const char *body;
	int rc;

	gb = fyai_ctx_transient_gb(ctx);
	fyai_error_check(ctx, gb, err_out, "%s %s could not acquire "
			 "transient storage", conn->name, req->method);
	rpc = fy_mapping(gb, "jsonrpc", "2.0",
			 "method", req->method, "params", params);
	if (!req->notification)
		rpc = fy_assoc(gb, rpc, "id", req->id);
	body = emit_request_body(gb, rpc);
	fyai_error_check(ctx, body, err_out, "%s %s could not encode request",
			 conn->name, req->method);
	req->body = strdup(body);
	fyai_error_check(ctx, req->body, err_out, "out of memory");
	rc = append_header(&req->headers, "Content-Type: application/json");
	if (!rc)
		rc = append_header(&req->headers,
				   "Accept: application/json, text/event-stream");
	fyai_error_check(ctx, !rc, err_out,
			 "%s %s could not create request headers", conn->name,
			 req->method);
	if (conn->has_hooks && conn->hooks.add_headers) {
		rc = conn->hooks.add_headers(conn->hooks.userdata, ctx,
					     &req->headers);
		fyai_error_check(ctx, !rc, err_out,
				 "%s %s could not add request headers",
				 conn->name, req->method);
	}
	req->curl = curl_easy_init();
	/* Reserve SIGALRM for the watchdog. */
	curl_easy_setopt(req->curl, CURLOPT_NOSIGNAL, 1L);

	fyai_error_check(ctx, req->curl, err_out,
			 "%s %s could not create curl handle", conn->name,
			 req->method);
	curl_easy_setopt(req->curl, CURLOPT_URL, conn->endpoint);
	curl_easy_setopt(req->curl, CURLOPT_HTTPHEADER, req->headers);
	curl_easy_setopt(req->curl, CURLOPT_POSTFIELDS, req->body);
	curl_easy_setopt(req->curl, CURLOPT_WRITEFUNCTION, write_response);
	curl_easy_setopt(req->curl, CURLOPT_WRITEDATA, &req->response);
	curl_easy_setopt(req->curl, CURLOPT_HEADERFUNCTION, jsonrpc_http_header);
	curl_easy_setopt(req->curl, CURLOPT_HEADERDATA, req);
	curl_easy_setopt(req->curl, CURLOPT_TIMEOUT, (long)conn->timeout_s);
	curl_easy_setopt(req->curl, CURLOPT_USERAGENT, ctx->user_agent);
	req->transfer = fyai_curl_submit(ctx, req->curl, jsonrpc_http_complete,
					 req);
	fyai_error_check(ctx, req->transfer, err_out,
			 "%s %s could not submit request", conn->name,
			 req->method);
	return 0;

err_out:
	return -1;
}

/*
 * Public API
 */

struct jsonrpc_conn *
jsonrpc_conn_stdio(struct fyai_ctx *ctx, int stdin_fd, int stdout_fd,
		   int timeout_s, const char *name, const char *log_channel)
{
	struct jsonrpc_conn *conn;

	conn = calloc(1, sizeof(*conn));
	fyai_error_check(ctx, conn, err_out, "out of memory");
	conn->ctx = ctx;
	conn->transport = JSONRPC_STDIO;
	conn->name = name;
	conn->log_channel = log_channel;
	conn->timeout_s = timeout_s;
	conn->stdin_fd = stdin_fd;
	conn->stdout_fd = stdout_fd;
	return conn;

err_out:
	return NULL;
}

struct jsonrpc_conn *
jsonrpc_conn_http(struct fyai_ctx *ctx, const char *endpoint, int timeout_s,
		  const char *name, const char *log_channel,
		  const struct jsonrpc_http_hooks *hooks)
{
	struct jsonrpc_conn *conn;

	conn = calloc(1, sizeof(*conn));
	fyai_error_check(ctx, conn, err_out, "out of memory");
	conn->ctx = ctx;
	conn->transport = JSONRPC_HTTP;
	conn->name = name;
	conn->log_channel = log_channel;
	conn->timeout_s = timeout_s;
	conn->endpoint = endpoint;
	if (hooks) {
		conn->hooks = *hooks;
		conn->has_hooks = true;
	}
	return conn;

err_out:
	return NULL;
}

void jsonrpc_conn_destroy(struct jsonrpc_conn *conn)
{
	if (conn && conn->transport == JSONRPC_STDIO) {
		jsonrpc_drop_source(&conn->read_src);
		jsonrpc_drop_source(&conn->write_src);
		free(conn->rx.data);
		free(conn->tx.data);
		conn->rx.data = conn->tx.data = NULL;
	}
	free(conn);
}

void jsonrpc_conn_stdio_set_fds(struct jsonrpc_conn *conn, int stdin_fd,
				int stdout_fd)
{
	if (!conn)
		return;
	conn->stdin_fd = stdin_fd;
	conn->stdout_fd = stdout_fd;
}

long long jsonrpc_conn_next_id(struct jsonrpc_conn *conn)
{
	return ++conn->next_id;
}

struct jsonrpc_request *
jsonrpc_request_submit(struct jsonrpc_conn *conn, const char *method,
		       fy_generic params, long long id, bool notification,
		       jsonrpc_complete_fn complete, void *userdata)
{
	struct jsonrpc_request *req;
	int rc;

	req = calloc(1, sizeof(*req));
	fyai_error_check(conn->ctx, req, err_out, "out of memory");
	req->conn = conn;
	req->method = method;
	req->id = id;
	req->notification = notification;
	req->complete = complete;
	req->userdata = userdata;
	req->result = fy_invalid;
	req->code = CURLE_OK;
	clock_gettime(CLOCK_MONOTONIC, &req->started);

	rc = conn->transport == JSONRPC_STDIO ?
		jsonrpc_stdio_submit(req, params) :
		jsonrpc_http_submit(req, params);
	fyai_error_check(conn->ctx, !rc, err_free, "%s could not submit %s",
			 conn->name, method);
	return req;

err_free:
	jsonrpc_request_free(req);
err_out:
	return NULL;
}

void jsonrpc_request_cancel(struct jsonrpc_request *req)
{
	if (!req || req->done)
		return;
	req->cancel_requested = true;
	if (req->conn->transport == JSONRPC_STDIO)
		jsonrpc_finish(req, false, fy_invalid, 0, CURLE_OK);
	else if (req->transfer)
		fyai_curl_cancel(req->transfer);
}

bool jsonrpc_request_done(const struct jsonrpc_request *req)
{
	return req && req->done;
}

bool jsonrpc_request_ok(const struct jsonrpc_request *req)
{
	return req && req->ok;
}

fy_generic jsonrpc_request_result(const struct jsonrpc_request *req)
{
	return req ? req->result : fy_invalid;
}

long jsonrpc_request_http_status(const struct jsonrpc_request *req)
{
	return req ? req->status : 0;
}

CURLcode jsonrpc_request_curl_code(const struct jsonrpc_request *req)
{
	return req ? req->code : CURLE_OK;
}

const char *jsonrpc_request_response_body(
		const struct jsonrpc_request *req)
{
	if (!req)
		return NULL;
	/* Only HTTP requests retain a response body. */
	if (req->conn->transport == JSONRPC_STDIO)
		return NULL;
	return req->response.data;
}

const char *
jsonrpc_request_response_header(const struct jsonrpc_request *req,
				const char *name)
{
	const struct curl_slist *item;
	const char *value;
	size_t len;

	if (!req || !name || !*name)
		return NULL;
	len = strlen(name);
	for (item = req->response_headers; item; item = item->next) {
		if (strncasecmp(item->data, name, len) ||
		    item->data[len] != ':')
			continue;
		value = item->data + len + 1;
		while (*value == ' ' || *value == '\t')
			value++;
		return value;
	}
	return NULL;
}

static void jsonrpc_request_free(struct jsonrpc_request *req)
{
	if (!req)
		return;
	jsonrpc_conn_unlink(req);
	jsonrpc_stdio_sources_drop(req);
	if (req->transfer)
		fyai_curl_transfer_destroy(req->transfer);
	curl_easy_cleanup(req->curl);
	curl_slist_free_all(req->headers);
	curl_slist_free_all(req->response_headers);
	free(req->response.data);
	free(req->body);
	free(req);
}

void jsonrpc_request_destroy(struct jsonrpc_request *req)
{
	jsonrpc_request_free(req);
}
