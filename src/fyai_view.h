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

/*
 * Run this invocation again in the view named session when view/isolate_session
 * asks for it. Return 1 when the session ran and ended, with the exit status in
 * the configuration; 0 when it does not apply, and the caller runs the verb;
 * -1 when the cause was reported.
 */
int fyai_view_session_bootstrap(struct fyai_ctx *ctx);

enum fyai_view_pull_mode {
	/* List the changes of the result. */
	FYAI_VIEW_PULL_CHANGES,
	/* Report what an apply would do. */
	FYAI_VIEW_PULL_DRY_RUN,
	/* Write the result to the project. */
	FYAI_VIEW_PULL_APPLY,
};

/*
 * Read the result of a view back into the project: the one entry of the
 * `view diff --stat`, `view apply --dry-run` and `view apply` commands and of
 * the project_view tool. The result is built in gb. name is a view or a
 * reference, and base the state that the change starts from: a view's baseline,
 * or the head, when it is NULL. paths selects what to apply; none selects every
 * change. Return 0, or -1 with the cause reported.
 */
int fyai_view_pull(struct fyai_ctx *ctx, struct fy_generic_builder *gb, const char *name,
		   const char *base, const char *const *paths, size_t count,
		   enum fyai_view_pull_mode mode, fy_generic *result);

/* The stored views of the branch, by name; the value is borrowed. */
fy_generic fyai_view_list(struct fyai_ctx *ctx, struct fy_generic_builder *gb);

/*
 * The project state of a ref-log entry. When view/track_project is on, each
 * entry that moves the head of a branch records a reference to a manifest of
 * the project, so a reference to a point of the branch is a reference to the
 * files at that point. Only a run whose arena is in the .fyai directory of the
 * project records one. Capture builds the reference with its storage path in
 * ctx->gb; it returns 0, with *ref invalid when there is nothing to capture,
 * and -1 after a warning when the capture failed.
 */
bool fyai_project_state_enabled(struct fyai_ctx *ctx);
int fyai_project_state_capture(struct fyai_ctx *ctx, fy_generic *ref);

/*
 * Whether this run can give a sub-agent a view of the project: Linux, a writable
 * durable arena in the .fyai directory of the project, no credential isolation,
 * and a run that is not itself in a view.
 */
bool fyai_view_isolation_available(struct fyai_ctx *ctx);

/*
 * Restore the project state that a reference recorded, for reset. The project
 * takes the state by the rule of view apply, with the state recorded at the head
 * as the base: a path that changed since is a conflict, nothing is written and the
 * call fails. With force the base is the project as it is now, which is recorded
 * as an entry first. Without track_project, or without a state at the reference,
 * the call does nothing. On success the next entry records the restored state,
 * and report holds a line for the caller.
 */
int fyai_project_state_restore(struct fyai_ctx *ctx, const char *spec, bool force,
			       struct fy_generic_builder *gb, fy_generic *report);

#endif
