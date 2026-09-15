/* SPDX-License-Identifier: MIT */
/*
 * fyai_page.c - the live screen as one UI Markdown page.
 *
 * The page states what the band stack draws in C: the transcript tail, the
 * work pane, the header, the prompt between two rules and the status. The
 * components that draw a slot are the terminal library's; this file writes
 * the source, renders it and hands the rows and the regions over.
 */

#define FYAI_MODULE FYAIEM_DISPLAY

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libfymd4c.h>
#include <libfytimui.h>

#include "fyai.h"
#include "fyai_config.h"
#include "fyai_markdown.h"
#include "fyai_page.h"
#include "fyai_schema.h"
#include "fyai_terminal.h"
#include "fyai_workpane.h"

bool fyai_page_requested(const struct fyai_cfg *cfg)
{
	return cfg && cfg->renderer && !strcmp(cfg->renderer, "page");
}

bool fyai_page_supported(void)
{
	return true;
}

const struct fyai_page_action *
fyai_page_action_find(const struct fyai_page_action *actions, size_t n,
		      const char *name, size_t len)
{
	size_t i;

	for (i = 0; actions && name && i < n; i++)
		if (strlen(actions[i].name) == len &&
		    !strncmp(actions[i].name, name, len))
			return &actions[i];
	return NULL;
}
struct fyai_page {
	struct fyai_ctx *ctx;
	struct fymd_renderer *renderer;	/* UI renderer at @cols */
	int cols;
	struct response_buffer source;
	/* The page drawn as cells: a surface in the "canvas" slot under every
	 * other slot, and the grid it is drawn from. */
	struct fytim_surface *canvas;
	struct fytim_cell *cells;
	int cells_rows, cells_cols;
	size_t cells_alloc;
	struct response_buffer blank;	/* the blank rows the page is set with */
	/* The ids of the acts of the heads, which the page is set with. */
	char act_ids[FYTIM_PAGE_REGIONS_MAX][FYTIM_PAGE_ID_MAX + 1];
	/* The document the page transcribes: the embedded one, or the file of
	 * display/page, which lives in @doc_gb. @doc_path is the setting the
	 * page was made for, and @rejected says why its file was not used. */
	struct fy_generic_builder *doc_gb;
	fy_generic doc;
	char *doc_path;
	char *rejected;
	/* The last frame, for /page: its state lives in @frame_gb until the
	 * next frame, and its regions are copied. */
	struct fy_generic_builder *frame_gb;
	fy_generic last_state;
	size_t last_nregions;
	struct {
		char id[FYTIM_PAGE_ID_MAX + 1];
		bool act;
		int row, col, width, height;
	} last_regions[FYTIM_PAGE_REGIONS_MAX];
};

/*
 * Append @text to a row of Markdown: no tag of UI Markdown can open in it, a
 * newline cannot end the row, and SGR is removed, because the renderer would
 * strip it from the source anyway. At the start of a row, *@row_start is set
 * and leading blanks are removed: four of them make an indented code block.
 * *@row_start is cleared once the row holds text.
 */
static int page_append_text(struct response_buffer *out, const char *text,
			    bool *row_start)
{
	char *copy, *p, *o;
	int rc;

	if (fy_str_empty(text))
		return 0;
	copy = markdown_ui_escape(text);
	if (!copy)
		return -1;
	for (p = o = copy; *p; p++) {
		if (*p == '\x1b' && p[1] == '[') {
			p += 2;
			while (*p && !(*p >= '@' && *p <= '~'))
				p++;
			if (!*p)
				break;
			continue;
		}
		if (*row_start && (*p == ' ' || *p == '\t' || *p == '\n' ||
				   *p == '\r'))
			continue;
		*row_start = false;
		*o++ = (*p == '\n' || *p == '\r') ? ' ' : *p;
	}
	*o = '\0';
	rc = response_buffer_append(out, copy);
	free(copy);
	return rc;
}

static int page_slot(struct response_buffer *out, const char *id, int rows)
{
	char buf[96];

	if (rows < 1)
		return 0;
	snprintf(buf, sizeof(buf), "<fy-slot id=\"%s\" height=\"%d\"/>\n\n",
		 id, rows);
	return response_buffer_append(out, buf);
}

/* A space that Markdown does not remove at the start of a row: a margin. */
#define PAGE_SPACE "&#32;"

/* The header row and the blank row above it, as the band stack draws them. */
#define FYAI_PAGE_HEADER_ROWS 2

/*
 * @text in @gb with its UI tags escaped. The header and the status are UI
 * Markdown that hold values fyai did not write, so no value can open a tag of
 * the page. Each is one row: a line break becomes a space, as the band stack
 * folds it. Leading blanks go, as Markdown removes them from the band stack.
 * A row that cannot be escaped is drawn empty.
 */
static fy_generic page_escaped(struct fy_generic_builder *gb, const char *text)
{
	fy_generic v;
	char *escaped, *p;

	while (text && (*text == ' ' || *text == '\t'))
		text++;
	if (fy_str_empty(text))
		return fy_value(gb, "");
	escaped = markdown_ui_escape(text);
	if (!escaped)
		return fy_value(gb, "");
	for (p = escaped; *p; p++)
		if (*p == '\n' || *p == '\r')
			*p = ' ';
	v = fy_value(gb, escaped);
	free(escaped);
	return v;
}

int fyai_page_chrome_rows(const struct fyai_page_state *st)
{
	int rows = 0;

	if (!st)
		return 0;
	if (st->prompt_rows > 0)
		/* the header under its blank row, the prompt with its two framing
		 * rows, two status rows */
		rows = FYAI_PAGE_HEADER_ROWS + st->prompt_rows + 2 + 2;
	else
		rows = (!fy_str_empty(st->header) || !fy_str_empty(st->elapsed)) *
		       FYAI_PAGE_HEADER_ROWS +
		       (st->completion || !fy_str_empty(st->hint)) +
		       !fy_str_empty(st->status);
	if (st->pane_rows > 0 && !fy_str_empty(st->cap))
		rows++;
	/* A question: its row, who asks, each option, and the hint. */
	if (st->ask_question)
		rows += 2 + !fy_str_empty(st->ask_from) + (int)st->ask_noptions;
	return rows + st->note_nlines;
}

void fyai_page_fit(struct fyai_page_state *st, int height)
{
	int left;

	if (!st || height <= 0)
		return;
	/* A popup covers the page: its heading, then its rows. */
	if (st->fullscreen && st->popup_title) {
		st->popup_rows = height > 1 ? height - 1 : 0;
		st->pane_rows = 0;
		st->tail_rows = 0;
		st->transcript_rows = 0;
		return;
	}
	left = height - fyai_page_chrome_rows(st);
	/* The work outranks the tail, which shows its last rows. */
	if (st->pane_rows > left) {
		st->pane_rows = left > 0 ? left : (st->pane_rows > 0);
		st->tail_rows = 0;
		st->transcript_rows = 0;
		return;
	}
	left -= st->pane_rows;
	if (st->tail_rows > left)
		st->tail_rows = left > 0 ? left : 0;
	/* A fullscreen page gives its transcript view what the tail leaves. */
	if (st->fullscreen)
		st->transcript_rows = left - st->tail_rows;
}

static int page_repeat(struct response_buffer *out, const char *s, int n)
{
	while (n-- > 0)
		if (response_buffer_append(out, s))
			return -1;
	return 0;
}

static int page_style(struct response_buffer *out, const char *sgr)
{
	return fy_str_empty(sgr) ? 0 : response_buffer_append(out, sgr);
}

/*
 * The gutter of the status row: the activity mark without its colour, padded
 * to @cols, or a blank gutter. The mark keeps the status text in its column.
 */
static int page_gutter(struct response_buffer *out, const char *activity,
		       int cols)
{
	struct response_buffer mark = {0};
	bool row_start = true;
	int width = 0, rc;

	if (cols < 1)
		return 0;
	rc = page_append_text(&mark, activity, &row_start);
	if (!rc && mark.data)
		width = (int)fymd_str_width(mark.data, mark.len);
	if (!rc && width > 0 && width <= cols)
		rc = response_buffer_append(out, mark.data) ||
		     page_repeat(out, PAGE_SPACE, cols - width);
	else if (!rc)
		rc = page_repeat(out, PAGE_SPACE, cols);
	free(mark.data);
	return rc;
}

/*
 * The rows of the tracks of @g for @cells, as the terminal library solves a
 * pane: a tile that spans rows asks each of them for its share.
 */
static void page_grid_rows(const struct fyai_workpane_grid *g,
			   const struct fyai_page_cell *cells, int n,
			   int height, int *h)
{
	int natural[FYAI_WORKPANE_GRID_MAX];
	int i, r, rs, nat, fixed = 0, flex = 0, want = 0, left, sum, big, give;

	for (r = 0; r < g->rows; r++)
		natural[r] = 0;
	for (i = 0; i < n; i++) {
		rs = cells[i].row_span > 0 ? cells[i].row_span : 1;
		if (cells[i].row < 0 || cells[i].row >= g->rows)
			continue;
		nat = (cells[i].rows + rs - 1) / rs;
		for (r = cells[i].row; r < cells[i].row + rs && r < g->rows; r++)
			if (nat > natural[r])
				natural[r] = nat;
	}
	for (r = 0; r < g->rows; r++) {
		if (g->row_size[r] == FYAI_WORKPANE_TRACK_FIT) {
			h[r] = natural[r] > 0 ? natural[r] : 1;
			fixed += h[r];
		} else if (g->row_size[r] > 0) {
			h[r] = g->row_size[r];
			fixed += h[r];
		} else {
			h[r] = natural[r] > 0 ? natural[r] : 1;
			want += h[r];
			flex++;
		}
	}
	if (height <= 0)
		return;
	/* Sized rows that overrun the height are cut back, newest first. */
	for (r = g->rows - 1; r >= 0 && fixed + flex > height; r--)
		while (h[r] > 1 && fixed + flex > height &&
		       g->row_size[r] != 0) {
			h[r]--;
			fixed--;
		}
	if (flex < 1)
		return;
	left = height - fixed;
	if (left < flex)
		left = flex;
	if (want == left)
		return;
	for (r = 0; r < g->rows; r++) {
		if (g->row_size[r] != 0)
			continue;
		h[r] = want > 0 ? (int)(((long)h[r] * left) / want) : 1;
		if (h[r] < 1)
			h[r] = 1;
	}
	/* Rounding leaves a remainder; settle it on the tallest row. */
	for (;;) {
		sum = 0;
		big = -1;
		for (r = 0; r < g->rows; r++)
			if (g->row_size[r] == 0) {
				sum += h[r];
				if (big < 0 || h[r] > h[big])
					big = r;
			}
		if (big < 0 || sum == left)
			break;
		if (sum < left) {
			h[big]++;
			continue;
		}
		give = -1;
		for (r = 0; r < g->rows; r++)
			if (g->row_size[r] == 0 && h[r] > 1 &&
			    (give < 0 || h[r] > h[give]))
				give = r;
		if (give < 0)
			break;
		h[give]--;
	}
}

/* Whether @cell stands inside @g. */
static bool page_cell_placed(const struct fyai_workpane_grid *g,
			     const struct fyai_page_cell *cell)
{
	int rs = cell->row_span > 0 ? cell->row_span : 1;
	int cs = cell->col_span > 0 ? cell->col_span : 1;

	return cell->row >= 0 && cell->col >= 0 && cell->row + rs <= g->rows &&
	       cell->col + cs <= g->cols;
}

/* The tallest head of the tiles of @cells that start on the row @row. */
static int page_row_head(const struct fyai_workpane_grid *g,
			 const struct fyai_page_cell *cells, int n, int row)
{
	int i, head = 0;

	for (i = 0; i < n; i++)
		if (cells[i].row == row && page_cell_placed(g, &cells[i]) &&
		    cells[i].head_rows > head)
			head = cells[i].head_rows;
	return head;
}

int fyai_page_grid(const struct fyai_workpane_grid *g,
		   const struct fyai_page_cell *cells, int n, int height,
		   const char *sep, int sep_cols, struct response_buffer *out,
		   int *rowsp)
{
	int h[FYAI_WORKPANE_GRID_MAX];
	char buf[256];
	int i, r, c, rows = 0, span, rs, cs, head;

	if (!g || g->rows < 1 || g->cols < 1 ||
	    g->rows > FYAI_WORKPANE_GRID_MAX ||
	    g->cols > FYAI_WORKPANE_GRID_MAX || (n > 0 && !cells))
		return -1;
	page_grid_rows(g, cells, n, height, h);
	for (r = 0; r < g->rows; r++)
		rows += h[r];
	if (rowsp)
		*rowsp = rows;
	if (!out)
		return 0;

	if (response_buffer_append(out, "<fy-grid rows=\""))
		return -1;
	for (r = 0; r < g->rows; r++) {
		snprintf(buf, sizeof(buf), "%s%d", r ? "," : "", h[r]);
		if (response_buffer_append(out, buf))
			return -1;
	}
	if (response_buffer_append(out, "\" cols=\""))
		return -1;
	for (c = 0; c < g->cols; c++) {
		if (g->col_size[c] > 0)
			snprintf(buf, sizeof(buf), "%s%d", c ? "," : "",
				 g->col_size[c]);
		else
			snprintf(buf, sizeof(buf), "%s*", c ? "," : "");
		if (response_buffer_append(out, buf))
			return -1;
	}
	/* A separator that could end the attribute is not drawn. */
	if (fy_str_empty(sep) || sep_cols < 1 || strchr(sep, '"'))
		sep_cols = 0;
	snprintf(buf, sizeof(buf), "\" gap=\"%d\"", sep_cols);
	if (response_buffer_append(out, buf) ||
	    (sep_cols > 0 &&
	     (response_buffer_append(out, " sep=\"") ||
	      response_buffer_append(out, sep) ||
	      response_buffer_append(out, "\""))) ||
	    response_buffer_append(out, ">\n"))
		return -1;

	for (i = 0; i < n; i++) {
		if (!page_cell_placed(g, &cells[i]))
			continue;
		rs = cells[i].row_span > 0 ? cells[i].row_span : 1;
		cs = cells[i].col_span > 0 ? cells[i].col_span : 1;
		for (r = cells[i].row, span = 0; r < cells[i].row + rs; r++)
			span += h[r];
		/* The screens of one row stand level under the tallest head. A
		 * tile of text keeps its own chrome and takes no head. */
		head = cells[i].band ? 0 :
		       page_row_head(g, cells, n, cells[i].row);
		if (head > span - 1)
			head = span - 1;
		snprintf(buf, sizeof(buf),
			 "<fy-cell row=\"%d\" col=\"%d\" rowspan=\"%d\" "
			 "colspan=\"%d\">\n\n",
			 cells[i].row, cells[i].col, rs, cs);
		if (response_buffer_append(out, buf))
			return -1;
		if (head > 0 && cells[i].present != FYAI_WORKPANE_PRESENT_OUTPUT) {
			snprintf(buf, sizeof(buf),
				 "<fy-slot id=\"head:%u\" height=\"%d\"/>\n\n",
				 cells[i].slot, head);
			if (response_buffer_append(out, buf))
				return -1;
		}
		snprintf(buf, sizeof(buf),
			 "<fy-slot id=\"%s:%u\" height=\"%d\"/>\n\n"
			 "</fy-cell>\n", cells[i].band ? "text" :
			 cells[i].screen ? "screen" : "tile",
			 cells[i].slot, span - head);
		if (response_buffer_append(out, buf))
			return -1;
	}
	return response_buffer_append(out, "</fy-grid>\n\n");
}

/* The embedded page document, data/page.yaml. */
#include "embedded_page.inc"

/*
 * The page document, parsed once. The builder lives as long as the process
 * and the document is read only, as the embedded catalogue is.
 */
static fy_generic page_doc(void)
{
	static struct fy_generic_builder *gb;
	static fy_generic doc = fy_invalid;
	struct fy_generic_builder_cfg cfg = {
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED,
	};
	fy_generic_sized_string embedded;

	if (fy_is_valid(doc))
		return doc;
	if (!gb)
		gb = fy_generic_builder_create(&cfg);
	if (!gb)
		return fy_invalid;
	embedded.data = (const char *)FYAI_EMBEDDED_PAGE;
	embedded.size = FYAI_EMBEDDED_PAGE_LEN;
	doc = fy_parse(gb, embedded,
		       FYAI_YAML_PARSE_FLAGS | FYOPPF_INPUT_TYPE_STRING, NULL);
	return doc;
}

static const char *page_str(const char *s)
{
	return s ? s : "";
}

/*
 * The state of @st that the document names. Every flag is decided here: the
 * document has no expressions.
 */
fy_generic fyai_page_state_generic(struct fy_generic_builder *gb,
				   const struct fyai_page_state *st)
{
	bool prompt = st->prompt_rows > 0, pane = st->pane_rows > 0;
	bool grid = !fy_str_empty(st->pane_source);
	fy_generic options = fy_seq_empty;
	size_t i;

	for (i = 0; st->ask_question && i < st->ask_noptions; i++)
		options = fy_append(gb, options, fy_mapping(gb,
			"text", page_str(st->ask_options[i]),
			"selected", (bool)(i == st->ask_selected),
			"other", (bool)(i != st->ask_selected)));

	return fy_mapping(gb,
		"input", fy_mapping(gb, "mode",
				    st->input_mode ? st->input_mode : "prompt"),
		"ask", fy_mapping(gb,
			"question", page_str(st->ask_question),
			"from", page_str(st->ask_from),
			"from_agent", (bool)!fy_str_empty(st->ask_from),
			"options", options,
			"waiting", st->ask_waiting,
			"waiting_shown", (bool)(st->ask_waiting > 0)),
		"screen", fy_mapping(gb, "mode",
				     st->fullscreen ? "fullscreen" : "inline"),
		"tail", fy_mapping(gb, "rows", st->tail_rows),
		"transcript", fy_mapping(gb, "rows", st->transcript_rows),
		"popup", fy_mapping(gb,
			"mode", st->popup_title ? "open" : "closed",
			"title", page_str(st->popup_title),
			"rows", st->popup_rows),
		"note", fy_mapping(gb,
			"shown", (bool)(st->note_nlines > 0),
			"rows", st->note_nlines),
		"pane", fy_mapping(gb,
			"above", (bool)(pane && !st->pane_below),
			"below", (bool)(pane && st->pane_below),
			"cap", (bool)!fy_str_empty(st->cap),
			"cap_source", page_str(st->cap),
			"grid", grid,
			"grid_source", page_str(st->pane_source),
			"slot", (bool)!grid,
			"rows", st->pane_rows),
		"header", fy_mapping(gb,
			"shown", (bool)(prompt || !fy_str_empty(st->header) ||
					!fy_str_empty(st->elapsed)),
			"text", page_escaped(gb, st->header),
			"elapsed", page_str(st->elapsed),
			"on", page_str(st->header_on),
			"off", page_str(st->header_off)),
		"prompt", fy_mapping(gb,
			"card", (bool)(prompt && st->prompt_card),
			"rules", (bool)(prompt && !st->prompt_card),
			"rows", st->prompt_rows,
			"card_rows", st->prompt_rows + 2),
		"status", fy_mapping(gb,
			"shown", (bool)(prompt || st->completion ||
					!fy_str_empty(st->hint) ||
					!fy_str_empty(st->status)),
			"completion", (bool)st->completion,
			"hint", (bool)(!st->completion &&
				       (!fy_str_empty(st->hint) || prompt)),
			"hint_text", page_str(st->hint),
			"row", (bool)(!fy_str_empty(st->status) || prompt),
			"text", page_escaped(gb, st->status),
			"activity", page_str(st->activity),
			"gutter", st->gutter_cols,
			"on", page_str(st->status_on),
			"off", page_str(st->status_off)));
}

/* The value at @path ("a.b") of @state, or fy_invalid. */
static fy_generic page_lookup(fy_generic state, const char *path)
{
	fy_generic v = state;
	const char *dot;
	char key[64];
	size_t len;

	while (fy_is_valid(v) && path && *path) {
		dot = strchr(path, '.');
		len = dot ? (size_t)(dot - path) : strlen(path);
		if (len >= sizeof(key))
			return fy_invalid;
		memcpy(key, path, len);
		key[len] = '\0';
		v = fy_get(v, key, fy_invalid);
		path = dot ? dot + 1 : path + len;
	}
	return v;
}

/* A walk of the page document: the state it names and where it writes. */
struct page_doc_ctx {
	fy_generic pages;
	fy_generic state;
	struct response_buffer *out;
	int depth;
	/* The actions a document may name, and where the keys of its active
	 * cases go. */
	const struct fyai_page_action *actions;
	size_t nactions;
	struct fyai_page_keys *keys;
	/* Inside each: the item, which a name is looked up in first, and its
	 * position. */
	fy_generic item;
	bool in_each;
	long long index;
	/* The first reason the document does not transcribe goes to @why. A
	 * check walks every branch, whatever the state. */
	char *why;
	size_t why_size;
	bool check;
};

/* Pages of pages deeper than this do not transcribe. */
#define PAGE_DOC_DEPTH_MAX 8

static void page_why(char *why, size_t size, const char *fmt, va_list ap)
	__attribute__((format(printf, 3, 0)));

/* Write the reason @fmt to @why when it holds none yet. */
static void page_why(char *why, size_t size, const char *fmt, va_list ap)
{
	if (why && size && !why[0])
		vsnprintf(why, size, fmt, ap);
}

static int page_doc_fail(struct page_doc_ctx *c, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

/* Record why the document does not transcribe; the first reason stays. */
static int page_doc_fail(struct page_doc_ctx *c, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	page_why(c->why, c->why_size, fmt, ap);
	va_end(ap);
	return -1;
}

/* The value at @path: in the item of an each first, then in the state. */
static fy_generic page_doc_lookup(const struct page_doc_ctx *c,
				  const char *path)
{
	fy_generic v;

	if (c->in_each) {
		v = page_lookup(c->item, path);
		if (fy_is_valid(v))
			return v;
	}
	return page_lookup(c->state, path);
}

/* The position {index} or {number} of an each, into *@vp. */
static bool page_doc_position(const struct page_doc_ctx *c, const char *path,
			      long long *vp)
{
	if (!c->in_each)
		return false;
	if (!strcmp(path, "index"))
		*vp = c->index;
	else if (!strcmp(path, "number"))
		*vp = c->index + 1;
	else
		return false;
	return true;
}

/* Whether @node is shown: it names no flag, or its flag is set. */
static bool page_doc_shown(const struct page_doc_ctx *c, fy_generic node)
{
	fy_generic flag = fy_get(node, "if", fy_invalid);
	fy_generic v;

	/* A check walks the node whatever its flag. */
	if (c->check || !fy_is_valid(flag))
		return true;
	v = page_doc_lookup(c, fy_castp(&flag, ""));
	return fy_cast(v, false);
}

/* A number of the document: a literal, or a binding "{a.b}" of the state. */
static long long page_doc_int(const struct page_doc_ctx *c, fy_generic v)
{
	const char *s, *close;
	char path[64];
	fy_generic bound;
	long long n;
	size_t len;

	if (!fy_is_string(v))
		return fy_cast(v, 0LL);
	s = fy_castp(&v, "");
	close = strchr(s, '}');
	if (*s != '{' || !close || close[1])
		return 0;
	len = (size_t)(close - s - 1);
	if (len >= sizeof(path))
		return 0;
	memcpy(path, s + 1, len);
	path[len] = '\0';
	if (page_doc_position(c, path, &n))
		return n;
	bound = page_doc_lookup(c, path);
	return fy_cast(bound, 0LL);
}

/* Expand the bindings "{a.b}" of @tmpl with the state into @buf. */
static int page_doc_expand(const struct page_doc_ctx *c, const char *tmpl,
			   struct response_buffer *buf)
{
	const char *p, *close;
	char path[64], one[2];
	long long pos;
	fy_generic v;
	size_t len;

	for (p = tmpl; *p; p++) {
		close = *p == '{' ? strchr(p, '}') : NULL;
		len = close ? (size_t)(close - p - 1) : 0;
		if (!close || len >= sizeof(path)) {
			one[0] = *p;
			one[1] = '\0';
			if (response_buffer_append(buf, one))
				return -1;
			continue;
		}
		memcpy(path, p + 1, len);
		path[len] = '\0';
		if (page_doc_position(c, path, &pos)) {
			snprintf(path, sizeof(path), "%lld", pos);
			if (response_buffer_append(buf, path))
				return -1;
			p = close;
			continue;
		}
		v = page_doc_lookup(c, path);
		if (fy_is_string(v) &&
		    response_buffer_append(buf, fy_castp(&v, "")))
			return -1;
		if (fy_generic_is_int(v)) {
			snprintf(path, sizeof(path), "%lld", fy_cast(v, 0LL));
			if (response_buffer_append(buf, path))
				return -1;
		}
		p = close;
	}
	return 0;
}

/* The kind of @node: the first of @kinds that it holds, or -1. */
static int page_doc_kind(fy_generic node, const char *const *kinds, int n)
{
	int i;

	for (i = 0; i < n; i++)
		if (fy_is_valid(fy_get(node, kinds[i], fy_invalid)))
			return i;
	return -1;
}

enum page_doc_item { PDI_TEXT, PDI_SPACE, PDI_SGR, PDI_GUTTER, PDI_HOLD,
		     PDI_ROLE, PDI_FILL, PDI_MARKUP, PDI_ACT };

/* Whether @s holds only the bytes of an act id. */
static bool page_id_chars(const char *s)
{
	for (; *s; s++)
		if (!isalnum((unsigned char)*s) && !strchr("_-.:/", *s))
			return false;
	return true;
}

/* An act: a label for an action of the document, with its argument. */
static int page_doc_act(struct page_doc_ctx *c, fy_generic act,
			bool *row_start)
{
	struct response_buffer arg = {0}, text = {0};
	const char *action = fy_get(act, "action", "");
	fy_generic v;
	char tag[FYTIM_PAGE_ID_MAX + 32];
	int rc = -1;

	if (!fyai_page_action_find(c->actions, c->nactions, action,
				   strlen(action)) || !page_id_chars(action)) {
		page_doc_fail(c, "an act names the unknown action '%s'", action);
		goto out;
	}
	v = fy_get(act, "arg", fy_invalid);
	if (fy_is_valid(v) && page_doc_expand(c, fy_castp(&v, ""), &arg))
		goto out;
	if (arg.data && (!page_id_chars(arg.data) || !*arg.data)) {
		page_doc_fail(c, "the argument of an act of '%s' is not an id",
			      action);
		goto out;
	}
	if (strlen(action) + (arg.data ? arg.len + 1 : 0) > FYTIM_PAGE_ID_MAX) {
		page_doc_fail(c, "the id of an act of '%s' is too long", action);
		goto out;
	}
	snprintf(tag, sizeof(tag), "<fy-act id=\"%s%s%s\">", action,
		 arg.data ? ":" : "", arg.data ? arg.data : "");
	v = fy_get(act, "text", fy_invalid);
	if (page_doc_expand(c, fy_castp(&v, ""), &text))
		goto out;
	rc = response_buffer_append(c->out, tag) ||
	     (text.data && page_append_text(c->out, text.data, row_start)) ||
	     response_buffer_append(c->out, "</fy-act>");
out:
	free(arg.data);
	free(text.data);
	return rc;
}

static int page_doc_items(struct page_doc_ctx *c, fy_generic items,
			  bool *row_start)
{
	static const char *const kinds[] = {
		"text", "space", "sgr", "gutter", "hold", "role", "fill",
		"markup", "act",
	};
	struct response_buffer text = {0};
	fy_generic item, v, sub, bound;
	const char *s;
	char tag[160];
	int rc = 0;

	if (!fy_is_sequence(items))
		return page_doc_fail(c, "a row is not a list of items");
	fy_foreach(item, items) {
		if (rc)
			break;
		if (!page_doc_shown(c, item))
			continue;
		switch (page_doc_kind(item, kinds, 9)) {
		case PDI_TEXT:
			v = fy_get(item, "text", fy_invalid);
			text.len = 0;
			rc = page_doc_expand(c, fy_castp(&v, ""), &text);
			if (!rc && text.data)
				rc = page_append_text(c->out, text.data,
						      row_start);
			break;
		case PDI_SPACE:
			rc = page_repeat(c->out, PAGE_SPACE,
					 (int)page_doc_int(c, fy_get(item, "space",
								fy_invalid)));
			break;
		case PDI_SGR:
			v = fy_get(item, "sgr", fy_invalid);
			bound = page_doc_lookup(c, fy_castp(&v, ""));
			rc = page_style(c->out, fy_castp(&bound, ""));
			break;
		case PDI_GUTTER:
			sub = fy_get(item, "gutter", fy_invalid);
			v = fy_get(sub, "mark", fy_invalid);
			bound = page_doc_lookup(c, fy_castp(&v, ""));
			rc = page_gutter(c->out, fy_castp(&bound, ""),
					 (int)page_doc_int(c, fy_get(sub, "cols",
								fy_invalid)));
			break;
		case PDI_HOLD:
			if (*row_start)
				rc = response_buffer_append(c->out, PAGE_SPACE);
			break;
		case PDI_ROLE:
			sub = fy_get(item, "role", fy_invalid);
			snprintf(tag, sizeof(tag), "<fy-role name=\"%s\">",
				 fy_get(sub, "name", ""));
			rc = response_buffer_append(c->out, tag) ||
			     page_doc_items(c, fy_get(sub, "items", fy_invalid),
					    row_start) ||
			     response_buffer_append(c->out, "</fy-role>");
			break;
		case PDI_FILL:
			sub = fy_get(item, "fill", fy_invalid);
			s = fy_get(sub, "char", "");
			if (*s)
				snprintf(tag, sizeof(tag), "<fy-fill char=\"%s\"/>",
					 s);
			else
				snprintf(tag, sizeof(tag), "<fy-fill/>");
			rc = response_buffer_append(c->out, tag);
			break;
		case PDI_MARKUP:
			v = fy_get(item, "markup", fy_invalid);
			bound = page_doc_lookup(c, fy_castp(&v, ""));
			s = fy_castp(&bound, "");
			rc = *s ? response_buffer_append(c->out, s) : 0;
			/* Text after the markup is not at the start of the row. */
			if (*s)
				*row_start = false;
			break;
		case PDI_ACT:
			rc = page_doc_act(c, fy_get(item, "act", fy_invalid),
					  row_start);
			break;
		default:
			rc = page_doc_fail(c, "an item of a row is none of text, "
					   "space, sgr, gutter, hold, role, fill, "
					   "markup or act");
			break;
		}
	}
	free(text.data);
	return rc;
}

enum page_doc_node { PDN_TIGHT, PDN_SLOT, PDN_SWITCH, PDN_EACH, PDN_DROP,
		     PDN_PAGE, PDN_MARKUP, PDN_ROW };

static int page_doc_nodes(struct page_doc_ctx *c, fy_generic nodes);

/* The keys that fyai keeps for itself: a document cannot bind them. */
static bool page_key_reserved(const char *name)
{
	return !strcmp(name, "Ctrl-]") || !strcasecmp(name, "Ctrl-t") ||
	       !strcmp(name, "Ctrl-Tab");
}

/* The keys of a transcribed case: each names a key and an action. */
static int page_doc_keys(struct page_doc_ctx *c, fy_generic keys)
{
	fy_generic key, action;
	const char *name, *act, *colon;

	if (!fy_is_valid(keys))
		return 0;
	if (!fy_is_mapping(keys))
		return page_doc_fail(c, "the keys of a case are not a mapping");
	if (fy_len(keys) > (size_t)FYAI_PAGE_KEYS_MAX)
		return page_doc_fail(c, "a case binds more than %d keys",
				     (int)FYAI_PAGE_KEYS_MAX);
	fy_foreach_key_value(key, action, keys) {
		name = fy_castp(&key, "");
		act = fy_castp(&action, "");
		/* A key can give its action an argument: "action:arg". */
		colon = strchr(act, ':');
		if (!*name)
			return page_doc_fail(c, "a key has no name");
		if (page_key_reserved(name))
			return page_doc_fail(c, "the key %s is kept by fyai",
					     name);
		if (!fyai_page_action_find(c->actions, c->nactions, act,
					   colon ? (size_t)(colon - act) :
						   strlen(act)))
			return page_doc_fail(c, "the key %s names the unknown "
					     "action '%s'", name, act);
		if (colon && (!colon[1] || !page_id_chars(colon + 1)))
			return page_doc_fail(c, "the argument of the key %s is "
					     "not an id", name);
		if (!c->keys)
			continue;
		if (c->keys->count >= FYAI_PAGE_KEYS_MAX)
			return page_doc_fail(c, "the cases shown bind more than "
					     "%d keys", (int)FYAI_PAGE_KEYS_MAX);
		c->keys->key[c->keys->count].name = name;
		c->keys->key[c->keys->count].action = act;
		c->keys->count++;
	}
	return 0;
}

/* A case of a switch: its keys, then its body, which it may leave out. */
static int page_doc_case(struct page_doc_ctx *c, fy_generic kase,
			 const char *name)
{
	fy_generic body = fy_get(kase, "body", fy_invalid);

	if (!fy_is_mapping(kase))
		return page_doc_fail(c, "the case '%s' is not a mapping", name);
	if (page_doc_keys(c, fy_get(kase, "keys", fy_invalid)))
		return -1;
	return fy_is_valid(body) ? page_doc_nodes(c, body) : 0;
}

static int page_doc_node(struct page_doc_ctx *c, fy_generic node)
{
	static const char *const kinds[] = {
		"tight", "slot", "switch", "each", "drop", "page", "markup", "row",
	};
	fy_generic v, sub, bound, item, saved_item, cases, key;
	bool row_start, saved_in_each;
	long long saved_index, index;
	const char *s;
	char tag[96];
	int rc;

	if (!page_doc_shown(c, node))
		return 0;
	switch (page_doc_kind(node, kinds, 8)) {
	case PDN_TIGHT:
		return response_buffer_append(c->out, "<fy-tight>\n\n");
	case PDN_SLOT:
		sub = fy_get(node, "slot", fy_invalid);
		return page_slot(c->out, fy_get(sub, "id", ""),
				 (int)page_doc_int(c, fy_get(sub, "rows",
							fy_invalid)));
	case PDN_SWITCH:
		cases = fy_get(node, "cases", fy_invalid);
		if (!fy_is_mapping(cases))
			return page_doc_fail(c, "a switch has no cases");
		/* A check walks every case. */
		if (c->check) {
			fy_foreach_key_value(key, sub, cases) {
				rc = page_doc_case(c, sub, fy_castp(&key, ""));
				if (rc)
					return rc;
			}
			return 0;
		}
		/* One case, which the mode of the state names; none, when the
		 * state names no case. */
		v = fy_get(node, "switch", fy_invalid);
		bound = page_doc_lookup(c, fy_castp(&v, ""));
		sub = fy_get(cases, fy_castp(&bound, ""), fy_invalid);
		if (!fy_is_mapping(sub))
			return 0;
		return page_doc_case(c, sub, fy_castp(&bound, ""));
	case PDN_EACH:
		v = fy_get(node, "each", fy_invalid);
		bound = page_doc_lookup(c, fy_castp(&v, ""));
		if (!c->check && !fy_is_sequence(bound))
			return 0;
		saved_item = c->item;
		saved_in_each = c->in_each;
		saved_index = c->index;
		rc = 0;
		index = 0;
		if (c->check) {
			/* A check walks the body once, for an item that holds
			 * nothing. */
			c->item = fy_map_empty;
			c->in_each = true;
			c->index = 0;
			rc = page_doc_nodes(c, fy_get(node, "body", fy_invalid));
		} else {
			fy_foreach(item, bound) {
				c->item = item;
				c->in_each = true;
				c->index = index++;
				rc = page_doc_nodes(c, fy_get(node, "body",
							      fy_invalid));
				if (rc)
					break;
			}
		}
		c->item = saved_item;
		c->in_each = saved_in_each;
		c->index = saved_index;
		return rc;
	case PDN_DROP:
		snprintf(tag, sizeof(tag), "<fy-drop order=\"%lld\">\n\n",
			 page_doc_int(c, fy_get(node, "drop", fy_invalid)));
		return response_buffer_append(c->out, tag) ||
		       page_doc_nodes(c, fy_get(node, "body", fy_invalid)) ||
		       response_buffer_append(c->out, "</fy-drop>\n\n");
	case PDN_PAGE:
		v = fy_get(node, "page", fy_invalid);
		if (c->depth >= PAGE_DOC_DEPTH_MAX)
			return page_doc_fail(c, "pages nest deeper than %d at "
					     "'%s'", PAGE_DOC_DEPTH_MAX,
					     fy_castp(&v, ""));
		sub = fy_get(c->pages, fy_castp(&v, ""), fy_invalid);
		if (!fy_is_sequence(sub))
			return page_doc_fail(c, "the page '%s' is not in pages",
					     fy_castp(&v, ""));
		c->depth++;
		rc = page_doc_nodes(c, sub);
		c->depth--;
		return rc;
	case PDN_MARKUP:
		v = fy_get(node, "markup", fy_invalid);
		bound = page_doc_lookup(c, fy_castp(&v, ""));
		s = fy_castp(&bound, "");
		return *s ? response_buffer_append(c->out, s) : 0;
	case PDN_ROW:
		row_start = true;
		return page_doc_items(c, fy_get(node, "row", fy_invalid),
				      &row_start) ||
		       response_buffer_append(c->out, "\n\n");
	default:
		return page_doc_fail(c, "a node is none of tight, slot, switch, "
				     "each, drop, page, markup or row");
	}
}

static int page_doc_nodes(struct page_doc_ctx *c, fy_generic nodes)
{
	fy_generic node;
	int rc;

	if (!fy_is_sequence(nodes))
		return page_doc_fail(c, "a body is not a list of nodes");
	fy_foreach(node, nodes) {
		rc = page_doc_node(c, node);
		if (rc)
			return rc;
	}
	return 0;
}

/* Walk @doc with @state into @out; a check walks every branch. */
static int page_doc_walk(fy_generic doc, fy_generic state,
			 const struct fyai_page_action *actions, size_t n,
			 struct response_buffer *out,
			 struct fyai_page_keys *keys, bool check, char *why,
			 size_t why_size)
{
	struct page_doc_ctx c;

	if (why && why_size)
		why[0] = '\0';
	memset(&c, 0, sizeof(c));
	c.why = why;
	c.why_size = why_size;
	c.check = check;
	if (!out || !fy_is_mapping(doc) || !fy_is_mapping(state))
		return page_doc_fail(&c, "the document is not a mapping");
	c.pages = fy_get(doc, "pages", fy_invalid);
	c.state = state;
	c.out = out;
	c.depth = 0;
	c.actions = actions;
	c.nactions = n;
	c.keys = keys;
	c.item = fy_invalid;
	c.in_each = false;
	c.index = 0;
	if (keys)
		keys->count = 0;
	/* A failure inside a row reaches here as the 1 of a `||`. */
	return page_doc_nodes(&c, fy_get(doc, "page", fy_invalid)) ? -1 : 0;
}

int fyai_page_transcribe(fy_generic doc, fy_generic state,
			 const struct fyai_page_action *actions, size_t n,
			 struct response_buffer *out,
			 struct fyai_page_keys *keys)
{
	return page_doc_walk(doc, state, actions, n, out, keys, false, NULL, 0);
}

int fyai_page_check(fy_generic doc, const struct fyai_page_action *actions,
		    size_t n, char *why, size_t why_size)
{
	struct response_buffer scratch = {0};
	int rc;

	rc = page_doc_walk(doc, fy_map_empty, actions, n, &scratch, NULL, true,
			   why, why_size);
	free(scratch.data);
	return rc;
}

static void page_load_why(char *why, size_t size, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

static void page_load_why(char *why, size_t size, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	page_why(why, size, fmt, ap);
	va_end(ap);
}

/* The embedded page schema, data/page.schema.yaml. */
#include "embedded_page_schema.inc"

fy_generic fyai_page_load(struct fy_generic_builder *gb, const char *path,
			  const struct fyai_page_action *actions, size_t n,
			  char *why, size_t why_size)
{
	fy_generic_sized_string text;
	fy_generic doc, schema, report;
	char reason[512];
	char *problems;

	if (why && why_size)
		why[0] = '\0';
	if (!gb || fy_str_empty(path)) {
		page_load_why(why, why_size, "no page document file is named");
		return fy_invalid;
	}
	if (access(path, R_OK)) {
		page_load_why(why, why_size, "cannot read %s: %s", path,
			      strerror(errno));
		return fy_invalid;
	}
	doc = fy_parse_file(gb, FYAI_YAML_PARSE_FLAGS | FYOPPF_COLLECT_DIAG,
			    path);
	if (!fy_is_mapping(doc)) {
		page_load_why(why, why_size,
			      "%s does not hold a YAML mapping", path);
		return fy_invalid;
	}
	text.data = (const char *)FYAI_EMBEDDED_PAGE_SCHEMA;
	text.size = FYAI_EMBEDDED_PAGE_SCHEMA_LEN;
	schema = fy_parse(gb, text,
			  FYAI_YAML_PARSE_FLAGS | FYOPPF_INPUT_TYPE_STRING, NULL);
	if (!fy_is_mapping(schema)) {
		page_load_why(why, why_size, "the page schema does not parse");
		return fy_invalid;
	}
	report = fyai_schema_validate(gb, schema, doc);
	if (!fyai_schema_valid(report)) {
		problems = fyai_schema_report_string(report);
		page_load_why(why, why_size,
			      "%s does not match the page schema: %s", path,
			      problems ? problems : "no problem was reported");
		free(problems);
		return fy_invalid;
	}
	if (fyai_page_check(doc, actions, n, reason, sizeof(reason))) {
		page_load_why(why, why_size, "%s: %s", path,
			      reason[0] ? reason :
			      "the document does not transcribe");
		return fy_invalid;
	}
	return doc;
}

int fyai_page_source(const struct fyai_page_state *st,
		     struct response_buffer *out)
{
	struct fy_generic_builder_cfg cfg = {
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED,
	};
	struct fy_generic_builder *gb;
	fy_generic doc;
	int rc;

	if (!st || !out)
		return -1;
	doc = page_doc();
	if (!fy_is_mapping(doc))
		return -1;
	gb = fy_generic_builder_create(&cfg);
	if (!gb)
		return -1;
	rc = fyai_page_transcribe(doc, fyai_page_state_generic(gb, st),
				  st->actions, st->nactions, out, st->keys);
	fy_generic_builder_destroy(gb);
	return rc;
}

int fyai_page_view_for(int present)
{
	switch (present) {
	case FYAI_WORKPANE_PRESENT_FULL:
		return FYTIM_PAGE_VIEW_FULL;
	case FYAI_WORKPANE_PRESENT_OUTPUT:
		return FYTIM_PAGE_VIEW_SCREEN;
	default:
		/* The head says whose call it is; a tile granted nothing draws
		 * nothing whatever its view. */
		return FYTIM_PAGE_VIEW_HEAD;
	}
}

struct fyai_page *fyai_page_create(struct fyai_ctx *ctx,
				   const struct fyai_page_action *actions,
				   size_t n)
{
	struct fy_generic_builder_cfg cfg = {
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED,
	};
	const char *path = ctx->cfg->page_path;
	struct fyai_page *pg;
	fy_generic doc;
	char why[1024];

	pg = calloc(1, sizeof(*pg));
	fyai_error_check(ctx, pg, err_out, "cannot allocate the page");
	pg->ctx = ctx;
	pg->last_state = fy_invalid;
	pg->doc = page_doc();
	fyai_error_check(ctx, fy_is_mapping(pg->doc), err_free,
			 "cannot read the embedded page document");
	if (fy_str_empty(path))
		return pg;
	pg->doc_path = strdup(path);
	fyai_error_check(ctx, pg->doc_path, err_free,
			 "cannot keep the path of display/page");
	pg->doc_gb = fy_generic_builder_create(&cfg);
	fyai_error_check(ctx, pg->doc_gb, err_free,
			 "cannot make a builder for the page document");
	doc = fyai_page_load(pg->doc_gb, path, actions, n, why, sizeof(why));
	if (fy_is_mapping(doc)) {
		pg->doc = doc;
		return pg;
	}
	pg->rejected = strdup(why);
	fyai_error_check(ctx, pg->rejected, err_free,
			 "cannot keep why display/page is not used");
	fyai_warning(ctx, "display/page is not used: %s; the embedded page is "
		     "used", why);
	return pg;

err_free:
	fyai_page_destroy(pg);
err_out:
	return NULL;
}

const char *fyai_page_document_path(const struct fyai_page *pg)
{
	return pg ? pg->doc_path : NULL;
}

void fyai_page_destroy(struct fyai_page *pg)
{
	if (!pg)
		return;
	/* The canvas is a band of the terminal library: the band stack would
	 * draw it once the page is gone. */
	if (pg->canvas)
		fytim_surface_close(pg->canvas);
	fymd_renderer_destroy(pg->renderer);
	free(pg->source.data);
	free(pg->cells);
	free(pg->blank.data);
	if (pg->doc_gb)
		fy_generic_builder_destroy(pg->doc_gb);
	if (pg->frame_gb)
		fy_generic_builder_destroy(pg->frame_gb);
	free(pg->doc_path);
	free(pg->rejected);
	free(pg);
}

/* Rows of rendered text: its lines, and a last line without a newline. */
static int page_count_rows(const char *text, size_t len)
{
	size_t i;
	int n = 0;

	for (i = 0; i < len; i++)
		n += text[i] == '\n';
	if (len && text[len - 1] != '\n')
		n++;
	return n;
}

/* The tile of the region @id, "@prefix:N", in @st, or NULL. */
static struct fyai_page_tile *page_tile_of(const struct fyai_page_state *st,
					   const char *id, const char *prefix)
{
	size_t len = strlen(prefix);
	unsigned long slot;
	char *end;
	int i;

	if (strncmp(id, prefix, len) || id[len] != ':')
		return NULL;
	slot = strtoul(id + len + 1, &end, 10);
	if (end == id + len + 1 || *end)
		return NULL;
	for (i = 0; i < st->ntiles; i++)
		if (st->tiles[i].slot == slot)
			return &st->tiles[i];
	return NULL;
}

/* The columns of the margin of @hd in a region @width wide. */
static int page_tile_margin(const struct fyai_page_tile *hd, int width)
{
	if (fy_str_empty(hd->margin) || hd->margin_cols < 1)
		return 0;
	return hd->margin_cols < width ? hd->margin_cols : width;
}

/*
 * Draw @hd into the cells of the region @r as the terminal library draws the
 * head of a tile: its margin at the left of each row it draws, the whole
 * region on the ground of the tile, and plain text dim as chrome. The margin
 * is chrome and draws dim under any styling of its own. Returns the rows
 * drawn, or -1.
 */
/* The marks of a head, in its last two columns: zoom, then close. */
#define PAGE_MARK_ZOOM	"\xe2\xa4\xa2"
#define PAGE_MARK_CLOSE	"\xc3\x97"

static int page_head_draw(struct fyai_page *pg, const struct fyai_page_tile *hd,
			  const struct fymd_region *r, const char *marks)
{
	const char *text = hd->rows, *margin;
	int mc, n, row, rc;

	mc = page_tile_margin(hd, r->width);
	if (!strchr(text, '\x1b'))
		text = fy_sprintfa("\x1b[2m%s", text);
	n = fytim_cells_draw_text(pg->cells, pg->cells_rows, pg->cells_cols,
				  (int)r->row, r->col + mc, r->width - mc,
				  r->height, text, strlen(text));
	if (n < 0)
		return -1;
	if (mc > 0) {
		margin = fy_sprintfa("\x1b[2m%s", hd->margin);
		for (row = 0; row < n; row++) {
			rc = fytim_cells_draw_text(pg->cells, pg->cells_rows,
						   pg->cells_cols,
						   (int)r->row + row, r->col, mc,
						   1, margin, strlen(margin));
			if (rc < 0)
				return -1;
		}
	}
	/* The marks stand where the band stack draws them, on the tile's own
	 * ground. */
	if (marks && r->width >= 2) {
		rc = fytim_cells_draw_text(pg->cells, pg->cells_rows,
				pg->cells_cols, (int)r->row, r->col + r->width - 2,
				2, 1, fy_sprintfa("%s" PAGE_MARK_ZOOM PAGE_MARK_CLOSE,
						  marks),
				strlen(marks) + sizeof(PAGE_MARK_ZOOM) - 1 +
				sizeof(PAGE_MARK_CLOSE) - 1);
		if (rc < 0)
			return -1;
	}
	rc = fytim_cells_ground(pg->cells, pg->cells_rows, pg->cells_cols,
				(int)r->row, r->col, r->width, r->height,
				hd->ground);
	return rc ? -1 : n;
}

/*
 * Add the acts of @hd, drawn in the region @r, to @regions from *@np. An act
 * is named for its tile, "tile:N:act", because the page is one component.
 */
static void page_head_acts(struct fyai_page *pg,
			   const struct fyai_page_tile *hd,
			   const struct fymd_region *r, bool marks,
			   struct fytim_page_region *regions, size_t *np)
{
	static const char *const mark_ids[] = { "zoom", "close" };
	const struct markdown_region *a;
	const char *name;
	int col, width, right;
	size_t i;

	right = r->col + r->width;
	for (i = 0; i < hd->nacts && *np < FYTIM_PAGE_REGIONS_MAX; i++) {
		a = &hd->acts[i];
		if (!a->id || a->row >= (size_t)r->height || a->width < 1)
			continue;
		col = r->col + page_tile_margin(hd, r->width) + a->col;
		if (col >= right)
			continue;
		width = a->width < right - col ? a->width : right - col;
		name = strncmp(a->id, "tile:", 5) ? a->id : a->id + 5;
		snprintf(pg->act_ids[*np], sizeof(pg->act_ids[*np]), "tile:%u:%s",
			 hd->slot, name);
		regions[*np].id = pg->act_ids[*np];
		regions[*np].kind = FYTIM_PAGE_ACT;
		regions[*np].row = (int)r->row + (int)a->row;
		regions[*np].col = col;
		regions[*np].width = width;
		regions[*np].height = 1;
		(*np)++;
	}
	for (i = 0; marks && r->width >= 2 && i < 2 &&
	     *np < FYTIM_PAGE_REGIONS_MAX; i++) {
		snprintf(pg->act_ids[*np], sizeof(pg->act_ids[*np]), "tile:%u:%s",
			 hd->slot, mark_ids[i]);
		regions[*np].id = pg->act_ids[*np];
		regions[*np].kind = FYTIM_PAGE_ACT;
		regions[*np].row = (int)r->row;
		regions[*np].col = r->col + r->width - 2 + (int)i;
		regions[*np].width = 1;
		regions[*np].height = 1;
		(*np)++;
	}
}

/*
 * Draw the screen of @t into the cells of the region @r as the terminal
 * library draws a surface: its last rows when the region is short, the margin
 * at the left of each row, and the cells on the ground of the tile with the
 * cursor reversed. The grant of @t is what the region gives the screen; a
 * tile shown as its head keeps it and draws no screen. Returns 0, or -1.
 */
static int page_screen_draw(struct fyai_page *pg, struct fyai_page_tile *t,
			    const struct fymd_region *r, bool truecolor)
{
	const struct fytim_cell *src;
	struct fytim_cell *dst;
	const char *margin = NULL;
	int grid_rows = 0, grid_cols = 0, crow = 0, ccol = 0;
	int rows, first, row, y, x, mc, n, rc;
	bool cursor = false;

	rows = t->content_rows < r->height ? t->content_rows : r->height;
	if (rows < 1)
		rows = 1;
	mc = page_tile_margin(t, r->width);
	t->granted_rows = rows;
	t->granted_cols = r->width - mc;
	if (t->present == FYAI_WORKPANE_PRESENT_HEAD ||
	    t->present == FYAI_WORKPANE_PRESENT_HIDDEN)
		return 0;
	if (fytim_surface_size(t->surface, &grid_rows, &grid_cols) != FYTIM_OK ||
	    fytim_surface_cursor(t->surface, &crow, &ccol, &cursor) != FYTIM_OK)
		return -1;
	/* The ground of the tile, where the grid does not cover it too. */
	rc = fytim_cells_ground(pg->cells, pg->cells_rows, pg->cells_cols,
				(int)r->row, r->col, r->width, rows, t->ground);
	if (rc)
		return -1;
	if (mc > 0)
		margin = fy_sprintfa("\x1b[2m%s", t->margin);
	first = grid_rows > rows ? grid_rows - rows : 0;
	for (row = first, y = (int)r->row; row < grid_rows && y < pg->cells_rows;
	     row++, y++) {
		if (mc > 0) {
			n = fytim_cells_draw_text(pg->cells, pg->cells_rows,
						  pg->cells_cols, y, r->col, mc, 1,
						  margin, strlen(margin));
			rc = fytim_cells_ground(pg->cells, pg->cells_rows,
						pg->cells_cols, y, r->col, mc, 1,
						t->ground);
			if (n < 0 || rc)
				return -1;
		}
		src = fytim_surface_row(t->surface, row);
		x = r->col + mc;
		n = grid_cols < r->width - mc ? grid_cols : r->width - mc;
		if (n > pg->cells_cols - x)
			n = pg->cells_cols - x;
		if (!src || n < 1)
			continue;
		dst = &pg->cells[(size_t)y * (size_t)pg->cells_cols + (size_t)x];
		memcpy(dst, src, (size_t)n * sizeof(*dst));
		/* A wide glyph cut at the edge is not drawn half. */
		if (dst[n - 1].width > 1) {
			memset(dst[n - 1].chars, 0, sizeof(dst[n - 1].chars));
			dst[n - 1].width = 1;
		}
		rc = fytim_cells_wash(pg->cells, pg->cells_rows, pg->cells_cols,
				      y, x, n, 1, t->ground, t->mix, truecolor);
		if (rc)
			return -1;
		/* The cursor is the one cell that is not on the ground. */
		if (cursor && row == crow && ccol >= 0 && ccol < n)
			dst[ccol].attrs ^= FYTIM_ATTR_REVERSE;
	}
	return 0;
}

/* Rows of chrome text: one for each of its lines, and none for NULL. */
static int page_chrome_rows(const char *s)
{
	int n = 1;

	if (!s)
		return 0;
	for (; *s; s++)
		n += *s == '\n';
	return n;
}

/* A full-width rule of @width columns on the row @y, in @chrome. */
static int page_rule_draw(struct fyai_page *pg, int y, int x, int width,
			  const char *chrome)
{
	struct response_buffer rule = {0};
	int i, n;

	if (response_buffer_append(&rule, chrome))
		goto err_out;
	for (i = 0; i < width; i++)
		if (response_buffer_append(&rule, "\xe2\x94\x80"))
			goto err_out;
	n = fytim_cells_draw_text(pg->cells, pg->cells_rows, pg->cells_cols, y,
				  x, width, 1, rule.data, rule.len);
	free(rule.data);
	return n < 0 ? -1 : 0;

err_out:
	free(rule.data);
	return -1;
}

/*
 * Draw the tile of text @t into the cells of the region @r as the terminal
 * library draws a tile of a work band: the top chrome, the last rows of the
 * content that fit, and the bottom chrome. A short region sheds the top rows
 * first, then the bottom, then the content. An empty chrome slot is a rule.
 * Chrome that carries no styling of its own draws in @chrome; the bottom
 * always does. Sets the grant of @t and returns 0, or -1.
 */
static int page_text_draw(struct fyai_page *pg, struct fyai_page_tile *t,
			  const struct fymd_region *r, const char *chrome)
{
	const char *content, *top, *bottom, *p, *nl, *base;
	int lines = 0, rows, top_rows, bottom_rows, max, y, skip, n, i;

	content = fytim_workband_content(t->band, &lines);
	top = fytim_workband_top(t->band);
	bottom = fytim_workband_bottom(t->band);
	max = fytim_workband_max_rows(t->band);
	rows = lines;
	if (rows < 1)
		rows = top || bottom ? 0 : 1;
	if (max > 0 && rows > max)
		rows = max;
	top_rows = page_chrome_rows(top);
	bottom_rows = page_chrome_rows(bottom);
	while (top_rows + rows + bottom_rows > r->height) {
		if (top_rows)
			top_rows--;
		else if (bottom_rows)
			bottom_rows = 0;
		else
			rows--;
	}
	t->granted_rows = rows;
	t->granted_cols = r->width;
	y = (int)r->row;

	if (top_rows > 0 && !*top) {
		if (page_rule_draw(pg, y, r->col, r->width, chrome))
			return -1;
		top_rows = 1;
	} else if (top_rows > 0) {
		/* Each row of chrome starts from its base, as the library draws
		 * them one by one. */
		base = strchr(top, '\x1b') ? "" : chrome;
		for (p = top, i = 0; i < top_rows; i++) {
			nl = strchr(p, '\n');
			n = fytim_cells_draw_text(pg->cells, pg->cells_rows,
				pg->cells_cols, y + i, r->col, r->width, 1,
				fy_sprintfa("%s%.*s", base,
					    (int)(nl ? nl - p : (long)strlen(p)), p),
				strlen(base) + (size_t)(nl ? nl - p : (long)strlen(p)));
			if (n < 0)
				return -1;
			if (!nl)
				break;
			p = nl + 1;
		}
	}
	y += top_rows;

	if (rows > 0 && content && *content) {
		skip = lines - rows;
		for (p = content; skip > 0 && p; skip--) {
			p = strchr(p, '\n');
			if (p)
				p++;
		}
		if (p) {
			n = fytim_cells_draw_text(pg->cells, pg->cells_rows,
						  pg->cells_cols, y, r->col,
						  r->width, rows, p, strlen(p));
			if (n < 0)
				return -1;
		}
	}
	y += rows;

	if (bottom_rows > 0 && !*bottom)
		return page_rule_draw(pg, y, r->col, r->width, chrome);
	if (bottom_rows > 0) {
		p = fy_sprintfa("%s%s", chrome, bottom);
		n = fytim_cells_draw_text(pg->cells, pg->cells_rows,
					  pg->cells_cols, y, r->col, r->width,
					  bottom_rows, p, strlen(p));
		if (n < 0)
			return -1;
	}
	return 0;
}

/*
 * Draw the rendered rows of the page and the tiles of @st in their regions
 * @fr into its grid of cells, and publish them to the canvas, opened and
 * sized to @nrows by @cols as needed.
 */
/* The @nlines rendered @lines of a view, from the top of its region. */
static int page_lines_draw(struct fyai_page *pg, const char *const *lines,
			   int nlines, const struct fymd_region *r)
{
	const char *line;
	int y, n;

	for (y = 0; y < (int)r->height && y < nlines; y++) {
		line = lines[y] ? lines[y] : "";
		n = fytim_cells_draw_text(pg->cells, pg->cells_rows,
					  pg->cells_cols, (int)r->row + y, r->col,
					  r->width, 1, line, strlen(line));
		if (n < 0)
			return -1;
	}
	return 0;
}

/*
 * The header row in its region @r, as the band stack draws it: the rendered
 * row and the time of the running turn, in the heading style of the theme,
 * cut at the edge of the region. The band stack ends a row that carries SGR
 * in an ellipsis, and stops a plain row at the edge; a plain row is drawn one
 * column wider, so its ellipsis falls outside the region.
 */
static int page_header_draw(struct fyai_page *pg,
			    const struct fyai_page_state *st,
			    const struct fymd_region *r)
{
	struct fytim_cell *row, *at;
	const char *text, *styled;
	bool plain;
	int n;

	if (r->height < 1 || r->width < 1 || r->col >= pg->cells_cols ||
	    (int)r->row >= pg->cells_rows ||
	    (fy_str_empty(st->header_row) && fy_str_empty(st->elapsed)))
		return 0;
	text = fy_sprintfa("%s%s", st->header_row ? st->header_row : "",
			   st->elapsed ? st->elapsed : "");
	styled = fy_sprintfa("%s%s%s", st->header_on ? st->header_on : "",
			     text, st->header_off ? st->header_off : "");
	plain = !strchr(text, '\x1b');
	at = &pg->cells[(size_t)r->row * (size_t)pg->cells_cols + r->col];
	if (r->width > pg->cells_cols - r->col)
		return -1;
	if (!plain) {
		n = fytim_cells_draw_text(pg->cells, pg->cells_rows,
					  pg->cells_cols, (int)r->row, r->col,
					  r->width, 1, styled, strlen(styled));
		return n < 0 ? -1 : 0;
	}
	row = calloc((size_t)r->width + 1, sizeof(*row));
	if (!row)
		return -1;
	memcpy(row, at, (size_t)r->width * sizeof(*row));
	row[r->width] = row[r->width - 1];
	n = fytim_cells_draw_text(row, 1, r->width + 1, 0, 0, r->width + 1, 1,
				  styled, strlen(styled));
	if (n >= 0)
		memcpy(at, row, (size_t)r->width * sizeof(*row));
	free(row);
	return n < 0 ? -1 : 0;
}

/* The last rows of the transcript tail that fit its region @r. */
static int page_tail_draw(struct fyai_page *pg, struct fytim *ft,
			  const struct fymd_region *r)
{
	const char *content, *p;
	int lines = 0, rows, skip, n;

	content = fytim_tail_content(ft, &lines);
	if (!content || !*content || lines < 1 || r->height < 1)
		return 0;
	rows = lines < (int)r->height ? lines : (int)r->height;
	for (p = content, skip = lines - rows; skip > 0 && p; skip--) {
		p = strchr(p, '\n');
		if (p)
			p++;
	}
	if (!p)
		return 0;
	n = fytim_cells_draw_text(pg->cells, pg->cells_rows, pg->cells_cols,
				  (int)r->row, r->col, r->width, rows, p,
				  strlen(p));
	return n < 0 ? -1 : 0;
}

static int page_canvas(struct fyai_page *pg, struct fytim *ft,
		       struct fyai_page_state *st,
		       const struct fymd_region *fr, size_t count,
		       const char *rows, size_t len, int nrows, int cols)
{
	struct fyai_ctx *ctx = pg->ctx;
	struct fyai_page_tile *t;
	struct fytim_cell *cells;
	struct fytim_cell ground = {
		.fg = FYTIM_COLOR_DEFAULT,
		.bg = FYTIM_COLOR_DEFAULT,
		.width = 1,
	};
	char sgr[64];
	size_t need, i;
	bool truecolor;
	int r, n, rc;

	if (nrows < 1 || cols < 1)
		return 0;
	if (!pg->canvas) {
		pg->canvas = fytim_surface_open(ft, nrows, cols);
		fyai_error_check(ctx, pg->canvas, err_out,
				 "cannot open the page canvas");
		(void)fytim_surface_set_max_rows(pg->canvas, 0);
		(void)fytim_surface_set_cursor(pg->canvas, 0, 0, false);
		fyai_error_check(ctx, fytim_surface_bind(pg->canvas, "canvas") ==
				 FYTIM_OK, err_out,
				 "cannot bind the page canvas to its slot");
	} else if (pg->cells_rows != nrows || pg->cells_cols != cols) {
		fyai_error_check(ctx, fytim_surface_resize(pg->canvas, nrows,
							    cols) == FYTIM_OK,
				 err_out, "cannot size the page canvas to %dx%d",
				 cols, nrows);
	}
	need = (size_t)nrows * (size_t)cols;
	if (need > pg->cells_alloc) {
		cells = realloc(pg->cells, need * sizeof(*cells));
		fyai_error_check(ctx, cells, err_out,
				 "cannot allocate the page cells");
		pg->cells = cells;
		pg->cells_alloc = need;
	}
	pg->cells_rows = nrows;
	pg->cells_cols = cols;
	memset(pg->cells, 0, need * sizeof(*pg->cells));
	for (i = 0; i < need; i++) {
		pg->cells[i].fg = FYTIM_COLOR_DEFAULT;
		pg->cells[i].bg = FYTIM_COLOR_DEFAULT;
		pg->cells[i].width = 1;
	}
	n = fytim_cells_draw_text(pg->cells, nrows, cols, 0, 0, cols, nrows,
				  rows, len);
	fyai_error_check(ctx, n >= 0, err_out,
			 "cannot draw the page rows into cells");
	truecolor = fytim_truecolor(ft);
	for (i = 0; i < count; i++) {
		if (fr[i].kind == FYMD_REGION_ACT)
			continue;
		if (!strcmp(fr[i].id, "tail")) {
			rc = page_tail_draw(pg, ft, &fr[i]);
			fyai_error_check(ctx, !rc, err_out,
					 "cannot draw the tail into cells");
		}
		if (!strcmp(fr[i].id, "header")) {
			rc = page_header_draw(pg, st, &fr[i]);
			fyai_error_check(ctx, !rc, err_out,
					 "cannot draw the header into cells");
		}
		if (!strcmp(fr[i].id, "transcript") && st->transcript_lines) {
			rc = page_lines_draw(pg, st->transcript_lines,
					     st->transcript_nlines, &fr[i]);
			fyai_error_check(ctx, !rc, err_out,
					 "cannot draw the transcript into cells");
		}
		if (!strcmp(fr[i].id, "popup") && st->popup_lines) {
			rc = page_lines_draw(pg, st->popup_lines,
					     st->popup_nlines, &fr[i]);
			fyai_error_check(ctx, !rc, err_out,
					 "cannot draw the popup into cells");
		}
		if (!strcmp(fr[i].id, "note") && st->note_lines) {
			rc = page_lines_draw(pg, st->note_lines,
					     st->note_nlines, &fr[i]);
			fyai_error_check(ctx, !rc, err_out,
					 "cannot draw the result into cells");
		}
		t = page_tile_of(st, fr[i].id, "head");
		if (t && t->rows) {
			n = page_head_draw(pg, t, &fr[i],
					   st->tile_marks && t->surface ?
					   fy_sprintfa("\x1b[2m%s",
						       st->band_chrome ?
						       st->band_chrome : "") :
					   NULL);
			fyai_error_check(ctx, n >= 0, err_out,
					 "cannot draw the head of tile %u into cells",
					 t->slot);
		}
		t = page_tile_of(st, fr[i].id, "screen");
		if (t && t->surface) {
			rc = page_screen_draw(pg, t, &fr[i], truecolor);
			fyai_error_check(ctx, !rc, err_out,
					 "cannot draw the screen of tile %u into cells",
					 t->slot);
		}
		t = page_tile_of(st, fr[i].id, "text");
		if (t && t->band) {
			rc = page_text_draw(pg, t, &fr[i],
					    fy_sprintfa("\x1b[2m%s",
							st->band_chrome ?
							st->band_chrome : ""));
			fyai_error_check(ctx, !rc, err_out,
					 "cannot draw the text of tile %u into cells",
					 t->slot);
		}
	}
	n = st->fullscreen ?
	    markdown_fullscreen_ground_sgr(ctx->cfg, sgr, sizeof(sgr) - 1) : 0;
	if (n > 0) {
		sgr[n++] = ' ';
		rc = fytim_cells_draw_text(&ground, 1, 1, 0, 0, 1, 1, sgr, n);
		fyai_error_check(ctx, rc >= 0, err_out,
				 "cannot read the fullscreen palette ground");
		/* Explicit backgrounds and attributes belong to their content. */
		for (i = 0; i < need; i++)
			if (pg->cells[i].bg == FYTIM_COLOR_DEFAULT)
				pg->cells[i].bg = ground.bg;
	}
	for (r = 0; r < nrows; r++) {
		n = fytim_surface_put_row(pg->canvas, r,
					  &pg->cells[(size_t)r * (size_t)cols],
					  cols);
		fyai_error_check(ctx, n == FYTIM_OK, err_out,
				 "cannot publish row %d of the page canvas", r);
	}
	return 0;

err_out:
	return -1;
}

/* The page places its rows itself: no document margin. */
static const char *page_no_margin(void *userdata, size_t row)
{
	(void)userdata;
	(void)row;
	return "";
}

static int page_renderer(struct fyai_page *pg, int cols)
{
	struct fyai_ctx *ctx = pg->ctx;
	struct fymd_renderer_cfg rcfg;

	if (pg->renderer && pg->cols == cols)
		return 0;
	fymd_renderer_destroy(pg->renderer);
	markdown_renderer_cfg(ctx->cfg, &rcfg,
			      markdown_color_enabled(ctx->cfg->color),
			      ctx->cfg->theme_variant, FYMD_RF_UI);
	/* The renderer keeps a right margin of two columns: the page is given
	 * them back so that its rows and slots take the whole width. */
	rcfg.width = cols + FYAI_PAGE_RIGHT_MARGIN;
	/* The header and the status carry the SGR pairs of the theme. */
	rcfg.sgr_input = FYMD_SGR_SAFE;
	pg->renderer = markdown_renderer_new(ctx->cfg, &rcfg);
	pg->cols = pg->renderer ? cols : 0;
	fyai_error_check(ctx, pg->renderer, err_out,
			 "cannot create the page renderer at %d columns", cols);
	return 0;

err_out:
	return -1;
}

int fyai_page_publish(struct fyai_page *pg, struct fytim *ft,
		      struct fyai_page_state *st, int cols, int rows)
{
	struct fy_generic_builder_cfg gbcfg = {
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED,
	};
	struct fytim_page_region regions[FYTIM_PAGE_REGIONS_MAX];
	struct fyai_page_tile *t;
	const struct fymd_region *fr;
	struct fyai_ctx *ctx;
	enum fytim_result res;
	char *out = NULL;
	size_t len = 0, count = 0, i, n = 0, tiles = 0;
	int rc, nrows, r;

	if (!pg || !ft || !st)
		return -1;
	ctx = pg->ctx;
	/* A tile the page places nowhere is granted nothing. */
	for (r = 0; r < st->ntiles; r++)
		st->tiles[r].granted_rows = st->tiles[r].granted_cols = 0;
	pg->source.len = 0;
	/* The state of the frame stays in its builder until the next frame,
	 * so /page can show it. */
	pg->last_state = fy_invalid;
	if (pg->frame_gb)
		fy_generic_builder_destroy(pg->frame_gb);
	pg->frame_gb = fy_generic_builder_create(&gbcfg);
	fyai_error_check(ctx, pg->frame_gb, err_out,
			 "cannot make a builder for the page state");
	pg->last_state = fyai_page_state_generic(pg->frame_gb, st);
	rc = fyai_page_transcribe(pg->doc, pg->last_state, st->actions,
				  st->nactions, &pg->source, st->keys);
	fyai_error_check(ctx, !rc, err_out, "cannot build the page source");
	rc = page_renderer(pg, cols > 0 ? cols : 80);
	if (rc)
		goto err_out;
	rc = fymd_renderer_set_height(pg->renderer, rows > 0 ? rows : 0);
	fyai_error_check(ctx, !rc, err_out,
			 "cannot give the page a height of %d rows", rows);
	rc = fymd_render_with_margins(pg->renderer, pg->source.data,
				      pg->source.len, page_no_margin, NULL,
				      &out, &len);
	fyai_error_check(ctx, !rc, err_out, "cannot render the page");
	rc = fymd_renderer_get_regions(pg->renderer, &fr, &count);
	fyai_error_check(ctx, !rc, err_out, "cannot read the page regions");
	/* The page is drawn as cells onto the canvas; the other slots stand
	 * on it, so the canvas is the first region. */
	nrows = page_count_rows(out, len);
	rc = page_canvas(pg, ft, st, fr, count, out, len, nrows,
			 cols > 0 ? cols : 80);
	if (rc)
		goto err_out;
	if (nrows > 0) {
		regions[n].id = "canvas";
		regions[n].kind = FYTIM_PAGE_SLOT;
		regions[n].row = 0;
		regions[n].col = 0;
		regions[n].width = cols > 0 ? cols : 80;
		regions[n].height = nrows;
		n++;
	}
	pg->blank.len = 0;
	for (r = 0; r < nrows; r++) {
		rc = response_buffer_append(&pg->blank, "\n");
		fyai_error_check(ctx, !rc, err_out,
				 "cannot build the rows of the page");
	}
	for (i = 0; i < count && n < FYTIM_PAGE_REGIONS_MAX; i++) {
		/* The tail is drawn on the canvas, on its ground. */
		if (fr[i].kind != FYMD_REGION_ACT && !strcmp(fr[i].id, "tail"))
			continue;
		if (fr[i].kind != FYMD_REGION_ACT &&
		    (!strncmp(fr[i].id, "tile:", 5) ||
		     !strncmp(fr[i].id, "screen:", 7) ||
		     !strncmp(fr[i].id, "text:", 5)))
			tiles++;
		regions[n].id = fr[i].id;
		/* The transcript view and the popup are text the user
		 * selects. */
		regions[n].kind = fr[i].kind == FYMD_REGION_ACT ?
				  FYTIM_PAGE_ACT :
				  !strcmp(fr[i].id, "transcript") ||
				  !strcmp(fr[i].id, "popup") ?
				  FYTIM_PAGE_TEXT : FYTIM_PAGE_SLOT;
		regions[n].row = (int)fr[i].row;
		regions[n].col = fr[i].col;
		regions[n].width = fr[i].width;
		regions[n].height = fr[i].height;
		n++;
	}
	/* The acts of the heads are the page's. */
	for (i = 0; i < count; i++) {
		t = fr[i].kind == FYMD_REGION_ACT ? NULL :
		    page_tile_of(st, fr[i].id, "head");
		if (t && t->rows)
			page_head_acts(pg, t, &fr[i],
				       st->tile_marks && t->surface, regions, &n);
	}
	pg->last_nregions = n;
	for (i = 0; i < n; i++) {
		snprintf(pg->last_regions[i].id, sizeof(pg->last_regions[i].id),
			 "%s", regions[i].id);
		pg->last_regions[i].act = regions[i].kind == FYTIM_PAGE_ACT;
		pg->last_regions[i].row = regions[i].row;
		pg->last_regions[i].col = regions[i].col;
		pg->last_regions[i].width = regions[i].width;
		pg->last_regions[i].height = regions[i].height;
	}
	res = fytim_page_set(ft, pg->blank.data ? pg->blank.data : "",
			     pg->blank.len, regions, n);
	fymd_free(out);
	out = NULL;
	fyai_error_check(ctx, res == FYTIM_OK, err_out,
			 "the terminal library rejected the page: %s",
			 fytim_result_string(res));
	fyai_diag_tracef("page", "cols=%d height=%d rows=%d regions=%zu "
			 "tiles=%zu source=%zu tail=%d pane=%d prompt=%d",
			 cols, rows, fytim_page_rows(ft), n, tiles,
			 pg->source.len, st->tail_rows, st->pane_rows,
			 st->prompt_rows);
	return 0;

err_out:
	fymd_free(out);
	return -1;
}

/* Append @text and, when it does not end one, a newline. */
static int page_report_block(struct response_buffer *md, const char *text)
{
	size_t len = text ? strlen(text) : 0;

	return (len && response_buffer_append(md, text)) ||
	       ((!len || text[len - 1] != '\n') &&
		response_buffer_append(md, "\n"));
}

int fyai_page_report(const struct fyai_page *pg, struct response_buffer *md)
{
	char line[FYTIM_PAGE_ID_MAX + 96];
	fy_generic yaml;
	size_t i;

	if (!pg || !md)
		return -1;
	if (response_buffer_append(md, "## The page\n\n"))
		return -1;
	if (pg->doc_path && !pg->rejected) {
		if (response_buffer_append(md, "The document is the file of "
					   "`display/page`:\n\n```text\n") ||
		    page_report_block(md, pg->doc_path) ||
		    response_buffer_append(md, "```\n\n"))
			return -1;
	} else if (response_buffer_append(md, "The document is the embedded "
					  "`data/page.yaml`.\n\n")) {
		return -1;
	}
	if (pg->rejected &&
	    (response_buffer_append(md, "The file of `display/page` is not "
				    "used:\n\n```text\n") ||
	     page_report_block(md, pg->rejected) ||
	     response_buffer_append(md, "```\n\n")))
		return -1;
	if (response_buffer_append(md, "### State\n\n"))
		return -1;
	if (pg->frame_gb && fy_is_mapping(pg->last_state)) {
		yaml = fy_gb_emit(pg->frame_gb, pg->last_state,
				  FYAI_YAML_EMIT_FLAGS, NULL);
		if (response_buffer_append(md, "```yaml\n") ||
		    page_report_block(md, fy_castp(&yaml, "")) ||
		    response_buffer_append(md, "```\n\n"))
			return -1;
	} else if (response_buffer_append(md, "No frame is drawn yet.\n\n")) {
		return -1;
	}
	if (response_buffer_append(md, "### Source\n\n````text\n") ||
	    page_report_block(md, pg->source.data) ||
	    response_buffer_append(md, "````\n\n### Regions\n\n```text\n"))
		return -1;
	for (i = 0; i < pg->last_nregions; i++) {
		snprintf(line, sizeof(line), "%-24s %-4s row %d col %d %dx%d\n",
			 pg->last_regions[i].id,
			 pg->last_regions[i].act ? "act" : "slot",
			 pg->last_regions[i].row, pg->last_regions[i].col,
			 pg->last_regions[i].width, pg->last_regions[i].height);
		if (response_buffer_append(md, line))
			return -1;
	}
	return response_buffer_append(md, "```\n");
}
