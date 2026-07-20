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
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fyai_log.h"
#include "fyai_tools.h"
#include "utils.h"

#define MCP_PREFIX "mcp__default__"

struct fyai_mcp_ctx {
	CURL *curl;
	const char *session_id;
	long long request_id;
	const char *endpoint;
	const char *auth_token;
	const char *protocol_version;
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

static fy_generic mcp_request(struct fyai_ctx *ctx, const char *method,
			      fy_generic params, bool notification)
{
	struct response_buffer response = {};
	struct fyai_mcp_ctx *mcp = ctx->mcp;
	struct mcp_headers mh = { .ctx = ctx, .mcp = mcp };
	struct curl_slist *headers = NULL;
	char *auth = NULL, *session = NULL, *version = NULL;
	fy_generic request, doc, error;
	const char *body, *json;
	char *sse_json = NULL;
	CURLcode rc;
	long status = 0;
	long long id = 0;
	struct timespec start, end;
	double elapsed_ms;
	bool logging = ctx->cfg->mcp_logging;

	request = fy_mapping(ctx->transient_gb, "jsonrpc", "2.0",
			     "method", method, "params", params);
	if (!notification) {
		id = ++mcp->request_id;
		request = fy_assoc(ctx->transient_gb, request, "id",
				   id);
	}
	body = emit_request_body(ctx->transient_gb, request);
	if (!body)
		return fy_invalid;

	append_header(&headers, "Content-Type: application/json");
	append_header(&headers, "Accept: application/json, text/event-stream");
	version = make_header("MCP-Protocol-Version: ", mcp->protocol_version);
	if (version)
		append_header(&headers, version);
	if (mcp->auth_token && *mcp->auth_token) {
		auth = make_header("Authorization: Bearer ", mcp->auth_token);
		if (auth)
			append_header(&headers, auth);
	}
	if (mcp->session_id) {
		session = make_header("Mcp-Session-Id: ", mcp->session_id);
		if (session)
			append_header(&headers, session);
	}
	curl_easy_setopt(mcp->curl, CURLOPT_URL, mcp->endpoint);
	curl_easy_setopt(mcp->curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(mcp->curl, CURLOPT_POSTFIELDS, body);
	curl_easy_setopt(mcp->curl, CURLOPT_WRITEFUNCTION, write_response);
	curl_easy_setopt(mcp->curl, CURLOPT_WRITEDATA, &response);
	curl_easy_setopt(mcp->curl, CURLOPT_HEADERFUNCTION, mcp_header);
	curl_easy_setopt(mcp->curl, CURLOPT_HEADERDATA, &mh);
	curl_easy_setopt(mcp->curl, CURLOPT_TIMEOUT, (long)ctx->cfg->mcp_timeout);
	curl_easy_setopt(mcp->curl, CURLOPT_USERAGENT, ctx->user_agent);
	if (logging)
		clock_gettime(CLOCK_MONOTONIC, &start);
	rc = curl_easy_perform(mcp->curl);
	if (rc == CURLE_OK)
		curl_easy_getinfo(mcp->curl, CURLINFO_RESPONSE_CODE, &status);
	if (logging) {
		clock_gettime(CLOCK_MONOTONIC, &end);
		elapsed_ms = (double)(end.tv_sec - start.tv_sec) * 1000.0 +
			(double)(end.tv_nsec - start.tv_nsec) / 1000000.0;
		(void)fyai_log_generic(ctx, "mcp", fy_null_filtered_mapping(
			"event", "request",
			"method", method,
			"id", notification ? fy_null : fy_value(id),
			"notification", notification,
			"http_status", status,
			"curl_code", (long long)rc,
			"elapsed_ms", elapsed_ms,
			"session", mcp->session_id ? "active" : "none"));
	}
	curl_slist_free_all(headers);
	free(auth);
	free(session);
	free(version);
	if (rc != CURLE_OK || status < 200 || status >= 300) {
		fyai_error(ctx, "MCP %s failed: %s (HTTP %ld)", method,
			   curl_easy_strerror(rc), status);
		free(response.data);
		return fy_invalid;
	}
	if (notification) {
		free(response.data);
		return fy_null;
	}
	json = response.data ? response.data : "";
	/* Streamable HTTP may return one JSON-RPC response as an SSE event. */
	if (!strncmp(json, "event:", 6) || !strncmp(json, "data:", 5)) {
		const char *data = strstr(json, "data:");
		const char *end;

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
	free(response.data);
	if (fy_generic_is_invalid(doc)) {
		fyai_error(ctx, "MCP %s returned invalid JSON", method);
		return fy_invalid;
	}
	error = fy_get(doc, "error");
	if (fy_generic_is_valid(error)) {
		fyai_error(ctx, "MCP %s: %s", method,
			   fy_get(error, "message", "server error"));
		return fy_invalid;
	}
	return fy_get(doc, "result", fy_invalid);
}

bool fyai_mcp_tool_name(const char *name)
{
	return name && !strncmp(name, MCP_PREFIX, sizeof(MCP_PREFIX) - 1);
}

fy_generic fyai_mcp_tools(struct fyai_ctx *ctx)
{
	return fy_generic_is_valid(ctx->mcp_tools) ? ctx->mcp_tools : fy_seq_empty;
}

int fyai_mcp_refresh(struct fyai_ctx *ctx)
{
	struct fyai_mcp_ctx *mcp;
	fy_generic result, tools, tool, out;
	const char *name;

	if (!ctx->cfg->mcp_endpoint || !*ctx->cfg->mcp_endpoint) {
		fyai_error(ctx, "MCP is enabled but mcp.endpoint is empty");
		return -1;
	}
	mcp = ctx->mcp;
	if (mcp && fy_generic_is_valid(ctx->mcp_tools) &&
	    !strcmp(mcp->endpoint, ctx->cfg->mcp_endpoint) &&
	    !strcmp(mcp->protocol_version, ctx->cfg->mcp_protocol_version) &&
	    !strcmp(mcp->auth_token ? mcp->auth_token : "",
		    ctx->cfg->mcp_auth_token ? ctx->cfg->mcp_auth_token : "")) {
		if (ctx->cfg->mcp_logging)
			(void)fyai_log_generic(ctx, "mcp", fy_mapping(
				"event", "reuse", "tools",
				(long long)fy_len(ctx->mcp_tools)));
		return 0;
	}

	fyai_mcp_cleanup(ctx);
	ctx->mcp_tools = fy_invalid;
	mcp = calloc(1, sizeof(*mcp));
	if (!mcp)
		return -1;
	mcp->endpoint = fy_gb_intern_string(ctx->cfg->gb,
					 ctx->cfg->mcp_endpoint);
	mcp->protocol_version = fy_gb_intern_string(ctx->cfg->gb,
						 ctx->cfg->mcp_protocol_version);
	if (ctx->cfg->mcp_auth_token)
		mcp->auth_token = fy_gb_intern_string(ctx->cfg->gb,
						 ctx->cfg->mcp_auth_token);
	mcp->curl = curl_easy_init();
	if (!mcp->endpoint || !mcp->protocol_version ||
	    (ctx->cfg->mcp_auth_token && !mcp->auth_token) || !mcp->curl) {
		ctx->mcp = mcp;
		fyai_mcp_cleanup(ctx);
		return -1;
	}
	ctx->mcp = mcp;
	if (ctx->cfg->mcp_logging)
		(void)fyai_log_generic(ctx, "mcp", fy_mapping(
			"event", "connect", "transport", "streamable-http"));
	result = mcp_request(ctx, "initialize", fy_mapping(ctx->transient_gb,
			"protocolVersion", ctx->cfg->mcp_protocol_version,
			"capabilities", fy_map_empty,
			"clientInfo", fy_mapping("name", "fyai",
						 "version", VERSION)), false);
	if (fy_generic_is_invalid(result))
		goto err;
	if (fy_generic_is_invalid(mcp_request(ctx, "notifications/initialized",
						 fy_map_empty, true)))
		goto err;
	result = mcp_request(ctx, "tools/list", fy_map_empty, false);
	if (fy_generic_is_invalid(result))
		goto err;
	tools = fy_get(result, "tools", fy_seq_empty);
	out = fy_seq_empty;
	fy_foreach(tool, tools) {
		name = fy_get(tool, "name", "");
		if (!*name)
			continue;
		out = fy_append(ctx->gb, out, fy_mapping(ctx->gb,
			"type", "function", "function", fy_mapping(
				"name", fy_sprintfa(MCP_PREFIX "%s", name),
				"description", fy_get(tool, "description", ""),
				"parameters", fy_get(tool, "inputSchema",
						     fy_mapping("type", "object")))));
	}
	ctx->mcp_tools = fy_gb_internalize(ctx->gb, out);
	if (fy_generic_is_invalid(ctx->mcp_tools))
		goto err;
	if (ctx->cfg->mcp_logging)
		(void)fyai_log_generic(ctx, "mcp", fy_mapping(
			"event", "discovery", "tools",
			(long long)fy_len(ctx->mcp_tools)));
	return 0;
err:
	fyai_mcp_cleanup(ctx);
	ctx->mcp_tools = fy_invalid;
	return -1;
}

fy_generic fyai_mcp_call(struct fyai_ctx *ctx, const char *name,
			 fy_generic args)
{
	fy_generic result, content, item, output;
	const char *text;

	name += sizeof(MCP_PREFIX) - 1;
	if (ctx->cfg->mcp_logging)
		(void)fyai_log_generic(ctx, "mcp", fy_mapping(
			"event", "tool_call", "tool", name));
	result = mcp_request(ctx, "tools/call", fy_mapping(ctx->transient_gb,
			"name", name, "arguments", args), false);
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

void fyai_mcp_cleanup(struct fyai_ctx *ctx)
{
	struct fyai_mcp_ctx *mcp;

	if (!ctx || !ctx->mcp)
		return;
	mcp = ctx->mcp;
	if (ctx->cfg->mcp_logging)
		(void)fyai_log_generic(ctx, "mcp",
			fy_mapping("event", "disconnect"));
	if (mcp->curl)
		curl_easy_cleanup(mcp->curl);
	free(mcp);
	ctx->mcp = NULL;
}
