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

#include "fyai_grammar.h"
#include "fyai_provider.h"
#include "fyai_cost.h"
#include "fyai_media.h"
#include "fyai_tool_spec.h"
#include "fyai_tools.h"

fy_generic fyai_provider_result(struct fyai_ctx *ctx, fy_generic v,
				const char *what)
{
	if (fy_is_invalid(v))
		fyai_error(ctx, "%s", what);
	return v;
}

/* Provider grammars carry an agent message as attributed user-role text. */
fy_generic fyai_agent_wire_text(struct fyai_ctx *ctx, fy_generic message)
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

fy_generic fyai_response_canonical(struct fyai_ctx *ctx, fy_generic doc)
{
	struct fy_generic_builder *gb = ctx->transient_gb;
	fy_generic response;

	response = fyai_grammar_response(ctx, doc);
	if (fy_is_valid(response))
		response = fy_assoc(gb, response,
			"calls", fyai_tool_calls_take_ownership(ctx,
					fy_get(response, "calls", fy_seq_empty)),
			"messages", fyai_tool_calls_take_ownership(ctx,
					fy_get(response, "messages", fy_seq_empty)));
	response = fy_gb_internalize(gb, response);
	return fyai_provider_result(ctx, response,
				    "could not read the provider response");
}

fy_generic fyai_response_output_text(struct fyai_ctx *ctx, fy_generic response)
{
	(void)ctx;
	return fy_get(response, "text", fy_invalid);
}

fy_generic fyai_response_tool_calls(struct fyai_ctx *ctx, fy_generic response)
{
	(void)ctx;
	return fy_get(response, "calls", fy_seq_empty);
}

fy_generic fyai_response_id(struct fyai_ctx *ctx, fy_generic response)
{
	fy_generic id;

	(void)ctx;
	id = fy_get(response, "id", fy_invalid);
	return fy_is_string(id) && fy_len(id) ? id : fy_invalid;
}

bool fyai_response_needs_tool_calls(struct fyai_ctx *ctx,
				    fy_generic response)
{
	return fy_len(fyai_response_tool_calls(ctx, response)) > 0;
}

bool fyai_response_is_final(struct fyai_ctx *ctx, fy_generic response)
{
	(void)ctx;
	return fy_get(response, "final", true);
}

fy_generic fyai_extract_usage(struct fyai_ctx *ctx, fy_generic response)
{
	struct fyai_cfg *cfg = ctx->cfg;
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

	usage = fy_get(response, "usage");
	if (!fy_is_mapping(usage))
		return fy_invalid;

	input = fy_get(usage, "input", 0LL);
	cached = fy_get(usage, "cached", 0LL);
	cache_write = fy_get(usage, "cache_write", 0LL);
	output = fy_get(usage, "output", 0LL);
	reasoning = fy_get(usage, "reasoning", 0LL);
	total = fy_get(usage, "total", 0LL);
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
	return fyai_provider_result(ctx, out, "could not extract the response usage");
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
	return fyai_grammar(cfg)->builtin_shell && !cfg->chatgpt_auth &&
	       cfg->shell_tool_supported;
}

bool fyai_provider_native_web_search(const struct fyai_cfg *cfg)
{
	return cfg && cfg->web_search && cfg->web_search_supported;
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

/*
 * A tool result that is a structure goes on the wire as its JSON text: every grammar
 * takes a string as the output of a function call, and an object is a request error.
 */
fy_generic fyai_wire_tool_text(struct fyai_ctx *ctx, fy_generic output)
{
	const char *text;

	if (!fy_is_valid(output) || fy_is_null(output))
		return fy_value(ctx->transient_gb, "");
	if (!fy_is_mapping(output))
		return output;
	text = emit_json_string(ctx->transient_gb, output);
	return fy_value(ctx->transient_gb, text ? text : "");
}

/*
 * Canonical content as the wire of the grammar takes it: the text runs under
 * @text_type, and each image in the shape of the grammar. Content with no
 * image part is returned borrowed, so a text-only message keeps the shape it
 * always had and a stored provider block array is not rewritten.
 */
fy_generic fyai_wire_content(struct fyai_ctx *ctx, fy_generic content,
			     const char *text_type)
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
			wire = fyai_grammar_image_part(ctx, part);
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

/*
 * The words of a native message item. Such an item is the output of a model:
 * it carries no file part, which only a message of the user does, and
 * wire_content() builds that one.
 */
fy_generic fyai_item_text(struct fyai_ctx *ctx, fy_generic item)
{
	return fyai_media_text(ctx->transient_gb, fy_get(item, "content"));
}

void fyai_wire_loss(struct fyai_ctx *ctx, fy_generic *lossesp,
		    const char *kind, fy_generic type)
{
	struct fy_generic_builder *gb = ctx->transient_gb;

	if (!lossesp)
		return;
	*lossesp = fy_append(gb, *lossesp, fy_mapping(gb,
		"kind", kind,
		"content_type", fy_is_string(type) ? type :
						      fy_value(gb, "unknown")));
}

/*
 * The words of the content parts of a wire message. A part of another kind
 * than text is a loss when @lossesp collects them, and its place in the
 * text keeps a placeholder; a reply passes NULL and drops such a part.
 */
fy_generic fyai_wire_parts_text(struct fyai_ctx *ctx, fy_generic content,
				fy_generic *lossesp)
{
	struct fy_generic_builder *gb = ctx->transient_gb;
	fy_generic part, type, text, joined;

	if (fy_is_string(content))
		return content;
	joined = fy_value(gb, "");
	fy_foreach(part, content) {
		type = fy_get(part, "type", fy_invalid);
		/* Calls and results are items of their own. */
		if (fy_any_equal(type, "tool_use", "tool_result", "thinking",
				 "redacted_thinking"))
			continue;
		if (fy_any_equal(type, "text", "input_text", "output_text")) {
			text = fy_get(part, "text", fy_invalid);
			if (fy_is_string(text))
				joined = fy_join(gb, "", joined, text);
			continue;
		}
		if (!lossesp)
			continue;
		fyai_wire_loss(ctx, lossesp, "unsupported_content", type);
		joined = fy_join(gb, "", joined, "[content omitted]");
	}
	return joined;
}

/* The output of a tool result as text: the words of its parts, else JSON. */
fy_generic fyai_wire_output_text(struct fyai_ctx *ctx, fy_generic output)
{
	struct fy_generic_builder *gb = ctx->transient_gb;
	fy_generic part, text, joined;
	const char *json;

	if (fy_is_string(output))
		return output;
	if (!fy_is_valid(output) || fy_is_null(output))
		return fy_value(gb, "");
	if (fy_is_sequence(output)) {
		joined = fy_value(gb, "");
		fy_foreach(part, output) {
			text = fy_get(part, "text", fy_invalid);
			if (fy_is_string(text))
				joined = fy_join(gb, "", joined, text);
		}
		return joined;
	}
	json = emit_json_string(gb, output);
	return fy_value(gb, json ? json : "");
}

fy_generic fyai_wire_calls(struct fyai_ctx *ctx, fy_generic messages)
{
	fy_generic calls, item;

	calls = fy_seq_empty;
	fy_foreach(item, messages)
		if (fyai_item_type_is_call(fy_get(item, "type")))
			calls = fy_append(ctx->transient_gb, calls, item);
	return calls;
}
