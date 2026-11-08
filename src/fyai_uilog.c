/* SPDX-License-Identifier: MIT */
#define FYAI_MODULE FYAIEM_DISPLAY

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai.h"
#include "fyai_uilog.h"
#include "fyai_uimodel.h"

/* The span of a command that records its output. */
struct fyai_uilog {
	/* The parts of the open entry. Each begin resets it. */
	struct fy_generic_builder *gb;
	fy_generic parts;
	enum fyai_error_type level;
	long long next_id;
	bool open;
};

static const char *uilog_stream_name(enum fyai_sink_stream stream)
{
	switch (stream) {
	case FYAI_SINK_TRANSCRIPT:
		return "transcript";
	case FYAI_SINK_NOTICE:
		return "notice";
	case FYAI_SINK_STATUS:
		return "status";
	case FYAI_SINK_DIAG:
		return "diag";
	case FYAI_SINK_MACHINE:
		return "machine";
	}
	return "notice";
}

static void uilog_append(struct fyai_uilog *l, fy_generic part)
{
	fy_generic parts;

	if (fy_is_invalid(part))
		return;
	parts = fy_append(l->gb, l->parts, part);
	if (fy_is_valid(parts))
		l->parts = parts;
}

/* A record that the drain reports while the span is open. */
static void uilog_diag(void *user, fy_generic item)
{
	struct fyai_ctx *ctx = user;
	struct fyai_uilog *l = ctx->uilog;
	enum fyai_error_type type;

	if (!l || !l->open)
		return;
	type = (enum fyai_error_type)fy_get(item, "type", 0LL);
	if (type > l->level)
		l->level = type;
	uilog_append(l, fy_mapping(l->gb, "stream", "drain",
				   "item", fy_gb_internalize(l->gb, item)));
}

void fyai_uilog_begin(struct fyai_ctx *ctx)
{
	struct fyai_uilog *l;

	if (!ctx || ctx->uimodel_detached)
		return;
	l = ctx->uilog;
	if (!l) {
		l = calloc(1, sizeof(*l));
		fyai_error_check(ctx, l, err, "cannot keep the log of the session");
		ctx->uilog = l;
	}
	if (!l->gb) {
		l->gb = fyai_uimodel_builder_new(ctx);
		fyai_error_check(ctx, l->gb, err,
				 "cannot make the builder of the session log");
	} else {
		fy_generic_builder_reset(l->gb);
	}
	l->parts = fy_seq_empty;
	l->level = FYAIET_INFO;
	l->open = true;
	fyai_diag_drain_tap(uilog_diag, ctx);
	return;
err:
	return;
}

void fyai_uilog_sink(struct fyai_ctx *ctx, enum fyai_sink_stream stream,
		     bool markdown, const char *text, size_t len)
{
	struct fyai_uilog *l = ctx ? ctx->uilog : NULL;

	if (!l || !l->open)
		return;
	uilog_append(l, fy_mapping(l->gb, "stream", uilog_stream_name(stream),
		markdown ? "md" : "text",
		fy_gb_string_size_create(l->gb, text, len)));
}

void fyai_uilog_sink_source(struct fyai_ctx *ctx, enum fyai_sink_stream stream,
			    const char *md, size_t len)
{
	struct fyai_uilog *l = ctx ? ctx->uilog : NULL;

	if (!l || !l->open)
		return;
	uilog_append(l, fy_mapping(l->gb, "stream", uilog_stream_name(stream),
		"md", fy_gb_string_size_create(l->gb, md, len),
		"source", true));
}

void fyai_uilog_end(struct fyai_ctx *ctx, const char *title, bool error)
{
	struct fyai_uilog *l = ctx ? ctx->uilog : NULL;
	struct fy_generic_builder *gb;
	fy_generic entries, kept, entry, item;
	enum fyai_error_type level;
	size_t n, skip, i;

	if (!l || !l->open)
		return;
	l->open = false;
	fyai_diag_drain_tap(NULL, NULL);
	if (fy_empty(l->parts))
		return;
	level = l->level;
	if (error && level < FYAIET_ERROR)
		level = FYAIET_ERROR;
	gb = fyai_uimodel_begin(ctx);
	if (gb) {
		entry = fy_mapping(gb, "id", ++l->next_id,
				   "title", title ? title : "",
				   "error", error, "level", (long long)level,
				   "parts", l->parts);
		entries = fy_get(fyai_uimodel_get(ctx, FYAI_UI_LOG), "entries",
				 fy_seq_empty);
		n = fy_len(entries);
		skip = n >= FYAI_UILOG_MAX ? n - FYAI_UILOG_MAX + 1 : 0;
		kept = fy_seq_empty;
		i = 0;
		fy_foreach(item, entries)
			if (i++ >= skip)
				kept = fy_append(gb, kept, item);
		(void)fyai_uimodel_set_member(ctx, FYAI_UI_LOG, "entries",
					      fy_append(gb, kept, entry));
	}
	fyai_uimodel_end(ctx);
}

void fyai_uilog_release(struct fyai_ctx *ctx)
{
	struct fyai_uilog *l = ctx ? ctx->uilog : NULL;

	if (!l)
		return;
	if (l->open)
		fyai_diag_drain_tap(NULL, NULL);
	fy_generic_builder_destroy(l->gb);
	free(l);
	ctx->uilog = NULL;
}
