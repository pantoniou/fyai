/*
 * fyai_turn.c - canonical turn construction and traversal
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_UNKNOWN

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>

#include "fyai_provider.h"
#include "fyai_turn.h"

static fy_generic fyai_turn_meta_set(struct fyai_ctx *ctx, fy_generic turn,
				     const char *key, fy_generic value)
{
	fy_generic meta;
	fy_generic v;

	meta = fyai_turn_meta(turn);
	if (fy_generic_is_invalid(meta) || fy_generic_is_null_type(meta))
		meta = fy_map_empty;

	meta = fy_assoc(meta, key, value);
	v = fy_assoc(turn, "metadata", meta);

	v = fy_gb_internalize(ctx->transient_gb, v);
	if (fy_generic_is_invalid(v))
		fyai_error(ctx, "could not update the turn metadata");
	return v;
}

static fy_generic fyai_make_turn(struct fyai_ctx *ctx, fy_generic previous,
				 fy_generic messages)
{
	struct fyai_cfg *cfg = ctx->cfg;
	fy_generic meta;
	fy_generic turn;

	/*
	 * Record how the turn was produced: provider/API identity and the
	 * sampling parameters. Pure provenance for dump/stats - continuations
	 * take their settings from the arena config published with the root.
	 */
	meta = fy_null_filtered_mapping(
		"api", fyai_api_to_string(cfg->api_mode),
		"provider", cfg->provider ? cfg->provider : "",
		"model", cfg->model ? cfg->model : "",
		"temperature", cfg->temperature,
		"reasoning_effort", cfg->reasoning_effort && *cfg->reasoning_effort ?
				fy_value(cfg->reasoning_effort) : fy_null,
		"reasoning_summary", cfg->reasoning_summary && *cfg->reasoning_summary ?
				fy_value(cfg->reasoning_summary) : fy_null);

	turn = fy_mapping(
		"previous", fy_generic_is_valid(previous) ? previous : fy_null,
		"messages", messages,
		"metadata", meta);

	turn = fy_gb_internalize(ctx->transient_gb, turn);
	if (fy_generic_is_invalid(turn))
		fyai_error(ctx, "could not build the turn");
	return turn;
}

fy_generic fyai_turn_append(struct fyai_ctx *ctx, fy_generic turn,
				   fy_generic messages)
{
	return fyai_make_turn(ctx, turn, messages);
}

fy_generic fyai_turn_append_display_output(struct fyai_ctx *ctx,
					    fy_generic turn,
					    fy_generic output)
{
	fy_generic outputs;

	if (fy_generic_is_invalid(turn) || fy_generic_is_invalid(output))
		return turn;
	outputs = fy_get(turn, "display_outputs", fy_seq_empty);
	outputs = fy_append(ctx->transient_gb ? ctx->transient_gb : ctx->gb,
			    outputs, output);
	fyai_error_check(ctx, fy_generic_is_valid(outputs), err,
			 "could not append display output");
	turn = fy_assoc(turn, "display_outputs", outputs);
	turn = fy_gb_internalize(ctx->transient_gb ? ctx->transient_gb : ctx->gb,
				 turn);
	fyai_error_check(ctx, fy_generic_is_valid(turn), err,
			 "could not retain display output");
	return turn;
err:
	return fy_invalid;
}

fy_generic fyai_turn_set_response_id(struct fyai_ctx *ctx,
					    fy_generic turn,
					    fy_generic response_id)
{
	if (fy_generic_is_invalid(turn) || fy_generic_is_null_type(turn))
		return turn;

	return fyai_turn_meta_set(ctx, turn, "response_id", response_id);
}

int fyai_turn_stack_init(struct fyai_turn_stack *stack, fy_generic turn,
			 fy_generic previous)
{
	fy_generic cur;
	size_t i, count;

	stack->items = NULL;
	stack->count = 0;

	if (fy_generic_is_invalid(turn) || fy_generic_is_null_type(turn))
		return 0;

	count = 0;
	fyai_turn_foreach(cur, turn) {
		if (fy_generic_is_valid(previous) && cur.v == previous.v)
			break;
		count++;
	}

	if (!count)
		return 0;

	stack->items = malloc(count * sizeof(*stack->items));
	if (!stack->items)
		return -1;
	stack->count = count;

	i = count;
	fyai_turn_foreach(cur, turn) {
		if (fy_generic_is_valid(previous) && cur.v == previous.v)
			break;
		stack->items[--i] = cur;
	}

	return 0;
}

void fyai_turn_stack_cleanup(struct fyai_turn_stack *stack)
{
	free(stack->items);
	stack->items = NULL;
	stack->count = 0;
}

fy_generic fyai_turn_messages_since(struct fyai_ctx *ctx, fy_generic turn,
					   fy_generic previous)
{
	struct fyai_turn_stack stack;
	fy_generic stream;
	size_t i;

	if (fyai_turn_stack_init(&stack, turn, previous)) {
		fyai_error(ctx, "could not allocate the turn stack");
		return fy_invalid;
	}

	stream = fy_seq_empty;
	for (i = 0; i < stack.count; i++)
		stream = fy_concat(ctx->transient_gb, stream,
				      fy_get(stack.items[i], "messages", fy_seq_empty));

	fyai_turn_stack_cleanup(&stack);

	stream = fy_gb_internalize(ctx->transient_gb, stream);
	if (fy_generic_is_invalid(stream))
		fyai_error(ctx, "could not collect the turn messages");
	return stream;
}

/*
 * Reduce a provider's assistant message to canonical, provider-agnostic
 * content: role plus content, and tool_calls when present
 * since the protocol needs them for call/result pairing. Provider wire
 * fields (reasoning_content, refusal, reasoning_details, ...) are dropped
 * from canonical content; the full message is preserved separately in the
 * turn's provider stream. This is what lets a conversation continue against
 * a different provider without leaking one provider's wire fields to another.
 */
static fy_generic fyai_canonical_assistant_message(struct fyai_ctx *ctx,
						   fy_generic raw)
{
	fy_generic content, tool_calls, msg;

	content = fy_get(raw, "content");
	msg = fy_mapping(
		"role", fy_get(raw, "role", "assistant"),
		"content", fy_generic_is_valid(content) ? content : fy_null);

	tool_calls = fy_get(raw, "tool_calls");
	if (fy_generic_is_valid(tool_calls) && !fy_generic_is_null_type(tool_calls))
		msg = fy_assoc(msg, "tool_calls", tool_calls);

	msg = fy_gb_internalize(ctx->transient_gb, msg);
	if (fy_generic_is_invalid(msg))
		fyai_error(ctx, "could not build the assistant message");
	return msg;
}

/*
 * Attach the provider's verbatim wire messages to @turn under a
 * provider-streams map keyed by provider identity. The canonical
 * `messages` remain the only thing replayed to a model; this parallel stream
 * is persisted for fidelity but never sent.
 */
static fy_generic fyai_turn_attach_provider(struct fyai_ctx *ctx,
					    fy_generic turn,
					    fy_generic provider_messages)
{
	struct fyai_cfg *cfg = ctx->cfg;
	const char *name = cfg->provider ? cfg->provider : cfg->model;
	fy_generic stream;

	if (fy_generic_is_invalid(turn) ||
	    fy_generic_is_invalid(provider_messages)) {
		fyai_error(ctx, "could not attach the provider messages");
		return fy_invalid;
	}

	stream = fy_mapping(name ? name : "", provider_messages);
	turn = fy_assoc(turn, "provider_stream", stream);

	if (fy_generic_is_invalid(turn)) {
		fyai_error(ctx, "could not attach the provider messages");
		return fy_invalid;
	}
	turn = fy_gb_internalize(ctx->transient_gb, turn);
	if (fy_generic_is_invalid(turn))
		fyai_error(ctx, "could not retain the provider messages");
	return turn;
}

/* Persist a turn's normalized token usage in the metadata layer. */
static fy_generic fyai_turn_attach_usage(struct fyai_ctx *ctx, fy_generic turn,
					 fy_generic usage)
{
	if (fy_generic_is_invalid(usage))
		return turn;
	return fyai_turn_meta_set(ctx, turn, "usage", usage);
}

/*
 * Persist the token extents the streamed call collected ({text, pos, lp} per
 * logprob token, {text, pos} per chunk on the fallback paths) in the metadata
 * layer, consuming the ctx hand-off so a later call never inherits them.
 */
static fy_generic fyai_turn_attach_tokens(struct fyai_ctx *ctx,
					  fy_generic turn)
{
	fy_generic extents;

	extents = ctx->last_token_extents;
	ctx->last_token_extents = fy_invalid;
	if (fy_generic_is_invalid(extents))
		return turn;
	return fyai_turn_meta_set(ctx, turn, "tokens", extents);
}

fy_generic fyai_append_assistant_response(struct fyai_ctx *ctx,
						 fy_generic turn,
						 fy_generic response_doc)
{
	struct fyai_cfg *cfg = ctx->cfg;
	fy_generic raw;
	fy_generic usage;
	fy_generic output;
	fy_generic messages;

	usage = fyai_extract_usage(ctx, response_doc);

	switch (cfg->api_mode) {
	case FYAI_API_RESPONSES:
		/*
		 * Responses mode: the provider's wire representation of the turn is
		 * the `output` array. Preserve it as the provider stream so that
		 * `dump providers` is populated for Responses conversations too, the
		 * same way the Chat Completions path keeps its raw message.
		 */
		output = fy_get(response_doc, "output");

		if (fyai_response_is_final(ctx, response_doc)) {
			messages = fy_sequence(
					fy_mapping(
						"role", "assistant",
						"content", fyai_response_output_text(ctx, response_doc)));
			turn = fyai_turn_append(ctx, turn, messages);
		} else {
			turn = fyai_turn_append(ctx, turn, output);
		}

		if (fy_generic_is_valid(output) && !fy_generic_is_null_type(output))
			turn = fyai_turn_attach_provider(ctx, turn, output);
		break;

	case FYAI_API_CHAT_COMPLETIONS:
		raw = response_message(ctx->transient_gb, response_doc);
		messages = fy_sequence(fyai_canonical_assistant_message(ctx, raw));
		turn = fyai_turn_append(ctx, turn, messages);
		turn = fyai_turn_attach_provider(ctx, turn, fy_sequence(raw));
		break;
	case FYAI_API_MESSAGES:
		/*
		 * Messages mode: the wire representation is the `content`
		 * block array; preserve it as the provider stream.
		 * Canonically store the joined text as a plain assistant
		 * message plus, for a tool-use turn, the function_call items
		 * normalized by fyai_response_tool_calls() - the same shapes
		 * a Responses turn stores, so display and cross-provider
		 * replay work unchanged.
		 */
		output = fy_get(response_doc, "content");

		messages = fy_seq_empty;
		raw = fyai_response_output_text(ctx, response_doc);
		if (*fy_cast(raw, ""))
			messages = fy_append(messages,
					fy_mapping(
						"role", "assistant",
						"content", raw));
		if (!fyai_response_is_final(ctx, response_doc))
			messages = fy_concat(ctx->transient_gb, messages,
					fyai_response_tool_calls(ctx, response_doc));
		turn = fyai_turn_append(ctx, turn, messages);

		if (fy_generic_is_valid(output) && !fy_generic_is_null_type(output))
			turn = fyai_turn_attach_provider(ctx, turn, output);
		break;
	}

	turn = fyai_turn_attach_usage(ctx, turn, usage);
	return fyai_turn_attach_tokens(ctx, turn);
}

fy_generic fyai_make_user_message(struct fyai_ctx *ctx, const char *text)
{
	return fy_mapping(ctx->gb,
		"role", "user",
		"content", text);
}

fy_generic fyai_make_system_message(struct fyai_ctx *ctx, const char *text)
{
	return fy_mapping(ctx->gb,
		"role", "system",
		"content", text);
}
