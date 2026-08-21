/* SPDX-License-Identifier: MIT */
#ifndef FYAI_TERMINAL_VIEW_H
#define FYAI_TERMINAL_VIEW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "utils.h"

/* The size used when no argument, no setting and no real terminal supply one. */
#define FYAI_TTY_ROWS_DEFAULT	24
#define FYAI_TTY_COLS_DEFAULT	80

/*
 * The terminal state of one command, held by the process that reads its
 * output. A program uses a terminal in one of two ways. The view keeps both
 * readings at the same time, so the choice follows what the program did and
 * not what it was called:
 *
 * - It writes text and newlines. The useful reading is then the line log,
 *   which is the byte stream without the colour and control sequences, and
 *   with a carriage return as an overwrite of the line. A line is whole, and
 *   the width of the screen does not cut it.
 *
 * - It draws a screen. The useful reading is then the screen that libfyvterm
 *   interprets, because the bytes hold cursor movements and give nothing on
 *   their own.
 *
 * The view enters screen mode when the program first addresses the screen, and
 * it stays there.
 */
struct fyai_terminal_view;

enum fyai_terminal_read {
	FYAITR_NEW,	/* what appeared since the previous read */
	FYAITR_SCREEN,	/* the visible screen */
	FYAITR_ALL,	/* everything the command has produced */
	FYAITR_REGION,	/* a rectangle of the visible screen */
};

struct fyai_terminal_region {
	int row;
	int col;
	int rows;
	int cols;
};

struct fyai_terminal_view *fyai_terminal_view_create(int rows, int cols,
						     size_t max_bytes);
void fyai_terminal_view_destroy(struct fyai_terminal_view *view);

/* Give the view the bytes the program wrote to its terminal. */
int fyai_terminal_view_feed(struct fyai_terminal_view *view, const char *data,
			    size_t len);
void fyai_terminal_view_resize(struct fyai_terminal_view *view, int rows,
			       int cols);

/*
 * Report each line as it is completed, for the live display. Reporting stops
 * when the program takes the screen, because its rows are not lines.
 */
void fyai_terminal_view_line_cb(struct fyai_terminal_view *view,
				shell_output_fn cb, void *userdata);

bool fyai_terminal_view_screen_mode(const struct fyai_terminal_view *view);
bool fyai_terminal_view_binary(const struct fyai_terminal_view *view);
size_t fyai_terminal_view_raw_bytes(const struct fyai_terminal_view *view);
void fyai_terminal_view_size(const struct fyai_terminal_view *view, int *rowsp,
			     int *colsp);


/*
 * The painted reading of the screen. A renderer needs the cells and their
 * colours, and not the text that the model reads. The view therefore gives the
 * cells in a form that does not name libfyvterm, because the header of a
 * producer must not carry the interpreter of the terminal.
 */
#define FYAI_TERM_CELL_CHARS	6

struct fyai_term_color {
	bool is_default;	/* the colour of the terminal, not one chosen */
	unsigned char r, g, b;
};

struct fyai_term_cell {
	uint32_t chars[FYAI_TERM_CELL_CHARS];	/* base then combining */
	int width;				/* 2 for a double-width glyph */
	struct fyai_term_color fg;
	struct fyai_term_color bg;
	bool bold;
	bool underline;
	bool italic;
	bool blink;
	bool reverse;
	bool strike;
};

/* One cell of the visible screen. False when @row or @col is outside it. */
bool fyai_terminal_view_cell(const struct fyai_terminal_view *view, int row,
			     int col, struct fyai_term_cell *cell);

/* Where the program left its cursor, and whether it asked for it to show. */
void fyai_terminal_view_cursor(const struct fyai_terminal_view *view,
			       int *rowp, int *colp, bool *visiblep);

/*
 * The rows that changed since the previous call, inclusive, and false when
 * nothing did. Taking the damage clears it, thus a renderer paints each
 * change one time.
 */
bool fyai_terminal_view_take_damage(struct fyai_terminal_view *view,
				    int *firstp, int *lastp);

/* True when a paint has work: a changed row, or a cursor that moved. */
bool fyai_terminal_view_dirty(const struct fyai_terminal_view *view);

/* Mark the whole screen as changed; the next paint draws all of it. */
void fyai_terminal_view_damage_all(struct fyai_terminal_view *view);

/*
 * Read a bare line feed as a new line. Use it for a program that is not on a
 * terminal and writes one byte where a terminal writes both.
 */
void fyai_terminal_view_cooked(struct fyai_terminal_view *view, bool cooked);

/*
 * Read the view. The caller owns the returned string. @region applies to
 * FYAITR_REGION only. FYAITR_NEW moves the read mark; the other reads do not.
 */
char *fyai_terminal_view_read(struct fyai_terminal_view *view,
			      enum fyai_terminal_read what,
			      const struct fyai_terminal_region *region,
			      size_t *lenp);

/*
 * The last thing the program drew: the row the cursor is on, or the nearest
 * one above it that has anything on it. This is where a prompt is, which is
 * what a program stopped for input has left on the screen. The caller owns
 * the returned string, and gets NULL when the screen is empty.
 */
char *fyai_terminal_view_last_line(const struct fyai_terminal_view *view);

#endif
