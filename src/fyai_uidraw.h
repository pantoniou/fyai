/* SPDX-License-Identifier: MIT */
#ifndef FYAI_UIDRAW_H
#define FYAI_UIDRAW_H

#include <stdbool.h>
#include <stddef.h>

#include "fyai_uimodel.h"

struct fyai_ctx;

/*
 * The renderer of the UI model. It draws the stores of the model of a context
 * with the sink of that context, from the stores alone. The
 * model gives it each change of a store when the change is made, so it draws
 * in the order of the writes. doc/ui-model.md gives the design.
 */

/* @store of the model of @ctx changed to @root, borrowed for the call. */
void fyai_uidraw_store(struct fyai_ctx *ctx, enum fyai_ui_store store,
		       fy_generic root);

/*
 * Record what the legacy path gives the sink for the live document: @len
 * bytes of source, or an operation in "<...>" form. The renderer compares it
 * with what it gave the sink when the document ends.
 */
void fyai_uidraw_oracle(struct fyai_ctx *ctx, const char *data, size_t len);
/* As fyai_uidraw_oracle(), for an operation that @fmt formats. */
void fyai_uidraw_oracle_printf(struct fyai_ctx *ctx, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

/*
 * Whether this renderer draws the bands of the producers of @ctx: a display
 * is open and the model is its own. The bands of a producer are then shadows
 * (src/fyai_sink.h).
 */
bool fyai_uidraw_bands(struct fyai_ctx *ctx);
/* The width of the band of the display that draws the shadow @shadow. */
struct fyai_sink_band;
int fyai_uidraw_band_cols(struct fyai_ctx *ctx,
			  const struct fyai_sink_band *shadow);
/* Drop the bands of the display: the sink that holds them goes. */
void fyai_uidraw_bands_release(struct fyai_ctx *ctx);

/* End what the renderer draws, before the display of @ctx closes. */
void fyai_uidraw_finish(struct fyai_ctx *ctx);

/* Release the renderer at cleanup. */
void fyai_uidraw_release(struct fyai_ctx *ctx);

/*
 * Drop the renderer in a forked child without drawing: what it drew is the
 * display of the parent.
 */
void fyai_uidraw_abandon(struct fyai_ctx *ctx);

#endif
