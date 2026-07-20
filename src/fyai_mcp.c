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
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

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
static void mcp_stdio_stop(struct fyai_mcp_ctx *mcp);

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

static fy_generic mcp_http_request_once(struct fyai_ctx *ctx,
				   struct fyai_mcp_ctx *mcp,
				   const char *method, fy_generic params,
				   bool notification, long long id,
				   CURLcode *rcp, long *statusp)
{
	struct response_buffer response = {};
	struct mcp_headers mh = { .ctx = ctx, .mcp = mcp };
	struct curl_slist *headers = NULL;
	char *auth = NULL, *session = NULL, *version = NULL;
	fy_generic request, doc, error, out;
	const char *body, *json;
	char *sse_json = NULL;
	CURLcode rc;
	long status = 0;
	struct timespec start_ts, end_ts;
	double elapsed_ms;
	const char *data, *end;
	bool logging;
	int ret;

	logging = ctx->cfg->mcp_logging;
	request = fy_mapping(ctx->transient_gb, "jsonrpc", "2.0",
			     "method", method, "params", params);
	if (!notification)
		request = fy_assoc(ctx->transient_gb, request, "id", id);
	body = emit_request_body(ctx->transient_gb, request);
	fyai_error_check(ctx, body, err_out,
			 "MCP %s could not encode request", method);

	ret = append_header(&headers, "Content-Type: application/json");
	fyai_error_check(ctx, !ret,
		err_out, "MCP %s could not create request headers", method);
	ret = append_header(&headers,
			    "Accept: application/json, text/event-stream");
	fyai_error_check(ctx, !ret,
		err_out, "MCP %s could not create request headers", method);
	version = make_header("MCP-Protocol-Version: ", mcp->protocol_version);
	ret = version ? append_header(&headers, version) : -1;
	fyai_error_check(ctx, !ret,
			err_out, "MCP %s could not create protocol header", method);
	if (mcp->auth_token && *mcp->auth_token) {
		auth = make_header("Authorization: Bearer ", mcp->auth_token);
		ret = auth ? append_header(&headers, auth) : -1;
		fyai_error_check(ctx, !ret,
				err_out, "MCP %s could not create auth header", method);
	}
	if (mcp->session_id) {
		session = make_header("Mcp-Session-Id: ", mcp->session_id);
		ret = session ? append_header(&headers, session) : -1;
		fyai_error_check(ctx, !ret,
				err_out, "MCP %s could not create session header", method);
	}
	curl_easy_setopt(mcp->curl, CURLOPT_URL, mcp->endpoint);
	curl_easy_setopt(mcp->curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(mcp->curl, CURLOPT_POSTFIELDS, body);
	curl_easy_setopt(mcp->curl, CURLOPT_WRITEFUNCTION, write_response);
	curl_easy_setopt(mcp->curl, CURLOPT_WRITEDATA, &response);
	curl_easy_setopt(mcp->curl, CURLOPT_HEADERFUNCTION, mcp_header);
	curl_easy_setopt(mcp->curl, CURLOPT_HEADERDATA, &mh);
	curl_easy_setopt(mcp->curl, CURLOPT_TIMEOUT, (long)mcp->timeout);
	curl_easy_setopt(mcp->curl, CURLOPT_USERAGENT, ctx->user_agent);
	if (logging)
		clock_gettime(CLOCK_MONOTONIC, &start_ts);
	rc = curl_easy_perform(mcp->curl);
	if (rc == CURLE_OK)
		curl_easy_getinfo(mcp->curl, CURLINFO_RESPONSE_CODE, &status);
	*rcp = rc;
	*statusp = status;
	if (logging) {
		clock_gettime(CLOCK_MONOTONIC, &end_ts);
		elapsed_ms = (double)(end_ts.tv_sec - start_ts.tv_sec) * 1000.0 +
			(double)(end_ts.tv_nsec - start_ts.tv_nsec) / 1000000.0;
		(void)fyai_log_generic(ctx, "mcp", fy_null_filtered_mapping(
			"event", "request",
			"server", mcp->name,
			"method", method,
			"id", notification ? fy_null : fy_value(id),
			"notification", notification,
			"http_status", status,
			"curl_code", (long long)rc,
			"elapsed_ms", elapsed_ms,
			"session", mcp->session_id ? "active" : "none"));
	}
	curl_slist_free_all(headers);
	headers = NULL;
	free(auth);
	free(session);
	free(version);
	auth = session = version = NULL;

	if (rc != CURLE_OK || status < 200 || status >= 300)
		goto err_out;

	if (notification) {
		free(response.data);
		return fy_null;
	}

	json = response.data ? response.data : "";
	/* Streamable HTTP may return one JSON-RPC response as an SSE event. */
	if (!strncmp(json, "event:", 6) || !strncmp(json, "data:", 5)) {
		data = strstr(json, "data:");
		if (data) {
			data += 5;
			while (*data == ' ' || *data == '\t')
				data++;
			end = strchr(data, '\n');
			sse_json = strndup(data, end ? (size_t)(end - data) : strlen(data));
			if (sse_json)
				json = sse_json;
		}
	}
	doc = parse_json_string(ctx->transient_gb, json);
	free(sse_json);
	sse_json = NULL;
	free(response.data);
	response.data = NULL;

	fyai_error_check(ctx, fy_generic_is_valid(doc), err_out,
			 "MCP %s returned invalid JSON", method);
	error = fy_get(doc, "error");
	fyai_error_check(ctx, fy_generic_is_invalid(error), err_out,
			 "MCP %s: %s", method,
			 fy_get(error, "message", "server error"));
	out = fy_get(doc, "result", fy_invalid);
	fyai_error_check(ctx, fy_generic_is_valid(out), err_out,
			 "MCP %s response has no result", method);

	return out;

err_out:
	curl_slist_free_all(headers);
	free(auth);
	free(session);
	free(version);
	free(response.data);
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

	pid = fork();
	fyai_error_check(ctx, pid >= 0, err_out,
			 "MCP stdio server '%s' could not fork: %s",
			 mcp->name, strerror(errno));

	if (!pid) {
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
	signal(SIGPIPE, SIG_IGN);
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
	mcp_stdio_stop(mcp);
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
	curl_easy_setopt(mcp->curl, CURLOPT_POSTFIELDS, NULL);
	curl_easy_setopt(mcp->curl, CURLOPT_WRITEFUNCTION, write_response);
	curl_easy_setopt(mcp->curl, CURLOPT_WRITEDATA, &response);
	curl_easy_setopt(mcp->curl, CURLOPT_HEADERFUNCTION, NULL);
	curl_easy_setopt(mcp->curl, CURLOPT_HEADERDATA, NULL);
	curl_easy_setopt(mcp->curl, CURLOPT_TIMEOUT, (long)mcp->timeout);
	rc = curl_easy_perform(mcp->curl);
	if (rc == CURLE_OK)
		curl_easy_getinfo(mcp->curl, CURLINFO_RESPONSE_CODE, &status);
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

static void mcp_stdio_stop(struct fyai_mcp_ctx *mcp)
{
	struct timespec delay = { .tv_nsec = 10000000 };
	int status, i;

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
	for (i = 0; i < 100; i++) {
		if (waitpid(mcp->pid, &status, WNOHANG) == mcp->pid)
			goto out;
		nanosleep(&delay, NULL);
	}
	kill(mcp->pid, SIGTERM);
	for (i = 0; i < 50; i++) {
		if (waitpid(mcp->pid, &status, WNOHANG) == mcp->pid)
			goto out;
		nanosleep(&delay, NULL);
	}
	kill(mcp->pid, SIGKILL);
	while (waitpid(mcp->pid, &status, 0) < 0 && errno == EINTR)
		;
out:
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
		mcp_stdio_stop(mcp);
		free(mcp);
	}
	ctx->mcp = NULL;
}
