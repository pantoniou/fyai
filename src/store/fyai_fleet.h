/* SPDX-License-Identifier: MIT */
#ifndef FYAI_FLEET_H
#define FYAI_FLEET_H

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

#endif
