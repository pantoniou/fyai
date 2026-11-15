/*
 * fyai_tool_template.c - {{name}} in the descriptions of the tools
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_TOOLS

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai.h"
#include "fyai_tool_template.h"
#include "fyai_view.h"

#define PLACEHOLDER_MAX 64

/* The state of project isolation: "default", "optional", or "" when the run cannot isolate. */
static const char *template_project_isolation(struct fyai_ctx *ctx)
{
	fy_generic section = fy_get(ctx->cfg->config_doc, "agent", fy_invalid);

	if (!fyai_view_isolation_available(ctx))
		return "";
	if (fy_equal(fy_get(section, "isolation", "none"), "view"))
		return "default";
	return "optional";
}

static const struct {
	const char *name;
	const char *(*state)(struct fyai_ctx *ctx);
} template_states[] = {
	{ "project_isolation_description", template_project_isolation },
};

const char *fyai_tool_template_state(struct fyai_ctx *ctx, const char *name)
{
	size_t i;

	for (i = 0; i < sizeof(template_states) / sizeof(template_states[0]); i++)
		if (!strcmp(name, template_states[i].name))
			return template_states[i].state(ctx);
	return NULL;
}

/* The text of name for its state in the templates of the tool; empty when it has none. */
static const char *template_value(struct fyai_ctx *ctx, fy_generic templates, const char *name,
				  fy_generic *holder)
{
	const char *state = fyai_tool_template_state(ctx, name);

	if (!state || !*state)
		return "";
	*holder = fy_get(fy_get(templates, name, fy_invalid), state, fy_invalid);
	return fy_is_string(*holder) ? fy_castp(holder, "") : "";
}

/* The text with each {{name}} replaced. Return NULL when there is none to replace. */
static char *template_text(struct fyai_ctx *ctx, fy_generic templates, const char *text)
{
	const char *open, *close, *value;
	fy_generic holder;
	char name[PLACEHOLDER_MAX];
	size_t length;
	FILE *out;
	char *result = NULL;
	size_t size = 0;

	if (!strstr(text, "{{"))
		return NULL;
	out = open_memstream(&result, &size);
	fyai_error_check(ctx, out, err, "could not expand a tool description");
	while ((open = strstr(text, "{{"))) {
		close = strstr(open + 2, "}}");
		if (!close)
			break;
		fwrite(text, 1, (size_t)(open - text), out);
		length = (size_t)(close - open - 2);
		value = "";
		if (length < sizeof(name)) {
			memcpy(name, open + 2, length);
			name[length] = '\0';
			value = template_value(ctx, templates, name, &holder);
		}
		fputs(value, out);
		text = close + 2;
	}
	fputs(text, out);
	if (fclose(out)) {
		free(result);
		return NULL;
	}
	/* A name with no text leaves no trailing blank behind it. */
	while (size && (result[size - 1] == ' ' || result[size - 1] == '\n'))
		result[--size] = '\0';
	return result;
err:
	return NULL;
}

static fy_generic template_expand(struct fyai_ctx *ctx, fy_generic templates, fy_generic tree)
{
	struct fy_generic_builder *gb = ctx->cfg->gb;
	fy_generic value, item, out, changed;
	const char *text, *name;
	char *expanded;

	if (fy_is_string(tree)) {
		text = fy_castp(&tree, "");
		expanded = template_text(ctx, templates, text);
		if (!expanded)
			return tree;
		out = fy_value(gb, expanded);
		free(expanded);
		return out;
	}
	if (fy_is_mapping(tree)) {
		out = tree;
		fy_foreach_key_value(name, value, tree) {
			changed = template_expand(ctx, templates, value);
			if (changed.v != value.v)
				out = fy_assoc(gb, out, name, changed);
		}
		return out;
	}
	if (fy_is_sequence(tree)) {
		out = fy_sequence(gb);
		fy_foreach(item, tree)
			out = fy_append(gb, out, template_expand(ctx, templates, item));
		return out;
	}
	return tree;
}

fy_generic fyai_tool_template_expand(struct fyai_ctx *ctx, fy_generic tool)
{
	fy_generic templates = fy_get(tool, "templates", fy_invalid), out;

	out = template_expand(ctx, templates, tool);
	if (fy_is_valid(templates))
		out = fy_disassoc(ctx->cfg->gb, out, "templates");
	return out;
}
