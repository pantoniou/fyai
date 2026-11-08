/*
 * fyai_ptyscr.c - terminal screen model for the PTY driver
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * The model answers one question: what did the user see. A frame of the UI
 * moves with relative cursor motion and not with newlines, so a byte capture
 * cannot show the rows. The emulator is libfyvterm, the one that draws the
 * terminal view; this file reads its cells and keeps the three lists that a
 * wait needs: the rows that scrolled off, the rows that an erase of the display
 * removed, and the copies.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libfyvterm.h>

#include "fyai_ptyscr.h"

/* The bytes of a character in a cell: four, and a selector that joins it. */
#define CELL_BYTES (FYVT_MAX_CHARS_PER_CELL * 4)

static void list_free(struct fyai_ptyscr_list *l)
{
	size_t i;

	for (i = 0; i < l->n; i++)
		free(l->v[i]);
	free(l->v);
	memset(l, 0, sizeof(*l));
}

static int list_add(struct fyai_ptyscr_list *l, char *str)
{
	char **v;

	if (!str)
		return -1;
	if (l->n == l->cap) {
		v = realloc(l->v, (l->cap ? l->cap * 2 : 32) * sizeof(*v));
		if (!v) {
			free(str);
			return -1;
		}
		l->v = v;
		l->cap = l->cap ? l->cap * 2 : 32;
	}
	l->v[l->n++] = str;
	return 0;
}

static size_t utf8_put(char *out, uint32_t cp)
{
	if (cp < 0x80) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = (char)(0xC0 | cp >> 6);
		out[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = (char)(0xE0 | cp >> 12);
		out[1] = (char)(0x80 | (cp >> 6 & 0x3F));
		out[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	out[0] = (char)(0xF0 | cp >> 18);
	out[1] = (char)(0x80 | (cp >> 12 & 0x3F));
	out[2] = (char)(0x80 | (cp >> 6 & 0x3F));
	out[3] = (char)(0x80 | (cp & 0x3F));
	return 4;
}

/* The end of @str without the blanks, a no-break space included. */
static size_t trim_end(const char *str, size_t len)
{
	while (len) {
		if (str[len - 1] == ' ' || str[len - 1] == '\t' ||
		    str[len - 1] == '\r' || str[len - 1] == '\n' ||
		    str[len - 1] == '\f' || str[len - 1] == '\v')
			len--;
		else if (len >= 2 && (unsigned char)str[len - 2] == 0xC2 &&
			 (unsigned char)str[len - 1] == 0xA0)
			len -= 2;
		else
			break;
	}
	return len;
}

static bool is_blank(const char *p, const char *end)
{
	return *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' ||
	       *p == '\f' || *p == '\v' ||
	       (end - p >= 2 && (unsigned char)p[0] == 0xC2 &&
		(unsigned char)p[1] == 0xA0);
}

size_t fyai_ptyscr_row_size(const struct fyai_ptyscr *s)
{
	return (size_t)s->cols * CELL_BYTES + 1;
}

/*
 * Row @r as text. An empty cell is a blank and the cell behind a wide
 * character is nothing. @offs, when given, gets the byte offset of each
 * column.
 */
static size_t row_build(const struct fyai_ptyscr *s, int r, char *buf,
			size_t *offs)
{
	struct fyvt_screen_cell cell;
	struct fyvt_pos pos = { .row = r };
	size_t len = 0;
	int c, k, next;

	for (c = 0; c < s->cols; c = next) {
		pos.col = c;
		next = c + 1;
		if (offs)
			offs[c] = len;
		if (!fyvt_screen_get_cell(s->screen, pos, &cell) ||
		    !cell.chars[0]) {
			buf[len++] = ' ';
			continue;
		}
		for (k = 0; k < FYVT_MAX_CHARS_PER_CELL && cell.chars[k]; k++)
			len += utf8_put(buf + len, cell.chars[k]);
		if (cell.width == 2) {
			if (offs && c + 1 < s->cols)
				offs[c + 1] = len;
			next = c + 2;
		}
	}
	if (offs)
		offs[s->cols] = len;
	return len;
}

size_t fyai_ptyscr_row(const struct fyai_ptyscr *s, int r, char *buf)
{
	size_t len = row_build(s, r, buf, NULL);

	len = trim_end(buf, len);
	buf[len] = '\0';
	return len;
}

/* The colour @c of a cell as "#rrggbb", or "d" for the default colour. */
static void dump_color(const struct fyai_ptyscr *s, union fyvt_color c,
		       bool bg, char *buf, size_t size)
{
	if (bg ? FYVT_COLOR_IS_DEFAULT_BG(&c) : FYVT_COLOR_IS_DEFAULT_FG(&c)) {
		snprintf(buf, size, "d");
		return;
	}
	fyvt_screen_convert_color_to_rgb(s->screen, &c);
	snprintf(buf, size, "#%02x%02x%02x", c.rgb.red, c.rgb.green,
		 c.rgb.blue);
}

/* The style of @cell as text; "-" is the default style. */
static void dump_style(const struct fyai_ptyscr *s,
		       const struct fyvt_screen_cell *cell, char *buf,
		       size_t size)
{
	const struct fyvt_screen_cell_attrs *a = &cell->attrs;
	char fg[8], bg[8];

	dump_color(s, cell->fg, false, fg, sizeof(fg));
	dump_color(s, cell->bg, true, bg, sizeof(bg));
	snprintf(buf, size, "%s/%s/%s%s%.*s%s%s%s%s%s%s", fg, bg,
		 a->bold ? "b" : "", a->italic ? "i" : "",
		 a->underline, "uu", a->blink ? "k" : "",
		 a->reverse ? "r" : "", a->conceal ? "c" : "",
		 a->strike ? "s" : "", a->dim ? "d" : "",
		 a->overline ? "o" : "");
	if (!strcmp(buf, "d/d/"))
		snprintf(buf, size, "-");
}

/* Mark in @masked each column of row text @buf under a match of @masks. */
static void dump_masks(const char *buf, const size_t *offs, int cols,
		       const regex_t *masks, size_t nmasks, bool *masked)
{
	regmatch_t m;
	size_t i, at, so, eo;
	int c;

	for (i = 0; i < nmasks; i++) {
		for (at = 0; buf[at] &&
		     !regexec(&masks[i], buf + at, 1, &m,
			      at ? REG_NOTBOL : 0); ) {
			so = at + (size_t)m.rm_so;
			eo = at + (size_t)m.rm_eo;
			for (c = 0; c < cols; c++)
				if (offs[c] >= so && offs[c] < eo)
					masked[c] = true;
			at = eo > so ? eo : eo + 1;
		}
	}
}

/* Write a row that left the top, "B|" and its text, with each match of
 * @masks as '?'. */
static int dump_text(const char *text, const regex_t *masks, size_t nmasks,
		     FILE *f)
{
	char *copy = strdup(text);
	regmatch_t m;
	size_t i, at, k;

	if (!copy)
		return -1;
	for (i = 0; i < nmasks; i++)
		for (at = 0; copy[at] &&
		     !regexec(&masks[i], copy + at, 1, &m,
			      at ? REG_NOTBOL : 0); ) {
			for (k = (size_t)m.rm_so; k < (size_t)m.rm_eo; k++)
				copy[at + k] = '?';
			at += m.rm_eo > m.rm_so ? (size_t)m.rm_eo :
			      (size_t)m.rm_eo + 1;
		}
	i = strlen(copy);
	while (i && copy[i - 1] == ' ')
		copy[--i] = '\0';
	k = fprintf(f, "B|%s\n", copy) < 0;
	free(copy);
	return k ? -1 : 0;
}

int fyai_ptyscr_dump(const struct fyai_ptyscr *s, const regex_t *masks,
		     size_t nmasks, FILE *f)
{
	struct fyvt_screen_cell cell;
	struct fyvt_pos pos;
	char *buf = NULL, *line = NULL, style[64], prev[64];
	size_t *offs = NULL, len, n, seg;
	bool *masked = NULL, styled;
	int r, c, start, row, col;

	buf = malloc(fyai_ptyscr_row_size(s));
	line = malloc(fyai_ptyscr_row_size(s));
	offs = calloc((size_t)s->cols + 1, sizeof(*offs));
	masked = calloc((size_t)s->cols, sizeof(*masked));
	if (!buf || !line || !offs || !masked)
		goto err;
	for (n = 0; n < s->scrollback.n; n++)
		if (dump_text(s->scrollback.v[n], masks, nmasks, f))
			goto err;
	for (r = 0; r < s->rows; r++) {
		len = row_build(s, r, buf, offs);
		buf[len] = '\0';
		memset(masked, 0, (size_t)s->cols * sizeof(*masked));
		dump_masks(buf, offs, s->cols, masks, nmasks, masked);
		for (n = 0, c = 0; c < s->cols; c++) {
			seg = offs[c + 1] - offs[c];
			if (masked[c] && seg) {
				line[n++] = '?';
				continue;
			}
			memcpy(line + n, buf + offs[c], seg);
			n += seg;
		}
		n = trim_end(line, n);
		line[n] = '\0';
		if (fprintf(f, "R|%s\n", line) < 0)
			goto err;
	}
	for (r = 0; r < s->rows; r++) {
		styled = false;
		start = 0;
		prev[0] = '\0';
		len = row_build(s, r, buf, offs);
		buf[len] = '\0';
		memset(masked, 0, (size_t)s->cols * sizeof(*masked));
		dump_masks(buf, offs, s->cols, masks, nmasks, masked);
		for (c = 0; c <= s->cols; c++) {
			style[0] = '\0';
			if (c < s->cols) {
				pos.row = r;
				pos.col = c;
				if (masked[c])
					snprintf(style, sizeof(style), "*");
				else if (fyvt_screen_get_cell(s->screen, pos,
							      &cell))
					dump_style(s, &cell, style,
						   sizeof(style));
				else
					snprintf(style, sizeof(style), "-");
			}
			if (c && !strcmp(style, prev))
				continue;
			if (c && strcmp(prev, "-")) {
				if (!styled && fprintf(f, "S|%d|", r) < 0)
					goto err;
				if (fprintf(f, "%s%d-%d %s", styled ? " " : "",
					    start, c - 1, prev) < 0)
					goto err;
				styled = true;
			}
			start = c;
			snprintf(prev, sizeof(prev), "%s", style);
		}
		if (styled && fputc('\n', f) == EOF)
			goto err;
	}
	/* Where a hidden cursor stands is not on the screen. */
	fyai_ptyscr_cursor(s, &row, &col);
	if ((s->cursor_hidden ? fprintf(f, "C|hidden\n") :
	     fprintf(f, "C|%d,%d\n", row, col)) < 0)
		goto err;
	free(masked);
	free(offs);
	free(line);
	free(buf);
	return 0;
err:
	free(masked);
	free(offs);
	free(line);
	free(buf);
	return -1;
}

static int cb_pushline(int cols, const struct fyvt_screen_cell *cells,
		       bool continuation, void *user)
{
	struct fyai_ptyscr *s = user;
	char *buf = malloc((size_t)cols * CELL_BYTES + 1);
	size_t len = 0;
	int c, k;

	(void)continuation;
	if (!buf) {
		s->failed = true;
		return 1;
	}
	for (c = 0; c < cols; c++) {
		if (!cells[c].chars[0] || cells[c].chars[0] == (uint32_t)-1) {
			if (cells[c].chars[0] != (uint32_t)-1)
				buf[len++] = ' ';
			continue;
		}
		for (k = 0; k < FYVT_MAX_CHARS_PER_CELL && cells[c].chars[k]; k++)
			len += utf8_put(buf + len, cells[c].chars[k]);
	}
	len = trim_end(buf, len);
	buf[len] = '\0';
	if (list_add(&s->scrollback, buf))
		s->failed = true;
	return 1;
}

/* An erase of the whole display removes the rows that were shown. */
static int cb_preerase(struct fyvt_rect rect, void *user)
{
	struct fyai_ptyscr *s = user;
	char *buf;
	int r;

	if (rect.start_row || rect.start_col || rect.end_row < s->rows ||
	    rect.end_col < s->cols)
		return 1;
	for (r = 0; r < s->rows; r++) {
		buf = malloc(fyai_ptyscr_row_size(s));
		if (!buf) {
			s->failed = true;
			break;
		}
		if (!fyai_ptyscr_row(s, r, buf))
			free(buf);
		else if (list_add(&s->erased, buf))
			s->failed = true;
	}
	s->display_erases++;
	return 1;
}

static int cb_selection(enum fyvt_selection_mask mask,
			struct fyvt_string_fragment frag, void *user)
{
	struct fyai_ptyscr *s = user;
	char *grown;

	(void)mask;
	if (frag.initial) {
		free(s->copy);
		s->copy = NULL;
		s->ncopy = 0;
	}
	grown = realloc(s->copy, s->ncopy + frag.len + 1);
	if (!grown) {
		s->failed = true;
		return 1;
	}
	s->copy = grown;
	if (frag.len)
		memcpy(s->copy + s->ncopy, frag.str, frag.len);
	s->ncopy += frag.len;
	s->copy[s->ncopy] = '\0';
	if (frag.final) {
		if (list_add(&s->clipboard, s->copy))
			s->failed = true;
		s->copy = NULL;
		s->ncopy = 0;
	}
	return 1;
}

/* The replies of the emulator are not used: the driver answers by itself. */
static void cb_output(const char *bytes, size_t len, void *user)
{
	(void)bytes;
	(void)len;
	(void)user;
}

/* The emulator reports the cursor shown or hidden as a property. */
static int cb_settermprop(enum fyvt_prop prop, union fyvt_value *val,
			  void *user)
{
	struct fyai_ptyscr *s = user;

	if (prop == FYVT_PROP_CURSORVISIBLE)
		s->cursor_hidden = !val->boolean;
	return 1;
}

static const struct fyvt_screen_callbacks screen_cbs = {
	.settermprop = cb_settermprop,
	.sb_pushline4 = cb_pushline,
	.preerase = cb_preerase,
};

static const struct fyvt_selection_callbacks selection_cbs = {
	.set = cb_selection,
};

static int emulator_start(struct fyai_ptyscr *s, int rows, int cols)
{
	struct fyvt_cfg cfg;

	fyvt_cfg_default(&cfg);
	cfg.rows = rows;
	cfg.cols = cols;
	s->vt = fyvt_create(&cfg);
	if (!s->vt)
		return -1;
	fyvt_set_utf8(s->vt, 1);
	fyvt_set_emoji_vs_wide(s->vt, !s->emoji_narrow);
	fyvt_output_set_callback(s->vt, cb_output, s);
	s->screen = fyvt_obtain_screen(s->vt);
	s->state = fyvt_obtain_state(s->vt);
	fyvt_screen_set_callbacks(s->screen, &screen_cbs, s);
	fyvt_screen_callbacks_has_pushline4(s->screen);
	fyvt_screen_callbacks_has_preerase(s->screen);
	fyvt_screen_enable_altscreen(s->screen, 1);
	fyvt_state_set_selection_callbacks(s->state, &selection_cbs, s,
					   s->selbuf, sizeof(s->selbuf));
	fyvt_screen_reset(s->screen, 1);
	/* The reset erases the display; the model starts with none seen. */
	list_free(&s->erased);
	s->display_erases = 0;
	s->rows = rows;
	s->cols = cols;
	return 0;
}

int fyai_ptyscr_init(struct fyai_ptyscr *s, int rows, int cols,
		     bool emoji_narrow)
{
	memset(s, 0, sizeof(*s));
	s->emoji_narrow = emoji_narrow;
	return emulator_start(s, rows, cols);
}

void fyai_ptyscr_destroy(struct fyai_ptyscr *s)
{
	if (s->vt)
		fyvt_destroy(s->vt);
	free(s->copy);
	list_free(&s->scrollback);
	list_free(&s->erased);
	list_free(&s->clipboard);
	memset(s, 0, sizeof(*s));
}

int fyai_ptyscr_resize(struct fyai_ptyscr *s, int rows, int cols)
{
	struct fyai_ptyscr_list clip = s->clipboard;
	bool narrow = s->emoji_narrow;

	memset(&s->clipboard, 0, sizeof(s->clipboard));
	fyai_ptyscr_destroy(s);
	s->clipboard = clip;
	s->emoji_narrow = narrow;
	return emulator_start(s, rows, cols);
}

void fyai_ptyscr_feed(struct fyai_ptyscr *s, const void *data, size_t len)
{
	fyvt_input_write(s->vt, data, len);
}

void fyai_ptyscr_cursor(const struct fyai_ptyscr *s, int *row, int *col)
{
	struct fyvt_pos pos;

	fyvt_state_get_cursorpos(s->state, &pos);
	*row = pos.row;
	*col = pos.col;
}

static bool row_has(const char *row, size_t len, const char *text, bool whole)
{
	const char *p = row, *end = row + len;

	if (whole) {
		while (p < end && is_blank(p, end))
			p += (unsigned char)*p == 0xC2 ? 2 : 1;
		return strlen(text) == (size_t)(end - p) &&
		       !memcmp(p, text, (size_t)(end - p));
	}
	return strstr(row, text) != NULL;
}

bool fyai_ptyscr_has(const struct fyai_ptyscr *s, const char *text,
		     size_t scrolled, size_t erased, bool whole)
{
	char *buf = malloc(fyai_ptyscr_row_size(s));
	bool found = false;
	size_t i, len;
	int r;

	if (!buf)
		return false;
	for (r = 0; r < s->rows && !found; r++) {
		len = fyai_ptyscr_row(s, r, buf);
		found = row_has(buf, len, text, whole);
	}
	free(buf);
	for (i = scrolled; i < s->scrollback.n && !found; i++)
		found = row_has(s->scrollback.v[i], strlen(s->scrollback.v[i]),
				text, whole);
	for (i = erased; i < s->erased.n && !found; i++)
		found = row_has(s->erased.v[i], strlen(s->erased.v[i]), text,
				whole);
	return found;
}

bool fyai_ptyscr_locate(const struct fyai_ptyscr *s, const char *text,
			int *row, int *col)
{
	char *buf = malloc(fyai_ptyscr_row_size(s));
	size_t *offs = malloc(((size_t)s->cols + 1) * sizeof(*offs));
	const char *at;
	size_t off, len;
	int r, c;

	if (!buf || !offs) {
		free(buf);
		free(offs);
		return false;
	}
	for (r = 0; r < s->rows; r++) {
		len = row_build(s, r, buf, offs);
		buf[len] = '\0';
		at = strstr(buf, text);
		if (!at)
			continue;
		off = (size_t)(at - buf);
		for (c = 0; c < s->cols && offs[c] < off; c++)
			;
		free(buf);
		free(offs);
		*row = r;
		*col = c;
		return true;
	}
	free(buf);
	free(offs);
	return false;
}
