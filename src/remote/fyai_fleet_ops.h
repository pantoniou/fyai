/* SPDX-License-Identifier: MIT */
#ifndef FYAI_FLEET_OPS_H
#define FYAI_FLEET_OPS_H

#include <stdbool.h>

#include <libfyaml.h>
#include <libfyaml/libfyaml-generic.h>

struct fyai_ctx;

/*
 * The fleet of the root of the current directory (doc/fyai-fleet-sdd.md), as
 * the `fleet` commands and the `fleet` tool give it. Each function returns 0
 * with its answer in @gb, or -1 with the cause reported.
 *
 * A reader peeks at the arena of each project. A process maps one arena at a
 * fixed address, so in a process whose arena is open the reader runs the
 * matching `fleet` command in a child with no arena and takes its rows.
 */

/* Each session: {project, branch, state, attached, pid, task, parent}. */
int fyai_fleet_rows_list(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
			 fy_generic *rowsp);

/* Each project: {project, sessions, live}. */
int fyai_fleet_rows_scan(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
			 fy_generic *rowsp);

/* The tasks of the root sessions: {id, session, name, state, text}. */
int fyai_fleet_rows_tasks(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
			  fy_generic *rowsp);

/*
 * The sessions and tasks that hold each word of @query, in any case: in the
 * project, the branch, the description, the text of a task, or the words of
 * the last FYAI_FLEET_SEARCH_TURNS turns. At most @limit rows of {name, kind,
 * where, text}, where @text is the part of the field around the first word.
 */
#define FYAI_FLEET_SEARCH_TURNS	20
#define FYAI_FLEET_SEARCH_MAX	50
int fyai_fleet_rows_search(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
			   const char *query, long long limit, fy_generic *rowsp);

/* The words of the last @turns turns of the session @name, oldest first: {role, text}. */
int fyai_fleet_rows_read(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
			 const char *name, long long turns, fy_generic *rowsp);

/*
 * Start a session in @project (relative to the root) on @branch, or on
 * `fleet/` and the first words of @task, with @task as its first message. In
 * a session, the new one records this one as its parent, and a session of
 * the root keeps a task for it: published now with @publish, else staged for
 * the next publish (a call of a tool runs inside a turn). *@resultp is
 * {project, branch, name, task}.
 */
int fyai_fleet_start(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
		     const char *project, const char *task, const char *branch,
		     bool publish, fy_generic *resultp);

/* Mark the task @id of this root session @state: open, done or dropped. */
int fyai_fleet_task_mark(struct fyai_ctx *ctx, const char *id, const char *state,
			 bool publish);

/*
 * Whether this session is of a root with a project below it: the session
 * that supervises the fleet. Found once for a run.
 */
bool fyai_fleet_is_root(struct fyai_ctx *ctx);

/* The open tasks of this root session, a mapping by id (possibly empty). */
fy_generic fyai_fleet_tasks_open(struct fyai_ctx *ctx);

/* Start the session @name (PROJECT:BRANCH) again on its branch, with no message. */
int fyai_fleet_revive(struct fyai_ctx *ctx, const char *name);

/*
 * The name of the session @name (BRANCH of this project, or PROJECT:BRANCH)
 * from the root: PROJECT:BRANCH, in @gb, or NULL with the cause reported.
 */
const char *fyai_fleet_name_full(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
				 const char *name);

/*
 * Make this process the client of the session @name when it ends: start the
 * session when no process runs it, and join it from the directory of its
 * project, as a watcher when another client drives it. @ret, or NULL, is the
 * session that `/root` returns to (doc/fyai-fleet-sdd.md, section 11).
 * *@watchp says whether it only watches.
 */
int fyai_fleet_attach(struct fyai_ctx *ctx, const char *name, const char *ret,
		      bool *watchp);

/* A request of a supervisor to the live session @name (BRANCH or PROJECT:BRANCH). */
int fyai_fleet_supervise(struct fyai_ctx *ctx, const char *name,
			 const char *subtype, fy_generic fields);

/* End the live session @name; *@busyp: it ran a turn. */
int fyai_fleet_close(struct fyai_ctx *ctx, const char *name, bool force,
		     bool *busyp);

#endif
