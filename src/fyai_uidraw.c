/* SPDX-License-Identifier: MIT */
#define FYAI_MODULE FYAIEM_DISPLAY

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai.h"
#include "fyai_display.h"
#include "fyai_flow.h"
#include "fyai_markdown.h"
#include "fyai_sink.h"
#include "fyai_ui.h"
#include "fyai_uidraw.h"
#include "fyai_uiwork.h"
#include "utils.h"

/*
 * A band of the work store, drawn by a band of the display. A band with
 * source feeds a stream that renders it at the width of the band.
 */
struct uidraw_band {
	struct uidraw_band *next;
	long long id;
	struct fyai_sink_band *band;
	bool shared;
	struct fyai_fenced_stream stream;
	long long source;
	size_t shown;
	char *title, *command, *language;
	char *viewed;			/* the tool view last rendered */
};

struct fyai_uidraw {
	/* The live document: its text, the bytes the sink has, its tag, and
	 * whether its render stopped at a checkpoint. */
	long long live_source;
	size_t live_shown;
	char live_tag[16];
	bool live_paused;
	/* What this renderer gave the sink for the live document, and what
	 * the legacy path gave it. */
	struct response_buffer trace, oracle;
	/* The newest entry of the log shown. A mirror does not show the
	 * entries of its snapshot: they were shown before it attached. */
	long long log_shown;
	bool attached;
	struct uidraw_band *bands;
	/* The lines that this mirror echoed, oldest first, whose cards the
	 * owner of the model records later. */
	char **echoed;
	size_t nechoed;
	/* The text of a card that a mirror draws when its document ends: the
	 * text of the model closes with the document. */
	struct response_buffer card;
};

static struct fyai_uidraw *uidraw(struct fyai_ctx *ctx)
{
	if (!ctx || ctx->uimodel_detached)
		return NULL;
	if (!ctx->uidraw) {
		ctx->uidraw = calloc(1, sizeof(*ctx->uidraw));
		fyai_error_check(ctx, ctx->uidraw, err,
				 "cannot keep the renderer of the UI model");
	}
	return ctx->uidraw;
err:
	return NULL;
}

/* A failure to draw is a display failure: the turn continues. */
#define uidraw_warn_check(_ctx, _cond, _what)				\
	do {								\
		if (!(_cond))						\
			fyai_warning((_ctx), "cannot draw %s", (_what));\
	} while (0)

static void uidraw_trace(struct fyai_ctx *ctx, struct response_buffer *buf,
			 const char *data, size_t len)
{
	/* A mirror has no legacy path to compare with. */
	if (fyai_uimodel_mirrored(ctx) || !len)
		return;
	if (response_buffer_append_data(buf, data, len))
		fyai_warning(ctx, "cannot keep the trace of the live document");
}

static void uidraw_trace_reset(struct fyai_uidraw *d)
{
	d->trace.len = 0;
	d->oracle.len = 0;
	if (d->trace.data)
		d->trace.data[0] = '\0';
	if (d->oracle.data)
		d->oracle.data[0] = '\0';
}

void fyai_uidraw_oracle(struct fyai_ctx *ctx, const char *data, size_t len)
{
	struct fyai_uidraw *d = uidraw(ctx);

	if (d)
		uidraw_trace(ctx, &d->oracle, data, len);
}

void fyai_uidraw_oracle_printf(struct fyai_ctx *ctx, const char *fmt, ...)
{
	char *op;
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vasprintf(&op, fmt, ap);
	va_end(ap);
	if (n < 0) {
		fyai_error(ctx, "cannot format an operation of the live document");
		return;
	}
	fyai_uidraw_oracle(ctx, op, (size_t)n);
	free(op);
}

static enum fyai_sink_doc_kind uidraw_doc_kind(const char *tag)
{
	if (!strcmp(tag, "system"))
		return FYAI_SINK_DOC_SYSTEM;
	if (!strcmp(tag, "user") || !strcmp(tag, "agent"))
		return FYAI_SINK_DOC_USER;
	return FYAI_SINK_DOC_ASSISTANT;
}

/*
 * Give the sink the bytes of the live document up to @length, the length
 * that the store names, less the recorded ranges: a band presented them.
 */
static void uidraw_live_feed(struct fyai_ctx *ctx, struct fyai_uidraw *d,
			     fy_generic live)
{
	fy_generic range, recorded;
	const char *text;
	size_t len, start, end, from;
	int rc = 0;

	text = fyai_uimodel_text(ctx, d->live_source, &len);
	end = fy_get(live, "length", (size_t)0);
	if (end < len)
		len = end;
	if (len <= d->live_shown)
		return;
	if (fyai_uimodel_mirrored(ctx) && strcmp(d->live_tag, "assistant") &&
	    response_buffer_append_data(&d->card, text + d->live_shown,
					len - d->live_shown))
		fyai_warning(ctx, "cannot keep the text of a card");
	from = d->live_shown;
	recorded = fy_get(live, "recorded", fy_seq_empty);
	fy_foreach(range, recorded) {
		start = fy_number(fy_get_at(range, 0), (size_t)0);
		end = fy_number(fy_get_at(range, 1), (size_t)0);
		if (end <= from || start >= len)
			continue;
		if (start > from && !rc) {
			uidraw_trace(ctx, &d->trace, text + from,
				     start - from);
			rc = fyai_sink_doc_append(ctx->sink, text + from,
						  start - from);
		}
		from = end < len ? end : len;
	}
	if (from < len && !rc) {
		uidraw_trace(ctx, &d->trace, text + from, len - from);
		rc = fyai_sink_doc_append(ctx->sink, text + from, len - from);
	}
	d->live_shown = len;
	uidraw_warn_check(ctx, !rc, "the live document");
}

/* The live document ended as @state says: finalized, aborted or discarded. */
static void uidraw_live_end(struct fyai_ctx *ctx, struct fyai_uidraw *d,
			    const char *state)
{
	const char *op, *text;
	size_t len;
	int rc = 0;

	op = fy_sprintfa("<end %s>", state);
	uidraw_trace(ctx, &d->trace, op, strlen(op));
	if (!strcmp(state, "discarded"))
		fyai_sink_doc_discard(ctx->sink);
	else
		rc = fyai_sink_doc_end(ctx->sink, !strcmp(state, "aborted"));
	uidraw_warn_check(ctx, !rc, "the end of the live document");
	/* A mirror draws a card whole when its document ends: the line was
	 * typed in the process that owns the model. */
	if (fyai_uimodel_mirrored(ctx) && strcmp(d->live_tag, "assistant") &&
	    strcmp(state, "discarded")) {
		text = d->card.data ? d->card.data : "";
		len = d->card.len;
		if (!strcmp(d->live_tag, "user") && d->nechoed &&
		    strlen(d->echoed[0]) == len &&
		    !memcmp(d->echoed[0], text, len)) {
			free(d->echoed[0]);
			memmove(d->echoed, d->echoed + 1,
				--d->nechoed * sizeof(*d->echoed));
		} else {
			(void)fyai_render_display_output(ctx, d->live_tag,
							 text);
		}
	}
	if (d->trace.data || d->oracle.data)
		(void)fyai_uimodel_check(ctx, FYAI_UI_TRANSCRIPT,
			"live document",
			d->trace.data ? d->trace.data : "",
			d->oracle.data ? d->oracle.data : "");
	uidraw_trace_reset(d);
	d->card.len = 0;
	d->live_source = 0;
	d->live_shown = 0;
	d->live_paused = false;
}

/* Follow the members "live" and "last" of the transcript store. */
static void uidraw_transcript(struct fyai_ctx *ctx, struct fyai_uidraw *d,
			      fy_generic root)
{
	fy_generic live = fy_get(root, "live", fy_invalid);
	fy_generic last = fy_get(root, "last", fy_invalid);
	long long source = fy_get(live, "source", 0LL);
	bool paused = fy_get(live, "paused", false);
	const char *op;
	int rc;

	if (d->live_source && d->live_source != source)
		uidraw_live_end(ctx, d, fy_get(last, "state", "discarded"));
	if (!source)
		return;
	if (!d->live_source) {
		d->live_source = source;
		d->live_shown = 0;
		d->live_paused = false;
		/* The legacy path records its begin after this one. */
		uidraw_trace_reset(d);
		snprintf(d->live_tag, sizeof(d->live_tag), "%s",
			 fy_get(live, "tag", "assistant"));
		op = fy_sprintfa("<begin %s>", d->live_tag);
		uidraw_trace(ctx, &d->trace, op, strlen(op));
		rc = fyai_sink_doc_begin(ctx->sink,
					 uidraw_doc_kind(d->live_tag));
		uidraw_warn_check(ctx, !rc, "the live document");
	}
	uidraw_live_feed(ctx, d, live);
	if (paused == d->live_paused)
		return;
	d->live_paused = paused;
	uidraw_trace(ctx, &d->trace, paused ? "<pause>" : "<resume>",
		     paused ? 7 : 8);
	rc = paused ? fyai_sink_doc_pause(ctx->sink) :
		      fyai_sink_doc_resume(ctx->sink);
	uidraw_warn_check(ctx, !rc, "the live document");
}

/*
 * Present a part of an entry of the log as the span gave it to the sink. The
 * span captured every stream as the output of the command, so each part is a
 * notice here.
 */
static int uidraw_log_part(struct fyai_ctx *ctx, fy_generic part)
{
	enum fyai_sink_stream stream = FYAI_SINK_NOTICE;
	fy_generic text;
	const char *str;
	char *item = NULL;
	size_t len = 0;
	FILE *fp = NULL;
	int rc;

	if (fy_equal(fy_get(part, "stream", ""), "drain")) {
		fp = open_memstream(&item, &len);
		fyai_error_check(ctx, fp, err, "cannot format a diagnostic");
		/* The pane shows a diagnostic with no module prefix. */
		fyai_diag_format_item(fp, fy_get(part, "item", fy_invalid),
				      ctx->cfg->diag.source, false);
		rc = fclose(fp);
		rc = rc ? -1 : fyai_sink_write(ctx->sink, FYAI_SINK_NOTICE,
					       item, len);
		free(item);
		return rc;
	}
	text = fy_get(part, "md", fy_invalid);
	if (fy_is_invalid(text)) {
		text = fy_get(part, "text", fy_invalid);
		str = fy_castp(&text, "");
		return fyai_sink_write(ctx->sink, stream, str, strlen(str));
	}
	str = fy_castp(&text, "");
	if (fy_get(part, "source", false))
		return fyai_sink_markdown_source(ctx->sink, stream, str);
	return fyai_sink_markdown(ctx->sink, stream, str);
err:
	return -1;
}

/* Show each new entry of the log store with the display of @ctx. */
static void uidraw_log(struct fyai_ctx *ctx, struct fyai_uidraw *d,
		       fy_generic root)
{
	fy_generic entry, part, entries, parts;
	bool skip = !d->attached && fyai_uimodel_mirrored(ctx);
	char *rows;
	size_t len;
	long long id;
	int rc;

	entries = fy_get(root, "entries", fy_seq_empty);
	fy_foreach(entry, entries) {
		id = fy_get(entry, "id", 0LL);
		if (id <= d->log_shown)
			continue;
		d->log_shown = id;
		if (skip || !fyai_ui_active(ctx))
			continue;
		rc = 0;
		fyai_ui_pane_capture(ctx);
		parts = fy_get(entry, "parts", fy_seq_empty);
		fy_foreach(part, parts) {
			rc = uidraw_log_part(ctx, part);
			if (rc)
				break;
		}
		rows = fyai_ui_pane_captured(ctx, &len);
		uidraw_warn_check(ctx, !rc && rows, "the output of a command");
		if (rows)
			fyai_ui_log_show(ctx, fy_get(entry, "title", ""),
				fy_get(entry, "error", false),
				fy_get(entry, "level", FYAIET_INFO),
				rows, len);
		free(rows);
	}
}

bool fyai_uidraw_bands(struct fyai_ctx *ctx)
{
	return ctx && !ctx->uimodel_detached && fyai_ui_active(ctx) &&
	       !fyai_uimodel_mirrored(ctx);
}

/* The sink calls of this renderer open the bands of the display. */
static bool uidraw_sink_begin(struct fyai_ctx *ctx)
{
	bool saved;

	if (!ctx->sink)
		return false;
	saved = ctx->sink->drawing;
	ctx->sink->drawing = true;
	return saved;
}

static void uidraw_sink_end(struct fyai_ctx *ctx, bool saved)
{
	if (ctx->sink)
		ctx->sink->drawing = saved;
}

static void uidraw_band_free(struct uidraw_band *b)
{
	free(b->title);
	free(b->command);
	free(b->language);
	free(b->viewed);
	free(b);
}

/* Push the source of @b up to @length, the length the store names. */
static int uidraw_band_feed(struct fyai_ctx *ctx, struct uidraw_band *b,
			    size_t length)
{
	const char *text;
	size_t len;
	int rc;

	if (!b->source || !b->stream.active)
		return 0;
	text = fyai_uimodel_text(ctx, b->source, &len);
	if (len > length)
		len = length;
	if (len <= b->shown)
		return 0;
	rc = fyai_fenced_stream_push(&b->stream, text + b->shown,
				     len - b->shown);
	b->shown = len;
	return rc;
}

/*
 * Start the stream of @b for the source that @item names: a quoted stream for
 * Markdown, a highlighted one for code, a plain one for the bytes of a
 * program, with the rows and the indent of the stream of the producer.
 */
static int uidraw_band_stream(struct fyai_ctx *ctx, struct uidraw_band *b,
			      fy_generic item)
{
	const char *format, *language;
	size_t lines;
	int rc;

	format = fy_get(item, "format", "");
	language = fy_get(item, "language", (const char *)NULL);
	lines = ctx->cfg->tool_history_lines > 0 ?
		ctx->cfg->tool_history_lines : 0;
	lines = fy_get(item, "lines", lines);
	fyai_fenced_stream_finish(&b->stream);
	memset(&b->stream, 0, sizeof(b->stream));
	free(b->language);
	b->language = language ? strdup(language) : NULL;
	b->source = fy_get(item, "source", 0LL);
	b->shown = 0;
	rc = !strcmp(format, "markdown") ?
		fyai_markdown_quote_stream_start(&b->stream, ctx, ctx->cfg,
			lines, NULL, true) :
		fyai_fenced_stream_start(&b->stream, ctx, ctx->cfg,
			b->language, lines,
			fy_get(item, "indent", true) ?
			markdown_tool_output_indent(ctx->cfg) : NULL,
			NULL, true);
	if (rc)
		return -1;
	b->stream.title = b->title;
	b->stream.command = b->command;
	fyai_fenced_stream_bind_band(&b->stream, b->band);
	return uidraw_band_feed(ctx, b, fy_get(item, "length", (size_t)0));
}

/* Render the view of a tool call that @item holds, as its producer did. */
static int uidraw_band_view(struct fyai_ctx *ctx, struct uidraw_band *b,
			    fy_generic item)
{
	struct fyai_md_block blocks[64];
	struct fyai_md_blocks list = { .item = blocks };
	struct response_buffer body = {0};
	fy_generic text = fy_get(item, "text", fy_invalid), block, blks;
	const char *str;
	char *title = NULL;
	int rc;

	str = fy_castp(&text, "");

	/* A change of another band repeats this one: render it once. */
	if (b->viewed && !strcmp(b->viewed, str))
		return 0;
	free(b->viewed);
	b->viewed = strdup(str);
	blks = fy_get(item, "blocks", fy_seq_empty);
	fy_foreach(block, blks) {
		if (list.count == ARRAY_SIZE(blocks))
			break;
		blocks[list.count].start = fy_get(block, "start", (size_t)0);
		blocks[list.count].end = fy_get(block, "end", (size_t)0);
		/* Borrowed from the item for the render. */
		blocks[list.count].lang = (char *)fy_get(block, "lang",
							 (const char *)NULL);
		list.count++;
	}
	rc = fyai_tool_view_render(ctx, str, strlen(str), &list,
				   fy_get(item, "preview", (size_t)0),
				   &title, &body);
	if (!rc && body.len)
		fyai_sink_band_paint(b->band, NULL, NULL, body.data, body.len,
				     NULL);
	free(title);
	free(body.data);
	return rc;
}

/* Compare the last paint of @b with the last paint of the shadow band. */
static void uidraw_band_check(struct fyai_ctx *ctx, struct uidraw_band *b)
{
	struct fyai_sink_band *shadow = fyai_uiwork_band(ctx, b->id);
	const char *model, *oracle;
	size_t mlen, olen;

	if (!shadow)
		return;
	model = fyai_sink_band_body(b->band, &mlen);
	oracle = fyai_sink_band_body(shadow, &olen);
	(void)fyai_uimodel_check(ctx, FYAI_UI_WORK, "band", model, oracle);
}

/* End @b as the band of its producer ended. */
static void uidraw_band_end(struct fyai_ctx *ctx, struct uidraw_band *b,
			    fy_generic item)
{
	const char *state = fy_get(item, "state", "destroyed");

	fyai_fenced_stream_finish(&b->stream);
	uidraw_band_check(ctx, b);
	if (!strcmp(state, "closed"))
		fyai_sink_band_close(ctx->sink, fy_get(item, "ok", true),
				     fy_get(item, "cause", (const char *)NULL));
	else if (!strcmp(state, "committed"))
		fyai_sink_band_commit(b->band);
	else if (!b->shared)
		fyai_sink_band_destroy(b->band);
}

/* Keep @slot as @text, "" for none; true when it changed. */
static int uidraw_band_name(struct fyai_ctx *ctx, char **slot,
			    const char *text, bool *changed)
{
	char *copy;

	if (!strcmp(*slot ? *slot : "", text))
		return 0;
	copy = *text ? strdup(text) : NULL;
	fyai_error_check(ctx, copy || !*text, err, "cannot keep a band");
	free(*slot);
	*slot = copy;
	*changed = true;
	return 0;
err:
	return -1;
}

/* Take the title and the command of @b from the store. A stream that runs
 * draws its head again. */
static int uidraw_band_names(struct fyai_ctx *ctx, struct uidraw_band *b,
			     const char *title, const char *command)
{
	bool changed = false;

	if (uidraw_band_name(ctx, &b->title, title, &changed) ||
	    uidraw_band_name(ctx, &b->command, command, &changed))
		return -1;
	if (!changed)
		return 0;
	b->stream.title = b->title;
	b->stream.command = b->command;
	if (b->stream.active)
		fyai_fenced_stream_repaint(&b->stream);
	return 0;
}

/* Draw the member "bands" of the work store with bands of the display. */
static int uidraw_bands_draw(struct fyai_ctx *ctx, struct fyai_uidraw *d,
			     fy_generic root)
{
	struct uidraw_band **bp, *b;
	fy_generic bands, item;
	const char *title, *command, *format;
	bool found;
	int rc;

	bands = fy_get(root, "bands", fy_seq_empty);
	/* A band that left without a state is dropped. */
	bp = &d->bands;
	while ((b = *bp)) {
		found = false;
		fy_foreach(item, bands)
			found = found || fy_get(item, "id", 0LL) == b->id;
		if (found) {
			bp = &b->next;
			continue;
		}
		*bp = b->next;
		fyai_fenced_stream_finish(&b->stream);
		if (!b->shared)
			fyai_sink_band_destroy(b->band);
		uidraw_band_free(b);
	}
	fy_foreach(item, bands) {
		for (b = d->bands; b && b->id != fy_get(item, "id", 0LL);
		     b = b->next)
			;
		title = fy_get(item, "title", "");
		command = fy_get(item, "command", "");
		format = fy_get(item, "format", "rows");
		if (!b && fy_is_valid(fy_get(item, "state", fy_invalid)))
			continue;
		if (!b) {
			b = calloc(1, sizeof(*b));
			fyai_error_check(ctx, b, err, "cannot keep a band");
			b->id = fy_get(item, "id", 0LL);
			b->shared = fy_get(item, "shared", false);
			b->title = *title ? strdup(title) : NULL;
			b->command = *command ? strdup(command) : NULL;
			b->band = fyai_sink_band_open(ctx->sink, b->shared,
						      b->title, b->command);
			if (!b->band) {
				uidraw_band_free(b);
				continue;
			}
			b->next = d->bands;
			d->bands = b;
		}
		/* A producer names the call after the band opens. */
		if (uidraw_band_names(ctx, b, title, command))
			return -1;
		if (fy_is_valid(fy_get(item, "state", fy_invalid))) {
			for (bp = &d->bands; *bp != b; bp = &(*bp)->next)
				;
			*bp = b->next;
			uidraw_band_end(ctx, b, item);
			uidraw_band_free(b);
			continue;
		}
		if (!strcmp(format, "tool-view")) {
			if (uidraw_band_view(ctx, b, item))
				return -1;
			continue;
		}
		if (strcmp(format, "rows")) {
			rc = b->source != fy_get(item, "source", 0LL) ?
				uidraw_band_stream(ctx, b, item) :
				uidraw_band_feed(ctx, b,
					fy_get(item, "length", (size_t)0));
			if (rc)
				return -1;
			continue;
		}
		/* No title is NULL, as a producer gives it. */
		fyai_sink_band_paint(b->band, b->title, b->command,
			fy_get(item, "text", ""),
			strlen(fy_get(item, "text", "")), NULL);
	}
	return 0;
err:
	return -1;
}

static void uidraw_work(struct fyai_ctx *ctx, struct fyai_uidraw *d,
			fy_generic root)
{
	bool saved;
	int rc;

	if (!fyai_ui_active(ctx) ||
	    (!fyai_uidraw_bands(ctx) && !fyai_uimodel_mirrored(ctx)))
		return;
	saved = uidraw_sink_begin(ctx);
	rc = uidraw_bands_draw(ctx, d, root);
	uidraw_sink_end(ctx, saved);
	uidraw_warn_check(ctx, !rc, "the bands of the work pane");
}

int fyai_uidraw_band_cols(struct fyai_ctx *ctx,
			  const struct fyai_sink_band *shadow)
{
	struct fyai_uidraw *d = ctx ? ctx->uidraw : NULL;
	long long id = fyai_uiwork_band_id(ctx, shadow);
	struct uidraw_band *b;

	for (b = d ? d->bands : NULL; b; b = b->next)
		if (b->id == id)
			return fyai_sink_band_cols(b->band);
	return 0;
}

void fyai_uidraw_bands_release(struct fyai_ctx *ctx)
{
	struct fyai_uidraw *d = ctx ? ctx->uidraw : NULL;
	struct uidraw_band *b;
	bool saved;

	if (!d || !d->bands)
		return;
	saved = uidraw_sink_begin(ctx);
	while ((b = d->bands)) {
		d->bands = b->next;
		fyai_fenced_stream_finish(&b->stream);
		if (!b->shared)
			fyai_sink_band_destroy(b->band);
		uidraw_band_free(b);
	}
	uidraw_sink_end(ctx, saved);
}

void fyai_uidraw_store(struct fyai_ctx *ctx, enum fyai_ui_store store,
		       fy_generic root)
{
	struct fyai_uidraw *d = uidraw(ctx);

	if (!d)
		return;
	switch (store) {
	case FYAI_UI_TRANSCRIPT:
		uidraw_transcript(ctx, d, root);
		break;
	case FYAI_UI_LOG:
		uidraw_log(ctx, d, root);
		break;
	case FYAI_UI_WORK:
		uidraw_work(ctx, d, root);
		break;
	default:
		break;
	}
}

void fyai_uidraw_attached(struct fyai_ctx *ctx)
{
	struct fyai_uidraw *d = uidraw(ctx);

	if (d)
		d->attached = true;
}

int fyai_uidraw_echoed(struct fyai_ctx *ctx, const char *line)
{
	struct fyai_uidraw *d = uidraw(ctx);
	char **v;

	if (!d)
		return -1;
	v = realloc(d->echoed, (d->nechoed + 1) * sizeof(*v));
	fyai_error_check(ctx, v, err, "cannot keep the line that was echoed");
	d->echoed = v;
	d->echoed[d->nechoed] = strdup(line);
	fyai_error_check(ctx, d->echoed[d->nechoed], err,
			 "cannot keep the line that was echoed");
	d->nechoed++;
	return 0;
err:
	return -1;
}

void fyai_uidraw_finish(struct fyai_ctx *ctx)
{
	struct fyai_uidraw *d = ctx ? ctx->uidraw : NULL;

	if (d && d->live_source)
		uidraw_live_end(ctx, d, "aborted");
	fyai_uidraw_bands_release(ctx);
}

void fyai_uidraw_release(struct fyai_ctx *ctx)
{
	struct fyai_uidraw *d = ctx ? ctx->uidraw : NULL;

	if (!d)
		return;
	fyai_uidraw_bands_release(ctx);
	while (d->nechoed)
		free(d->echoed[--d->nechoed]);
	free(d->echoed);
	free(d->card.data);
	free(d->trace.data);
	free(d->oracle.data);
	free(d);
	ctx->uidraw = NULL;
}

void fyai_uidraw_abandon(struct fyai_ctx *ctx)
{
	struct fyai_uidraw *d = ctx ? ctx->uidraw : NULL;
	struct uidraw_band *b;

	/* The bands are the display of the parent: forget them. */
	while (d && (b = d->bands)) {
		d->bands = b->next;
		uidraw_band_free(b);
	}
	fyai_uidraw_release(ctx);
}
