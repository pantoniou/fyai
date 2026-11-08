/* SPDX-License-Identifier: MIT */
#define FYAI_MODULE FYAIEM_DISPLAY

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai.h"
#include "fyai_uimodel.h"
#include "fyai_uitranscript.h"

/* The live document. Each begin resets @gb, which holds its fragments. */
struct fyai_uitranscript {
	struct fy_generic_builder *gb;
	fy_generic fragments;
	fy_generic recorded;	/* the closed recorded ranges */
	char tag[16];
	long long source;	/* the text of the model, or 0 */
	size_t length;
	/* The last recorded range, open while appends continue it. */
	size_t open_start, open_end;
	bool open;
	bool paused;
};

static struct fyai_uitranscript *uitranscript(struct fyai_ctx *ctx)
{
	if (!ctx || ctx->uimodel_detached)
		return NULL;
	if (!ctx->uitranscript) {
		ctx->uitranscript = calloc(1, sizeof(*ctx->uitranscript));
		fyai_error_check(ctx, ctx->uitranscript, err,
				 "cannot keep the live document of the display");
	}
	return ctx->uitranscript;
err:
	return NULL;
}

/* Write the member "live" of the transcript store. */
static void uitranscript_publish(struct fyai_ctx *ctx)
{
	struct fyai_uitranscript *t = ctx->uitranscript;
	struct fy_generic_builder *gb = fyai_uimodel_begin(ctx);
	fy_generic recorded;

	if (gb && t && t->source) {
		recorded = t->open ? fy_append(gb, t->recorded,
			fy_sequence(gb, (long long)t->open_start,
				    (long long)t->open_end)) : t->recorded;
		(void)fyai_uimodel_set_member(ctx, FYAI_UI_TRANSCRIPT, "live",
			fy_mapping(gb, "tag", t->tag, "source", t->source,
				   "length", (long long)t->length,
				   "fragments", t->fragments,
				   "recorded", recorded, "paused", t->paused));
	} else if (gb) {
		(void)fyai_uimodel_set_member(ctx, FYAI_UI_TRANSCRIPT, "live",
					      fy_null);
	}
	fyai_uimodel_end(ctx);
}

void fyai_uitranscript_begin(struct fyai_ctx *ctx, const char *tag)
{
	struct fyai_uitranscript *t = uitranscript(ctx);

	if (!t)
		return;
	if (t->source)
		fyai_uitranscript_end(ctx, "discarded");
	if (!t->gb) {
		t->gb = fyai_uimodel_builder_new(ctx);
		fyai_error_check(ctx, t->gb, err, "cannot make the builder of "
				 "the live document of the display");
	} else {
		fy_generic_builder_reset(t->gb);
	}
	t->source = fyai_uimodel_text_open(ctx);
	if (!t->source)
		return;
	snprintf(t->tag, sizeof(t->tag), "%s", tag);
	t->length = 0;
	t->open = false;
	t->paused = false;
	t->fragments = fy_seq_empty;
	t->recorded = fy_seq_empty;
	uitranscript_publish(ctx);
	return;
err:
	return;
}

void fyai_uitranscript_append(struct fyai_ctx *ctx, const char *text,
			      size_t len, bool present)
{
	struct fyai_uitranscript *t = ctx ? ctx->uitranscript : NULL;
	fy_generic recorded;

	if (!t || !t->source || !len)
		return;
	if (fyai_uimodel_text_append(ctx, t->source, text, len))
		return;
	if (!present && t->open && t->open_end == t->length) {
		t->open_end += len;
	} else if (!present) {
		/* A range that does not continue the last one closes it. */
		if (t->open) {
			recorded = fy_append(t->gb, t->recorded,
				fy_sequence(t->gb, (long long)t->open_start,
					    (long long)t->open_end));
			if (fy_is_valid(recorded))
				t->recorded = recorded;
		}
		t->open = true;
		t->open_start = t->length;
		t->open_end = t->length + len;
	}
	t->length += len;
	uitranscript_publish(ctx);
}

void fyai_uitranscript_pause(struct fyai_ctx *ctx, bool paused)
{
	struct fyai_uitranscript *t = ctx ? ctx->uitranscript : NULL;

	if (!t || !t->source || t->paused == paused)
		return;
	t->paused = paused;
	uitranscript_publish(ctx);
}

void fyai_uitranscript_fragment(struct fyai_ctx *ctx, fy_generic fragment)
{
	struct fyai_uitranscript *t = ctx ? ctx->uitranscript : NULL;
	fy_generic fragments;

	if (!t || !t->source)
		return;
	/* The fragment can live on the stack of the caller. */
	fragments = fy_append(t->gb, t->fragments,
			      fy_gb_internalize(t->gb, fragment));
	if (fy_is_invalid(fragments))
		return;
	t->fragments = fragments;
	uitranscript_publish(ctx);
}

void fyai_uitranscript_end(struct fyai_ctx *ctx, const char *state)
{
	struct fyai_uitranscript *t = ctx ? ctx->uitranscript : NULL;
	struct fy_generic_builder *gb;

	if (!t || !t->source)
		return;
	fyai_uimodel_text_close(ctx, t->source);
	t->source = 0;
	gb = fyai_uimodel_begin(ctx);
	if (gb)
		(void)fyai_uimodel_set_member(ctx, FYAI_UI_TRANSCRIPT, "last",
			fy_mapping(gb, "tag", t->tag, "state", state));
	fyai_uimodel_end(ctx);
	uitranscript_publish(ctx);
}

void fyai_uitranscript_release(struct fyai_ctx *ctx)
{
	struct fyai_uitranscript *t = ctx ? ctx->uitranscript : NULL;

	if (!t)
		return;
	if (t->source)
		fyai_uimodel_text_close(ctx, t->source);
	fy_generic_builder_destroy(t->gb);
	free(t);
	ctx->uitranscript = NULL;
}
