/*
 * fyai_patch_test.c - tests for the Codex-style patch applier
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fyai_patch.h"
#include "utils.h"

static void expect_ok(char *result)
{
	if (!result || strncmp(result, "ok:", 3)) {
		fprintf(stderr, "expected ok, got: %s\n",
			result ? result : "(null)");
		exit(1);
	}
	free(result);
}

static void expect_error(char *result)
{
	if (!result || strncmp(result, "tool error:", 11)) {
		fprintf(stderr, "expected error, got: %s\n",
			result ? result : "(null)");
		exit(1);
	}
	free(result);
}

static void expect_file(const char *path, const char *expected)
{
	char *content;

	content = read_text_file(path);
	if (!content) {
		fprintf(stderr, "missing file: %s\n", path);
		exit(1);
	}
	if (strcmp(content, expected)) {
		fprintf(stderr, "bad content for %s:\n%s\n", path, content);
		exit(1);
	}
	free(content);
}

static void write_file_or_die(const char *path, const char *content)
{
	FILE *fp;

	fp = fopen(path, "wb");
	if (!fp || fputs(content, fp) == EOF || fclose(fp)) {
		fprintf(stderr, "failed to write %s\n", path);
		exit(1);
	}
}

int main(void)
{
	char tmpl[] = "/tmp/fyai-patch-test-XXXXXX";
	char *dir;

	dir = mkdtemp(tmpl);
	if (!dir)
		return 1;
	if (chdir(dir))
		return 1;

	expect_ok(fyai_apply_patch_text(
		"*** Begin Patch\n"
		"*** Add File: a.txt\n"
		"+one\n"
		"+two\n"
		"*** End Patch\n"));
	expect_file("a.txt", "one\ntwo\n");

	expect_ok(fyai_apply_patch_text(
		"*** Begin Patch\n"
		"*** Update File: a.txt\n"
		"@@\n"
		" one\n"
		"-two\n"
		"+three\n"
		"*** End Patch\n"));
	expect_file("a.txt", "one\nthree\n");

	expect_error(fyai_apply_patch_text(
		"*** Begin Patch\n"
		"*** Update File: a.txt\n"
		"@@\n"
		"-missing\n"
		"+bad\n"
		"*** End Patch\n"));
	expect_file("a.txt", "one\nthree\n");

	expect_error(fyai_apply_patch_text(
		"*** Begin Patch\n"
		"*** Add File: ../escape.txt\n"
		"+bad\n"
		"*** End Patch\n"));

	expect_error(fyai_apply_patch_text(
		"*** Begin Patch\n"
		"*** Add File: b.txt\n"
		"+bad\n"));
	if (!access("b.txt", F_OK)) {
		fprintf(stderr, "unterminated add wrote file\n");
		return 1;
	}

	expect_error(fyai_apply_patch_text(
		"*** Begin Patch\n"
		"*** Update File: a.txt\n"
		"@@\n"
		"-three\n"
		"+bad\n"));
	expect_file("a.txt", "one\nthree\n");

	/* An Update File hunk with no chunks is rejected, matching codex-rs. */
	expect_error(fyai_apply_patch_text(
		"*** Begin Patch\n"
		"*** Update File: a.txt\n"
		"*** End Patch\n"));
	expect_file("a.txt", "one\nthree\n");

	write_file_or_die("eof.txt", "one\ntwo\n");
	expect_ok(fyai_apply_patch_text(
		"*** Begin Patch\n"
		"*** Update File: eof.txt\n"
		"@@\n"
		" one\n"
		"-two\n"
		"+three\n"
		"*** End of File\n"
		"*** End Patch\n"));
	expect_file("eof.txt", "one\nthree\n");

	write_file_or_die("tricky.txt",
		"alpha\n"
		"\n"
		"section A:\n"
		"  key: value\n"
		"  repeated: same\n"
		"  target: old-a\n"
		"\n"
		"section B:\n"
		"  key: value\n"
		"  repeated: same\n"
		"  target: old-b\n"
		"\n"
		"section C:\n"
		"  key: value\n"
		"  repeated: same\n"
		"  target: old-c\n"
		"omega\n");
	expect_ok(fyai_apply_patch_text(
		"*** Begin Patch\n"
		"*** Update File: tricky.txt\n"
		"@@\n"
		" section A:\n"
		"   key: value\n"
		"   repeated: same\n"
		"-  target: old-a\n"
		"+  target: new-a\n"
		"@@\n"
		" section B:\n"
		"   key: value\n"
		"   repeated: same\n"
		"-  target: old-b\n"
		"+  target: new-b\n"
		"+  inserted: yes\n"
		"@@\n"
		" section C:\n"
		"   key: value\n"
		"   repeated: same\n"
		"-  target: old-c\n"
		"+  target: new-c\n"
		" omega\n"
		"*** Add File: nested/dir/added.txt\n"
		"+nested add worked\n"
		"+with multiple lines\n"
		"*** End Patch\n"));
	expect_file("tricky.txt",
		"alpha\n"
		"\n"
		"section A:\n"
		"  key: value\n"
		"  repeated: same\n"
		"  target: new-a\n"
		"\n"
		"section B:\n"
		"  key: value\n"
		"  repeated: same\n"
		"  target: new-b\n"
		"  inserted: yes\n"
		"\n"
		"section C:\n"
		"  key: value\n"
		"  repeated: same\n"
		"  target: new-c\n"
		"omega\n");
	expect_file("nested/dir/added.txt",
		    "nested add worked\nwith multiple lines\n");

	write_file_or_die("partial.txt", "old\n");
	expect_error(fyai_apply_patch_text(
		"*** Begin Patch\n"
		"*** Update File: partial.txt\n"
		"@@\n"
		"-old\n"
		"+new\n"
		"*** Update File: missing.txt\n"
		"@@\n"
		"-old\n"
		"+new\n"
		"*** End Patch\n"));
	expect_file("partial.txt", "old\n");

	/* Add File overwrites an existing destination, matching codex-rs. */
	write_file_or_die("exists.txt", "exists\n");
	expect_ok(fyai_apply_patch_text(
		"*** Begin Patch\n"
		"*** Add File: exists.txt\n"
		"+overwritten\n"
		"*** End Patch\n"));
	expect_file("exists.txt", "overwritten\n");

	write_file_or_die("move.txt", "hello\n");
	expect_ok(fyai_apply_patch_text(
		"*** Begin Patch\n"
		"*** Update File: move.txt\n"
		"*** Move to: moved/renamed.txt\n"
		"@@\n"
		"-hello\n"
		"+hello moved\n"
		"*** End Patch\n"));
	if (!access("move.txt", F_OK)) {
		fprintf(stderr, "move source still exists\n");
		return 1;
	}
	expect_file("moved/renamed.txt", "hello moved\n");

	expect_ok(fyai_apply_patch_text(
		"*** Begin Patch\n"
		"*** Update File: moved/renamed.txt\n"
		"*** Move to: moved/pure.txt\n"
		"@@\n"
		" hello moved\n"
		"*** End Patch\n"));
	if (!access("moved/renamed.txt", F_OK)) {
		fprintf(stderr, "pure move source still exists\n");
		return 1;
	}
	expect_file("moved/pure.txt", "hello moved\n");

	/* Move to overwrites an existing destination, matching codex-rs. */
	write_file_or_die("move_exists.txt", "exists\n");
	expect_ok(fyai_apply_patch_text(
		"*** Begin Patch\n"
		"*** Update File: moved/pure.txt\n"
		"*** Move to: move_exists.txt\n"
		"@@\n"
		"-hello moved\n"
		"+overwritten\n"
		"*** End Patch\n"));
	if (!access("moved/pure.txt", F_OK)) {
		fprintf(stderr, "move-overwrite source still exists\n");
		return 1;
	}
	expect_file("move_exists.txt", "overwritten\n");

	expect_ok(fyai_apply_patch_text(
		"*** Begin Patch\n"
		"*** Delete File: a.txt\n"
		"*** End Patch\n"));
	if (!access("a.txt", F_OK)) {
		fprintf(stderr, "delete failed\n");
		return 1;
	}

	return 0;
}
