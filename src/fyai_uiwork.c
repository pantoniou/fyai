/* SPDX-License-Identifier: MIT */
#define FYAI_MODULE FYAIEM_DISPLAY

#include <stdlib.h>
#include <string.h>

#include "fyai.h"
#include "fyai_uimodel.h"
#include "fyai_uiwork.h"

/*
 * One band of the sink. @band identifies it in this process only, while the
 * sink holds it; the store names it by @id.
 */
struct uiwork_band {
	struct uiwork_band *next;
	const struct fyai_sink_band *band;
	long long id;
	bool shared;
	char *title, *command;
	char *rows;			/* a body painted in its final form */
	const char *format;		/* a static string */
	char *language;
	size_t lines;			/* the rows a stream keeps */
	bool indent;			/* under the indent of tool output */
	long long source;		/* the text of the model, or 0 */
	const void *owner;		/* the fenced stream that writes it */
	/* The source of a tool view, {text, blocks, preview}, in @view_gb. */
	struct fy_generic_builder *view_gb;
	fy_generic view;
	/* How it ended: committed, destroyed or closed, with its outcome. */
	const char *state;		/* a static string, or NULL */
	bool ok;
	char *cause;
};

struct fyai_uiwork {
	struct uiwork_band *bands;	/* oldest first */
	long long next_id;
};

static struct fyai_uiwork *uiwork(struct fyai_ctx *ctx)
{
	if (!ctx || ctx->uimodel_detached)
		return NULL;
	if (!ctx->uiwork) {
		ctx->uiwork = calloc(1, sizeof(*ctx->uiwork));
		fyai_error_check(ctx, ctx->uiwork, err,
				 "cannot keep the bands of the display");
	}
	return ctx->uiwork;
err:
	return NULL;
}

static struct uiwork_band **uiwork_find(struct fyai_uiwork *w,
					const struct fyai_sink_band *band)
{
	struct uiwork_band **bp;

	for (bp = &w->bands; *bp; bp = &(*bp)->next)
		if (band ? (*bp)->band == band : (*bp)->shared)
			return bp;
	return NULL;
}

/* Keep a copy of @text in @slot; NULL keeps what is there. */
static int uiwork_keep(struct fyai_ctx *ctx, char **slot, const char *text,
		       size_t len)
{
	char *copy;

	if (!text)
		return 0;
	copy = strndup(text, len);
	fyai_error_check(ctx, copy, err, "cannot keep a band of the display");
	free(*slot);
	*slot = copy;
	return 0;
err:
	return -1;
}

static void uiwork_free(struct fyai_ctx *ctx, struct uiwork_band *b)
{
	if (b->source)
		fyai_uimodel_text_close(ctx, b->source);
	free(b->title);
	free(b->command);
	free(b->rows);
	free(b->language);
	free(b->cause);
	fy_generic_builder_destroy(b->view_gb);
	free(b);
}

/* Write the member "bands" of the work store from the bands that live. */
static void uiwork_publish(struct fyai_ctx *ctx)
{
	struct fy_generic_builder *gb = fyai_uimodel_begin(ctx);
	struct fyai_uiwork *w = ctx->uiwork;
	struct uiwork_band *b;
	fy_generic bands = fy_seq_empty, item;
	size_t len;

	if (gb && w) {
		for (b = w->bands; b; b = b->next) {
			item = fy_mapping(gb, "id", b->id, "shared", b->shared,
				"title", b->title ? b->title : "",
				"command", b->command ? b->command : "",
				"format", b->format);
			if (b->view_gb) {
				item = fy_assoc(gb, item,
					"text", fy_get(b->view, "text", ""),
					"blocks", fy_get(b->view, "blocks",
							 fy_seq_empty),
					"preview", fy_get(b->view, "preview",
							  0LL));
			} else if (b->source) {
				(void)fyai_uimodel_text(ctx, b->source, &len);
				item = fy_assoc(gb, item, "source", b->source,
						"length", (long long)len);
				if (b->language)
					item = fy_assoc(gb, item, "language",
						fy_value(gb, b->language));
				item = fy_assoc(gb, item, "lines",
						(long long)b->lines,
						"indent", b->indent);
			} else if (b->rows) {
				item = fy_assoc(gb, item, "text",
						fy_value(gb, b->rows));
			}
			if (b->state)
				item = fy_assoc(gb, item, "state", b->state,
					"ok", b->ok, "cause",
					b->cause ? fy_value(gb, b->cause) :
						   fy_null);
			bands = fy_append(gb, bands, item);
		}
		(void)fyai_uimodel_set_member(ctx, FYAI_UI_WORK, "bands", bands);
	}
	fyai_uimodel_end(ctx);
}

void fyai_uiwork_band_open(struct fyai_ctx *ctx, struct fyai_sink_band *band,
			   bool shared, const char *title, const char *command)
{
	struct fyai_uiwork *w = uiwork(ctx);
	struct uiwork_band *b, **bp;

	if (!w || !band)
		return;
	/* One shared band at a time: the sink replaced the one before. */
	if (shared && (bp = uiwork_find(w, NULL))) {
		b = *bp;
		*bp = b->next;
		uiwork_free(ctx, b);
	}
	b = calloc(1, sizeof(*b));
	fyai_error_check(ctx, b, err, "cannot keep a band of the display");
	b->band = band;
	b->id = ++w->next_id;
	b->shared = shared;
	b->format = "rows";
	if ((title && uiwork_keep(ctx, &b->title, title, strlen(title))) ||
	    (command && uiwork_keep(ctx, &b->command, command,
				    strlen(command)))) {
		uiwork_free(ctx, b);
		return;
	}
	for (bp = &w->bands; *bp; bp = &(*bp)->next)
		;
	*bp = b;
	uiwork_publish(ctx);
	return;
err:
	return;
}

void fyai_uiwork_band_paint(struct fyai_ctx *ctx, struct fyai_sink_band *band,
			    const char *title, const char *command,
			    const char *body, size_t len)
{
	struct fyai_uiwork *w = ctx ? ctx->uiwork : NULL;
	struct uiwork_band **bp, *b;

	if (!w || !band || !(bp = uiwork_find(w, band)))
		return;
	b = *bp;
	if ((title && uiwork_keep(ctx, &b->title, title, strlen(title))) ||
	    (command && uiwork_keep(ctx, &b->command, command,
				    strlen(command))))
		return;
	/* A fenced stream paints what it rendered from its source. */
	if (!b->source && !b->view_gb && body && len &&
	    uiwork_keep(ctx, &b->rows, body, len))
		return;
	uiwork_publish(ctx);
}

void fyai_uiwork_band_source(struct fyai_ctx *ctx, struct fyai_sink_band *band,
			     const void *owner, const char *format,
			     const char *language, size_t lines, bool indent,
			     const char *data, size_t len)
{
	struct fyai_uiwork *w = ctx ? ctx->uiwork : NULL;
	struct uiwork_band **bp, *b;

	if (!w || !band || !(bp = uiwork_find(w, band)))
		return;
	b = *bp;
	if (b->owner != owner) {
		if (b->source)
			fyai_uimodel_text_close(ctx, b->source);
		b->source = fyai_uimodel_text_open(ctx);
		if (!b->source)
			return;
		b->owner = owner;
		b->format = format;
		b->lines = lines;
		b->indent = indent;
		free(b->rows);
		b->rows = NULL;
		free(b->language);
		b->language = NULL;
		if (language &&
		    uiwork_keep(ctx, &b->language, language, strlen(language)))
			return;
	}
	if (len && fyai_uimodel_text_append(ctx, b->source, data, len))
		return;
	uiwork_publish(ctx);
}

void fyai_uiwork_band_view(struct fyai_ctx *ctx, struct fyai_sink_band *band,
			   fy_generic view)
{
	struct fyai_uiwork *w = ctx ? ctx->uiwork : NULL;
	struct uiwork_band **bp, *b;
	struct fy_generic_builder *gb;

	if (!w || !band || !fy_is_mapping(view) ||
	    !(bp = uiwork_find(w, band)))
		return;
	b = *bp;
	gb = fy_generic_builder_create(NULL);
	fyai_error_check(ctx, gb, err, "cannot keep the view of a band");
	fy_generic_builder_destroy(b->view_gb);
	b->view_gb = gb;
	b->view = fy_gb_internalize(gb, view);
	b->format = "tool-view";
	free(b->rows);
	b->rows = NULL;
	uiwork_publish(ctx);
	return;
err:
	return;
}

void fyai_uiwork_band_end(struct fyai_ctx *ctx, struct fyai_sink_band *band,
			  const char *state, bool ok, const char *cause)
{
	struct fyai_uiwork *w = ctx ? ctx->uiwork : NULL;
	struct uiwork_band **bp, *b;

	if (!w || !(bp = uiwork_find(w, band)))
		return;
	b = *bp;
	/* A consumer sees how the band ended before it goes. */
	b->state = state;
	b->ok = ok;
	if (cause)
		(void)uiwork_keep(ctx, &b->cause, cause, strlen(cause));
	uiwork_publish(ctx);
	*bp = b->next;
	uiwork_free(ctx, b);
	uiwork_publish(ctx);
}

struct fyai_sink_band *fyai_uiwork_band(struct fyai_ctx *ctx, long long id)
{
	struct fyai_uiwork *w = ctx ? ctx->uiwork : NULL;
	struct uiwork_band *b;

	for (b = w ? w->bands : NULL; b; b = b->next)
		if (b->id == id)
			return (struct fyai_sink_band *)b->band;
	return NULL;
}

long long fyai_uiwork_band_id(struct fyai_ctx *ctx,
			      const struct fyai_sink_band *band)
{
	struct fyai_uiwork *w = ctx ? ctx->uiwork : NULL;
	struct uiwork_band *b;

	for (b = w ? w->bands : NULL; b; b = b->next)
		if (b->band == band)
			return b->id;
	return 0;
}

void fyai_uiwork_release(struct fyai_ctx *ctx)
{
	struct fyai_uiwork *w = ctx ? ctx->uiwork : NULL;
	struct uiwork_band *b;

	if (!w)
		return;
	while ((b = w->bands)) {
		w->bands = b->next;
		uiwork_free(ctx, b);
	}
	free(w);
	ctx->uiwork = NULL;
}
