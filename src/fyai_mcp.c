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
	struct mcp_http_exchange *exchange;
	struct fyai_event_source *read_src;
	struct fyai_event_source *write_src;
	struct fyai_event_source *timer_src;
	struct response_buffer stdio_response;
	fyai_mcp_call_complete_fn complete;
	void *userdata;
	char *tool_name;
	char *params_text;
	char *stdio_request;
	size_t stdio_request_len;
	size_t stdio_request_off;
	long long id;
	struct timespec started;
	fy_generic result;
	enum fyai_mcp_call_state state;
	bool result_ok;
	bool cancel_requested;
};

static void fyai_mcp_call_drop_source(struct fyai_event_source **srcp)
{
	fyai_event_source_remove(*srcp);
	*srcp = NULL;
}

static void fyai_mcp_call_stdio_sources_drop(
		struct fyai_mcp_call_request *request)
{
	fyai_mcp_call_drop_source(&request->read_src);
	fyai_mcp_call_drop_source(&request->write_src);
	fyai_mcp_call_drop_source(&request->timer_src);
}

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
	fyai_mcp_call_stdio_sources_drop(request);
	request->state = state;
	request->result = result;
	request->result_ok = ok;
	if (request->mcp->transport == MCP_TRANSPORT_STDIO)
		mcp_log_stdio_request(request->ctx, request->mcp,
				      "tools/call", request->id, false, ok,
				      &request->started);
	if (request->complete)
		request->complete(request, request->userdata);
}

static void
fyai_mcp_call_stdio_fail(struct fyai_mcp_call_request *request,
			 const char *message)
{
	fy_generic result;

	fyai_error(request->ctx, "MCP %s %s", request->mcp->name, message);
	result = fy_value(request->ctx->transient_gb,
			  "tool error: MCP call failed");
	fyai_mcp_call_finish(request, FYAIMCS_FAILED, result, false);
}

static bool
fyai_mcp_call_stdio_line(struct fyai_mcp_call_request *request)
{
	struct fyai_ctx *ctx;
	fy_generic doc;
	fy_generic error;
	fy_generic result;

	ctx = request->ctx;
	if (!request->stdio_response.len)
		return false;
	doc = parse_json_string(ctx->transient_gb,
				request->stdio_response.data);
	if (!fy_generic_is_mapping(doc)) {
		fyai_mcp_call_stdio_fail(request,
					"returned invalid stdio JSON");
		return true;
	}
	if (fy_generic_is_invalid(fy_get(doc, "id", fy_invalid)))
		return false;
	if (fy_get(doc, "id", -1LL) != request->id)
		return false;
	error = fy_get(doc, "error", fy_invalid);
	if (fy_generic_is_valid(error)) {
		fyai_error(ctx, "MCP tools/call: %s",
			   fy_get(error, "message", "server error"));
		fyai_mcp_call_stdio_fail(request,
					"returned a tool-call error");
		return true;
	}
	result = fy_get(doc, "result", fy_invalid);
	if (fy_generic_is_invalid(result)) {
		fyai_mcp_call_stdio_fail(request,
					"response has no result");
		return true;
	}
	result = mcp_tool_output(ctx, result);
	fyai_mcp_call_finish(request, FYAIMCS_COMPLETED, result, true);
	return true;
}

static enum fyai_event_action
fyai_mcp_call_stdio_readable(const struct fyai_event *ev)
{
	struct fyai_mcp_call_request *request;
	char chunk[4096];
	char *newline;
	size_t consumed;
	size_t remaining;
	ssize_t n;
	int rc;

	request = ev->userdata;
	for (;;) {
		n = read(ev->fd, chunk, sizeof(chunk));
		if (n > 0) {
			rc = response_buffer_reserve(&request->stdio_response,
				request->stdio_response.len + (size_t)n + 1);
			if (rc) {
				fyai_mcp_call_stdio_fail(request,
						"response is too large");
				return FYAIEA_CONTINUE;
			}
			memcpy(request->stdio_response.data +
			       request->stdio_response.len, chunk, (size_t)n);
			request->stdio_response.len += (size_t)n;
			request->stdio_response.data[
				request->stdio_response.len] = '\0';
			while ((newline = memchr(request->stdio_response.data,
					'\n',
					request->stdio_response.len))) {
				*newline = '\0';
				consumed = (size_t)(newline -
					request->stdio_response.data) + 1;
				if (fyai_mcp_call_stdio_line(request))
					return FYAIEA_CONTINUE;
				remaining = request->stdio_response.len -
					consumed;
				memmove(request->stdio_response.data,
					request->stdio_response.data + consumed,
					remaining);
				request->stdio_response.len = remaining;
				request->stdio_response.data[remaining] = '\0';
			}
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		break;
	}
	if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
		return FYAIEA_CONTINUE;
	fyai_mcp_call_stdio_fail(request, "stdio server closed");
	return FYAIEA_CONTINUE;
}

static enum fyai_event_action
fyai_mcp_call_stdio_writable(const struct fyai_event *ev)
{
	struct fyai_mcp_call_request *request;
	fy_generic result;
	ssize_t n;

	request = ev->userdata;
	while (request->stdio_request_off < request->stdio_request_len) {
		n = write(ev->fd,
			  request->stdio_request + request->stdio_request_off,
			  request->stdio_request_len -
			  request->stdio_request_off);
		if (n > 0) {
			request->stdio_request_off += (size_t)n;
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return FYAIEA_CONTINUE;
		fyai_mcp_call_stdio_fail(request, "stdio write failed");
		return FYAIEA_CONTINUE;
	}
	fyai_mcp_call_drop_source(&request->write_src);
	if (!request->id) {
		result = fy_null;
		fyai_mcp_call_finish(request, FYAIMCS_COMPLETED, result, true);
	}
	return FYAIEA_CONTINUE;
}

static enum fyai_event_action
fyai_mcp_call_stdio_timeout(const struct fyai_event *ev)
{
	struct fyai_mcp_call_request *request;

	request = ev->userdata;
	request->timer_src = NULL;
	fyai_mcp_call_stdio_fail(request, "stdio response timed out");
	return FYAIEA_CONTINUE;
}

static int
fyai_mcp_call_stdio_submit(struct fyai_mcp_call_request *request,
			   fy_generic params)
{
	struct fyai_ctx *ctx;
	struct fyai_event_loop *el;
	fy_generic rpc;
	const char *body;
	size_t body_len;
	int flags;
	int rc;

	ctx = request->ctx;
	el = fyai_ctx_loop(ctx);
	fyai_error_check(ctx, el, err_out,
			 "could not acquire the application event loop");
	rpc = fy_mapping(ctx->transient_gb, "jsonrpc", "2.0",
			 "method", "tools/call", "params", params,
			 "id", request->id);
	body = emit_request_body(ctx->transient_gb, rpc);
	fyai_error_check(ctx, body, err_out,
			 "could not encode MCP stdio request");
	body_len = strlen(body);
	request->stdio_request = malloc(body_len + 2);
	fyai_error_check(ctx, request->stdio_request, err_out,
			 "out of memory");
	memcpy(request->stdio_request, body, body_len);
	request->stdio_request[body_len] = '\n';
	request->stdio_request[body_len + 1] = '\0';
	request->stdio_request_len = body_len + 1;

	flags = fcntl(request->mcp->stdin_fd, F_GETFL);
	fyai_error_check(ctx, flags >= 0, err_out,
			 "could not inspect MCP stdin: %s", strerror(errno));
	rc = fcntl(request->mcp->stdin_fd, F_SETFL, flags | O_NONBLOCK);
	fyai_error_check(ctx, !rc, err_out,
			 "could not make MCP stdin non-blocking: %s",
			 strerror(errno));
	flags = fcntl(request->mcp->stdout_fd, F_GETFL);
	fyai_error_check(ctx, flags >= 0, err_out,
			 "could not inspect MCP stdout: %s", strerror(errno));
	rc = fcntl(request->mcp->stdout_fd, F_SETFL, flags | O_NONBLOCK);
	fyai_error_check(ctx, !rc, err_out,
			 "could not make MCP stdout non-blocking: %s",
			 strerror(errno));
	rc = fyai_event_add_fd(el, request->mcp->stdout_fd, FYAIEV_READ,
			       fyai_mcp_call_stdio_readable, request,
			       &request->read_src);
	fyai_error_check(ctx, !rc, err_out,
			 "could not watch MCP stdout");
	rc = fyai_event_add_fd(el, request->mcp->stdin_fd, FYAIEV_WRITE,
			       fyai_mcp_call_stdio_writable, request,
			       &request->write_src);
	fyai_error_check(ctx, !rc, err_out,
			 "could not watch MCP stdin");
	rc = fyai_event_add_timer(el, request->mcp->timeout * 1000, 0,
				  fyai_mcp_call_stdio_timeout, request,
				  &request->timer_src);
	fyai_error_check(ctx, !rc, err_out,
			 "could not arm MCP response timeout");
	return 0;

err_out:
	fyai_mcp_call_stdio_sources_drop(request);
	return -1;
}

static void fyai_mcp_call_exchange_complete(
		struct mcp_http_exchange *exchange, void *userdata);

static int
fyai_mcp_call_submit_phase(struct fyai_mcp_call_request *request)
{
	struct fyai_ctx *ctx;
	const char *method;
	fy_generic params;
	long long id;
	bool notification;

	ctx = request->ctx;
	notification = false;
	id = request->id;
	switch (request->state) {
	case FYAIMCS_PRIMARY:
		method = "tools/call";
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
	case FYAIMCS_COMPLETED:
	case FYAIMCS_CANCELLED:
	case FYAIMCS_FAILED:
		return -1;
	}
	fyai_error_check(ctx, fy_generic_is_valid(params), err_out,
			 "MCP call parameters are invalid");
	request->exchange = mcp_http_exchange_submit(ctx, request->mcp,
			method, params, notification, id,
			fyai_mcp_call_exchange_complete, request);
	fyai_error_check(ctx, request->exchange, err_out,
			 "MCP %s could not submit %s",
			 request->mcp->name, method);
	return 0;

err_out:
	return -1;
}

static void fyai_mcp_call_exchange_complete(
		struct mcp_http_exchange *exchange, void *userdata)
{
	struct fyai_mcp_call_request *request;
	struct fyai_ctx *ctx;
	fy_generic result;
	const char *protocol;
	CURLcode code;
	long status;
	int rc;

	request = userdata;
	ctx = request->ctx;
	code = exchange->code;
	status = exchange->status;
	result = mcp_http_exchange_result(exchange);
	mcp_http_exchange_destroy(exchange);
	request->exchange = NULL;
	if (request->cancel_requested) {
		result = fy_value(ctx->transient_gb,
				  "tool error: interrupted");
		fyai_mcp_call_finish(request, FYAIMCS_CANCELLED,
				     result, false);
		return;
	}
	if (fy_generic_is_valid(result)) {
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
			rc = fyai_mcp_call_submit_phase(request);
			if (!rc)
				return;
			goto failed;
		case FYAIMCS_RECOVER_NOTIFY:
			request->state = FYAIMCS_PRIMARY;
			rc = fyai_mcp_call_submit_phase(request);
			if (!rc)
				return;
			goto failed;
		case FYAIMCS_COMPLETED:
		case FYAIMCS_CANCELLED:
		case FYAIMCS_FAILED:
			goto failed;
		}
	}
	if (request->state == FYAIMCS_PRIMARY && status == 404 &&
	    request->mcp->session_id) {
		request->mcp->session_id = NULL;
		request->state = FYAIMCS_RECOVER_INITIALIZE;
		rc = fyai_mcp_call_submit_phase(request);
		if (!rc)
			return;
	}
	fyai_error(ctx, "MCP %s failed: %s (HTTP %ld)",
		   request->mcp->name, curl_easy_strerror(code), status);
failed:
	result = fy_value(ctx->transient_gb,
			  "tool error: MCP call failed");
	fyai_mcp_call_finish(request, FYAIMCS_FAILED, result, false);
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
	if (mcp->transport == MCP_TRANSPORT_STDIO)
		clock_gettime(CLOCK_MONOTONIC, &request->started);
	fyai_error_check(ctx, request->tool_name && request->params_text,
			 err_free, "out of memory");
	if (ctx->cfg->mcp_logging)
		(void)fyai_log_generic(ctx, "mcp", fy_mapping(
			"event", "tool_call", "server", mcp->name,
			"tool", tool_name));
	rc = mcp->transport == MCP_TRANSPORT_STDIO ?
		fyai_mcp_call_stdio_submit(request, params) :
		fyai_mcp_call_submit_phase(request);
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
	if (request->mcp->transport == MCP_TRANSPORT_STDIO) {
		fyai_mcp_call_finish(request, FYAIMCS_CANCELLED,
			fy_value(request->ctx->transient_gb,
				 "tool error: interrupted"), false);
	} else if (request->exchange && request->exchange->transfer) {
		fyai_curl_cancel(request->exchange->transfer);
	}
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
	fyai_mcp_call_stdio_sources_drop(request);
	mcp_http_exchange_destroy(request->exchange);
	free(request->stdio_response.data);
	free(request->stdio_request);
	free(request->tool_name);
	free(request->params_text);
	free(request);
}

fy_generic fyai_mcp_call(struct fyai_ctx *ctx, const char *name,
			 fy_generic args)
{
	struct fyai_mcp_ctx *mcp;
	fy_generic result, content, item, output;
	const char *text, *sep, *server_name, *tool_name;
	size_t server_len;

	server_name = name + sizeof(MCP_PREFIX) - 1;
	sep = strstr(server_name, "__");
	if (!sep)
		return fy_value(ctx->transient_gb, "tool error: invalid MCP tool name");

	server_len = (size_t)(sep - server_name);
	tool_name = sep + 2;
	for (mcp = ctx->mcp; mcp; mcp = mcp->next)
		if (strlen(mcp->name) == server_len &&
		    !strncmp(mcp->name, server_name, server_len))
			break;
	if (!mcp)
		return fy_value(ctx->transient_gb, "tool error: MCP server not found");

	if (ctx->cfg->mcp_logging)
		(void)fyai_log_generic(ctx, "mcp", fy_mapping(
			"event", "tool_call", "server", mcp->name,
			"tool", tool_name));

	result = mcp_request(ctx, mcp, "tools/call", fy_mapping(ctx->transient_gb,
			"name", tool_name, "arguments", args), false);

	if (fy_generic_is_invalid(result))
		return fy_value(ctx->transient_gb, "tool error: MCP call failed");

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
