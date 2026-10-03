/*
 * fyai_diff_test.c - tests for the unified line diff
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai_diff.h"

#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(diff, unified, diff_unified)
FYAI_TEST_ENTRY(diff, many_edits, diff_many_edits)

static int failures;

static void expect_diff(const char *what, const char *a, const char *b,
			const char *want)
{
	char *out;

	if (fyai_diff_unified(a, strlen(a), b, strlen(b), "a", "b", 1,
			      &out)) {
		fprintf(stderr, "FAIL: %s: no diff\n", what);
		failures++;
		return;
	}
	if (strcmp(out, want)) {
		fprintf(stderr, "FAIL: %s:\n--- want\n%s--- got\n%s", what,
			want, out);
		failures++;
	}
	free(out);
}

/* The expected texts are what GNU diff -U1 writes for the same input. */
int diff_unified(void)
{
	failures = 0;
	expect_diff("equal", "x\ny\n", "x\ny\n", "");
	expect_diff("empty", "", "", "");
	expect_diff("change",
		    "1\n2\n3\n4\n5\n", "1\n2\nthree\n4\n5\n",
		    "--- a\n+++ b\n@@ -2,3 +2,3 @@\n 2\n-3\n+three\n 4\n");
	expect_diff("insert at start", "b\nc\n", "a\nb\nc\n",
		    "--- a\n+++ b\n@@ -1 +1,2 @@\n+a\n b\n");
	expect_diff("from empty", "", "a\n",
		    "--- a\n+++ b\n@@ -0,0 +1 @@\n+a\n");
	expect_diff("no line feed", "a\nb", "a\nb\n",
		    "--- a\n+++ b\n@@ -1,2 +1,2 @@\n a\n-b\n"
		    "\\ No newline at end of file\n+b\n");
	/* Two changes further apart than twice the context are two hunks. */
	expect_diff("two hunks", "1\n2\n3\n4\n5\n6\n", "x\n2\n3\n4\n5\ny\n",
		    "--- a\n+++ b\n@@ -1,2 +1,2 @@\n-1\n+x\n 2\n"
		    "@@ -5,2 +5,2 @@\n 5\n-6\n+y\n");
	return failures ? 1 : 0;
}

/*
 * Past the bound on edits the region is one replacement: every line of the
 * old region is removed and every line of the new one is added.
 */
int diff_many_edits(void)
{
	char *a, *b, *out, *p;
	size_t i, n, minus, plus;
	int rc;

	n = 3000;
	a = malloc(n * 8);
	b = malloc(n * 8);
	if (!a || !b)
		return 1;
	a[0] = b[0] = '\0';
	for (i = 0; i < n; i++) {
		snprintf(a + strlen(a), n * 8 - strlen(a), "a%zu\n", i);
		snprintf(b + strlen(b), n * 8 - strlen(b), "b%zu\n", i);
	}
	rc = fyai_diff_unified(a, strlen(a), b, strlen(b), "a", "b", 3, &out);
	free(a);
	free(b);
	if (rc)
		return 1;
	minus = plus = 0;
	for (p = out; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL) {
		if (!strncmp(p, "-a", 2))
			minus++;
		if (!strncmp(p, "+b", 2))
			plus++;
	}
	free(out);
	if (minus != n || plus != n) {
		fprintf(stderr, "FAIL: many edits: %zu removed, %zu added\n",
			minus, plus);
		return 1;
	}
	return 0;
}
