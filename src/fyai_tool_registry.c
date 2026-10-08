/* SPDX-License-Identifier: MIT */
#include <string.h>

#include "fyai_tool_registry.h"
#include "fyai_tools.h"

struct fyai_tool_table {
	const struct fyai_tool_def *defs;
	const size_t *count;
};

static const struct fyai_tool_table tables[] = {
	{ fyai_tools_defs, &fyai_tools_defs_count },
	{ fyai_wait_defs, &fyai_wait_defs_count },
	{ fyai_agent_defs, &fyai_agent_defs_count },
	{ fyai_monitor_defs, &fyai_monitor_defs_count },
	{ fyai_display_defs, &fyai_display_defs_count },
};

const struct fyai_tool_def *fyai_tool_at(size_t index)
{
	size_t i;

	for (i = 0; i < sizeof(tables) / sizeof(tables[0]); i++) {
		if (index < *tables[i].count)
			return &tables[i].defs[index];
		index -= *tables[i].count;
	}
	return NULL;
}

const struct fyai_tool_def *fyai_tool_find(const char *name)
{
	const struct fyai_tool_def *def;
	size_t i;

	if (!name)
		return NULL;
	name = fyai_tool_name_canonical(name);
	for (i = 0; (def = fyai_tool_at(i)); i++)
		if (!strcmp(def->name, name))
			return def;
	return NULL;
}

bool fyai_tool_has(const char *name, unsigned int flag)
{
	const struct fyai_tool_def *def = fyai_tool_find(name);

	return def && (def->flags & flag);
}
