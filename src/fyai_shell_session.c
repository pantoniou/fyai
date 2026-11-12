/*
 * fyai_shell_session.c - shell sessions and their tiles
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

/*
 * Write the live work of the session to the work store of the UI model: each
 * program that runs in a terminal session, the user's or the model's, and
 * each sub-agent that runs.
 */
void fyai_tools_work_update(struct fyai_ctx *ctx)
{
	struct fy_generic_builder *gb = fyai_uimodel_begin(ctx);
	struct fyai_shell_session *sess;
	struct fyai_tool_job *job;
	fy_generic items = fy_seq_empty, item;
	int rows, cols;

	if (gb) {
		/* A session that ended stays while it is kept: a client
		 * ends its screen as the outcome says. */
		for (sess = ctx->shell_sessions; sess; sess = sess->next) {
			item = fy_mapping(gb, "kind", sess->user_owned ?
						"user-shell" : "shell",
					  "name", sess->name ? sess->name : "",
					  "title", sess->title ? sess->title : "",
					  "command", sess->command ? sess->command : "",
					  "keep", sess->keep_tile,
					  "pane", sess->takes_pane);
			if (sess->exited)
				item = fy_assoc(gb, item, "exited", true, "ok",
					(_Bool)!(sess->signal || sess->exit_code),
					"exit_code", (long long)sess->exit_code,
					"signal", (long long)sess->signal,
					"closing", sess->closing,
					"timed_out", sess->timed_out);
			if (sess->model_text && sess->view) {
				fyai_terminal_view_size(sess->view, &rows,
							&cols);
				item = fy_assoc(gb, item, "terminal",
					fy_mapping(gb, "source",
						   sess->model_text,
						   "rows", (long long)rows,
						   "cols", (long long)cols));
			}
			items = fy_append(gb, items, item);
		}
		for (job = ctx->tool_jobs; job; job = job->next) {
			if (!job->agent || job->phase == FYAI_JOB_DONE)
				continue;
			item = fy_mapping(gb, "kind", "agent",
					  "name", job->branch ? job->branch : "");
			if (job->model_text && job->view) {
				fyai_terminal_view_size(job->view, &rows,
							&cols);
				item = fy_assoc(gb, item, "terminal",
					fy_mapping(gb, "source", job->model_text,
						   "rows", (long long)rows,
						   "cols", (long long)cols));
			}
			items = fy_append(gb, items, item);
		}
		(void)fyai_uimodel_set_member(ctx, FYAI_UI_WORK, "items",
					      items);
	}
	fyai_uimodel_end(ctx);
}

/* The bytes of a terminal text before it starts again with a snapshot. */
#define TOOLS_TERMINAL_TEXT_MAX	(256u * 1024u)

/*
 * Give the bytes that a program wrote to @view, the view of its terminal, to
 * the text *@textp of the UI model, before the view takes them. A text starts
 * with a snapshot of the screen, which a consumer replays as the bytes that
 * drew it: on the first bytes, and when the text grows past its bound, so a
 * consumer that attaches replays little and the text stays bounded. @name
 * names the terminal in a diagnostic.
 */
void fyai_terminal_tap(struct fyai_ctx *ctx,
		       struct fyai_terminal_view *view,
		       long long *textp, const char *name,
		       const char *data, size_t len)
{
	struct fyai_buf snap = {0}, rows = {0};
	size_t have = 0, i;
	int row, col;
	bool visible;
	int rc;

	if (!fyai_uimodel_consumed(ctx) || !view)
		return;
	if (*textp)
		(void)fyai_uimodel_text(ctx, *textp, &have);
	if (*textp && have + len > TOOLS_TERMINAL_TEXT_MAX) {
		fyai_uimodel_text_close(ctx, *textp);
		*textp = 0;
	}
	if (!*textp) {
		*textp = fyai_uimodel_text_open(ctx);
		if (!*textp)
			return;
		fyai_terminal_view_cursor(view, &row, &col, &visible);
		rc = fyai_terminal_view_rows_sgr(view, NULL, false, &rows);
		/* Clear, the rows, each newline with a return, the cursor. */
		rc = rc || fyai_buf_append(&snap, "\033[H\033[2J");
		for (i = 0; !rc && i < rows.len; i++)
			rc = fyai_buf_append_data(&snap,
				rows.data[i] == '\n' ? "\r\n" : rows.data + i,
				rows.data[i] == '\n' ? 2 : 1);
		rc = rc || fyai_buf_append(&snap, fy_sprintfa(
			"\033[%d;%dH%s", row + 1, col + 1,
			visible ? "" : "\033[?25l"));
		rc = rc || fyai_uimodel_text_append(ctx, *textp, snap.data,
						    snap.len);
		free(rows.data);
		free(snap.data);
		if (rc)
			fyai_warning(ctx, "cannot give the screen of '%s' to "
				     "the display", name);
		fyai_tools_work_update(ctx);
	}
	(void)fyai_uimodel_text_append(ctx, *textp, data, len);
}

/* A session name is short and says what the shell is for. */
#define FYAI_SHELL_SESSION_NAME_MAX	32
/* Time a program gets to answer input before the reading is taken. */
#define FYAI_SHELL_INPUT_WAIT_MS	250
#define FYAI_SHELL_INPUT_WAIT_MAX_MS	30000
/*
 * Time a session must be quiet before it is typed at. Input written to a
 * program that is still starting is lost: the line discipline holds the bytes
 * while the terminal is still cooked, and a program that starts by asking the
 * terminal about itself reads them as the answer it waits for. A program
 * stopped for input writes nothing, thus it is quiet and is answered at once.
 */
#define FYAI_SHELL_IDLE_TRIGGER_MS	100
/*
 * The gate opens at this time whatever the program does. A program that never
 * stops writing is one the quiet time cannot describe, and its input is owed
 * to it all the same.
 */
#define FYAI_SHELL_IDLE_MAX_WAIT_MS	5000
#define FYAI_SHELL_IDLE_WAIT_MAX_MS	30000
/*
 * The interval at which a session that drew nothing is examined again. Such a
 * session is quiet because it did not start, and not because it settled, thus
 * the quiet time does not apply to it yet. The wait stops at the first byte
 * that the session draws, when the program reads its terminal, or at the time
 * limit of the call. A constant time limit is correct for one machine only.
 */
#define FYAI_SHELL_FIRST_DRAW_POLL_MS	50
/* Time a program gets to leave after it is asked to. */
#define FYAI_TTY_CLOSE_WAIT_MS		500

static void fyai_shell_session_close(struct fyai_shell_session *sess,
				     bool force);

/* Draw the head of a live terminal session at its current frame. */
static char *fyai_shell_session_cause(const struct fyai_shell_session *sess,
				      bool *okp);

static void fyai_shell_session_head_paint(void *owner)
{
	struct fyai_shell_session *sess = owner;
	char *cause;
	bool ok;

	if (!sess->surface)
		return;
	/* A screen kept after its program draws the head of its outcome. */
	if (sess->exited) {
		cause = fyai_shell_session_cause(sess, &ok);
		(void)fyai_chrome_update(sess->ctx, sess->surface,
			&(struct fyai_chrome_spec){
				.title = sess->title ? sess->title : "**shell**",
				.command = sess->command,
				.cause = cause,
				.mark = ok ? FYAI_UI_MARK_OK : FYAI_UI_MARK_FAILED,
			}, NULL);
		free(cause);
		fyai_ui_wake(sess->ctx);
		return;
	}
	(void)fyai_chrome_update(sess->ctx, sess->surface,
		&(struct fyai_chrome_spec){
			.title = sess->title ? sess->title : "**shell**",
			.command = sess->command,
			.mark = FYAI_UI_MARK_RUNNING,
			.frame = sess->animation_frame,
		}, NULL);
	fyai_ui_wake(sess->ctx);
}

/*
 * The rows that the flow puts before a block of the transcript now. A block
 * takes them when it is painted, so a block that is painted before the text
 * above it arrives is painted again when they change.
 */
int fyai_inline_sep(struct fyai_ctx *ctx)
{
	struct fyai_flow *flow = fyai_sink_flow(ctx->sink);
	struct fyai_flow_sep sep;

	if (!flow)
		return 0;
	sep = fyai_flow_before(flow, FYAI_FLOW_TOOL_HEAD);
	return (int)sep.rows + (sep.markdown ? 1 : 0);
}

/*
 * Arm the timer that advances the state mark of a block of the transcript, at
 * the interval of the indicator of the theme. A mark that does not move asks
 * for none.
 */
void fyai_inline_animation_arm(struct fyai_ctx *ctx,
		enum fyai_event_action (*fn)(const struct fyai_event *ev),
		void *owner, struct fyai_event_source **srcp)
{
	struct fyai_event_loop *el = fyai_ctx_loop(ctx);
	unsigned int ms = 0;
	char *mark;

	mark = fyai_ui_indicator(ctx, FYAI_UI_MARK_RUNNING, 0, &ms);
	free(mark);
	if (!el || !ms)
		return;
	if (fyai_event_add_timer(el, ms, ms, fn, owner, srcp))
		fyai_warning(ctx, "the indicator of a call in the transcript "
			     "could not start");
}

/* Advance the state mark on the title of a live terminal session. */
static enum fyai_event_action
fyai_shell_session_animate(const struct fyai_event *ev)
{
	struct fyai_shell_session *sess = ev->userdata;

	if (sess->inline_block && !sess->exited) {
		sess->animation_frame++;
		fyai_shell_session_inline_paint(sess, false);
		return FYAIEA_CONTINUE;
	}
	if (!sess->surface || sess->exited)
		return FYAIEA_CONTINUE;
	sess->animation_frame++;
	fyai_shell_session_head_paint(sess);
	return FYAIEA_CONTINUE;
}

/* The live display of a session shows its lines, as a normal tool call does. */
static void fyai_shell_session_line(void *userdata,
				    enum shell_output_stream stream,
				    const char *data, size_t len)
{
	struct fyai_shell_session *sess = userdata;

	(void)stream;
	if (!sess->job || !sess->job->stream.active)
		return;
	(void)fyai_fenced_stream_push(&sess->job->stream, data, len);
}

struct fyai_shell_session *fyai_shell_session_find(struct fyai_ctx *ctx,
						   const char *name)
{
	struct fyai_shell_session *sess;

	if (!ctx || !name || !*name)
		return NULL;
	for (sess = ctx->shell_sessions; sess; sess = sess->next)
		if (!strcmp(sess->name, name))
			return sess;
	return NULL;
}

/*
 * A session name says what the shell is for. Write it as a sub-agent name is
 * written: letters, digits, a dash or an underscore.
 */
bool fyai_shell_session_name_valid(const char *name)
{
	size_t i;

	if (!name || !*name || strlen(name) > FYAI_SHELL_SESSION_NAME_MAX)
		return false;
	for (i = 0; name[i]; i++) {
		if (isalnum((unsigned char)name[i]) || name[i] == '-' ||
		    name[i] == '_' || name[i] == '/')
			continue;
		return false;
	}
	return true;
}

static void fyai_shell_session_idle_arm(struct fyai_shell_session *sess);

/* Nothing was read from or written to the session: end it. */
static enum fyai_event_action
fyai_shell_session_idle(const struct fyai_event *ev)
{
	struct fyai_shell_session *sess = ev->userdata;

	sess->idle = NULL;
	if (sess->exited)
		return FYAIEA_CONTINUE;
	sess->timed_out = true;
	fyai_shell_session_close(sess, false);
	return FYAIEA_CONTINUE;
}

static void fyai_shell_session_idle_arm(struct fyai_shell_session *sess)
{
	struct fyai_event_loop *el;
	int ms;

	/* The worker of a mirror watches its program. */
	if (sess->remote)
		return;
	ms = sess->ctx->cfg->shell_session_timeout_ms;
	if (sess->idle) {
		if (ms > 0)
			(void)fyai_event_timer_rearm(sess->idle, ms, 0);
		return;
	}
	if (ms <= 0 || sess->exited)
		return;
	el = fyai_ctx_loop(sess->ctx);
	if (!el)
		return;
	(void)fyai_event_add_timer(el, ms, 0, fyai_shell_session_idle, sess,
				   &sess->idle);
}

/*
 * The tool child holds the terminal of a session, thus a reply uses the same
 * path as typed input. A program that sends a query waits for the reply.
 * Without it the program waits for its own time limit, and it then reads the
 * next typed input as the reply.
 */
static void fyai_shell_session_reply(const char *data, size_t len, void *user)
{
	struct fyai_shell_session *sess = user;
	struct fy_generic_builder *gb;

	if (sess->remote && !sess->exited) {
		sess->remote->input(sess->remote_data, sess->name, data, len);
		return;
	}
	if (!sess->job || !sess->job->conn || sess->exited)
		return;
	gb = fyai_ctx_transient_gb(sess->ctx);
	if (!gb)
		return;
	(void)jsonrpc_notify(sess->job->conn, "shell/write",
			     fyai_bytes_to_generic(gb, data, len));
}

static const struct fyai_workpane_tile_ops fyai_shell_session_tile_ops;

/* Sizes below which a live screen is not worth drawing; the head still
 * shows whose call this is and whether it is running. */
#define FYAI_TILE_FULL_ROWS	4
#define FYAI_TILE_FULL_COLS	30
#define FYAI_TILE_OUTPUT_ROWS	2

const struct fyai_workpane_ladder fyai_tile_ladder = {
	.full_rows = FYAI_TILE_FULL_ROWS,
	.full_cols = FYAI_TILE_FULL_COLS,
	.output_rows = FYAI_TILE_OUTPUT_ROWS,
};

/* True while the tile is large enough to show the screen of its program. */
bool fyai_tile_draws_program(enum fyai_workpane_present p)
{
	return p == FYAI_WORKPANE_PRESENT_FULL ||
	       p == FYAI_WORKPANE_PRESENT_OUTPUT;
}

/* Create the session and reserve its name, before the process is spawned. */
struct fyai_shell_session *
fyai_shell_session_create(struct fyai_ctx *ctx, const char *name,
			  const char *command, const char *title,
			  const char *description, int rows,
			  int cols, size_t max_bytes, bool pipes,
			  bool user_owned, const struct fyai_tools_remote *remote,
			  void *remote_data)
{
	struct fyai_event_loop *el;
	struct fyai_shell_session *sess;
	unsigned int animation_ms;
	int rc;

	animation_ms = 0;
	sess = calloc(1, sizeof(*sess));
	if (!sess)
		return NULL;
	sess->ctx = ctx;
	/* A session that has drawn nothing has been quiet since it opened. */
	sess->quiet_since = fyai_event_now_ms();
	sess->name = strdup(name);
	sess->command = strdup(command ? command : "");
	sess->branch = strdup(fyai_ctx_branch(ctx));
	sess->view = fyai_terminal_view_create(ctx, rows, cols, max_bytes);
	fyai_error_check(ctx,
			 sess->name && sess->command && sess->branch && sess->view,
			 fail, "shell: could not allocate the terminal session");
	rc = fyai_terminal_view_set_history(sess->view,
					    ctx->cfg->work_history_rows);
	fyai_error_check(ctx, !rc, fail,
			 "shell: cannot allocate terminal scrollback");
	/* Pipe output uses bare line feeds. */
	fyai_terminal_view_cooked(sess->view, pipes);
	fyai_terminal_view_line_cb(sess->view, fyai_shell_session_line, sess);
	fyai_terminal_view_reply_cb(sess->view, fyai_shell_session_reply, sess);
	sess->title = title ? strdup(title) : NULL;
	sess->description = description ? strdup(description) : NULL;
	sess->remote = remote;
	sess->remote_data = remote_data;
	sess->rows = rows;
	sess->pipes = pipes;
	sess->user_owned = user_owned;
	/* A call of the model draws its screen at the call in the
	 * transcript; a program of the user keeps its tile. */
	sess->inline_block = !user_owned && fyai_ui_tools_inline(ctx);
	sess->surface = sess->inline_block ? NULL :
			fyai_ui_surface_open(ctx, rows, cols);
	if (sess->inline_block) {
		fyai_inline_animation_arm(ctx, fyai_shell_session_animate, sess,
					  &sess->animation);
		fyai_terminal_view_damage_all(sess->view);
	}
	if (sess->surface) {
		/* A user program accepts what the pane grants; a model
		 * session keeps the height it asked for. */
		(void)fyai_workpane_register(ctx->workpane, sess->surface,
					     FYAI_WORKPANE_TILE_SHELL, sess,
					     &fyai_shell_session_tile_ops,
					     user_owned ? FYAI_WORKPANE_FILL :
							  rows,
					     user_owned ? 0 : rows);
		fyai_workpane_tile_set_ladder(ctx->workpane, sess->surface,
					      &fyai_tile_ladder);
		(void)fyai_chrome_update(ctx, sess->surface,
			&(struct fyai_chrome_spec){
				.title = sess->title ? sess->title : "**shell**",
				.command = sess->command,
				.mark = FYAI_UI_MARK_RUNNING,
			}, &animation_ms);
		fyai_chrome_body(ctx, sess->surface);
		el = fyai_ctx_loop(ctx);
		if (el && animation_ms) {
			rc = fyai_event_add_timer(el, animation_ms, animation_ms,
					  fyai_shell_session_animate, sess,
					  &sess->animation);
			if (rc)
				fyai_warning(ctx, "shell session indicator animation "
					     "timer could not start");
		}
		/* Publish the initial blank screen. */
		fyai_terminal_view_damage_all(sess->view);
		if (fyai_ui_surface_publish(sess->surface, sess->view) > 0)
			fyai_ui_wake(ctx);
	}
	sess->next = ctx->shell_sessions;
	ctx->shell_sessions = sess;
	fyai_tools_work_update(ctx);
	/* The program of the session outlives the exchange that started it.
	 * The program of a mirror is in the scope of its worker. */
	if (!remote) {
		sess->pinned = true;
		fyai_scope_pin(ctx);
	}
	fyai_shell_session_idle_arm(sess);
	return sess;

fail:
		fyai_terminal_view_destroy(sess->view);
		free(sess->name);
		free(sess->command);
		free(sess->branch);
		free(sess);
	return NULL;
}

/* Apply a layout grant: size the pseudo-terminal and the grid. */
static void fyai_shell_session_apply_grant(void *owner, int rows, int cols)
{
	struct fyai_shell_session *sess = owner;
	struct fy_generic_builder *gb;
	int have_rows = 0, have_cols = 0;

	if ((!sess->surface && !sess->inline_block) || cols < 1)
		return;
	fyai_terminal_view_size(sess->view, &have_rows, &have_cols);
	if (rows < 1)
		rows = sess->rows;
	if (cols == have_cols && rows == have_rows)
		return;

	/* The worker of a mirror resizes its program as a child does, and
	 * the size it took comes back with the bytes of the program. */
	if (sess->remote && !sess->exited) {
		if (rows == sess->resize_rows && cols == sess->resize_cols)
			return;
		sess->resize_rows = rows;
		sess->resize_cols = cols;
		sess->remote->resize(sess->remote_data, sess->name, rows, cols);
		return;
	}
	/* The child owns the PTY and applies session resize requests. */
	if (!sess->job || !sess->job->conn || sess->exited)
		goto local;
	if (rows == sess->resize_rows && cols == sess->resize_cols)
		return;
	gb = fyai_ctx_transient_gb(sess->ctx);
	if (!gb)
		return;
	if (!jsonrpc_notify(sess->job->conn, "tty/resize",
			     fy_gb_mapping(gb, "rows", (long long)rows,
					   "cols", (long long)cols))) {
		sess->resize_rows = rows;
		sess->resize_cols = cols;
	}
	return;
local:
	fyai_terminal_view_resize(sess->view, rows, cols);
	/* A consumer replays the bytes that follow at the new size. */
	if (sess->model_text)
		fyai_tools_work_update(sess->ctx);
	(void)fyai_ui_surface_resize(sess->surface, rows, cols);
	fyai_workpane_grid_resized(sess->ctx->workpane, sess->surface, rows,
				   cols);
}

/*
 * A program that prints once and ends, such as ls, reads the size of its
 * terminal when it starts. The tile of the session is registered already, so
 * the layout solved now gives it the size it will have, and the program starts
 * at that size. A size the call asks for wins.
 */
void fyai_shell_session_start_size(struct fyai_tool_job *job,
				   fy_generic args)
{
	struct fyai_shell_session *sess = job->session;
	int rows, cols;

	/* A block of the transcript has the size it opened with. */
	if (sess && sess->inline_block) {
		fyai_terminal_view_size(sess->view, &rows, &cols);
		job->start_rows = rows;
		job->start_cols = cols;
		return;
	}
	if (!sess || !sess->surface || fy_get(args, "rows", 0LL) > 0 ||
	    fy_get(args, "cols", 0LL) > 0 || !fyai_ui_layout_now(sess->ctx))
		return;
	rows = fyai_ui_surface_granted_rows(sess->ctx, sess->surface);
	cols = fyai_ui_surface_granted_cols(sess->ctx, sess->surface);
	if (rows < 1 || cols < 1)
		return;
	job->start_rows = rows;
	job->start_cols = cols;
	/* The grid and the view take the size before the program writes. */
	fyai_shell_session_apply_grant(sess, rows, cols);
}

/* Chrome only: focus changes nothing about the size of this session. */
static void fyai_shell_session_focus_changed(void *owner, bool focused)
{
	struct fyai_shell_session *sess = owner;

	(void)sess;
	(void)focused;
}

/* Draw the program, or blank the grid and leave the head standing. */
static void fyai_shell_session_present(void *owner,
				       enum fyai_workpane_present p)
{
	struct fyai_shell_session *sess = owner;

	if (!sess->surface)
		return;
	sess->present = p;
	if (p == FYAI_WORKPANE_PRESENT_HEAD ||
	    p == FYAI_WORKPANE_PRESENT_HIDDEN) {
		(void)fyai_ui_surface_clear(sess->surface);
	} else {
		/* Draw the whole screen again at the size it now has. */
		fyai_terminal_view_damage_all(sess->view);
		(void)fyai_ui_surface_publish(sess->surface, sess->view);
	}
	fyai_ui_wake(sess->ctx);
}

static const struct fyai_workpane_tile_ops fyai_shell_session_tile_ops = {
	.apply_grant = fyai_shell_session_apply_grant,
	.focus_changed = fyai_shell_session_focus_changed,
	.set_presentation = fyai_shell_session_present,
	.repaint_head = fyai_shell_session_head_paint,
};

void fyai_shell_session_resized(struct fyai_shell_session *sess,
				int rows, int cols)
{
	if (!sess || (!sess->surface && !sess->inline_block &&
		      !sess->model_text) || rows <= 0 || cols <= 0)
		return;
	fyai_terminal_view_resize(sess->view, rows, cols);
	/* A consumer replays the bytes that follow at the new size. */
	if (sess->model_text)
		fyai_tools_work_update(sess->ctx);
	if (sess->inline_block || !sess->surface) {
		fyai_terminal_view_damage_all(sess->view);
		if (rows == sess->resize_rows && cols == sess->resize_cols) {
			sess->resize_rows = 0;
			sess->resize_cols = 0;
		}
		fyai_ui_wake(sess->ctx);
		return;
	}
	(void)fyai_ui_surface_resize(sess->surface, rows, cols);
	/* An acknowledgement records the grid; it schedules no new request. */
	fyai_workpane_grid_resized(sess->ctx->workpane, sess->surface, rows,
				   cols);
	/* Publish the complete resized grid before the next frame. */
	(void)fyai_ui_surface_publish(sess->surface, sess->view);
	if (rows == sess->resize_rows && cols == sess->resize_cols) {
		sess->resize_rows = 0;
		sess->resize_cols = 0;
	}
	fyai_ui_wake(sess->ctx);
}

/* Show what the program has drawn since the last look. */
void fyai_shell_session_refresh(struct fyai_shell_session *sess)
{
	if (!sess || !sess->surface)
		return;
	if (fyai_ui_surface_publish(sess->surface, sess->view) > 0)
		fyai_ui_wake(sess->ctx);
}

char *fyai_shell_session_ended_text(struct fyai_ctx *ctx, const char *name,
				    bool *knownp)
{
	struct fyai_shell_session *sess = fyai_shell_session_find(ctx, name);

	*knownp = sess != NULL;
	if (!sess || !sess->exited)
		return NULL;
	return strdup(fy_sprintfa("[shell '%s' ended: status %d]", name,
				  sess->signal ? 128 + sess->signal :
						 sess->exit_code));
}

/* Whether the queued wait report of session @name still has a live owner. */
bool fyai_event_session_live(struct fyai_ctx *ctx, const char *name)
{
	struct fyai_shell_session *sess;

	sess = fyai_shell_session_find(ctx, name);
	return sess && !sess->exited && !sess->closing && sess->job;
}

/* Queue the session's input request and visible prompt for the model. */
static void fyai_shell_session_input_wanted(struct fyai_shell_session *sess)
{
	char *prompt;
	char *text;

	prompt = fyai_terminal_view_last_line(sess->view);
	text = prompt && *prompt ?
		strdup(fy_sprintfa("[shell '%s' is waiting for input: %s]",
				   sess->name, prompt)) :
		strdup(fy_sprintfa("[shell '%s' is waiting for input]",
				   sess->name));
	free(prompt);
	if (!text)
		return;
	/*
	 * Name the session: the report reaches the model after this poll,
	 * and the session may have ended by then. A stale report names a
	 * session with no live owner and is dropped instead of submitted.
	 */
	if (fyai_event_inject_owned(sess->ctx, text, FYAI_EVENT_OWNER_SESSION,
				    strdup(sess->name))) {
		fyai_warning(sess->ctx,
			     "the shell '%s' asked for input, which was lost",
			     sess->name);
	}
}

/* Poll for transitions into and out of an input wait. */
static enum fyai_event_action fyai_shell_session_wait_poll(
					const struct fyai_event *ev)
{
	struct fyai_shell_session *sess = ev->userdata;
	bool wants;

	if (sess->exited || sess->closing)
		return FYAIEA_CONTINUE;
	/*
	 * A program held at a gate is one that is being typed at: its answer
	 * is on its way. Asking the model to answer it would ask for what it
	 * has already sent.
	 */
	if (sess->feeding)
		return FYAIEA_CONTINUE;
	wants = fyai_process_reads_stdin(sess->pid);
	if (wants == sess->wants_input)
		return FYAIEA_CONTINUE;
	sess->wants_input = wants;
	/* Said one time for each wait; reading on is not a new question. */
	if (wants)
		fyai_shell_session_input_wanted(sess);
	return FYAIEA_CONTINUE;
}

/* Watch @pid, the program this session runs, for a stop on its input. */
void fyai_shell_session_watch(struct fyai_shell_session *sess, pid_t pid)
{
	struct fyai_event_loop *el;
	int ms;

	if (!sess || pid <= 0 || sess->waiter)
		return;
	sess->pid = pid;
	/*
	 * A user-owned session is read by the user on its screen. The model
	 * must not get its output, and a turn started for it would take the
	 * screen from the user.
	 */
	if (sess->user_owned)
		return;
	ms = sess->ctx->cfg->shell_input_poll_ms;
	el = fyai_ctx_loop(sess->ctx);
	if (ms <= 0 || !el)
		return;
	(void)fyai_event_add_timer(el, ms, ms, fyai_shell_session_wait_poll,
				   sess, &sess->waiter);
}

/* How the session ended, for the mark and the cause beside its title. */
static char *fyai_shell_session_cause(const struct fyai_shell_session *sess,
				      bool *okp)
{
	char buf[64];

	*okp = true;
	if (sess->timed_out) {
		*okp = false;
		return strdup("timed out");
	}
	/* An expected close signal is not a failure. */
	if (sess->closing)
		return NULL;
	if (sess->signal) {
		*okp = false;
		snprintf(buf, sizeof(buf), "killed by signal %d", sess->signal);
		return strdup(buf);
	}
	if (sess->exit_code) {
		*okp = false;
		snprintf(buf, sizeof(buf), "exit %d", sess->exit_code);
		return strdup(buf);
	}
	return NULL;
}

/* The columns of a terminal drawn in the transcript: the render width less
 * the indent of tool output. */
int fyai_shell_inline_cols(struct fyai_ctx *ctx)
{
	int width = ctx->cfg->render_width, rows = 0;
	int indent = (int)strlen(markdown_tool_output_indent(ctx->cfg));

	/* Without a render width the transcript is as wide as the terminal. */
	if (width < 1 && (fyai_ui_size(ctx, &width, &rows) || width < 1))
		width = FYAI_TTY_COLS_DEFAULT;

	return width - indent > 10 ? width - indent : 10;
}

/*
 * The length of the @len bytes of @screen without the rows at its end that
 * hold only the indent of tool output: what the transcript keeps ends at the
 * last row the program drew.
 */
size_t fyai_inline_screen_trim(struct fyai_ctx *ctx, const char *screen,
			       size_t len)
{
	const char *indent = markdown_tool_output_indent(ctx->cfg);
	size_t ilen = strlen(indent);

	while (len >= ilen + 1 && screen[len - 1] == '\n' &&
	       !memcmp(screen + len - 1 - ilen, indent, ilen) &&
	       (len - 1 - ilen == 0 || screen[len - 2 - ilen] == '\n'))
		len -= ilen + 1;
	return len;
}

/* Draw the screen of an inline session in its block of the transcript, or
 * with @done present it there with its outcome. */
void fyai_shell_session_inline_paint(struct fyai_shell_session *sess,
				     bool done)
{
	struct fyai_buf screen = {0};
	const char *indent = markdown_tool_output_indent(sess->ctx->cfg);
	char *cause = NULL;
	char edge[256];
	int first, last, rc;
	bool ok = true;

	/* The paint draws every row: the damage is taken. */
	(void)fyai_terminal_view_take_damage(sess->view, &first, &last);
	sess->inline_sep = fyai_inline_sep(sess->ctx);
	if (!done && fyai_ui_inline_focused(sess->ctx) == (uintptr_t)sess)
		indent = fyai_ui_inline_margin(sess->ctx, indent, edge,
					       sizeof(edge));
	rc = fyai_terminal_view_rows_sgr(sess->view, indent, !done, &screen);
	fyai_error_check(sess->ctx, !rc, out,
			 "shell: cannot draw the screen of session '%s'",
			 sess->name ? sess->name : "");
	if (done) {
		cause = fyai_shell_session_cause(sess, &ok);
		screen.len = fyai_inline_screen_trim(sess->ctx, screen.data,
						     screen.len);
	}
	if (fyai_ui_inline_terminal(sess->ctx, (uintptr_t)sess,
				    sess->title ? sess->title : "**shell**",
				    sess->command, screen.data, screen.len,
				    sess->rows, sess->animation_frame, done,
				    ok))
		fyai_warning(sess->ctx,
			     "shell: cannot draw session '%s' in the transcript",
			     sess->name ? sess->name : "");
out:
	free(cause);
	free(screen.data);
}

/* A terminal drawn in the transcript follows the width of the transcript. */
void fyai_shell_session_inline_follow(struct fyai_shell_session *sess)
{
	int rows = 0, cols = 0, want;

	if (sess->exited)
		return;
	fyai_terminal_view_size(sess->view, &rows, &cols);
	want = fyai_shell_inline_cols(sess->ctx);
	/* The block has the rows of the setting. The view of a mirror still
	 * has its old rows while a resize is on its way to the worker, and
	 * those rows would undo it. */
	/* The view of a mirror still has its old rows while a resize is on its
	 * way to the worker, and those rows would undo it. */
	if (sess->remote && sess->resize_rows > 0)
		rows = sess->resize_rows;
	if (want != cols)
		fyai_shell_session_apply_grant(sess, rows, want);
}

/* A program that ended may no longer be focused or zoomed. */
void fyai_surface_retire_zoom(struct fyai_ctx *ctx,
			      struct fytim_surface *surface)
{
	if (!ctx || !surface)
		return;
	fyai_workpane_tile_set_selectable(ctx->workpane, surface, false);
	if (fyai_workpane_focused(ctx->workpane) == surface)
		fyai_workpane_clear_focus(ctx->workpane);
	if (fyai_workpane_zoomed(ctx->workpane) == surface)
		fyai_workpane_clear_zoom(ctx->workpane);
}

/*
 * Commit the completed session to the transcript and terminal scrollback. A
 * bang command is not a part of the conversation: it records nothing and
 * commits nothing, and its tile stays with the outcome until the user
 * dismisses it, and keeps the keys if it held them. A full-screen program leaves
 * nothing to read, and its tile goes at once.
 */
static void fyai_shell_session_display_finish(struct fyai_shell_session *sess)
{
	char *cause;
	char *text;
	size_t len;
	bool ok;

	if (!sess || sess->recorded)
		return;
	sess->recorded = true;
	/* The worker of a mirror records the exchange. */
	if (!sess->keep_tile && !sess->remote) {
		cause = fyai_shell_session_cause(sess, &ok);
		text = fyai_terminal_view_read(sess->view, FYAITR_ALL, NULL,
					       &len);
		if (fyai_record_shell_screen(sess->ctx, sess->description,
					     sess->command, text, ok, cause))
			fyai_warning(sess->ctx,
				     "shell: could not record the session '%s'",
				     sess->name ? sess->name : "");
		free(text);
		free(cause);
	}
	if (sess->inline_block) {
		if (sess->animation) {
			fyai_event_source_remove(sess->animation);
			sess->animation = NULL;
		}
		fyai_shell_session_inline_paint(sess, true);
		sess->inline_block = false;
		fyai_ui_wake(sess->ctx);
		return;
	}

	if (!sess->surface)
		return;

	if (sess->animation) {
		fyai_event_source_remove(sess->animation);
		sess->animation = NULL;
	}
	fyai_shell_session_refresh(sess);
	cause = fyai_shell_session_cause(sess, &ok);
	(void)fyai_chrome_update(sess->ctx, sess->surface,
		&(struct fyai_chrome_spec){
			.title = sess->title ? sess->title : "**shell**",
			.command = sess->command,
			.cause = cause,
			.mark = ok ? FYAI_UI_MARK_OK : FYAI_UI_MARK_FAILED,
		}, NULL);
	free(cause);
	/* A full-screen program leaves nothing to read, and a tile the user
	 * closed is not wanted: its tile goes. */
	if (sess->keep_tile &&
	    (sess->close_on_exit ||
	     fyai_terminal_view_used_alt_screen(sess->view))) {
		fyai_surface_retire_zoom(sess->ctx, sess->surface);
		fyai_ui_surface_close(sess->ctx, sess->surface);
		sess->surface = NULL;
		fyai_ui_wake(sess->ctx);
		return;
	}
	/* The tile keeps the focus it had: Escape closes it there, which the
	 * hint of the status row says. */
	if (sess->keep_tile) {
		if (fyai_workpane_focused(sess->ctx->workpane) == sess->surface)
			fyai_chrome_focus(sess->ctx, sess->surface, true);
		fyai_ui_wake(sess->ctx);
		return;
	}
	fyai_surface_retire_zoom(sess->ctx, sess->surface);
	fyai_ui_surface_commit(sess->ctx, sess->surface);
	sess->surface = NULL;
	fyai_ui_wake(sess->ctx);
}

/* Tell the owner of the program that it ended, one time. */
static void fyai_shell_session_notify_exit(struct fyai_shell_session *sess,
					   int exit_code, int signal)
{
	fyai_tools_exit_fn fn;
	void *data;

	fn = sess->on_exit;
	data = sess->on_exit_data;
	sess->on_exit = NULL;
	sess->on_exit_data = NULL;
	if (fn)
		fn(data, exit_code, signal);
}

/* The program ended. The view stays; only the process is gone. */
void fyai_shell_session_exited(struct fyai_shell_session *sess,
			       int exit_code, int signal)
{
	if (!sess || sess->exited)
		return;
	sess->exited = true;
	sess->exit_code = exit_code;
	sess->signal = signal;
	fyai_tools_work_update(sess->ctx);
	/* The program has gone, so the session no longer needs the scope. */
	if (sess->pinned) {
		sess->pinned = false;
		fyai_scope_unpin(sess->ctx);
	}
	fyai_shell_session_display_finish(sess);
	if (sess->idle) {
		fyai_event_source_remove(sess->idle);
		sess->idle = NULL;
	}
	/* Remove the obsolete input waiter. */
	if (sess->waiter) {
		fyai_event_source_remove(sess->waiter);
		sess->waiter = NULL;
	}
	/*
	 * The exited program answers no question: drop the wait reports it
	 * queued, or a later turn asks the model about a program that ended.
	 */
	fyai_events_drop_session(sess->ctx, sess->name);
	fyai_shell_session_notify_exit(sess, exit_code, signal);
	fyai_waiters_kick(sess->ctx);
}

static void fyai_shell_session_close(struct fyai_shell_session *sess,
				     bool force)
{
	struct fy_generic_builder *gb;

	if (sess && !sess->exited && sess->remote) {
		sess->closing = true;
		sess->remote->close(sess->remote_data, sess->name, force);
		return;
	}
	if (!sess || sess->exited || !sess->job || !sess->job->conn)
		return;
	gb = fyai_ctx_transient_gb(sess->ctx);
	if (!gb)
		return;
	sess->closing = true;
	(void)jsonrpc_notify(sess->job->conn, "shell/close",
			     fy_mapping("force", force));
	if (force && sess->job->pid > 0)
		(void)kill(-sess->job->pid, SIGKILL);
}

static void fyai_shell_session_destroy(struct fyai_shell_session *sess)
{
	if (!sess)
		return;
	/* A session that goes before its program ends still reports an end. */
	if (!sess->exited)
		fyai_shell_session_notify_exit(sess, -1, SIGKILL);
	/* Commit output displayed before teardown. */
	fyai_shell_session_display_finish(sess);
	if (sess->inline_block)
		fyai_ui_inline_drop(sess->ctx, (uintptr_t)sess);
	/* A finished bang session keeps its tile until now. */
	if (sess->surface) {
		fyai_surface_retire_zoom(sess->ctx, sess->surface);
		fyai_ui_surface_close(sess->ctx, sess->surface);
		sess->surface = NULL;
	}
	if (sess->animation)
		fyai_event_source_remove(sess->animation);
	if (sess->idle)
		fyai_event_source_remove(sess->idle);
	if (sess->waiter)
		fyai_event_source_remove(sess->waiter);
	if (sess->model_text)
		fyai_uimodel_text_close(sess->ctx, sess->model_text);
	fyai_terminal_view_destroy(sess->view);
	if (sess->pinned)
		fyai_scope_unpin(sess->ctx);
	free(sess->name);
	free(sess->command);
	free(sess->branch);
	free(sess->title);
	free(sess->description);
	free(sess);
}

/* Drop inherited session records without touching parent-owned sessions. */
void fyai_shell_sessions_abandon(struct fyai_ctx *ctx)
{
	struct fyai_shell_session *sess, *next;

	if (!ctx)
		return;
	for (sess = ctx->shell_sessions; sess; sess = next) {
		next = sess->next;
		if (sess->job)
			sess->job->session = NULL;
		sess->job = NULL;
		sess->surface = NULL;
		sess->idle = NULL;
		sess->waiter = NULL;
		sess->animation = NULL;
		/* Exit callbacks belong to the parent invocation. */
		sess->on_exit = NULL;
		sess->on_exit_data = NULL;
		/* Only the parent records its session. */
		sess->recorded = true;
		fyai_shell_session_destroy(sess);
	}
	ctx->shell_sessions = NULL;
}

static void fyai_shell_session_release_one(struct fyai_shell_session *sess,
						   bool force)
{
	struct fyai_tool_job *job = sess->job;

	/*
	 * The session answers no question after this: drop the wait reports
	 * it queued with it.
	 */
	fyai_events_drop_session(sess->ctx, sess->name);
	if (job) {
		job->session = NULL;
		sess->job = NULL;
		if (!job->reaped && job->pid > 0)
			(void)kill(-job->pid, force ? SIGKILL : SIGTERM);
		fyai_tool_job_discard(job);
	}
	fyai_shell_session_destroy(sess);
}

/* A finished bang session leaves the pane: its tile goes, and the session
 * with it. */
static void fyai_shell_session_dismiss(struct fyai_shell_session *sess)
{
	struct fyai_ctx *ctx = sess->ctx;
	struct fyai_shell_session **link;

	/* The worker of a mirror drops its session too. */
	if (sess->remote)
		sess->remote->dismiss(sess->remote_data, sess->name);
	for (link = &ctx->shell_sessions; *link && *link != sess;
	     link = &(*link)->next)
		;
	if (*link)
		*link = sess->next;
	fyai_shell_session_release_one(sess, false);
	fyai_tools_work_update(ctx);
	fyai_ui_wake(ctx);
}

void fyai_tools_display_closed(struct fyai_ctx *ctx)
{
	struct fyai_shell_session *sess;
	struct fyai_tool_job *job;

	if (!ctx)
		return;
	for (sess = ctx->shell_sessions; sess; sess = sess->next) {
		if (sess->animation) {
			fyai_event_source_remove(sess->animation);
			sess->animation = NULL;
		}
		sess->surface = NULL;
	}
	for (job = ctx->tool_jobs; job; job = job->next) {
		if (job->animation) {
			fyai_event_source_remove(job->animation);
			job->animation = NULL;
		}
		job->surface = NULL;
	}
}

/* End and release every session owned by this invocation. */
void fyai_shell_sessions_release(struct fyai_ctx *ctx, bool force)
{
	struct fyai_shell_session *sess;

	if (!ctx)
		return;
	/* Each session leaves the list before it goes, so what its release
	 * calls finds only sessions that live. */
	while ((sess = ctx->shell_sessions) != NULL) {
		ctx->shell_sessions = sess->next;
		fyai_shell_session_release_one(sess, force);
	}
	fyai_tools_work_update(ctx);
}

/*
 * End and release every session owned by the turn. A shell the user opened
 * with a bang line is not the turn work: interrupting the turn must not
 * take it down.
 */
void fyai_shell_sessions_release_turn(struct fyai_ctx *ctx)
{
	struct fyai_shell_session *sess, **link;

	if (!ctx)
		return;
	link = &ctx->shell_sessions;
	while ((sess = *link) != NULL) {
		if (sess->user_owned) {
			link = &sess->next;
			continue;
		}
		*link = sess->next;
		fyai_shell_session_release_one(sess, true);
	}
	fyai_tools_work_update(ctx);
}

/* The reading a session offers: what appeared, the screen, or part of it. */
static enum fyai_terminal_read fyai_shell_view_kind(fy_generic args,
						    struct fyai_terminal_region *region)
{
	fy_generic view, r;

	view = fy_get(args, "view", fy_invalid);
	if (fy_equal(view, "screen"))
		return FYAITR_SCREEN;
	if (fy_equal(view, "all"))
		return FYAITR_ALL;
	if (fy_equal(view, "region")) {
		r = fy_get(args, "region", fy_invalid);
		region->row = fy_get(r, "row", 0);
		region->col = fy_get(r, "col", 0);
		region->rows = fy_get(r, "rows", 0);
		region->cols = fy_get(r, "cols", 0);
		return FYAITR_REGION;
	}
	return FYAITR_NEW;
}

/* Say what the session is and what the reading is, then give the text. */
static char *fyai_shell_session_result(struct fyai_ctx *ctx,
				       struct fyai_shell_session *sess,
				       fy_generic args,
				       enum fyai_terminal_read what,
				       const struct fyai_terminal_region *region)
{
	struct fyai_buf buf = {};
	char *text = NULL;
	char *bounded = NULL;
	size_t len = 0;
	int rows = 0, cols = 0;
	int rc;
	bool screen;

	screen = fyai_terminal_view_screen_mode(sess->view);
	fyai_terminal_view_size(sess->view, &rows, &cols);
	text = fyai_terminal_view_read(sess->view, what, region, &len);
	fyai_error_check(ctx, text, err,
			 "shell: could not read session '%s'", sess->name);

	if (screen)
		rc = fyai_buf_append(&buf,
			fy_sprintfa("[shell '%s': the screen it draws, %dx%d]\n",
				    sess->name, rows, cols));
	else
		rc = fyai_buf_append(&buf,
			fy_sprintfa("[shell '%s': %s]\n", sess->name,
				what == FYAITR_NEW ? "the lines since the last read" :
				"its output"));
	fyai_error_check(ctx, !rc, err,
			 "shell: could not build session '%s' result", sess->name);

	bounded = fyai_shell_bound_alloc(text,
					 fyai_shell_output_bytes(ctx, args));
	rc = fyai_buf_append(&buf, bounded ? bounded : text);
	fyai_error_check(ctx, !rc, err,
			 "shell: could not retain session '%s' output", sess->name);
	free(bounded);
	bounded = NULL;
	free(text);
	text = NULL;

	if (sess->exited) {
		if (sess->signal)
			rc = fyai_buf_append(&buf,
				fy_sprintfa("\n[shell '%s' ended: signal %d%s]\n",
					    sess->name, sess->signal,
					    sess->timed_out ? ", idle limit" : ""));
		else
			rc = fyai_buf_append(&buf,
				fy_sprintfa("\n[shell '%s' ended: status %d]\n",
					    sess->name, sess->exit_code));
		fyai_error_check(ctx, !rc, err,
				 "shell: could not append session '%s' outcome",
				 sess->name);
	}
	return buf.data;

err:
	free(bounded);
	free(text);
	free(buf.data);
	return NULL;
}

/*
 * Wait for the session to be quiet for @idle_ms before its input is written,
 * and no longer than @max_ms whatever it does. Each byte the program writes
 * restarts the quiet time, so this waits out a program that is still starting
 * and returns at once for one that is stopped for input.
 */
static void fyai_shell_session_idle_gate(struct fyai_shell_session *sess,
					 long long idle_ms, long long max_ms)
{
	struct fyai_event_loop *el;
	fyai_event_ms_t deadline, quiet, owed, now;

	if (idle_ms <= 0 || max_ms <= 0 || sess->exited)
		return;
	el = fyai_ctx_loop(sess->ctx);
	if (!el)
		return;
	sess->feeding = true;
	now = fyai_event_now_ms();
	deadline = now + max_ms;
	/*
	 * A session that drew nothing did not settle, because it did not
	 * start. Wait for its start before you read the quiet time, so that
	 * the result of a call does not depend on the start time of the
	 * program. A program that draws nothing and waits for input did
	 * start, and a read of its terminal reports this.
	 */
	while (!sess->drew && !sess->exited && now < deadline &&
	       !fyai_process_reads_stdin(sess->pid)) {
		owed = deadline - now;
		if (owed > FYAI_SHELL_FIRST_DRAW_POLL_MS)
			owed = FYAI_SHELL_FIRST_DRAW_POLL_MS;
		if (!fyai_wait_sleep(sess->ctx, owed))
			break;
		now = fyai_event_now_ms();
	}
	for (;;) {
		quiet = now - sess->quiet_since;
		if (quiet >= idle_ms || sess->exited || now >= deadline)
			break;
		/*
		 * Sleep what the quiet time still owes, or what is left before
		 * the gate opens anyway. Output during the sleep moves the
		 * quiet time, and the next round waits out the rest of it.
		 */
		owed = idle_ms - quiet;
		if (owed > deadline - now)
			owed = deadline - now;
		if (!fyai_wait_sleep(sess->ctx, owed))
			break;
		now = fyai_event_now_ms();
	}
	sess->feeding = false;
}

/* Find the session a call names, or say why it cannot be used. */
static struct fyai_shell_session *
fyai_shell_session_of(struct fyai_ctx *ctx, fy_generic args, char **errp)
{
	struct fyai_shell_session *sess;
	fy_generic name;

	*errp = NULL;
	name = fy_get(args, "name", fy_invalid);
	sess = fyai_shell_session_find(ctx, fy_castp(&name, ""));
	if (sess)
		return sess;
	/*
	 * A session lives for one invocation. A name from an earlier one names
	 * nothing, so report that rather than let the model wait for output.
	 */
	*errp = strdup(fy_sprintfa(
		"tool error: no shell named '%s' is open on branch %s; "
		"open one with the shell tool and a name",
		fy_castp(&name, ""), fyai_ctx_branch(ctx)));
	return NULL;
}

char *fyai_shell_output_tool(struct fyai_ctx *ctx, fy_generic args,
			     bool *okp)
{
	struct fyai_terminal_region region = {};
	struct fyai_shell_session *sess;
	enum fyai_terminal_read what;
	long long idle, max_wait;
	char *err;

	*okp = false;
	sess = fyai_shell_session_of(ctx, args, &err);
	if (!sess) {
		fyai_error_check(ctx, err, out,
				 "shell: could not report the missing session");
		return err;
	}

	/*
	 * Read only a program that has settled on its terminal. A session that
	 * has just opened has drawn nothing yet, and the reading of it must
	 * not depend on how fast the program started.
	 */
	idle = fy_get(args, "idle_trigger",
		      (long long)FYAI_SHELL_IDLE_TRIGGER_MS);
	if (idle > FYAI_SHELL_IDLE_WAIT_MAX_MS)
		idle = FYAI_SHELL_IDLE_WAIT_MAX_MS;
	max_wait = fy_get(args, "max_wait",
			  (long long)FYAI_SHELL_IDLE_MAX_WAIT_MS);
	if (max_wait > FYAI_SHELL_IDLE_WAIT_MAX_MS)
		max_wait = FYAI_SHELL_IDLE_WAIT_MAX_MS;
	fyai_shell_session_idle_gate(sess, idle, max_wait);

	fyai_shell_session_idle_arm(sess);
	what = fyai_shell_view_kind(args, &region);
	*okp = true;
	return fyai_shell_session_result(ctx, sess, args, what, &region);

out:
	return NULL;
}

char *fyai_shell_input_tool(struct fyai_ctx *ctx, fy_generic args,
			    bool *okp)
{
	struct fyai_terminal_region region = {};
	struct fyai_shell_session *sess;
	struct fy_generic_builder *gb;
	struct fyai_buf in = {};
	fy_generic input;
	const char *text;
	long long wait, idle, max_wait;
	char *err, *result;
	int rc;

	*okp = false;
	sess = fyai_shell_session_of(ctx, args, &err);
	if (!sess) {
		fyai_error_check(ctx, err, out,
				 "shell: could not report the missing session");
		return err;
	}
	if (sess->exited)
		return strdup(fy_sprintfa(
			"tool error: the shell '%s' has ended; its output can "
			"still be read with shell_output", sess->name));

	gb = fyai_ctx_transient_gb(ctx);
	if (!gb || !sess->job || !sess->job->conn)
		return strdup("tool error: the shell session is not reachable");

	input = fy_get(args, "input", fy_invalid);
	text = fy_castp(&input, "");
	rc = fyai_buf_append(&in, text);
	fyai_error_check(ctx, !rc, out,
			 "shell: could not retain session input");
	/* End the line as the session's terminal or pipe expects. */
	if (fy_get(args, "enter", true)) {
		rc = fyai_buf_append(&in, sess->pipes ? "\n" : "\r");
		fyai_error_check(ctx, !rc, out,
				 "shell: could not append return to session input");
	}
	/* Type only into a program that has settled on its terminal. */
	idle = fy_get(args, "idle_trigger",
		      (long long)FYAI_SHELL_IDLE_TRIGGER_MS);
	if (idle > FYAI_SHELL_IDLE_WAIT_MAX_MS)
		idle = FYAI_SHELL_IDLE_WAIT_MAX_MS;
	max_wait = fy_get(args, "max_wait",
			  (long long)FYAI_SHELL_IDLE_MAX_WAIT_MS);
	if (max_wait > FYAI_SHELL_IDLE_WAIT_MAX_MS)
		max_wait = FYAI_SHELL_IDLE_WAIT_MAX_MS;
	fyai_shell_session_idle_gate(sess, idle, max_wait);

	(void)jsonrpc_notify(sess->job->conn, "shell/write",
			     fyai_bytes_to_generic(gb, in.data, in.len));
	free(in.data);
	in.data = NULL;

	/* Let the program answer before the reading is taken. */
	wait = fy_get(args, "wait_ms", (long long)FYAI_SHELL_INPUT_WAIT_MS);
	if (wait < 0)
		wait = 0;
	if (wait > FYAI_SHELL_INPUT_WAIT_MAX_MS)
		wait = FYAI_SHELL_INPUT_WAIT_MAX_MS;
	if (wait)
		(void)fyai_wait_sleep(ctx, wait);

	fyai_shell_session_idle_arm(sess);
	*okp = true;
	result = fyai_shell_session_result(ctx, sess, args,
					   fyai_shell_view_kind(args, &region),
					   &region);
	fyai_error_check(ctx, result, out,
			 "shell: could not read session '%s'", sess->name);
	return result;

out:
	free(in.data);
	return NULL;
}

char *fyai_shell_close_tool(struct fyai_ctx *ctx, fy_generic args,
			    bool *okp)
{
	struct fyai_shell_session *sess;
	struct fyai_event_loop *el;
	bool force;
	char *err, *result;

	*okp = false;
	sess = fyai_shell_session_of(ctx, args, &err);
	if (!sess) {
		fyai_error_check(ctx, err, out,
				 "shell: could not report the missing session");
		return err;
	}
	if (sess->user_owned) {
		return strdup(fy_sprintfa(
			"tool error: the shell '%s' belongs to the user and cannot "
			"be closed by the model", sess->name));
	}

	force = fy_get(args, "force", false);
	if (!sess->exited) {
		fyai_shell_session_close(sess, force);
		/* Allow the program to exit before reporting its result. */
		el = fyai_ctx_loop(ctx);
		assert(el);
		(void)fyai_event_sleep_until(el, force ? 100 :
						    FYAI_TTY_CLOSE_WAIT_MS,
					     &sess->exited);
	}
	*okp = true;
	if (sess->exited)
		result = strdup(fy_sprintfa("[shell '%s' ended: status %d]",
					  sess->name, sess->signal ?
					  128 + sess->signal : sess->exit_code));
	else
		result = strdup(fy_sprintfa("[shell '%s' was asked to end]",
					    sess->name));
	fyai_error_check(ctx, result, out,
			 "shell: could not build session close result");
	return result;

out:
	return NULL;
}

/* Find the live session or sub-agent that owns @sf. */
static void fyai_tile_owner(struct fyai_ctx *ctx, struct fytim_surface *sf,
			    struct fyai_shell_session **sessp,
			    struct fyai_tool_job **jobp)
{
	enum fyai_workpane_tile_kind kind = FYAI_WORKPANE_TILE_TEXT;
	void *owner;

	*sessp = NULL;
	*jobp = NULL;
	owner = fyai_workpane_tile_owner(ctx ? ctx->workpane : NULL, sf, &kind);
	if (!owner)
		return;
	if (kind == FYAI_WORKPANE_TILE_SHELL)
		*sessp = owner;
	else if (kind == FYAI_WORKPANE_TILE_AGENT)
		*jobp = owner;
}

static void fyai_tools_zoom_write(struct fyai_shell_session *sess,
				  struct fyai_tool_job *job, const char *data,
				  size_t len);

/*
 * Return the view scroll in rows for a chunk of arrow and page keys. A chunk
 * can hold several keys; a chunk with any other byte scrolls nothing.
 */
static int fyai_btw_scroll_delta(struct fyai_tool_job *job,
				 const char *data, size_t len)
{
	int page = job->pty_rows > 2 ? job->pty_rows - 2 : 1;
	int delta = 0;

	while (len) {
		if (len >= 4 && !memcmp(data, "\x1b[5~", 4)) {
			delta += page;
			data += 4;
			len -= 4;
		} else if (len >= 4 && !memcmp(data, "\x1b[6~", 4)) {
			delta -= page;
			data += 4;
			len -= 4;
		} else if (len >= 3 && !memcmp(data, "\x1b[A", 3)) {
			delta += 1;
			data += 3;
			len -= 3;
		} else if (len >= 3 && !memcmp(data, "\x1b[B", 3)) {
			delta -= 1;
			data += 3;
			len -= 3;
		} else {
			return 0;
		}
	}
	return delta;
}

/* Whether @data starts with the key that closes a finished tile: q, or an
 * Escape that does not start a sequence. */
static bool fyai_tools_kept_closes(const char *data, size_t len)
{
	if (!len)
		return false;
	if (data[0] == 'q')
		return true;
	return data[0] == FYAI_KEY_ESC &&
	       (len == 1 || (data[1] != '[' && data[1] != 'O'));
}

/* Route keyboard input from the focused tile. */
static void fyai_tools_zoom_keys(void *user, const char *data, size_t len)
{
	struct fyai_ctx *ctx = user;
	struct fyai_shell_session *sess;
	struct fyai_tool_job *job;
	int delta;

	if (fyai_agents_keys(ctx, data, len))
		return;
	if (fyai_browser_surface(ctx, fyai_workpane_focused(ctx->workpane))) {
		(void)fyai_browser_keys(ctx, data, len);
		return;
	}
	/* A tile of a remote session: its program is in the worker. */
	if (fyai_uimodel_keys(ctx, fyai_workpane_focused(ctx->workpane), data,
			      len))
		return;
	fyai_tile_owner(ctx, fyai_workpane_focused(ctx->workpane), &sess,
			&job);
	if (!sess && !job) {
		/* Return focus after the program exits. */
		fyai_tools_unzoom(ctx);
		return;
	}
	if (job && job->btw_panel && len == 1 && data[0] == FYAI_KEY_ESC) {
		fyai_tool_job_discard(job);
		return;
	}
	/* A finished bang tile takes Escape or q to leave; its program reads
	 * nothing more. The keys after it go to the prompt. */
	if (sess && sess->exited && sess->keep_tile &&
	    fyai_tools_kept_closes(data, len)) {
		fyai_shell_session_dismiss(sess);
		fyai_tools_unzoom(ctx);
		if (len > 1 && fyai_ui_keys_return(ctx, data + 1, len - 1))
			fyai_warning(ctx, "the keys after the close of a tile "
				     "are lost");
		return;
	}
	if (job && job->btw_panel && job->view) {
		delta = fyai_btw_scroll_delta(job, data, len);
		if (delta) {
			if (fyai_terminal_view_scroll(job->view, delta))
				fyai_agent_view_refresh(job);
			return;
		}
	}
	fyai_tools_zoom_write(sess, job, data, len);
}

/* Route a surface control request to its owner. */
void fyai_tools_surface_request(struct fyai_ctx *ctx, struct fytim_surface *sf,
				int delta)
{
	struct fyai_shell_session *sess;
	struct fyai_tool_job *job;
	struct fytim_surface *surface;
	struct fyai_terminal_view *view;

	if (!ctx || !sf)
		return;
	if (delta) {
		/* Scroll the terminal view without sending input to the program. */
		fyai_tile_owner(ctx, sf, &sess, &job);
		view = sess ? sess->view : job ? job->view : NULL;
		surface = sess ? sess->surface : job ? job->surface : NULL;
		if (view && surface && fyai_terminal_view_scroll(view, delta) &&
		    fyai_ui_surface_publish(surface, view) > 0)
			fyai_ui_wake(ctx);
		return;
	}
	if (fyai_browser_surface(ctx, sf)) {
		fyai_browser_close(ctx);
		return;
	}
	if (fyai_agents_surface(ctx, sf)) {
		fyai_agents_detach(ctx);
		return;
	}

	fyai_tile_owner(ctx, sf, &sess, &job);
	/* Request graceful termination so the result remains available. */
	if (sess && sess->exited && sess->keep_tile)
		fyai_shell_session_dismiss(sess);
	else if (sess) {
		/* A closed tile goes when its program ends. */
		sess->close_on_exit = true;
		fyai_shell_session_close(sess, false);
	}
	else if (job) {
		if (job->btw_panel)
			fyai_tool_job_discard(job);
		else
			fyai_tool_job_cancel(job);
	}
}

/* Send @len bytes to the terminal owner. */
static void fyai_tools_zoom_write(struct fyai_shell_session *sess,
				  struct fyai_tool_job *job, const char *data,
				  size_t len)
{
	struct fyai_terminal_view *view = sess ? sess->view : job ? job->view :
					  NULL;

	/* Input returns the view to the live screen. */
	if (view && !(job && job->btw_panel) &&
	    fyai_terminal_view_scroll_live(view) &&
	    (sess ? sess->surface : job->surface))
		(void)fyai_ui_surface_publish(sess ? sess->surface :
					      job->surface, view);
	if (sess)
		fyai_shell_session_reply(data, len, sess);
	else if (job && job->pty >= 0)
		fyai_agent_view_reply(data, len, job);
}

int fyai_tools_session_input(struct fyai_ctx *ctx, const char *name,
			     const char *data, size_t len)
{
	struct fyai_shell_session *sess = fyai_shell_session_find(ctx, name);
	struct fyai_tool_job *job;

	/* The terminal of a sub-agent is named by its branch. */
	if (!sess) {
		for (job = ctx->tool_jobs; job; job = job->next)
			if (job->agent && job->phase != FYAI_JOB_DONE &&
			    job->pty >= 0 &&
			    job->branch && !strcmp(job->branch, name))
				break;
		if (!job)
			return -1;
		fyai_tools_zoom_write(NULL, job, data, len);
		return 0;
	}
	/* A finished command of the user takes Escape or q to leave. */
	if (sess->exited && sess->keep_tile && len == 1 &&
	    (data[0] == FYAI_KEY_ESC || data[0] == 'q')) {
		fyai_shell_session_dismiss(sess);
		return 0;
	}
	if (sess->exited || !sess->job)
		return -1;
	fyai_tools_zoom_write(sess, NULL, data, len);
	return 0;
}

int fyai_tools_session_resize(struct fyai_ctx *ctx, const char *name,
			      int rows, int cols)
{
	struct fyai_shell_session *sess = fyai_shell_session_find(ctx, name);
	struct fy_generic_builder *gb;

	if (!sess || rows < 1 || cols < 1)
		return -1;
	if (!sess->job || !sess->job->conn || sess->exited) {
		fyai_shell_session_resized(sess, rows, cols);
		return 0;
	}
	if (rows == sess->resize_rows && cols == sess->resize_cols)
		return 0;
	gb = fyai_ctx_transient_gb(ctx);
	if (!gb || jsonrpc_notify(sess->job->conn, "tty/resize",
				  fy_mapping(gb, "rows", (long long)rows,
					     "cols", (long long)cols)))
		return -1;
	sess->resize_rows = rows;
	sess->resize_cols = cols;
	return 0;
}

int fyai_tools_session_close(struct fyai_ctx *ctx, const char *name,
			     bool force)
{
	struct fyai_shell_session *sess = fyai_shell_session_find(ctx, name);

	if (!sess)
		return -1;
	sess->close_on_exit = true;
	fyai_shell_session_close(sess, force);
	return 0;
}

int fyai_tools_session_dismiss(struct fyai_ctx *ctx, const char *name)
{
	struct fyai_shell_session *sess = fyai_shell_session_find(ctx, name);

	if (!sess || !sess->exited)
		return -1;
	fyai_shell_session_dismiss(sess);
	return 0;
}

/* Give @sf terminal keyboard focus. Focus changes no geometry. */
static bool fyai_tools_focus(struct fyai_ctx *ctx, struct fytim_surface *sf)
{
	if (!ctx || !sf)
		return false;
	fyai_workpane_set_keys_router(ctx->workpane, fyai_tools_zoom_keys, ctx);
	fyai_workpane_set_focus(ctx->workpane, sf);
	return fyai_workpane_focused(ctx->workpane) == sf;
}

/* Give the named live tile keyboard focus and the full work pane. */
const char *fyai_tools_zoom(struct fyai_ctx *ctx, const char *name)
{
	struct fyai_shell_session *sess;
	struct fyai_shell_session *zoom_sess = NULL;
	struct fyai_tool_job *job;
	struct fyai_tool_job *zoom_job = NULL;
	struct fytim_surface *sf = NULL;
	const char *what = NULL;

	if (!ctx)
		return NULL;
	if (fyai_agents_ambiguous(ctx, name)) {
		fyai_error(ctx, "zoom: agent name '%s' is ambiguous; use its full branch name", name);
		return NULL;
	}
	for (sess = ctx->shell_sessions; sess && !sf; sess = sess->next) {
		if (!sess->surface || sess->exited)
			continue;
		if (!name || !*name || (sess->name && !strcmp(sess->name, name))) {
			sf = sess->surface;
			what = sess->name;
			zoom_sess = sess;
		}
	}
	for (job = ctx->tool_jobs; job && !sf; job = job->next) {
		const char *agent;

		if (!job->surface)
			continue;
		/* Derive a sub-agent name from its branch. */
		agent = job->agent ? fyai_agent_job_name(job) : job->title;
		if (!name || !*name || (job->branch && !strcmp(job->branch, name)) ||
		    (agent && !strcmp(agent, name))) {
			sf = job->surface;
			what = agent;
			zoom_job = job;
		}
	}
	if (!sf)
		return fyai_agents_zoom(ctx, name, false);

	(void)zoom_sess;
	fyai_tools_unzoom(ctx);
	if (fyai_ui_surface_zoom(ctx, sf))
		return NULL;
	if (!fyai_tools_focus(ctx, sf)) {
		(void)fyai_ui_surface_zoom(ctx, NULL);
		return NULL;
	}
	if (zoom_job && zoom_job->btw_panel) {
		fyai_terminal_view_damage_all(zoom_job->view);
		fyai_agent_view_refresh(zoom_job);
	}
	return what ? what : "";
}

/* Return the work pane and keyboard focus to the prompt. */
void fyai_tools_unzoom(struct fyai_ctx *ctx)
{
	if (!ctx)
		return;
	fyai_agents_detach(ctx);
	fyai_workpane_clear_focus(ctx->workpane);
	fyai_workpane_clear_zoom(ctx->workpane);
}

void fyai_tools_counts(struct fyai_ctx *ctx, int *userp, int *shellp,
		       int *agentp)
{
	struct fyai_shell_session *sess;
	struct fyai_tool_job *job;

	*userp = *shellp = *agentp = 0;
	if (!ctx)
		return;
	for (sess = ctx->shell_sessions; sess; sess = sess->next) {
		if (sess->exited)
			continue;
		if (sess->user_owned)
			(*userp)++;
		else
			(*shellp)++;
	}
	for (job = ctx->tool_jobs; job; job = job->next)
		if (job->agent && job->phase != FYAI_JOB_DONE)
			(*agentp)++;
}

void fyai_tools_config_changed(struct fyai_ctx *ctx)
{
	struct fyai_shell_session *sess;
	struct fyai_tool_job *job;
	int rows;

	if (!ctx)
		return;
	/* Apply the new scrollback limit to each active terminal view. */
	rows = ctx->cfg->work_history_rows;
	for (sess = ctx->shell_sessions; sess; sess = sess->next)
		if (sess->view &&
		    !fyai_terminal_view_set_history(sess->view, rows) &&
		    sess->surface)
			(void)fyai_ui_surface_publish(sess->surface, sess->view);
	for (job = ctx->tool_jobs; job; job = job->next)
		if (job->view &&
		    !fyai_terminal_view_set_history(job->view, rows) &&
		    job->surface)
			(void)fyai_ui_surface_publish(job->surface, job->view);
}

bool fyai_tools_focus_tile(struct fyai_ctx *ctx, struct fytim_surface *sf)
{
	struct fytim_surface *tiles[FYAI_WORKPANE_TILES_MAX];
	int n, i;

	if (!ctx || !sf)
		return false;
	/* Only visible tiles may receive keyboard focus. */
	n = fyai_workpane_screen_order(ctx->workpane, tiles,
				       FYAI_WORKPANE_TILES_MAX);
	for (i = 0; i < n && tiles[i] != sf; i++)
		;
	if (i == n)
		return false;
	if (fyai_workpane_focused(ctx->workpane) == sf)
		return true;
	return fyai_tools_focus(ctx, sf);
}

void fyai_tools_focus_prompt(struct fyai_ctx *ctx)
{
	/* Preserve zoom and attachment state while returning focus. */
	if (ctx)
		fyai_workpane_clear_focus(ctx->workpane);
}

/* What Ctrl-T can give the keys to: a block of the transcript or a tile,
 * where the last frame drew it. */
struct fyai_focus_target {
	int row, col, seq;
	uintptr_t key;			/* a block of the transcript, or 0 */
	struct fytim_surface *sf;	/* a tile, or NULL */
};

static int fyai_focus_target_cmp(const void *a, const void *b)
{
	const struct fyai_focus_target *x = a, *y = b;

	if (x->row != y->row)
		return x->row < y->row ? -1 : 1;
	if (x->col != y->col)
		return x->col < y->col ? -1 : 1;
	return x->seq - y->seq;
}

/*
 * The places that take the keys, in the order the screen shows them: from
 * the top row down, and from left to right in a row. A block or a tile that
 * the page did not place goes after those it did, in the order it opened.
 */
static int fyai_focus_targets(struct fyai_ctx *ctx,
			      struct fyai_focus_target *out, int max)
{
	struct fytim_surface *tiles[FYAI_WORKPANE_TILES_MAX];
	struct fyai_shell_session *sess;
	struct fyai_tool_job *job;
	uintptr_t key;
	int n = 0, ntiles, i, row, col, at;

	for (sess = ctx->shell_sessions; sess; sess = sess->next) {
		if (!sess->inline_block || sess->exited || n >= max)
			continue;
		out[n].key = (uintptr_t)sess;
		out[n].sf = NULL;
		n++;
	}
	for (job = ctx->tool_jobs; job; job = job->next) {
		if (!job->inline_block || job->phase == FYAI_JOB_DONE ||
		    n >= max)
			continue;
		out[n].key = (uintptr_t)job;
		out[n].sf = NULL;
		n++;
	}
	for (i = 0; i < n; i++) {
		key = out[i].key;
		at = fyai_ui_inline_index(ctx, key);
		out[i].seq = at;
		if (fyai_ui_inline_pos(ctx, key, &row, &col)) {
			out[i].row = row;
			out[i].col = col;
		} else {
			out[i].row = INT_MAX / 2 + at;
			out[i].col = 0;
		}
	}
	ntiles = fyai_workpane_screen_order(ctx->workpane, tiles,
					    FYAI_WORKPANE_TILES_MAX);
	for (i = 0; i < ntiles && n < max; i++, n++) {
		out[n].key = 0;
		out[n].sf = tiles[i];
		out[n].seq = FYAI_WORKPANE_TILES_MAX + i;
		if (!fyai_ui_tile_pos(ctx, tiles[i], &out[n].row,
				      &out[n].col)) {
			out[n].row = INT_MAX / 2 + FYAI_WORKPANE_TILES_MAX + i;
			out[n].col = 0;
		}
	}
	qsort(out, (size_t)n, sizeof(*out), fyai_focus_target_cmp);
	return n;
}

/* The owner of the live block of @key: a session or a sub-agent. */
static void fyai_inline_owner(struct fyai_ctx *ctx, uintptr_t key,
			      struct fyai_shell_session **sessp,
			      struct fyai_tool_job **jobp)
{
	struct fyai_shell_session *sess;
	struct fyai_tool_job *job;

	*sessp = NULL;
	*jobp = NULL;
	if (!key)
		return;
	for (sess = ctx->shell_sessions; sess; sess = sess->next)
		if ((uintptr_t)sess == key && sess->inline_block) {
			*sessp = sess;
			return;
		}
	for (job = ctx->tool_jobs; job; job = job->next)
		if ((uintptr_t)job == key && job->inline_block) {
			*jobp = job;
			return;
		}
}

/* Draw the block of @key again, as the focus edge moved to it or from it. */
static void fyai_inline_repaint(struct fyai_ctx *ctx, uintptr_t key)
{
	struct fyai_shell_session *sess;
	struct fyai_tool_job *job;

	fyai_inline_owner(ctx, key, &sess, &job);
	if (sess)
		fyai_shell_session_inline_paint(sess, false);
	else if (job)
		fyai_agent_inline_paint(job, false, true);
}

static void fyai_inline_keys(void *user, const char *data, size_t len);

/* Give the keys to the block of @key, or with 0 back to the prompt. */
static bool fyai_inline_focus(struct fyai_ctx *ctx, uintptr_t key)
{
	uintptr_t was = fyai_ui_inline_focused(ctx);
	int rc;

	rc = fyai_ui_inline_keys(ctx, key, fyai_inline_keys, ctx);
	if (was && was != key)
		fyai_inline_repaint(ctx, was);
	if (key && !rc)
		fyai_inline_repaint(ctx, key);
	return !rc;
}

/*
 * What is typed while a block of the transcript holds the keys goes to its
 * program. The keys that move the focus are keys of the surface mode of the
 * terminal library: it reports them in order with these bytes, so what
 * follows such a key goes to the new owner.
 */
static void fyai_inline_keys(void *user, const char *data, size_t len)
{
	struct fyai_ctx *ctx = user;
	struct fyai_shell_session *sess;
	struct fyai_tool_job *job;

	fyai_inline_owner(ctx, fyai_ui_inline_focused(ctx), &sess, &job);
	if (!sess && !job) {
		(void)fyai_inline_focus(ctx, 0);
		(void)fyai_ui_keys_return(ctx, data, len);
		return;
	}
	fyai_tools_zoom_write(sess, job, data, len);
}

/*
 * Give the keys to the prompt from wherever they are: a view of the agents
 * is left, a block of the transcript lets go of them, and a tile or the
 * branch browser is unzoomed.
 */
void fyai_tools_keys_to_prompt(struct fyai_ctx *ctx)
{
	if (fyai_ui_inline_focused(ctx))
		(void)fyai_inline_focus(ctx, 0);
	else if (fyai_agents_surface(ctx, fyai_workpane_focused(ctx->workpane)))
		fyai_agents_detach(ctx);
	else
		fyai_tools_unzoom(ctx);
}

/*
 * Move the keys on to the next place that takes them in the order the screen
 * shows them, blocks of the transcript and tiles alike, and from the last
 * back to the prompt. A zoomed tile is the whole cycle.
 */
bool fyai_tools_focus_next(struct fyai_ctx *ctx)
{
	struct fyai_focus_target targets[2 * FYAI_WORKPANE_TILES_MAX];
	struct fytim_surface *focused;
	uintptr_t cur;
	int n, i;

	if (!ctx)
		return false;
	fyai_workpane_set_keys_router(ctx->workpane, fyai_tools_zoom_keys, ctx);
	if (fyai_workpane_zoomed(ctx->workpane))
		return fyai_workpane_focus_next(ctx->workpane);
	cur = fyai_ui_inline_focused(ctx);
	focused = fyai_workpane_focused(ctx->workpane);
	n = fyai_focus_targets(ctx, targets, 2 * FYAI_WORKPANE_TILES_MAX);
	for (i = 0; i < n; i++)
		if ((cur && targets[i].key == cur) ||
		    (!cur && focused && targets[i].sf == focused))
			break;
	/* From the prompt the cycle starts at the first place. */
	i = (cur || focused) ? i + 1 : 0;
	if (i >= n) {
		if (!cur && !focused)
			return false;
		(void)fyai_inline_focus(ctx, 0);
		fyai_workpane_clear_focus(ctx->workpane);
		return true;
	}
	if (targets[i].key)
		return fyai_inline_focus(ctx, targets[i].key);
	(void)fyai_inline_focus(ctx, 0);
	return fyai_tools_focus(ctx, targets[i].sf);
}

fy_generic fyai_tools_sessions_data(struct fyai_ctx *ctx,
				    struct fy_generic_builder *gb)
{
	struct fyai_shell_session *sess;
	struct fyai_tool_job *job;
	fy_generic rows, agents, agent;

	rows = fy_gb_sequence(gb);
	agents = fyai_agents_rows(ctx, gb);
	fy_foreach(agent, agents) {
		if (!fy_get(agent, "active", false) ||
		    fy_get(agent, "parent", 0LL) == ctx->agent_execution)
			continue;
		rows = fy_append(gb, rows, fy_mapping(gb,
			"name", fy_get(agent, "branch", ""), "kind", "agent",
			"state", fy_get(agent, "state", "running")));
	}
	for (sess = ctx->shell_sessions; sess; sess = sess->next) {
		if (sess->exited || !sess->surface)
			continue;
		rows = fy_append(gb, rows, fy_mapping(gb,
			"name", sess->name,
			"kind", "shell",
			"state", sess->closing ? "stopping" :
				fyai_workpane_focused(ctx->workpane) ==
					sess->surface ? "focused" :
				"running"));
	}
	for (job = ctx->tool_jobs; job; job = job->next) {
		if (!job->agent ||
		    (job->phase == FYAI_JOB_DONE && !job->btw_panel))
			continue;
		rows = fy_append(gb, rows, fy_mapping(gb,
			"name", fyai_agent_job_name(job),
			"kind", job->btw ? "btw" : "agent",
			"state", job->btw_panel ? "finished" :
				job->terminating ? "stopping" :
				fyai_workpane_focused(ctx->workpane) ==
					job->surface ? "focused" :
				job->wants_input ? "waiting" : "running"));
	}
	return rows;
}

int fyai_tools_kill(struct fyai_ctx *ctx, const char *name,
		    const char **actionp)
{
	struct fyai_shell_session *sess = NULL, *candidate;
	struct fyai_tool_job *agent;

	if (!ctx || fy_str_empty(name)) {
		fyai_error(ctx, "kill: a session name is required");
		return -1;
	}
	if (fyai_agents_ambiguous(ctx, name)) {
		fyai_error(ctx, "kill: agent name '%s' is ambiguous; use its full branch name", name);
		return -1;
	}
	for (candidate = ctx->shell_sessions; candidate;
	     candidate = candidate->next)
		if ((!candidate->exited ||
		     (candidate->keep_tile && candidate->surface)) &&
		    !strcmp(candidate->name, name)) {
			sess = candidate;
			break;
		}
	agent = fyai_agent_job_named(ctx, name);
	if (!agent)
		for (agent = ctx->tool_jobs; agent; agent = agent->next)
			if (agent->btw_panel &&
			    (!strcmp(agent->branch, name) ||
			     !strcmp(fyai_agent_job_name(agent), name)))
				break;
	if (sess && agent) {
		fyai_error(ctx, "kill: '%s' names both a shell and a sub-agent",
			   name);
		return -1;
	}
	if (!sess && !agent) {
		if (!fyai_agents_kill(ctx, name)) {
			*actionp = "stopping agent";
			return 0;
		}
		if (fyai_scope_stop(ctx, name)) {
			*actionp = "stopping daemon";
			return 0;
		}
		fyai_error(ctx, "kill: no active shell session, sub-agent or daemon is "
			   "called '%s'", name);
		return -1;
	}
	if (sess && sess->exited) {
		fyai_shell_session_dismiss(sess);
		*actionp = "closed shell";
	} else if (sess) {
		fyai_shell_session_close(sess, false);
		*actionp = "stopping shell";
	} else {
		if (agent->btw_panel) {
			fyai_tool_job_discard(agent);
			*actionp = "closed btw panel";
		} else {
			fyai_tool_job_cancel(agent);
			*actionp = "stopping agent";
		}
	}
	return 0;
}

/*
 * Start @command as a user-owned terminal session in the work pane, named
 * @prefix-N. @what names the program in a diagnostic. *@sessp is the session
 * of the job, or NULL when the job has none.
 */
static int fyai_tools_user_start(struct fyai_ctx *ctx, const char *command,
				 const char *prefix, const char *what,
				 fy_generic env_keep, int rows, int cols,
				 struct fyai_shell_session **sessp)
{
	struct fyai_tool_job *job = NULL;
	struct fy_generic_builder *gb;
	fy_generic call;
	fy_generic args;
	const char *args_text;
	char name[32];
	unsigned int n;

	*sessp = NULL;
	if (!ctx)
		return -1;
	/* A remote client shows the program of a worker with no display. */
	fyai_error_check(ctx, fyai_ui_active(ctx) ||
			 fyai_uimodel_consumed(ctx), err,
			 "a %s needs an interactive terminal UI", what);
	/* A key event starts a program between turns, with no turn storage. */
	gb = fyai_ctx_transient_gb(ctx);
	fyai_error_check(ctx, gb, err,
			 "could not make scratch storage for the %s", what);
	/* Allocate a unique user-visible session name. */
	for (n = 1; n < 1000000; n++) {
		snprintf(name, sizeof(name), "%s-%u", prefix, n);
		if (!fyai_shell_session_find(ctx, name))
			break;
	}
	fyai_error_check(ctx, n != 1000000, err,
			 "could not allocate a name for the %s", what);
	args = fy_mapping(gb,
			  "command", command ? command : "",
			  "tty", true,
			  "name", name,
			  "_fyai_user_owned", true);
	/* A client of a remote session gives the size of the tile that shows
	 * the program, as a local tile gives it. */
	if (fy_is_valid(args) && rows > 0 && cols > 0)
		args = fy_assoc(gb, args, "rows", (long long)rows,
				"cols", (long long)cols);
	/* A program of the configuration runs as the catalogue verb does. */
	if (fy_is_valid(args) && fy_is_sequence(env_keep))
		args = fy_assoc(gb, args, "_fyai_env_keep", env_keep,
				"_fyai_unconfined", true);
	args_text = emit_json_string(gb, args);
	call = fy_mapping(gb,
			  "type", "function_call",
			  "name", "shell",
			  "arguments", args_text ? args_text : "");
	fyai_error_check(ctx, fy_is_valid(call), err,
			 "could not build the %s request", what);
	/* Retain the call beyond transient interactive storage. */
	call = fy_gb_internalize(ctx->gb, call);
	fyai_error_check(ctx, fy_is_valid(call), err,
			 "could not retain the %s request", what);
	job = fyai_tool_job_submit(ctx, call);
	fyai_error_check(ctx, job, err, "could not start the %s", what);
	*sessp = job->session;
	return 0;

err:
	return -1;
}

int fyai_tools_bang(struct fyai_ctx *ctx, const char *command, int rows,
		    int cols)
{
	struct fyai_shell_session *sess;

	if (fyai_tools_user_start(ctx, command, "bang", "bang shell",
				  fy_invalid, rows, cols, &sess))
		return -1;
	if (sess)
		sess->keep_tile = true;
	if (sess && sess->surface)
		(void)fyai_tools_focus(ctx, sess->surface);
	return 0;
}

static void fyai_tools_probe_input(void *data, const char *name,
				   const char *bytes, size_t len)
{
	(void)data;
	(void)name;
	(void)bytes;
	(void)len;
}

static void fyai_tools_probe_resize(void *data, const char *name, int rows,
				    int cols)
{
	int *size = data;

	(void)name;
	size[0] = rows;
	size[1] = cols;
}

static void fyai_tools_probe_close(void *data, const char *name, bool force)
{
	(void)data;
	(void)name;
	(void)force;
}

static void fyai_tools_probe_dismiss(void *data, const char *name)
{
	(void)data;
	(void)name;
}

/* A mirror that is opened only to read the grant of its tile. */
static const struct fyai_tools_remote fyai_tools_probe_ops = {
	.input = fyai_tools_probe_input,
	.resize = fyai_tools_probe_resize,
	.close = fyai_tools_probe_close,
	.dismiss = fyai_tools_probe_dismiss,
};

int fyai_tools_bang_size(struct fyai_ctx *ctx, const char *command,
			 int *rowsp, int *colsp)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	struct fyai_shell_session *sess;
	char name[32], *title;
	int size[2] = { 0, 0 };
	unsigned int n;

	*rowsp = *colsp = 0;
	for (n = 1; n < 1000000; n++) {
		snprintf(name, sizeof(name), "bang-%u", n);
		if (!fyai_shell_session_find(ctx, name))
			break;
	}
	/* The tile that a typed bang opens, with its head, so its grant is the
	 * grant of the mirror that shows the program. */
	title = fyai_format_shell_label(fy_mapping(gb, "command", command,
						   "name", name));
	sess = fyai_shell_session_create(ctx, name, command, title, NULL,
					 FYAI_TTY_ROWS_DEFAULT,
					 FYAI_TTY_COLS_DEFAULT, 0, false, true,
					 &fyai_tools_probe_ops, size);
	free(title);
	if (!sess)
		return -1;
	sess->keep_tile = true;
	if (sess->surface && fyai_ui_layout_now(ctx)) {
		*rowsp = fyai_ui_surface_granted_rows(ctx, sess->surface);
		*colsp = fyai_ui_surface_granted_cols(ctx, sess->surface);
	}
	fyai_tools_mirror_drop(sess);
	return 0;
}

struct fyai_shell_session *fyai_tools_mirror_open(struct fyai_ctx *ctx,
		fy_generic item, const struct fyai_tools_remote *remote,
		void *data)
{
	fy_generic term = fy_get(item, "terminal", fy_invalid);
	const char *name = fy_get(item, "name", ""), *title;
	struct fyai_shell_session *sess;
	bool user_owned;
	int rows, cols;

	title = fy_get(item, "title", "");
	user_owned = fy_equal(fy_get(item, "kind", ""), "user-shell");
	rows = fy_get(term, "rows", FYAI_TTY_ROWS_DEFAULT);
	cols = fy_get(term, "cols", FYAI_TTY_COLS_DEFAULT);
	/* Opened at the size that a local session opens with, and then given
	 * the size of the program, as the grant of a local tile gives it: the
	 * head is made again for the width of the tile. */
	sess = fyai_shell_session_create(ctx, name, fy_get(item, "command", ""),
					 *title ? title : NULL, NULL,
					 FYAI_TTY_ROWS_DEFAULT,
					 FYAI_TTY_COLS_DEFAULT, 0, false,
					 user_owned, remote, data);
	fyai_error_check(ctx, sess, err, "cannot show the terminal %s", name);
	if (rows != FYAI_TTY_ROWS_DEFAULT || cols != FYAI_TTY_COLS_DEFAULT)
		fyai_shell_session_resized(sess, rows, cols);
	/* A screen drawn in the transcript has the rows of the setting and the
	 * width of the transcript, as the screen of a local call has. */
	if (sess->inline_block && !sess->exited &&
	    (rows != ctx->cfg->inline_terminal_rows ||
	     cols != fyai_shell_inline_cols(ctx)))
		fyai_shell_session_apply_grant(sess,
					       ctx->cfg->inline_terminal_rows,
					       fyai_shell_inline_cols(ctx));
	fyai_tools_mirror_update(sess, item);
	return sess;
err:
	return NULL;
}

void fyai_tools_mirror_feed(struct fyai_shell_session *sess,
			    const char *data, size_t len)
{
	if (!sess || !len)
		return;
	/* As the output of a local program: see "shell/output". */
	sess->wants_input = false;
	sess->quiet_since = fyai_event_now_ms();
	sess->drew = true;
	(void)fyai_terminal_view_feed(sess->view, data, len);
	fyai_shell_session_refresh(sess);
}

bool fyai_tools_mirror_update(struct fyai_shell_session *sess, fy_generic item)
{
	fy_generic term = fy_get(item, "terminal", fy_invalid);
	int rows = 0, cols = 0;
	bool resized = false;

	if (!sess)
		return false;
	/* A command of the user is kept to read, and takes the keys, as a
	 * typed bang takes them. */
	if (!sess->keep_tile && fy_get(item, "keep", false)) {
		sess->keep_tile = true;
		if (sess->user_owned && sess->surface)
			(void)fyai_tools_focus(sess->ctx, sess->surface);
	}
	/* A program of the user, such as an editor, takes the pane and the
	 * keys, as it takes them here. */
	if (!sess->takes_pane && fy_get(item, "pane", false)) {
		sess->takes_pane = true;
		if (sess->surface) {
			(void)fyai_tools_focus(sess->ctx, sess->surface);
			(void)fyai_ui_surface_zoom(sess->ctx, sess->surface);
		}
	}
	/* The worker resized its program: the bytes that follow are at the
	 * new size, as after "tty/resized". */
	fyai_terminal_view_size(sess->view, &rows, &cols);
	if (fy_is_mapping(term) &&
	    (fy_get(term, "rows", rows) != rows ||
	     fy_get(term, "cols", cols) != cols)) {
		fyai_shell_session_resized(sess, fy_get(term, "rows", 0),
					   fy_get(term, "cols", 0));
		resized = true;
	}
	if (sess->exited || !fy_get(item, "exited", false))
		return resized;
	sess->closing = fy_get(item, "closing", false);
	sess->timed_out = fy_get(item, "timed_out", false);
	fyai_shell_session_exited(sess, fy_get(item, "exit_code", 0),
				  fy_get(item, "signal", 0));
	/* A tile that the user closed is not kept to read. */
	if (sess->close_on_exit && sess->keep_tile)
		fyai_shell_session_dismiss(sess);
	return resized;
}

void fyai_tools_mirror_drop(struct fyai_shell_session *sess)
{
	struct fyai_ctx *ctx;
	struct fyai_shell_session **link;

	if (!sess)
		return;
	ctx = sess->ctx;
	for (link = &ctx->shell_sessions; *link && *link != sess;
	     link = &(*link)->next)
		;
	if (*link)
		*link = sess->next;
	/* The worker dropped it: a release sends nothing back. */
	fyai_shell_session_release_one(sess, false);
	fyai_ui_wake(ctx);
}

static struct fyai_shell_session *
fyai_tools_program_start(struct fyai_ctx *ctx, const char *command,
			 const char *prefix, const char *what,
			 fy_generic env_keep, fyai_tools_exit_fn done,
			 void *userdata)
{
	struct fyai_shell_session *sess;

	if (fyai_tools_user_start(ctx, command, prefix, what, env_keep, 0, 0,
				  &sess))
		return NULL;
	fyai_error_check(ctx, sess, err,
			 "the %s was given no terminal session", what);
	/* The end is reported from the loop, so it cannot come before this. */
	sess->on_exit = done;
	sess->on_exit_data = userdata;
	sess->takes_pane = true;
	if (sess->surface) {
		(void)fyai_tools_focus(ctx, sess->surface);
		(void)fyai_ui_surface_zoom(ctx, sess->surface);
	}
	/* A client of a remote session gives it the pane as this does. */
	fyai_tools_work_update(ctx);
	return sess;

err:
	return NULL;
}

struct fyai_shell_session *
fyai_tools_user_program(struct fyai_ctx *ctx, const char *command,
			fyai_tools_exit_fn done, void *userdata)
{
	return fyai_tools_program_start(ctx, command, "edit", "editor",
					fy_invalid, done, userdata);
}

struct fyai_shell_session *
fyai_tools_config_program(struct fyai_ctx *ctx, const char *command,
			  const char *prefix, fy_generic env_keep,
			  fyai_tools_exit_fn done, void *userdata)
{
	return fyai_tools_program_start(ctx, command, prefix, prefix,
					fy_is_sequence(env_keep) ? env_keep :
					fy_seq_empty, done, userdata);
}

void fyai_tools_user_program_close(struct fyai_shell_session *sess)
{
	fyai_shell_session_close(sess, false);
}

void fyai_tools_user_program_forget(struct fyai_shell_session *sess)
{
	if (!sess)
		return;
	sess->on_exit = NULL;
	sess->on_exit_data = NULL;
}
