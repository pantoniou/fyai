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
#include "fyai_merge.h"
#include "fyai_storage.h"

#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(config, root_decode, config_root_decode)
FYAI_TEST_ENTRY(config, branch_names, config_branch_names)
FYAI_TEST_ENTRY(config, branch_refs, config_branch_refs)
FYAI_TEST_ENTRY(config, merge_base, config_merge_base)

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
	check(fyai_root_head_name(&r) &&
	      !strcmp(fyai_root_head_name(&r), "main"),
	      "container root: HEAD name");

	check(fyai_branch_lookup(r.branches, "main", &b),
	      "branch lookup: main found");
	check(fy_generic_is_valid(b.head), "branch: head extracted");
	check(fy_generic_is_valid(b.config), "branch: config extracted");
	check(!strcmp(fy_get(b.config, "model", ""), "m1"),
	      "branch: config content");
	check(fy_generic_is_invalid(b.prev), "branch: null prev is invalid");

	check(!fyai_branch_lookup(r.branches, "nope", &b),
	      "branch lookup: absent reports false");
	check(fy_generic_is_invalid(b.head) && fy_generic_is_invalid(b.config),
	      "branch lookup: absent clears the parts");

	/* minimal root: version only */
	root = fy_gb_mapping(gb, "fyai", (long long)FYAI_ROOT_VERSION);
	ver = fyai_root_decode(root, &r);
	check(ver == FYAI_ROOT_VERSION, "minimal root: version");
	check(fy_generic_is_invalid(r.branches) &&
	      fy_generic_is_invalid(r.catalog), "minimal root: all parts absent");
	check(!fyai_root_head_name(&r), "minimal root: no HEAD name");

	/* legacy turn-shaped root: rejected (no back-compat) */
	ver = fyai_root_decode(turn, &r);
	check(ver < 0, "legacy turn root rejected");
	check(fy_generic_is_invalid(r.branches), "rejected root: parts cleared");

	/* version 1 is the pre-branching schema and is not migrated */
	root = fy_gb_mapping(gb, "fyai", 1LL,
			     "config", fy_gb_mapping(gb, "model", "m1"),
			     "head", turn);
	check(fyai_root_decode(root, &r) < 0, "version 1 rejected");

	/* future version: rejected */
	root = fy_gb_mapping(gb, "fyai", 999LL);
	check(fyai_root_decode(root, &r) < 0, "future version rejected");

	/* garbage */
	check(fyai_root_decode(fy_invalid, &r) < 0, "invalid root rejected");
	check(fyai_root_decode(fy_value(gb, "scalar"), &r) < 0,
	      "scalar root rejected");
}

static void test_branch_names(void)
{
	check(fyai_branch_name_valid("main"), "name: main");
	check(fyai_branch_name_valid("main/explore-1"), "name: nested");
	check(fyai_branch_name_valid("a/b/c"), "name: deeply nested");
	check(fyai_branch_name_valid("feature.x"), "name: dot inside a word");

	check(!fyai_branch_name_valid(NULL), "name: NULL rejected");
	check(!fyai_branch_name_valid(""), "name: empty rejected");
	check(!fyai_branch_name_valid("HEAD"), "name: HEAD rejected");
	check(!fyai_branch_name_valid("/main"), "name: leading slash rejected");
	check(!fyai_branch_name_valid("main/"), "name: trailing slash rejected");
	check(!fyai_branch_name_valid("a//b"), "name: doubled slash rejected");
	check(!fyai_branch_name_valid("a/./b"), "name: dot component rejected");
	check(!fyai_branch_name_valid("a/../b"), "name: dotdot rejected");
	check(!fyai_branch_name_valid("a b"), "name: space rejected");
	check(!fyai_branch_name_valid("a~1"), "name: tilde rejected");
	check(!fyai_branch_name_valid("a@b"), "name: at rejected");
	check(!fyai_branch_name_valid("a:b"), "name: colon rejected");
	check(!fyai_branch_name_valid("a*b"), "name: star rejected");

	check(fyai_branch_is_below("main", "main"), "below: self");
	check(fyai_branch_is_below("main/explore-1", "main"), "below: child");
	check(fyai_branch_is_below("main/a/b", "main/a"), "below: grandchild");
	check(!fyai_branch_is_below("mainline", "main"),
	      "below: prefix without a slash boundary is not a child");
	check(!fyai_branch_is_below("other", "main"), "below: unrelated");
	check(fyai_branch_is_below("anything", NULL), "below: NULL parent");

	check(fyai_branch_depth("main") == 0, "depth: top level");
	check(fyai_branch_depth("main/a") == 1, "depth: one down");
	check(fyai_branch_depth("main/a/b") == 2, "depth: two down");
}

static void test_branch_sanitize(void)
{
	char b[FYAI_BRANCH_COMPONENT_MAX + 1];

	fyai_branch_sanitize("Explore", "agent", b, sizeof(b));
	check(!strcmp(b, "explore"), "sanitize: lower-cased");
	fyai_branch_sanitize("general-purpose", "agent", b, sizeof(b));
	check(!strcmp(b, "general-purpose"), "sanitize: already clean");
	fyai_branch_sanitize("Code Reviewer", "agent", b, sizeof(b));
	check(!strcmp(b, "code-reviewer"), "sanitize: space becomes a dash");
	fyai_branch_sanitize("a//b", "agent", b, sizeof(b));
	check(!strcmp(b, "a-b"), "sanitize: slash cannot survive");
	check(!strchr(b, '/'), "sanitize: never contains a separator");
	fyai_branch_sanitize("~weird^stuff@", "agent", b, sizeof(b));
	check(!strcmp(b, "weird-stuff"), "sanitize: trimmed and collapsed");
	fyai_branch_sanitize("...", "agent", b, sizeof(b));
	check(!strcmp(b, "agent"), "sanitize: dots fall back");
	fyai_branch_sanitize("", "agent", b, sizeof(b));
	check(!strcmp(b, "agent"), "sanitize: empty falls back");
	fyai_branch_sanitize(NULL, "agent", b, sizeof(b));
	check(!strcmp(b, "agent"), "sanitize: NULL falls back");
	fyai_branch_sanitize("HEAD", "agent", b, sizeof(b));
	check(fyai_branch_name_valid(b), "sanitize: HEAD is made usable");

	fyai_branch_sanitize("....~~~^^^@@@:::", "agent", b, sizeof(b));
	check(fyai_branch_name_valid(b), "sanitize: punctuation only is valid");
	fyai_branch_sanitize(
		"an extremely long agent name that runs well past the limit",
		"agent", b, sizeof(b));
	check(strlen(b) <= FYAI_BRANCH_COMPONENT_MAX, "sanitize: truncated");
	check(fyai_branch_name_valid(b), "sanitize: truncation stays valid");
}

static void test_ref_grammar(struct fy_generic_builder *gb)
{
	fy_generic t1, t2, t3, entry, branches;
	struct fyai_branch b;

	/* a three turn chain: t3 -> t2 -> t1 */
	t1 = fy_gb_mapping(gb, "messages", fy_gb_sequence(gb),
			   "previous", fy_null);
	t2 = fy_gb_mapping(gb, "messages", fy_gb_sequence(gb), "previous", t1);
	t3 = fy_gb_mapping(gb, "messages", fy_gb_sequence(gb), "previous", t2);

	/* one ref-log step behind, recording the operation that made it */
	entry = fy_gb_mapping(gb, "head", t2, "op", "turn", "prev", fy_null);
	entry = fy_gb_mapping(gb, "head", t3, "op", "reset", "prev", entry);
	branches = fy_gb_mapping(gb, "main", entry);

	check(fyai_branch_lookup(branches, "main", &b), "refs: branch found");
	check(fy_equal(b.head, t3), "refs: head is the tip");
	check(fy_generic_is_valid(b.op), "refs: the operation is stored");
	check(!strcmp(fy_castp(&b.op, ""), "reset"),
	      "refs: a reset is recorded, not inferred");

	/* the ref log keeps what a reset moved away from */
	check(fyai_branch_decode(b.prev, &b), "refs: ref log has a predecessor");
	check(fy_equal(b.head, t2), "refs: @{1} recovers the previous head");
}


/* Build a turn whose only user message is @text, chained onto @prev. */
static fy_generic mk_turn(struct fy_generic_builder *gb, fy_generic prev,
			  const char *role, const char *text)
{
	return fy_gb_mapping(gb,
		"previous", fy_generic_is_valid(prev) ? prev : fy_null,
		"messages", fy_gb_sequence(gb,
			fy_gb_mapping(gb, "role", role, "content", text)));
}

static void test_merge_base(struct fy_generic_builder *gb)
{
	fy_generic sys, a1, a2, b1, base, o1, o2, dup;

	/* sys -> a1 -> a2   and   sys -> a1 -> b1 */
	sys = mk_turn(gb, fy_invalid, "system", "S");
	a1 = mk_turn(gb, sys, "user", "A");
	a2 = mk_turn(gb, a1, "user", "B");
	b1 = mk_turn(gb, a1, "user", "C");

	base = fyai_merge_base(NULL, a2, b1);
	check(fy_generic_is_valid(base), "merge base: found");
	check(base.v == a1.v, "merge base: newest shared turn");

	/* identical chains: the base is the tip itself */
	base = fyai_merge_base(NULL, a2, a2);
	check(base.v == a2.v, "merge base: self");

	/*
	 * Two chains that share nothing. Content-addressing means a turn with
	 * identical content IS the same value, so an unrelated chain has to
	 * differ in content to be genuinely unrelated.
	 */
	o1 = mk_turn(gb, fy_invalid, "system", "OTHER");
	o2 = mk_turn(gb, o1, "user", "Z");
	check(fy_generic_is_invalid(fyai_merge_base(NULL, a2, o2)),
	      "merge base: unrelated chains share nothing");

	/* a turn shared by content is shared in fact: dedup makes it one value */
	dup = mk_turn(gb, fy_invalid, "system", "S");
	check(dup.v == sys.v, "merge base: equal content is one stored value");
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

int config_branch_names(void)
{
	failures = 0;
	test_branch_names();
	test_branch_sanitize();
	return failures ? 1 : 0;
}

int config_merge_base(void)
{
	struct fy_generic_builder_cfg gb_cfg = {
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED,
	};
	struct fy_generic_builder *gb;

	failures = 0;

	gb = fy_generic_builder_create(&gb_cfg);
	if (!gb)
		return 1;

	test_merge_base(gb);

	fy_generic_builder_destroy(gb);
	return failures ? 1 : 0;
}

int config_branch_refs(void)
{
	struct fy_generic_builder_cfg gb_cfg = {
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED,
	};
	struct fy_generic_builder *gb;

	failures = 0;

	gb = fy_generic_builder_create(&gb_cfg);
	if (!gb)
		return 1;

	test_ref_grammar(gb);

	fy_generic_builder_destroy(gb);
	return failures ? 1 : 0;
}
