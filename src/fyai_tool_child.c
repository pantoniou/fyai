/*
 * fyai_tool_child.c - tool child process
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_TOOLS

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef __FILC__
#include <stdfil.h>
#endif
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>

#include <fcntl.h>
#include <termios.h>
#include "fyai_reqcache.h"
#include "fyai_host.h"
#include <sys/ioctl.h>
#include <sys/wait.h>

#include "fyai_transport_boot.h"
#include "fyai_transport_sock.h"
#include "fyai_agent.h"
#include "fyai_monitor.h"
#include "fyai_tool_registry.h"
#include "fyai_branch.h"
#include "fyai_browser.h"
#include "fyai_agents.h"
#include "fyai_chrome.h"
#include "fyai_jsonrpc.h"
#include "fyai_config.h"
#include "fyai_display.h"
#include "fyai_event.h"
#include "fyai_markdown.h"
#include "fyai_patch.h"
#include "fyai_sandbox.h"
#include "fyai_sandbox_profile.h"
#include "fyai_session.h"
#include "fyai_storage.h"
#include "fyai_terminal.h"
#include "fyai_terminal_session.h"
#include "fyai_output.h"
#include "fyai_ask.h"
#include "fyai_skill.h"
#include "fyai_settings.h"
#include "fyai_todo.h"
#include "fyai_home.h"
#include "fyai_tools.h"
#include "fyai_tools_int.h"
#include "fyai_orphan.h"
#include "fyai_view.h"
#include "fyai_cover.h"
#include "fyai_fsview.h"
#include "fyai_asks.h"
#include "fyai_wait.h"
#include "fyai_prof.h"
#include "fyai_render.h"
#include "fyai_sink.h"
#include "fyai_ui.h"
#include "fyai_uimodel.h"
#include "fyai_uilog.h"
#include "fyai_uiwork.h"
#include "fyai_uitranscript.h"
#include "fyai_workpane.h"

/* These descriptors contain the private JSON-RPC channel. */
#define FYAI_TOOL_CHILD_REQ_FD 3	/* parent -> child, read by the child */
#define FYAI_TOOL_CHILD_RSP_FD 4	/* child -> parent, written by the child */

/* Install the control descriptors and close all other inherited descriptors. */
/* Make @slave the session leader's controlling terminal. */
int fyai_tool_child_tty(int slave)
{
	if (ioctl(slave, TIOCSCTTY, 0) < 0)
		return -1;
	if (dup2(slave, STDIN_FILENO) < 0 ||
	    dup2(slave, STDOUT_FILENO) < 0 ||
	    dup2(slave, STDERR_FILENO) < 0)
		return -1;
	if (slave > STDERR_FILENO)
		close(slave);
	return 0;
}

/* An executed sub-agent also inherits its channels to the credential transport. */
#define FYAI_TOOL_CHILD_TP_AGENT_FD 5
#define FYAI_TOOL_CHILD_TP_CTL_FD 6

/*
 * Arrange the descriptors of a tool child: the control channel on 3 and 4, and,
 * for a sub-agent under credential isolation, its transport channels on 5 and
 * 6. @tp_agent_fd and @tp_ctl_fd are -1 for a child with none. A
 * non-negative @view_fd moves to FYAI_TOOL_CHILD_VIEW_FD, which only the
 * entry of the view reads. Every other descriptor is closed.
 */
int fyai_tool_child_fds(struct fyai_ctx *ctx, int req_fd, int rsp_fd, int tp_agent_fd,
			int tp_ctl_fd, int view_fd, bool keep_arena)
{
	int req_dup = -1, rsp_dup = -1, agent_dup = -1, ctl_dup = -1, view_dup = -1;
	bool tp = tp_agent_fd >= 0 && tp_ctl_fd >= 0;
	int first_free = view_fd >= 0 ? FYAI_TOOL_CHILD_VIEW_FD + 1 :
			 tp ? FYAI_TOOL_CHILD_TP_CTL_FD + 1 : FYAI_TOOL_CHILD_RSP_FD + 1;
	int devnull, arena_dup;
	int rc;

	/* Only a sub-agent runtime keeps the arena of the view, above the fixed targets. */
	if (keep_arena && ctx->cfg->view_arena_fd >= 0) {
		arena_dup = fcntl(ctx->cfg->view_arena_fd, F_DUPFD_CLOEXEC, first_free);
		if (arena_dup < 0)
			goto err;
		close(ctx->cfg->view_arena_fd);
		if (fyai_fsview_arena_adopt(ctx->cfg, arena_dup))
			goto err;
	} else {
		fyai_fsview_arena_drop(ctx->cfg);
	}
	/* Move all clear of the target numbers before dup2 can clobber one. */
	req_dup = fcntl(req_fd, F_DUPFD_CLOEXEC, 7);
	if (req_dup < 0)
		goto err;
	rsp_dup = fcntl(rsp_fd, F_DUPFD_CLOEXEC, 7);
	if (rsp_dup < 0)
		goto err;
	if (tp) {
		agent_dup = fcntl(tp_agent_fd, F_DUPFD_CLOEXEC, 7);
		if (agent_dup < 0)
			goto err;
		ctl_dup = fcntl(tp_ctl_fd, F_DUPFD_CLOEXEC, 7);
		if (ctl_dup < 0)
			goto err;
	}
	if (view_fd >= 0) {
		view_dup = fcntl(view_fd, F_DUPFD_CLOEXEC, FYAI_TOOL_CHILD_VIEW_FD + 1);
		if (view_dup < 0)
			goto err;
	}

	rc = dup2(req_dup, FYAI_TOOL_CHILD_REQ_FD);
	if (rc < 0)
		goto err;
	rc = dup2(rsp_dup, FYAI_TOOL_CHILD_RSP_FD);
	if (rc < 0)
		goto err;
	if (tp) {
		rc = dup2(agent_dup, FYAI_TOOL_CHILD_TP_AGENT_FD);
		if (rc < 0)
			goto err;
		rc = dup2(ctl_dup, FYAI_TOOL_CHILD_TP_CTL_FD);
		if (rc < 0)
			goto err;
	}
	if (view_fd >= 0) {
		rc = dup2(view_dup, FYAI_TOOL_CHILD_VIEW_FD);
		if (rc < 0)
			goto err;
		rc = fcntl(FYAI_TOOL_CHILD_VIEW_FD, F_SETFD, FD_CLOEXEC);
		if (rc < 0)
			goto err;
	}
	/* Do not pass the channels to a shell command. */
	rc = fcntl(FYAI_TOOL_CHILD_REQ_FD, F_SETFD, FD_CLOEXEC);
	if (rc < 0)
		goto err;
	rc = fcntl(FYAI_TOOL_CHILD_RSP_FD, F_SETFD, FD_CLOEXEC);
	if (rc < 0)
		goto err;
	if (tp && (fcntl(FYAI_TOOL_CHILD_TP_AGENT_FD, F_SETFD, FD_CLOEXEC) < 0 ||
		   fcntl(FYAI_TOOL_CHILD_TP_CTL_FD, F_SETFD, FD_CLOEXEC) < 0))
		goto err;

	/* Detach unused input unless the child owns this terminal. */
	if (isatty(STDIN_FILENO) && ttyname(STDIN_FILENO) &&
	    getsid(0) == tcgetsid(STDIN_FILENO)) {
		goto close_fds;
	}
	devnull = open("/dev/null", O_RDONLY);
	if (devnull < 0)
		goto err;
	rc = dup2(devnull, STDIN_FILENO);
	if (devnull != STDIN_FILENO)
		close(devnull);
	if (rc < 0)
		goto err;

close_fds:
	fyai_close_fds_except(first_free, ctx->cfg->view_arena_fd);
	return 0;

err:
	if (req_dup >= 0)
		close(req_dup);
	if (rsp_dup >= 0)
		close(rsp_dup);
	if (agent_dup >= 0)
		close(agent_dup);
	if (ctl_dup >= 0)
		close(ctl_dup);
	if (view_dup >= 0)
		close(view_dup);
	return -1;
}

/*
 * Keep the spawn state of an executed child. fyai_agent_run() re-opens the
 * arena and replaces the builders, so the state is kept as JSON until it is
 * adopted there. The identity of the parent, a command-line key and the
 * configuration are applied now.
 */
static int fyai_tool_child_spawn_take(struct fyai_ctx *ctx, fy_generic spawn)
{
	struct fyai_cfg *cfg = ctx->cfg;
	const char *json, *key;

	if (!fy_is_mapping(spawn))
		return 0;
	json = emit_json_string(fyai_ctx_transient_gb(ctx), spawn);
	fyai_error_check(ctx, json, err,
			 "could not keep the sub-agent spawn state");
	free(ctx->agent_spawn_json);
	ctx->agent_spawn_json = strdup(json);
	fyai_error_check(ctx, ctx->agent_spawn_json, err,
			 "could not keep the sub-agent spawn state");
	ctx->agent_parent = fy_get(spawn, "parent", 0LL);
	/* Credential isolation: the parent has registered this child. */
	if (fyai_transport_supervised()) {
		fyai_error_check(ctx, fy_get(fy_get(spawn, "transport", fy_invalid),
					     "exec", 0LL), err,
				 "the parent of this sub-agent did not register it with "
				 "the credential transport");
		if (fyai_transport_child_attach(ctx,
				fy_get(spawn, "transport", fy_invalid)))
			goto err;
	}
	key = fy_get(spawn, "api_key", "");
	if (!fy_str_empty(key)) {
		cfg->api_key = fy_gb_intern_string(cfg->gb, key);
		fyai_error_check(ctx, cfg->api_key, err,
				 "could not keep the parent API key");
		cfg->api_key_explicit = true;
		cfg->api_key_auto = false;
	}
	return fyai_agent_spawn_config(ctx, spawn);

err:
	return -1;
}

/*
 * The one request of a tool child. It only moves forward, so a second
 * tool/run is refused from REQUESTED on.
 */
enum fyai_tool_child_phase {
	FYAI_TC_IDLE,		/* no tool/run yet */
	FYAI_TC_REQUESTED,	/* tool/run came and the loop has not taken it */
	FYAI_TC_TAKEN,		/* the loop took it: the work runs or has run */
};

struct fyai_tool_child {
	struct fyai_ctx *ctx;
	struct jsonrpc_conn *conn;
	struct fyai_terminal_relay *relay;	/* a session this child drives */
	fy_generic id;
	fy_generic args;
	fy_generic branch;	/* sub-agent branch named by the parent */
	enum fyai_tool_child_phase phase;
	/* Outcomes of independent steps of the request, not phases. */
	bool session_started;	/* the session this child was asked for opened */
	bool spawn_failed;	/* the parent state could not be adopted */
	/* The size of the tile the session starts at, or 0. */
	int size_rows, size_cols;
};

static fy_generic fyai_tool_child_serve(struct jsonrpc_conn *conn,
					const char *method, fy_generic params,
					fy_generic id, void *userdata,
					fy_generic *errorp)
{
	struct fyai_tool_child *tc = userdata;
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(tc->ctx);
	fy_generic result = fy_invalid;
	char *bytes;
	size_t len;

	if (fyai_agents_serve(tc->ctx, conn, method, params, id, &result, errorp))
		return result;
	if (!strcmp(method, "tool/run")) {
		if (!fy_is_valid(id) || tc->phase >= FYAI_TC_REQUESTED) {
			*errorp = fy_gb_mapping(gb, "code", -32600LL,
						"message", "unexpected tool/run");
			return fy_invalid;
		}
		tc->args = fy_get(params, "call", fy_invalid);
		tc->branch = fy_get(params, "branch", fy_invalid);
		tc->ctx->relay_gate = fy_get(params, "gate", false);
		tc->ctx->relay_steps = fy_get(params, "steps", false);
		tc->size_rows = (int)fy_get(fy_get(params, "size", fy_invalid),
					   "rows", 0LL);
		tc->size_cols = (int)fy_get(fy_get(params, "size", fy_invalid),
					   "cols", 0LL);
		/* The result carries the diagnostics of a refused state. */
		if (tc->ctx->cfg->tool_exec)
			tc->spawn_failed = fyai_tool_child_spawn_take(tc->ctx,
					fy_get(params, "spawn", fy_invalid));
		tc->id = id;
		tc->phase = FYAI_TC_REQUESTED;
		jsonrpc_conn_defer(conn);
		return fy_invalid;
	}
	if (!strcmp(method, "tty/resize")) {
		tc->ctx->tty_rows = (int)fy_get(params, "rows", 0LL);
		tc->ctx->tty_cols = (int)fy_get(params, "cols", 0LL);
		/*
		 * A resize before the session starts replaces the size of
		 * the run: the parent sends each grant one time.
		 */
		if (!tc->relay && tc->ctx->tty_rows > 0 &&
		    tc->ctx->tty_cols > 0) {
			tc->size_rows = tc->ctx->tty_rows;
			tc->size_cols = tc->ctx->tty_cols;
		}
		fyai_terminal_session_resize(tc->ctx, tc->ctx->tty_rows,
					     tc->ctx->tty_cols);
		if (tc->relay)
			fyai_terminal_relay_resize(tc->relay, tc->ctx->tty_rows,
						   tc->ctx->tty_cols);
		(void)jsonrpc_notify(conn, "tty/resized",
			fy_gb_mapping(gb, "rows", (long long)tc->ctx->tty_rows,
				      "cols", (long long)tc->ctx->tty_cols));
		return fy_invalid;
	}
	if (!strcmp(method, "shell/write")) {
		bytes = fyai_bytes_from_generic(params, &len);
		if (bytes && tc->relay)
			(void)fyai_terminal_relay_write(tc->relay, bytes, len);
		free(bytes);
		return fy_invalid;
	}
	if (!strcmp(method, "shell/close")) {
		if (tc->relay)
			fyai_terminal_relay_close(tc->relay,
					fy_get(params, "force", false));
		return fy_invalid;
	}
	*errorp = fy_gb_mapping(gb, "code", -32601LL,
				"message", "method not found");
	return fy_invalid;
}

/* Most credential names a program of the configuration keeps. */
#define FYAI_TOOL_ENV_KEEP_MAX	32

/*
 * Fill @names with the credentials that a user-owned call keeps, NULL
 * terminated. Only fyai_tools_config_program() sets them, and a provider
 * call cannot be user-owned, so a model cannot name one. The names point into
 * @args, which must outlive their use.
 */
static void fyai_tool_env_keep(fy_generic args, const char **names)
{
	fy_generic keep;
	const char *name;
	size_t n;

	n = 0;
	if (fy_get(args, "_fyai_user_owned", false)) {
		keep = fy_get(args, "_fyai_env_keep", fy_invalid);
		fy_foreach(name, keep) {
			if (n == FYAI_TOOL_ENV_KEEP_MAX)
				break;
			if (!fy_str_empty(name))
				names[n++] = name;
		}
	}
	names[n] = NULL;
}

/* True when a user-owned call runs without the shell sandbox. */
static bool fyai_tool_unconfined(fy_generic args)
{
	return fy_get(args, "_fyai_user_owned", false) &&
	       fy_get(args, "_fyai_unconfined", false);
}

/* Open a session, answer its start, then serve it until it ends. */
static void fyai_tool_child_session(struct fyai_ctx *ctx,
				    struct fyai_tool_child *tc,
				    struct jsonrpc_conn *conn)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	const struct fyai_sandbox_spec *sandbox;
	const char *env_keep[FYAI_TOOL_ENV_KEEP_MAX + 1];
	struct fyai_terminal_opts opts = {};
	struct fyai_shell_sandbox sb;
	struct fyai_event_loop *el;
	fy_generic args, command, workdir;
	fy_generic diag;
	bool started, stop;
	int rc;

	args = fyai_tool_call_args(ctx, tc->args);
	command = fy_get(args, "command", fy_invalid);
	workdir = fy_get(args, "workdir", fy_invalid);

	fyai_tool_env_keep(args, env_keep);
	if (fyai_tool_unconfined(args)) {
		memset(&sb, 0, sizeof(sb));
		sandbox = NULL;
		rc = 0;
	} else {
		rc = fyai_shell_sandbox_begin(ctx, &sb, &sandbox);
	}
	if (!rc) {
		opts.env_keep = env_keep;
		opts.workdir = fy_castp(&workdir, (const char *)NULL);
		opts.term = ctx->cfg->shell_tty_term;
		fyai_shell_tty_size(ctx, args, &opts.rows, &opts.cols);
		/* The parent laid out the tile before the start. */
		if (tc->size_rows > 0 && tc->size_cols > 0) {
			opts.rows = tc->size_rows;
			opts.cols = tc->size_cols;
		}
		/* Use pipes unless the call explicitly requests a terminal. */
		opts.pipes = !fyai_shell_tty_requested(ctx, args);
		opts.shell = fyai_shell_shell_requested(ctx, args);
		opts.login = fyai_shell_login_requested(ctx, args);
		tc->relay = fyai_terminal_relay_start(ctx, conn,
						fy_castp(&command, ""),
						sandbox, &opts);
		if (tc->relay && !opts.pipes)
			fyai_diag_tracef("shell", "terminal %dx%d", opts.rows,
					 opts.cols);
		/* The spec is only needed by the fork inside the start. */
		fyai_shell_sandbox_end(&sb);
	}
	if (rc)
		fyai_error(ctx,
			   "shell: the session was refused before it started");
	else if (!tc->relay)
		fyai_error(ctx, "shell: the session terminal did not open");

	/* A C comparison is an int; the flag must reach the wire as a bool. */
	started = tc->relay != NULL;
	tc->session_started = started;
	diag = fyai_diag_take_generic(&ctx->cfg->diag, gb);
	jsonrpc_conn_respond(conn, tc->id,
		fy_mapping("result",
			   fy_mapping("session", started,
				      "rows", (long long)opts.rows,
				      "cols", (long long)opts.cols),
			   "ok", started, "display", fy_null, "diag", diag),
		fy_invalid);
	if (!tc->relay)
		return;

	/*
	 * Stop the program when this serving child is terminated, and when the
	 * control channel closes: the parent that owns the session is gone, and
	 * the child is in a session of its own, which no signal of the parent
	 * reaches.
	 */
	el = fyai_ctx_loop(ctx);
	assert(el);
	while (!fyai_terminal_relay_done(tc->relay)) {
		stop = ctx->terminate_pending || jsonrpc_conn_closed(conn);
		if (stop)
			fyai_terminal_relay_close(tc->relay, true);
		if (fyai_event_loop_step(el, stop ? 200 : -1) < 0)
			break;
		if (stop && fyai_terminal_relay_reaped(tc->relay))
			break;
	}

	fyai_terminal_relay_destroy(tc->relay);
	tc->relay = NULL;
}

/* Flush the output of a tool child and exit. */
static void fyai_tool_child_finish(struct fyai_ctx *ctx, struct jsonrpc_conn *conn,
				   struct fyai_event_loop *el, bool ok)
{
	while (jsonrpc_conn_has_output(conn))
		if (fyai_event_loop_step(el, 1000) <= 0)
			break;
	/*
	 * This child leaves through _exit and never returns to main(), so it
	 * removes the skills it wrote out itself.
	 */
	fyai_skill_dirs_release(ctx);
	fyai_prof_report();
	_exit(ok ? 0 : 1);
}

/* Run the call of a tool child, reply and exit. Does not return. */
static void fyai_tool_child_dispatch(struct fyai_ctx *ctx, struct fyai_tool_child *tc,
				     struct jsonrpc_conn *conn, struct fyai_event_loop *el)
{
	const char *env_keep[FYAI_TOOL_ENV_KEEP_MAX + 1];
	fy_generic result, diag;
	bool ok = false, unconfined, agent_call;
	bool reopen_failed = false;

#ifdef FILC_HAS_ZCALL_ON_FRESH_STACK
	/* On the fresh stack, before a callee holds the call. */
	if (ctx->agent_branch && fy_equal(fyai_tool_call_name(ctx, tc->args), "agent"))
		reopen_failed = fyai_agent_reopen_for_call(ctx, &tc->args) != 0;
#endif

	/*
	 * A sub-agent needs provider credentials after a persona
	 * changes its model. Its own tool children sanitize again.
	 */
	fyai_tool_env_keep(fyai_tool_call_args(ctx, tc->args),
			   env_keep);
	if (!fy_equal(fyai_tool_call_name(ctx, tc->args), "agent") &&
	    fyai_env_sanitize(env_keep))
		fyai_error(ctx,
			   "could not remove every credential from the tool environment");
	/*
	 * A sub-agent runtime publishes to the arena that the sandbox
	 * denies. Each tool that it runs is confined in its own child.
	 */
	/* What a program detaches becomes a child of this one. */
	agent_call = fy_equal(fyai_tool_call_name(ctx, tc->args), "agent");
	if (!agent_call)
		fyai_orphan_subreaper();
	unconfined = !fy_equal(fyai_tool_call_name(ctx, tc->args), "agent") &&
		     fyai_tool_apply_sandbox(ctx);
	/* A joined scope forked the program: it is the parent that counts. */
	if (!agent_call)
		fyai_orphan_subreaper();
	if (!unconfined && fyai_shell_session_call(ctx, tc->args)) {
		fyai_tool_child_session(ctx, tc, conn);
		/* Exit according to whether the session opened. */
		fyai_tool_child_finish(ctx, conn, el, tc->session_started);
	}
	if (unconfined) {
		result = fy_value(fyai_ctx_transient_gb(ctx),
			"tool error: the sandbox could not confine the tool");
	} else if (reopen_failed) {
		result = fy_value(fyai_ctx_transient_gb(ctx),
			"tool error: the sub-agent could not re-open the arena");
	} else if (tc->spawn_failed) {
		result = fy_value(fyai_ctx_transient_gb(ctx),
			"tool error: the sub-agent could not "
			"adopt the state of its parent");
	} else if (fy_equal(fyai_tool_call_name(ctx, tc->args),
			    "agent") &&
		   fyai_agents_enter(ctx)) {
		result = fy_value(fyai_ctx_transient_gb(ctx),
			"tool error: agent admission refused");
	} else {
		result = fyai_execute_tool_call(ctx, tc->args, &ok);
	}
	fyai_agents_leave(ctx, ok);
	result = fyai_tool_result_note(ctx, agent_call, result);
	/* Return child diagnostics with the tool result. */
	diag = fyai_diag_take_generic(&ctx->cfg->diag,
				      fyai_ctx_transient_gb(ctx));
	jsonrpc_conn_respond(conn, tc->id,
		fy_gb_mapping(fyai_ctx_transient_gb(ctx),
			      "result", result, "ok", ok,
			      "display", ctx->patch_display ?
				fy_value(ctx->patch_display) :
				fy_null,
			      "detached", fyai_orphan_children(
				fyai_ctx_transient_gb(ctx)),
			      "diag", diag),
		fy_invalid);
	fyai_tool_child_finish(ctx, conn, el, ok);
}

#ifdef FILC_HAS_ZCALL_ON_FRESH_STACK
/* Arguments of fyai_tool_child_dispatch() on the fresh stack. */
struct fyai_tool_child_dispatch_args {
	struct fyai_ctx *ctx;
	struct fyai_tool_child *tc;
	struct jsonrpc_conn *conn;
	struct fyai_event_loop *el;
};

static void *fyai_tool_child_dispatch_entry(void *arg)
{
	struct fyai_tool_child_dispatch_args *da = arg;

	fyai_tool_child_dispatch(da->ctx, da->tc, da->conn, da->el);
	return NULL;
}
#endif

void fyai_tool_child_serve_loop(struct fyai_ctx *ctx)
{
	struct fyai_tool_child tc;
	struct fyai_event_loop *el;
	struct jsonrpc_conn *conn;
#ifdef FILC_HAS_ZCALL_ON_FRESH_STACK
	struct fyai_tool_child_dispatch_args da;
#endif

	memset(&tc, 0, sizeof(tc));
	tc.ctx = ctx;

	el = fyai_ctx_loop(ctx);
	if (!el)
		_exit(1);
	/* Write responses to fd 4 and read requests from fd 3. */
	conn = jsonrpc_conn_stdio(ctx, FYAI_TOOL_CHILD_RSP_FD,
				  FYAI_TOOL_CHILD_REQ_FD, 0, "tool", NULL);
	if (!conn || jsonrpc_conn_serve(conn, fyai_tool_child_serve, &tc))
		_exit(1);
	ctx->tool_rpc = conn;

	while (tc.phase != FYAI_TC_TAKEN) {
		if (tc.phase == FYAI_TC_REQUESTED) {
			tc.phase = FYAI_TC_TAKEN;
			if (fy_is_invalid(tc.args)) {
				jsonrpc_conn_respond(conn, tc.id, fy_invalid,
					fy_gb_mapping(fyai_ctx_transient_gb(ctx),
						      "code", -32602LL,
						      "message", "call is required"));
				break;
			}
			/* Keep the durable branch on the child context. */
			if (fy_is_string(tc.branch))
				ctx->agent_branch = fyai_ctx_keep_str(ctx,
					fy_castp(&tc.branch, ""));
#ifdef FILC_HAS_ZCALL_ON_FRESH_STACK
			/*
			 * Run a sub-agent call on a fresh stack: no frame of this
			 * function or its callers may point into the old arena.
			 */
			if (ctx->agent_branch) {
				da.ctx = ctx;
				da.tc = &tc;
				da.conn = conn;
				da.el = el;
				zcall_on_fresh_stack(fyai_tool_child_dispatch_entry, &da);
			}
#endif
			fyai_tool_child_dispatch(ctx, &tc, conn, el);
		}
		if (fyai_event_loop_step(el, -1) < 0)
			_exit(1);
	}

	fyai_tool_child_finish(ctx, conn, el, false);
}

/* Handle termination in the child event loop. Ignore SIGPIPE. */
static enum fyai_event_action fyai_tool_child_signal(const struct fyai_event *ev)
{
	struct fyai_ctx *ctx = ev->userdata;

	if (ev->signo == SIGPIPE)
		return FYAIEA_CONTINUE;
	ctx->interrupt_pending = true;
	ctx->terminate_pending = true;
	return FYAIEA_CONTINUE;
}

static const int fyai_tool_child_signal_set[] = { SIGTERM, SIGHUP, SIGPIPE };

/* Block the signals fyai_tool_child_signals() arms, before any other
 * setup. A blocked signal queues; it does not take its default action. */
void fyai_tool_child_signals_block(void)
{
	sigset_t mask;
	size_t i;

	sigemptyset(&mask);
	for (i = 0; i < ARRAY_SIZE(fyai_tool_child_signal_set); i++)
		sigaddset(&mask, fyai_tool_child_signal_set[i]);
	(void)sigprocmask(SIG_BLOCK, &mask, NULL);
}

void fyai_tool_child_signals(struct fyai_ctx *ctx)
{
	struct fyai_event_loop *el;
	struct fyai_event_source *src;
	size_t i;
	int rc;

	el = fyai_ctx_loop(ctx);
	if (!el)
		return;
	for (i = 0; i < ARRAY_SIZE(fyai_tool_child_signal_set); i++) {
		src = NULL;
		rc = fyai_event_add_signal(el, fyai_tool_child_signal_set[i],
					   fyai_tool_child_signal, ctx, &src);
		(void)rc;
	}
}

int fyai_tool_child_exec_serve(struct fyai_ctx *ctx)
{
	/*
	 * A parent gives a child a terminal only from an interactive display,
	 * and the child opens a display of its own on it.
	 */
	ctx->cfg->interactive = ctx->cfg->agent_pty;
	fyai_diag_tracef("exec", "tool child of pid %ld%s", (long)getppid(),
			 ctx->cfg->agent_pty ? ", on a terminal" : "");
	fyai_tool_child_signals_block();
	fyai_tool_child_signals(ctx);
	if (!ctx->transient_gb && fyai_setup_transient_builder(ctx))
		return -1;
	fyai_tool_child_serve_loop(ctx);	/* never returns */
	return -1;
}

/*
 * True when a sub-agent child executes fyai again rather than continuing the
 * forked image. The executed child opens the arena itself, so its fork point
 * must be in the arena: a transient run keeps its state in memory, and a
 * pinned root is read-only. `agent/spawn: fork` selects the forked child.
 */
bool fyai_agent_spawn_exec(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg = ctx->cfg;
	/*
	 * With credential isolation there is one way to start a sub-agent: a
	 * forked child would keep the address space of its parent and could not
	 * be told from it. `agent/spawn: fork` then means exec.
	 */
	bool want_exec = ctx->transport.client ||
		(!fy_str_empty(cfg->agent_spawn) && !strcmp(cfg->agent_spawn, "exec"));

	return fyai_exec_self_available() && want_exec &&
	       !cfg->transient && !cfg->root_pinned && cfg->arena_dir;
}

/*
 * The state an executed sub-agent child cannot read from the arena: the
 * configuration of this run, the branch configuration, the conversation head
 * it forks from, and the identity of this execution. The catalogue of the
 * branch is sent as its arena value: the child shares the arena and checks
 * that the value is in it. A command-line key is
 * sent on the private channel; it is never stored.
 */
fy_generic fyai_agent_spawn_state(struct fyai_ctx *ctx,
				  struct fy_generic_builder *gb,
				  uint64_t transport_exec)
{
	struct fyai_cfg *cfg = ctx->cfg;
	fy_generic fork;

	fork = fy_null;
	if (fy_is_valid(ctx->last_message) && !ctx->session_unstored)
		fork = fy_gb_mapping(gb,
			"branch", fy_value(gb, fyai_ctx_branch(ctx)),
			"head", (long long)ctx->last_message.v);
	return fy_gb_mapping(gb,
		"config", fyai_generic_or_null(cfg->config_doc),
		"theme_variant", cfg->theme_variant ?
			fy_value(gb, cfg->theme_variant) : fy_null,
		"branch_config", fyai_generic_or_null(ctx->arena_config),
		"branch_catalog", fy_is_mapping(ctx->arena_catalog) ?
			fy_value(gb, (long long)ctx->arena_catalog.v) : fy_null,
		"branch_skills", fy_is_mapping(ctx->arena_skills) ?
			fy_value(gb, (long long)ctx->arena_skills.v) : fy_null,
		"fork", fork,
		"parent", ctx->agent_execution,
		"transport", transport_exec ?
			fyai_transport_spawn_state(ctx, gb, transport_exec) :
			fy_null,
		"api_key", cfg->api_key_explicit && cfg->api_key ?
			fy_value(gb, cfg->api_key) : fy_null);
}

/*
 * Replace the forked image with a new fyai that serves the control channel on
 * fds 3 and 4. Runs in the child between fork and exec: no allocation.
 * fyai_exec_self() owns the platform-specific executable path.
 */
void fyai_tool_child_exec(struct fyai_ctx *ctx, bool pty, bool tp)
{
	const char *argv[24];
	char arena_text[32];
	int argc, i;

	argc = 0;
	argv[argc++] = "fyai";
	for (i = 0; i < ctx->cfg->debug && i < 4; i++)
		argv[argc++] = "-d";
	argv[argc++] = "-b";
	argv[argc++] = fyai_ctx_branch(ctx);
	if (ctx->cfg->view_project) {
		argv[argc++] = "--view-project";
		argv[argc++] = ctx->cfg->view_project;
		argv[argc++] = "--view-scratch";
		argv[argc++] = ctx->cfg->view_scratch;
	}
	if (ctx->cfg->view_arena_fd >= 0) {
		snprintf(arena_text, sizeof(arena_text), "%d", ctx->cfg->view_arena_fd);
		argv[argc++] = "--view-arena-fd";
		argv[argc++] = arena_text;
		if (fyai_fsview_arena_pass(ctx->cfg))
			_exit(126);
	}
	argv[argc++] = "agent";
	argv[argc++] = "--tool-child";
	if (pty)
		argv[argc++] = "--pty";
	argv[argc++] = "--arena";
	argv[argc++] = ctx->cfg->arena_dir;
	argv[argc] = NULL;

	/* The control channel is the one inherited descriptor pair. */
	if (fcntl(FYAI_TOOL_CHILD_REQ_FD, F_SETFD, 0) < 0 ||
	    fcntl(FYAI_TOOL_CHILD_RSP_FD, F_SETFD, 0) < 0)
		_exit(126);
	/* and, under credential isolation, its two channels to the transport */
	if (tp && (fcntl(FYAI_TOOL_CHILD_TP_AGENT_FD, F_SETFD, 0) < 0 ||
		   fcntl(FYAI_TOOL_CHILD_TP_CTL_FD, F_SETFD, 0) < 0))
		_exit(126);
	if (ctx->signal_mask_valid)
		(void)sigprocmask(SIG_SETMASK, &ctx->signal_mask, NULL);
	fyai_exec_self(argv);
	_exit(127);
}
