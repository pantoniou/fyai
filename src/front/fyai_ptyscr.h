/*
 * fyai_ptyscr.h - terminal screen model for the PTY driver
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_PTYSCR_H
#define FYAI_PTYSCR_H

#include <regex.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

struct fyvt;
struct fyvt_screen;
struct fyvt_state;

struct fyai_ptyscr_list {
	char **v;
	size_t n;
	size_t cap;
};

/*
 * What a person sees of a terminal, rebuilt from the bytes it was sent: the
 * cells, the cursor, the rows that left the top, the rows that an erase of the
 * display removed, and the text of each OSC 52 copy. The emulator is
 * libfyvterm; the lists come from its callbacks. The structure is not moved
 * after fyai_ptyscr_init(): the emulator keeps its address.
 */
struct fyai_ptyscr {
	struct fyvt *vt;
	struct fyvt_screen *screen;
	struct fyvt_state *state;
	int rows;
	int cols;
	struct fyai_ptyscr_list scrollback;
	struct fyai_ptyscr_list erased;
	struct fyai_ptyscr_list clipboard;
	size_t display_erases;		/* erases of the whole display */
	char *copy;			/* a copy that arrives in pieces */
	size_t ncopy;
	char selbuf[4096];		/* the emulator decodes a copy here */
	bool emoji_narrow;		/* U+FE0F joins the cell before it */
	bool cursor_hidden;		/* the program hid the cursor */
	bool failed;			/* an allocation failed */
};

/* Make a blank screen. Return -1 when memory is short. */
int fyai_ptyscr_init(struct fyai_ptyscr *s, int rows, int cols,
		     bool emoji_narrow);
void fyai_ptyscr_destroy(struct fyai_ptyscr *s);
/*
 * Make the screen blank at a new size. The copies and the narrow-emoji mode
 * stay: a copy is terminal state, and the window does not take it away.
 */
int fyai_ptyscr_resize(struct fyai_ptyscr *s, int rows, int cols);
void fyai_ptyscr_feed(struct fyai_ptyscr *s, const void *data, size_t len);
/* The cursor, from zero. */
void fyai_ptyscr_cursor(const struct fyai_ptyscr *s, int *row, int *col);

/*
 * Write row @r without the blanks at its end to @buf, which holds at least
 * fyai_ptyscr_row_size() bytes. Return the length.
 */
size_t fyai_ptyscr_row_size(const struct fyai_ptyscr *s);
size_t fyai_ptyscr_row(const struct fyai_ptyscr *s, int r, char *buf);

/*
 * Whether @text is in a row of the display, or in the scrollback from
 * @scrolled, or in the erased rows from @erased. With @whole, a row must be
 * @text alone with the blanks at both ends removed.
 */
bool fyai_ptyscr_has(const struct fyai_ptyscr *s, const char *text,
		     size_t scrolled, size_t erased, bool whole);
/* The first cell of the first @text on the display. */
bool fyai_ptyscr_locate(const struct fyai_ptyscr *s, const char *text,
			int *row, int *col);

/*
 * Write the screen to @f as text that two screens compare by: a line "B|" for
 * each row that left the top, with its text; a line for each
 * row, "R|" and its text without the blanks at its end; a line "S|ROW|" with
 * the style runs of each row that has a style, "COL-COL STYLE" with the style
 * as the colours in RGB and the attributes; and "C|ROW,COL", the cursor, or
 * "C|hidden". A
 * cell under a match of one of the @nmasks patterns of @masks is '?' with the
 * style '*'. Return -1 when memory is short or the write fails.
 */
int fyai_ptyscr_dump(const struct fyai_ptyscr *s, const regex_t *masks,
		     size_t nmasks, FILE *f);

#endif
