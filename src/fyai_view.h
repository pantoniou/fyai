/* SPDX-License-Identifier: MIT */
#ifndef FYAI_VIEW_H
#define FYAI_VIEW_H

#include <stdbool.h>

#include <libfyaml/libfyaml-generic.h>

struct fyai_ctx;

/* The parameters of a capture that makes or replaces a stored view. */
struct fyai_view_request {
	struct fyai_ctx *ctx;
	const char *name;
	/* NULL when replace is set: the stored project is used. */
	const char *project;
	/* NULL selects the stored or configured policy. */
	const char *durability;
	bool replace;
	bool verify;
	bool copy_git_objects;
	/* Show a progress band. */
	bool progress;
};

/*
 * Capture the project and store the view record under the name. On success the
 * record is built in the builder of the context. Return 0, or -1 with the cause
 * reported.
 */
int fyai_view_capture(const struct fyai_view_request *request, fy_generic *record);

/*
 * The run of an isolated agent in a view. Begin captures the project into the
 * view of the name, which it makes or replaces, and prepares it for the agent
 * projection. The spec is valid until free. Finish captures the result after
 * every process of the run has ended and stores it; summary is a malloc'ed
 * account of the change, or NULL when it cannot be made.
 */
struct fyai_view_run;
struct fyai_fsview;

int fyai_view_run_begin(struct fyai_ctx *ctx, const char *name, struct fyai_view_run **run);
const struct fyai_fsview *fyai_view_run_spec(const struct fyai_view_run *run);
const char *fyai_view_run_name(const struct fyai_view_run *run);
int fyai_view_run_finish(struct fyai_ctx *ctx, struct fyai_view_run *run, char **summary);
void fyai_view_run_free(struct fyai_view_run *run);

#endif
