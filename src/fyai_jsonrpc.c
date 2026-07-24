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

	/* stdio */
	int stdin_fd;
	int stdout_fd;

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
	struct response_buffer response;
	char *body;

	/* stdio */
	struct fyai_event_source *read_src;
	struct fyai_event_source *write_src;
	struct fyai_event_source *timer_src;
	struct response_buffer stdio_response;
	char *stdio_request;
	size_t stdio_request_len;
	size_t stdio_request_off;

	fy_generic result;
	long status;
	CURLcode code;
	bool done;
	bool ok;
	bool cancel_requested;
};

static void jsonrpc_request_free(struct jsonrpc_request *req);

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
	jsonrpc_drop_source(&req->read_src);
	jsonrpc_drop_source(&req->write_src);
	jsonrpc_drop_source(&req->timer_src);
}

/* Latch completion once and hand the result to the owner. */
static void jsonrpc_finish(struct jsonrpc_request *req, bool ok,
			   fy_generic result, long status, CURLcode code)
{
	if (req->done)
		return;
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
static bool jsonrpc_stdio_line(struct jsonrpc_request *req)
{
	struct fyai_ctx *ctx = req->conn->ctx;
	fy_generic doc, error, result;

	if (!req->stdio_response.len)
		return false;
	doc = parse_json_string(ctx->transient_gb, req->stdio_response.data);
	if (!fy_generic_is_mapping(doc)) {
		fyai_error(ctx, "%s %s returned invalid stdio JSON",
			   req->conn->name, req->method);
		jsonrpc_finish(req, false, fy_invalid, 0, CURLE_OK);
		return true;
	}
	/* Ignore asynchronous notifications while waiting for our response. */
	if (fy_generic_is_invalid(fy_get(doc, "id", fy_invalid)))
		return false;
	if (fy_get(doc, "id", -1LL) != req->id)
		return false;
	error = fy_get(doc, "error", fy_invalid);
	if (fy_generic_is_valid(error)) {
		fyai_error(ctx, "%s %s: %s", req->conn->name, req->method,
			   fy_get(error, "message", "server error"));
		jsonrpc_finish(req, false, fy_invalid, 0, CURLE_OK);
		return true;
	}
	result = fy_get(doc, "result", fy_invalid);
	if (fy_generic_is_invalid(result)) {
		fyai_error(ctx, "%s %s response has no result",
			   req->conn->name, req->method);
		jsonrpc_finish(req, false, fy_invalid, 0, CURLE_OK);
		return true;
	}
	jsonrpc_finish(req, true, result, 0, CURLE_OK);
	return true;
}

static enum fyai_event_action jsonrpc_stdio_readable(const struct fyai_event *ev)
{
	struct jsonrpc_request *req = ev->userdata;
	char chunk[4096];
	char *newline;
	size_t consumed, remaining;
	ssize_t n;
	int rc;

	for (;;) {
		n = read(ev->fd, chunk, sizeof(chunk));
		if (n > 0) {
			rc = response_buffer_reserve(&req->stdio_response,
				req->stdio_response.len + (size_t)n + 1);
			if (rc) {
				fyai_error(req->conn->ctx,
					   "%s %s response is too large",
					   req->conn->name, req->method);
				jsonrpc_finish(req, false, fy_invalid, 0,
					       CURLE_OK);
				return FYAIEA_CONTINUE;
			}
			memcpy(req->stdio_response.data +
			       req->stdio_response.len, chunk, (size_t)n);
			req->stdio_response.len += (size_t)n;
			req->stdio_response.data[req->stdio_response.len] = '\0';
			while ((newline = memchr(req->stdio_response.data, '\n',
						 req->stdio_response.len))) {
				*newline = '\0';
				consumed = (size_t)(newline -
					req->stdio_response.data) + 1;
				if (jsonrpc_stdio_line(req))
					return FYAIEA_CONTINUE;
				remaining = req->stdio_response.len - consumed;
				memmove(req->stdio_response.data,
					req->stdio_response.data + consumed,
					remaining);
				req->stdio_response.len = remaining;
				req->stdio_response.data[remaining] = '\0';
			}
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		break;
	}
	if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
		return FYAIEA_CONTINUE;
	fyai_error(req->conn->ctx, "%s %s stdio server closed",
		   req->conn->name, req->method);
	jsonrpc_finish(req, false, fy_invalid, 0, CURLE_OK);
	return FYAIEA_CONTINUE;
}

static enum fyai_event_action jsonrpc_stdio_writable(const struct fyai_event *ev)
{
	struct jsonrpc_request *req = ev->userdata;
	ssize_t n;

	while (req->stdio_request_off < req->stdio_request_len) {
		n = write(ev->fd, req->stdio_request + req->stdio_request_off,
			  req->stdio_request_len - req->stdio_request_off);
		if (n > 0) {
			req->stdio_request_off += (size_t)n;
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return FYAIEA_CONTINUE;
		fyai_error(req->conn->ctx, "%s %s stdio write failed",
			   req->conn->name, req->method);
		jsonrpc_finish(req, false, fy_invalid, 0, CURLE_OK);
		return FYAIEA_CONTINUE;
	}
	jsonrpc_drop_source(&req->write_src);
	/* A notification is complete once written; no response is expected. */
	if (req->notification)
		jsonrpc_finish(req, true, fy_null, 0, CURLE_OK);
	return FYAIEA_CONTINUE;
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
	struct fyai_event_loop *el;
	fy_generic rpc;
	const char *body;
	size_t body_len;
	int flags, rc;

	el = fyai_ctx_loop(ctx);
	fyai_error_check(ctx, el, err_out,
			 "could not acquire the application event loop");
	rpc = fy_mapping(ctx->transient_gb, "jsonrpc", "2.0",
			 "method", req->method, "params", params);
	if (!req->notification)
		rpc = fy_assoc(ctx->transient_gb, rpc, "id", req->id);
	body = emit_request_body(ctx->transient_gb, rpc);
	fyai_error_check(ctx, body, err_out,
			 "could not encode JSON-RPC stdio request");
	body_len = strlen(body);
	req->stdio_request = malloc(body_len + 2);
	fyai_error_check(ctx, req->stdio_request, err_out, "out of memory");
	memcpy(req->stdio_request, body, body_len);
	req->stdio_request[body_len] = '\n';
	req->stdio_request[body_len + 1] = '\0';
	req->stdio_request_len = body_len + 1;

	flags = fcntl(conn->stdin_fd, F_GETFL);
	fyai_error_check(ctx, flags >= 0, err_out,
			 "could not inspect stdin: %s", strerror(errno));
	rc = fcntl(conn->stdin_fd, F_SETFL, flags | O_NONBLOCK);
	fyai_error_check(ctx, !rc, err_out,
			 "could not make stdin non-blocking: %s",
			 strerror(errno));
	rc = fyai_event_add_fd(el, conn->stdin_fd, FYAIEV_WRITE,
			       jsonrpc_stdio_writable, req, &req->write_src);
	fyai_error_check(ctx, !rc, err_out, "could not watch stdin");
	if (req->notification)
		return 0;

	flags = fcntl(conn->stdout_fd, F_GETFL);
	fyai_error_check(ctx, flags >= 0, err_out,
			 "could not inspect stdout: %s", strerror(errno));
	rc = fcntl(conn->stdout_fd, F_SETFL, flags | O_NONBLOCK);
	fyai_error_check(ctx, !rc, err_out,
			 "could not make stdout non-blocking: %s",
			 strerror(errno));
	rc = fyai_event_add_fd(el, conn->stdout_fd, FYAIEV_READ,
			       jsonrpc_stdio_readable, req, &req->read_src);
	fyai_error_check(ctx, !rc, err_out, "could not watch stdout");
	rc = fyai_event_add_timer(el, conn->timeout_s * 1000, 0,
				  jsonrpc_stdio_timeout, req, &req->timer_src);
	fyai_error_check(ctx, !rc, err_out, "could not arm response timeout");
	return 0;

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
	size_t len = size * nmemb;

	if (req->conn->has_hooks && req->conn->hooks.response_header)
		req->conn->hooks.response_header(req->conn->hooks.userdata,
						 ptr, len);
	return len;
}

/* Parse the JSON-RPC result out of an HTTP response body (JSON or SSE). */
static fy_generic jsonrpc_http_result(struct jsonrpc_request *req)
{
	struct fyai_ctx *ctx = req->conn->ctx;
	fy_generic doc, error, out;
	const char *json, *data, *end;
	char *sse_json;

	if (req->code != CURLE_OK || req->status < 200 || req->status >= 300)
		return fy_invalid;
	if (req->notification)
		return fy_null;
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
	doc = parse_json_string(ctx->transient_gb, json);
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
	fy_generic rpc;
	const char *body;
	int rc;

	rpc = fy_mapping(ctx->transient_gb, "jsonrpc", "2.0",
			 "method", req->method, "params", params);
	if (!req->notification)
		rpc = fy_assoc(ctx->transient_gb, rpc, "id", req->id);
	body = emit_request_body(ctx->transient_gb, rpc);
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

static void jsonrpc_request_free(struct jsonrpc_request *req)
{
	if (!req)
		return;
	jsonrpc_stdio_sources_drop(req);
	if (req->transfer)
		fyai_curl_transfer_destroy(req->transfer);
	curl_easy_cleanup(req->curl);
	curl_slist_free_all(req->headers);
	free(req->response.data);
	free(req->body);
	free(req->stdio_response.data);
	free(req->stdio_request);
	free(req);
}

void jsonrpc_request_destroy(struct jsonrpc_request *req)
{
	jsonrpc_request_free(req);
}
