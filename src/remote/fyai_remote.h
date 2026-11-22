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
/* True if the worker was handed off to a server and detached from client. */
bool fyai_remote_ui_released(const struct fyai_remote_ui *view);
/* Take ownership of the handed-off branch name (or NULL). Caller frees. */
char *fyai_remote_ui_take_left(struct fyai_remote_ui *view, bool *parkedp);
/* Open the resume picker when the display opens; Escape ends the client. */
void fyai_remote_ui_resume(struct fyai_remote_ui *view, bool all);
/* Keep a frame of the worker for the next step. */
void fyai_remote_ui_receive(struct fyai_remote_ui *view, fy_generic frame);
/* Serve the frames and the typed input; 1 to detach, -1 on error. */
int fyai_remote_ui_step(struct fyai_remote_ui *view);
void fyai_remote_ui_close(struct fyai_remote_ui *view);

/*
 * Live sessions from the current project server's record as {branch, busy,
 * attached} in @gb. Returns an empty sequence if no server is running.
 */
fy_generic fyai_remote_live_sessions(struct fyai_ctx *ctx,
				     struct fy_generic_builder *gb);
/* Same as fyai_remote_live_sessions() for the project at @root. */
fy_generic fyai_remote_project_sessions(const char *root,
					struct fy_generic_builder *gb);
/* Map a live session record entry to "waiting", "running", or "idle". */
const char *fyai_remote_live_state(fy_generic session);
bool fyai_remote_session_live(struct fyai_ctx *ctx, const char *branch);
/*
 * Hand off the local worker session on @branch to the project server.
 * On success, *@fdp receives the new server channel fd and *@imagep indicates
 * image support. @busy reports an active turn; @keep retains the session after
 * turns complete.
 */
int fyai_remote_adopt(struct fyai_ctx *ctx, const char *branch, bool busy,
		      bool keep, int *fdp, bool *imagep);
/*
 * Register a client-driven local worker session on @branch with the project
 * server or nearest ancestor server (doc/fyai-fleet-sdd.md, section 6.2).
 * Watchers attach through *@fdp; *@imagep indicates image support.
 * Returns 0 on registration, 1 if no server is running, or -1 on error.
 */
int fyai_remote_register(struct fyai_ctx *ctx, const char *branch, bool busy,
			 int *fdp, bool *imagep);

/* Request ID prefix assigned by the server to watcher-originated requests. */
#define FYAI_REMOTE_WATCH_PREFIX	"fyai-watch-"

/*
 * Terminate the live session on @branch in project @dir (NULL for cwd).
 * *@busyp is set to true if a turn was in flight.
 */
int fyai_remote_session_close(struct fyai_ctx *ctx, const char *dir,
			      const char *branch, bool force, bool *busyp);
/*
 * Send a supervisor request @subtype (fyai_session_send, _interrupt, or
 * _answer) with @fields to the live session on @branch in @dir (NULL for cwd)
 * via its server (doc/fyai-fleet-sdd.md, section 7). Returns 0, or -1 on error.
 */
int fyai_remote_supervise(struct fyai_ctx *ctx, const char *dir,
			  const char *branch, const char *subtype,
			  fy_generic fields);
/*
 * Open a persistent supervisor connection to the live session on @branch in
 * @dir (doc/fyai-fleet-sdd.md, section 7). *@fdp receives fyai session_state
 * frames on each transition, *@imagep indicates image support, and *@statep
 * receives the initial static state string. Returns 0, or -1 on error.
 */
int fyai_remote_supervisor_open(struct fyai_ctx *ctx, const char *dir,
				const char *branch, int *fdp, bool *imagep,
				const char **statep);
/*
 * Start a persistent session on @branch in @dir via its server with initial
 * message @task and global CLI options @args (string sequence or fy_invalid).
 * If @task is NULL, starts the worker without sending a message to resume
 * pending work from a dead process. Returns 0, or -1 on error.
 */
int fyai_remote_start(struct fyai_ctx *ctx, const char *dir, const char *branch,
		      const char *task, fy_generic args);
/*
 * Exec into a new session, `resume BRANCH`, or `fleet attach` for `jump:` and
 * `root:` targets (doc/fyai-fleet-sdd.md, section 11).
 */
int fyai_remote_next_exec(const char *argv0, const char *branch);

/* Env var holding the return session target for `/root`. */
#define FYAI_FLEET_RETURN_ENV	"FYAI_FLEET_RETURN"

/*
 * Exec into a client attached to live session @branch (in @dir if non-NULL),
 * in watch mode when @watch is true. If @ret is non-NULL, stores it in
 * $FYAI_FLEET_RETURN for `/root`.
 */
int fyai_remote_attach_exec(const char *argv0, const char *dir,
			    const char *branch, bool watch, const char *ret);

/*
 * Present a frame of the sink of a remote command (src/front/fyai_sink_frames.c)
 * with the sink of @ctx. -1 with a diagnostic for a frame it cannot present.
 */
int fyai_sink_frames_present(struct fyai_ctx *ctx, fy_generic frame);

#endif
