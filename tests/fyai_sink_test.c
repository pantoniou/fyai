/*
 * fyai_sink_test.c - unit tests for the rendering sink
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_DISPLAY

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fyai.h"
#include "fyai_output.h"
#include "fyai_sink.h"
#include "fyai_terminal.h"

#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(sink, chunked_equals_whole, sink_chunked_equals_whole)
FYAI_TEST_ENTRY(sink, reasoning_blockquote, sink_reasoning_blockquote)
FYAI_TEST_ENTRY(sink, recorded_is_not_presented, sink_recorded_not_presented)
FYAI_TEST_ENTRY(sink, fragments_survive_pause, sink_fragments_survive_pause)
FYAI_TEST_ENTRY(sink, discard_presents_nothing, sink_discard_presents_nothing)
FYAI_TEST_ENTRY(sink, bands_absent_without_backend, sink_bands_absent)
FYAI_TEST_ENTRY(sink, protocol_fd_stays_clean, sink_protocol_fd_clean)
FYAI_TEST_ENTRY(sink, reflow_follows_width, sink_reflow_follows_width)

static int failures;

static void expect_str(const char *what, const char *got, const char *want)
{
	if (got && want && !strcmp(got, want))
		return;
	fprintf(stderr, "FAIL %s\n  got:  %s\n  want: %s\n", what,
		got ? got : "(null)", want ? want : "(null)");
	failures++;
}

static void expect_true(const char *what, bool cond)
{
	if (cond)
		return;
	fprintf(stderr, "FAIL %s\n", what);
	failures++;
}

/*
 * A context with a capture sink. Only the fields the output document and the
 * sink read are set: this is deliberately not a full run.
 */
struct sink_fixture {
	struct fyai_cfg cfg;
	struct fyai_ctx ctx;
};

static void fixture_open(struct sink_fixture *f)
{
	memset(f, 0, sizeof(*f));
	f->cfg.markdown = true;
	f->cfg.markdown_mode = "stream";
	f->cfg.color = "off";
	f->ctx.cfg = &f->cfg;
	f->ctx.last_message = fy_invalid;
	f->ctx.tools = fy_invalid;
	f->ctx.tools_spec = fy_invalid;
	f->ctx.arena_config = fy_invalid;
	f->ctx.arena_catalog = fy_invalid;
	f->ctx.arena_branches = fy_invalid;
	f->ctx.branch_prev = fy_invalid;
	f->ctx.branch_desc = fy_invalid;
	f->ctx.branch_agent = fy_invalid;
	f->ctx.last_token_extents = fy_invalid;
	f->ctx.sink = fyai_sink_create_capture(&f->ctx);
	/* Fragments and turns are built here, as they are in a real run. */
	if (!f->ctx.sink || !fyai_ctx_transient_gb(&f->ctx)) {
		fprintf(stderr, "could not open the sink fixture\n");
		exit(1);
	}
}

static void fixture_close(struct sink_fixture *f)
{
	fyai_output_cleanup(&f->ctx);
	fyai_sink_destroy(f->ctx.sink);
	f->ctx.sink = NULL;
	fyai_cleanup_transient_builder(&f->ctx);
}

static const char *captured(struct sink_fixture *f)
{
	return fyai_sink_captured(f->ctx.sink, NULL);
}

/*
 * Close the document the way a turn does. A real turn generic is needed:
 * finalizing against a null turn discards the document instead of ending it.
 */
static void fixture_finish(struct sink_fixture *f)
{
	struct fy_generic_builder *gb;
	fy_generic turn;

	gb = fyai_ctx_transient_gb(&f->ctx);
	turn = fy_mapping(gb, "messages", fy_seq_empty);
	turn = fyai_output_finalize(&f->ctx, turn, false);
	if (fy_is_invalid(turn)) {
		fprintf(stderr, "could not finalize the display output\n");
		exit(1);
	}
}

/*
 * A document written one byte at a time and the same document written in one
 * call must present the same bytes. The two progressive renderers this sink
 * replaced drifted precisely here.
 */
static void test_chunked_equals_whole(void)
{
	static const char text[] = "# Title\n\nA paragraph, then a list:\n\n"
				   "- one\n- two\n\nDone.\n";
	struct sink_fixture f;
	char *whole;
	size_t i;

	fixture_open(&f);
	(void)fyai_output_begin(&f.ctx, FYAI_OUTPUT_ASSISTANT);
	(void)fyai_output_append_string(&f.ctx, text);
	fixture_finish(&f);
	whole = strdup(captured(&f));
	/* Guard the comparison below against two empty strings. */
	expect_true("the document was presented", whole && *whole);
	fixture_close(&f);

	fixture_open(&f);
	(void)fyai_output_begin(&f.ctx, FYAI_OUTPUT_ASSISTANT);
	for (i = 0; text[i]; i++)
		(void)fyai_output_append(&f.ctx, text + i, 1);
	fixture_finish(&f);
	expect_str("chunked matches whole", captured(&f), whole);
	fixture_close(&f);
	free(whole);
}

/*
 * Reasoning is a blockquote, so a chunk that splits in the middle of a line
 * must not leave a row outside the quote.
 */
static void test_reasoning_blockquote(void)
{
	struct sink_fixture f;
	const char *out;

	fixture_open(&f);
	(void)fyai_output_begin(&f.ctx, FYAI_OUTPUT_ASSISTANT);
	(void)fyai_output_reasoning_append(&f.ctx, "first line\nsec");
	(void)fyai_output_reasoning_append(&f.ctx, "ond line\n");
	(void)fyai_output_reasoning_finish(&f.ctx);
	fixture_finish(&f);

	out = captured(&f);
	expect_true("reasoning opens a quote",
		    strstr(out, "> **💭 reasoning**") != NULL);
	expect_true("the split line stays quoted",
		    strstr(out, "\n> second line") != NULL);
	expect_true("no row escapes the quote",
		    strstr(out, "\nsecond line") == NULL);
	fixture_close(&f);
}

/*
 * A tool exchange is drawn by the tool path, so recording it must add it to
 * the durable source without presenting it a second time.
 */
static void test_recorded_not_presented(void)
{
	struct sink_fixture f;

	fixture_open(&f);
	(void)fyai_output_begin(&f.ctx, FYAI_OUTPUT_ASSISTANT);
	(void)fyai_output_append_string(&f.ctx, "prose\n");
	(void)fyai_output_append_recorded(&f.ctx, "**tool**\n", 9);
	expect_str("the source keeps both",
		   fyai_output_markdown(&f.ctx, NULL), "prose\n**tool**\n");
	fixture_finish(&f);
	expect_str("only the prose is presented", captured(&f), "prose\n");
	fixture_close(&f);
}

/*
 * A work band pauses the document. Offsets recorded around the pause index the
 * durable source, so they must not move.
 */
static void test_fragments_survive_pause(void)
{
	struct sink_fixture f;
	size_t start;

	fixture_open(&f);
	(void)fyai_output_begin(&f.ctx, FYAI_OUTPUT_ASSISTANT);
	(void)fyai_output_append_string(&f.ctx, "before\n");
	(void)fyai_output_checkpoint(&f.ctx);
	start = strlen(fyai_output_markdown(&f.ctx, NULL));
	(void)fyai_output_append_recorded(&f.ctx, "BODY\n", 5);
	expect_true("the fragment records",
		    !fyai_output_add_fragment(&f.ctx, "tool_body", start,
					      start + 5, NULL, "shell"));
	(void)fyai_output_resume(&f.ctx);
	(void)fyai_output_append_string(&f.ctx, "after\n");
	expect_str("the source is continuous",
		   fyai_output_markdown(&f.ctx, NULL),
		   "before\nBODY\nafter\n");
	expect_true("the fragment still indexes its body",
		    !memcmp(fyai_output_markdown(&f.ctx, NULL) + start,
			    "BODY\n", 5));
	fixture_finish(&f);
	fixture_close(&f);
}

/* An aborted document presents nothing further. */
static void test_discard_presents_nothing(void)
{
	struct sink_fixture f;

	fixture_open(&f);
	(void)fyai_output_begin(&f.ctx, FYAI_OUTPUT_ASSISTANT);
	(void)fyai_output_append_string(&f.ctx, "partial");
	fyai_output_abort(&f.ctx);
	expect_str("nothing reaches the sink", captured(&f), "");
	fixture_close(&f);
}

/*
 * A backend with no bands reports none, and opening one yields nothing rather
 * than failing: the producer then presents the tool its own way.
 */
static void test_bands_absent(void)
{
	struct sink_fixture f;

	fixture_open(&f);
	expect_true("capture offers no bands",
		    !fyai_sink_bands_available(f.ctx.sink));
	expect_true("opening a band yields none",
		    fyai_sink_band_open(f.ctx.sink, true, "t", NULL) == NULL);
	/* Painting and closing a band that does not exist must be safe. */
	fyai_sink_band_paint(NULL, "t", NULL, "b", 1, NULL);
	fyai_sink_band_close(f.ctx.sink, true, NULL);
	fyai_sink_band_destroy(NULL);
	fixture_close(&f);
}

int sink_chunked_equals_whole(void)
{
	failures = 0;
	test_chunked_equals_whole();
	return failures ? -1 : 0;
}

int sink_reasoning_blockquote(void)
{
	failures = 0;
	test_reasoning_blockquote();
	return failures ? -1 : 0;
}

int sink_recorded_not_presented(void)
{
	failures = 0;
	test_recorded_not_presented();
	return failures ? -1 : 0;
}

int sink_fragments_survive_pause(void)
{
	failures = 0;
	test_fragments_survive_pause();
	return failures ? -1 : 0;
}

int sink_discard_presents_nothing(void)
{
	failures = 0;
	test_discard_presents_nothing();
	return failures ? -1 : 0;
}

int sink_bands_absent(void)
{
	failures = 0;
	test_bands_absent();
	return failures ? -1 : 0;
}

/*
 * A delegated sub-agent and a forked tool child own file descriptor 1 for
 * JSON-RPC frames. One byte that is not a frame makes the parent read a
 * fragment and lose the frame around it, so the sink must put nothing there.
 *
 * The producers each test for this themselves. This pins the promise in the
 * one place that can keep it, for whatever a producer, a debug dump or a later
 * caller forgets.
 */
static void test_protocol_fd_stays_clean(void)
{
	struct sink_fixture f;
	char buf[256];
	int saved, tmp;
	ssize_t n;

	memset(&f, 0, sizeof(f));
	f.cfg.markdown = false;
	f.cfg.color = "off";
	/* A forked tool child: descriptor 1 is the response pipe. */
	f.cfg.tool_child = true;
	f.ctx.cfg = &f.cfg;
	f.ctx.last_message = fy_invalid;
	f.ctx.tools = fy_invalid;
	f.ctx.tools_spec = fy_invalid;
	f.ctx.arena_config = fy_invalid;
	f.ctx.arena_catalog = fy_invalid;
	f.ctx.arena_branches = fy_invalid;
	f.ctx.branch_prev = fy_invalid;
	f.ctx.branch_desc = fy_invalid;
	f.ctx.branch_agent = fy_invalid;
	f.ctx.last_token_extents = fy_invalid;
	/* The terminal backend: the one that reaches a descriptor. */
	f.ctx.sink = fyai_sink_create(&f.ctx);
	expect_true("the terminal sink opened", f.ctx.sink != NULL);
	if (!f.ctx.sink)
		return;

	tmp = fileno(tmpfile());
	saved = dup(STDOUT_FILENO);
	fflush(stdout);
	(void)dup2(tmp, STDOUT_FILENO);

	/* Every stream that would otherwise reach descriptor 1. */
	(void)fyai_sink_write(f.ctx.sink, FYAI_SINK_MACHINE, "{\"a\":1}\n", 8);
	(void)fyai_sink_write(f.ctx.sink, FYAI_SINK_TRANSCRIPT, "prose\n", 6);
	(void)fyai_sink_write(f.ctx.sink, FYAI_SINK_NOTICE, "notice\n", 7);
	(void)fyai_sink_markdown(f.ctx.sink, FYAI_SINK_TRANSCRIPT, "# heading");
	fflush(stdout);

	n = pread(tmp, buf, sizeof(buf), 0);
	(void)dup2(saved, STDOUT_FILENO);
	close(saved);
	close(tmp);
	expect_true("nothing reached the protocol descriptor", n <= 0);
	if (n > 0)
		fprintf(stderr, "  wrote %zd bytes: %.*s\n", n,
			(int)(n > 120 ? 120 : n), buf);

	fyai_sink_destroy(f.ctx.sink);
	f.ctx.sink = NULL;
	fyai_cleanup_transient_builder(&f.ctx);
}

int sink_protocol_fd_clean(void)
{
	failures = 0;
	test_protocol_fd_stays_clean();
	return failures ? -1 : 0;
}

/* What follows the last erase-down: the rows the reader is left looking at. */
static const char *after_last_erase(const char *text)
{
	const char *p, *last;

	last = text;
	for (p = text; (p = strstr(p, FYAI_ANSI_ERASE_DOWN)); ) {
		p += strlen(FYAI_ANSI_ERASE_DOWN);
		last = p;
	}
	return last;
}

/* The widest row of @text, in columns. The colour is off, so a byte is a
 * column and only the row breaks have to be found. */
static size_t widest_row(const char *text, size_t len)
{
	size_t i, start, widest;

	widest = 0;
	start = 0;
	for (i = 0; i <= len; i++) {
		if (i < len && text[i] != '\n')
			continue;
		if (i - start > widest)
			widest = i - start;
		start = i + 1;
	}
	return widest;
}

/*
 * Rows are hard-wrapped when they are made, thus a window that changed width
 * leaves every row it already drew at the old one. A reflow makes them again
 * from the source of the document, and the rows it draws fit the new width.
 */
static void test_reflow_follows_width(void)
{
	static const char text[] =
		"A paragraph long enough that it has to be wrapped, and "
		"wrapped again when the window it was made for is not the "
		"window it is looked at in.\n";
	struct sink_fixture f;
	char buf[8192];
	const char *last;
	int saved, tmp;
	ssize_t n;

	memset(&f, 0, sizeof(f));
	f.cfg.markdown = true;
	f.cfg.markdown_mode = "stream";
	f.cfg.color = "off";
	f.cfg.render_width = 60;
	f.ctx.cfg = &f.cfg;
	f.ctx.stdout_tty = true;	/* the live path draws only for a screen */
	f.ctx.last_message = fy_invalid;
	f.ctx.tools = fy_invalid;
	f.ctx.tools_spec = fy_invalid;
	f.ctx.arena_config = fy_invalid;
	f.ctx.arena_catalog = fy_invalid;
	f.ctx.arena_branches = fy_invalid;
	f.ctx.branch_prev = fy_invalid;
	f.ctx.branch_desc = fy_invalid;
	f.ctx.branch_agent = fy_invalid;
	f.ctx.last_token_extents = fy_invalid;
	f.ctx.sink = fyai_sink_create(&f.ctx);
	expect_true("the terminal sink opened", f.ctx.sink != NULL);
	if (!f.ctx.sink || !fyai_ctx_transient_gb(&f.ctx))
		return;

	tmp = fileno(tmpfile());
	saved = dup(STDOUT_FILENO);
	fflush(stdout);
	(void)dup2(tmp, STDOUT_FILENO);

	(void)fyai_output_begin(&f.ctx, FYAI_OUTPUT_ASSISTANT);
	(void)fyai_output_append_string(&f.ctx, text);
	expect_true("the document is live",
		    fyai_sink_doc_is_live(f.ctx.sink));
	/* The window is made narrower under the rows already drawn. */
	f.cfg.render_width = 30;
	fyai_sink_reflow(f.ctx.sink);
	fflush(stdout);

	n = pread(tmp, buf, sizeof(buf) - 1, 0);
	(void)dup2(saved, STDOUT_FILENO);
	close(saved);
	close(tmp);
	buf[n > 0 ? n : 0] = '\0';

	expect_true("the reflow drew something", n > 0);
	/* What the reader is left with is what follows the last erase. */
	last = after_last_erase(buf);
	expect_true("the rows fit the new width",
		    last && widest_row(last, strlen(last)) <= 30);
	if (last && widest_row(last, strlen(last)) > 30)
		fprintf(stderr, "  widest row: %zu\n",
			widest_row(last, strlen(last)));

	fyai_output_abort(&f.ctx);
	fyai_sink_destroy(f.ctx.sink);
	f.ctx.sink = NULL;
	fyai_cleanup_transient_builder(&f.ctx);
}

int sink_reflow_follows_width(void)
{
	failures = 0;
	test_reflow_follows_width();
	return failures ? -1 : 0;
}
