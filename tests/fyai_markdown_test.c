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
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fyai.h"
#include "fyai_diag.h"
#include "fyai_markdown.h"
#include "fyai_test.h"

#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(markdown, window_bounds_render, markdown_window_bounds_render)
FYAI_TEST_ENTRY(markdown, window_reopens_fence, markdown_window_reopens_fence)
FYAI_TEST_ENTRY(markdown, window_off_when_unbounded, markdown_window_off_when_unbounded)
FYAI_TEST_ENTRY(markdown, final_render_is_whole, markdown_final_render_is_whole)

static struct fyai_cfg test_cfg;
static struct fyai_ctx test_ctx = { .cfg = &test_cfg };

/* Rows the live band shows. */
#define TEST_MAX_LINES 5

static double now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* Push @steps exchanges of prose and a fenced C block, repainting each time. */
static double push_steps(struct fyai_fenced_stream *fs, size_t steps)
{
	char chunk[256];
	double worst = 0, t0, dt;
	size_t i;
	int n;

	for (i = 0; i < steps; i++) {
		n = snprintf(chunk, sizeof(chunk),
			     "Step %zu: editing a file.\n\n```c\n"
			     "static int helper_%zu(int a)\n{\n"
			     "\treturn a + 1;\n}\n```\n\n", i, i);
		/* Render every push: the throttle is not what is under test. */
		fs->next_render_ms = 0;
		t0 = now_ms();
		FYAI_TCHECK(!fyai_fenced_stream_push(fs, chunk, (size_t)n));
		dt = now_ms() - t0;
		if (dt > worst)
			worst = dt;
	}
	return worst;
}

/*
 * A repaint late in a long stream must not cost more than an early one. The
 * window holds this flat, so the bound leaves room for a loaded machine and
 * still separates the two shapes: rendering the whole accumulator instead
 * makes the late repaint grow with everything pushed before it.
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
	early = push_steps(&fs, 100);
	push_steps(&fs, 600);
	late = push_steps(&fs, 100);

	FYAI_TCHECK(fs.accum.len > 60000);	/* the accumulator did grow */
	FYAI_TCHECK(late < early * 2 + 2.0);

	fyai_fenced_stream_finish(&fs);
	fclose(fp);
	printf("ok - a live repaint stays bounded (early %.2fms, late %.2fms)\n",
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
