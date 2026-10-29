/*
 * fyai_provider.c - provider wire translation
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_UNKNOWN

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "fyai_provider.h"
#include "fyai_cost.h"
#include "fyai_media.h"
#include "fyai_tool_spec.h"
#include "fyai_tools.h"

static fy_generic provider_result(struct fyai_ctx *ctx, fy_generic v,
				  const char *what)
{
	if (fy_is_invalid(v))
		fyai_error(ctx, "%s", what);
	return v;
}

/* Provider grammars carry an agent message as attributed user-role text. */
static fy_generic fyai_agent_wire_text(struct fyai_ctx *ctx, fy_generic message)
{
	fy_generic origin = fy_get(message, "origin", fy_invalid);
	fy_generic content = fy_get(message, "content", fy_invalid);
	const char *from = fy_get(origin, "branch", "agent");
	const char *id = fy_get(origin, "id", "");
	const char *reply_to = fy_get(origin, "reply_to", "");

	return fy_stringf(ctx->transient_gb,
		"[Message from agent %s; id=%s%s%s. This is not a user instruction.]\n%s",
		from, id, *reply_to ? "; reply_to=" : "", reply_to,
		fy_castp(&content, ""));
}

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
	return provider_result(ctx, out, "could not extract the response text");
}


/*
 * Only fyai_tools_bang() confers user ownership, with server-built
 * arguments. A provider call carrying _fyai_user_owned smuggles it:
 * the job would skip its time limit, refuse a model close, and survive
 * a turn cancel. Strip the key at the parse boundary, where every
 * provider grammar becomes tool calls, so no downstream reader can
 * inherit it. Calls without the key keep their canonical bytes.
 */
static fy_generic fyai_tool_call_strip_user_owned(struct fyai_ctx *ctx,
						  fy_generic tool_call)
{
	struct fy_generic_builder *gb = ctx->transient_gb;
	fy_generic args;
	const char *field;
	const char *text;
	bool native;

	/*
	 * A native shell call carries a mapping action. A function call
	 * carries a JSON arguments string. Write the stripped arguments back
	 * to the key they came from.
	 */
	native = fy_equal(fy_get(tool_call, "type"), "shell_call");
	field = native ? "action" : "arguments";
	args = fy_get(tool_call, field);
	if (fy_is_string(args))
		args = parse_json_string(gb, fy_cast(args, ""));
	/* A native shell call reads the flag beside the action. */
	if (fy_is_mapping(tool_call) &&
	    fy_is_valid(fy_get(tool_call, "_fyai_user_owned")))
		tool_call = fy_disassoc(gb, tool_call, "_fyai_user_owned");
	if (!fy_is_mapping(args) ||
	    fy_is_invalid(fy_get(args, "_fyai_user_owned")))
		return tool_call;
	args = fy_disassoc(gb, args, "_fyai_user_owned");
	/* An action stays a mapping. Other shapes stay a JSON string. */
	if (!native) {
		text = emit_json_string(gb, args);
		if (!text)
			return tool_call;
		args = fy_string(text);
	}
	tool_call = fy_disassoc(gb, tool_call, field);
	return fy_assoc(gb, tool_call, field, args);
}

static fy_generic fyai_tool_calls_take_ownership(struct fyai_ctx *ctx,
						 fy_generic tool_calls)
{
	fy_generic call, stripped;
	size_t i, n;

	if (!fy_is_sequence(tool_calls))
		return tool_calls;
	n = fy_len(tool_calls);
	for (i = 0; i < n; i++) {
		call = fy_get_at(tool_calls, i);
		stripped = fyai_tool_call_strip_user_owned(ctx, call);
		if (fy_equal(stripped, call))
			continue;
		tool_calls = fy_replace(ctx->transient_gb, tool_calls, i,
					stripped);
		if (fy_is_invalid(tool_calls))
			return fy_invalid;
	}
	return tool_calls;
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
			if (fyai_item_type_is_call(type))
				tool_calls = fy_append(tool_calls, item);
		}
		break;

	case FYAI_API_CHAT_COMPLETIONS:
		/* Chat nests each call: give it the shape of a call item. */
		tool_calls = fy_seq_empty;
		output = response_tool_calls(ctx->transient_gb, response_doc);
		fy_foreach(item, output) {
			tmp = fy_mapping(
				"type", "function_call",
				"call_id", fy_get(item, "id", ""),
				"name", fy_get_at_path(item, "function", "name"),
				"arguments", fy_get_at_path(item, "function",
							    "arguments"));
			tool_calls = fy_append(tool_calls, tmp);
		}
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

	tool_calls = fyai_tool_calls_take_ownership(ctx, tool_calls);

	/*
	 * A plain answer carries no tool_calls key, so the lookup yields an
	 * invalid generic; normalise it to an empty sequence (callers count it
	 * with fy_len()). Only internalize a real result.
	 */
	if (fy_is_invalid(tool_calls) || fy_is_null(tool_calls))
		return fy_seq_empty;

	tool_calls = fy_gb_internalize(ctx->transient_gb, tool_calls);
	return provider_result(ctx, tool_calls,
			       "could not collect the response tool calls");
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
	return provider_result(ctx, v, "could not retain the response id");
}

bool fyai_response_needs_tool_calls(struct fyai_ctx *ctx,
					   fy_generic response_doc)
{
	return fy_len(fyai_response_tool_calls(ctx, response_doc)) > 0;
}

bool fyai_response_is_final(struct fyai_ctx *ctx, fy_generic response_doc)
{
	if (ctx->cfg->api_mode == FYAI_API_MESSAGES &&
	    fy_equal(fy_get(response_doc, "stop_reason"), "pause_turn"))
		return false;
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
	struct fyai_pricing price;
	double cost;
	bool estimated;

	usage = fy_get(doc, "usage");
	if (fy_is_invalid(usage))
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
		input += cached + cache_write;
	}
	total = fy_get(usage, "total_tokens", 0LL);
	if (!total)
		total = input + output;
	/* Some providers (e.g. OpenRouter) report a per-call dollar cost. */
	cost = fy_get(usage, "cost", 0.0);
	/* The others are priced from the catalogue, at the model of the call. */
	estimated = false;
	if (cost <= 0.0 &&
	    fyai_pricing_lookup(cfg, cfg->provider, cfg->model, &price)) {
		cost = fyai_pricing_cost(&price, input, cached, cache_write,
					 output);
		estimated = cost > 0.0;
	}

	out = fy_mapping(
		"input", input,
		"cached", cached,
		"cache_write", cache_write,
		"output", output,
		"reasoning", reasoning,
		"total", total,
		"cost", cost,
		"est", estimated,
		"model", cfg->model ? cfg->model : "");

	out = fy_gb_internalize(ctx->transient_gb, out);
	return provider_result(ctx, out, "could not extract the response usage");
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
		if (fy_is_valid(bytes) &&
		    !fy_is_null(bytes))
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
		if (fy_is_invalid(extents))
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
		if (fy_is_invalid(extents))
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
	bool native_shell;

	response_tools = fy_seq_empty;

	native_shell = cfg->enable_builtin_shell &&
		       fyai_provider_native_shell(cfg);

	if (cfg->enable_tools) {
		tools = make_tools_filtered(ctx);
		fy_foreach(tool, tools) {
			function = fy_get(tool, "function");
			/* Replace the function tool with the native tool. */
			if (native_shell &&
			    fy_equal(fy_get(function, "name"),
				     FYAI_TOOL_EXEC_WIRE_NAME))
				continue;
			response_tool = fy_mapping(
				"type", "function",
				"name", fy_get(function, "name", ""),
				"description", fy_get(function, "description", ""),
				"parameters", fy_get(function, "parameters"));
			response_tools = fy_append(response_tools, response_tool);
		}
	}

	/* Add the native Responses shell only when explicitly enabled. */
	if (native_shell) {
		response_tool = fy_mapping(
				"type", "shell",
				"environment", fy_mapping("type", "local"));
		response_tools = fy_append(response_tools, response_tool);
	}
	if (fyai_provider_native_web_search(cfg))
		response_tools = fy_append(response_tools,
				fy_mapping("type", fyai_provider_is_openrouter(cfg) ?
					"openrouter:web_search" : "web_search"));

	response_tools = fy_gb_internalize(ctx->gb, response_tools);
	return provider_result(ctx, response_tools,
			       "could not build the Responses tool definitions");
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
	if (ctx->cfg->enable_tools || ctx->cfg->enable_builtin_shell) {
		tools = make_tools_filtered(ctx);
		fy_foreach(tool, tools) {
			function = fy_get(tool, "function");
			tmp = fy_mapping("name", fy_get(function, "name", ""),
						 "description", fy_get(function, "description", ""),
						 "input_schema", fy_get(function, "parameters"));
			messages_tools = fy_append(messages_tools, tmp);
		}
	}
	if (fyai_provider_native_web_search(ctx->cfg))
		messages_tools = fy_append(messages_tools,
				fy_mapping("type", "web_search_20250305",
						   "name", "web_search"));

	messages_tools = fy_gb_internalize(ctx->gb, messages_tools);
	return provider_result(ctx, messages_tools,
			       "could not build the Messages tool definitions");
}

/*
 * Return true if this endpoint accepts OpenAI's built-in shell tool.
 *
 * Multiple providers implement the Responses grammar. The `shell` tool and
 * its `shell_call` items are not available from all of these providers. An
 * unsupported item does not match the provider input union, and the provider
 * rejects the complete request. For example, OpenRouter returns HTTP 400 with
 * `invalid_prompt`.
 *
 * Read `shell_tool_supported` from the catalogue for each provider endpoint.
 * Do not use a provider name in this test. The catalogue cannot describe the
 * ChatGPT Codex authentication mode as an endpoint. This backend does not
 * accept the declaration, and Codex clients use a function tool for local
 * execution.
 *
 * Use this function for declaration and replay decisions. This requirement
 * prevents different decisions at the two call sites.
 */
bool fyai_provider_native_shell(const struct fyai_cfg *cfg)
{
	return cfg->api_mode == FYAI_API_RESPONSES && !cfg->chatgpt_auth &&
	       cfg->shell_tool_supported;
}

bool fyai_provider_native_web_search(const struct fyai_cfg *cfg)
{
	return cfg && cfg->web_search && cfg->web_search_supported;
}

bool fyai_provider_is_openrouter(const struct fyai_cfg *cfg)
{
	return cfg && cfg->provider &&
		!strcasecmp(cfg->provider, "openrouter");
}

/* Detect transient errors that arrive inside a successful HTTP stream. */
bool fyai_provider_error_transient(fy_generic err)
{
	fy_generic code, type;

	code = fy_get(err, "code");
	type = fy_get(err, "type");
	if (fy_any_equal(code, "subscription_sharing_usage_limit_exceeded",
		"subscription_sharing_user_not_eligible", "subscription_sharing_invalid_user",
		"subscription_sharing_unsupported_capability", "subscription_sharing_route_not_supported",
		"chatpass_v2_scope_not_authorized", "chatpass_v2_invalid_authorization_context"))
		return false;
	if (fy_any_equal(code, "subscription_sharing_usage_unavailable",
		"subscription_sharing_user_unavailable"))
		return true;
	if (fy_any_equal(code, "rate_limit_exceeded", "server_error",
			 "service_unavailable"))
		return true;
	return fy_any_equal(type, "rate_limit_error", "server_error",
			    "overloaded_error", "api_error");
}

/* Convert a shell result to a provider-supported outcome. */
static fy_generic fyai_shell_outcome_sanitized(struct fy_generic_builder *gb,
					       fy_generic entry)
{
	fy_generic outcome;
	long long signal;

	outcome = fy_get(entry, "outcome");
	if (fy_equal(fy_get(outcome, "type"), "timeout"))
		outcome = fy_mapping(gb, "type", "timeout");
	else if (fy_equal(fy_get(outcome, "type"), "signal")) {
		signal = fy_get(outcome, "signal", 0LL);
		outcome = fy_mapping(gb, "type", "exit",
					"exit_code", 128 + signal);
	} else {
		outcome = fy_mapping(gb, "type", "exit",
					"exit_code",
					fy_get(outcome, "exit_code", 0LL));
	}
	return fy_mapping(gb, "stdout", fy_get(entry, "stdout", ""),
			     "stderr", fy_get(entry, "stderr", ""),
			     "outcome", outcome);
}

/* @m, a shell_call_output item, with every outcome of its output sanitized. */
/*
 * A tool result that is a structure goes on the wire as its JSON text: every grammar
 * takes a string as the output of a function call, and an object is a request error.
 */
static fy_generic wire_tool_text(struct fyai_ctx *ctx, fy_generic output)
{
	const char *text;

	if (!fy_is_valid(output) || fy_is_null(output))
		return fy_value(ctx->transient_gb, "");
	if (!fy_is_mapping(output))
		return output;
	text = emit_json_string(ctx->transient_gb, output);
	return fy_value(ctx->transient_gb, text ? text : "");
}

/* The base64 bytes of one image, as the part of @api that carries them. */
static fy_generic wire_image_part(struct fyai_ctx *ctx, fy_generic part,
				  enum fyai_api_mode api)
{
	struct fy_generic_builder *gb = ctx->transient_gb;
	const char *type;
	fy_generic out;
	char *data;

	type = fy_get(part, "media_type", "");
	switch (api) {
	case FYAI_API_RESPONSES:
		data = fyai_media_part_data_url(ctx, part);
		if (!data)
			return fy_invalid;
		out = fy_mapping(gb, "type", "input_image",
				 "image_url", fy_value(gb, data));
		free(data);
		return out;
	case FYAI_API_CHAT_COMPLETIONS:
		data = fyai_media_part_data_url(ctx, part);
		if (!data)
			return fy_invalid;
		out = fy_mapping(gb, "type", "image_url",
				 "image_url", fy_mapping(gb,
						"url", fy_value(gb, data)));
		free(data);
		return out;
	case FYAI_API_MESSAGES:
		data = fyai_media_part_base64(ctx, part);
		if (!data)
			return fy_invalid;
		out = fy_mapping(gb, "type", "image",
				 "source", fy_mapping(gb,
						"type", "base64",
						"media_type", type,
						"data", fy_value(gb, data)));
		free(data);
		return out;
	default:
		assert(0);
		__builtin_unreachable();
	}
}

/*
 * Canonical content as the wire of @api takes it: the text runs under
 * @text_type, and each image in the shape of the grammar. Content with no
 * image part is returned borrowed, so a text-only message keeps the shape it
 * always had and a stored provider block array is not rewritten.
 */
static fy_generic wire_content(struct fyai_ctx *ctx, fy_generic content,
			       enum fyai_api_mode api, const char *text_type)
{
	struct fy_generic_builder *gb = ctx->transient_gb;
	fy_generic out, part, wire;

	/* Files without images are text: the words, then each file. */
	if (!fyai_media_content_has_images(content))
		return fyai_media_content_has_files(content) ?
		       fyai_media_model_text(gb, content) : content;
	if (!ctx->cfg->image_input_supported) {
		fyai_error(ctx, "the model '%s' takes no image input",
			   ctx->cfg->model ? ctx->cfg->model : "");
		return fy_invalid;
	}
	out = fy_seq_empty;
	fy_foreach(part, content) {
		if (fyai_media_part_is_image(part)) {
			wire = wire_image_part(ctx, part, api);
			if (fy_is_invalid(wire))
				return fy_invalid;
			out = fy_append(gb, out, wire);
			continue;
		}
		if (fy_not_equal(fy_get(part, "type"), FYAI_MEDIA_PART_TEXT))
			continue;
		out = fy_append(gb, out, fy_mapping(gb,
					"type", text_type,
					"text", fy_get(part, "text", "")));
	}
	return out;
}

static fy_generic fyai_shell_output_sanitized(struct fyai_ctx *ctx,
					      fy_generic m)
{
	fy_generic outputs = fy_seq_empty;
	fy_generic output;
	fy_generic entry;
	fy_generic sanitized;

	output = fy_get(m, "output");
	if (!fy_is_sequence(output))
		return m;
	fy_foreach(entry, output) {
		sanitized = fyai_shell_outcome_sanitized(ctx->transient_gb, entry);
		outputs = fy_append(ctx->transient_gb, outputs,
				    sanitized);
	}
	return fy_null_filtered_mapping(ctx->transient_gb,
			"type", fy_get(m, "type", "shell_call_output"),
			"call_id", fy_get(m, "call_id", ""),
			"output", outputs,
			"max_output_length",
			fyai_generic_or_null(fy_get(m, "max_output_length")));
}

fy_generic fyai_responses_input(struct fyai_ctx *ctx, fy_generic messages)
{
	fy_generic type;
	fy_generic role;
	fy_generic content;
	fy_generic input;
	fy_generic output;
	fy_generic m;
	fy_generic tmp;
	fy_generic cmdv;
	const char *cmd, *args, *text;
	bool native_shell;

	input = fy_seq_empty;
	native_shell = fyai_provider_native_shell(ctx->cfg);

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

		/*
		 * This provider does not accept a native shell item. Convert
		 * the item to the standard `shell` function tool. The Messages
		 * and Chat builders use the same format. The capability check
		 * enables the function tool before this conversion.
		 */
		if (!native_shell && fy_equal(type, "shell_call")) {
			cmdv = fy_get_at_path(m, "action", "commands", 0);
			cmd = fy_castp(&cmdv, "");
			args = emit_json_string(ctx->transient_gb,
						fy_mapping("command", cmd));
			input = fy_append(ctx->transient_gb, input,
					fy_mapping(
						"type", "function_call",
						"call_id", fy_get(m, "call_id", ""),
						"name", FYAI_TOOL_EXEC_WIRE_NAME,
						"arguments", args ? args : "{}"));
			continue;
		}
		if (!native_shell && fy_equal(type, "shell_call_output")) {
			output = fy_get(m, "output");
			if (!fy_is_string(output)) {
				text = emit_json_string(ctx->transient_gb,
							output);
				output = fy_value(text ? text : "");
			}
			input = fy_append(ctx->transient_gb, input,
					fy_mapping(
						"type", "function_call_output",
						"call_id", fy_get(m, "call_id", ""),
						"output", output));
			continue;
		}

		/*
		 * Remove canonical outcome fields that the endpoint rejects.
		 */
		if (native_shell && fy_equal(type, "shell_call_output")) {
			input = fy_append(ctx->transient_gb, input,
				fyai_shell_output_sanitized(ctx, m));
			continue;
		}

		/* Other native Responses items (function_call,
		 * function_call_output, message, ...): pass through. */
		if (fy_equal(type, "function_call_output"))
			m = fy_assoc(ctx->transient_gb, m, "output",
				     wire_tool_text(ctx, fy_get(m, "output", fy_invalid)));
		if (fy_is_string(type)) {
			input = fy_append(ctx->transient_gb, input, m);
			continue;
		}

		role = fy_get(m, "role");
		content = fy_get(m, "content");

		/* The system prompt is surfaced via the request `instructions`
		 * field, so drop the canonical system turn from `input` to
		 * avoid sending it twice. */
		if (fy_equal(role, "system"))
			continue;

		/* Plain role message: Responses rejects null content. */
		if (fy_equal(role, "agent")) {
			input = fy_append(ctx->transient_gb, input,
				fy_mapping("role", "user", "content",
					fyai_agent_wire_text(ctx, m)));
			continue;
		}
		if (fy_is_invalid(content) ||
		    fy_is_null(content))
			content = fy_value("");
		content = wire_content(ctx, content, FYAI_API_RESPONSES,
				       "input_text");
		if (fy_is_invalid(content))
			return provider_result(ctx, content,
					       "could not build the Responses input");
		tmp = fy_mapping("role", fy_is_string(role) ? role : fy_value("user"),
				 "content", content);
		input = fy_append(ctx->transient_gb, input, tmp);
	}

	input = fy_gb_internalize(ctx->transient_gb, input);
	return provider_result(ctx, input, "could not build the Responses input");
}

/*
 * The words of a native message item. Such an item is the output of a model:
 * it carries no file part, which only a message of the user does, and
 * wire_content() builds that one.
 */
fy_generic fyai_item_text(struct fyai_ctx *ctx, fy_generic item)
{
	return fyai_media_text(ctx->transient_gb, fy_get(item, "content"));
}

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

fy_generic fyai_chat_input(struct fyai_ctx *ctx, fy_generic messages)
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
					 "content", wire_tool_text(ctx, fy_get(m, "output", fy_invalid)));
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
			content = wire_content(ctx, fy_get(m, "content"),
					       FYAI_API_CHAT_COMPLETIONS, "text");
			if (fy_is_invalid(content))
				return provider_result(ctx, content,
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
	return provider_result(ctx, out, "could not build the Chat input");
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
			blocks = wire_content(ctx, content, FYAI_API_MESSAGES,
					      "text");
			if (fy_is_invalid(blocks))
				return provider_result(ctx, blocks,
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
fy_generic fyai_messages_input(struct fyai_ctx *ctx, fy_generic messages)
{
	struct fy_generic_builder *gb = ctx->transient_gb;
	fy_generic out, m, content, tc;
	size_t j, n;

	out = fyai_messages_blocks(ctx, messages);
	if (fy_is_invalid(out))
		return provider_result(ctx, out,
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
	return provider_result(ctx, out, "could not build the Messages input");
}
