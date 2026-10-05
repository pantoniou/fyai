/* SPDX-License-Identifier: MIT */
#ifndef FYAI_VIEW_H
#define FYAI_VIEW_H

#include <stdbool.h>
#include <stdint.h>

#include <libfyaml/libfyaml-generic.h>

struct fyai_ctx;

/*
 * The namespace of the views that sub-agents leave. A view in it belongs to
 * the agent that started the sub-agent and is stored in the branch of that
 * agent, under the name of the sub-agent: `agent/NAME`. A user cannot create a
 * view there, and the project_view tool reaches nothing outside it.
 */
#define FYAI_VIEW_AGENT_PREFIX "agent/"

/* Whether a view name is in the namespace of the sub-agents, or is its root. */
static inline bool fyai_view_name_is_agent(const char *name)
{
	return name && (!strncmp(name, FYAI_VIEW_AGENT_PREFIX, sizeof(FYAI_VIEW_AGENT_PREFIX) - 1) ||
			!strcmp(name, "agent"));
}

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
 * The project_view tool reads a sub-agent view through this entry. child is the
 * name that the caller gave the sub-agent, with no prefix and no separator.
 * Only a view in the namespace of the sub-agents of this branch is reached: a
 * user view, another branch and a reference are not. Otherwise as
 * fyai_view_pull().
 */
int fyai_view_pull_agent(struct fyai_ctx *ctx, struct fy_generic_builder *gb, const char *child,
			 const char *const *paths, size_t count, enum fyai_view_pull_mode mode,
			 fy_generic *result);

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
 * The name of the view that the main session runs in (view/isolate_session), or
 * NULL when it runs in none. A sub-agent and a tool child are not the main
 * session. The name is static storage.
 */
const char *fyai_view_session_name(const struct fyai_ctx *ctx);

/*
 * Whether the main session can run in a view here (view/isolate_session): not in
 * a view, with the arena in the project, the scratch directory outside it, and
 * user namespaces that a probe could make. Nothing changes.
 */
bool fyai_view_session_available(struct fyai_ctx *ctx);

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

/*
 * Whether this run shows what each group of tool calls changed (view/tool_diff).
 * It needs a run that owns its conversation and an arena in the project; a forked
 * child, a sub-agent and a pinned root do not. A session that runs in a view
 * (view/isolate_session) does: the storage of the project is out of its reach, so
 * its states are kept in a directory of the arena that the run removes when it ends.
 */
bool fyai_tool_diff_enabled(struct fyai_ctx *ctx);

/* Remove the private storage of the project states of a run in a view, if it made one. */
void fyai_tool_diff_cleanup(struct fyai_ctx *ctx);

/*
 * Keep the project states around a group of tool calls whose diff was shown, for
 * undo. The newest are kept; the older ones are dropped.
 */
void fyai_tool_change_record(struct fyai_ctx *ctx, fy_generic before, fy_generic after);

/*
 * Take the project back from the state after a recorded group of tool calls to the
 * state before it, back groups from the newest (1 is the newest). It is `view apply`
 * the other way: a path that still has what the group left takes what it had
 * before, and a path that changed since is a conflict and is left alone. The
 * group is dropped when it had no conflict. The result holds the counts and the
 * rows, as `view apply` does. Return 0, or -1 with the cause reported.
 */
int fyai_tool_change_undo(struct fyai_ctx *ctx, struct fy_generic_builder *gb, long long back,
			  fy_generic *result);

/*
 * The unified patch between two project states that fyai_project_state_capture()
 * made, as `view diff` writes it, with no metadata rows. The patch is built in gb.
 * *text is an empty string when nothing changed, and is cut with a note at limit
 * bytes. Return 0, or -1 with the cause reported.
 */
int fyai_project_state_diff(struct fyai_ctx *ctx, struct fy_generic_builder *gb, fy_generic before,
			    fy_generic after, size_t limit, fy_generic *text);

/* The project that the arena belongs to, or NULL; the caller frees it. */
char *fyai_view_project_root(struct fyai_ctx *ctx);

/* Remove the runtime tree of a view: the directory and all that it holds. */
int fyai_view_runtime_remove(const char *runtime);

/*
 * Garbage collection of the project storage. A manifest, a blob and a view runtime
 * that no reference of the arena reaches is removed. The references are the project
 * state of the ref-log entries, the stored views of each branch, and the base of a
 * delta manifest. A file that is newer than grace seconds stays, because the
 * capture that made it can record it later. A storage in use by a capture or by gc
 * is skipped. The storage that the capture holds shared is the one that gc takes
 * exclusive.
 */
struct fyai_view_gc_stats {
	size_t manifests;
	size_t objects;
	size_t runtimes;
	/* The bytes that the removed files held alone. */
	uint64_t bytes;
};

int fyai_view_gc(struct fyai_ctx *ctx, unsigned int grace, struct fyai_view_gc_stats *stats);

/* Take the lock of a storage that a capture holds while it writes; close the fd to release. */
int fyai_view_storage_lock_shared(const char *storage);

#endif
