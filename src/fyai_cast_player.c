/*
 * fyai_cast_player.c - the interactive player of a recording
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * The player takes the alternate screen. The recording is replayed on a
 * terminal view, and the cells of the view are the content of one surface that
 * fills the page. The controls are drawn over the bottom rows of the surface
 * while they show: any key or click shows them, and they go after a time with no
 * input. A click on the screen shows them, a click on a control acts, and the
 * wheel moves through the recording. A seek back replays the recording from its
 * start, which is the simple answer for now.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libfytimui.h>

#include "fyai.h"
#include "fyai_cast_file.h"
#include "fyai_cast_player.h"
#include "fyai_terminal_view.h"
#include "utils.h"

#define FYAI_MODULE FYAIEM_UNKNOWN

/* Seconds that the controls stay after the last key or click. */
#define PLAYER_CONTROLS_SECONDS 3.0
/* The longest pause that plays; a longer one is cut to this. */
#define PLAYER_IDLE_SECONDS 2.0
/* Columns of the timeline: it has one act for each, so it is bounded. */
#define PLAYER_BAR_MAX 160
/* Rows of the list of markers. */
#define PLAYER_LIST_ROWS 12

struct player_ev {
	double t;
	char kind;
	size_t off;
	size_t len;
};

struct player_button {
	const char *id;
	const char *label;
};

struct player {
	struct fyai_ctx *ctx;
	struct fytim *ft;
	struct fytim_surface *sf;
	/* The recording. */
	struct player_ev *ev;
	size_t nev, evcap;
	char *pool;
	size_t poolen, poolcap;
	size_t *marks;		/* indexes of the markers in @ev */
	size_t nmarks;
	double duration;
	int rec_rows, rec_cols;
	/* Where the replay stands. */
	struct fyai_terminal_view *view;
	size_t pos;		/* events applied */
	double now;		/* seconds into the recording */
	bool playing;
	double speed;
	double ref_wall, ref_rec;
	/* The display. */
	int w, h;
	struct fytim_cell *grid;
	bool controls;
	double hide_at;
	bool list;
	size_t list_sel;
	bool dirty;
	bool quit;
};

static const struct player_button buttons[] = {
	{ "start", "|<" },
	{ "back10", "-10s" },
	{ "back1", "-1s" },
	{ "play", "play" },
	{ "fwd1", "+1s" },
	{ "fwd10", "+10s" },
	{ "end", ">|" },
	{ "mprev", "m<" },
	{ "mnext", "m>" },
	{ "slower", "-" },
	{ "faster", "+" },
	{ "events", "events" },
	{ "quit", "quit" },
};

static void player_seek(struct player *p, double t);

static int pool_add(struct player *p, const char *s, size_t n, size_t *off)
{
	if (p->poolen + n + 1 > p->poolcap) {
		size_t cap = p->poolcap ? p->poolcap * 2 : 1 << 20;
		char *grown;

		while (cap < p->poolen + n + 1)
			cap *= 2;
		grown = realloc(p->pool, cap);
		if (!grown)
			return -1;
		p->pool = grown;
		p->poolcap = cap;
	}
	memcpy(p->pool + p->poolen, s, n);
	*off = p->poolen;
	p->poolen += n;
	p->pool[p->poolen++] = '\0';
	return 0;
}

static int player_load(struct player *p, const char *path)
{
	struct fyai_cast_file f;
	fy_generic header, ev, data, kv;
	struct player_ev *grown, *e;
	const char *text, *kind;
	size_t *marks;
	int rc = -1, op_rc;

	if (fyai_cast_file_open(p->ctx, &f, path, &header))
		return -1;
	p->rec_cols = (int)fy_get(header, "width", 80LL);
	p->rec_rows = (int)fy_get(header, "height", 24LL);
	if (p->rec_cols < 1 || p->rec_rows < 1) {
		p->rec_cols = 80;
		p->rec_rows = 24;
	}
	while (fy_is_valid(ev = fyai_cast_file_next(&f))) {
		kv = fy_get_at(ev, 1);
		kind = fy_castp(&kv, "");
		if (kind[0] != 'o' && kind[0] != 'm')
			continue;
		if (p->nev == p->evcap) {
			p->evcap = p->evcap ? p->evcap * 2 : 4096;
			grown = realloc(p->ev, p->evcap * sizeof(*grown));
			fyai_error_check(p->ctx, grown, out, "cast: out of memory");
			p->ev = grown;
		}
		e = &p->ev[p->nev];
		data = fy_get_at(ev, 2);
		text = fy_castp(&data, "");
		e->t = fy_number(fy_get_at(ev, 0), 0.0);
		e->kind = kind[0];
		e->len = strlen(text);
		op_rc = pool_add(p, text, e->len, &e->off);
		fyai_error_check(p->ctx, !op_rc, out,
				 "cast: out of memory");
		if (e->kind == 'm') {
			marks = realloc(p->marks, (p->nmarks + 1) * sizeof(*marks));
			fyai_error_check(p->ctx, marks, out, "cast: out of memory");
			p->marks = marks;
			p->marks[p->nmarks++] = p->nev;
		}
		if (e->t > p->duration)
			p->duration = e->t;
		p->nev++;
	}
	fyai_error_check(p->ctx, p->nev, out, "cast: %s has no output", path);
	rc = 0;
out:
	fyai_cast_file_close(&f);
	return rc;
}

/* A new replay terminal, empty, at the size that the recording had. */
static int player_view_reset(struct player *p)
{
	fyai_terminal_view_destroy(p->view);
	p->view = fyai_terminal_view_create(p->ctx, p->rec_rows, p->rec_cols, 0);
	p->pos = 0;
	return p->view ? 0 : -1;
}

/* Give the events up to second @t to the replay terminal. */
static void player_apply(struct player *p, double t)
{
	struct player_ev *e;

	while (p->pos < p->nev && p->ev[p->pos].t <= t) {
		e = &p->ev[p->pos++];
		if (e->kind == 'o')
			(void)fyai_terminal_view_feed(p->view, p->pool + e->off, e->len);
	}
}

static void player_seek(struct player *p, double t)
{
	if (t < 0)
		t = 0;
	if (t > p->duration)
		t = p->duration;
	if (t < p->now && player_view_reset(p)) {
		p->quit = true;
		return;
	}
	player_apply(p, t);
	p->now = t;
	p->ref_wall = fyai_monotonic_sec();
	p->ref_rec = t;
	p->dirty = true;
}

static void player_show_controls(struct player *p)
{
	p->controls = true;
	p->hide_at = fyai_monotonic_sec() + PLAYER_CONTROLS_SECONDS;
	p->dirty = true;
}

static void player_set_playing(struct player *p, bool on)
{
	p->playing = on;
	p->ref_wall = fyai_monotonic_sec();
	p->ref_rec = p->now;
	p->dirty = true;
}

/* The time of the next event that has not been applied, or a negative number. */
static double player_next_time(const struct player *p)
{
	return p->pos < p->nev ? p->ev[p->pos].t : -1.0;
}

/* Move the clock with the wall clock, and apply what it passed. */
static void player_advance(struct player *p)
{
	double wall, t, next;

	if (!p->playing)
		return;
	wall = fyai_monotonic_sec();
	t = p->ref_rec + (wall - p->ref_wall) * p->speed;
	/* A pause that the recording holds is shortened. */
	next = player_next_time(p);
	if (next > t && next - t > PLAYER_IDLE_SECONDS * p->speed) {
		p->ref_rec += next - t - PLAYER_IDLE_SECONDS * p->speed;
		t = next - PLAYER_IDLE_SECONDS * p->speed;
	}
	if (t >= p->duration) {
		t = p->duration;
		p->playing = false;
	}
	if (t > p->now) {
		player_apply(p, t);
		p->now = t;
		p->dirty = true;
	}
}

/* The index of the marker at or before time @t in the list of markers, or -1. */
static long player_marker_at(const struct player *p, double t)
{
	long i, found = -1;

	for (i = 0; i < (long)p->nmarks; i++) {
		if (p->ev[p->marks[i]].t > t)
			break;
		found = i;
	}
	return found;
}

static void player_marker_step(struct player *p, int dir)
{
	size_t i;

	if (dir > 0) {
		for (i = 0; i < p->nmarks; i++)
			if (p->ev[p->marks[i]].t > p->now + 1e-6) {
				player_seek(p, p->ev[p->marks[i]].t);
				return;
			}
		return;
	}
	for (i = p->nmarks; i-- > 0;)
		if (p->ev[p->marks[i]].t < p->now - 1e-6) {
			player_seek(p, p->ev[p->marks[i]].t);
			return;
		}
	player_seek(p, 0);
}

/* One step of output: the time of the event that follows or precedes @now. */
static void player_frame_step(struct player *p, int dir)
{
	size_t i;

	if (dir > 0) {
		for (i = p->pos; i < p->nev; i++)
			if (p->ev[i].kind == 'o') {
				player_seek(p, p->ev[i].t);
				return;
			}
		return;
	}
	for (i = p->pos; i-- > 0;)
		if (p->ev[i].kind == 'o' && p->ev[i].t < p->now) {
			player_seek(p, p->ev[i].t - 1e-6);
			return;
		}
	player_seek(p, 0);
}

static void format_time(char *out, size_t size, double t)
{
	/* Up to 99:59:59, so that the text fits the callers' buffers. */
	unsigned long s = t > 0 ? (unsigned long)t : 0;

	if (s > 359999)
		s = 359999;
	if (s >= 3600)
		snprintf(out, size, "%lu:%02lu:%02lu", s / 3600, (s / 60) % 60, s % 60);
	else
		snprintf(out, size, "%02lu:%02lu", s / 60, s % 60);
}

/* ---- drawing -------------------------------------------------------------- */

static void cell_from_term(const struct fyai_term_cell *src, struct fytim_cell *dst)
{
	int i;

	memset(dst, 0, sizeof(*dst));
	for (i = 0; i < FYAI_TERM_CELL_CHARS && i < FYTIM_CELL_CHARS; i++)
		dst->chars[i] = src->chars[i];
	dst->fg = src->fg.is_default ? FYTIM_COLOR_DEFAULT :
		((uint32_t)src->fg.r << 16) | ((uint32_t)src->fg.g << 8) | src->fg.b;
	dst->bg = src->bg.is_default ? FYTIM_COLOR_DEFAULT :
		((uint32_t)src->bg.r << 16) | ((uint32_t)src->bg.g << 8) | src->bg.b;
	if (src->bold)
		dst->attrs |= FYTIM_ATTR_BOLD;
	if (src->italic)
		dst->attrs |= FYTIM_ATTR_ITALIC;
	if (src->underline)
		dst->attrs |= FYTIM_ATTR_UNDERLINE;
	if (src->reverse)
		dst->attrs |= FYTIM_ATTR_REVERSE;
	if (src->strike)
		dst->attrs |= FYTIM_ATTR_STRIKE;
	dst->width = (unsigned char)(src->width > 1 ? 2 : 1);
}

/* Draw styled text at a cell of the grid. */
static void grid_text(struct player *p, int row, int col, int width, const char *sgr)
{
	if (row < 0 || row >= p->h || col >= p->w)
		return;
	(void)fytim_cells_draw_text(p->grid, p->h, p->w, row, col,
				    width < p->w - col ? width : p->w - col, 1,
				    sgr, strlen(sgr));
}

/* The regions of this frame: the screen, and the acts of the controls. */
struct frame_regions {
	struct fytim_page_region r[FYTIM_PAGE_REGIONS_MAX];
	char ids[FYTIM_PAGE_REGIONS_MAX][32];
	size_t n;
};

static void region_add(struct frame_regions *fr, const char *id,
		       enum fytim_page_region_kind kind, int row, int col,
		       int width, int height)
{
	struct fytim_page_region *r;

	if (fr->n >= FYTIM_PAGE_REGIONS_MAX)
		return;
	snprintf(fr->ids[fr->n], sizeof(fr->ids[0]), "%s", id);
	r = &fr->r[fr->n++];
	r->id = fr->ids[fr->n - 1];
	r->kind = kind;
	r->row = row;
	r->col = col;
	r->width = width;
	r->height = height;
}

/* The columns of the timeline: what the labels and the spaces leave. */
static int timeline_width(const struct player *p)
{
	char left[16], right[16];
	int bar;

	format_time(left, sizeof(left), p->now);
	format_time(right, sizeof(right), p->duration);
	bar = p->w - (int)strlen(left) - (int)strlen(right) - 6;
	return bar > PLAYER_BAR_MAX ? PLAYER_BAR_MAX : bar;
}

/* The timeline on row @row: the elapsed part reversed, one act for each column. */
static void draw_timeline(struct player *p, struct frame_regions *fr, int row)
{
	char left[16], right[16], line[1024], id[32];
	int bar, filled, col0, i, used;
	size_t n;

	format_time(left, sizeof(left), p->now);
	format_time(right, sizeof(right), p->duration);
	bar = timeline_width(p);
	if (bar < 4)
		return;
	filled = p->duration > 0 ? (int)(p->now / p->duration * bar) : 0;
	col0 = (int)strlen(left) + 3;
	n = (size_t)snprintf(line, sizeof(line), "\x1b[7m %s \x1b[0m ", left);
	used = 0;
	for (i = 0; i < bar && n + 16 < sizeof(line); i++) {
		/* A marker shows as a bar in the track. */
		bool mark = false;
		double t0 = p->duration * i / bar, t1 = p->duration * (i + 1) / bar;
		size_t m;

		for (m = 0; m < p->nmarks && !mark; m++)
			mark = p->ev[p->marks[m]].t >= t0 && p->ev[p->marks[m]].t < t1;
		n += (size_t)snprintf(line + n, sizeof(line) - n, "%s%s",
				      i < filled ? "\x1b[7m" : "\x1b[2m",
				      mark ? "|" : (i < filled ? " " : "-"));
		n += (size_t)snprintf(line + n, sizeof(line) - n, "\x1b[0m");
		used++;
		snprintf(id, sizeof(id), "seek:%d", i);
		region_add(fr, id, FYTIM_PAGE_ACT, row, col0 + i, 1, 1);
	}
	snprintf(line + n, sizeof(line) - n, " \x1b[7m %s \x1b[0m", right);
	grid_text(p, row, 0, p->w, line);
	(void)used;
}

/* The buttons on row @row, each an act, and the speed. */
static void draw_buttons(struct player *p, struct frame_regions *fr, int row)
{
	char line[1024], tail[96];
	long m = player_marker_at(p, p->now);
	size_t i, n = 0;
	int col = 1;

	for (i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
		int len = (int)strlen(buttons[i].label) + 2;

		if (col + len > p->w)
			break;
		n += (size_t)snprintf(line + n, sizeof(line) - n, "\x1b[7m %s \x1b[0m ",
				      !strcmp(buttons[i].id, "play") && !p->playing ?
				      "play" : !strcmp(buttons[i].id, "play") ?
				      "pause" : buttons[i].label);
		region_add(fr, buttons[i].id, FYTIM_PAGE_ACT, row, col, len, 1);
		col += len + 1;
	}
	grid_text(p, row, 1, p->w - 1, line);
	snprintf(tail, sizeof(tail), "%gx", p->speed);
	if (col + (int)strlen(tail) + 2 < p->w)
		grid_text(p, row, col, (int)strlen(tail) + 1, tail);
	if (m >= 0) {
		const char *mt = p->pool + p->ev[p->marks[m]].off;
		char buf[160];

		snprintf(buf, sizeof(buf), "\x1b[2m%.150s\x1b[0m", mt);
		if (col + (int)strlen(tail) + 4 < p->w)
			grid_text(p, row, col + (int)strlen(tail) + 2,
				  p->w - (col + (int)strlen(tail) + 2), buf);
	}
}

/* The list of markers over the middle of the screen. */
static void draw_list(struct player *p, struct frame_regions *fr)
{
	int rows = PLAYER_LIST_ROWS, top, i;
	size_t first, idx;
	char line[512], id[32], tm[16];

	if (rows > p->h - 4)
		rows = p->h - 4;
	if (rows < 3 || p->nmarks == 0)
		return;
	top = (p->h - rows) / 2;
	first = p->list_sel >= (size_t)rows / 2 ? p->list_sel - (size_t)rows / 2 : 0;
	if (first + (size_t)rows > p->nmarks)
		first = p->nmarks > (size_t)rows ? p->nmarks - (size_t)rows : 0;
	for (i = 0; i < rows; i++) {
		idx = first + (size_t)i;
		if (idx >= p->nmarks) {
			snprintf(line, sizeof(line), "\x1b[7m%*s\x1b[0m", p->w - 4, "");
		} else {
			format_time(tm, sizeof(tm), p->ev[p->marks[idx]].t);
			snprintf(line, sizeof(line), "%s %s %-9s %.400s\x1b[0m",
				 idx == p->list_sel ? "\x1b[1;7m" : "\x1b[7m",
				 idx == p->list_sel ? ">" : " ", tm,
				 p->pool + p->ev[p->marks[idx]].off);
			snprintf(id, sizeof(id), "mark:%zu", idx);
			region_add(fr, id, FYTIM_PAGE_ACT, top + i, 2, p->w - 4, 1);
		}
		grid_text(p, top + i, 2, p->w - 4, line);
	}
}

static void player_draw(struct player *p)
{
	struct frame_regions *fr;
	struct fyai_term_cell src;
	struct fytim_cell blank = { .fg = FYTIM_COLOR_DEFAULT,
				    .bg = FYTIM_COLOR_DEFAULT, .width = 1 };
	char *rows;
	enum fytim_result res;
	int r, c, crow = 0, ccol = 0;
	bool cvis = false;

	fr = calloc(1, sizeof(*fr));
	if (!fr)
		return;
	for (r = 0; r < p->h; r++)
		for (c = 0; c < p->w; c++) {
			if (r < p->rec_rows && c < p->rec_cols &&
			    fyai_terminal_view_cell(p->view, r, c, &src))
				cell_from_term(&src, &p->grid[r * p->w + c]);
			else
				p->grid[r * p->w + c] = blank;
		}
	region_add(fr, "screen", FYTIM_PAGE_SLOT, 0, 0, p->w, p->h);
	if (p->controls || !p->playing) {
		draw_timeline(p, fr, p->h - 2);
		draw_buttons(p, fr, p->h - 1);
	}
	if (p->list)
		draw_list(p, fr);
	for (r = 0; r < p->h; r++)
		(void)fytim_surface_put_row(p->sf, r, &p->grid[r * p->w], p->w);
	fyai_terminal_view_cursor(p->view, &crow, &ccol, &cvis);
	(void)fytim_surface_set_cursor(p->sf, crow, ccol,
				       cvis && !p->controls && !p->list);
	rows = malloc((size_t)p->h + 1);
	if (rows) {
		memset(rows, '\n', (size_t)p->h);
		res = fytim_page_set(p->ft, rows, (size_t)(p->h > 1 ? p->h - 1 : 0),
				     fr->r, fr->n);
		if (res != FYTIM_OK)
			fyai_warning(p->ctx, "cast: the page was rejected: %s",
				     fytim_result_string(res));
		free(rows);
	}
	free(fr);
	p->dirty = false;
}

/* ---- input ---------------------------------------------------------------- */

static void player_act(struct player *p, const char *id)
{
	int n;

	if (!strncmp(id, "seek:", 5)) {
		n = atoi(id + 5);
		/* The same width as the timeline that was drawn. */
		if (timeline_width(p) > 0)
			player_seek(p, p->duration * ((double)n + 0.5) /
				       timeline_width(p));
	} else if (!strncmp(id, "mark:", 5)) {
		size_t i = (size_t)atoi(id + 5);

		if (i < p->nmarks) {
			p->list_sel = i;
			player_seek(p, p->ev[p->marks[i]].t);
			p->list = false;
		}
	} else if (!strcmp(id, "start")) {
		player_seek(p, 0);
	} else if (!strcmp(id, "end")) {
		player_seek(p, p->duration);
	} else if (!strcmp(id, "back10")) {
		player_seek(p, p->now - 10);
	} else if (!strcmp(id, "back1")) {
		player_seek(p, p->now - 1);
	} else if (!strcmp(id, "fwd1")) {
		player_seek(p, p->now + 1);
	} else if (!strcmp(id, "fwd10")) {
		player_seek(p, p->now + 10);
	} else if (!strcmp(id, "play")) {
		if (!p->playing && p->now >= p->duration)
			player_seek(p, 0);
		player_set_playing(p, !p->playing);
	} else if (!strcmp(id, "mprev")) {
		player_marker_step(p, -1);
	} else if (!strcmp(id, "mnext")) {
		player_marker_step(p, 1);
	} else if (!strcmp(id, "fstep+")) {
		player_set_playing(p, false);
		player_frame_step(p, 1);
	} else if (!strcmp(id, "fstep-")) {
		player_set_playing(p, false);
		player_frame_step(p, -1);
	} else if (!strcmp(id, "slower")) {
		if (p->speed > 0.25)
			p->speed /= 2;
		player_set_playing(p, p->playing);
	} else if (!strcmp(id, "faster")) {
		if (p->speed < 64)
			p->speed *= 2;
		player_set_playing(p, p->playing);
	} else if (!strcmp(id, "events")) {
		long m = player_marker_at(p, p->now);

		p->list = !p->list;
		p->list_sel = m >= 0 ? (size_t)m : 0;
		p->dirty = true;
	} else if (!strcmp(id, "enter")) {
		if (p->list && p->list_sel < p->nmarks) {
			player_seek(p, p->ev[p->marks[p->list_sel]].t);
			p->list = false;
		}
	} else if (!strcmp(id, "up") || !strcmp(id, "down")) {
		if (p->list) {
			if (!strcmp(id, "up") && p->list_sel > 0)
				p->list_sel--;
			else if (!strcmp(id, "down") && p->list_sel + 1 < p->nmarks)
				p->list_sel++;
			p->dirty = true;
		} else {
			player_act(p, !strcmp(id, "up") ? "faster" : "slower");
		}
	} else if (!strcmp(id, "controls")) {
		p->controls = !p->controls;
		p->dirty = true;
	} else if (!strcmp(id, "quit")) {
		/* Escape closes the list before it leaves. */
		if (p->list) {
			p->list = false;
			p->dirty = true;
		} else {
			p->quit = true;
		}
	}
}

static int player_keys(struct player *p)
{
	static const struct fytim_keymap_entry keys[] = {
		{ "Space", "play" }, { "Left", "back1" }, { "Right", "fwd1" },
		{ "Shift-Left", "back10" }, { "Shift-Right", "fwd10" },
		{ "Up", "up" }, { "Down", "down" },
		{ "[", "mprev" }, { "]", "mnext" },
		{ ",", "fstep-" }, { ".", "fstep+" },
		{ "Home", "start" }, { "End", "end" },
		{ "e", "events" }, { "Enter", "enter" },
		{ "c", "controls" }, { "q", "quit" }, { "Escape", "quit" },
		{ "-", "slower" }, { "+", "faster" },
	};

	if (fytim_mode_bind(p->ft, "player", keys, sizeof(keys) / sizeof(keys[0])) !=
	    FYTIM_OK)
		return -1;
	(void)fytim_mode_set_parent(p->ft, "player", NULL);
	return fytim_set_mode(p->ft, "player") == FYTIM_OK ? 0 : -1;
}

static void player_events(struct player *p)
{
	struct fytim_event ev;
	char id[48];
	int w, h;

	while (fytim_next_event(p->ft, &ev)) {
		switch (ev.type) {
		case FYTIM_EVENT_KEY:
		case FYTIM_EVENT_ACT:
			snprintf(id, sizeof(id), "%.*s", (int)ev.text_len, ev.text ? ev.text : "");
			if (strcmp(id, "quit") || p->controls || p->list || ev.type == FYTIM_EVENT_KEY)
				player_show_controls(p);
			player_act(p, id);
			break;
		case FYTIM_EVENT_FOCUS_PROMPT:
			/* A click on the screen shows the controls. */
			player_show_controls(p);
			break;
		case FYTIM_EVENT_INTERRUPT:
		case FYTIM_EVENT_QUIT:
			p->quit = true;
			break;
		case FYTIM_EVENT_SCROLLBACK:
			/* The wheel moves through the recording: a turn is a second. */
			player_show_controls(p);
			player_seek(p, p->now - (double)ev.delta / 3.0);
			break;
		case FYTIM_EVENT_RESIZE:
			w = ev.width;
			h = ev.height;
			if (w > 0 && h > 0 && (w != p->w || h != p->h)) {
				struct fytim_cell *grid = realloc(p->grid,
					(size_t)w * (size_t)h * sizeof(*grid));

				if (grid) {
					p->grid = grid;
					p->w = w;
					p->h = h;
					(void)fytim_surface_resize(p->sf, h, w);
					p->dirty = true;
				}
			}
			break;
		default:
			break;
		}
	}
}

/* ---- the loop ------------------------------------------------------------- */

static int player_wait_ms(struct player *p)
{
	double wall = fyai_monotonic_sec(), next, due;
	int lib = fytim_poll_timeout_ms(p->ft), ms = 1000;

	if (p->dirty)
		return 0;
	if (p->playing) {
		next = player_next_time(p);
		if (next >= 0) {
			due = p->ref_wall + (next - p->ref_rec) / p->speed - wall;
			/* A pause that is cut ends sooner. */
			if (due > PLAYER_IDLE_SECONDS)
				due = PLAYER_IDLE_SECONDS;
			ms = due <= 0 ? 0 : (int)(due * 1000.0) + 1;
		} else {
			ms = 50;
		}
	}
	if (p->controls && p->playing) {
		due = p->hide_at - wall;
		if (due <= 0)
			return 0;
		if ((int)(due * 1000.0) + 1 < ms)
			ms = (int)(due * 1000.0) + 1;
	}
	if (lib >= 0 && lib < ms)
		ms = lib;
	return ms;
}

int fyai_cast_play_interactive(struct fyai_ctx *ctx, const char *path,
			       double speed)
{
	struct fytim_cfg cfg;
	struct player *p = calloc(1, sizeof(*p));
	struct pollfd pfd;
	int rc = -1, w = 0, h = 0, fd, op_rc;

	fyai_error_check(ctx, p, out, "cast: out of memory");
	p->ctx = ctx;
	p->speed = speed > 0 ? speed : 1.0;
	if (player_load(p, path))
		goto out;
	op_rc = player_view_reset(p);
	fyai_error_check(ctx, !op_rc, out,
			 "cast: cannot make the replay terminal");
	fytim_cfg_default(&cfg);
	cfg.title = "fyai cast";
	cfg.mouse = true;
	cfg.screen = FYTIM_SCREEN_ALT;
	p->ft = fytim_create(&cfg);
	fyai_error_check(ctx, p->ft, out, "cast: cannot open the terminal UI");
	(void)fytim_set_prompt_enabled(p->ft, false);
	(void)fytim_set_idle_poll_ms(p->ft, 50);
	(void)fytim_size(p->ft, &w, &h);
	p->w = w > 0 ? w : 80;
	p->h = h > 1 ? h : 24;
	p->sf = fytim_surface_open(p->ft, p->h, p->w);
	fyai_error_check(ctx, p->sf, out, "cast: cannot open the screen");
	(void)fytim_surface_set_max_rows(p->sf, 0);
	op_rc = fytim_surface_bind(p->sf, "screen");
	fyai_error_check(ctx, op_rc == FYTIM_OK, out,
			 "cast: cannot bind the screen");
	p->grid = calloc((size_t)p->w * (size_t)p->h, sizeof(*p->grid));
	fyai_error_check(ctx, p->grid, out, "cast: out of memory");
	op_rc = player_keys(p);
	fyai_error_check(ctx, !op_rc, out, "cast: cannot bind the keys");
	p->playing = true;
	p->ref_wall = fyai_monotonic_sec();
	player_show_controls(p);
	fd = fytim_poll_fd(p->ft);
	while (!p->quit) {
		pfd.fd = fd;
		pfd.events = POLLIN;
		(void)poll(&pfd, fd >= 0 ? 1 : 0, player_wait_ms(p));
		if (fytim_pump(p->ft) != FYTIM_OK)
			break;
		player_events(p);
		player_advance(p);
		if (p->controls && p->playing && fyai_monotonic_sec() >= p->hide_at) {
			p->controls = false;
			p->dirty = true;
		}
		if (p->dirty) {
			player_draw(p);
			(void)fytim_pump(p->ft);
		}
	}
	rc = 0;
out:
	if (p) {
		if (p->sf)
			fytim_surface_close(p->sf);
		if (p->ft)
			fytim_destroy(p->ft);
		fyai_terminal_view_destroy(p->view);
		free(p->grid);
		free(p->ev);
		free(p->pool);
		free(p->marks);
		free(p);
	}
	return rc;
}
