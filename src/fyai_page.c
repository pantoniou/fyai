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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libfymd4c.h>
#include <libfytimui.h>

#include "fyai.h"
#include "fyai_markdown.h"
#include "fyai_page.h"
#include "fyai_terminal.h"
#include "fyai_workpane.h"

bool fyai_page_requested(const struct fyai_cfg *cfg)
{
	return cfg && cfg->renderer && !strcmp(cfg->renderer, "page");
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

int fyai_page_chrome_rows(const struct fyai_page_state *st)
{
	int rows = 0;

	if (!st)
		return 0;
	if (st->prompt_rows > 0)
		/* header, the prompt with its two framing rows, two status rows */
		rows = 1 + st->prompt_rows + 2 + 2;
	else
		rows = (!fy_str_empty(st->header) || !fy_str_empty(st->elapsed)) +
		       (st->completion || !fy_str_empty(st->hint)) +
		       !fy_str_empty(st->status);
	if (st->pane_rows > 0 && !fy_str_empty(st->cap))
		rows++;
	return rows;
}

void fyai_page_fit(struct fyai_page_state *st, int height)
{
	int left;

	if (!st || height <= 0)
		return;
	left = height - fyai_page_chrome_rows(st);
	/* The work outranks the tail, which shows its last rows. */
	if (st->pane_rows > left) {
		st->pane_rows = left > 0 ? left : (st->pane_rows > 0);
		st->tail_rows = 0;
		return;
	}
	left -= st->pane_rows;
	if (st->tail_rows > left)
		st->tail_rows = left > 0 ? left : 0;
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
 * The work pane: its cap row, which goes before any other chrome when the
 * terminal is short, and its slot. fyai writes the cap, so it is not escaped.
 */
static int page_pane(struct response_buffer *out,
		     const struct fyai_page_state *st)
{
	if (st->pane_rows < 1)
		return 0;
	if (!fy_str_empty(st->cap) &&
	    (response_buffer_append(out, "<fy-drop order=\"0\">\n\n") ||
	     response_buffer_append(out, st->cap) ||
	     response_buffer_append(out, "\n\n</fy-drop>\n\n")))
		return -1;
	if (!fy_str_empty(st->pane_source))
		return response_buffer_append(out, st->pane_source);
	return page_slot(out, "pane", st->pane_rows);
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

int fyai_page_grid(struct fyai_ctx *ctx, const struct fyai_workpane_grid *g,
		   const struct fyai_page_cell *cells, int n, int height,
		   const char *sep, int sep_cols, struct response_buffer *out,
		   int *rowsp)
{
	int h[FYAI_WORKPANE_GRID_MAX];
	char buf[256];
	int i, r, c, rows = 0, span, rs, cs, head, rc;

	fyai_error_check(ctx, g && g->rows > 0 && g->cols > 0 &&
			 g->rows <= FYAI_WORKPANE_GRID_MAX &&
			 g->cols <= FYAI_WORKPANE_GRID_MAX &&
			 (n <= 0 || cells), err_out,
			 "cannot build an invalid page grid");
	page_grid_rows(g, cells, n, height, h);
	for (r = 0; r < g->rows; r++)
		rows += h[r];
	if (rowsp)
		*rowsp = rows;
	if (!out)
		return 0;

	rc = response_buffer_append(out, "<fy-grid rows=\"");
	fyai_error_check(ctx, !rc, err_out, "cannot write the page grid rows");
	for (r = 0; r < g->rows; r++) {
		rc = snprintf(buf, sizeof(buf), "%s%d", r ? "," : "", h[r]);
		fyai_error_check(ctx, rc >= 0 && (size_t)rc < sizeof(buf), err_out,
				 "cannot format page grid row %d", r);
		rc = response_buffer_append(out, buf);
		fyai_error_check(ctx, !rc, err_out, "cannot write page grid row %d", r);
	}
	rc = response_buffer_append(out, "\" cols=\"");
	fyai_error_check(ctx, !rc, err_out, "cannot write the page grid columns");
	for (c = 0; c < g->cols; c++) {
		if (g->col_size[c] > 0)
			rc = snprintf(buf, sizeof(buf), "%s%d", c ? "," : "",
				 g->col_size[c]);
		else
			rc = snprintf(buf, sizeof(buf), "%s*", c ? "," : "");
		fyai_error_check(ctx, rc >= 0 && (size_t)rc < sizeof(buf), err_out,
				 "cannot format page grid column %d", c);
		rc = response_buffer_append(out, buf);
		fyai_error_check(ctx, !rc, err_out,
				 "cannot write page grid column %d", c);
	}
	/* A separator that could end the attribute is not drawn. */
	if (fy_str_empty(sep) || sep_cols < 1 || strchr(sep, '"'))
		sep_cols = 0;
	rc = snprintf(buf, sizeof(buf), "\" gap=\"%d\"", sep_cols);
	fyai_error_check(ctx, rc >= 0 && (size_t)rc < sizeof(buf), err_out,
			 "cannot format the page grid gap");
	rc = response_buffer_append(out, buf);
	fyai_error_check(ctx, !rc, err_out, "cannot write the page grid gap");
	if (sep_cols > 0) {
		rc = response_buffer_append(out, " sep=\"");
		fyai_error_check(ctx, !rc, err_out,
				 "cannot start the page grid separator");
		rc = response_buffer_append(out, sep);
		fyai_error_check(ctx, !rc, err_out,
				 "cannot write the page grid separator");
		rc = response_buffer_append(out, "\"");
		fyai_error_check(ctx, !rc, err_out,
				 "cannot end the page grid separator");
	}
	rc = response_buffer_append(out, ">\n");
	fyai_error_check(ctx, !rc, err_out, "cannot open the page grid");

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
		rc = snprintf(buf, sizeof(buf),
			 "<fy-cell row=\"%d\" col=\"%d\" rowspan=\"%d\" "
			 "colspan=\"%d\">\n\n",
			 cells[i].row, cells[i].col, rs, cs);
		fyai_error_check(ctx, rc >= 0 && (size_t)rc < sizeof(buf), err_out,
				 "cannot format page grid cell %d", i);
		rc = response_buffer_append(out, buf);
		fyai_error_check(ctx, !rc, err_out,
				 "cannot write page grid cell %d", i);
		if (head > 0 && cells[i].present != FYAI_WORKPANE_PRESENT_OUTPUT) {
			rc = snprintf(buf, sizeof(buf),
				 "<fy-slot id=\"head:%u\" height=\"%d\"/>\n\n",
				 cells[i].slot, head);
			fyai_error_check(ctx, rc >= 0 && (size_t)rc < sizeof(buf),
					 err_out, "cannot format tile %u head slot",
					 cells[i].slot);
			rc = response_buffer_append(out, buf);
			fyai_error_check(ctx, !rc, err_out,
					 "cannot write tile %u head slot", cells[i].slot);
		}
		rc = snprintf(buf, sizeof(buf),
			 "<fy-slot id=\"%s:%u\" height=\"%d\"/>\n\n"
			 "</fy-cell>\n", cells[i].band ? "text" :
			 cells[i].screen ? "screen" : "tile",
			 cells[i].slot, span - head);
		fyai_error_check(ctx, rc >= 0 && (size_t)rc < sizeof(buf), err_out,
				 "cannot format tile %u content slot", cells[i].slot);
		rc = response_buffer_append(out, buf);
		fyai_error_check(ctx, !rc, err_out,
				 "cannot write tile %u content slot", cells[i].slot);
	}
	rc = response_buffer_append(out, "</fy-grid>\n\n");
	fyai_error_check(ctx, !rc, err_out, "cannot close the page grid");
	return 0;

err_out:
	return -1;
}

/* A full-width rule in the chrome role, removed with order 3. */
static int page_rule(struct response_buffer *out)
{
	return response_buffer_append(out,
		"<fy-drop order=\"3\">\n\n"
		"<fy-role name=\"chrome\"><fy-fill char=\"\xe2\x94\x80\"/>"
		"</fy-role>\n\n</fy-drop>\n\n");
}

int fyai_page_source(const struct fyai_page_state *st,
		     struct response_buffer *out)
{
	bool status, row_start;

	if (!st || !out)
		return -1;
	if (response_buffer_append(out, "<fy-tight>\n\n") ||
	    page_slot(out, "tail", st->tail_rows) ||
	    (!st->pane_below && page_pane(out, st)))
		return -1;

	/* The header row, which the band stack always has under a prompt. */
	if (st->prompt_rows > 0 || !fy_str_empty(st->header) ||
	    !fy_str_empty(st->elapsed)) {
		row_start = true;
		if (response_buffer_append(out, "<fy-drop order=\"2\">\n\n") ||
		    page_repeat(out, PAGE_SPACE, FYAI_PAGE_HEADER_MARGIN) ||
		    page_style(out, st->header_on) ||
		    page_append_text(out, st->header, &row_start) ||
		    page_append_text(out, st->elapsed, &row_start) ||
		    page_style(out, st->header_off) ||
		    response_buffer_append(out, "\n\n</fy-drop>\n\n"))
			return -1;
	}

	/* The prompt on its card, or between two rules. */
	if (st->prompt_rows > 0) {
		if (st->prompt_card) {
			if (page_slot(out, "prompt", st->prompt_rows + 2))
				return -1;
		} else if (page_rule(out) ||
			   page_slot(out, "prompt", st->prompt_rows) ||
			   page_rule(out)) {
			return -1;
		}
	}

	/* Two status rows: the hint or the ribbon, then the status. */
	status = st->prompt_rows > 0 || st->completion ||
		 !fy_str_empty(st->hint) || !fy_str_empty(st->status);
	if (status && response_buffer_append(out, "<fy-drop order=\"1\">\n\n"))
		return -1;
	if (st->completion) {
		if (page_slot(out, "completion", 1))
			return -1;
	} else if (!fy_str_empty(st->hint) || st->prompt_rows > 0) {
		row_start = true;
		if (page_style(out, st->status_on) ||
		    page_append_text(out, st->hint, &row_start) ||
		    (row_start && response_buffer_append(out, PAGE_SPACE)) ||
		    page_style(out, st->status_off) ||
		    response_buffer_append(out, "\n\n"))
			return -1;
	}
	if (!fy_str_empty(st->status) || st->prompt_rows > 0) {
		row_start = true;
		if (page_gutter(out, st->activity, st->gutter_cols) ||
		    page_style(out, st->status_on) ||
		    page_append_text(out, st->status, &row_start) ||
		    page_style(out, st->status_off) ||
		    response_buffer_append(out, "\n\n"))
			return -1;
	}
	if (status && response_buffer_append(out, "</fy-drop>\n\n"))
		return -1;

	if (st->pane_below && page_pane(out, st))
		return -1;
	return 0;
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

struct fyai_page *fyai_page_create(struct fyai_ctx *ctx)
{
	struct fyai_page *pg;

	pg = calloc(1, sizeof(*pg));
	fyai_error_check(ctx, pg, err_out, "cannot allocate the page");
	pg->ctx = ctx;
	return pg;

err_out:
	return NULL;
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
	struct fyai_ctx *ctx = hd->ctx ? hd->ctx : pg->ctx;
	const char *text = hd->rows, *margin;
	int mc, n, row, rc;

	mc = page_tile_margin(hd, r->width);
	if (!strchr(text, '\x1b'))
		text = fy_sprintfa("\x1b[2m%s", text);
	n = fytim_cells_draw_text(pg->cells, pg->cells_rows, pg->cells_cols,
				  (int)r->row, r->col + mc, r->width - mc,
				  r->height, text, strlen(text));
	fyai_error_check(ctx, n >= 0, err_out,
			 "cannot draw the head of tile %u", hd->slot);
	if (mc > 0) {
		margin = fy_sprintfa("\x1b[2m%s", hd->margin);
		for (row = 0; row < n; row++) {
			rc = fytim_cells_draw_text(pg->cells, pg->cells_rows,
						   pg->cells_cols,
						   (int)r->row + row, r->col, mc,
						   1, margin, strlen(margin));
			fyai_error_check(ctx, rc >= 0, err_out,
					 "cannot draw the head margin of tile %u",
					 hd->slot);
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
		fyai_error_check(ctx, rc >= 0, err_out,
				 "cannot draw the controls of tile %u", hd->slot);
	}
	rc = fytim_cells_ground(pg->cells, pg->cells_rows, pg->cells_cols,
				(int)r->row, r->col, r->width, r->height,
				hd->ground);
	fyai_error_check(ctx, !rc, err_out,
			 "cannot fill the head ground of tile %u", hd->slot);
	return n;

err_out:
	return -1;
}

/*
 * Add the acts of @hd, drawn in the region @r, to @regions from *@np. An act
 * is named for its tile, "tile:N:act", because the page is one component.
 */
static int page_head_acts(struct fyai_page *pg,
			   const struct fyai_page_tile *hd,
			   const struct fymd_region *r, bool marks,
			   struct fytim_page_region *regions, size_t *np)
{
	static const char *const mark_ids[] = { "zoom", "close" };
	const struct markdown_region *a;
	const char *name;
	int col, width, right, rc;
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
		rc = snprintf(pg->act_ids[*np], sizeof(pg->act_ids[*np]),
			      "tile:%u:%s", hd->slot, name);
		fyai_error_check(pg->ctx, rc >= 0 &&
				 (size_t)rc < sizeof(pg->act_ids[*np]), err_out,
				 "cannot format an action of tile %u", hd->slot);
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
		rc = snprintf(pg->act_ids[*np], sizeof(pg->act_ids[*np]),
			      "tile:%u:%s", hd->slot, mark_ids[i]);
		fyai_error_check(pg->ctx, rc >= 0 &&
				 (size_t)rc < sizeof(pg->act_ids[*np]), err_out,
				 "cannot format a control of tile %u", hd->slot);
		regions[*np].id = pg->act_ids[*np];
		regions[*np].kind = FYTIM_PAGE_ACT;
		regions[*np].row = (int)r->row;
		regions[*np].col = r->col + r->width - 2 + (int)i;
		regions[*np].width = 1;
		regions[*np].height = 1;
		(*np)++;
	}
	return 0;

err_out:
	return -1;
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
	struct fyai_ctx *ctx = t->ctx ? t->ctx : pg->ctx;
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
	rc = fytim_surface_size(t->surface, &grid_rows, &grid_cols);
	fyai_error_check(ctx, rc == FYTIM_OK, err_out,
			 "cannot read the size of tile %u", t->slot);
	rc = fytim_surface_cursor(t->surface, &crow, &ccol, &cursor);
	fyai_error_check(ctx, rc == FYTIM_OK, err_out,
			 "cannot read the cursor of tile %u", t->slot);
	/* The ground of the tile, where the grid does not cover it too. */
	rc = fytim_cells_ground(pg->cells, pg->cells_rows, pg->cells_cols,
				(int)r->row, r->col, r->width, rows, t->ground);
	fyai_error_check(ctx, !rc, err_out,
			 "cannot fill the ground of tile %u", t->slot);
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
			fyai_error_check(ctx, n >= 0, err_out,
					 "cannot draw the margin of tile %u", t->slot);
			fyai_error_check(ctx, !rc, err_out,
					 "cannot fill the margin of tile %u", t->slot);
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
		fyai_error_check(ctx, !rc, err_out,
				 "cannot apply the ground of tile %u", t->slot);
		/* The cursor is the one cell that is not on the ground. */
		if (cursor && row == crow && ccol >= 0 && ccol < n)
			dst[ccol].attrs ^= FYTIM_ATTR_REVERSE;
	}
	return 0;

err_out:
	return -1;
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
	int i, n, rc;

	rc = response_buffer_append(&rule, chrome);
	fyai_error_check(pg->ctx, !rc, err_out,
			 "cannot write the page rule style");
	for (i = 0; i < width; i++) {
		rc = response_buffer_append(&rule, "\xe2\x94\x80");
		fyai_error_check(pg->ctx, !rc, err_out,
				 "cannot write column %d of the page rule", i);
	}
	n = fytim_cells_draw_text(pg->cells, pg->cells_rows, pg->cells_cols, y,
				  x, width, 1, rule.data, rule.len);
	fyai_error_check(pg->ctx, n >= 0, err_out,
			 "cannot draw the page rule");
	free(rule.data);
	return 0;

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
static int page_canvas(struct fyai_page *pg, struct fytim *ft,
		       struct fyai_page_state *st,
		       const struct fymd_region *fr, size_t count,
		       const char *rows, size_t len, int nrows, int cols)
{
	struct fyai_ctx *ctx = pg->ctx;
	struct fyai_page_tile *t;
	struct fytim_cell *cells;
	size_t need, i;
	bool truecolor;
	int r, n, rc;

	if (nrows < 1 || cols < 1)
		return 0;
	if (!pg->canvas) {
		pg->canvas = fytim_surface_open(ft, nrows, cols);
		fyai_error_check(ctx, pg->canvas, err_out,
				 "cannot open the page canvas");
		rc = fytim_surface_set_max_rows(pg->canvas, 0);
		fyai_error_check(ctx, rc == FYTIM_OK, err_out,
				 "cannot remove the page canvas row limit");
		rc = fytim_surface_set_cursor(pg->canvas, 0, 0, false);
		fyai_error_check(ctx, rc == FYTIM_OK, err_out,
				 "cannot hide the page canvas cursor");
		rc = fytim_surface_bind(pg->canvas, "canvas");
		fyai_error_check(ctx, rc == FYTIM_OK, err_out,
				 "cannot bind the page canvas to its slot");
	} else if (pg->cells_rows != nrows || pg->cells_cols != cols) {
		rc = fytim_surface_resize(pg->canvas, nrows, cols);
		fyai_error_check(ctx, rc == FYTIM_OK,
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
	rc = fyai_page_source(st, &pg->source);
	fyai_error_check(ctx, !rc, err_out, "cannot build the page source");
	rc = page_renderer(pg, cols > 0 ? cols : 80);
	fyai_error_check(ctx, !rc, err_out, "cannot prepare the page renderer");
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
	fyai_error_check(ctx, !rc, err_out, "cannot build the page canvas");
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
	for (i = 0; i < count && n < FYTIM_PAGE_REGIONS_MAX; i++, n++) {
		if (fr[i].kind != FYMD_REGION_ACT &&
		    (!strncmp(fr[i].id, "tile:", 5) ||
		     !strncmp(fr[i].id, "screen:", 7) ||
		     !strncmp(fr[i].id, "text:", 5)))
			tiles++;
		regions[n].id = fr[i].id;
		regions[n].kind = fr[i].kind == FYMD_REGION_ACT ?
				  FYTIM_PAGE_ACT : FYTIM_PAGE_SLOT;
		regions[n].row = (int)fr[i].row;
		regions[n].col = fr[i].col;
		regions[n].width = fr[i].width;
		regions[n].height = fr[i].height;
	}
	/* The acts of the heads are the page's. */
	for (i = 0; i < count; i++) {
		t = fr[i].kind == FYMD_REGION_ACT ? NULL :
		    page_tile_of(st, fr[i].id, "head");
		if (t && t->rows) {
			rc = page_head_acts(pg, t, &fr[i],
					    st->tile_marks && t->surface, regions, &n);
			fyai_error_check(ctx, !rc, err_out,
					 "cannot build the actions of tile %u", t->slot);
		}
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
