/* SPDX-License-Identifier: MIT */
#ifndef FYAI_PROVIDER_H
#define FYAI_PROVIDER_H

#include "fyai.h"

/*
 * Canonical item types. A tool call is a function call or a native shell
 * call; each has its own output type. Keep both tests here so a new call
 * type is one change.
 */
static inline bool fyai_item_type_is_call(fy_generic type)
{
	return fy_any_equal(type, "function_call", "shell_call");
}

static inline bool fyai_item_type_is_call_output(fy_generic type)
{
	return fy_any_equal(type, "function_call_output", "shell_call_output");
}

fy_generic fyai_extract_usage(struct fyai_ctx *ctx, fy_generic doc);
fy_generic fyai_token_extents_append(struct fy_generic_builder *gb,
				     fy_generic extents, fy_generic entries,
				     size_t *posp);
fy_generic fyai_chunk_extents(struct fy_generic_builder *gb, fy_generic chunks);
fy_generic fyai_make_responses_tools(struct fyai_ctx *ctx);
bool fyai_provider_native_shell(const struct fyai_cfg *cfg);
/* Return true for a transient error in an HTTP stream. */
bool fyai_provider_error_transient(fy_generic err);
fy_generic fyai_responses_input(struct fyai_ctx *ctx, fy_generic messages);
fy_generic fyai_item_text(struct fyai_ctx *ctx, fy_generic item);
fy_generic fyai_chat_input(struct fyai_ctx *ctx, fy_generic messages);
fy_generic fyai_make_messages_tools(struct fyai_ctx *ctx);
fy_generic fyai_messages_input(struct fyai_ctx *ctx, fy_generic messages);
fy_generic fyai_response_output_text(struct fyai_ctx *ctx, fy_generic doc);
fy_generic fyai_response_tool_calls(struct fyai_ctx *ctx, fy_generic doc);
fy_generic fyai_response_id(struct fyai_ctx *ctx, fy_generic doc);
bool fyai_response_needs_tool_calls(struct fyai_ctx *ctx, fy_generic doc);
bool fyai_response_is_final(struct fyai_ctx *ctx, fy_generic doc);

#endif
