/* SPDX-License-Identifier: MIT */
#ifndef FYAI_FLEET_H
#define FYAI_FLEET_H

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

#endif
