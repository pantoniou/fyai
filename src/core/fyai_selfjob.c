/*
 * fyai_selfjob.c - fyai itself as a child on the event loop
 *
 * SPDX-License-Identifier: MIT
 */
#define FYAI_MODULE FYAIEM_UNKNOWN
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fyai.h"
#include "fyai_branch.h"
#include "fyai_buf.h"
#include "fyai_config.h"
#include "fyai_event.h"
#include "fyai_host.h"
#include "fyai_loop.h"
#include "fyai_selfjob.h"
#include "fyai_transport_boot.h"
#include "utils.h"

/* The output kept; a child that writes more is cut there. */
#define SELFJOB_OUT_MAX		(1024 * 1024)
/* Fixed descriptors of the credential channels in the child. */
#define SELFJOB_AGENT_FD	3
#define SELFJOB_CTL_FD		4
#define SELFJOB_SYNC_FD		5
/* Descriptors are moved above this before they take the fixed slots. */
#define SELFJOB_FD_HIGH		64

struct fyai_selfjob {
	struct fyai_ctx *ctx;
	pid_t pid;
	int out_fd;
	struct fyai_event_source *out_src, *child_src, *timer;
	struct fyai_buf out;
	int status;
	bool exited, eof, timed_out;
	fyai_selfjob_done_fn done;
	void *data;
};

/* In the child: take the channels, close the rest, and become fyai. */
static void selfjob_exec(struct fyai_ctx *ctx, const char *const *argv,
			 int out, int agent, int ctl, int sync)
{
	bool isolated = ctx->transport->client;
	const char *env;
	int null;

	(void)setpgid(0, 0);
	out = fcntl(out, F_DUPFD, SELFJOB_FD_HIGH);
	if (isolated) {
		agent = fcntl(agent, F_DUPFD, SELFJOB_FD_HIGH);
		ctl = fcntl(ctl, F_DUPFD, SELFJOB_FD_HIGH);
		sync = fcntl(sync, F_DUPFD, SELFJOB_FD_HIGH);
	}
	if (out < 0 || (isolated && (agent < 0 || ctl < 0 || sync < 0)))
		_exit(126);
	fyai_loop_restore_signal_mask(ctx->loop);
	null = open("/dev/null", O_RDWR);
	if (null < 0 || dup2(null, STDIN_FILENO) < 0 ||
	    dup2(out, STDOUT_FILENO) < 0 || dup2(null, STDERR_FILENO) < 0)
		_exit(126);
	if (isolated &&
	    (dup2(agent, SELFJOB_AGENT_FD) < 0 || dup2(ctl, SELFJOB_CTL_FD) < 0 ||
	     dup2(sync, SELFJOB_SYNC_FD) < 0))
		_exit(126);
	fyai_close_fds_from(isolated ? SELFJOB_SYNC_FD + 1 : 3);
	/*
	 * Without credential isolation the key may have come from --api-key,
	 * which the child does not see: it is given in the variable that the
	 * child falls back on, in the child only.
	 */
	if (!isolated && ctx->cfg->api_key && *ctx->cfg->api_key &&
	    ctx->cfg->provider && *ctx->cfg->provider) {
		env = fyai_config_provider_env_key(ctx->cfg->gb,
						   ctx->cfg->provider);
		if (env && setenv(env, ctx->cfg->api_key, 1))
			_exit(126);
	}
	if (isolated) {
		if (fyai_transport_worker_attach(SELFJOB_SYNC_FD,
						 SELFJOB_AGENT_FD,
						 SELFJOB_CTL_FD))
			_exit(126);
		close(SELFJOB_SYNC_FD);
	}
	(void)fyai_exec_self(argv);
	_exit(127);
}

static void selfjob_free(struct fyai_selfjob *job)
{
	if (job->out_src)
		fyai_event_source_remove(job->out_src);
	if (job->timer)
		fyai_event_source_remove(job->timer);
	if (job->child_src)
		fyai_event_source_remove(job->child_src);
	if (job->out_fd >= 0)
		close(job->out_fd);
	free(job->out.data);
	free(job);
}

/* Both the output and the child have ended: report, then go. */
static void selfjob_settle(struct fyai_selfjob *job)
{
	if (!job->eof || !job->exited)
		return;
	if (job->timer) {
		fyai_event_source_remove(job->timer);
		job->timer = NULL;
	}
	job->done(job->data, job->timed_out ? -1 : job->status,
		  job->out.data ? job->out.data : "");
	selfjob_free(job);
}

static void selfjob_eof(struct fyai_selfjob *job)
{
	if (job->out_src) {
		fyai_event_source_remove(job->out_src);
		job->out_src = NULL;
	}
	if (job->out_fd >= 0) {
		close(job->out_fd);
		job->out_fd = -1;
	}
	job->eof = true;
}

static enum fyai_event_action selfjob_readable(const struct fyai_event *ev)
{
	struct fyai_selfjob *job = ev->userdata;
	char buf[16384];
	ssize_t n;

	n = read(job->out_fd, buf, sizeof(buf));
	if (n < 0 && (errno == EINTR || errno == EAGAIN))
		return FYAIEA_CONTINUE;
	if (n > 0) {
		if (job->out.len + (size_t)n <= SELFJOB_OUT_MAX &&
		    !fyai_buf_append_data(&job->out, buf, (size_t)n))
			return FYAIEA_CONTINUE;
		/* Too much, or no memory: what was kept is the output. */
		if (job->pid > 0)
			(void)kill(-job->pid, SIGKILL);
	}
	selfjob_eof(job);
	selfjob_settle(job);
	return FYAIEA_CONTINUE;
}

static enum fyai_event_action selfjob_exited(const struct fyai_event *ev)
{
	struct fyai_selfjob *job = ev->userdata;

	job->child_src = NULL;		/* a child source retires itself */
	job->status = WIFEXITED(ev->status) ? WEXITSTATUS(ev->status) : -1;
	job->exited = true;
	job->pid = -1;
	selfjob_settle(job);
	return FYAIEA_CONTINUE;
}

static enum fyai_event_action selfjob_expired(const struct fyai_event *ev)
{
	struct fyai_selfjob *job = ev->userdata;

	job->timer = NULL;		/* a one-shot timer retires itself */
	job->timed_out = true;
	if (job->pid > 0)
		(void)kill(-job->pid, SIGKILL);
	return FYAIEA_CONTINUE;
}

struct fyai_selfjob *fyai_selfjob_start(struct fyai_ctx *ctx,
					const char *const *argv,
					long long timeout_ms,
					fyai_selfjob_done_fn done, void *data)
{
	struct fyai_event_loop *el = fyai_ctx_loop(ctx);
	struct fyai_selfjob *job;
	int out[2] = { -1, -1 }, agent[2] = { -1, -1 }, ctl[2] = { -1, -1 };
	int sync[2] = { -1, -1 }, i, status;
	bool isolated = ctx->transport->client;

	if (!el || !fyai_exec_self_available())
		return NULL;
	job = calloc(1, sizeof(*job));
	if (!job)
		return NULL;
	job->ctx = ctx;
	job->pid = -1;
	job->out_fd = -1;
	job->done = done;
	job->data = data;
	if (fyai_host_pipe(out) ||
	    (isolated && (fyai_host_seqpacket_pair(agent) ||
			  fyai_host_seqpacket_pair(ctl) ||
			  fyai_host_pipe(sync))))
		goto err;
	job->pid = fork();
	if (job->pid < 0)
		goto err;
	if (!job->pid)
		selfjob_exec(ctx, argv, out[1], agent[0], ctl[0], sync[0]);
	(void)setpgid(job->pid, job->pid);
	if (isolated && fyai_transport_worker_admit(ctx->transport, job->pid,
						    agent[1], ctl[1], sync[1]))
		goto err_child;
	job->out_fd = out[0];
	out[0] = -1;
	(void)fcntl(job->out_fd, F_SETFL,
		    fcntl(job->out_fd, F_GETFL) | O_NONBLOCK);
	if (fyai_event_add_fd(el, job->out_fd, FYAIEV_READ, selfjob_readable,
			      job, &job->out_src) ||
	    fyai_event_add_child(el, job->pid, selfjob_exited, job,
				 &job->child_src) ||
	    (timeout_ms > 0 &&
	     fyai_event_add_timer(el, timeout_ms, 0, selfjob_expired, job,
				  &job->timer)))
		goto err_child;
	for (i = 0; i < 2; i++) {
		if (out[i] >= 0)
			close(out[i]);
		if (agent[i] >= 0)
			close(agent[i]);
		if (ctl[i] >= 0)
			close(ctl[i]);
		if (sync[i] >= 0)
			close(sync[i]);
	}
	return job;

err_child:
	(void)kill(-job->pid, SIGKILL);
	if (job->child_src) {
		fyai_event_source_remove(job->child_src);
		job->child_src = NULL;
	}
	while (waitpid(job->pid, &status, 0) < 0 && errno == EINTR)
		;
	job->pid = -1;
err:
	for (i = 0; i < 2; i++) {
		if (out[i] >= 0)
			close(out[i]);
		if (agent[i] >= 0)
			close(agent[i]);
		if (ctl[i] >= 0)
			close(ctl[i]);
		if (sync[i] >= 0)
			close(sync[i]);
	}
	selfjob_free(job);
	return NULL;
}

void fyai_selfjob_cancel(struct fyai_selfjob *job)
{
	int status;

	if (!job)
		return;
	if (job->pid > 0) {
		/* Reaped here, so nothing of the job outlives the call. */
		if (job->child_src) {
			fyai_event_source_remove(job->child_src);
			job->child_src = NULL;
		}
		(void)kill(-job->pid, SIGKILL);
		while (waitpid(job->pid, &status, 0) < 0 && errno == EINTR)
			;
	}
	selfjob_free(job);
}

struct fyai_selfjob *fyai_selfjob_decide_start(struct fyai_ctx *ctx,
					       const char *spec,
					       const char *input,
					       long long timeout_ms,
					       fyai_selfjob_done_fn done,
					       void *data)
{
	struct fyai_cfg *cfg = ctx->cfg;
	const char *argv[24], *branch = fyai_ctx_branch(ctx);
	int argc = 0;

	argv[argc++] = "fyai";
	argv[argc++] = "--color";
	argv[argc++] = "off";
	if (branch) {
		argv[argc++] = "-b";
		argv[argc++] = branch;
	}
	if (cfg->model && *cfg->model) {
		argv[argc++] = "-m";
		argv[argc++] = cfg->model;
	}
	argv[argc++] = "--transient";
	argv[argc++] = "--set";
	argv[argc++] = fy_sprintfa("api=%s", fyai_api_to_string(cfg->api_mode));
	if (cfg->api_url && *cfg->api_url) {
		argv[argc++] = "--set";
		argv[argc++] = fy_sprintfa("api_url=%s", cfg->api_url);
	}
	argv[argc++] = "decide";
	argv[argc++] = "--spec";
	argv[argc++] = spec;
	argv[argc++] = "--input";
	argv[argc++] = input;
	argv[argc] = NULL;
	return fyai_selfjob_start(ctx, argv, timeout_ms, done, data);
}
