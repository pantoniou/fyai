/* SPDX-License-Identifier: MIT */
#ifndef FYAI_FLEET_H
#define FYAI_FLEET_H

#include <libfyaml.h>
#include <libfyaml/libfyaml-generic.h>

/*
 * Project tree discovery and fleet lineage (doc/fyai-fleet-sdd.md, s5.1).
 * A project is a directory containing .fyai/arena; its parent is the nearest
 * enclosing project.
 *
 * Parent walks stop before inspecting ceilings: @home, any colon-separated
 * path in @ceilings, or a filesystem boundary (different st_dev). @home and
 * @ceilings may be NULL. Returned paths are heap-allocated realpaths that the
 * caller must free, or NULL if no project is found.
 */

/* Environment variable listing colon-separated ceiling directories. */
#define FYAI_CEILING_ENV	"FYAI_CEILING_DIRECTORIES"

/* Returns the nearest project at or above @dir, or NULL. */
char *fyai_fleet_project_at(const char *dir, const char *home,
			    const char *ceilings);

/* Returns the nearest project strictly above @root, or NULL. */
char *fyai_fleet_parent_at(const char *root, const char *home,
			   const char *ceilings);

/* Variants using $HOME and $FYAI_CEILING_DIRECTORIES as ceilings. */
char *fyai_fleet_project_of(const char *dir);
char *fyai_fleet_parent(const char *root);

/* Returns the topmost project enclosing @dir, or NULL. */
char *fyai_fleet_root_of(const char *dir);

/*
 * Build fleet session lineage in @gb: {root, parent: {project, branch}, task}.
 * @dir is the session directory, @parent is the spawning session's
 * PROJECT:BRANCH, and @task is the task id (omitted when <= 0).
 * Returns fy_invalid on failure.
 */
fy_generic fyai_fleet_lineage(struct fy_generic_builder *gb, const char *dir,
			      const char *parent, long long task);

/*
 * Split a session spec `PROJECT:BRANCH` or `BRANCH`. PROJECT is a path
 * relative to the fleet root of @cwd (`.` for the root) containing
 * .fyai/arena; if the prefix before the first colon is not a project, the
 * entire string is treated as a branch name (e.g. `main/agent:x`).
 * Sets *@dirp to the heap-allocated project realpath (caller frees) or NULL
 * for @cwd's project, and *@branchp into @name.
 * Returns 0 on success, or -1 if the branch portion is empty.
 */
int fyai_fleet_name_split(const char *cwd, const char *name, char **dirp,
			  const char **branchp);

/*
 * Recursively invoke @found with the realpath of each project at or under
 * @root (@root first if it is a project) in lexicographical order, including
 * nested projects. Skips .fyai, .git, symlinks, mount crossings, names
 * matching @exclude (fnmatch), and entries beyond @depth levels below @root.
 * Returns 0, the first nonzero return value from @found, or -1 if @root
 * cannot be read.
 */
int fyai_fleet_scan(const char *root, int depth, fy_generic exclude,
		    int (*found)(const char *dir, void *data), void *data);

#endif
