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
#include "fyai_test.h"

#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(diff, unified, diff_unified)
FYAI_TEST_ENTRY(diff, many_edits, diff_many_edits)
FYAI_TEST_ENTRY(diff, stat, diff_stat)
FYAI_TEST_ENTRY(diff, stat_unified, diff_stat_unified)

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

static struct fy_generic_builder *stat_builder(void)
{
	struct fy_generic_builder_cfg cfg = {
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED,
	};

	return fy_generic_builder_create(&cfg);
}

/* Whether row @i of @rows is the file @path with these counts. */
static bool stat_row_is(fy_generic rows, size_t i, const char *path,
			long long added, long long removed, bool binary)
{
	fy_generic row = fy_get_at(rows, i);
	fy_generic gp = fy_get(row, "path", fy_invalid);
	const char *got = fy_castp(&gp, "");
	long long a = fy_get(row, "added", -1LL);
	long long r = fy_get(row, "removed", -1LL);
	/* Two defaults tell a bool of the wrong value from a missing one. */
	bool b = fy_get(row, "binary", false);
	bool b2 = fy_get(row, "binary", true);

	if (strcmp(got, path) || a != added || r != removed || b != binary ||
	    b2 != binary) {
		fprintf(stderr, "row %zu: want %s +%lld -%lld bin %d, got %s +%lld -%lld bin %d\n",
			i, path, added, removed, binary, got, a, r, b);
		return false;
	}
	return true;
}

/* The rows are those of the project diff: git format, one part for a path. */
int diff_stat(void)
{
	static const char diff[] =
		"diff --git a/src/a b.c b/src/a b.c\n"
		"--- a/src/a b.c\n+++ b/src/a b.c\n"
		"@@ -1,3 +1,3 @@\n x\n-old\n+new\n y\n"
		"@@ -9 +9,2 @@\n-- not a header\n+one\n+two\n"
		"diff --git a/new.txt b/new.txt\nnew file mode 100644\n"
		"--- /dev/null\n+++ b/new.txt\n"
		"@@ -0,0 +1,2 @@\n+a\n+b\n"
		"diff --git a/gone.txt b/gone.txt\ndeleted file mode 100644\n"
		"--- a/gone.txt\n+++ /dev/null\n"
		"@@ -1 +0,0 @@\n-bye\n\\ No newline at end of file\n"
		"diff --git a/img.png b/img.png\n"
		"Binary files a/img.png and b/img.png differ\n"
		"diff --git a/mode.sh b/mode.sh\nold mode 100644\nnew mode 100755\n";
	struct fy_generic_builder *gb = stat_builder();
	fy_generic rows;
	int rc = 1;

	FYAI_TCHECK(gb);
	FYAI_TCHECK(!fyai_diff_stat(gb, diff, strlen(diff), &rows));
	FYAI_TCHECK(fy_is_sequence(rows) && fy_len(rows) == 5);
	FYAI_TCHECK(stat_row_is(rows, 0, "src/a b.c", 3, 2, false));
	FYAI_TCHECK(stat_row_is(rows, 1, "new.txt", 2, 0, false));
	FYAI_TCHECK(stat_row_is(rows, 2, "gone.txt", 0, 1, false));
	FYAI_TCHECK(stat_row_is(rows, 3, "img.png", 0, 0, true));
	FYAI_TCHECK(stat_row_is(rows, 4, "mode.sh", 0, 0, false));
	FYAI_TCHECK(!fyai_diff_stat(gb, "", 0, &rows));
	FYAI_TCHECK(fy_is_sequence(rows) && fy_len(rows) == 0);
	rc = 0;
	fy_generic_builder_destroy(gb);
	return rc;
}

/* A plain unified diff has no git rows: the headers start each file. */
int diff_stat_unified(void)
{
	static const char diff[] =
		"--- a\t2026-01-01\n+++ b\t2026-01-02\n"
		"@@ -1,2 +1 @@\n-1\n-2\n+x\n"
		"--- c\n+++ d\n@@ -0,0 +1 @@\n+z\n";
	struct fy_generic_builder *gb = stat_builder();
	static const char from[] = "1\n2\n3\n", to[] = "1\nx\n3\ny\n";
	char *out = NULL;
	fy_generic rows;
	int rc = 1;

	FYAI_TCHECK(gb);
	FYAI_TCHECK(!fyai_diff_stat(gb, diff, strlen(diff), &rows));
	FYAI_TCHECK(fy_len(rows) == 2);
	FYAI_TCHECK(stat_row_is(rows, 0, "b", 1, 2, false));
	FYAI_TCHECK(stat_row_is(rows, 1, "d", 1, 0, false));
	/* The counts agree with the diff that fyai_diff_unified() writes. */
	FYAI_TCHECK(!fyai_diff_unified(from, strlen(from), to, strlen(to), "a",
				       "b", 1, &out));
	FYAI_TCHECK(!fyai_diff_stat(gb, out, strlen(out), &rows));
	FYAI_TCHECK(fy_len(rows) == 1);
	FYAI_TCHECK(stat_row_is(rows, 0, "b", 2, 1, false));
	rc = 0;
	free(out);
	fy_generic_builder_destroy(gb);
	return rc;
}
