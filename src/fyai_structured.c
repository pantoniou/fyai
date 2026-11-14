/*
 * fyai_structured.c - the structured output of a turn
 *
 * SPDX-License-Identifier: MIT
 *
 * A front end that asks for an answer of a JSON Schema sets the schema of
 * the context. The structured_output tool then takes that schema as its
 * parameters, and a call that matches it is the answer: the context keeps
 * it, and the turn ends. Without a schema the tool is not offered.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>

#include "fyai.h"
#include "fyai_schema.h"
#include "fyai_tool_registry.h"
#include "utils.h"

#define FYAI_MODULE FYAIEM_UNKNOWN

static fy_generic structured_run(struct fyai_ctx *ctx, fy_generic args,
				 bool *okp)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	fy_generic report;
	char *problems;
	const char *text;

	*okp = false;
	if (!fy_is_mapping(ctx->output_schema))
		return fy_value(gb, "no structured output is asked for");
	report = fyai_schema_validate(gb, ctx->output_schema, args);
	if (!fyai_schema_valid(report)) {
		problems = fyai_schema_report_string(report);
		text = fy_sprintfa("the output does not match the schema:\n%s",
				   problems ? problems : "no detail");
		free(problems);
		return fy_value(gb, text);
	}
	ctx->structured_output = fy_gb_internalize(ctx->arena->gb, args);
	if (fy_is_invalid(ctx->structured_output))
		return fy_value(gb, "the structured output could not be kept");
	ctx->turn_end_requested = true;
	*okp = true;
	return fy_value(gb, "The structured output was accepted.");
}

/* The tool takes the schema of the context as its parameters. */
static fy_generic structured_describe(struct fyai_ctx *ctx,
				      struct fy_generic_builder *gb,
				      fy_generic tool)
{
	fy_generic fn;

	if (!fy_is_mapping(ctx->output_schema))
		return fy_invalid;
	fn = fy_get(tool, "function", fy_invalid);
	fn = fy_assoc(gb, fn, "parameters", ctx->output_schema);
	return fy_assoc(gb, tool, "function", fn);
}

static void structured_head(struct fyai_ctx *ctx, FILE *mf,
			    struct fy_generic_builder *gb, fy_generic args,
			    int preview_lines, struct fyai_md_blocks *blocks)
{
	(void)ctx;
	(void)gb;
	(void)args;
	(void)preview_lines;
	(void)blocks;
	fprintf(mf, "**structured output**\n\n");
}

const struct fyai_tool_def fyai_structured_defs[] = {
	{ .name = "structured_output", .run = structured_run,
	  .head = structured_head, .describe = structured_describe,
	  .flags = FYAI_TOOL_PARENT },
};
const size_t fyai_structured_defs_count = ARRAY_SIZE(fyai_structured_defs);
