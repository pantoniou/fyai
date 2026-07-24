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
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "fyai_curl.h"
#include "fyai_event.h"
#include "fyai_jsonrpc.h"
#include "fyai_log.h"
#include "fyai_tools.h"
#include "utils.h"

#define MCP_PREFIX "mcp__"

enum mcp_transport {
	MCP_TRANSPORT_HTTP,
	MCP_TRANSPORT_STDIO,
};

enum mcp_server_state {
	MCP_SRV_CONNECTING,	/* startup handshake in flight */
	MCP_SRV_READY,		/* initialized and tools discovered */
	MCP_SRV_FAILED,		/* terminal failure; dropped from the catalogue */
};

struct mcp_startup;

struct fyai_mcp_ctx {
	struct fyai_mcp_ctx *next;
	struct fyai_ctx *ctx;
	struct jsonrpc_conn *conn;
	struct mcp_startup *startup;
	fy_generic tools;
	enum mcp_server_state state;
	enum mcp_transport transport;
	CURL *curl;
	pid_t pid;
	int stdin_fd;
	int stdout_fd;
	const char *session_id;
	const char *name;
	const char *endpoint;
	const char *auth_token;
	const char *protocol_version;
	int timeout;
	bool recovering;

	/* Asynchronous teardown state, driven by fyai_mcp_cleanup(). */
	struct fyai_curl_transfer *del_xfer;
	struct curl_slist *del_headers;
	struct response_buffer del_resp;
	struct fyai_event_source *term_src;
	bool del_done;
	bool term_done;
};

static void mcp_stdio_stop(struct fyai_ctx *ctx, struct fyai_mcp_ctx *mcp);

static bool mcp_transient_status(CURLcode rc, long status)
{
	return rc == CURLE_COULDNT_CONNECT || rc == CURLE_COULDNT_RESOLVE_HOST ||
		rc == CURLE_RECV_ERROR || rc == CURLE_SEND_ERROR ||
		rc == CURLE_OPERATION_TIMEDOUT || status == 408 || status == 429 ||
		status == 500 || status == 502 || status == 503 || status == 504;
}

/*
 * HTTP transport policy for the server's JSON-RPC connection: the MCP auth,
 * protocol and session headers, and capturing the session id the server
 * assigns. These are the "extra bits" the generic transport delegates back.
 */
static int mcp_http_add_headers(void *userdata, struct fyai_ctx *ctx,
				struct curl_slist **headers)
{
	struct fyai_mcp_ctx *mcp = userdata;
	char *version = NULL, *auth = NULL, *session = NULL;
	int rc;

	(void)ctx;
	version = make_header("MCP-Protocol-Version: ", mcp->protocol_version);
	rc = version ? append_header(headers, version) : -1;
	if (!rc && mcp->auth_token && *mcp->auth_token) {
		auth = make_header("Authorization: Bearer ", mcp->auth_token);
		rc = auth ? append_header(headers, auth) : -1;
	}
	if (!rc && mcp->session_id) {
		session = make_header("Mcp-Session-Id: ", mcp->session_id);
		rc = session ? append_header(headers, session) : -1;
	}
	free(version);
	free(auth);
	free(session);
	return rc;
}

static void mcp_http_response_header(void *userdata, const char *line,
				     size_t len)
{
	struct fyai_mcp_ctx *mcp = userdata;
	const char key[] = "mcp-session-id:";
	const char *p, *end;

	if (len < sizeof(key) - 1 || strncasecmp(line, key, sizeof(key) - 1))
		return;
	p = line + sizeof(key) - 1;
	end = line + len;
	while (p < end && isspace((unsigned char)*p))
		p++;
	while (end > p && isspace((unsigned char)end[-1]))
		end--;
	mcp->session_id = fy_gb_intern_string(mcp->ctx->cfg->gb,
			fy_sprintfa("%.*s", (int)(end - p), p));
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

#define MCP_STARTUP_RETRIES 3

enum mcp_startup_phase {
	MCP_SU_INIT,
	MCP_SU_NOTIFY,
	MCP_SU_DISCOVER,
};

/* One server's asynchronous startup handshake: initialize, initialized, then
 * paginated tools/list. It settles the server READY or FAILED and drives the
 * next step from each request completion. */
struct mcp_startup {
	struct fyai_mcp_ctx *mcp;
	struct jsonrpc_request *req;
	struct fyai_event_source *timer;
	char *cursor;
	long long id;			/* current request id, reused across retry */
	enum mcp_startup_phase phase;
	int attempt;
};

static void mcp_startup_step(struct mcp_startup *su);
static void mcp_startup_enter(struct mcp_startup *su,
			      enum mcp_startup_phase phase);

static void mcp_startup_settle(struct mcp_startup *su,
			       enum mcp_server_state state)
{
	struct fyai_mcp_ctx *mcp = su->mcp;
	struct fyai_ctx *ctx = mcp->ctx;

	if (su->timer) {
		fyai_event_source_remove(su->timer);
		su->timer = NULL;
	}
	if (su->req) {
		jsonrpc_request_destroy(su->req);
		su->req = NULL;
	}
	mcp->state = state;
	mcp->startup = NULL;
	free(su->cursor);
	free(su);
	if (ctx->cfg->mcp_logging)
		(void)fyai_log_generic(ctx, "mcp", fy_mapping(
			"event", state == MCP_SRV_READY ? "discovery" : "failed",
			"server", mcp->name, "tools",
			(long long)(fy_generic_is_valid(mcp->tools) ?
				    fy_len(mcp->tools) : 0)));
	if (state == MCP_SRV_FAILED)
		fyai_warning(ctx, "MCP server '%s' is unavailable", mcp->name);
}

static enum fyai_event_action mcp_startup_retry(const struct fyai_event *ev)
{
	struct mcp_startup *su = ev->userdata;

	su->timer = NULL;
	mcp_startup_step(su);
	return FYAIEA_CONTINUE;
}

/* Bad auth, an unretryable status, or any stdio error is terminal at this stage
 * (mid-session reconnect is a later step); transient statuses retry. */
static bool mcp_startup_terminal(struct fyai_mcp_ctx *mcp, long status,
				 CURLcode code)
{
	if (mcp->transport == MCP_TRANSPORT_STDIO)
		return true;
	if (status == 401 || status == 403)
		return true;
	return !mcp_transient_status(code, status);
}

static void mcp_startup_collect(struct fyai_mcp_ctx *mcp, fy_generic result)
{
	struct fyai_ctx *ctx = mcp->ctx;
	fy_generic tools = fy_get(result, "tools", fy_seq_empty), tool;
	const char *tool_name;

	fy_foreach(tool, tools) {
		tool_name = fy_get(tool, "name", "");
		if (!*tool_name)
			continue;
		mcp->tools = fy_append(ctx->gb, mcp->tools, fy_mapping(ctx->gb,
			"type", "function", "function", fy_mapping(
				"name", fy_sprintfa(MCP_PREFIX "%s__%s",
						    mcp->name, tool_name),
				"description", fy_get(tool, "description", ""),
				"parameters", fy_get(tool, "inputSchema",
						fy_mapping("type", "object")))));
	}
}

static void mcp_startup_req_done(struct jsonrpc_request *req, void *userdata)
{
	struct mcp_startup *su = userdata;
	struct fyai_mcp_ctx *mcp = su->mcp;
	struct fyai_ctx *ctx = mcp->ctx;
	bool ok = jsonrpc_request_ok(req);
	fy_generic result = jsonrpc_request_result(req);
	long status = jsonrpc_request_http_status(req);
	CURLcode code = jsonrpc_request_curl_code(req);
	struct fyai_event_loop *el;
	const char *protocol, *cursor;

	jsonrpc_request_destroy(req);
	su->req = NULL;

	if (!ok) {
		if (!mcp_startup_terminal(mcp, status, code) &&
		    su->attempt + 1 < MCP_STARTUP_RETRIES) {
			su->attempt++;
			el = fyai_ctx_loop(ctx);
			if (el && !fyai_event_add_timer(el, 100 << su->attempt,
					0, mcp_startup_retry, su, &su->timer))
				return;
		}
		mcp_startup_settle(su, MCP_SRV_FAILED);
		return;
	}
	switch (su->phase) {
	case MCP_SU_INIT:
		protocol = fy_get(result, "protocolVersion", "");
		if (*protocol)
			mcp->protocol_version =
				fy_gb_intern_string(ctx->cfg->gb, protocol);
		mcp_startup_enter(su, MCP_SU_NOTIFY);
		break;
	case MCP_SU_NOTIFY:
		mcp_startup_enter(su, MCP_SU_DISCOVER);
		break;
	case MCP_SU_DISCOVER:
		mcp_startup_collect(mcp, result);
		cursor = fy_get(result, "nextCursor", "");
		free(su->cursor);
		su->cursor = *cursor ? strdup(cursor) : NULL;
		if (su->cursor)
			mcp_startup_enter(su, MCP_SU_DISCOVER);
		else
			mcp_startup_settle(su, MCP_SRV_READY);
		break;
	}
}

/* (Re)submit the request for the current phase. The id is fixed on phase entry
 * and reused here so a retry replays with the same id, as the protocol wants. */
static void mcp_startup_step(struct mcp_startup *su)
{
	struct fyai_mcp_ctx *mcp = su->mcp;
	struct fyai_ctx *ctx = mcp->ctx;
	struct fy_generic_builder *gb;
	const char *method;
	fy_generic params;
	bool notification = false;

	gb = fyai_ctx_transient_gb(ctx);
	if (!gb) {
		fyai_error(ctx, "%s could not acquire transient storage",
			   mcp->name);
		mcp_startup_settle(su, MCP_SRV_FAILED);
		return;
	}

	switch (su->phase) {
	case MCP_SU_INIT:
		method = "initialize";
		params = fy_mapping(gb,
			"protocolVersion", mcp->protocol_version,
			"capabilities", fy_map_empty,
			"clientInfo", fy_mapping("name", "fyai",
						 "version", VERSION));
		break;
	case MCP_SU_NOTIFY:
		method = "notifications/initialized";
		notification = true;
		params = fy_map_empty;
		break;
	case MCP_SU_DISCOVER:
		method = "tools/list";
		params = su->cursor ? fy_mapping(gb,
				"cursor", su->cursor) : fy_map_empty;
		break;
	default:
		mcp_startup_settle(su, MCP_SRV_FAILED);
		return;
	}
	su->req = jsonrpc_request_submit(mcp->conn, method, params, su->id,
			notification, mcp_startup_req_done, su);
	if (!su->req)
		mcp_startup_settle(su, MCP_SRV_FAILED);
}

/* Advance to @phase with a fresh retry budget and, for a request phase, a fresh
 * id (a notification keeps id 0), then submit it. */
static void mcp_startup_enter(struct mcp_startup *su,
			      enum mcp_startup_phase phase)
{
	su->phase = phase;
	su->attempt = 0;
	su->id = phase == MCP_SU_NOTIFY ? 0 :
		jsonrpc_conn_next_id(su->mcp->conn);
	mcp_startup_step(su);
}

/* Begin a created server's asynchronous startup. */
static void mcp_startup_begin(struct fyai_mcp_ctx *mcp)
{
	struct mcp_startup *su;

	su = calloc(1, sizeof(*su));
	if (!su) {
		mcp->state = MCP_SRV_FAILED;
		return;
	}
	su->mcp = mcp;
	mcp->startup = su;
	mcp_startup_enter(su, MCP_SU_INIT);
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

static int mcp_create_server(struct fyai_ctx *ctx, const char *server_name,
			     fy_generic config)
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
	mcp->ctx = ctx;
	mcp->tools = fy_seq_empty;
	mcp->state = MCP_SRV_CONNECTING;
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

	if (mcp->transport == MCP_TRANSPORT_STDIO) {
		mcp->conn = jsonrpc_conn_stdio(ctx, mcp->stdin_fd,
				mcp->stdout_fd, mcp->timeout, mcp->name, "mcp");
	} else {
		struct jsonrpc_http_hooks hooks = {
			.userdata = mcp,
			.add_headers = mcp_http_add_headers,
			.response_header = mcp_http_response_header,
		};
		mcp->conn = jsonrpc_conn_http(ctx, mcp->endpoint, mcp->timeout,
				mcp->name, "mcp", &hooks);
	}
	fyai_error_check(ctx, mcp->conn, err_out,
			 "MCP server '%s' could not create connection",
			 server_name);

	tail = &ctx->mcp;
	while (*tail)
		tail = &(*tail)->next;
	*tail = mcp;

	if (ctx->cfg->mcp_logging)
		(void)fyai_log_generic(ctx, "mcp", fy_mapping(
			"event", "connect", "server", mcp->name,
			"transport", mcp->transport == MCP_TRANSPORT_STDIO ?
				"stdio" : "streamable-http"));

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
	jsonrpc_conn_destroy(mcp->conn);
	if (mcp->curl)
		curl_easy_cleanup(mcp->curl);
	mcp_stdio_stop(ctx, mcp);
	free(mcp);
	return -1;
}

/* True once every configured server has settled (READY or FAILED). */
bool fyai_mcp_settled(struct fyai_ctx *ctx)
{
	struct fyai_mcp_ctx *mcp;

	for (mcp = ctx->mcp; mcp; mcp = mcp->next)
		if (mcp->state == MCP_SRV_CONNECTING)
			return false;
	return true;
}

/* Fold every ready server's discovered tools into ctx->mcp_tools, in
 * configuration order (ctx->mcp is built in that order). */
void fyai_mcp_publish_tools(struct fyai_ctx *ctx)
{
	struct fyai_mcp_ctx *mcp;
	fy_generic out = fy_seq_empty;

	for (mcp = ctx->mcp; mcp; mcp = mcp->next)
		if (mcp->state == MCP_SRV_READY &&
		    fy_generic_is_valid(mcp->tools))
			out = fy_concat(ctx->gb, out, mcp->tools);
	ctx->mcp_tools = fy_gb_internalize(ctx->gb, out);
}

/*
 * Create and connect every configured server, then submit its startup
 * handshake. The servers initialize concurrently; a server that cannot even be
 * created is warned and skipped (degrade gracefully). This does not wait -
 * callers observe fyai_mcp_settled() and then fyai_mcp_publish_tools().
 */
int fyai_mcp_start(struct fyai_ctx *ctx)
{
	fy_generic servers = ctx->cfg->mcp_servers;
	struct fy_generic_builder *gb;
	fy_generic key, config;
	struct fyai_mcp_ctx *mcp;
	const char *name;

	fyai_mcp_cleanup(ctx);
	ctx->mcp_tools = fy_invalid;

	if (fy_generic_is_mapping(servers) && fy_len(servers)) {
		fy_foreach(key, servers) {
			name = fy_castp(&key, "");
			config = fy_get(servers, key, fy_invalid);
			if (!fy_generic_is_mapping(config)) {
				fyai_warning(ctx, "MCP server '%s' configuration "
					     "is not a mapping; skipped", name);
				continue;
			}
			(void)mcp_create_server(ctx, name, config);
		}
	} else if (ctx->cfg->mcp_endpoint && *ctx->cfg->mcp_endpoint) {
		gb = fyai_ctx_transient_gb(ctx);
		if (!gb) {
			fyai_error(ctx, "could not acquire transient storage");
			return -1;
		}
		config = fy_mapping(gb,
			"endpoint", ctx->cfg->mcp_endpoint,
			"protocol_version", ctx->cfg->mcp_protocol_version,
			"timeout", ctx->cfg->mcp_timeout);
		(void)mcp_create_server(ctx, "default", config);
	} else {
		fyai_error(ctx, "MCP is enabled but no servers are configured");
		return -1;
	}

	for (mcp = ctx->mcp; mcp; mcp = mcp->next)
		mcp_startup_begin(mcp);
	return 0;
}

/*
 * Synchronous compatibility wrapper: start every server, pump the shared loop
 * until all have settled, then publish their tools. A live catalogue is reused.
 */
int fyai_mcp_refresh(struct fyai_ctx *ctx)
{
	struct fyai_event_loop *el;

	if (ctx->mcp && fy_generic_is_valid(ctx->mcp_tools)) {
		if (ctx->cfg->mcp_logging)
			(void)fyai_log_generic(ctx, "mcp", fy_mapping(
				"event", "reuse", "tools",
				(long long)fy_len(ctx->mcp_tools)));
		return 0;
	}
	if (fyai_mcp_start(ctx))
		return -1;
	el = fyai_ctx_loop(ctx);
	while (el && !fyai_mcp_settled(ctx)) {
		if (fyai_event_loop_step(el, -1) < 0)
			break;
	}
	fyai_mcp_publish_tools(ctx);
	return 0;
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
	struct jsonrpc_request *req;
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

static void fyai_mcp_call_req_done(struct jsonrpc_request *req, void *userdata);

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
		id = jsonrpc_conn_next_id(request->mcp->conn);
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
	request->req = jsonrpc_request_submit(request->mcp->conn, method,
					      params, id, notification,
					      fyai_mcp_call_req_done, request);
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
static void fyai_mcp_call_req_done(struct jsonrpc_request *req, void *userdata)
{
	struct fyai_mcp_call_request *request = userdata;
	struct fyai_ctx *ctx = request->ctx;
	fy_generic result = jsonrpc_request_result(req);
	bool ok = jsonrpc_request_ok(req);
	long status = jsonrpc_request_http_status(req);
	CURLcode code = jsonrpc_request_curl_code(req);
	const char *protocol;

	jsonrpc_request_destroy(req);
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
	request->id = jsonrpc_conn_next_id(mcp->conn);
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
		jsonrpc_request_cancel(request->req);
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
	jsonrpc_request_destroy(request->req);
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

static void mcp_delete_complete(struct fyai_curl_transfer *xfer, void *userdata)
{
	struct fyai_mcp_ctx *mcp = userdata;
	struct fyai_ctx *ctx = mcp->ctx;
	CURLcode rc = fyai_curl_collect(xfer);
	long status = 0;

	curl_easy_getinfo(mcp->curl, CURLINFO_RESPONSE_CODE, &status);
	fyai_curl_transfer_destroy(xfer);
	mcp->del_xfer = NULL;
	/* Teardown failure is nonfatal; the server will time the session out. */
	if (rc != CURLE_OK || status < 200 || status >= 300)
		fyai_warning(ctx, "MCP %s session delete failed: %s (HTTP %ld)",
			     mcp->name, curl_easy_strerror(rc), status);
	if (ctx->cfg->mcp_logging)
		(void)fyai_log_generic(ctx, "mcp", fy_mapping(
			"event", "session_delete", "server", mcp->name,
			"http_status", status, "curl_code", (long long)rc));
	mcp->session_id = NULL;
	mcp->del_done = true;
}

/* Submit the session DELETE. Returns true if a transfer is now in flight. */
static bool mcp_delete_session_begin(struct fyai_ctx *ctx,
				     struct fyai_mcp_ctx *mcp)
{
	char *auth = NULL, *session = NULL, *version = NULL;

	if (!mcp->curl || !mcp->session_id)
		return false;

	version = make_header("MCP-Protocol-Version: ", mcp->protocol_version);
	session = make_header("Mcp-Session-Id: ", mcp->session_id);
	if (version)
		append_header(&mcp->del_headers, version);
	if (session)
		append_header(&mcp->del_headers, session);
	if (mcp->auth_token && *mcp->auth_token) {
		auth = make_header("Authorization: Bearer ", mcp->auth_token);
		if (auth)
			append_header(&mcp->del_headers, auth);
	}
	free(version);
	free(session);
	free(auth);
	curl_easy_setopt(mcp->curl, CURLOPT_URL, mcp->endpoint);
	curl_easy_setopt(mcp->curl, CURLOPT_HTTPHEADER, mcp->del_headers);
	curl_easy_setopt(mcp->curl, CURLOPT_CUSTOMREQUEST, "DELETE");
	curl_easy_setopt(mcp->curl, CURLOPT_POSTFIELDS, "");
	curl_easy_setopt(mcp->curl, CURLOPT_POSTFIELDSIZE, 0L);
	curl_easy_setopt(mcp->curl, CURLOPT_WRITEFUNCTION, write_response);
	curl_easy_setopt(mcp->curl, CURLOPT_WRITEDATA, &mcp->del_resp);
	curl_easy_setopt(mcp->curl, CURLOPT_HEADERFUNCTION, NULL);
	curl_easy_setopt(mcp->curl, CURLOPT_HEADERDATA, NULL);
	curl_easy_setopt(mcp->curl, CURLOPT_TIMEOUT, (long)mcp->timeout);
	mcp->del_xfer = fyai_curl_submit(ctx, mcp->curl, mcp_delete_complete,
					 mcp);
	return mcp->del_xfer != NULL;
}

static enum fyai_event_action mcp_term_complete(const struct fyai_event *ev)
{
	struct fyai_mcp_ctx *mcp = ev->userdata;

	/* The one-shot child source is withdrawn by the event layer. */
	mcp->term_src = NULL;
	mcp->pid = -1;
	mcp->term_done = true;
	return FYAIEA_CONTINUE;
}

/* Begin a server's asynchronous teardown: session DELETE for HTTP, the
 * SIGTERM->SIGKILL child ladder for stdio. Both are optional; whichever is not
 * needed is marked done immediately. */
static void mcp_teardown_begin(struct fyai_ctx *ctx, struct fyai_mcp_ctx *mcp)
{
	struct fyai_event_loop *el = fyai_ctx_loop(ctx);
	int status;

	mcp->del_done = !mcp_delete_session_begin(ctx, mcp);

	if (mcp->stdin_fd >= 0) {
		close(mcp->stdin_fd);
		mcp->stdin_fd = -1;
	}
	if (mcp->stdout_fd >= 0) {
		close(mcp->stdout_fd);
		mcp->stdout_fd = -1;
	}
	mcp->term_done = true;
	if (mcp->pid <= 0)
		return;
	if (el && !fyai_event_add_child_terminate(el, mcp->pid, 1000, 500,
						  mcp_term_complete, mcp,
						  &mcp->term_src)) {
		mcp->term_done = false;
		return;
	}
	/* No loop, or registration failed: reap synchronously as a fallback. */
	kill(mcp->pid, SIGKILL);
	while (waitpid(mcp->pid, &status, 0) < 0 && errno == EINTR)
		;
	mcp->pid = -1;
}

static bool mcp_teardown_settled(struct fyai_ctx *ctx)
{
	struct fyai_mcp_ctx *mcp;

	for (mcp = ctx->mcp; mcp; mcp = mcp->next)
		if (!mcp->del_done || !mcp->term_done)
			return false;
	return true;
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

/* Bounded deadline for the whole shutdown group, ms. */
#define MCP_SHUTDOWN_DEADLINE_MS 5000

void fyai_mcp_cleanup(struct fyai_ctx *ctx)
{
	struct fyai_mcp_ctx *mcp, *next;
	struct fyai_event_loop *el;
	fyai_event_ms_t deadline, now;
	int status;

	if (!ctx || !ctx->mcp)
		return;

	/* Cancel any in-flight startup, then fire every server's teardown so
	 * they drain concurrently. */
	for (mcp = ctx->mcp; mcp; mcp = mcp->next) {
		if (mcp->startup) {
			struct mcp_startup *su = mcp->startup;

			if (su->timer)
				fyai_event_source_remove(su->timer);
			jsonrpc_request_destroy(su->req);
			free(su->cursor);
			free(su);
			mcp->startup = NULL;
		}
		mcp_teardown_begin(ctx, mcp);
	}

	/* Pump the shared loop until every teardown settles or the deadline. */
	el = fyai_ctx_loop(ctx);
	deadline = fyai_event_now_ms() + MCP_SHUTDOWN_DEADLINE_MS;
	while (el && !mcp_teardown_settled(ctx)) {
		now = fyai_event_now_ms();
		if (now >= deadline)
			break;
		if (fyai_event_loop_step(el, deadline - now) < 0)
			break;
	}

	for (mcp = ctx->mcp; mcp; mcp = next) {
		next = mcp->next;
		/* Force any teardown that did not finish in time. */
		if (mcp->del_xfer)
			fyai_curl_transfer_destroy(mcp->del_xfer);
		if (mcp->term_src)
			fyai_event_source_remove(mcp->term_src);
		if (mcp->pid > 0) {
			kill(mcp->pid, SIGKILL);
			while (waitpid(mcp->pid, &status, 0) < 0 && errno == EINTR)
				;
		}
		if (ctx->cfg->mcp_logging)
			(void)fyai_log_generic(ctx, "mcp", fy_mapping(
				"event", "disconnect", "server", mcp->name));
		curl_slist_free_all(mcp->del_headers);
		free(mcp->del_resp.data);
		jsonrpc_conn_destroy(mcp->conn);
		if (mcp->curl)
			curl_easy_cleanup(mcp->curl);
		free(mcp);
	}
	ctx->mcp = NULL;
}
