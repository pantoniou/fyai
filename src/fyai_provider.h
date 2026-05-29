/* SPDX-License-Identifier: MIT */
#ifndef FYAI_PROVIDER_H
#define FYAI_PROVIDER_H

#include "fyai.h"

fy_generic fyai_extract_usage(struct fyai_ctx *ctx, fy_generic doc);
fy_generic fyai_token_extents_append(struct fy_generic_builder *gb,
				     fy_generic extents, fy_generic entries,
				     size_t *posp);
fy_generic fyai_chunk_extents(struct fy_generic_builder *gb, fy_generic chunks);
fy_generic fyai_make_responses_tools(struct fyai_ctx *ctx);
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
