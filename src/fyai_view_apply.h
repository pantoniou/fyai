/* SPDX-License-Identifier: MIT */
#ifndef FYAI_VIEW_APPLY_H
#define FYAI_VIEW_APPLY_H

#include <stdbool.h>
#include <stddef.h>

#include <libfyaml/libfyaml-generic.h>

#include "fyai_manifest.h"

struct fyai_apply_summary {
	size_t applied;
	size_t satisfied;
	size_t conflicts;
	size_t skipped;
};

/*
 * Apply the changes from a baseline to a result onto the host project.
 *
 * Each changed path is reconciled against the host: the host takes the result
 * when it still equals the baseline, a path that already equals the result is
 * satisfied, and a path that both sides changed differently is a conflict and
 * is left alone. Timestamps and ownership are not compared; a written file
 * has the time of the write. A selected path takes the path and what is
 * beneath it; no selection takes every change. Directories and the paths
 * .git and .fyai are never replaced.
 *
 * project_fd names the project root and objects_fd the object store of the
 * result. Every path is walked from project_fd without following a symlink.
 * With dry_run nothing is written. The rows go to the sequence in gb: path,
 * status, action (applied, satisfied, conflict or skipped) and a reason.
 * Return 0, or -1 with errno set when the comparison could not be made.
 */
int fyai_view_apply(struct fy_generic_builder *gb, int project_fd, int objects_fd,
		    const struct fyai_manifest *baseline, const struct fyai_manifest *result,
		    const char *const *paths, size_t path_count, bool dry_run,
		    fy_generic *rows, struct fyai_apply_summary *summary);

/*
 * A test that leaves a path out of the apply: true for a path that is not to be
 * written, as when the project ignores it. The path is that of a change, and is_dir
 * says whether it is a directory on either side. A path that is left out is not
 * reported and does not count as a conflict or as skipped.
 */
typedef bool (*fyai_apply_skip_fn)(void *arg, const char *path, bool is_dir);

/* fyai_view_apply() with a test for the paths that it leaves out; NULL leaves out none. */
int fyai_view_apply_filtered(struct fy_generic_builder *gb, int project_fd, int objects_fd,
			     const struct fyai_manifest *baseline, const struct fyai_manifest *result,
			     const char *const *paths, size_t path_count, bool dry_run,
			     fyai_apply_skip_fn skip, void *skip_arg, fy_generic *rows,
			     struct fyai_apply_summary *summary);

#endif
