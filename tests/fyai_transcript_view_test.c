/* SPDX-License-Identifier: MIT */
/*
 * fyai_transcript_view_test.c - the rows and the window of the transcript view
 * of a fullscreen page.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai_test.h"
#include "fyai_transcript_view.h"

#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(transcript_view, window_shows_the_last_rows, transcript_view_window_shows_the_last_rows)
FYAI_TEST_ENTRY(transcript_view, scroll_stops_at_the_ends, transcript_view_scroll_stops_at_the_ends)
FYAI_TEST_ENTRY(transcript_view, live_rows_follow_the_stored, transcript_view_live_rows_follow_the_stored)
FYAI_TEST_ENTRY(transcript_view, scrolled_view_keeps_its_top, transcript_view_scrolled_view_keeps_its_top)
FYAI_TEST_ENTRY(transcript_view, copy_reads_the_selected_cells, transcript_view_copy_reads_the_selected_cells)
FYAI_TEST_ENTRY(transcript_view, update_renders_what_changed, transcript_view_update_renders_what_changed)
FYAI_TEST_ENTRY(transcript_view, update_keeps_the_row_at_the_top, transcript_view_update_keeps_the_row_at_the_top)
FYAI_TEST_ENTRY(transcript_view, update_renders_what_shows, transcript_view_update_renders_what_shows)

/* Whether the @count rows of @w are @want, one row for each string. */
static bool rows_are(const char *const *w, int count, const char *const *want,
		     int nwant)
{
	int i;

	if (count != nwant)
		return false;
	for (i = 0; i < count; i++)
		if (!w || strcmp(w[i], want[i]))
			return false;
	return true;
}

/* A region shows the last rows of the transcript, or all of them from its
 * top when they are fewer than its rows. */
int transcript_view_window_shows_the_last_rows(void)
{
	static const char *const last[] = { "c", "d" };
	static const char *const all[] = { "a", "b", "c", "d" };
	struct fyai_transcript_view *v = fyai_transcript_view_create();
	const char *const *w;
	int n;

	FYAI_TCHECK(v != NULL);
	FYAI_TCHECK(!fyai_transcript_view_set_stored(v, "a\nb\nc\nd\n", 8));
	FYAI_TCHECK(fyai_transcript_view_rows(v) == 4);
	w = fyai_transcript_view_window(v, 2, &n);
	FYAI_TCHECK(rows_are(w, n, last, 2));
	w = fyai_transcript_view_window(v, 10, &n);
	FYAI_TCHECK(rows_are(w, n, all, 4));
	/* A last line without a newline is a row too. */
	FYAI_TCHECK(!fyai_transcript_view_set_stored(v, "a\nb", 3));
	FYAI_TCHECK(fyai_transcript_view_rows(v) == 2);
	FYAI_TCHECK(!fyai_transcript_view_set_stored(v, "", 0));
	FYAI_TCHECK(fyai_transcript_view_rows(v) == 0);
	w = fyai_transcript_view_window(v, 3, &n);
	FYAI_TCHECK(n == 0);
	FYAI_TCHECK(fyai_transcript_view_window(v, 0, &n) == NULL && n == 0);
	fyai_transcript_view_destroy(v);
	return 0;
}

/* Scrolling goes back to the first row and forward to the end, no further. */
int transcript_view_scroll_stops_at_the_ends(void)
{
	static const char *const top[] = { "a", "b" };
	static const char *const mid[] = { "b", "c" };
	static const char *const end[] = { "d", "e" };
	struct fyai_transcript_view *v = fyai_transcript_view_create();
	const char *const *w;
	int n;

	FYAI_TCHECK(v != NULL);
	FYAI_TCHECK(!fyai_transcript_view_set_stored(v, "a\nb\nc\nd\ne\n", 10));
	FYAI_TCHECK(fyai_transcript_view_at_end(v));
	fyai_transcript_view_scroll(v, 2, 2);
	FYAI_TCHECK(!fyai_transcript_view_at_end(v));
	w = fyai_transcript_view_window(v, 2, &n);
	FYAI_TCHECK(rows_are(w, n, mid, 2));
	fyai_transcript_view_scroll(v, 100, 2);
	w = fyai_transcript_view_window(v, 2, &n);
	FYAI_TCHECK(rows_are(w, n, top, 2));
	fyai_transcript_view_scroll(v, -100, 2);
	FYAI_TCHECK(fyai_transcript_view_at_end(v));
	w = fyai_transcript_view_window(v, 2, &n);
	FYAI_TCHECK(rows_are(w, n, end, 2));
	/* A region as tall as the transcript has nowhere to scroll. */
	fyai_transcript_view_scroll(v, 3, 5);
	FYAI_TCHECK(fyai_transcript_view_at_end(v));
	fyai_transcript_view_destroy(v);
	return 0;
}

/* The rows of the turn in flight follow the stored rows, a row a commit did
 * not end continues, and storing the turn drops them. */
int transcript_view_live_rows_follow_the_stored(void)
{
	static const char *const rows[] = { "stored", "live one", "live two" };
	static const char *const stored[] = { "stored" };
	struct fyai_transcript_view *v = fyai_transcript_view_create();
	const char *const *w;
	int n;

	FYAI_TCHECK(v != NULL);
	FYAI_TCHECK(!fyai_transcript_view_set_stored(v, "stored\n", 7));
	FYAI_TCHECK(!fyai_transcript_view_append_live(v, "live ", 5));
	FYAI_TCHECK(!fyai_transcript_view_append_live(v, "one\nlive two\n", 13));
	FYAI_TCHECK(!fyai_transcript_view_append_live(v, NULL, 0));
	FYAI_TCHECK(fyai_transcript_view_append_live(v, NULL, 1) == -1);
	w = fyai_transcript_view_window(v, 5, &n);
	FYAI_TCHECK(rows_are(w, n, rows, 3));
	/* New stored rows leave the live rows until the turn is stored. */
	FYAI_TCHECK(!fyai_transcript_view_set_stored(v, "stored\n", 7));
	FYAI_TCHECK(fyai_transcript_view_rows(v) == 3);
	fyai_transcript_view_clear_live(v);
	w = fyai_transcript_view_window(v, 5, &n);
	FYAI_TCHECK(rows_are(w, n, stored, 1));
	fyai_transcript_view_destroy(v);
	return 0;
}

/* A view the user scrolled back stays on the rows being read while new rows
 * arrive; a view at the end follows them. */
int transcript_view_scrolled_view_keeps_its_top(void)
{
	static const char *const read[] = { "a", "b" };
	static const char *const follow[] = { "x", "y" };
	struct fyai_transcript_view *v = fyai_transcript_view_create();
	const char *const *w;
	int n;

	FYAI_TCHECK(v != NULL);
	FYAI_TCHECK(!fyai_transcript_view_set_stored(v, "a\nb\nc\nd\n", 8));
	fyai_transcript_view_scroll(v, 2, 2);
	FYAI_TCHECK(!fyai_transcript_view_append_live(v, "x\ny\n", 4));
	w = fyai_transcript_view_window(v, 2, &n);
	FYAI_TCHECK(rows_are(w, n, read, 2));
	fyai_transcript_view_scroll(v, -100, 2);
	FYAI_TCHECK(!fyai_transcript_view_append_live(v, "", 0));
	w = fyai_transcript_view_window(v, 2, &n);
	FYAI_TCHECK(rows_are(w, n, follow, 2));
	fyai_transcript_view_destroy(v);
	return 0;
}

/* A selection copies the text in its cells as the rows read: from its first
 * cell to its last in reading order, without styles, links or the blanks that
 * end a row, with a wide character taken whole. */
int transcript_view_copy_reads_the_selected_cells(void)
{
	static const char rows[] =
		"  \x1b[1mHello\x1b[0m world   \n"
		"  second row\n"
		"\x1b]8;;http://example\x1b\\link\x1b]8;;\x1b\\ \xe6\x97\xa5\xe6\x9c\xacx\n";
	struct fyai_transcript_view *v = fyai_transcript_view_create();
	char *text;

	FYAI_TCHECK(v != NULL);
	FYAI_TCHECK(!fyai_transcript_view_set_stored(v, rows, strlen(rows)));
	text = fyai_transcript_view_copy(v, 5, 0, 2, 1, 7);
	FYAI_TCHECK(text && !strcmp(text, "Hello world\n  second"));
	free(text);
	/* The ends of a drag back up the screen are the same selection. */
	text = fyai_transcript_view_copy(v, 5, 1, 7, 0, 2);
	FYAI_TCHECK(text && !strcmp(text, "Hello world\n  second"));
	free(text);
	text = fyai_transcript_view_copy(v, 5, 2, 0, 2, 3);
	FYAI_TCHECK(text && !strcmp(text, "link"));
	free(text);
	/* Columns 5 to 7 are the two wide characters: the second starts on 7. */
	text = fyai_transcript_view_copy(v, 5, 2, 6, 2, 7);
	FYAI_TCHECK(text && !strcmp(text, "\xe6\x9c\xac"));
	free(text);
	/* A row below the view has no text. */
	text = fyai_transcript_view_copy(v, 5, 4, 0, 4, 9);
	FYAI_TCHECK(text && !strcmp(text, ""));
	free(text);
	FYAI_TCHECK(fyai_transcript_view_copy(NULL, 5, 0, 0, 0, 1) == NULL);
	fyai_transcript_view_destroy(v);
	return 0;
}

/* A render of exchange @key at a width: @rows rows named for both. */
struct fake_render {
	const uintptr_t *keys;
	int rows;		/* rows of an exchange */
	int calls;		/* the renders so far */
	int fail_at;		/* the call that fails, or -1 */
};

static int fake_render(void *user, size_t index, int width, char **textp,
		       size_t *lenp)
{
	struct fake_render *f = user;
	char row[64];
	FILE *fp;
	int i;

	if (f->calls++ == f->fail_at)
		return -1;
	fp = open_memstream(textp, lenp);
	if (!fp)
		return -1;
	for (i = 0; i < f->rows; i++) {
		snprintf(row, sizeof(row), "x%lu.%d@%d",
			 (unsigned long)f->keys[index], i, width);
		fprintf(fp, "%s\n", row);
	}
	return fclose(fp) ? -1 : 0;
}

/* An update renders the exchanges that are new or at a new width, keeps the
 * others, drops the ones that went, and leaves the view as it was when a
 * render fails. */
int transcript_view_update_renders_what_changed(void)
{
	static const uintptr_t two[] = { 1, 2 };
	static const uintptr_t three[] = { 1, 2, 3 };
	static const uintptr_t other[] = { 3 };
	static const char *const tail[] = { "x3.0@40", "x3.1@40" };
	struct fyai_transcript_view *v = fyai_transcript_view_create();
	struct fake_render f = { .rows = 2, .fail_at = -1 };
	const char *const *w;
	int n;

	FYAI_TCHECK(v != NULL);
	f.keys = two;
	FYAI_TCHECK(!fyai_transcript_view_update(v, 40, 0, two, 2, fake_render, NULL, &f));
	FYAI_TCHECK(f.calls == 2 && fyai_transcript_view_rows(v) == 4);
	f.keys = three;
	FYAI_TCHECK(!fyai_transcript_view_update(v, 40, 0, three, 3, fake_render, NULL, &f));
	FYAI_TCHECK(f.calls == 3 && fyai_transcript_view_rows(v) == 6);
	w = fyai_transcript_view_window(v, 2, &n);
	FYAI_TCHECK(rows_are(w, n, tail, 2));
	/* Nothing changed: nothing renders. */
	FYAI_TCHECK(!fyai_transcript_view_update(v, 40, 0, three, 3, fake_render, NULL, &f));
	FYAI_TCHECK(f.calls == 3);
	/* A new width makes every row again. */
	FYAI_TCHECK(!fyai_transcript_view_update(v, 30, 0, three, 3, fake_render, NULL, &f));
	FYAI_TCHECK(f.calls == 6);
	w = fyai_transcript_view_window(v, 6, &n);
	FYAI_TCHECK(n == 6 && !strcmp(w[0], "x1.0@30") && !strcmp(w[5], "x3.1@30"));
	/* A failed render leaves the rows of the last update. */
	f.fail_at = f.calls + 1;
	FYAI_TCHECK(fyai_transcript_view_update(v, 20, 0, three, 3, fake_render, NULL, &f) == -1);
	FYAI_TCHECK(fyai_transcript_view_rows(v) == 6);
	f.fail_at = -1;
	FYAI_TCHECK(!fyai_transcript_view_update(v, 30, 0, three, 3, fake_render, NULL, &f));
	w = fyai_transcript_view_window(v, 6, &n);
	FYAI_TCHECK(n == 6 && !strcmp(w[0], "x1.0@30") && !strcmp(w[5], "x3.1@30"));
	/* Exchanges that went are dropped; one that stays keeps its rows. */
	f.keys = other;
	n = f.calls;
	FYAI_TCHECK(!fyai_transcript_view_update(v, 30, 0, other, 1, fake_render, NULL, &f));
	FYAI_TCHECK(f.calls == n && fyai_transcript_view_rows(v) == 2);
	FYAI_TCHECK(fyai_transcript_view_update(v, 30, 0, other, 1, NULL, NULL, &f) == -1);
	fyai_transcript_view_destroy(v);
	return 0;
}

/* A view scrolled back stays on the row at its top through a new width, even
 * when each exchange has another number of rows at that width. */
int transcript_view_update_keeps_the_row_at_the_top(void)
{
	static const uintptr_t keys[] = { 7, 8, 9 };
	struct fyai_transcript_view *v = fyai_transcript_view_create();
	struct fake_render f = { .keys = keys, .rows = 5, .fail_at = -1 };
	const char *const *w;
	int n;

	FYAI_TCHECK(v != NULL);
	FYAI_TCHECK(!fyai_transcript_view_update(v, 40, 0, keys, 3, fake_render, NULL, &f));
	w = fyai_transcript_view_window(v, 4, &n);
	/* Back to the second row of the second exchange: rows 6 to 9 of 15. */
	fyai_transcript_view_scroll(v, 15 - 4 - 6, 4);
	w = fyai_transcript_view_window(v, 4, &n);
	FYAI_TCHECK(n == 4 && !strcmp(w[0], "x8.1@40"));
	f.rows = 8;
	FYAI_TCHECK(!fyai_transcript_view_update(v, 30, 0, keys, 3, fake_render, NULL, &f));
	w = fyai_transcript_view_window(v, 4, &n);
	FYAI_TCHECK(n == 4 && !strcmp(w[0], "x8.1@30"));
	/* A view at the end stays at the end. */
	fyai_transcript_view_scroll(v, -100, 4);
	f.rows = 3;
	FYAI_TCHECK(!fyai_transcript_view_update(v, 20, 0, keys, 3, fake_render, NULL, &f));
	FYAI_TCHECK(fyai_transcript_view_at_end(v));
	w = fyai_transcript_view_window(v, 4, &n);
	FYAI_TCHECK(n == 4 && !strcmp(w[3], "x9.2@20"));
	fyai_transcript_view_destroy(v);
	return 0;
}

static size_t fake_measure(void *user, size_t index, int width)
{
	(void)user;
	(void)index;
	(void)width;
	return 5;
}

/* With a measure, only the exchanges a region shows render; the others count
 * their measured rows until a scroll shows them, and a render that makes
 * other rows than the measure said keeps the view on its rows. */
int transcript_view_update_renders_what_shows(void)
{
	static const uintptr_t keys[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
	struct fyai_transcript_view *v = fyai_transcript_view_create();
	struct fake_render f = { .keys = keys, .rows = 5, .fail_at = -1 };
	const char *const *w;
	int n;

	FYAI_TCHECK(v != NULL);
	FYAI_TCHECK(!fyai_transcript_view_update(v, 40, 4, keys, 10,
						 fake_render, fake_measure, &f));
	/* Four rows show the end of the last exchange only. */
	FYAI_TCHECK(f.calls == 1);
	FYAI_TCHECK(fyai_transcript_view_rows(v) == 50);
	FYAI_TCHECK(!fyai_transcript_view_needs_render(v, 4));
	w = fyai_transcript_view_window(v, 4, &n);
	FYAI_TCHECK(n == 4 && !strcmp(w[0], "x10.1@40") && !strcmp(w[3], "x10.4@40"));
	/* Back twelve rows: rows 34 to 37, the ends of exchanges 7 and 8. */
	fyai_transcript_view_scroll(v, 12, 4);
	FYAI_TCHECK(fyai_transcript_view_needs_render(v, 4));
	FYAI_TCHECK(!fyai_transcript_view_update(v, 40, 4, keys, 10,
						 fake_render, fake_measure, &f));
	FYAI_TCHECK(f.calls == 3);
	FYAI_TCHECK(!fyai_transcript_view_needs_render(v, 4));
	w = fyai_transcript_view_window(v, 4, &n);
	FYAI_TCHECK(n == 4 && !strcmp(w[0], "x7.4@40") && !strcmp(w[1], "x8.0@40"));
	/* A new width measures again and renders only what shows: the row at
	 * the top stays, though each exchange renders to six rows. */
	f.rows = 6;
	FYAI_TCHECK(!fyai_transcript_view_update(v, 30, 4, keys, 10,
						 fake_render, fake_measure, &f));
	FYAI_TCHECK(f.calls <= 3 + 3);
	w = fyai_transcript_view_window(v, 4, &n);
	FYAI_TCHECK(n == 4 && !strcmp(w[0], "x7.4@30"));
	fyai_transcript_view_destroy(v);
	return 0;
}
