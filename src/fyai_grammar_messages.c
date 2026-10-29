/*
 * fyai_grammar_messages.c - the Anthropic Messages grammar
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_UNKNOWN

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <string.h>

#include "fyai_grammar.h"
#include "fyai_media.h"
#include "fyai_provider.h"
#include "fyai_tool_spec.h"
#include "fyai_tools.h"
#include "utils.h"

/* Anthropic tools are flat: {name, description, input_schema}. */
static fy_generic messages_tool_def(struct fyai_ctx *ctx, fy_generic spec)
{
	fy_generic function = fy_get(spec, "function");

	return fy_mapping(ctx->gb,
		"name", fy_get(function, "name", ""),
		"description", fy_get(function, "description", ""),
		"input_schema", fy_get(function, "parameters"));
}

static fy_generic messages_make_tools(struct fyai_ctx *ctx)
{
	fy_generic messages_tools;
	fy_generic tools;
	fy_generic tool;

	messages_tools = fy_seq_empty;
	if (ctx->cfg->enable_tools || ctx->cfg->enable_builtin_shell) {
		tools = make_tools_filtered(ctx);
		fy_foreach(tool, tools) {
			messages_tools = fy_append(messages_tools,
						   messages_tool_def(ctx, tool));
		}
	}
	if (fyai_provider_native_web_search(ctx->cfg))
		messages_tools = fy_append(messages_tools,
				fy_mapping("type", "web_search_20250305",
						   "name", "web_search"));

	messages_tools = fy_gb_internalize(ctx->gb, messages_tools);
	return fyai_provider_result(ctx, messages_tools,
			       "could not build the Messages tool definitions");
}

/*
 * Append @block to the Anthropic message list @out under @role: the Messages
 * API requires strictly alternating user/assistant roles, so a block whose
 * role matches the last message's is merged into that message's content
 * array instead of opening a new message. Content is always a block array.
 */
static fy_generic messages_append_block(struct fyai_ctx *ctx, fy_generic out,
					const char *role, fy_generic block)
{
	struct fy_generic_builder *gb = ctx->transient_gb;
	fy_generic last, content;
	size_t n;

	n = fy_len(out);
	if (n) {
		last = fy_get_at(out, n - 1);
		if (fy_equal(fy_get(last, "role"), role)) {
			content = fy_append(gb,
					fy_get(last, "content", fy_seq_empty),
					block);
			last = fy_assoc(gb, last, "content", content);
			return fy_replace(gb, out, n - 1, last);
		}
	}
	return fy_append(gb, out,
			fy_mapping("role", role,
				   "content", fy_sequence(gb, block)));
}

/*
 * Build the block in the builder, not as a builder-less scratch generic: a
 * scratch generic lives in the creating function's stack frame and dies with
 * it, so it must never be returned (same lifetime family as the fy_cast
 * short-string rule in CLAUDE.md). Text travels as a generic, not a char *,
 * for the same reason.
 */
static fy_generic messages_text_block(struct fyai_ctx *ctx, fy_generic text)
{
	return fy_mapping(ctx->transient_gb, "type", "text", "text", text);
}

/*
 * Translate canonical messages into Anthropic Messages shape so any
 * conversation (native, or begun under Responses/Chat Completions) can be
 * continued against the Messages API. The system turn is dropped here - it
 * travels in the request's top-level `system` field. Tool requests
 * (Responses-style function_call/shell_call items, or Chat assistant
 * tool_calls) become assistant `tool_use` blocks; tool results
 * (function_call_output/shell_call_output items, or Chat {role: tool})
 * become user `tool_result` blocks. Reasoning items are provider wire
 * detail with no Messages analogue and are dropped. It adds no cache
 * marker. The result is in the transient builder.
 */
fy_generic fyai_messages_blocks(struct fyai_ctx *ctx, fy_generic messages)
{
	struct fy_generic_builder *gb = ctx->transient_gb;
	fy_generic out = fy_seq_empty;
	fy_generic m, content, input, output;
	fy_generic type, role;
	fy_generic tmp;
	fy_generic blocks, block;
	const char *cmd, *text;
	const char *role_name;

	fy_foreach(m, messages) {
		type = fy_get(m, "type");

		/* Responses-style native call items -> assistant tool_use. */
		if (fy_equal(type, "function_call")) {
			input = parse_json_string(gb, fy_get(m, "arguments", "{}"));
			if (fy_is_invalid(input))
				input = fy_map_empty;
			tmp = fy_mapping("type", "tool_use",
					 "id", fy_get(m, "call_id", ""),
					 "name", fy_get(m, "name", ""),
					 "input", input);
			out = messages_append_block(ctx, out, "assistant", tmp);
			continue;
		}
		if (fy_equal(type, "shell_call")) {
			cmd = fy_cast(fy_get_at_path(m, "action", "commands", 0), "");
			tmp = fy_mapping("type", "tool_use",
					 "id", fy_get(m, "call_id", ""),
					 "name", FYAI_TOOL_EXEC_WIRE_NAME,
					 "input", fy_mapping("command", cmd));
			out = messages_append_block(ctx, out, "assistant", tmp);
			continue;
		}

		/* Native output items -> user tool_result. */
		if (fyai_item_type_is_call_output(type)) {
			output = fy_get(m, "output");
			if (!fy_is_string(output)) {
				text = emit_json_string(gb, output);
				output = fy_value(text ? text : "");
			}
			tmp = fy_mapping("type", "tool_result",
					 "tool_use_id", fy_get(m, "call_id", ""),
					 "content", output);
			out = messages_append_block(ctx, out, "user", tmp);
			continue;
		}

		/* A Responses `message` item -> its joined text. */
		if (fy_equal(type, "message")) {
			out = messages_append_block(ctx, out,
					fy_cast(fy_get(m, "role", "assistant"), ""),
					messages_text_block(ctx, fyai_item_text(ctx, m)));
			continue;
		}

		/* reasoning and other native items: no Messages analogue. */
		if (fy_is_string(type))
			continue;

		role = fy_get(m, "role");
		if (fy_equal(role, "agent")) {
			out = messages_append_block(ctx, out, "user",
				messages_text_block(ctx,
					fyai_agent_wire_text(ctx, m)));
			continue;
		}

		/* The system prompt travels in the request `system` field. */
		if (fy_equal(role, "system"))
			continue;

		/* Plain role message. */
		content = fy_get(m, "content");
		if (fy_is_invalid(content) ||
		    fy_is_null(content))
			content = fy_value("");
		/* Files without images are the text of one block. */
		if (!fyai_media_content_has_images(content) &&
		    fyai_media_content_has_files(content))
			content = fyai_media_model_text(gb, content);
		if (fyai_media_content_has_images(content)) {
			blocks = fyai_wire_content(ctx, content, "text");
			if (fy_is_invalid(blocks))
				return fyai_provider_result(ctx, blocks,
					"could not build the Messages input");
			role_name = fy_is_string(role) ? fy_castp(&role, "") :
						       "user";
			fy_foreach(block, blocks)
				out = messages_append_block(ctx, out, role_name,
							    block);
			continue;
		}
		if (fy_is_sequence(content)) {
			out = fy_append(gb, out,
					fy_mapping("role", fy_is_string(role) ?
							fy_castp(&role, "") : "user",
						   "content", content));
			continue;
		}
		out = messages_append_block(ctx, out,
				fy_is_string(role) ? fy_castp(&role, "") : "user",
				messages_text_block(ctx, content));
	}
	return fy_gb_internalize(gb, out);
}

/* The Messages request input: the blocks with a prompt-cache breakpoint. */
static fy_generic messages_input(struct fyai_ctx *ctx, fy_generic messages)
{
	struct fy_generic_builder *gb = ctx->transient_gb;
	fy_generic out, m, content, tc;
	size_t j, n;

	out = fyai_messages_blocks(ctx, messages);
	if (fy_is_invalid(out))
		return fyai_provider_result(ctx, out,
				       "could not build the Messages input");

	/*
	 * Prompt-cache breakpoint on the last block of the history: Anthropic
	 * caches the exact prefix up to a cache_control marker and ignores
	 * the markers themselves when matching, so moving the breakpoint
	 * forward each turn re-reads the previous span and extends it. Only
	 * the outbound request carries the marker; canonical state does not.
	 */
	n = fy_len(out);
	if (n) {
		m = fy_get_at(out, n - 1);
		content = fy_get(m, "content", fy_seq_empty);
		j = fy_len(content);
		if (j) {
			tc = fy_get_at(content, j - 1);
			tc = fy_assoc(gb, tc, "cache_control",
				      fy_mapping(gb, "type", "ephemeral"));
			content = fy_replace(gb, content, j - 1, tc);
			m = fy_assoc(gb, m, "content", content);
			out = fy_replace(gb, out, n - 1, m);
		}
	}

	out = fy_gb_internalize(ctx->transient_gb, out);
	return fyai_provider_result(ctx, out, "could not build the Messages input");
}

static fy_generic messages_image_part(struct fyai_ctx *ctx, fy_generic part)
{
	struct fy_generic_builder *gb = ctx->transient_gb;
	fy_generic out;
	char *data;

	data = fyai_media_part_base64(ctx, part);
	if (!data)
		return fy_invalid;
	out = fy_mapping(gb, "type", "image",
			 "source", fy_mapping(gb,
				"type", "base64",
				"media_type", fy_get(part, "media_type", ""),
				"data", fy_value(gb, data)));
	free(data);
	return out;
}

/*
 * Messages takes no temperature, reasoning or logprobs here; the
 * configuration gate rejects the last two. Anthropic caches only on an
 * explicit cache_control breakpoint.
 */
static fy_generic messages_build_request(struct fyai_ctx *ctx,
					 const struct fyai_grammar_request *req)
{
	struct fy_generic_builder *gb = ctx->transient_gb;
	struct fyai_cfg *cfg = ctx->cfg;
	fy_generic request;

	request = fy_mapping(gb,
		"model", cfg->model,
		"max_tokens", req->max_tokens,
		"system", fy_sequence(gb,
				fy_mapping(gb, "type", "text",
					   "text", req->instructions,
					   "cache_control", fy_mapping(gb, "type", "ephemeral"))),
		"messages", messages_input(ctx, req->messages));
	/* Messages spells tool_choice as an object, not a string. */
	if (fyai_grammar_tools_wanted(ctx) ||
	    fyai_provider_native_web_search(cfg))
		request = fy_assoc(gb, request, "tools", ctx->tools,
			"tool_choice", fy_mapping(gb,
				"type", "auto",
				"disable_parallel_tool_use",
				cfg->parallel_tool_calls ? fy_false : fy_true));
	return fyai_grammar_add_stream(ctx, request, fy_map_empty);
}

static const struct fyai_grammar_header messages_headers[] = {
	{ "anthropic-version", ANTHROPIC_VERSION },
	{ NULL, NULL },
};

const struct fyai_grammar fyai_grammar_messages = {
	.mode			= FYAI_API_MESSAGES,
	.name			= "messages",
	.protocol		= "messages",
	.title			= "Messages API",
	.default_url		= ANTHROPIC_MESSAGES_URL,
	.default_model		= DEFAULT_ANTHROPIC_MODEL,
	.default_provider	= "anthropic",
	.key_header		= "x-api-key",
	.headers		= messages_headers,
	.input			= messages_input,
	.make_tools		= messages_make_tools,
	.tool_def		= messages_tool_def,
	.image_part		= messages_image_part,
	.build_request		= messages_build_request,
};
