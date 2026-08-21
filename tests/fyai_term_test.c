/*
 * fyai_term_test.c - the cells fyai publishes to a surface
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 *
 * The drawing belongs to libfytimui, which proves what a cell looks like on a
 * terminal in its own suite. What fyai owns is the reading: the cells and the
 * damage that the terminal view reports from the bytes a program wrote.
 */

#include <string.h>

#include "fyai_test.h"
#include "fyai_test_registry.h"

#include "fyai_terminal_view.h"

FYAI_TEST_ENTRY(term, view_cells, term_view_cells)
FYAI_TEST_ENTRY(term, view_damage, term_view_damage)
FYAI_TEST_ENTRY(term, view_cursor, term_view_cursor)

static void feed(struct fyai_terminal_view *view, const char *bytes)
{
	FYAI_TCHECK(!fyai_terminal_view_feed(view, bytes, strlen(bytes)));
}

int term_view_cells(void)
{
	struct fyai_terminal_view *view;
	struct fyai_term_cell cell;

	view = fyai_terminal_view_create(NULL, 4, 20, 0);
	FYAI_TCHECK(view != NULL);

	feed(view, "a\033[1;38;2;10;20;30mB\033[0m \344\270\255");

	/* Plain text keeps the colours of the terminal. */
	FYAI_TCHECK(fyai_terminal_view_cell(view, 0, 0, &cell));
	FYAI_TCHECK(cell.chars[0] == 'a');
	FYAI_TCHECK(cell.fg.is_default);
	FYAI_TCHECK(cell.bg.is_default);
	FYAI_TCHECK(!cell.bold);

	/* A colour the program chose is reported as RGB, with its attribute. */
	FYAI_TCHECK(fyai_terminal_view_cell(view, 0, 1, &cell));
	FYAI_TCHECK(cell.chars[0] == 'B');
	FYAI_TCHECK(cell.bold);
	FYAI_TCHECK(!cell.fg.is_default);
	FYAI_TCHECK(cell.fg.r == 10 && cell.fg.g == 20 && cell.fg.b == 30);

	/* A double-width glyph says so, so that the filler cell is stepped
	 * over instead of drawn. */
	FYAI_TCHECK(fyai_terminal_view_cell(view, 0, 3, &cell));
	FYAI_TCHECK(cell.chars[0] == 0x4E2D);
	FYAI_TCHECK(cell.width == 2);

	/* A cell the program never wrote is blank. */
	FYAI_TCHECK(fyai_terminal_view_cell(view, 1, 0, &cell));
	FYAI_TCHECK(cell.chars[0] == 0);

	/* Outside the screen there is no cell. */
	FYAI_TCHECK(!fyai_terminal_view_cell(view, 4, 0, &cell));
	FYAI_TCHECK(!fyai_terminal_view_cell(view, 0, 20, &cell));
	FYAI_TCHECK(!fyai_terminal_view_cell(view, -1, 0, &cell));

	fyai_terminal_view_destroy(view);
	return 0;
}

int term_view_damage(void)
{
	struct fyai_terminal_view *view;
	int first = -1, last = -1;

	view = fyai_terminal_view_create(NULL, 6, 10, 0);
	FYAI_TCHECK(view != NULL);

	/* A new view has the whole screen to draw. */
	FYAI_TCHECK(fyai_terminal_view_dirty(view));
	FYAI_TCHECK(fyai_terminal_view_take_damage(view, &first, &last));
	FYAI_TCHECK(first == 0 && last == 5);

	/* Taking it clears it: a repaint draws each change one time. */
	FYAI_TCHECK(!fyai_terminal_view_dirty(view));
	FYAI_TCHECK(!fyai_terminal_view_take_damage(view, &first, &last));

	/* Only the row the program wrote is reported. */
	feed(view, "\033[3;1Hxy");
	FYAI_TCHECK(fyai_terminal_view_dirty(view));
	FYAI_TCHECK(fyai_terminal_view_take_damage(view, &first, &last));
	FYAI_TCHECK(first == 2 && last == 2);

	/* A resize changes the whole picture. */
	fyai_terminal_view_resize(view, 6, 14);
	FYAI_TCHECK(fyai_terminal_view_take_damage(view, &first, &last));
	FYAI_TCHECK(first == 0 && last == 5);

	fyai_terminal_view_destroy(view);
	return 0;
}

int term_view_cursor(void)
{
	struct fyai_terminal_view *view;
	int row = -1, col = -1;
	bool visible = false;

	view = fyai_terminal_view_create(NULL, 5, 10, 0);
	FYAI_TCHECK(view != NULL);

	/* The cursor is where the program left it, not where its text ends. */
	feed(view, "hello\033[4;3H");
	fyai_terminal_view_cursor(view, &row, &col, &visible);
	FYAI_TCHECK(row == 3 && col == 2);
	FYAI_TCHECK(visible);

	/* A program hiding the cursor is presentation the view carries. */
	feed(view, "\033[?25l");
	fyai_terminal_view_cursor(view, &row, &col, &visible);
	FYAI_TCHECK(!visible);
	feed(view, "\033[?25h");
	fyai_terminal_view_cursor(view, &row, &col, &visible);
	FYAI_TCHECK(visible);

	fyai_terminal_view_destroy(view);
	return 0;
}
