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

#include <libfyaml.h>
#include <libfyaml/libfyaml-generic.h>

#include "fyai_fleet.h"
#include "fyai_fleet_ops.h"
#include "fyai_fleet_watch.h"

#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(fleet, walk, fleet_walk)
FYAI_TEST_ENTRY(fleet, panel_blinks, fleet_panel_blinks)
FYAI_TEST_ENTRY(fleet, rows_in_root, fleet_rows_in_root)

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

/* Running member indicators blink across phases; idle ones stay static. */
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

	a = fyai_fleet_panel_source(rows, "\u25cf", &blinks);
	CHECK(a && blinks);
	b = fyai_fleet_panel_source(rows, "\u25cb", NULL);
	c = fyai_fleet_panel_source(rows, "\u25cf", NULL);
	CHECK(b && c && strcmp(a, b) && !strcmp(a, c));
	CHECK(a && strstr(a, "\u25cf a") && strstr(a, "\u00b7 b"));
	CHECK(b && strstr(b, "\u25cb a") && strstr(b, "\u00b7 b"));
	free(a);
	free(b);
	free(c);

	/* Member title renders before the parenthesized session name. */
	rows = fy_sequence(gb, fy_mapping(gb, "name", "a:x", "state", "idle",
					  "title", "Fix the build"));
	a = fyai_fleet_panel_source(rows, "\u25cf", NULL);
	CHECK(a && strstr(a, "Fix the build (a") && strstr(a, "idle"));
	free(a);

	/* With no running members, blinking is disabled. */
	rows = fy_sequence(gb, idle);
	a = fyai_fleet_panel_source(rows, "\u25cf", &blinks);
	CHECK(a && !blinks);
	free(a);
	/* Empty sequence renders "no members"; invalid value hides the panel. */
	a = fyai_fleet_panel_source(fy_seq_empty, "\u25cf", &blinks);
	CHECK(a && strstr(a, "no members") && !blinks);
	free(a);
	CHECK(!fyai_fleet_panel_source(fy_invalid, "\u25cf", &blinks) && !blinks);
	fy_generic_builder_destroy(gb);
	return failures ? 1 : 0;
}

/* Verify fyai_fleet_rows_in filters by project prefix, with "." matching all. */
int fleet_rows_in_root(void)
{
	struct fy_generic_builder_cfg gbc = { .flags = FYGBCF_SCOPE_LEADER };
	struct fy_generic_builder *gb = fy_generic_builder_create(&gbc);
	fy_generic rows;

	failures = 0;
	CHECK(gb);
	rows = fy_sequence(gb, fy_mapping(gb, "project", "."),
			   fy_mapping(gb, "project", "a"),
			   fy_mapping(gb, "project", "a/b"),
			   fy_mapping(gb, "project", "ab"));
	CHECK(fy_len(fyai_fleet_rows_in(gb, rows, ".")) == 4);
	CHECK(fy_len(fyai_fleet_rows_in(gb, rows, "./")) == 4);
	CHECK(fy_len(fyai_fleet_rows_in(gb, rows, "a")) == 2);
	CHECK(fy_len(fyai_fleet_rows_in(gb, rows, "a/b")) == 1);
	CHECK(fy_len(fyai_fleet_rows_in(gb, rows, NULL)) == 4);
	fy_generic_builder_destroy(gb);
	return failures ? 1 : 0;
}
