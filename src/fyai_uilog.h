/* SPDX-License-Identifier: MIT */
#ifndef FYAI_UILOG_H
#define FYAI_UILOG_H

#include <stdbool.h>
#include <stddef.h>

#include "fyai_sink.h"

struct fyai_ctx;

/*
 * The log of the session: the results, notices and diagnostics of commands,
 * in the log store of the UI model (src/fyai_uimodel.h). An entry holds what
 * the span of a command gave the sink and the diagnostics it drained, as
 * source:
 *
 *   {id, title, error, level, parts: [{stream, md | text | item}]}
 *
 * @stream is the sink stream, or "drain" for a drained record in @item. The
 * store keeps the last FYAI_UILOG_MAX entries.
 */
#define FYAI_UILOG_MAX	200

/* Open the span of a command. A span still open is dropped. */
void fyai_uilog_begin(struct fyai_ctx *ctx);

/*
 * Close the span: append its entry to the log store, titled @title and an
 * error when @error is set or a drained record was one. A span that gave
 * nothing adds no entry.
 */
void fyai_uilog_end(struct fyai_ctx *ctx, const char *title, bool error);

/*
 * Record what the sink was given while a span is open: Markdown source when
 * @markdown is set, else bytes in their final form. The sink calls it.
 */
void fyai_uilog_sink(struct fyai_ctx *ctx, enum fyai_sink_stream stream,
		     bool markdown, const char *text, size_t len);

/* Release the span at cleanup, and drop it in a forked child. */
void fyai_uilog_release(struct fyai_ctx *ctx);

#endif
