/* SPDX-License-Identifier: MIT */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fyai_fleet.h"

/* Returns true if ceiling path [@entry, @end) matches @dir (length @len). */
static bool fleet_entry_is(const char *entry, const char *end, const char *dir,
			   size_t len)
{
	/* Strip trailing slashes except for "/". */
	while (end - entry > 1 && end[-1] == '/')
		end--;
	return (size_t)(end - entry) == len && !memcmp(entry, dir, len);
}

static bool fleet_ceiling(const char *dir, const char *home, const char *ceilings)
{
	size_t len = strlen(dir);
	const char *entry, *end;

	if (home && *home && fleet_entry_is(home, home + strlen(home), dir, len))
		return true;
	for (entry = ceilings; entry && *entry; entry = *end ? end + 1 : end) {
		end = strchr(entry, ':');
		if (!end)
			end = entry + strlen(entry);
		if (end > entry && fleet_entry_is(entry, end, dir, len))
			return true;
	}
	return false;
}

/* Returns true if @dir has a .fyai/arena (not just a socket or log). */
static bool fleet_is_project(const char *dir)
{
	char probe[PATH_MAX];
	int n;

	n = snprintf(probe, sizeof(probe), "%s/.fyai/arena",
		     strcmp(dir, "/") ? dir : "");
	return n > 0 && (size_t)n < sizeof(probe) && !access(probe, F_OK);
}

/*
 * Walk parent directories from realpath @cur to find the nearest project.
 * Takes ownership of @cur; returns a heap path or NULL.
 */
static char *fleet_walk(char *cur, const char *home, const char *ceilings,
			const dev_t *start_dev)
{
	struct stat st;
	dev_t dev = start_dev ? *start_dev : 0;
	bool have_dev = start_dev != NULL;
	char *slash;

	for (;;) {
		if (fleet_ceiling(cur, home, ceilings) || stat(cur, &st))
			break;
		if (have_dev && st.st_dev != dev)
			break;
		dev = st.st_dev;
		have_dev = true;
		if (fleet_is_project(cur))
			return cur;
		if (!strcmp(cur, "/"))
			break;
		slash = strrchr(cur, '/');
		if (!slash)
			break;
		if (slash == cur)
			slash[1] = '\0';
		else
			*slash = '\0';
	}
	free(cur);
	return NULL;
}

char *fyai_fleet_project_at(const char *dir, const char *home,
			    const char *ceilings)
{
	char *cur = realpath(dir, NULL);

	return cur ? fleet_walk(cur, home, ceilings, NULL) : NULL;
}

char *fyai_fleet_parent_at(const char *root, const char *home,
			   const char *ceilings)
{
	char *cur = realpath(root, NULL), *slash;
	struct stat st;

	if (!cur)
		return NULL;
	/* Stay on the project's filesystem; do not cross mount boundaries. */
	if (stat(cur, &st)) {
		free(cur);
		return NULL;
	}
	slash = strrchr(cur, '/');
	if (!slash || !strcmp(cur, "/")) {
		free(cur);
		return NULL;
	}
	if (slash == cur)
		slash[1] = '\0';
	else
		*slash = '\0';
	return fleet_walk(cur, home, ceilings, &st.st_dev);
}

/* Returns realpath($HOME), or NULL. Caller must free. */
static char *fleet_home(void)
{
	const char *home = getenv("HOME");

	return home && *home ? realpath(home, NULL) : NULL;
}

char *fyai_fleet_project_of(const char *dir)
{
	char *home = fleet_home(), *project;

	project = fyai_fleet_project_at(dir, home, getenv(FYAI_CEILING_ENV));
	free(home);
	return project;
}

char *fyai_fleet_parent(const char *root)
{
	char *home = fleet_home(), *parent;

	parent = fyai_fleet_parent_at(root, home, getenv(FYAI_CEILING_ENV));
	free(home);
	return parent;
}
