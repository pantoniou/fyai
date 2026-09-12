/*
 * fyai_markdown_test.c - unit tests for the progressive Markdown streams
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * A live repaint renders only the newest source lines. These cases pin what
 * that has to preserve: the cost stops following the accumulator, a window
 * opening inside a fenced block still renders as code, an unbounded stream
 * keeps every row, and the final render leaves the window behind.
 */

/* Diagnostics raised from this file are the test harness's own. */
#define FYAI_MODULE FYAIEM_UNKNOWN

#include <stdio.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fyai.h"
#include "fyai_diag.h"
#include "fyai_display.h"
#include "fyai_markdown.h"
#include "fyai_test.h"

#include "fyai_test_registry.h"

#ifdef FYAI_WITH_FYPALETTE
#include <libfypalette.h>
#endif

FYAI_TEST_ENTRY(markdown, window_bounds_render, markdown_window_bounds_render)
FYAI_TEST_ENTRY(markdown, window_reopens_fence, markdown_window_reopens_fence)
FYAI_TEST_ENTRY(markdown, window_off_when_unbounded, markdown_window_off_when_unbounded)
FYAI_TEST_ENTRY(markdown, final_render_is_whole, markdown_final_render_is_whole)
FYAI_TEST_ENTRY(markdown, tool_head_chrome, markdown_tool_head_chrome)
FYAI_TEST_ENTRY(markdown, source_rows_utf8, markdown_source_rows_utf8)
FYAI_TEST_ENTRY(markdown, role_palette, markdown_role_palette)
FYAI_TEST_ENTRY(markdown, gutter_palette, markdown_gutter_palette)
FYAI_TEST_ENTRY(markdown, reasoning_palette, markdown_reasoning_palette)
FYAI_TEST_ENTRY(markdown, theme_selectors, markdown_theme_selectors_test)

static struct fyai_cfg test_cfg;
static struct fyai_ctx test_ctx = { .cfg = &test_cfg };

/* Rows the live band shows. */
#define TEST_MAX_LINES 5

int markdown_source_rows_utf8(void)
{
	const char *wide = "\xe6\x97\xa5\xe6\x9c\xac";
	const char *combining = "e\xcc\x81";

	FYAI_TCHECK(fyai_display_source_rows(&test_cfg, "abcd", 4, 8) == 1);
	FYAI_TCHECK(fyai_display_source_rows(&test_cfg, wide, strlen(wide), 8) == 1);
	FYAI_TCHECK(fyai_display_source_rows(&test_cfg, combining,
					strlen(combining), 8) == 1);
	FYAI_TCHECK(fyai_display_source_rows(&test_cfg, "**abcd**", 8, 8) == 1);
	return EXIT_SUCCESS;
}

static double cpu_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/*
 * Push @steps exchanges of prose and a fenced C block, repainting each time,
 * and return what the batch cost in total.
 *
 * Measure the batch total so growth in every repaint remains visible.
 */
static double push_steps(struct fyai_fenced_stream *fs, size_t steps)
{
	char chunk[256];
	double t0;
	size_t i;
	int n;

	t0 = cpu_ms();
	for (i = 0; i < steps; i++) {
		n = snprintf(chunk, sizeof(chunk),
			     "Step %zu: editing a file.\n\n```c\n"
			     "static int helper_%zu(int a)\n{\n"
			     "\treturn a + 1;\n}\n```\n\n", i, i);
		/* Render every push: the throttle is not what is under test. */
		fs->next_render_ms = 0;
		FYAI_TCHECK(!fyai_fenced_stream_push(fs, chunk, (size_t)n));
	}
	return cpu_ms() - t0;
}

/*
 * Use the cheapest batch to exclude one-time allocator and cache costs.
 */
static double best_of(struct fyai_fenced_stream *fs, int rounds)
{
	double best = 0, dt;
	int i;

	for (i = 0; i < rounds; i++) {
		dt = push_steps(fs, 100);
		if (!i || dt < best)
			best = dt;
	}
	return best;
}

/*
 * A late batch of repaints must not cost more than an early one. The window
 * holds this flat. The bound allows allocator and cache variation while still
 * rejecting a render of the whole accumulator.
 */
static void test_window_bounds_render(void)
{
	struct fyai_fenced_stream fs;
	double early, late;
	FILE *fp;

	fp = tmpfile();
	FYAI_TCHECK(fp);
	test_cfg.tool_update_interval_ms = 0;
	FYAI_TCHECK(!fyai_markdown_quote_stream_start(&fs, &test_ctx, &test_cfg,
						      TEST_MAX_LINES, fp, true));
	/* The window has filled well before either measurement. */
	push_steps(&fs, 100);
	early = best_of(&fs, 3);
	push_steps(&fs, 600);
	late = best_of(&fs, 3);

	FYAI_TCHECK(fs.accum.len > 60000);	/* the accumulator did grow */
	FYAI_TCHECK(late < early * 2 + 20.0);

	fyai_fenced_stream_finish(&fs);
	fclose(fp);
	printf("ok - live repaints stay bounded (early %.2fms, late %.2fms "
	       "for a hundred)\n",
	       early, late);
}

/*
 * A window that opens inside a fenced block reopens it, so the tail renders as
 * code. Without that the fence text leaks into the body as prose.
 */
static void test_window_reopens_fence(void)
{
	struct fyai_fenced_stream fs;
	char line[128];
	size_t i;
	FILE *fp;
	int n;

	fp = tmpfile();
	FYAI_TCHECK(fp);
	test_cfg.tool_update_interval_ms = 0;
	FYAI_TCHECK(!fyai_markdown_quote_stream_start(&fs, &test_ctx, &test_cfg,
						      TEST_MAX_LINES, fp, true));
	FYAI_TCHECK(!fyai_fenced_stream_push(&fs, "```c\n", 5));
	/* Overrun the window from inside the block, so its start is in it. */
	for (i = 0; i < 400; i++) {
		n = snprintf(line, sizeof(line),
			     "\tint value_%zu = %zu;\n", i, i);
		fs.next_render_ms = 0;
		FYAI_TCHECK(!fyai_fenced_stream_push(&fs, line, (size_t)n));
	}
	/* Off a terminal the repaint writes through the diffing path, so the
	 * last rendered bytes are in shown rather than in the band body. They
	 * must not carry a literal fence marker: the reopen goes into the
	 * source handed to the renderer, never into its output. */
	FYAI_TCHECK(fs.shown.len > 0);
	FYAI_TCHECK(!memmem(fs.shown.data, fs.shown.len, "```", 3));

	fyai_fenced_stream_finish(&fs);
	fclose(fp);
	printf("ok - a window inside a fence renders as code\n");
}

/* An unbounded stream keeps no window: its caller asked for every row. */
static void test_window_off_when_unbounded(void)
{
	struct fyai_fenced_stream fs;
	FILE *fp;

	fp = tmpfile();
	FYAI_TCHECK(fp);
	test_cfg.tool_update_interval_ms = 0;
	FYAI_TCHECK(!fyai_markdown_quote_stream_start(&fs, &test_ctx, &test_cfg,
						      0, fp, true));
	FYAI_TCHECK(!fs.mark_cap);
	push_steps(&fs, 20);
	FYAI_TCHECK(!fs.mark_cap);

	fyai_fenced_stream_finish(&fs);
	fclose(fp);
	printf("ok - an unbounded stream keeps no window\n");
}

/*
 * The commit payload is rebuilt from the whole accumulator, so the final
 * render must see every line the stream was given, not the tail window.
 */
static void test_final_render_is_whole(void)
{
	struct fyai_fenced_stream fs;
	size_t accumulated;
	FILE *fp;

	fp = tmpfile();
	FYAI_TCHECK(fp);
	test_cfg.tool_update_interval_ms = 0;
	FYAI_TCHECK(!fyai_markdown_quote_stream_start(&fs, &test_ctx, &test_cfg,
						      TEST_MAX_LINES, fp, true));
	push_steps(&fs, 200);
	accumulated = fs.accum.len;
	FYAI_TCHECK(accumulated > 0);
	/* The window is in force while live. */
	FYAI_TCHECK(fs.mark_cap && fs.mark_count == fs.mark_cap);
	FYAI_TCHECK(!fs.full_render);
	FYAI_TCHECK(fs.total_lines > fs.mark_count);

	fyai_fenced_stream_finish(&fs);
	fclose(fp);
	printf("ok - the final render leaves the window behind\n");
}

int markdown_window_bounds_render(void)
{
	int rc;

	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);
	test_window_bounds_render();
	fyai_diag_drain(&test_cfg.diag);
	fyai_diag_cleanup(&test_cfg.diag);
	return 0;
}

int markdown_window_reopens_fence(void)
{
	int rc;

	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);
	test_window_reopens_fence();
	fyai_diag_drain(&test_cfg.diag);
	fyai_diag_cleanup(&test_cfg.diag);
	return 0;
}

int markdown_window_off_when_unbounded(void)
{
	int rc;

	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);
	test_window_off_when_unbounded();
	fyai_diag_drain(&test_cfg.diag);
	fyai_diag_cleanup(&test_cfg.diag);
	return 0;
}

int markdown_final_render_is_whole(void)
{
	int rc;

	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);
	test_final_render_is_whole();
	fyai_diag_drain(&test_cfg.diag);
	fyai_diag_cleanup(&test_cfg.diag);
	return 0;
}

/*
 * The tool title row is one renderer for the live band and for the replayed
 * transcript. Pin what that row must carry: a mark that separates success from
 * failure, and the failure cause beside the label.
 */
static void test_tool_head_chrome(void)
{
	struct response_buffer ok_out = {0};
	struct response_buffer bad_out = {0};
	char *ok_mark, *bad_mark;

	ok_mark = markdown_indicator_margin_cfg(&test_cfg,
						FYMD_INDICATOR_SUCCESS);
	bad_mark = markdown_indicator_margin_cfg(&test_cfg,
						 FYMD_INDICATOR_FAILURE);
	FYAI_TCHECK(ok_mark && bad_mark);
	/* A replayed call must be able to say which of the two states it is. */
	FYAI_TCHECK(strcmp(ok_mark, bad_mark) != 0);

	FYAI_TCHECK(!markdown_render_tool_head(&test_cfg, "**shell**", 9, NULL,
					       ok_mark, "  ", &ok_out));
	FYAI_TCHECK(!markdown_render_tool_head(&test_cfg, "**shell**", 9,
					       "exit 3", bad_mark, "  ",
					       &bad_out));
	FYAI_TCHECK(ok_out.len && bad_out.len);
	FYAI_TCHECK(strstr(ok_out.data, "shell") != NULL);
	FYAI_TCHECK(strstr(bad_out.data, "shell") != NULL);
	/* The cause belongs on the title row, and only on a failure. */
	FYAI_TCHECK(strstr(bad_out.data, "exit 3") != NULL);
	FYAI_TCHECK(strstr(ok_out.data, "exit 3") == NULL);
	FYAI_TCHECK(strstr(bad_out.data, ok_mark) == NULL);

	free(ok_out.data);
	free(bad_out.data);
	free(ok_mark);
	free(bad_mark);
}

int markdown_tool_head_chrome(void)
{
	int rc;

	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);
	test_cfg.color = "off";
	test_tool_head_chrome();
	fyai_diag_drain(&test_cfg.diag);
	fyai_diag_cleanup(&test_cfg.diag);
	return 0;
}

/* An element that fyai colours itself takes the escape of its palette role,
 * and keeps its own escape without a palette or without the role. */
int markdown_role_palette(void)
{
	struct fyai_cfg cfg;
#ifdef FYAI_WITH_FYPALETTE
	const char *on;
	int rc;
#endif

	memset(&cfg, 0, sizeof(cfg));
	FYAI_TCHECK(!strcmp(markdown_role_on(NULL, "tool.fail", "x"), "x"));
	FYAI_TCHECK(!strcmp(markdown_role_on(&cfg, "tool.fail", "\033[31m"),
			    "\033[31m"));
	FYAI_TCHECK(!strcmp(markdown_role_off(&cfg, "tool.fail", "\033[0m"),
			    "\033[0m"));
#ifdef FYAI_WITH_FYPALETTE
	cfg.palette = fypal_ctx_create(NULL);
	FYAI_TCHECK(cfg.palette != NULL);
	if (!cfg.palette)
		return EXIT_FAILURE;
	rc = fypal_ctx_load(cfg.palette,
			    "colors: {bad: '#c02010'}\n"
			    "roles: {tool: {fail: {fg: bad}}}\n", "test");
	FYAI_TCHECK(!rc);
	on = markdown_role_on(&cfg, "tool.fail", "\033[31m");
	FYAI_TCHECK(strstr(on, "38;2;192;32;16") != NULL);
	FYAI_TCHECK(!strcmp(markdown_role_off(&cfg, "tool.fail", "\033[0m"),
			    "\033[39m"));
	FYAI_TCHECK(!strcmp(markdown_role_on(&cfg, "notice.sigil", "\033[31m"),
			    "\033[31m"));
	fypal_ctx_destroy(cfg.palette);
#endif
	return EXIT_SUCCESS;
}

#ifdef FYAI_PALETTE_GLYPHS
/* The terminal columns of @s: escapes take none, a character one. */
static int gutter_test_cols(const char *s)
{
	int cols = 0;

	while (*s) {
		if (*s == '\033') {
			for (s++; *s && !(*s >= '@' && *s <= '~' && *s != '['); s++)
				;
			if (*s)
				s++;
			continue;
		}
		cols += ((unsigned char)*s & 0xc0) != 0x80;
		s++;
	}
	return cols;
}
#endif

/*
 * A palette theme sets the width of the gutter. Every mark in it keeps that
 * width in both charsets: each frame of the blinking arrow, the arrow of a
 * finished call, and the result marker, and tool output starts after it.
 * Without a palette the gutter keeps two columns.
 */
int markdown_gutter_palette(void)
{
	char mark[FYAI_GLYPH_MAX];
#ifdef FYAI_PALETTE_GLYPHS
	static const enum fymd_indicator_state states[] = {
		FYMD_INDICATOR_PENDING,
		FYMD_INDICATOR_SUCCESS,
		FYMD_INDICATOR_FAILURE,
	};
	struct fymd_renderer_cfg rcfg;
	struct fymd_renderer *r;
	const char *arrow;
	char *margin;
	size_t frame;
	size_t i;
	int pass;
#endif
	int rc;

	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);
	test_cfg.color = "off";
	test_cfg.palette = NULL;
	FYAI_TCHECK(markdown_gutter_cols(&test_cfg) == 2);
	FYAI_TCHECK(!strcmp(markdown_gutter_blank(&test_cfg), "  "));
	markdown_tool_marker(&test_cfg, mark, sizeof(mark));
	FYAI_TCHECK(!strcmp(mark, FYAI_TOOL_MARKER));
	FYAI_TCHECK(!strcmp(markdown_tool_output_indent(&test_cfg),
			    FYAI_TOOL_OUTPUT_INDENT));
#ifdef FYAI_PALETTE_GLYPHS
	test_cfg.palette = fypal_ctx_create(NULL);
	FYAI_TCHECK(test_cfg.palette != NULL);
	if (!test_cfg.palette)
		return EXIT_FAILURE;
	rc = fypal_ctx_load_builtin(test_cfg.palette, "ember");
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(markdown_gutter_cols(&test_cfg) == 3);
	FYAI_TCHECK(!strcmp(markdown_gutter_blank(&test_cfg), "   "));
	/* Tool output starts at the text column. */
	FYAI_TCHECK(!strcmp(markdown_tool_output_indent(&test_cfg), "   "));
	for (pass = 0; pass < 2; pass++) {
		test_cfg.diagram_charset = pass ? "ascii" : "unicode";
		arrow = pass ? "->" : "\u2192";
		markdown_renderer_cfg(&test_cfg, &rcfg, false,
				      test_cfg.theme_variant, 0);
		r = markdown_renderer_new(&test_cfg, &rcfg);
		FYAI_TCHECK(r != NULL);
		for (i = 0; r && i < sizeof(states) / sizeof(states[0]); i++) {
			for (frame = 0; frame < 4; frame++) {
				margin = markdown_indicator_margin(&test_cfg, r,
								   states[i],
								   frame, NULL);
				FYAI_TCHECK(margin != NULL);
				if (!margin)
					continue;
				FYAI_TCHECK(gutter_test_cols(margin) == 3);
				/* The arrow blinks while the call runs. */
				if (states[i] != FYMD_INDICATOR_PENDING ||
				    !(frame & 1))
					FYAI_TCHECK(strstr(margin, arrow) != NULL);
				else
					FYAI_TCHECK(strstr(margin, arrow) == NULL);
				free(margin);
			}
		}
		fymd_renderer_destroy(r);
		markdown_tool_marker(&test_cfg, mark, sizeof(mark));
		FYAI_TCHECK(gutter_test_cols(mark) == FYAI_TOOL_MARKER_WIDTH);
		FYAI_TCHECK(!strncmp(mark, pass ? "`-" : "\u23bf",
				     strlen(pass ? "`-" : "\u23bf")));
	}
	test_cfg.diagram_charset = NULL;
	fypal_ctx_destroy(test_cfg.palette);
	test_cfg.palette = NULL;
#endif
	fyai_diag_drain(&test_cfg.diag);
	fyai_diag_cleanup(&test_cfg.diag);
	return 0;
}

/*
 * A stored assistant document marks reasoning with a heading. A palette theme
 * draws the reasoning quote under its hairline without that heading, and the
 * card of the user under its own bar; every other theme keeps the heading.
 */
int markdown_reasoning_palette(void)
{
	static const char doc[] =
		FYAI_REASONING_HEAD "> weigh the cache\n\nThe answer.\n";
	struct response_buffer out = {0};
	int rc;

	rc = fyai_diag_setup(&test_cfg.diag);
	FYAI_TCHECK(!rc);
	test_cfg.color = "off";
	test_cfg.palette = NULL;
	FYAI_TCHECK(!markdown_reasoning_quoted(&test_cfg));
	rc = markdown_render(&test_cfg, doc, sizeof(doc) - 1, &out, false,
			     test_cfg.theme_variant);
	FYAI_TCHECK(!rc && out.data && strstr(out.data, "reasoning") != NULL);
	free(out.data);
#ifdef FYAI_PALETTE_GLYPHS
	test_cfg.palette = fypal_ctx_create(NULL);
	FYAI_TCHECK(test_cfg.palette != NULL);
	if (!test_cfg.palette)
		return EXIT_FAILURE;
	rc = fypal_ctx_load_builtin(test_cfg.palette, "ember");
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(markdown_reasoning_quoted(&test_cfg));
	memset(&out, 0, sizeof(out));
	rc = markdown_render(&test_cfg, doc, sizeof(doc) - 1, &out, false,
			     test_cfg.theme_variant);
	FYAI_TCHECK(!rc && out.data);
	FYAI_TCHECK(out.data && strstr(out.data, "reasoning") == NULL);
	FYAI_TCHECK(out.data && strstr(out.data, "\u258f weigh the cache"));
	FYAI_TCHECK(out.data && strstr(out.data, "The answer."));
	free(out.data);
	memset(&out, 0, sizeof(out));
	rc = markdown_render_reverse(&test_cfg, "> hello\n", 8, &out, true,
				     test_cfg.theme_variant);
	FYAI_TCHECK(!rc && out.data && strstr(out.data, "\u258c") != NULL);
	free(out.data);
	fypal_ctx_destroy(test_cfg.palette);
	test_cfg.palette = NULL;
#endif
	fyai_diag_drain(&test_cfg.diag);
	fyai_diag_cleanup(&test_cfg.diag);
	return 0;
}

/* /theme offers every theme the build has, the palette themes included. */
int markdown_theme_selectors_test(void)
{
	const char *const *sel;
	const char *const *v;
	bool dflt = false;
#ifdef FYAI_WITH_FYPALETTE
	bool ember = false;
#endif

	sel = markdown_theme_selectors();
	FYAI_TCHECK(sel != NULL);
	if (!sel)
		return EXIT_FAILURE;
	/* The array is made one time. */
	FYAI_TCHECK(markdown_theme_selectors() == sel);
	for (v = sel; *v; v++) {
		FYAI_TCHECK(markdown_theme_selector_valid(*v));
		dflt |= !strcmp(*v, "default:auto");
#ifdef FYAI_WITH_FYPALETTE
		ember |= !strcmp(*v, "ember:dark");
#endif
	}
	FYAI_TCHECK(dflt);
#ifdef FYAI_WITH_FYPALETTE
	FYAI_TCHECK(ember);
#endif
	return EXIT_SUCCESS;
}
