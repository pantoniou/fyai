/*
 * fyai_workpane.c - the one owner of work-pane geometry, focus, and zoom
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_DISPLAY

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <libfytimui.h>

#include "fyai.h"
#include "fyai_diag.h"
#include "fyai_sink.h"
#include "fyai_ui.h"
#include "fyai_workpane.h"

struct fyai_workpane_tile {
	struct fytim_surface *surface;
	struct fytim_workband *band;
	enum fyai_workpane_tile_kind kind;

	void *owner;
	const struct fyai_workpane_tile_ops *ops;

	/* Layout request. */
	int preferred_rows;
	int max_rows;

	/* Layout grant. */
	int granted_rows;
	int granted_cols;

	/* Program grid. */
	int grid_rows;
	int grid_cols;

	bool selectable;

	/* Presentation thresholds. */
	struct fyai_workpane_ladder ladder;
	enum fyai_workpane_present present;

	struct fyai_workpane_tile *next;
};

struct fyai_workpane_manager {
	struct fyai_ctx *ctx;
	struct fytim *ft;
	struct fytim_workpane *pane;
	int refs;			/* tiles holding the pane open */

	enum fyai_workpane_mode mode;
	enum fyai_workpane_disposition disposition;

	int fixed_rows;			/* rows when the disposition is FIXED */
	int max_rows;			/* configured pane ceiling, 0 for none */
	int terminal_rows;
	int terminal_cols;
	int resolved_max_rows;		/* what the pane was last capped at */

	struct fytim_surface *focused;
	struct fytim_surface *zoomed;

	fyai_ui_keys_fn keys_cb;
	void *keys_user;

	enum fyai_workpane_layout layout;
	enum fyai_workpane_pos position;
	int columns;			/* tracks for LAYOUT_COLUMNS */
	fyai_workpane_policy_fn policy;
	void *policy_user;

	bool layout_pending;
	bool reconciling;

	struct fyai_workpane_tile *tiles;
};

/* Keep the fitted-track marker compatible with the layout library. */
_Static_assert(FYAI_WORKPANE_TRACK_FIT == FYTIM_TRACK_FIT,
	       "the fitted-track marker must match the layout library");

#define for_each_tile(_t, _wm) \
	for ((_t) = (_wm)->tiles; (_t); (_t) = (_t)->next)

struct fyai_workpane_manager *fyai_workpane_of(const struct fyai_ctx *ctx)
{
	return ctx ? ctx->workpane : NULL;
}

static struct fyai_workpane_tile *
workpane_tile(const struct fyai_workpane_manager *wm,
	      const struct fytim_surface *sf)
{
	struct fyai_workpane_tile *t;

	if (!wm || !sf)
		return NULL;
	for_each_tile(t, wm)
		if (t->surface == sf)
			return t;
	return NULL;
}

/* Resolve pane disposition for the current terminal height. */
static int fyai_workpane_resolve_rows(const struct fyai_workpane_manager *wm)
{
	int rows = wm->terminal_rows > 0 ? wm->terminal_rows : 1;
	int cap = 0;

	switch (wm->disposition) {
	case FYAI_WORKPANE_FIXED:
		cap = wm->fixed_rows > 0 ? wm->fixed_rows : rows;
		break;
	case FYAI_WORKPANE_HALF:
		cap = (rows + 1) / 2;
		break;
	case FYAI_WORKPANE_QUARTER:
		cap = (rows + 3) / 4;
		break;
	case FYAI_WORKPANE_FULL:
		cap = 0;
		break;
	}
	/* Apply the configured pane ceiling. */
	if (wm->max_rows > 0 && (cap <= 0 || wm->max_rows < cap))
		cap = wm->max_rows;
	return cap;
}

static const char *workpane_disposition_name(enum fyai_workpane_disposition d)
{
	switch (d) {
	case FYAI_WORKPANE_HALF:	return "half";
	case FYAI_WORKPANE_QUARTER:	return "quarter";
	case FYAI_WORKPANE_FIXED:	return "fixed";
	case FYAI_WORKPANE_FULL:	break;
	}
	return "full";
}

static void workpane_sample_size(struct fyai_workpane_manager *wm)
{
	int cols = 0, rows = 0;

	if (!wm->ft)
		return;
	(void)fytim_size(wm->ft, &cols, &rows);
	if (rows > 0)
		wm->terminal_rows = rows;
	if (cols > 0)
		wm->terminal_cols = cols;
}

void fyai_workpane_adopt_config(struct fyai_workpane_manager *wm)
{
	const struct fyai_cfg *cfg;
	const char *policy;

	if (!wm)
		return;
	cfg = wm->ctx->cfg;
	wm->max_rows = cfg->work_max_rows;
	wm->columns = cfg->work_columns;
	wm->layout = FYAI_WORKPANE_LAYOUT_AUTO;
	if (cfg->work_layout) {
		if (!strcmp(cfg->work_layout, "stack"))
			wm->layout = FYAI_WORKPANE_LAYOUT_STACK;
		else if (!strcmp(cfg->work_layout, "columns"))
			wm->layout = FYAI_WORKPANE_LAYOUT_COLUMNS;
		else if (!strcmp(cfg->work_layout, "main-top"))
			wm->layout = FYAI_WORKPANE_LAYOUT_MAIN_TOP;
		else if (!strcmp(cfg->work_layout, "main-left"))
			wm->layout = FYAI_WORKPANE_LAYOUT_MAIN_LEFT;
	}
	wm->position = FYAI_WORKPANE_POS_ABOVE;
	if (cfg->work_position && !strcmp(cfg->work_position, "below-prompt"))
		wm->position = FYAI_WORKPANE_POS_BELOW;
	wm->fixed_rows = 0;
	policy = cfg->work_zoom_rows;
	if (cfg->work_zoom_fixed_rows > 0) {
		wm->disposition = FYAI_WORKPANE_FIXED;
		wm->fixed_rows = cfg->work_zoom_fixed_rows;
	} else if (policy && !strcmp(policy, "half")) {
		wm->disposition = FYAI_WORKPANE_HALF;
	} else if (policy && !strcmp(policy, "quarter")) {
		wm->disposition = FYAI_WORKPANE_QUARTER;
	} else {
		wm->disposition = FYAI_WORKPANE_FULL;
	}
	wm->layout_pending = true;
}

struct fyai_workpane_manager *fyai_workpane_create(struct fyai_ctx *ctx,
						   struct fytim *ft)
{
	struct fyai_workpane_manager *wm;

	if (!ctx || !ctx->cfg)
		return NULL;
	wm = calloc(1, sizeof(*wm));
	if (!wm)
		return NULL;
	wm->ctx = ctx;
	wm->ft = ft;
	wm->mode = FYAI_WORKPANE_NORMAL;
	workpane_sample_size(wm);
	fyai_workpane_adopt_config(wm);
	return wm;
}

void fyai_workpane_destroy(struct fyai_workpane_manager *wm)
{
	struct fyai_workpane_tile *t, *n;

	if (!wm)
		return;
	for (t = wm->tiles; t; t = n) {
		n = t->next;
		free(t);
	}
	if (wm->pane)
		fytim_workpane_destroy(wm->pane);
	free(wm);
}

/* Map "none" to absent chrome; preserve an empty rule string. */
static const char *workpane_chrome_text(const char *v)
{
	if (!v || !strcmp(v, "none"))
		return NULL;
	return v;
}

static unsigned int workpane_controls(const struct fyai_ctx *ctx)
{
	const char *v = ctx->cfg->work_controls;

	if (!v || !strcmp(v, "none"))
		return 0;
	if (!strcmp(v, "zoom"))
		return FYTIM_WORKPANE_ZOOM | FYTIM_WORKPANE_CLOSE;
	return FYTIM_WORKPANE_ZOOM | FYTIM_WORKPANE_CLOSE |
	       FYTIM_WORKPANE_SCROLLBAR | FYTIM_WORKPANE_ARROWS;
}

bool fyai_workpane_wants_mouse(const struct fyai_ctx *ctx)
{
	return ctx && workpane_controls(ctx) != 0;
}

void fyai_workpane_configure(struct fyai_workpane_manager *wm)
{
	const struct fyai_cfg *cfg;
	int cols = 0;

	if (!wm || !wm->pane)
		return;
	cfg = wm->ctx->cfg;
	/* The arrangement decides the columns; the automatic one still needs
	 * the count a host asked for. */
	if (wm->layout == FYAI_WORKPANE_LAYOUT_COLUMNS)
		cols = wm->columns;
	else if (wm->layout == FYAI_WORKPANE_LAYOUT_STACK)
		cols = 1;
	(void)fytim_workpane_set_place(wm->pane,
			wm->position == FYAI_WORKPANE_POS_BELOW ?
			FYTIM_WORKPANE_BELOW_PROMPT :
			FYTIM_WORKPANE_ABOVE_PROMPT);
	(void)fytim_workpane_set_columns(wm->pane, cols);
	(void)fytim_workpane_set_min_tile_cols(wm->pane, cfg->work_min_tile_cols);
	(void)fytim_workpane_set_top(wm->pane,
				     workpane_chrome_text(cfg->work_frame));
	(void)fytim_workpane_set_bottom(wm->pane,
					workpane_chrome_text(cfg->work_frame));
	(void)fytim_workpane_set_tile_sep(wm->pane, cfg->tile_sep);
	(void)fytim_workpane_set_controls(wm->pane, workpane_controls(wm->ctx));
	wm->layout_pending = true;
}

struct fytim_workpane *fyai_workpane_acquire(struct fyai_workpane_manager *wm)
{
	if (!wm || !wm->ft)
		return NULL;
	if (!wm->pane) {
		wm->pane = fytim_workpane_create(wm->ft);
		if (!wm->pane)
			return NULL;
		wm->refs = 0;
		wm->resolved_max_rows = -1;
	}
	fyai_workpane_configure(wm);
	wm->refs++;
	return wm->pane;
}

void fyai_workpane_release(struct fyai_workpane_manager *wm)
{
	if (!wm)
		return;
	if (wm->refs > 0)
		wm->refs--;
	if (wm->refs > 0 || !wm->pane)
		return;
	fytim_workpane_destroy(wm->pane);
	wm->pane = NULL;
	wm->focused = NULL;
	wm->zoomed = NULL;
	wm->mode = FYAI_WORKPANE_NORMAL;
	wm->resolved_max_rows = -1;
}

static int workpane_add(struct fyai_workpane_manager *wm,
			struct fytim_surface *sf, struct fytim_workband *band,
			enum fyai_workpane_tile_kind kind, void *owner,
			const struct fyai_workpane_tile_ops *ops,
			int preferred_rows, int max_rows)
{
	struct fyai_workpane_tile **tailp;
	struct fyai_workpane_tile *t;

	if (!wm)
		return -1;
	fyai_error_check(wm->ctx, sf || band, err_out,
			 "workpane: register with neither a surface nor a band");
	t = calloc(1, sizeof(*t));
	if (!t)
		return -1;
	t->surface = sf;
	t->band = band;
	t->kind = kind;
	t->owner = owner;
	t->ops = ops;
	t->preferred_rows = preferred_rows;
	t->max_rows = max_rows;
	t->selectable = sf != NULL;
	/* Keep registration order: main layouts select the oldest tile. */
	tailp = &wm->tiles;
	while (*tailp)
		tailp = &(*tailp)->next;
	*tailp = t;
	wm->layout_pending = true;
	return 0;
err_out:
	return -1;
}

int fyai_workpane_register(struct fyai_workpane_manager *wm,
			   struct fytim_surface *sf,
			   enum fyai_workpane_tile_kind kind, void *owner,
			   const struct fyai_workpane_tile_ops *ops,
			   int preferred_rows, int max_rows)
{
	return workpane_add(wm, sf, NULL, kind, owner, ops, preferred_rows,
			    max_rows);
}

int fyai_workpane_register_band(struct fyai_workpane_manager *wm,
				struct fytim_workband *band,
				enum fyai_workpane_tile_kind kind, void *owner)
{
	return workpane_add(wm, NULL, band, kind, owner, NULL, 0, 0);
}

static void workpane_drop(struct fyai_workpane_manager *wm,
			  struct fyai_workpane_tile *t)
{
	struct fyai_workpane_tile **pp;

	for (pp = &wm->tiles; *pp; pp = &(*pp)->next) {
		if (*pp != t)
			continue;
		*pp = t->next;
		free(t);
		wm->layout_pending = true;
		return;
	}
}

void fyai_workpane_unregister(struct fyai_workpane_manager *wm,
			      struct fytim_surface *sf)
{
	struct fyai_workpane_tile *t;

	t = workpane_tile(wm, sf);
	if (!t)
		return;
	/* Retiring a tile also clears its focus and zoom state. */
	if (wm->focused == sf)
		fyai_workpane_clear_focus(wm);
	if (wm->zoomed == sf)
		fyai_workpane_clear_zoom(wm);
	workpane_drop(wm, t);
}

void fyai_workpane_unregister_band(struct fyai_workpane_manager *wm,
				   struct fytim_workband *band)
{
	struct fyai_workpane_tile *t;

	if (!wm || !band)
		return;
	for_each_tile(t, wm)
		if (t->band == band) {
			workpane_drop(wm, t);
			return;
		}
}

void *fyai_workpane_tile_owner(const struct fyai_workpane_manager *wm,
			       const struct fytim_surface *sf,
			       enum fyai_workpane_tile_kind *kindp)
{
	const struct fyai_workpane_tile *t = workpane_tile(wm, sf);

	if (!t)
		return NULL;
	if (kindp)
		*kindp = t->kind;
	return t->owner;
}

bool fyai_workpane_keys_deliver(struct fyai_workpane_manager *wm,
				const char *data, size_t len)
{
	if (!wm || !wm->focused || !wm->keys_cb || !data || !len)
		return false;
	wm->keys_cb(wm->keys_user, data, len);
	return true;
}

void fyai_workpane_set_keys_router(struct fyai_workpane_manager *wm,
				   fyai_ui_keys_fn cb, void *user)
{
	if (!wm)
		return;
	wm->keys_cb = cb;
	wm->keys_user = user;
}

void fyai_workpane_tile_set_selectable(struct fyai_workpane_manager *wm,
				       struct fytim_surface *sf, bool ok)
{
	struct fyai_workpane_tile *t = workpane_tile(wm, sf);

	if (t)
		t->selectable = ok;
}

struct fytim_surface *
fyai_workpane_focused(const struct fyai_workpane_manager *wm)
{
	return wm ? wm->focused : NULL;
}

void fyai_workpane_clear_focus(struct fyai_workpane_manager *wm)
{
	struct fyai_workpane_tile *t;
	struct fytim_surface *sf;

	if (!wm || !wm->focused)
		return;
	sf = wm->focused;
	wm->focused = NULL;
	/* Chrome and key routing belong to the display; focus does not. */
	if (fyai_ui_active(wm->ctx)) {
		fyai_ui_surface_focus(wm->ctx, sf, false);
		(void)fyai_ui_surface_keys(wm->ctx, sf, false, NULL, NULL);
	}
	t = workpane_tile(wm, sf);
	if (t && t->ops && t->ops->focus_changed)
		t->ops->focus_changed(t->owner, false);
	fyai_ui_wake(wm->ctx);
}

void fyai_workpane_set_focus(struct fyai_workpane_manager *wm,
			     struct fytim_surface *sf)
{
	struct fyai_workpane_tile *t;

	if (!wm || !sf)
		return;
	t = workpane_tile(wm, sf);
	assert(t != NULL);
	if (!t)
		return;
	if (wm->focused == sf)
		return;
	fyai_workpane_clear_focus(wm);
	/* A headless pane has no keys to route. */
	if (fyai_ui_active(wm->ctx)) {
		if (fyai_ui_surface_keys(wm->ctx, sf, true, wm->keys_cb,
					 wm->keys_user))
			return;
		fyai_ui_surface_focus(wm->ctx, sf, true);
	}
	wm->focused = sf;
	if (t->ops && t->ops->focus_changed)
		t->ops->focus_changed(t->owner, true);
	fyai_ui_wake(wm->ctx);
}

bool fyai_workpane_focus_next(struct fyai_workpane_manager *wm)
{
	struct fyai_workpane_tile *t;
	struct fytim_surface *next = NULL;
	bool take_next;

	if (!wm)
		return false;
	take_next = wm->focused == NULL;
	/* Cycle from the oldest tile back to the prompt. */
	for_each_tile(t, wm) {
		if (!t->surface || !t->selectable)
			continue;
		if (take_next) {
			next = t->surface;
			break;
		}
		if (t->surface == wm->focused)
			take_next = true;
	}
	if (!next) {
		/* The cycle ends at the prompt. */
		if (!wm->focused)
			return false;
		fyai_workpane_clear_focus(wm);
		return true;
	}
	fyai_workpane_set_focus(wm, next);
	return wm->focused == next;
}

struct fytim_surface *
fyai_workpane_zoomed(const struct fyai_workpane_manager *wm)
{
	return wm ? wm->zoomed : NULL;
}

int fyai_workpane_set_zoom(struct fyai_workpane_manager *wm,
			   struct fytim_surface *sf)
{
	if (!wm || !sf)
		return -1;
	assert(workpane_tile(wm, sf) != NULL);
	if (!workpane_tile(wm, sf))
		return -1;
	wm->zoomed = sf;
	wm->mode = FYAI_WORKPANE_ZOOMED;
	wm->layout_pending = true;
	fyai_workpane_reconcile(wm);
	return 0;
}

void fyai_workpane_clear_zoom(struct fyai_workpane_manager *wm)
{
	if (!wm || !wm->zoomed)
		return;
	wm->zoomed = NULL;
	wm->mode = FYAI_WORKPANE_NORMAL;
	wm->layout_pending = true;
	fyai_workpane_reconcile(wm);
}

void fyai_workpane_terminal_resize(struct fyai_workpane_manager *wm, int rows,
				   int cols)
{
	if (!wm || rows <= 0 || cols <= 0)
		return;
	if (rows == wm->terminal_rows && cols == wm->terminal_cols)
		return;
	wm->terminal_rows = rows;
	wm->terminal_cols = cols;
	/* Recalculate fractional dispositions after terminal resize. */
	wm->layout_pending = true;
	fyai_workpane_reconcile(wm);
}

void fyai_workpane_set_disposition(struct fyai_workpane_manager *wm,
				   enum fyai_workpane_disposition d,
				   int fixed_rows)
{
	if (!wm)
		return;
	wm->disposition = d;
	wm->fixed_rows = d == FYAI_WORKPANE_FIXED ? fixed_rows : 0;
	wm->layout_pending = true;
	fyai_workpane_reconcile(wm);
}

enum fyai_workpane_disposition
fyai_workpane_disposition(const struct fyai_workpane_manager *wm)
{
	return wm ? wm->disposition : FYAI_WORKPANE_FULL;
}

int fyai_workpane_resolved_rows(const struct fyai_workpane_manager *wm)
{
	return wm ? wm->resolved_max_rows : 0;
}

int fyai_workpane_tile_preferred_rows(const struct fyai_workpane_manager *wm,
				      const struct fytim_surface *sf)
{
	const struct fyai_workpane_tile *t = workpane_tile(wm, sf);

	return t ? t->preferred_rows : 0;
}

int fyai_workpane_tile_granted_rows(const struct fyai_workpane_manager *wm,
				    const struct fytim_surface *sf)
{
	const struct fyai_workpane_tile *t = workpane_tile(wm, sf);

	return t ? t->granted_rows : 0;
}

void fyai_workpane_cycle_disposition(struct fyai_workpane_manager *wm)
{
	struct fyai_cfg *cfg;

	if (!wm)
		return;
	cfg = wm->ctx->cfg;
	cfg->work_zoom_fixed_rows = 0;
	wm->fixed_rows = 0;
	switch (wm->disposition) {
	case FYAI_WORKPANE_FULL:
		wm->disposition = FYAI_WORKPANE_HALF;
		break;
	case FYAI_WORKPANE_HALF:
		wm->disposition = FYAI_WORKPANE_QUARTER;
		break;
	case FYAI_WORKPANE_QUARTER:
	case FYAI_WORKPANE_FIXED:
		wm->disposition = FYAI_WORKPANE_FULL;
		break;
	}
	cfg->work_zoom_rows = workpane_disposition_name(wm->disposition);
	fyai_report(wm->ctx, "work pane height: %s", cfg->work_zoom_rows);
	wm->layout_pending = true;
	fyai_workpane_reconcile(wm);
}

/*
 * The standard arrangements. A policy is given the tiles in registration
 * order, so the first is the oldest, which is the one a main-tile layout
 * gives the room to. Columns are as many as keep every tile at the minimum
 * width, and never more than a square arrangement needs.
 */
static int workpane_auto_cols(const struct fyai_workpane_manager *wm, int n)
{
	int min = wm->ctx->cfg->work_min_tile_cols;
	int fit, sq, cols;

	fit = min > 0 ? wm->terminal_cols / min : n;
	if (fit < 1)
		fit = 1;
	for (sq = 1; sq * sq < n; sq++)
		;
	cols = fit < sq ? fit : sq;
	if (cols < 1)
		cols = 1;
	if (cols > n)
		cols = n;
	return cols;
}

/*
 * Place @n tiles in reading order over @cols columns, and let the tiles of a
 * short last row share the columns it leaves. A screen is a share of the
 * pane, not a cell of a square that happens to have one spare: three tiles
 * are two and a wide one, never two and a hole.
 */
static void workpane_place_rows(int n, int cols,
				struct fyai_workpane_grid *g)
{
	int rows, last, base, extra, span;
	int i, col = 0;

	if (cols < 1)
		cols = 1;
	rows = (n + cols - 1) / cols;
	last = n - (rows - 1) * cols;	/* tiles on the last row */
	base = cols / last;
	extra = cols % last;

	g->rows = rows;
	g->cols = cols;
	for (i = 0; i < n; i++) {
		g->place[i].row = i / cols;
		g->place[i].col = i % cols;
		g->place[i].row_span = g->place[i].col_span = 1;
	}
	if (last == cols)
		return;
	/* Fill the final row across the available columns. */
	for (i = n - last; i < n; i++) {
		span = base + (n - i <= extra ? 1 : 0);

		g->place[i].row = rows - 1;
		g->place[i].col = col;
		g->place[i].col_span = span;
		col += span;
	}
}

static int workpane_place_work(const struct fyai_workpane_manager *wm, int n,
			       enum fyai_workpane_layout layout,
			       struct fyai_workpane_grid *g)
{
	int i, cols;

	switch (layout) {
	case FYAI_WORKPANE_LAYOUT_STACK:
		g->rows = n;
		g->cols = 1;
		for (i = 0; i < n; i++) {
			g->place[i].row = i;
			g->place[i].col = 0;
			g->place[i].row_span = g->place[i].col_span = 1;
		}
		return 0;
	case FYAI_WORKPANE_LAYOUT_COLUMNS:
		cols = wm->columns > 0 ? wm->columns : 1;
		if (cols > n)
			cols = n;
		workpane_place_rows(n, cols, g);
		return 0;
	case FYAI_WORKPANE_LAYOUT_MAIN_TOP:
		/* The oldest tile spans the top row. */
		if (n < 2)
			return -1;
		g->rows = 2;
		g->cols = n - 1;
		g->place[0].row = 0;
		g->place[0].col = 0;
		g->place[0].row_span = 1;
		g->place[0].col_span = g->cols;
		for (i = 1; i < n; i++) {
			g->place[i].row = 1;
			g->place[i].col = i - 1;
			g->place[i].row_span = g->place[i].col_span = 1;
		}
		return 0;
	case FYAI_WORKPANE_LAYOUT_MAIN_LEFT:
		/* The oldest tile spans the left column. */
		if (n < 2)
			return -1;
		g->rows = n - 1;
		g->cols = 2;
		g->place[0].row = 0;
		g->place[0].col = 0;
		g->place[0].row_span = g->rows;
		g->place[0].col_span = 1;
		for (i = 1; i < n; i++) {
			g->place[i].row = i - 1;
			g->place[i].col = 1;
			g->place[i].row_span = g->place[i].col_span = 1;
		}
		return 0;
	case FYAI_WORKPANE_LAYOUT_AUTO:
		/* Use the widest readable arrangement without empty cells. */
		workpane_place_rows(n, workpane_auto_cols(wm, n), g);
		return 0;
	case FYAI_WORKPANE_LAYOUT_CUSTOM:
		break;
	}
	return -1;
}

/*
 * Place the work by the chosen layout, then give each notice a row of its
 * own beneath it. A report is full width because it is read as text, and it
 * goes under the work because the work is what the user is watching.
 */
static int workpane_place_standard(const struct fyai_workpane_manager *wm,
				   const struct fyai_workpane_tile_info *info,
				   int n, struct fyai_workpane_grid *g)
{
	int order[FYAI_WORKPANE_TILES_MAX];
	int notices[FYAI_WORKPANE_TILES_MAX];
	int i, row, work = 0, nn = 0, rc;

	memset(g, 0, sizeof(*g));
	for (i = 0; i < n; i++) {
		if (fyai_workpane_kind_is_work(info[i].kind))
			order[work++] = i;
		else
			notices[nn++] = i;
	}
	if (work > 0) {
		rc = workpane_place_work(wm, work, wm->layout, g);
		if (rc)
			return rc;
		/* The placements came back in work order; spread them back
		 * over the tiles they belong to. */
		for (i = work - 1; i >= 0; i--)
			g->place[order[i]] = g->place[i];
	}
	if (nn < 1)
		return 0;
	/* Notices span the work grid. */
	if (g->cols < 1)
		g->cols = 1;
	for (i = 0; i < nn; i++) {
		row = g->rows + i;
		g->place[notices[i]].row = row;
		g->place[notices[i]].col = 0;
		g->place[notices[i]].row_span = 1;
		g->place[notices[i]].col_span = g->cols;
		/* Reports fit their content; work tiles share remaining rows. */
		if (row < FYAI_WORKPANE_GRID_MAX)
			g->row_size[row] = FYAI_WORKPANE_TRACK_FIT;
	}
	g->rows += nn;
	if (g->rows > FYAI_WORKPANE_GRID_MAX)
		return -1;
	return 0;
}

/* Describe tiles to a policy in registration order. */
static int workpane_tile_infos(const struct fyai_workpane_manager *wm,
			       struct fyai_workpane_tile_info *info,
			       struct fyai_workpane_tile **order)
{
	struct fyai_workpane_tile *t;
	int n = 0;

	for_each_tile(t, wm) {
		/* A band is a tile like any other: it is placed, and it is
		 * what a notice is drawn in. */
		if ((!t->surface && !t->band) || n >= FYAI_WORKPANE_TILES_MAX)
			continue;
		info[n].kind = t->kind;
		info[n].preferred_rows = t->preferred_rows;
		info[n].focused = wm->focused == t->surface;
		order[n] = t;
		n++;
	}
	return n;
}

int fyai_workpane_place(struct fyai_workpane_manager *wm,
			const struct fyai_workpane_tile_info *info, int n,
			struct fyai_workpane_grid *out)
{
	struct fyai_workpane_tile_info own[FYAI_WORKPANE_TILES_MAX];
	struct fyai_workpane_tile *order[FYAI_WORKPANE_TILES_MAX];
	int rc;

	if (!wm || !out)
		return -1;
	if (!info) {
		n = workpane_tile_infos(wm, own, order);
		info = own;
	}
	if (n < 1 || n > FYAI_WORKPANE_TILES_MAX)
		return -1;
	memset(out, 0, sizeof(*out));
	if (wm->layout == FYAI_WORKPANE_LAYOUT_CUSTOM) {
		if (!wm->policy)
			return -1;
		rc = wm->policy(wm->policy_user, info, n, wm->terminal_rows,
				wm->terminal_cols, out);
	} else {
		rc = workpane_place_standard(wm, info, n, out);
	}
	/* Fall back when the policy returns no usable placement. */
	if (rc || out->rows < 1 || out->cols < 1 ||
	    out->rows > FYAI_WORKPANE_GRID_MAX ||
	    out->cols > FYAI_WORKPANE_GRID_MAX)
		return -1;
	return 0;
}

/*
 * Apply the arrangement to the pane. A layout this program states becomes an
 * explicit grid; AUTO leaves the pane to solve its own.
 */
static void workpane_apply_layout(struct fyai_workpane_manager *wm)
{
	struct fyai_workpane_tile_info info[FYAI_WORKPANE_TILES_MAX];
	struct fyai_workpane_tile *order[FYAI_WORKPANE_TILES_MAX];
	struct fyai_workpane_grid g;
	int n, i;

	n = workpane_tile_infos(wm, info, order);
	if (n < 1 || fyai_workpane_place(wm, info, n, &g)) {
		(void)fytim_workpane_set_grid(wm->pane, 0, 0);
		return;
	}
	if (fytim_workpane_set_grid(wm->pane, g.rows, g.cols) != FYTIM_OK) {
		(void)fytim_workpane_set_grid(wm->pane, 0, 0);
		return;
	}
	for (i = 0; i < g.rows; i++)
		(void)fytim_workpane_set_row_size(wm->pane, i, g.row_size[i]);
	for (i = 0; i < g.cols; i++)
		(void)fytim_workpane_set_col_size(wm->pane, i, g.col_size[i]);
	for (i = 0; i < n; i++) {
		int rs = g.place[i].row_span > 0 ? g.place[i].row_span : 1;
		int cs = g.place[i].col_span > 0 ? g.place[i].col_span : 1;

		/* A placement the grid cannot hold leaves the tile to the free
		 * cells, which is what the pane does with an unplaced tile. */
		if (order[i]->surface)
			(void)fytim_surface_set_cell(order[i]->surface,
						     g.place[i].row,
						     g.place[i].col, rs, cs);
		else
			(void)fytim_workband_set_cell(order[i]->band,
						      g.place[i].row,
						      g.place[i].col, rs, cs);
	}
}

/* Select the presentation for the tile grant. */
static enum fyai_workpane_present
workpane_present_for(const struct fyai_workpane_tile *t, int rows, int cols)
{
	const struct fyai_workpane_ladder *l = &t->ladder;

	if (rows < 1 || cols < 1)
		return FYAI_WORKPANE_PRESENT_HIDDEN;
	/* A tile without thresholds always draws its program. */
	if (l->full_rows < 1 && l->full_cols < 1 && l->output_rows < 1)
		return FYAI_WORKPANE_PRESENT_FULL;
	if ((l->full_rows < 1 || rows >= l->full_rows) &&
	    (l->full_cols < 1 || cols >= l->full_cols))
		return FYAI_WORKPANE_PRESENT_FULL;
	if (l->output_rows > 0 && rows >= l->output_rows)
		return FYAI_WORKPANE_PRESENT_OUTPUT;
	return FYAI_WORKPANE_PRESENT_HEAD;
}

enum fyai_workpane_present
fyai_workpane_present_at(const struct fyai_workpane_manager *wm,
			 const struct fytim_surface *sf, int rows, int cols)
{
	const struct fyai_workpane_tile *t = workpane_tile(wm, sf);

	if (!t)
		return FYAI_WORKPANE_PRESENT_HIDDEN;
	return workpane_present_for(t, rows, cols);
}

void fyai_workpane_set_layout(struct fyai_workpane_manager *wm,
			      enum fyai_workpane_layout layout, int columns)
{
	if (!wm)
		return;
	wm->layout = layout;
	wm->columns = columns;
	wm->layout_pending = true;
	fyai_workpane_reconcile(wm);
}

void fyai_workpane_set_position(struct fyai_workpane_manager *wm,
				enum fyai_workpane_pos pos)
{
	if (!wm)
		return;
	wm->position = pos;
	if (wm->pane)
		(void)fytim_workpane_set_place(wm->pane,
				pos == FYAI_WORKPANE_POS_BELOW ?
				FYTIM_WORKPANE_BELOW_PROMPT :
				FYTIM_WORKPANE_ABOVE_PROMPT);
	wm->layout_pending = true;
	fyai_workpane_reconcile(wm);
}

enum fyai_workpane_pos
fyai_workpane_position(const struct fyai_workpane_manager *wm)
{
	return wm ? wm->position : FYAI_WORKPANE_POS_ABOVE;
}

enum fyai_workpane_layout
fyai_workpane_layout(const struct fyai_workpane_manager *wm)
{
	return wm ? wm->layout : FYAI_WORKPANE_LAYOUT_AUTO;
}

void fyai_workpane_set_policy(struct fyai_workpane_manager *wm,
			      fyai_workpane_policy_fn fn, void *user)
{
	if (!wm)
		return;
	wm->policy = fn;
	wm->policy_user = user;
	wm->layout_pending = true;
	fyai_workpane_reconcile(wm);
}

void fyai_workpane_tile_set_ladder(struct fyai_workpane_manager *wm,
				   struct fytim_surface *sf,
				   const struct fyai_workpane_ladder *ladder)
{
	struct fyai_workpane_tile *t = workpane_tile(wm, sf);

	if (!t || !ladder)
		return;
	t->ladder = *ladder;
	wm->layout_pending = true;
}

enum fyai_workpane_present
fyai_workpane_tile_presentation(const struct fyai_workpane_manager *wm,
				const struct fytim_surface *sf)
{
	const struct fyai_workpane_tile *t = workpane_tile(wm, sf);

	return t ? t->present : FYAI_WORKPANE_PRESENT_HIDDEN;
}

	/* Record one trace entry per reconciliation pass. */
static void workpane_trace(const struct fyai_workpane_manager *wm)
{
	const struct fyai_workpane_tile *t;

	if (!fyai_diag_trace_path())
		return;
	fyai_diag_tracef("workpane", "terminal=%dx%d disposition=%s cap=%d "
			 "focused=%p zoomed=%p tiles=%d",
			 wm->terminal_rows, wm->terminal_cols,
			 workpane_disposition_name(wm->disposition),
			 wm->resolved_max_rows, (const void *)wm->focused,
			 (const void *)wm->zoomed, wm->refs);
	for_each_tile(t, wm)
		fyai_diag_tracef("workpane", "tile=%p kind=%d preferred=%d "
				 "grant=%dx%d grid=%dx%d",
				 (const void *)t->surface, (int)t->kind,
				 t->preferred_rows, t->granted_rows,
				 t->granted_cols, t->grid_rows, t->grid_cols);
}

void fyai_workpane_reconcile(struct fyai_workpane_manager *wm)
{
	struct fyai_workpane_tile *t;
	int cap, req;

	if (!wm || wm->reconciling)
		return;
	wm->reconciling = true;

	workpane_sample_size(wm);
	cap = fyai_workpane_resolve_rows(wm);
	wm->resolved_max_rows = cap;
	if (!wm->pane) {
		wm->reconciling = false;
		return;
	}

	/* Apply topology, arrangement, pane cap, and tile requests. */
	(void)fytim_workpane_set_zoom(wm->pane, wm->zoomed);
	/* A zoomed tile is one screen and needs no arrangement. */
	if (wm->zoomed)
		(void)fytim_workpane_set_grid(wm->pane, 0, 0);
	else
		workpane_apply_layout(wm);
	/* The layout library represents a full pane with an uncapped request. */
	(void)fytim_workpane_set_max_rows(wm->pane, cap);

	for_each_tile(t, wm) {
		req = t->preferred_rows;

		if (!t->surface)
			continue;
		/* A fill tile follows the pane, not its rendered content. */
		if (req < 0)
			req = wm->terminal_rows > 0 ? wm->terminal_rows : 0;
		(void)fyai_ui_surface_request_rows(t->surface, req);
		(void)fyai_ui_surface_set_max_rows(t->surface, t->max_rows);
	}

	workpane_trace(wm);
	wm->layout_pending = false;
	wm->reconciling = false;
	fyai_ui_wake(wm->ctx);
}

void fyai_workpane_layout_complete(struct fyai_workpane_manager *wm)
{
	struct fyai_workpane_tile *t;
	enum fyai_workpane_present p;
	int rows, cols;

	if (!wm || !wm->pane)
		return;
	/* Grants must not alter layout requests. */
	for_each_tile(t, wm) {
		if (!t->surface)
			continue;
		cols = fyai_ui_surface_granted_cols(t->surface);
		rows = fyai_ui_surface_granted_rows(t->surface);
		/*
		 * What a tile draws follows the size it was given, so a screen
		 * too small to read becomes the one line that says whose it
		 * is. This is told even to a tile that was granted nothing.
		 */
		p = workpane_present_for(t, rows, cols);
		if (p != t->present) {
			t->present = p;
			if (t->ops && t->ops->set_presentation)
				t->ops->set_presentation(t->owner, p);
		}
		if (cols < 1 || !t->ops || !t->ops->apply_grant)
			continue;
		t->granted_rows = rows;
		t->granted_cols = cols;
		t->ops->apply_grant(t->owner, rows, cols);
	}
}

void fyai_workpane_grid_resized(struct fyai_workpane_manager *wm,
				struct fytim_surface *sf, int rows, int cols)
{
	struct fyai_workpane_tile *t = workpane_tile(wm, sf);

	/* An acknowledgement records what the grid is. It states no intent. */
	assert(!sf || t || !wm);
	if (!t || rows <= 0 || cols <= 0)
		return;
	t->grid_rows = rows;
	t->grid_cols = cols;
}
