/* SPDX-License-Identifier: MIT */
#ifndef FYAI_UIWORK_H
#define FYAI_UIWORK_H

#include <stdbool.h>
#include <stddef.h>

struct fyai_ctx;
struct fyai_sink_band;

/*
 * The bands of the work pane, in the member "bands" of the work store of the
 * UI model (src/fyai_uimodel.h). The sink and the fenced streams give each
 * band to it at the input, and a renderer draws it again:
 *
 *   {id, shared, title, command, format, text | source, length, language,
 *    lines, indent, state, ok, cause}
 *
 * @format says how to read the body. "markdown" and "code" are source in the
 * text @source of the model, of @length bytes, which the renderer renders at
 * its own width; "code" has a @language. A stream keeps the last @lines rows,
 * and @indent says that it stands under the indent of tool output.
 * "terminal" is the bytes a program
 * wrote, which can move a cursor: a renderer replays them as a terminal does.
 * "tool-view" is the Markdown of the view of a tool call in @text, with the
 * code @blocks it holds, [{start, end, lang}], and the @preview rows of each,
 * which fyai_tool_view_render() renders.
 * "rows" is a body the producer painted in its final form, in @text. A band
 * that ended has @state, @ok and @cause for one write.
 */

/* A band opened. A shared band replaces the shared band before it. */
void fyai_uiwork_band_open(struct fyai_ctx *ctx, struct fyai_sink_band *band,
			   bool shared, const char *title, const char *command);

/* A band was painted. A NULL title or command keeps the one it has. */
void fyai_uiwork_band_paint(struct fyai_ctx *ctx, struct fyai_sink_band *band,
			    const char *title, const char *command,
			    const char *body, size_t len);

/*
 * A fenced stream of @owner that paints @band was given @len bytes of source.
 * A stream that is not the owner of the source of the band starts it again.
 */
void fyai_uiwork_band_source(struct fyai_ctx *ctx, struct fyai_sink_band *band,
			     const void *owner, const char *format,
			     const char *language, size_t lines, bool indent,
			     const char *data, size_t len);

/* The band shows the view of a tool call: @view is its source. */
void fyai_uiwork_band_view(struct fyai_ctx *ctx, struct fyai_sink_band *band,
			   fy_generic view);

/*
 * A band ended: @state is "committed", "destroyed" or "closed", the shared
 * band (NULL) with its outcome, @ok and @cause. The store holds the band with
 * its state for one write, so a consumer ends its band the same way, and then
 * drops it.
 */
void fyai_uiwork_band_end(struct fyai_ctx *ctx, struct fyai_sink_band *band,
			  const char *state, bool ok, const char *cause);

/* The band of the sink that the store names @id, or NULL. */
struct fyai_sink_band *fyai_uiwork_band(struct fyai_ctx *ctx, long long id);
/* The id of @band in the store, or 0. */
long long fyai_uiwork_band_id(struct fyai_ctx *ctx,
			      const struct fyai_sink_band *band);

/* Drop every band: the sink that drew them went, or the process forked. */
void fyai_uiwork_release(struct fyai_ctx *ctx);

#endif
