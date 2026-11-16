/* SPDX-License-Identifier: MIT */
#define FYAI_MODULE FYAIEM_UNKNOWN

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai.h"
#include "fyai_branch.h"
#include "fyai_query.h"
#include "fyai_test.h"
#include "fyai_test_registry.h"
#include "fyai_test_diag.h"

FYAI_TEST_ENTRY(query, pages, query_pages)
FYAI_TEST_ENTRY(query, stale_cursor, query_stale_cursor)
FYAI_TEST_ENTRY(query, bad_requests, query_bad_requests)
FYAI_TEST_ENTRY(query, if_rev, query_if_rev)
FYAI_TEST_ENTRY(query, row_rev, query_row_rev)
FYAI_TEST_ENTRY(query, limits, query_limits)
FYAI_TEST_ENTRY(query, directory, query_directory)
FYAI_TEST_ENTRY(query, bytes, query_bytes)
FYAI_TEST_ENTRY(query, empty, query_empty)

static struct fy_generic_builder *builder_new(void)
{
	struct fy_generic_builder_cfg config = {
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED,
	};

	return fy_generic_builder_create(&config);
}

static fy_generic entry_new(struct fy_generic_builder *gb, long long updated,
			    const char *cwd, const char *description)
{
	struct fyai_branch b;

	fyai_branch_decode(fy_invalid, &b);
	b.created = fy_value(gb, updated);
	b.updated = fy_value(gb, updated);
	b.cwd = cwd ? fy_value(gb, cwd) : fy_invalid;
	b.op = fy_value(gb, FYAI_BRANCH_OP_TURN);
	if (description)
		b.description = fy_value(gb, description);
	return fyai_branch_build(gb, &b);
}

/* The table of @n branches b000, b001, ... of which b000 is the newest. */
static fy_generic table_new(struct fy_generic_builder *gb, int n)
{
	fy_generic table = fy_invalid;
	char name[16];
	int i;

	for (i = 0; i < n; i++) {
		snprintf(name, sizeof(name), "b%03d", i);
		table = fyai_branches_set(gb, table, name,
					  entry_new(gb, 10000 - i, "/w", NULL));
	}
	return table;
}

static fy_generic ask(struct fy_generic_builder *gb, fy_generic table,
		      fy_generic request)
{
	struct fyai_query_source src = { .branches = table, .cwd = "/w" };

	return fyai_query_answer(gb, &src, request);
}

static const char *error_code(fy_generic answer)
{
	return fy_get(fy_get(answer, "error", fy_invalid), "code", "");
}

int query_pages(void)
{
	struct fy_generic_builder *gb = builder_new();
	fy_generic table, answer, rows;
	const char *next;
	char want[16];
	int seen = 0, pages = 0;
	size_t i;

	FYAI_TCHECK(gb);
	table = table_new(gb, 5);
	answer = ask(gb, table, fy_mapping(gb, "what", "branches", "limit", 2LL));
	for (;;) {
		FYAI_TCHECK(!strcmp(fy_get(answer, "what", ""), "branches"));
		FYAI_TCHECK(fy_get(answer, "total", 0LL) == 5);
		rows = fy_get(answer, "rows", fy_invalid);
		for (i = 0; i < fy_len(rows); i++) {
			snprintf(want, sizeof(want), "b%03d", seen++);
			FYAI_TCHECK(!strcmp(fy_get(fy_get_at(rows, i), "branch", ""),
					    want));
		}
		pages++;
		if (fy_is_null(fy_get(answer, "next", fy_null)))
			break;
		next = fy_get(answer, "next", "");
		answer = ask(gb, table, fy_mapping(gb, "what", "branches",
						   "limit", 2LL, "after", next));
	}
	/* 2 + 2 + 1, and the last page has no cursor. */
	FYAI_TCHECK(seen == 5);
	FYAI_TCHECK(pages == 3);
	fy_generic_builder_destroy(gb);
	return 0;
}

int query_stale_cursor(void)
{
	struct fy_generic_builder *gb = builder_new();
	fy_generic table, answer, moved;
	const char *next;

	FYAI_TCHECK(gb);
	table = table_new(gb, 5);
	answer = ask(gb, table, fy_mapping(gb, "what", "branches", "limit", 2LL));
	next = fy_get(answer, "next", "");
	FYAI_TCHECK(*next);
	/* Another publish: the table is another value, and the cursor is old. */
	moved = fyai_branches_set(gb, table, "b002",
				  entry_new(gb, 20000, "/w", NULL));
	answer = ask(gb, moved, fy_mapping(gb, "what", "branches", "limit", 2LL,
					   "after", next));
	FYAI_TCHECK(!strcmp(error_code(answer), FYAI_QUERY_STALE_CURSOR));
	/* The same table still honours it. */
	answer = ask(gb, table, fy_mapping(gb, "what", "branches", "limit", 2LL,
					   "after", next));
	FYAI_TCHECK(fy_is_valid(fy_get(answer, "rows", fy_invalid)));
	fy_generic_builder_destroy(gb);
	return 0;
}

int query_bad_requests(void)
{
	struct fy_generic_builder *gb = builder_new();
	fy_generic table, answer;

	FYAI_TCHECK(gb);
	table = table_new(gb, 3);
	answer = ask(gb, table, fy_mapping(gb, "what", "nothing"));
	FYAI_TCHECK(!strcmp(error_code(answer), FYAI_QUERY_UNSUPPORTED));
	answer = ask(gb, table, fy_value(gb, "branches"));
	FYAI_TCHECK(!strcmp(error_code(answer), FYAI_QUERY_BAD_REQUEST));
	answer = ask(gb, table, fy_mapping(gb, "what", "branches",
					   "after", "garbage"));
	FYAI_TCHECK(!strcmp(error_code(answer), FYAI_QUERY_BAD_REQUEST));
	answer = ask(gb, table, fy_mapping(gb, "what", "branches",
					   "after", 7LL));
	FYAI_TCHECK(!strcmp(error_code(answer), FYAI_QUERY_BAD_REQUEST));
	fy_generic_builder_destroy(gb);
	return 0;
}

int query_if_rev(void)
{
	struct fy_generic_builder *gb = builder_new();
	fy_generic table, answer, moved;
	long long rev;

	FYAI_TCHECK(gb);
	table = table_new(gb, 3);
	answer = ask(gb, table, fy_mapping(gb, "what", "branches"));
	rev = fy_get(answer, "rev", 0LL);
	FYAI_TCHECK(rev != 0);
	/* The same data: no rows. */
	answer = ask(gb, table, fy_mapping(gb, "what", "branches",
					   "if_rev", rev));
	FYAI_TCHECK(fy_get(answer, "unchanged", false));
	FYAI_TCHECK(fy_get(answer, "rev", 0LL) == rev);
	FYAI_TCHECK(!fy_is_valid(fy_get(answer, "rows", fy_invalid)));
	/* The data moved: the rows come with the new rev. */
	moved = fyai_branches_set(gb, table, "b001",
				  entry_new(gb, 20000, "/w", NULL));
	answer = ask(gb, moved, fy_mapping(gb, "what", "branches",
					   "if_rev", rev));
	FYAI_TCHECK(!fy_get(answer, "unchanged", false));
	FYAI_TCHECK(fy_get(answer, "rev", 0LL) != rev);
	FYAI_TCHECK(fy_len(fy_get(answer, "rows", fy_invalid)) == 3);
	fy_generic_builder_destroy(gb);
	return 0;
}

int query_row_rev(void)
{
	struct fy_generic_builder *gb = builder_new();
	fy_generic table, moved, before, after;
	long long rev_b000, rev_b001;
	size_t i;

	FYAI_TCHECK(gb);
	table = table_new(gb, 3);
	before = fy_get(ask(gb, table, fy_mapping(gb, "what", "branches")),
			"rows", fy_invalid);
	moved = fyai_branches_set(gb, table, "b001",
				  entry_new(gb, 20000, "/w", NULL));
	after = fy_get(ask(gb, moved, fy_mapping(gb, "what", "branches")),
		       "rows", fy_invalid);
	/* b001 is the newest now: b001, b000, b002. */
	FYAI_TCHECK(!strcmp(fy_get(fy_get_at(after, 0), "branch", ""), "b001"));
	rev_b000 = rev_b001 = 0;
	for (i = 0; i < 3; i++) {
		if (!strcmp(fy_get(fy_get_at(before, i), "branch", ""), "b000"))
			rev_b000 = fy_get(fy_get_at(before, i), "rev", 0LL);
		if (!strcmp(fy_get(fy_get_at(before, i), "branch", ""), "b001"))
			rev_b001 = fy_get(fy_get_at(before, i), "rev", 0LL);
	}
	/* The branch that did not change keeps its rev, and the one that did
	 * has another. */
	FYAI_TCHECK(rev_b000 && rev_b001);
	FYAI_TCHECK(fy_get(fy_get_at(after, 1), "rev", 0LL) == rev_b000);
	FYAI_TCHECK(fy_get(fy_get_at(after, 0), "rev", 0LL) != rev_b001);
	fy_generic_builder_destroy(gb);
	return 0;
}

int query_limits(void)
{
	struct fy_generic_builder *gb = builder_new();
	fy_generic table, answer;

	FYAI_TCHECK(gb);
	table = table_new(gb, 1100);
	/* No limit, or a limit under one: the default. */
	answer = ask(gb, table, fy_mapping(gb, "what", "branches"));
	FYAI_TCHECK(fy_len(fy_get(answer, "rows", fy_invalid)) ==
		    FYAI_QUERY_LIMIT_DEFAULT);
	answer = ask(gb, table, fy_mapping(gb, "what", "branches", "limit", -4LL));
	FYAI_TCHECK(fy_len(fy_get(answer, "rows", fy_invalid)) ==
		    FYAI_QUERY_LIMIT_DEFAULT);
	/* A limit over the most: the most. */
	answer = ask(gb, table, fy_mapping(gb, "what", "branches",
					   "limit", 100000LL));
	FYAI_TCHECK(fy_len(fy_get(answer, "rows", fy_invalid)) ==
		    FYAI_QUERY_LIMIT_MAX);
	FYAI_TCHECK(fy_get(answer, "total", 0LL) == 1100);
	FYAI_TCHECK(!fy_is_null(fy_get(answer, "next", fy_null)));
	fy_generic_builder_destroy(gb);
	return 0;
}

int query_directory(void)
{
	struct fy_generic_builder *gb = builder_new();
	fy_generic table, answer;

	FYAI_TCHECK(gb);
	table = fyai_branches_set(gb, fy_invalid, "here",
				  entry_new(gb, 30, "/w", NULL));
	table = fyai_branches_set(gb, table, "there",
				  entry_new(gb, 20, "/other", NULL));
	/* The directory of the source, then the one that the request names,
	 * then all of them. */
	answer = ask(gb, table, fy_mapping(gb, "what", "branches"));
	FYAI_TCHECK(fy_get(answer, "total", 0LL) == 1);
	FYAI_TCHECK(!strcmp(fy_get(fy_get_at(fy_get(answer, "rows", fy_invalid),
					     0), "branch", ""), "here"));
	answer = ask(gb, table, fy_mapping(gb, "what", "branches", "params",
					   fy_mapping(gb, "dir", "/other")));
	FYAI_TCHECK(fy_get(answer, "total", 0LL) == 1);
	FYAI_TCHECK(!strcmp(fy_get(fy_get_at(fy_get(answer, "rows", fy_invalid),
					     0), "branch", ""), "there"));
	answer = ask(gb, table, fy_mapping(gb, "what", "branches", "params",
					   fy_mapping(gb, "all", true)));
	FYAI_TCHECK(fy_get(answer, "total", 0LL) == 2);
	fy_generic_builder_destroy(gb);
	return 0;
}

int query_bytes(void)
{
	struct fy_generic_builder *gb = builder_new();
	fy_generic table, answer;
	char *big;
	int pages = 0, rows = 0;
	const char *next;

	FYAI_TCHECK(gb);
	big = malloc(600 * 1024 + 1);
	FYAI_TCHECK(big);
	memset(big, 'x', 600 * 1024);
	big[600 * 1024] = '\0';
	/* Each description is over half of an answer: one row a page. */
	table = fyai_branches_set(gb, fy_invalid, "a",
				  entry_new(gb, 30, "/w", big));
	table = fyai_branches_set(gb, table, "b", entry_new(gb, 20, "/w", big));
	table = fyai_branches_set(gb, table, "c", entry_new(gb, 10, "/w", big));
	answer = ask(gb, table, fy_mapping(gb, "what", "branches"));
	for (;;) {
		FYAI_TCHECK(fy_len(fy_get(answer, "rows", fy_invalid)) == 1);
		rows++;
		pages++;
		if (fy_is_null(fy_get(answer, "next", fy_null)))
			break;
		next = fy_get(answer, "next", "");
		answer = ask(gb, table, fy_mapping(gb, "what", "branches",
						   "after", next));
	}
	FYAI_TCHECK(rows == 3 && pages == 3);
	free(big);
	fy_generic_builder_destroy(gb);
	return 0;
}

int query_empty(void)
{
	struct fy_generic_builder *gb = builder_new();
	fy_generic answer;

	FYAI_TCHECK(gb);
	answer = ask(gb, fy_invalid, fy_mapping(gb, "what", "branches"));
	FYAI_TCHECK(fy_len(fy_get(answer, "rows", fy_invalid)) == 0);
	FYAI_TCHECK(fy_get(answer, "total", -1LL) == 0);
	FYAI_TCHECK(fy_is_null(fy_get(answer, "next", fy_invalid)));
	fy_generic_builder_destroy(gb);
	return 0;
}
