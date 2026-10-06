/*
 * fyai_chrome_test.c - unit tests for the chrome policy of a tile
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_DISPLAY

#include <stdio.h>
#include <string.h>

#include "fyai.h"
#include "fyai_chrome.h"

#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(chrome, none_leaves_a_tile_bare, chrome_none)
FYAI_TEST_ENTRY(chrome, zoom_gives_controls_and_no_bar, chrome_zoom)
FYAI_TEST_ENTRY(chrome, full_gives_every_screen_a_bar, chrome_full)
FYAI_TEST_ENTRY(chrome, a_committed_tile_has_no_buttons, chrome_committed)
FYAI_TEST_ENTRY(chrome, a_tile_of_text_has_no_bar, chrome_text)
FYAI_TEST_ENTRY(chrome, a_tool_exchange_scrolls, chrome_tool_text)
FYAI_TEST_ENTRY(chrome, the_bar_follows_the_rows_it_shows, chrome_bar)
FYAI_TEST_ENTRY(chrome, a_row_divides_into_margin_bar_and_body, chrome_frame)

static int failures;

static void expect_items(const char *what, unsigned int got, unsigned int want)
{
	if (got == want)
		return;
	fprintf(stderr, "FAIL %s\n  got:  %#x\n  want: %#x\n", what, got, want);
	failures++;
}

static unsigned int items(const char *controls,
			  enum fyai_workpane_tile_kind kind,
			  enum fyai_chrome_phase phase)
{
	struct fyai_cfg cfg = { .work_controls = controls };
	struct fyai_ctx ctx = { .cfg = &cfg };

	return fyai_chrome_items(&ctx, kind, phase);
}

int chrome_none(void)
{
	failures = 0;
	expect_items("shell running", items("none", FYAI_WORKPANE_TILE_SHELL,
					    FYAI_CHROME_RUNNING), 0);
	expect_items("agent kept", items("none", FYAI_WORKPANE_TILE_AGENT,
					 FYAI_CHROME_KEPT), 0);
	return failures;
}

int chrome_zoom(void)
{
	const unsigned int want = FYAI_CHROME_BUTTONS | FYAI_CHROME_WHEEL;

	failures = 0;
	expect_items("shell running", items("zoom", FYAI_WORKPANE_TILE_SHELL,
					    FYAI_CHROME_RUNNING), want);
	expect_items("agent running", items("zoom", FYAI_WORKPANE_TILE_AGENT,
					    FYAI_CHROME_RUNNING), want);
	return failures;
}

int chrome_full(void)
{
	const unsigned int want = FYAI_CHROME_BUTTONS | FYAI_CHROME_WHEEL |
				  FYAI_CHROME_SCROLL;

	failures = 0;
	expect_items("shell running", items("full", FYAI_WORKPANE_TILE_SHELL,
					    FYAI_CHROME_RUNNING), want);
	expect_items("agent kept", items("full", FYAI_WORKPANE_TILE_AGENT,
					 FYAI_CHROME_KEPT), want);
	expect_items("agent view", items("full", FYAI_WORKPANE_TILE_AGENT_VIEW,
					 FYAI_CHROME_RUNNING), want);
	if (fyai_chrome_scroll_cols(want) != 1 ||
	    fyai_chrome_scroll_cols(FYAI_CHROME_BUTTONS) != 0) {
		fprintf(stderr, "FAIL the scroll bar column\n");
		failures++;
	}
	return failures;
}

int chrome_committed(void)
{
	failures = 0;
	expect_items("committed shell", items("full", FYAI_WORKPANE_TILE_SHELL,
					      FYAI_CHROME_COMMITTED),
		     FYAI_CHROME_WHEEL | FYAI_CHROME_SCROLL);
	return failures;
}

int chrome_text(void)
{
	failures = 0;
	expect_items("notice", items("full", FYAI_WORKPANE_TILE_NOTICE,
				     FYAI_CHROME_RUNNING),
		     FYAI_CHROME_BUTTONS | FYAI_CHROME_WHEEL);
	return failures;
}

static void expect_frame(const char *what, struct fyai_chrome_frame got,
			 int margin, int bar, int body)
{
	if (got.margin == margin && got.bar == bar && got.body == body)
		return;
	fprintf(stderr, "FAIL %s\n  got:  %d %d %d\n  want: %d %d %d\n", what,
		got.margin, got.bar, got.body, margin, bar, body);
	failures++;
}

int chrome_frame(void)
{
	failures = 0;
	expect_frame("margin and bar", fyai_chrome_frame(FYAI_CHROME_SCROLL, 2, 40),
		     2, 1, 37);
	expect_frame("no bar asked", fyai_chrome_frame(FYAI_CHROME_BUTTONS, 2, 40),
		     2, 0, 38);
	expect_frame("a margin wider than the row", fyai_chrome_frame(0, 5, 3),
		     3, 0, 0);
	/* A bar needs a column of the body beside it. */
	expect_frame("no room for a bar", fyai_chrome_frame(FYAI_CHROME_SCROLL, 2, 3),
		     2, 0, 1);
	expect_frame("empty", fyai_chrome_frame(FYAI_CHROME_SCROLL, 0, 0), 0, 0, 0);
	return failures;
}

int chrome_tool_text(void)
{
	failures = 0;
	expect_items("tool exchange", items("full", FYAI_WORKPANE_TILE_TEXT,
					    FYAI_CHROME_RUNNING),
		     FYAI_CHROME_BUTTONS | FYAI_CHROME_WHEEL |
		     FYAI_CHROME_SCROLL);
	return failures;
}

static void expect_part(const char *what, enum fyai_chrome_bar_part got,
			enum fyai_chrome_bar_part want)
{
	if (got == want)
		return;
	fprintf(stderr, "FAIL %s\n  got:  %d\n  want: %d\n", what, got, want);
	failures++;
}

int chrome_bar(void)
{
	int i;

	failures = 0;
	/* 10 rows of which 5 show at the end: arrows, then the thumb below. */
	expect_part("up arrow", fyai_chrome_bar_part(10, 5, 5, 5, true, 0),
		    FYAI_CHROME_BAR_UP);
	expect_part("down arrow", fyai_chrome_bar_part(10, 5, 5, 5, true, 4),
		    FYAI_CHROME_BAR_DOWN);
	/* The track is three rows; the thumb is two (ceil(3 * 5 / 10)). */
	expect_part("thumb at the end", fyai_chrome_bar_part(10, 5, 5, 5, true, 3),
		    FYAI_CHROME_BAR_THUMB);
	expect_part("track above the thumb",
		    fyai_chrome_bar_part(10, 5, 5, 5, true, 1),
		    FYAI_CHROME_BAR_TRACK);
	/* At the start the thumb is on top. */
	expect_part("thumb at the start", fyai_chrome_bar_part(10, 0, 5, 5, true, 1),
		    FYAI_CHROME_BAR_THUMB);
	/* A bar of two rows has no arrows. */
	expect_part("no arrows when short", fyai_chrome_bar_part(10, 0, 2, 2, true, 0),
		    FYAI_CHROME_BAR_THUMB);
	/* A top past the end is held at the end. */
	expect_part("top clamps", fyai_chrome_bar_part(10, 99, 5, 5, true, 3),
		    FYAI_CHROME_BAR_THUMB);
	/* Content that fits fills the track. */
	for (i = 1; i < 4; i++)
		expect_part("short content", fyai_chrome_bar_part(3, 0, 5, 5, true, i),
			    FYAI_CHROME_BAR_THUMB);
	return failures;
}
