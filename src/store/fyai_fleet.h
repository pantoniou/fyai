/* SPDX-License-Identifier: MIT */
#ifndef FYAI_FLEET_H
#define FYAI_FLEET_H

#include <libfyaml.h>
#include <libfyaml/libfyaml-generic.h>

/*
 * The tree of projects (doc/fyai-fleet-sdd.md, section 5.1). A project is a
 * directory whose .fyai holds an arena; the parent of a project is the
 * nearest project above it.
 *
 * The walk up never examines a ceiling: @home, a directory of the
 * colon-separated list @ceilings, or a directory on another file system than
 * the directory below it. @home and @ceilings can be NULL. Each function
 * returns a real path that the caller frees, or NULL when the walk finds no
 * project.
 */

/* The environment variable of the ceiling directories. */
#define FYAI_CEILING_ENV	"FYAI_CEILING_DIRECTORIES"

/* The nearest project at or above @dir. */
char *fyai_fleet_project_at(const char *dir, const char *home,
			    const char *ceilings);

/* The parent of the project @root: the nearest project above it. */
char *fyai_fleet_parent_at(const char *root, const char *home,
			   const char *ceilings);

/* As above, with the ceilings of the environment: $HOME and $FYAI_CEILING_DIRECTORIES. */
char *fyai_fleet_project_of(const char *dir);
char *fyai_fleet_parent(const char *root);

/* The root of the tree that holds @dir: the topmost project above its project. */
char *fyai_fleet_root_of(const char *dir);

/*
 * The lineage of a session of the fleet in @gb: {root, parent: {project,
 * branch}, task}. @dir is the directory of the session, @parent the
 * PROJECT:BRANCH of the session that started it, and @task its task, or 0 for
 * none. fy_invalid on failure.
 */
fy_generic fyai_fleet_lineage(struct fy_generic_builder *gb, const char *dir,
			      const char *parent, long long task);

/*
 * Split the name of a session, `PROJECT:BRANCH` or `BRANCH`. PROJECT is a
 * directory relative to the root of @cwd (`.` is the root) whose .fyai holds
 * an arena; a name whose text before its first colon is no such project is a
 * branch alone (`main/agent:x`). *@dirp is the real path of the project, which
 * the caller frees, or NULL for the project of @cwd; *@branchp points into
 * @name. Returns 0, or -1 when the name has no branch.
 */
int fyai_fleet_name_split(const char *cwd, const char *name, char **dirp,
			  const char **branchp);

/*
 * Call @found with the real path of each project at or below @root, @root
 * first when it is one, in the order of the names. A project below a project
 * is found too. The scan does not go into .fyai, .git, a symbolic link, a
 * directory on another file system, a directory whose name matches a pattern
 * of the sequence @exclude (fnmatch()), or deeper than @depth levels below
 * @root. Returns 0, the first nonzero return of @found, or -1 when @root
 * cannot be read.
 */
int fyai_fleet_scan(const char *root, int depth, fy_generic exclude,
		    int (*found)(const char *dir, void *data), void *data);

#endif
