/*
 * fyai_shell_capture.c - shell capture and child start
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
#include "fyai_shell_capture.h"

/* Shell-capture state carried through the event callbacks. */
/* Each participant owns its own source pointer so that withdrawing is
 * idempotent. */
struct shell_capture {
	struct fyai_buf *buf;
	enum shell_output_stream stream;
	shell_output_fn output_fn;
	void *userdata;
	struct shell_capture_job *job;
	struct fyai_event_source *src;
	bool open;
	bool failed;
	/* The screen of a program that draws, made when it first does. */
	struct fyai_terminal_view *view;
	struct fyai_terminal_sniff sniff;
	const struct shell_command_opts *opts;
	struct fyai_ctx *ctx;
};

/*
 * Give the bytes that arrived to the capture. Until the program draws they are
 * the text of the result, and the live display shows them. From the first byte
 * that draws they go to a screen, made with all that came before, and the
 * result is what that screen shows.
 */
static int read_shell_pipe(int fd, struct shell_capture *cap,
			   const struct shell_command_opts *opts)
{
	char tmp[4096];
	ssize_t nread;

	nread = read(fd, tmp, sizeof(tmp));
	if (nread < 0)
		return -1;
	if (!nread)
		return 0;

	if (cap->view)
		return fyai_terminal_view_feed(cap->view, tmp, (size_t)nread) ? -1 : 1;
	if (fyai_buf_reserve(cap->buf, cap->buf->len + (size_t)nread + 1))
		return -1;
	memcpy(cap->buf->data + cap->buf->len, tmp, (size_t)nread);
	cap->buf->len += (size_t)nread;
	cap->buf->data[cap->buf->len] = '\0';
	if (cap->output_fn)
		cap->output_fn(cap->userdata, cap->stream, tmp, (size_t)nread);
	if (opts && opts->rows > 0 && opts->cols > 0 &&
	    fyai_terminal_sniff(&cap->sniff, tmp, (size_t)nread)) {
		cap->view = fyai_terminal_view_create(cap->ctx, opts->rows,
						      opts->cols, opts->max_bytes);
		if (!cap->view)
			return 1;
		fyai_terminal_view_cooked(cap->view, true);
		if (fyai_terminal_view_feed(cap->view, cap->buf->data, cap->buf->len))
			return -1;
		free(cap->buf->data);
		cap->buf->data = NULL;
		cap->buf->len = 0;
		cap->buf->cap = 0;
	}
	return 1;
}

struct shell_capture_job {
	struct shell_capture out;
	struct shell_capture err;
	struct fyai_event_source *csrc;
	struct fyai_event_source *tsrc;
	struct fyai_event_source *killer;
	struct fyai_event_source *deadline;
	struct fyai_ctx *ctx;
	pid_t pid;
	bool reaped;
	bool cancelling;
	bool isolated_pgrp;
	bool timed_out;
	int status;
	bool done;
};

static void shell_capture_signal_group(struct shell_capture_job *job, int sig);

static void shell_capture_drop(struct fyai_event_source **srcp)
{
	fyai_event_source_remove(*srcp);
	*srcp = NULL;
}

/* Capture is complete after the direct child exits. */
static void shell_capture_update_done(struct shell_capture_job *job)
{
	job->done = job->reaped;
}

static enum fyai_event_action shell_capture_readable(const struct fyai_event *ev)
{
	struct shell_capture *cap = ev->userdata;
	int rc;

	if (ev->events & (FYAIEV_READ | FYAIEV_EOF)) {
		/* Drain greedily rather than one read per wakeup. */
		for (;;) {
			rc = read_shell_pipe(ev->fd, cap, cap->opts);
			if (rc > 0)
				continue;
			if (rc < 0 && errno == EINTR)
				continue;
			break;
		}
		if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return FYAIEA_CONTINUE;
		if (rc < 0)
			cap->failed = true;
	} else if (ev->events & FYAIEV_ERROR) {
		cap->failed = true;
	} else {
		return FYAIEA_CONTINUE;
	}

	cap->open = false;
	shell_capture_drop(&cap->src);
	shell_capture_update_done(cap->job);
	return FYAIEA_CONTINUE;
}

/* Drain data that is available when the direct child exits. */
static void shell_capture_drain(struct shell_capture *cap, int fd)
{
	int rc;

	for (;;) {
		rc = read_shell_pipe(fd, cap, cap->opts);
		if (rc > 0)
			continue;
		if (rc < 0 && errno == EINTR)
			continue;
		if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return;
		if (rc < 0)
			cap->failed = true;
		cap->open = false;
		return;
	}
}

static enum fyai_event_action shell_capture_child(const struct fyai_event *ev)
{
	struct shell_capture_job *job = ev->userdata;

	job->csrc = NULL;
	job->reaped = true;
	job->status = ev->status;
	/* Stop descendants that inherited capture descriptors. */
	shell_capture_signal_group(job, SIGKILL);
	shell_capture_update_done(job);
	return FYAIEA_CONTINUE;
}

static enum fyai_event_action shell_capture_signal(const struct fyai_event *ev)
{
	struct shell_capture_job *job = ev->userdata;

	/* Treat SIGINT and SIGTERM as shell-capture interrupts. */
	if (job && job->ctx &&
	    (ev->signo == SIGINT || ev->signo == SIGTERM))
		job->ctx->loop->interrupt_pending = true;
	return FYAIEA_CONTINUE;
}

/* Maximum time for a command to stop after SIGTERM. */
#define SHELL_CAPTURE_KILL_MS 2000

static void shell_capture_signal_group(struct shell_capture_job *job, int sig)
{
	if (job->isolated_pgrp) {
		if (!kill(-job->pid, sig))
			return;
		if (errno != ESRCH)
			return;
	}
	if (!job->reaped)
		(void)kill(job->pid, sig);
}

/* Use SIGKILL when descendants keep the capture descriptors open. */
static enum fyai_event_action shell_capture_kill(const struct fyai_event *ev)
{
	struct shell_capture_job *job = ev->userdata;

	job->killer = NULL;
	if (job->pid > 0 && !job->reaped)
		shell_capture_signal_group(job, SIGKILL);
	return FYAIEA_CONTINUE;
}

static void shell_capture_cancel(pid_t pid, struct shell_capture_job *job)
{
	struct fyai_event_loop *el;

	if (pid <= 0 || !job || job->reaped || job->cancelling)
		return;

	job->cancelling = true;
	job->pid = pid;
	shell_capture_signal_group(job, SIGTERM);
	shell_capture_update_done(job);

	el = job->ctx ? fyai_ctx_loop(job->ctx) : NULL;
	if (el && !job->killer)
		(void)fyai_event_add_timer(el, SHELL_CAPTURE_KILL_MS, 0,
					   shell_capture_kill, job,
					   &job->killer);
}

/* Abort without the delayed kill timer. */
static void shell_capture_abort(pid_t pid, struct shell_capture_job *job,
				int *statusp)
{
	pid_t waited;

	if (pid <= 0 || !job)
		return;

	shell_capture_signal_group(job, SIGKILL);
	if (job->reaped)
		return;

	do {
		waited = waitpid(pid, statusp, 0);
	} while (waited < 0 && errno == EINTR);
	job->reaped = true;
}

/* The time limit expired. Stop the command in the same way as an interrupt. */
static enum fyai_event_action
shell_capture_deadline(const struct fyai_event *ev)
{
	struct shell_capture_job *job = ev->userdata;

	job->deadline = NULL;
	job->timed_out = true;
	shell_capture_cancel(job->pid, job);
	return FYAIEA_CONTINUE;
}

bool fyai_process_reads_stdin(pid_t pid)
{
	/* Search the bounded child tree for the process reading input. */
	pid_t queue[FYAI_PROC_WALK_MAX];
	size_t head = 0, tail = 0;
	size_t room;

	if (pid <= 0)
		return false;
	queue[tail++] = pid;
	while (head < tail) {
		pid = queue[head++];
		if (fyai_host_process_reads_stdin(pid))
			return true;
		room = FYAI_PROC_WALK_MAX - tail;
		if (room)
			tail += fyai_host_process_children(pid, queue + tail, room);
	}
	return false;
}

int fyai_child_status_open(int fds[2])
{
	fds[0] = fds[1] = -1;
	return fyai_host_pipe(fds);
}

void fyai_child_status_report(int fd, enum fyai_child_stage stage, int err)
{
	struct fyai_child_start start;
	ssize_t n;

	if (fd < 0)
		return;
	start.stage = stage;
	start.err = err;
	/* One write of one record: there is nothing to interleave with. */
	do {
		n = write(fd, &start, sizeof(start));
	} while (n < 0 && errno == EINTR);
}

void fyai_child_status_read(int fd, struct fyai_child_start *start)
{
	ssize_t n;

	if (!start)
		return;
	start->stage = FYAI_CHILD_STAGE_NONE;
	start->err = 0;
	if (fd < 0)
		return;
	/* Read one failure record; EOF means exec succeeded. */
	do {
		n = read(fd, start, sizeof(*start));
	} while (n < 0 && errno == EINTR);
	if (n != (ssize_t)sizeof(*start)) {
		start->stage = FYAI_CHILD_STAGE_NONE;
		start->err = 0;
	}
}

const char *fyai_child_start_text(const struct fyai_child_start *start,
				  const char *shell, const char *workdir,
				  char *buf, size_t size)
{
	const char *why;

	if (!start || start->stage == FYAI_CHILD_STAGE_NONE || !buf || !size)
		return NULL;
	why = start->err ? strerror(start->err) : "no reason was reported";
	switch (start->stage) {
	case FYAI_CHILD_STAGE_VIEW:
		snprintf(buf, size,
			 "could not establish the filesystem view: %s", why);
		break;
	case FYAI_CHILD_STAGE_WORKDIR:
		snprintf(buf, size, "cannot enter workdir %s: %s",
			 workdir ? workdir : "", why);
		break;
	case FYAI_CHILD_STAGE_SANDBOX:
		snprintf(buf, size, "could not confine the command: %s", why);
		break;
	case FYAI_CHILD_STAGE_SESSION:
		snprintf(buf, size, "could not isolate the command: %s", why);
		break;
	case FYAI_CHILD_STAGE_CTTY:
		snprintf(buf, size,
			 "could not give the command its terminal: %s", why);
		break;
	case FYAI_CHILD_STAGE_STDIN:
		snprintf(buf, size,
			 "could not set the command standard input: %s", why);
		break;
	case FYAI_CHILD_STAGE_STDOUT:
		snprintf(buf, size,
			 "could not set the command standard output: %s", why);
		break;
	case FYAI_CHILD_STAGE_STDERR:
		snprintf(buf, size,
			 "could not set the command standard error: %s", why);
		break;
	case FYAI_CHILD_STAGE_STATUS:
		snprintf(buf, size,
			 "could not keep the command status descriptor: %s",
			 why);
		break;
	case FYAI_CHILD_STAGE_ENV:
		snprintf(buf, size,
			 "could not remove the credentials: %s", why);
		break;
	case FYAI_CHILD_STAGE_EXEC:
		snprintf(buf, size, "could not start the shell %s: %s",
			 shell && *shell ? shell : "/bin/sh", why);
		break;
	case FYAI_CHILD_STAGE_SETUP:
	case FYAI_CHILD_STAGE_NONE:
	default:
		snprintf(buf, size, "could not prepare the command: %s", why);
		break;
	}
	return buf;
}

/* Exit on a signal delivered before this process becomes the command.
 * Async-signal-safe. */
static void fyai_child_exec_prepare_signal(int signo)
{
	_exit(128 + signo);
}

/* Handle SIGTERM and SIGHUP instead of taking their default action.
 * exec() resets the disposition, so the command inherits none of it. */
static void fyai_child_exec_prepare_signals(void)
{
	struct sigaction sa = {};

	sa.sa_handler = fyai_child_exec_prepare_signal;
	(void)sigaction(SIGTERM, &sa, NULL);
	(void)sigaction(SIGHUP, &sa, NULL);
}


/*
 * Close every descriptor from @lowfd on except the ones that the spec passes,
 * which first take the numbers that the child asks for. A copy goes to a number
 * above every target first, so that no target overwrites a source that is still
 * to be copied. A status descriptor takes number 3 after the copies: the number
 * can be that of a source, and a copy made later would read the status pipe.
 */
static int child_pass_fds(const struct fyai_child_spec *spec, int lowfd, int status_fd)
{
	int tmp[FYAI_CHILD_PASS_MAX], keep[FYAI_CHILD_PASS_MAX], top = lowfd, swap;
	unsigned int next = (unsigned int)lowfd;
	size_t i, j;

	for (i = 0; i < spec->pass_n; i++) {
		keep[i] = spec->pass_as[i];
		if (keep[i] >= top)
			top = keep[i] + 1;
	}
	for (i = 0; i < spec->pass_n; i++) {
		tmp[i] = fcntl(spec->pass_fd[i], F_DUPFD_CLOEXEC, top);
		if (tmp[i] < 0)
			return -1;
	}
	if (status_fd >= 0) {
		if (dup2(status_fd, 3) < 0)
			return -1;
		(void)fcntl(3, F_SETFD, FD_CLOEXEC);
	}
	if (!spec->pass_n) {
		fyai_close_fds_from(lowfd);
		return 0;
	}
	for (i = 0; i < spec->pass_n; i++)
		if (dup2(tmp[i], spec->pass_as[i]) < 0)
			return -1;
	/* The numbers in order, so that the ranges between them can be closed. */
	for (i = 1; i < spec->pass_n; i++)
		for (j = i; j > 0 && keep[j - 1] > keep[j]; j--) {
			swap = keep[j];
			keep[j] = keep[j - 1];
			keep[j - 1] = swap;
		}
	for (i = 0; i < spec->pass_n; i++) {
		if ((unsigned int)keep[i] > next)
			fyai_close_fds_range(next, (unsigned int)keep[i] - 1);
		next = (unsigned int)keep[i] + 1;
	}
	/* The copies sit above every number kept, and go with the rest. */
	fyai_close_fds_range(next, ~0U);
	return 0;
}

int fyai_child_exec_prepare(struct fyai_ctx *ctx,
			    const struct fyai_child_spec *spec)
{
	int status_fd = spec->status_fd;
	enum fyai_child_stage stage;
	char num[16];

	/*
	 * A cancellation can race this setup. Install the handler before
	 * abandoning the parent's loop: abandon unblocks the signals, and
	 * an install after it is too late for one already pending. Install
	 * it again after for a backend that restores signal actions as
	 * part of abandon.
	 */
	fyai_child_exec_prepare_signals();
	/* The application loop belongs to fyai, never to the command. */
	fyai_ctx_loop_abandon(ctx);
	fyai_child_exec_prepare_signals();
	/* Isolate the child tree, optionally in a new session. */
	stage = FYAI_CHILD_STAGE_SESSION;
	if (spec->own_session) {
		/* A leader already cannot start a session; a group still
		 * separates it from the group of this process. */
		if (setsid() < 0 && setpgid(0, 0) < 0)
			goto err_setup;
	} else if (setpgid(0, 0) < 0) {
		goto err_setup;
	}
	stage = FYAI_CHILD_STAGE_CTTY;
	if (spec->ctty_fd >= 0 && ioctl(spec->ctty_fd, TIOCSCTTY, 0) < 0)
		goto err_setup;
	stage = FYAI_CHILD_STAGE_STDIN;
	if (spec->in_fd >= 0 && dup2(spec->in_fd, STDIN_FILENO) < 0)
		goto err_setup;
	stage = FYAI_CHILD_STAGE_STDOUT;
	if (spec->out_fd >= 0 && dup2(spec->out_fd, STDOUT_FILENO) < 0)
		goto err_setup;
	stage = FYAI_CHILD_STAGE_STDERR;
	if (spec->err_fd >= 0 && dup2(spec->err_fd, STDERR_FILENO) < 0)
		goto err_setup;
	/* Preserve the close-on-exec status fd while closing inherited fds. */
	stage = FYAI_CHILD_STAGE_STATUS;
	if (child_pass_fds(spec, status_fd >= 0 ? 4 : 3, status_fd))
		goto err_setup;
	if (status_fd >= 0)
		status_fd = 3;
	/* Fail closed if any provider credential cannot be removed. */
	stage = FYAI_CHILD_STAGE_ENV;
	if (!spec->inherit_env && fyai_env_sanitize(spec->env_keep))
		goto err_setup;
	/* Describe the child's terminal and screen dimensions. */
	if (spec->term)
		setenv("TERM", spec->term, 1);
	if (spec->rows > 0 && spec->cols > 0) {
		snprintf(num, sizeof(num), "%d", spec->rows);
		setenv("LINES", num, 1);
		snprintf(num, sizeof(num), "%d", spec->cols);
		setenv("COLUMNS", num, 1);
	} else if (spec->term) {
		unsetenv("LINES");
		unsetenv("COLUMNS");
	}
	if (spec->view && fyai_fsview_enter(ctx->cfg, spec->view, status_fd,
					  spec->announce_as > 0 ? spec->announce_as : -1)) {
		fyai_child_status_report(status_fd, FYAI_CHILD_STAGE_VIEW,
					 errno);
		return FYAI_SHELL_EXIT_SANDBOX;
	}
	/* Enter the directory before applying confinement. */
	if (spec->workdir && *spec->workdir && chdir(spec->workdir) < 0) {
		fyai_child_status_report(status_fd, FYAI_CHILD_STAGE_WORKDIR,
					 errno);
		return FYAI_SHELL_EXIT_WORKDIR;
	}
	/* Confine the tool before handing control to the shell. */
	if (spec->sandbox && fyai_sandbox_apply(spec->sandbox)) {
		fyai_child_status_report(status_fd, FYAI_CHILD_STAGE_SANDBOX,
					 errno);
		return FYAI_SHELL_EXIT_SANDBOX;
	}
	return 0;

err_setup:
	fyai_child_status_report(status_fd, stage, errno);
	return FYAI_SHELL_EXIT_EXEC;
}

/* Replace this process with a command or interactive shell. */
void fyai_exec_shell_command(int status_fd, const char *command,
			     const char *shell, bool login)
{
	char argv0[64];
	const char *base;

	if (!shell || !*shell)
		shell = "/bin/sh";
	base = strrchr(shell, '/');
	base = base ? base + 1 : shell;
	snprintf(argv0, sizeof(argv0), "%s%s", login ? "-" : "", base);

	/*
	 * Resolve a shell name through PATH; a name with '/' is already a path.
	 * A login shell is asked for two times: the leading dash is what a
	 * shell that reads argv[0] looks at, and -l is what bash answers when
	 * it runs a command. Every supported shell accepts -l.
	 */
	if (command && *command) {
		if (login)
			execlp(shell, argv0, "-l", "-c", command,
			       (char *)NULL);
		else
			execlp(shell, argv0, "-c", command, (char *)NULL);
	} else {
		execlp(shell, argv0, (char *)NULL);
	}
	fyai_child_status_report(status_fd, FYAI_CHILD_STAGE_EXEC, errno);
}

static void shell_capture_exec(struct fyai_ctx *ctx, const char *command,
			       const struct shell_command_opts *opts,
			       const struct fyai_sandbox_spec *sandbox,
			       int stdout_pipe[2], int stderr_pipe[2],
			       int status_fd)
{
	struct fyai_child_spec spec = {};
	int devnull;
	int rc;

	close(stdout_pipe[0]);
	close(stderr_pipe[0]);
	/* Nothing here reads: a command that asks is answered end of input. */
	devnull = open("/dev/null", O_RDONLY);

	spec.in_fd = devnull;
	spec.out_fd = stdout_pipe[1];
	spec.err_fd = stderr_pipe[1];
	spec.ctty_fd = -1;
	spec.view = opts ? opts->view : NULL;
	spec.workdir = opts ? opts->workdir : NULL;
	spec.sandbox = sandbox;
	spec.status_fd = status_fd;
	spec.env_keep = opts ? opts->env_keep : NULL;
	rc = fyai_child_exec_prepare(ctx, &spec);
	if (rc)
		_exit(rc);
	if (devnull < 0)
		close(STDIN_FILENO);
	fyai_exec_shell_command(spec.status_fd < 0 ? -1 : 3, command,
				opts ? opts->shell : NULL,
				opts && opts->login);
	_exit(FYAI_SHELL_EXIT_EXEC);
}

int run_shell_command_capture_cb(struct fyai_ctx *ctx, const char *command,
				 struct shell_command_result *result,
				 shell_output_fn output_fn,
				 void *userdata,
				 const struct fyai_sandbox_spec *sandbox,
				 const struct shell_command_opts *opts)
{
	unsigned int timeout_ms = opts ? opts->timeout_ms : 0;
	struct fyai_buf stdout_buf = {};
	struct fyai_buf stderr_buf = {};
	struct shell_capture_job job = {};
	struct fyai_event_loop *el = NULL;
	int stdout_pipe[2] = { -1, -1 };
	int stderr_pipe[2] = { -1, -1 };
	int status_pipe[2] = { -1, -1 };
	int status = 0;
	pid_t pid = -1;
	int ret = -1;

	memset(result, 0, sizeof(*result));

	if (pipe(stdout_pipe) || pipe(stderr_pipe))
		goto out;
	if (fyai_child_status_open(status_pipe))
		goto out;

	pid = fork();
	if (pid < 0)
		goto out;

	if (!pid) {
		close(status_pipe[0]);
		shell_capture_exec(ctx, command, opts, sandbox,
				   stdout_pipe, stderr_pipe, status_pipe[1]);
	}
	close(status_pipe[1]);
	status_pipe[1] = -1;
	/* Why the command never started, when it did not. */
	fyai_child_status_read(status_pipe[0], &result->start);
	close(status_pipe[0]);
	status_pipe[0] = -1;
	/* Close the fork-to-exec race from the parent side. */
	do {
		ret = setpgid(pid, pid);
	} while (ret < 0 && errno == EINTR);
	job.isolated_pgrp = !ret || errno == EACCES;
	ret = -1;
	job.pid = pid;

	close(stdout_pipe[1]);
	close(stderr_pipe[1]);
	stdout_pipe[1] = -1;
	stderr_pipe[1] = -1;

	ret = fcntl(stdout_pipe[0], F_SETFL, O_NONBLOCK);
	fyai_error_check(ctx, ret >= 0, out_wait,
			 "could not make shell stdout nonblocking");
	ret = fcntl(stderr_pipe[0], F_SETFL, O_NONBLOCK);
	fyai_error_check(ctx, ret >= 0, out_wait,
			 "could not make shell stderr nonblocking");

	/* Watch both pipes and the child together. */
	job.out.buf = &stdout_buf;
	job.out.stream = SHELL_OUTPUT_STDOUT;
	job.err.buf = &stderr_buf;
	job.err.stream = SHELL_OUTPUT_STDERR;
	job.out.opts = job.err.opts = opts;
	job.out.ctx = job.err.ctx = ctx;
	job.out.output_fn = job.err.output_fn = output_fn;
	job.out.userdata = job.err.userdata = userdata;
	job.out.job = job.err.job = &job;
	job.out.open = job.err.open = true;
	job.ctx = ctx;

	el = fyai_ctx_loop(ctx);
	if (!el)
		goto out_wait;

	if (timeout_ms) {
		ret = fyai_event_add_timer(el, timeout_ms, 0,
					   shell_capture_deadline,
					   &job, &job.deadline);
		fyai_error_check(ctx, !ret, out_wait,
				 "could not arm the shell time limit");
	}

	if (fyai_event_add_fd(el, stdout_pipe[0], FYAIEV_READ,
			      shell_capture_readable, &job.out, &job.out.src) ||
	    fyai_event_add_fd(el, stderr_pipe[0], FYAIEV_READ,
			      shell_capture_readable, &job.err, &job.err.src) ||
	    fyai_event_add_child(el, pid, shell_capture_child, &job, &job.csrc) ||
	    /* Do not block SIGINT. The watchdog handler must receive it. */
	    fyai_event_add_signal(el, SIGTERM, shell_capture_signal, &job,
				  &job.tsrc))
		goto out_wait;

	/* Clear the pid once the loop owns the reap. */
	while (!job.done) {
		if (fyai_interrupt_pending(ctx)) {
			fyai_event_interrupt_ack(ctx->loop);
			shell_capture_cancel(pid, &job);
		}
		ret = fyai_event_loop_step(el, -1);
		fyai_error_check(ctx, ret >= 0, out_wait,
				 "shell capture event loop failed");
	}
	shell_capture_drain(&job.out, stdout_pipe[0]);
	shell_capture_drain(&job.err, stderr_pipe[0]);

	if (job.reaped)
		pid = -1;
	if (!job.done || job.out.failed || job.err.failed)
		goto out_wait;

	status = job.status;

	/* A stream that drew is what its screen shows. */
	if (job.out.view)
		result->stdout_data = fyai_terminal_view_read(job.out.view, FYAITR_ALL,
							      NULL, &result->stdout_len);
	else
		result->stdout_data = stdout_buf.data ? stdout_buf.data : strdup("");
	if (job.err.view)
		result->stderr_data = fyai_terminal_view_read(job.err.view, FYAITR_ALL,
							      NULL, &result->stderr_len);
	else
		result->stderr_data = stderr_buf.data ? stderr_buf.data : strdup("");
	if (!job.out.view)
		result->stdout_len = stdout_buf.len;
	if (!job.err.view)
		result->stderr_len = stderr_buf.len;
	if (!job.out.view)
		stdout_buf.data = NULL;
	if (!job.err.view)
		stderr_buf.data = NULL;
	if (!result->stdout_data || !result->stderr_data)
		goto out;

	result->timed_out = job.timed_out;
	if (WIFSIGNALED(status)) {
		result->signaled = true;
		result->signal = WTERMSIG(status);
	} else if (WIFEXITED(status)) {
		result->exit_code = WEXITSTATUS(status);
	}

	ret = 0;
	goto out;

out_wait:
	shell_capture_abort(pid, &job, &status);
	pid = -1;
out:
	/* Withdraw sources before closing their pipes. */
	shell_capture_drop(&job.out.src);
	shell_capture_drop(&job.err.src);
	shell_capture_drop(&job.csrc);
	shell_capture_drop(&job.tsrc);
	shell_capture_drop(&job.killer);
	shell_capture_drop(&job.deadline);

	if (pid > 0)
		shell_capture_abort(pid, &job, NULL);
	if (stdout_pipe[0] >= 0)
		close(stdout_pipe[0]);
	if (stdout_pipe[1] >= 0)
		close(stdout_pipe[1]);
	if (stderr_pipe[0] >= 0)
		close(stderr_pipe[0]);
	if (stderr_pipe[1] >= 0)
		close(stderr_pipe[1]);
	if (status_pipe[0] >= 0)
		close(status_pipe[0]);
	if (status_pipe[1] >= 0)
		close(status_pipe[1]);
	fyai_terminal_view_destroy(job.out.view);
	fyai_terminal_view_destroy(job.err.view);
	free(stdout_buf.data);
	free(stderr_buf.data);
	if (ret)
		shell_command_result_cleanup(result);
	return ret;
}

void shell_command_result_cleanup(struct shell_command_result *result)
{
	if (!result)
		return;

	free(result->stdout_data);
	free(result->stderr_data);
	memset(result, 0, sizeof(*result));
}
