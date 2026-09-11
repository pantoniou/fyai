/* SPDX-License-Identifier: MIT */
#define FYAI_MODULE FYAIEM_UNKNOWN

#include <stdlib.h>
#include <string.h>

#include "fyai.h"
#include "fyai_branch.h"
#include "fyai_test.h"
#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(branch, select_order, branch_select_order)
FYAI_TEST_ENTRY(branch, select_directory, branch_select_directory)
FYAI_TEST_ENTRY(branch, select_legacy, branch_select_legacy)
FYAI_TEST_ENTRY(branch, select_empty, branch_select_empty)
FYAI_TEST_ENTRY(branch, pick_last, branch_pick_last_newest)
FYAI_TEST_ENTRY(branch, import_provenance, branch_import_provenance)

/* An entry with both timestamps and a directory. */
static fy_generic entry_new(struct fy_generic_builder *gb, long long created,
			    long long updated, const char *cwd)
{
	struct fyai_branch b;

	memset(&b, 0, sizeof(b));
	b.entry = fy_invalid;
	b.config = fy_invalid;
	b.head = fy_invalid;
	b.created = fy_value(gb, created);
	b.updated = fy_value(gb, updated);
	b.cwd = cwd ? fy_value(gb, cwd) : fy_invalid;
	b.description = fy_invalid;
	b.agent = fy_invalid;
	b.import = fy_invalid;
	b.op = fy_value(gb, FYAI_BRANCH_OP_TURN);
	b.from = fy_invalid;
	b.prev = fy_invalid;
	return fyai_branch_build(gb, &b);
}

/* An entry as written before the branch carried two timestamps. */
static fy_generic entry_legacy(struct fy_generic_builder *gb, long long created)
{
	return fy_mapping(gb, "config", fy_null, "head", fy_null,
			  "created", fy_value(gb, created),
			  "description", fy_null, "agent", fy_null,
			  "import", fy_null,
			  "op", fy_value(gb, FYAI_BRANCH_OP_TURN),
			  "from", fy_null, "prev", fy_null);
}

static struct fy_generic_builder *builder_new(void)
{
	struct fy_generic_builder_cfg config = {
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED,
	};

	return fy_generic_builder_create(&config);
}

static const char *row_branch(fy_generic rows, size_t i)
{
	fy_generic row = fy_get_at(rows, i);

	return fy_get(row, "branch", "");
}

int branch_select_order(void)
{
	struct fy_generic_builder *gb;
	fy_generic branches, rows;

	gb = builder_new();
	FYAI_TCHECK(gb);
	branches = fy_mapping(gb,
		"alpha", entry_new(gb, 10, 300, "/w"),
		"beta", entry_new(gb, 20, 100, "/w"),
		/* Equal update times: the name decides, so the order is stable. */
		"delta", entry_new(gb, 30, 200, "/w"),
		"charlie", entry_new(gb, 40, 200, "/w"));

	rows = fyai_branch_select_rows(NULL, gb, branches, "/w", false);
	FYAI_TCHECK(fy_generic_sequence_get_item_count(rows) == 4);
	FYAI_TCHECK(!strcmp(row_branch(rows, 0), "alpha"));
	FYAI_TCHECK(!strcmp(row_branch(rows, 1), "charlie"));
	FYAI_TCHECK(!strcmp(row_branch(rows, 2), "delta"));
	FYAI_TCHECK(!strcmp(row_branch(rows, 3), "beta"));

	/* A configuration-only publish advances the update time, so it counts
	 * as activity and moves the branch to the front. */
	branches = fyai_branches_set(gb, branches, "beta",
				     entry_new(gb, 20, 900, "/w"));
	rows = fyai_branch_select_rows(NULL, gb, branches, "/w", false);
	FYAI_TCHECK(!strcmp(row_branch(rows, 0), "beta"));

	fy_generic_builder_destroy(gb);
	return 0;
}

int branch_select_directory(void)
{
	struct fy_generic_builder *gb;
	fy_generic branches, rows;

	gb = builder_new();
	FYAI_TCHECK(gb);
	branches = fy_mapping(gb,
		"here", entry_new(gb, 10, 100, "/w"),
		"there", entry_new(gb, 10, 200, "/elsewhere"),
		/* A sub-agent branch is work of a turn, never a session. */
		"here/agent:helper", fy_mapping(gb, "config", fy_null,
			"head", fy_null, "created", fy_value(gb, 10LL),
			"updated", fy_value(gb, 300LL),
			"cwd", fy_value(gb, "/w"),
			"description", fy_null,
			"agent", fy_mapping(gb, "persona", "helper"),
			"op", fy_value(gb, FYAI_BRANCH_OP_TURN),
			"from", fy_null, "prev", fy_null));

	rows = fyai_branch_select_rows(NULL, gb, branches, "/w", false);
	FYAI_TCHECK(fy_generic_sequence_get_item_count(rows) == 1);
	FYAI_TCHECK(!strcmp(row_branch(rows, 0), "here"));

	/* Every directory, and still no sub-agent branch. */
	rows = fyai_branch_select_rows(NULL, gb, branches, "/w", true);
	FYAI_TCHECK(fy_generic_sequence_get_item_count(rows) == 2);
	FYAI_TCHECK(!strcmp(row_branch(rows, 0), "there"));

	fy_generic_builder_destroy(gb);
	return 0;
}

int branch_select_legacy(void)
{
	struct fy_generic_builder *gb;
	fy_generic branches, rows;
	struct fyai_branch b;

	gb = builder_new();
	FYAI_TCHECK(gb);
	branches = fy_mapping(gb,
		"old", entry_legacy(gb, 500),
		"new", entry_new(gb, 10, 100, "/w"));

	/* A legacy entry records no directory, so it is unknown here. */
	rows = fyai_branch_select_rows(NULL, gb, branches, "/w", false);
	FYAI_TCHECK(fy_generic_sequence_get_item_count(rows) == 1);
	FYAI_TCHECK(!strcmp(row_branch(rows, 0), "new"));

	/* It is resumable from the all-directories listing, ordered by the
	 * time its single "created" member stands for. */
	rows = fyai_branch_select_rows(NULL, gb, branches, "/w", true);
	FYAI_TCHECK(fy_generic_sequence_get_item_count(rows) == 2);
	FYAI_TCHECK(!strcmp(row_branch(rows, 0), "old"));

	FYAI_TCHECK(fyai_branch_lookup(branches, "old", &b));
	FYAI_TCHECK(fyai_branch_updated(&b) == 500);
	/* The creation time of a legacy branch is not known. */
	FYAI_TCHECK(!fyai_branch_created(&b));
	FYAI_TCHECK(!fyai_branch_cwd(&b));

	fy_generic_builder_destroy(gb);
	return 0;
}

int branch_select_empty(void)
{
	struct fy_generic_builder *gb;
	fy_generic rows;

	gb = builder_new();
	FYAI_TCHECK(gb);
	rows = fyai_branch_select_rows(NULL, gb, fy_invalid, "/w", false);
	FYAI_TCHECK(fy_is_valid(rows) && fy_empty(rows));
	FYAI_TCHECK(!fyai_branch_pick_last(NULL, fy_invalid, "/w", false));

	rows = fyai_branch_select_rows(NULL, gb, fy_mapping(gb,
			"there", entry_new(gb, 10, 100, "/elsewhere")),
		"/w", false);
	FYAI_TCHECK(fy_is_valid(rows) && fy_empty(rows));

	fy_generic_builder_destroy(gb);
	return 0;
}

int branch_pick_last_newest(void)
{
	struct fy_generic_builder *gb;
	fy_generic branches;
	char *name;

	gb = builder_new();
	FYAI_TCHECK(gb);
	branches = fy_mapping(gb,
		"one", entry_new(gb, 10, 100, "/w"),
		"two", entry_new(gb, 10, 300, "/w"),
		"far", entry_new(gb, 10, 900, "/elsewhere"));

	name = fyai_branch_pick_last(NULL, branches, "/w", false);
	FYAI_TCHECK(name && !strcmp(name, "two"));
	free(name);

	name = fyai_branch_pick_last(NULL, branches, "/w", true);
	FYAI_TCHECK(name && !strcmp(name, "far"));
	free(name);

	name = fyai_branch_pick_last(NULL, branches, "/nowhere", false);
	FYAI_TCHECK(!name);

	fy_generic_builder_destroy(gb);
	return 0;
}

int branch_import_provenance(void)
{
	struct fy_generic_builder *gb;
	struct fyai_branch input, output;
	fy_generic entry;

	gb = builder_new();
	FYAI_TCHECK(gb);
	memset(&input, 0, sizeof(input));
	input.entry = fy_invalid;
	input.config = fy_invalid;
	input.head = fy_invalid;
	input.created = fy_value(gb, 10LL);
	input.updated = fy_value(gb, 20LL);
	input.cwd = fy_value(gb, "/work");
	input.description = fy_invalid;
	input.agent = fy_invalid;
	input.import = fy_mapping(gb, "version", 1LL,
				  "source", "codex", "session_id", "s1",
				  "losses", fy_sequence(gb,
					fy_mapping(gb, "kind", "unsupported_content",
						   "content_type", "input_image")));
	input.op = fy_value(gb, FYAI_BRANCH_OP_TURN);
	input.from = fy_invalid;
	input.prev = fy_invalid;
	entry = fyai_branch_build(gb, &input);
	FYAI_TCHECK(fy_is_valid(entry));
	FYAI_TCHECK(fyai_branch_decode(entry, &output));
	FYAI_TCHECK(fy_is_mapping(output.import));
	FYAI_TCHECK(fy_get(output.import, "version", 0LL) == 1);
	FYAI_TCHECK(!strcmp(fy_get(output.import, "source", ""), "codex"));
	FYAI_TCHECK(!strcmp(fy_get(output.import, "session_id", ""), "s1"));
	FYAI_TCHECK(fy_equal(fy_get_at_path(output.import, "losses", 0,
					  "kind"),
			     "unsupported_content"));
	FYAI_TCHECK(fy_equal(fy_get_at_path(output.import, "losses", 0,
					  "content_type"),
			     "input_image"));

	fy_generic_builder_destroy(gb);
	return 0;
}
