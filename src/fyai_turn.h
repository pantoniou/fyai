/* SPDX-License-Identifier: MIT */
#ifndef FYAI_TURN_H
#define FYAI_TURN_H

#include "fyai.h"

/* Bound walks through damaged or cyclic durable turn chains. */
#define FYAI_TURN_CHAIN_MAX	(1UL << 22)

/* Brent cycle-detection state for fyai_turn_foreach(). */
struct fyai_turn_walk {
	fy_generic slow;
	unsigned long lam;	/* steps since @slow was left behind */
	unsigned long power;	/* next spacing */
	unsigned long n;	/* steps taken */
};

static inline void fyai_turn_walk_start(struct fyai_turn_walk *w,
					fy_generic head, fy_generic *cur)
{
	w->slow = head;
	w->lam = 0;
	w->power = 1;
	w->n = 0;
	*cur = head;
}

static inline bool fyai_turn_walk_valid(const struct fyai_turn_walk *w,
					fy_generic cur)
{
	return fy_is_valid(cur) && !fy_is_null(cur) && w->n < FYAI_TURN_CHAIN_MAX;
}

static inline void fyai_turn_walk_next(struct fyai_turn_walk *w,
				       fy_generic *cur)
{
	if (w->lam == w->power) {
		w->slow = *cur;
		w->power *= 2;
		w->lam = 0;
	}
	*cur = fy_get(*cur, "previous");
	w->lam++;
	w->n++;
	/* The chain closed on itself: end the walk instead of repeating it. */
	if (fy_is_valid(*cur) && cur->v == w->slow.v)
		*cur = fy_invalid;
}

#define fyai_turn_foreach(cur, head)					\
	for (struct fyai_turn_walk fyai_turn_walk__ ## cur,		\
	     *fyai_turn_walkp__ ## cur =				\
		(fyai_turn_walk_start(&fyai_turn_walk__ ## cur, (head),	\
				      &(cur)),				\
		 &fyai_turn_walk__ ## cur);				\
	     fyai_turn_walk_valid(fyai_turn_walkp__ ## cur, (cur));	\
	     fyai_turn_walk_next(fyai_turn_walkp__ ## cur, &(cur)))

struct fyai_turn_stack {
	fy_generic *items;
	size_t count;
};

fy_generic fyai_turn_append(struct fyai_ctx *ctx, fy_generic turn,
			    fy_generic messages);
fy_generic fyai_turn_append_display_output(struct fyai_ctx *ctx,
					    fy_generic turn,
					    fy_generic output);
fy_generic fyai_turn_set_response_id(struct fyai_ctx *ctx, fy_generic turn,
				     fy_generic response_id);
/*
 * True when a turn carries a user message. This is what starts an exchange:
 * an exchange runs from one such turn up to the turn before the next. Merging
 * works in exchanges, not turns, so a question is never separated from the
 * answer to it.
 */
bool fyai_turn_has_user_message(fy_generic turn);

/* True when a turn carries only system messages (and at least one). */
bool fyai_turn_is_system_only(fy_generic turn);

int fyai_turn_stack_init(struct fyai_turn_stack *stack, fy_generic turn,
			 fy_generic previous);
void fyai_turn_stack_cleanup(struct fyai_turn_stack *stack);
fy_generic fyai_turn_messages_since(struct fyai_ctx *ctx, fy_generic turn,
				    fy_generic previous);
fy_generic fyai_append_assistant_response(struct fyai_ctx *ctx,
					  fy_generic turn,
					  fy_generic response_doc);
fy_generic fyai_make_user_message(struct fyai_ctx *ctx, const char *text);
fy_generic fyai_make_system_message(struct fyai_ctx *ctx, const char *text);

/* The provider identity recorded on a turn (provider_stream's only key). */
static inline fy_generic
fyai_turn_provider(fy_generic turn)
{
	return fy_get_key_at(fy_get(turn, "provider_stream"), 0);
}

static inline fy_generic
fyai_turn_meta(fy_generic turn)
{
	return fy_get(turn, "metadata");
}

#endif
