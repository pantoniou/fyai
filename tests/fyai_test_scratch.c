/*
 * fyai_test_scratch.c - temporary files and directories that a test leaves
 *                       behind nowhere
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#include <ftw.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fyai_test_scratch.h"

/*
 * The recorded paths. A test binary makes a few of them, so a plain grown
 * array is enough. The exit handler owns this storage and each string.
 */
static char **scratch_paths;
static size_t scratch_count;
static bool scratch_armed;

static int scratch_remove_one(const char *path, const struct stat *sb,
			      int typeflag, struct FTW *ftwbuf)
{
	(void)sb;
	(void)ftwbuf;
	if (typeflag == FTW_DP)
		(void)rmdir(path);
	else
		(void)unlink(path);
	return 0;
}

void fyai_test_scratch_cleanup(void)
{
	size_t i;

	for (i = 0; i < scratch_count; i++) {
		/* A file goes with one unlink; a tree needs the walk. */
		if (unlink(scratch_paths[i]))
			(void)nftw(scratch_paths[i], scratch_remove_one, 16,
				   FTW_DEPTH | FTW_PHYS);
		free(scratch_paths[i]);
	}
	free(scratch_paths);
	scratch_paths = NULL;
	scratch_count = 0;
}

void fyai_test_scratch_forget(void)
{
	size_t i;

	for (i = 0; i < scratch_count; i++)
		free(scratch_paths[i]);
	free(scratch_paths);
	scratch_paths = NULL;
	scratch_count = 0;
}

/* Record @path, which this module then owns. */
static const char *scratch_keep(char *path)
{
	char **grown;

	grown = realloc(scratch_paths, (scratch_count + 1) * sizeof(*grown));
	if (!grown) {
		/* Nothing can hold the path, so do not make it. */
		(void)unlink(path);
		(void)rmdir(path);
		free(path);
		return NULL;
	}
	scratch_paths = grown;
	scratch_paths[scratch_count++] = path;
	if (!scratch_armed) {
		atexit(fyai_test_scratch_cleanup);
		scratch_armed = true;
	}
	return path;
}

int fyai_test_scratch_add(const char *path)
{
	char *copy;

	copy = strdup(path);
	if (!copy)
		return -1;
	return scratch_keep(copy) ? 0 : -1;
}

/* Build "$TMPDIR/fyai-<tag>-XXXXXX<suffix>". */
static char *scratch_template(const char *tag, const char *suffix)
{
	const char *tmpdir;
	char *tmpl;
	int rc;

	tmpdir = getenv("TMPDIR");
	if (!tmpdir || !*tmpdir)
		tmpdir = "/tmp";
	rc = asprintf(&tmpl, "%s/fyai-%s-XXXXXX%s", tmpdir, tag,
		      suffix ? suffix : "");
	return rc < 0 ? NULL : tmpl;
}

const char *fyai_test_scratch_dir(const char *tag)
{
	char *tmpl;

	tmpl = scratch_template(tag, NULL);
	if (!tmpl)
		return NULL;
	if (!mkdtemp(tmpl)) {
		free(tmpl);
		return NULL;
	}
	return scratch_keep(tmpl);
}

const char *fyai_test_scratch_file(const char *tag, const char *suffix)
{
	char *tmpl;
	int fd;

	tmpl = scratch_template(tag, suffix);
	if (!tmpl)
		return NULL;
	fd = mkstemps(tmpl, suffix ? (int)strlen(suffix) : 0);
	if (fd < 0) {
		free(tmpl);
		return NULL;
	}
	close(fd);
	return scratch_keep(tmpl);
}

int fyai_test_scratch_root(void)
{
	const char *root;

	root = fyai_test_scratch_dir("unit");
	if (!root)
		return -1;
	return setenv("TMPDIR", root, 1);
}
