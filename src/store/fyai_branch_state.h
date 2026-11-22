/*
 * fyai_branch_state.h - the branch cursor component
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_BRANCH_STATE_H
#define FYAI_BRANCH_STATE_H

#include <stdbool.h>

#include <libfyaml.h>
#include <libfyaml/libfyaml-generic.h>

struct fyai_arena;
struct fyai_diag;

/*
 * Where the invocation stands in the branch table: the active branch, stored
 * HEAD, the configuration, catalogue and skills of the branch, the branch
 * table, the record of the next publish, and the project states with the
 * tool changes. The values are generics that the arena holds. The component
 * reads the arena and reports into the diagnostics. The owner is struct
 * fyai_app; a view context keeps a copy of its own.
 */
struct fyai_branch_state {
	/* Dependencies. */
	struct fyai_arena *arena;
	struct fyai_diag *diag;

	fy_generic config;		/* the active branch's config, or fy_invalid */
	fy_generic catalog;		/* catalogue of the branch, or fy_invalid */
	/* Skills of the branch, a mapping by name, or fy_invalid. */
	fy_generic skills;
	fy_generic branches;		/* the branch table of the root */
	/* Active branch state and its next ref-log predecessor. */
	const char *name;
	/* Stored HEAD, which can differ from the active branch. */
	const char *head_branch;
	/*
	 * The fresh session that no publish stored yet, or NULL. While its
	 * conversation holds no exchange, a publish on it stores nothing and
	 * its configuration stays in this context.
	 */
	const char *session_unstored;
	/* Durable branch for a sub-agent conversation. */
	const char *agent_branch;
	/* The arena was reopened by fyai_agent_reopen_for_call(). */
	bool reopened;
	fy_generic prev;
	fy_generic desc;		/* free-text purpose of this branch */
	/*
	 * Label for the next publish on this branch, and the old name when
	 * that label is a rename. Both are consumed and cleared by the
	 * publish, so a label cannot leak into a later, unrelated entry.
	 */
	const char *op;
	const char *op_from;
	fy_generic agent;		/* sub-agent provenance for this branch */
	/*
	 * Store that the next publish starts from in place of the store of
	 * prev, or fy_invalid. The publish consumes it.
	 */
	fy_generic store;
	/* The project state that the next entry records, or fy_invalid. */
	fy_generic project_state;
	/*
	 * Staged pending-work sequence for the next publish (fy_null to clear,
	 * fy_invalid to keep stored). Consumed on publish.
	 */
	fy_generic pending;
	/*
	 * Staged fleet lineage and root-session tasks for the next publish
	 * (fy_invalid to keep stored). Consumed on publish.
	 */
	fy_generic fleet;
	fy_generic tasks;
	/*
	 * The groups of tool calls of this session that changed the project and
	 * showed it: a sequence of {before, after} project states, the newest
	 * last. `undo` takes it back.
	 */
	fy_generic tool_changes;
	/* The private storage of the project states of a run in a view exists. */
	bool tool_diff_storage;
	/*
	 * The turn in flight shows what each group of tool calls changed, so a
	 * patch does not display its own diff. Set from the start of the turn
	 * until it ends, and cleared when the project states can no longer be
	 * taken.
	 */
	bool tool_diff_tracking;
};

void fyai_branch_state_init(struct fyai_branch_state *bs,
			    struct fyai_arena *arena, struct fyai_diag *diag);
/*
 * The record of the next publish stays: a sub-agent sets it from the branch
 * table, and the other children publish nothing.
 */
void fyai_branch_state_abandon(struct fyai_branch_state *bs);

#endif
