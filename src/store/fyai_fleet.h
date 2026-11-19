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
