/*
 * fyai_builtin.c - checks of the data that the binary embeds
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <string.h>

#include "fyai.h"
#include "fyai_builtin.h"
#include "fyai_catalog.h"
#include "fyai_cmd.h"
#include "fyai_config.h"
#include "fyai_page.h"
#include "fyai_tool_spec.h"

/* The embedded config.yaml.sample. */
#include "embedded_config.inc"

#define FYAI_MODULE FYAIEM_UNKNOWN

static int builtin_check_registry(struct fyai_ctx *ctx)
{
	int rc;

	rc = fyai_cmd_registry_check();
	fyai_error_check(ctx, !rc, err, "command definitions: %s",
			 fyai_cmd_registry_why());
	return 0;
err:
	return -1;
}

static int builtin_check_catalog(struct fyai_ctx *ctx)
{
	int rc;

	/* No arena is open, so this is the embedded catalogue. */
	rc = fyai_catalog_validate(ctx);
	fyai_error_check(ctx, !rc, err, "the embedded catalogue is not right");
	return 0;
err:
	return -1;
}

static int builtin_check_config_sample(struct fyai_ctx *ctx)
{
	fy_generic_sized_string sample = {
		.data = (const char *)FYAI_EMBEDDED_CONFIG,
		.size = FYAI_EMBEDDED_CONFIG_LEN,
	};
	fy_generic doc;
	int rc;

	doc = fy_parse(ctx->cfg->gb, sample,
		       FYAI_YAML_PARSE_FLAGS | FYOPPF_INPUT_TYPE_STRING, NULL);
	fyai_error_check(ctx, fy_is_mapping(doc), err,
			 "the config sample does not parse");
	rc = fyai_config_validate_schema(ctx->cfg, doc, "the config sample");
	fyai_error_check(ctx, !rc, err, "the config sample is not right");
	return 0;
err:
	return -1;
}

static int builtin_check_page(struct fyai_ctx *ctx)
{
	char why[1024];
	int rc;

	rc = fyai_page_check_embedded(why, sizeof(why));
	fyai_error_check(ctx, !rc, err, "%s", why);
	return 0;
err:
	return -1;
}

/* A tool has a type and, for a function, a name that no other tool has. */
static int builtin_check_tools(struct fyai_ctx *ctx)
{
	fy_generic tools, tool, name, other;
	size_t i = 0, j;

	tools = make_tools(ctx);
	fyai_error_check(ctx, fy_is_sequence(tools) && fy_len(tools), err,
			 "the tool specifications are not a sequence");
	fy_foreach(tool, tools) {
		fyai_error_check(ctx, fy_is_string(fy_get(tool, "type",
							  fy_invalid)), err,
				 "tool %zu has no type", i);
		name = fy_get(fy_get(tool, "function", fy_invalid), "name",
			      fy_invalid);
		fyai_error_check(ctx, fy_is_string(name) && !fy_empty(name),
				 err, "tool %zu has no name", i);
		for (j = 0; j < i; j++) {
			other = fy_get(fy_get(fy_get_at(tools, j), "function",
					      fy_invalid), "name", fy_invalid);
			fyai_error_check(ctx, !fy_equal(name, other), err,
					 "tool '%s' is defined twice",
					 fy_castp(&name, ""));
		}
		i++;
	}
	return 0;
err:
	return -1;
}

int fyai_builtin_check(struct fyai_ctx *ctx)
{
	static int (*const checks[])(struct fyai_ctx *) = {
		builtin_check_registry,
		builtin_check_catalog,
		builtin_check_config_sample,
		builtin_check_page,
		builtin_check_tools,
	};
	size_t i;

	for (i = 0; i < sizeof(checks) / sizeof(checks[0]); i++)
		if (checks[i](ctx))
			return -1;
	return 0;
}
