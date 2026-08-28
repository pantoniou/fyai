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

FYAI_TEST_ENTRY(markdown, window_bounds_render, markdown_window_bounds_render)
FYAI_TEST_ENTRY(markdown, window_reopens_fence, markdown_window_reopens_fence)
FYAI_TEST_ENTRY(markdown, window_off_when_unbounded, markdown_window_off_when_unbounded)
FYAI_TEST_ENTRY(markdown, final_render_is_whole, markdown_final_render_is_whole)
FYAI_TEST_ENTRY(markdown, tool_head_chrome, markdown_tool_head_chrome)
FYAI_TEST_ENTRY(markdown, source_rows_utf8, markdown_source_rows_utf8)

static struct fyai_cfg test_cfg;
static struct fyai_ctx test_ctx = { .cfg = &test_cfg };

/* Rows the live band shows. */
#define TEST_MAX_LINES 5

int markdown_source_rows_utf8(void)
{
	const char *wide = "\xe6\x97\xa5\xe6\x9c\xac";
	const char *combining = "e\xcc\x81";

	setlocale(LC_CTYPE, "");
	FYAI_TCHECK(fyai_display_source_rows("abcd", 4, 4) == 2);
	FYAI_TCHECK(fyai_display_source_rows(wide, strlen(wide), 4) == 2);
	FYAI_TCHECK(fyai_display_source_rows(combining, strlen(combining), 1) == 2);
	return EXIT_SUCCESS;
}

static double now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/*
 * Push @steps exchanges of prose and a fenced C block, repainting each time,
 * and return what the batch cost in total.
 *
 * The total and not the slowest push: a loaded machine takes one repaint away
 * for milliseconds at a time, which is ten times a normal push and says
 * nothing about the window. Over a batch that stall is a small part of the
 * measurement, and the growth this test looks for is in every push.
 */
static double push_steps(struct fyai_fenced_stream *fs, size_t steps)
{
	char chunk[256];
	double t0;
	size_t i;
	int n;

	t0 = now_ms();
	for (i = 0; i < steps; i++) {
		n = snprintf(chunk, sizeof(chunk),
			     "Step %zu: editing a file.\n\n```c\n"
			     "static int helper_%zu(int a)\n{\n"
			     "\treturn a + 1;\n}\n```\n\n", i, i);
		/* Render every push: the throttle is not what is under test. */
		fs->next_render_ms = 0;
		FYAI_TCHECK(!fyai_fenced_stream_push(fs, chunk, (size_t)n));
	}
	return now_ms() - t0;
}

/*
 * The cheapest of @rounds batches.
 *
 * A machine under load takes a batch away for milliseconds at a time, which
 * only ever adds to what one costs. The cheapest round is the one that says
 * what the work costs, and it is the same measurement on an idle machine.
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
 * holds this flat, so the bound leaves room for a loaded machine and still
 * separates the two shapes: rendering the whole accumulator instead makes the
 * late repaints grow with everything pushed before them, which over a hundred
 * of them is far more than the allowance.
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
