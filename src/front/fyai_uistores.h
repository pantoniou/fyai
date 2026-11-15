/*
 * fyai_uistores.h - the UI model component
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_UISTORES_H
#define FYAI_UISTORES_H

#include <stdbool.h>

struct fyai_diag;
struct fyai_loop;
struct fyai_uidraw;
struct fyai_uilog;
struct fyai_uimodel;
struct fyai_uitranscript;
struct fyai_uiwork;

/*
 * The state that renderers read: the model and its stores, the bands of the
 * display, the live document, the span of a command and the renderer of the
 * model. It stands on the loop and the diagnostics. The owner is struct
 * fyai_app; a view context that detaches from the UI keeps a copy of its own,
 * with no model.
 */
struct fyai_uistores {
	/* Dependencies. */
	struct fyai_loop *loop;
	struct fyai_diag *diag;

	/* The state that renderers read (src/front/fyai_uimodel.h). */
	struct fyai_uimodel *uimodel;
	struct fyai_uiwork *uiwork;	/* the bands of the display (src/front/fyai_uiwork.h) */
	/* A copy that renders stored output: it has no UI model. */
	bool uimodel_detached;
	/*
	 * The process publishes the model and draws nothing: a remote worker,
	 * whose client draws the model it replicates.
	 */
	bool uidraw_off;
	struct fyai_uitranscript *uitranscript;	/* the live document (src/front/fyai_uitranscript.h) */
	struct fyai_uilog *uilog;	/* the span of a command (src/front/fyai_uilog.h) */
	struct fyai_uidraw *uidraw;	/* the renderer of the model (src/front/fyai_uidraw.h) */
};

void fyai_uistores_init(struct fyai_uistores *stores, struct fyai_loop *loop,
			struct fyai_diag *diag);
/* The renderers and stores of the parent are released elsewhere; forget none here. */
void fyai_uistores_abandon(struct fyai_uistores *stores);

#endif
