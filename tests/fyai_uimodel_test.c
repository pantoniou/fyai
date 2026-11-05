/*
 * fyai_uimodel_test.c - tests for the stores of the UI model
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_UNKNOWN

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fyai.h"
#include "fyai_uilog.h"
#include "fyai_uimodel.h"
#include "fyai_uitranscript.h"
#include "fyai_uiwork.h"
#include "fyai_test.h"

#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(uimodel, set_and_seq, uimodel_set_and_seq)
FYAI_TEST_ENTRY(uimodel, consumers, uimodel_consumers)
FYAI_TEST_ENTRY(uimodel, builders_bounded, uimodel_builders_bounded)
FYAI_TEST_ENTRY(uimodel, check_record, uimodel_check_record)
FYAI_TEST_ENTRY(uimodel, log_entry, uimodel_log_entry)
FYAI_TEST_ENTRY(uimodel, log_bounded, uimodel_log_bounded)
FYAI_TEST_ENTRY(uimodel, bands, uimodel_bands)
FYAI_TEST_ENTRY(uimodel, view_detached, uimodel_view_detached)
FYAI_TEST_ENTRY(uimodel, transcript_live, uimodel_transcript_live)

struct seen {
	int calls;
	unsigned long long seq;
	char model[32];
};

static void uimodel_seen(void *data, enum fyai_ui_store store,
			 unsigned long long seq, fy_generic root)
{
	struct seen *seen = data;

	(void)store;
	seen->calls++;
	seen->seq = seq;
	snprintf(seen->model, sizeof(seen->model), "%s",
		 fy_get(root, "model", ""));
}

/* Write a session root with one member "model". */
static int uimodel_put(struct fyai_ctx *ctx, const char *model)
{
	struct fy_generic_builder *gb = fyai_uimodel_begin(ctx);
	int rc;

	rc = gb ? fyai_uimodel_set(ctx, FYAI_UI_SESSION,
				   fy_mapping(gb, "model", model)) : -1;
	fyai_uimodel_end(ctx);
	return rc;
}

int uimodel_set_and_seq(void)
{
	struct fyai_cfg cfg = { 0 };
	struct fyai_ctx ctx = { .cfg = &cfg };
	fy_generic root;
	unsigned long long seq;
	int rc;

	FYAI_TCHECK(!fyai_uimodel_create(&ctx));
	root = fyai_uimodel_get(&ctx, FYAI_UI_SESSION);
	seq = fyai_uimodel_seq(&ctx, FYAI_UI_SESSION);
	FYAI_TCHECK(fy_is_invalid(root) && seq == 0);
	rc = uimodel_put(&ctx, "alpha");
	seq = fyai_uimodel_seq(&ctx, FYAI_UI_SESSION);
	FYAI_TCHECK(rc == 1 && seq == 1);
	/* The store keeps a copy: a reset of the scratch builder keeps it. */
	(void)fyai_uimodel_begin(&ctx);
	fyai_uimodel_end(&ctx);
	root = fyai_uimodel_get(&ctx, FYAI_UI_SESSION);
	FYAI_TCHECK(!strcmp(fy_get(root, "model", ""), "alpha"));
	/* An equal root changes nothing. */
	rc = uimodel_put(&ctx, "alpha");
	seq = fyai_uimodel_seq(&ctx, FYAI_UI_SESSION);
	FYAI_TCHECK(rc == 0 && seq == 1);
	rc = uimodel_put(&ctx, "beta");
	seq = fyai_uimodel_seq(&ctx, FYAI_UI_SESSION);
	FYAI_TCHECK(rc == 1 && seq == 2);
	fyai_uimodel_release(&ctx);
	FYAI_TCHECK(!ctx.uimodel);
	printf("ok - a store copies its root and counts each change\n");
	return 0;
}

int uimodel_consumers(void)
{
	struct fyai_cfg cfg = { 0 };
	struct fyai_ctx ctx = { .cfg = &cfg };
	struct seen seen = { 0 };
	int rc;

	FYAI_TCHECK(!fyai_uimodel_create(&ctx));
	rc = fyai_uimodel_subscribe(&ctx, uimodel_seen, &seen);
	FYAI_TCHECK(!rc);
	rc = uimodel_put(&ctx, "gamma");
	FYAI_TCHECK(rc == 1 && seen.calls == 1 && seen.seq == 1);
	FYAI_TCHECK(!strcmp(seen.model, "gamma"));
	/* A forked child drops the model and the consumers of the parent. */
	fyai_uimodel_abandon(&ctx);
	FYAI_TCHECK(!ctx.uimodel);
	FYAI_TCHECK(!fyai_uimodel_create(&ctx));
	rc = uimodel_put(&ctx, "delta");
	FYAI_TCHECK(rc == 1 && seen.calls == 1);
	fyai_uimodel_release(&ctx);
	printf("ok - a consumer sees each change until it is dropped\n");
	return 0;
}

/*
 * Writes reuse the same builders, the scratch builder and the two of the
 * store, so the model does not grow with the number of writes.
 */
int uimodel_builders_bounded(void)
{
	struct fyai_cfg cfg = { 0 };
	struct fyai_ctx ctx = { .cfg = &cfg };
	struct fy_generic_builder *first, *gb;
	fy_generic prev, root, value;
	int i, rc;

	FYAI_TCHECK(!fyai_uimodel_create(&ctx));
	first = fyai_uimodel_begin(&ctx);
	fyai_uimodel_end(&ctx);
	FYAI_TCHECK(first);
	prev = fy_invalid;
	for (i = 0; i < 10000; i++) {
		gb = fyai_uimodel_begin(&ctx);
		FYAI_TCHECK(gb == first);
		value = fy_mapping(gb, "count", (long long)i, "text",
				   fy_sprintfa("value %d of a string that does "
					       "not fit in a word", i));
		rc = fyai_uimodel_set(&ctx, FYAI_UI_SESSION, value);
		fyai_uimodel_end(&ctx);
		root = fyai_uimodel_get(&ctx, FYAI_UI_SESSION);
		FYAI_TCHECK(rc == 1 && fy_get(root, "count", -1LL) == i);
		/* The root before stays valid until the next write. */
		FYAI_TCHECK(!i || fy_get(prev, "count", -1LL) == i - 1);
		prev = root;
	}
	fyai_uimodel_release(&ctx);
	printf("ok - writes reuse the builders of the model\n");
	return 0;
}

int uimodel_check_record(void)
{
	struct fyai_cfg cfg = { 0 };
	struct fyai_ctx ctx = { .cfg = &cfg };
	const char *tmp = getenv("TMPDIR");
	char path[512], buf[512];
	bool same;
	ssize_t n;
	off_t end;
	int fd, rc;

	FYAI_TCHECK(!fyai_uimodel_create(&ctx));
	snprintf(path, sizeof(path), "%s/fyai-uimodel-test-XXXXXX",
		 tmp && *tmp ? tmp : "/tmp");
	fd = mkstemp(path);
	FYAI_TCHECK(fd >= 0);
	rc = setenv("FYAI_UI_MODEL_CHECK", path, 1);
	FYAI_TCHECK(!rc);
	same = fyai_uimodel_check(&ctx, FYAI_UI_SESSION, "header",
				  "same", "same");
	end = lseek(fd, 0, SEEK_END);
	FYAI_TCHECK(same && end == 0);
	same = fyai_uimodel_check(&ctx, FYAI_UI_SESSION, "header",
				  "model text", "modal text");
	rc = unsetenv("FYAI_UI_MODEL_CHECK");
	FYAI_TCHECK(!same && !rc);
	n = pread(fd, buf, sizeof(buf) - 1, 0);
	close(fd);
	unlink(path);
	FYAI_TCHECK(n > 0);
	buf[n] = '\0';
	FYAI_TCHECK(strstr(buf, "session/header diverges at byte 3"));
	FYAI_TCHECK(strstr(buf, "--- model\nmodel text\n"));
	FYAI_TCHECK(strstr(buf, "--- oracle\nmodal text\n"));
	printf("ok - a divergence is recorded with both texts\n");
	return 0;
}

/* The newest entry of the log store, or fy_invalid. */
static fy_generic uimodel_log_last(struct fyai_ctx *ctx)
{
	fy_generic entries = fy_get(fyai_uimodel_get(ctx, FYAI_UI_LOG),
				    "entries", fy_seq_empty);

	return fy_empty(entries) ? fy_invalid :
		fy_get_at(entries, fy_len(entries) - 1);
}

int uimodel_log_entry(void)
{
	struct fyai_cfg cfg = { 0 };
	struct fyai_ctx ctx = { .cfg = &cfg };
	fy_generic entry, parts, part;
	unsigned long long seq;

	FYAI_TCHECK(!fyai_uimodel_create(&ctx));
	/* Output outside a span is not recorded. */
	fyai_uilog_sink(&ctx, FYAI_SINK_NOTICE, false, "stray\n", 6);
	FYAI_TCHECK(!ctx.uilog);
	fyai_uilog_begin(&ctx);
	fyai_uilog_sink(&ctx, FYAI_SINK_NOTICE, true, "**done**", 8);
	fyai_uilog_sink(&ctx, FYAI_SINK_STATUS, false, "status\n", 7);
	fyai_uilog_end(&ctx, "status", false);
	entry = uimodel_log_last(&ctx);
	parts = fy_get(entry, "parts", fy_seq_empty);
	FYAI_TCHECK(!strcmp(fy_get(entry, "title", ""), "status"));
	FYAI_TCHECK(fy_get(entry, "level", -1LL) == FYAIET_INFO);
	FYAI_TCHECK(fy_len(parts) == 2);
	part = fy_get_at(parts, 0);
	FYAI_TCHECK(!strcmp(fy_get(part, "stream", ""), "notice"));
	FYAI_TCHECK(!strcmp(fy_get(part, "md", ""), "**done**"));
	part = fy_get_at(parts, 1);
	FYAI_TCHECK(!strcmp(fy_get(part, "stream", ""), "status"));
	FYAI_TCHECK(!strcmp(fy_get(part, "text", ""), "status\n"));
	/* A span that gave nothing adds no entry. */
	seq = fyai_uimodel_seq(&ctx, FYAI_UI_LOG);
	fyai_uilog_begin(&ctx);
	fyai_uilog_end(&ctx, "quiet", false);
	FYAI_TCHECK(fyai_uimodel_seq(&ctx, FYAI_UI_LOG) == seq);
	/* An error span is an error entry. */
	fyai_uilog_begin(&ctx);
	fyai_uilog_sink(&ctx, FYAI_SINK_NOTICE, false, "failed\n", 7);
	fyai_uilog_end(&ctx, "fail", true);
	entry = uimodel_log_last(&ctx);
	FYAI_TCHECK(fy_get(entry, "level", -1LL) == FYAIET_ERROR);
	FYAI_TCHECK(fy_get(entry, "error", false));
	fyai_uilog_release(&ctx);
	fyai_uimodel_release(&ctx);
	printf("ok - a span records its output as one entry of the log\n");
	return 0;
}

int uimodel_log_bounded(void)
{
	struct fyai_cfg cfg = { 0 };
	struct fyai_ctx ctx = { .cfg = &cfg };
	fy_generic entries;
	int i;

	FYAI_TCHECK(!fyai_uimodel_create(&ctx));
	for (i = 0; i < FYAI_UILOG_MAX + 25; i++) {
		fyai_uilog_begin(&ctx);
		fyai_uilog_sink(&ctx, FYAI_SINK_NOTICE, false, "x\n", 2);
		fyai_uilog_end(&ctx, "n", false);
	}
	entries = fy_get(fyai_uimodel_get(&ctx, FYAI_UI_LOG), "entries",
			 fy_seq_empty);
	FYAI_TCHECK(fy_len(entries) == FYAI_UILOG_MAX);
	FYAI_TCHECK(fy_get(fy_get_at(entries, 0), "id", 0LL) == 26);
	fyai_uilog_release(&ctx);
	fyai_uimodel_release(&ctx);
	printf("ok - the log store keeps the newest entries\n");
	return 0;
}

/* The band of the work store at @index, or fy_invalid. */
static fy_generic uimodel_band(struct fyai_ctx *ctx, size_t index)
{
	fy_generic bands = fy_get(fyai_uimodel_get(ctx, FYAI_UI_WORK), "bands",
				  fy_seq_empty);

	return index < fy_len(bands) ? fy_get_at(bands, index) : fy_invalid;
}

int uimodel_bands(void)
{
	struct fyai_cfg cfg = { 0 };
	struct fyai_ctx ctx = { .cfg = &cfg };
	/* The registry knows a band by its address only. */
	char one = 0, two = 0, owner_a = 0, owner_b = 0;
	struct fyai_sink_band *a = (struct fyai_sink_band *)&one;
	struct fyai_sink_band *b = (struct fyai_sink_band *)&two;
	fy_generic band;
	const char *text;
	size_t len;

	FYAI_TCHECK(!fyai_uimodel_create(&ctx));
	fyai_uiwork_band_open(&ctx, a, true, "**shell**", "ls");
	fyai_uiwork_band_paint(&ctx, a, NULL, NULL, "rows\n", 5);
	band = uimodel_band(&ctx, 0);
	FYAI_TCHECK(!strcmp(fy_get(band, "format", ""), "rows"));
	FYAI_TCHECK(!strcmp(fy_get(band, "text", ""), "rows\n"));
	FYAI_TCHECK(!strcmp(fy_get(band, "command", ""), "ls"));
	/* The source of a stream replaces what was painted. */
	fyai_uiwork_band_source(&ctx, a, &owner_a, "terminal", NULL, "ab", 2);
	fyai_uiwork_band_source(&ctx, a, &owner_a, "terminal", NULL, "\rc", 2);
	band = uimodel_band(&ctx, 0);
	FYAI_TCHECK(!strcmp(fy_get(band, "format", ""), "terminal"));
	FYAI_TCHECK(fy_is_invalid(fy_get(band, "text", fy_invalid)));
	FYAI_TCHECK(fy_get(band, "length", 0LL) == 4);
	text = fyai_uimodel_text(&ctx, fy_get(band, "source", 0LL), &len);
	FYAI_TCHECK(len == 4 && !memcmp(text, "ab\rc", 4));
	/* A painted body does not replace the source. */
	fyai_uiwork_band_paint(&ctx, a, NULL, NULL, "rendered\n", 9);
	FYAI_TCHECK(fy_is_invalid(fy_get(uimodel_band(&ctx, 0), "text",
					 fy_invalid)));
	/* Another stream starts the source again. */
	fyai_uiwork_band_source(&ctx, a, &owner_b, "code", "c", "int", 3);
	band = uimodel_band(&ctx, 0);
	FYAI_TCHECK(fy_get(band, "length", 0LL) == 3);
	FYAI_TCHECK(!strcmp(fy_get(band, "language", ""), "c"));
	/* A second shared band replaces the first; a tile band stands. */
	fyai_uiwork_band_open(&ctx, b, false, "tile", NULL);
	fyai_uiwork_band_open(&ctx, a, true, "again", NULL);
	FYAI_TCHECK(!strcmp(fy_get(uimodel_band(&ctx, 0), "title", ""),
			    "tile"));
	FYAI_TCHECK(!strcmp(fy_get(uimodel_band(&ctx, 1), "title", ""),
			    "again"));
	FYAI_TCHECK(fy_is_invalid(uimodel_band(&ctx, 2)));
	fyai_uiwork_band_end(&ctx, NULL);
	fyai_uiwork_band_end(&ctx, b);
	FYAI_TCHECK(fy_is_invalid(uimodel_band(&ctx, 0)));
	fyai_uiwork_release(&ctx);
	fyai_uimodel_release(&ctx);
	printf("ok - the work store holds each band from its input\n");
	return 0;
}

int uimodel_view_detached(void)
{
	struct fyai_cfg cfg = { 0 };
	struct fyai_ctx ctx = { .cfg = &cfg };
	struct fyai_ctx view;
	char one;
	int rc;

	FYAI_TCHECK(!fyai_uimodel_create(&ctx));
	rc = uimodel_put(&ctx, "live");
	FYAI_TCHECK(rc == 1 && ctx.uimodel);
	view = ctx;
	fyai_uimodel_view_detach(&view);
	rc = uimodel_put(&view, "view");
	fyai_uiwork_band_open(&view, (struct fyai_sink_band *)&one, true,
			      "view", NULL);
	fyai_uilog_begin(&view);
	FYAI_TCHECK(rc < 0 && !view.uimodel && !view.uiwork && !view.uilog);
	FYAI_TCHECK(!strcmp(fy_get(fyai_uimodel_get(&ctx, FYAI_UI_SESSION),
				   "model", ""), "live"));
	fyai_uimodel_release(&ctx);
	printf("ok - an offscreen view writes no model\n");
	return 0;
}

int uimodel_transcript_live(void)
{
	struct fyai_cfg cfg = { 0 };
	struct fyai_ctx ctx = { .cfg = &cfg };
	fy_generic root, live, recorded;
	const char *text;
	size_t len;

	FYAI_TCHECK(!fyai_uimodel_create(&ctx));
	fyai_uitranscript_begin(&ctx, "assistant");
	fyai_uitranscript_append(&ctx, "Hello ", 6, true);
	fyai_uitranscript_append(&ctx, "[tool", 5, false);
	fyai_uitranscript_append(&ctx, " call]", 6, false);
	fyai_uitranscript_append(&ctx, " done", 5, true);
	fyai_uitranscript_append(&ctx, "[x]", 3, false);
	fyai_uitranscript_fragment(&ctx, fy_mapping("kind", "tool_head",
						    "start", 6LL, "end", 17LL));
	live = fy_get(fyai_uimodel_get(&ctx, FYAI_UI_TRANSCRIPT), "live",
		      fy_invalid);
	FYAI_TCHECK(!strcmp(fy_get(live, "tag", ""), "assistant"));
	FYAI_TCHECK(fy_get(live, "length", 0LL) == 25);
	text = fyai_uimodel_text(&ctx, fy_get(live, "source", 0LL), &len);
	FYAI_TCHECK(len == 25 && !memcmp(text, "Hello [tool call] done[x]", 25));
	/* Recorded appends that follow each other make one range. */
	recorded = fy_get(live, "recorded", fy_seq_empty);
	FYAI_TCHECK(fy_len(recorded) == 2);
	FYAI_TCHECK(fy_equal(fy_get_at(recorded, 0), fy_sequence(6LL, 17LL)));
	FYAI_TCHECK(fy_equal(fy_get_at(recorded, 1), fy_sequence(22LL, 25LL)));
	FYAI_TCHECK(!strcmp(fy_get(fy_get_at(fy_get(live, "fragments",
		fy_seq_empty), 0), "kind", ""), "tool_head"));
	fyai_uitranscript_end(&ctx, "finalized");
	root = fyai_uimodel_get(&ctx, FYAI_UI_TRANSCRIPT);
	FYAI_TCHECK(fy_is_null(fy_get(root, "live", fy_invalid)));
	FYAI_TCHECK(!strcmp(fy_get(fy_get(root, "last", fy_invalid), "state",
				   ""), "finalized"));
	fyai_uitranscript_release(&ctx);
	fyai_uimodel_release(&ctx);
	printf("ok - the transcript store holds the live document\n");
	return 0;
}
