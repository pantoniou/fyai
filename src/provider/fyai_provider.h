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
bool fyai_provider_native_shell(const struct fyai_cfg *cfg);
bool fyai_provider_native_web_search(const struct fyai_cfg *cfg);
/* Return true for a transient error in an HTTP stream. */
bool fyai_provider_error_transient(fy_generic err);
fy_generic fyai_item_text(struct fyai_ctx *ctx, fy_generic item);
/*
 * The canonical response of the reply document @doc of the configured
 * grammar (see struct fyai_grammar), with the tool calls of the user taken
 * away. The fyai_response_*() readers and fyai_extract_usage() read it.
 */
fy_generic fyai_response_canonical(struct fyai_ctx *ctx, fy_generic doc);
fy_generic fyai_response_output_text(struct fyai_ctx *ctx, fy_generic doc);
fy_generic fyai_response_tool_calls(struct fyai_ctx *ctx, fy_generic doc);
fy_generic fyai_response_id(struct fyai_ctx *ctx, fy_generic doc);
bool fyai_response_needs_tool_calls(struct fyai_ctx *ctx, fy_generic doc);
bool fyai_response_is_final(struct fyai_ctx *ctx, fy_generic doc);

#endif
