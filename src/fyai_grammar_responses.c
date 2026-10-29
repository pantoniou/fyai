/*
 * fyai_grammar_responses.c - the OpenAI Responses grammar
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

static fy_generic responses_tool_def(struct fyai_ctx *ctx, fy_generic spec)
{
	fy_generic function = fy_get(spec, "function");

	return fy_mapping(ctx->gb,
		"type", "function",
		"name", fy_get(function, "name", ""),
		"description", fy_get(function, "description", ""),
		"parameters", fy_get(function, "parameters"));
}

static fy_generic responses_make_tools(struct fyai_ctx *ctx)
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
			response_tool = responses_tool_def(ctx, tool);
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
	return fyai_provider_result(ctx, response_tools,
			       "could not build the Responses tool definitions");
}

/* Convert a shell result to a provider-supported outcome. */
static fy_generic responses_shell_outcome(struct fy_generic_builder *gb,
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
static fy_generic responses_shell_output(struct fyai_ctx *ctx,
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
		sanitized = responses_shell_outcome(ctx->transient_gb, entry);
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

static fy_generic responses_input(struct fyai_ctx *ctx, fy_generic messages)
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
				responses_shell_output(ctx, m));
			continue;
		}

		/* Other native Responses items (function_call,
		 * function_call_output, message, ...): pass through. */
		if (fy_equal(type, "function_call_output"))
			m = fy_assoc(ctx->transient_gb, m, "output",
				     fyai_wire_tool_text(ctx, fy_get(m, "output", fy_invalid)));
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
		content = fyai_wire_content(ctx, content, "input_text");
		if (fy_is_invalid(content))
			return fyai_provider_result(ctx, content,
					       "could not build the Responses input");
		tmp = fy_mapping("role", fy_is_string(role) ? role : fy_value("user"),
				 "content", content);
		input = fy_append(ctx->transient_gb, input, tmp);
	}

	input = fy_gb_internalize(ctx->transient_gb, input);
	return fyai_provider_result(ctx, input, "could not build the Responses input");
}


static fy_generic responses_image_part(struct fyai_ctx *ctx, fy_generic part)
{
	struct fy_generic_builder *gb = ctx->transient_gb;
	fy_generic out;
	char *data;

	data = fyai_media_part_data_url(ctx, part);
	if (!data)
		return fy_invalid;
	out = fy_mapping(gb, "type", "input_image",
			 "image_url", fy_value(gb, data));
	free(data);
	return out;
}

static fy_generic responses_build_request(struct fyai_ctx *ctx,
					  const struct fyai_grammar_request *req)
{
	struct fy_generic_builder *gb = ctx->transient_gb;
	struct fyai_cfg *cfg = ctx->cfg;
	fy_generic request, reasoning, options;

	request = fy_mapping(gb,
		"model", cfg->model,
		"instructions", req->instructions,
		"input", responses_input(ctx, req->messages),
		"store", cfg->response_chain);
	if (req->temperature)
		request = fy_assoc(gb, request, "temperature", cfg->temperature);
	if (fy_is_valid(req->previous_response_id))
		request = fy_assoc(gb, request, "previous_response_id",
				   req->previous_response_id);
	if (fyai_grammar_reasoning_set(cfg)) {
		reasoning = fy_map_empty;
		if (!fy_str_empty(cfg->reasoning_effort))
			reasoning = fy_assoc(gb, reasoning, "effort",
					     cfg->reasoning_effort);
		if (!fy_str_empty(cfg->reasoning_summary))
			reasoning = fy_assoc(gb, reasoning, "summary",
					     cfg->reasoning_summary);
		request = fy_assoc(gb, request, "reasoning", reasoning);
	}
	if (fyai_grammar_tools_wanted(ctx) ||
	    fyai_provider_native_web_search(cfg)) {
		request = fy_assoc(gb, request, "tools", ctx->tools,
				   "tool_choice", "auto");
		if (cfg->parallel_tool_calls)
			request = fy_assoc(gb, request,
					   "parallel_tool_calls", true);
	}
	if (req->extents_lp)
		request = fy_assoc(gb, request, "top_logprobs", 0,
				   "include", fy_sequence(gb,
					"message.output_text.logprobs"));
	if (cfg->top_logprobs >= 0)
		request = fy_assoc(gb, request, "top_logprobs",
				   cfg->top_logprobs);
	options = fy_map_empty;
	if (cfg->no_obfuscation)
		options = fy_assoc(gb, options, "include_obfuscation", false);
	return fyai_grammar_add_stream(ctx, request, options);
}

/* The compact route is beside the Responses route of the endpoint. */
static bool responses_compact_url(const struct fyai_cfg *cfg, char *out,
				  size_t size)
{
	static const char suffix[] = "/responses";
	size_t len;

	if (!cfg->api_url)
		return false;
	len = strlen(cfg->api_url);
	if (len < sizeof(suffix) - 1 ||
	    strcmp(cfg->api_url + len - (sizeof(suffix) - 1), suffix))
		return false;
	return snprintf(out, size, "%s/compact", cfg->api_url) < (int)size;
}

static const struct fyai_grammar_header responses_headers[] = {
	{ NULL, NULL },
};

const struct fyai_grammar fyai_grammar_responses = {
	.mode			= FYAI_API_RESPONSES,
	.name			= "responses",
	.protocol		= "responses",
	.title			= "Responses API",
	.default_url		= OPENAI_RESPONSES_URL,
	.default_model		= DEFAULT_OPENAI_MODEL,
	.default_provider	= "openai",
	.headers		= responses_headers,
	.input			= responses_input,
	.make_tools		= responses_make_tools,
	.tool_def		= responses_tool_def,
	.image_part		= responses_image_part,
	.build_request		= responses_build_request,
	.compact_url		= responses_compact_url,
	.reasoning		= true,
	.logprobs		= true,
	.builtin_shell		= true,
	.chaining		= true,
	.chatgpt		= true,
};
