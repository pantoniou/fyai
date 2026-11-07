/* SPDX-License-Identifier: MIT */
#ifndef FYAI_UIMODEL_H
#define FYAI_UIMODEL_H

#include <stdbool.h>
#include <stddef.h>

#include <libfyaml.h>
#include <libfyaml/libfyaml-generic.h>

struct fyai_ctx;

/*
 * The UI model: the state that every renderer reads, as one generic root for
 * each store. The engine writes a store; a renderer reads its root at frame
 * time. A store holds data and Markdown source only: no SGR, no rendered rows
 * and nothing that depends on a width or a theme. View state - focus, zoom,
 * scroll, layout, theme, prompt text - belongs to each renderer and is not in
 * the model. doc/ui-model.md gives the design.
 */
enum fyai_ui_store {
	FYAI_UI_SESSION,	/* the values of the header and the status */
	FYAI_UI_PANELS,		/* the panels by name, each as data rows */
	FYAI_UI_QUESTIONS,	/* the sets of questions put to the user */
	FYAI_UI_LOG,		/* the results of commands (src/fyai_uilog.h) */
	FYAI_UI_WORK,		/* the live work and the bands (src/fyai_uiwork.h) */
	FYAI_UI_TRANSCRIPT,	/* the live document (src/fyai_uitranscript.h) */
	FYAI_UI_STORE_COUNT
};

struct fyai_uimodel;

/* The name of @store, as the trace and a divergence record name it. */
const char *fyai_ui_store_name(enum fyai_ui_store store);

/*
 * Make the model of @ctx, with all its builders, at setup. Every other
 * function reads the model that this one made; it makes none. Returns 0, or
 * -1 with a diagnostic.
 */
int fyai_uimodel_create(struct fyai_ctx *ctx);

/*
 * A new builder for a store that keeps its own text, such as the log. The
 * caller destroys it. NULL when @ctx has no model.
 */
struct fy_generic_builder *fyai_uimodel_builder_new(struct fyai_ctx *ctx);

/*
 * The root of @store, or fy_invalid when nothing wrote it. The root is valid
 * until the next write of the store; keep it no longer.
 */
fy_generic fyai_uimodel_get(struct fyai_ctx *ctx, enum fyai_ui_store store);

/* The sequence number of @store: it grows by one on each change. */
unsigned long long fyai_uimodel_seq(struct fyai_ctx *ctx,
				    enum fyai_ui_store store);

/*
 * Begin a write: return the scratch builder that a writer makes a new root
 * in. The outermost begin resets it, so it holds only what the writes of one
 * span made; a writer that runs during the write of another, such as the
 * adoption of a branch, keeps what that writer built. End each begin with
 * fyai_uimodel_end(). A writer that runs on every frame or every stream chunk
 * uses it, not the transient builder, which grows until the turn ends.
 */
struct fy_generic_builder *fyai_uimodel_begin(struct fyai_ctx *ctx);
void fyai_uimodel_end(struct fyai_ctx *ctx);

/*
 * Replace the root of @store with a copy of @root. The store keeps two roots
 * at most: the copy goes into the builder of the root before the current one,
 * so a root that fyai_uimodel_get() returned is valid until the second write
 * after it. Do not build @root in a builder of the model other than the
 * scratch builder. A root equal to the current one changes nothing. Returns
 * 1 for a change, 0 for none, and -1 with a diagnostic when the copy cannot
 * be made.
 */
int fyai_uimodel_set(struct fyai_ctx *ctx, enum fyai_ui_store store,
		     fy_generic root);

/*
 * Replace the member @key of the mapping of @store with @value, and keep the
 * other members. A store that holds no mapping starts from an empty one. The
 * new root is built in the scratch builder, so @value may live there too.
 * Returns as fyai_uimodel_set().
 */
int fyai_uimodel_set_member(struct fyai_ctx *ctx, enum fyai_ui_store store,
			    const char *key, fy_generic value);

/*
 * A text that grows by appends, such as the source of a band. A store names
 * it by its id with the length it had at the write; a renderer reads it with
 * fyai_uimodel_text(), and a remote client takes the bytes after the length
 * it has. An append copies nothing that is in a store. Returns the id, or 0
 * with a diagnostic.
 */
long long fyai_uimodel_text_open(struct fyai_ctx *ctx);
int fyai_uimodel_text_append(struct fyai_ctx *ctx, long long id,
			     const char *data, size_t len);
/* The bytes of text @id, "" for an unknown id; valid until the next append. */
const char *fyai_uimodel_text(struct fyai_ctx *ctx, long long id,
			      size_t *lenp);
void fyai_uimodel_text_close(struct fyai_ctx *ctx, long long id);

/*
 * A consumer of changes. @root is the new root of @store, borrowed for the
 * call. A consumer that renders in a frame reads the root again at frame time
 * and does not keep @root.
 */
typedef void (*fyai_uimodel_fn)(void *data, enum fyai_ui_store store,
				unsigned long long seq, fy_generic root);

int fyai_uimodel_subscribe(struct fyai_ctx *ctx, fyai_uimodel_fn fn,
			   void *data);
void fyai_uimodel_unsubscribe(struct fyai_ctx *ctx, fyai_uimodel_fn fn,
			      void *data);

/*
 * Compare what a renderer made from the model, @model, with what the legacy
 * path made, @oracle. A difference is a defect of the model: it goes to the
 * trace and, when $FYAI_UI_MODEL_CHECK names a file, to that file. Returns
 * true when the two are equal.
 */
bool fyai_uimodel_check(struct fyai_ctx *ctx, enum fyai_ui_store store,
			const char *what, const char *model,
			const char *oracle);

/*
 * Detach @view, a copy of a context that renders stored output offscreen,
 * from the UI model of the context it copies: its writes go nowhere, and it
 * makes no model, log or band registry of its own.
 */
void fyai_uimodel_view_detach(struct fyai_ctx *view);

/* Release the model of @ctx at cleanup. */
void fyai_uimodel_release(struct fyai_ctx *ctx);

/*
 * Drop the model in a forked child. Its values describe the display of the
 * parent, such as a turn that runs there; the child writes its own.
 */
void fyai_uimodel_abandon(struct fyai_ctx *ctx);

#endif
