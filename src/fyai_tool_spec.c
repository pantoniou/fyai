/*
 * fyai_tool_spec.c - built-in tool specifications (data/tools.yaml)
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_TOOLS

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include "fyai.h"
#include "fyai_config.h"
#include "fyai_tool_spec.h"

/* The embedded data/tools.yaml document. */
#include "embedded_tools.inc"

fy_generic make_tools(struct fyai_ctx *ctx)
{
	fy_generic_sized_string embedded;
	fy_generic tools;

	embedded.data = (const char *)FYAI_EMBEDDED_TOOLS;
	embedded.size = FYAI_EMBEDDED_TOOLS_LEN;
	tools = fy_parse(ctx->cfg->gb, embedded,
			 FYAI_YAML_PARSE_FLAGS | FYOPPF_INPUT_TYPE_STRING,
			 NULL);
	return assert_valid_generic(tools,
				    "Unable to parse embedded tools (data/tools.yaml)");
}

/* Add configured personas to the agent tool description. */
static fy_generic tools_describe_personas(struct fyai_ctx *ctx, fy_generic tool)
{
	struct fy_generic_builder *gb = ctx->cfg->gb;
	fy_generic personas, key, val, props, prop, params, fn;
	const char *list = "";
	const char *desc;
	size_t i, n;

	personas = fy_get(fy_get(ctx->cfg->config_doc, "agent"), "personas",
			  fy_invalid);
	if (!fy_is_mapping(personas))
		return tool;
	fy_foreach_key_value(key, val, personas) {
		desc = fy_get(val, "description", "");
		list = fy_sprintfa("%s\n- `%s`%s%s", list,
				   fy_castp(&key, "?"),
				   *desc ? ": " : "", desc);
	}
	if (!*list)
		return tool;

	fn = fy_get(tool, "function");
	params = fy_get(fn, "parameters");
	props = fy_get(params, "properties");
	prop = fy_get(props, "persona");
	prop = fy_assoc(gb, prop, fy_value(gb, "description"),
		fy_value(gb, fy_sprintfa(
			"Optional named persona for the sub-agent. Available:%s",
			list)));
	props = fy_assoc(gb, props, fy_value(gb, "persona"), prop);
	return fy_mapping(gb,
		"type", fy_get(tool, "type"),
		"function", fy_mapping(gb,
			"name", fy_get(fn, "name"),
			"description", fy_get(fn, "description"),
			"parameters", fy_mapping(gb,
				"type", fy_get(params, "type"),
				"properties", props,
				"required", fy_get(params, "required"),
				"additionalProperties",
					fy_get(params, "additionalProperties"))));
}

static bool fyai_tool_spec_needs_update(struct fyai_ctx *ctx)
{
	return fy_is_invalid(ctx->tools_spec) ||
	       ctx->tools_spec_generation != ctx->cfg->config_generation ||
	       ctx->tools_spec_agent_child != ctx->cfg->agent_child;
}

/* Remove ask_user and agent from a sub-agent tool set. */
fy_generic make_tools_filtered(struct fyai_ctx *ctx)
{
	struct fy_generic_builder *gb = ctx->cfg->gb;
	fy_generic tools, tool, fn, name, out;

	if (!fyai_tool_spec_needs_update(ctx))
		return ctx->tools_spec;

	tools = make_tools(ctx);
	out = fy_seq_empty;
	fy_foreach(tool, tools) {
		fn = fy_get(tool, "function");
		name = fy_get(fn, "name");
		out = fy_append(gb, out,
				fy_equal(name, "agent") ?
				tools_describe_personas(ctx, tool) : tool);
	}
	tools = assert_valid_generic(fy_gb_internalize(gb, out),
				     "Unable to describe personas");

	if (ctx->cfg->agent_child) {
		out = fy_seq_empty;
		fy_foreach(tool, tools) {
			fn = fy_get(tool, "function");
			name = fy_get(fn, "name");
			if (fy_equal(name, "agent") || fy_equal(name, "ask_user"))
				continue;
			out = fy_append(gb, out, tool);
		}
		out = assert_valid_generic(fy_gb_internalize(gb, out),
					   "Unable to filter agent tools");
	} else
		out = tools;

	ctx->tools_spec = out;
	ctx->tools_spec_generation = ctx->cfg->config_generation;
	ctx->tools_spec_agent_child = ctx->cfg->agent_child;
	return out;
}
