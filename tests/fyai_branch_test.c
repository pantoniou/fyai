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
FYAI_TEST_ENTRY(branch, select_empty, branch_select_empty)
FYAI_TEST_ENTRY(branch, pick_last, branch_pick_last_newest)
FYAI_TEST_ENTRY(branch, import_provenance, branch_import_provenance)
FYAI_TEST_ENTRY(branch, adopt_ends_unstored, branch_adopt_ends_unstored)
FYAI_TEST_ENTRY(branch, name_outlives_change, branch_name_outlives_change)

/* An entry with both timestamps and a directory. */
static fy_generic entry_new(struct fy_generic_builder *gb, long long created,
			    long long updated, const char *cwd)
{
	struct fyai_branch b;

	fyai_branch_decode(fy_invalid, &b);
	b.created = fy_value(gb, created);
	b.updated = fy_value(gb, updated);
	b.cwd = cwd ? fy_value(gb, cwd) : fy_invalid;
	b.op = fy_value(gb, FYAI_BRANCH_OP_TURN);
	return fyai_branch_build(gb, &b);
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
	fy_generic branches, rows, store, helper;

	gb = builder_new();
	FYAI_TCHECK(gb);
	/* Each level is built apart: a nested builder call expands to megabytes. */
	store = fy_mapping(gb, "created", fy_value(gb, 10LL), "cwd", fy_value(gb, "/w"),
			   "agent", fy_mapping(gb, "persona", "helper"));
	helper = fy_mapping(gb, "head", fy_null,
		"updated", fy_value(gb, 300LL),
		"op", fy_value(gb, FYAI_BRANCH_OP_TURN),
		"from", fy_null, "prev", fy_null, "store", store);
	branches = fy_mapping(gb,
		"here", entry_new(gb, 10, 100, "/w"),
		"there", entry_new(gb, 10, 200, "/elsewhere"),
		/* A sub-agent branch is work of a turn, never a session. */
		"here/agent:helper", helper);

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

int branch_select_empty(void)
{
	struct fy_generic_builder *gb;
	fy_generic rows;

	gb = builder_new();
	FYAI_TCHECK(gb);
	rows = fyai_branch_select_rows(NULL, gb, fy_invalid, "/w", false);
	FYAI_TCHECK(fy_is_valid(rows) && fy_empty(rows));
	FYAI_TCHECK(!fyai_branch_pick_last(NULL, fy_invalid, "/w", false, NULL));

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

	name = fyai_branch_pick_last(NULL, branches, "/w", false, NULL);
	FYAI_TCHECK(name && !strcmp(name, "two"));
	free(name);

	name = fyai_branch_pick_last(NULL, branches, "/w", true, NULL);
	FYAI_TCHECK(name && !strcmp(name, "far"));
	free(name);

	name = fyai_branch_pick_last(NULL, branches, "/nowhere", false, NULL);
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

/* Adopting a stored branch ends the session that no publish stored. */
int branch_adopt_ends_unstored(void)
{
	struct fy_generic_builder *gb;
	struct fyai_cfg cfg;
	struct fyai_ctx ctx;

	gb = builder_new();
	FYAI_TCHECK(gb);
	memset(&cfg, 0, sizeof(cfg));
	memset(&ctx, 0, sizeof(ctx));
	cfg.gb = builder_new();
	FYAI_TCHECK(cfg.gb);
	ctx.cfg = &cfg;
	ctx.gb = gb;
	ctx.last_message = fy_invalid;
	ctx.arena_config = fy_invalid;
	ctx.arena_catalog = fy_invalid;
	ctx.arena_skills = fy_invalid;
	ctx.branch_prev = fy_invalid;
	ctx.branch_desc = fy_invalid;
	ctx.branch_agent = fy_invalid;
	ctx.arena_branches = fy_mapping(gb,
		"stored", entry_new(gb, 10, 20, "/w"));
	FYAI_TCHECK(!fyai_ctx_set_branch(&ctx, "session/fresh"));
	ctx.session_unstored = fyai_ctx_keep_str(&ctx, "session/fresh");
	FYAI_TCHECK(ctx.session_unstored);

	/* A branch that is not stored is not adopted, and the session stays. */
	FYAI_TCHECK(fyai_branch_adopt(&ctx, "missing", true));
	FYAI_TCHECK(ctx.session_unstored);

	FYAI_TCHECK(!fyai_branch_adopt(&ctx, "stored", true));
	FYAI_TCHECK(!strcmp(fyai_ctx_branch(&ctx), "stored"));
	FYAI_TCHECK(!ctx.session_unstored);

	fy_generic_builder_destroy(cfg.gb);
	fy_generic_builder_destroy(gb);
	return 0;
}

/* A branch name that was handed out stays valid when the branch changes. */
int branch_name_outlives_change(void)
{
	struct fy_generic_builder *gb;
	struct fyai_cfg cfg;
	struct fyai_ctx ctx;
	const char *first, *again;

	gb = builder_new();
	FYAI_TCHECK(gb);
	memset(&cfg, 0, sizeof(cfg));
	memset(&ctx, 0, sizeof(ctx));
	cfg.gb = gb;
	ctx.cfg = &cfg;
	ctx.gb = gb;

	FYAI_TCHECK(!fyai_ctx_set_branch(&ctx, "session/first"));
	first = fyai_ctx_branch(&ctx);
	FYAI_TCHECK(!fyai_ctx_set_branch(&ctx, "other"));
	FYAI_TCHECK(!strcmp(first, "session/first"));
	FYAI_TCHECK(!strcmp(fyai_ctx_branch(&ctx), "other"));

	/* The same name is one string, so a repeated name costs nothing. */
	FYAI_TCHECK(!fyai_ctx_set_branch(&ctx, "session/first"));
	again = fyai_ctx_branch(&ctx);
	FYAI_TCHECK(again == first);

	FYAI_TCHECK(fyai_ctx_set_branch(&ctx, "bad name"));
	FYAI_TCHECK(!strcmp(fyai_ctx_branch(&ctx), "session/first"));
	fy_generic_builder_destroy(gb);
	return 0;
}
