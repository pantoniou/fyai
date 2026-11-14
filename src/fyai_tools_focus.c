/*
 * fyai_tools_focus.c - focus, zoom and keys of the tiles and blocks of the work
 * pane, and the control of a session by name
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

/* Find the live session or sub-agent that owns @sf. */
static void fyai_tile_owner(struct fyai_ctx *ctx, struct fytim_surface *sf,
			    struct fyai_shell_session **sessp,
			    struct fyai_tool_job **jobp)
{
	enum fyai_workpane_tile_kind kind = FYAI_WORKPANE_TILE_TEXT;
	void *owner;

	*sessp = NULL;
	*jobp = NULL;
	owner = fyai_workpane_tile_owner(ctx ? ctx->front->workpane : NULL, sf, &kind);
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
	if (fyai_browser_surface(ctx, fyai_workpane_focused(ctx->front->workpane))) {
		(void)fyai_browser_keys(ctx, data, len);
		return;
	}
	/* A tile of a remote session: its program is in the worker. */
	if (fyai_uimodel_keys(ctx, fyai_workpane_focused(ctx->front->workpane), data,
			      len))
		return;
	fyai_tile_owner(ctx, fyai_workpane_focused(ctx->front->workpane), &sess,
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
		for (job = ctx->jobs->tools; job; job = job->next)
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
bool fyai_tools_focus(struct fyai_ctx *ctx, struct fytim_surface *sf)
{
	if (!ctx || !sf)
		return false;
	fyai_workpane_set_keys_router(ctx->front->workpane, fyai_tools_zoom_keys, ctx);
	fyai_workpane_set_focus(ctx->front->workpane, sf);
	return fyai_workpane_focused(ctx->front->workpane) == sf;
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
	for (sess = ctx->jobs->sessions; sess && !sf; sess = sess->next) {
		if (!sess->surface || sess->exited)
			continue;
		if (!name || !*name || (sess->name && !strcmp(sess->name, name))) {
			sf = sess->surface;
			what = sess->name;
			zoom_sess = sess;
		}
	}
	for (job = ctx->jobs->tools; job && !sf; job = job->next) {
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
	fyai_workpane_clear_focus(ctx->front->workpane);
	fyai_workpane_clear_zoom(ctx->front->workpane);
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
	for (sess = ctx->jobs->sessions; sess; sess = sess->next)
		if (sess->view &&
		    !fyai_terminal_view_set_history(sess->view, rows) &&
		    sess->surface)
			(void)fyai_ui_surface_publish(sess->surface, sess->view);
	for (job = ctx->jobs->tools; job; job = job->next)
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
	n = fyai_workpane_screen_order(ctx->front->workpane, tiles,
				       FYAI_WORKPANE_TILES_MAX);
	for (i = 0; i < n && tiles[i] != sf; i++)
		;
	if (i == n)
		return false;
	if (fyai_workpane_focused(ctx->front->workpane) == sf)
		return true;
	return fyai_tools_focus(ctx, sf);
}

void fyai_tools_focus_prompt(struct fyai_ctx *ctx)
{
	/* Preserve zoom and attachment state while returning focus. */
	if (ctx)
		fyai_workpane_clear_focus(ctx->front->workpane);
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

	for (sess = ctx->jobs->sessions; sess; sess = sess->next) {
		if (!sess->inline_block || sess->exited || n >= max)
			continue;
		out[n].key = (uintptr_t)sess;
		out[n].sf = NULL;
		n++;
	}
	for (job = ctx->jobs->tools; job; job = job->next) {
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
	ntiles = fyai_workpane_screen_order(ctx->front->workpane, tiles,
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
	for (sess = ctx->jobs->sessions; sess; sess = sess->next)
		if ((uintptr_t)sess == key && sess->inline_block) {
			*sessp = sess;
			return;
		}
	for (job = ctx->jobs->tools; job; job = job->next)
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
	else if (fyai_agents_surface(ctx, fyai_workpane_focused(ctx->front->workpane)))
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
	fyai_workpane_set_keys_router(ctx->front->workpane, fyai_tools_zoom_keys, ctx);
	if (fyai_workpane_zoomed(ctx->front->workpane))
		return fyai_workpane_focus_next(ctx->front->workpane);
	cur = fyai_ui_inline_focused(ctx);
	focused = fyai_workpane_focused(ctx->front->workpane);
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
		fyai_workpane_clear_focus(ctx->front->workpane);
		return true;
	}
	if (targets[i].key)
		return fyai_inline_focus(ctx, targets[i].key);
	(void)fyai_inline_focus(ctx, 0);
	return fyai_tools_focus(ctx, targets[i].sf);
}
