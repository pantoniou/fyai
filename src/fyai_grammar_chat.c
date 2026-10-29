/*
 * fyai_grammar_chat.c - the Chat Completions grammar
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

/*
 * Chat Completions carries the text of an assistant turn and the calls it
 * makes in one message. The stored turn holds them as a text message and
 * call items: put a held assistant text into the message of the calls that
 * follow it, so the turn goes back on the wire as the provider sent it.
 */
static fy_generic chat_flush(struct fyai_ctx *ctx, fy_generic out,
			     fy_generic *heldp, fy_generic *pendingp)
{
	struct fy_generic_builder *gb = ctx->transient_gb;
	fy_generic msg;

	if (fy_is_valid(*pendingp)) {
		msg = fy_mapping("role", "assistant",
				 "content", fy_is_valid(*heldp) ?
					fy_get(*heldp, "content") : fy_null,
				 "tool_calls", *pendingp);
		out = fy_append(gb, out, msg);
	} else if (fy_is_valid(*heldp)) {
		out = fy_append(gb, out, *heldp);
	}
	*heldp = fy_invalid;
	*pendingp = fy_invalid;
	return out;
}

static fy_generic chat_input(struct fyai_ctx *ctx, fy_generic messages)
{
	struct fy_generic_builder *gb = ctx->transient_gb;
	fy_generic out = fy_seq_empty;
	fy_generic pending = fy_invalid;	/* the calls of an assistant turn */
	fy_generic held = fy_invalid;		/* its text, before the calls */
	fy_generic m, call, function, tmp;
	fy_generic type, content;
	const char *cmd;
	const char *args;

	fy_foreach(m, messages) {
		type = fy_get(m, "type");

		if (fyai_item_type_is_call(type)) {
			if (fy_equal(type, "shell_call")) {
				cmd = fy_cast(fy_get_at_path(m, "action", "commands", 0), "");
				args = emit_json_string(gb, fy_mapping("command", cmd));
				function = fy_mapping(
						"name", FYAI_TOOL_EXEC_WIRE_NAME,
						"arguments", args ? args : "{}");
			} else {
				function = fy_mapping(
						"name", fy_get(m, "name", ""),
						"arguments", fy_get(m, "arguments", ""));
			}

			call = fy_mapping(
					"id", fy_get(m, "call_id", ""),
					"type", "function",
					"function", function);
			pending = fy_is_valid(pending) ?
					fy_append(gb, pending, call) :
					fy_sequence(gb, call);
			continue;
		}

		out = chat_flush(ctx, out, &held, &pending);

		if (fyai_item_type_is_call_output(type)) {
			tmp = fy_mapping("role", "tool",
					 "tool_call_id", fy_get(m, "call_id", ""),
					 "content", fyai_wire_tool_text(ctx, fy_get(m, "output", fy_invalid)));
			out = fy_append(gb, out, tmp);
			continue;
		}

		if (fy_equal(type, "message")) {
			tmp = fy_mapping("role", fy_get(m, "role", "assistant"),
					 "content", fyai_item_text(ctx, m));
			if (fy_equal(fy_get(tmp, "role"), "assistant"))
				held = tmp;
			else
				out = fy_append(gb, out, tmp);
			continue;
		}

		/* reasoning and other native items: no Chat analogue. */
		if (fy_is_string(type))
			continue;

		if (fy_equal(fy_get(m, "role"), "agent")) {
			out = fy_append(gb, out,
				fy_mapping("role", "user", "content",
					fyai_agent_wire_text(ctx, m)));
			continue;
		}
		if (fyai_media_content_has_attachments(fy_get(m, "content"))) {
			content = fyai_wire_content(ctx, fy_get(m, "content"),
						    "text");
			if (fy_is_invalid(content))
				return fyai_provider_result(ctx, content,
						       "could not build the Chat input");
			m = fy_assoc(gb, m, "content", content);
		}
		if (fy_is_valid(fy_get(m, "acp_message_id", fy_invalid)))
			m = fy_mapping("role", fy_get(m, "role", "user"),
				       "content", fy_get(m, "content", ""));
		if (fy_equal(fy_get(m, "role"), "assistant") &&
		    fy_is_string(fy_get(m, "content", fy_invalid)))
			held = m;
		else
			out = fy_append(gb, out, m);
	}
	out = chat_flush(ctx, out, &held, &pending);

	out = fy_gb_internalize(gb, out);
	return fyai_provider_result(ctx, out, "could not build the Chat input");
}


static fy_generic chat_make_tools(struct fyai_ctx *ctx)
{
	if (!ctx->cfg->enable_tools && !ctx->cfg->enable_builtin_shell)
		return fy_seq_empty;
	return make_tools_filtered(ctx);
}

/* A function tool is declared in the shape the specification has. */
static fy_generic chat_tool_def(struct fyai_ctx *ctx, fy_generic spec)
{
	(void)ctx;
	return spec;
}

static fy_generic chat_image_part(struct fyai_ctx *ctx, fy_generic part)
{
	struct fy_generic_builder *gb = ctx->transient_gb;
	fy_generic out;
	char *data;

	data = fyai_media_part_data_url(ctx, part);
	if (!data)
		return fy_invalid;
	out = fy_mapping(gb, "type", "image_url",
			 "image_url", fy_mapping(gb,
				"url", fy_value(gb, data)));
	free(data);
	return out;
}

static fy_generic chat_build_request(struct fyai_ctx *ctx,
				     const struct fyai_grammar_request *req)
{
	struct fy_generic_builder *gb = ctx->transient_gb;
	struct fyai_cfg *cfg = ctx->cfg;
	fy_generic options;
	fy_generic request;

	request = fy_mapping(gb,
		"model", cfg->model,
		"messages", chat_input(ctx, req->messages));
	if (req->temperature)
		request = fy_assoc(gb, request, "temperature", cfg->temperature);
	if (!fy_str_empty(cfg->reasoning_effort))
		request = fy_assoc(gb, request, "reasoning_effort",
				   cfg->reasoning_effort);
	if (fyai_provider_native_web_search(cfg)) {
		if (fyai_provider_is_openrouter(cfg))
			request = fy_assoc(gb, request, "tools",
				fy_append(gb, ctx->tools,
					  fy_mapping(gb, "type",
						     "openrouter:web_search")));
		else
			request = fy_assoc(gb, request, "web_search_options",
					   fy_map_empty);
	}
	if (fyai_grammar_tools_wanted(ctx)) {
		request = fy_assoc(gb, request, "tools", ctx->tools,
				   "tool_choice", "auto");
		if (cfg->parallel_tool_calls)
			request = fy_assoc(gb, request,
					   "parallel_tool_calls", true);
	}
	if (cfg->logprobs || req->extents_lp)
		request = fy_assoc(gb, request, "logprobs", true);
	if (cfg->top_logprobs >= 0)
		request = fy_assoc(gb, request, "top_logprobs",
				   cfg->top_logprobs);
	options = fy_mapping(gb, "include_usage", true);
	if (cfg->no_obfuscation)
		options = fy_assoc(gb, options, "include_obfuscation", false);
	return fyai_grammar_add_stream(ctx, request, options);
}

static const struct fyai_grammar_header chat_headers[] = {
	{ NULL, NULL },
};

const struct fyai_grammar fyai_grammar_chat = {
	.mode			= FYAI_API_CHAT_COMPLETIONS,
	.name			= "chat-completions",
	.alias			= "chat",
	.protocol		= "chat_completions",
	.title			= "Chat Completions API",
	.default_url		= OPENAI_CHAT_COMPLETIONS_URL,
	.default_model		= DEFAULT_OPENAI_MODEL,
	.default_provider	= "openai",
	.headers		= chat_headers,
	.input			= chat_input,
	.make_tools		= chat_make_tools,
	.tool_def		= chat_tool_def,
	.image_part		= chat_image_part,
	.build_request		= chat_build_request,
	.reasoning		= true,
	.logprobs		= true,
};
