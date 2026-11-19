/* SPDX-License-Identifier: MIT */
#ifndef FYAI_REMOTE_H
#define FYAI_REMOTE_H

#include <stdbool.h>
#include <libfyaml.h>
#include <libfyaml/libfyaml-generic.h>

struct fyai_ctx;
struct jsonrpc_conn;
struct fyai_remote_ui;

/*
 * The version of the fyai protocol of `fyai serve` and its clients. A client
 * and a server that both speak it run a fyai session; a change of a request,
 * an answer or a frame of a fyai session changes it
 * (doc/remote-sessions-sdd.md, section 6.1.3).
 */
#define FYAI_REMOTE_PROTOCOL	1

/*
 * The full-screen client of a remote session. It makes the UI model of @ctx a
 * mirror of the model of the worker, opens the display, and asks the worker
 * for the model. The view borrows the context and the connection until it is
 * closed. NULL with a diagnostic.
 */
struct fyai_remote_ui *fyai_remote_ui_open(struct fyai_ctx *ctx,
					   struct jsonrpc_conn *conn);
/*
 * A watching view sends the worker no input: not a line, a key, an answer or
 * an interrupt. It asks for the model of the UI and for stored turns only.
 */
void fyai_remote_ui_watch(struct fyai_remote_ui *view, bool watch);
/* True when the worker left for a server: it lives on, with no client. */
bool fyai_remote_ui_released(const struct fyai_remote_ui *view);
/* The branch of the session that went to a server, or NULL; the caller frees it. */
char *fyai_remote_ui_take_left(struct fyai_remote_ui *view, bool *parkedp);
/* Open the resume picker when the display opens; Escape ends the client. */
void fyai_remote_ui_resume(struct fyai_remote_ui *view, bool all);
/* Keep a frame of the worker for the next step. */
void fyai_remote_ui_receive(struct fyai_remote_ui *view, fy_generic frame);
/* Serve the frames and the typed input; 1 to detach, -1 on error. */
int fyai_remote_ui_step(struct fyai_remote_ui *view);
void fyai_remote_ui_close(struct fyai_remote_ui *view);

/*
 * The live sessions of the server of this project, read from its record:
 * {branch, busy, attached} in @gb. Empty when no server runs.
 */
fy_generic fyai_remote_live_sessions(struct fyai_ctx *ctx,
				     struct fy_generic_builder *gb);
/* As fyai_remote_live_sessions(), for the project at @root. */
fy_generic fyai_remote_project_sessions(const char *root,
					struct fy_generic_builder *gb);
/* What a live session of the record is doing: "waiting", "running" or "idle". */
const char *fyai_remote_live_state(fy_generic session);
bool fyai_remote_session_live(struct fyai_ctx *ctx, const char *branch);
/*
 * Give the session on @branch, which this process runs, to the server of the
 * project. *@fdp is the connection that is the channel of the session from
 * now on, and *@imagep says whether it speaks images. @busy: a turn is in
 * flight. @keep: the server keeps the session after its turns.
 */
int fyai_remote_adopt(struct fyai_ctx *ctx, const char *branch, bool busy,
		      bool keep, int *fdp, bool *imagep);
/*
 * Register the session on @branch, which this process runs and its own client
 * drives, with the server of its project, or of the nearest project above
 * that a server serves (doc/fyai-fleet-sdd.md, section 6.2). The server lists
 * the session, and its watchers read it through *@fdp; *@imagep says whether
 * the connection speaks images. Return 1 when no server runs, 0 when the
 * session is registered, or -1 with a diagnostic.
 */
int fyai_remote_register(struct fyai_ctx *ctx, const char *branch, bool busy,
			 int *fdp, bool *imagep);

/* The prefix of the ID of a request of a watcher, as the server gives it. */
#define FYAI_REMOTE_WATCH_PREFIX	"fyai-watch-"

/*
 * End the live session on @branch of the project of @dir (NULL: of this
 * directory); *@busyp: it ran a turn.
 */
int fyai_remote_session_close(struct fyai_ctx *ctx, const char *dir,
			      const char *branch, bool force, bool *busyp);
/*
 * Send the supervisor request @subtype (fyai_session_send, _interrupt or
 * _answer) with the members @fields to the live session on @branch of the
 * project of @dir (NULL: of this directory), through the server that runs
 * its sessions (doc/fyai-fleet-sdd.md, section 7). 0, or -1 with the cause
 * reported: the server or the worker says why it refuses.
 */
int fyai_remote_supervise(struct fyai_ctx *ctx, const char *dir,
			  const char *branch, const char *subtype,
			  fy_generic fields);
/*
 * Start a session on @branch of the project of @dir at the server that runs
 * its sessions, with @task as its first message and the global options @args
 * (a sequence of strings, or fy_invalid). The server keeps the session after
 * its turn. 0, or -1 with the cause reported.
 */
int fyai_remote_start(struct fyai_ctx *ctx, const char *dir, const char *branch,
		      const char *task, fy_generic args);
/* Replace this process with a new session, or with `resume BRANCH`. */
int fyai_remote_next_exec(const char *argv0, const char *branch);
/* Replace this process with the client of the live session @branch. */
int fyai_remote_attach_exec(const char *argv0, const char *branch);

/*
 * Present a frame of the sink of a remote command (src/front/fyai_sink_frames.c)
 * with the sink of @ctx. -1 with a diagnostic for a frame it cannot present.
 */
int fyai_sink_frames_present(struct fyai_ctx *ctx, fy_generic frame);

#endif
