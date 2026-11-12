/*
 * fyai_tool_job.c - tool jobs and job groups
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_TOOLS

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef __FILC__
#include <stdfil.h>
#endif
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>

#include <fcntl.h>
#include <termios.h>
#include "fyai_reqcache.h"
#include "fyai_host.h"
#include <sys/ioctl.h>
#include <sys/wait.h>

#include "fyai_transport_boot.h"
#include "fyai_transport_sock.h"
#include "fyai_agent.h"
#include "fyai_monitor.h"
#include "fyai_tool_registry.h"
#include "fyai_branch.h"
#include "fyai_browser.h"
#include "fyai_agents.h"
#include "fyai_chrome.h"
#include "fyai_jsonrpc.h"
#include "fyai_config.h"
#include "fyai_display.h"
#include "fyai_event.h"
#include "fyai_markdown.h"
#include "fyai_patch.h"
#include "fyai_sandbox.h"
#include "fyai_sandbox_profile.h"
#include "fyai_session.h"
#include "fyai_storage.h"
#include "fyai_terminal.h"
#include "fyai_terminal_session.h"
#include "fyai_output.h"
#include "fyai_ask.h"
#include "fyai_skill.h"
#include "fyai_settings.h"
#include "fyai_todo.h"
#include "fyai_home.h"
#include "fyai_tools.h"
#include "fyai_tools_int.h"
#include "fyai_orphan.h"
#include "fyai_view.h"
#include "fyai_cover.h"
#include "fyai_fsview.h"
#include "fyai_asks.h"
#include "fyai_wait.h"
#include "fyai_prof.h"
#include "fyai_render.h"
#include "fyai_sink.h"
#include "fyai_ui.h"
#include "fyai_uimodel.h"
#include "fyai_uilog.h"
#include "fyai_uiwork.h"
#include "fyai_uitranscript.h"
#include "fyai_workpane.h"

/* Keep the live jobs reachable, so that a resize finds every child. */
static void fyai_tool_job_link(struct fyai_ctx *ctx, struct fyai_tool_job *job)
{
	job->next = ctx->tool_jobs;
	ctx->tool_jobs = job;
	fyai_tools_work_update(ctx);
}

static void fyai_tool_job_unlink(struct fyai_ctx *ctx,
				 struct fyai_tool_job *job)
{
	struct fyai_tool_job **pp;

	for (pp = &ctx->tool_jobs; *pp; pp = &(*pp)->next) {
		if (*pp != job)
			continue;
		*pp = job->next;
		job->next = NULL;
		fyai_tools_work_update(ctx);
		return;
	}
}

/* Close inherited job descriptors without affecting parent-owned jobs. */
void fyai_tool_jobs_abandon(struct fyai_ctx *ctx)
{
	struct fyai_tool_job *job, *next;

	if (!ctx)
		return;
	for (job = ctx->tool_jobs; job; job = next) {
		next = job->next;
		/* Never a standard descriptor: this child keeps its own. */
		if (job->pfd > STDERR_FILENO)
			close(job->pfd);
		if (job->rfd > STDERR_FILENO)
			close(job->rfd);
		if (job->pty > STDERR_FILENO)
			close(job->pty);
		job->pfd = job->rfd = job->pty = -1;
		job->conn = NULL;
		job->session = NULL;
		job->animation = NULL;
		job->next = NULL;
	}
	ctx->tool_jobs = NULL;
}

/*
 * The window of the user changed. A tool child called setsid(), so the kernel
 * does not signal it. The parent sends the new size on the control channel,
 * and the child applies it to its pseudo-terminal.
 */
void fyai_tool_jobs_resize(struct fyai_ctx *ctx, int rows, int cols)
{
	struct fy_generic_builder *gb;
	struct fyai_tool_job *job;

	if (!ctx || rows <= 0 || cols <= 0)
		return;
	gb = fyai_ctx_transient_gb(ctx);
	if (!gb)
		return;

	for (job = ctx->tool_jobs; job; job = job->next) {
		if (!job->conn || job->phase == FYAI_JOB_DONE)
			continue;
		/* Displayed terminals use their post-layout tile size. */
		if ((job->session && job->session->surface) || job->surface)
			continue;
		(void)jsonrpc_notify(job->conn, "tty/resize",
				fy_gb_mapping(gb, "rows", (long long)rows,
					      "cols", (long long)cols));
	}
}

static void fyai_tool_job_drop(struct fyai_event_source **srcp)
{
	fyai_event_source_remove(*srcp);
	*srcp = NULL;
}

/*
 * Set the verdict of the job. The verdict of a failed control channel is
 * final.
 */
static void fyai_tool_job_verdict_set(struct fyai_tool_job *job, bool ok)
{
	if (job->verdict == FYAI_JOB_CHANNEL_FAILED)
		return;
	job->verdict = ok ? FYAI_JOB_OK : FYAI_JOB_NOT_OK;
}

/*
 * Move a job with a settled request to DONE. A session job completes when its
 * start answer arrives; any other job also waits for its child.
 */
static void fyai_tool_job_settle(struct fyai_tool_job *job)
{
	if (job->phase != FYAI_JOB_ANSWERED)
		return;
	if (!job->session && !job->reaped)
		return;
	job->phase = FYAI_JOB_DONE;

	fyai_tools_work_update(job->ctx);
	fyai_tool_job_drop(&job->band_delay);
	job->elapsed_ms = fyai_event_now_ms() - job->started_ms;
	if (job->agent && job->timeout_ms &&
	    job->elapsed_ms >= job->timeout_ms)
		job->overdue = true;
	/* Remove the deadline before the job waits for its group. */
	fyai_tool_job_drop(&job->deadline);
	fyai_tool_job_drop(&job->hang_deadline);
	if (job->agent && job->branch) {
		/*
		 * The sub-agent answers no question now: drop the wait
		 * reports it queued, or a later turn asks the model about
		 * an agent that ended.
		 */
		fyai_events_drop_agent(job->ctx, job->branch);
	}
	if (job->stream.active) {
		/*
		 * Test `timed_out` here and during collection. The parent owns
		 * the deadline. A stopped job can report success immediately
		 * before termination. This state selects the mark before
		 * collection corrects the result.
		 */
		(void)fyai_fenced_stream_set_indicator(&job->stream,
			job->verdict == FYAI_JOB_OK && !job->timed_out ?
			FYMD_INDICATOR_SUCCESS : FYMD_INDICATOR_FAILURE, 0);
		(void)fyai_fenced_stream_push(&job->stream, NULL, 0);
	}
	if (job->group)
		fyai_tool_job_group_service(job->group);
}

static enum fyai_event_action fyai_tool_job_child(const struct fyai_event *ev)
{
	struct fyai_tool_job *job = ev->userdata;

	job->csrc = NULL;
	job->reaped = true;
	/* Preserve a session start result after its serving child exits. */
	if (!job->have_result)
		fyai_tool_job_verdict_set(job, WIFEXITED(ev->status) &&
					       WEXITSTATUS(ev->status) == 0);
	job->term_signal = WIFSIGNALED(ev->status) ?
				WTERMSIG(ev->status) : 0;
	fyai_tool_job_settle(job);
	return FYAIEA_CONTINUE;
}

/* A sub-agent is named by the branch it owns: main/agent:<name>. */
const char *fyai_agent_job_name(const struct fyai_tool_job *job)
{
	const char *who;

	who = job->branch ? strrchr(job->branch, '/') : NULL;
	return who && !strncmp(who + 1, FYAI_BRANCH_AGENT_PREFIX,
		strlen(FYAI_BRANCH_AGENT_PREFIX)) ?
		who + 1 + strlen(FYAI_BRANCH_AGENT_PREFIX) : "agent";
}

/* The running sub-agent called @name, or NULL. */
struct fyai_tool_job *fyai_agent_job_named(struct fyai_ctx *ctx,
					   const char *name)
{
	struct fyai_tool_job *job;
	const char *who;
	char slug[FYAI_BRANCH_COMPONENT_MAX + 1];

	if (fy_str_empty(name))
		return NULL;
	fyai_branch_sanitize(name, "agent", slug, sizeof(slug));
	for (job = ctx->tool_jobs; job; job = job->next) {
		if (!job->agent || job->phase == FYAI_JOB_DONE || !job->branch)
			continue;
		who = fyai_agent_job_name(job);
		if (!strcmp(job->branch, name) || !strcmp(who, name) ||
		    !strcmp(who, slug))
			return job;
	}
	return NULL;
}

const char *fyai_tool_agent_live_branch(struct fyai_ctx *ctx, const char *name)
{
	struct fyai_tool_job *job = fyai_agent_job_named(ctx, name);

	return job ? job->branch : NULL;
}

bool fyai_tool_agent_foreground_wait(struct fyai_ctx *ctx,
				     struct jsonrpc_conn *conn)
{
	struct fyai_tool_job *job;
	fy_generic args;

	for (job = ctx->tool_jobs; job; job = job->next) {
		if (!job->agent || job->phase == FYAI_JOB_DONE ||
		    job->conn != conn)
			continue;
		args = fyai_tool_call_args(ctx, job->call);
		if (!fy_get(args, "_fyai_background", false) && !job->btw)
			return true;
	}
	return false;
}

/* Present a named sub-agent's question and return the user's answer. */
static fy_generic fyai_ask_user_for_child(struct fyai_tool_job *job,
					  struct jsonrpc_conn *conn,
					  fy_generic id, fy_generic params)
{
	fy_generic result;
	const char *who, *origin;
	int rc;

	who = fyai_agent_job_name(job);
	origin = fy_get(params, "branch", "");
	if (*origin && (!job->branch || strcmp(origin, job->branch)))
		who = origin;
	result = fyai_ask_user_from(job->ctx, params, who);
	if (fy_is_string(result)) {
		/* The question was not valid, or nobody can be asked. */
		fyai_error(job->ctx, "agent: the questions of '%s' were not asked: %s",
			   who, fy_castp(&result, ""));
		result = fyai_ask_declined(job->ctx);
	}
	rc = jsonrpc_conn_respond(conn, id, result, fy_invalid);
	fyai_error_check(job->ctx, !rc, err,
			 "agent: could not return the user's answer to '%s'", who);

err:
	return fy_invalid;
}

/* The live job of this process that owns @branch, or NULL. */
static struct fyai_tool_job *fyai_tool_job_by_branch(struct fyai_ctx *ctx,
						      const char *branch)
{
	struct fyai_tool_job *job;

	for (job = ctx->tool_jobs; job; job = job->next) {
		if (job->branch && !strcmp(job->branch, branch) &&
		    job->phase != FYAI_JOB_DONE)
			return job;
	}
	return NULL;
}

int fyai_tool_agent_respond(struct fyai_ctx *ctx, const char *branch,
			    long long rpc_id, fy_generic result)
{
	struct fyai_tool_job *job;

	job = fyai_tool_job_by_branch(ctx, branch);
	if (!job || !job->conn)
		return -1;
	return jsonrpc_conn_respond(job->conn,
				    fy_value(fyai_ctx_transient_gb(ctx), rpc_id),
				    result, fy_invalid);
}

uint64_t fyai_tool_agent_transport_exec(struct fyai_ctx *ctx, const char *name)
{
	struct fyai_tool_job *job;
	size_t bl, nl;

	nl = strlen(name);
	for (job = ctx->tool_jobs; job; job = job->next) {
		if (!job->agent || job->phase == FYAI_JOB_DONE || !job->branch)
			continue;
		bl = strlen(job->branch);
		if (!strcmp(job->branch, name))
			return job->transport_exec;
		/* The name that the delegation gave, without the branch path. */
		if (bl > nl + 6 && !strcmp(job->branch + bl - nl, name) &&
		    !strncmp(job->branch + bl - nl - 6, "agent:", 6))
			return job->transport_exec;
	}
	return 0;
}

/* True while a job of this process owns @branch. */
static bool fyai_tool_job_branch_live(struct fyai_ctx *ctx, const char *branch)
{
	return fyai_tool_job_by_branch(ctx, branch) != NULL;
}

void fyai_tool_agent_title_refresh(struct fyai_ctx *ctx, const char *branch)
{
	struct fyai_tool_job *job;

	job = fyai_tool_job_by_branch(ctx, branch);
	if (job && job->surface)
		fyai_agent_head_paint(job);
}

/* Spawn a tool child, optionally with a PTY on its standard descriptors. */
static int fyai_tool_job_spawn(struct fyai_ctx *ctx,
			       struct fyai_tool_job *job, bool pty, bool exec,
			       const struct fyai_fsview *view, bool agent_runtime)
{
	int req[2] = { -1, -1 };	/* parent -> child */
	int rsp[2] = { -1, -1 };	/* child -> parent */
	int tpa[2] = { -1, -1 };	/* the transport channel of a sub-agent */
	int tpc[2] = { -1, -1 };	/* its control connection */
	int vsync[2] = { -1, -1 };	/* the view namespace init announces itself */
	int master = -1, slave = -1;
	struct winsize ws = {};
	int rows = 0, cols = 0;
	bool tp = exec && ctx->transport.client;
	pid_t pid;
	int pidfd, rc;

	memset(job, 0, sizeof(*job));
	job->ctx = ctx;
	job->exec = exec;
	job->rfd = -1;
	job->pfd = -1;
	job->pty = -1;
	rc = pipe(req);
	fyai_error_check(ctx, !rc, err,
			 "could not create tool request pipe: %s",
			 strerror(errno));
	rc = pipe(rsp);
	fyai_error_check(ctx, !rc, err,
			 "could not create tool response pipe: %s",
			 strerror(errno));
	rc = fcntl(rsp[0], F_SETFL, O_NONBLOCK);
	fyai_error_check(ctx, !rc, err,
			 "could not make the tool channel non-blocking: %s",
			 strerror(errno));
	if (pty) {
		rc = fyai_host_openpty(&master, &slave);
		fyai_error_check(ctx, !rc, err,
				 "could not open a terminal for the tool: %s",
				 strerror(errno));
		fyai_agent_tty_size(ctx, &rows, &cols);
		ws.ws_row = (unsigned short)rows;
		ws.ws_col = (unsigned short)cols;
		(void)ioctl(slave, TIOCSWINSZ, &ws);
	}
	if (tp) {
		rc = fyai_transport_socketpair(tpa);
		fyai_error_check(ctx, !rc, err,
				 "could not create the sub-agent transport channel: %s",
				 strerror(errno));
		rc = fyai_transport_socketpair(tpc);
		fyai_error_check(ctx, !rc, err,
				 "could not create the sub-agent control channel: %s",
				 strerror(errno));
	}
	if (tp && view) {
		rc = fyai_transport_socketpair(vsync);
		fyai_error_check(ctx, !rc, err,
				 "could not create the view announcement channel: %s",
				 strerror(errno));
	}
	pid = fork();
	fyai_error_check(ctx, pid >= 0, err,
			 "could not fork tool process: %s", strerror(errno));

	if (!pid) {			/* child */
		close(req[1]);
		close(rsp[0]);
		if (vsync[0] >= 0)
			close(vsync[0]);
		if (tp) {
			close(tpa[1]);
			close(tpc[1]);
		}
		/* A credential grant is for this child; it then holds no other. */
		fyai_transport_env_take(ctx);
		if (setsid() < 0)
			(void)setpgid(0, 0);	/* already a leader: still isolate */
		fyai_ctx_loop_abandon(ctx);
		/* Abandon unblocks SIGTERM. Re-block it before the handler
		 * below is armed, or a cancellation here kills the child. */
		fyai_tool_child_signals_block();
		if (master >= 0)
			close(master);
		/* Install the PTY before arranging standard and control descriptors. */
		if (slave >= 0 && fyai_tool_child_tty(slave))
			_exit(126);
		/* Record whether this child presents through its own terminal. */
		ctx->cfg->agent_pty = slave >= 0;
		if (fyai_tool_child_fds(ctx, req[0], rsp[1], tp ? tpa[0] : -1,
					tp ? tpc[0] : -1, vsync[1], agent_runtime))
			_exit(126);
		/*
		 * The view is entered after the descriptors are arranged:
		 * entering closes none of them. The process that enters
		 * waits for the namespace init and then ends with its status.
		 */
		if (view && fyai_fsview_enter(ctx->cfg, view, -1, vsync[1] >= 0 ?
						       FYAI_TOOL_CHILD_VIEW_FD : -1))
			_exit(FYAI_SHELL_EXIT_SANDBOX);
		if (view && agent_runtime)
			ctx->cfg->arena_dir = ctx->cfg->view_arena;
		if (exec)
			fyai_tool_child_exec(ctx, slave >= 0, tp);

		fyai_ctx_fork_disown(ctx);
		ctx->cfg->tool_child = true;
		if (agent_runtime)
			ctx->cfg->diag.mask &= ~(1u << FYAIET_NOTICE);
		fyai_diag_trace_reopen();
		fyai_tool_child_signals(ctx);
		if (fyai_setup_transient_builder(ctx))
			_exit(1);
		fyai_tool_child_serve_loop(ctx);	/* never returns */
		_exit(1);
	}

	close(req[0]);
	req[0] = -1;
	close(rsp[1]);
	rsp[1] = -1;
	if (slave >= 0) {
		close(slave);
		slave = -1;
	}
	if (tp) {
		/*
		 * Register the child with the transport before it can send a
		 * request: it sends none until tool/run, which follows this.
		 * The transport keeps the other ends; ours are closed.
		 */
		close(tpa[0]);
		tpa[0] = -1;
		close(tpc[0]);
		tpc[0] = -1;
		/*
		 * A child in a view runs in a PID namespace of its own, which the
		 * transport cannot name by a PID: it takes the pidfd that the child
		 * sent.
		 */
		pidfd = -1;
		if (vsync[0] >= 0) {
			close(vsync[1]);
			vsync[1] = -1;
			rc = fyai_fsview_init_pidfd(vsync[0], &pidfd);
			fyai_error_check(ctx, !rc, err_kill,
					 "could not learn the process of the sub-agent in its view: %s",
					 strerror(errno));
		}
		/* A pidfd names the child also from inside a PID namespace of its own. */
		if (pidfd < 0)
			pidfd = fyai_host_pidfd_open(pid);
		rc = fyai_transport_admit_child(ctx, pid, pidfd, tpa[1], tpc[1],
						&job->transport_exec);
		if (!rc && vsync[0] >= 0) {
			/* The agent runs only after the transport knows it. */
			rc = fyai_fsview_init_release(vsync[0], NULL, NULL);
			fyai_error_check(ctx, !rc, err_kill,
					 "could not release the sub-agent in its view: %s",
					 strerror(errno));
		}
		if (pidfd >= 0)
			close(pidfd);
		close(tpa[1]);
		tpa[1] = -1;
		close(tpc[1]);
		tpc[1] = -1;
		if (vsync[0] >= 0) {
			close(vsync[0]);
			vsync[0] = -1;
		}
		if (rc)
			goto err_kill;
	}
	job->pid = pid;
	job->rfd = rsp[0];
	job->pfd = req[1];
	job->pty = master;
	job->pty_rows = rows;
	job->pty_cols = cols;
	rsp[0] = -1;
	req[1] = -1;
	master = -1;
	return 0;

err_kill:
	kill(pid, SIGKILL);
	waitpid(pid, NULL, 0);
err:
	if (req[0] >= 0)
		close(req[0]);
	if (req[1] >= 0)
		close(req[1]);
	if (rsp[0] >= 0)
		close(rsp[0]);
	if (rsp[1] >= 0)
		close(rsp[1]);
	if (tpa[0] >= 0)
		close(tpa[0]);
	if (tpa[1] >= 0)
		close(tpa[1]);
	if (tpc[0] >= 0)
		close(tpc[0]);
	if (tpc[1] >= 0)
		close(tpc[1]);
	if (vsync[0] >= 0)
		close(vsync[0]);
	if (vsync[1] >= 0)
		close(vsync[1]);
	if (master >= 0)
		close(master);
	if (slave >= 0)
		close(slave);
	return -1;
}


static void fyai_tool_job_live_close(struct fyai_tool_job *job,
				     bool commit_band)
{
	fyai_event_source_remove(job->band_delay);
	job->band_delay = NULL;
	free(job->pending_output.data);
	memset(&job->pending_output, 0, sizeof(job->pending_output));
	/* A sub-agent's screen is committed with its outcome, not discarded. */
	if (job->agent)
		fyai_agent_view_close(job, job->verdict == FYAI_JOB_OK,
				      NULL);
	if (job->stream.active)
		fyai_fenced_stream_finish(&job->stream);
	if (commit_band)
		fyai_sink_band_commit(job->band);
	else
		fyai_sink_band_destroy(job->band);
	job->band = NULL;
	if (!job->btw_panel) {
		free(job->title);
		job->title = NULL;
	}
	free(job->command);
	job->command = NULL;
}

static int fyai_tool_job_attach(struct fyai_ctx *ctx,
				struct fyai_tool_job *job);

static void fyai_tool_job_close_channel(struct fyai_tool_job *job)
{
	if (job->agent && job->branch)
		fyai_agents_message_branch_closed(job->ctx, job->branch);
	if (job->run) {
		jsonrpc_request_destroy(job->run);
		job->run = NULL;
	}
	if (job->conn) {
		fyai_agents_conn_closed(job->ctx, job->conn);
		jsonrpc_conn_destroy(job->conn);
		job->conn = NULL;
	}
	if (job->rfd >= 0)
		close(job->rfd);
	job->rfd = -1;
	if (job->pfd >= 0)
		close(job->pfd);
	job->pfd = -1;
}

void fyai_tool_job_discard(struct fyai_tool_job *job)
{
	if (!job)
		return;
	job->btw_panel = false;
	if (job->inline_block) {
		fyai_ui_inline_drop(job->ctx, (uintptr_t)job);
		job->inline_block = false;
	}
	fyai_tool_job_cancel(job);
	/*
	 * A discarded job settles nothing through update_done, so drop its
	 * queued wait reports here: the branch answers no question after this.
	 */
	if (job->agent && job->branch)
		fyai_events_drop_agent(job->ctx, job->branch);
	fyai_tool_job_close_channel(job);
	fyai_tool_job_drop(&job->deadline);
	fyai_tool_job_drop(&job->hang_deadline);
	fyai_tool_job_drop(&job->csrc);
	if (!job->reaped && job->pid > 0)
		while (waitpid(job->pid, NULL, 0) < 0 && errno == EINTR)
			;
	fyai_tool_job_live_close(job, false);
	fyai_tool_job_unlink(job->ctx, job);
	/* A cancelled isolated agent still leaves what it wrote in its view. */
	if (job->view_run) {
		char *summary = NULL;

		(void)fyai_view_run_finish(job->ctx, job->view_run, &summary);
		free(summary);
	}
	fyai_view_run_free(job->view_run);
	free(job->progress.data);
	free(job->branch);
	free(job->monitor);
	free(job->origin);
	fy_generic_builder_destroy(job->call_gb);
	free(job);
}

static enum fyai_event_action
fyai_tool_job_deadline(const struct fyai_event *ev);
static enum fyai_event_action
fyai_tool_job_hang_deadline(const struct fyai_event *ev);
static unsigned int fyai_tool_job_timeout_ms(struct fyai_ctx *ctx,
					     const char *name, bool native_call,
					     fy_generic args);

static const char *fyai_tool_submit_error(struct fyai_ctx *ctx)
{
	if (!ctx->tool_submit_error)
		return "tool error: could not start the tool";
	return ctx->tool_submit_error;
}

static void fyai_tool_submit_error_set(struct fyai_ctx *ctx,
				       const char *fmt, ...)
{
	va_list ap;
	char *msg = NULL;

	va_start(ap, fmt);
	if (vasprintf(&msg, fmt, ap) < 0)
		msg = NULL;
	va_end(ap);
	free(ctx->tool_submit_error);
	ctx->tool_submit_error = msg;
}

/*
 * Report a submission failure and jump to a cleanup label. A failure the model
 * is told about is handled: the caller answers the call with the message that
 * fyai_tool_submit_error_set() left. Such a report stays below error severity,
 * so it neither latches as the cause of the run - hiding a later real failure
 * behind it - nor fails an otherwise complete sub-agent. A failure with no
 * message for the model has nothing to answer it, and keeps error severity.
 */
#define fyai_tool_submit_check(_ctx, _cond, _label, _fmt, ...)		\
	do {								\
		if (!(_cond)) {						\
			if ((_ctx)->tool_submit_error)			\
				fyai_warning((_ctx), (_fmt),		\
					     ##__VA_ARGS__);		\
			else						\
				fyai_error((_ctx), (_fmt),		\
					   ##__VA_ARGS__);		\
			goto _label;					\
		}							\
	} while (0)

/*
 * The rows that a live tool band keeps. The band shows the preview rows and
 * the user scrolls back through the rest, so the stream renders more rows than
 * the band draws. The committed exchange keeps the preview. A call drawn in
 * the transcript keeps the preview alone.
 */
static size_t fyai_tool_history_lines(const struct fyai_ctx *ctx)
{
	const struct fyai_cfg *cfg = ctx->cfg;

	if (cfg->tool_preview_lines <= 0)
		return 0;
	/* The transcript scrolls a call drawn in it: it keeps no history. */
	if (fyai_ui_tools_inline(ctx))
		return (size_t)cfg->tool_preview_lines;
	return cfg->tool_history_lines > cfg->tool_preview_lines ?
	       (size_t)cfg->tool_history_lines :
	       (size_t)cfg->tool_preview_lines;
}

static void fyai_tool_job_band_open(struct fyai_tool_job *job)
{
	struct fyai_ctx *ctx = job->ctx;
	size_t lines = fyai_tool_history_lines(ctx);
	int rc = -1;

	job->band = fyai_sink_band_open(ctx->sink, false, NULL, NULL);
	if (job->band && job->agent) {
		/* The progress of a sub-agent is its prose: a quoted stream. */
		rc = fyai_markdown_quote_stream_start(&job->stream, ctx,
				ctx->cfg, lines, NULL, true);
	} else if (job->band) {
		rc = fyai_fenced_stream_start(&job->stream, ctx, ctx->cfg,
				NULL, lines,
				markdown_tool_output_indent(ctx->cfg), NULL,
				true);
	}
	if (rc) {
		fyai_tool_job_live_close(job, false);
		return;
	}
	fyai_fenced_stream_bind_band(&job->stream, job->band);
	job->stream.title = job->title;
	job->stream.command = job->command;
	fyai_sink_band_paint(job->band, job->title, job->command, NULL, 0,
			     NULL);
	if (job->pending_output.len)
		(void)fyai_fenced_stream_push(&job->stream,
			job->pending_output.data, job->pending_output.len);
	free(job->pending_output.data);
	memset(&job->pending_output, 0, sizeof(job->pending_output));
}

static enum fyai_event_action
fyai_tool_job_band_delay(const struct fyai_event *ev)
{
	struct fyai_tool_job *job = ev->userdata;

	job->band_delay = NULL;
	if (job->phase != FYAI_JOB_DONE)
		fyai_tool_job_band_open(job);
	return FYAIEA_CONTINUE;
}

/* Whether a scope lasts as long as the interactive session. */
static bool fyai_scope_session(const struct fyai_ctx *ctx)
{
	return !strcmp(ctx->cfg->sandbox_scope, "session");
}

/* End the scope when no exchange runs and no object keeps it. */
static void fyai_scope_settle(struct fyai_ctx *ctx)
{
	if (ctx->scope_open || ctx->scope_pins > 0 || fyai_scope_session(ctx))
		return;
	/* A process that did not start the holder does not end it. */
	if (ctx->cover.owner) {
		fyai_diag_tracef("scope", "end, holder %ld", (long)ctx->cover.holder);
		fyai_cover_release(&ctx->cover);
	}
	if (fyai_orphan_count(ctx))
		fyai_diag_tracef("scope", "stop %zu tracked daemons",
				 fyai_orphan_count(ctx));
	fyai_orphan_sweep(ctx);
}

void fyai_scope_exchange(struct fyai_ctx *ctx, bool begin)
{
	ctx->scope_open = begin;
	if (!begin)
		fyai_scope_settle(ctx);
}

void fyai_scope_pin(struct fyai_ctx *ctx)
{
	ctx->scope_pins++;
}

void fyai_scope_unpin(struct fyai_ctx *ctx)
{
	if (ctx->scope_pins > 0)
		ctx->scope_pins--;
	fyai_scope_settle(ctx);
}

/*
 * Start the holder of the scope of the exchange before a tool child forks, so
 * that the child joins it and the holder outlives the call. The tool child
 * and a scope of one call make their own.
 */
static void fyai_scope_ensure(struct fyai_ctx *ctx)
{
	struct fyai_shell_sandbox sb;
	const struct fyai_sandbox_spec *spec;
	pid_t holder;

	if (ctx->cfg->tool_child || !ctx->cfg->enable_sandbox ||
	    ctx->sandbox_applied || !strcmp(ctx->cfg->sandbox_scope, "call"))
		return;
	holder = ctx->cover.holder;
	if (fyai_shell_sandbox_begin(ctx, &sb, &spec))
		return;
	fyai_shell_sandbox_end(&sb);
	if (ctx->cover.holder && ctx->cover.holder != holder)
		fyai_diag_tracef("scope", "begin, holder %ld", (long)ctx->cover.holder);
}

struct fyai_tool_job *fyai_tool_job_submit(struct fyai_ctx *ctx,
					    fy_generic tool_call)
{
	struct fyai_tool_job *job = NULL;
	struct fyai_view_run *view_run = NULL;
	struct fyai_buf view = {0};
	const char *name, *command;
	const char *asked, *cmdtext;
	fy_generic args, progress_args, agent_name, monitor_v;
	bool agent_stored = false;
	fy_generic session_call, session_command, session_desc;
	struct fyai_event_loop *el;
	char child_branch[FYAI_BRANCH_NAME_MAX + 1];
	char *session_name = NULL;
	char *session_title = NULL;
	char *label = NULL;
	bool have_session = false;
	bool have_branch = false;
	bool native_call;
	bool eligible;
	bool user_owned;
	struct fy_generic_builder_cfg call_cfg = {};
	int srows = 0, scols = 0;
	int rc;

	free(ctx->tool_submit_error);
	ctx->tool_submit_error = NULL;
	eligible = fyai_tool_call_parallel_eligible(ctx, tool_call);
	fyai_error_check(ctx, eligible, err,
		"tool call is not eligible for asynchronous submission");
	name = fyai_tool_call_name(ctx, tool_call);
	native_call = fy_equal(fy_get(tool_call, "type"), "shell_call");
	args = native_call ? tool_call : fyai_tool_call_args(ctx, tool_call);
	fyai_error_check(ctx, fy_is_valid(args), err,
			 "invalid tool call arguments");
	if (!native_call && fyai_shell_args_lack_command(name, args)) {
		fyai_tool_submit_error_set(ctx, "%s", FYAI_SHELL_NO_COMMAND);
		fyai_tool_submit_check(ctx, false, err,
				       "the shell call names no command");
	}
	if (fyai_agent_delegated(ctx)) {
		char *header;

		progress_args = native_call ?
			fy_mapping("command",
				fy_cast(fy_get_at_path(tool_call, "action",
						      "commands", 0), "")) :
			args;
		header = fyai_format_tool_header(ctx, name, progress_args,
					fyai_tool_preview_lines(ctx->cfg, name));
		if (header) {
			fyai_tool_progress_emit(ctx, header, strlen(header));
			fyai_tool_progress_flush(ctx);
			free(header);
		}
	}
	/* Reserve the sub-agent branch before the job is spawned. */
	if (fy_equal(name, "agent")) {
		rc = fyai_branches_refresh(ctx);
		fyai_error_check(ctx, !rc, err,
				 "could not refresh the branch table");
		agent_name = fy_get(args, "name", fy_invalid);
		/* Reuse stored agents, but reject a name owned by a live job. */
		rc = fyai_branch_alloc_child(ctx, fyai_ctx_branch(ctx),
				fy_castp(&agent_name, "agent"),
				(unsigned int)ctx->cfg->agent_max_branch_depth,
				child_branch, sizeof(child_branch),
				&agent_stored);
		if (!rc && fyai_tool_job_branch_live(ctx, child_branch)) {
			fyai_tool_submit_error_set(ctx,
				"tool error: the sub-agent named '%s' is still "
				"running; wait for its report or choose a "
				"different name",
				fy_castp(&agent_name, "agent"));
			rc = -1;
		}
		fyai_tool_submit_check(ctx, !rc, err,
				       "could not name the sub-agent branch");
		have_branch = true;
		if (fyai_agent_isolated(ctx, args)) {
			rc = fyai_agent_view_begin(ctx, fy_castp(&agent_name, "agent"),
						   &view_run);
			fyai_tool_submit_check(ctx, !rc, err,
					       "could not isolate the sub-agent");
		}
	}

	/* A named session must explicitly request a terminal. */
	if (fyai_shell_named_call(ctx, tool_call) &&
	    !fyai_shell_tty_requested(ctx, args)) {
		session_call = fy_get(args, "name", fy_invalid);
		fyai_tool_submit_error_set(ctx,
			"tool error: a shell that stays open needs a terminal; "
			"add \"tty\": true to keep '%s' open, or drop \"name\" "
			"to run the command to completion and read what it "
			"wrote", fy_castp(&session_call, ""));
		fyai_tool_submit_check(ctx, false, err,
				       "a session was asked for with no terminal");
	}

	/* Reserve and validate the session name before spawning its job. */
	if (fyai_shell_session_call(ctx, tool_call)) {
		session_call = fy_get(args, "name", fy_invalid);
		asked = fy_castp(&session_call, "");
		if (!fyai_shell_session_name_valid(asked))
			fyai_tool_submit_error_set(ctx,
				"tool error: '%s' is not a usable session name; "
				"use letters, digits, '-' or '_'", asked);
		else if (fyai_shell_session_find(ctx, asked))
			fyai_tool_submit_error_set(ctx,
				"tool error: a shell named '%s' is already open "
				"on branch %s; write to it or choose another name",
				asked, fyai_ctx_branch(ctx));
		else
			/* Own the name: the generic it came from is transient. */
			session_name = strdup(asked);
		fyai_tool_submit_check(ctx,
				       session_name && !ctx->tool_submit_error,
				       err,
				       "could not name the terminal session");
		have_session = true;
	}

	/*
	 * A forked sub-agent would keep the address space of its parent, and it
	 * could not be told from the parent by the transport. With credential
	 * isolation it must be an executed child.
	 */
	if (ctx->transport.client && fy_equal(name, "agent") && !fyai_agent_spawn_exec(ctx))
		fyai_tool_submit_error_set(ctx,
			"tool error: with credential isolation a sub-agent must "
			"execute fyai again: use agent/spawn exec with a saved "
			"arena and a root that is not pinned");
	fyai_tool_submit_check(ctx, !ctx->tool_submit_error, err,
			       "a sub-agent cannot be forked with credential isolation");

	fyai_diag_tracef("tool", "%s start", name);
	/* The first call of an exchange makes the scope that its calls join. */
	if (!fy_equal(name, "agent")) {
		fyai_home_ensure(ctx);
		fyai_scope_ensure(ctx);
	}

	job = calloc(1, sizeof(*job));
	fyai_error_check(ctx, job, err,
			 "could not allocate tool job");
	/* A sub-agent renders to a terminal of its own; the parent shows it. */
	rc = fyai_tool_job_spawn(ctx, job, fy_equal(name, "agent") &&
				 fyai_agents_detail(ctx->agent_execution ? 2 : 1) == 2 &&
				 (fyai_ui_active(ctx) ||
				  fyai_uimodel_consumed(ctx)),
				 fy_equal(name, "agent") &&
				 fyai_agent_spawn_exec(ctx),
				 view_run ? fyai_view_run_spec(view_run) : NULL, fy_equal(name, "agent"));
	fyai_error_check(ctx, !rc, err,
		"could not spawn tool job");
	job->view_run = view_run;
	view_run = NULL;
	job->agent = fy_equal(name, "agent");
		fyai_tool_job_link(ctx, job);
	if (have_session) {
		user_owned = fy_get(args, "_fyai_user_owned", false);
		fyai_shell_tty_size(ctx, args, &srows, &scols);
		/* A screen drawn in the transcript has the rows of the setting
		 * and the width of the transcript, unless the call asks. */
		if (!user_owned && fyai_ui_tools_inline(ctx)) {
			if (fy_get(args, "rows", 0LL) <= 0)
				srows = ctx->cfg->inline_terminal_rows;
			if (fy_get(args, "cols", 0LL) <= 0)
				scols = fyai_shell_inline_cols(ctx);
		}
		session_command = fy_get(args, "command", fy_invalid);
		session_desc = fy_get(args, "description", fy_invalid);
		session_title = fyai_format_shell_label(args);
		job->session = fyai_shell_session_create(ctx, session_name,
					fy_castp(&session_command, ""),
					session_title,
					fy_castp(&session_desc, ""),
					srows, scols,
					user_owned ? 0 : fyai_shell_output_bytes(ctx, args),
					!fyai_shell_tty_requested(ctx, args),
					user_owned, NULL, NULL);
		free(session_title);
		session_title = NULL;
		fyai_error_check(ctx, job->session, err,
				 "could not open the terminal session");
		job->session->job = job;
		fyai_shell_session_start_size(job, args);
		/* The session copied the name it was reserved under. */
		free(session_name);
		session_name = NULL;
	}
	/* A child that asked for a terminal must follow the window of the
	 * user, and only the parent can see it change. */
	if (fyai_shell_tty_requested(ctx, native_call ?
				     fy_get(tool_call, "action") : args))
		(void)fyai_terminal_winch_open(ctx);
	/* The spawn clears the job, so the name is carried in a local. */
	if (have_branch) {
		job->branch = strdup(child_branch);
		fyai_error_check(ctx, job->branch, err,
				 "out of memory naming the sub-agent branch");
	}
	/* The call outlives the turn that made it. */
	if (fy_get(args, "_fyai_btw", false) ||
	    fy_get(args, "_fyai_background", false)) {
		job->call_gb = fy_generic_builder_create(&call_cfg);
		fyai_error_check(ctx, job->call_gb, err,
				 "could not retain side question call");
		job->call = fy_gb_internalize(job->call_gb, tool_call);
		fyai_error_check(ctx, fy_is_valid(job->call), err,
				 "could not retain side question call");
	} else {
		job->call = tool_call;
	}
	job->btw = fy_get(args, "_fyai_btw", false);
	monitor_v = fy_get(args, "_fyai_monitor", fy_invalid);
	if (fy_is_string(monitor_v)) {
		job->monitor = strdup(fy_castp(&monitor_v, ""));
		fyai_error_check(ctx, job->monitor, err,
				 "out of memory naming the monitor");
	}
	job->native_shell = native_call;
	/* A zeroed generic decodes as an empty sequence, not as invalid. */
	job->diag = fy_invalid;
	/* Save the identity that the parent adds to child diagnostics. */
	if (job->branch) {
		job->origin = strdup(job->branch);
		rc = job->origin ? 0 : -1;
	} else {
		rc = asprintf(&job->origin, "%s %s", fyai_ctx_branch(ctx), name);
	}
	fyai_error_check(ctx, rc >= 0 && job->origin, err,
			 "out of memory naming the tool job");
	/* The trace pairs with the reap record below: a child that dies leaves
	 * these two lines and nothing else. */
	fyai_diag_tracef("spawn", "%s, pid %ld", job->origin, (long)job->pid);
	/* Stream slow shell work on the sub-agent terminal. */
	if (fy_equal(name, "shell") && !fyai_ui_active(ctx) &&
	    ctx->cfg->agent_pty) {
		/* Native shell commands live in the action object. */
		cmdtext = native_call ?
			fy_cast(fy_get_at_path(tool_call, "action", "commands",
					       0), "") :
			fy_get(args, "command", "");
		if (!fyai_shell_view(ctx, *cmdtext ? cmdtext : name, args,
				     &label, &view))
			fyai_print_tool_view(ctx, label, &view);
		free(label);
		free(view.data);
		if (!fyai_fenced_stream_start(&job->stream, ctx, ctx->cfg,
				NULL, ctx->cfg->tool_preview_lines > 0 ?
				(size_t)ctx->cfg->tool_preview_lines : 0,
				markdown_tool_output_indent(ctx->cfg),
				stderr, true)) {
			/*
			 * The surface that the parent shows already carries the state of this call.
			 * A second mark here has no animation.
			 */
			fyai_fenced_stream_clear_indicator(&job->stream);
			job->band_progress = true;
		}
	}
	/* A worker with no display has bands while a client reads it. */
	if (fy_any_equal(name, "shell", "agent") &&
	    fyai_sink_bands_available(ctx->sink)) {
		if (fy_equal(name, "agent")) {
			if (job->btw) {
				job->title = strdup("**btw**\n");
			} else {
				command = fy_get(args, "description", "");
				job->title = fyai_format_tool_header(ctx, "agent",
					fy_mapping("name", fy_get(args, "name", ""),
						   "description",
						   *command ? command : name), 0);
			}
		} else {
			command = native_call ?
				fy_cast(fy_get_at_path(tool_call, "action",
						       "commands", 0), "") :
				fy_get(args, "command", "");
			job->title = fyai_format_shell_label(
					native_call ? fy_invalid : args);
			job->command = strdup(*command ? command : name);
			fyai_error_check(ctx, job->command, err,
					 "out of memory formatting shell progress");
			job->band_progress = true;
		}
		/* Agents use their surface; shells continue to use a work band.
		 * Without a display, an agent reports its progress in a band. */
		/* Without a display a sub-agent with a terminal gives its
		 * screen to a remote client; with none, its progress. */
		if (job->agent && !fyai_ui_active(ctx) && job->pty < 0)
			job->band_progress = true;
		else if (job->agent) {
			if (fyai_agent_view_open(ctx, job))
				fyai_error(ctx,
					   "agent: could not show the sub-agent terminal");
			goto live_open_done;
		}
		/* A terminal session displays output on its own surface. */
		if (have_session)
			goto live_open_done;
		el = fyai_ctx_loop(ctx);
		/* A call drawn in the transcript grows in place: it has no
		 * jump of the page to hide. */
		if (ctx->cfg->work_open_delay_ms > 0 && el &&
		    !fyai_ui_tools_inline(ctx) &&
		    !fyai_event_add_timer(el, ctx->cfg->work_open_delay_ms, 0,
					 fyai_tool_job_band_delay, job,
					 &job->band_delay))
			goto live_open_done;
		fyai_tool_job_band_open(job);
	}
live_open_done:
	rc = fyai_tool_job_attach(ctx, job);
	fyai_error_check(ctx, !rc, err,
			 "could not attach tool job to event loop");

	job->timeout_ms = fyai_tool_job_timeout_ms(ctx, name, native_call, args);
	fyai_diag_tracef("tool", "%s limit %u ms", name, job->timeout_ms);
	job->started_ms = fyai_event_now_ms();
	if (job->timeout_ms) {
		el = fyai_ctx_loop(ctx);
		assert(el);
		rc = fyai_event_add_timer(el, job->timeout_ms, 0,
					  fyai_tool_job_deadline, job,
					  &job->deadline);
		fyai_error_check(ctx, !rc, err,
				 "could not arm the tool job time limit");
	}
	if (fy_equal(name, "agent") && ctx->cfg->agent_timeout_kill &&
	    ctx->cfg->agent_hang_timeout_ms > 0) {
		job->hang_timeout_ms = ctx->cfg->agent_hang_timeout_ms;
		el = fyai_ctx_loop(ctx);
		assert(el);
		/* With no advisory limit, the hang limit starts at submission. */
		rc = fyai_event_add_timer(el,
			(fyai_event_ms_t)job->timeout_ms + job->hang_timeout_ms,
			0, fyai_tool_job_hang_deadline, job,
			&job->hang_deadline);
		fyai_error_check(ctx, !rc, err,
				 "could not arm the sub-agent hang limit");
	}
	return job;

err:
	free(session_name);
	fyai_view_run_free(view_run);
	fyai_tool_job_discard(job);
	return NULL;
}

/*
 * Keep the end of the live output from a time-limited job. The deadline can
 * stop a job before it reports a result. In this case, progress notifications
 * are the only command output. The end usually contains the failure cause.
 */
#define FYAI_TOOL_PROGRESS_TAIL	8192

static void fyai_tool_job_progress_retain(struct fyai_tool_job *job,
					  const char *p, size_t len)
{
	struct fyai_buf *buf = &job->progress;
	int rc;

	rc = fyai_buf_reserve(buf, buf->len + len + 1);
	if (rc)
		return;
	memcpy(buf->data + buf->len, p, len);
	buf->len += len;
	buf->data[buf->len] = '\0';
	if (buf->len <= FYAI_TOOL_PROGRESS_TAIL)
		return;
	memmove(buf->data, buf->data + buf->len - FYAI_TOOL_PROGRESS_TAIL,
		FYAI_TOOL_PROGRESS_TAIL);
	buf->len = FYAI_TOOL_PROGRESS_TAIL;
	buf->data[buf->len] = '\0';
}

/*
 * Put a tool call of a delegated sub-agent before the gate of this process.
 * With no gate, the call runs as it would here.
 */
static fy_generic fyai_tool_job_gate(struct fyai_tool_job *job,
				     struct jsonrpc_conn *conn, fy_generic id,
				     fy_generic params)
{
	struct fyai_ctx *ctx = job->ctx;
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	bool after;
	int rc;

	after = fy_equal(fy_get(params, "phase", fy_invalid), "after");
	if (!ctx->tool_gate)
		return after ? fy_mapping(gb, "stop", false) :
			       fy_mapping(gb, "allow", true);
	/* The gate can answer during the relay. */
	jsonrpc_conn_defer(conn);
	rc = fyai_tool_gate_relay(ctx, job->branch, fy_cast(id, -1LL), params);
	if (!rc)
		return fy_invalid;
	rc = jsonrpc_conn_respond(conn, id, after ?
		fy_mapping(gb, "stop", false) :
		fy_mapping(gb, "allow", false,
			   "message", "the tool call could not be decided",
			   "stop", true), fy_invalid);
	if (rc)
		fyai_warning(ctx, "could not answer the tool call of the "
			     "sub-agent '%s'", fyai_agent_job_name(job));
	return fy_invalid;
}

/* The child's tool/progress notifications: its live output. */
static fy_generic fyai_tool_job_serve(struct jsonrpc_conn *conn,
				      const char *method, fy_generic params,
				      fy_generic id, void *userdata,
				      fy_generic *errorp)
{
	struct fyai_tool_job *job = userdata;
	fy_generic text;
	const char *p;
	char *bytes;
	size_t len, n;

	if (fyai_agents_serve(job->ctx, conn, method, params, id, &text, errorp))
		return text;
	if (job->agent && fy_is_valid(id) && !strcmp(method, "tool/gate"))
		return fyai_tool_job_gate(job, conn, id, params);
	if (job->agent && !strcmp(method, "turn/step")) {
		if (job->ctx->agent_observe)
			job->ctx->agent_observe(job->ctx, job->call, params,
						job->ctx->agent_observe_data);
		return fy_invalid;
	}
	/* Handle the question request a delegated child sends to its parent. */
	if (fy_is_valid(id)) {
		if (strcmp(method, "user/ask"))
			return fy_invalid;
		return fyai_ask_user_for_child(job, conn, id, params);
	}

	/* A session sends the bytes of its terminal; the parent renders. */
	if (!strcmp(method, "shell/output")) {
		if (!job->session)
			return fy_invalid;
		bytes = fyai_bytes_from_generic(params, &n);
		if (bytes) {
			/* It wrote: whatever it waited for, it has it. */
			job->session->wants_input = false;
			/* It is drawing, thus it is not quiet. */
			job->session->quiet_since = fyai_event_now_ms();
			job->session->drew = true;
			fyai_terminal_tap(job->ctx, job->session->view,
					  &job->session->model_text,
					  job->session->name, bytes, n);
			(void)fyai_terminal_view_feed(job->session->view,
						      bytes, n);
			fyai_shell_session_refresh(job->session);
		}
		free(bytes);
		return fy_invalid;
	}
	if (!strcmp(method, "tty/resized")) {
		if (job->session)
			fyai_shell_session_resized(job->session,
				(int)fy_get(params, "rows", 0LL),
				(int)fy_get(params, "cols", 0LL));
		return fy_invalid;
	}
	/* Watch the child-reported process for input waits. */
	if (!strcmp(method, "shell/started")) {
		if (job->session)
			fyai_shell_session_watch(job->session,
					(pid_t)fy_get(params, "pid", 0LL));
		return fy_invalid;
	}
	if (!strcmp(method, "shell/exit")) {
		/* What the program left is tracked before the session lets go of the scope. */
		fyai_orphan_track(job->ctx, fy_get(params, "detached", fy_invalid));
		if (job->session)
			fyai_shell_session_exited(job->session,
				(int)fy_get(params, "exit_code", 0LL),
				(int)fy_get(params, "signal", 0LL));
		return fy_invalid;
	}
	if (strcmp(method, "tool/progress"))
		return fy_invalid;
	text = fy_get(params, "text", fy_invalid);
	if (!fy_is_string(text))
		return fy_invalid;
	p = fy_castp(&text, "");
	len = strlen(p);
	if (data_is_binary(p, len))
		return fy_invalid;
	if (job->monitor)
		fyai_monitor_output(job->ctx, job->monitor, p, len);
	if (!job->agent && job->timeout_ms)
		fyai_tool_job_progress_retain(job, p, len);
	if (job->band_progress && job->stream.active)
		(void)fyai_fenced_stream_push(&job->stream, p, len);
	else if (job->band_delay &&
		 !fyai_buf_reserve(&job->pending_output,
					  job->pending_output.len + len + 1)) {
		memcpy(job->pending_output.data + job->pending_output.len, p, len);
		job->pending_output.len += len;
		job->pending_output.data[job->pending_output.len] = '\0';
	}
	else if (!job->agent && job->ctx->shell_stream &&
		 job->ctx->shell_stream->active)
		/* Stream command output into the sub-agent's live shell region. */
		(void)fyai_fenced_stream_push(job->ctx->shell_stream, p, len);
	return fy_invalid;
}

static void fyai_tool_job_run_done(struct jsonrpc_request *req, void *userdata)
{
	struct fyai_tool_job *job = userdata;

	job->phase = FYAI_JOB_ANSWERED;
	if (jsonrpc_request_ok(req)) {
		fy_generic r = jsonrpc_request_result(req);

		job->result = fy_get(r, "result", fy_invalid);
		fyai_tool_job_verdict_set(job, fy_get(r, "ok", false));
		job->display = fy_get(r, "display", fy_invalid);
		job->diag = fy_get(r, "diag", fy_invalid);
		/* The scope keeps what the call left running. */
		fyai_orphan_track(job->ctx, fy_get(r, "detached", fy_invalid));
		if (!strcmp(job->ctx->cfg->sandbox_scope, "call"))
			fyai_orphan_sweep(job->ctx);
		/*
		 * The reply is in the transient builder, which the turn
		 * releases when it ends. A retained call is collected later,
		 * so its reply moves to the builder that the job owns.
		 */
		if (job->call_gb) {
			job->result = fy_gb_internalize(job->call_gb,
							job->result);
			job->display = fy_gb_internalize(job->call_gb,
							 job->display);
			job->diag = fy_gb_internalize(job->call_gb, job->diag);
		}
		if (fy_is_string(job->display))
			fyai_patch_display_record(job->ctx, job->call,
					fy_castp(&job->display, ""));
		job->have_result = true;
	} else {
		job->verdict = FYAI_JOB_CHANNEL_FAILED;
	}
	fyai_tool_job_settle(job);
}

static int fyai_tool_job_attach(struct fyai_ctx *ctx,
				struct fyai_tool_job *job)
{
	struct fyai_event_loop *el;
	struct fy_generic_builder *gb;
	fy_generic params;
	int rc;

	fyai_error_check(ctx, job, err,
			 "cannot attach an empty tool job");
	el = fyai_ctx_loop(ctx);
	fyai_error_check(ctx, el, err,
			 "tool job requires an event loop");
	gb = fyai_ctx_transient_gb(ctx);
	fyai_error_check(ctx, gb, err,
			 "tool job requires transient storage");

	job->conn = jsonrpc_conn_stdio(ctx, job->pfd, job->rfd, 0,
				       "tool", NULL);
	fyai_error_check(ctx, job->conn, err,
			 "could not open the tool control channel");
	rc = jsonrpc_conn_serve(job->conn, fyai_tool_job_serve, job);
	fyai_error_check(ctx, !rc, err,
			 "could not serve the tool control channel");

	job->phase = FYAI_JOB_RUNNING;
	params = !fy_str_empty(job->branch) ?
		fy_gb_mapping(gb, "call", job->call, "branch", job->branch) :
		fy_gb_mapping(gb, "call", job->call);
	if (job->exec)
		params = fy_assoc(gb, params, fy_value(gb, "spawn"),
				     fyai_agent_spawn_state(job->ctx, gb, job->transport_exec));
	/* A sub-agent asks the gate of this process, and reports its steps. */
	if (job->agent && ctx->tool_gate)
		params = fy_assoc(gb, params, "gate", true);
	if (job->agent && ctx->agent_observe)
		params = fy_assoc(gb, params, "steps", true);
	if (job->start_cols > 0)
		params = fy_assoc(gb, params, fy_value(gb, "size"),
				  fy_gb_mapping(gb,
					"rows", (long long)job->start_rows,
					"cols", (long long)job->start_cols));
	fyai_error_check(ctx, fy_is_mapping(params), err,
			 "could not build the tool call request");
	job->run = jsonrpc_request_submit(job->conn, "tool/run", params,
					  jsonrpc_conn_next_id(job->conn),
					  false, fyai_tool_job_run_done, job);
	fyai_error_check(ctx, job->run, err,
			 "could not dispatch the tool call");

	rc = fyai_event_add_child(el, job->pid, fyai_tool_job_child,
				  job, &job->csrc);
	fyai_error_check(ctx, !rc, err,
			 "could not attach tool process");
	return 0;

err:
	if (job)
		job->verdict = FYAI_JOB_CHANNEL_FAILED;
	return -1;
}

bool fyai_tool_job_done(const struct fyai_tool_job *job)
{
	return job && job->phase == FYAI_JOB_DONE;
}

bool fyai_tools_active(const struct fyai_ctx *ctx)
{
	const struct fyai_tool_job *job;
	const struct fyai_shell_session *session;

	for (job = ctx->tool_jobs; job; job = job->next)
		if (job->phase != FYAI_JOB_DONE)
			return true;
	for (session = ctx->shell_sessions; session; session = session->next)
		if (!session->exited)
			return true;
	return false;
}

void fyai_tool_job_cancel(struct fyai_tool_job *job)
{
	struct fyai_event_loop *el;
	int rc;

	if (!job || job->reaped || job->pid <= 0 || job->terminating)
		return;
	job->terminating = true;
	jsonrpc_conn_expect_close(job->conn);

	el = fyai_ctx_loop(job->ctx);
	if (el) {
		fyai_tool_job_drop(&job->csrc);
		rc = fyai_event_add_child_terminate_group(el, job->pid, 0,
							  FYAI_TOOL_TERM_MS,
							  fyai_tool_job_child,
							  job, &job->csrc);
		if (!rc)
			return;
		/* Watch the child without staged termination. */
		(void)fyai_event_add_child(el, job->pid, fyai_tool_job_child,
					   job, &job->csrc);
	}
	rc = kill(-job->pid, SIGTERM);
	if (rc && errno == ESRCH)
		(void)kill(job->pid, SIGTERM);
}

/* Mark an overdue agent; shell limits still terminate immediately. */
static enum fyai_event_action
fyai_tool_job_deadline(const struct fyai_event *ev)
{
	struct fyai_tool_job *job = ev->userdata;

	job->deadline = NULL;
	if (job->agent) {
		job->overdue = true;
		if (job->surface)
			fyai_agent_head_paint(job);
		return FYAIEA_CONTINUE;
	}
	job->timed_out = true;
	fyai_tool_job_cancel(job);
	return FYAIEA_CONTINUE;
}

static enum fyai_event_action
fyai_tool_job_hang_deadline(const struct fyai_event *ev)
{
	struct fyai_tool_job *job = ev->userdata;

	job->hang_deadline = NULL;
	job->timed_out = true;
	fyai_tool_job_cancel(job);
	return FYAIEA_CONTINUE;
}

/* Return the time limit for a shell or agent job. */
static unsigned int fyai_tool_job_timeout_ms(struct fyai_ctx *ctx,
					     const char *name, bool native_call,
					     fy_generic args)
{
	fy_generic persona_name, personas, persona;
	long long ms;

	if (native_call || fy_equal(name, "shell"))
		return fyai_shell_timeout_ms(ctx, args, native_call);
	if (fy_equal(name, "agent")) {
		/*
		 * Use the first available limit in this order: the call, the
		 * persona, and the global sub-agent setting. Only the call
		 * contains an untrusted value. Apply the maximum only to this
		 * value.
		 */
		ms = fy_get(args, "timeout", 0LL);
		if (ms > 0 && ctx->cfg->agent_max_timeout_ms > 0 &&
		    ms > ctx->cfg->agent_max_timeout_ms)
			ms = ctx->cfg->agent_max_timeout_ms;
		persona_name = fy_get(args, "persona", fy_invalid);
		personas = fy_get(fy_get(ctx->cfg->config_doc, "agent"),
				  "personas", fy_invalid);
		persona = fy_get(personas, persona_name, fy_invalid);
		if (ms <= 0)
			ms = fy_get(persona, "timeout_ms", 0LL);
		if (ms <= 0)
			ms = ctx->cfg->agent_timeout_ms;
		return ms > 0 ? (unsigned int)ms : 0;
	}
	return 0;
}

/*
 * Return the length of @s without a final "tool error: interrupted" report.
 *
 * A job sees the group termination from a parent deadline as an interrupt.
 * The parent knows that a timeout occurred and removes this incorrect report.
 * Remove only a final report. The same text at an earlier position is command
 * output.
 */
static size_t fyai_tool_drop_interrupt(const char *s)
{
	static const char intr[] = "tool error: interrupted";
	const char *p, *last;
	size_t len;

	len = strlen(s);
	last = NULL;
	for (p = strstr(s, intr); p; p = strstr(p + 1, intr))
		last = p;
	if (!last)
		return len;
	for (p = last + sizeof(intr) - 1; *p; p++)
		if (!isspace((unsigned char)*p))
			return len;
	len = (size_t)(last - s);
	while (len && isspace((unsigned char)s[len - 1]))
		len--;
	return len;
}

/*
 * Report an expired native-shell limit in stderr for the next model request.
 */
#define FYAI_SHELL_TIMEOUT_NOTE \
	"tool error: command timed out after %u ms; request a larger " \
	"timeout_ms (up to %u ms) if the command needs longer"
#define FYAI_SHELL_TIMEOUT_NOTE_NOMAX \
	"tool error: command timed out after %u ms; request a larger " \
	"timeout_ms if the command needs longer"

/*
 * Change the outcome of a native shell result after a parent deadline.
 * The child sees group termination only as a signal and cannot identify the
 * timeout. The parent identifies the timeout and keeps all captured output.
 */
static fy_generic fyai_shell_result_retime(struct fyai_ctx *ctx,
					   fy_generic result,
					   unsigned int timeout_ms,
					   const char *note)
{
	fy_generic outputs = fy_seq_empty;
	fy_generic entry, outcome;
	fy_generic gerr;
	const char *err;

	if (!fy_is_sequence(result))
		return result;
	fy_foreach(entry, result) {
		outcome = fy_get(entry, "outcome");
		if (fy_equal(fy_get(outcome, "type"), "signal")) {
			gerr = fy_get(entry, "stderr");
			err = fy_castp(&gerr, "");
			entry = fy_mapping(
				"stdout", fy_get(entry, "stdout", ""),
				"stderr", fy_stringf(ctx->transient_gb,
						     "%s%s%s", err,
						     *err ? "\n" : "", note),
				"outcome", fy_mapping(
					"type", "timeout",
					"timeout_ms",
					(long long)timeout_ms));
		}
		outputs = fy_append(outputs, entry);
	}
	return fy_gb_internalize(ctx->transient_gb, outputs);
}

fy_generic fyai_tool_job_collect(struct fyai_ctx *ctx,
				 struct fyai_tool_job *job, bool *okp)
{
	const char *reason;
	const char *note;
	const char *captured;
	fy_generic result;
	fy_generic out;
	char *cause;
	size_t keep;
	bool genuine;
	int status;
	pid_t rc;

	if (!job)
		return fy_invalid;
	if (job->phase != FYAI_JOB_DONE) {
		*okp = false;
		return fy_invalid;
	}
	/* A started session keeps its process after this call completes. */
	if (job->session) {
		fyai_tool_job_live_close(job, false);
		fyai_tool_job_drop(&job->deadline);
		fyai_tool_job_drop(&job->hang_deadline);
		job->group = NULL;
		if (fy_is_valid(job->diag))
			fyai_diag_adopt(fyai_ctx_diag(ctx), job->diag,
					job->origin);
		*okp = job->verdict == FYAI_JOB_OK;
		if (!*okp) {
			/* Quote the cause without consuming the diagnostic. */
			cause = fyai_diag_string(fyai_ctx_diag(ctx));
			out = fy_stringf(ctx->transient_gb,
				"tool error: the terminal session could not be "
				"opened: %s", cause && *cause ? cause :
				"no reason was recorded");

			free(cause);
			return out;
		}
		return fy_stringf(ctx->transient_gb,
			"[terminal session '%s' started: %s]\n"
			"Read it with shell_output, drive it with shell_input, "
			"end it with shell_close.", job->session->name,
			job->session->command);
	}
	if (!job->btw_panel)
		fyai_tool_job_live_close(job, false);
	fyai_tool_job_close_channel(job);
	fyai_tool_job_drop(&job->deadline);
	fyai_tool_job_drop(&job->hang_deadline);
	fyai_tool_job_drop(&job->csrc);
	if (!job->reaped && job->pid > 0) {
		do {
			rc = waitpid(job->pid, &status, 0);
		} while (rc < 0 && errno == EINTR);
		if (rc == job->pid) {
			job->reaped = true;
			/* The answer stands; see fyai_tool_job_child(). */
			if (!job->have_result)
				fyai_tool_job_verdict_set(job,
						WIFEXITED(status) &&
						WEXITSTATUS(status) == 0);
			job->term_signal = WIFSIGNALED(status) ?
						WTERMSIG(status) : 0;
			job->view_entry_failed = job->view_run && !job->have_result &&
				WIFEXITED(status) &&
				WEXITSTATUS(status) == FYAI_SHELL_EXIT_SANDBOX;
		}
	}
	/* Adopt diagnostics before reporting a missing child result. */
	fyai_diag_tracef("reap", "%s, pid %ld: %s%d%s",
			 job->origin ? job->origin : "tool", (long)job->pid,
			 job->term_signal ? "signal " : "exit ",
			 job->term_signal ? job->term_signal :
			 job->verdict == FYAI_JOB_OK ? 0 : 1,
			 job->timed_out ? ", timed out" :
			 (job->have_result ? "" : ", no result"));
	if (fy_is_valid(job->diag)) {
		if (job->agent)
			fyai_diag_adopt_agent(fyai_ctx_diag(ctx), job->diag,
					      job->origin);
		else
			fyai_diag_adopt(fyai_ctx_diag(ctx), job->diag,
					    job->origin);
	}
	/* Supply a cause when a failed child returned no diagnostic. */
	if (!job->timed_out && job->term_signal) {
		/* A job we stopped ended as we asked it to; that is not a
		 * failure to report, only detail for whoever is debugging. */
		fyai_diag_type(fyai_ctx_diag(ctx),
			       job->terminating ? FYAIET_DEBUG : FYAIET_ERROR,
			       "[%s] terminated by signal %d",
			       job->origin ? job->origin : "tool",
			       job->term_signal);
	} else if (job->view_entry_failed) {
		fyai_error(ctx, "[%s] could not enter its view: the mount setup "
			   "failed; run it without isolation",
			   job->origin ? job->origin : "agent");
	} else if (!job->timed_out && !job->have_result) {
		fyai_error(ctx, "[%s] ended without a result%s",
			   job->origin ? job->origin : "tool",
			   job->verdict == FYAI_JOB_CHANNEL_FAILED ?
				" (the control channel failed)" : "");
	}
	if (job->term_signal || !job->have_result)
		result = fy_invalid;
	else
		result = job->result;
	if (fy_is_invalid(result) && job->term_signal) {
		if (job->native_shell)
			result = fy_gb_internalize(ctx->transient_gb,
				fy_sequence(fy_mapping(
					"stdout", "",
					"stderr", "",
					"outcome", fy_mapping(
						"type", "signal",
						"signal", job->term_signal))));
		else
			result = fy_value(ctx->transient_gb,
					  "tool error: interrupted");
	}
	/* The parent knows whether its time limit expired. */
	if (job->timed_out) {
		/*
		 * A job that reports before termination has a complete result.
		 * Otherwise, use the retained end of the progress output.
		 */
		genuine = job->have_result && !job->term_signal;
		captured = job->progress.len ? job->progress.data : "";
		if (job->agent && job->timeout_ms)
			reason = fy_sprintfa("tool error: agent hang timeout "
				"after %u ms (%u ms advisory limit and %u ms grace)",
				job->timeout_ms + job->hang_timeout_ms,
				job->timeout_ms, job->hang_timeout_ms);
		else if (job->agent)
			reason = fy_sprintfa("tool error: agent hang timeout "
				"after %u ms", job->hang_timeout_ms);
		else
			reason = fy_sprintfa("tool error: timed out after %u ms",
				job->timeout_ms);
		if (ctx->cfg->shell_max_timeout_ms > 0)
			note = fy_sprintfa(FYAI_SHELL_TIMEOUT_NOTE,
					      job->timeout_ms,
					      (unsigned int)
					      ctx->cfg->shell_max_timeout_ms);
		else
			note = fy_sprintfa(FYAI_SHELL_TIMEOUT_NOTE_NOMAX,
					      job->timeout_ms);
		if (!job->native_shell) {
			assert(fy_is_string(result));
			if (genuine)
				captured = fy_castp(&result, "");
			keep = fyai_tool_drop_interrupt(captured);
			result = fy_gb_internalize(ctx->transient_gb,
				fy_stringf("%.*s%s%s", (int)keep, captured,
					   keep ? "\n" : "", reason));
		} else if (genuine) {
			result = fyai_shell_result_retime(ctx, result,
							  job->timeout_ms,
							  note);
		} else {
			/*
			 * Keep the native result shape after a timeout. The model
			 * always receives a sequence of {stdout, stderr, outcome}.
			 */
			result = fy_gb_internalize(ctx->transient_gb,
				fy_sequence(fy_mapping(
					"stdout", captured,
					"stderr", note,
					"outcome", fy_mapping(
						"type", "timeout",
						"timeout_ms",
						(long long)job->timeout_ms))));
		}
		fyai_tool_job_verdict_set(job, false);
	}
	if (job->agent && job->overdue && !job->timed_out) {
		fyai_notice(ctx, "[%s] exceeded the %u ms advisory limit; "
			    "completed after %lld ms",
			    job->origin ? job->origin : "agent", job->timeout_ms,
			    (long long)job->elapsed_ms);
		if (fy_is_string(result))
			result = fy_stringf(ctx->transient_gb, "%s\n\n"
				"[Agent exceeded its %u ms advisory limit; "
				"completed after %lld ms.]",
				fy_castp(&result, ""), job->timeout_ms,
				(long long)job->elapsed_ms);
	}
	if (job->view_run)
		fyai_agent_view_finish(ctx, job, &result);
	*okp = job->verdict == FYAI_JOB_OK;
	if (job->btw_panel) {
		job->group = NULL;
		return result;
	}
	/* The result lives in the builder of the job, which goes with it. */
	if (job->call_gb && fy_is_valid(result))
		result = fy_gb_internalize(fyai_ctx_transient_gb(ctx), result);
	fyai_tool_job_unlink(job->ctx, job);
	free(job->progress.data);
	free(job->branch);
	free(job->monitor);
	free(job->origin);
	fy_generic_builder_destroy(job->call_gb);
	free(job);
	return result;
}

static void
fyai_tool_job_group_mcp_complete(struct fyai_mcp_call_request *request,
				 void *userdata)
{
	struct fyai_tool_job_group *group;

	if (!fyai_mcp_call_done(request))
		return;
	group = userdata;
	fyai_tool_job_group_service(group);
}

static unsigned int
fyai_tool_job_group_animation_interval(struct fyai_tool_job_group *group)
{
	struct fyai_tool_group_entry *entry;
	unsigned int interval;
	size_t i;

	interval = 0;
	for (i = 0; i < group->next; i++) {
		entry = &group->entries[i];
		if (entry->state != FYAITGS_RUNNING || !entry->job ||
		    !entry->job->stream.active ||
		    entry->job->stream.indicator_state !=
			    FYMD_INDICATOR_PENDING)
			continue;
		if (!entry->job->stream.indicator_interval_ms)
			continue;
		if (!interval ||
		    entry->job->stream.indicator_interval_ms < interval)
			interval = entry->job->stream.indicator_interval_ms;
	}
	return interval;
}

static enum fyai_event_action
fyai_tool_job_group_animation_cb(const struct fyai_event *ev)
{
	struct fyai_tool_job_group *group;

	group = ev->userdata;
	fyai_tool_job_group_service(group);
	return FYAIEA_CONTINUE;
}

static void
fyai_tool_job_group_animation_sync(struct fyai_tool_job_group *group)
{
	unsigned int interval;
	int rc;

	interval = fyai_tool_job_group_animation_interval(group);
	if (!interval) {
		fyai_event_source_remove(group->animation_timer);
		group->animation_timer = NULL;
		return;
	}
	if (group->animation_timer)
		return;
	rc = fyai_event_add_timer(fyai_ctx_loop(group->ctx), interval,
				  interval,
				  fyai_tool_job_group_animation_cb, group,
				  &group->animation_timer);
	if (rc)
		fyai_warning(group->ctx,
			     "tool indicator animation timer could not start");
}

static int fyai_tool_job_group_reserve(struct fyai_tool_job_group *group)
{
	struct fyai_tool_group_entry *entries;
	size_t capacity;

	if (group->count < group->capacity)
		return 0;
	capacity = group->capacity ? group->capacity * 2 : 8;
	entries = realloc(group->entries, capacity * sizeof(*entries));
	if (!entries)
		return -1;
	memset(entries + group->capacity, 0,
	       (capacity - group->capacity) * sizeof(*entries));
	group->entries = entries;
	group->capacity = capacity;
	return 0;
}

struct fyai_tool_job_group *
fyai_tool_job_group_create_notify(struct fyai_ctx *ctx,
				  fyai_tool_group_complete_fn complete,
				  void *userdata)
{
	struct fyai_tool_job_group *group;

	if (!ctx)
		return NULL;
	group = calloc(1, sizeof(*group));
	if (!group)
		return NULL;
	group->ctx = ctx;
	group->complete = complete;
	group->userdata = userdata;
	group->max_parallel = ctx->cfg->parallel_tool_calls ? 16 : 1;
	return group;
}

struct fyai_tool_job_group *
fyai_tool_job_group_create_open(struct fyai_ctx *ctx,
				fyai_tool_group_complete_fn complete,
				void *userdata)
{
	struct fyai_tool_job_group *group;

	group = fyai_tool_job_group_create_notify(ctx, complete, userdata);
	if (!group)
		return NULL;
	group->phase = FYAITGP_OPEN;
	return group;
}

int fyai_tool_job_group_add(struct fyai_tool_job_group *group,
			    fy_generic tool_call)
{
	const char *text;
	char *copy;
	bool parallel, instant;
	int rc;
	size_t i;

	if (!group || group->phase >= FYAITGP_SEALED)
		return -1;
	rc = fyai_tool_job_group_reserve(group);
	if (rc)
		return -1;
	parallel = fyai_tool_call_parallel_eligible(group->ctx, tool_call);
	instant = fyai_tool_call_instant(group->ctx, tool_call);
	if (group->phase == FYAITGP_OPEN && !parallel)
		return 1;
	if (group->count && (group->exclusive || !parallel))
		return -1;
	/* The job runs the call that the tool call expands to. */
	tool_call = fyai_tool_call_effective(group->ctx, tool_call);
	text = emit_json_string(group->ctx->transient_gb, tool_call);
	copy = text ? strdup(text) : NULL;
	if (!copy)
		return -1;
	for (i = 0; i < group->count; i++) {
		if (!strcmp(group->entries[i].call_text, copy)) {
			free(copy);
			return 0;
		}
	}
	group->entries[group->count].call_text = copy;
	group->entries[group->count].parallel = parallel && !instant;
	group->entries[group->count].state = FYAITGS_QUEUED;
	if (!parallel) {
		group->exclusive = true;
		group->max_parallel = 1;
	}
	group->count++;
	if (group->phase == FYAITGP_OPEN)
		fyai_tool_job_group_service(group);
	return 0;
}

/* A waiter of the group ended: look at the group again. */
static void fyai_tool_job_group_waiter_done(void *userdata)
{
	fyai_tool_job_group_service(userdata);
}

static void fyai_tool_job_group_dispatch(struct fyai_tool_job_group *group)
{
	struct fyai_tool_group_entry *entry;
	fy_generic call;
	fy_generic args;
	fy_generic result;
	const char *name;
	const char *text;
	char *why;

	while (group->phase != FYAITGP_CANCELLED && group->next < group->count &&
	       group->active < group->max_parallel) {
		entry = &group->entries[group->next++];
		call = parse_json_string(group->ctx->transient_gb,
					 entry->call_text);
		name = fy_is_valid(call) ?
			fyai_tool_call_name(group->ctx, call) : NULL;
		if (name && (fyai_mcp_tool_name(name) ||
			     !strcmp(name, "mcp_resource"))) {
			args = fyai_tool_call_args(group->ctx, call);
			entry->mcp_request =
				fy_is_valid(args) &&
				fyai_skill_tool_allowed(group->ctx, name) ?
				(!strcmp(name, "mcp_resource") ?
				 fyai_mcp_resource_submit(group->ctx, args,
					fyai_tool_job_group_mcp_complete, group) :
				 fyai_mcp_call_submit(group->ctx, name, args,
					fyai_tool_job_group_mcp_complete, group)) :
				NULL;
			if (entry->mcp_request) {
				entry->state = FYAITGS_RUNNING;
				group->active++;
				continue;
			}
			result = fy_value(group->ctx->transient_gb,
				fyai_skill_tool_allowed(group->ctx, name) ?
				"tool error: MCP call failed" :
				"tool error: tool is not allowed by this skill");
			text = emit_json_string(group->ctx->transient_gb,
						result);
			entry->result_text = text ? strdup(text) : NULL;
			entry->state = entry->result_text ?
				FYAITGS_PARKED : FYAITGS_SUBMIT_FAILED;
			group->parked++;
			continue;
		}
		if (entry->parallel && name && !strcmp(name, "wait") &&
		    fyai_wait_for_requested(fyai_tool_call_args(group->ctx,
								  call))) {
			/* A wait for an event runs beside the other calls. */
			why = NULL;
			entry->waiter = fyai_waiter_start(group->ctx,
				fyai_tool_call_args(group->ctx, call),
				fyai_tool_job_group_waiter_done, group, &why);
			if (entry->waiter) {
				entry->state = FYAITGS_RUNNING;
				group->active++;
				/* It can end at once: nothing else parks it. */
				if (fyai_waiter_done(entry->waiter)) {
					entry->state = FYAITGS_PARKED;
					group->active--;
					group->parked++;
				}
				continue;
			}
			result = fy_value(group->ctx->transient_gb,
					  why ? why : "tool error: the wait "
					  "could not start");
			free(why);
			text = emit_json_string(group->ctx->transient_gb,
						result);
			entry->result_text = text ? strdup(text) : NULL;
			entry->state = entry->result_text ?
				FYAITGS_PARKED : FYAITGS_SUBMIT_FAILED;
			group->parked++;
			continue;
		}
		if (!entry->parallel) {
			result = fyai_execute_tool_call(group->ctx, call,
							&entry->result_ok);
			text = fy_is_valid(result) ?
				emit_json_string(group->ctx->transient_gb,
						 result) : NULL;
			entry->result_text = text ? strdup(text) : NULL;
			entry->state = entry->result_text ?
				FYAITGS_PARKED : FYAITGS_SUBMIT_FAILED;
			group->parked++;
			continue;
		}
		entry->job = fy_is_valid(call) ?
			fyai_tool_job_submit(group->ctx, call) : NULL;
		if (!entry->job) {
			/* Return a submission error as the tool result. */
			result = fy_value(group->ctx->transient_gb,
					  fyai_tool_submit_error(group->ctx));
			text = emit_json_string(group->ctx->transient_gb,
						result);
			entry->result_text = text ? strdup(text) : NULL;
			entry->result_ok = false;
			entry->state = entry->result_text ?
				FYAITGS_PARKED : FYAITGS_SUBMIT_FAILED;
			group->parked++;
			continue;
		}
		entry->state = FYAITGS_RUNNING;
		entry->job->group = group;
		group->active++;
	}
}

static void fyai_tool_job_group_notify(struct fyai_tool_job_group *group)
{
	fyai_tool_group_complete_fn complete;
	void *userdata;

	if (group->phase < FYAITGP_SEALED || group->parked != group->count ||
	    group->notified)
		return;
	group->notified = true;
	complete = group->complete;
	userdata = group->userdata;
	if (complete)
		complete(group, userdata);
}

void fyai_tool_job_group_service(struct fyai_tool_job_group *group)
{
	struct fyai_tool_group_entry *entry;
	fyai_event_ms_t now;
	size_t i;

	if (!group || group->phase == FYAITGP_BUILDING)
		return;
	now = fyai_event_now_ms();
	for (i = 0; i < group->next; i++) {
		entry = &group->entries[i];
		if (entry->state != FYAITGS_RUNNING)
			continue;
		if (entry->mcp_request) {
			if (!fyai_mcp_call_done(entry->mcp_request))
				continue;
			entry->state = FYAITGS_PARKED;
			group->active--;
			group->parked++;
			continue;
		}
		if (entry->waiter) {
			if (!fyai_waiter_done(entry->waiter))
				continue;
			entry->state = FYAITGS_PARKED;
			group->active--;
			group->parked++;
			continue;
		}
		(void)fyai_fenced_stream_animate(&entry->job->stream, now);
		if (!fyai_tool_job_done(entry->job))
			continue;
		/* Keep a side answer in its tile until the user dismisses it. */
		entry->job->btw_panel = entry->job->btw && entry->job->surface;
		fyai_tool_job_live_close(entry->job, false);
		entry->job->group = NULL;
		entry->state = FYAITGS_PARKED;
		group->active--;
		group->parked++;
	}
	fyai_tool_job_group_dispatch(group);
	fyai_tool_job_group_animation_sync(group);
	fyai_tool_job_group_notify(group);
}

int fyai_tool_job_group_submit(struct fyai_tool_job_group *group)
{
	if (!group || group->phase != FYAITGP_BUILDING || !group->count)
		return -1;
	group->phase = FYAITGP_SEALED;
	fyai_tool_job_group_dispatch(group);
	fyai_tool_job_group_animation_sync(group);
	fyai_tool_job_group_notify(group);
	return 0;
}

int fyai_tool_job_group_seal(struct fyai_tool_job_group *group)
{
	if (!group || group->phase != FYAITGP_OPEN)
		return -1;
	group->phase = FYAITGP_SEALED;
	fyai_tool_job_group_service(group);
	return 0;
}

bool fyai_tool_job_group_done(const struct fyai_tool_job_group *group)
{
	return group && group->phase >= FYAITGP_SEALED &&
	       group->parked == group->count;
}

size_t fyai_tool_job_group_count(const struct fyai_tool_job_group *group)
{
	return group ? group->count : 0;
}

void fyai_tool_job_group_cancel(struct fyai_tool_job_group *group)
{
	struct fyai_tool_group_entry *entry;
	size_t i;

	if (!group || group->phase == FYAITGP_CANCELLED)
		return;
	group->phase = FYAITGP_CANCELLED;
	for (i = 0; i < group->count; i++) {
		entry = &group->entries[i];
		if (entry->state == FYAITGS_RUNNING && entry->waiter) {
			/* Nothing runs for it: it ends here and parks. */
			fyai_waiter_cancel(entry->waiter);
			entry->state = FYAITGS_PARKED;
			group->active--;
			group->parked++;
		} else if (entry->state == FYAITGS_RUNNING) {
			if (entry->mcp_request)
				fyai_mcp_call_cancel(entry->mcp_request);
			else
				fyai_tool_job_cancel(entry->job);
		}
		else if (entry->state == FYAITGS_QUEUED) {
			entry->state = FYAITGS_PARKED;
			group->parked++;
		}
	}
	group->next = group->count;
	fyai_tool_job_group_notify(group);
}

static fy_generic
fyai_tool_job_group_cancelled_result(struct fyai_tool_job_group *group,
				     struct fyai_tool_group_entry *entry)
{
	fy_generic call;

	call = parse_json_string(group->ctx->transient_gb, entry->call_text);
	if (fy_equal(fy_get(call, "type"), "shell_call"))
		return fy_gb_internalize(group->ctx->transient_gb,
			fy_sequence(fy_mapping(
				"stdout", "",
				"stderr", "",
				"outcome", fy_mapping(
					"type", "signal",
					"signal", SIGTERM))));
	return fy_value(group->ctx->transient_gb, "tool error: interrupted");
}

int fyai_tool_job_group_collect(struct fyai_tool_job_group *group,
				size_t index, fy_generic *result, bool *okp)
{
	struct fyai_tool_group_entry *entry;
	char *text;

	if (!group || !result || !okp || index >= group->count)
		return -1;
	entry = &group->entries[index];
	if (entry->state != FYAITGS_PARKED &&
	    entry->state != FYAITGS_SUBMIT_FAILED)
		return -1;
	if (entry->job) {
		*result = fyai_tool_job_collect(group->ctx, entry->job, okp);
		entry->job = NULL;
	} else if (entry->waiter) {
		text = fyai_waiter_result(entry->waiter, okp);
		fyai_waiter_destroy(entry->waiter);
		entry->waiter = NULL;
		*result = fy_gb_internalize(group->ctx->transient_gb,
					    fy_value(text ? text : ""));
		free(text);
	} else if (entry->mcp_request) {
		*result = fyai_mcp_call_collect(entry->mcp_request, okp);
		fyai_mcp_call_destroy(entry->mcp_request);
		entry->mcp_request = NULL;
	} else if (entry->result_text) {
		*result = parse_json_string(group->ctx->transient_gb,
					    entry->result_text);
		*okp = entry->result_ok;
	} else if (group->phase == FYAITGP_CANCELLED) {
		*result = fyai_tool_job_group_cancelled_result(group, entry);
		*okp = false;
	} else {
		return -1;
	}
	entry->state = FYAITGS_COLLECTED;
	return 0;
}

void fyai_tool_job_group_destroy(struct fyai_tool_job_group *group)
{
	struct fyai_tool_group_entry *entry;
	size_t i;

	if (!group)
		return;
	fyai_tool_job_group_cancel(group);
	fyai_event_source_remove(group->animation_timer);
	group->animation_timer = NULL;
	for (i = 0; i < group->count; i++) {
		entry = &group->entries[i];
		if (entry->job)
			fyai_tool_job_discard(entry->job);
		fyai_mcp_call_destroy(entry->mcp_request);
		fyai_waiter_destroy(entry->waiter);
		free(entry->call_text);
		free(entry->result_text);
	}
	free(group->entries);
	free(group);
}
