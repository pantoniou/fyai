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

/* True when the last message of @out is the assistant's and holds tool_use @id. */
static bool messages_has_tool_use(fy_generic out, fy_generic id)
{
	fy_generic last, content, block;

	if (!fy_len(out) || !fy_is_string(id))
		return false;
	last = fy_get_at(out, fy_len(out) - 1);
	if (fy_not_equal(fy_get(last, "role"), "assistant"))
		return false;
	content = fy_get(last, "content", fy_seq_empty);
	fy_foreach(block, content)
		if (fy_equal(fy_get(block, "type"), "tool_use") &&
		    fy_equal(fy_get(block, "id"), id))
			return true;
	return false;
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

		/*
		 * Call items -> assistant tool_use. The block array of a
		 * paused turn holds its calls already.
		 */
		if (fyai_item_type_is_call(type) &&
		    messages_has_tool_use(out, fy_get(m, "call_id", fy_invalid)))
			continue;
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

/* The state of a streamed Messages reply. */
struct messages_stream {
	/* The server_tool_use blocks: {index, id, name, chunks}. */
	fy_generic hosted;
};

static struct messages_stream *messages_stream(struct fyai_stream_parse *p)
{
	return p->state;
}

static void messages_stream_init(struct fyai_stream_parse *p)
{
	messages_stream(p)->hosted = fy_seq_empty;
}

static fy_generic messages_tool_call(struct fyai_stream_parse *p,
				     size_t index)
{
	struct fy_generic_builder *gb;
	fy_generic tool_call;
	fy_generic function;
	fy_generic arguments;

	gb = p->gb;
	tool_call = fy_get(p->tool_calls, index);
	function = fy_get(tool_call, "function");
	arguments = fyai_join_strings(gb,
			fy_get(function, "argument_chunks", fy_seq_empty));
	return fy_mapping(gb,
		"type", "function_call",
		"call_id", fy_get(tool_call, "id", ""),
		"name", fy_get(tool_call, "name", ""),
		"arguments", arguments);
}

/*
 * Find the server_tool_use block with content index @index, or with the
 * block ID @id when @id is not NULL. Return its position or -1.
 */
static ssize_t messages_hosted_find(struct fyai_stream_parse *p,
				    long long index, const char *id)
{
	fy_generic call;
	size_t i;

	fy_foreach_idx_item(i, call, messages_stream(p)->hosted) {
		if (id ? fy_equal(fy_get(call, "id"), id) :
			 fy_get(call, "index", -1LL) == index)
			return (ssize_t)i;
	}
	return -1;
}

static int messages_hosted_append(struct fyai_stream_parse *p,
				  long long index, const char *text)
{
	struct fy_generic_builder *gb = p->gb;
	fy_generic call;
	ssize_t pos;

	pos = messages_hosted_find(p, index, NULL);
	call = fy_get(messages_stream(p)->hosted, (size_t)pos);
	call = fy_assoc(gb, call, "chunks",
			fy_append(gb, fy_get(call, "chunks", fy_seq_empty),
				  text));
	messages_stream(p)->hosted = fy_replace(gb, messages_stream(p)->hosted,
					  (size_t)pos, call);
	return fy_is_invalid(messages_stream(p)->hosted) ? -1 : 0;
}

/*
 * A web_search_tool_result block ends the server_tool_use block that it
 * names. Its content is a list of results, or an error object.
 */
static int messages_hosted_result(struct fyai_stream_parse *p,
				  fy_generic block)
{
	struct fy_generic_builder *gb = p->gb;
	fy_generic call, args, content, code, json;
	const char *id;
	ssize_t pos;
	bool ok;

	id = fy_get(block, "tool_use_id", "");
	pos = messages_hosted_find(p, -1, id);
	call = pos >= 0 ? fy_get(messages_stream(p)->hosted, (size_t)pos) :
			  fy_map_empty;
	json = fyai_join_strings(gb, fy_get(call, "chunks", fy_seq_empty));
	args = parse_json_string(gb, fy_castp(&json, ""));
	if (!fy_is_mapping(args))
		args = fy_map_empty;
	content = fy_get(block, "content");
	ok = !fy_is_mapping(content);
	code = fy_get(content, "error_code");
	return fyai_stream_hosted(p, "web_search", args, ok,
			ok ? NULL : fy_is_string(code) ? fy_castp(&code, "") :
							 "search failed");
}

/* message_start: the id and model of the reply, and the input usage. */
static int messages_message_start(struct fyai_stream_parse *p,
				  fy_generic event)
{
	fy_generic message, usage;

	message = fy_get(event, "message");
	p->metadata = fy_assoc(p->gb, p->metadata,
		"id", fy_get(message, "id", ""),
		"model", fy_get(message, "model", ""));
	usage = fy_get(message, "usage");
	if (fy_is_valid(usage))
		p->usage = usage;
	return 0;
}

/*
 * content_block_start: a server_tool_use block opens a hosted call, a
 * web_search_tool_result block ends one, and a tool_use block names the tool
 * slot of its index.
 */
static int messages_block_start(struct fyai_stream_parse *p, fy_generic event)
{
	struct messages_stream *ms = messages_stream(p);
	struct fy_generic_builder *gb = p->gb;
	fy_generic block, type, tool_call;
	long long index;

	block = fy_get(event, "content_block");
	type = fy_get(block, "type");
	index = fy_get(event, "index", 0LL);
	if (fy_equal(type, "server_tool_use")) {
		ms->hosted = fy_append(gb, ms->hosted,
			fy_mapping(gb, "index", index,
				   "id", fy_get(block, "id", ""),
				   "name", fy_get(block, "name", ""),
				   "chunks", fy_seq_empty));
		return fy_is_invalid(ms->hosted) ? -1 : 0;
	}
	if (fy_equal(type, "web_search_tool_result"))
		return messages_hosted_result(p, block);
	if (fy_not_equal(type, "tool_use"))
		return 0;
	if (index < 0 || fyai_stream_tool_slot(p, (size_t)index))
		return -1;
	tool_call = fy_get(p->tool_calls, (size_t)index);
	tool_call = fy_assoc(gb, tool_call,
		"id", fy_get(block, "id", ""),
		"name", fy_get(block, "name", ""));
	p->tool_calls = fy_replace(gb, p->tool_calls, (size_t)index, tool_call);
	return fy_is_invalid(p->tool_calls) ? -1 : 0;
}

/*
 * A fragment of the JSON input of the block at @index: of a hosted call, or
 * of the tool slot of that index.
 */
static int messages_input_delta(struct fyai_stream_parse *p, long long index,
				const char *text)
{
	struct fy_generic_builder *gb = p->gb;
	fy_generic tool_call, function, chunks;

	if (messages_hosted_find(p, index, NULL) >= 0)
		return messages_hosted_append(p, index, text);
	if (index < 0 || fyai_stream_tool_slot(p, (size_t)index))
		return -1;
	tool_call = fy_get(p->tool_calls, (size_t)index);
	function = fy_get(tool_call, "function");
	chunks = fy_append(gb, fy_get(function, "argument_chunks", fy_seq_empty),
			   text);
	function = fy_assoc(gb, function, "argument_chunks", chunks);
	tool_call = fy_assoc(gb, tool_call, "function", function);
	p->tool_calls = fy_replace(gb, p->tool_calls, (size_t)index, tool_call);
	return fy_is_invalid(p->tool_calls) ? -1 : 0;
}

/* content_block_delta: text, reasoning, or a fragment of a JSON input. */
static int messages_block_delta(struct fyai_stream_parse *p, fy_generic event)
{
	fy_generic delta, type;
	const char *text;

	delta = fy_get(event, "delta");
	type = fy_get(delta, "type");
	if (fy_equal(type, "text_delta")) {
		text = fy_get(delta, "text", "");
		return *text ? fyai_stream_content(p, text) : 0;
	}
	if (fy_equal(type, "thinking_delta")) {
		fyai_stream_reasoning(p, fy_get(delta, "thinking", ""));
		return 0;
	}
	if (fy_equal(type, "input_json_delta")) {
		text = fy_get(delta, "partial_json", "");
		return *text ? messages_input_delta(p,
				fy_get(event, "index", 0LL), text) : 0;
	}
	return 0;
}

/* content_block_stop: a complete tool_use block starts its call. */
static int messages_block_stop(struct fyai_stream_parse *p, fy_generic event)
{
	fy_generic tool_call;
	long long index;

	index = fy_get(event, "index", -1LL);
	if (index < 0 || (size_t)index >= fy_len(p->tool_calls))
		return 0;
	tool_call = fy_get(p->tool_calls, (size_t)index);
	if (!*fy_get(tool_call, "id", ""))
		return 0;
	return fyai_stream_tool_emit(p, (size_t)index,
				     messages_tool_call(p, (size_t)index));
}

/* message_delta: the stop reason and the output usage. */
static int messages_message_delta(struct fyai_stream_parse *p,
				  fy_generic event)
{
	struct fy_generic_builder *gb = p->gb;
	fy_generic reason, usage;
	const char *text;

	reason = fy_get_at_path(event, "delta", "stop_reason");
	text = fy_castp(&reason, "");
	if (*text)
		p->finish_reason = fy_value(gb, text);
	usage = fy_get(event, "usage");
	if (fy_is_valid(usage))
		p->usage = fy_assoc(gb,
			fy_is_valid(p->usage) ? p->usage : fy_map_empty,
			"output_tokens", fy_get(usage, "output_tokens", 0LL));
	return 0;
}

/*
 * Anthropic Messages SSE: content arrives as indexed block events. Text
 * deltas stream straight to the renderer; tool_use blocks use the tool slots
 * (indexed by content-block index, id/name at block start, input_json_delta
 * fragments accumulated as argument chunks). Usage arrives split across
 * message_start (input) and message_delta (output). There is no [DONE];
 * message_stop ends the stream. Other events, such as ping, are ignored.
 */
static int messages_stream_event(struct fyai_stream_parse *p,
				 fy_generic event)
{
	fy_generic type;

	type = fy_get(event, "type");
	if (fy_equal(type, "message_start"))
		return messages_message_start(p, event);
	if (fy_equal(type, "content_block_start"))
		return messages_block_start(p, event);
	if (fy_equal(type, "content_block_delta"))
		return messages_block_delta(p, event);
	if (fy_equal(type, "content_block_stop"))
		return messages_block_stop(p, event);
	if (fy_equal(type, "message_delta"))
		return messages_message_delta(p, event);
	if (fy_equal(type, "message_stop")) {
		fyai_stream_done(p);
		return 0;
	}
	if (fy_equal(type, "error")) {
		fyai_stream_fail(p, type, event);
		return -1;
	}
	return 0;
}

/*
 * Assemble a full Anthropic message document from the stream state so the
 * streamed path hands the engine the same shape a buffered /v1/messages
 * reply has: {id, model, role, content: [text?, tool_use...], stop_reason,
 * usage}. tool_use input is the parsed JSON of the accumulated
 * input_json_delta fragments.
 */
static fy_generic messages_stream_finish(struct fyai_stream_parse *p)
{
	struct fy_generic_builder *gb = p->gb;
	fy_generic content, tool_call, function, arguments, input, doc, text;

	content = fy_seq_empty;
	if (fy_len(p->content_chunks)) {
		text = fyai_join_strings(gb, p->content_chunks);
		content = fy_append(gb, content, fy_mapping(
				"type", "text",
				"text", text));
	}

	fy_foreach(tool_call, p->tool_calls) {
		/* skip slots never opened as tool_use blocks (text indexes) */
		if (!*fy_get(tool_call, "id", ""))
			continue;
		function = fy_get(tool_call, "function");
		arguments = fyai_join_strings(gb,
				fy_get(function, "argument_chunks", fy_seq_empty));
		input = parse_json_string(gb, fy_cast(arguments, "{}"));
		if (fy_is_invalid(input))
			input = fy_map_empty;
		content = fy_append(gb, content, fy_mapping(
				"type", "tool_use",
				"id", fy_get(tool_call, "id", ""),
				"name", fy_get(tool_call, "name", ""),
				"input", input));
	}

	doc = fy_mapping(
		"id", fy_get(p->metadata, "id", ""),
		"type", "message",
		"role", "assistant",
		"model", fy_get(p->metadata, "model", ""),
		"content", content,
		"stop_reason", fy_is_valid(p->finish_reason) ?
			p->finish_reason : fy_string("end_turn"));
	if (!fy_is_invalid(p->usage))
		doc = fy_assoc(doc, "usage", p->usage);

	if (fy_is_invalid(doc)) {
		fyai_error(p->ctx, "could not build the streamed message");
		return fy_invalid;
	}
	doc = fy_gb_internalize(gb, doc);
	if (fy_is_invalid(doc))
		fyai_error(p->ctx, "could not retain the streamed message");
	return doc;
}


static fy_generic messages_usage(struct fy_generic_builder *gb,
				 fy_generic usage)
{
	long long cached, cache_write;

	if (fy_is_invalid(usage))
		return fy_invalid;
	/* Anthropic reports cache traffic beside the input, not in it. */
	cached = fy_get(usage, "cache_read_input_tokens", 0LL);
	cache_write = fy_get(usage, "cache_creation_input_tokens", 0LL);
	return fy_mapping(gb,
		"input", fy_get(usage, "input_tokens", 0LL) + cached +
			 cache_write,
		"cached", cached,
		"cache_write", cache_write,
		"output", fy_get(usage, "output_tokens", 0LL),
		"reasoning", 0LL,
		"total", fy_get(usage, "total_tokens", 0LL),
		"cost", fy_get(usage, "cost", 0.0));
}

/*
 * Messages of content blocks as canonical messages: the words of a message,
 * then each tool_use block as a call item and each tool_result block as an
 * output item. `input` arrives as parsed JSON; it is encoded again so the
 * canonical `arguments` is a JSON string.
 */
static fy_generic messages_history(struct fyai_ctx *ctx, fy_generic wire,
				   fy_generic *lossesp)
{
	struct fy_generic_builder *gb = ctx->transient_gb;
	fy_generic out, m, role, content, text, block, type;
	const char *args;

	out = fy_seq_empty;
	fy_foreach(m, wire) {
		role = fy_get(m, "role", fy_invalid);
		if (!fy_is_string(role))
			role = fy_value(gb, "user");
		content = fy_get(m, "content", fy_invalid);
		if (fy_is_string(content)) {
			out = fy_append(gb, out, fy_mapping(gb,
					"role", role, "content", content));
			continue;
		}
		text = fyai_wire_parts_text(ctx, content, lossesp);
		if (!fy_empty(text))
			out = fy_append(gb, out, fy_mapping(gb,
					"role", role, "content", text));
		fy_foreach(block, content) {
			type = fy_get(block, "type", fy_invalid);
			if (fy_equal(type, "tool_use")) {
				args = emit_json_string(gb, fy_get(block,
						"input", fy_map_empty));
				out = fy_append(gb, out, fy_mapping(gb,
					"type", "function_call",
					"call_id", fy_get(block, "id", ""),
					"name", fy_get(block, "name", ""),
					"arguments", args ? args : "{}"));
			} else if (fy_equal(type, "tool_result")) {
				out = fy_append(gb, out, fy_mapping(gb,
					"type", "function_call_output",
					"call_id", fy_get(block, "tool_use_id", ""),
					"output", fyai_wire_output_text(ctx,
						fy_get(block, "content",
						       fy_invalid))));
			}
		}
	}
	return out;
}

/*
 * A turn that pause_turn ends keeps its content blocks in a message,
 * because a server tool continues only from the complete block array.
 */
static fy_generic messages_response(struct fyai_ctx *ctx, fy_generic doc)
{
	struct fy_generic_builder *gb = ctx->transient_gb;
	fy_generic content, history, calls, text, first, messages;
	bool paused, final;

	content = fy_get(doc, "content");
	history = messages_history(ctx, fy_sequence(gb, fy_mapping(gb,
					"role", "assistant",
					"content", fy_is_sequence(content) ?
						content : fy_seq_empty)),
				   NULL);
	calls = fyai_wire_calls(ctx, history);
	first = fy_get_at(history, 0);
	text = fy_is_string(fy_get(first, "role", fy_invalid)) ?
		fy_get(first, "content") : fy_value(gb, "");
	paused = fy_equal(fy_get(doc, "stop_reason"), "pause_turn");
	final = !paused && !fy_len(calls);

	messages = history;
	if (paused)
		messages = fy_concat(gb, fy_len(content) ?
			fy_sequence(gb, fy_mapping(gb, "role", "assistant",
						   "content", content)) :
			fy_seq_empty, calls);
	return fy_mapping(gb,
		"id", fy_get(doc, "id", ""),
		"text", text,
		"calls", calls,
		"messages", messages,
		"final", final,
		"usage", fyai_generic_or_null(messages_usage(gb,
						fy_get(doc, "usage"))),
		"wire", fy_is_sequence(content) ? content : fy_null,
		"doc", doc);
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
	.stream_state_size	= sizeof(struct messages_stream),
	.stream_init		= messages_stream_init,
	.stream_event		= messages_stream_event,
	.stream_finish		= messages_stream_finish,
	.response		= messages_response,
	.history		= messages_history,
};
