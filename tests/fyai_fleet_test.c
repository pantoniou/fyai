/*
 * fyai_fleet_test.c - unit tests for fleet project tree discovery and panel UI
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fyai_fleet.h"

#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(fleet, walk, fleet_walk)

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

/* Create @base/@rel and any missing parent directories. */
static int fleet_mkdirs(const char *base, const char *rel)
{
	char path[PATH_MAX], *p;

	if (snprintf(path, sizeof(path), "%s/%s", base, rel) >= (int)sizeof(path))
		return -1;
	for (p = path + strlen(base) + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(path, 0700) && access(path, F_OK))
			return -1;
		*p = '/';
	}
	return mkdir(path, 0700) && access(path, F_OK) ? -1 : 0;
}

/* Verify @got matches @base/@rel (or NULL when @rel is NULL) and free @got. */
static bool fleet_is(char *got, const char *base, const char *rel)
{
	char want[PATH_MAX];
	bool ok;
	int n;

	if (!rel) {
		ok = !got;
	} else {
		n = snprintf(want, sizeof(want), "%s%s%s", base, *rel ? "/" : "", rel);
		ok = n > 0 && (size_t)n < sizeof(want) && got && !strcmp(got, want);
	}
	if (!ok)
		fprintf(stderr, "fleet: got '%s', want '%s'\n", got ? got : "(none)",
			rel ? rel : "(none)");
	free(got);
	return ok;
}

/* Format @base/@rel into @buf (size PATH_MAX). */
static const char *fleet_path(char *buf, const char *base, const char *rel)
{
	size_t blen = strlen(base), rlen = strlen(rel);

	if (blen + 1 + rlen >= PATH_MAX)
		return "/nonexistent";
	memcpy(buf, base, blen);
	buf[blen] = '/';
	memcpy(buf + blen + 1, rel, rlen + 1);
	return buf;
}

static void fleet_remove(const char *base)
{
	char cmd[PATH_MAX + 16];

	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", base);
	if (system(cmd))
		fprintf(stderr, "fleet: cannot remove '%s'\n", base);
}

int fleet_walk(void)
{
	char tmpl[] = "/tmp/fyai-fleet-test-XXXXXX";
	char base[PATH_MAX], top[PATH_MAX], ceil[PATH_MAX * 2], home[PATH_MAX];
	char dir[PATH_MAX];
	const char *made;

	failures = 0;
	made = mkdtemp(tmpl);
	CHECK(made);
	if (!made || !realpath(made, base))
		return 1;
	/* Set ceiling above temp tree so upward discovery stays inside it. */
	snprintf(top, sizeof(top), "%s", base);
	*strrchr(top, '/') = '\0';
	if (!*top)
		strcpy(top, "/");

	CHECK(!fleet_mkdirs(base, "w/.fyai/arena"));
	CHECK(!fleet_mkdirs(base, "w/a/.fyai/arena"));
	CHECK(!fleet_mkdirs(base, "w/a/src/deep"));
	CHECK(!fleet_mkdirs(base, "w/libs/.fyai/unix"));	/* no arena */
	CHECK(!fleet_mkdirs(base, "w/libs/inner/.fyai/arena"));
	CHECK(!fleet_mkdirs(base, "w/notes"));
	CHECK(!fleet_mkdirs(base, "h/.fyai/arena"));
	CHECK(!fleet_mkdirs(base, "h/p/.fyai/arena"));
	CHECK(!fleet_mkdirs(base, "h/q"));

	snprintf(ceil, sizeof(ceil), "/nonexistent:%s/", top);

	/* Nearest enclosing project owns subdirectories at any depth. */
	CHECK(fleet_is(fyai_fleet_project_at(fleet_path(dir, base, "w/a/src/deep"),
					     NULL, ceil), base, "w/a"));
	CHECK(fleet_is(fyai_fleet_project_at(fleet_path(dir, base, "w/a"), NULL, ceil),
		       base, "w/a"));
	CHECK(fleet_is(fyai_fleet_project_at(fleet_path(dir, base, "w/notes"), NULL, ceil),
		       base, "w"));
	/* Ignore .fyai directories lacking an arena. */
	CHECK(fleet_is(fyai_fleet_project_at(fleet_path(dir, base, "w/libs"), NULL, ceil),
		       base, "w"));

	/* Parent discovery skips non-project intermediate directories. */
	CHECK(fleet_is(fyai_fleet_parent_at(fleet_path(dir, base, "w/libs/inner"), NULL,
					    ceil), base, "w"));
	CHECK(fleet_is(fyai_fleet_parent_at(fleet_path(dir, base, "w/a"), NULL, ceil),
		       base, "w"));
	CHECK(fleet_is(fyai_fleet_parent_at(fleet_path(dir, base, "w"), NULL, ceil),
		       base, NULL));

	/* Ceiling directories stop traversal, with or without trailing slash. */
	CHECK(snprintf(ceil, sizeof(ceil), "%s/w/:%s", base, top) < (int)sizeof(ceil));
	CHECK(fleet_is(fyai_fleet_project_at(fleet_path(dir, base, "w/notes"), NULL, ceil),
		       base, NULL));
	CHECK(fleet_is(fyai_fleet_parent_at(fleet_path(dir, base, "w/a"), NULL, ceil),
		       base, NULL));
	CHECK(fleet_is(fyai_fleet_project_at(fleet_path(dir, base, "w/a/src"), NULL, ceil),
		       base, "w/a"));

	/* $HOME acts as a ceiling so ~/.fyai is not treated as a project. */
	snprintf(ceil, sizeof(ceil), "%s", top);
	fleet_path(home, base, "h");
	CHECK(fleet_is(fyai_fleet_parent_at(fleet_path(dir, base, "h/p"), home, ceil),
		       base, NULL));
	CHECK(fleet_is(fyai_fleet_project_at(fleet_path(dir, base, "h/q"), home, ceil),
		       base, NULL));
	CHECK(fleet_is(fyai_fleet_parent_at(fleet_path(dir, base, "h/p"), NULL, ceil),
		       base, "h"));

	/* Non-existent paths resolve to no project. */
	CHECK(fleet_is(fyai_fleet_project_at(fleet_path(dir, base, "w/none"), NULL, ceil),
		       base, NULL));

	fleet_remove(base);
	return failures ? 1 : 0;
}
