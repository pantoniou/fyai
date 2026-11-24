/*
 * fyai_claude_cli.c - the Claude Code CLI as a model connection
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * A session is one CLI process and the conversation that it holds. It lives
 * from exchange to exchange while the branch grows only by its own answers
 * and new user messages, and the model, the system prompt and the tools stay;
 * else a new session replays the branch. A slot of its host owns the current
 * session: the link of an agent, or the channel of an
 * execution in the credential transport. A session that a new exchange
 * replaces, or that failed, is retired: it leaves its slot at once and is
 * destroyed from the loop, so no callback of its connection destroys the
 * connection that runs it.
 */

#define FYAI_MODULE FYAIEM_STREAM

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fyai.h"
#include "fyai_buf.h"
#include "fyai_claude_cli.h"
#include "fyai_event.h"
#include "fyai_jsonrpc.h"
#include "fyai_shell_capture.h"
#include "utils.h"

/* The SDK MCP server that carries the tools of fyai, and its tool prefix. */
#define CCLI_SERVER		"fyai"
#define CCLI_TOOL_PREFIX	"mcp__fyai__"
/* The system prompt goes in one argument; Linux limits one to 128 KiB. */
#define CCLI_ARG_MAX		(120 * 1024)
/* The end of the standard error of the CLI kept for a diagnostic. */
#define CCLI_STDERR_MAX		4096
/* Time to leave at EOF before SIGTERM, and before SIGKILL after it. */
#define CCLI_GRACE_MS		2000
#define CCLI_TERM_MS		1000

enum ccli_phase {
	CCLI_STARTING,		/* initialize is sent; no content yet */
	CCLI_RUNNING,		/* a model reply is due */
	CCLI_WAITING,		/* the reply asked for tools; results are due */
	CCLI_FINISHING,		/* the last reply ended; its result is due */
	CCLI_IDLE,		/* the turn ended; the next user message can come */
	CCLI_ENDED,		/* the CLI failed or is stopped */
};

/* A tool_use block of the last reply, and the tools/call that runs it. */
struct ccli_use {
	struct ccli_use *next;
	char *id;		/* the tool_use id, which is the call_id */
	char *request_id;	/* of the control request; NULL until it comes */
	fy_generic rpc_id;	/* of the JSON-RPC request, in cli->gb */
	char *output;		/* the result; NULL until fyai gives it */
	bool answered;
};

struct fyai_claude_cli {
	struct fyai_ctx *ctx;
	struct fyai_claude_cli **slot;	/* of the host; NULL once retired */
	char *command;
	char *config_dir;	/* CLAUDE_CONFIG_DIR of the CLI, or NULL */
	bool api_billing;
	pid_t pid;
	int in_fd;		/* the standard input of the CLI */
	int out_fd;		/* its standard output */
	int err_fd;		/* its standard error */
	struct jsonrpc_conn *conn;
	struct fyai_event_source *err_src;
	struct fyai_buf err;	/* the end of its standard error */
	struct fy_generic_builder *gb;	/* the session values below */
	fy_generic tools;	/* the tools/list result */
	fy_generic user;	/* the user frame, sent after the check */
	enum ccli_phase phase;
	struct ccli_use *uses;
	/* The identities of the stored messages that the CLI has seen. */
	char **seen;
	size_t nseen;
	/* What the CLI started with, in @gb: a change needs a new CLI. */
	fy_generic identity;
	/* Results of earlier turns that come after the next user message. */
	unsigned int stale_results;
	/* The reply in progress: its message and its open block. */
	bool message_open;
	long block_open;	/* the index of the open block, or -1 */
	bool block_tool;	/* the open block is a tool_use */
	bool tool_use;		/* the reply stops for tool use */
	bool retired;
	struct fyai_claude_cli_call *call;
};

struct fyai_claude_cli_call {
	struct fyai_claude_cli *cli;
	struct fyai_claude_cli_callbacks cb;
	struct fyai_ctx *ctx;
	enum fyai_claude_cli_end end;
	long status;
	char *error;
	bool ended;
	bool deferred;
};

/* The context that this object stands on. */
static inline struct fyai_ctx *
fyai_claude_cli_call_ctx(const struct fyai_claude_cli_call *o)
{
	return o->ctx;
}

static void ccli_retire(struct fyai_claude_cli *cli);

/* The context that this object stands on. */
static inline struct fyai_ctx *fyai_claude_cli_ctx(const struct fyai_claude_cli *o)
{
	return o->ctx;
}

static void ccli_uses_free(struct fyai_claude_cli *cli)
{
	struct ccli_use *u;

	while ((u = cli->uses)) {
		cli->uses = u->next;
		free(u->id);
		free(u->request_id);
		free(u->output);
		free(u);
	}
}

static void ccli_seen_free(struct fyai_claude_cli *cli)
{
	size_t i;

	for (i = 0; i < cli->nseen; i++)
		free(cli->seen[i]);
	free(cli->seen);
	cli->seen = NULL;
	cli->nseen = 0;
}

/* The CLI has seen the stored messages of @turns: record them. */
static int ccli_seen_set(struct fyai_claude_cli *cli, fy_generic turns)
{
	fy_generic t;
	size_t n = 0;

	ccli_seen_free(cli);
	cli->seen = calloc(fy_len(turns) + 1, sizeof(*cli->seen));
	if (!cli->seen)
		return -1;
	fy_foreach(t, turns) {
		cli->seen[n] = strdup(fy_get(t, "d", ""));
		if (!cli->seen[n])
			return -1;
		cli->nseen = ++n;
	}
	return 0;
}

/*
 * What the CLI starts with: the model, the system prompt and the tools. The
 * builder of the session keeps one value for equal content, so an unchanged
 * value of a later request is the same word.
 */
static fy_generic ccli_identity(struct fyai_claude_cli *cli, fy_generic req)
{
	struct fy_generic_builder *tgb = fyai_ctx_transient_gb(fyai_claude_cli_ctx(cli));

	return fy_gb_internalize(cli->gb, fy_mapping(tgb,
		"model", fy_get(req, "model", ""),
		"system", fy_get(req, "system", ""),
		"tools", fy_get(req, "tools", fy_seq_empty)));
}

static int ccli_identity_set(struct fyai_claude_cli *cli, fy_generic req)
{
	cli->identity = ccli_identity(cli, req);
	return fy_is_valid(cli->identity) ? 0 : -1;
}

static bool ccli_identity_same(struct fyai_claude_cli *cli, fy_generic req)
{
	return fy_is_valid(cli->identity) &&
	       ccli_identity(cli, req).v == cli->identity.v;
}

static struct ccli_use *ccli_use_find(struct fyai_claude_cli *cli,
				      const char *id)
{
	struct ccli_use *u;

	for (u = cli->uses; u; u = u->next)
		if (!strcmp(u->id, id))
			return u;
	return NULL;
}

/* Run the end callback of @call from the loop. */
static void ccli_call_end_cb(void *ud)
{
	struct fyai_claude_cli_call *call = ud;

	call->deferred = false;
	if (call->cb.end)
		call->cb.end(call->end, call->status, call->error,
			     call->cb.userdata);
}

/*
 * End the request of @cli. The request leaves the session first: its
 * callback can submit the next request.
 */
static void ccli_end(struct fyai_claude_cli *cli, enum fyai_claude_cli_end end,
		     long status, const char *error)
{
	struct fyai_claude_cli_call *call = cli->call;
	struct fyai_event_loop *el;

	if (!call)
		return;
	cli->call = NULL;
	call->cli = NULL;
	call->ended = true;
	call->end = end;
	call->status = status;
	call->error = error ? strdup(error) : NULL;
	el = fyai_ctx_loop(fyai_claude_cli_call_ctx(call));
	call->deferred = el && !fyai_event_defer(el, ccli_call_end_cb, call);
	if (!call->deferred)
		ccli_call_end_cb(call);
}

/* The diagnostic of a CLI that went away: its last line on standard error. */
static const char *ccli_stderr_tail(struct fyai_claude_cli *cli)
{
	const char *s, *line;
	size_t n;

	n = fyai_buf_pending(&cli->err);
	s = cli->err.data + cli->err.off;
	while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' '))
		n--;
	if (!n)
		return NULL;
	for (line = s + n; line > s && line[-1] != '\n'; line--)
		;
	return fy_gb_intern_string(fyai_ctx_transient_gb(fyai_claude_cli_ctx(cli)),
				   fy_sprintfa("%.*s", (int)(s + n - line), line));
}

/* Fail the session: end its request with @why and retire it. */
static void ccli_fail(struct fyai_claude_cli *cli, enum fyai_claude_cli_end end,
		      long status, const char *why)
{
	if (cli->retired)
		return;
	cli->phase = CCLI_ENDED;
	ccli_end(cli, end, status, why);
	ccli_retire(cli);
}

static int ccli_send(struct fyai_claude_cli *cli, fy_generic frame)
{
	if (cli->retired || fy_is_invalid(frame) ||
	    jsonrpc_conn_send_frame(cli->conn, frame)) {
		ccli_fail(cli, FYAI_CLAUDE_CLI_LOST, 0,
			  "cannot write to the Claude Code CLI");
		return -1;
	}
	return 0;
}

static void ccli_control_reply(struct fyai_claude_cli *cli,
			       const char *request_id, fy_generic response,
			       const char *error)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(fyai_claude_cli_ctx(cli));
	fy_generic inner;

	if (error)
		inner = fy_mapping(gb, "subtype", "error",
				   "request_id", request_id, "error", error);
	else
		inner = fy_mapping(gb, "subtype", "success",
				   "request_id", request_id,
				   "response", response);
	(void)ccli_send(cli, fy_mapping(gb, "type", "control_response",
					"response", inner));
}

/* Give the result of @u to the CLI, once both are there. */
static void ccli_use_answer(struct fyai_claude_cli *cli, struct ccli_use *u)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(fyai_claude_cli_ctx(cli));
	fy_generic block, content, result, reply;

	if (u->answered || !u->request_id || !u->output)
		return;
	u->answered = true;
	block = fy_mapping(gb, "type", "text", "text", u->output);
	content = fy_sequence(gb, block);
	result = fy_mapping(gb, "content", content);
	reply = fy_mapping(gb, "jsonrpc", "2.0", "id", u->rpc_id,
			   "result", result);
	ccli_control_reply(cli, u->request_id,
			   fy_mapping(gb, "mcp_response", reply), NULL);
}

/* An MCP message of the CLI to the SDK server of fyai. */
static void ccli_mcp(struct fyai_claude_cli *cli, const char *request_id,
		     fy_generic msg)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(fyai_claude_cli_ctx(cli));
	fy_generic id, params, result, error, reply;
	const char *method, *use_id;
	struct ccli_use *u;

	method = fy_get(msg, "method", "");
	id = fy_get(msg, "id", fy_invalid);
	params = fy_get(msg, "params", fy_map_empty);
	result = fy_invalid;
	error = fy_invalid;

	if (fy_is_invalid(id)) {
		/* A notification: the SDK answers each one with an empty result. */
		reply = fy_mapping(gb, "jsonrpc", "2.0", "result", fy_map_empty);
		ccli_control_reply(cli, request_id,
				   fy_mapping(gb, "mcp_response", reply), NULL);
		return;
	}
	if (!strcmp(method, "initialize")) {
		result = fy_mapping(gb,
			"protocolVersion", fy_get(params, "protocolVersion",
						  "2025-06-18"),
			"capabilities", fy_mapping(gb, "tools", fy_map_empty),
			"serverInfo", fy_mapping(gb, "name", CCLI_SERVER,
						 "version", VERSION));
	} else if (!strcmp(method, "tools/list")) {
		result = fy_mapping(gb, "tools", cli->tools);
	} else if (!strcmp(method, "tools/call")) {
		use_id = fy_get(fy_get(params, "_meta"),
				"claudecode/toolUseId", "");
		u = *use_id ? ccli_use_find(cli, use_id) : NULL;
		if (u && !u->request_id) {
			u->request_id = strdup(request_id);
			u->rpc_id = fy_gb_internalize(cli->gb, id);
			if (!u->request_id || fy_is_invalid(u->rpc_id)) {
				ccli_fail(cli, FYAI_CLAUDE_CLI_FAILED, 0,
					  "out of memory");
				return;
			}
			ccli_use_answer(cli, u);
			return;
		}
		/* A call that no reply of this exchange made is not run. */
		error = fy_mapping(gb, "code", -32602LL, "message",
			fy_sprintfa("fyai has no tool call '%s' to run", use_id));
	} else {
		error = fy_mapping(gb, "code", -32601LL, "message",
				   fy_sprintfa("unknown method '%s'", method));
	}
	reply = fy_is_valid(error) ?
		fy_mapping(gb, "jsonrpc", "2.0", "id", id, "error", error) :
		fy_mapping(gb, "jsonrpc", "2.0", "id", id, "result", result);
	ccli_control_reply(cli, request_id,
			   fy_mapping(gb, "mcp_response", reply), NULL);
}

static void ccli_control_request(struct fyai_claude_cli *cli, fy_generic frame)
{
	fy_generic req;
	const char *request_id, *subtype;

	req = fy_get(frame, "request", fy_map_empty);
	request_id = fy_get(frame, "request_id", "");
	subtype = fy_get(req, "subtype", "");
	if (!strcmp(subtype, "mcp_message") &&
	    fy_equal(fy_get(req, "server_name"), CCLI_SERVER)) {
		ccli_mcp(cli, request_id, fy_get(req, "message", fy_map_empty));
		return;
	}
	ccli_control_reply(cli, request_id, fy_invalid,
			   fy_sprintfa("fyai does not answer %s", subtype));
}

/*
 * The account that the CLI reports in its answer to initialize. A run on a
 * subscription stops here, before any content is sent, when the CLI would
 * bill an API key or has no login.
 */
static const char *ccli_account_check(struct fyai_claude_cli *cli,
				      fy_generic account)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(fyai_claude_cli_ctx(cli));
	const char *key_source, *provider, *plan;

	key_source = fy_get(account, "apiKeySource", "");
	provider = fy_get(account, "apiProvider", "");
	plan = fy_get(account, "subscriptionType", "");
	if (cli->api_billing)
		return NULL;
	if (*key_source)
		return fy_gb_intern_string(gb, fy_sprintfa("the Claude Code CLI would bill the API key "
				   "of %s, not a Claude subscription; remove "
				   "that key from its configuration, or set "
				   "claude_code/api_billing to true",
				   key_source));
	if (*provider && strcmp(provider, "firstParty"))
		return fy_gb_intern_string(gb, fy_sprintfa("the Claude Code CLI uses the provider %s, "
				   "not a Claude subscription; set "
				   "claude_code/api_billing to true to use it",
				   provider));
	if (!*plan)
		return "the Claude Code CLI is not signed in to a Claude "
		       "subscription; run `claude` and sign in with /login";
	return NULL;
}

static void ccli_control_response(struct fyai_claude_cli *cli, fy_generic frame)
{
	fy_generic resp;
	const char *why;

	if (cli->phase != CCLI_STARTING)
		return;
	resp = fy_get(frame, "response", fy_map_empty);
	if (!fy_equal(fy_get(resp, "subtype"), "success")) {
		ccli_fail(cli, FYAI_CLAUDE_CLI_FAILED, 0,
			  fy_sprintfa("the CLI refused to start: %s",
				      fy_get(resp, "error", "no reason")));
		return;
	}
	why = ccli_account_check(cli, fy_get(fy_get(resp, "response"),
					     "account", fy_map_empty));
	if (why) {
		ccli_fail(cli, FYAI_CLAUDE_CLI_FAILED, 0, why);
		return;
	}
	cli->phase = CCLI_RUNNING;
	(void)ccli_send(cli, cli->user);
}

/* Give one stream event to the request as an SSE data line. */
static void ccli_forward(struct fyai_claude_cli *cli, fy_generic event)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(fyai_claude_cli_ctx(cli));
	struct fyai_claude_cli_call *call = cli->call;
	const char *json, *line;
	size_t len;

	if (!call || !call->cb.body)
		return;
	json = emit_json_string(gb, event);
	if (!json) {
		ccli_fail(cli, FYAI_CLAUDE_CLI_FAILED, 0,
			  "cannot encode a stream event");
		return;
	}
	line = fy_sprintfa("data: %s\n\n", json);
	len = strlen(line);
	if (call->cb.body(line, len, call->cb.userdata) != len)
		ccli_fail(cli, FYAI_CLAUDE_CLI_FAILED, 0,
			  "the reply was not taken");
}

static void ccli_stream_event(struct fyai_claude_cli *cli, fy_generic frame)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(fyai_claude_cli_ctx(cli));
	fy_generic event, block;
	const char *type, *name, *id;
	struct ccli_use *u, **tail;

	/* The messages of a sub-agent of the CLI are not replies of fyai. */
	if (!fy_is_null(fy_get(frame, "parent_tool_use_id", fy_null)))
		return;
	event = fy_get(frame, "event", fy_map_empty);
	type = fy_get(event, "type", "");
	if (!strcmp(type, "message_start")) {
		ccli_uses_free(cli);
		cli->tool_use = false;
		cli->message_open = true;
		cli->block_open = -1;
	} else if (!strcmp(type, "content_block_stop")) {
		cli->block_open = -1;
	} else if (!strcmp(type, "content_block_start")) {
		block = fy_get(event, "content_block", fy_map_empty);
		name = fy_get(block, "name", "");
		cli->block_open = fy_get(event, "index", 0LL);
		cli->block_tool = fy_equal(fy_get(block, "type"), "tool_use");
		if (fy_equal(fy_get(block, "type"), "tool_use")) {
			if (!strncmp(name, CCLI_TOOL_PREFIX,
				     strlen(CCLI_TOOL_PREFIX))) {
				block = fy_assoc(gb, block, "name",
					name + strlen(CCLI_TOOL_PREFIX));
				event = fy_assoc(gb, event, "content_block",
						 block);
			}
			id = fy_get(block, "id", "");
			u = calloc(1, sizeof(*u));
			if (!u || !(u->id = strdup(id))) {
				free(u);
				ccli_fail(cli, FYAI_CLAUDE_CLI_FAILED, 0,
					  "out of memory");
				return;
			}
			u->rpc_id = fy_invalid;
			for (tail = &cli->uses; *tail; tail = &(*tail)->next)
				;
			*tail = u;
		}
	} else if (!strcmp(type, "message_delta")) {
		cli->tool_use = fy_equal(fy_get(fy_get(event, "delta"),
						"stop_reason"), "tool_use");
	}
	ccli_forward(cli, event);
	if (!cli->retired && !strcmp(type, "message_stop")) {
		cli->message_open = false;
		cli->phase = cli->tool_use && cli->uses ? CCLI_WAITING :
							  CCLI_FINISHING;
		ccli_end(cli, FYAI_CLAUDE_CLI_OK, 200, NULL);
	}
}

/*
 * The CLI ended its turn. A reply that did not end is a failure; a turn that
 * ended well leaves the CLI for the next exchange.
 */
static void ccli_result(struct fyai_claude_cli *cli, fy_generic frame)
{
	const char *text;
	long status;

	/* The result of a turn that the next user message already followed. */
	if (cli->stale_results) {
		cli->stale_results--;
		return;
	}
	if (!cli->call && cli->phase == CCLI_FINISHING &&
	    !fy_get(frame, "is_error", false)) {
		cli->phase = CCLI_IDLE;
		return;
	}
	cli->phase = CCLI_ENDED;
	if (cli->call) {
		text = fy_get(frame, "result", "");
		status = fy_get(frame, "api_error_status", 0LL);
		if (!*text)
			text = fy_get(frame, "subtype", "error");
		ccli_end(cli, status ? FYAI_CLAUDE_CLI_OK :
			 FYAI_CLAUDE_CLI_FAILED, status ? status : 0,
			 fy_sprintfa("the Claude Code CLI ended the turn: %s",
				     text));
	}
	ccli_retire(cli);
}

static void ccli_frame(struct jsonrpc_conn *conn, fy_generic frame, void *data)
{
	struct fyai_claude_cli *cli = data;
	const char *type;

	(void)conn;
	if (cli->retired)
		return;
	type = fy_get(frame, "type", "");
	if (!strcmp(type, "stream_event"))
		ccli_stream_event(cli, frame);
	else if (!strcmp(type, "control_request"))
		ccli_control_request(cli, frame);
	else if (!strcmp(type, "control_response"))
		ccli_control_response(cli, frame);
	else if (!strcmp(type, "result"))
		ccli_result(cli, frame);
}

/*
 * The CLI went away in the middle of a reply that the user has seen. A reply
 * cut in its words ends as a paused turn: fyai keeps the words and asks
 * again, and a new CLI continues from them. A reply cut in a tool call
 * cannot be kept: its input is incomplete. True when the reply was ended.
 */
static bool ccli_pause_reply(struct fyai_claude_cli *cli)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(fyai_claude_cli_ctx(cli));
	fy_generic delta, usage;

	if (!cli->call || !cli->message_open ||
	    (cli->block_open >= 0 && cli->block_tool))
		return false;
	if (cli->block_open >= 0)
		ccli_forward(cli, fy_mapping(gb, "type", "content_block_stop",
					     "index", (long long)cli->block_open));
	delta = fy_mapping(gb, "stop_reason", "pause_turn");
	usage = fy_mapping(gb, "output_tokens", 0LL);
	if (!cli->retired)
		ccli_forward(cli, fy_mapping(gb, "type", "message_delta",
					     "delta", delta, "usage", usage));
	if (!cli->retired)
		ccli_forward(cli, fy_mapping(gb, "type", "message_stop"));
	if (cli->retired)
		return true;
	fyai_diag_tracef("claude-code", "pid %d went away in a reply; paused",
			 (int)cli->pid);
	cli->phase = CCLI_ENDED;
	ccli_end(cli, FYAI_CLAUDE_CLI_OK, 200, NULL);
	ccli_retire(cli);
	return true;
}

static void ccli_closed(void *data)
{
	struct fyai_claude_cli *cli = data;
	const char *tail;

	if (cli->retired || ccli_pause_reply(cli))
		return;
	tail = ccli_stderr_tail(cli);
	ccli_fail(cli, cli->phase == CCLI_STARTING ? FYAI_CLAUDE_CLI_FAILED :
		  FYAI_CLAUDE_CLI_LOST, 0,
		  tail ? fy_sprintfa("the Claude Code CLI exited: %s", tail) :
			 "the Claude Code CLI exited");
}

static enum fyai_event_action ccli_err_event(const struct fyai_event *ev)
{
	struct fyai_claude_cli *cli = ev->userdata;
	char buf[1024];
	ssize_t n;
	size_t keep;

	for (;;) {
		n = read(cli->err_fd, buf, sizeof(buf));
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		if (fyai_buf_append_data(&cli->err, buf, n))
			break;
		keep = fyai_buf_pending(&cli->err);
		if (keep > CCLI_STDERR_MAX)
			fyai_buf_consume(&cli->err, keep - CCLI_STDERR_MAX);
	}
	if (!n || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
		fyai_event_source_remove(cli->err_src);
		cli->err_src = NULL;
	}
	return FYAIEA_CONTINUE;
}

/*
 * The CLI was reaped. A terminator with no callback stops the run of the loop,
 * and the loop of the host is not this session's to stop.
 */
static enum fyai_event_action ccli_reaped(const struct fyai_event *ev)
{
	(void)ev;
	return FYAIEA_CONTINUE;
}

/*
 * Release what @cli holds. The program is stopped and reaped: now with @sync,
 * else on the loop.
 */
static void ccli_free(struct fyai_claude_cli *cli, bool sync)
{
	struct fyai_event_loop *el;
	int status;

	if (cli->call)
		ccli_end(cli, FYAI_CLAUDE_CLI_CANCELLED, 0, NULL);
	if (cli->err_src)
		fyai_event_source_remove(cli->err_src);
	cli->err_src = NULL;
	if (cli->conn) {
		jsonrpc_conn_expect_close(cli->conn);
		jsonrpc_conn_destroy(cli->conn);
	}
	cli->conn = NULL;
	if (cli->in_fd >= 0)
		close(cli->in_fd);
	if (cli->out_fd >= 0)
		close(cli->out_fd);
	if (cli->err_fd >= 0)
		close(cli->err_fd);
	cli->in_fd = cli->out_fd = cli->err_fd = -1;
	if (cli->pid > 0) {
		/* The CLI leaves at EOF on its input; the signals are a bound. */
		el = fyai_ctx_loop(fyai_claude_cli_ctx(cli));
		if (!el || (sync ?
		    fyai_event_child_terminate_group(el, cli->pid,
				CCLI_GRACE_MS, CCLI_TERM_MS, NULL) :
		    fyai_event_add_child_terminate_group(el, cli->pid,
				CCLI_GRACE_MS, CCLI_TERM_MS, ccli_reaped, NULL,
				NULL))) {
			kill(-cli->pid, SIGKILL);
			while (waitpid(cli->pid, &status, 0) < 0 &&
			       errno == EINTR)
				;
		}
	}
	cli->pid = -1;
	ccli_uses_free(cli);
	ccli_seen_free(cli);
	fyai_buf_free(&cli->err);
	free(cli->command);
	free(cli->config_dir);
	if (cli->gb)
		fy_generic_builder_destroy(cli->gb);
	free(cli);
}

static void ccli_destroy_cb(void *ud)
{
	ccli_free(ud, false);
}

/* Take @cli from the link and destroy it from the loop. */
static void ccli_retire(struct fyai_claude_cli *cli)
{
	struct fyai_ctx *ctx = fyai_claude_cli_ctx(cli);
	struct fyai_event_loop *el;

	if (cli->retired)
		return;
	cli->retired = true;
	if (cli->slot && *cli->slot == cli)
		*cli->slot = NULL;
	cli->slot = NULL;
	el = fyai_ctx_loop(ctx);
	if (!el || fyai_event_defer(el, ccli_destroy_cb, cli))
		ccli_free(cli, true);
}

/* The tools of the request as MCP tools of the SDK server. */
static fy_generic ccli_mcp_tools(struct fy_generic_builder *gb, fy_generic tools)
{
	fy_generic out, tool, schema;

	out = fy_seq_empty;
	fy_foreach(tool, tools) {
		schema = fy_get(tool, "input_schema", fy_invalid);
		if (!fy_is_mapping(schema))
			continue;
		out = fy_append(gb, out, fy_mapping(gb,
			"name", fy_get(tool, "name", ""),
			"description", fy_get(tool, "description", ""),
			"inputSchema", schema));
	}
	return out;
}

static fy_generic ccli_text_block(struct fy_generic_builder *gb,
				  const char *text)
{
	return fy_mapping(gb, "type", "text", "text", text);
}

/*
 * The user message that starts the CLI: the conversation before the last
 * user message as a transcript, that message, and the tool calls already
 * made for it.
 */
static fy_generic ccli_user_frame(struct fyai_claude_cli *cli, fy_generic req)
{
	struct fy_generic_builder *gb = cli->gb;
	fy_generic content, prompt, block, message;
	const char *history, *progress;

	history = fy_get(req, "history", "");
	progress = fy_get(req, "progress", "");
	prompt = fy_get(req, "prompt", fy_invalid);
	content = fy_seq_empty;
	if (*history) {
		block = ccli_text_block(gb, fy_sprintfa(
			"<conversation_history>\n%s</conversation_history>\n\n"
			"The transcript above is the earlier part of this "
			"conversation. Continue it with the message that "
			"follows.", history));
		content = fy_append(gb, content, block);
	}
	if (fy_is_sequence(prompt)) {
		fy_foreach(block, prompt)
			content = fy_append(gb, content, block);
	} else if (!fy_str_empty(fy_str(prompt))) {
		content = fy_append(gb, content,
				    ccli_text_block(gb, fy_castp(&prompt, "")));
	}
	if (*progress) {
		block = ccli_text_block(gb, fy_sprintfa(
			"<progress>\n%s</progress>\n\nThis is the work already "
			"done for this message: what you wrote and the tool calls "
			"and their results. Continue from it.",
			progress));
		content = fy_append(gb, content, block);
	}
	if (!fy_len(content))
		content = fy_sequence(gb, ccli_text_block(gb, "Continue."));
	message = fy_mapping(gb, "role", "user", "content", content);
	return fy_mapping(gb, "type", "user", "session_id", "",
			  "parent_tool_use_id", fy_null, "message", message);
}

static const char *const ccli_env_keep[] = {
	"CLAUDE_CONFIG_DIR", "CLAUDE_CODE_OAUTH_TOKEN",
	"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME",
	"XDG_RUNTIME_DIR", "DBUS_SESSION_BUS_ADDRESS",
	"HTTPS_PROXY", "HTTP_PROXY", "NO_PROXY",
	"https_proxy", "http_proxy", "no_proxy", "NODE_EXTRA_CA_CERTS",
	NULL,
};

/* With api_billing the CLI may also take the key of the Anthropic API. */
static const char *const ccli_env_keep_billing[] = {
	"CLAUDE_CONFIG_DIR", "CLAUDE_CODE_OAUTH_TOKEN",
	"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME",
	"XDG_RUNTIME_DIR", "DBUS_SESSION_BUS_ADDRESS",
	"HTTPS_PROXY", "HTTP_PROXY", "NO_PROXY",
	"https_proxy", "http_proxy", "no_proxy", "NODE_EXTRA_CA_CERTS",
	"ANTHROPIC_API_KEY",
	NULL,
};

/*
 * Start the CLI. It runs outside the confinement of tools, as an MCP server
 * does: it reads its own login. Its standard error is kept for a
 * diagnostic.
 */
static int ccli_spawn(struct fyai_claude_cli *cli, fy_generic req)
{
	struct fyai_ctx *ctx = fyai_claude_cli_ctx(cli);
	int inpipe[2] = { -1, -1 }, outpipe[2] = { -1, -1 };
	int errpipe[2] = { -1, -1 };
	const char *argv[32], *command, *system, *model, *mcp;
	size_t argc = 0, i;
	pid_t pid;
	int rc;

	command = cli->command;
	system = fy_get(req, "system", "");
	model = fy_get(req, "model", "");
	fyai_error_check(ctx, strlen(system) < CCLI_ARG_MAX, err_out,
			 "claude-code: the system prompt is %zu bytes; the "
			 "limit is %d", strlen(system), CCLI_ARG_MAX);
	mcp = "{\"mcpServers\":{\"" CCLI_SERVER "\":{\"type\":\"sdk\","
	      "\"name\":\"" CCLI_SERVER "\"}}}";

	argv[argc++] = command;
	argv[argc++] = "-p";
	argv[argc++] = "--input-format";
	argv[argc++] = "stream-json";
	argv[argc++] = "--output-format";
	argv[argc++] = "stream-json";
	argv[argc++] = "--verbose";
	argv[argc++] = "--include-partial-messages";
	argv[argc++] = "--no-session-persistence";
	/* No built-in tool: every tool is a tool of fyai. */
	argv[argc++] = "--tools";
	argv[argc++] = "";
	argv[argc++] = "--mcp-config";
	argv[argc++] = mcp;
	argv[argc++] = "--strict-mcp-config";
	/* fyai asks the user; the CLI must not ask again. */
	argv[argc++] = "--allowedTools";
	argv[argc++] = "mcp__" CCLI_SERVER;
	argv[argc++] = "--setting-sources";
	argv[argc++] = "";
	argv[argc++] = "--disable-slash-commands";
	argv[argc++] = "--system-prompt";
	argv[argc++] = system;
	if (*model && strcmp(model, "default")) {
		argv[argc++] = "--model";
		argv[argc++] = model;
	}
	argv[argc] = NULL;

	/* pipe2() is not portable: macOS has none. */
	rc = pipe(inpipe);
	if (!rc)
		rc = pipe(outpipe);
	if (!rc)
		rc = pipe(errpipe);
	fyai_error_check(ctx, !rc, err_close,
			 "claude-code: cannot create pipes: %s",
			 strerror(errno));
	for (i = 0; !rc && i < 2; i++) {
		rc = fcntl(inpipe[i], F_SETFD, FD_CLOEXEC);
		if (!rc)
			rc = fcntl(outpipe[i], F_SETFD, FD_CLOEXEC);
		if (!rc)
			rc = fcntl(errpipe[i], F_SETFD, FD_CLOEXEC);
	}
	if (!rc)
		rc = fcntl(errpipe[0], F_SETFL, O_NONBLOCK);
	fyai_error_check(ctx, !rc, err_close,
			 "claude-code: cannot protect the pipes: %s",
			 strerror(errno));

	pid = fork();
	fyai_error_check(ctx, pid >= 0, err_close,
			 "claude-code: cannot fork: %s", strerror(errno));
	if (!pid) {
		struct fyai_child_spec spec = {};

		signal(SIGPIPE, SIG_DFL);
		spec.in_fd = inpipe[0];
		spec.out_fd = outpipe[1];
		spec.err_fd = errpipe[1];
		spec.ctty_fd = -1;
		spec.status_fd = -1;
		spec.own_session = true;
		spec.env_keep = cli->api_billing ? ccli_env_keep_billing :
						   ccli_env_keep;
		rc = fyai_child_exec_prepare(ctx, &spec);
		if (rc)
			_exit(rc);
		/* The home of a view is read-only; the granted directory is not. */
		if (cli->config_dir && setenv("CLAUDE_CONFIG_DIR", cli->config_dir, 1))
			_exit(126);
		execvp(command, (char *const *)argv);
		_exit(127);
	}
	close(inpipe[0]);
	close(outpipe[1]);
	close(errpipe[1]);
	cli->pid = pid;
	cli->in_fd = inpipe[1];
	cli->out_fd = outpipe[0];
	cli->err_fd = errpipe[0];
	fyai_diag_tracef("claude-code", "pid %d %s model %s", (int)pid,
			 command, *model ? model : "default");
	return 0;

err_close:
	for (i = 0; i < 2; i++) {
		if (inpipe[i] >= 0)
			close(inpipe[i]);
		if (outpipe[i] >= 0)
			close(outpipe[i]);
		if (errpipe[i] >= 0)
			close(errpipe[i]);
	}
err_out:
	return -1;
}

/* A new session for the request @req, with its CLI started. */
static struct fyai_claude_cli *
ccli_start(struct fyai_ctx *ctx, const struct fyai_claude_cli_opts *opts,
	   fy_generic req)
{
	struct fy_generic_builder_cfg gb_cfg = {
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED,
	};
	struct fy_generic_builder *tgb = fyai_ctx_transient_gb(ctx);
	struct fyai_claude_cli *cli;
	struct fyai_event_loop *el;
	fy_generic init;
	int rc;

	el = fyai_ctx_loop(ctx);
	fyai_error_check(ctx, el, err_out, "claude-code: no event loop");
	cli = calloc(1, sizeof(*cli));
	fyai_error_check(ctx, cli, err_out,
			 "claude-code: cannot allocate the session");
	cli->ctx = ctx;
	cli->pid = -1;
	cli->in_fd = cli->out_fd = cli->err_fd = -1;
	cli->phase = CCLI_STARTING;
	cli->block_open = -1;
	cli->identity = fy_invalid;
	cli->api_billing = opts->api_billing;
	cli->command = strdup(fy_str_empty(opts->command) ? "claude" :
			      opts->command);
	fyai_error_check(ctx, cli->command, err_free,
			 "claude-code: cannot allocate the session");
	if (opts->in_view) {
		cli->config_dir = malloc(4096);
		fyai_error_check(ctx, cli->config_dir &&
				 fyai_claude_cli_config_dir(cli->config_dir, 4096),
				 err_free, "claude-code: the CLI has no "
				 "configuration directory in this view");
	}
	cli->gb = fy_generic_builder_create(&gb_cfg);
	fyai_error_check(ctx, cli->gb, err_free,
			 "claude-code: cannot create the session builder");
	cli->tools = fy_gb_internalize(cli->gb,
		ccli_mcp_tools(tgb, fy_get(req, "tools", fy_seq_empty)));
	cli->user = ccli_user_frame(cli, req);
	fyai_error_check(ctx, fy_is_valid(cli->tools) &&
			 fy_is_valid(cli->user), err_free,
			 "claude-code: cannot build the first message");
	fyai_diag_tracef("claude-code", "start history %zu bytes",
			 strlen(fy_get(req, "history", "")));

	rc = ccli_spawn(cli, req);
	if (rc)
		goto err_free;
	cli->conn = jsonrpc_conn_stdio(ctx, cli->in_fd, cli->out_fd, 0,
				       "claude-code", NULL);
	fyai_error_check(ctx, cli->conn, err_free,
			 "claude-code: cannot open the CLI channel");
	rc = jsonrpc_conn_frames(cli->conn, ccli_frame, cli);
	fyai_error_check(ctx, !rc, err_free,
			 "claude-code: cannot read the CLI channel");
	jsonrpc_conn_on_close(cli->conn, ccli_closed, cli);
	rc = fyai_event_add_fd(el, cli->err_fd, FYAIEV_READ, ccli_err_event,
			       cli, &cli->err_src);
	fyai_error_check(ctx, !rc, err_free,
			 "claude-code: cannot watch the CLI errors");

	init = fy_mapping(tgb, "type", "control_request",
		"request_id", "fyai-init",
		"request", fy_mapping(tgb, "subtype", "initialize",
				      "hooks", fy_null));
	rc = jsonrpc_conn_send_frame(cli->conn, init);
	fyai_error_check(ctx, !rc, err_free,
			 "claude-code: cannot write to the CLI");
	return cli;

err_free:
	ccli_free(cli, true);
err_out:
	return NULL;
}

/*
 * True when @cli waits for the results that @results gives, one for each
 * tool_use of its last reply. Copy them into the session.
 */
static bool ccli_continues(struct fyai_claude_cli *cli, fy_generic results)
{
	struct ccli_use *u;
	fy_generic r;
	bool found;

	if (!cli || cli->retired || cli->phase != CCLI_WAITING)
		return false;
	for (u = cli->uses; u; u = u->next) {
		found = false;
		fy_foreach(r, results)
			if (fy_equal(fy_get(r, "call_id"), u->id))
				found = true;
		if (!found)
			return false;
	}
	for (u = cli->uses; u; u = u->next) {
		fy_foreach(r, results) {
			if (!fy_equal(fy_get(r, "call_id"), u->id))
				continue;
			free(u->output);
			u->output = strdup(fy_get(r, "output", ""));
			if (!u->output)
				return false;
		}
	}
	return true;
}

/*
 * The user message that resumes a kept CLI with the stored messages of @req
 * that it has not seen, or fy_invalid when it cannot resume. It can when its
 * turn ended well, it runs the model, the system prompt and the tools of
 * @req, and the stored conversation is what it has seen, then its own last
 * answer, then user messages only. The last of them is the prompt of @req.
 */
static fy_generic ccli_resume_frame(struct fyai_claude_cli *cli, fy_generic req)
{
	struct fy_generic_builder *gb;
	fy_generic turns, t, content, prompt, block, message;
	size_t n, i, first_user;
	const char *kind;

	if (!cli || cli->retired ||
	    (cli->phase != CCLI_IDLE && cli->phase != CCLI_FINISHING) ||
	    !ccli_identity_same(cli, req))
		return fy_invalid;
	gb = fyai_ctx_transient_gb(fyai_claude_cli_ctx(cli));
	turns = fy_get(req, "turns", fy_seq_empty);
	n = fy_len(turns);
	if (n <= cli->nseen)
		return fy_invalid;
	for (i = 0; i < cli->nseen; i++)
		if (strcmp(cli->seen[i], fy_get(fy_get_at(turns, i), "d", "")))
			return fy_invalid;
	/* Its own answer: the words of the reply that ended its turn. */
	for (i = cli->nseen; i < n; i++)
		if (strcmp(fy_get(fy_get_at(turns, i), "kind", ""), "assistant"))
			break;
	first_user = i;
	if (first_user == n)
		return fy_invalid;
	content = fy_seq_empty;
	for (i = first_user; i < n; i++) {
		t = fy_get_at(turns, i);
		kind = fy_get(t, "kind", "");
		if (strcmp(kind, "user"))
			return fy_invalid;
		if (i + 1 < n) {
			block = fy_mapping(gb, "type", "text",
					   "text", fy_get(t, "text", ""));
			content = fy_append(gb, content, block);
			continue;
		}
		prompt = fy_get(req, "prompt", fy_invalid);
		if (fy_is_sequence(prompt)) {
			fy_foreach(block, prompt)
				content = fy_append(gb, content, block);
		} else {
			block = fy_mapping(gb, "type", "text",
					   "text", fy_get(t, "text", ""));
			content = fy_append(gb, content, block);
		}
	}
	message = fy_mapping(gb, "role", "user", "content", content);
	return fy_mapping(gb, "type", "user", "session_id", "",
			  "parent_tool_use_id", fy_null, "message", message);
}

struct fyai_claude_cli_call *
fyai_claude_cli_submit(struct fyai_ctx *ctx, struct fyai_claude_cli **slot,
		       const struct fyai_claude_cli_opts *opts,
		       const char *body, size_t len,
		       const struct fyai_claude_cli_callbacks *cb)
{
	struct fyai_claude_cli_call *call;
	struct fyai_claude_cli *cli;
	struct ccli_use *u;
	fy_generic req, frame;
	int rc;

	req = parse_json_string_size(fyai_ctx_transient_gb(ctx),
				     body ? body : "", body ? len : 0);
	fyai_error_check(ctx, fy_is_mapping(req), err_out,
			 "claude-code: the request is not a JSON object");
	call = calloc(1, sizeof(*call));
	fyai_error_check(ctx, call, err_out,
			 "claude-code: cannot allocate the request");
	call->ctx = ctx;
	call->cb = *cb;

	cli = *slot;
	/* The next step of the exchange: the results that the CLI waits for. */
	if (ccli_continues(cli, fy_get(req, "results", fy_seq_empty))) {
		rc = ccli_seen_set(cli, fy_get(req, "turns", fy_seq_empty));
		fyai_error_check(ctx, !rc, err_free,
				 "claude-code: cannot record the conversation");
		cli->phase = CCLI_RUNNING;
		cli->call = call;
		call->cli = cli;
		for (u = cli->uses; u && !cli->retired; u = u->next)
			ccli_use_answer(cli, u);
		return call;
	}
	/* The next exchange of the conversation that the CLI holds. */
	frame = ccli_resume_frame(cli, req);
	if (fy_is_valid(frame)) {
		rc = ccli_seen_set(cli, fy_get(req, "turns", fy_seq_empty));
		fyai_error_check(ctx, !rc, err_free,
				 "claude-code: cannot record the conversation");
		if (cli->phase == CCLI_FINISHING)
			cli->stale_results++;
		cli->phase = CCLI_RUNNING;
		cli->call = call;
		call->cli = cli;
		fyai_diag_tracef("claude-code", "resume pid %d at %zu messages",
				 (int)cli->pid, cli->nseen);
		/* A failed send ends the request through its callback. */
		(void)ccli_send(cli, frame);
		return call;
	}
	/* Another conversation, or one that this session cannot continue. */
	if (cli)
		ccli_retire(cli);
	cli = ccli_start(ctx, opts, req);
	if (!cli)
		goto err_free;
	rc = ccli_seen_set(cli, fy_get(req, "turns", fy_seq_empty));
	if (!rc)
		rc = ccli_identity_set(cli, req);
	if (rc) {
		ccli_free(cli, true);
		fyai_error(ctx, "claude-code: cannot record the conversation");
		goto err_free;
	}
	cli->slot = slot;
	*slot = cli;
	cli->call = call;
	call->cli = cli;
	return call;

err_free:
	free(call);

err_out:
	return NULL;
}

void fyai_claude_cli_cancel(struct fyai_claude_cli_call *call)
{
	struct fyai_claude_cli *cli;

	if (!call || call->ended)
		return;
	cli = call->cli;
	/* The CLI cannot drop a reply and go on, so it stops with it. */
	if (cli) {
		ccli_end(cli, FYAI_CLAUDE_CLI_CANCELLED, 0, NULL);
		ccli_retire(cli);
	}
}

void fyai_claude_cli_call_destroy(struct fyai_claude_cli_call *call)
{
	if (!call)
		return;
	if (call->cli)
		call->cli->call = NULL;
	if (call->deferred)
		fyai_event_defer_cancel(fyai_ctx_loop(fyai_claude_cli_call_ctx(call)),
					ccli_call_end_cb, call);
	free(call->error);
	free(call);
}

bool fyai_claude_cli_config_dir(char *out, size_t size)
{
	const char *dir = getenv("CLAUDE_CONFIG_DIR");
	const char *home = getenv("HOME");
	int n;

	if (dir && dir[0] == '/')
		n = snprintf(out, size, "%s", dir);
	else if (home && home[0] == '/')
		n = snprintf(out, size, "%s/.claude", home);
	else
		return false;
	return n > 0 && (size_t)n < size;
}

void fyai_claude_cli_release(struct fyai_claude_cli **slot)
{
	if (slot && *slot)
		ccli_retire(*slot);
}

void fyai_claude_cli_exchange_end(struct fyai_claude_cli **slot)
{
	struct fyai_claude_cli *cli = slot ? *slot : NULL;

	/* A CLI whose turn ended well holds the conversation for the next. */
	if (cli && cli->phase != CCLI_IDLE && cli->phase != CCLI_FINISHING)
		ccli_retire(cli);
}

void fyai_claude_cli_cleanup(struct fyai_claude_cli **slot)
{
	struct fyai_claude_cli *cli;

	if (!slot || !*slot)
		return;
	cli = *slot;
	*slot = NULL;
	cli->slot = NULL;
	cli->retired = true;
	ccli_free(cli, true);
}

void fyai_claude_cli_abandon(struct fyai_claude_cli **slot)
{
	struct fyai_claude_cli *cli;

	if (!slot || !*slot)
		return;
	cli = *slot;
	*slot = NULL;
	/*
	 * The descriptors are of this process; the loop, the connection and
	 * the program are of the parent. The rest stays allocated.
	 */
	if (cli->in_fd >= 0)
		close(cli->in_fd);
	if (cli->out_fd >= 0)
		close(cli->out_fd);
	if (cli->err_fd >= 0)
		close(cli->err_fd);
}
