/*
 * fyai_grammar_claude_code.c - the Claude Code CLI backend grammar
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * The installed Claude Code CLI serves the model (fyai_claude_cli.c). Its
 * stream_event lines are Anthropic Messages stream events, so the reply is
 * read and stored as a Messages reply. The request is not a Messages body:
 * it is what the CLI backend needs to start or continue one CLI process, as
 * described at cc_build_request().
 */

#define FYAI_MODULE FYAIEM_UNKNOWN

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <string.h>

#include "fyai_buf.h"
#include "fyai_grammar.h"
#include "fyai_media.h"
#include "fyai_provider.h"
#include "utils.h"

#define G (&fyai_grammar_messages)

static fy_generic cc_input(struct fyai_ctx *ctx, fy_generic messages)
{
	return G->input(ctx, messages);
}

static fy_generic cc_make_tools(struct fyai_ctx *ctx)
{
	return G->make_tools(ctx);
}

static fy_generic cc_tool_def(struct fyai_ctx *ctx, fy_generic spec)
{
	return G->tool_def(ctx, spec);
}

static fy_generic cc_image_part(struct fyai_ctx *ctx, fy_generic part)
{
	return G->image_part(ctx, part);
}

static fy_generic cc_response(struct fyai_ctx *ctx, fy_generic doc)
{
	return G->response(ctx, doc);
}

static fy_generic cc_history(struct fyai_ctx *ctx, fy_generic wire,
			     fy_generic *lossesp)
{
	return G->history(ctx, wire, lossesp);
}

static int cc_append(struct fyai_buf *buf, const char *a, const char *b)
{
	if (fyai_buf_append(buf, a) || fyai_buf_append(buf, b ? b : ""))
		return -1;
	return fyai_buf_append(buf, "\n\n");
}

/* One stored message as transcript text; system messages give nothing. */
static int cc_render(struct fyai_ctx *ctx, struct fyai_buf *buf, fy_generic m)
{
	struct fy_generic_builder *gb = ctx->transient->gb;
	fy_generic type, role, text;
	const char *s;

	type = fy_get(m, "type", fy_invalid);
	role = fy_get(m, "role", fy_invalid);
	if (fy_equal(type, "function_call"))
		return cc_append(buf, fy_sprintfa("[assistant calls %s %s]\n",
				 fy_get(m, "name", ""),
				 fy_get(m, "arguments", "{}")), NULL);
	if (fy_equal(type, "shell_call")) {
		s = emit_json_string(gb, fy_get(m, "action", fy_map_empty));
		return cc_append(buf, "[assistant calls shell ", s ? s : "{}");
	}
	if (fyai_item_type_is_call_output(type)) {
		text = fyai_wire_tool_text(ctx, fy_get(m, "output", fy_invalid));
		if (!fy_is_string(text)) {
			s = emit_json_string(gb, text);
			text = fy_value(gb, s ? s : "");
		}
		return cc_append(buf, "[tool result]\n", fy_castp(&text, ""));
	}
	if (fy_equal(role, "system") || fy_equal(type, "reasoning"))
		return 0;
	text = fyai_media_model_text(gb, fy_get(m, "content", fy_invalid));
	if (!fy_is_string(text))
		text = fy_value(gb, "");
	if (fy_equal(role, "user"))
		return cc_append(buf, "[user]\n", fy_castp(&text, ""));
	if (fy_equal(role, "agent"))
		return cc_append(buf, "[sub-agent message]\n",
				 fy_castp(&text, ""));
	return cc_append(buf, "[assistant]\n", fy_castp(&text, ""));
}

/*
 * The identity of a stored message: its word in the arena. The arena keeps one
 * value for equal content, so a message has the same word in each request of a
 * process, before its turn is published and after. It is a name for this
 * process only, as the turn ids of the SDK front are (fyai_claude_sdk.c).
 */
static fy_generic cc_identity(struct fyai_ctx *ctx, fy_generic m)
{
	fy_generic v = fy_gb_internalize(ctx->arena->gb, m);

	return fy_stringf(ctx->transient->gb, "%llx",
			  (unsigned long long)v.v);
}

/* What a kept CLI needs to know of a stored message. */
static fy_generic cc_turn(struct fyai_ctx *ctx, fy_generic m)
{
	struct fy_generic_builder *gb = ctx->transient->gb;
	fy_generic role = fy_get(m, "role", fy_invalid), text;
	const char *kind;

	if (fy_equal(role, "user") || fy_equal(role, "agent"))
		kind = "user";
	else if (fy_equal(role, "assistant"))
		kind = "assistant";
	else if (fyai_item_type_is_call(fy_get(m, "type", fy_invalid)))
		kind = "call";
	else
		kind = "other";
	text = !strcmp(kind, "user") ?
	       fyai_media_model_text(gb, fy_get(m, "content", fy_invalid)) :
	       fy_value(gb, "");
	if (!fy_is_string(text))
		text = fy_value(gb, "");
	return fy_mapping(gb, "d", cc_identity(ctx, m), "kind", kind,
			  "text", text);
}

/*
 * The request of one model step:
 *
 *   model, system, tools, max_tokens
 *   history: the conversation before the last user message, as text
 *   prompt:  the last user message, as text or Messages content blocks,
 *            followed by the items after it when there are any
 *   results: the tool results at the end of the conversation,
 *            [{call_id, output, is_error}]
 *   turns:   each stored message but the system one, as {d, kind, text}:
 *            its identity, user|assistant|call|other, and the text of a
 *            user message
 *
 * A CLI process that waits for these results continues with them; a new
 * process starts from history and prompt. The CLI cannot take earlier turns
 * as structured messages, so a new process reads them as text.
 */
static fy_generic cc_build_request(struct fyai_ctx *ctx,
				   const struct fyai_grammar_request *req)
{
	struct fy_generic_builder *gb = ctx->transient->gb;
	struct fyai_cfg *cfg = ctx->cfg;
	struct fyai_buf history = {}, tail = {};
	fy_generic messages, m, results, result, prompt, content, output, turns;
	size_t n, i, last_user, tail_from;
	fy_generic request = fy_invalid;

	messages = req->messages;
	n = fy_len(messages);
	last_user = n;
	for (i = n; i > 0; i--) {
		if (fy_equal(fy_get(fy_get_at(messages, i - 1), "role"),
			     "user")) {
			last_user = i - 1;
			break;
		}
	}
	tail_from = n;
	while (tail_from > 0 &&
	       fyai_item_type_is_call_output(fy_get(fy_get_at(messages,
				tail_from - 1), "type")))
		tail_from--;

	results = fy_seq_empty;
	for (i = tail_from; i < n; i++) {
		m = fy_get_at(messages, i);
		output = fyai_wire_tool_text(ctx, fy_get(m, "output",
							 fy_invalid));
		if (!fy_is_string(output))
			output = fy_value(gb, emit_json_string(gb, output));
		result = fy_mapping(gb,
			"call_id", fy_get(m, "call_id", ""),
			"output", output);
		results = fy_append(gb, results, result);
	}

	turns = fy_seq_empty;
	for (i = 0; i < n; i++) {
		m = fy_get_at(messages, i);
		if (fy_equal(fy_get(m, "role"), "system"))
			continue;
		turns = fy_append(gb, turns, cc_turn(ctx, m));
	}
	for (i = 0; i < last_user && i < n; i++)
		if (cc_render(ctx, &history, fy_get_at(messages, i)))
			goto out;
	for (i = last_user + 1; i < n; i++)
		if (cc_render(ctx, &tail, fy_get_at(messages, i)))
			goto out;

	content = last_user < n ? fy_get(fy_get_at(messages, last_user),
					 "content", fy_invalid) : fy_invalid;
	if (fy_is_invalid(content))
		content = fy_value(gb, "");
	if (fyai_media_content_has_images(content))
		prompt = fyai_wire_content(ctx, content, "text");
	else
		prompt = fyai_media_model_text(gb, content);
	if (fy_is_invalid(prompt))
		goto out;

	request = fy_mapping(gb,
		"model", cfg->model ? cfg->model : "",
		"system", req->instructions ? req->instructions : "",
		"max_tokens", req->max_tokens,
		"tools", fy_is_valid(ctx->tools) ? ctx->tools : fy_seq_empty,
		"history", fy_value(gb, history.data ? history.data : ""),
		"prompt", prompt,
		"progress", fy_value(gb, tail.data ? tail.data : ""),
		"results", results,
		"turns", turns);
out:
	fyai_buf_free(&history);
	fyai_buf_free(&tail);
	return request;
}

/* The CLI is not reached over HTTP. */
static const struct fyai_grammar_header cc_headers[] = {
	{ NULL, NULL },
};

const struct fyai_grammar fyai_grammar_claude_code = {
	.mode			= FYAI_API_CLAUDE_CODE,
	.name			= "claude-code",
	.protocol		= "claude_code",
	.title			= "Claude Code CLI",
	.default_url		= "",
	.default_model		= "default",
	.default_provider	= "claude-code",
	.headers		= cc_headers,
	.input			= cc_input,
	.make_tools		= cc_make_tools,
	.tool_def		= cc_tool_def,
	.image_part		= cc_image_part,
	.build_request		= cc_build_request,
	.stream			= &fyai_grammar_messages,
	.response		= cc_response,
	.history		= cc_history,
	.cli			= true,
};
