/*
 * fyai_out.h - the output component
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_OUT_H
#define FYAI_OUT_H

struct fyai_diag;
struct fyai_display_output;
struct fyai_loop;
struct fyai_sink;

/*
 * What the engine says to the user: the one rendering sink, which every byte
 * the user sees goes through, and the open transcript document that the sink
 * presents. The engine reaches the display only through this component. It
 * stands on the loop and the diagnostics. The owner is struct fyai_app; a view
 * context keeps a copy of its own.
 */
struct fyai_out {
	/* Dependencies. */
	struct fyai_loop *loop;
	struct fyai_diag *diag;

	/* The one rendering component. Every byte the user sees goes here. */
	struct fyai_sink *sink;
	/*
	 * The sole progressive transcript document for the active user or
	 * assistant output. Owned by this context, never by a signal handler.
	 */
	struct fyai_display_output *display_output;
};

void fyai_out_init(struct fyai_out *out, struct fyai_loop *loop,
		   struct fyai_diag *diag);
/* The child presents nothing of its parent: the sink and the document stay with it. */
void fyai_out_abandon(struct fyai_out *out);

#endif
