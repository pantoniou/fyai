/*
 * fyai_ignore_test.c - tests for the ignore rules of gitignore(5)
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fyai_ignore.h"
#include "fyai_scratch.h"

#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(ignore, patterns, ignore_patterns)
FYAI_TEST_ENTRY(ignore, precedence, ignore_precedence)
FYAI_TEST_ENTRY(ignore, tree, ignore_tree)

static int failures;

#define CHECK(cond)                                                                       \
	do {                                                                              \
		if (!(cond)) {                                                            \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                                       \
		}                                                                         \
	} while (0)

static void expect(const struct fyai_ignore_scope *first, const struct fyai_ignore_scope *scope,
		   const char *path, bool is_dir, bool want)
{
	if (fyai_ignore_match(first, scope, path, is_dir) == want)
		return;
	fprintf(stderr, "FAIL: %s%s is %s\n", path, is_dir ? "/" : "",
		want ? "not ignored" : "ignored");
	failures++;
}

static struct fyai_ignore_scope *parse(struct fyai_scratch *scratch,
				       const struct fyai_ignore_scope *parent, const char *base,
				       const char *text)
{
	struct fyai_ignore_scope *scope = fyai_ignore_parse(scratch, parent, base, text, strlen(text));

	if (!scope) {
		fprintf(stderr, "FAIL: cannot parse\n");
		failures++;
	}
	return scope;
}

int ignore_patterns(void)
{
	struct fyai_scratch scratch = { 0 };
	struct fyai_ignore_scope *s;

	failures = 0;
	if (fyai_scratch_open(&scratch))
		return 1;

	/* A pattern with no slash names a file or a directory at any depth. */
	s = parse(&scratch, NULL, "", "*.o\nfoo\n");
	expect(NULL, s, "a.o", false, true);
	expect(NULL, s, "x/y/a.o", false, true);
	expect(NULL, s, "a.c", false, false);
	expect(NULL, s, "x/foo", true, true);
	expect(NULL, s, "x/foo", false, true);

	/* A trailing slash selects directories. */
	s = parse(&scratch, NULL, "", "build/\n");
	expect(NULL, s, "build", true, true);
	expect(NULL, s, "src/build", true, true);
	expect(NULL, s, "build", false, false);

	/* A slash at the start or in the middle anchors the pattern. */
	s = parse(&scratch, NULL, "", "/top\ndoc/*.txt\n");
	expect(NULL, s, "top", false, true);
	expect(NULL, s, "src/top", false, false);
	expect(NULL, s, "doc/a.txt", false, true);
	expect(NULL, s, "doc/x/a.txt", false, false);
	expect(NULL, s, "src/doc/a.txt", false, false);

	/* ** takes directories. */
	s = parse(&scratch, NULL, "", "a/**/b\n**/leaf\nabc/**\n");
	expect(NULL, s, "a/b", false, true);
	expect(NULL, s, "a/x/b", false, true);
	expect(NULL, s, "a/x/y/b", false, true);
	expect(NULL, s, "b", false, false);
	expect(NULL, s, "leaf", false, true);
	expect(NULL, s, "x/y/leaf", false, true);
	expect(NULL, s, "abc/x", false, true);
	expect(NULL, s, "abc/x/y", false, true);
	expect(NULL, s, "abc", true, false);

	/* ? and a class match one name character. */
	s = parse(&scratch, NULL, "", "f?.[ch]\n");
	expect(NULL, s, "fa.c", false, true);
	expect(NULL, s, "fa.h", false, true);
	expect(NULL, s, "fa.o", false, false);
	expect(NULL, s, "fab.c", false, false);

	/* Comments, blank lines, trailing blanks, a carriage return and escapes. */
	s = parse(&scratch, NULL, "", "# comment\n\nkeep  \r\n\\#hash\n\\!bang\nsp\\ \n");
	expect(NULL, s, "# comment", false, false);
	expect(NULL, s, "keep", false, true);
	expect(NULL, s, "#hash", false, true);
	expect(NULL, s, "!bang", false, true);
	expect(NULL, s, "sp ", false, true);

	fyai_scratch_close(&scratch);
	return failures ? 1 : 0;
}

int ignore_precedence(void)
{
	struct fyai_scratch scratch = { 0 };
	const char *const lines[] = { "!keep.o", "extra/" };
	struct fyai_ignore_scope *root, *doc, *config;

	failures = 0;
	if (fyai_scratch_open(&scratch))
		return 1;

	/* The last rule that matches decides. */
	root = parse(&scratch, NULL, "", "*.log\n!keep.log\n*.tmp\n");
	expect(NULL, root, "a.log", false, true);
	expect(NULL, root, "keep.log", false, false);
	expect(NULL, root, "dir/keep.log", false, false);

	/* A file nearer to the path decides before one farther from it. */
	root = parse(&scratch, NULL, "", "*.txt\n");
	doc = parse(&scratch, root, "doc", "!a.txt\n/only.md\n");
	expect(NULL, doc, "doc/a.txt", false, false);
	expect(NULL, doc, "doc/b.txt", false, true);
	expect(NULL, doc, "x/a.txt", false, true);
	expect(NULL, doc, "doc/only.md", false, true);
	expect(NULL, doc, "doc/sub/only.md", false, false);
	expect(NULL, doc, "other/only.md", false, false);

	/* The configuration outranks every file. */
	root = parse(&scratch, NULL, "", "*.o\n");
	config = fyai_ignore_parse_lines(&scratch, lines, 2);
	if (!config) {
		fprintf(stderr, "FAIL: cannot parse the lines\n");
		failures++;
	} else {
		expect(config, root, "a.o", false, true);
		expect(config, root, "keep.o", false, false);
		expect(config, root, "extra", true, true);
		expect(config, root, "extra", false, false);
	}

	/* No rules: nothing is ignored. */
	expect(NULL, NULL, "anything", false, false);

	fyai_scratch_close(&scratch);
	return failures ? 1 : 0;
}

static void put_file(int root, const char *path, const char *text)
{
	int fd = openat(root, path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

	if (fd < 0 || write(fd, text, strlen(text)) < 0) {
		fprintf(stderr, "FAIL: cannot write %s\n", path);
		failures++;
	}
	if (fd >= 0)
		close(fd);
}

static void expect_tree(struct fyai_ignore_tree *tree, const char *path, bool is_dir, bool want)
{
	if (fyai_ignore_tree_match(tree, path, is_dir) == want)
		return;
	fprintf(stderr, "FAIL: tree: %s%s is %s\n", path, is_dir ? "/" : "",
		want ? "not ignored" : "ignored");
	failures++;
}

/* Remove what the test made by name: no glob, and nothing outside the directory. */
static void tree_cleanup(int root, const char *base)
{
	static const char *const files[] = { ".gitignore", "doc/.gitignore", ".git/info/exclude" };
	static const char *const dirs[] = { "doc", ".git/info", ".git", "sub", "build" };
	size_t i;

	for (i = 0; i < sizeof(files) / sizeof(*files); i++)
		unlinkat(root, files[i], 0);
	for (i = 0; i < sizeof(dirs) / sizeof(*dirs); i++)
		unlinkat(root, dirs[i], AT_REMOVEDIR);
	rmdir(base);
}

int ignore_tree(void)
{
	char base[] = "/tmp/fyai-ignore-XXXXXX";
	struct fyai_ignore_spec spec = { .gitignore = true, .patterns = fy_seq_empty };
	struct fyai_ignore_tree *tree;
	int root;

	failures = 0;
	if (!mkdtemp(base))
		return 1;
	root = open(base, O_RDONLY | O_DIRECTORY);
	mkdirat(root, "doc", 0755);
	mkdirat(root, "sub", 0755);
	mkdirat(root, "build", 0755);
	mkdirat(root, ".git", 0755);
	mkdirat(root, ".git/info", 0755);
	put_file(root, ".gitignore", "*.o\nbuild/\n!keep.o\n");
	put_file(root, "doc/.gitignore", "*.tmp\n!a.o\n");
	put_file(root, ".git/info/exclude", "excluded.txt\n");

	/* The files of the tree are read as git reads them, from the directory of the path. */
	tree = fyai_ignore_tree_open(&spec, root);
	CHECK(tree != NULL);
	expect_tree(tree, "a.o", false, true);
	expect_tree(tree, "keep.o", false, false);
	expect_tree(tree, "src/b.o", false, true);
	expect_tree(tree, "main.c", false, false);
	/* A directory that is ignored hides what is below it, whatever the name below is. */
	expect_tree(tree, "build", true, true);
	expect_tree(tree, "build/out", false, true);
	expect_tree(tree, "build/deep/keep.o", false, true);
	/* A file in a directory is nearer to the path than the root, and decides first. */
	expect_tree(tree, "doc/x.tmp", false, true);
	expect_tree(tree, "doc/a.o", false, false);
	expect_tree(tree, "doc/b.o", false, true);
	expect_tree(tree, "sub/x.tmp", false, false);
	/* The exclude file of the repository counts, below the ignore files. */
	expect_tree(tree, "excluded.txt", false, true);
	expect_tree(tree, "sub/excluded.txt", false, true);
	fyai_ignore_tree_close(tree);

	/* The rules of the configuration outrank the files. */
	spec.patterns = fy_sequence("!a.o", "src/", "cfg.local");
	tree = fyai_ignore_tree_open(&spec, root);
	CHECK(tree != NULL);
	expect_tree(tree, "a.o", false, false);
	expect_tree(tree, "b.o", false, true);
	expect_tree(tree, "src", true, true);
	expect_tree(tree, "src/main.c", false, true);
	expect_tree(tree, "cfg.local", false, true);
	fyai_ignore_tree_close(tree);

	/* Without the ignore files only the rules of the configuration count. */
	spec.gitignore = false;
	tree = fyai_ignore_tree_open(&spec, root);
	CHECK(tree != NULL);
	expect_tree(tree, "b.o", false, false);
	expect_tree(tree, "build/out", false, false);
	expect_tree(tree, "cfg.local", false, true);
	fyai_ignore_tree_close(tree);

	/* Nothing to apply: no matcher. */
	spec.patterns = fy_seq_empty;
	CHECK(fyai_ignore_tree_open(&spec, root) == NULL);

	tree_cleanup(root, base);
	close(root);
	return failures ? 1 : 0;
}
