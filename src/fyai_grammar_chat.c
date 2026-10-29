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
	fy_generic tools;
	bool hosted_tool;

	request = fy_mapping(gb,
		"model", cfg->model,
		"messages", chat_input(ctx, req->messages));
	if (req->temperature)
		request = fy_assoc(gb, request, "temperature", cfg->temperature);
	if (!fy_str_empty(cfg->reasoning_effort))
		request = fy_assoc(gb, request, "reasoning_effort",
				   cfg->reasoning_effort);
	/*
	 * An endpoint that names a tool type for its search declares it as a
	 * tool beside the function tools; the others take a request option.
	 */
	tools = fy_is_sequence(ctx->tools) ? ctx->tools : fy_seq_empty;
	hosted_tool = false;
	if (fyai_provider_native_web_search(cfg)) {
		hosted_tool = cfg->web_search_type != NULL;
		if (hosted_tool)
			tools = fy_append(gb, tools,
					  fy_mapping(gb, "type",
						     cfg->web_search_type));
		else
			request = fy_assoc(gb, request, "web_search_options",
					   fy_map_empty);
	}
	if (hosted_tool && !fyai_grammar_tools_wanted(ctx))
		request = fy_assoc(gb, request, "tools", tools);
	if (fyai_grammar_tools_wanted(ctx)) {
		request = fy_assoc(gb, request, "tools", tools,
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

/* The state of a streamed Chat Completions reply. */
struct chat_stream {
	fy_generic logprob_content;
	fy_generic logprob_refusal;
};

static struct chat_stream *chat_stream(struct fyai_stream_parse *p)
{
	return p->state;
}

static void chat_stream_init(struct fyai_stream_parse *p)
{
	chat_stream(p)->logprob_content = fy_seq_empty;
	chat_stream(p)->logprob_refusal = fy_invalid;
}

static int chat_tool_delta(struct fyai_stream_parse *p,
			   fy_generic delta_tool_call)
{
	struct fy_generic_builder *gb = p->gb;
	fy_generic tool_call;
	fy_generic function, delta_function;
	fy_generic chunks;
	const char *text;
	long long index;

	index = fy_get(delta_tool_call, "index", 0LL);
	if (index < 0)
		return -1;

	if (fyai_stream_tool_slot(p, (size_t)index))
		return -1;

	tool_call = fy_get(p->tool_calls, (size_t)index);
	function = fy_get(tool_call, "function");
	delta_function = fy_get(delta_tool_call, "function");

	text = fy_get(delta_tool_call, "id", "");
	if (*text)
		tool_call = fy_assoc(gb, tool_call, "id", text);

	text = fy_get(delta_tool_call, "type", "");
	if (*text)
		tool_call = fy_assoc(gb, tool_call, "type", text);

	text = fy_get(delta_function, "name", "");
	if (*text) {
		function = fy_get(tool_call, "function");
		chunks = fy_get(function, "name_chunks", fy_seq_empty);
		chunks = fy_append(gb, chunks, text);
		function = fy_assoc(gb, function, "name_chunks", chunks);
		tool_call = fy_assoc(gb, tool_call, "function", function);
	}

	text = fy_get(delta_function, "arguments", "");
	if (*text) {
		function = fy_get(tool_call, "function");
		chunks = fy_get(function, "argument_chunks", fy_seq_empty);
		chunks = fy_append(gb, chunks, text);
		function = fy_assoc(gb, function, "argument_chunks", chunks);
		tool_call = fy_assoc(gb, tool_call, "function", function);
	}

	p->tool_calls = fy_replace(gb, p->tool_calls, (size_t)index,
				   tool_call);
	return fy_is_invalid(p->tool_calls) ? -1 : 0;
}

/* The call of tool slot @index as a call item. */
static fy_generic chat_tool_call(struct fyai_stream_parse *p, size_t index)
{
	struct fy_generic_builder *gb = p->gb;
	fy_generic tool_call;
	fy_generic function;

	tool_call = fy_get(p->tool_calls, index);
	function = fy_get(tool_call, "function");
	return fy_mapping(gb,
		"type", "function_call",
		"call_id", fy_get(tool_call, "id", ""),
		"name", fyai_join_strings(gb,
				fy_get(function, "name_chunks", fy_seq_empty)),
		"arguments", fyai_join_strings(gb,
				fy_get(function, "argument_chunks", fy_seq_empty)));
}

static int chat_stream_event(struct fyai_stream_parse *p, fy_generic chunk)
{
	struct chat_stream *cs = chat_stream(p);
	struct fy_generic_builder *gb = p->gb;
	struct fyai_cfg *cfg = p->ctx->cfg;
	fy_generic choice, delta, tool_calls, tool_call, usage, logprobs;
	fy_generic content, refusal;
	const char *text;
	size_t i;

	/* Handle an error object that arrives with HTTP status 200. */
	if (fy_is_valid(fy_get(chunk, "error"))) {
		fyai_stream_fail(p, fy_get(chunk, "type"), chunk);
		return -1;
	}

	usage = fy_get(chunk, "usage");
	if (!fy_is_invalid(usage))
		p->usage = usage;

	p->metadata = fy_assoc(gb, p->metadata,
		"id", fy_get(chunk, "id", ""),
		"created", fy_get(chunk, "created", 0LL),
		"model", fy_get(chunk, "model", ""),
		"service_tier", fy_get(chunk, "service_tier", ""),
		"system_fingerprint", fy_get(chunk, "system_fingerprint", ""));

	choice = fy_get_at_path(chunk, "choices", 0);
	if (fy_is_invalid(choice))
		return 0;

	logprobs = fy_get(choice, "logprobs");
	if (!fy_is_invalid(logprobs) && !fy_is_null(logprobs)) {
		content = fy_get(logprobs, "content");
		if (!fy_is_invalid(content)) {
			cs->logprob_content = fy_concat(gb,
					cs->logprob_content, content);
			if (cfg->token_extents &&
			    fy_is_valid(p->token_extents)) {
				p->token_extents =
					fyai_token_extents_append(gb,
						p->token_extents, content,
						&p->extents_pos);
				p->extents_lp = true;
			}
		}
		refusal = fy_get(logprobs, "refusal");
		if (!fy_is_invalid(refusal))
			cs->logprob_refusal = refusal;
	}

	text = fy_get(choice, "finish_reason", "");
	if (*text)
		p->finish_reason = fy_value(gb, text);

	delta = fy_get(choice, "delta");

	/*
	 * Some Chat Completions providers (deepseek, openrouter) stream a
	 * reasoning trace alongside the answer under `reasoning_content`. Show
	 * it live; it is provider wire detail, not part of the answer.
	 */
	text = fy_get(delta, "reasoning_content", "");
	if (*text)
		fyai_stream_reasoning(p, text);

	text = fy_get(delta, "content", "");
	if (*text && fyai_stream_content(p, text))
		return -1;

	tool_calls = fy_get(delta, "tool_calls");
	fy_foreach(tool_call, tool_calls) {
		if (chat_tool_delta(p, tool_call))
			return -1;
	}
	if (!fy_equal(p->finish_reason, "tool_calls"))
		return 0;
	for (i = 0; i < fy_len(p->tool_calls); i++)
		if (fyai_stream_tool_emit(p, i, chat_tool_call(p, i)))
			return -1;
	return 0;
}

/* The streamed reply as the document a buffered reply is. */
static fy_generic chat_stream_finish(struct fyai_stream_parse *p)
{
	struct chat_stream *cs = chat_stream(p);
	struct fy_generic_builder *gb = p->gb;
	struct fyai_cfg *cfg = p->ctx->cfg;
	fy_generic message, choice, doc, tool_calls, tool_call, function;

	tool_calls = fy_seq_empty;
	fy_foreach(tool_call, p->tool_calls) {
		function = fy_get(tool_call, "function");
		tool_calls = fy_append(gb, tool_calls, fy_mapping(gb,
			"id", fy_get(tool_call, "id", ""),
			"type", fy_get(tool_call, "type", "function"),
			"function", fy_mapping(gb,
				"name", fyai_join_strings(gb,
					fy_get(function, "name_chunks", fy_seq_empty)),
				"arguments", fyai_join_strings(gb,
					fy_get(function, "argument_chunks", fy_seq_empty)))));
	}

	message = fy_mapping("role", "assistant");
	if (fy_len(p->content_chunks))
		message = fy_assoc(message,
			"content", fyai_join_strings(gb, p->content_chunks));
	if (!fy_len(tool_calls))
		message = fy_assoc(message,
			"refusal", fy_null,
			"annotations", fy_seq_empty);
	else
		message = fy_assoc(message, "tool_calls", tool_calls);

	choice = fy_mapping(
		"index", 0,
		"message", message,
		"logprobs", cfg->logprobs ?
			fy_mapping(
				"content", cs->logprob_content,
				"refusal", fyai_generic_or_null(cs->logprob_refusal)) :
			fy_null,
		"finish_reason", fy_is_valid(p->finish_reason) ?
			p->finish_reason : fy_string("stop"));

	doc = fy_mapping(
		"id", fy_get(p->metadata, "id", ""),
		"object", "chat.completion",
		"created", fy_get(p->metadata, "created", 0LL),
		"model", fy_get(p->metadata, "model", ""),
		"choices", fy_sequence(choice));
	if (!fy_is_invalid(p->usage))
		doc = fy_assoc(doc, "usage", p->usage);
	doc = fy_assoc(doc,
		"service_tier", fy_get(p->metadata, "service_tier", ""),
		"system_fingerprint", fy_get(p->metadata,
					     "system_fingerprint", ""));
	return fy_gb_internalize(gb, doc);
}

static fy_generic chat_usage(struct fy_generic_builder *gb, fy_generic usage)
{
	fy_generic in, out;

	if (fy_is_invalid(usage))
		return fy_invalid;
	in = fy_get(usage, "prompt_tokens_details");
	out = fy_get(usage, "completion_tokens_details");
	return fy_mapping(gb,
		"input", fy_get(usage, "prompt_tokens", 0LL),
		"cached", fy_get(in, "cached_tokens", 0LL),
		"cache_write", fy_get(in, "cache_write_tokens", 0LL),
		"output", fy_get(usage, "completion_tokens", 0LL),
		"reasoning", fy_get(out, "reasoning_tokens", 0LL),
		"total", fy_get(usage, "total_tokens", 0LL),
		"cost", fy_get(usage, "cost", 0.0));
}

/*
 * Chat messages as canonical messages: the words of a message, then each of
 * its calls as a call item; a tool message is an output item. An assistant
 * message with calls and no words keeps no text message.
 */
static fy_generic chat_history(struct fyai_ctx *ctx, fy_generic wire,
			       fy_generic *lossesp)
{
	struct fy_generic_builder *gb = ctx->transient_gb;
	fy_generic out, m, role, text, calls, call;

	out = fy_seq_empty;
	fy_foreach(m, wire) {
		role = fy_get(m, "role", fy_invalid);
		if (!fy_is_string(role))
			role = fy_value(gb, "user");
		if (fy_equal(role, "tool")) {
			out = fy_append(gb, out, fy_mapping(gb,
				"type", "function_call_output",
				"call_id", fy_get(m, "tool_call_id", ""),
				"output", fyai_wire_output_text(ctx,
					fy_get(m, "content", fy_invalid))));
			continue;
		}
		text = fyai_wire_parts_text(ctx,
			fy_get(m, "content", fy_seq_empty), lossesp);
		calls = fy_get(m, "tool_calls", fy_invalid);
		if (!fy_is_sequence(calls) || !fy_empty(text))
			out = fy_append(gb, out, fy_mapping(gb,
					"role", role, "content", text));
		fy_foreach(call, calls) {
			out = fy_append(gb, out, fy_mapping(gb,
				"type", "function_call",
				"call_id", fy_get(call, "id", ""),
				"name", fy_get_at_path(call, "function", "name"),
				"arguments", fy_get_at_path(call, "function",
							    "arguments")));
		}
	}
	return out;
}

/* The message is the wire record of the turn. */
static fy_generic chat_response(struct fyai_ctx *ctx, fy_generic doc)
{
	struct fy_generic_builder *gb = ctx->transient_gb;
	fy_generic message, content, text, calls, messages;
	bool final;

	message = fy_get_at_path(doc, "choices", 0, "message");
	content = fy_get(message, "content");
	text = fy_is_string(content) ? content : fy_value(gb, "");
	messages = chat_history(ctx, fy_sequence(gb, message), NULL);
	calls = fyai_wire_calls(ctx, messages);
	final = !fy_len(calls);
	return fy_mapping(gb,
		"id", fy_get(doc, "id", ""),
		"text", text,
		"calls", calls,
		"messages", messages,
		"final", final,
		"usage", fyai_generic_or_null(chat_usage(gb,
						fy_get(doc, "usage"))),
		"wire", fy_is_mapping(message) ?
			fy_sequence(gb, message) : fy_null,
		"doc", doc);
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
	.stream_state_size	= sizeof(struct chat_stream),
	.stream_init		= chat_stream_init,
	.stream_event		= chat_stream_event,
	.stream_finish		= chat_stream_finish,
	.response		= chat_response,
	.history		= chat_history,
	.reasoning		= true,
	.logprobs		= true,
};
