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

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libfymd4c.h>

#include "fyai.h"
#include "fyai_config.h"
#include "fyai_schema.h"
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
FYAI_TEST_ENTRY(page, source_matches_golden, page_source_matches_golden)
FYAI_TEST_ENTRY(page, transcribe_switches_modes, page_transcribe_switches_modes)
FYAI_TEST_ENTRY(page, transcribe_repeats_items, page_transcribe_repeats_items)
FYAI_TEST_ENTRY(page, transcribe_writes_acts, page_transcribe_writes_acts)
FYAI_TEST_ENTRY(page, transcribe_checks_keys, page_transcribe_checks_keys)
FYAI_TEST_ENTRY(page, state_matches_schema, page_state_matches_schema)
FYAI_TEST_ENTRY(page, keys_take_arguments, page_keys_take_arguments)
FYAI_TEST_ENTRY(page, chrome_counts_a_question, page_chrome_counts_a_question)
FYAI_TEST_ENTRY(page, check_walks_every_case, page_check_walks_every_case)
FYAI_TEST_ENTRY(page, load_takes_the_embedded_document, page_load_takes_the_embedded_document)
FYAI_TEST_ENTRY(page, load_says_why, page_load_says_why)
FYAI_TEST_ENTRY(page, fullscreen_takes_the_transcript, page_fullscreen_takes_the_transcript)

/* The recorded page sources of the golden matrix. */
#include "embedded_page_golden.inc"
/* The schema of the state of the page document. */
#include "embedded_page_state_schema.inc"
/* The embedded page document, which a file of display/page replaces. */
#include "embedded_page.inc"

static struct fyai_page_state page_state(void)
{
	struct fyai_page_state st;

	memset(&st, 0, sizeof(st));
	st.header = "HEADMARK";
	st.status = "STATUSMARK";
	st.prompt_rows = 1;
	return st;
}

/* One value of @n for field @k of golden case @i: a mix of both, so that the
 * fields of a case do not follow one another. */
static unsigned page_golden_pick(unsigned i, unsigned k, unsigned n)
{
	uint64_t z = (((uint64_t)i << 8) | k) + 0x9e3779b97f4a7c15ull;

	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
	z ^= z >> 31;
	return (unsigned)(z % n);
}

/* The state of golden case @i. Each field takes its own value, so every
 * value meets many others over the cases. */
static void page_golden_state(unsigned i, struct fyai_page_state *st)
{
	static const char *const hints[] = { NULL, "HINT <b>x</b>", "  " };
	static const char *const statuses[] = { NULL, "  STATUS\nline" };
	static const char *const headers[] = { NULL, "HEAD <fy-act id=\"x\">y</fy-act>" };
	static const char *const elapsed[] = { NULL, " 4s" };
	static const char *const acts[] = { NULL, "\x1b[33m*\x1b[0m", "**" };
	static const int gutters[] = { 0, 1, 3 };
	static const char *const caps[] = { NULL, "CAP <fy-fill/>" };
	static const char *const sources[] = {
		NULL, "<fy-grid rows=\"3\" cols=\"*\">\n</fy-grid>\n\n",
	};

	memset(st, 0, sizeof(*st));
	st->prompt_rows = (int)page_golden_pick(i, 0, 3);
	st->prompt_card = page_golden_pick(i, 1, 2);
	st->completion = page_golden_pick(i, 2, 2);
	st->hint = hints[page_golden_pick(i, 3, 3)];
	st->status = statuses[page_golden_pick(i, 4, 2)];
	st->header = headers[page_golden_pick(i, 5, 2)];
	st->elapsed = elapsed[page_golden_pick(i, 6, 2)];
	st->activity = acts[page_golden_pick(i, 7, 3)];
	st->gutter_cols = gutters[page_golden_pick(i, 8, 3)];
	if (page_golden_pick(i, 9, 2)) {
		st->header_on = "\x1b[1m";
		st->header_off = "\x1b[22m";
		st->status_on = "\x1b[2m";
		st->status_off = "\x1b[22m";
	}
	st->tail_rows = page_golden_pick(i, 10, 2) ? 2 : 0;
	st->pane_rows = page_golden_pick(i, 11, 2) ? 3 : 0;
	st->pane_below = page_golden_pick(i, 12, 2);
	st->cap = caps[page_golden_pick(i, 13, 2)];
	st->pane_source = sources[page_golden_pick(i, 14, 2)];
}

#define PAGE_GOLDEN_CASES 600

/* The sources of every golden case, one after another. */
static int page_golden_sources(struct response_buffer *all)
{
	struct fyai_page_state st;
	char head[48];
	unsigned i;

	for (i = 0; i < PAGE_GOLDEN_CASES; i++) {
		page_golden_state(i, &st);
		snprintf(head, sizeof(head), "=== case %u\n", i);
		if (response_buffer_append(all, head) ||
		    fyai_page_source(&st, all) ||
		    response_buffer_append(all, "\n"))
			return -1;
	}
	/* The file ends on a line of text, not on the blank line of a source. */
	return response_buffer_append(all, "=== end\n");
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
	p = after(p, "<fy-slot id=\"header\" height=\"1\"/>");
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
	st.status = "\x1b[1mbold\x1b[0m <fy-act id=\"worse\">x</fy-act> line\nbreak";
	st.activity = "\x1b[33m*\x1b[0m";
	rc = fyai_page_source(&st, &out);
	FYAI_TCHECK(!rc);
	/* The status opens no tag of the page, and the header, which the canvas
	 * draws, is not in it. */
	FYAI_TCHECK(!strstr(out.data, "<fy-act id=\"evil\""));
	FYAI_TCHECK(!strstr(out.data, "<fy-act id=\"worse\""));
	FYAI_TCHECK(!strstr(out.data, "click"));
	/* They keep the colours fyai gave them and stay one row. */
	FYAI_TCHECK(strstr(out.data, "\x1b[1mbold\x1b[0m") != NULL);
	FYAI_TCHECK(strstr(out.data, "</fy-act> line break") != NULL);
	/* The activity is text, which loses its SGR. */
	FYAI_TCHECK(!strstr(out.data, "\x1b[33m"));
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
	cfg.width = 40 + markdown_gutter_cols(st->ctx ? st->ctx->cfg : NULL);
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
	const struct fymd_region *tail, *prompt, *header;
	struct fymd_renderer *r;
	char *out;
	int head;

	st.tail_rows = 2;
	out = page_render(&st, 0, &r);
	tail = region(r, "tail");
	prompt = region(r, "prompt");
	header = region(r, "header");
	FYAI_TCHECK(header != NULL && header->height == 1);
	head = header ? (int)header->row : -1;
	FYAI_TCHECK(tail != NULL && tail->row == 0 && tail->height == 2);
	/* a blank row stands between the tail and the header */
	FYAI_TCHECK(head == 3);
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
	FYAI_TCHECK(region(r, "header") != NULL);
	FYAI_TCHECK(region(r, "prompt") != NULL);
	fymd_free(out);
	fymd_renderer_destroy(r);

	out = page_render(&st, 1, &r);
	FYAI_TCHECK(!strstr(out, "STATUSMARK"));
	FYAI_TCHECK(region(r, "header") == NULL);
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
	out = page_render(&st, 0, &r);
	status = row_of(out, "STATUSMARK");
	FYAI_TCHECK(rows_of(out) == natural);
	FYAI_TCHECK(region(r, "header") != NULL &&
		    status == (int)region(r, "header")->row + 5);
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
	FYAI_TCHECK(prompt != NULL && region(r, "header") != NULL &&
		    prompt->row == region(r, "header")->row + 1);
	FYAI_TCHECK(row_of(rendered, "STATUSMARK") ==
		    (int)prompt->row + 4 + 1);
	fymd_free(rendered);
	fymd_renderer_destroy(r);
	return 0;
}

/* The status takes the gutter with the activity mark in it and carries the
 * SGR pair of the theme. The canvas draws the header. */
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
	FYAI_TCHECK(chrome == 2 + 3 + 2 + 1 + 1);
	fyai_page_fit(&st, 30);
	FYAI_TCHECK(st.pane_rows == 30 - chrome);
	FYAI_TCHECK(st.tail_rows == 0);
	rendered = page_render(&st, 30, &r);
	FYAI_TCHECK(rows_of(rendered) <= 30);
	FYAI_TCHECK(strstr(rendered, "HINTMARK") && strstr(rendered, "STATUSMARK") &&
		    region(r, "header") && strstr(rendered, "CAPMARK"));
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
	FYAI_TCHECK(a->row == 1 && b->row == 1 && a->height == 5);
	FYAI_TCHECK(a->col == 0 && b->col == a->col + a->width + 3);
	FYAI_TCHECK(region(r, "pane") == NULL);
	FYAI_TCHECK(region(r, "header") != NULL &&
		    region(r, "header")->row == 7);
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
	FYAI_TCHECK(h1->row == 1 && h1->height == 2 && t1->row == 3 &&
		    t1->height == 4 && t1->col == h1->col);
	FYAI_TCHECK(t2->row == 1 && t2->height == 4);
	FYAI_TCHECK(h3->row == 7 && t3->row == 9 && t3->height == 1);
	fymd_free(out);
	fymd_renderer_destroy(r);
	free(src.data);
	return 0;
}

int page_grid_stands_heads_level(void)
{
	return page_grid_stands_heads_level_run();
}

/* A generic of the @len bytes of @yaml, in @gb. */
static fy_generic page_yaml_n(struct fy_generic_builder *gb, const char *yaml,
			      size_t len)
{
	fy_generic_sized_string s;

	s.data = yaml;
	s.size = len;
	return fy_parse(gb, s, FYAI_YAML_PARSE_FLAGS | FYOPPF_INPUT_TYPE_STRING,
			NULL);
}

static fy_generic page_yaml(struct fy_generic_builder *gb, const char *yaml)
{
	return page_yaml_n(gb, yaml, strlen(yaml));
}

static struct fy_generic_builder *page_test_builder(void)
{
	struct fy_generic_builder_cfg cfg = {
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED,
	};

	return fy_generic_builder_create(&cfg);
}

static void page_test_noop(struct fyai_ctx *ctx, const char *arg)
{
	(void)ctx;
	(void)arg;
}

/* The actions a test document may name. */
static const struct fyai_page_action page_test_actions[] = {
	{ "pick.next", page_test_noop },
	{ "pick.accept", page_test_noop },
	{ "pick.choose", page_test_noop },
};

#define PAGE_TEST_NACTIONS \
	(sizeof(page_test_actions) / sizeof(page_test_actions[0]))

/* The Markdown of @doc with @state, or NULL when it does not transcribe. */
static char *page_test_transcribe(struct fy_generic_builder *gb,
				  const char *doc, const char *state,
				  struct fyai_page_keys *keys)
{
	struct response_buffer out = {0};

	if (fyai_page_transcribe(NULL, page_yaml(gb, doc), page_yaml(gb, state),
				 page_test_actions, PAGE_TEST_NACTIONS, &out,
				 keys)) {
		free(out.data);
		return NULL;
	}
	if (!out.data)
		return strdup("");
	return out.data;
}

/* A switch writes the case its mode names, and binds the keys of that case
 * only. A mode that names no case writes nothing. */
static int page_transcribe_switches_modes_run(void)
{
	static const char doc[] =
		"page:\n"
		"  - switch: mode\n"
		"    cases:\n"
		"      a:\n"
		"        keys: { Up: pick.next }\n"
		"        body:\n"
		"          - row: [ { text: A } ]\n"
		"      b:\n"
		"        keys: { Enter: pick.accept, Down: pick.next }\n"
		"        body:\n"
		"          - row: [ { text: B } ]\n";
	struct fy_generic_builder *gb = page_test_builder();
	struct fyai_page_keys keys;
	char *out;

	FYAI_TCHECK(gb != NULL);
	out = page_test_transcribe(gb, doc, "mode: b\n", &keys);
	FYAI_TCHECK(out != NULL && !strcmp(out, "B\n\n"));
	FYAI_TCHECK(keys.count == 2);
	FYAI_TCHECK(!strcmp(keys.key[0].name, "Enter") &&
		    !strcmp(keys.key[0].action, "pick.accept"));
	FYAI_TCHECK(!strcmp(keys.key[1].name, "Down") &&
		    !strcmp(keys.key[1].action, "pick.next"));
	free(out);

	out = page_test_transcribe(gb, doc, "mode: zzz\n", &keys);
	FYAI_TCHECK(out != NULL && !*out && keys.count == 0);
	free(out);
	fy_generic_builder_destroy(gb);
	return 0;
}

/* An each writes its body for each item: a name is the item's first, then the
 * state's, and {index} and {number} are its position. */
static int page_transcribe_repeats_items_run(void)
{
	static const char doc[] =
		"page:\n"
		"  - each: items\n"
		"    body:\n"
		"      - row: [ { text: \"{number}. {name} of {title} ({index})\" } ]\n";
	struct fy_generic_builder *gb = page_test_builder();
	char *out;

	FYAI_TCHECK(gb != NULL);
	out = page_test_transcribe(gb, doc,
		"title: T\n"
		"items:\n"
		"  - { name: a }\n"
		"  - { name: b, title: U }\n", NULL);
	FYAI_TCHECK(out != NULL && !strcmp(out, "1. a of T (0)\n\n2. b of U (1)\n\n"));
	free(out);

	/* an empty list, or none, writes nothing */
	out = page_test_transcribe(gb, doc, "title: T\nitems: []\n", NULL);
	FYAI_TCHECK(out != NULL && !*out);
	free(out);
	out = page_test_transcribe(gb, doc, "title: T\n", NULL);
	FYAI_TCHECK(out != NULL && !*out);
	free(out);
	fy_generic_builder_destroy(gb);
	return 0;
}

/* An act names an action of the page with its argument, and its text is
 * escaped; an unknown action or an argument that is no id does not
 * transcribe. */
static int page_transcribe_writes_acts_run(void)
{
	static const char doc[] =
		"page:\n"
		"  - each: options\n"
		"    body:\n"
		"      - row: [ { act: { action: pick.choose, arg: \"{index}\", text: \"{text}\" } } ]\n";
	static const char state[] =
		"options:\n"
		"  - { text: one }\n"
		"  - { text: \"<fy-slot id=\\\"x\\\"/>\" }\n";
	struct fy_generic_builder *gb = page_test_builder();
	char *out;

	FYAI_TCHECK(gb != NULL);
	out = page_test_transcribe(gb, doc, state, NULL);
	FYAI_TCHECK(out != NULL);
	FYAI_TCHECK(strstr(out, "<fy-act id=\"pick.choose:0\">one</fy-act>\n\n") != NULL);
	FYAI_TCHECK(strstr(out, "<fy-act id=\"pick.choose:1\">&lt;fy-slot") != NULL);
	FYAI_TCHECK(!strstr(out, "<fy-slot"));
	free(out);

	out = page_test_transcribe(gb,
		"page:\n  - row: [ { act: { action: nope, text: x } } ]\n",
		"a: 1\n", NULL);
	FYAI_TCHECK(out == NULL);
	out = page_test_transcribe(gb,
		"page:\n  - row: [ { act: { action: pick.next, arg: \"a b\", text: x } } ]\n",
		"a: 1\n", NULL);
	FYAI_TCHECK(out == NULL);
	fy_generic_builder_destroy(gb);
	return 0;
}

/* A key that fyai keeps, or that names no action, does not transcribe. A case
 * that is not transcribed binds nothing, and is not checked. */
static int page_transcribe_checks_keys_run(void)
{
	static const char *const bad[] = {
		"page:\n  - switch: mode\n    cases:\n      a:\n"
		"        keys: { \"Ctrl-]\": pick.next }\n        body: []\n",
		"page:\n  - switch: mode\n    cases:\n      a:\n"
		"        keys: { Ctrl-T: pick.next }\n        body: []\n",
		"page:\n  - switch: mode\n    cases:\n      a:\n"
		"        keys: { Up: nope }\n        body: []\n",
	};
	struct fy_generic_builder *gb = page_test_builder();
	struct fyai_page_keys keys;
	char *out;
	size_t i;

	FYAI_TCHECK(gb != NULL);
	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
		FYAI_TCHECK(page_test_transcribe(gb, bad[i], "mode: a\n",
						 &keys) == NULL);
	out = page_test_transcribe(gb, bad[0], "mode: b\n", &keys);
	FYAI_TCHECK(out != NULL && keys.count == 0);
	free(out);
	fy_generic_builder_destroy(gb);
	return 0;
}

/* The state fyai builds is the one its schema describes, for every state of
 * the golden matrix; a state of another shape is refused. */
static int page_state_matches_schema_run(void)
{
	struct fy_generic_builder *gb = page_test_builder();
	struct fyai_page_state st;
	fy_generic schema, report;
	char *problems;
	unsigned i;

	FYAI_TCHECK(gb != NULL);
	schema = page_yaml_n(gb, (const char *)FYAI_EMBEDDED_PAGE_STATE_SCHEMA,
			     FYAI_EMBEDDED_PAGE_STATE_SCHEMA_LEN);
	FYAI_TCHECK(fy_is_mapping(schema));
	for (i = 0; i < PAGE_GOLDEN_CASES; i++) {
		page_golden_state(i, &st);
		report = fyai_schema_validate(gb, schema,
					      fyai_page_state_generic(gb, &st));
		if (!fyai_schema_valid(report)) {
			problems = fyai_schema_report_string(report);
			fprintf(stderr, "case %u: %s\n", i,
				problems ? problems : "");
			free(problems);
		}
		FYAI_TCHECK(fyai_schema_valid(report));
	}
	report = fyai_schema_validate(gb, schema,
				      page_yaml(gb, "input: { mode: 3 }\n"));
	FYAI_TCHECK(!fyai_schema_valid(report));
	fy_generic_builder_destroy(gb);
	return 0;
}

/* A key can give its action an argument; an argument that is empty or no id,
 * or an action that is not there, does not transcribe. */
static int page_keys_take_arguments_run(void)
{
	static const char good[] =
		"page:\n  - switch: mode\n    cases:\n      a:\n"
		"        keys: { \"1\": \"pick.choose:1\" }\n        body: []\n";
	static const char *const bad[] = {
		"page:\n  - switch: mode\n    cases:\n      a:\n"
		"        keys: { \"1\": \"pick.choose:\" }\n        body: []\n",
		"page:\n  - switch: mode\n    cases:\n      a:\n"
		"        keys: { \"1\": \"pick.choose:a b\" }\n        body: []\n",
		"page:\n  - switch: mode\n    cases:\n      a:\n"
		"        keys: { \"1\": \"nope:1\" }\n        body: []\n",
	};
	struct fy_generic_builder *gb = page_test_builder();
	struct fyai_page_keys keys;
	char *out;
	size_t i;

	FYAI_TCHECK(gb != NULL);
	out = page_test_transcribe(gb, good, "mode: a\n", &keys);
	FYAI_TCHECK(out != NULL && keys.count == 1 &&
		    !strcmp(keys.key[0].name, "1") &&
		    !strcmp(keys.key[0].action, "pick.choose:1"));
	free(out);
	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
		FYAI_TCHECK(page_test_transcribe(gb, bad[i], "mode: a\n",
						 &keys) == NULL);
	fy_generic_builder_destroy(gb);
	return 0;
}

static void page_test_ask_noop(struct fyai_ctx *ctx, const char *arg)
{
	(void)ctx;
	(void)arg;
}

/* The actions the page document names for a question. */
static const struct fyai_page_action page_test_ask_actions[] = {
	{ "ask.prev", page_test_ask_noop },
	{ "ask.next", page_test_ask_noop },
	{ "ask.choose", page_test_ask_noop },
	{ "ask.accept", page_test_ask_noop },
	{ "ask.dismiss", page_test_ask_noop },
	{ "popup.close", page_test_ask_noop },
	{ "popup.scroll", page_test_ask_noop },
};

/* The chrome of the page counts the rows a question takes as the document
 * draws them, so a page with a question fits as one without. */
static int page_chrome_counts_a_question_run(void)
{
	static const char *const options[] = { "yes", "no", "maybe" };
	struct fyai_page_state plain = page_state(), asked;
	struct fymd_renderer *r;
	int plain_rows, asked_rows;
	char *out;

	plain.actions = page_test_ask_actions;
	plain.nactions = sizeof(page_test_ask_actions) /
			 sizeof(page_test_ask_actions[0]);
	asked = plain;
	asked.input_mode = "ask";
	asked.ask_question = "Proceed?";
	asked.ask_from = "main/agent:asker";
	asked.ask_options = options;
	asked.ask_noptions = 3;
	asked.ask_selected = 1;

	out = page_render(&plain, 0, &r);
	plain_rows = rows_of(out);
	fymd_free(out);
	fymd_renderer_destroy(r);
	out = page_render(&asked, 0, &r);
	asked_rows = rows_of(out);
	FYAI_TCHECK(strstr(out, "Proceed?") && strstr(out, "asked by main/agent:asker"));
	FYAI_TCHECK(strstr(out, "2. no") && strstr(out, "or type an answer"));
	FYAI_TCHECK(region(r, "ask.choose:3") != NULL);
	fymd_free(out);
	fymd_renderer_destroy(r);

	FYAI_TCHECK(asked_rows - plain_rows ==
		    fyai_page_chrome_rows(&asked) - fyai_page_chrome_rows(&plain));
	FYAI_TCHECK(fyai_page_chrome_rows(&asked) - fyai_page_chrome_rows(&plain) ==
		    2 + 1 + 3);
	return 0;
}

/* Write the @len bytes of @text to a new file, whose name goes to @path. */
static int page_test_file(const char *text, size_t len, char *path,
			  size_t size)
{
	const char *tmp = getenv("TMPDIR");
	ssize_t n;
	int fd;

	snprintf(path, size, "%s/fyai-page-test-XXXXXX",
		 tmp && *tmp ? tmp : "/tmp");
	fd = mkstemp(path);
	if (fd < 0)
		return -1;
	n = write(fd, text, len);
	close(fd);
	if (n < 0 || (size_t)n != len) {
		unlink(path);
		return -1;
	}
	return 0;
}

/* A frame transcribes what its state shows; a check walks every case, every
 * flagged node and every each, and says what does not transcribe. */
static int page_check_walks_every_case_run(void)
{
	static const char cases[] =
		"page:\n"
		"  - switch: mode\n"
		"    cases:\n"
		"      a:\n"
		"        body:\n"
		"          - row: [ { text: A } ]\n"
		"      b:\n"
		"        keys: { x: nope }\n";
	static const char good[] =
		"page:\n"
		"  - switch: mode\n"
		"    cases:\n"
		"      a:\n"
		"        body:\n"
		"          - row: [ { text: A } ]\n"
		"      b:\n"
		"        keys: { x: pick.next }\n";
	static const char flagged[] =
		"page:\n"
		"  - if: never\n"
		"    row: [ { act: { action: ghost, text: G } } ]\n";
	static const char paged[] =
		"page:\n"
		"  - each: items\n"
		"    body:\n"
		"      - page: missing\n";
	struct fy_generic_builder *gb = page_test_builder();
	char why[256], *out;

	FYAI_TCHECK(gb != NULL);
	out = page_test_transcribe(gb, cases, "mode: a\n", NULL);
	FYAI_TCHECK(out && strstr(out, "A"));
	free(out);
	FYAI_TCHECK(fyai_page_check(page_yaml(gb, cases), page_test_actions,
				    PAGE_TEST_NACTIONS, why, sizeof(why)) == -1);
	FYAI_TCHECK(strstr(why, "nope") != NULL);
	FYAI_TCHECK(fyai_page_check(page_yaml(gb, good), page_test_actions,
				    PAGE_TEST_NACTIONS, why, sizeof(why)) == 0);
	FYAI_TCHECK(why[0] == '\0');

	out = page_test_transcribe(gb, flagged, "{}\n", NULL);
	FYAI_TCHECK(out != NULL);
	free(out);
	FYAI_TCHECK(fyai_page_check(page_yaml(gb, flagged), page_test_actions,
				    PAGE_TEST_NACTIONS, why, sizeof(why)) == -1);
	FYAI_TCHECK(strstr(why, "ghost") != NULL);

	out = page_test_transcribe(gb, paged, "{}\n", NULL);
	FYAI_TCHECK(out != NULL);
	free(out);
	FYAI_TCHECK(fyai_page_check(page_yaml(gb, paged), page_test_actions,
				    PAGE_TEST_NACTIONS, why, sizeof(why)) == -1);
	FYAI_TCHECK(strstr(why, "missing") != NULL);
	fy_generic_builder_destroy(gb);
	return 0;
}

/* The embedded document matches the page schema and names only the actions of
 * the input area, so a file that holds it loads. */
static int page_load_takes_the_embedded_document_run(void)
{
	struct fy_generic_builder *gb = page_test_builder();
	char path[256], why[1024];
	fy_generic doc;

	FYAI_TCHECK(gb != NULL);
	FYAI_TCHECK(!page_test_file((const char *)FYAI_EMBEDDED_PAGE,
				    FYAI_EMBEDDED_PAGE_LEN, path, sizeof(path)));
	doc = fyai_page_load(gb, path, page_test_ask_actions,
			     sizeof(page_test_ask_actions) /
			     sizeof(page_test_ask_actions[0]),
			     why, sizeof(why));
	unlink(path);
	if (!fy_is_mapping(doc))
		fprintf(stderr, "  %s\n", why);
	FYAI_TCHECK(fy_is_mapping(doc));
	FYAI_TCHECK(why[0] == '\0');
	fy_generic_builder_destroy(gb);
	return 0;
}

/* A fullscreen page shows the transcript view over the tail, in the rows the
 * chrome, the pane and the tail leave it, and its state matches the schema. */
static int page_fullscreen_takes_the_transcript_run(void)
{
	struct fyai_page_state full = page_state(), inl = page_state();
	struct response_buffer src = {0};
	struct fy_generic_builder *gb = page_test_builder();
	fy_generic schema, report;
	char *problems;

	FYAI_TCHECK(gb != NULL);
	full.fullscreen = true;
	full.tail_rows = 2;
	full.pane_rows = 3;
	fyai_page_fit(&full, 20);
	FYAI_TCHECK(full.tail_rows == 2);
	FYAI_TCHECK(full.pane_rows == 3);
	FYAI_TCHECK(full.transcript_rows ==
		    20 - fyai_page_chrome_rows(&full) - 3);
	FYAI_TCHECK(!fyai_page_source(&full, &src));
	/* The live tail is drawn inside the transcript region. */
	FYAI_TCHECK(src.data && strstr(src.data, "<fy-slot id=\"transcript\""));
	FYAI_TCHECK(!strstr(src.data, "<fy-slot id=\"tail\""));
	free(src.data);
	memset(&src, 0, sizeof(src));

	inl.tail_rows = 2;
	fyai_page_fit(&inl, 20);
	FYAI_TCHECK(inl.tail_rows == 2 && inl.transcript_rows == 0);
	FYAI_TCHECK(!fyai_page_source(&inl, &src));
	FYAI_TCHECK(src.data && strstr(src.data, "<fy-slot id=\"tail\""));
	FYAI_TCHECK(!strstr(src.data, "id=\"transcript\""));
	free(src.data);

	schema = page_yaml_n(gb, (const char *)FYAI_EMBEDDED_PAGE_STATE_SCHEMA,
			     FYAI_EMBEDDED_PAGE_STATE_SCHEMA_LEN);
	report = fyai_schema_validate(gb, schema,
				      fyai_page_state_generic(gb, &full));
	if (!fyai_schema_valid(report)) {
		problems = fyai_schema_report_string(report);
		fprintf(stderr, "  %s\n", problems ? problems : "");
		free(problems);
	}
	FYAI_TCHECK(fyai_schema_valid(report));
	fy_generic_builder_destroy(gb);
	return 0;
}

/* A file that does not load is rejected with a reason that names it. */
static int page_load_says_why_run(void)
{
	static const struct {
		const char *text;
		const char *why;
	} files[] = {
		{ "- a list\n", "does not hold a YAML mapping" },
		{ "page:\n  - bogus: 1\n", "does not match the page schema" },
		{ "page:\n  - row: [ { act: { action: ghost, text: G } } ]\n",
		  "unknown action 'ghost'" },
	};
	struct fy_generic_builder *gb = page_test_builder();
	char path[256], why[1024];
	fy_generic doc;
	size_t i;

	FYAI_TCHECK(gb != NULL);
	for (i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
		FYAI_TCHECK(!page_test_file(files[i].text, strlen(files[i].text),
					    path, sizeof(path)));
		doc = fyai_page_load(gb, path, page_test_actions,
				     PAGE_TEST_NACTIONS, why, sizeof(why));
		unlink(path);
		FYAI_TCHECK(fy_is_invalid(doc));
		FYAI_TCHECK(strstr(why, files[i].why) != NULL);
		FYAI_TCHECK(strstr(why, path) != NULL);
	}
	doc = fyai_page_load(gb, "/nonexistent/fyai-page.yaml",
			     page_test_actions, PAGE_TEST_NACTIONS, why,
			     sizeof(why));
	FYAI_TCHECK(fy_is_invalid(doc));
	FYAI_TCHECK(strstr(why, "cannot read /nonexistent/fyai-page.yaml") !=
		    NULL);
	fy_generic_builder_destroy(gb);
	return 0;
}

/* The page source of a matrix of states is the one recorded. Set
 * FYAI_PAGE_GOLDEN_WRITE to a path to record the sources again. */
static int page_source_matches_golden_run(void)
{
	const char *golden = (const char *)FYAI_EMBEDDED_PAGE_GOLDEN;
	const char *path = getenv("FYAI_PAGE_GOLDEN_WRITE");
	struct response_buffer all = {0};
	size_t i, n, head;
	FILE *fp;

	FYAI_TCHECK(!page_golden_sources(&all));
	if (path) {
		fp = fopen(path, "wb");
		FYAI_TCHECK(fp != NULL);
		if (fp) {
			FYAI_TCHECK(fwrite(all.data, 1, all.len, fp) == all.len);
			fclose(fp);
		}
		free(all.data);
		return 0;
	}
	/* Name the first case that differs. */
	n = all.len < FYAI_EMBEDDED_PAGE_GOLDEN_LEN ?
	    all.len : FYAI_EMBEDDED_PAGE_GOLDEN_LEN;
	for (i = 0; i < n && all.data[i] == golden[i]; i++)
		;
	if (i < n || all.len != FYAI_EMBEDDED_PAGE_GOLDEN_LEN) {
		for (head = i; head > 0 &&
		     strncmp(all.data + head, "=== case ", 9); head--)
			;
		fprintf(stderr, "page source differs from the golden at byte %zu, "
			"in \"%.16s\"\n", i, all.data + head);
	}
	FYAI_TCHECK(all.len == FYAI_EMBEDDED_PAGE_GOLDEN_LEN &&
		    !memcmp(all.data, golden, all.len));
	free(all.data);
	return 0;
}

int page_source_matches_golden(void)
{
	return page_source_matches_golden_run();
}

int page_transcribe_switches_modes(void)
{
	return page_transcribe_switches_modes_run();
}

int page_transcribe_repeats_items(void)
{
	return page_transcribe_repeats_items_run();
}

int page_transcribe_writes_acts(void)
{
	return page_transcribe_writes_acts_run();
}

int page_transcribe_checks_keys(void)
{
	return page_transcribe_checks_keys_run();
}

int page_state_matches_schema(void)
{
	return page_state_matches_schema_run();
}

int page_keys_take_arguments(void)
{
	return page_keys_take_arguments_run();
}

int page_chrome_counts_a_question(void)
{
	return page_chrome_counts_a_question_run();
}

int page_check_walks_every_case(void)
{
	return page_check_walks_every_case_run();
}

int page_load_takes_the_embedded_document(void)
{
	return page_load_takes_the_embedded_document_run();
}

int page_load_says_why(void)
{
	return page_load_says_why_run();
}

int page_fullscreen_takes_the_transcript(void)
{
	return page_fullscreen_takes_the_transcript_run();
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
