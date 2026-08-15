/*
 * fyai_sink.c - the one rendering component and its backends
 *
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_DISPLAY

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai.h"
#include "fyai_markdown.h"
#include "fyai_sink.h"
#include "fyai_terminal.h"
#include "fyai_ui.h"

/* The only progressive Markdown renderer in the process. */
struct sink_term {
	struct markdown_renderer renderer;
	struct response_buffer pending;	/* appended, not yet pushed */
	size_t active_rows;		/* rows the live region occupies */
	enum fyai_sink_doc_kind kind;
	bool render_live;
	bool doc_open;
};

static struct sink_term *sink_term_state(const struct fyai_sink *s)
{
	return s ? s->state : NULL;
}

static bool sink_term_wants_live(const struct fyai_sink *s,
				 enum fyai_sink_doc_kind kind)
{
	struct fyai_ctx *ctx = s->ctx;

	return kind == FYAI_SINK_DOC_ASSISTANT && ctx->cfg->markdown &&
	       ctx->stdout_tty && markdown_available(ctx->cfg);
}

static int sink_term_renderer_start(struct fyai_sink *s)
{
	struct sink_term *t = sink_term_state(s);
	struct fyai_ctx *ctx = s->ctx;
	int rc;

	if (!sink_term_wants_live(s, t->kind))
		return 0;
	rc = markdown_renderer_start(ctx->cfg, &t->renderer,
			markdown_color_enabled(ctx->cfg->color),
			ctx->cfg->theme_variant);
	fyai_error_check(ctx, !rc, err,
		"could not start the display renderer");
	t->render_live = true;
	return 0;
err:
	return -1;
}

/* Replace the mutable rows and retain the rows that the renderer freezes. */
static void sink_term_apply(struct fyai_sink *s,
			    const struct markdown_update *update)
{
	struct sink_term *t = sink_term_state(s);
	size_t backtrack = update->backtrack;

	if (fyai_ui_active(s->ctx)) {
		(void)fyai_ui_tail_apply(s->ctx, update);
		return;
	}
	if (backtrack) {
		fprintf(stdout, FYAI_ANSI_CURSOR_UP_FMT, backtrack);
		fputs(FYAI_ANSI_ERASE_DOWN, stdout);
		t->active_rows -= backtrack;
	}
	if (update->content_len)
		fwrite(update->content, 1, update->content_len, stdout);
	t->active_rows += fyai_count_newlines(update->content,
					      update->content_len);
	if (update->freeze >= t->active_rows)
		t->active_rows = 0;
	else
		t->active_rows -= update->freeze;
	fflush(stdout);
}

static int sink_term_push_pending(struct fyai_sink *s)
{
	struct sink_term *t = sink_term_state(s);
	struct markdown_update update;

	if (!t->render_live || !t->pending.len)
		return 0;
	if (markdown_renderer_push(&t->renderer, t->pending.data,
				   t->pending.len, &update))
		return -1;
	sink_term_apply(s, &update);
	t->pending.len = 0;
	t->pending.data[0] = '\0';
	return 0;
}

/* Replace the progressive region with the healed final rendering. */
static int sink_term_render_finish(struct fyai_sink *s)
{
	struct sink_term *t = sink_term_state(s);
	struct response_buffer rendered = {0};
	size_t end;
	bool line_start;

	if (!t->render_live)
		return 0;
	if (sink_term_push_pending(s) ||
	    markdown_renderer_finish(&t->renderer, &rendered)) {
		free(rendered.data);
		return -1;
	}
	end = terminal_trim_blank_rows(rendered.data, rendered.len);
	line_start = terminal_text_at_line_start(rendered.data, end);
	if (fyai_ui_active(s->ctx)) {
		fyai_ui_tail_finish(s->ctx, rendered.data, end);
		if (!line_start)
			(void)fyai_ui_commit(s->ctx, "\n", 1);
	} else {
		if (t->active_rows) {
			fprintf(stdout, FYAI_ANSI_CURSOR_UP_FMT, t->active_rows);
			fputs(FYAI_ANSI_ERASE_DOWN, stdout);
		}
		if (end)
			fwrite(rendered.data, 1, end, stdout);
		if (!line_start)
			fputc('\n', stdout);
		fflush(stdout);
	}
	t->active_rows = 0;
	t->render_live = false;
	free(rendered.data);
	return 0;
}

static int sink_term_doc_begin(struct fyai_sink *s,
			       const struct fyai_sink_doc *doc)
{
	struct sink_term *t = sink_term_state(s);

	t->kind = doc->kind;
	t->active_rows = 0;
	t->pending.len = 0;
	if (t->pending.data)
		t->pending.data[0] = '\0';
	t->doc_open = true;
	return sink_term_renderer_start(s);
}

static int sink_term_doc_append(struct fyai_sink *s, const char *text,
				size_t len)
{
	struct sink_term *t = sink_term_state(s);
	struct fyai_ctx *ctx = s->ctx;
	int rc;
	if (!t->render_live || !len)
		return 0;
	rc = response_buffer_reserve(&t->pending, t->pending.len + len + 1);
	fyai_error_check(ctx, !rc,
		err, "could not grow the progressive display buffer");
	memcpy(t->pending.data + t->pending.len, text, len);
	t->pending.len += len;
	t->pending.data[t->pending.len] = '\0';
	rc = sink_term_push_pending(s);
	fyai_error_check(ctx, !rc, err,
			 "could not render the display output");
	return 0;
err:
	return -1;
}

static int sink_term_doc_end(struct fyai_sink *s, bool aborted)
{
	struct sink_term *t = sink_term_state(s);
	int rc;

	(void)aborted;
	rc = sink_term_render_finish(s);
	markdown_renderer_destroy(&t->renderer);
	t->doc_open = false;
	return rc;
}

/*
 * Drop the document. The renderer is released without asking it for the healed
 * final form, so nothing further reaches the display; whatever the progressive
 * repaint already drew stays on screen as the record of what happened.
 */
static void sink_term_doc_discard(struct fyai_sink *s)
{
	struct sink_term *t = sink_term_state(s);

	if (!t)
		return;
	markdown_renderer_destroy(&t->renderer);
	t->pending.len = 0;
	t->active_rows = 0;
	t->render_live = false;
	t->doc_open = false;
}

static int sink_term_doc_pause(struct fyai_sink *s)
{
	struct sink_term *t = sink_term_state(s);
	struct fyai_ctx *ctx = s->ctx;
	int rc;

	if (!t->render_live)
		return 0;
	rc = sink_term_render_finish(s);
	fyai_error_check(ctx, !rc, err,
			 "could not checkpoint the display output");
	markdown_renderer_destroy(&t->renderer);
	return 0;
err:
	return -1;
}

static int sink_term_doc_resume(struct fyai_sink *s)
{
	struct sink_term *t = sink_term_state(s);
	struct fyai_ctx *ctx = s->ctx;
	int rc;

	if (!t->doc_open || t->render_live)
		return 0;
	rc = sink_term_renderer_start(s);
	fyai_error_check(ctx, !rc, err,
			 "could not resume the display output");
	return 0;
err:
	return -1;
}

static bool sink_term_doc_is_live(const struct fyai_sink *s)
{
	const struct sink_term *t = sink_term_state(s);

	return t && t->render_live;
}

static void sink_term_destroy(struct fyai_sink *s)
{
	struct sink_term *t = sink_term_state(s);

	if (!t)
		return;
	markdown_renderer_destroy(&t->renderer);
	free(t->pending.data);
	free(t);
	s->state = NULL;
}

static const struct fyai_sink_ops sink_terminal_ops = {
	.name		= "terminal",
	.doc_begin	= sink_term_doc_begin,
	.doc_append	= sink_term_doc_append,
	.doc_end	= sink_term_doc_end,
	.doc_discard	= sink_term_doc_discard,
	.doc_pause	= sink_term_doc_pause,
	.doc_resume	= sink_term_doc_resume,
	.doc_is_live	= sink_term_doc_is_live,
	.destroy	= sink_term_destroy,
};

struct fyai_sink *fyai_sink_create(struct fyai_ctx *ctx)
{
	struct fyai_sink *s;
	struct sink_term *t;

	if (!ctx)
		return NULL;
	s = calloc(1, sizeof(*s));
	if (!s)
		return NULL;
	t = calloc(1, sizeof(*t));
	if (!t) {
		free(s);
		return NULL;
	}
	s->ctx = ctx;
	s->ops = &sink_terminal_ops;
	s->state = t;
	return s;
}

void fyai_sink_destroy(struct fyai_sink *s)
{
	if (!s)
		return;
	if (s->ops->destroy)
		s->ops->destroy(s);
	free(s);
}

int fyai_sink_doc_begin(struct fyai_sink *s, enum fyai_sink_doc_kind kind)
{
	struct fyai_sink_doc doc;

	if (!s || !s->ops->doc_begin)
		return 0;
	memset(&doc, 0, sizeof(doc));
	doc.kind = kind;
	return s->ops->doc_begin(s, &doc);
}

int fyai_sink_doc_append(struct fyai_sink *s, const char *text, size_t len)
{
	if (!s || !s->ops->doc_append)
		return 0;
	return s->ops->doc_append(s, text, len);
}

int fyai_sink_doc_end(struct fyai_sink *s, bool aborted)
{
	if (!s || !s->ops->doc_end)
		return 0;
	return s->ops->doc_end(s, aborted);
}

void fyai_sink_doc_discard(struct fyai_sink *s)
{
	if (s && s->ops->doc_discard)
		s->ops->doc_discard(s);
}

int fyai_sink_doc_pause(struct fyai_sink *s)
{
	if (!s || !s->ops->doc_pause)
		return 0;
	return s->ops->doc_pause(s);
}

int fyai_sink_doc_resume(struct fyai_sink *s)
{
	if (!s || !s->ops->doc_resume)
		return 0;
	return s->ops->doc_resume(s);
}

bool fyai_sink_doc_is_live(const struct fyai_sink *s)
{
	if (!s || !s->ops->doc_is_live)
		return false;
	return s->ops->doc_is_live(s);
}
