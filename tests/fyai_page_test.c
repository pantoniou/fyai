/*
 * fyai_page_test.c - the source and the layout of the UI Markdown page
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 *
 * The page source is a function of one frame of state. These tests read the
 * source, and render it with libfymd4c to check where the rows land and which
 * chrome goes first on a short terminal. What a slot draws is the terminal
 * library's, which proves it in its own tests.
 */

#define FYAI_MODULE FYAIEM_UNKNOWN

#include <stdlib.h>
#include <string.h>

#include <libfymd4c.h>

#include "fyai.h"
#include "fyai_test.h"
#include "fyai_page.h"
#include "fyai_markdown.h"
#include "fyai_workpane.h"
#include "utils.h"
#include <libfytimui.h>

#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(page, source_orders_the_chrome, page_source_orders_the_chrome)
FYAI_TEST_ENTRY(page, source_escapes_text, page_source_escapes_text)
FYAI_TEST_ENTRY(page, source_omits_empty_slots, page_source_omits_empty_slots)
FYAI_TEST_ENTRY(page, rows_are_adjacent, page_rows_are_adjacent)
FYAI_TEST_ENTRY(page, status_drops_first, page_status_drops_first)
FYAI_TEST_ENTRY(page, blank_activity_is_not_code, page_blank_activity_is_not_code)
FYAI_TEST_ENTRY(page, cap_stands_over_the_pane, page_cap_stands_over_the_pane)
FYAI_TEST_ENTRY(page, prompt_card_takes_the_rules, page_prompt_card_takes_the_rules)
FYAI_TEST_ENTRY(page, chrome_keeps_the_margins, page_chrome_keeps_the_margins)
FYAI_TEST_ENTRY(page, fit_gives_the_chrome_its_rows, page_fit_gives_the_chrome_its_rows)
FYAI_TEST_ENTRY(page, view_follows_the_presentation, page_view_follows_the_presentation)
FYAI_TEST_ENTRY(page, grid_places_the_tiles, page_grid_places_the_tiles)
FYAI_TEST_ENTRY(page, grid_shares_the_height, page_grid_shares_the_height)
FYAI_TEST_ENTRY(page, grid_fits_and_spans, page_grid_fits_and_spans)
FYAI_TEST_ENTRY(page, grid_stands_heads_level, page_grid_stands_heads_level)
FYAI_TEST_ENTRY(page, grid_names_what_the_page_draws, page_grid_names_what_the_page_draws)

static struct fyai_page_state page_state(void)
{
	struct fyai_page_state st;

	memset(&st, 0, sizeof(st));
	st.header = "HEADMARK";
	st.status = "STATUSMARK";
	st.prompt_rows = 1;
	return st;
}

static const char *after(const char *hay, const char *needle)
{
	const char *hit = hay ? strstr(hay, needle) : NULL;

	return hit ? hit + strlen(needle) : NULL;
}

static int page_source_orders_the_chrome_run(void)
{
	struct response_buffer out = {0};
	struct fyai_page_state st = page_state();
	const char *p;
	int rc;

	st.tail_rows = 2;
	st.pane_rows = 3;
	rc = fyai_page_source(&st, &out);
	FYAI_TCHECK(!rc);
	p = after(out.data, "<fy-tight>");
	p = after(p, "<fy-slot id=\"tail\" height=\"2\"/>");
	p = after(p, "<fy-slot id=\"pane\" height=\"3\"/>");
	p = after(p, "<fy-drop order=\"2\">");
	p = after(p, "HEADMARK");
	p = after(p, "<fy-slot id=\"prompt\" height=\"1\"/>");
	p = after(p, "<fy-drop order=\"1\">");
	p = after(p, "STATUSMARK");
	FYAI_TCHECK(p != NULL);
	free(out.data);
	return 0;
}

static int page_source_escapes_text_run(void)
{
	struct response_buffer out = {0};
	struct fyai_page_state st = page_state();
	int rc;

	st.header = "<fy-act id=\"evil\">click</fy-act>";
	st.status = "\x1b[1mbold\x1b[0m line\nbreak";
	st.activity = "\x1b[33m*\x1b[0m";
	rc = fyai_page_source(&st, &out);
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(!strstr(out.data, "<fy-act id=\"evil\""));
	FYAI_TCHECK(strstr(out.data, "click") != NULL);
	FYAI_TCHECK(!strchr(out.data, '\x1b'));
	FYAI_TCHECK(strstr(out.data, "bold line break") != NULL);
	free(out.data);
	return 0;
}

static int page_source_omits_empty_slots_run(void)
{
	struct response_buffer out = {0};
	struct fyai_page_state st = page_state();
	const char *p;
	int rc;

	rc = fyai_page_source(&st, &out);
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(!strstr(out.data, "id=\"tail\""));
	FYAI_TCHECK(!strstr(out.data, "id=\"pane\""));
	FYAI_TCHECK(!strstr(out.data, "id=\"completion\""));
	free(out.data);

	/* a pane below the prompt comes after the status */
	out = (struct response_buffer){0};
	st.pane_rows = 4;
	st.pane_below = true;
	rc = fyai_page_source(&st, &out);
	FYAI_TCHECK(!rc);
	p = after(out.data, "STATUSMARK");
	FYAI_TCHECK(after(p, "<fy-slot id=\"pane\" height=\"4\"/>") != NULL);
	free(out.data);

	/* completion takes the place of the hint, and no prompt has no rules */
	out = (struct response_buffer){0};
	st = page_state();
	st.completion = true;
	st.hint = "HINTMARK";
	st.prompt_rows = 0;
	rc = fyai_page_source(&st, &out);
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(strstr(out.data, "id=\"completion\"") != NULL);
	FYAI_TCHECK(!strstr(out.data, "HINTMARK"));
	FYAI_TCHECK(!strstr(out.data, "id=\"prompt\""));
	FYAI_TCHECK(!strstr(out.data, "fy-fill char"));
	free(out.data);
	return 0;
}

static const char *no_margin(void *userdata, size_t row)
{
	(void)userdata;
	(void)row;
	return "";
}

/* Render @st at 40 columns and @height rows; the output and its regions. */
static char *page_render(const struct fyai_page_state *st, int height,
			 struct fymd_renderer **rp)
{
	struct response_buffer src = {0};
	struct fymd_renderer_cfg cfg;
	struct fymd_renderer *r;
	char *out = NULL;
	size_t len = 0;
	int rc;

	rc = fyai_page_source(st, &src);
	FYAI_TCHECK(!rc);
	memset(&cfg, 0, sizeof(cfg));
	cfg.flags = FYMD_RF_DEFAULT | FYMD_RF_UI | FYMD_RF_NO_COLOR;
	cfg.sgr_input = FYMD_SGR_SAFE;
	cfg.width = 40 + FYAI_PAGE_RIGHT_MARGIN;
	r = fymd_renderer_create(&cfg);
	FYAI_TCHECK(r != NULL);
	rc = fymd_renderer_set_height(r, height);
	FYAI_TCHECK(!rc);
	rc = fymd_render_with_margins(r, src.data, src.len, no_margin, NULL,
				      &out, &len);
	FYAI_TCHECK(!rc && out != NULL);
	free(src.data);
	*rp = r;
	return out;
}

static int row_of(const char *out, const char *needle)
{
	const char *hit = strstr(out, needle), *p;
	int n = 0;

	if (!hit)
		return -1;
	for (p = out; p < hit; p++)
		n += *p == '\n';
	return n;
}

static int rows_of(const char *out)
{
	int n = 0;

	for (; *out; out++)
		n += *out == '\n';
	return n;
}

static const struct fymd_region *region(struct fymd_renderer *r,
					const char *id)
{
	const struct fymd_region *rg;
	size_t count, i;

	if (fymd_renderer_get_regions(r, &rg, &count))
		return NULL;
	for (i = 0; i < count; i++)
		if (!strcmp(rg[i].id, id))
			return &rg[i];
	return NULL;
}

/* The chrome stands on adjacent rows: header, rule, prompt, rule, status. */
static int page_rows_are_adjacent_run(void)
{
	struct fyai_page_state st = page_state();
	const struct fymd_region *tail, *prompt;
	struct fymd_renderer *r;
	char *out;
	int head;

	st.tail_rows = 2;
	out = page_render(&st, 0, &r);
	tail = region(r, "tail");
	prompt = region(r, "prompt");
	head = row_of(out, "HEADMARK");
	FYAI_TCHECK(tail != NULL && tail->row == 0 && tail->height == 2);
	FYAI_TCHECK(head == 2);
	FYAI_TCHECK(prompt != NULL && (int)prompt->row == head + 2);
	/* the hint row stands between the lower rule and the status */
	FYAI_TCHECK(row_of(out, "STATUSMARK") == head + 5);
	FYAI_TCHECK(prompt->col == 0 && prompt->width == 40);
	fymd_free(out);
	fymd_renderer_destroy(r);
	return 0;
}

/* A short terminal loses the status first, then the header; never the
 * prompt. */
static int page_status_drops_first_run(void)
{
	struct fyai_page_state st = page_state();
	struct fymd_renderer *r;
	char *out;
	int natural;

	out = page_render(&st, 0, &r);
	natural = rows_of(out);
	fymd_free(out);
	fymd_renderer_destroy(r);

	out = page_render(&st, natural - 1, &r);
	FYAI_TCHECK(!strstr(out, "STATUSMARK"));
	FYAI_TCHECK(strstr(out, "HEADMARK") != NULL);
	FYAI_TCHECK(region(r, "prompt") != NULL);
	fymd_free(out);
	fymd_renderer_destroy(r);

	out = page_render(&st, 1, &r);
	FYAI_TCHECK(!strstr(out, "STATUSMARK"));
	FYAI_TCHECK(!strstr(out, "HEADMARK"));
	FYAI_TCHECK(region(r, "prompt") != NULL);
	fymd_free(out);
	fymd_renderer_destroy(r);
	return 0;
}

/*
 * A blank activity mark and a status that starts with blanks are not an
 * indented code block: the status stays one row under the lower rule.
 */
static int page_blank_activity_is_not_code_run(void)
{
	struct fyai_page_state st = page_state();
	struct fymd_renderer *r;
	char *out;
	int natural, status;

	out = page_render(&st, 0, &r);
	natural = rows_of(out);
	fymd_free(out);
	fymd_renderer_destroy(r);

	st.activity = "  ";
	st.status = "    STATUSMARK";
	st.header = "     HEADMARK";
	out = page_render(&st, 0, &r);
	status = row_of(out, "STATUSMARK");
	FYAI_TCHECK(rows_of(out) == natural);
	FYAI_TCHECK(status == row_of(out, "HEADMARK") + 5);
	/* the header starts after its margin, not after its own blanks */
	FYAI_TCHECK(!strncmp(out, "  HEADMARK", 10));
	fymd_free(out);
	fymd_renderer_destroy(r);
	return 0;
}

/* The cap row is fyai's UI Markdown, kept as it is, and goes first. */
static int page_cap_stands_over_the_pane_run(void)
{
	struct response_buffer out = {0};
	struct fyai_page_state st = page_state();
	const char *p;
	int rc;

	st.pane_rows = 3;
	st.cap = "<fy-role name=\"chrome\">work</fy-role> 2 tiles";
	rc = fyai_page_source(&st, &out);
	FYAI_TCHECK(!rc);
	p = after(out.data, "<fy-drop order=\"0\">");
	p = after(p, "<fy-role name=\"chrome\">work</fy-role> 2 tiles");
	p = after(p, "<fy-slot id=\"pane\" height=\"3\"/>");
	FYAI_TCHECK(p != NULL);
	free(out.data);

	/* no pane, no cap */
	out = (struct response_buffer){0};
	st.pane_rows = 0;
	rc = fyai_page_source(&st, &out);
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(!strstr(out.data, "2 tiles"));
	free(out.data);
	return 0;
}

/* A prompt on a card is one slot two rows taller, with no rules. */
static int page_prompt_card_takes_the_rules_run(void)
{
	struct response_buffer out = {0};
	struct fyai_page_state st = page_state();
	struct fymd_renderer *r;
	const struct fymd_region *prompt;
	char *rendered;
	int rc;

	st.prompt_rows = 2;
	st.prompt_card = true;
	rc = fyai_page_source(&st, &out);
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(strstr(out.data, "<fy-slot id=\"prompt\" height=\"4\"/>"));
	FYAI_TCHECK(!strstr(out.data, "fy-fill char"));
	free(out.data);

	rendered = page_render(&st, 0, &r);
	prompt = region(r, "prompt");
	FYAI_TCHECK(prompt != NULL &&
		    (int)prompt->row == row_of(rendered, "HEADMARK") + 1);
	FYAI_TCHECK(row_of(rendered, "STATUSMARK") ==
		    (int)prompt->row + 4 + 1);
	fymd_free(rendered);
	fymd_renderer_destroy(r);
	return 0;
}

/* The header takes the document margin, the status the gutter with the
 * activity mark in it, and both carry the SGR pairs of the theme. */
static int page_chrome_keeps_the_margins_run(void)
{
	struct response_buffer out = {0};
	struct fyai_page_state st = page_state();
	int rc;

	st.gutter_cols = 3;
	st.activity = "\x1b[33m*\x1b[0m";
	st.elapsed = " 4s";
	st.header_on = "\x1b[1m";
	st.header_off = "\x1b[22m";
	st.status_on = "\x1b[2m";
	st.status_off = "\x1b[22m";
	rc = fyai_page_source(&st, &out);
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(strstr(out.data, "&#32;&#32;\x1b[1mHEADMARK 4s\x1b[22m"));
	FYAI_TCHECK(strstr(out.data, "*&#32;&#32;\x1b[2mSTATUSMARK\x1b[22m"));
	free(out.data);

	/* no activity: a blank gutter of the same width */
	out = (struct response_buffer){0};
	st.activity = NULL;
	rc = fyai_page_source(&st, &out);
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(strstr(out.data, "&#32;&#32;&#32;\x1b[2mSTATUSMARK"));
	free(out.data);
	return 0;
}

/* A pane that asks for the whole terminal leaves the chrome its rows. */
static int page_fit_gives_the_chrome_its_rows_run(void)
{
	struct fyai_page_state st = page_state();
	struct fymd_renderer *r;
	char *rendered;
	int chrome;

	st.prompt_card = true;
	st.hint = "HINTMARK";
	st.cap = "CAPMARK";
	st.pane_rows = 32;
	st.tail_rows = 4;
	chrome = fyai_page_chrome_rows(&st);
	FYAI_TCHECK(chrome == 1 + 3 + 2 + 1);
	fyai_page_fit(&st, 30);
	FYAI_TCHECK(st.pane_rows == 30 - chrome);
	FYAI_TCHECK(st.tail_rows == 0);
	rendered = page_render(&st, 30, &r);
	FYAI_TCHECK(rows_of(rendered) <= 30);
	FYAI_TCHECK(strstr(rendered, "HINTMARK") && strstr(rendered, "STATUSMARK") &&
		    strstr(rendered, "HEADMARK") && strstr(rendered, "CAPMARK"));
	FYAI_TCHECK(region(r, "prompt") != NULL);
	fymd_free(rendered);
	fymd_renderer_destroy(r);

	/* a small pane leaves the tail the rest */
	st = page_state();
	st.pane_rows = 3;
	st.tail_rows = 40;
	fyai_page_fit(&st, 20);
	FYAI_TCHECK(st.pane_rows == 3);
	FYAI_TCHECK(st.tail_rows == 20 - fyai_page_chrome_rows(&st) - 3);
	return 0;
}

/* The ladder of the work pane selects what a tile page draws. */
static int page_view_follows_the_presentation_run(void)
{
	FYAI_TCHECK(fyai_page_view_for(FYAI_WORKPANE_PRESENT_FULL) ==
		    FYTIM_PAGE_VIEW_FULL);
	FYAI_TCHECK(fyai_page_view_for(FYAI_WORKPANE_PRESENT_OUTPUT) ==
		    FYTIM_PAGE_VIEW_SCREEN);
	FYAI_TCHECK(fyai_page_view_for(FYAI_WORKPANE_PRESENT_HEAD) ==
		    FYTIM_PAGE_VIEW_HEAD);
	FYAI_TCHECK(fyai_page_view_for(FYAI_WORKPANE_PRESENT_HIDDEN) ==
		    FYTIM_PAGE_VIEW_HEAD);
	return 0;
}

/* Two tiles side by side: each in its cell, its slot as tall as its row. */
static int page_grid_places_the_tiles_run(void)
{
	static const struct fyai_page_cell cells[2] = {
		{ .slot = 1, .row = 0, .col = 0, .row_span = 1, .col_span = 1,
		  .rows = 5 },
		{ .slot = 2, .row = 0, .col = 1, .row_span = 1, .col_span = 1,
		  .rows = 3 },
	};
	struct fyai_workpane_grid g;
	struct response_buffer src = {0};
	struct fyai_page_state st = page_state();
	const struct fymd_region *a, *b;
	struct fymd_renderer *r;
	char *out;
	int rows = -1, rc;

	memset(&g, 0, sizeof(g));
	g.rows = 1;
	g.cols = 2;
	rc = fyai_page_grid(NULL, &g, cells, 2, 0, " | ", 3, &src, &rows);
	FYAI_TCHECK(!rc && rows == 5);
	FYAI_TCHECK(strstr(src.data, "<fy-grid rows=\"5\" cols=\"*,*\" gap=\"3\" sep=\" | \">"));
	FYAI_TCHECK(strstr(src.data, "<fy-slot id=\"tile:1\" height=\"5\"/>"));
	FYAI_TCHECK(strstr(src.data, "<fy-slot id=\"tile:2\" height=\"5\"/>"));

	st.pane_rows = rows;
	st.pane_source = src.data;
	out = page_render(&st, 0, &r);
	a = region(r, "tile:1");
	b = region(r, "tile:2");
	FYAI_TCHECK(a != NULL && b != NULL);
	FYAI_TCHECK(a->row == 0 && b->row == 0 && a->height == 5);
	FYAI_TCHECK(a->col == 0 && b->col == a->col + a->width + 3);
	FYAI_TCHECK(region(r, "pane") == NULL);
	FYAI_TCHECK(row_of(out, "HEADMARK") == 5);
	fymd_free(out);
	fymd_renderer_destroy(r);
	free(src.data);
	return 0;
}

/* Rows that share take what the height leaves in proportion to their tiles. */
static int page_grid_shares_the_height_run(void)
{
	static const struct fyai_page_cell cells[2] = {
		{ .slot = 7, .row = 0, .col = 0, .row_span = 1, .col_span = 1,
		  .rows = 4 },
		{ .slot = 8, .row = 1, .col = 0, .row_span = 1, .col_span = 1,
		  .rows = 2 },
	};
	struct fyai_workpane_grid g;
	struct response_buffer src = {0};
	int rows = -1, rc;

	memset(&g, 0, sizeof(g));
	g.rows = 2;
	g.cols = 1;
	rc = fyai_page_grid(NULL, &g, cells, 2, 12, "", 0, &src, &rows);
	FYAI_TCHECK(!rc && rows == 12);
	FYAI_TCHECK(strstr(src.data, "rows=\"8,4\" cols=\"*\" gap=\"0\">"));
	FYAI_TCHECK(!strstr(src.data, "sep="));
	FYAI_TCHECK(strstr(src.data, "id=\"tile:7\" height=\"8\""));
	FYAI_TCHECK(strstr(src.data, "id=\"tile:8\" height=\"4\""));
	free(src.data);

	/* no height keeps what the tiles ask for */
	src = (struct response_buffer){0};
	rc = fyai_page_grid(NULL, &g, cells, 2, 0, "", 0, &src, &rows);
	FYAI_TCHECK(!rc && rows == 6 && strstr(src.data, "rows=\"4,2\""));
	free(src.data);
	return 0;
}

/* A fitted row keeps its tile; a spanning tile asks each row for a share. */
static int page_grid_fits_and_spans_run(void)
{
	static const struct fyai_page_cell cells[3] = {
		{ .slot = 1, .row = 0, .col = 0, .row_span = 2, .col_span = 1,
		  .rows = 6 },
		{ .slot = 2, .row = 0, .col = 1, .row_span = 1, .col_span = 1,
		  .rows = 2 },
		{ .slot = 3, .row = 1, .col = 1, .row_span = 1, .col_span = 1,
		  .rows = 1 },
	};
	struct fyai_workpane_grid g;
	struct response_buffer src = {0};
	int rows = -1, rc;

	memset(&g, 0, sizeof(g));
	g.rows = 2;
	g.cols = 2;
	g.row_size[1] = FYAI_WORKPANE_TRACK_FIT;
	g.col_size[0] = 20;
	rc = fyai_page_grid(NULL, &g, cells, 3, 10, "", 0, &src, &rows);
	FYAI_TCHECK(!rc && rows == 10);
	/* the fitted row keeps 3, the shared row takes the other 7 */
	FYAI_TCHECK(strstr(src.data, "rows=\"7,3\" cols=\"20,*\""));
	FYAI_TCHECK(strstr(src.data, "rowspan=\"2\" colspan=\"1\">\n\n"
			   "<fy-slot id=\"tile:1\" height=\"10\"/>"));
	FYAI_TCHECK(strstr(src.data, "id=\"tile:3\" height=\"3\""));

	/* a cell outside the grid is left out */
	free(src.data);
	src = (struct response_buffer){0};
	g.rows = 1;
	rc = fyai_page_grid(NULL, &g, cells, 3, 0, "", 0, &src, &rows);
	FYAI_TCHECK(!rc && !strstr(src.data, "tile:1") &&
		    !strstr(src.data, "tile:3") && strstr(src.data, "tile:2"));
	free(src.data);
	return 0;
}

int page_source_orders_the_chrome(void)
{
	return page_source_orders_the_chrome_run();
}

int page_grid_places_the_tiles(void)
{
	return page_grid_places_the_tiles_run();
}

int page_grid_shares_the_height(void)
{
	return page_grid_shares_the_height_run();
}

int page_grid_fits_and_spans(void)
{
	return page_grid_fits_and_spans_run();
}

int page_view_follows_the_presentation(void)
{
	return page_view_follows_the_presentation_run();
}

int page_prompt_card_takes_the_rules(void)
{
	return page_prompt_card_takes_the_rules_run();
}

int page_chrome_keeps_the_margins(void)
{
	return page_chrome_keeps_the_margins_run();
}

int page_fit_gives_the_chrome_its_rows(void)
{
	return page_fit_gives_the_chrome_its_rows_run();
}

int page_cap_stands_over_the_pane(void)
{
	return page_cap_stands_over_the_pane_run();
}

int page_source_escapes_text(void)
{
	return page_source_escapes_text_run();
}

int page_source_omits_empty_slots(void)
{
	return page_source_omits_empty_slots_run();
}

int page_rows_are_adjacent(void)
{
	return page_rows_are_adjacent_run();
}

int page_status_drops_first(void)
{
	return page_status_drops_first_run();
}

int page_blank_activity_is_not_code(void)
{
	return page_blank_activity_is_not_code_run();
}

/*
 * A head slot stands over the screen slot of its tile, as tall as the tallest
 * head of its row; a tile shown without its head starts at the top, and a
 * short cell keeps one screen row.
 */
static int page_grid_stands_heads_level_run(void)
{
	static const struct fyai_page_cell cells[3] = {
		{ .slot = 1, .row = 0, .col = 0, .row_span = 1, .col_span = 1,
		  .rows = 6, .head_rows = 2 },
		{ .slot = 2, .row = 0, .col = 1, .row_span = 1, .col_span = 1,
		  .rows = 4, .head_rows = 1,
		  .present = FYAI_WORKPANE_PRESENT_OUTPUT },
		{ .slot = 3, .row = 1, .col = 0, .row_span = 1, .col_span = 2,
		  .rows = 3, .head_rows = 3 },
	};
	struct fyai_workpane_grid g;
	struct response_buffer src = {0};
	struct fyai_page_state st = page_state();
	const struct fymd_region *h1, *t1, *t2, *h3, *t3;
	struct fymd_renderer *r;
	char *out;
	int rows = -1, rc;

	memset(&g, 0, sizeof(g));
	g.rows = 2;
	g.cols = 2;
	rc = fyai_page_grid(NULL, &g, cells, 3, 0, "", 0, &src, &rows);
	FYAI_TCHECK(!rc && rows == 9);
	FYAI_TCHECK(strstr(src.data, "<fy-slot id=\"head:1\" height=\"2\"/>\n\n"
			   "<fy-slot id=\"tile:1\" height=\"4\"/>"));
	FYAI_TCHECK(!strstr(src.data, "head:2"));
	FYAI_TCHECK(strstr(src.data, "<fy-slot id=\"tile:2\" height=\"4\"/>"));
	FYAI_TCHECK(strstr(src.data, "<fy-slot id=\"head:3\" height=\"2\"/>\n\n"
			   "<fy-slot id=\"tile:3\" height=\"1\"/>"));

	st.pane_rows = rows;
	st.pane_source = src.data;
	out = page_render(&st, 0, &r);
	h1 = region(r, "head:1");
	t1 = region(r, "tile:1");
	t2 = region(r, "tile:2");
	h3 = region(r, "head:3");
	t3 = region(r, "tile:3");
	FYAI_TCHECK(h1 && t1 && t2 && h3 && t3);
	FYAI_TCHECK(h1->row == 0 && h1->height == 2 && t1->row == 2 &&
		    t1->height == 4 && t1->col == h1->col);
	FYAI_TCHECK(t2->row == 0 && t2->height == 4);
	FYAI_TCHECK(h3->row == 6 && t3->row == 8 && t3->height == 1);
	fymd_free(out);
	fymd_renderer_destroy(r);
	free(src.data);
	return 0;
}

int page_grid_stands_heads_level(void)
{
	return page_grid_stands_heads_level_run();
}

/* The page draws a screen and a tile of text itself: their slots bind no
 * component of the terminal library. */
static int page_grid_names_what_the_page_draws_run(void)
{
	static const struct fyai_page_cell cells[3] = {
		{ .slot = 1, .row = 0, .col = 0, .row_span = 1, .col_span = 1,
		  .rows = 3, .head_rows = 1, .screen = true },
		{ .slot = 2, .row = 0, .col = 1, .row_span = 1, .col_span = 1,
		  .rows = 2, .band = true },
		{ .slot = 3, .row = 1, .col = 0, .row_span = 1, .col_span = 2,
		  .rows = 1 },
	};
	struct fyai_workpane_grid g;
	struct response_buffer src = {0};
	int rows = -1, rc;

	memset(&g, 0, sizeof(g));
	g.rows = 2;
	g.cols = 2;
	rc = fyai_page_grid(NULL, &g, cells, 3, 0, "", 0, &src, &rows);
	FYAI_TCHECK(!rc && rows == 4);
	FYAI_TCHECK(strstr(src.data, "<fy-slot id=\"head:1\" height=\"1\"/>"));
	FYAI_TCHECK(strstr(src.data, "<fy-slot id=\"screen:1\" height=\"2\"/>"));
	/* a tile of text has no head of the page's, beside a head or not: its
	 * chrome is its own, and it takes the whole cell */
	FYAI_TCHECK(strstr(src.data, "<fy-slot id=\"text:2\" height=\"3\"/>"));
	FYAI_TCHECK(!strstr(src.data, "head:2") && !strstr(src.data, "tile:2"));
	FYAI_TCHECK(strstr(src.data, "<fy-slot id=\"tile:3\" height=\"1\"/>"));
	free(src.data);
	return 0;
}

int page_grid_names_what_the_page_draws(void)
{
	return page_grid_names_what_the_page_draws_run();
}
