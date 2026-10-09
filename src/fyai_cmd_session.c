/*
 * fyai_cmd_session.c - handlers of the commands that only a session has
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * These act on the live session: its tiles, its pickers, and its side
 * questions. The backends present what they do.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai.h"
#include "fyai_cmd.h"
#include "fyai_cmd_int.h"
#include "fyai_agents.h"
#include "fyai_branch.h"
#include "fyai_browser.h"
#include "fyai_sink.h"
#include "fyai_session.h"
#include "fyai_storage.h"
#include "fyai_tools.h"
#include "fyai_todo.h"
#include "fyai_transport_boot.h"
#include "fyai_ui.h"
#include "utils.h"

#define FYAI_MODULE FYAIEM_SESSION

/* The dispatcher ends the session before a handler runs. */
int fyai_cmd_exit(struct fyai_cmd_call *call, fy_generic *result)
{
	(void)result;
	fyai_ui_quit_request(call->ctx);
	return 0;
}

int fyai_cmd_reload(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = call->ctx;
	char *branch;
	int rc;

	(void)result;
	fyai_error_check(ctx, !ctx->cfg->transient, err,
			 "reload: transient state cannot survive a restart");
	fyai_error_check(ctx, !ctx->cfg->root_pinned, err,
			 "reload: a pinned root cannot be republished");
	fyai_error_check(ctx, ctx->durable_allocator && ctx->durable_gb, err,
			 "reload: no arena is open to retain the session");
	fyai_error_check(ctx, !fyai_tools_active(ctx), err,
			 "reload: close live shells and sub-agents before restarting");
	fyai_error_check(ctx, !fyai_agents_attached(ctx), err,
			 "reload: close attached agents before restarting");
	fyai_error_check(ctx, !fyai_ui_has_line(ctx), err,
			 "reload: process queued input before restarting");
	fyai_error_check(ctx, fyai_exec_self_available(), err,
			 "reload: executing this binary is not supported here");
#ifndef __linux__
	fyai_error_check(ctx,
			 !ctx->cfg->api_key_explicit || !ctx->cfg->api_key, err,
			 "reload: this platform cannot pass an explicit API key "
			 "without storing it");
#endif
	branch = strdup(fyai_ctx_branch(ctx));
	fyai_error_check(ctx, branch, err,
			 "reload: cannot retain the active branch");
	ctx->cfg->reload_branch = branch;
	rc = fyai_publish_state(ctx);
	if (rc) {
		ctx->cfg->reload_branch = NULL;
		free(branch);
		return -1;
	}
	return 0;
err:
	return -1;
}

int fyai_cmd_btw(struct fyai_cmd_call *call, fy_generic *result)
{
	(void)result;
	return fyai_session_btw(call->ctx, fyai_cmd_arg_str(call, "question"));
}

int fyai_cmd_branches(struct fyai_cmd_call *call, fy_generic *result)
{
	(void)result;
	return fyai_browser_open(call->ctx);
}

/*
 * A named session is selected for this invocation; HEAD does not move. With
 * no name, the picker selects: in a session, and before the verb starts its
 * session.
 */
int fyai_cmd_resume(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *name = fyai_cmd_arg_str(call, "session");

	(void)result;
	/* The verb selected its session before setup; the prompt runs it. */
	if (call->surface == FYAI_CMD_CLI)
		return fyai_prompt(call->ctx);
	if (!name)
		return fyai_browser_open_switch(call->ctx,
						fyai_cmd_arg_bool(call, "all"));
	return fyai_session_branch_switch(call->ctx, name, false, true);
}

/* A new `session/` branch that starts empty; the branch left is not changed. */
int fyai_cmd_session_new(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = call->ctx;
	char name[FYAI_BRANCH_NAME_MAX + 1];
	int rc;

	rc = fyai_branches_refresh(ctx);
	fyai_error_check(ctx, !rc, err, "session: could not refresh the branches");
	rc = fyai_branch_session_name(ctx->arena_branches, name, sizeof(name));
	fyai_error_check(ctx, !rc, err, "session: could not name a new session");
	rc = fyai_session_branch_switch(ctx, name, true, true);
	if (rc)
		return -1;
	rc = fyai_session_clear(ctx);
	fyai_error_check(ctx, !rc, err,
			 "session: could not empty the new session '%s'", name);
	*result = fy_mapping(call->gb, "branch", fyai_ctx_branch(ctx));
	return 0;
err:
	return -1;
}

int fyai_cmd_zoom(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *name = fyai_cmd_arg_str(call, "name");
	const char *what;

	if (name && !strcmp(name, "off")) {
		/* fyai_tools_unzoom() reports the focus change. */
		fyai_tools_unzoom(call->ctx);
		return 0;
	}
	what = fyai_tools_zoom(call->ctx, name);
	*result = what ? fy_mapping(call->gb, "zoomed", what) :
		  fy_mapping(call->gb, "unknown", (bool)name,
			     "idle", !name);
	return 0;
}

int fyai_cmd_sessions(struct fyai_cmd_call *call, fy_generic *result)
{
	*result = fyai_tools_sessions_data(call->ctx, call->gb);
	fyai_error_check(call->ctx, fy_is_valid(*result), err,
			 "sessions: cannot list the sessions");
	return 0;
err:
	return -1;
}

int fyai_cmd_kill(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *name = fyai_cmd_arg_str(call, "name");
	const char *action = NULL;

	if (fyai_tools_kill(call->ctx, name, &action))
		return -1;
	*result = fy_mapping(call->gb, "name", name, "action", action);
	return 0;
}

/* One table shows the sections of the status: `Auth / status`, ... */
static fy_generic status_flatten(struct fy_generic_builder *gb,
				 fy_generic data)
{
	fy_generic out, key, value, k2, v2;
	const char *prefix, *name;

	out = fy_map_empty;
	fy_foreach_key_value(key, value, data) {
		if (!fy_is_mapping(value)) {
			out = fy_assoc(gb, out, key, value);
			continue;
		}
		prefix = fy_equal(key, "auth") ? "Auth" : "Usage";
		fy_foreach_key_value(k2, v2, value) {
			if (fy_is_mapping(v2) || fy_is_sequence(v2))
				continue;
			name = fy_sprintfa("%s / %s", prefix,
					   fy_castp(&k2, ""));
			out = fy_assoc(gb, out, name, v2);
		}
	}
	return out;
}

/*
 * The sub-agent that @name designates: a transport execution number, or a
 * name or branch of a live sub-agent of this process. 0 when there is none.
 */
static uint64_t profiles_agent(struct fyai_ctx *ctx, const char *name)
{
	char *end;
	unsigned long long n;

	n = strtoull(name, &end, 10);
	if (*name && !*end)
		return n;
	return fyai_tool_agent_transport_exec(ctx, name);
}

int fyai_cmd_profiles(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *agent = fyai_cmd_arg_str(call, "agent");
	uint64_t id = 0;
	fy_generic reply;

	if (!call->ctx->tclient) {
		fyai_error(call->ctx, "profiles: credential isolation is not "
			   "active in this run");
		return -1;
	}
	if (!fy_str_empty(agent)) {
		id = profiles_agent(call->ctx, agent);
		if (!id) {
			fyai_error(call->ctx, "profiles: no running sub-agent '%s'",
				   agent);
			return -1;
		}
	}
	if (fyai_transport_describe(call->ctx, call->gb, id, &reply))
		return -1;
	*result = fy_get(reply, "profiles", fy_seq_empty);
	return 0;
}

int fyai_cmd_status(struct fyai_cmd_call *call, fy_generic *result)
{
	fy_generic data;

	data = fyai_session_status_data(call->ctx, call->gb);
	fyai_error_check(call->ctx, fy_is_valid(data), err,
			 "status: cannot build the report");
	*result = call->format == FYAI_CMD_OUT_MARKDOWN ?
		  status_flatten(call->gb, data) : data;
	return 0;
err:
	return -1;
}

int fyai_cmd_todo_show(struct fyai_cmd_call *call, fy_generic *result)
{
	*result = fyai_todo_rows(call->ctx, call->gb);
	fyai_error_check(call->ctx, fy_is_valid(*result), err,
			 "todo: cannot build the todo list");
	return 0;
err:
	return -1;
}

int fyai_cmd_todo_clear(struct fyai_cmd_call *call, fy_generic *result)
{
	(void)result;
	if (fyai_todo_clear(call->ctx))
		return -1;
	*result = fy_mapping(call->gb, "cleared", true);
	return 0;
}
