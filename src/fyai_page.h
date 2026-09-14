/* SPDX-License-Identifier: MIT */
#ifndef FYAI_PAGE_H
#define FYAI_PAGE_H

#include <stdbool.h>
#include <stdint.h>

#include "utils.h"

/* Columns a libfymd4c render keeps at the right of a document. */
#define FYAI_PAGE_RIGHT_MARGIN 2

struct fyai_cfg;
struct fyai_ctx;
struct fytim;
struct fytim_page_region;
struct fytim_surface;
struct fytim_workband;
struct markdown_region;
struct fyai_workpane_grid;
struct fyai_page;

/*
 * The state that one frame of the page is built from. The strings are
 * borrowed for the call. header, hint and status are Markdown that fyai or
 * the user configuration wrote; activity may carry SGR, which is removed.
 */
struct fyai_page_state {
	const char *header;
	const char *elapsed;
	const char *activity;
	const char *hint;
	const char *status;
	const char *cap;	/* UI Markdown of the pane cap row; fyai wrote it */
	/* UI Markdown of the pane, an fy-grid of tile slots fyai wrote, or
	 * NULL for one pane slot the terminal library lays out. */
	const char *pane_source;
	int tail_rows;
	int pane_rows;
	bool pane_below;
	int prompt_rows;
	bool prompt_card;	/* the prompt stands on a card: two rows more */
	bool completion;
	int gutter_cols;	/* columns of the status gutter */
	/* SGR pairs of the theme for the header and the status, or NULL. */
	const char *header_on, *header_off;
	const char *status_on, *status_off;
	/* The tiles that hold a surface, which the page draws onto the canvas.
	 * The page sets the grant of each. */
	struct fyai_page_tile *tiles;
	int ntiles;
	/* The SGR the chrome of a tile takes over dim, or NULL. */
	const char *band_chrome;
	bool tile_marks;	/* draw the zoom and close marks of the heads */
};

/*
 * One tile the page draws: its head in the slot "head:N" and the screen of
 * its surface in the slot "screen:N", with the margin and the ground of the
 * surface. The strings, @acts and @surface are borrowed for the call.
 */
struct fyai_page_tile {
	unsigned int slot;
	struct fytim_surface *surface;
	struct fytim_workband *band;	/* a tile of text in the slot "text:N" */
	const char *rows;	/* the rendered head, no trailing newline, or NULL */
	const struct markdown_region *acts;	/* from the first cell of @rows */
	size_t nacts;
	const char *margin;	/* drawn at the left of each row, or NULL */
	int margin_cols;
	uint32_t ground;	/* FYTIM_COLOR_DEFAULT for no ground */
	int mix;		/* percent of @ground in a colour of the program */
	int content_rows;	/* the rows the screen asks for */
	int present;		/* enum fyai_workpane_present */
	/* Set by the page: the rows and the columns the screen was given. */
	int granted_rows, granted_cols;
};

/* Whether the configuration asks for the page renderer. */
bool fyai_page_requested(const struct fyai_cfg *cfg);
/* Whether this build can compose a page. */
bool fyai_page_supported(void);

/* Columns of the document margin before the header row. */
#define FYAI_PAGE_HEADER_MARGIN 2

/* Rows the chrome of @st takes: the header, the prompt block, the status
 * rows and the cap row. */
int fyai_page_chrome_rows(const struct fyai_page_state *st);

/*
 * Fit @st to @height rows, as the band stack does: the chrome keeps its rows,
 * the pane takes what is left and the tail what the pane leaves. A height of
 * zero or less changes nothing.
 */
void fyai_page_fit(struct fyai_page_state *st, int height);

/*
 * Append the page source for @st to @out. Text of the state is escaped, so it
 * places no slot and no act. The chrome stands as the band stack draws it: a
 * header row, the prompt between two framing rows, and two status rows. It
 * goes in this order when the page is too tall: the cap row, the status, the
 * header, then the framing rules; the prompt stays. Returns 0, or -1 when
 * memory runs out.
 */
int fyai_page_source(const struct fyai_page_state *st,
		     struct response_buffer *out);

/*
 * The view of a tile page for a presentation of the work pane (enum
 * fyai_workpane_present): the whole page, the screen without its head, or
 * the head alone. The value is an enum fytim_page_view.
 */
int fyai_page_view_for(int present);

/* One tile for the pane source: its slot, its cell, and the rows it asks for. */
struct fyai_page_cell {
	unsigned int slot;
	int row, col, row_span, col_span;
	int rows;		/* the rows it asks for, its head included */
	int head_rows;		/* the rows of its head */
	int present;		/* enum fyai_workpane_present */
	bool screen;		/* it holds a surface, whose screen the page draws */
	bool band;		/* it holds text, which the page draws */
};

/*
 * Append the pane as an fy-grid to @out, which may be NULL to size it only.
 * Each tile of @cells stands in its cell of @g: a slot "head:N" as tall as the
 * tallest head of the tiles that start on its row, and under it a slot
 * "tile:N" for the screen in the rows the cell has left. A cell keeps one
 * screen row. A tile shown without its head has no head slot, and its screen
 * starts at the top of the cell. A tile that holds a surface names its screen
 * slot "screen:N" and a tile of text names its slot "text:N", because the page
 * draws them. @sep of @sep_cols columns stands
 * between the
 * columns. The rows are solved as the terminal library solves a pane: a sized
 * row keeps its size, a fitted row takes its tallest tile, and the other rows
 * share what @height leaves in proportion to their tallest tile. A @height of
 * zero or less keeps the rows the tiles ask for. *@rowsp receives the rows of
 * the grid. Returns 0, or -1 for a grid that cannot be written.
 */
int fyai_page_grid(const struct fyai_workpane_grid *g,
		   const struct fyai_page_cell *cells, int n, int height,
		   const char *sep, int sep_cols, struct response_buffer *out,
		   int *rowsp);

struct fyai_page *fyai_page_create(struct fyai_ctx *ctx);
void fyai_page_destroy(struct fyai_page *pg);

/*
 * Build, render and give the page of this frame to @ft, at @cols by @rows.
 * The renderer is kept while the width stays, and each tile of @st is given
 * its grant. Returns 0, or -1 after it
 * reported the cause.
 */
int fyai_page_publish(struct fyai_page *pg, struct fytim *ft,
		      struct fyai_page_state *st, int cols, int rows);

#endif
