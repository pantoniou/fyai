/* SPDX-License-Identifier: MIT */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai.h"
#include "fyai_branch.h"
#include "fyai_workpane.h"
#include "fyai_terminal_view.h"
#include "fyai_tools.h"
#include "fyai_browser.h"
#include "fyai_chrome.h"
#include "fyai_cmd.h"
#include "fyai_config.h"
#include "fyai_diag.h"
#include "fyai_display.h"
#include "fyai_scratch.h"
#include "fyai_event.h"
#include "fyai_flow.h"
#include "fyai_jsonrpc.h"
#include "fyai_markdown.h"
#include "fyai_remote.h"
#include "fyai_session.h"
#include "fyai_sink.h"
#include "fyai_ui.h"
#include "fyai_uidraw.h"
#include "fyai_uilog.h"
#include "fyai_storage.h"
#include "fyai_uimodel.h"
#include "utils.h"

#define FYAI_MODULE FYAIEM_REMOTE

/* The stored turns that a client shows when it attaches. */
#define REMOTE_UI_HISTORY	64
/* The dropped turns that the replica tolerates beyond the turns it holds. */
#define REMOTE_UI_GARBAGE_MIN	64
#define REMOTE_UI_TURN_ID	24
/* The time the worker has to answer the initialize. */
#define REMOTE_UI_INIT_MS	10000

/*
 * A terminal of the worker, replayed into a view of this process from the
 * text of its bytes. A terminal session is a mirror session of this process
 * (fyai_tools_mirror_open()), which has the tile or the block of a local
 * session; the screen of a sub-agent is shown in a tile of its own here.
 */
struct remote_ui_term {
	struct remote_ui_term *next;
	const char *name;
	long long source;
	size_t shown;
	int rows, cols;
	bool mirror;			/* a mirror session, found by its name */
	bool agent;			/* the terminal of a sub-agent */
	bool exited;
	bool dismissed;			/* closed here; the worker drops it */
	bool inline_block;		/* drawn in the transcript, no tile */
	bool btw;			/* keep the side-question tile */
	char *title;			/* the title that the worker gave */
	unsigned int frame;		/* the step of the running mark */
	struct fyai_terminal_view *view;
	struct fytim_surface *surface;
};

/*
 * The full-screen client of a remote session. The model of this process is a
 * mirror of the model of the worker: the frames of the worker write it, and
 * the renderer of this process draws from it as it draws a local session.
 */
/* A command sent to the worker, whose output the transcript may take. */
struct remote_ui_command {
	struct remote_ui_command *next;
	char id[48];			/* the request id */
	char *line;
};

#define REMOTE_UI_COMMANDS_MAX	32

struct fyai_remote_ui {
	struct fyai_ctx *ctx;
	struct jsonrpc_conn *conn;
	/* The frames that arrived, served at the next step: a frame callback
	 * does not draw. */
	struct fy_generic_builder *inbox_gb;
	fy_generic inbox;
	/* The replica of the stored turns the transcript view shows: the turns
	 * the worker sent, oldest first, linked by `previous` in the builder
	 * @history_gb, each with the id the worker gave it. */
	struct fy_generic_builder *history_gb;
	struct remote_ui_turn {
		char id[REMOTE_UI_TURN_ID];
		fy_generic turn;
	} *turns;
	size_t nturns, turns_cap;
	/* The turns that the builder still holds although the replica dropped
	 * them: a builder gives nothing back until it is made again. */
	size_t garbage;
	const char *epoch;		/* the incarnation of the worker */
	struct fyai_event_source *deadline;
	struct fyai_event_source *animation;	/* agent animation */
	unsigned long long requests;
	long long head;			/* the time of the stored head shown */
	struct remote_ui_term *terms;
	/* The configuration of this invocation, which the state of the
	 * worker completes. */
	fy_generic base;
	/* The frames that wait for the display, in order: it opens with the
	 * first stored turns, as a local session opens on its conversation. */
	struct fy_generic_builder *held_gb;
	fy_generic held;
	bool opened;			/* the display is open */
	bool terms_seen;		/* initial terminal list received */
	size_t syncs;			/* the syncs in flight */
	bool idle_held;			/* keep UI busy until turns arrive */
	struct remote_ui_command *commands;	/*
						 * pending output, oldest first
						 */
	bool restarted;			/* recap the new worker's turns */
	/* The lines typed during a turn stay in the queue of the display,
	 * which shows them as pending. The worker has the first @queued of
	 * them and says how many it took; the client drops those. */
	size_t queued;
	long long taken;
	bool initialized, busy, failed;
	bool input;			/* worker command reading stdin lines */
	bool watch;			/* reads a session that another client drives */
	/* The view of an agent that the worker shows here: its document, which
	 * the client draws at its own size and scrolls, and the draft that the
	 * prompt held while the keys are the agent's. */
	struct fytim_surface *av_surface;
	char *av_md, *av_draft;
	size_t av_offset;
	bool av_attached;
	bool picker, picker_all;	/* show resume picker on open */
	bool released;			/* worker handed off to a server */
	char *left;			/* handed-off branch for exit notice */
	bool left_parked;		/* server holds worker idle */
	char *error;			/* the cause of the last failed turn */
	/* The queries that the renderer asked: sent in order, a few at a time,
	 * and answered by the number of their request. */
	struct remote_ui_query *queries, **query_tail;
	struct fy_generic_builder *query_gb;	/* the requests that wait */
	unsigned long long query_ids;
	unsigned query_inflight;
};

/* The queries of the renderer that are in flight at the same time. */
#define REMOTE_UI_QUERIES_MAX	8

struct remote_ui_query {
	struct remote_ui_query *next;
	unsigned long long id;		/* what the renderer holds */
	unsigned long long seq;		/* wire ID, or 0 if unsent */
	fy_generic request;		/* in the builder of the view */
	const char *subtype;		/* of the request: a literal */
	fyai_uimodel_answer_fn fn;	/* NULL once the renderer cancelled */
	void *arg;
};

/* The context that this object stands on. */
static inline struct fyai_ctx *fyai_remote_ui_ctx(const struct fyai_remote_ui *o)
{
	return o->ctx;
}

static int remote_ui_sync(struct fyai_remote_ui *view);

/* Send @subtype with the members of @fields as a request to the worker. */
static int remote_ui_request(struct fyai_remote_ui *view, const char *subtype,
			     fy_generic fields)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(fyai_remote_ui_ctx(view));
	const char *id;

	/* A watcher asks for the model and the turns, which change nothing. */
	if (view->watch && strcmp(subtype, "initialize") &&
	    strcmp(subtype, "fyai_sync") && strcmp(subtype, "fyai_query"))
		return 0;
	/* fy_sprintfa() reads its arguments twice: no side effect in them. */
	view->requests++;
	id = fy_gb_intern_string(gb, fy_sprintfa("ui-%s-%llu", subtype,
						 view->requests));
	return jsonrpc_conn_send_frame(view->conn, fy_mapping(gb,
		"type", "control_request", "request_id", id,
		"request", fy_assoc(gb, fields, "subtype", subtype)));
}

/* The initialize asks for the model, and for the markers of a recording. */
static fy_generic remote_ui_init_fields(struct fyai_ctx *ctx)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	bool cast = ctx->cfg->cast_path && *ctx->cfg->cast_path;
	bool trace = cast && !ctx->cfg->cast_compat;

	return fy_mapping(gb, "fyai", fy_mapping(gb, "model", 1LL,
				"cast", cast,
				"cast_trace", trace));
}

static int remote_ui_intent(void *data, const char *subtype,
			    fy_generic request)
{
	return remote_ui_request(data, subtype, request);
}

/* The worker owns the size of its terminal: a grant changes nothing. */
static void remote_ui_term_grant(void *owner, int rows, int cols)
{
	(void)owner;
	(void)rows;
	(void)cols;
}

static char *remote_ui_term_last_line(void *owner)
{
	struct remote_ui_term *t = owner;

	return fyai_terminal_view_last_line(t->view);
}

static const struct fyai_workpane_tile_ops remote_ui_term_ops = {
	.apply_grant = remote_ui_term_grant,
	.last_line = remote_ui_term_last_line,
};

static void remote_ui_term_free(struct fyai_ctx *ctx, struct remote_ui_term *t)
{
	if (t->mirror)
		fyai_tools_mirror_drop(fyai_shell_session_find(ctx, t->name));
	if (t->inline_block && !t->exited)
		fyai_ui_inline_drop(ctx, (uintptr_t)t);
	if (t->surface) {
		fyai_workpane_unregister(ctx->screen->workpane, t->surface);
		fyai_ui_surface_close(ctx, t->surface);
	}
	fyai_terminal_view_destroy(t->view);
	free(t->title);
	free(t);
}

/* Draw the screen of an agent in its block of the transcript. */
static void remote_ui_term_paint(struct fyai_remote_ui *view,
				 struct remote_ui_term *t, bool done, bool ok)
{
	struct fyai_ctx *ctx = fyai_remote_ui_ctx(view);

	if (fyai_tools_inline_view(ctx, (uintptr_t)t,
				   t->title ? t->title : "**agent**",
				   t->view, t->rows, t->frame, done, ok))
		fyai_warning(ctx, "cannot draw the agent %s in the transcript",
			     t->name);
}

/* Replay the bytes of the terminal @t that its view has not had. */
static int remote_ui_term_feed(struct fyai_remote_ui *view,
			       struct remote_ui_term *t)
{
	struct fyai_ctx *ctx = fyai_remote_ui_ctx(view);
	const char *text;
	size_t len;

	text = fyai_uimodel_text(ctx, t->source, &len);
	if (len <= t->shown)
		return 0;
	if (t->mirror) {
		fyai_tools_mirror_feed(fyai_shell_session_find(ctx, t->name),
				       text + t->shown, len - t->shown);
		t->shown = len;
		return 0;
	}
	(void)fyai_terminal_view_feed(t->view, text + t->shown,
				      len - t->shown);
	t->shown = len;
	if (t->inline_block)
		remote_ui_term_paint(view, t, false, false);
	if (t->surface)
		(void)fyai_ui_surface_publish(t->surface, t->view);
	fyai_ui_wake(ctx);
	return 0;
}

/* Open the view and the tile of @t at the size the worker gives. */
/* The title that the worker gave to an agent tile, or NULL. */
static const char *remote_ui_term_title(const struct remote_ui_term *t,
					fy_generic item)
{
	const char *title = fy_get(item, "title", "");

	return t->agent && *title ? title : NULL;
}

static enum fyai_event_action remote_ui_animate(const struct fyai_event *ev);

/* Draw the running header of an agent tile, and arm the mark to turn. */
static void remote_ui_term_run(struct fyai_remote_ui *view,
			       struct remote_ui_term *t, const char *title)
{
	struct fyai_ctx *ctx = fyai_remote_ui_ctx(view);
	unsigned int ms = 0;
	char *mark;

	if (t->inline_block) {
		remote_ui_term_paint(view, t, false, false);
		if (view->animation)
			return;
		mark = fyai_ui_indicator(ctx, FYAI_UI_MARK_RUNNING, 0, &ms);
		free(mark);
	} else {
		(void)fyai_chrome_update(ctx, t->surface, &(struct fyai_chrome_spec){
			.title = title,
			.mark = FYAI_UI_MARK_RUNNING,
			.frame = t->frame,
		}, &ms);
	}
	if (ms && !view->animation)
		(void)fyai_event_add_timer(fyai_ctx_loop(ctx), ms, ms,
					   remote_ui_animate, view,
					   &view->animation);
}

static enum fyai_event_action remote_ui_animate(const struct fyai_event *ev)
{
	struct fyai_remote_ui *view = ev->userdata;
	struct remote_ui_term *t;
	bool running = false;

	for (t = view->terms; t; t = t->next) {
		if (!t->agent || t->exited || t->dismissed ||
		    (!t->surface && !t->inline_block) || !t->title)
			continue;
		running = true;
		t->frame++;
		remote_ui_term_run(view, t, t->title);
	}
	if (!running) {
		fyai_event_source_remove(view->animation);
		view->animation = NULL;
		return FYAIEA_CONTINUE;
	}
	fyai_ui_wake(fyai_remote_ui_ctx(view));
	return FYAIEA_CONTINUE;
}

static int remote_ui_term_open(struct fyai_remote_ui *view,
			       struct remote_ui_term *t, fy_generic item)
{
	struct fyai_ctx *ctx = fyai_remote_ui_ctx(view);
	const char *title;
	int rc;

	t->view = fyai_terminal_view_create(ctx, t->rows, t->cols, 0);
	fyai_error_check(ctx, t->view, err, "cannot replay the terminal %s",
			 t->name);
	t->inline_block = t->agent && !t->btw && fyai_ui_tools_inline(ctx);
	if (t->inline_block) {
		title = remote_ui_term_title(t, item) ? : "**agent**";
		t->title = strdup(title);
		remote_ui_term_run(view, t, t->title);
		return remote_ui_term_feed(view, t);
	}
	t->surface = fyai_ui_surface_open(ctx, t->rows, t->cols);
	fyai_error_check(ctx, t->surface, err, "cannot show the terminal %s",
			 t->name);
	fyai_chrome_body(ctx, t->surface);
	rc = fyai_workpane_register(ctx->screen->workpane, t->surface,
				    FYAI_WORKPANE_TILE_SHELL, t,
				    &remote_ui_term_ops, t->rows, 0);
	fyai_error_check(ctx, !rc, err, "cannot place the terminal %s",
			 t->name);
	/* The terminal of a sub-agent of the worker is listed with agents. */
	fyai_workpane_tile_set_agent(ctx->screen->workpane, t->surface,
				     t->agent);
	title = remote_ui_term_title(t, item) ? :
		fy_sprintfa("**%s** %s", t->agent ? "agent" : "shell", t->name);
	if (t->agent) {
		t->title = strdup(title);
		remote_ui_term_run(view, t, title);
	} else {
		(void)fyai_chrome_update(ctx, t->surface,
			&(struct fyai_chrome_spec){
				.title = title,
				.mark = FYAI_UI_MARK_RUNNING,
			}, NULL);
	}
	return remote_ui_term_feed(view, t);
err:
	return -1;
}

static void remote_ui_mirror_input(void *data, const char *name,
				   const char *bytes, size_t len)
{
	struct fyai_remote_ui *view = data;

	if (remote_ui_request(view, "fyai_terminal_input", fy_mapping(
			"name", name, "data", fy_string_size(bytes, len))))
		view->failed = true;
}

static void remote_ui_mirror_resize(void *data, const char *name, int rows,
				    int cols)
{
	struct fyai_remote_ui *view = data;

	if (remote_ui_request(view, "fyai_terminal_size", fy_mapping(
			"name", name, "rows", (long long)rows,
			"cols", (long long)cols)))
		view->failed = true;
}

static void remote_ui_mirror_close(void *data, const char *name, bool force)
{
	struct fyai_remote_ui *view = data;

	if (remote_ui_request(view, "fyai_terminal_close", fy_mapping(
			"name", name, "force", force)))
		view->failed = true;
}

static void remote_ui_mirror_dismiss(void *data, const char *name)
{
	struct fyai_remote_ui *view = data;
	struct remote_ui_term *t;

	/* The session goes here now; the item goes when the worker drops it. */
	for (t = view->terms; t && strcmp(t->name, name); t = t->next)
		;
	if (t)
		t->dismissed = true;
	if (remote_ui_request(view, "fyai_terminal_dismiss", fy_mapping(
			"name", name)))
		view->failed = true;
}

static const struct fyai_tools_remote remote_ui_mirror_ops = {
	.input = remote_ui_mirror_input,
	.resize = remote_ui_mirror_resize,
	.close = remote_ui_mirror_close,
	.dismiss = remote_ui_mirror_dismiss,
};

/* The key that closes the tile of a program that ended, at the start of
 * @bytes: q, or an Escape that does not start a sequence. */
static bool remote_ui_term_closes(const char *bytes, size_t len)
{
	if (bytes[0] == 'q')
		return true;
	return bytes[0] == FYAI_KEY_ESC &&
	       (len == 1 || (bytes[1] != '[' && bytes[1] != 'O'));
}

/* Draw the document of the view of an agent into its tile. */
static void remote_ui_av_paint(struct fyai_remote_ui *view)
{
	struct fyai_ctx *ctx = fyai_remote_ui_ctx(view);

	if (view->av_surface && view->av_md &&
	    fyai_sink_page(ctx->out->sink, view->av_surface, view->av_md,
			   view->av_offset))
		fyai_warning(ctx, "the agent view did not render");
}

static void remote_ui_av_grant(void *owner, int rows, int cols)
{
	struct fyai_remote_ui *view = owner;
	struct fyai_ctx *ctx = fyai_remote_ui_ctx(view);

	if (fyai_ui_surface_resize(view->av_surface, rows, cols))
		fyai_warning(ctx, "the agent view did not take its grant");
	fyai_workpane_grid_resized(ctx->screen->workpane, view->av_surface,
				   rows, cols);
	remote_ui_av_paint(view);
}

static const struct fyai_workpane_tile_ops remote_ui_av_ops = {
	.apply_grant = remote_ui_av_grant,
};

/* Take the view of an agent away. */
static void remote_ui_av_close(struct fyai_remote_ui *view)
{
	struct fyai_ctx *ctx = fyai_remote_ui_ctx(view);

	if (view->av_surface) {
		fyai_workpane_unregister(ctx->screen->workpane, view->av_surface);
		fyai_ui_surface_close(ctx, view->av_surface);
		view->av_surface = NULL;
	}
	if (view->av_attached && view->av_draft)
		fyai_ui_input_set(ctx, view->av_draft);
	free(view->av_draft);
	view->av_draft = NULL;
	free(view->av_md);
	view->av_md = NULL;
	view->av_attached = false;
	view->av_offset = 0;
	fyai_ui_wake(ctx);
}

/* The member "agent_view" of the work store: show, change or end the view. */
static int remote_ui_av(struct fyai_remote_ui *view, fy_generic av)
{
	struct fyai_ctx *ctx = fyai_remote_ui_ctx(view);
	bool attached;
	int rc;

	if (!fy_is_mapping(av)) {
		if (view->av_surface || view->av_md)
			remote_ui_av_close(view);
		return 0;
	}
	attached = fy_get(av, "attached", false);
	if (!view->av_surface) {
		view->av_surface = fyai_ui_surface_open(ctx, 16, 80);
		fyai_error_check(ctx, view->av_surface, err,
				 "cannot open the agent view");
		rc = fyai_workpane_register(ctx->screen->workpane, view->av_surface,
				FYAI_WORKPANE_TILE_AGENT_VIEW, view,
				&remote_ui_av_ops, 16, 0);
		fyai_error_check(ctx, !rc, err,
				 "cannot place the agent view in the work pane");
		(void)fyai_ui_surface_zoom(ctx, view->av_surface);
		view->av_attached = attached;
		if (attached) {
			view->av_draft = fyai_ui_input_copy(ctx);
			fyai_ui_input_set(ctx, "");
		} else {
			(void)fyai_tools_focus_next(ctx);
			fyai_workpane_set_focus(ctx->screen->workpane,
						view->av_surface);
		}
	}
	free(view->av_md);
	view->av_md = strdup(fy_get(av, "md", ""));
	fyai_error_check(ctx, view->av_md, err, "cannot keep the agent view");
	(void)fyai_ui_surface_set_title(view->av_surface,
					fy_get(av, "branch", ""),
					fy_get(av, "state", ""));
	remote_ui_av_paint(view);
	fyai_ui_wake(ctx);
	return 0;
err:
	return -1;
}

/*
 * Close the tile of @t, whose program ended. The worker drops its session
 * later; a side question has none to tell. @key is what the user typed for
 * the worker of another terminal.
 */
static void remote_ui_term_dismiss(struct fyai_remote_ui *view,
				   struct remote_ui_term *t, const char *key,
				   size_t keylen)
{
	struct fyai_ctx *ctx = fyai_remote_ui_ctx(view);

	if (!t->btw &&
	    remote_ui_request(view, "fyai_terminal_input", fy_mapping(
			"name", t->name, "data", fy_string_size(key, keylen))))
		view->failed = true;
	t->dismissed = true;
	fyai_tools_unzoom(ctx);
	fyai_workpane_unregister(ctx->screen->workpane, t->surface);
	fyai_ui_surface_close(ctx, t->surface);
	t->surface = NULL;
	fyai_ui_wake(ctx);
}

/*
 * The keys of the tile of a terminal go to its program in the worker. The
 * tile of a program that ended goes here at once when a key closes it, and
 * the keys after that key go to the prompt: the worker drops its session
 * later, and a key typed meanwhile must not go to a program that is gone.
 */
static bool remote_ui_term_keys(void *data, struct fytim_surface *sf,
				const char *bytes, size_t len)
{
	struct fyai_remote_ui *view = data;
	struct fyai_ctx *ctx = fyai_remote_ui_ctx(view);
	struct remote_ui_term *t;
	size_t keylen;

	size_t i;

	if (view->av_surface && view->av_surface == sf) {
		for (i = 0; i < len; i++) {
			if (bytes[i] == 'j' || bytes[i] == 'k') {
				if (bytes[i] == 'j')
					view->av_offset++;
				else if (view->av_offset)
					view->av_offset--;
				remote_ui_av_paint(view);
				fyai_ui_wake(ctx);
				continue;
			}
			/* Escape or ^C leaves the view: the worker detaches. */
			if (bytes[i] == FYAI_KEY_ESC || bytes[i] == FYAI_KEY_INTR) {
				if (remote_ui_request(view, "fyai_command",
						fy_mapping("line",
							   "/branch detach")))
					view->failed = true;
				break;
			}
		}
		return true;
	}
	for (t = view->terms; t && t->surface != sf; t = t->next)
		;
	if (!t || !len)
		return false;
	if (t->exited) {
		keylen = len > 1 ? 1 : len;
		/* A program that ended reads nothing more. */
		if (!remote_ui_term_closes(bytes, len))
			return true;
		remote_ui_term_dismiss(view, t, bytes, keylen);
		if (len > 1 && fyai_ui_keys_return(ctx, bytes + 1, len - 1))
			fyai_warning(ctx, "the keys after the close of the "
				     "terminal %s are lost", t->name);
		return true;
	}
	if (remote_ui_request(view, "fyai_terminal_input", fy_mapping(
			"name", t->name, "data", fy_string_size(bytes, len))))
		view->failed = true;
	return true;
}

/* Whether @sf is the tile of a program that ended, kept to read. */
static bool remote_ui_term_kept(void *data, struct fytim_surface *sf)
{
	struct fyai_remote_ui *view = data;
	struct remote_ui_term *t;

	for (t = view->terms; t && t->surface != sf; t = t->next)
		;
	return t && t->exited && !t->dismissed;
}

/*
 * A control of the tile of a terminal of the worker: the owner of the tile is
 * this record, not a session or a job of this process. A close of a tile
 * that ended drops it here; of a running one, asks the worker to end its
 * program. A scroll moves the view that this process keeps.
 */
static bool remote_ui_term_request(void *data, struct fytim_surface *sf,
				   int delta)
{
	struct fyai_remote_ui *view = data;
	struct remote_ui_term *t;

	for (t = view->terms; t && t->surface != sf; t = t->next)
		;
	/* A mirror of a session has the session of this process. */
	if (!t || t->mirror)
		return false;
	if (delta) {
		if (fyai_terminal_view_scroll(t->view, delta) &&
		    fyai_ui_surface_publish(t->surface, t->view) > 0)
			fyai_ui_wake(fyai_remote_ui_ctx(view));
		return true;
	}
	if (t->exited)
		remote_ui_term_dismiss(view, t, "\x1b", 1);
	else if (remote_ui_request(view, "fyai_terminal_close", fy_mapping(
			"name", t->name, "force", false)))
		view->failed = true;
	return true;
}

static const struct fyai_uimodel_tile_ops remote_ui_tile_ops = {
	.keys = remote_ui_term_keys,
	.kept = remote_ui_term_kept,
	.request = remote_ui_term_request,
};

/* Show each terminal that the member "items" of the work store names. */
/*
 * True when an agent that started before @item still runs and has no terminal:
 * its tile is to open first. The items come newest first.
 */
static bool remote_ui_term_waits(fy_generic items, fy_generic item)
{
	fy_generic other;
	bool after = false;

	if (!fy_equal(fy_get(item, "kind", ""), "agent"))
		return false;
	fy_foreach(other, items) {
		if (!after && !strcmp(fy_get(other, "name", ""),
				      fy_get(item, "name", ""))) {
			after = true;
			continue;
		}
		if (after && fy_equal(fy_get(other, "kind", ""), "agent") &&
		    !fy_get(other, "btw", false) &&
		    !fy_get(other, "exited", false) &&
		    !fy_is_mapping(fy_get(other, "terminal", fy_invalid)))
			return true;
	}
	return false;
}

static int remote_ui_terms(struct fyai_remote_ui *view, fy_generic root)
{
	struct fyai_ctx *ctx = fyai_remote_ui_ctx(view);
	struct remote_ui_term **tp, *t;
	fy_generic items = fy_get(root, "items", fy_seq_empty), item, term;
	const char *name, *title;
	size_t i;
	bool found, first;

	/* Skip user-shell tiles already exited before this client attached. */
	first = !view->terms_seen && fy_len(items);
	if (fy_len(items))
		view->terms_seen = true;
	tp = &view->terms;
	while ((t = *tp)) {
		found = false;
		fy_foreach(item, items)
			found = found || (fy_is_valid(fy_get(item, "terminal",
							      fy_invalid)) &&
					  !strcmp(fy_get(item, "name", ""),
						  t->name));
		/* Keep the side-question tile until the user closes it. */
		if (found || (t->btw && t->exited && !t->dismissed)) {
			tp = &t->next;
			continue;
		}
		*tp = t->next;
		remote_ui_term_free(ctx, t);
	}
	/* Open tiles in agent start order. */
	for (i = fy_len(items); i-- > 0;) {
		item = fy_get_at(items, i);
		term = fy_get(item, "terminal", fy_invalid);
		if (!fy_is_mapping(term))
			continue;
		name = fy_get(item, "name", "");
		for (t = view->terms; t && strcmp(t->name, name); t = t->next)
			;
		/* A new text starts again from a snapshot of the screen,
		 * which the view of a mirror replays over what it shows. */
		if (t && t->mirror && t->source != fy_get(term, "source", 0LL)) {
			t->source = fy_get(term, "source", 0LL);
			t->shown = 0;
		}
		if (t && t->mirror) {
			if (!t->dismissed &&
			    fyai_tools_mirror_update(
					fyai_shell_session_find(ctx, name),
					item))
				t->shown = 0;
			if (remote_ui_term_feed(view, t))
				return -1;
			continue;
		}
		if (t && t->source != fy_get(term, "source", 0LL)) {
			for (tp = &view->terms; *tp != t; tp = &(*tp)->next)
				;
			*tp = t->next;
			remote_ui_term_free(ctx, t);
			t = NULL;
		}
		if (!t && first && fy_get(item, "exited", false) &&
		    !strcmp(fy_get(item, "kind", ""), "user-shell"))
			continue;
		/*
		 * User-shell tiles are local to the driving client; watchers
		 * omit them (e.g., `/split` runs a watcher inside one).
		 */
		if (!t && view->watch && !strcmp(fy_get(item, "kind", ""),
						 "user-shell"))
			continue;
		/* The tiles of agents open in the order they started: the
		 * worker lists the newest first, and an older agent that has
		 * no terminal yet comes first. */
		if (!t && remote_ui_term_waits(items, item))
			continue;
		if (!t) {
			t = calloc(1, sizeof(*t));
			fyai_error_check(ctx, t, err, "cannot keep a terminal");
			t->name = fyai_ctx_keep_str(ctx, name);
			t->agent = !strcmp(fy_get(item, "kind", ""), "agent");
			t->btw = fy_get(item, "btw", false);
			t->source = fy_get(term, "source", 0LL);
			t->rows = fy_get(term, "rows", 24);
			t->cols = fy_get(term, "cols", 80);
			t->next = view->terms;
			view->terms = t;
			fyai_error_check(ctx, t->name, err, "cannot keep a terminal");
			if (!t->agent) {
				t->mirror = true;
				if (!fyai_tools_mirror_open(ctx, item,
						&remote_ui_mirror_ops, view) ||
				    remote_ui_term_feed(view, t))
					return -1;
				continue;
			}
			if (remote_ui_term_open(view, t, item))
				return -1;
			/* A command of the user takes the keys, as it does
			 * in a local session. */
			if (!strcmp(fy_get(item, "kind", ""), "user-shell"))
				(void)fyai_tools_focus_tile(ctx, t->surface);
		}
		/* A tile closed here waits for the worker to drop it. */
		if (t->dismissed)
			continue;
		/* Add the model to the title once it is known. */
		title = remote_ui_term_title(t, item);
		if (title && (!t->title || strcmp(t->title, title))) {
			free(t->title);
			t->title = strdup(title);
			if (!t->exited)
				remote_ui_term_run(view, t, t->title);
		}
		/* A program that ended leaves its screen to read. */
		if (!t->exited && fy_get(item, "exited", false)) {
			t->exited = true;
			if (t->inline_block) {
				if (remote_ui_term_feed(view, t))
					return -1;
				remote_ui_term_paint(view, t, true,
						     fy_get(item, "ok", false));
				t->dismissed = true;
				continue;
			}
			(void)fyai_chrome_update(ctx, t->surface,
				&(struct fyai_chrome_spec){
					.title = t->title ? t->title :
						 fy_sprintfa("**%s** %s",
						t->agent ? "agent" : "shell",
						t->name),
					.mark = fy_get(item, "ok", false) ?
						FYAI_UI_MARK_OK :
						FYAI_UI_MARK_FAILED,
				}, NULL);
			/* A side question keeps its tile, which takes the
			 * pane until the user closes it. */
			if (t->btw) {
				if (remote_ui_term_feed(view, t))
					return -1;
				(void)fyai_tools_focus_tile(ctx, t->surface);
				(void)fyai_ui_surface_zoom(ctx, t->surface);
				fyai_chrome_focus(ctx, t->surface, true);
				continue;
			}
			/* The screen of an agent that ended goes to the
			 * scrollback, as a local session commits it. */
			if (t->agent) {
				if (remote_ui_term_feed(view, t))
					return -1;
				fyai_workpane_tile_set_selectable(
					ctx->screen->workpane, t->surface, false);
				if (fyai_workpane_focused(ctx->screen->workpane) ==
				    t->surface)
					fyai_workpane_clear_focus(ctx->screen->workpane);
				if (fyai_workpane_zoomed(ctx->screen->workpane) ==
				    t->surface)
					fyai_workpane_clear_zoom(ctx->screen->workpane);
				fyai_ui_surface_commit(ctx, t->surface);
				t->surface = NULL;
				t->dismissed = true;
				continue;
			}
			/* The hint of a focused tile says that Escape closes
			 * it now. */
			if (fyai_workpane_focused(ctx->screen->workpane) == t->surface)
				fyai_chrome_focus(ctx, t->surface, true);
		}
		if (t->rows != fy_get(term, "rows", 24) ||
		    t->cols != fy_get(term, "cols", 80)) {
			t->rows = fy_get(term, "rows", 24);
			t->cols = fy_get(term, "cols", 80);
			fyai_terminal_view_resize(t->view, t->rows, t->cols);
			(void)fyai_ui_surface_resize(t->surface, t->rows,
						     t->cols);
		}
	}
	return 0;
err:
	return -1;
}

/*
 * The state of the worker: what a local session reads from its arena. The
 * stored display configuration of the branch is the configuration that this
 * client starts from, as a local session starts from it; the settings that the
 * client changes stay over it.
 */
static int remote_ui_state(struct fyai_remote_ui *view, fy_generic root)
{
	struct fyai_ctx *ctx = fyai_remote_ui_ctx(view);
	struct fyai_cfg *cfg = ctx->cfg;
	fy_generic display = fy_get(root, "display", fy_invalid), doc;
	int rc;

	if (fy_is_mapping(display)) {
		doc = fy_assoc(cfg->gb, view->base, "display",
			       fy_gb_internalize(cfg->gb, display));
		fyai_error_check(ctx, fy_is_valid(doc), err,
				 "cannot keep the display configuration of "
				 "the worker");
		ctx->bstate->config = doc;
		rc = fyai_config_rederive(ctx);
		fyai_error_check(ctx, !rc, err,
				 "cannot take the display configuration of the "
				 "worker");
	}
	return 0;
err:
	return -1;
}

/* Append @line to the shared interactive input history file. */
static void remote_ui_history_save(struct fyai_remote_ui *view,
				   struct fyai_ctx *ctx, const char *line)
{
	char *path;

	/* Do not record lines consumed by an interactive command prompt. */
	if (view->input)
		return;
	path = fyai_history_path();
	fyai_ui_history_save(ctx, path, line);
	free(path);
}

/* Open the display, with the configuration that the state gave. */
static int remote_ui_display_open(struct fyai_remote_ui *view)
{
	struct fyai_ctx *ctx = fyai_remote_ui_ctx(view);
	char *path;
	int rc;

	rc = fyai_ui_open(ctx);
	fyai_error_check(ctx, !rc && fyai_ui_active(ctx), err,
			 "the client of a session needs a terminal; use "
			 "--protocol sdk for SDK frames");
	/* Watchers forward typed lines as messages to the session. */
	if (view->watch) {
		fyai_ui_pane_begin(ctx);
		(void)fyai_sink_printf(ctx->out->sink, FYAI_SINK_NOTICE,
				       "watching: what you type goes to the "
				       "session as a message; ^C leaves\n");
		fyai_ui_pane_end(ctx, "remote", true, true);
	}
	path = fyai_history_path();
	fyai_ui_history_load(ctx, path);
	free(path);
	view->opened = true;
	if (view->picker) {
		view->picker = false;
		rc = fyai_browser_open_resume(ctx, view->picker_all);
		fyai_error_check(ctx, !rc, err, "cannot open the resume picker");
	}
	return 0;
err:
	return -1;
}

/* Keep @line, which is about to go as the next request, for its output. */
static void remote_ui_command_add(struct fyai_remote_ui *view, const char *line)
{
	struct remote_ui_command *c, **link;
	size_t n = 0;

	c = calloc(1, sizeof(*c));
	if (!c)
		return;
	snprintf(c->id, sizeof(c->id), "ui-fyai_command-%llu",
		 view->requests + 1);
	c->line = strdup(line);
	if (!c->line) {
		free(c);
		return;
	}
	/* A command with no output never comes back: the oldest go. */
	for (link = &view->commands; *link; link = &(*link)->next)
		if (++n >= REMOTE_UI_COMMANDS_MAX) {
			struct remote_ui_command *old = *link;

			*link = old->next;
			free(old->line);
			free(old);
			break;
		}
	for (link = &view->commands; *link; link = &(*link)->next)
		;
	*link = c;
}

static void remote_ui_commands_free(struct fyai_remote_ui *view)
{
	struct remote_ui_command *c;

	while ((c = view->commands)) {
		view->commands = c->next;
		free(c->line);
		free(c);
	}
}

/*
 * The output that the transcript took for the entry of the log that the
 * request names goes back to the worker, which stores it with the command.
 */
static void remote_ui_log_kept(void *data, const char *request, char *kept)
{
	struct fyai_remote_ui *view = data;
	struct remote_ui_command *c, **link;

	if (!request)
		goto out;
	for (link = &view->commands; (c = *link); link = &c->next)
		if (!strcmp(c->id, request))
			break;
	if (!c)
		goto out;
	*link = c->next;
	if (kept && remote_ui_request(view, "fyai_record",
			fy_mapping("command", c->id, "line", c->line,
				   "output", kept)))
		view->failed = true;
	free(c->line);
	free(c);
out:
	free(kept);
}

/* A store of the worker changed: apply it, then draw what follows from it. */
static int remote_ui_store(struct fyai_remote_ui *view, fy_generic frame)
{
	struct fyai_ctx *ctx = fyai_remote_ui_ctx(view);
	const char *name = fy_get(frame, "store", "");
	fy_generic root = fy_get(frame, "root", fy_invalid);
	long long head, taken;
	bool busy;
	char *line;

	if (!strcmp(name, "state") && remote_ui_state(view, root))
		return -1;
	if (fyai_uimodel_mirror_store(ctx, name, root))
		return -1;
	if (!strcmp(name, "session")) {
		if (fyai_ctx_set_branch(ctx, fy_get(fy_get(root, "header",
				fy_invalid), "branch", fyai_ctx_branch(ctx))))
			return -1;
		fyai_ui_header_refresh(ctx);
		view->input = fy_get(root, "input", false);
		busy = fy_get(fy_get(root, "turn", fy_invalid), "busy", false);
		if (busy != view->busy) {
			view->busy = busy;
			/* The UI repaints when it is idle: the turns of the
			 * worker are to be there first. */
			if (!busy && view->syncs)
				view->idle_held = true;
			else {
				view->idle_held = false;
				fyai_ui_set_busy(ctx, busy);
			}
		}
		/* The lines that the worker took leave the queue, which showed
		 * them as pending. */
		taken = fy_get(root, "queued", 0LL);
		while (view->queued && view->taken < taken) {
			/* Dequeue queued messages only, not secret prompts. */
			line = fyai_ui_take_line(ctx);
			if (line)
				remote_ui_history_save(view, ctx, line);
			free(line);
			view->queued--;
			view->taken++;
		}
		/* The stored turns replace the live rows when the head
		 * moves, as a published turn replaces them in a local
		 * session. */
		head = fy_get(fy_get(root, "head", fy_invalid), "updated", 0LL);
		if (head != view->head) {
			view->head = head;
			if (view->initialized &&
			    remote_ui_sync(view))
				return -1;
		}
	} else if (!strcmp(name, "work")) {
		if (remote_ui_av(view, fy_get(root, "agent_view", fy_invalid)))
			return -1;
		return remote_ui_terms(view, root);
	}
	return 0;
}

/*
 * Ask the worker for the turns this replica lacks: the ids of the newest turn
 * held and of the turns 1, 2, 4, 8, ... before it say what is held, however
 * the head moved.
 */
static int remote_ui_sync(struct fyai_remote_ui *view)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(fyai_remote_ui_ctx(view));
	fy_generic have = fy_seq_empty;
	size_t back, i;

	for (back = 0; back < view->nturns; back = back ? back * 2 : 1) {
		i = view->nturns - 1 - back;
		have = fy_append(gb, have, fy_value(gb, view->turns[i].id));
		if (!i)
			break;
	}
	fyai_diag_tracef("sync", "client: asks, holds %zu turns", view->nturns);
	view->syncs++;
	return remote_ui_request(view, "fyai_sync", fy_mapping(gb,
		"have", have, "depth", (long long)REMOTE_UI_HISTORY));
}

/* Keep @id and @turn as the newest turn of the replica. */
static int remote_ui_turns_push(struct fyai_remote_ui *view, const char *id,
				fy_generic turn)
{
	struct remote_ui_turn *v;
	size_t cap;

	if (view->nturns == view->turns_cap) {
		cap = view->turns_cap ? view->turns_cap * 2 : 64;
		v = realloc(view->turns, cap * sizeof(*v));
		fyai_error_check(fyai_remote_ui_ctx(view), v, err,
				 "cannot keep the stored turns");
		view->turns = v;
		view->turns_cap = cap;
	}
	snprintf(view->turns[view->nturns].id, REMOTE_UI_TURN_ID, "%s", id);
	view->turns[view->nturns++].turn = turn;
	return 0;
err:
	return -1;
}

/*
 * The turns of the worker that the replica lacked join it. @base names the
 * newest turn that both hold: the turns after it, here, are dropped, because
 * the head moved to another line. Without a base the replica starts again.
 */
static int remote_ui_history(struct fyai_remote_ui *view, fy_generic response)
{
	struct fyai_ctx *ctx = fyai_remote_ui_ctx(view);
	struct fy_generic_builder *gb = view->history_gb;
	fy_generic head = fy_null, turn, base = fy_get(response, "base",
						       fy_invalid);
	const char *epoch = fy_get(response, "epoch", "");
	const char *base_id = fy_is_string(base) ? fy_castp(&base, "") : NULL;
	bool fresh = !gb, first, dropped = false;
	size_t k, held = view->nturns;

	if (view->epoch && strcmp(view->epoch, epoch)) {
		view->nturns = 0;
		view->restarted = true;
	}
	if (!view->epoch || strcmp(view->epoch, epoch)) {
		view->epoch = fyai_ctx_keep_str(ctx, epoch);
		fyai_error_check(ctx, view->epoch, err,
				 "cannot keep the stored turns");
	}
	if (base_id) {
		for (k = view->nturns; k-- > 0;)
			if (!strcmp(view->turns[k].id, base_id))
				break;
		/* A base that is not held: ask again from nothing. */
		if (k == (size_t)-1) {
			view->nturns = 0;
			return remote_ui_sync(view);
		}
		view->nturns = k + 1;
		dropped = view->nturns < held;
		head = view->turns[k].turn;
		view->garbage += held - view->nturns;
		if (view->garbage > view->nturns + REMOTE_UI_GARBAGE_MIN) {
			/* Make the builder again from the turns that remain. */
			gb = fy_generic_builder_create(NULL);
			fyai_error_check(ctx, gb, err,
					 "cannot keep the stored turns");
			head = fy_null;
			for (k = 0; k < view->nturns; k++) {
				turn = view->turns[k].turn;
				head = fy_gb_internalize(gb, fy_mapping(gb,
					"messages", fy_get(turn, "messages",
							   fy_seq_empty),
					"display_outputs", fy_get(turn,
						"display_outputs", fy_seq_empty),
					"reply", fy_get(turn, "reply",
							fy_null),
					"previous", head));
				if (fy_is_invalid(head)) {
					fy_generic_builder_destroy(gb);
					view->nturns = 0;
					fyai_error(ctx, "cannot keep the stored "
						   "turns");
					goto err;
				}
				view->turns[k].turn = head;
			}
			view->garbage = 0;
		}
	} else {
		view->nturns = 0;
		view->garbage = 0;
		dropped = held > 0;
		gb = fy_generic_builder_create(NULL);
		fyai_error_check(ctx, gb, err, "cannot keep the stored turns");
	}
	fyai_diag_tracef("sync", "client: answer, base %s, %zu turns, held %zu "
			 "(dropped %zu)", base_id ? "found" : "none",
			 fy_len(fy_get(response, "turns", fy_seq_empty)),
			 view->nturns, held - view->nturns);
	fy_foreach(turn, fy_get(response, "turns", fy_seq_empty)) {
		head = fy_gb_internalize(gb, fy_mapping(gb, "messages",
			fy_get(turn, "messages", fy_seq_empty),
			"display_outputs",
			fy_get(turn, "display_outputs", fy_seq_empty),
			"reply", fy_get(turn, "reply", fy_null),
			"previous", head));
		if (fy_is_invalid(head) ||
		    remote_ui_turns_push(view, fy_get(turn, "id", ""), head)) {
			if (gb != view->history_gb)
				fy_generic_builder_destroy(gb);
			fyai_error(ctx, "cannot keep the stored turns");
			goto err;
		}
	}
	if (gb != view->history_gb) {
		/* The transient builder has the builder of the turns as its
		 * parent: it goes before that builder does. */
		fyai_cleanup_transient_builder(ctx);
		ctx->arena->gb = gb;
	}
	ctx->turn->last_message = fy_is_null(head) ? fy_invalid : head;
	/* A session opens on the screen its conversation left. After that,
	 * the stored turns replace the live rows as a published turn replaces
	 * them in a local session: the transcript view reads them on its next
	 * frame, and an inline screen keeps the rows it committed. */
	if (!view->opened && remote_ui_display_open(view)) {
		if (gb != view->history_gb) {
			/* The arena no longer names the builder that goes. */
			ctx->arena->gb = view->history_gb;
			ctx->turn->last_message = fy_invalid;
			fy_generic_builder_destroy(gb);
		}
		goto err;
	}
	/* A worker that started again begins a session of its own: the screen
	 * says what the conversation holds, as a restarted process does. */
	first = fresh || view->restarted;
	view->restarted = false;
	if (gb != view->history_gb) {
		fy_generic_builder_destroy(view->history_gb);
		view->history_gb = gb;
	}
	if (dropped) {
		/* The head moved to another line, as a command of a local
		 * session moves it: the screen is made again. */
		fyai_ui_repaint(ctx);
	}
	if (first) {
		fyai_interactive_recap(ctx);
		fyai_ui_drain_output(ctx);
	}
	return 0;
err:
	return -1;
}

/* Send queued queries up to the in-flight limit. */
static int remote_ui_query_pump(struct fyai_remote_ui *view)
{
	struct remote_ui_query *q;
	int rc;

	for (q = view->queries; q; q = q->next) {
		if (view->query_inflight >= REMOTE_UI_QUERIES_MAX)
			break;
		if (q->seq)
			continue;
		rc = remote_ui_request(view, q->subtype, q->request);
		if (rc)
			return rc;
		q->seq = view->requests;
		view->query_inflight++;
	}
	return 0;
}

/* Forget the queries when none is left, so the builder does not grow. */
static void remote_ui_query_settle(struct fyai_remote_ui *view)
{
	if (!view->queries && view->query_gb)
		fy_generic_builder_reset(view->query_gb);
}

static int remote_ui_query_start(void *data, const char *subtype,
				 fy_generic request, fyai_uimodel_answer_fn fn,
				 void *arg, unsigned long long *idp)
{
	struct fyai_remote_ui *view = data;
	struct fyai_ctx *ctx = fyai_remote_ui_ctx(view);
	struct remote_ui_query *q;
	int rc;

	/* A watcher cannot send commands. */
	if (view->watch && strcmp(subtype, "fyai_query"))
		return -1;
	if (!view->query_gb)
		view->query_gb = fy_generic_builder_create(NULL);
	fyai_error_check(ctx, view->query_gb, err,
			 "cannot keep a query of the renderer");
	q = calloc(1, sizeof(*q));
	fyai_error_check(ctx, q, err, "cannot keep a query of the renderer");
	/* The renderer's request may not outlive the call. */
	q->request = fy_gb_internalize(view->query_gb, request);
	q->subtype = subtype;
	q->fn = fn;
	q->arg = arg;
	q->id = ++view->query_ids;
	if (!view->query_tail)
		view->query_tail = &view->queries;
	*view->query_tail = q;
	view->query_tail = &q->next;
	if (idp)
		*idp = q->id;
	fyai_diag_tracef("query", "client: query %llu asked, %u in flight",
			 q->id, view->query_inflight);
	rc = remote_ui_query_pump(view);
	fyai_error_check(ctx, !rc, err, "cannot send a query to the worker");
	return 0;
err:
	return -1;
}

/* Take @q out of the list, and free it. */
static void remote_ui_query_drop(struct fyai_remote_ui *view,
				 struct remote_ui_query *q)
{
	struct remote_ui_query **pp;

	for (pp = &view->queries; *pp && *pp != q; pp = &(*pp)->next)
		;
	if (!*pp)
		return;
	*pp = q->next;
	if (view->query_tail == &q->next)
		view->query_tail = pp;
	if (q->seq && view->query_inflight)
		view->query_inflight--;
	free(q);
}

static void remote_ui_query_cancel(void *data, unsigned long long id)
{
	struct fyai_remote_ui *view = data;
	struct remote_ui_query *q;

	for (q = view->queries; q && q->id != id; q = q->next)
		;
	if (!q)
		return;
	/* Keep sent requests queued but discard their answers. */
	if (q->seq) {
		q->fn = NULL;
		return;
	}
	remote_ui_query_drop(view, q);
	remote_ui_query_settle(view);
}

static const struct fyai_uimodel_query_ops remote_ui_query_ops = {
	.start = remote_ui_query_start,
	.cancel = remote_ui_query_cancel,
};

/* Handle the worker's answer to request @seq. */
static int remote_ui_query_answer(struct fyai_remote_ui *view,
				  unsigned long long seq, fy_generic response)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(fyai_remote_ui_ctx(view));
	struct remote_ui_query *q;
	fyai_uimodel_answer_fn fn;
	void *arg;
	fy_generic answer, inner;

	for (q = view->queries; q && q->seq != seq; q = q->next)
		;
	if (!q)
		return 0;
	fn = q->fn;
	arg = q->arg;
	fyai_diag_tracef("query", "client: query %llu answered%s", q->id,
			 fn ? "" : ", dropped");
	remote_ui_query_drop(view, q);
	if (fn) {
		if (fy_equal(fy_get(response, "subtype", ""), "error")) {
			inner = fy_mapping(gb, "code", "refused", "message",
					   fy_get(response, "error", "no cause"));
			answer = fy_mapping(gb, "error", inner);
		} else {
			answer = fy_get(response, "response", fy_map_empty);
		}
		fn(arg, answer);
	}
	remote_ui_query_settle(view);
	return remote_ui_query_pump(view);
}

static int remote_ui_response(struct fyai_remote_ui *view, fy_generic frame)
{
	struct fyai_ctx *ctx = fyai_remote_ui_ctx(view);
	fy_generic response = fy_get(frame, "response", fy_invalid);
	const char *id = fy_get(response, "request_id", "");
	bool init = !strncmp(id, "ui-initialize-", 14);
	int rc;

	if (!strncmp(id, "ui-fyai_query-", 14))
		return remote_ui_query_answer(view, strtoull(id + 14, NULL, 10),
					      response);
	if (!strncmp(id, "ui-fyai_command-", 16))
		return remote_ui_query_answer(view, strtoull(id + 16, NULL, 10),
					      response);
	if (fy_equal(fy_get(response, "subtype", ""), "error")) {
		/* A sync that reached a restarted worker before its
		 * initialize: the initialize that follows syncs again. */
		if (!view->initialized && !init &&
		    !strncmp(id, "ui-fyai_sync-", 13))
			return 0;
		fyai_error(ctx, "the worker refused %s: %s", id,
			   fy_get(response, "error", "no cause"));
		return init || !strncmp(id, "ui-fyai_sync-", 13) ? -1 : 0;
	}
	if (init) {
		fyai_error_check(ctx, fy_get(fy_get(fy_get(response, "response",
				fy_invalid), "fyai", fy_invalid), "model",
				0LL) == 1, err,
			"the endpoint does not stream the UI model");
		view->initialized = true;
		fyai_event_source_remove(view->deadline);
		view->deadline = NULL;
		return remote_ui_sync(view);
	}
	if (!strncmp(id, "ui-fyai_sync-", 13)) {
		/* The snapshot of the stores came before this answer. */
		fyai_uidraw_attached(ctx);
		if (view->syncs)
			view->syncs--;
		rc = remote_ui_history(view, fy_get(response, "response",
						    fy_invalid));
		if (!view->syncs && view->idle_held) {
			view->idle_held = false;
			fyai_ui_set_busy(ctx, false);
		}
		return rc;
	}
	return 0;
err:
	return -1;
}

static int remote_ui_frame(struct fyai_remote_ui *view, fy_generic frame)
{
	struct fyai_ctx *ctx = fyai_remote_ui_ctx(view);
	struct remote_ui_term *t;
	fy_generic type = fy_get(frame, "type", fy_invalid);
	const char *subtype = fy_get(frame, "subtype", ""), *next;
	int rc;

	if (fy_equal(type, "control_response"))
		return remote_ui_response(view, frame);
	if (fy_equal(type, "result")) {
		if (fy_get(frame, "is_error", false)) {
			free(view->error);
			view->error = strdup(fy_get(frame, "result",
						    "the remote turn failed"));
		}
		return 0;
	}
	if (!fy_equal(type, "fyai"))
		return 0;
	if (!strcmp(subtype, "backgrounded")) {
		/* Worker moved to server; exit without waiting on it. */
		view->released = true;
		next = fy_get(frame, "next", (const char *)NULL);
		/* `/root` is valid only when entered via `/jump`. */
		if (next && !strcmp(next, "root:") &&
		    fy_str_empty(getenv(FYAI_FLEET_RETURN_ENV))) {
			(void)fyai_report(ctx, "root: no /jump reached this "
					  "session; it stays as it is\n");
			next = NULL;
		}
		if (next) {
			free(ctx->cfg->next_session);
			ctx->cfg->next_session = strdup(next);
			fyai_error_check(ctx, ctx->cfg->next_session, err,
					 "cannot keep the next session");
		}
		free(view->left);
		view->left = strdup(fy_get(frame, "branch", ""));
		view->left_parked = fy_get(frame, "parked", false);
		fyai_error_check(ctx, view->left, err,
				 "cannot keep the name of the session that left");
		fyai_ui_quit_request(ctx);
		return 0;
	}
	if (!strcmp(subtype, "restarted")) {
		/* A new worker took the channel: it waits for the initialize.
		 * It has none of the lines that the old one held. */
		view->initialized = false;
		view->busy = false;
		view->queued = 0;
		view->taken = 0;
		fyai_uidraw_restart(ctx);
		(void)fyai_report(ctx, "the session restarted\n");
		rc = remote_ui_request(view, "initialize",
				       remote_ui_init_fields(ctx));
		fyai_error_check(ctx, !rc, err, "cannot send the initialize");
		return 0;
	}
	if (!strcmp(subtype, "history")) {
		/* The turns that the head names, sent before the store that
		 * names it: nothing is left to ask for. */
		fyai_uidraw_attached(ctx);
		rc = remote_ui_history(view, fy_get(frame, "response",
						    fy_invalid));
		if (!rc)
			view->head = fy_get(frame, "updated", 0LL);
		return rc;
	}
	if (!strcmp(subtype, "marker")) {
		fyai_ui_marker(ctx, fy_get(frame, "label", ""));
		return 0;
	}
	if (!strcmp(subtype, "store")) {
			return remote_ui_store(view, frame);
	}
	if (!strcmp(subtype, "text")) {
		if (fyai_uimodel_mirror_text(ctx, fy_get(frame, "id", 0LL),
				fy_get(frame, "offset", (size_t)0),
				fy_get(frame, "data", ""),
				strlen(fy_get(frame, "data", "")),
				fy_get(frame, "closed", false)))
			return -1;
		for (t = view->terms; t; t = t->next)
			if (t->source == fy_get(frame, "id", 0LL))
				return remote_ui_term_feed(view, t);
	}
	return 0;
err:
	return -1;
}

static enum fyai_event_action remote_ui_timeout(const struct fyai_event *ev)
{
	struct fyai_remote_ui *view = ev->userdata;

	view->deadline = NULL;
	fyai_error(fyai_remote_ui_ctx(view), "the worker did not answer the initialize");
	view->failed = true;
	return FYAIEA_CONTINUE;
}

struct fyai_remote_ui *fyai_remote_ui_open(struct fyai_ctx *ctx,
					   struct jsonrpc_conn *conn)
{
	struct fyai_remote_ui *view;
	int rc;

	view = calloc(1, sizeof(*view));
	fyai_error_check(ctx, view, err, "cannot open the full-screen client");
	view->ctx = ctx;
	view->conn = conn;
	view->inbox = fy_seq_empty;
	view->inbox_gb = fy_generic_builder_create(NULL);
	view->held_gb = fy_generic_builder_create(NULL);
	view->held = fy_seq_empty;
	fyai_error_check(ctx, view->inbox_gb && view->held_gb, err_view,
			 "cannot keep the frames of the worker");
	rc = fyai_uimodel_mirror(ctx, remote_ui_intent, view);
	fyai_error_check(ctx, !rc, err_view, "cannot mirror the UI model");
	fyai_uimodel_mirror_tiles(ctx, &remote_ui_tile_ops, view);
	fyai_uimodel_mirror_queries(ctx, &remote_ui_query_ops, view);
	ctx->uistores->log_kept = remote_ui_log_kept;
	ctx->uistores->log_data = view;
	/* A branch that is only at the worker has no configuration here. */
	view->base = fy_is_mapping(ctx->cfg->config_doc) ?
		     ctx->cfg->config_doc : fy_map_empty;
	rc = remote_ui_request(view, "initialize", remote_ui_init_fields(ctx));
	fyai_error_check(ctx, !rc, err_view, "cannot send the initialize");
	rc = fyai_event_add_timer(fyai_ctx_loop(ctx), REMOTE_UI_INIT_MS, 0,
				  remote_ui_timeout, view, &view->deadline);
	fyai_error_check(ctx, !rc, err_view, "cannot wait for the worker");
	return view;
err_view:
	fyai_remote_ui_close(view);
err:
	return NULL;
}

void fyai_remote_ui_watch(struct fyai_remote_ui *view, bool watch)
{
	view->watch = watch;
}

bool fyai_remote_ui_released(const struct fyai_remote_ui *view)
{
	return view->released;
}

char *fyai_remote_ui_take_left(struct fyai_remote_ui *view, bool *parkedp)
{
	char *left = view->left;

	*parkedp = view->left_parked;
	view->left = NULL;
	return left;
}

void fyai_remote_ui_resume(struct fyai_remote_ui *view, bool all)
{
	view->picker = true;
	view->picker_all = all;
}

void fyai_remote_ui_receive(struct fyai_remote_ui *view, fy_generic frame)
{
	fy_generic inbox;

	inbox = fy_append(view->inbox_gb, view->inbox,
			  fy_gb_internalize(view->inbox_gb, frame));
	if (fy_is_invalid(inbox)) {
		fyai_error(fyai_remote_ui_ctx(view), "cannot keep a frame of the worker");
		view->failed = true;
		return;
	}
	view->inbox = inbox;
}

/*
 * Forward a line typed in watch mode as a fyai_session_send control request.
 * The control_response is ignored; replies arrive through the session stream.
 */
static int remote_ui_watch_line(struct fyai_remote_ui *view, const char *line)
{
	struct fyai_ctx *ctx = fyai_remote_ui_ctx(view);
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);

	/* Slash commands are disabled in watch mode; `//` escapes `/`. */
	if (line[0] == '/' && line[1] != '/') {
		fyai_warning(ctx, "watching: a command cannot run here; type a "
			     "message, or ^C to leave");
		return 0;
	}
	return jsonrpc_conn_send_frame(view->conn, fy_mapping(gb,
		"type", "control_request",
		"request_id", fy_stringf(gb, "watch-send-%llu", ++view->requests),
		"request", fy_mapping(gb, "subtype", "fyai_session_send",
				      "branch", fyai_ctx_branch(ctx),
				      "text", line[0] == '/' ? line + 1 : line,
				      "from", "a watcher")));
}

/*
 * Send a line the user typed: a message of the conversation. It is echoed
 * here, as a local session echoes it, and its card from the worker is not
 * drawn again.
 */
static int remote_ui_line(struct fyai_remote_ui *view, const char *line)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(fyai_remote_ui_ctx(view));

	if (view->watch)
		return remote_ui_watch_line(view, line);
	/* A line for an attached agent is not a message of the session. */
	if (view->av_attached)
		return jsonrpc_conn_send_frame(view->conn, fy_mapping(gb,
			"type", "user", "message", fy_mapping(gb, "role",
				"user", "content", line)));
	fyai_echo_user_turn(fyai_remote_ui_ctx(view), line);
	fyai_ui_drain_output(fyai_remote_ui_ctx(view));
	if (fyai_uidraw_echoed(fyai_remote_ui_ctx(view), line))
		return -1;

	return jsonrpc_conn_send_frame(view->conn, fy_mapping(gb,
		"type", "user", "message", fy_mapping(gb, "role", "user",
						      "content", line)));
}

/* Send a line that the user typed during a turn. It is not echoed: its card
 * comes from the worker when the turn takes it. */
static int remote_ui_queued(struct fyai_remote_ui *view, const char *line)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(fyai_remote_ui_ctx(view));

	return jsonrpc_conn_send_frame(view->conn, fy_mapping(gb,
		"type", "user", "queued", true,
		"message", fy_mapping(gb, "role", "user", "content", line)));
}

/* A frame that draws: a store other than the state, or a text. */
static bool remote_ui_waits(fy_generic frame)
{
	const char *subtype = fy_get(frame, "subtype", "");

	return fy_equal(fy_get(frame, "type", fy_invalid), "fyai") &&
	       (!strcmp(subtype, "text") ||
		(!strcmp(subtype, "store") &&
		 !fy_equal(fy_get(frame, "store", fy_invalid), "state")));
}

static int remote_ui_hold(struct fyai_remote_ui *view, fy_generic frame)
{
	fy_generic held;

	held = fy_append(view->held_gb, view->held,
			 fy_gb_internalize(view->held_gb, frame));
	fyai_error_check(fyai_remote_ui_ctx(view), fy_is_valid(held), err,
			 "cannot keep a frame of the worker");
	view->held = held;
	return 0;
err:
	return -1;
}

/* Serve the frames that waited for the display, in their order. */
static int remote_ui_release(struct fyai_remote_ui *view)
{
	fy_generic frame;

	fy_foreach(frame, view->held)
		if (remote_ui_frame(view, frame))
			return -1;
	view->held = fy_seq_empty;
	fy_generic_builder_reset(view->held_gb);
	return 0;
}

/* The line at position @n of the queue when it is a command, else NULL. */
static char *remote_ui_take_command(struct fyai_ctx *ctx, size_t n)
{
	const char *line = fyai_ui_peek_line_nth(ctx, n);

	if (!line || (line[0] != '/' && line[0] != '!') ||
	    !strncmp(line, "//", 2))
		return NULL;
	return fyai_ui_take_line_nth(ctx, n);
}

int fyai_remote_ui_step(struct fyai_remote_ui *view)
{
	struct fyai_ctx *ctx = fyai_remote_ui_ctx(view);
	fy_generic frame;
	bool delivered;
	char *line;
	const char *head;
	int rc, rows, cols;

	if (view->failed)
		return -1;
	fy_foreach(frame, view->inbox) {
		if (!view->opened && remote_ui_waits(frame)) {
			if (remote_ui_hold(view, frame))
				return -1;
			continue;
		}
		rc = remote_ui_frame(view, frame);
		if (rc)
			return -1;
		if (view->opened && remote_ui_release(view))
			return -1;
	}
	/* A command of the view that waits for the worker ends here. */
	(void)fyai_cmd_session_step(ctx);
	/* An action that the browser chose goes to the worker. */
	fyai_browser_step(ctx);
	fyai_ui_settle(ctx);
	view->inbox = fy_seq_empty;
	fy_generic_builder_reset(view->inbox_gb);
	if (view->error) {
		fyai_ui_pane_begin(ctx);
		(void)fyai_sink_printf(ctx->out->sink, FYAI_SINK_NOTICE, "%s\n",
				       view->error);
		fyai_ui_pane_end(ctx, "remote", true, true);
		free(view->error);
		view->error = NULL;
	}
	if (ctx->loop->interrupt_pending) {
		fyai_event_interrupt_ack(ctx->loop);
		/* Each signal is handled once. A key that asks for an
		 * interrupt sets the flag with no signal: it ends no idle
		 * session. */
		delivered = false;
		if (ctx->loop->interrupt_seq != ctx->loop->interrupt_seen) {
			ctx->loop->interrupt_seen = ctx->loop->interrupt_seq;
			delivered = fyai_ui_interrupt(ctx);
		}
		/* A command of the view that waits ends as cancelled. */
		if (!delivered && ctx->screen->cmd_call) {
			fyai_cmd_session_interrupt(ctx);
			delivered = true;
		}
		ctx->loop->interrupt_pending =
			ctx->loop->interrupt_seq != ctx->loop->interrupt_seen;
		/* A watcher has no turn to cancel: ^C leaves, as it says. */
		if (view->watch && !delivered)
			fyai_ui_quit_request(ctx);
		/* A turn of the worker is the worker's to cancel. The
		 * interrupt returns the queued lines to the prompt, and the
		 * worker drops the lines that it holds. */
		if (!delivered && view->busy) {
			view->queued = 0;
			if (remote_ui_request(view, "interrupt", fy_map_empty))
				return -1;
		}
	}
	/* The display opens with the state of the worker. */
	if (!view->opened) {
		fyai_cleanup_transient_builder(ctx);
		return 0;
	}
	if (fyai_ui_quit_requested(ctx))
		return 1;
	/* A line typed during a turn stays in the queue, which shows it as
	 * pending. The worker has it, and joins it to the turn. */
	while (view->initialized && !view->watch && view->busy &&
	       (head = fyai_ui_peek_line_nth(ctx, view->queued)) &&
	       head[0] != '/' && head[0] != '!') {
		if (remote_ui_queued(view, head))
			return -1;
		view->queued++;
	}
	/*
	 * The lines in front of a command are messages that the worker has
	 * and has not taken: the command goes on, as a command of a local
	 * session runs beside the turn, and the worker puts it in order.
	 */
	while (view->initialized &&
	       (line = view->queued ? remote_ui_take_command(ctx, view->queued) :
				      fyai_ui_take_line(ctx))) {
		remote_ui_history_save(view, ctx, line);
		/* A new line asks for new candidates. */
		fyai_cmd_complete_reset(ctx);
		if (line[0] == '/' && fyai_cmd_session_ends(line + 1)) {
			free(line);
			return 1;
		}
		/* Reject `/root` locally if not entered via `/jump`. */
		if (!strcmp(line, "/root") &&
		    fy_str_empty(getenv(FYAI_FLEET_RETURN_ENV))) {
			(void)fyai_report(ctx, "root: no /jump reached this "
					  "session; it stays as it is\n");
			free(line);
			continue;
		}
		/* "//" sends a line that starts with a slash. */
		if (!strncmp(line, "//", 2)) {
			rc = remote_ui_line(view, line + 1);
			free(line);
			if (rc)
				return -1;
			continue;
		}
		/* A command of the user runs in a terminal of the worker,
		 * which a tile of this client shows. */
		if (line[0] == '!') {
			rc = fyai_tools_bang_size(ctx, line + 1, &rows, &cols);
			rc = rc || remote_ui_request(view, "fyai_bang",
				fy_mapping("command", line + 1,
					   "rows", (long long)rows,
					   "cols", (long long)cols));
			free(line);
			if (rc)
				return -1;
			continue;
		}
		/* A command of the view runs here; the others run in the
		 * worker, and their output comes back in the log store. */
		if (line[0] == '/' && fyai_cmd_session_local(line + 1)) {
			(void)fyai_session_slash(ctx, line);
			free(line);
			continue;
		}
		/* The output of a command that the transcript takes is stored
		 * with the command, which the worker names by its line. */
		if (line[0] == '/')
			remote_ui_command_add(view, line);
		rc = line[0] == '/' ?
			remote_ui_request(view, "fyai_command",
					  fy_mapping("line", line)) :
			remote_ui_line(view, line);
		free(line);
		if (rc)
			return -1;
	}
	fyai_cleanup_transient_builder(ctx);
	return 0;
}

void fyai_remote_ui_close(struct fyai_remote_ui *view)
{
	struct remote_ui_query *q;
	struct remote_ui_term *t;
	struct fyai_ctx *ctx;

	if (!view)
		return;
	ctx = fyai_remote_ui_ctx(view);
	fyai_event_source_remove(view->deadline);
	fyai_event_source_remove(view->animation);
	/* Suppress query callbacks during view teardown. */
	fyai_cmd_complete_reset(ctx);
	fyai_uimodel_mirror_queries(ctx, NULL, NULL);
	while ((q = view->queries)) {
		view->queries = q->next;
		free(q);
	}
	fy_generic_builder_destroy(view->query_gb);
	fyai_uidraw_finish(ctx);
	remote_ui_av_close(view);
	while ((t = view->terms)) {
		view->terms = t->next;
		remote_ui_term_free(ctx, t);
	}
	remote_ui_commands_free(view);
	ctx->uistores->log_kept = NULL;
	ctx->uistores->log_data = NULL;
	fyai_ui_set_busy(ctx, false);
	fyai_ui_close(ctx);
	fyai_cleanup_transient_builder(ctx);
	if (view->history_gb) {
		ctx->turn->last_message = fy_invalid;
		ctx->arena->gb = NULL;
	}
	fy_generic_builder_destroy(view->history_gb);
	free(view->turns);
	fy_generic_builder_destroy(view->inbox_gb);
	fy_generic_builder_destroy(view->held_gb);
	free(view->error);
	free(view->left);
	free(view);
}
