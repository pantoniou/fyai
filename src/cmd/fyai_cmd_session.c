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
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fyai_fleet.h"
#include "fyai_pending.h"
#include "fyai_remote.h"
#include "fyai_turn.h"
#include "fyai_uimodel.h"
#include "fyai_host.h"
#include "fyai.h"
#include "fyai_cmd.h"
#include "fyai_cmd_int.h"
#include "fyai_agents.h"
#include "fyai_arena.h"
#include "fyai_buf.h"
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

/* ---- the fleet (doc/fyai-fleet-sdd.md, section 6) ---------------------- */

/* The directories that a scan does not go into when the configuration names none. */
static const char *const fleet_exclude_default[] = {
	"node_modules", "build*", ".cache",
};

struct fleet_walk {
	struct fyai_cmd_call *call;
	const char *root;
	/* {project, sessions, live} of each project: its stored rows and the
	 * rows of the record of its server. */
	fy_generic projects;
};

static int fleet_found(const char *dir, void *data)
{
	struct fleet_walk *w = data;
	struct fy_generic_builder *gb = w->call->gb;
	const char *rel = strcmp(dir, w->root) ? dir + strlen(w->root) + 1 : ".";
	fy_generic rows, live;

	/* A project that cannot be read is listed with no stored session. */
	if (fyai_peek_sessions(fy_sprintfa("%s/.fyai/arena", dir), gb, &rows))
		rows = fy_seq_empty;
	live = fyai_remote_project_sessions(dir, gb);
	w->projects = fy_append(gb, w->projects, fy_mapping(gb,
		"project", fy_value(gb, rel), "sessions", rows, "live", live));
	return fy_is_valid(w->projects) ? 0 : -1;
}

/*
 * Scan the root of the fleet of the current directory into @w, as `fleet/scan`
 * of the stored configuration of the root says. The root is read with a peek:
 * a process with an arena open reads no other, so the commands run with none.
 */
static int fleet_collect(struct fyai_cmd_call *call, struct fleet_walk *w)
{
	struct fyai_ctx *ctx = fyai_cmd_call_ctx(call);
	fy_generic config = fy_invalid, scan, exclude;
	char *cwd, *root = NULL;
	size_t i;
	int rc = -1;

	w->call = call;
	w->projects = fy_seq_empty;
	cwd = getcwd(NULL, 0);
	fyai_error_check(ctx, cwd, out, "fleet: cannot read the current directory");
	root = fyai_fleet_root_of(cwd);
	fyai_error_check(ctx, root, out, "fleet: no project holds '%s'", cwd);
	/* A root whose configuration cannot be read scans as the defaults say. */
	if (fyai_peek_arena_config(fy_sprintfa("%s/.fyai/arena", root), NULL, NULL,
				   call->gb, &config, NULL, NULL, NULL))
		config = fy_invalid;
	scan = fy_get(fy_get(config, "fleet", fy_invalid), "scan", fy_invalid);
	exclude = fy_get(scan, "exclude", fy_invalid);
	if (!fy_is_sequence(exclude)) {
		exclude = fy_seq_empty;
		for (i = 0; i < ARRAY_SIZE(fleet_exclude_default); i++)
			exclude = fy_append(call->gb, exclude,
					    fy_value(call->gb, fleet_exclude_default[i]));
	}
	w->root = root;
	rc = fyai_fleet_scan(root, (int)fy_get(scan, "depth",
					       (long long)DEFAULT_FLEET_SCAN_DEPTH),
			     exclude, fleet_found, w);
	fyai_error_check(ctx, rc >= 0, out, "fleet: cannot scan the root '%s'", root);
	fyai_error_check(ctx, !rc, out, "fleet: cannot keep the projects of the root");
out:
	free(root);
	free(cwd);
	w->root = NULL;
	return rc ? -1 : 0;
}

/* The row of @rows whose branch is @branch, or fy_invalid. */
static fy_generic fleet_live_row(fy_generic live, fy_generic branch)
{
	fy_generic row;

	fy_foreach(row, live)
		if (fy_equal(fy_get(row, "branch", fy_invalid), branch))
			return row;
	return fy_invalid;
}

/*
 * A process whose arena is open reads no other (an arena maps at a fixed
 * address): a session runs `fleet SUB --output json` in a child with no arena,
 * and takes its rows. The child is short; the read waits for it.
 */
static int fleet_child(struct fyai_cmd_call *call, const char *sub,
		       fy_generic *result)
{
	struct fyai_ctx *ctx = fyai_cmd_call_ctx(call);
	struct fyai_buf out = { 0 }, err = { 0 };
	int fds[2][2] = { { -1, -1 }, { -1, -1 } }, null = -1, status = 0, i, rc = -1;
	struct pollfd pfd[2];
	char self[PATH_MAX], chunk[4096];
	const char *argv[6];
	ssize_t n;
	pid_t pid = -1;

	n = fyai_host_executable(self, sizeof(self));
	fyai_error_check(ctx, n > 0 && (size_t)n < sizeof(self), out,
			 "fleet: cannot find this program");
	self[n] = '\0';
	argv[0] = self;
	argv[1] = "fleet";
	argv[2] = sub;
	argv[3] = "--output";
	argv[4] = "json";
	argv[5] = NULL;
	null = open("/dev/null", O_RDONLY | O_CLOEXEC);
	rc = null < 0 || pipe2(fds[0], O_CLOEXEC) || pipe2(fds[1], O_CLOEXEC);
	fyai_error_check(ctx, !rc, out, "fleet: cannot make the pipes of the scan: %s",
			 strerror(errno));
	rc = -1;
	pid = fork();
	fyai_error_check(ctx, pid >= 0, out, "fleet: cannot fork the scan: %s",
			 strerror(errno));
	if (!pid) {
		if (dup2(null, STDIN_FILENO) < 0 || dup2(fds[0][1], STDOUT_FILENO) < 0 ||
		    dup2(fds[1][1], STDERR_FILENO) < 0)
			_exit(126);
		fyai_close_fds_from(STDERR_FILENO + 1);
		fyai_loop_restore_signal_mask(ctx->loop);
		signal(SIGPIPE, SIG_DFL);
		execv(self, (char *const *)argv);
		_exit(127);
	}
	for (i = 0; i < 2; i++) {
		close(fds[i][1]);
		fds[i][1] = -1;
		pfd[i].fd = fds[i][0];
		pfd[i].events = POLLIN;
	}
	while (pfd[0].fd >= 0 || pfd[1].fd >= 0) {
		if (poll(pfd, 2, -1) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		for (i = 0; i < 2; i++) {
			if (pfd[i].fd < 0 || !pfd[i].revents)
				continue;
			n = read(pfd[i].fd, chunk, sizeof(chunk));
			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0 || fyai_buf_append_data(i ? &err : &out, chunk,
							   (size_t)n)) {
				pfd[i].fd = -1;
				continue;
			}
		}
	}
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	pid = -1;
	if (fyai_buf_append_data(&out, "", 1) || fyai_buf_append_data(&err, "", 1))
		goto out;
	fyai_error_check(ctx, WIFEXITED(status) && !WEXITSTATUS(status), out,
			 "%s", err.len > 1 ? (char *)err.data : "fleet: the scan failed");
	*result = parse_json_string(call->gb, out.data);
	fyai_error_check(ctx, fy_is_sequence(*result), out,
			 "fleet: the scan gave no rows");
	rc = 0;
out:
	if (pid > 0)
		while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
			;
	for (i = 0; i < 2; i++) {
		if (fds[i][0] >= 0)
			close(fds[i][0]);
		if (fds[i][1] >= 0)
			close(fds[i][1]);
	}
	if (null >= 0)
		close(null);
	fyai_buf_free(&out);
	fyai_buf_free(&err);
	return rc;
}

/* The arena of this process is open: the peeks run in a child. */
static bool fleet_arena_open(struct fyai_cmd_call *call)
{
	return fyai_cmd_call_ctx(call)->arena->durable_allocator != NULL;
}

int fyai_cmd_fleet_list(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fy_generic_builder *gb = call->gb;
	struct fleet_walk w;
	fy_generic project, name, row, live, owner, task, out = fy_seq_empty;
	fy_generic row_fleet, parent;
	const char *state;

	if (fleet_arena_open(call))
		return fleet_child(call, "list", result);
	if (fleet_collect(call, &w))
		return -1;
	fy_foreach(project, w.projects) {
		name = fy_get(project, "project", fy_invalid);
		/* A session that a server lists: its state is the server's. A
		 * session that no server lists is stored, runs with no server, or
		 * was lost with work that a start restores. */
		fy_foreach(row, fy_get(project, "sessions", fy_seq_empty)) {
			live = fleet_live_row(fy_get(project, "live", fy_seq_empty),
					      fy_get(row, "branch", fy_invalid));
			owner = fy_get(row, "owner", fy_invalid);
			row_fleet = fy_get(row, "fleet", fy_invalid);
			task = fy_get(row_fleet, "task", fy_invalid);
			if (fy_is_valid(live)) {
				row = fy_mapping(gb, "project", name,
					"branch", fy_get(row, "branch", ""),
					"state", fyai_remote_live_state(live),
					"attached", fy_get(live, "attached", false),
					"pid", fy_get(live, "pid", 0LL));
			} else {
				state = !fy_is_valid(owner) ? "stored" :
					fyai_pending_owner_alive(owner) ? "running" : "lost";
				row = fy_mapping(gb, "project", name,
						 "branch", fy_get(row, "branch", ""),
						 "state", state, "attached", false);
				if (!strcmp(state, "running"))
					row = fy_assoc(gb, row, "pid",
						       fy_get(owner, "pid", 0LL));
			}
			if (fy_is_int(task))
				row = fy_assoc(gb, row, "task", task);
			parent = fy_get(fy_get(row_fleet, "parent", fy_invalid), "branch",
					fy_invalid);
			if (fy_is_string(parent))
				row = fy_assoc(gb, row, "parent", fy_stringf(gb, "%s:%s",
					fy_get(fy_get(row_fleet, "parent", fy_invalid),
					       "project", "."), fy_castp(&parent, "")));
			out = fy_append(gb, out, row);
		}
		/* A live session with no stored exchange yet. */
		fy_foreach(live, fy_get(project, "live", fy_seq_empty)) {
			if (fy_is_valid(fleet_live_row(fy_get(project, "sessions",
							      fy_seq_empty),
						       fy_get(live, "branch", fy_invalid))))
				continue;
			out = fy_append(gb, out, fy_mapping(gb, "project", name,
				"branch", fy_get(live, "branch", ""),
				"state", fyai_remote_live_state(live),
				"attached", fy_get(live, "attached", false),
				"pid", fy_get(live, "pid", 0LL)));
		}
	}
	*result = out;
	return fy_is_valid(out) ? 0 : -1;
}

int fyai_cmd_fleet_scan(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fy_generic_builder *gb = call->gb;
	struct fleet_walk w;
	fy_generic project, out = fy_seq_empty;

	if (fleet_arena_open(call))
		return fleet_child(call, "scan", result);
	if (fleet_collect(call, &w))
		return -1;
	fy_foreach(project, w.projects)
		out = fy_append(gb, out, fy_mapping(gb,
			"project", fy_get(project, "project", fy_invalid),
			"sessions", (long long)fy_len(fy_get(project, "sessions",
							   fy_seq_empty)),
			"live", (long long)fy_len(fy_get(project, "live",
						       fy_seq_empty))));
	*result = out;
	return fy_is_valid(out) ? 0 : -1;
}

/* The tasks of this branch, a mapping by id: staged, else stored, else none. */
static fy_generic fleet_tasks_of(struct fyai_ctx *ctx)
{
	struct fyai_branch b;

	if (fy_is_mapping(ctx->bstate->tasks))
		return ctx->bstate->tasks;
	if (fy_is_valid(ctx->bstate->prev) && fyai_branch_decode(ctx->bstate->prev, &b) &&
	    fy_is_mapping(b.tasks))
		return b.tasks;
	return fy_map_empty;
}

/* The id of the next task: one above the highest id of @tasks. */
static long long fleet_task_next(fy_generic tasks)
{
	fy_generic key, value;
	long long id, top = 0;

	fy_foreach_key_value(key, value, tasks) {
		(void)value;
		id = strtoll(fy_castp(&key, "0"), NULL, 10);
		if (id > top)
			top = id;
	}
	return top + 1;
}

/*
 * Record @tasks as the tasks of this branch, now when it is stored. A session
 * with no stored exchange keeps them staged for its first publish.
 */
static int fleet_tasks_publish(struct fyai_ctx *ctx, fy_generic tasks)
{
	ctx->bstate->tasks = tasks;
	if (ctx->bstate->session_unstored)
		return 0;
	fyai_branch_op_set(ctx, FYAI_BRANCH_OP_CONFIG, NULL);
	return fyai_publish_state(ctx);
}

/* The tasks of the root sessions: each branch of the root and its tasks. */
int fyai_cmd_fleet_tasks(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = fyai_cmd_call_ctx(call);
	struct fy_generic_builder *gb = call->gb;
	fy_generic rows, row, key, task, out = fy_seq_empty;
	char *cwd, *root = NULL;
	int rc = -1;

	if (fleet_arena_open(call))
		return fleet_child(call, "tasks", result);
	cwd = getcwd(NULL, 0);
	fyai_error_check(ctx, cwd, out, "fleet: cannot read the current directory");
	root = fyai_fleet_root_of(cwd);
	fyai_error_check(ctx, root, out, "fleet: no project holds '%s'", cwd);
	rc = fyai_peek_sessions(fy_sprintfa("%s/.fyai/arena", root), gb, &rows);
	fyai_error_check(ctx, !rc, out, "fleet: cannot read the root '%s'", root);
	fy_foreach(row, rows)
		fy_foreach_key_value(key, task, fy_get(row, "tasks", fy_map_empty))
			out = fy_append(gb, out, fy_mapping(gb,
				"id", strtoll(fy_castp(&key, "0"), NULL, 10),
				"session", fy_get(row, "branch", ""),
				"name", fy_stringf(gb, "%s:%s",
						   fy_get(task, "project", "."),
						   fy_get(task, "branch", "")),
				"state", fy_get(task, "state", "open"),
				"text", fy_get(task, "text", "")));
	*result = out;
	rc = fy_is_valid(out) ? 0 : -1;
out:
	free(root);
	free(cwd);
	return rc;
}

/* Mark a task of this root session open, done or dropped. */
int fyai_cmd_fleet_task(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = fyai_cmd_call_ctx(call);
	struct fy_generic_builder *gb = ctx->arena->gb;
	const char *id = fyai_cmd_arg_str(call, "id");
	const char *state = fyai_cmd_arg_str(call, "state");
	fy_generic tasks, task;

	fyai_error_check(ctx, fleet_arena_open(call), err,
			 "fleet: a task changes in the root session that keeps it");
	tasks = fleet_tasks_of(ctx);
	task = fy_get(tasks, id, fy_invalid);
	fyai_error_check(ctx, fy_is_mapping(task), err,
			 "fleet: this session has no task %s", id);
	tasks = fy_assoc(gb, tasks, fy_value(gb, id),
			 fy_assoc(gb, task, "state", fy_value(gb, state)));
	fyai_error_check(ctx, fy_is_mapping(tasks), err, "fleet: cannot keep the task");
	if (fleet_tasks_publish(ctx, tasks))
		return -1;
	*result = fy_mapping(call->gb, "id", id, "state", state);
	return 0;
err:
	return -1;
}

/*
 * The project directory and the branch of the session that the argument
 * "branch" names: `PROJECT:BRANCH` of the fleet, or `BRANCH` of this project.
 * *@dirp is NULL for this project; the caller frees it.
 */
static int session_name(struct fyai_cmd_call *call, char **dirp,
			const char **branchp)
{
	struct fyai_ctx *ctx = fyai_cmd_call_ctx(call);
	const char *name = fyai_cmd_arg_str(call, "branch");
	char *cwd = getcwd(NULL, 0);
	int rc;

	fyai_error_check(ctx, cwd, err, "cannot read the current directory");
	rc = fyai_fleet_name_split(cwd, name, dirp, branchp);
	free(cwd);
	fyai_error_check(ctx, !rc, err, "'%s' names no branch", name);
	return 0;
err:
	return -1;
}

/* End one live session at the server. */
int fyai_cmd_session_kill(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = fyai_cmd_call_ctx(call);
	const char *branch;
	char *dir = NULL;
	bool busy = false;
	int rc;

	if (session_name(call, &dir, &branch))
		return -1;
	rc = fyai_remote_session_close(ctx, dir, branch,
				       fyai_cmd_arg_bool(call, "force"), &busy);
	free(dir);
	if (rc)
		return -1;
	*result = fy_mapping(call->gb, "branch", fyai_cmd_arg_str(call, "branch"),
			     "busy", busy);
	return 0;
}

/* A request of a supervisor to the session that "branch" names. */
static int session_supervise(struct fyai_cmd_call *call, const char *subtype,
			     fy_generic fields, fy_generic *result)
{
	struct fyai_ctx *ctx = fyai_cmd_call_ctx(call);
	const char *branch;
	char *dir = NULL;
	int rc;

	if (session_name(call, &dir, &branch))
		return -1;
	rc = fyai_remote_supervise(ctx, dir, branch, subtype, fields);
	free(dir);
	if (rc)
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

/*
 * A branch for a task with no branch named: `fleet/` and the first words of
 * the task, in the builder of @call.
 */
static const char *fleet_task_branch(struct fyai_cmd_call *call, const char *task)
{
	char slug[33];
	size_t n = 0;
	bool dash = false;
	const char *p;

	for (p = task; *p && n < sizeof(slug) - 1; p++) {
		if ((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')) {
			if (dash && n)
				slug[n++] = '-';
			if (n < sizeof(slug) - 1)
				slug[n++] = *p;
			dash = false;
		} else if (*p >= 'A' && *p <= 'Z') {
			if (dash && n)
				slug[n++] = '-';
			if (n < sizeof(slug) - 1)
				slug[n++] = (char)(*p - 'A' + 'a');
			dash = false;
		} else {
			dash = true;
		}
	}
	slug[n] = '\0';
	return fy_gb_intern_string(call->gb, fy_sprintfa("fleet/%s", n ? slug : "task"));
}

/* Start a session in a project of the fleet with a task. */
int fyai_cmd_fleet_start(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = fyai_cmd_call_ctx(call);
	const char *project = fyai_cmd_arg_str(call, "project");
	const char *task = fyai_cmd_arg_str(call, "task");
	const char *branch = fyai_cmd_arg_str(call, "branch");
	fy_generic args = fy_invalid, tasks = fy_invalid;
	char *cwd = NULL, *root = NULL, *dir = NULL, *home = NULL;
	long long id = 0;
	int rc = -1;

	cwd = getcwd(NULL, 0);
	fyai_error_check(ctx, cwd, out, "cannot read the current directory");
	root = fyai_fleet_root_of(cwd);
	fyai_error_check(ctx, root, out, "fleet: no project holds '%s'", cwd);
	dir = realpath(strcmp(project, ".") ? fy_sprintfa("%s/%s", root, project) :
		       root, NULL);
	fyai_error_check(ctx, dir && !strncmp(dir, root, strlen(root)) &&
			 (!dir[strlen(root)] || dir[strlen(root)] == '/'), out,
			 "fleet: '%s' is no directory under the root '%s'", project,
			 root);
	if (!branch)
		branch = fleet_task_branch(call, task);
	fyai_error_check(ctx, branch && fyai_branch_name_valid(branch), out,
			 "fleet: '%s' is not a valid branch", branch ? branch : "");
	/*
	 * A session that starts one is its parent; a session of the root
	 * keeps a task for it (doc/fyai-fleet-sdd.md, section 9). A command
	 * line has no session: the new one has no parent and no task.
	 */
	if (fleet_arena_open(call) && !ctx->cfg->transient) {
		home = fyai_fleet_project_of(cwd);
		args = fy_sequence(call->gb, "--fleet-parent",
			fy_stringf(call->gb, "%s:%s", home && strcmp(home, root) ?
				   home + strlen(root) + 1 : ".", fyai_ctx_branch(ctx)));
		if (home && !strcmp(home, root)) {
			tasks = fleet_tasks_of(ctx);
			id = fleet_task_next(tasks);
			args = fy_append(call->gb, fy_append(call->gb, args,
				fy_string("--fleet-task")),
				fy_stringf(call->gb, "%lld", id));
			tasks = fy_assoc(ctx->arena->gb, tasks,
				fy_stringf(ctx->arena->gb, "%lld", id),
				fy_mapping(ctx->arena->gb, "project", project,
					   "branch", branch, "text", task,
					   "created", (long long)fyai_branch_timestamp(),
					   "state", "open"));
			fyai_error_check(ctx, fy_is_mapping(tasks), out,
					 "fleet: cannot keep the task");
		}
	}
	rc = fyai_remote_start(ctx, dir, branch, task, args);
	if (!rc && id && fleet_tasks_publish(ctx, tasks))
		rc = -1;
	if (!rc) {
		*result = fy_mapping(call->gb, "project", project, "branch", branch,
				     "name", fy_stringf(call->gb, "%s:%s", project,
							branch));
		if (id)
			*result = fy_assoc(call->gb, *result, "task", id);
	}
out:
	free(home);
	free(dir);
	free(root);
	free(cwd);
	return rc ? -1 : 0;
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
