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

#include "fyai_fleet_ops.h"
#include "fyai_pending.h"
#include "fyai_remote.h"
#include "fyai_turn.h"
#include "fyai_uimodel.h"
#include "fyai_host.h"
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
	fyai_ui_quit_request(fyai_cmd_call_ctx(call));
	return 0;
}

int fyai_cmd_reload(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = fyai_cmd_call_ctx(call);
	char *branch;
	int rc;

	(void)result;
	fyai_error_check(ctx, !ctx->cfg->transient, err,
			 "reload: transient state cannot survive a restart");
	fyai_error_check(ctx, !ctx->cfg->root_pinned, err,
			 "reload: a pinned root cannot be republished");
	fyai_error_check(ctx, ctx->arena->durable_allocator && ctx->arena->durable_gb, err,
			 "reload: no arena is open to retain the session");
	fyai_error_check(ctx, !fyai_tools_active(ctx), err,
			 "reload: close live shells and sub-agents before restarting");
	fyai_error_check(ctx, !fyai_agents_attached(ctx), err,
			 "reload: close attached agents before restarting");
	fyai_error_check(ctx, !fyai_ui_has_line(ctx), err,
			 "reload: process queued input before restarting");
	fyai_error_check(ctx, fyai_exec_self_available(), err,
			 "reload: executing this binary is not supported here");
	if (!fyai_host_memfd_available())
		fyai_error_check(ctx,
			 !ctx->cfg->api_key_explicit || !ctx->cfg->api_key, err,
			 "reload: this platform cannot pass an explicit API key "
			 "without storing it");
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
	return fyai_session_btw(fyai_cmd_call_ctx(call), fyai_cmd_arg_str(call, "question"));
}

int fyai_cmd_branches(struct fyai_cmd_call *call, fy_generic *result)
{
	(void)result;
	return fyai_browser_open(fyai_cmd_call_ctx(call));
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
		return fyai_prompt(fyai_cmd_call_ctx(call));
	if (!name)
		return fyai_browser_open_switch(fyai_cmd_call_ctx(call),
						fyai_cmd_arg_bool(call, "all"));
	return fyai_session_branch_switch(fyai_cmd_call_ctx(call), name, false, true);
}

/* A new `session/` branch that starts empty; the branch left is not changed. */
int fyai_cmd_session_new(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = fyai_cmd_call_ctx(call);
	char name[FYAI_BRANCH_NAME_MAX + 1];
	int rc;

	/* The session that does work goes on at a server; an idle one ends. */
	if (fyai_session_working(ctx))
		return fyai_session_leave(ctx, "");
	rc = fyai_branches_refresh(ctx);
	fyai_error_check(ctx, !rc, err, "session: could not refresh the branches");
	rc = fyai_branch_session_name(ctx->bstate->branches, name, sizeof(name));
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

/*
 * Ask the interactive loop to run the model on the head as it is. The loop
 * starts the turn when this command returns.
 */
int fyai_cmd_continue(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = fyai_cmd_call_ctx(call);

	(void)result;
	fyai_error_check(ctx, fyai_turn_continuable(ctx->turn->last_message), err,
			 "continue: the conversation does not end with a tool result "
			 "or a message to answer");
	ctx->turn->continue_requested = true;
	/* The turn that was lost may have left work behind it. */
	(void)fyai_pending_restore(ctx);
	return 0;
err:
	return -1;
}

/* The sessions that a server of this project runs, from its record. */
int fyai_cmd_session_list(struct fyai_cmd_call *call, fy_generic *result)
{
	fy_generic live, session, rows = fy_seq_empty;

	live = fyai_remote_live_sessions(fyai_cmd_call_ctx(call), call->gb);
	fy_foreach(session, live)
		rows = fy_append(call->gb, rows, fy_mapping(call->gb,
			"branch", fy_get(session, "branch", ""),
			"state", fyai_remote_live_state(session),
			"attached", fy_get(session, "attached", false),
			"pid", fy_get(session, "pid", 0LL)));
	*result = rows;
	return 0;
}

/* ---- the fleet (doc/fyai-fleet-sdd.md) --------------------------------- */

int fyai_cmd_fleet_list(struct fyai_cmd_call *call, fy_generic *result)
{
	return fyai_fleet_rows_list(fyai_cmd_call_ctx(call), call->gb, result);
}

int fyai_cmd_fleet_scan(struct fyai_cmd_call *call, fy_generic *result)
{
	return fyai_fleet_rows_scan(fyai_cmd_call_ctx(call), call->gb, result);
}

int fyai_cmd_fleet_tasks(struct fyai_cmd_call *call, fy_generic *result)
{
	return fyai_fleet_rows_tasks(fyai_cmd_call_ctx(call), call->gb, result);
}

int fyai_cmd_fleet_search(struct fyai_cmd_call *call, fy_generic *result)
{
	return fyai_fleet_rows_search(fyai_cmd_call_ctx(call), call->gb,
				      fyai_cmd_arg_str(call, "query"),
				      fy_get(call->args, "limit", 0LL), result);
}

int fyai_cmd_fleet_read(struct fyai_cmd_call *call, fy_generic *result)
{
	return fyai_fleet_rows_read(fyai_cmd_call_ctx(call), call->gb,
				    fyai_cmd_arg_str(call, "branch"),
				    fy_get(call->args, "turns", 0LL), result);
}

/* Mark a task of this root session open, done or dropped. */
int fyai_cmd_fleet_task(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *id = fyai_cmd_arg_str(call, "id");
	const char *state = fyai_cmd_arg_str(call, "state");

	if (fyai_fleet_task_mark(fyai_cmd_call_ctx(call), id, state, true))
		return -1;
	*result = fy_mapping(call->gb, "id", id, "state", state);
	return 0;
}

/* End one live session at the server. */
int fyai_cmd_session_kill(struct fyai_cmd_call *call, fy_generic *result)
{
	bool busy = false;

	if (fyai_fleet_close(fyai_cmd_call_ctx(call), fyai_cmd_arg_str(call, "branch"),
			     fyai_cmd_arg_bool(call, "force"), &busy))
		return -1;
	*result = fy_mapping(call->gb, "branch", fyai_cmd_arg_str(call, "branch"),
			     "busy", busy);
	return 0;
}

/* A request of a supervisor to the session that "branch" names. */
static int session_supervise(struct fyai_cmd_call *call, const char *subtype,
			     fy_generic fields, fy_generic *result)
{
	if (fyai_fleet_supervise(fyai_cmd_call_ctx(call),
				 fyai_cmd_arg_str(call, "branch"), subtype, fields))
		return -1;
	*result = fy_mapping(call->gb, "branch", fyai_cmd_arg_str(call, "branch"));
	return 0;
}

int fyai_cmd_session_send(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *from = fyai_cmd_arg_str(call, "from");

	return session_supervise(call, "fyai_session_send", fy_mapping(call->gb,
		"text", fyai_cmd_arg_str(call, "text"),
		"from", from ? from : "user"), result);
}

int fyai_cmd_session_halt(struct fyai_cmd_call *call, fy_generic *result)
{
	return session_supervise(call, "fyai_session_interrupt", fy_map_empty,
				 result);
}

int fyai_cmd_session_answer(struct fyai_cmd_call *call, fy_generic *result)
{
	return session_supervise(call, "fyai_session_answer", fy_mapping(call->gb,
		"answer", fyai_cmd_arg_str(call, "answer")), result);
}

/* Make a live session that a person started a member of this root session. */
int fyai_cmd_session_adopt(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = fyai_cmd_call_ctx(call);
	const char *name = fyai_cmd_arg_str(call, "name"), *text;
	long long id = 0;

	text = fyai_cmd_arg_str(call, "text");
	if (fyai_fleet_adopt(ctx, call->gb, name, text, true, &id))
		return -1;
	*result = fy_mapping(call->gb, "name", fyai_fleet_name_full(ctx, call->gb,
								     name),
			     "task", id);
	return 0;
}

/*
 * Go into a session of the fleet (doc/fyai-fleet-sdd.md, section 11): this
 * session goes to a server as /background leaves it, and the client of this
 * terminal joins NAME, with the way back to this session for `/root`.
 */
int fyai_cmd_fleet_jump(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = fyai_cmd_call_ctx(call);
	const char *name, *self;

	name = fyai_fleet_name_full(ctx, call->gb, fyai_cmd_arg_str(call, "name"));
	self = name ? fyai_fleet_name_full(ctx, call->gb, fyai_ctx_branch(ctx)) : NULL;
	if (!name || !self)
		return -1;
	fyai_error_check(ctx, strcmp(name, self), err,
			 "jump: '%s' is this session", name);
	if (fyai_session_leave(ctx, fy_gb_intern_string(call->gb,
			fy_sprintfa("jump:%s %s", name, self))))
		return -1;
	*result = fy_mapping(call->gb, "name", name, "back", self);
	return 0;
err:
	return -1;
}

/* Go back to the session that `/jump` came from. */
int fyai_cmd_fleet_root(struct fyai_cmd_call *call, fy_generic *result)
{
	(void)result;
	return fyai_session_leave(fyai_cmd_call_ctx(call), "root:");
}

/* Join a session of the fleet in this terminal, after this run. */
int fyai_cmd_fleet_attach(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *name = fyai_cmd_arg_str(call, "name");
	bool watch = false;

	if (fyai_fleet_attach(fyai_cmd_call_ctx(call), name,
			      fyai_cmd_arg_str(call, "return"), &watch))
		return -1;
	*result = fy_mapping(call->gb, "name", name, "watch", watch);
	return 0;
}

/* Start a session in a project of the fleet with a task. */
int fyai_cmd_fleet_start(struct fyai_cmd_call *call, fy_generic *result)
{
	return fyai_fleet_start(fyai_cmd_call_ctx(call), call->gb,
				fyai_cmd_arg_str(call, "project"),
				fyai_cmd_arg_str(call, "task"),
				fyai_cmd_arg_str(call, "branch"), true, result);
}

/*
 * Move the session to a server of the project, with its turn in flight. Only
 * a worker of a split session can: it owns the turn and the tools, and the
 * terminal is the client's.
 */
int fyai_cmd_background(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = fyai_cmd_call_ctx(call);

	fyai_error_check(ctx, ctx->turn->background, err,
			 "background: this session does not run at a worker; a session "
			 "on a terminal does");
	fyai_error_check(ctx, !ctx->cfg->transient && !ctx->cfg->root_pinned, err,
			 "background: a server cannot read a transient or pinned session");
	fyai_error_check(ctx, !ctx->bstate->session_unstored, err,
			 "background: the session has no stored exchange yet");
	if (ctx->turn->background(ctx, ctx->turn->background_data))
		return -1;
	(void)result;
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
		fyai_tools_unzoom(fyai_cmd_call_ctx(call));
		return 0;
	}
	what = fyai_tools_zoom(fyai_cmd_call_ctx(call), name);
	*result = what ? fy_mapping(call->gb, "zoomed", what) :
		  fy_mapping(call->gb, "unknown", (bool)name,
			     "idle", !name);
	return 0;
}

int fyai_cmd_sessions(struct fyai_cmd_call *call, fy_generic *result)
{
	*result = fyai_tools_sessions_data(fyai_cmd_call_ctx(call), call->gb);
	fyai_error_check(fyai_cmd_call_ctx(call), fy_is_valid(*result), err,
			 "sessions: cannot list the sessions");
	return 0;
err:
	return -1;
}

int fyai_cmd_kill(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *name = fyai_cmd_arg_str(call, "name");
	const char *action = NULL;

	if (fyai_tools_kill(fyai_cmd_call_ctx(call), name, &action))
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

	if (!fyai_cmd_call_ctx(call)->transport->client) {
		fyai_error(fyai_cmd_call_ctx(call), "profiles: credential isolation is not "
			   "active in this run");
		return -1;
	}
	if (!fy_str_empty(agent)) {
		id = profiles_agent(fyai_cmd_call_ctx(call), agent);
		if (!id) {
			fyai_error(fyai_cmd_call_ctx(call), "profiles: no running sub-agent '%s'",
				   agent);
			return -1;
		}
	}
	if (fyai_transport_describe((fyai_cmd_call_ctx(call))->transport, call->gb, id, &reply))
		return -1;
	*result = fy_get(reply, "profiles", fy_seq_empty);
	return 0;
}

int fyai_cmd_status(struct fyai_cmd_call *call, fy_generic *result)
{
	fy_generic data;

	data = fyai_session_status_data(fyai_cmd_call_ctx(call), call->gb);
	fyai_error_check(fyai_cmd_call_ctx(call), fy_is_valid(data), err,
			 "status: cannot build the report");
	*result = call->format == FYAI_CMD_OUT_MARKDOWN ?
		  status_flatten(call->gb, data) : data;
	return 0;
err:
	return -1;
}

int fyai_cmd_todo_show(struct fyai_cmd_call *call, fy_generic *result)
{
	*result = fyai_todo_rows(fyai_cmd_call_ctx(call), call->gb);
	fyai_error_check(fyai_cmd_call_ctx(call), fy_is_valid(*result), err,
			 "todo: cannot build the todo list");
	return 0;
err:
	return -1;
}

int fyai_cmd_todo_clear(struct fyai_cmd_call *call, fy_generic *result)
{
	(void)result;
	if (fyai_todo_clear(fyai_cmd_call_ctx(call)))
		return -1;
	*result = fy_mapping(call->gb, "cleared", true);
	return 0;
}
