/*
 * fyai_editor.c - the external editor child
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_UNKNOWN

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <libfyaml/libfyaml-align.h>

#include "fyai.h"
#include "fyai_auth_util.h"
#include "fyai_diag.h"
#include "fyai_event.h"
#include "fyai_host.h"
#include "fyai_sink.h"
#include "fyai_sandbox.h"
#include "fyai_fsview.h"
#include "fyai_terminal.h"
#include "fyai_terminal_view.h"
#include "fyai_tools.h"
#include "fyai_ui.h"
#include "fyai_uimodel.h"
#include "fyai_editor.h"

/*
 * External editor child operation:
 *
 *   NEW -> RUNNING -> COMPLETED
 *             |
 *             +-> CANCEL_REQUESTED -> COMPLETED
 *             |
 *             +-> ABANDONED -> REAPED_AND_FREED
 *
 * Cancellation signals the child but retains its child source. Destruction
 * before completion abandons caller notification; the child callback performs
 * the final reap and frees the request. No editor child becomes a zombie just
 * because its original UI or command owner went away.
 */
struct fyai_editor_request {
	struct fyai_ctx *ctx;
	struct fyai_event_source *child_src;
	/* The tile the editor runs in instead of a child, until it ends. */
	struct fyai_shell_session *session;
	fyai_editor_complete_fn complete;
	void *userdata;
	pid_t pid;
	int status;
	bool done;
	bool cancelled;
	bool notified;
	bool abandoned;
};

/* The context that this object stands on. */
static inline struct fyai_ctx *fyai_editor_request_ctx(const struct fyai_editor_request *o)
{
	return o->ctx;
}

static void fyai_editor_notify(struct fyai_editor_request *request)
{
	if (request->notified)
		return;
	request->notified = true;
	if (request->complete)
		request->complete(request, request->userdata);
}

static enum fyai_event_action
fyai_editor_child_complete(const struct fyai_event *ev)
{
	struct fyai_editor_request *request;

	request = ev->userdata;
	request->child_src = NULL;
	request->pid = -1;
	request->status = ev->status;
	request->done = true;
	fyai_editor_notify(request);
	if (request->abandoned)
		free(request);
	return FYAIEA_CONTINUE;
}

bool fyai_editor_in_pane(struct fyai_ctx *ctx)
{
	if (!ctx || !ctx->cfg)
		return false;
	/* A worker has no terminal: the client of its model shows the editor
	 * in a tile, which a remote terminal session has. */
	if (!fyai_ui_active(ctx))
		return fyai_uimodel_consumed(ctx);
	return !ctx->cfg->editor_mode ||
	       strcmp(ctx->cfg->editor_mode, "terminal");
}

/* The editor in a tile ended. Its wait status is made as a child's. */
static void fyai_editor_program_exited(void *userdata, int exit_code,
				       int signal)
{
	struct fyai_editor_request *request;

	request = userdata;
	request->session = NULL;
	request->status = signal ? (signal & 0x7f) : ((exit_code & 0xff) << 8);
	request->done = true;
	fyai_editor_notify(request);
	if (request->abandoned)
		free(request);
}

struct fyai_editor_request *
fyai_editor_submit(struct fyai_ctx *ctx, const char *path, bool readonly,
		   fyai_editor_complete_fn complete, void *userdata)
{
	const char *editor;
	struct fyai_editor_request *request;
	struct fyai_event_loop *el;
	char *cmd = NULL;
	pid_t pid;
	int rc;

	if (!ctx || !path)
		return NULL;
	request = calloc(1, sizeof(*request));
	if (!request)
		return NULL;
	request->ctx = ctx;
	request->pid = -1;
	request->complete = complete;
	request->userdata = userdata;
	editor = getenv("VISUAL");
	if (!editor || !*editor)
		editor = getenv("EDITOR");
	if (!editor || !*editor)
		editor = "vi";
	rc = asprintf(&cmd, readonly ? "%s -R '%s'" : "%s '%s'",
		      editor, path);
	fyai_error_check(ctx, rc >= 0, err_out,
			 "could not format editor command");
	if (fyai_editor_in_pane(ctx)) {
		request->session = fyai_tools_user_program(ctx, cmd,
				fyai_editor_program_exited, request);
		fyai_error_check(ctx, request->session, err_out,
				 "could not start the editor in the work pane");
		free(cmd);
		return request;
	}
	pid = fork();
	fyai_error_check(ctx, pid >= 0, err_out, "could not start editor: %s",
			 strerror(errno));
	if (!pid) {
		fyai_loop_restore_signal_mask(ctx ? ctx->loop : NULL);
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
	request->pid = pid;
	el = fyai_ctx_loop(ctx);
	fyai_error_check(ctx, el, err_child,
			 "could not acquire event loop for editor");
	rc = fyai_event_add_child(el, pid, fyai_editor_child_complete,
				  request, &request->child_src);
	fyai_error_check(ctx, !rc, err_child,
			 "could not watch editor process");
	free(cmd);
	return request;

err_child:
	(void)kill(pid, SIGKILL);
	while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
		;
err_out:
	free(cmd);
	free(request);
	return NULL;
}

void fyai_editor_cancel(struct fyai_editor_request *request)
{
	if (!request || request->done || request->cancelled)
		return;
	request->cancelled = true;
	if (request->session)
		fyai_tools_user_program_close(request->session);
	if (request->pid > 0)
		(void)kill(request->pid, SIGTERM);
}

bool fyai_editor_done(const struct fyai_editor_request *request)
{
	return request && request->done;
}

int fyai_editor_collect(const struct fyai_editor_request *request)
{
	if (!request || !request->done || request->cancelled)
		return -1;
	return WIFEXITED(request->status) && !WEXITSTATUS(request->status) ?
		0 : -1;
}

void fyai_editor_destroy(struct fyai_editor_request *request)
{
	if (!request)
		return;
	if (!request->done) {
		fyai_editor_cancel(request);
		request->complete = NULL;
		request->userdata = NULL;
		request->abandoned = true;
		return;
	}
	free(request);
}

static void fyai_editor_sync_complete(struct fyai_editor_request *request,
				      void *userdata)
{
	volatile bool *done;

	(void)request;
	done = userdata;
	*done = true;
}

static int fyai_spawn_editor_mode(struct fyai_ctx *ctx, const char *path,
				  bool readonly)
{
	struct fyai_editor_request *request;
	struct fyai_event_loop *el;
	volatile bool done;
	int rc;

	done = false;
	request = fyai_editor_submit(ctx, path, readonly,
				     fyai_editor_sync_complete, (void *)&done);
	if (!request)
		return -1;
	el = fyai_ctx_loop(ctx);
	while (!done) {
		rc = fyai_event_loop_step(el, -1);
		if (rc < 0) {
			fyai_editor_cancel(request);
			break;
		}
	}
	rc = fyai_editor_done(request) ? fyai_editor_collect(request) : -1;
	if (rc)
		fyai_error(ctx, "editor exited unsuccessfully");
	fyai_editor_destroy(request);
	return rc;
}

int fyai_spawn_editor_readonly(struct fyai_ctx *ctx, const char *path)
{
	return fyai_spawn_editor_mode(ctx, path, true);
}
