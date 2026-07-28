/*
 * fyai_config_test.c - unit tests for config/storage document handling
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai_branch.h"
#include "fyai_storage.h"

#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(config, root_decode, config_root_decode)

static int failures;

#define check(_cond, _msg) \
	do { \
		if (!(_cond)) { \
			fprintf(stderr, "FAIL: %s\n", (_msg)); \
			failures++; \
		} \
	} while (0)

static void test_root_decode(struct fy_generic_builder *gb)
{
	fy_generic root, turn, entry;
	struct fyai_branch b;
	struct fyai_root r;
	int ver;

	/* full container root */
	turn = fy_gb_mapping(gb, "messages", fy_gb_sequence(gb),
			     "previous", fy_null);
	entry = fy_gb_mapping(gb,
			      "config", fy_gb_mapping(gb, "model", "m1"),
			      "head", turn,
			      "prev", fy_null);
	root = fy_gb_mapping(gb,
			     "fyai", (long long)FYAI_ROOT_VERSION,
			     "catalog", fy_null,
			     "HEAD", "main",
			     "branches", fy_gb_mapping(gb, "main", entry));
	ver = fyai_root_decode(root, &r);
	check(ver == FYAI_ROOT_VERSION, "container root: version");
	check(fy_generic_is_valid(r.branches), "container root: branches");
	check(fy_generic_is_invalid(r.catalog),
	      "container root: null catalog decodes as invalid");

	check(fyai_branch_lookup(r.branches, "main", &b),
	      "branch lookup: main found");
	check(fy_generic_is_valid(b.head), "branch: head extracted");
	check(!strcmp(fy_get(b.config, "model", ""), "m1"),
	      "branch: config content");

	/* minimal root: version only */
	root = fy_gb_mapping(gb, "fyai", (long long)FYAI_ROOT_VERSION);
	ver = fyai_root_decode(root, &r);
	check(ver == FYAI_ROOT_VERSION, "minimal root: version");
	check(fy_generic_is_invalid(r.branches) &&
	      fy_generic_is_invalid(r.catalog), "minimal root: all parts absent");

	/* legacy turn-shaped root: rejected (no back-compat) */
	ver = fyai_root_decode(turn, &r);
	check(ver < 0, "legacy turn root rejected");
	check(fy_generic_is_invalid(r.branches), "rejected root: parts cleared");

	/* future version: rejected */
	root = fy_gb_mapping(gb, "fyai", 999LL);
	check(fyai_root_decode(root, &r) < 0, "future version rejected");

	/* garbage */
	check(fyai_root_decode(fy_invalid, &r) < 0, "invalid root rejected");
	check(fyai_root_decode(fy_value(gb, "scalar"), &r) < 0,
	      "scalar root rejected");
}

int config_root_decode(void)
{
	struct fy_generic_builder_cfg gb_cfg = {
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED,
	};
	struct fy_generic_builder *gb;

	failures = 0;

	gb = fy_generic_builder_create(&gb_cfg);
	if (!gb)
		return 1;

	test_root_decode(gb);

	fy_generic_builder_destroy(gb);
	return failures ? 1 : 0;
}
