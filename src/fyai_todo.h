/* SPDX-License-Identifier: MIT */
#ifndef FYAI_TODO_H
#define FYAI_TODO_H

#include "fyai.h"

/* Todo list states, as the model writes them. */
#define FYAI_TODO_PENDING	"pending"
#define FYAI_TODO_IN_PROGRESS	"in_progress"
#define FYAI_TODO_COMPLETED	"completed"
#define FYAI_TODO_CANCELLED	"cancelled"

/* Todo list priorities, as the model writes them. */
#define FYAI_TODO_HIGH		"high"
#define FYAI_TODO_MEDIUM	"medium"
#define FYAI_TODO_LOW		"low"

/* Longest task text kept; a longer one is refused, not cut. */
#define FYAI_TODO_CONTENT_MAX	512
/* Most items one list holds. */
#define FYAI_TODO_ITEMS_MAX	64

/*
 * Check the arguments of the todo_write tool and return the normalized
 * sequence of {content, status, priority} mappings, built in @gb.
 * Returns fy_invalid and writes the cause in @why when an item is not valid.
 */
fy_generic fyai_todo_normalize(struct fy_generic_builder *gb, fy_generic args,
			       char *why, size_t why_size);

/* The stored list of this branch, or fy_invalid when it holds none. */
fy_generic fyai_todo_stored(struct fyai_ctx *ctx);

/* Whether every item of @todos is finished: completed or cancelled. */
bool fyai_todo_all_done(fy_generic todos);

/*
 * Replace the stored list with @todos, normalized by fyai_todo_normalize(),
 * and publish the branch. Returns the text for the model, or a "tool error:"
 * text. Returns NULL after it reported why the list cannot be kept.
 */
char *fyai_todo_commit(struct fyai_ctx *ctx, fy_generic todos);

/* Drop the stored list and publish the branch. Returns 0, or -1. */
int fyai_todo_clear(struct fyai_ctx *ctx);

/* The stored list as rows of content, status, and priority, built in @gb. */
fy_generic fyai_todo_rows(struct fyai_ctx *ctx, struct fy_generic_builder *gb);

/*
 * The stored list as UI Markdown for the page panel, borrowed from the
 * scratch of the call. NULL when the branch holds no list.
 */
const char *fyai_todo_panel(struct fyai_ctx *ctx);

/*
 * The stored list as Markdown for the user: each item with its state mark.
 * The caller frees the result. NULL when the branch holds no list.
 */
char *fyai_todo_format(struct fyai_ctx *ctx);

/*
 * The result of the tool as Markdown for the user, through the shared
 * formatter. @args are the arguments of the call, or fy_invalid.
 * Returns a malloc'd string, or NULL to show the result as it is.
 */
char *fyai_todo_format_result(struct fyai_ctx *ctx,
			      struct fy_generic_builder *gb,
			      fy_generic args, const char *result);

#endif
