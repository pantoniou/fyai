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
	char *name;
	long long source;
	size_t shown;
	int rows, cols;
	bool mirror;			/* a mirror session, found by its name */
	bool agent;			/* the terminal of a sub-agent */
	bool exited;
	bool dismissed;			/* closed here; the worker drops it */
	struct fyai_terminal_view *view;
	struct fytim_surface *surface;
};

/*
 * The full-screen client of a remote session. The model of this process is a
 * mirror of the model of the worker: the frames of the worker write it, and
 * the renderer of this process draws from it as it draws a local session.
 */
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
	char *epoch;			/* the incarnation of the worker */
	struct fyai_event_source *deadline;
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
	bool initialized, busy, failed;
	bool watch;			/* reads a session that another client drives */
	char *error;			/* the cause of the last failed turn */
};

static int remote_ui_sync(struct fyai_remote_ui *view);

/* Send @subtype with the members of @fields as a request to the worker. */
static int remote_ui_request(struct fyai_remote_ui *view, const char *subtype,
			     fy_generic fields)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(view->ctx);
	const char *id;

	/* A watcher asks for the model and the turns, which change nothing. */
	if (view->watch && strcmp(subtype, "initialize") &&
	    strcmp(subtype, "fyai_sync"))
		return 0;
	id = fy_gb_intern_string(gb, fy_sprintfa("ui-%s-%llu", subtype,
						 ++view->requests));
	return jsonrpc_conn_send_frame(view->conn, fy_mapping(gb,
		"type", "control_request", "request_id", id,
		"request", fy_assoc(gb, fields, "subtype", subtype)));
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

static const struct fyai_workpane_tile_ops remote_ui_term_ops = {
	.apply_grant = remote_ui_term_grant,
};

static void remote_ui_term_free(struct fyai_ctx *ctx, struct remote_ui_term *t)
{
	if (t->mirror)
		fyai_tools_mirror_drop(fyai_shell_session_find(ctx, t->name));
	if (t->surface) {
		fyai_workpane_unregister(ctx->workpane, t->surface);
		fyai_ui_surface_close(ctx, t->surface);
	}
	fyai_terminal_view_destroy(t->view);
	free(t->name);
	free(t);
}

/* Replay the bytes of the terminal @t that its view has not had. */
static int remote_ui_term_feed(struct fyai_remote_ui *view,
			       struct remote_ui_term *t)
{
	struct fyai_ctx *ctx = view->ctx;
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
	if (t->surface)
		(void)fyai_ui_surface_publish(t->surface, t->view);
	fyai_ui_wake(ctx);
	return 0;
}

/* Open the view and the tile of @t at the size the worker gives. */
static int remote_ui_term_open(struct fyai_remote_ui *view,
			       struct remote_ui_term *t)
{
	struct fyai_ctx *ctx = view->ctx;
	int rc;

	t->view = fyai_terminal_view_create(ctx, t->rows, t->cols, 0);
	fyai_error_check(ctx, t->view, err, "cannot replay the terminal %s",
			 t->name);
	t->surface = fyai_ui_surface_open(ctx, t->rows, t->cols);
	fyai_error_check(ctx, t->surface, err, "cannot show the terminal %s",
			 t->name);
	rc = fyai_workpane_register(ctx->workpane, t->surface,
				    FYAI_WORKPANE_TILE_SHELL, t,
				    &remote_ui_term_ops, t->rows, 0);
	fyai_error_check(ctx, !rc, err, "cannot place the terminal %s",
			 t->name);
	(void)fyai_chrome_update(ctx, t->surface, &(struct fyai_chrome_spec){
		.title = fy_sprintfa("**%s** %s", t->agent ? "agent" :
			     "shell", t->name),
		.mark = FYAI_UI_MARK_RUNNING,
	}, NULL);
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
	struct fyai_ctx *ctx = view->ctx;
	struct remote_ui_term *t;
	size_t keylen;

	for (t = view->terms; t && t->surface != sf; t = t->next)
		;
	if (!t || !len)
		return false;
	if (t->exited) {
		keylen = len > 1 ? 1 : len;
		/* A program that ended reads nothing more. */
		if (!remote_ui_term_closes(bytes, len))
			return true;
		if (remote_ui_request(view, "fyai_terminal_input", fy_mapping(
				"name", t->name, "data", fy_string_size(bytes, keylen))))
			view->failed = true;
		t->dismissed = true;
		fyai_tools_unzoom(ctx);
		fyai_workpane_unregister(ctx->workpane, t->surface);
		fyai_ui_surface_close(ctx, t->surface);
		t->surface = NULL;
		if (len > 1 && fyai_ui_keys_return(ctx, bytes + 1, len - 1))
			fyai_warning(ctx, "the keys after the close of the "
				     "terminal %s are lost", t->name);
		fyai_ui_wake(ctx);
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

static const struct fyai_uimodel_tile_ops remote_ui_tile_ops = {
	.keys = remote_ui_term_keys,
	.kept = remote_ui_term_kept,
};

/* Show each terminal that the member "items" of the work store names. */
static int remote_ui_terms(struct fyai_remote_ui *view, fy_generic root)
{
	struct fyai_ctx *ctx = view->ctx;
	struct remote_ui_term **tp, *t;
	fy_generic items = fy_get(root, "items", fy_seq_empty), item, term;
	const char *name;
	bool found;

	tp = &view->terms;
	while ((t = *tp)) {
		found = false;
		fy_foreach(item, items)
			found = found || (fy_is_valid(fy_get(item, "terminal",
							      fy_invalid)) &&
					  !strcmp(fy_get(item, "name", ""),
						  t->name));
		if (found) {
			tp = &t->next;
			continue;
		}
		*tp = t->next;
		remote_ui_term_free(ctx, t);
	}
	fy_foreach(item, items) {
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
			if (!t->dismissed)
				fyai_tools_mirror_update(
					fyai_shell_session_find(ctx, name),
					item);
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
		if (!t) {
			t = calloc(1, sizeof(*t));
			fyai_error_check(ctx, t, err, "cannot keep a terminal");
			t->name = strdup(name);
			t->agent = !strcmp(fy_get(item, "kind", ""), "agent");
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
			if (remote_ui_term_open(view, t))
				return -1;
			/* A command of the user takes the keys, as it does
			 * in a local session. */
			if (!strcmp(fy_get(item, "kind", ""), "user-shell"))
				(void)fyai_tools_focus_tile(ctx, t->surface);
		}
		/* A tile closed here waits for the worker to drop it. */
		if (t->dismissed)
			continue;
		/* A program that ended leaves its screen to read. */
		if (!t->exited && fy_get(item, "exited", false)) {
			t->exited = true;
			(void)fyai_chrome_update(ctx, t->surface,
				&(struct fyai_chrome_spec){
					.title = fy_sprintfa("**%s** %s",
						t->agent ? "agent" : "shell",
						t->name),
					.mark = fy_get(item, "ok", false) ?
						FYAI_UI_MARK_OK :
						FYAI_UI_MARK_FAILED,
				}, NULL);
			/* The hint of a focused tile says that Escape closes
			 * it now. */
			if (fyai_workpane_focused(ctx->workpane) == t->surface)
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
	struct fyai_ctx *ctx = view->ctx;
	struct fyai_cfg *cfg = ctx->cfg;
	fy_generic display = fy_get(root, "display", fy_invalid), doc;
	int rc;

	if (fy_is_mapping(display)) {
		doc = fy_assoc(cfg->gb, view->base, "display",
			       fy_gb_internalize(cfg->gb, display));
		fyai_error_check(ctx, fy_is_valid(doc), err,
				 "cannot keep the display configuration of "
				 "the worker");
		ctx->arena_config = doc;
		rc = fyai_config_rederive(ctx);
		fyai_error_check(ctx, !rc, err,
				 "cannot take the display configuration of the "
				 "worker");
	}
	return 0;
err:
	return -1;
}

/* Open the display, with the configuration that the state gave. */
static int remote_ui_display_open(struct fyai_remote_ui *view)
{
	struct fyai_ctx *ctx = view->ctx;
	int rc;

	rc = fyai_ui_open(ctx);
	fyai_error_check(ctx, !rc && fyai_ui_active(ctx), err,
			 "the client of a session needs a terminal; use "
			 "--protocol sdk for SDK frames");
	/* Nobody types into a watching view: it has no prompt. */
	if (view->watch) {
		fyai_ui_prompt_enabled(ctx, false);
		fyai_ui_pane_begin(ctx);
		(void)fyai_sink_printf(ctx->sink, FYAI_SINK_NOTICE,
				       "watching: this client sends no input; "
				       "^C leaves\n");
		fyai_ui_pane_end(ctx, "remote", true, true);
	}
	view->opened = true;
	return 0;
err:
	return -1;
}

/* A store of the worker changed: apply it, then draw what follows from it. */
static int remote_ui_store(struct fyai_remote_ui *view, fy_generic frame)
{
	struct fyai_ctx *ctx = view->ctx;
	const char *name = fy_get(frame, "store", "");
	fy_generic root = fy_get(frame, "root", fy_invalid);
	long long head;
	bool busy;

	if (!strcmp(name, "state") && remote_ui_state(view, root))
		return -1;
	if (fyai_uimodel_mirror_store(ctx, name, root))
		return -1;
	if (!strcmp(name, "session")) {
		if (fyai_ctx_set_branch(ctx, fy_get(fy_get(root, "header",
				fy_invalid), "branch", fyai_ctx_branch(ctx))))
			return -1;
		fyai_ui_header_refresh(ctx);
		busy = fy_get(fy_get(root, "turn", fy_invalid), "busy", false);
		if (busy != view->busy) {
			view->busy = busy;
			fyai_ui_set_busy(ctx, busy);
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
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(view->ctx);
	fy_generic have = fy_seq_empty;
	size_t back, i;

	for (back = 0; back < view->nturns; back = back ? back * 2 : 1) {
		i = view->nturns - 1 - back;
		have = fy_append(gb, have, fy_value(gb, view->turns[i].id));
		if (!i)
			break;
	}
	fyai_diag_tracef("sync", "client: asks, holds %zu turns", view->nturns);
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
		fyai_error_check(view->ctx, v, err,
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
	struct fyai_ctx *ctx = view->ctx;
	struct fy_generic_builder *gb = view->history_gb;
	fy_generic head = fy_null, turn, base = fy_get(response, "base",
						       fy_invalid);
	const char *epoch = fy_get(response, "epoch", "");
	const char *base_id = fy_is_string(base) ? fy_castp(&base, "") : NULL;
	bool fresh = !gb, first, dropped = false;
	size_t k, held = view->nturns;

	if (view->epoch && strcmp(view->epoch, epoch))
		view->nturns = 0;
	if (!view->epoch || strcmp(view->epoch, epoch)) {
		free(view->epoch);
		view->epoch = strdup(epoch);
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
		ctx->gb = gb;
	}
	ctx->last_message = fy_is_null(head) ? fy_invalid : head;
	/* A session opens on the screen its conversation left. After that,
	 * the stored turns replace the live rows as a published turn replaces
	 * them in a local session: the transcript view reads them on its next
	 * frame, and an inline screen keeps the rows it committed. */
	if (!view->opened && remote_ui_display_open(view)) {
		if (gb != view->history_gb)
			fy_generic_builder_destroy(gb);
		goto err;
	}
	first = fresh;
	if (gb != view->history_gb) {
		fy_generic_builder_destroy(view->history_gb);
		view->history_gb = gb;
	}
	if (first) {
		fyai_interactive_recap(ctx);
		fyai_ui_drain_output(ctx);
	} else if (dropped) {
		/* The head moved to another line, as a command of a local
		 * session moves it: the screen is made again. */
		fyai_ui_repaint(ctx);
	}
	return 0;
err:
	return -1;
}

static int remote_ui_response(struct fyai_remote_ui *view, fy_generic frame)
{
	struct fyai_ctx *ctx = view->ctx;
	fy_generic response = fy_get(frame, "response", fy_invalid);
	const char *id = fy_get(response, "request_id", "");
	bool init = !strncmp(id, "ui-initialize-", 14);

	if (fy_equal(fy_get(response, "subtype", ""), "error")) {
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
		return remote_ui_history(view, fy_get(response, "response",
						      fy_invalid));
	}
	return 0;
err:
	return -1;
}

static int remote_ui_frame(struct fyai_remote_ui *view, fy_generic frame)
{
	struct fyai_ctx *ctx = view->ctx;
	struct remote_ui_term *t;
	fy_generic type = fy_get(frame, "type", fy_invalid);
	const char *subtype = fy_get(frame, "subtype", "");

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
	if (!strcmp(subtype, "store"))
		return remote_ui_store(view, frame);
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
}

static enum fyai_event_action remote_ui_timeout(const struct fyai_event *ev)
{
	struct fyai_remote_ui *view = ev->userdata;

	view->deadline = NULL;
	fyai_error(view->ctx, "the worker did not answer the initialize");
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
	/* A branch that is only at the worker has no configuration here. */
	view->base = fy_is_mapping(ctx->cfg->config_doc) ?
		     ctx->cfg->config_doc : fy_map_empty;
	rc = remote_ui_request(view, "initialize", fy_mapping(
		"fyai", fy_mapping("model", 1LL)));
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

void fyai_remote_ui_receive(struct fyai_remote_ui *view, fy_generic frame)
{
	fy_generic inbox;

	inbox = fy_append(view->inbox_gb, view->inbox,
			  fy_gb_internalize(view->inbox_gb, frame));
	if (fy_is_invalid(inbox)) {
		fyai_error(view->ctx, "cannot keep a frame of the worker");
		view->failed = true;
		return;
	}
	view->inbox = inbox;
}

/*
 * Send a line the user typed: a message of the conversation. It is echoed
 * here, as a local session echoes it, and its card from the worker is not
 * drawn again.
 */
static int remote_ui_line(struct fyai_remote_ui *view, const char *line)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(view->ctx);

	if (view->watch)
		return 0;
	fyai_echo_user_turn(view->ctx, line);
	fyai_ui_drain_output(view->ctx);
	if (fyai_uidraw_echoed(view->ctx, line))
		return -1;

	return jsonrpc_conn_send_frame(view->conn, fy_mapping(gb,
		"type", "user", "message", fy_mapping(gb, "role", "user",
						      "content", line)));
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
	fyai_error_check(view->ctx, fy_is_valid(held), err,
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

int fyai_remote_ui_step(struct fyai_remote_ui *view)
{
	struct fyai_ctx *ctx = view->ctx;
	fy_generic frame;
	sig_atomic_t interrupt_seq;
	bool delivered;
	char *line;
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
	view->inbox = fy_seq_empty;
	fy_generic_builder_reset(view->inbox_gb);
	if (view->error) {
		fyai_ui_pane_begin(ctx);
		(void)fyai_sink_printf(ctx->sink, FYAI_SINK_NOTICE, "%s\n",
				       view->error);
		fyai_ui_pane_end(ctx, "remote", true, true);
		free(view->error);
		view->error = NULL;
	}
	if (ctx->interrupt_pending) {
		fyai_event_interrupt_ack(ctx);
		interrupt_seq = ctx->interrupt_seq;
		delivered = fyai_ui_interrupt(ctx);
		ctx->interrupt_pending = ctx->interrupt_seq != interrupt_seq;
		/* A turn of the worker is the worker's to cancel. */
		if (!delivered && view->busy &&
		    remote_ui_request(view, "interrupt", fy_map_empty))
			return -1;
	}
	/* The display opens with the state of the worker. */
	if (!view->opened) {
		fyai_cleanup_transient_builder(ctx);
		return 0;
	}
	if (fyai_ui_quit_requested(ctx))
		return 1;
	while (view->initialized && (line = fyai_ui_take_line(ctx))) {
		if (line[0] == '/' && fyai_cmd_session_ends(line + 1)) {
			free(line);
			return 1;
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
	struct remote_ui_term *t;
	struct fyai_ctx *ctx;

	if (!view)
		return;
	ctx = view->ctx;
	fyai_event_source_remove(view->deadline);
	fyai_uidraw_finish(ctx);
	while ((t = view->terms)) {
		view->terms = t->next;
		remote_ui_term_free(ctx, t);
	}
	fyai_ui_set_busy(ctx, false);
	fyai_ui_close(ctx);
	fyai_cleanup_transient_builder(ctx);
	if (view->history_gb) {
		ctx->last_message = fy_invalid;
		ctx->gb = NULL;
	}
	fy_generic_builder_destroy(view->history_gb);
	free(view->turns);
	free(view->epoch);
	fy_generic_builder_destroy(view->inbox_gb);
	fy_generic_builder_destroy(view->held_gb);
	free(view->error);
	free(view);
}
