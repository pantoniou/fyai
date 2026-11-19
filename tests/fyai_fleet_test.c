/*
 * fyai_fleet_test.c - unit tests for the tree of projects
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

#include <libfyaml.h>
#include <libfyaml/libfyaml-generic.h>

#include "fyai_fleet.h"
#include "fyai_fleet_watch.h"

#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(fleet, walk, fleet_walk)
FYAI_TEST_ENTRY(fleet, panel_blinks, fleet_panel_blinks)

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

/* Make @base/@rel and its parents. */
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

/* @got is @base/@rel, or NULL when @rel is NULL; @got is freed. */
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

/* @base/@rel in @buf, which has PATH_MAX bytes. */
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
	/* The ceiling above the tree keeps the walk in it. */
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

	/* The nearest project owns a directory, at any depth. */
	CHECK(fleet_is(fyai_fleet_project_at(fleet_path(dir, base, "w/a/src/deep"),
					     NULL, ceil), base, "w/a"));
	CHECK(fleet_is(fyai_fleet_project_at(fleet_path(dir, base, "w/a"), NULL, ceil),
		       base, "w/a"));
	CHECK(fleet_is(fyai_fleet_project_at(fleet_path(dir, base, "w/notes"), NULL, ceil),
		       base, "w"));
	/* A .fyai with no arena is not a project. */
	CHECK(fleet_is(fyai_fleet_project_at(fleet_path(dir, base, "w/libs"), NULL, ceil),
		       base, "w"));

	/* The parent is the nearest project above, past a directory with none. */
	CHECK(fleet_is(fyai_fleet_parent_at(fleet_path(dir, base, "w/libs/inner"), NULL,
					    ceil), base, "w"));
	CHECK(fleet_is(fyai_fleet_parent_at(fleet_path(dir, base, "w/a"), NULL, ceil),
		       base, "w"));
	CHECK(fleet_is(fyai_fleet_parent_at(fleet_path(dir, base, "w"), NULL, ceil),
		       base, NULL));

	/* A ceiling is not examined, with or without a trailing slash. */
	CHECK(snprintf(ceil, sizeof(ceil), "%s/w/:%s", base, top) < (int)sizeof(ceil));
	CHECK(fleet_is(fyai_fleet_project_at(fleet_path(dir, base, "w/notes"), NULL, ceil),
		       base, NULL));
	CHECK(fleet_is(fyai_fleet_parent_at(fleet_path(dir, base, "w/a"), NULL, ceil),
		       base, NULL));
	CHECK(fleet_is(fyai_fleet_project_at(fleet_path(dir, base, "w/a/src"), NULL, ceil),
		       base, "w/a"));

	/* The home is a ceiling: its .fyai is the directory of the user. */
	snprintf(ceil, sizeof(ceil), "%s", top);
	fleet_path(home, base, "h");
	CHECK(fleet_is(fyai_fleet_parent_at(fleet_path(dir, base, "h/p"), home, ceil),
		       base, NULL));
	CHECK(fleet_is(fyai_fleet_project_at(fleet_path(dir, base, "h/q"), home, ceil),
		       base, NULL));
	CHECK(fleet_is(fyai_fleet_parent_at(fleet_path(dir, base, "h/p"), NULL, ceil),
		       base, "h"));

	/* A directory that does not exist has no project. */
	CHECK(fleet_is(fyai_fleet_project_at(fleet_path(dir, base, "w/none"), NULL, ceil),
		       base, NULL));

	fleet_remove(base);
	return failures ? 1 : 0;
}

/* The mark of a running member alternates with the phase; the others stay. */
int fleet_panel_blinks(void)
{
	struct fy_generic_builder_cfg gbc = { .flags = FYGBCF_SCOPE_LEADER };
	struct fy_generic_builder *gb = fy_generic_builder_create(&gbc);
	fy_generic rows, idle;
	char *a, *b, *c;
	bool blinks = false;

	failures = 0;
	CHECK(gb);
	rows = fy_sequence(gb, fy_mapping(gb, "name", "a:x", "state", "running"));
	idle = fy_mapping(gb, "name", "b:y", "state", "idle");
	rows = fy_append(gb, rows, idle);

	a = fyai_fleet_panel_source(rows, 0, &blinks);
	CHECK(a && blinks);
	b = fyai_fleet_panel_source(rows, 1, NULL);
	c = fyai_fleet_panel_source(rows, 2, NULL);
	CHECK(b && c && strcmp(a, b) && !strcmp(a, c));
	CHECK(a && strstr(a, "\u25cf a") && strstr(a, "\u00b7 b"));
	CHECK(b && strstr(b, "\u25cb a") && strstr(b, "\u00b7 b"));
	free(a);
	free(b);
	free(c);

	/* A title comes first, the name after it. */
	rows = fy_sequence(gb, fy_mapping(gb, "name", "a:x", "state", "idle",
					  "title", "Fix the build"));
	a = fyai_fleet_panel_source(rows, 0, NULL);
	CHECK(a && strstr(a, "Fix the build (a") && strstr(a, "idle"));
	free(a);

	/* No member runs: the text does not change, nothing blinks. */
	rows = fy_sequence(gb, idle);
	a = fyai_fleet_panel_source(rows, 0, &blinks);
	CHECK(a && !blinks);
	free(a);
	CHECK(!fyai_fleet_panel_source(fy_seq_empty, 0, &blinks) && !blinks);
	fy_generic_builder_destroy(gb);
	return failures ? 1 : 0;
}
