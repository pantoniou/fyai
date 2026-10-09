/* SPDX-License-Identifier: MIT */
#ifndef FYAI_ASKS_H
#define FYAI_ASKS_H

#include "fyai.h"

/*
 * Questions that wait for the user without holding the turn. A call of
 * ask_user that gives a name asks at once and returns. The question is an
 * object of the run, as a named wait is: `list` shows it, `cancel` takes it
 * back, and `wait` holds a call until the user has answered. The model is
 * told how it ended in a later turn, as for a wait that fires.
 */

/* A name is a short word of letters, digits, '-', '_' and '/'. */
#define FYAI_ASKS_NAME_MAX	32

/* Whether the arguments of ask_user ask without holding the turn. */
bool fyai_asks_background(fy_generic args);

/*
 * Put @questions, normalized by fyai_ask_normalize(), to the user under @name,
 * or under a name made for it when @name is empty. Returns the text for the
 * model, or a "tool error:" text. Returns NULL after it reported why it
 * cannot ask.
 */
char *fyai_asks_start(struct fyai_ctx *ctx, const char *name,
		      fy_generic questions);

/* Whether a question named @name waits for the user. */
bool fyai_asks_exists(struct fyai_ctx *ctx, const char *name);

/* Take the question @name back; false when no such question waits. */
bool fyai_asks_cancel(struct fyai_ctx *ctx, const char *name);

/* The questions that wait, as rows of name and the headers they carry. */
fy_generic fyai_asks_rows(struct fyai_ctx *ctx, struct fy_generic_builder *gb);

/* Drop every question at the end of the invocation. */
void fyai_asks_release(struct fyai_ctx *ctx);

/* Drop the questions of the parent in a forked child, without ending them. */
void fyai_asks_abandon(struct fyai_ctx *ctx);

#endif
