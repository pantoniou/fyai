/*
 * fyai_view_fs_test.c - tests for the file operations of `view cp` and `view rm`
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fyai_view_fs.h"

#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(view_fs, paths, view_fs_paths)
FYAI_TEST_ENTRY(view_fs, round_trip, view_fs_round_trip)
FYAI_TEST_ENTRY(view_fs, replace, view_fs_replace)
FYAI_TEST_ENTRY(view_fs, refusals, view_fs_refusals)
FYAI_TEST_ENTRY(view_fs, remove, view_fs_remove)

static int failures;

#define CHECK(cond)                                                                   \
	do {                                                                          \
		if (!(cond)) {                                                        \
			fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                                   \
		}                                                                     \
	} while (0)

struct stream {
	char *data;
	size_t length, capacity, position;
};

static int stream_emit(void *arg, const void *data, size_t length)
{
	struct stream *stream = arg;
	char *grown;

	if (stream->length + length > stream->capacity) {
		stream->capacity = (stream->length + length) * 2;
		grown = realloc(stream->data, stream->capacity);
		if (!grown)
			return -1;
		stream->data = grown;
	}
	memcpy(stream->data + stream->length, data, length);
	stream->length += length;
	return 0;
}

static long stream_fill(void *arg, void *data, size_t length)
{
	struct stream *stream = arg;
	size_t n = stream->length - stream->position;

	if (n > length)
		n = length;
	memcpy(data, stream->data + stream->position, n);
	stream->position += n;
	return (long)n;
}

/* A scratch directory with a source and a destination root. */
struct tree {
	char base[64];
	int source, destination;
};

static void put(int root, const char *path, const char *text, mode_t mode)
{
	int fd = openat(root, path, O_WRONLY | O_CREAT | O_TRUNC, mode);

	if (fd < 0 || write(fd, text, strlen(text)) < 0 || fchmod(fd, mode)) {
		fprintf(stderr, "FAIL: cannot write %s: %s\n", path, strerror(errno));
		failures++;
	}
	if (fd >= 0)
		close(fd);
}

static int tree_open(struct tree *tree)
{
	char path[PATH_MAX + 8];

	snprintf(tree->base, sizeof(tree->base), "/tmp/fyai-view-fs-XXXXXX");
	if (!mkdtemp(tree->base))
		return -1;
	snprintf(path, sizeof(path), "%s/src", tree->base);
	if (mkdir(path, 0755))
		return -1;
	tree->source = open(path, O_RDONLY | O_DIRECTORY);
	snprintf(path, sizeof(path), "%s/dst", tree->base);
	if (mkdir(path, 0755))
		return -1;
	tree->destination = open(path, O_RDONLY | O_DIRECTORY);
	return tree->source < 0 || tree->destination < 0 ? -1 : 0;
}

/* Remove everything that a test made, with the functions of the module and no glob. */
static void tree_close(struct tree *tree)
{
	const char *const roots[] = { "src", "dst" };
	char error[PATH_MAX];
	size_t i;

	close(tree->source);
	close(tree->destination);
	for (i = 0; i < 2; i++) {
		const char *const names[1] = { roots[i] };
		int fd;

		fd = open(tree->base, O_RDONLY | O_DIRECTORY);
		if (fd >= 0) {
			fyai_view_fs_remove(fd, names, 1, true, error, sizeof(error));
			close(fd);
		}
	}
	rmdir(tree->base);
}

static void expect_text(int root, const char *path, const char *want)
{
	char buffer[256] = "";
	int fd = openat(root, path, O_RDONLY | O_NOFOLLOW);
	ssize_t n = fd >= 0 ? read(fd, buffer, sizeof(buffer) - 1) : -1;

	if (fd >= 0)
		close(fd);
	if (n < 0 || strcmp(buffer, want)) {
		fprintf(stderr, "FAIL: %s holds '%s', want '%s'\n", path, n < 0 ? "(unreadable)" : buffer, want);
		failures++;
	}
}

int view_fs_paths(void)
{
	char out[PATH_MAX];

	failures = 0;
	CHECK(fyai_view_fs_path("a/b", out, sizeof(out)) && !strcmp(out, "a/b"));
	CHECK(fyai_view_fs_path("./a//b/", out, sizeof(out)) && !strcmp(out, "a/b"));
	CHECK(fyai_view_fs_path("././a", out, sizeof(out)) && !strcmp(out, "a"));
	CHECK(!fyai_view_fs_path("", out, sizeof(out)));
	CHECK(!fyai_view_fs_path(".", out, sizeof(out)));
	CHECK(!fyai_view_fs_path("/abs", out, sizeof(out)));
	CHECK(!fyai_view_fs_path("../x", out, sizeof(out)));
	CHECK(!fyai_view_fs_path("a/../x", out, sizeof(out)));
	CHECK(!fyai_view_fs_path(".git", out, sizeof(out)));
	CHECK(!fyai_view_fs_path("a/.git/config", out, sizeof(out)));
	CHECK(!fyai_view_fs_path(".fyai/arena", out, sizeof(out)));
	CHECK(fyai_view_fs_path(".gitignore", out, sizeof(out)));
	return failures ? 1 : 0;
}

int view_fs_round_trip(void)
{
	struct tree tree;
	struct stream stream = { 0 };
	const char *const paths[] = { "a", "empty" };
	const char *const names[] = { "x", "y/z" };
	char error[PATH_MAX], target[64] = "";
	struct stat st;

	failures = 0;
	if (tree_open(&tree))
		return 1;
	mkdirat(tree.source, "a", 0755);
	mkdirat(tree.source, "a/b", 0755);
	mkdirat(tree.source, "a/.git", 0755);
	mkdirat(tree.source, "empty", 0755);
	put(tree.source, "a/b/c.txt", "content\n", 0644);
	put(tree.source, "a/run.sh", "#!/bin/sh\n", 0755);
	put(tree.source, "a/.git/config", "secret", 0644);
	put(tree.source, "a/empty-file", "", 0600);
	CHECK(!symlinkat("b/c.txt", tree.source, "a/link"));

	/* A walk with no output checks the paths and writes nothing. */
	CHECK(!fyai_view_fs_pack(tree.source, paths, names, 2, NULL, NULL, error, sizeof(error)));
	CHECK(!fyai_view_fs_pack(tree.source, paths, names, 2, stream_emit, &stream, error, sizeof(error)));
	CHECK(stream.length > 0);
	CHECK(!fyai_view_fs_unpack(tree.destination, stream_fill, &stream, error, sizeof(error)));

	/* The stream renames the paths, makes the parents, and keeps the types and the modes. */
	expect_text(tree.destination, "x/b/c.txt", "content\n");
	expect_text(tree.destination, "x/empty-file", "");
	CHECK(!fstatat(tree.destination, "x/run.sh", &st, 0) && (st.st_mode & 0777) == 0755);
	CHECK(!fstatat(tree.destination, "x/empty-file", &st, 0) && (st.st_mode & 0777) == 0600);
	CHECK(readlinkat(tree.destination, "x/link", target, sizeof(target) - 1) == 7 &&
	      !strncmp(target, "b/c.txt", 7));
	CHECK(!fstatat(tree.destination, "y/z", &st, 0) && S_ISDIR(st.st_mode));
	/* The reserved directory is not copied. */
	CHECK(fstatat(tree.destination, "x/.git", &st, 0) && errno == ENOENT);
	/* A file is written beside its place: nothing of that is left. */
	CHECK(fstatat(tree.destination, ".fyai-cp-0", &st, 0));
	free(stream.data);
	tree_close(&tree);
	return failures ? 1 : 0;
}

int view_fs_replace(void)
{
	struct tree tree;
	struct stream stream = { 0 };
	const char *const paths[] = { "d", "f" };
	char error[PATH_MAX];
	struct stat st;

	failures = 0;
	if (tree_open(&tree))
		return 1;
	/* The source has a directory d and a file f; the destination has them the other way. */
	mkdirat(tree.source, "d", 0755);
	put(tree.source, "d/new", "new", 0644);
	put(tree.source, "f", "from the source", 0644);
	put(tree.destination, "d", "was a file", 0644);
	mkdirat(tree.destination, "f", 0755);
	put(tree.destination, "f/old", "old", 0644);
	CHECK(!fyai_view_fs_pack(tree.source, paths, NULL, 2, stream_emit, &stream, error, sizeof(error)));
	CHECK(!fyai_view_fs_unpack(tree.destination, stream_fill, &stream, error, sizeof(error)));
	expect_text(tree.destination, "d/new", "new");
	expect_text(tree.destination, "f", "from the source");
	free(stream.data);

	/* A directory that exists keeps what the stream does not name. */
	memset(&stream, 0, sizeof(stream));
	put(tree.destination, "d/other", "kept", 0644);
	put(tree.source, "d/new", "newer", 0644);
	CHECK(!fyai_view_fs_pack(tree.source, paths, NULL, 1, stream_emit, &stream, error, sizeof(error)));
	CHECK(!fyai_view_fs_unpack(tree.destination, stream_fill, &stream, error, sizeof(error)));
	expect_text(tree.destination, "d/new", "newer");
	expect_text(tree.destination, "d/other", "kept");
	CHECK(!fstatat(tree.destination, "d", &st, 0) && S_ISDIR(st.st_mode));
	free(stream.data);
	tree_close(&tree);
	return failures ? 1 : 0;
}

int view_fs_refusals(void)
{
	struct tree tree;
	struct stream stream = { 0 };
	const char *const missing[] = { "nothing" };
	const char *const escape[] = { "f" };
	const char *const through[] = { "link/f" };
	const char *const hostile[] = { "../evil" };
	char error[PATH_MAX], outside[PATH_MAX];
	struct stat st;
	int rc;

	failures = 0;
	if (tree_open(&tree))
		return 1;
	put(tree.source, "f", "data", 0644);

	/* A path that is absent fails the walk, with the path in the error. */
	rc = fyai_view_fs_pack(tree.source, missing, NULL, 1, NULL, NULL, error, sizeof(error));
	CHECK(rc && errno == ENOENT && !strcmp(error, "nothing"));

	/* A type that the stream does not hold is refused. */
	CHECK(!mkfifoat(tree.source, "fifo", 0600));
	{
		const char *const fifo[] = { "fifo" };

		rc = fyai_view_fs_pack(tree.source, fifo, NULL, 1, NULL, NULL, error, sizeof(error));
		CHECK(rc && errno == ENOTSUP);
	}

	/* A symlink in the path is not followed, and so the root cannot be left. */
	snprintf(outside, sizeof(outside), "%s", tree.base);
	CHECK(!symlinkat(outside, tree.source, "link"));
	rc = fyai_view_fs_pack(tree.source, through, NULL, 1, NULL, NULL, error, sizeof(error));
	CHECK(rc);

	/* A stream is data: a name that leaves the root is refused and nothing is made. */
	CHECK(!fyai_view_fs_pack(tree.source, escape, hostile, 1, stream_emit, &stream, error, sizeof(error)));
	rc = fyai_view_fs_unpack(tree.destination, stream_fill, &stream, error, sizeof(error));
	CHECK(rc && errno == EINVAL);
	CHECK(fstatat(tree.destination, "../evil", &st, 0) && errno == ENOENT);
	free(stream.data);

	/* A stream that stops short is refused. */
	memset(&stream, 0, sizeof(stream));
	CHECK(!fyai_view_fs_pack(tree.source, escape, NULL, 1, stream_emit, &stream, error, sizeof(error)));
	stream.length -= 3;
	rc = fyai_view_fs_unpack(tree.destination, stream_fill, &stream, error, sizeof(error));
	CHECK(rc && errno == EPIPE);
	free(stream.data);
	tree_close(&tree);
	return failures ? 1 : 0;
}

int view_fs_remove(void)
{
	struct tree tree;
	const char *const tree_paths[] = { "t" };
	const char *const both[] = { "gone", "t" };
	char error[PATH_MAX];
	struct stat st;
	int rc;

	failures = 0;
	if (tree_open(&tree))
		return 1;
	mkdirat(tree.source, "t", 0755);
	mkdirat(tree.source, "t/u", 0755);
	put(tree.source, "t/u/f", "x", 0644);
	put(tree.source, "t/g", "x", 0644);
	CHECK(!symlinkat("u", tree.source, "t/l"));
	CHECK(!fyai_view_fs_remove(tree.source, tree_paths, 1, false, error, sizeof(error)));
	CHECK(fstatat(tree.source, "t", &st, AT_SYMLINK_NOFOLLOW) && errno == ENOENT);

	/* An absent path is an error, unless it is forced; the others are still removed. */
	mkdirat(tree.source, "t", 0755);
	rc = fyai_view_fs_remove(tree.source, both, 2, false, error, sizeof(error));
	CHECK(rc && errno == ENOENT && !strcmp(error, "gone"));
	CHECK(!fyai_view_fs_remove(tree.source, both, 2, true, error, sizeof(error)));
	CHECK(fstatat(tree.source, "t", &st, AT_SYMLINK_NOFOLLOW) && errno == ENOENT);
	tree_close(&tree);
	return failures ? 1 : 0;
}
