/* SPDX-License-Identifier: MIT */
#ifndef FYAI_FLEET_OPS_H
#define FYAI_FLEET_OPS_H

#include <stdbool.h>

#include <libfyaml.h>
#include <libfyaml/libfyaml-generic.h>

struct fyai_ctx;

/*
 * Fleet operations rooted at the current directory's fleet root
 * (doc/fyai-fleet-sdd.md), backing both the `fleet` CLI and tool. Functions
 * populate output in @gb and return 0, or -1 on error.
 *
 * Because a process can map only one fixed-address arena at a time, read
 * operations called from a process with an open arena delegate to a child
 * `fleet` subprocess and collect its rows.
 */

/* List sessions as {project, branch, state, attached, pid, task, parent}. */
int fyai_fleet_rows_list(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
			 fy_generic *rowsp);

/* Scan projects as {project, sessions, live}. */
int fyai_fleet_rows_scan(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
			 fy_generic *rowsp);

/* List root-session tasks as {id, session, name, state, text}. */
int fyai_fleet_rows_tasks(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
			  fy_generic *rowsp);

/*
 * Case-insensitive all-words search across project, branch, description, task
 * text, and the last FYAI_FLEET_SEARCH_TURNS turns. Returns up to @limit rows
 * of {name, kind, where, text} with a snippet around the first matched term.
 */
#define FYAI_FLEET_SEARCH_TURNS	20
#define FYAI_FLEET_SEARCH_MAX	50
int fyai_fleet_rows_search(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
			   const char *query, long long limit, fy_generic *rowsp);

/* Read the last @turns turns of session @name oldest-first as {role, text}. */
int fyai_fleet_rows_read(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
			 const char *name, long long turns, fy_generic *rowsp);

/* Default fleet/max_live cap on concurrent members per root session. */
#define FYAI_FLEET_MAX_LIVE	8

/* 0 when this root session may start one more member, else -1 with the cause. */
int fyai_fleet_room(struct fyai_ctx *ctx);

/*
 * Start a session in @project (relative to fleet root) on @branch (default:
 * `fleet/<slug-of-task>`) with initial message @task. Records parent lineage
 * when called from a session, and tracks a task on root sessions (published if
 * @publish, else staged for the in-flight turn). Sets *@resultp to
 * {project, branch, name, task}.
 */
int fyai_fleet_start(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
		     const char *project, const char *task, const char *branch,
		     bool publish, fy_generic *resultp);

/* Set task @id on this root session to "open", "done", or "dropped". */
int fyai_fleet_task_mark(struct fyai_ctx *ctx, const char *id, const char *state,
			 bool publish);

/*
 * Whether this session is of a root with a project below it: the session
 * that supervises the fleet. Found once for a run.
 */
bool fyai_fleet_is_root(struct fyai_ctx *ctx);

/* Open tasks of this root session as an id-keyed mapping (may be empty). */
fy_generic fyai_fleet_tasks_open(struct fyai_ctx *ctx);

/* Restart session @name (PROJECT:BRANCH) on its branch with no new message. */
int fyai_fleet_revive(struct fyai_ctx *ctx, const char *name);

/*
 * Normalize @name (local BRANCH or PROJECT:BRANCH) to root-relative
 * PROJECT:BRANCH in @gb, or return NULL on error.
 */
const char *fyai_fleet_name_full(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
				 const char *name);

/*
 * Exec this process into a client for session @name (starting it if not live),
 * using watch mode when another client is already attached. @ret sets the
 * `/root` return target (doc/fyai-fleet-sdd.md, section 11). Sets *@watchp.
 */
int fyai_fleet_attach(struct fyai_ctx *ctx, const char *name, const char *ret,
		      bool *watchp);

/* Forward a supervisor control request to live session @name. */
int fyai_fleet_supervise(struct fyai_ctx *ctx, const char *name,
			 const char *subtype, fy_generic fields);

/* Terminate live session @name; *@busyp is set if a turn was active. */
int fyai_fleet_close(struct fyai_ctx *ctx, const char *name, bool force,
		     bool *busyp);

#endif
