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
FYAI_TEST_ENTRY(term, view_cooked, term_view_cooked)
FYAI_TEST_ENTRY(term, view_reply, term_view_reply)

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

/*
 * A program that is not on a terminal ends a line with a line feed alone. The
 * view reads it as the end of the line, or every line starts where the last
 * one ended and the text walks down the screen.
 */
int term_view_cooked(void)
{
	struct fyai_terminal_view *view;
	struct fyai_term_cell cell;

	view = fyai_terminal_view_create(NULL, 4, 20, 0);
	FYAI_TCHECK(view != NULL);

	/* As a terminal is: the carriage stays where the program left it. */
	feed(view, "one\ntwo");
	FYAI_TCHECK(fyai_terminal_view_cell(view, 1, 3, &cell));
	FYAI_TCHECK(cell.chars[0] == 't');
	FYAI_TCHECK(fyai_terminal_view_cell(view, 1, 0, &cell));
	FYAI_TCHECK(cell.chars[0] == 0);

	fyai_terminal_view_destroy(view);

	view = fyai_terminal_view_create(NULL, 4, 20, 0);
	FYAI_TCHECK(view != NULL);
	fyai_terminal_view_cooked(view, true);
	/* The bytes that asked for it are not the program's output. */
	FYAI_TCHECK(!fyai_terminal_view_raw_bytes(view));

	feed(view, "one\ntwo");
	FYAI_TCHECK(fyai_terminal_view_cell(view, 1, 0, &cell));
	FYAI_TCHECK(cell.chars[0] == 't');
	FYAI_TCHECK(fyai_terminal_view_cell(view, 0, 0, &cell));
	FYAI_TCHECK(cell.chars[0] == 'o');

	/* A carriage return of its own still works. */
	feed(view, "\rX");
	FYAI_TCHECK(fyai_terminal_view_cell(view, 1, 0, &cell));
	FYAI_TCHECK(cell.chars[0] == 'X');

	/* And it can be given back. */
	fyai_terminal_view_cooked(view, false);
	feed(view, "\ny");
	FYAI_TCHECK(fyai_terminal_view_cell(view, 2, 1, &cell));
	FYAI_TCHECK(cell.chars[0] == 'y');

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

struct reply_capture {
	char data[64];
	size_t len;
	int calls;
};

static void reply_taken(const char *data, size_t len, void *user)
{
	struct reply_capture *cap = user;

	if (cap->len + len > sizeof(cap->data))
		return;
	memcpy(cap->data + cap->len, data, len);
	cap->len += len;
	cap->calls++;
}

/*
 * A program can send a query to its terminal and wait for the reply. The view
 * is that terminal, thus it gives the reply to its owner, which can write to
 * the program. Without a reply the program waits for its own time limit, and
 * it then reads the next input bytes as the reply.
 */
int term_view_reply(void)
{
	struct fyai_terminal_view *view;
	struct reply_capture cap = {};

	view = fyai_terminal_view_create(NULL, 4, 20, 0);
	FYAI_TCHECK(view != NULL);
	fyai_terminal_view_reply_cb(view, reply_taken, &cap);

	/* The terminal type: a primary device attributes report. */
	feed(view, "\033[c");
	FYAI_TCHECK(cap.calls == 1);
	FYAI_TCHECK(cap.len > 2);
	FYAI_TCHECK(cap.data[0] == '\033' && cap.data[1] == '[');
	FYAI_TCHECK(cap.data[cap.len - 1] == 'c');

	/* The cursor position, after the text moved the cursor. */
	cap.len = 0;
	cap.calls = 0;
	feed(view, "ab\033[6n");
	FYAI_TCHECK(cap.calls == 1);
	FYAI_TCHECK(cap.len == strlen("\033[1;3R"));
	FYAI_TCHECK(!memcmp(cap.data, "\033[1;3R", cap.len));

	/* Plain text is not a query, thus there is no reply. */
	cap.calls = 0;
	feed(view, "cd");
	FYAI_TCHECK(!cap.calls);

	fyai_terminal_view_destroy(view);
	return 0;
}
