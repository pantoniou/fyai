/*
 * fyai_config_test.c - unit tests for config/storage document handling
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai_storage.h"

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
	fy_generic root, head, config, catalog, turn;
	int ver;

	/* full container root */
	turn = fy_gb_mapping(gb, "messages", fy_gb_sequence(gb),
			     "previous", fy_null);
	root = fy_gb_mapping(gb,
			     "fyai", (long long)FYAI_ROOT_VERSION,
			     "config", fy_gb_mapping(gb, "model", "m1"),
			     "catalog", fy_null,
			     "head", turn);
	ver = fyai_root_decode(root, &head, &config, &catalog);
	check(ver == FYAI_ROOT_VERSION, "container root: version");
	check(fy_generic_is_valid(head), "container root: head extracted");
	check(fy_generic_is_valid(config), "container root: config extracted");
	check(!strcmp(fy_get(config, "model", ""), "m1"),
	      "container root: config content");
	check(fy_generic_is_invalid(catalog),
	      "container root: null catalog decodes as invalid");

	/* minimal root: version only */
	root = fy_gb_mapping(gb, "fyai", (long long)FYAI_ROOT_VERSION);
	ver = fyai_root_decode(root, &head, &config, &catalog);
	check(ver == FYAI_ROOT_VERSION, "minimal root: version");
	check(fy_generic_is_invalid(head) && fy_generic_is_invalid(config) &&
	      fy_generic_is_invalid(catalog), "minimal root: all parts absent");

	/* legacy turn-shaped root: rejected (MVP, no back-compat) */
	ver = fyai_root_decode(turn, &head, &config, &catalog);
	check(ver < 0, "legacy turn root rejected");
	check(fy_generic_is_invalid(head), "rejected root: head cleared");

	/* future version: rejected */
	root = fy_gb_mapping(gb, "fyai", 999LL);
	check(fyai_root_decode(root, &head, &config, &catalog) < 0,
	      "future version rejected");

	/* garbage */
	check(fyai_root_decode(fy_invalid, &head, &config, &catalog) < 0,
	      "invalid root rejected");
	check(fyai_root_decode(fy_value(gb, "scalar"), &head,
			       &config, &catalog) < 0, "scalar root rejected");

	/* NULL out-pointers allowed */
	root = fy_gb_mapping(gb, "fyai", (long long)FYAI_ROOT_VERSION,
			     "head", turn);
	check(fyai_root_decode(root, NULL, NULL, NULL) == FYAI_ROOT_VERSION,
	      "NULL out-pointers");
}

int main(void)
{
	struct fy_generic_builder_cfg gb_cfg = {
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED,
	};
	struct fy_generic_builder *gb;

	gb = fy_generic_builder_create(&gb_cfg);
	if (!gb)
		return 1;

	test_root_decode(gb);

	fy_generic_builder_destroy(gb);
	return failures ? 1 : 0;
}
