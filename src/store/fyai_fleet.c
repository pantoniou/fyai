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

/* The ceiling from @entry to @end names @dir, of length @len. */
static bool fleet_entry_is(const char *entry, const char *end, const char *dir,
			   size_t len)
{
	/* A ceiling may end with a slash; the root keeps its own. */
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

/* @dir holds a project: .fyai with an arena, not only a socket or a log. */
static bool fleet_is_project(const char *dir)
{
	char probe[PATH_MAX];
	int n;

	n = snprintf(probe, sizeof(probe), "%s/.fyai/arena",
		     strcmp(dir, "/") ? dir : "");
	return n > 0 && (size_t)n < sizeof(probe) && !access(probe, F_OK);
}

/* The walk up from the real path @cur, which it takes and frees. */
static char *fleet_walk(char *cur, const char *home, const char *ceilings)
{
	struct stat st;
	dev_t dev = 0;
	bool have_dev = false;
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

	return cur ? fleet_walk(cur, home, ceilings) : NULL;
}

char *fyai_fleet_parent_at(const char *root, const char *home,
			   const char *ceilings)
{
	char *cur = realpath(root, NULL), *slash;

	if (!cur)
		return NULL;
	slash = strrchr(cur, '/');
	if (!slash || !strcmp(cur, "/")) {
		free(cur);
		return NULL;
	}
	if (slash == cur)
		slash[1] = '\0';
	else
		*slash = '\0';
	return fleet_walk(cur, home, ceilings);
}

/* $HOME as a real path, or NULL; the caller frees. */
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
