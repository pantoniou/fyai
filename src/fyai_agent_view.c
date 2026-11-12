/*
 * fyai_agent_view.c - sub-agent screens and inline terminals
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

/* Size an agent terminal to its display surface. */
#define FYAI_AGENT_TTY_ROWS	12

void fyai_agent_tty_size(struct fyai_ctx *ctx, int *rowsp, int *colsp)
{
	int rows = 0, cols = 0;

	/* A screen drawn in the transcript has the rows of the setting and
	 * the width of the transcript. */
	if (fyai_ui_tools_inline(ctx)) {
		*rowsp = ctx->cfg->inline_terminal_rows;
		*colsp = fyai_shell_inline_cols(ctx);
		return;
	}

	if (fyai_ui_size(ctx, &cols, &rows) || cols < 1) {
		cols = markdown_render_width();
		rows = markdown_render_height();
	}
	/* A worker with no display sizes the screen a remote client shows
	 * as a terminal of its own is sized. */
	if (!fyai_ui_active(ctx) && cols < FYAI_TTY_COLS_DEFAULT) {
		cols = FYAI_TTY_COLS_DEFAULT;
		rows = FYAI_AGENT_TTY_ROWS;
	}
	cols -= markdown_gutter_cols(ctx->cfg);
	if (cols < FYAI_TTY_COLS_DEFAULT / 4)
		cols = FYAI_TTY_COLS_DEFAULT / 4;
	if (rows > FYAI_AGENT_TTY_ROWS || rows < 1)
		rows = FYAI_AGENT_TTY_ROWS;
	*rowsp = rows;
	*colsp = cols;
}

static const struct fyai_workpane_tile_ops fyai_agent_tile_ops;
static void fyai_agent_head_repaint(void *owner);

/* Size the sub-agent's terminal to the grant it received. */
static void fyai_agent_apply_grant(void *owner, int rows, int cols)
{
	struct fyai_tool_job *job = owner;
	struct winsize ws = {};

	if ((!job->surface && !job->inline_block) || cols < 1)
		return;
	if (rows < 1)
		rows = job->pty_rows;
	if (cols == job->pty_cols && rows == job->pty_rows)
		return;

	job->pty_cols = cols;
	job->pty_rows = rows;
	ws.ws_row = (unsigned short)rows;
	ws.ws_col = (unsigned short)cols;
	if (job->pty >= 0)
		(void)ioctl(job->pty, TIOCSWINSZ, &ws);
	fyai_terminal_view_resize(job->view, rows, cols);
	if (job->inline_block) {
		fyai_terminal_view_damage_all(job->view);
		return;
	}
	(void)fyai_ui_surface_resize(job->surface, rows, cols);
	fyai_workpane_grid_resized(job->ctx->workpane, job->surface, rows, cols);
}

static void fyai_agent_focus_changed(void *owner, bool focused)
{
	(void)owner;
	(void)focused;
}

static void fyai_agent_present(void *owner, enum fyai_workpane_present p)
{
	struct fyai_tool_job *job = owner;

	if (!job->surface)
		return;
	job->present = p;
	if (p == FYAI_WORKPANE_PRESENT_HEAD ||
	    p == FYAI_WORKPANE_PRESENT_HIDDEN) {
		(void)fyai_ui_surface_clear(job->surface);
	} else {
		fyai_terminal_view_damage_all(job->view);
		(void)fyai_ui_surface_publish(job->surface, job->view);
	}
	fyai_ui_wake(job->ctx);
}

static const struct fyai_workpane_tile_ops fyai_agent_tile_ops = {
	.apply_grant = fyai_agent_apply_grant,
	.focus_changed = fyai_agent_focus_changed,
	.set_presentation = fyai_agent_present,
	.repaint_head = fyai_agent_head_repaint,
};

/* Draw the screen of an inline sub-agent in its block of the transcript, or
 * with @done present it there with the outcome @ok. */
void fyai_agent_inline_paint(struct fyai_tool_job *job, bool done,
			     bool ok)
{
	struct fyai_buf screen = {0};
	const char *indent;
	char edge[256];
	int first, last, rc;

	if (!job->view)
		return;
	(void)fyai_terminal_view_take_damage(job->view, &first, &last);
	job->inline_sep = fyai_inline_sep(job->ctx);
	indent = markdown_tool_output_indent(job->ctx->cfg);
	if (!done && fyai_ui_inline_focused(job->ctx) == (uintptr_t)job)
		indent = fyai_ui_inline_margin(job->ctx, indent, edge,
					       sizeof(edge));
	rc = fyai_terminal_view_rows_sgr(job->view, indent, false, &screen);
	fyai_error_check(job->ctx, !rc, out,
			 "agent: cannot draw the screen of a sub-agent");
	if (done)
		screen.len = fyai_inline_screen_trim(job->ctx, screen.data,
						     screen.len);
	if (fyai_ui_inline_terminal(job->ctx, (uintptr_t)job,
				    job->title ? job->title : "**agent**",
				    NULL, screen.data, screen.len,
				    job->pty_rows, job->animation_frame, done,
				    ok))
		fyai_warning(job->ctx,
			     "agent: cannot draw a sub-agent in the transcript");
out:
	free(screen.data);
}

/* A sub-agent drawn in the transcript follows the width of the transcript. */
static void fyai_agent_inline_follow(struct fyai_tool_job *job)
{
	int want = fyai_shell_inline_cols(job->ctx);

	if (want != job->pty_cols)
		fyai_agent_apply_grant(job, job->pty_rows, want);
}

void fyai_tools_inline_refence(struct fyai_ctx *ctx)
{
	struct fyai_shell_session *sess;
	struct fyai_tool_job *job;
	int sep;

	if (!ctx)
		return;
	sep = fyai_inline_sep(ctx);
	for (sess = ctx->shell_sessions; sess; sess = sess->next)
		if (sess->inline_block && sess->inline_sep != sep)
			fyai_shell_session_inline_paint(sess, false);
	for (job = ctx->tool_jobs; job; job = job->next)
		if (job->inline_block && job->inline_sep != sep)
			fyai_agent_inline_paint(job, false, true);
}

/* Publish what every live tile has drawn since the last frame. */
void fyai_tool_surfaces_publish(struct fyai_ctx *ctx)
{
	struct fyai_shell_session *sess;
	struct fyai_tool_job *job;

	if (!ctx)
		return;
	for (sess = ctx->shell_sessions; sess; sess = sess->next) {
		if (sess->inline_block) {
			fyai_shell_session_inline_follow(sess);
			if (fyai_terminal_view_dirty(sess->view))
				fyai_shell_session_inline_paint(sess, false);
			continue;
		}
		if (sess->surface && fyai_tile_draws_program(sess->present) &&
		    fyai_ui_surface_publish(sess->surface, sess->view) > 0)
			fyai_ui_wake(ctx);
	}
	for (job = ctx->tool_jobs; job; job = job->next) {
		if (job->inline_block) {
			fyai_agent_inline_follow(job);
			if (fyai_terminal_view_dirty(job->view))
				fyai_agent_inline_paint(job, false, true);
			continue;
		}
		if (job->surface && fyai_tile_draws_program(job->present) &&
		    fyai_ui_surface_publish(job->surface, job->view) > 0)
			fyai_ui_wake(ctx);
	}
}

bool fyai_tool_surfaces_active(const struct fyai_ctx *ctx)
{
	const struct fyai_shell_session *sess;
	const struct fyai_tool_job *job;

	if (!ctx)
		return false;
	for (sess = ctx->shell_sessions; sess; sess = sess->next)
		if (sess->surface)
			return true;
	for (job = ctx->tool_jobs; job; job = job->next)
		if (job->surface)
			return true;
	return false;
}

/* Show what the sub-agent has drawn on its terminal since the last look. */
void fyai_agent_view_refresh(struct fyai_tool_job *job)
{
	if (!job->surface || !job->view)
		return;
	if (fyai_ui_surface_publish(job->surface, job->view) > 0)
		fyai_ui_wake(job->ctx);
}

/*
 * Build the title of a sub-agent tile: the call title, then the branch leaf,
 * the model, and the execution id from the agent registry. Leave out a field
 * that the registry does not have. The caller frees the result.
 */
static char *fyai_agent_head_title(struct fyai_tool_job *job)
{
	const char *base, *end, *leaf, *model, *id;
	const char *leaf_part, *model_part, *text;
	long long execution, started;
	fy_generic call_args, call_name;
	char *title;

	base = job->title ? job->title : "**agent**";
	while (*base == '\n' || *base == '\r' ||
	       *base == ' ' || *base == '\t')
		base++;
	end = base + strlen(base);
	while (end > base && (end[-1] == '\n' || end[-1] == '\r'))
		end--;
	(void)fyai_agents_branch_identity(job->ctx, job->branch, &model,
					  &execution, &started);
	leaf = job->branch ? strstr(job->branch, FYAI_BRANCH_AGENT_PREFIX) : NULL;
	leaf = leaf ? leaf + sizeof(FYAI_BRANCH_AGENT_PREFIX) - 1 : job->branch;
	/* The call title already names the agent. */
	if (!fy_str_empty(leaf) && job->agent && fy_is_valid(job->call)) {
		call_args = fyai_tool_call_args(job->ctx, job->call);
		call_name = fy_get(call_args, "name");
		if (fy_equal(call_name, leaf))
			leaf = NULL;
	}
	id = execution > 0 ? fy_sprintfa(" #%lld", execution) : "";
	leaf_part = fy_str_empty(leaf) ? "" : fy_sprintfa(" `%s`", leaf);
	model_part = fy_str_empty(model) ? "" : fy_sprintfa(" `%s`", model);
	text = fy_sprintfa("%.*s%s%s%s\n", (int)(end - base), base,
			   leaf_part, model_part, id);
	title = strdup(text);
	fyai_error_check(job->ctx, title, err_out,
			 "cannot format the tile title of agent %s",
			 job->branch ? job->branch : "");
	return title;

err_out:
	return NULL;
}

/* Paint the title of a live sub-agent tile with the running mark. */
void fyai_agent_head_paint(struct fyai_tool_job *job)
{
	const char *right, *model;
	char elapsed[24];
	long long execution, started;
	char *title;

	/* The elapsed time of a running agent stands at the right edge. */
	(void)fyai_agents_branch_identity(job->ctx, job->branch, &model,
					  &execution, &started);
	fyai_event_elapsed_format(elapsed, sizeof(elapsed), started);
	right = fy_sprintfa("%s%s", job->overdue ? "overdue " : "",
			    elapsed[0] == ' ' ? elapsed + 1 : elapsed);
	title = fyai_agent_head_title(job);
	(void)fyai_chrome_update(job->ctx, job->surface,
		&(struct fyai_chrome_spec){
			.title = title ? title :
				 job->title ? job->title : "**agent**",
			.right = *right ? right : NULL,
			.mark = FYAI_UI_MARK_RUNNING,
			.frame = job->animation_frame,
		}, NULL);
	free(title);
	fyai_ui_wake(job->ctx);
}

/* Repaint an active agent header after its width changes. */
static void fyai_agent_head_repaint(void *owner)
{
	struct fyai_tool_job *job = owner;
	char *title;

	if (job->surface && !job->done)
		fyai_agent_head_paint(job);
	else if (job->surface && job->btw_panel) {
		title = fyai_agent_head_title(job);
		(void)fyai_chrome_update(job->ctx, job->surface,
			&(struct fyai_chrome_spec){
				.title = title ? title : "**btw**",
				.mark = job->result_ok && !job->failed ?
					FYAI_UI_MARK_OK : FYAI_UI_MARK_FAILED,
			}, NULL);
		free(title);
	}
}

/* Advance the state mark on the title of a live sub-agent terminal. */
static enum fyai_event_action
fyai_agent_view_animate(const struct fyai_event *ev)
{
	struct fyai_tool_job *job = ev->userdata;

	if (job->inline_block && !job->done) {
		job->animation_frame++;
		fyai_agent_inline_paint(job, false, true);
		return FYAIEA_CONTINUE;
	}
	if (!job->surface || job->done)
		return FYAIEA_CONTINUE;
	job->animation_frame++;
	fyai_agent_head_paint(job);
	return FYAIEA_CONTINUE;
}

/* Write the reply of the view to the terminal of the sub-agent. */
void fyai_agent_view_reply(const char *data, size_t len, void *user)
{
	struct fyai_tool_job *job = user;

	fyai_terminal_reply_write(job->pty, data, len);
}

static enum fyai_event_action fyai_agent_pty_read(const struct fyai_event *ev)
{
	struct fyai_tool_job *job = ev->userdata;
	char buf[4096];
	ssize_t n;

	for (;;) {
		n = read(job->pty, buf, sizeof(buf));
		if (n > 0) {
			/* It wrote: whatever it waited for, it has it. */
			job->wants_input = false;
			fyai_terminal_tap(job->ctx, job->view,
					  &job->model_text, job->branch ?
					  job->branch : "agent", buf,
					  (size_t)n);
			(void)fyai_terminal_view_feed(job->view, buf,
						      (size_t)n);
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
			fyai_agent_view_refresh(job);
			return FYAIEA_CONTINUE;
		}
		/* EIO means that the last slave closed and the sub-agent exited. */
		fyai_agent_view_refresh(job);
		if (job->ptysrc) {
			fyai_event_source_remove(job->ptysrc);
			job->ptysrc = NULL;
		}
		return FYAIEA_CONTINUE;
	}
}

/* Whether the queued wait report of the sub-agent on @branch is still live. */
bool fyai_event_agent_live(struct fyai_ctx *ctx, const char *branch)
{
	struct fyai_tool_job *job;

	if (!ctx || fy_str_empty(branch))
		return false;
	for (job = ctx->tool_jobs; job; job = job->next) {
		if (!job->agent || job->done || !job->branch)
			continue;
		if (!strcmp(job->branch, branch))
			return true;
	}
	return false;
}

/* Queue a sub-agent's new input request for the model. */
static enum fyai_event_action fyai_agent_wait_poll(const struct fyai_event *ev)
{
	struct fyai_tool_job *job = ev->userdata;
	const char *who;
	char *prompt = NULL;
	char *text;
	char *owner = NULL;
	bool wants;

	if (job->done || !job->branch)
		return FYAIEA_CONTINUE;
	wants = fyai_process_reads_stdin(job->pid);
	if (wants == job->wants_input)
		return FYAIEA_CONTINUE;
	job->wants_input = wants;
	if (!wants)
		return FYAIEA_CONTINUE;
	if (job->view)
		prompt = fyai_terminal_view_last_line(job->view);
	who = fyai_agent_job_name(job);
	text = prompt && *prompt ?
		strdup(fy_sprintfa("[agent '%s' is waiting for input: %s]",
				   who, prompt)) :
		strdup(fy_sprintfa("[agent '%s' is waiting for input]", who));
	free(prompt);
	if (!text)
		return FYAIEA_CONTINUE;
	/*
	 * Name the branch: the report reaches the model after this poll,
	 * and the sub-agent may have ended by then. A stale report names a
	 * branch with no live owner and is dropped instead of submitted.
	 */
	owner = strdup(job->branch);
	if (!owner) {
		free(text);
		fyai_warning(job->ctx,
			     "the sub-agent '%s' asked for input, which was lost",
			     who);
		return FYAIEA_CONTINUE;
	}
	if (fyai_event_inject_owned(job->ctx, text,
					  FYAI_EVENT_OWNER_AGENT, owner)) {
		fyai_warning(job->ctx,
			     "the sub-agent '%s' asked for input, which was lost",
			     who);
	}
	return FYAIEA_CONTINUE;
}

/* Write the model's answer to a running sub-agent terminal. */
char *fyai_agent_input_tool(struct fyai_ctx *ctx, fy_generic args,
			    bool *okp)
{
	struct fyai_tool_job *job;
	struct fyai_buf in = {};
	fy_generic name_v, input_v;
	const char *name;
	const char *text;
	ssize_t n;
	size_t off;
	int rc;

	*okp = false;
	name_v = fy_get(args, "name", fy_invalid);
	name = fy_castp(&name_v, "");
	job = fyai_agent_job_named(ctx, name);
	if (!job)
		return strdup(fy_sprintfa(
			"tool error: no sub-agent named '%s' is running; one "
			"that has finished is reached with the agent tool",
			name));
	if (job->pty < 0)
		return strdup(fy_sprintfa(
			"tool error: the sub-agent '%s' has no terminal to "
			"write to", name));

	input_v = fy_get(args, "input", fy_invalid);
	text = fy_castp(&input_v, "");
	rc = fyai_buf_append(&in, text);
	fyai_error_check(ctx, !rc, err,
			 "agent: could not retain input for '%s'", name);
	/* Its terminal turns the return into the end of a line for it. */
	if (fy_get(args, "enter", true)) {
		rc = fyai_buf_append(&in, "\r");
		fyai_error_check(ctx, !rc, err,
				 "agent: could not append return for '%s'", name);
	}
	for (off = 0; off < in.len; off += (size_t)n) {
		n = write(job->pty, in.data + off, in.len - off);
		if (n > 0)
			continue;
		if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
			n = 0;
			continue;
		}
		free(in.data);
		return strdup(fy_sprintfa(
			"tool error: could not write to the sub-agent '%s': %s",
			name, strerror(errno)));
	}
	free(in.data);
	/* It read what it was given: the next stop is a new question. */
	job->wants_input = false;
	*okp = true;
	return strdup(fy_sprintfa("[agent '%s': the input was typed]", name));

err:
	free(in.data);
	return NULL;
}

/* Open a display surface for a sub-agent terminal. */
int fyai_agent_view_open(struct fyai_ctx *ctx,
			 struct fyai_tool_job *job)
{
	struct fyai_event_loop *el;
	unsigned int animation_ms;
	int flags, rc;

	animation_ms = 0;
	if (job->pty < 0)
		return 0;

	flags = fcntl(job->pty, F_GETFL, 0);
	if (flags >= 0)
		(void)fcntl(job->pty, F_SETFL, flags | O_NONBLOCK);

	job->view = fyai_terminal_view_create(ctx, job->pty_rows,
					      job->pty_cols, 0);
	if (!job->view)
		return -1;
	if (fyai_terminal_view_set_history(job->view,
					   ctx->cfg->work_history_rows))
		return -1;
	fyai_terminal_view_reply_cb(job->view, fyai_agent_view_reply, job);
	el = fyai_ctx_loop(ctx);
	if (!el || fyai_event_add_fd(el, job->pty, FYAIEV_READ,
				     fyai_agent_pty_read, job, &job->ptysrc))
		return -1;
	/* The same watch a session has, on the terminal of a sub-agent. */
	if (ctx->cfg->shell_input_poll_ms > 0) {
		rc = fyai_event_add_timer(el, ctx->cfg->shell_input_poll_ms,
					   ctx->cfg->shell_input_poll_ms,
					   fyai_agent_wait_poll, job,
					   &job->waiter);
		fyai_error_check(ctx, !rc, err,
				 "agent: could not watch for input requests");
	}

	/* With no display, the screen goes only to the model. */
	if (!fyai_ui_active(ctx))
		return 0;
	/* A delegation of the model draws its screen at the call in the
	 * transcript; a side question keeps its tile. */
	if (!job->btw && fyai_ui_tools_inline(ctx)) {
		job->inline_block = true;
		fyai_inline_animation_arm(ctx, fyai_agent_view_animate, job,
					  &job->animation);
		fyai_terminal_view_damage_all(job->view);
		fyai_ui_wake(ctx);
		return 0;
	}
	job->surface = fyai_ui_surface_open(ctx, job->pty_rows, job->pty_cols);
	if (job->surface) {
		/* A sub-agent keeps the height it opened with, whatever it
		 * has drawn: two lines of output is not a two-row tile. */
		(void)fyai_workpane_register(ctx->workpane, job->surface,
					     FYAI_WORKPANE_TILE_AGENT, job,
					     &fyai_agent_tile_ops,
					     job->pty_rows, 0);
		fyai_workpane_tile_set_ladder(ctx->workpane, job->surface,
					      &fyai_tile_ladder);
		(void)fyai_chrome_update(ctx, job->surface,
			&(struct fyai_chrome_spec){
				.title = job->title ? job->title : "**agent**",
				.mark = FYAI_UI_MARK_RUNNING,
			}, &animation_ms);
		fyai_chrome_body(ctx, job->surface);
		if (animation_ms) {
			rc = fyai_event_add_timer(el, animation_ms, animation_ms,
					  fyai_agent_view_animate, job,
					  &job->animation);
			if (rc)
				fyai_warning(ctx, "agent indicator animation timer "
					     "could not start");
		}
		/* Publish the initial blank screen. */
		fyai_terminal_view_damage_all(job->view);
		fyai_agent_view_refresh(job);
	}
	return 0;

err:
	return -1;
}

/* The sub-agent is done: its last screen is committed, with its outcome. */
void fyai_agent_view_close(struct fyai_tool_job *job, bool ok,
			   const char *cause)
{
	char *title;

	if (job->animation) {
		fyai_event_source_remove(job->animation);
		job->animation = NULL;
	}
	if (job->waiter) {
		fyai_event_source_remove(job->waiter);
		job->waiter = NULL;
	}
	if (job->ptysrc) {
		fyai_event_source_remove(job->ptysrc);
		job->ptysrc = NULL;
	}
	if (job->inline_block) {
		fyai_agent_inline_paint(job, true, ok);
		job->inline_block = false;
		fyai_ui_wake(job->ctx);
	}
	if (job->btw && !job->btw_panel) {
		if (job->surface) {
			fyai_ui_surface_close(job->ctx, job->surface);
			job->surface = NULL;
		}
		if (job->pty >= 0) {
			close(job->pty);
			job->pty = -1;
		}
		if (job->model_text)
			fyai_uimodel_text_close(job->ctx, job->model_text);
		job->model_text = 0;
		fyai_terminal_view_destroy(job->view);
		job->view = NULL;
		return;
	}
	if (job->surface) {
		fyai_agent_view_refresh(job);
		title = fyai_agent_head_title(job);
		(void)fyai_chrome_update(job->ctx, job->surface,
			&(struct fyai_chrome_spec){
				.title = title ? title :
					 job->title ? job->title : "**agent**",
				.cause = cause,
				.mark = ok ? FYAI_UI_MARK_OK :
					     FYAI_UI_MARK_FAILED,
			}, NULL);
		free(title);
		if (!job->btw_panel) {
			fyai_surface_retire_zoom(job->ctx, job->surface);
			fyai_ui_surface_commit(job->ctx, job->surface);
			job->surface = NULL;
		}
		fyai_ui_wake(job->ctx);
	}
	if (job->pty >= 0) {
		close(job->pty);
		job->pty = -1;
	}
	if (!job->btw_panel) {
		if (job->model_text)
			fyai_uimodel_text_close(job->ctx, job->model_text);
		job->model_text = 0;
		fyai_terminal_view_destroy(job->view);
		job->view = NULL;
	}
}

bool fyai_agent_isolated(struct fyai_ctx *ctx, fy_generic args)
{
	fy_generic asked = fy_get(args, "isolated", fy_invalid);
	fy_generic section = fy_get(ctx->cfg->config_doc, "agent", fy_invalid);

	/* The view that an agent or a session runs in is shared by what it starts. */
	if (ctx->cfg->view_project)
		return false;
	if (fy_is_bool(asked))
		return fy_cast(asked, false);
	return fy_equal(fy_get(section, "isolation", "none"), "view");
}

/*
 * The view of a sub-agent is named for it in the namespace of the sub-agents. The
 * name is one component of characters that a command takes without quoting, so
 * the name of an agent cannot leave the namespace.
 */
int fyai_agent_view_begin(struct fyai_ctx *ctx, const char *agent,
			  struct fyai_view_run **run)
{
	char name[FYAI_BRANCH_NAME_MAX + 8];
	size_t i, n;
	int rc;

	if (ctx->cfg->view_project) {
		fyai_error(ctx, "a sub-agent that runs in a view cannot isolate "
			   "its own sub-agents");
		return -1;
	}
	n = snprintf(name, sizeof(name), FYAI_VIEW_AGENT_PREFIX);
	for (i = 0; agent[i] && n + 1 < sizeof(name); i++)
		name[n++] = isalnum((unsigned char)agent[i]) || agent[i] == '_' ||
			    agent[i] == '-' ? agent[i] : '-';
	name[n] = '\0';
	rc = fyai_view_run_begin(ctx, name, run);
	return rc;
}

/* Capture what the isolated sub-agent changed and tell its parent where it is. */
void fyai_agent_view_finish(struct fyai_ctx *ctx, struct fyai_tool_job *job,
			    fy_generic *result)
{
	char *summary = NULL;
	const char *name = fyai_view_run_name(job->view_run);
	int rc;

	rc = fyai_view_run_finish(ctx, job->view_run, &summary);
	if (!fy_is_string(*result)) {
		fyai_view_run_free(job->view_run);
		job->view_run = NULL;
		free(summary);
		return;
	}
	if (rc)
		*result = fy_stringf(ctx->transient_gb, "%s\n\n[The sub-agent ran in the view "
				     "'%s', but its changes could not be captured; see the "
				     "diagnostics.]", fy_castp(result, ""), name);
	else
		*result = fy_stringf(ctx->transient_gb, "%s\n\n[The sub-agent ran in the view "
				     "'%s'. Its changes are not in the project; %s\nReview "
				     "them with the project_view tool, as '%s', or `view diff %s`.]",
				     fy_castp(result, ""), name,
				     summary ? summary : "the change could not be listed",
				     name + sizeof(FYAI_VIEW_AGENT_PREFIX) - 1, name);
	free(summary);
	fyai_view_run_free(job->view_run);
	job->view_run = NULL;
}

void fyai_tools_btw_panels_close(struct fyai_ctx *ctx)
{
	struct fyai_tool_job *job, *next;

	if (!ctx)
		return;
	for (job = ctx->tool_jobs; job; job = next) {
		next = job->next;
		if (job->btw_panel)
			fyai_tool_job_discard(job);
	}
}

bool fyai_tools_btw_dismiss_focused(struct fyai_ctx *ctx)
{
	struct fyai_tool_job *job;
	struct fytim_surface *focused;

	if (!ctx || !ctx->workpane)
		return false;
	focused = fyai_workpane_focused(ctx->workpane);
	for (job = ctx->tool_jobs; job; job = job->next)
		if (job->btw_panel && job->surface == focused) {
			fyai_tool_job_discard(job);
			return true;
		}
	return false;
}

bool fyai_tools_kept_surface(struct fyai_ctx *ctx,
			     const struct fytim_surface *sf)
{
	struct fyai_shell_session *sess;

	if (!ctx || !sf)
		return false;
	for (sess = ctx->shell_sessions; sess; sess = sess->next)
		if (sess->surface == sf)
			return sess->exited && sess->keep_tile;
	return false;
}

bool fyai_tools_btw_surface(struct fyai_ctx *ctx, struct fytim_surface *sf)
{
	struct fyai_tool_job *job;

	if (!ctx || !sf)
		return false;
	for (job = ctx->tool_jobs; job; job = job->next)
		if (job->btw_panel && job->surface == sf)
			return true;
	return false;
}
