/*
 * fyai_mcp.c - minimal MCP Streamable HTTP client
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_TOOLS

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "fyai_curl.h"
#include "fyai_event.h"
#include "fyai_log.h"
#include "fyai_tools.h"
#include "utils.h"

#define MCP_PREFIX "mcp__"

enum mcp_transport {
	MCP_TRANSPORT_HTTP,
	MCP_TRANSPORT_STDIO,
};

struct fyai_mcp_ctx {
	struct fyai_mcp_ctx *next;
	enum mcp_transport transport;
	CURL *curl;
	pid_t pid;
	int stdin_fd;
	int stdout_fd;
	const char *session_id;
	long long request_id;
	const char *name;
	const char *endpoint;
	const char *auth_token;
	const char *protocol_version;
	int timeout;
	bool recovering;
};

struct mcp_headers {
	struct fyai_ctx *ctx;
	struct fyai_mcp_ctx *mcp;
};

struct mcp_http_exchange;
typedef void (*mcp_http_exchange_complete_fn)(
		struct mcp_http_exchange *exchange, void *userdata);

struct mcp_http_exchange {
	struct fyai_ctx *ctx;
	struct fyai_mcp_ctx *mcp;
	struct fyai_curl_transfer *transfer;
	CURL *curl;
	struct curl_slist *headers;
	struct response_buffer response;
	struct mcp_headers header_ctx;
	char *body;
	char *auth;
	char *session;
	char *version;
	const char *method;
	mcp_http_exchange_complete_fn complete;
	void *userdata;
	long long id;
	struct timespec started;
	CURLcode code;
	long status;
	bool notification;
	bool done;
};

static size_t mcp_header(void *ptr, size_t size, size_t nmemb, void *userdata)
{
	struct mcp_headers *mh = userdata;
	const char *line = ptr;
	size_t len = size * nmemb;
	const char key[] = "mcp-session-id:";
	const char *p, *end;

	if (len < sizeof(key) - 1 || strncasecmp(line, key, sizeof(key) - 1))
		return len;
	p = line + sizeof(key) - 1;
	end = line + len;
	while (p < end && isspace((unsigned char)*p))
		p++;
	while (end > p && isspace((unsigned char)end[-1]))
		end--;
	mh->mcp->session_id = fy_gb_intern_string(mh->ctx->cfg->gb,
			fy_sprintfa("%.*s", (int)(end - p), p));
	return len;
}

static int mcp_initialize(struct fyai_ctx *ctx, struct fyai_mcp_ctx *mcp);
static void mcp_stdio_stop(struct fyai_ctx *ctx, struct fyai_mcp_ctx *mcp);

static bool mcp_transient_status(CURLcode rc, long status)
{
	return rc == CURLE_COULDNT_CONNECT || rc == CURLE_COULDNT_RESOLVE_HOST ||
		rc == CURLE_RECV_ERROR || rc == CURLE_SEND_ERROR ||
		rc == CURLE_OPERATION_TIMEDOUT || status == 408 || status == 429 ||
		status == 500 || status == 502 || status == 503 || status == 504;
}

static bool mcp_idempotent_method(const char *method)
{
	return !strcmp(method, "initialize") || !strcmp(method, "tools/list");
}

static void mcp_http_exchange_log(struct mcp_http_exchange *exchange)
{
	struct fyai_ctx *ctx;
	struct timespec ended;
	double elapsed_ms;

	ctx = exchange->ctx;
	if (!ctx->cfg->mcp_logging)
		return;
	clock_gettime(CLOCK_MONOTONIC, &ended);
	elapsed_ms =
		(double)(ended.tv_sec - exchange->started.tv_sec) * 1000.0 +
		(double)(ended.tv_nsec - exchange->started.tv_nsec) /
			1000000.0;
	(void)fyai_log_generic(ctx, "mcp", fy_null_filtered_mapping(
		"event", "request",
		"server", exchange->mcp->name,
		"method", exchange->method,
		"id", exchange->notification ?
			fy_null : fy_value(exchange->id),
		"notification", exchange->notification,
		"http_status", exchange->status,
		"curl_code", (long long)exchange->code,
		"elapsed_ms", elapsed_ms,
		"session", exchange->mcp->session_id ?
			"active" : "none"));
}

static void mcp_http_exchange_complete(
		struct fyai_curl_transfer *transfer, void *userdata)
{
	struct mcp_http_exchange *exchange;

	exchange = userdata;
	exchange->code = fyai_curl_collect(transfer);
	fyai_curl_transfer_destroy(transfer);
	exchange->transfer = NULL;
	if (exchange->code == CURLE_OK)
		curl_easy_getinfo(exchange->curl, CURLINFO_RESPONSE_CODE,
				  &exchange->status);
	exchange->done = true;
	mcp_http_exchange_log(exchange);
	if (exchange->complete)
		exchange->complete(exchange, exchange->userdata);
}

static struct mcp_http_exchange *
mcp_http_exchange_submit(struct fyai_ctx *ctx, struct fyai_mcp_ctx *mcp,
			 const char *method, fy_generic params,
			 bool notification, long long id,
			 mcp_http_exchange_complete_fn complete,
			 void *userdata)
{
	struct mcp_http_exchange *exchange;
	fy_generic request;
	const char *body;
	int rc;

	exchange = calloc(1, sizeof(*exchange));
	fyai_error_check(ctx, exchange, err_out, "out of memory");
	exchange->ctx = ctx;
	exchange->mcp = mcp;
	exchange->method = method;
	exchange->id = id;
	exchange->notification = notification;
	exchange->complete = complete;
	exchange->userdata = userdata;
	exchange->code = CURLE_FAILED_INIT;
	exchange->header_ctx.ctx = ctx;
	exchange->header_ctx.mcp = mcp;
	request = fy_mapping(ctx->transient_gb, "jsonrpc", "2.0",
			     "method", method, "params", params);
	if (!notification)
		request = fy_assoc(ctx->transient_gb, request, "id", id);
	body = emit_request_body(ctx->transient_gb, request);
	fyai_error_check(ctx, body, err_free,
			 "MCP %s could not encode request", method);
	exchange->body = strdup(body);
	fyai_error_check(ctx, exchange->body, err_free, "out of memory");
	rc = append_header(&exchange->headers,
			   "Content-Type: application/json");
	fyai_error_check(ctx, !rc, err_free,
			 "MCP %s could not create request headers", method);
	rc = append_header(&exchange->headers,
			   "Accept: application/json, text/event-stream");
	fyai_error_check(ctx, !rc, err_free,
			 "MCP %s could not create request headers", method);
	exchange->version = make_header("MCP-Protocol-Version: ",
					mcp->protocol_version);
	rc = exchange->version ?
		append_header(&exchange->headers, exchange->version) : -1;
	fyai_error_check(ctx, !rc, err_free,
			 "MCP %s could not create protocol header", method);
	if (mcp->auth_token && *mcp->auth_token) {
		exchange->auth = make_header("Authorization: Bearer ",
					    mcp->auth_token);
		rc = exchange->auth ?
			append_header(&exchange->headers, exchange->auth) : -1;
		fyai_error_check(ctx, !rc, err_free,
				 "MCP %s could not create auth header", method);
	}
	if (mcp->session_id) {
		exchange->session = make_header("Mcp-Session-Id: ",
					       mcp->session_id);
		rc = exchange->session ?
			append_header(&exchange->headers,
				      exchange->session) : -1;
		fyai_error_check(ctx, !rc, err_free,
				 "MCP %s could not create session header",
				 method);
	}
	exchange->curl = curl_easy_init();
	fyai_error_check(ctx, exchange->curl, err_free,
			 "MCP %s could not create curl handle", method);
	curl_easy_setopt(exchange->curl, CURLOPT_URL, mcp->endpoint);
	curl_easy_setopt(exchange->curl, CURLOPT_HTTPHEADER,
			 exchange->headers);
	curl_easy_setopt(exchange->curl, CURLOPT_POSTFIELDS, exchange->body);
	curl_easy_setopt(exchange->curl, CURLOPT_WRITEFUNCTION,
			 write_response);
	curl_easy_setopt(exchange->curl, CURLOPT_WRITEDATA,
			 &exchange->response);
	curl_easy_setopt(exchange->curl, CURLOPT_HEADERFUNCTION, mcp_header);
	curl_easy_setopt(exchange->curl, CURLOPT_HEADERDATA,
			 &exchange->header_ctx);
	curl_easy_setopt(exchange->curl, CURLOPT_TIMEOUT, (long)mcp->timeout);
	curl_easy_setopt(exchange->curl, CURLOPT_USERAGENT, ctx->user_agent);
	if (ctx->cfg->mcp_logging)
		clock_gettime(CLOCK_MONOTONIC, &exchange->started);
	exchange->transfer = fyai_curl_submit(ctx, exchange->curl,
					      mcp_http_exchange_complete,
					      exchange);
	fyai_error_check(ctx, exchange->transfer, err_free,
			 "MCP %s could not submit request", method);
	return exchange;

err_free:
	if (exchange) {
		if (exchange->transfer)
			fyai_curl_transfer_destroy(exchange->transfer);
		curl_easy_cleanup(exchange->curl);
		curl_slist_free_all(exchange->headers);
		free(exchange->response.data);
		free(exchange->body);
		free(exchange->auth);
		free(exchange->session);
		free(exchange->version);
		free(exchange);
	}
err_out:
	return NULL;
}

static void mcp_http_exchange_destroy(struct mcp_http_exchange *exchange)
{
	if (!exchange)
		return;
	if (exchange->transfer)
		fyai_curl_transfer_destroy(exchange->transfer);
	curl_easy_cleanup(exchange->curl);
	curl_slist_free_all(exchange->headers);
	free(exchange->response.data);
	free(exchange->body);
	free(exchange->auth);
	free(exchange->session);
	free(exchange->version);
	free(exchange);
}

static fy_generic
mcp_http_exchange_result(struct mcp_http_exchange *exchange)
{
	struct fyai_ctx *ctx;
	fy_generic doc;
	fy_generic error;
	fy_generic out;
	const char *json;
	const char *data;
	const char *end;
	char *sse_json;

	ctx = exchange->ctx;
	if (exchange->code != CURLE_OK ||
	    exchange->status < 200 || exchange->status >= 300)
		return fy_invalid;
	if (exchange->notification)
		return fy_null;
	json = exchange->response.data ? exchange->response.data : "";
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
			 "MCP %s returned invalid JSON", exchange->method);
	error = fy_get(doc, "error");
	fyai_error_check(ctx, fy_generic_is_invalid(error), err_out,
			 "MCP %s: %s", exchange->method,
			 fy_get(error, "message", "server error"));
	out = fy_get(doc, "result", fy_invalid);
	fyai_error_check(ctx, fy_generic_is_valid(out), err_out,
			 "MCP %s response has no result", exchange->method);
	return out;

err_out:
	return fy_invalid;
}

static fy_generic mcp_http_request_once(struct fyai_ctx *ctx,
				   struct fyai_mcp_ctx *mcp,
				   const char *method, fy_generic params,
				   bool notification, long long id,
				   CURLcode *rcp, long *statusp)
{
	struct mcp_http_exchange *exchange;
	struct fyai_event_loop *el;
	fy_generic out;
	CURLcode rc;
	long status;
	int step_rc;

	exchange = NULL;
	exchange = mcp_http_exchange_submit(ctx, mcp, method, params,
					    notification, id, NULL, NULL);
	fyai_error_check(ctx, exchange, err_out,
			 "MCP %s could not submit request", method);
	el = fyai_ctx_loop(ctx);
	fyai_error_check(ctx, el, err_destroy,
			 "MCP %s request requires an event loop", method);
	while (!exchange->done) {
		step_rc = fyai_event_loop_step(el, -1);
		fyai_error_check(ctx, step_rc >= 0, err_destroy,
				 "MCP %s event loop failed", method);
	}
	rc = exchange->code;
	status = exchange->status;
	*rcp = rc;
	*statusp = status;
	if (rc != CURLE_OK || status < 200 || status >= 300)
		goto err_destroy;

	if (notification) {
		mcp_http_exchange_destroy(exchange);
		return fy_null;
	}
	out = mcp_http_exchange_result(exchange);
	fyai_error_check(ctx, fy_generic_is_valid(out), err_destroy,
			 "MCP %s response has no result", method);
	mcp_http_exchange_destroy(exchange);
	return out;

err_destroy:
	mcp_http_exchange_destroy(exchange);
err_out:
	return fy_invalid;
}

static int mcp_write_all(int fd, const char *data, size_t len)
{
	ssize_t n;

	while (len) {
		n = write(fd, data, len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		data += n;
		len -= (size_t)n;
	}
	return 0;
}

static void mcp_log_stdio_request(struct fyai_ctx *ctx,
				  struct fyai_mcp_ctx *mcp,
				  const char *method, long long id,
				  bool notification, bool success,
				  const struct timespec *start)
{
	struct timespec end;
	double elapsed_ms;

	if (!ctx->cfg->mcp_logging)
		return;
	clock_gettime(CLOCK_MONOTONIC, &end);
	elapsed_ms = (double)(end.tv_sec - start->tv_sec) * 1000.0 +
		(double)(end.tv_nsec - start->tv_nsec) / 1000000.0;
	(void)fyai_log_generic(ctx, "mcp", fy_null_filtered_mapping(
		"event", "request", "server", mcp->name,
		"transport", "stdio", "method", method,
		"id", notification ? fy_null : fy_value(id),
		"notification", notification, "success", success,
		"elapsed_ms", elapsed_ms));
}

static fy_generic mcp_stdio_request_once(struct fyai_ctx *ctx,
					 struct fyai_mcp_ctx *mcp,
					 const char *method, fy_generic params,
					 bool notification, long long id)
{
	struct response_buffer response = {};
	struct pollfd pfd = { .fd = mcp->stdout_fd, .events = POLLIN };
	fy_generic request, doc, error, out;
	const char *body;
	char ch;
	ssize_t n;
	int rc;
	struct timespec start;

	if (ctx->cfg->mcp_logging)
		clock_gettime(CLOCK_MONOTONIC, &start);

	request = fy_mapping(ctx->transient_gb, "jsonrpc", "2.0",
			     "method", method, "params", params);
	if (!notification)
		request = fy_assoc(ctx->transient_gb, request, "id", id);
	body = emit_request_body(ctx->transient_gb, request);
	rc = body ? mcp_write_all(mcp->stdin_fd, body, strlen(body)) : -1;
	if (!rc)
		rc = mcp_write_all(mcp->stdin_fd, "\n", 1);
	fyai_error_check(ctx, !rc, err_out,
			 "MCP %s stdio write failed", method);
	if (notification) {
		mcp_log_stdio_request(ctx, mcp, method, id, true, true, &start);
		return fy_null;
	}

	for (;;) {
		response.len = 0;
		if (response.data)
			response.data[0] = '\0';
		for (;;) {
			rc = poll(&pfd, 1, mcp->timeout * 1000);
			if (rc < 0 && errno == EINTR)
				continue;
			fyai_error_check(ctx, rc > 0 &&
					 (pfd.revents & (POLLIN | POLLHUP)), err_out,
					 "MCP %s stdio response timed out", method);
			n = read(mcp->stdout_fd, &ch, 1);
			fyai_error_check(ctx, n > 0, err_out,
					 "MCP %s stdio server closed", method);
			if (ch == '\n')
				break;
			rc = response_buffer_reserve(&response, response.len + 2);
			fyai_error_check(ctx, !rc,
					 err_out, "MCP %s response is too large", method);
			response.data[response.len++] = ch;
			response.data[response.len] = '\0';
		}
		if (!response.len)
			continue;
		doc = parse_json_string(ctx->transient_gb, response.data);
		fyai_error_check(ctx, fy_generic_is_mapping(doc), err_out,
				 "MCP %s returned invalid stdio JSON", method);
		/* Ignore asynchronous notifications while waiting for our response. */
		if (fy_generic_is_invalid(fy_get(doc, "id", fy_invalid)))
			continue;
		if (fy_get(doc, "id", -1LL) != id)
			continue;
		error = fy_get(doc, "error", fy_invalid);
		fyai_error_check(ctx, fy_generic_is_invalid(error), err_out,
				 "MCP %s: %s", method,
				 fy_get(error, "message", "server error"));
		/* done */
		break;
	}

	free(response.data);
	response.data = NULL;

	out = fy_get(doc, "result", fy_invalid);
	fyai_error_check(ctx, fy_generic_is_valid(out), err_out,
			 "MCP %s response has no result", method);
	mcp_log_stdio_request(ctx, mcp, method, id, false, true, &start);

	return out;

err_out:
	free(response.data);
	mcp_log_stdio_request(ctx, mcp, method, id, notification, false, &start);
	return fy_invalid;
}

static fy_generic mcp_request(struct fyai_ctx *ctx, struct fyai_mcp_ctx *mcp,
			      const char *method,
			      fy_generic params, bool notification)
{
	fy_generic result;
	CURLcode rc = CURLE_OK;
	long status = 0;
	long long id = notification ? 0 : ++mcp->request_id;
	struct timespec delay = { .tv_nsec = 100000000 };
	unsigned int attempt;

	if (mcp->transport == MCP_TRANSPORT_STDIO)
		return mcp_stdio_request_once(ctx, mcp, method, params,
					      notification, id);

	for (attempt = 0; attempt < 3; attempt++) {
		result = mcp_http_request_once(ctx, mcp, method, params, notification,
					  id, &rc, &status);
		if (fy_generic_is_valid(result))
			return result;
		if (status == 404 && mcp->session_id && !mcp->recovering &&
		    strcmp(method, "initialize")) {
			mcp->session_id = NULL;
			mcp->recovering = true;
			if (!mcp_initialize(ctx, mcp)) {
				mcp->recovering = false;
				result = mcp_http_request_once(ctx, mcp, method, params,
						notification, id, &rc, &status);
				if (fy_generic_is_valid(result))
					return result;
			} else {
				mcp->recovering = false;
			}
			break;
		}
		if (!mcp_idempotent_method(method) ||
		    !mcp_transient_status(rc, status) || attempt == 2)
			break;
		nanosleep(&delay, NULL);
		delay.tv_nsec *= 2;
	}
	fyai_error(ctx, "MCP %s failed: %s (HTTP %ld)", method,
		   curl_easy_strerror(rc), status);
	return fy_invalid;
}

/*
 * Shared asynchronous request primitive
 * =====================================
 *
 * One JSON-RPC request to one server over its transport, completing through a
 * callback with the parsed `result` generic (or a failure). HTTP wraps the curl
 * exchange; stdio drives nonblocking read/write/timeout event sources with
 * newline framing and id matching. Both the tool-call op and server startup are
 * sequences of these, so the request mechanics live here once.
 */
struct mcp_req;
typedef void (*mcp_req_complete_fn)(struct mcp_req *req, void *userdata);

struct mcp_req {
	struct fyai_ctx *ctx;
	struct fyai_mcp_ctx *mcp;
	mcp_req_complete_fn complete;
	void *userdata;
	const char *method;
	long long id;
	bool notification;

	/* HTTP backend. */
	struct mcp_http_exchange *exchange;

	/* stdio backend. */
	struct fyai_event_source *read_src;
	struct fyai_event_source *write_src;
	struct fyai_event_source *timer_src;
	struct response_buffer stdio_response;
	char *stdio_request;
	size_t stdio_request_len;
	size_t stdio_request_off;
	struct timespec started;

	fy_generic result;
	long status;
	CURLcode code;
	bool done;
	bool ok;
	bool cancel_requested;
};

static void mcp_req_destroy(struct mcp_req *req);

static void mcp_req_drop_source(struct fyai_event_source **srcp)
{
	fyai_event_source_remove(*srcp);
	*srcp = NULL;
}

static void mcp_req_stdio_sources_drop(struct mcp_req *req)
{
	mcp_req_drop_source(&req->read_src);
	mcp_req_drop_source(&req->write_src);
	mcp_req_drop_source(&req->timer_src);
}

/* Latch completion once and hand the result to the owner. */
static void mcp_req_finish(struct mcp_req *req, bool ok, fy_generic result,
			   long status, CURLcode code)
{
	if (req->done)
		return;
	mcp_req_stdio_sources_drop(req);
	req->result = result;
	req->ok = ok;
	req->status = status;
	req->code = code;
	req->done = true;
	if (req->mcp->transport == MCP_TRANSPORT_STDIO)
		mcp_log_stdio_request(req->ctx, req->mcp, req->method, req->id,
				      req->notification, ok, &req->started);
	if (req->complete)
		req->complete(req, req->userdata);
}

/* Match one framed stdio line against this request. Returns true once the
 * request is finished (matched response, or a fatal parse/transport error). */
static bool mcp_req_stdio_line(struct mcp_req *req)
{
	struct fyai_ctx *ctx = req->ctx;
	fy_generic doc, error, result;

	if (!req->stdio_response.len)
		return false;
	doc = parse_json_string(ctx->transient_gb, req->stdio_response.data);
	if (!fy_generic_is_mapping(doc)) {
		fyai_error(ctx, "MCP %s returned invalid stdio JSON", req->method);
		mcp_req_finish(req, false, fy_invalid, 0, CURLE_OK);
		return true;
	}
	/* Ignore asynchronous notifications while waiting for our response. */
	if (fy_generic_is_invalid(fy_get(doc, "id", fy_invalid)))
		return false;
	if (fy_get(doc, "id", -1LL) != req->id)
		return false;
	error = fy_get(doc, "error", fy_invalid);
	if (fy_generic_is_valid(error)) {
		fyai_error(ctx, "MCP %s: %s", req->method,
			   fy_get(error, "message", "server error"));
		mcp_req_finish(req, false, fy_invalid, 0, CURLE_OK);
		return true;
	}
	result = fy_get(doc, "result", fy_invalid);
	if (fy_generic_is_invalid(result)) {
		fyai_error(ctx, "MCP %s response has no result", req->method);
		mcp_req_finish(req, false, fy_invalid, 0, CURLE_OK);
		return true;
	}
	mcp_req_finish(req, true, result, 0, CURLE_OK);
	return true;
}

static enum fyai_event_action mcp_req_stdio_readable(const struct fyai_event *ev)
{
	struct mcp_req *req = ev->userdata;
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
				fyai_error(req->ctx, "MCP %s response is too large",
					   req->method);
				mcp_req_finish(req, false, fy_invalid, 0, CURLE_OK);
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
				if (mcp_req_stdio_line(req))
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
	fyai_error(req->ctx, "MCP %s stdio server closed", req->method);
	mcp_req_finish(req, false, fy_invalid, 0, CURLE_OK);
	return FYAIEA_CONTINUE;
}

static enum fyai_event_action mcp_req_stdio_writable(const struct fyai_event *ev)
{
	struct mcp_req *req = ev->userdata;
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
		fyai_error(req->ctx, "MCP %s stdio write failed", req->method);
		mcp_req_finish(req, false, fy_invalid, 0, CURLE_OK);
		return FYAIEA_CONTINUE;
	}
	mcp_req_drop_source(&req->write_src);
	/* A notification is complete once written; no response is expected. */
	if (req->notification)
		mcp_req_finish(req, true, fy_null, 0, CURLE_OK);
	return FYAIEA_CONTINUE;
}

static enum fyai_event_action mcp_req_stdio_timeout(const struct fyai_event *ev)
{
	struct mcp_req *req = ev->userdata;

	req->timer_src = NULL;
	fyai_error(req->ctx, "MCP %s stdio response timed out", req->method);
	mcp_req_finish(req, false, fy_invalid, 0, CURLE_OK);
	return FYAIEA_CONTINUE;
}

static int mcp_req_stdio_submit(struct mcp_req *req, fy_generic params)
{
	struct fyai_ctx *ctx = req->ctx;
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
			 "could not encode MCP stdio request");
	body_len = strlen(body);
	req->stdio_request = malloc(body_len + 2);
	fyai_error_check(ctx, req->stdio_request, err_out, "out of memory");
	memcpy(req->stdio_request, body, body_len);
	req->stdio_request[body_len] = '\n';
	req->stdio_request[body_len + 1] = '\0';
	req->stdio_request_len = body_len + 1;

	flags = fcntl(req->mcp->stdin_fd, F_GETFL);
	fyai_error_check(ctx, flags >= 0, err_out,
			 "could not inspect MCP stdin: %s", strerror(errno));
	rc = fcntl(req->mcp->stdin_fd, F_SETFL, flags | O_NONBLOCK);
	fyai_error_check(ctx, !rc, err_out,
			 "could not make MCP stdin non-blocking: %s",
			 strerror(errno));
	rc = fyai_event_add_fd(el, req->mcp->stdin_fd, FYAIEV_WRITE,
			       mcp_req_stdio_writable, req, &req->write_src);
	fyai_error_check(ctx, !rc, err_out, "could not watch MCP stdin");
	if (req->notification)
		return 0;

	flags = fcntl(req->mcp->stdout_fd, F_GETFL);
	fyai_error_check(ctx, flags >= 0, err_out,
			 "could not inspect MCP stdout: %s", strerror(errno));
	rc = fcntl(req->mcp->stdout_fd, F_SETFL, flags | O_NONBLOCK);
	fyai_error_check(ctx, !rc, err_out,
			 "could not make MCP stdout non-blocking: %s",
			 strerror(errno));
	rc = fyai_event_add_fd(el, req->mcp->stdout_fd, FYAIEV_READ,
			       mcp_req_stdio_readable, req, &req->read_src);
	fyai_error_check(ctx, !rc, err_out, "could not watch MCP stdout");
	rc = fyai_event_add_timer(el, req->mcp->timeout * 1000, 0,
				  mcp_req_stdio_timeout, req, &req->timer_src);
	fyai_error_check(ctx, !rc, err_out, "could not arm MCP response timeout");
	return 0;

err_out:
	mcp_req_stdio_sources_drop(req);
	return -1;
}

static void mcp_req_http_complete(struct mcp_http_exchange *exchange,
				  void *userdata)
{
	struct mcp_req *req = userdata;
	fy_generic result;
	CURLcode code = exchange->code;
	long status = exchange->status;

	result = mcp_http_exchange_result(exchange);
	mcp_http_exchange_destroy(exchange);
	req->exchange = NULL;
	if (req->cancel_requested)
		mcp_req_finish(req, false, fy_invalid, status, code);
	else
		mcp_req_finish(req, fy_generic_is_valid(result), result,
			       status, code);
}

static struct mcp_req *
mcp_req_submit(struct fyai_ctx *ctx, struct fyai_mcp_ctx *mcp,
	       const char *method, fy_generic params, bool notification,
	       long long id, mcp_req_complete_fn complete, void *userdata)
{
	struct mcp_req *req;
	int rc;

	req = calloc(1, sizeof(*req));
	fyai_error_check(ctx, req, err_out, "out of memory");
	req->ctx = ctx;
	req->mcp = mcp;
	req->method = method;
	req->id = id;
	req->notification = notification;
	req->complete = complete;
	req->userdata = userdata;
	req->result = fy_invalid;
	req->code = CURLE_OK;

	if (mcp->transport == MCP_TRANSPORT_STDIO) {
		clock_gettime(CLOCK_MONOTONIC, &req->started);
		rc = mcp_req_stdio_submit(req, params);
	} else {
		req->exchange = mcp_http_exchange_submit(ctx, mcp, method,
				params, notification, id, mcp_req_http_complete,
				req);
		rc = req->exchange ? 0 : -1;
	}
	fyai_error_check(ctx, !rc, err_free, "MCP %s could not submit %s",
			 mcp->name, method);
	return req;

err_free:
	mcp_req_destroy(req);
err_out:
	return NULL;
}

static void mcp_req_cancel(struct mcp_req *req)
{
	if (!req || req->done)
		return;
	req->cancel_requested = true;
	if (req->mcp->transport == MCP_TRANSPORT_STDIO)
		mcp_req_finish(req, false, fy_invalid, 0, CURLE_OK);
	else if (req->exchange && req->exchange->transfer)
		fyai_curl_cancel(req->exchange->transfer);
}

static void mcp_req_destroy(struct mcp_req *req)
{
	if (!req)
		return;
	mcp_req_stdio_sources_drop(req);
	mcp_http_exchange_destroy(req->exchange);
	free(req->stdio_response.data);
	free(req->stdio_request);
	free(req);
}

bool fyai_mcp_tool_name(const char *name)
{
	return name && !strncmp(name, MCP_PREFIX, sizeof(MCP_PREFIX) - 1) &&
		strstr(name + sizeof(MCP_PREFIX) - 1, "__");
}

fy_generic fyai_mcp_tools(struct fyai_ctx *ctx)
{
	return fy_generic_is_valid(ctx->mcp_tools) ? ctx->mcp_tools : fy_seq_empty;
}

static bool mcp_server_name_valid(const char *name)
{
	const unsigned char *p = (const unsigned char *)name;

	if (!p || !*p)
		return false;
	for (; *p; p++)
		if (!isalnum(*p) && *p != '_' && *p != '-')
			return false;
	return true;
}

static const char *mcp_server_auth_token(struct fyai_ctx *ctx,
					 fy_generic config)
{
	fy_generic ref = fy_get(config, "auth_token", fy_invalid);
	const char *type, *value, *token;

	if (fy_generic_is_invalid(ref))
		return ctx->cfg->mcp_auth_token;
	fyai_error_check(ctx, fy_generic_is_mapping(ref), err_out,
			 "MCP auth_token must be a secret indirection mapping");
	type = fy_get(ref, "type", "");
	if (!strcmp(type, "auto"))
		return NULL;
	fyai_error_check(ctx, !strcmp(type, "env"), err_out,
			 "MCP auth_token has unsupported secret type '%s'", type);
	value = fy_get(ref, "value", "");
	token = *value ? getenv(value) : NULL;
	fyai_error_check(ctx, token && *token, err_out,
			 "MCP auth_token environment variable '%s' is unset", value);
	return token;
err_out:
	return NULL;
}

static int mcp_initialize(struct fyai_ctx *ctx, struct fyai_mcp_ctx *mcp)
{
	fy_generic result;
	const char *protocol;

	result = mcp_request(ctx, mcp, "initialize", fy_mapping(ctx->transient_gb,
			"protocolVersion", mcp->protocol_version,
			"capabilities", fy_map_empty,
			"clientInfo", fy_mapping("name", "fyai",
						 "version", VERSION)), false);
	fyai_error_check(ctx, fy_generic_is_valid(result), err_out,
			 "MCP server '%s' initialization failed", mcp->name);
	protocol = fy_get(result, "protocolVersion", "");
	if (*protocol) {
		protocol = fy_gb_intern_string(ctx->cfg->gb, protocol);
		fyai_error_check(ctx, protocol, err_out,
				 "MCP server '%s' could not retain protocol version",
				 mcp->name);
		mcp->protocol_version = protocol;
	}
	result = mcp_request(ctx, mcp, "notifications/initialized",
			     fy_map_empty, true);
	fyai_error_check(ctx, fy_generic_is_valid(result), err_out,
			 "MCP server '%s' initialization notification failed",
			 mcp->name);
	return 0;
err_out:
	return -1;
}

static int mcp_discover_tools(struct fyai_ctx *ctx, struct fyai_mcp_ctx *mcp,
			      fy_generic *toolsp)
{
	fy_generic result, tools, tool, params = fy_map_empty;
	const char *tool_name, *cursor;
	long long count = 0;

	do {
		result = mcp_request(ctx, mcp, "tools/list", params, false);
		fyai_error_check(ctx, fy_generic_is_valid(result), err_out,
				 "MCP server '%s' tool discovery failed", mcp->name);
		tools = fy_get(result, "tools", fy_seq_empty);
		fy_foreach(tool, tools) {
			tool_name = fy_get(tool, "name", "");
			if (!*tool_name)
				continue;
			*toolsp = fy_append(ctx->gb, *toolsp, fy_mapping(ctx->gb,
				"type", "function", "function", fy_mapping(
					"name", fy_sprintfa(MCP_PREFIX "%s__%s",
							mcp->name, tool_name),
					"description", fy_get(tool, "description", ""),
					"parameters", fy_get(tool, "inputSchema",
							     fy_mapping("type", "object")))));
			fyai_error_check(ctx, fy_generic_is_valid(*toolsp), err_out,
					 "MCP server '%s' could not retain tool '%s'",
					 mcp->name, tool_name);
			count++;
		}
		cursor = fy_get(result, "nextCursor", "");
		params = *cursor ? fy_mapping(ctx->transient_gb, "cursor", cursor) :
			fy_map_empty;
	} while (*cursor);
	if (ctx->cfg->mcp_logging)
		(void)fyai_log_generic(ctx, "mcp", fy_mapping(
			"event", "discovery", "server", mcp->name,
			"tools", count));
	return 0;
err_out:
	return -1;
}

static int mcp_stdio_spawn(struct fyai_ctx *ctx, struct fyai_mcp_ctx *mcp,
			   fy_generic config)
{
	int inpipe[2] = { -1, -1 }, outpipe[2] = { -1, -1 };
	fy_generic args, env;
	fy_generic item, key;
	const char *command, *cwd, *name, *value;
	char **argv;
	pid_t pid;
	size_t i, argc;
	int rc;

	args = fy_get(config, "args", fy_seq_empty);
	env = fy_get(config, "env", fy_invalid);
	command = fy_get(config, "command", "");
	cwd = fy_get(config, "cwd", "");
	fyai_error_check(ctx, fy_generic_is_sequence(args), err_out,
			 "MCP stdio server '%s' args must be a sequence", mcp->name);
	fyai_error_check(ctx, fy_generic_is_invalid(env) ||
			 fy_generic_is_mapping(env), err_out,
			 "MCP stdio server '%s' env must be a mapping", mcp->name);

	argc = fy_len(args);
	fyai_error_check(ctx, *command, err_out,
			 "MCP stdio server '%s' has no command", mcp->name);

	argv = alloca((argc + 2) * sizeof(*argv));

	i = 0;
	argv[i++] = (char *)command;
	fy_foreach(item, args)
		argv[i++] = fy_castp(&item, "");
	argv[i] = NULL;

	rc = pipe(inpipe);
	if (!rc)
		rc = pipe(outpipe);
	fyai_error_check(ctx, !rc, err_out,
			 "MCP stdio server '%s' could not create pipes: %s",
			 mcp->name, strerror(errno));
	rc = fcntl(inpipe[0], F_SETFD, FD_CLOEXEC);
	if (!rc)
		rc = fcntl(inpipe[1], F_SETFD, FD_CLOEXEC);
	if (!rc)
		rc = fcntl(outpipe[0], F_SETFD, FD_CLOEXEC);
	if (!rc)
		rc = fcntl(outpipe[1], F_SETFD, FD_CLOEXEC);
	fyai_error_check(ctx, !rc, err_out,
			 "MCP stdio server '%s' could not protect pipes: %s",
			 mcp->name, strerror(errno));

	pid = fork();
	fyai_error_check(ctx, pid >= 0, err_out,
			 "MCP stdio server '%s' could not fork: %s",
			 mcp->name, strerror(errno));

	if (!pid) {
		fyai_ctx_loop_abandon(ctx);
		signal(SIGPIPE, SIG_DFL);
		close(inpipe[1]);
		close(outpipe[0]);
		inpipe[1] = outpipe[0] = -1;

		if (dup2(inpipe[0], STDIN_FILENO) < 0 ||
		    dup2(outpipe[1], STDOUT_FILENO) < 0)
			_exit(126);

		close(inpipe[0]);
		close(outpipe[1]);
		inpipe[0] = outpipe[1] = -1;

		if (*cwd && chdir(cwd))
			_exit(126);

		if (fy_generic_is_mapping(env)) {
			fy_foreach(key, env) {
				name = fy_castp(&key, "");
				value = fy_get(env, key, "");

				if (setenv(name, value, 1))
					_exit(126);
			}
		}
		execvp(command, argv);
		_exit(127);
	}
	close(inpipe[0]);
	close(outpipe[1]);
	inpipe[0] = outpipe[1] = -1;

	mcp->pid = pid;
	mcp->stdin_fd = inpipe[1];
	mcp->stdout_fd = outpipe[0];
	return 0;

err_out:
	for (i = 0; i < ARRAY_SIZE(inpipe); i++) {
		if (inpipe[i] >= 0)
			close(inpipe[i]);
	}
	for (i = 0; i < ARRAY_SIZE(outpipe); i++) {
		if (outpipe[i] >= 0)
			close(outpipe[i]);
	}
	return -1;
}

static int mcp_add_server(struct fyai_ctx *ctx, const char *server_name,
			  fy_generic config, fy_generic *toolsp)
{
	struct fyai_mcp_ctx *mcp = NULL, **tail, *prev;
	fy_generic auth_ref;
	const char *endpoint, *protocol, *token, *transport, *command;
	bool enabled;
	int rc;

	enabled = fy_get(config, "enabled", true);
	if (!enabled)
		return 0;

	fyai_error_check(ctx, mcp_server_name_valid(server_name), err_out,
			 "invalid MCP server name '%s'", server_name);
	command = fy_get(config, "command", "");
	transport = fy_get(config, "transport", *command ? "stdio" :
						       "streamable-http");
	fyai_error_check(ctx, !strcmp(transport, "stdio") ||
			 !strcmp(transport, "streamable-http") ||
			 !strcmp(transport, "http"), err_out,
			 "MCP server '%s' has unknown transport '%s'",
			 server_name, transport);
	endpoint = fy_get(config, "endpoint", "");
	fyai_error_check(ctx, !strcmp(transport, "stdio") || *endpoint, err_out,
			 "MCP HTTP server '%s' has no endpoint", server_name);
	protocol = fy_get(config, "protocol_version",
			  ctx->cfg->mcp_protocol_version);
	auth_ref = fy_get(config, "auth_token", fy_invalid);
	token = !strcmp(transport, "stdio") ? NULL :
		mcp_server_auth_token(ctx, config);
	fyai_error_check(ctx, !strcmp(transport, "stdio") ||
			 fy_generic_is_invalid(auth_ref) || token ||
			 fy_equal(fy_get(auth_ref, "type"), "auto"), err_out,
			 "MCP server '%s' could not resolve auth token", server_name);

	mcp = calloc(1, sizeof(*mcp));
	fyai_error_check(ctx, mcp, err_out,
			 "MCP server '%s' could not allocate context", server_name);
	mcp->pid = -1;
	mcp->stdin_fd = -1;
	mcp->stdout_fd = -1;
	mcp->transport = !strcmp(transport, "stdio") ? MCP_TRANSPORT_STDIO :
		MCP_TRANSPORT_HTTP;
	mcp->name = fy_gb_intern_string(ctx->cfg->gb, server_name);
	if (*endpoint)
		mcp->endpoint = fy_gb_intern_string(ctx->cfg->gb, endpoint);
	mcp->protocol_version = fy_gb_intern_string(ctx->cfg->gb,
						 protocol);
	if (token)
		mcp->auth_token = fy_gb_intern_string(ctx->cfg->gb, token);
	mcp->timeout = fy_get(config, "timeout", ctx->cfg->mcp_timeout);
	if (mcp->timeout <= 0)
		mcp->timeout = ctx->cfg->mcp_timeout;

	if (mcp->transport == MCP_TRANSPORT_HTTP)
		mcp->curl = curl_easy_init();

	fyai_error_check(ctx, mcp->name && mcp->protocol_version &&
			 (!token || mcp->auth_token) &&
			 (mcp->transport != MCP_TRANSPORT_HTTP ||
			  (mcp->endpoint && mcp->curl)), err_out,
			 "MCP server '%s' could not initialize context", server_name);

	rc = mcp->transport == MCP_TRANSPORT_STDIO ?
		mcp_stdio_spawn(ctx, mcp, config) : 0;
	fyai_error_check(ctx, !rc, err_out,
			 "MCP stdio server '%s' could not start", server_name);

	tail = &ctx->mcp;
	while (*tail)
		tail = &(*tail)->next;
	*tail = mcp;

	if (ctx->cfg->mcp_logging)
		(void)fyai_log_generic(ctx, "mcp", fy_mapping(
			"event", "connect", "server", mcp->name,
			"transport", mcp->transport == MCP_TRANSPORT_STDIO ?
				"stdio" : "streamable-http"));

	rc = mcp_initialize(ctx, mcp);
	fyai_error_check(ctx, !rc, err_out,
			 "MCP server '%s' initialization failed", server_name);

	rc = mcp_discover_tools(ctx, mcp, toolsp);
	fyai_error_check(ctx, !rc, err_out,
			 "MCP server '%s' discovery failed", server_name);

	return 0;

err_out:
	if (!mcp)
		return -1;
	if (ctx->mcp == mcp)
		ctx->mcp = mcp->next;
	else {
		for (prev = ctx->mcp; prev && prev->next != mcp;
		     prev = prev->next)
			;
		if (prev)
			prev->next = mcp->next;
	}
	if (mcp->curl)
		curl_easy_cleanup(mcp->curl);
	mcp_stdio_stop(ctx, mcp);
	free(mcp);
	return -1;
}

int fyai_mcp_refresh(struct fyai_ctx *ctx)
{
	fy_generic servers = ctx->cfg->mcp_servers;
	fy_generic key, config, out = fy_seq_empty;
	const char *name;
	int rc;

	if (ctx->mcp && fy_generic_is_valid(ctx->mcp_tools)) {
		if (ctx->cfg->mcp_logging)
			(void)fyai_log_generic(ctx, "mcp", fy_mapping(
				"event", "reuse", "tools",
				(long long)fy_len(ctx->mcp_tools)));
		return 0;
	}
	fyai_mcp_cleanup(ctx);
	ctx->mcp_tools = fy_invalid;
	if (fy_generic_is_mapping(servers) && fy_len(servers)) {
		fy_foreach(key, servers) {
			name = fy_castp(&key, "");
			config = fy_get(servers, key, fy_invalid);
			fyai_error_check(ctx, fy_generic_is_mapping(config), err_out,
					 "MCP server '%s' configuration is not a mapping",
					 name);
			rc = mcp_add_server(ctx, name, config, &out);
			fyai_error_check(ctx, !rc,
					 err_out, "MCP server '%s' setup failed", name);
		}
	} else {
		fyai_error_check(ctx, ctx->cfg->mcp_endpoint &&
				 *ctx->cfg->mcp_endpoint, err_out,
				 "MCP is enabled but no servers are configured");
		config = fy_mapping(ctx->transient_gb,
			"endpoint", ctx->cfg->mcp_endpoint,
			"protocol_version", ctx->cfg->mcp_protocol_version,
			"timeout", ctx->cfg->mcp_timeout);

		rc = mcp_add_server(ctx, "default", config, &out);
		fyai_error_check(ctx, !rc,
				 err_out, "default MCP server setup failed");
	}

	ctx->mcp_tools = fy_gb_internalize(ctx->gb, out);
	fyai_error_check(ctx, fy_generic_is_valid(ctx->mcp_tools), err_out,
			 "could not retain MCP tool definitions");

	return 0;
err_out:
	fyai_mcp_cleanup(ctx);
	ctx->mcp_tools = fy_invalid;
	return -1;
}

enum fyai_mcp_call_state {
	FYAIMCS_PRIMARY,
	FYAIMCS_RECOVER_INITIALIZE,
	FYAIMCS_RECOVER_NOTIFY,
	FYAIMCS_COMPLETED,
	FYAIMCS_CANCELLED,
	FYAIMCS_FAILED,
};

struct fyai_mcp_call_request {
	struct fyai_ctx *ctx;
	struct fyai_mcp_ctx *mcp;
	struct mcp_req *req;
	fyai_mcp_call_complete_fn complete;
	void *userdata;
	char *tool_name;
	char *params_text;
	long long id;			/* the tools/call id, reused across retry */
	fy_generic result;
	enum fyai_mcp_call_state state;
	bool result_ok;
	bool cancel_requested;
};

static bool
fyai_mcp_call_state_final(enum fyai_mcp_call_state state)
{
	return state == FYAIMCS_COMPLETED ||
	       state == FYAIMCS_CANCELLED ||
	       state == FYAIMCS_FAILED;
}

static struct fyai_mcp_ctx *
mcp_find_tool_server(struct fyai_ctx *ctx, const char *name,
		     const char **tool_namep)
{
	struct fyai_mcp_ctx *mcp;
	const char *server_name;
	const char *sep;
	size_t server_len;

	server_name = name + sizeof(MCP_PREFIX) - 1;
	sep = strstr(server_name, "__");
	if (!sep)
		return NULL;
	server_len = (size_t)(sep - server_name);
	for (mcp = ctx->mcp; mcp; mcp = mcp->next)
		if (strlen(mcp->name) == server_len &&
		    !strncmp(mcp->name, server_name, server_len))
			break;
	if (mcp && tool_namep)
		*tool_namep = sep + 2;
	return mcp;
}

static fy_generic
mcp_tool_output(struct fyai_ctx *ctx, fy_generic result)
{
	fy_generic content;
	fy_generic item;
	fy_generic output;
	const char *text;

	content = fy_get(result, "content", fy_seq_empty);
	output = fy_seq_empty;
	fy_foreach(item, content) {
		text = fy_get(item, "text", "");
		if (*text)
			output = fy_append(ctx->transient_gb, output, text);
	}
	if (fy_len(output) == 1)
		return fy_get(output, 0);
	if (fy_len(output))
		return output;
	return fy_gb_internalize(ctx->transient_gb, result);
}

static void
fyai_mcp_call_finish(struct fyai_mcp_call_request *request,
		     enum fyai_mcp_call_state state,
		     fy_generic result, bool ok)
{
	request->state = state;
	request->result = result;
	request->result_ok = ok;
	if (request->complete)
		request->complete(request, request->userdata);
}

static void fyai_mcp_call_req_done(struct mcp_req *req, void *userdata);

/* Submit the request for the current call phase. */
static int
fyai_mcp_call_submit_phase(struct fyai_mcp_call_request *request)
{
	struct fyai_ctx *ctx = request->ctx;
	const char *method;
	fy_generic params;
	long long id;
	bool notification = false;

	switch (request->state) {
	case FYAIMCS_PRIMARY:
		method = "tools/call";
		id = request->id;
		params = parse_json_string(ctx->transient_gb,
					   request->params_text);
		break;
	case FYAIMCS_RECOVER_INITIALIZE:
		method = "initialize";
		id = ++request->mcp->request_id;
		params = fy_mapping(ctx->transient_gb,
			"protocolVersion", request->mcp->protocol_version,
			"capabilities", fy_map_empty,
			"clientInfo", fy_mapping("name", "fyai",
						 "version", VERSION));
		break;
	case FYAIMCS_RECOVER_NOTIFY:
		method = "notifications/initialized";
		id = 0;
		notification = true;
		params = fy_map_empty;
		break;
	default:
		return -1;
	}
	fyai_error_check(ctx, fy_generic_is_valid(params), err_out,
			 "MCP call parameters are invalid");
	request->req = mcp_req_submit(ctx, request->mcp, method, params,
				      notification, id, fyai_mcp_call_req_done,
				      request);
	fyai_error_check(ctx, request->req, err_out,
			 "MCP %s could not submit %s", request->mcp->name,
			 method);
	return 0;

err_out:
	return -1;
}

/*
 * One request phase completed. Advance the tool-call state machine: deliver the
 * tool output, walk the session-recovery handshake, or fail. HTTP 404 with a
 * live session triggers reinitialize-then-retry; stdio never 404s, so it only
 * ever runs the PRIMARY phase.
 */
static void fyai_mcp_call_req_done(struct mcp_req *req, void *userdata)
{
	struct fyai_mcp_call_request *request = userdata;
	struct fyai_ctx *ctx = request->ctx;
	fy_generic result = req->result;
	bool ok = req->ok;
	long status = req->status;
	CURLcode code = req->code;
	const char *protocol;

	mcp_req_destroy(req);
	request->req = NULL;

	if (request->cancel_requested) {
		fyai_mcp_call_finish(request, FYAIMCS_CANCELLED,
			fy_value(ctx->transient_gb, "tool error: interrupted"),
			false);
		return;
	}
	if (ok) {
		switch (request->state) {
		case FYAIMCS_PRIMARY:
			result = mcp_tool_output(ctx, result);
			fyai_mcp_call_finish(request, FYAIMCS_COMPLETED,
					     result, true);
			return;
		case FYAIMCS_RECOVER_INITIALIZE:
			protocol = fy_get(result, "protocolVersion", "");
			if (*protocol) {
				protocol = fy_gb_intern_string(ctx->cfg->gb,
							       protocol);
				if (!protocol)
					goto failed;
				request->mcp->protocol_version = protocol;
			}
			request->state = FYAIMCS_RECOVER_NOTIFY;
			if (!fyai_mcp_call_submit_phase(request))
				return;
			goto failed;
		case FYAIMCS_RECOVER_NOTIFY:
			request->state = FYAIMCS_PRIMARY;
			if (!fyai_mcp_call_submit_phase(request))
				return;
			goto failed;
		default:
			goto failed;
		}
	}
	if (request->state == FYAIMCS_PRIMARY && status == 404 &&
	    request->mcp->session_id) {
		request->mcp->session_id = NULL;
		request->state = FYAIMCS_RECOVER_INITIALIZE;
		if (!fyai_mcp_call_submit_phase(request))
			return;
	}
	if (request->mcp->transport == MCP_TRANSPORT_HTTP)
		fyai_error(ctx, "MCP %s failed: %s (HTTP %ld)",
			   request->mcp->name, curl_easy_strerror(code), status);
failed:
	fyai_mcp_call_finish(request, FYAIMCS_FAILED,
		fy_value(ctx->transient_gb, "tool error: MCP call failed"),
		false);
}

struct fyai_mcp_call_request *
fyai_mcp_call_submit(struct fyai_ctx *ctx, const char *name,
		     fy_generic args, fyai_mcp_call_complete_fn complete,
		     void *userdata)
{
	struct fyai_mcp_call_request *request;
	struct fyai_mcp_ctx *mcp;
	fy_generic params;
	const char *params_text;
	const char *tool_name;
	int rc;

	tool_name = NULL;
	mcp = mcp_find_tool_server(ctx, name, &tool_name);
	if (!mcp)
		return NULL;
	params = fy_mapping(ctx->transient_gb,
			    "name", tool_name, "arguments", args);
	params_text = emit_json_string(ctx->transient_gb, params);
	fyai_error_check(ctx, params_text, err_out,
			 "could not encode MCP call parameters");
	request = calloc(1, sizeof(*request));
	fyai_error_check(ctx, request, err_out, "out of memory");
	request->ctx = ctx;
	request->mcp = mcp;
	request->complete = complete;
	request->userdata = userdata;
	request->tool_name = strdup(tool_name);
	request->params_text = strdup(params_text);
	request->id = ++mcp->request_id;
	request->result = fy_invalid;
	request->state = FYAIMCS_PRIMARY;
	fyai_error_check(ctx, request->tool_name && request->params_text,
			 err_free, "out of memory");
	if (ctx->cfg->mcp_logging)
		(void)fyai_log_generic(ctx, "mcp", fy_mapping(
			"event", "tool_call", "server", mcp->name,
			"tool", tool_name));
	rc = fyai_mcp_call_submit_phase(request);
	fyai_error_check(ctx, !rc, err_free,
			 "could not submit MCP call");
	return request;

err_free:
	fyai_mcp_call_destroy(request);
err_out:
	return NULL;
}

void fyai_mcp_call_cancel(struct fyai_mcp_call_request *request)
{
	if (!request || fyai_mcp_call_state_final(request->state))
		return;
	request->cancel_requested = true;
	if (request->req)
		mcp_req_cancel(request->req);
}

bool fyai_mcp_call_done(const struct fyai_mcp_call_request *request)
{
	return request && fyai_mcp_call_state_final(request->state);
}

fy_generic fyai_mcp_call_collect(struct fyai_mcp_call_request *request,
				 bool *okp)
{
	if (!fyai_mcp_call_done(request) || !okp)
		return fy_invalid;
	*okp = request->result_ok;
	return request->result;
}

void fyai_mcp_call_destroy(struct fyai_mcp_call_request *request)
{
	if (!request)
		return;
	mcp_req_destroy(request->req);
	free(request->tool_name);
	free(request->params_text);
	free(request);
}

static void fyai_mcp_call_sync_done(struct fyai_mcp_call_request *request,
				    void *userdata)
{
	(void)request;
	*(volatile bool *)userdata = true;
}

/* Synchronous compatibility wrapper over the asynchronous call operation:
 * submit, pump the shared loop until it completes, collect. */
fy_generic fyai_mcp_call(struct fyai_ctx *ctx, const char *name,
			 fy_generic args)
{
	struct fyai_mcp_call_request *request;
	struct fyai_event_loop *el;
	volatile bool done = false;
	fy_generic result = fy_invalid;
	bool ok = false;
	int rc;

	request = fyai_mcp_call_submit(ctx, name, args,
				       fyai_mcp_call_sync_done, (void *)&done);
	if (!request)
		return fy_value(ctx->transient_gb, "tool error: MCP call failed");
	el = fyai_ctx_loop(ctx);
	while (el && !done) {
		rc = fyai_event_loop_step(el, -1);
		if (rc < 0) {
			fyai_mcp_call_cancel(request);
			break;
		}
	}
	if (fyai_mcp_call_done(request))
		result = fyai_mcp_call_collect(request, &ok);
	fyai_mcp_call_destroy(request);
	if (fy_generic_is_invalid(result))
		return fy_value(ctx->transient_gb, "tool error: MCP call failed");
	return result;
}

static void mcp_delete_session(struct fyai_ctx *ctx, struct fyai_mcp_ctx *mcp)
{
	struct response_buffer response = {};
	struct curl_slist *headers = NULL;
	char *auth = NULL, *session = NULL, *version = NULL;
	CURLcode rc;
	long status = 0;

	if (!mcp->curl || !mcp->session_id)
		return;

	version = make_header("MCP-Protocol-Version: ", mcp->protocol_version);
	session = make_header("Mcp-Session-Id: ", mcp->session_id);
	if (version)
		append_header(&headers, version);
	if (session)
		append_header(&headers, session);
	if (mcp->auth_token && *mcp->auth_token) {
		auth = make_header("Authorization: Bearer ", mcp->auth_token);
		if (auth)
			append_header(&headers, auth);
	}
	curl_easy_setopt(mcp->curl, CURLOPT_URL, mcp->endpoint);
	curl_easy_setopt(mcp->curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(mcp->curl, CURLOPT_CUSTOMREQUEST, "DELETE");
	/* A body-less DELETE. */
	curl_easy_setopt(mcp->curl, CURLOPT_POSTFIELDS, "");
	curl_easy_setopt(mcp->curl, CURLOPT_POSTFIELDSIZE, 0L);
	curl_easy_setopt(mcp->curl, CURLOPT_WRITEFUNCTION, write_response);
	curl_easy_setopt(mcp->curl, CURLOPT_WRITEDATA, &response);
	curl_easy_setopt(mcp->curl, CURLOPT_HEADERFUNCTION, NULL);
	curl_easy_setopt(mcp->curl, CURLOPT_HEADERDATA, NULL);
	curl_easy_setopt(mcp->curl, CURLOPT_TIMEOUT, (long)mcp->timeout);
	rc = fyai_curl_perform(ctx, mcp->curl);
	if (rc == CURLE_OK)
		curl_easy_getinfo(mcp->curl, CURLINFO_RESPONSE_CODE, &status);
	/* Teardown failure is nonfatal; the server will time the session out. */
	if (rc != CURLE_OK || status < 200 || status >= 300)
		fyai_warning(ctx, "MCP %s session delete failed: %s (HTTP %ld)",
			     mcp->name, curl_easy_strerror(rc), status);
	if (ctx->cfg->mcp_logging)
		(void)fyai_log_generic(ctx, "mcp", fy_mapping(
			"event", "session_delete", "server", mcp->name,
			"http_status", status, "curl_code", (long long)rc));
	free(response.data);
	curl_slist_free_all(headers);
	free(auth);
	free(session);
	free(version);
	mcp->session_id = NULL;
}

/* Shut the server down: close its pipes so it sees EOF and leaves on its own,
 * then escalate through SIGTERM to SIGKILL if it does not. */
static void mcp_stdio_stop(struct fyai_ctx *ctx, struct fyai_mcp_ctx *mcp)
{
	struct fyai_event_loop *el;
	int status;

	if (mcp->stdin_fd >= 0) {
		close(mcp->stdin_fd);
		mcp->stdin_fd = -1;
	}
	if (mcp->stdout_fd >= 0) {
		close(mcp->stdout_fd);
		mcp->stdout_fd = -1;
	}
	if (mcp->pid <= 0)
		return;

	el = fyai_ctx_loop(ctx);
	if (!el || fyai_event_child_terminate(el, mcp->pid, 1000, 500, NULL)) {
		/* No loop, or it failed: the reap is still ours to do. */
		kill(mcp->pid, SIGKILL);
		while (waitpid(mcp->pid, &status, 0) < 0 && errno == EINTR)
			;
	}
	mcp->pid = -1;
}

void fyai_mcp_cleanup(struct fyai_ctx *ctx)
{
	struct fyai_mcp_ctx *mcp, *next;

	if (!ctx || !ctx->mcp)
		return;
	for (mcp = ctx->mcp; mcp; mcp = next) {
		next = mcp->next;
		mcp_delete_session(ctx, mcp);
		if (ctx->cfg->mcp_logging)
			(void)fyai_log_generic(ctx, "mcp", fy_mapping(
				"event", "disconnect", "server", mcp->name));
		if (mcp->curl)
			curl_easy_cleanup(mcp->curl);
		mcp_stdio_stop(ctx, mcp);
		free(mcp);
	}
	ctx->mcp = NULL;
}
