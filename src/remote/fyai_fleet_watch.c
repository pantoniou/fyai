/* SPDX-License-Identifier: MIT */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fyai.h"
#include "fyai_app.h"
#include "fyai_buf.h"
#include "fyai_config.h"
#include "fyai_fleet.h"
#include "fyai_fleet_ops.h"
#include "fyai_fleet_watch.h"
#include "fyai_jsonrpc.h"
#include "fyai_markdown.h"
#include "fyai_remote.h"
#include "fyai_uimodel.h"
#include "fyai_wait.h"
#include "fyai_waitq.h"

#define FYAI_MODULE FYAIEM_REMOTE

/* One supervisor connection that stays, to the server of a member. */
struct fyai_fleet_watcher {
	struct fyai_fleet_watcher *next;
	struct fyai_fleet_watch *owner;
	struct jsonrpc_conn *conn;
	int in, out;
	char *name;			/* PROJECT:BRANCH */
	const char *state;		/* the last state that the server said */
	char *last;			/* the first line of the last answer, or NULL */
	char *title;			/* the description of the branch, or NULL */
	bool ended;			/* free it outside its frame */
	bool crashed;			/* the worker went by a crash */
	int restarts;			/* the times that this root started it again */
};

/* The most times that a root starts a member again after a crash. */
#define FLEET_RESTARTS_MAX	2

/* The most characters of an answer that the fleet panel shows. */
#define FLEET_PANEL_LAST	60

void fyai_fleet_watch_init(struct fyai_fleet_watch *fw, struct fyai_loop *loop,
			   struct fyai_waitq *waitq)
{
	memset(fw, 0, sizeof(*fw));
	fw->loop = loop;
	fw->waitq = waitq;
}

/*
 * Show the members on the fleet panel of the page: the member `fleet` of the
 * panels store, one row {name, state, last} for each.
 */
static void fleet_watch_panel(struct fyai_ctx *ctx)
{
	struct fy_generic_builder *gb = fyai_uimodel_begin(ctx);
	struct fyai_fleet_watcher *w;
	fy_generic rows, row;

	if (!gb)
		return;
	rows = fy_seq_empty;
	for (w = ctx->fleet->watchers; w; w = w->next) {
		row = fy_mapping(gb, "name", w->name, "state", w->state);
		if (w->title)
			row = fy_assoc(gb, row, "title", fy_value(gb, w->title));
		if (w->last)
			row = fy_assoc(gb, row, "last", fy_value(gb, w->last));
		rows = fy_append(gb, rows, row);
	}
	/* A null hides the panel (fleet/panel); the write gives 1 on a change. */
	if (!fy_get(fy_get(ctx->cfg->config_doc, "fleet", fy_invalid), "panel",
		    true))
		rows = fy_null;
	if (!fy_is_valid(rows) ||
	    fyai_uimodel_set_member(ctx, FYAI_UI_PANELS, "fleet", rows) < 0)
		fyai_warning(ctx, "fleet: cannot keep the members for the display");
	fyai_uimodel_end(ctx);
}

/* Keep the first line of @text, cut at FLEET_PANEL_LAST characters. */
static void fleet_watch_last(struct fyai_fleet_watcher *w, const char *text)
{
	size_t len = strcspn(text, "\n"), i, chars;

	for (i = chars = 0; i < len; i++)
		if (((unsigned char)text[i] & 0xc0) != 0x80 &&
		    ++chars > FLEET_PANEL_LAST)
			break;
	free(w->last);
	w->last = *text ? strndup(text, i) : NULL;
}

/* The mark of a state: a running member shows @running, the indicator frame. */
static const char *fleet_panel_mark(const char *state, const char *running)
{
	if (!strcmp(state, "running"))
		return running;
	if (!strcmp(state, "waiting"))
		return "\u25c6";
	if (!strcmp(state, "idle"))
		return "\u00b7";
	return "\u2013";
}

char *fyai_fleet_panel_source(fy_generic rows, const char *running, bool *blinks)
{
	struct fyai_buf out = { 0 };
	const char *name, *last, *state, *title;
	fy_generic row, gn, gl, gs, gt;
	char *esc_name, *esc_last, *esc_title;
	int rc = 0;

	if (blinks)
		*blinks = false;
	if (!fy_is_sequence(rows))
		return NULL;
	if (fy_empty(rows))
		return strdup("- no members\n");
	fy_foreach(row, rows) {
		gs = fy_get(row, "state", fy_invalid);
		state = fy_castp(&gs, "?");
		if (blinks && !strcmp(state, "running"))
			*blinks = true;
		gn = fy_get(row, "name", fy_invalid);
		name = fy_castp(&gn, "");
		gl = fy_get(row, "last", fy_invalid);
		last = fy_castp(&gl, "");
		gt = fy_get(row, "title", fy_invalid);
		title = fy_castp(&gt, "");
		esc_name = markdown_ui_escape(name);
		esc_last = *last ? markdown_ui_escape(last) : NULL;
		esc_title = *title ? markdown_ui_escape(title) : NULL;
		if (!esc_name || (*last && !esc_last) || (*title && !esc_title))
			rc = -1;
		if (!rc)
			rc = fyai_buf_append(&out, out.data ? "\n- " : "- ") ||
			     fyai_buf_append(&out, fleet_panel_mark(state, running)) ||
			     fyai_buf_append(&out, " ") ||
			     (esc_title ? fyai_buf_append(&out, esc_title) ||
					  fyai_buf_append(&out, " (") ||
					  fyai_buf_append(&out, esc_name) ||
					  fyai_buf_append(&out, ")") :
					  fyai_buf_append(&out, esc_name)) ||
			     fyai_buf_append(&out, " \u00b7 ") ||
			     fyai_buf_append(&out, state) ||
			     (esc_last && (fyai_buf_append(&out, " \u2014 ") ||
					   fyai_buf_append(&out, esc_last)));
		free(esc_name);
		free(esc_last);
		free(esc_title);
		if (rc)
			break;
	}
	if (rc || fyai_buf_append(&out, "\n")) {
		free(out.data);
		return NULL;
	}
	return out.data;
}

static void fleet_watch_trace_failure(struct fyai_ctx *ctx, const char *name);

/*
 * Read the title of each member, the description of its branch, from the list
 * of the fleet. The list is one short child that the call waits for; a
 * title comes late (the process that writes it runs after a turn), so the
 * end of each turn asks again. A failure is traced and keeps the old titles.
 */
static void fleet_watch_titles(struct fyai_ctx *ctx)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	struct fyai_fleet_watcher *w;
	fy_generic rows, row, title;
	const char *name;

	if (!gb || fyai_fleet_rows_list(ctx, gb, &rows)) {
		fleet_watch_trace_failure(ctx, "the titles");
		return;
	}
	fy_foreach(row, rows) {
		title = fy_get(row, "title", fy_invalid);
		if (!fy_is_string(title))
			continue;
		name = fy_sprintfa("%s:%s", fy_get(row, "project", "."),
				   fy_get(row, "branch", ""));
		for (w = ctx->fleet->watchers; w; w = w->next) {
			if (w->ended || strcmp(w->name, name))
				continue;
			free(w->title);
			w->title = strdup(fy_castp(&title, ""));
		}
	}
}

/* A change of the state of a member, as an event for the model. */
static void fleet_watch_frame(struct jsonrpc_conn *conn, fy_generic frame,
			      void *data)
{
	struct fyai_fleet_watcher *w = data;
	struct fyai_ctx *ctx = fyai_fleet_watch_ctx(w->owner);
	fy_generic state = fy_get(frame, "state", fy_invalid);
	fy_generic last = fy_get(frame, "last", fy_invalid);
	int rc = 0;

	(void)conn;
	if (!fy_equal(fy_get(frame, "type", fy_invalid), "fyai") ||
	    !fy_equal(fy_get(frame, "subtype", fy_invalid), "session_state"))
		return;
	/* The panel shows each state, running too. */
	w->state = fy_equal(state, "running") ? "running" :
		   fy_equal(state, "waiting") ? "waiting" :
		   fy_equal(state, "ended") ? "ended" : "idle";
	if (fy_is_string(last)) {
		fleet_watch_last(w, fy_castp(&last, ""));
		fleet_watch_titles(ctx);
	}
	fleet_watch_panel(ctx);
	/* An idle state with no answer is not news: the answer comes with it. */
	if (fy_equal(state, "idle") && fy_is_string(last))
		rc = fyai_event_injectf(w->owner->waitq, "[session '%s' idle]\n%s",
					w->name, fy_castp(&last, ""));
	else if (fy_equal(state, "waiting"))
		rc = fyai_event_injectf(w->owner->waitq, "[session '%s' waiting]\n"
					"A question of the session waits for an "
					"answer.", w->name);
	else if (fy_equal(state, "ended")) {
		rc = fyai_event_injectf(w->owner->waitq, "[session '%s' ended]",
					w->name);
		w->ended = true;
		w->crashed = fy_get(frame, "crashed", false);
	} else {
		return;
	}
	if (rc)
		fyai_warning(ctx, "fleet: the change of '%s' was lost", w->name);
	/* A wait of the model sees the event at once. */
	fyai_waiters_kick(ctx);
}

static void fleet_watch_closed(void *data)
{
	struct fyai_fleet_watcher *w = data;

	w->ended = true;
}

static void fleet_watcher_free(struct fyai_fleet_watcher *w)
{
	free(w->last);
	free(w->title);
	jsonrpc_conn_destroy(w->conn);
	if (w->in >= 0)
		close(w->in);
	if (w->out >= 0)
		close(w->out);
	free(w->name);
	free(w);
}

int fyai_fleet_watch_add(struct fyai_ctx *ctx, const char *name)
{
	struct fyai_fleet_watch *fw = ctx->fleet;
	struct fyai_fleet_watcher *w;
	const char *branch, *state;
	char *cwd = NULL, *dir = NULL;
	bool image = false;
	int fd = -1, rc = -1;

	for (w = fw->watchers; w; w = w->next)
		if (!w->ended && !strcmp(w->name, name))
			return 0;
	w = calloc(1, sizeof(*w));
	fyai_error_check(ctx, w, out, "fleet: cannot keep the supervision of '%s'", name);
	w->in = w->out = -1;
	w->owner = fw;
	w->name = strdup(name);
	cwd = getcwd(NULL, 0);
	fyai_error_check(ctx, w->name && cwd, out, "fleet: cannot keep the name '%s'",
			 name);
	rc = fyai_fleet_name_split(cwd, name, &dir, &branch);
	fyai_error_check(ctx, !rc && dir, out,
			 "fleet: '%s' is not PROJECT:BRANCH of the fleet", name);
	rc = fyai_remote_supervisor_open(ctx, dir, branch, &fd, &image, &state);
	if (rc)
		goto out;
	rc = -1;
	w->in = fcntl(fd, F_DUPFD_CLOEXEC, 10);
	w->out = fcntl(fd, F_DUPFD_CLOEXEC, 10);
	close(fd);
	fyai_error_check(ctx, w->in >= 0 && w->out >= 0, out,
			 "fleet: cannot keep the connection to '%s': %s", name,
			 strerror(errno));
	w->conn = jsonrpc_conn_stdio(ctx, w->out, w->in, 0, "fleet supervisor", NULL);
	fyai_error_check(ctx, w->conn, out, "fleet: cannot open the connection to '%s'",
			 name);
	if (image)
		(void)jsonrpc_conn_set_image(w->conn);
	jsonrpc_conn_on_close(w->conn, fleet_watch_closed, w);
	rc = jsonrpc_conn_frames(w->conn, fleet_watch_frame, w);
	fyai_error_check(ctx, !rc, out, "fleet: cannot read the connection to '%s'",
			 name);
	w->state = state;
	w->next = fw->watchers;
	fw->watchers = w;
	w = NULL;
	fleet_watch_titles(ctx);
	fleet_watch_panel(ctx);
	/* A member that waits on a question now says so now. */
	if (!strcmp(state, "waiting") &&
	    !fyai_event_injectf(fw->waitq, "[session '%s' waiting]\nA question of "
				"the session waits for an answer.", name))
		fyai_waiters_kick(ctx);
	rc = 0;
out:
	if (w)
		fleet_watcher_free(w);
	free(dir);
	free(cwd);
	return rc ? -1 : 0;
}

/* The trace says why a member was not taken up; the session goes on. */
static void fleet_watch_trace_failure(struct fyai_ctx *ctx, const char *name)
{
	char *why = fyai_diag_string(fyai_ctx_diag(ctx));

	fyai_diag_reset(fyai_ctx_diag(ctx));
	fyai_diag_tracef("fleet", "'%s' not taken up: %s", name,
			 why && *why ? why : "unknown");
	free(why);
}

/* Take up the members of the open tasks. */
static void fleet_watch_restore_open(struct fyai_ctx *ctx)
{
	struct fy_generic_builder *gb;
	fy_generic tasks, rows, row, key, task, state;
	const char *name;

	tasks = fyai_fleet_tasks_open(ctx);
	if (fy_empty(tasks))
		return;
	gb = fyai_ctx_transient_gb(ctx);
	if (fyai_fleet_rows_list(ctx, gb, &rows)) {
		fleet_watch_trace_failure(ctx, "the fleet");
		return;
	}
	fy_foreach_key_value(key, task, tasks) {
		(void)key;
		name = fy_gb_intern_string(gb, fy_sprintfa("%s:%s",
			fy_get(task, "project", "."), fy_get(task, "branch", "")));
		state = fy_invalid;
		fy_foreach(row, rows)
			if (!strcmp(fy_sprintfa("%s:%s", fy_get(row, "project", "."),
						fy_get(row, "branch", "")), name))
				state = fy_get(row, "state", fy_invalid);
		/* A member lost with work in flight starts again. */
		if (fy_equal(state, "lost")) {
			if (fyai_fleet_revive(ctx, name)) {
				fleet_watch_trace_failure(ctx, name);
				continue;
			}
			(void)fyai_event_injectf(ctx->waitq, "[session '%s' restarted: "
				"its process was lost; it goes on from its last stored "
				"step]", name);
			state = fy_string("running");
		}
		if (fy_any_equal(state, "idle", "running", "waiting") &&
		    fyai_fleet_watch_add(ctx, name))
			fleet_watch_trace_failure(ctx, name);
	}
}

void fyai_fleet_watch_restore(struct fyai_ctx *ctx)
{
	if (ctx->fleet->restored || !fyai_fleet_is_root(ctx))
		return;
	ctx->fleet->restored = true;
	fleet_watch_restore_open(ctx);
	/* The panel shows from the start, empty when no member runs. */
	fleet_watch_panel(ctx);
	/* A root that was lost left queued tasks: they start where there is room. */
	(void)fyai_fleet_dequeue(ctx);
}

int fyai_fleet_watch_panel_set(struct fyai_ctx *ctx, bool on)
{
	if (fyai_config_session_set(ctx, "fleet/panel", on ? fy_true : fy_false))
		return -1;
	if (fyai_fleet_is_root(ctx))
		fleet_watch_panel(ctx);
	return 0;
}

size_t fyai_fleet_watch_live(struct fyai_ctx *ctx)
{
	struct fyai_fleet_watcher *w;
	size_t n = 0;

	for (w = ctx->fleet->watchers; w; w = w->next)
		if (!w->ended)
			n++;
	return n;
}

/* Whether a task of this root for the member @name is open. */
static bool fleet_task_open(struct fyai_ctx *ctx, const char *name)
{
	fy_generic tasks = fyai_fleet_tasks_open(ctx), key, task;

	fy_foreach_key_value(key, task, tasks) {
		(void)key;
		if (!strcmp(fy_sprintfa("%s:%s", fy_get(task, "project", "."),
					fy_get(task, "branch", "")), name))
			return true;
	}
	return false;
}

/*
 * Start the member @name again after a crash of its worker, as a root that
 * starts takes up a lost member, and supervise it. The count of the starts
 * goes on in the new supervisor.
 */
static void fleet_watch_restart(struct fyai_ctx *ctx, const char *name,
				int restarts)
{
	if (fyai_fleet_revive(ctx, name) || fyai_fleet_watch_add(ctx, name)) {
		fleet_watch_trace_failure(ctx, name);
		return;
	}
	if (ctx->fleet->watchers && !strcmp(ctx->fleet->watchers->name, name))
		ctx->fleet->watchers->restarts = restarts + 1;
	if (!fyai_event_injectf(ctx->fleet->waitq, "[session '%s' restarted: its "
				"process crashed; it goes on from its last stored "
				"step]", name))
		fyai_waiters_kick(ctx);
}

void fyai_fleet_watch_reap(struct fyai_ctx *ctx)
{
	struct fyai_fleet_watcher **link, *w;
	bool changed = false;
	char *again;
	int restarts;

	for (;;) {
		again = NULL;
		restarts = 0;
		link = &ctx->fleet->watchers;
		while ((w = *link)) {
			if (!w->ended) {
				link = &w->next;
				continue;
			}
			*link = w->next;
			changed = true;
			/* One member at a time: a start takes the list again. */
			if (w->crashed && w->restarts < FLEET_RESTARTS_MAX &&
			    fleet_task_open(ctx, w->name)) {
				again = strdup(w->name);
				restarts = w->restarts;
			}
			fleet_watcher_free(w);
			if (again)
				break;
		}
		if (!again)
			break;
		fleet_watch_restart(ctx, again, restarts);
		free(again);
	}
	/* A member that ended frees a place for a queued task. */
	if (changed && fyai_fleet_dequeue(ctx))
		changed = true;
	if (changed)
		fleet_watch_panel(ctx);
}

void fyai_fleet_watch_cleanup(struct fyai_fleet_watch *fw)
{
	struct fyai_fleet_watcher *w;

	while ((w = fw->watchers)) {
		fw->watchers = w->next;
		fleet_watcher_free(w);
	}
}

void fyai_fleet_watch_abandon(struct fyai_fleet_watch *fw)
{
	struct fyai_fleet_watcher *w;

	/* The connections are the parent's: close the descriptors of this
	 * child, and forget the sources on the shared loop. */
	while ((w = fw->watchers)) {
		fw->watchers = w->next;
		if (w->in >= 0)
			close(w->in);
		if (w->out >= 0)
			close(w->out);
		free(w->last);
		free(w->name);
		free(w);
	}
	fw->restored = true;
}
