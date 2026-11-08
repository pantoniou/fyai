/* SPDX-License-Identifier: MIT */
#ifndef FYAI_REMOTE_H
#define FYAI_REMOTE_H

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
/* Keep a frame of the worker for the next step. */
void fyai_remote_ui_receive(struct fyai_remote_ui *view, fy_generic frame);
/* Serve the frames and the typed input; 1 to detach, -1 on error. */
int fyai_remote_ui_step(struct fyai_remote_ui *view);
void fyai_remote_ui_close(struct fyai_remote_ui *view);

/*
 * Present a frame of the sink of a remote command (src/fyai_sink_frames.c)
 * with the sink of @ctx. -1 with a diagnostic for a frame it cannot present.
 */
int fyai_sink_frames_present(struct fyai_ctx *ctx, fy_generic frame);

#endif
