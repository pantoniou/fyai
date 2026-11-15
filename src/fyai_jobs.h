/*
 * fyai_jobs.h - the tool jobs component
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_JOBS_H
#define FYAI_JOBS_H

#include <stdbool.h>

struct fyai_agent_bg_run;
struct fyai_btw_run;
struct fyai_diag;
struct fyai_fenced_stream;
struct fyai_loop;
struct fyai_monitor_run;
struct fyai_patch_display;
struct fyai_shell_session;
struct fyai_tool_job;
struct jsonrpc_conn;

/*
 * The work that the tool calls of a run start: the live tool jobs, the side
 * questions, the background sub-agents, the monitors, the named terminal
 * sessions, the control channel of a tool child, and what a call shows while
 * it runs. The component stands on the loop and reports into the
 * diagnostics. The owner is struct fyai_app.
 */
struct fyai_jobs {
	/* Dependencies. */
	struct fyai_loop *loop;
	struct fyai_diag *diag;

	struct fyai_tool_job *tools;		/* live jobs, for a resize */
	struct fyai_btw_run *btw;		/* side questions in this session */
	struct fyai_agent_bg_run *agent_bg;	/* background sub-agents */
	struct fyai_monitor_run *monitors;	/* running monitors */
	/*
	 * Named terminal sessions, each one a process of its own. The view of
	 * a session lives here and so outlives the process that drove it.
	 */
	struct fyai_shell_session *sessions;
	struct fyai_fenced_stream *stream;	/* live progressive shell output */
	/* Resolved patch display data, indexed by tool-call ID. */
	struct fyai_patch_display *patch_views;
	/* Resolved display data for the active patch call. */
	char *patch_display;
	/* Forked tool control channel. */
	struct jsonrpc_conn *rpc;
	char *submit_error;
	/* Set once a tool row has been drawn live. */
	bool output_displayed;
};

void fyai_jobs_init(struct fyai_jobs *jobs, struct fyai_loop *loop,
		    struct fyai_diag *diag);
/* The live stream and the patch display of the parent are not the child's. */
void fyai_jobs_abandon(struct fyai_jobs *jobs);

#endif
