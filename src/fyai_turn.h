/* SPDX-License-Identifier: MIT */
#ifndef FYAI_TURN_H
#define FYAI_TURN_H

#include "fyai.h"

#define fyai_turn_foreach(cur, head)					\
	for ((cur) = (head);						\
	     fy_generic_is_valid(cur) && !fy_generic_is_null_type(cur);	\
	     (cur) = fy_get(cur, "previous"))

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
