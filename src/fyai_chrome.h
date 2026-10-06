/*
 * fyai_chrome.h - the chrome of a tile of the work pane
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */
#ifndef FYAI_CHROME_H
#define FYAI_CHROME_H

#include <stdbool.h>
#include <stddef.h>

#include "fyai_ui.h"
#include "fyai_workpane.h"

struct fyai_ctx;
struct fytim_surface;

/*
 * The one component that decides which chrome a tile carries and builds its
 * head. An owner states the content of the head with struct fyai_chrome_spec
 * and the tile kind decides the rest. Do not decide a control, a scroll bar or
 * a column for chrome anywhere else.
 */

/* The elements of chrome that a tile can carry. */
#define FYAI_CHROME_BUTTONS	(1u << 0)	/* minimize, maximize, close */
#define FYAI_CHROME_SCROLL	(1u << 1)	/* a scroll bar beside the screen */
#define FYAI_CHROME_WHEEL	(1u << 2)	/* the wheel acts on the screen */

/* Where a tile is in its life. */
enum fyai_chrome_phase {
	FYAI_CHROME_RUNNING,	/* the program is still there */
	FYAI_CHROME_KEPT,	/* it ended and the tile stays for reading */
	FYAI_CHROME_COMMITTED,	/* the screen goes into the transcript */
};

/* How much chrome the user asked for: display/work_controls. */
enum fyai_chrome_level {
	FYAI_CHROME_NONE,
	FYAI_CHROME_ZOOM,
	FYAI_CHROME_FULL,
};

/* The level of chrome that the configuration asks for. */
enum fyai_chrome_level fyai_chrome_level(const struct fyai_ctx *ctx);

/*
 * The elements that a tile of @kind carries in @phase, as a mask of
 * FYAI_CHROME_*. A tile of text has no scroll bar.
 */
unsigned int fyai_chrome_items(const struct fyai_ctx *ctx,
			       enum fyai_workpane_tile_kind kind,
			       enum fyai_chrome_phase phase);

/* The columns that the chrome of @items takes beside a screen. */
int fyai_chrome_scroll_cols(unsigned int items);

/* The columns of a tile that chrome takes beside its body. */
struct fyai_chrome_frame {
	int margin;	/* the session margin, at the left of each row */
	int bar;	/* the scroll bar, in the last column */
	int body;	/* what is left for the screen */
};

/*
 * Solve the columns of a tile @width wide that carries @items and a margin
 * that is @margin_cols wide. This is the one place that divides a row between
 * margin, bar and body: a head, a screen and a scroll bar act on the same
 * columns.
 */
struct fyai_chrome_frame fyai_chrome_frame(unsigned int items, int margin_cols,
					   int width);

/* The parts of a scroll bar. The arrows and the thumb act; the track does not. */
enum fyai_chrome_bar_part {
	FYAI_CHROME_BAR_TRACK,
	FYAI_CHROME_BAR_THUMB,
	FYAI_CHROME_BAR_UP,
	FYAI_CHROME_BAR_DOWN,
};

/*
 * The part of row @i of a bar @height rows tall for content of @total rows of
 * which @rows show from row @top. It is the geometry of the bar that the
 * terminal library draws on a screen, so a bar of text and a bar of a screen
 * look the same.
 */
enum fyai_chrome_bar_part fyai_chrome_bar_part(int total, int top, int rows,
					       int height, bool arrows, int i);
/* The glyph of @part. */
const char *fyai_chrome_bar_glyph(enum fyai_chrome_bar_part part);

/* What the head of a tile says. All strings are borrowed for the call. */
struct fyai_chrome_spec {
	const char *title;	/* what the call is: Markdown */
	const char *right;	/* fyai text at the right edge, or NULL */
	const char *command;	/* the command under the title, or NULL */
	const char *cause;	/* why a call failed, or NULL */
	enum fyai_ui_mark mark;	/* the state mark */
	size_t frame;		/* the frame of the animated mark */
};

/*
 * Make the head of the tile @sf from @spec and give it to the tile. The kind
 * and the phase of the tile decide its controls. The title row is one row: a
 * title that does not fit loses its end to an ellipsis. The same head is the
 * top of the rows that the tile commits. Returns the interval of the mark
 * animation in @interval_msp when it is not NULL. Returns 0, or -1.
 */
int fyai_chrome_update(struct fyai_ctx *ctx, struct fytim_surface *sf,
		       const struct fyai_chrome_spec *spec,
		       unsigned int *interval_msp);

/* What the cap row of the work pane reports. */
struct fyai_chrome_cap {
	const char *height;	/* the height the pane takes: "half", "6 rows" */
	bool zoomed;		/* one tile has the pane */
	int tiles;		/* the tiles of the pane */
	int hidden;		/* the tiles that the layout hid */
};

/*
 * The Markdown of the cap row of the pane into @buf: its height, its tiles,
 * how many are shown and how many the layout hid, and the keys that move
 * between them. A tile that the layout hid is never silently gone. Returns the
 * length, as snprintf() does.
 */
int fyai_chrome_cap_source(const struct fyai_ctx *ctx,
			   const struct fyai_chrome_cap *cap, char *buf,
			   size_t size);

/* The id of the act of the panel that shows or hides the work pane. */
#define FYAI_CHROME_PANEL_PANE	"panel:pane"

/*
 * The panel at the right edge of the input header: a button that hides and
 * shows the work pane, then the live shells of the user, the live shells of
 * the model and the running sub-agents, each only when there is one. Its
 * glyphs are glyphs of the theme and its colours are roles of the palette.
 * Returns 1 and the text in @out with its columns in @colsp and the columns of
 * the button, which is at its start, in @buttonp. Returns 0 when no panel is
 * due: the pane is shown and there is no live work. Returns -1 when memory runs
 * out; @out is then empty.
 */
int fyai_chrome_panel(struct fyai_ctx *ctx, struct response_buffer *out,
		      int *colsp, int *buttonp);

/* Map "none" to absent chrome; an empty string stays a rule. */
const char *fyai_chrome_text(const char *v);

/*
 * The look of the body of a tile that does not hold the keys: the session
 * margin at the left of each row, no ground, and the foot of display/tile_frame.
 * An owner calls it when it opens a tile.
 */
void fyai_chrome_body(struct fyai_ctx *ctx, struct fytim_surface *sf);

/*
 * Update the keyboard-focus chrome of @sf: the ground, the edge marker or the
 * reversed margin, and the hint row that says the way back. The tile keeps its
 * rows, so focus changes no geometry.
 */
void fyai_chrome_focus(struct fyai_ctx *ctx, struct fytim_surface *sf,
		       bool focused);

#endif /* FYAI_CHROME_H */
