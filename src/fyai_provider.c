/*
 * fyai_provider.c - provider wire translation
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <string.h>

#include "fyai_provider.h"

fy_generic fyai_response_output_text(struct fyai_ctx *ctx,
					    fy_generic response_doc)
{
	struct fyai_cfg *cfg = ctx->cfg;
	fy_generic content;
	fy_generic chunks;
	fy_generic output;
	fy_generic item;
	fy_generic part;
	fy_generic out;
	const char *text;

	switch (cfg->api_mode) {
	case FYAI_API_RESPONSES:
		text = fy_get(response_doc, "output_text", "");
		if (*text) {
			out = fy_value(text);
			break;
		}

		chunks = fy_seq_empty;
		output = fy_get(response_doc, "output");
		fy_foreach(item, output) {
			content = fy_get(item, "content");
			fy_foreach(part, content) {
				if (fy_not_equal(fy_get(part, "type"), "output_text"))
					continue;
				chunks = fy_append(chunks, fy_get(part, "text", ""));
			}
		}

		out = fyai_join_strings(ctx->transient_gb, chunks);
		break;

	case FYAI_API_CHAT_COMPLETIONS:
		out = response_content(ctx->transient_gb, response_doc);
		break;

	case FYAI_API_MESSAGES:
		/* Anthropic: join the text content blocks. */
		chunks = fy_seq_empty;
		content = fy_get(response_doc, "content");
		fy_foreach(part, content) {
			if (fy_not_equal(fy_get(part, "type"), "text"))
				continue;
			chunks = fy_append(chunks, fy_get(part, "text", ""));
		}
		out = fyai_join_strings(ctx->transient_gb, chunks);
		break;

	default:
		assert(0);
		__builtin_unreachable();
		break;
	}

	out = fy_gb_internalize(ctx->transient_gb, out);
	return assert_valid_generic(out, "fail fyai_response_output_text");
}

fy_generic fyai_response_tool_calls(struct fyai_ctx *ctx,
					   fy_generic response_doc)
{
	struct fyai_cfg *cfg = ctx->cfg;
	fy_generic tool_calls;
	fy_generic output;
	fy_generic item;
	fy_generic tmp;
	fy_generic type;
	const char *args;

	switch (cfg->api_mode) {
	case FYAI_API_RESPONSES:
		tool_calls = fy_seq_empty;
		output = fy_get(response_doc, "output");
		fy_foreach(item, output) {
			type = fy_get(item, "type");
			if (fy_equal(type, "function_call") ||
			    fy_equal(type, "shell_call"))
				tool_calls = fy_append(tool_calls, item);
		}
		break;

	case FYAI_API_CHAT_COMPLETIONS:
		tool_calls = response_tool_calls(ctx->transient_gb, response_doc);
		break;

	case FYAI_API_MESSAGES:
		/*
		 * Normalize Anthropic tool_use content blocks into
		 * Responses-style function_call items right here at the parse
		 * boundary, so the tool loop, canonical storage, display and
		 * the cross-provider translators all see the shape they
		 * already understand. `input` arrives as parsed JSON;
		 * re-encode it so canonical `arguments` stays a JSON string
		 * like the other modes.
		 */
		tool_calls = fy_seq_empty;
		output = fy_get(response_doc, "content");
		fy_foreach(item, output) {
			if (fy_not_equal(fy_get(item, "type"), "tool_use"))
				continue;
			args = emit_json_string(ctx->transient_gb,
					fy_get(item, "input", fy_map_empty));
			tmp = fy_mapping(
				"type", "function_call",
				"call_id", fy_get(item, "id", ""),
				"name", fy_get(item, "name", ""),
				"arguments", args ? args : "{}");
			tool_calls = fy_append(tool_calls, tmp);
		}
		break;

	default:
		assert(0);
		__builtin_unreachable();
		break;

	}

	/*
	 * A plain answer carries no tool_calls key, so the lookup yields an
	 * invalid generic; normalise it to an empty sequence (callers count it
	 * with fy_len()). Only internalize a real result.
	 */
	if (fy_generic_is_invalid(tool_calls) || fy_generic_is_null_type(tool_calls))
		return fy_seq_empty;

	tool_calls = fy_gb_internalize(ctx->transient_gb, tool_calls);
	return assert_valid_generic(tool_calls, NULL);
}

fy_generic fyai_response_id(struct fyai_ctx *ctx, fy_generic response_doc)
{
	fy_generic v;
	const char *id;

	id = fy_get(response_doc, "id", "");
	if (!*id)
		return fy_invalid;
	v = fy_value(id);

	v = fy_gb_internalize(ctx->transient_gb, v);
	return assert_valid_generic(v, NULL);
}

bool fyai_response_needs_tool_calls(struct fyai_ctx *ctx,
					   fy_generic response_doc)
{
	return fy_len(fyai_response_tool_calls(ctx, response_doc)) > 0;
}

bool fyai_response_is_final(struct fyai_ctx *ctx, fy_generic response_doc)
{
	return !fyai_response_needs_tool_calls(ctx, response_doc);
}

fy_generic fyai_extract_usage(struct fyai_ctx *ctx, fy_generic doc)
{
	struct fyai_cfg *cfg = ctx->cfg;
	fy_generic out_details;
	fy_generic in_details;
	fy_generic usage;
	fy_generic out;
	long long cache_write;
	long long reasoning;
	long long cached;
	long long output;
	long long input;
	long long total;
	double cost;

	usage = fy_get(doc, "usage");
	if (fy_generic_is_invalid(usage))
		return fy_invalid;

	switch (cfg->api_mode) {
	case FYAI_API_RESPONSES:
		in_details = fy_get(usage, "input_tokens_details");
		out_details = fy_get(usage, "output_tokens_details");
		input = fy_get(usage, "input_tokens", 0LL);
		output = fy_get(usage, "output_tokens", 0LL);
		break;

	case FYAI_API_CHAT_COMPLETIONS:
		in_details = fy_get(usage, "prompt_tokens_details");
		out_details = fy_get(usage, "completion_tokens_details");
		input = fy_get(usage, "prompt_tokens", 0LL);
		output = fy_get(usage, "completion_tokens", 0LL);
		break;

	case FYAI_API_MESSAGES:
		in_details = out_details = fy_invalid;
		input = fy_get(usage, "input_tokens", 0LL);
		output = fy_get(usage, "output_tokens", 0LL);
		break;

	default:
		assert(0);
		__builtin_unreachable();
		break;
	}

	cached = fy_get(in_details, "cached_tokens", 0LL);
	cache_write = fy_get(in_details, "cache_write_tokens", 0LL);
	reasoning = fy_get(out_details, "reasoning_tokens", 0LL);
	/* Anthropic reports cache traffic at the usage top level. */
	if (cfg->api_mode == FYAI_API_MESSAGES) {
		cached = fy_get(usage, "cache_read_input_tokens", 0LL);
		cache_write = fy_get(usage, "cache_creation_input_tokens", 0LL);
	}
	total = fy_get(usage, "total_tokens", 0LL);
	if (!total)
		total = input + output;
	/* Some providers (e.g. OpenRouter) report a per-call dollar cost. */
	cost = fy_get(usage, "cost", 0.0);

	out = fy_mapping(
		"input", input,
		"cached", cached,
		"cache_write", cache_write,
		"output", output,
		"reasoning", reasoning,
		"total", total,
		"cost", cost);

	out = fy_gb_internalize(ctx->transient_gb, out);
	return assert_valid_generic(out, "Unable to extract usage");
}

/*
 * Append one logprobs batch's per-token extents to @extents as {text, pos, lp}
 * mappings (keys <=7 bytes: they store inline in the generic), advancing
 * *posp by each token's byte length. When the entry carries a `bytes` array
 * it is authoritative for the length - the `token` string is lossy for tokens
 * that are not valid UTF-8 - so `pos` stays a correct byte offset into the
 * joined content even then, and may fall inside a multibyte sequence.
 */
fy_generic fyai_token_extents_append(struct fy_generic_builder *gb,
				     fy_generic extents, fy_generic entries,
				     size_t *posp)
{
	fy_generic entry;
	fy_generic bytes;
	fy_generic ext;
	const char *text;
	size_t len;
	double lp;

	fy_foreach(entry, entries) {
		text = fy_get(entry, "token", "");
		bytes = fy_get(entry, "bytes");
		if (fy_generic_is_valid(bytes) &&
		    !fy_generic_is_null_type(bytes))
			len = fy_len(bytes);
		else
			len = strlen(text);
		if (!len)
			continue;
		lp = fy_get(entry, "logprob", 0.0);
		ext = fy_mapping(gb,
			"text", text,
			"pos", (long long)*posp,
			"lp", lp);
		extents = fy_append(gb, extents, ext);
		if (fy_generic_is_invalid(extents))
			return fy_invalid;
		*posp += len;
	}

	return extents;
}

/*
 * Fallback extents when no per-token delimitation exists (Anthropic Messages,
 * providers that ignore logprobs): one {text, pos} entry per streamed content
 * chunk, positions being the running byte offsets of the joined chunks.
 */
fy_generic fyai_chunk_extents(struct fy_generic_builder *gb, fy_generic chunks)
{
	fy_generic extents;
	fy_generic chunk;
	const char *text;
	size_t pos;

	extents = fy_seq_empty;
	pos = 0;
	fy_foreach(chunk, chunks) {
		text = fy_castp(&chunk, "");
		if (!*text)
			continue;
		extents = fy_append(gb, extents,
				fy_mapping(gb,
					"text", text,
					"pos", (long long)pos));
		if (fy_generic_is_invalid(extents))
			return fy_invalid;
		pos += strlen(text);
	}

	return extents;
}

fy_generic fyai_make_responses_tools(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg = ctx->cfg;
	fy_generic response_tools;
	fy_generic response_tool;
	fy_generic function;
	fy_generic tools;
	fy_generic tool;

	response_tools = fy_seq_empty;

	if (cfg->enable_tools) {
		tools = make_tools(ctx->gb);
		fy_foreach(tool, tools) {
			function = fy_get(tool, "function");
			if (cfg->enable_builtin_shell &&
			    fy_equal(fy_get(function, "name"), "shell"))
				continue;
			response_tool = fy_mapping(
				"type", "function",
				"name", fy_get(function, "name", ""),
				"description", fy_get(function, "description", ""),
				"parameters", fy_get(function, "parameters"));
			response_tools = fy_append(response_tools, response_tool);
		}
	}

	if (cfg->enable_builtin_shell) {
		response_tool = fy_mapping(
				"type", "shell",
				"environment", fy_mapping("type", "local"));
		response_tools = fy_append(response_tools, response_tool);
	}

	response_tools = fy_gb_internalize(ctx->gb, response_tools);
	return assert_valid_generic(response_tools, "Unable to make tools");
}

fy_generic fyai_make_messages_tools(struct fyai_ctx *ctx)
{
	fy_generic messages_tools;
	fy_generic function;
	fy_generic tools;
	fy_generic tool;
	fy_generic tmp;

	/* Anthropic tools are flat: {name, description, input_schema} - no
	 * "function" wrapper, and the JSON schema key is input_schema. */
	messages_tools = fy_seq_empty;
	tools = make_tools(ctx->gb);
	fy_foreach(tool, tools) {
		function = fy_get(tool, "function");
		tmp = fy_mapping("name", fy_get(function, "name", ""),
				 "description", fy_get(function, "description", ""),
				 "input_schema", fy_get(function, "parameters"));
		messages_tools = fy_append(messages_tools, tmp);
	}

	messages_tools = fy_gb_internalize(ctx->gb, messages_tools);
	return assert_valid_generic(messages_tools, "Unable to make messages tools");
}

fy_generic fyai_responses_input(struct fyai_ctx *ctx, fy_generic messages)
{
	fy_generic type;
	fy_generic role;
	fy_generic tool_calls;
	fy_generic content;
	fy_generic input;
	fy_generic m;
	fy_generic fn;
	fy_generic tc;
	fy_generic tmp;

	input = fy_seq_empty;

	fy_foreach(m, messages) {
		type = fy_get(m, "type");

		/*
		 * A stored `reasoning` item references server-side state by id.
		 * With store=false that item is not persisted, so replaying it
		 * in `input` 404s ("Item with id rs_... not found"). Reasoning
		 * is non-canonical anyway, so never replay it.
		 */
		if (fy_equal(type, "reasoning"))
			continue;

		/* Other native Responses items (function_call,
		 * function_call_output, message, ...): pass through. */
		if (fy_generic_is_string(type)) {
			input = fy_append(ctx->transient_gb, input, m);
			continue;
		}

		role = fy_get(m, "role");
		content = fy_get(m, "content");
		tool_calls = fy_get(m, "tool_calls");

		/* The system prompt is surfaced via the request `instructions`
		 * field, so drop the canonical system turn from `input` to
		 * avoid sending it twice. */
		if (fy_equal(role, "system"))
			continue;

		/* Chat tool result -> function_call_output item. */
		if (fy_equal(role, "tool")) {
			input = fy_append(ctx->transient_gb, input,
					fy_mapping(
						"type", "function_call_output",
						"call_id", fy_get(m, "tool_call_id", ""),
						"output",
						fy_generic_is_valid(content) && !fy_generic_is_null_type(content) ?
							content :
							fy_value("")));
			continue;
		}

		/* Chat assistant tool request -> one function_call item per
		 * call; the accompanying null content is dropped, any real
		 * prose is preserved as a leading assistant message. */
		if (fy_equal(role, "assistant") &&
		    fy_generic_is_valid(tool_calls) &&
		    !fy_generic_is_null_type(tool_calls)) {
			if (fy_generic_is_valid(content) &&
			    !fy_generic_is_null_type(content))
				input = fy_append(ctx->transient_gb, input,
						fy_mapping(
							"role", "assistant",
							"content", content));

			fy_foreach(tc, tool_calls) {
				fn = fy_get(tc, "function");
			       	tmp = fy_mapping("type", "function_call",
						 "call_id", fy_get(tc, "id", ""),
						 "name", fy_get(fn, "name", ""),
						 "arguments", fy_get(fn, "arguments", ""));
				input = fy_append(ctx->transient_gb, input, tmp);
			}
			continue;
		}

		/* Plain role message: Responses rejects null content. */
		if (fy_generic_is_invalid(content) ||
		    fy_generic_is_null_type(content))
			content = fy_value("");
		tmp = fy_mapping("role", fy_generic_is_string(role) ? role : fy_value("user"),
				 "content", content);
		input = fy_append(ctx->transient_gb, input, tmp);
	}

	input = fy_gb_internalize(ctx->transient_gb, input);
	return assert_valid_generic(input, "unable to create responses input");
}

fy_generic fyai_item_text(struct fyai_ctx *ctx, fy_generic item)
{
	fy_generic content, chunks;
	fy_generic part;
	fy_generic t;

	content = fy_get(item, "content");
	chunks = fy_seq_empty;
	if (fy_generic_is_string(content))
		return content;

	fy_foreach(part, content) {
		t = fy_get(part, "type");

		if (fy_not_equal(t, "output_text") && fy_not_equal(t, "text"))
			continue;

		chunks = fy_append(ctx->transient_gb, chunks, fy_get(part, "text", ""));
	}
	return fyai_join_strings(ctx->transient_gb, chunks);
}

fy_generic fyai_chat_input(struct fyai_ctx *ctx, fy_generic messages)
{
	fy_generic out = fy_seq_empty;
	fy_generic pending = fy_invalid;	/* accumulated tool_calls */
	fy_generic tmp;
	size_t i, count;
	fy_generic m, call, function;
	fy_generic type;
	const char *cmd;
	const char *args;
	bool is_call;


	count = fy_len(messages);
	/* One extra iteration to flush a trailing run of call items. */
	for (i = 0; i <= count; i++) {

		m = i < count ? fy_get(messages, i) : fy_invalid;
		type = i < count ? fy_get(m, "type") : fy_invalid;
		is_call = fy_equal(type, "function_call") ||
			  fy_equal(type, "shell_call");

		if (fy_generic_is_valid(pending) && !is_call) {
			tmp = fy_mapping("role", "assistant",
					 "content", fy_null,
					 "tool_calls", pending);
			out = fy_append(ctx->transient_gb, out, tmp);
			pending = fy_invalid;
		}

		if (i == count)
			break;

		if (is_call) {
			if (fy_equal(type, "shell_call")) {
				cmd = fy_cast(fy_get_at_path(m, "action", "commands", 0), "");
				args = NULL;

				args = emit_json_string(ctx->transient_gb, fy_mapping("command", cmd));
				function = fy_mapping(
						"name", "shell",
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
			pending = fy_generic_is_valid(pending) ?
					fy_append(ctx->transient_gb, pending, call) :
					fy_sequence(ctx->transient_gb, call);
			continue;
		}

		if (fy_equal(type, "function_call_output") ||
		    fy_equal(type, "shell_call_output")) {
			tmp = fy_mapping("role", "tool",
					 "tool_call_id", fy_get(m, "call_id", ""),
					 "content", fy_get(m, "output", ""));
			out = fy_append(ctx->transient_gb, out, tmp);
			continue;
		}

		if (fy_equal(type, "message")) {
			tmp = fy_mapping("role", fy_get(m, "role", "assistant"),
					 "content", fyai_item_text(ctx, m));
			out = fy_append(ctx->transient_gb, out, tmp);
			continue;
		}

		/* reasoning and other native items: no Chat analogue. */
		if (fy_generic_is_string(type))
			continue;

		/* Already Chat-shaped. */
		out = fy_append(ctx->transient_gb, out, m);
	}

	out = fy_gb_internalize(ctx->transient_gb, out);
	return assert_valid_generic(out, NULL);
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
 * detail with no Messages analogue and are dropped.
 */
fy_generic fyai_messages_input(struct fyai_ctx *ctx, fy_generic messages)
{
	struct fy_generic_builder *gb = ctx->transient_gb;
	fy_generic out = fy_seq_empty;
	fy_generic m, tool_calls, tc, fn, content, input, output;
	fy_generic type, role;
	fy_generic tmp;
	const char *cmd, *text;
	size_t j, n;

	fy_foreach(m, messages) {
		type = fy_get(m, "type");

		/* Responses-style native call items -> assistant tool_use. */
		if (fy_equal(type, "function_call")) {
			input = parse_json_string(gb, fy_get(m, "arguments", "{}"));
			if (fy_generic_is_invalid(input))
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
					 "name", "shell",
					 "input", fy_mapping("command", cmd));
			out = messages_append_block(ctx, out, "assistant", tmp);
			continue;
		}

		/* Native output items -> user tool_result. */
		if (fy_equal(type, "function_call_output") ||
		    fy_equal(type, "shell_call_output")) {
			output = fy_get(m, "output");
			if (!fy_generic_is_string(output)) {
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
		if (fy_generic_is_string(type))
			continue;

		role = fy_get(m, "role");

		/* The system prompt travels in the request `system` field. */
		if (fy_equal(role, "system"))
			continue;

		/* Chat tool result -> user tool_result. */
		if (fy_equal(role, "tool")) {
			content = fy_get(m, "content");
			if (fy_generic_is_invalid(content) ||
			    fy_generic_is_null_type(content))
				content = fy_value("");
			tmp = fy_mapping("type", "tool_result",
					 "tool_use_id", fy_get(m, "tool_call_id", ""),
					 "content", content);
			out = messages_append_block(ctx, out, "user", tmp);
			continue;
		}

		/* Chat assistant tool request -> text (if any) + tool_use. */
		tool_calls = fy_get(m, "tool_calls");
		if (fy_equal(role, "assistant") &&
		    fy_generic_is_valid(tool_calls) &&
		    !fy_generic_is_null_type(tool_calls)) {
			content = fy_get(m, "content");
			if (*fy_cast(content, ""))
				out = messages_append_block(ctx, out,
						"assistant",
						messages_text_block(ctx, content));
			fy_foreach(tc, tool_calls) {
				fn = fy_get(tc, "function");
				input = parse_json_string(gb,
						fy_get(fn, "arguments", "{}"));
				if (fy_generic_is_invalid(input))
					input = fy_map_empty;
				tmp = fy_mapping("type", "tool_use",
						 "id", fy_get(tc, "id", ""),
						 "name", fy_get(fn, "name", ""),
						 "input", input);
				out = messages_append_block(ctx, out,
						"assistant", tmp);
			}
			continue;
		}

		/* Plain role message. */
		content = fy_get(m, "content");
		if (fy_generic_is_invalid(content) ||
		    fy_generic_is_null_type(content))
			content = fy_value("");
		out = messages_append_block(ctx, out,
				fy_generic_is_string(role) ? fy_castp(&role, "") : "user",
				messages_text_block(ctx, content));
	}

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
	return assert_valid_generic(out, NULL);
}
