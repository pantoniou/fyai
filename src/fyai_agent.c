/* SPDX-License-Identifier: MIT */
#include <stdbool.h>

#include <curl/curl.h>

#include <libfyaml.h>

#include "fyai.h"
#include "fyai_agent.h"
#include "fyai_turn.h"
#include "fyai_output.h"
#include "fyai_event.h"
#include "fyai_jsonrpc.h"
#include "fyai_provider.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define FYAI_MODULE FYAIEM_UNKNOWN

const char fyai_agent_system_prompt[] =
	"You are a fyai sub-agent: an autonomous coding assistant delegated a "
	"single, self-contained task. Work in the current workspace using the "
	"tools available to you (read_file, write_file, apply_patch, shell). "
	"Investigate before acting, make the smallest change that satisfies the "
	"task, and verify your work. You cannot ask the user questions - decide "
	"with sensible defaults and state any assumptions. When the task is "
	"complete, stop and reply with a concise final report of what you did "
	"and what you found; that report is your entire return value.";

bool fyai_agent_delegated(const struct fyai_ctx *ctx)
{
	if (!ctx || !ctx->cfg->agent_child)
		return false;
	return ctx->tool_rpc || ctx->cfg->agent_rpc;
}

static fy_generic fyai_agent_final_text(struct fyai_ctx *ctx, fy_generic turn)
{
	fy_generic msgs, m, role, type, text;
	size_t n, i;

	msgs = fy_get(turn, "messages");
	n = fy_len(msgs);
	for (i = n; i-- > 0; ) {
		m = fy_get(msgs, i);
		role = fy_get(m, "role");
		type = fy_get(m, "type");
		if (fy_equal(role, "assistant") || fy_equal(type, "message")) {
			text = fyai_item_text(ctx, m);
			if (fy_generic_is_string(text))
				return text;
			break;
		}
	}
	return fy_gb_internalize(ctx->transient_gb, fy_value(""));
}

fy_generic fyai_agent_run(struct fyai_ctx *ctx, const char *task, bool *okp)
{
	struct fyai_cfg *cfg = ctx->cfg;
	fy_generic turn;
	int rc;

	*okp = false;

	if (!ctx->transient_gb) {
		rc = fyai_setup_transient_builder(ctx);
		if (rc)
			return fy_invalid;
	}

	cfg->agent_child = true;
	cfg->mcp_enabled = false;
	cfg->system_prompt = fyai_agent_system_prompt;
	ctx->last_message = fy_invalid;

	if (!ctx->tool_rpc && ctx->curl) {
		curl_easy_cleanup(ctx->curl);
		ctx->curl = NULL;
	}
	rc = fyai_curl_easy_reinit(ctx);
	if (rc)
		return fy_invalid;

	rc = fyai_request_state_apply(ctx);
	if (rc)
		return fy_invalid;

	ctx->last_message = fyai_turn_append(ctx, ctx->last_message,
		fy_sequence(fyai_make_system_message(ctx, cfg->system_prompt)));
	ctx->last_message = fyai_output_record(ctx, ctx->last_message,
		FYAI_OUTPUT_SYSTEM, cfg->system_prompt);
	ctx->last_message = fyai_turn_append(ctx, ctx->last_message,
		fy_sequence(fyai_make_user_message(ctx, task)));
	ctx->last_message = fyai_output_record(ctx, ctx->last_message,
		FYAI_OUTPUT_USER, task);
	ctx->last_message = fy_gb_internalize(ctx->gb, ctx->last_message);
	if (fy_generic_is_invalid(ctx->last_message))
		return fy_invalid;

	ctx->stdout_tty = false;

	turn = fyai_run_turn(ctx, ctx->last_message);
	turn = fyai_report_diag(ctx, turn);
	if (fy_generic_is_invalid(turn))
		return fy_invalid;
	ctx->last_message = turn;

	*okp = true;
	return fyai_agent_final_text(ctx, turn);
}

static int fyai_agent_rpc_verb(struct fyai_ctx *ctx);

int fyai_agent_verb(struct fyai_ctx *ctx)
{
	fy_generic report;
	const char *task;
	bool ok = false;

	if (ctx->cfg->agent_rpc)
		return fyai_agent_rpc_verb(ctx);

	task = ctx->cfg->prompt ? ctx->cfg->prompt : "";
	report = fyai_agent_run(ctx, task, &ok);
	if (!ok || fy_generic_is_invalid(report)) {
		fyai_error(ctx, "agent: the sub-agent did not complete");
		return -1;
	}
	printf("%s\n", fy_castp(&report, ""));
	return 0;
}

struct agent_rpc {
	struct fyai_ctx *ctx;
	struct jsonrpc_conn *conn;
	fy_generic run_id;
	const char *task;
	bool run_pending;
	bool ran;
	bool quit;
};

static fy_generic agent_rpc_error(struct fyai_ctx *ctx, long long code,
				  const char *message)
{
	return fy_mapping(fyai_ctx_transient_gb(ctx),
			  "code", code, "message", message);
}

static fy_generic agent_rpc_serve(struct jsonrpc_conn *conn, const char *method,
				  fy_generic params, fy_generic id,
				  void *userdata, fy_generic *errorp)
{
	struct agent_rpc *rpc = userdata;
	struct fyai_ctx *ctx = rpc->ctx;
	const char *task;

	if (!strcmp(method, "initialize")) {
		return fy_mapping(fyai_ctx_transient_gb(ctx),
				  "agentId", fy_get(params, "name", "agent"),
				  "protocol", 1LL);
	}
	if (!strcmp(method, "shutdown")) {
		rpc->quit = true;
		return fy_null;
	}
	if (!strcmp(method, "agent/run")) {
		if (!fy_generic_is_valid(id)) {
			return fy_invalid;
		}
		if (rpc->ran || rpc->run_pending) {
			*errorp = agent_rpc_error(ctx, -32003,
						  "agent is already running");
			return fy_invalid;
		}
		task = fy_get(params, "task", "");
		if (!task || !*task) {
			*errorp = agent_rpc_error(ctx, -32602,
						  "task is required");
			return fy_invalid;
		}
		rpc->task = task;
		rpc->run_id = id;
		rpc->run_pending = true;
		jsonrpc_conn_defer(conn);
		return fy_invalid;
	}

	*errorp = agent_rpc_error(ctx, -32601, "method not found");
	return fy_invalid;
}

static int fyai_agent_rpc_verb(struct fyai_ctx *ctx)
{
	struct agent_rpc rpc;
	struct fyai_event_loop *el;
	struct jsonrpc_conn *conn;
	fy_generic report;
	bool ok;
	int rc = -1;

	memset(&rpc, 0, sizeof(rpc));
	rpc.ctx = ctx;

	el = fyai_ctx_loop(ctx);
	fyai_error_check(ctx, el, out,
			 "agent: could not acquire the application event loop");

	conn = jsonrpc_conn_stdio(ctx, STDOUT_FILENO, STDIN_FILENO,
				  0, "agent", NULL);
	fyai_error_check(ctx, conn, out,
			 "agent: could not open the control channel");
	rc = jsonrpc_conn_serve(conn, agent_rpc_serve, &rpc);
	fyai_error_check(ctx, !rc, out_conn,
			 "agent: could not serve the control channel");
	rpc.conn = conn;

	while (!rpc.quit) {
		if (rpc.run_pending) {
			rpc.run_pending = false;
			rpc.ran = true;
			report = fyai_agent_run(ctx, rpc.task, &ok);
			if (ok && fy_generic_is_valid(report))
				jsonrpc_conn_respond(conn, rpc.run_id,
					fy_mapping(fyai_ctx_transient_gb(ctx),
						   "report", report),
					fy_invalid);
			else
				jsonrpc_conn_respond(conn, rpc.run_id,
					fy_invalid,
					agent_rpc_error(ctx, -32000,
						"the sub-agent did not complete"));
			continue;
		}
		if (fyai_event_loop_step(el, -1) < 0)
			break;
	}

	while (jsonrpc_conn_has_output(conn))
		if (fyai_event_loop_step(el, 1000) <= 0)
			break;
	rc = 0;

out_conn:
	jsonrpc_conn_destroy(conn);
out:
	return rc;
}
