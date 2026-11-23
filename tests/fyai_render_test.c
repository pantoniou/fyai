/*
 * fyai_render_test.c - tests for the Markdown table source
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>
#include <string.h>

#include "fyai_render.h"
#include "fyai_test.h"

#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(render, column_role, render_column_role)

/* A column with a role tags each non-empty cell; an empty cell stays empty. */
int render_column_role(void)
{
	struct fy_generic_builder_cfg bcfg = {
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED,
	};
	struct fy_generic_builder *gb = fy_generic_builder_create(&bcfg);
	fy_generic items[2], data, columns, opts;
	char *md;

	FYAI_TCHECK(gb);
	items[0] = fy_mapping(gb, "name", "a.c", "added", "+3");
	items[1] = fy_mapping(gb, "name", "b.c", "added", "");
	data = fy_gb_sequence_create(gb, 2, items);
	columns = fy_mapping(gb, "added", fy_mapping(gb, "role", "diff.lineno.add"));
	opts = fy_mapping(gb, "keys", fy_sequence(gb, "name", "added"),
			  "columns", columns);

	md = fyai_generic_markdown_source(opts, data);
	FYAI_TCHECK(md);
	FYAI_TCHECK(strstr(md, "<fy-role name=\"diff.lineno.add\">+3</fy-role>"));
	FYAI_TCHECK(strstr(md, "| b.c |  |"));
	/* Only the cell with text has the tag. */
	FYAI_TCHECK(!strstr(strstr(md, "<fy-role") + 1, "<fy-role"));
	free(md);

	md = fyai_generic_markdown_source(fy_mapping(gb, "keys",
				fy_sequence(gb, "name", "added")), data);
	FYAI_TCHECK(md);
	FYAI_TCHECK(!strstr(md, "fy-role"));
	free(md);
	fy_generic_builder_destroy(gb);
	return 0;
}
