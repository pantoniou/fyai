/*
 * fyai_terminal_session.c - synchronous PTY terminal execution
 *
 * The bytes that a program writes to a terminal hold cursor movements, so a
 * model cannot use them. libfyvterm interprets them. The result is the screen
 * that the user would see, with the lines that scrolled off it in front. The
 * alternate screen is not enabled, so a full-screen program leaves its last
 * screen and does not restore an empty one.
 *
 * SPDX-License-Identifier: MIT
 */
#define FYAI_MODULE FYAIEM_TOOLS
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <errno.h>
#include <fcntl.h>
/* openpty(3) lives in <util.h> on the BSDs, <pty.h> on glibc */
#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "fyai.h"
#include "fyai_event.h"
#include "fyai_sandbox.h"
#include "fyai_terminal.h"
#include "fyai_tools.h"
#include "fyai_jsonrpc.h"
#include "fyai_terminal_session.h"
#include "fyai_terminal_view.h"
#include "utils.h"

/* Time a program gets to restore the terminal after the limit expires. */
#define FYAI_TTY_KILL_GRACE_MS	2000

struct tty_run {
	struct fyai_ctx *ctx;
	const struct fyai_terminal_opts *opts;
	struct fyai_terminal_view *view;
	int rows;
	int cols;
	int master;
	pid_t pid;
	bool done;
	bool reaped;
	bool timed_out;
	bool cancelling;
	int status;
	struct fyai_event_source *fdsrc;
	struct fyai_event_source *childsrc;
	struct fyai_event_source *timersrc;
	struct fyai_event_source *killer;
};

/* Write the reply of the view to the terminal of the program. */
static void tty_view_reply(const char *data, size_t len, void *user)
{
	struct tty_run *r = user;

	fyai_terminal_reply_write(r->master, data, len);
}

static enum fyai_event_action tty_read(const struct fyai_event *ev)
{
	struct tty_run *r = ev->userdata;
	char buf[4096];
	ssize_t n;

	for (;;) {
		n = read(r->master, buf, sizeof(buf));
		if (n > 0) {
			(void)fyai_terminal_view_feed(r->view, buf, (size_t)n);
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EINTR))
			return FYAIEA_CONTINUE;
		/*
		 * EIO is how the last slave descriptor closing is reported on
		 * Linux; it is the end of the stream and not a failure.
		 */
		if (r->fdsrc) {
			fyai_event_source_remove(r->fdsrc);
			r->fdsrc = NULL;
		}
		if (r->reaped)
			r->done = true;
		return FYAIEA_CONTINUE;
	}
}

static enum fyai_event_action tty_child(const struct fyai_event *ev)
{
	struct tty_run *r = ev->userdata;

	r->status = ev->status;
	r->reaped = true;
	if (r->childsrc) {
		fyai_event_source_remove(r->childsrc);
		r->childsrc = NULL;
	}
	/* Drain ready output; descendants may keep the PTY open indefinitely. */
	if (r->fdsrc)
		(void)tty_read(ev);
	r->done = true;
	return FYAIEA_CONTINUE;
}

static enum fyai_event_action tty_kill(const struct fyai_event *ev)
{
	struct tty_run *r = ev->userdata;

	(void)kill(-r->pid, SIGKILL);
	return FYAIEA_CONTINUE;
}

/*
 * Stop the command as the pipe path stops it. Send SIGHUP and SIGTERM to the
 * group, then SIGKILL after a grace time, so that a full-screen program can
 * restore the terminal. The terminal goes away, so the hangup is what an
 * interactive shell answers: it ignores SIGTERM. Call this one time.
 */
static void tty_cancel(struct tty_run *r)
{
	struct fyai_event_loop *el;

	if (r->cancelling || r->reaped)
		return;
	r->cancelling = true;
	(void)kill(-r->pid, SIGHUP);
	(void)kill(-r->pid, SIGTERM);

	el = fyai_ctx_loop(r->ctx);
	if (el && !r->killer)
		(void)fyai_event_add_timer(el, FYAI_TTY_KILL_GRACE_MS, 0,
					   tty_kill, r, &r->killer);
}

/*
 * The limit expired. The terminate source that owns the child sends SIGTERM
 * and then SIGKILL to the group; this only records why.
 */
static enum fyai_event_action tty_deadline(const struct fyai_event *ev)
{
	struct tty_run *r = ev->userdata;

	r->timed_out = true;
	return FYAIEA_CONTINUE;
}

static void tty_sources_remove(struct tty_run *r)
{
	if (r->killer) {
		fyai_event_source_remove(r->killer);
		r->killer = NULL;
	}
	if (r->timersrc) {
		fyai_event_source_remove(r->timersrc);
		r->timersrc = NULL;
	}
	if (r->fdsrc) {
		fyai_event_source_remove(r->fdsrc);
		r->fdsrc = NULL;
	}
	if (r->childsrc) {
		fyai_event_source_remove(r->childsrc);
		r->childsrc = NULL;
	}
}

/* The child side of the fork. It never returns. */
static void tty_child_exec(struct fyai_ctx *ctx, const char *command,
			   const struct fyai_sandbox_spec *sandbox,
			   const struct fyai_terminal_opts *opts, int slave,
			   int status_fd)
{
	struct fyai_child_spec spec = {};
	int rc;

	spec.in_fd = slave;
	spec.out_fd = slave;
	spec.err_fd = slave;
	spec.ctty_fd = slave;
	spec.own_session = true;
	/* Describe the libfyvterm terminal rather than the parent terminal. */
	spec.term = fy_str_empty(opts->term) ? "xterm-256color" : opts->term;
	spec.rows = opts->rows > 0 ? opts->rows : FYAI_TTY_ROWS_DEFAULT;
	spec.cols = opts->cols > 0 ? opts->cols : FYAI_TTY_COLS_DEFAULT;
	spec.workdir = opts->workdir;
	spec.sandbox = sandbox;
	spec.status_fd = status_fd;

	rc = fyai_child_exec_prepare(ctx, &spec);
	if (rc)
		_exit(rc);
	fyai_exec_shell_command(spec.status_fd < 0 ? -1 : 3, command,
				opts->shell, opts->login);
	_exit(FYAI_SHELL_EXIT_EXEC);
}

int fyai_terminal_pty_spawn(struct fyai_ctx *ctx, const char *command,
			    const struct fyai_sandbox_spec *sandbox,
			    const struct fyai_terminal_opts *opts, int rows,
			    int cols, int *masterp, pid_t *pidp,
			    struct fyai_child_start *startp)
{
	int status_pipe[2] = { -1, -1 };
	struct winsize ws = {};
	int master = -1;
	int slave = -1;
	int flags, rc;
	pid_t pid;

	rc = openpty(&master, &slave, NULL, NULL, NULL);
	if (rc) {
		fyai_error(ctx, "openpty: %s", strerror(errno));
		return -1;
	}

	ws.ws_row = (unsigned short)rows;
	ws.ws_col = (unsigned short)cols;
	rc = ioctl(slave, TIOCSWINSZ, &ws);
	if (rc)
		goto fail;

	if (fyai_child_status_open(status_pipe))
		goto fail;

	pid = fork();
	if (pid < 0)
		goto fail;
	if (!pid) {
		close(master);
		close(status_pipe[0]);
		tty_child_exec(ctx, command, sandbox, opts, slave,
			       status_pipe[1]);
	}
	close(slave);
	slave = -1;
	close(status_pipe[1]);
	status_pipe[1] = -1;
	fyai_child_status_read(status_pipe[0], startp);
	close(status_pipe[0]);
	status_pipe[0] = -1;
	if (startp && startp->stage != FYAI_CHILD_STAGE_NONE) {
		/* The child did not run the program: reap it and report
		 * nothing here, because the caller names the shell and the
		 * command. */
		(void)waitpid(pid, NULL, 0);
		close(master);
		return -1;
	}

	flags = fcntl(master, F_GETFL, 0);
	if (flags >= 0)
		(void)fcntl(master, F_SETFL, flags | O_NONBLOCK);

	*masterp = master;
	*pidp = pid;
	return 0;

fail:
	if (status_pipe[0] >= 0)
		close(status_pipe[0]);
	if (status_pipe[1] >= 0)
		close(status_pipe[1]);
	if (slave >= 0)
		close(slave);
	if (master >= 0)
		close(master);
	return -1;
}

/* Run a pipe session child in its own process tree. */
static void pipe_child_exec(struct fyai_ctx *ctx, const char *command,
			    const struct fyai_sandbox_spec *sandbox,
			    const struct fyai_terminal_opts *opts,
			    int in_fd, int out_fd, int status_fd)
{
	struct fyai_child_spec spec = {};
	int rc;

	spec.in_fd = in_fd;
	spec.out_fd = out_fd;
	spec.err_fd = out_fd;
	spec.ctty_fd = -1;
	/* Isolate the child tree without inheriting the user's terminal. */
	spec.own_session = true;
	/* There is no terminal here, and TERM must say so. */
	spec.term = "dumb";
	spec.workdir = opts->workdir;
	spec.sandbox = sandbox;
	spec.status_fd = status_fd;

	rc = fyai_child_exec_prepare(ctx, &spec);
	if (rc)
		_exit(rc);
	fyai_exec_shell_command(spec.status_fd < 0 ? -1 : 3, command,
				opts->shell, opts->login);
	_exit(FYAI_SHELL_EXIT_EXEC);
}

int fyai_terminal_pipe_spawn(struct fyai_ctx *ctx, const char *command,
			     const struct fyai_sandbox_spec *sandbox,
			     const struct fyai_terminal_opts *opts,
			     int *readp, int *writep, pid_t *pidp,
			     struct fyai_child_start *startp)
{
	int status_pipe[2] = { -1, -1 };
	int out_pipe[2] = { -1, -1 };
	int in_pipe[2] = { -1, -1 };
	int flags;
	pid_t pid;

	fyai_error_check(ctx, !pipe(out_pipe), fail,
			 "pipe: %s", strerror(errno));
	fyai_error_check(ctx, !pipe(in_pipe), fail,
			 "pipe: %s", strerror(errno));

	if (fyai_child_status_open(status_pipe)) {
		fyai_error(ctx, "pipe: %s", strerror(errno));
		goto fail;
	}

	pid = fork();
	fyai_error_check(ctx, pid >= 0, fail,
			 "fork: %s", strerror(errno));
	if (!pid) {
		close(out_pipe[0]);
		close(in_pipe[1]);
		close(status_pipe[0]);
		pipe_child_exec(ctx, command, sandbox, opts, in_pipe[0],
				out_pipe[1], status_pipe[1]);
	}
	close(out_pipe[1]);
	close(in_pipe[0]);
	out_pipe[1] = in_pipe[0] = -1;
	close(status_pipe[1]);
	status_pipe[1] = -1;
	fyai_child_status_read(status_pipe[0], startp);
	close(status_pipe[0]);
	status_pipe[0] = -1;
	if (startp && startp->stage != FYAI_CHILD_STAGE_NONE) {
		/* The child did not run the program; the caller reports why. */
		(void)waitpid(pid, NULL, 0);
		close(out_pipe[0]);
		close(in_pipe[1]);
		return -1;
	}

	flags = fcntl(out_pipe[0], F_GETFL, 0);
	if (flags >= 0)
		(void)fcntl(out_pipe[0], F_SETFL, flags | O_NONBLOCK);
	flags = fcntl(in_pipe[1], F_GETFL, 0);
	if (flags >= 0)
		(void)fcntl(in_pipe[1], F_SETFL, flags | O_NONBLOCK);

	*readp = out_pipe[0];
	*writep = in_pipe[1];
	*pidp = pid;
	return 0;

fail:
	if (status_pipe[0] >= 0)
		close(status_pipe[0]);
	if (status_pipe[1] >= 0)
		close(status_pipe[1]);
	if (out_pipe[0] >= 0)
		close(out_pipe[0]);
	if (out_pipe[1] >= 0)
		close(out_pipe[1]);
	if (in_pipe[0] >= 0)
		close(in_pipe[0]);
	if (in_pipe[1] >= 0)
		close(in_pipe[1]);
	return -1;
}

int fyai_terminal_session_run(struct fyai_ctx *ctx, const char *command,
			      const struct fyai_sandbox_spec *sandbox,
			      const struct fyai_terminal_opts *opts,
			      struct fyai_terminal_result *result)
{
	struct fyai_terminal_opts defaults = {};
	struct fyai_event_loop *el;
	char why_buf[FYAI_CHILD_START_TEXT_MAX];
	struct fyai_child_start start = {};
	struct tty_run r = {};
	const char *why;
	int rc;
	pid_t pid;

	if (!opts)
		opts = &defaults;

	memset(result, 0, sizeof(*result));
	r.ctx = ctx;
	r.opts = opts;
	r.rows = opts->rows > 0 ? opts->rows : FYAI_TTY_ROWS_DEFAULT;
	r.cols = opts->cols > 0 ? opts->cols : FYAI_TTY_COLS_DEFAULT;
	r.master = -1;

	r.view = fyai_terminal_view_create(ctx, r.rows, r.cols,
					   opts->max_bytes);
	if (!r.view)
		return -1;
	fyai_terminal_view_line_cb(r.view, opts->output_fn, opts->output_data);
	fyai_terminal_view_reply_cb(r.view, tty_view_reply, &r);

	rc = fyai_terminal_pty_spawn(ctx, command, sandbox, opts, r.rows,
				     r.cols, &r.master, &pid, &start);
	if (rc) {
		/* The caller reports it: it has the call that this came
		 * from. */
		result->start = start;
		why = fyai_child_start_text(&start, opts->shell, opts->workdir,
					    why_buf, sizeof(why_buf));
		if (why)
			fyai_error(ctx, "shell: %s", why);
		goto fail_view;
	}
	r.pid = pid;

	el = fyai_ctx_loop(ctx);
	if (!el)
		goto fail_child;
	(void)fyai_terminal_winch_open(ctx);

	rc = fyai_event_add_fd(el, r.master, FYAIEV_READ, tty_read, &r,
			       &r.fdsrc);
	if (rc)
		goto fail_child;

	/*
	 * With a limit a terminate source owns the child. The source sends SIGTERM to
	 * the group at the limit, and SIGKILL after the grace time, so that a full-
	 * screen program can restore the terminal. The timer only records that the
	 * limit ended the run and not an interrupt.
	 */
	if (opts->timeout_ms) {
		rc = fyai_event_add_child_terminate_group(el, pid,
						opts->timeout_ms,
						FYAI_TTY_KILL_GRACE_MS,
						tty_child, &r, &r.childsrc);
		if (!rc)
			rc = fyai_event_add_timer(el, opts->timeout_ms, 0,
						  tty_deadline, &r,
						  &r.timersrc);
	} else {
		rc = fyai_event_add_child(el, pid, tty_child, &r, &r.childsrc);
	}
	if (rc)
		goto fail_child;

	ctx->tty_session = &r;
	/*
	 * An interrupt must reach the command. Without this the user can only
	 * wait for the limit, because nothing else stops the process group.
	 */
	while (!r.done) {
		if (fyai_interrupt_pending(ctx)) {
			fyai_event_interrupt_ack(ctx);
			tty_cancel(&r);
		}
		rc = fyai_event_loop_step(el, -1);
		if (rc < 0)
			break;
	}
	ctx->tty_session = NULL;

	tty_sources_remove(&r);

	result->output = fyai_terminal_view_read(r.view, FYAITR_ALL, NULL,
						&result->output_len);
	result->screen_mode = fyai_terminal_view_screen_mode(r.view);
	result->binary = fyai_terminal_view_binary(r.view);
	result->raw_bytes = fyai_terminal_view_raw_bytes(r.view);
	result->rows = r.rows;
	result->cols = r.cols;
	result->timed_out = r.timed_out;
	result->cancelled = r.cancelling && !r.timed_out;
	if (r.reaped && WIFSIGNALED(r.status)) {
		result->signaled = true;
		result->signal = WTERMSIG(r.status);
	} else if (r.reaped && WIFEXITED(r.status)) {
		result->exit_code = WEXITSTATUS(r.status);
	}

	rc = result->output ? 0 : -1;
	fyai_terminal_view_destroy(r.view);
	close(r.master);
	return rc;

fail_child:
	(void)kill(-pid, SIGKILL);
	tty_sources_remove(&r);
	(void)waitpid(pid, NULL, 0);
	if (r.master >= 0)
		close(r.master);
fail_view:
	fyai_terminal_view_destroy(r.view);
	return -1;
}

/*
 * Apply a new size to the session in this process. TIOCSWINSZ makes the kernel
 * send SIGWINCH to the program. fyvt_set_size() keeps the interpretation at
 * the size that the program now draws. A line that the reflow pushes off the
 * top arrives through the scrollback callback as a continuation.
 */
void fyai_terminal_session_resize(struct fyai_ctx *ctx, int rows, int cols)
{
	struct tty_run *r;
	struct winsize ws = {};

	if (!ctx || rows <= 0 || cols <= 0)
		return;
	r = ctx->tty_session;
	if (!r || (rows == r->rows && cols == r->cols))
		return;

	r->rows = rows;
	r->cols = cols;
	ws.ws_row = (unsigned short)rows;
	ws.ws_col = (unsigned short)cols;
	(void)ioctl(r->master, TIOCSWINSZ, &ws);
	fyai_terminal_view_resize(r->view, rows, cols);
}

static enum fyai_event_action tty_winch(const struct fyai_event *ev)
{
	struct fyai_ctx *ctx = ev->userdata;
	int rows = 0, cols = 0;

	if (!terminal_window_size(STDOUT_FILENO, &rows, &cols) &&
	    !terminal_window_size(STDERR_FILENO, &rows, &cols))
		return FYAIEA_CONTINUE;
	if (rows == ctx->tty_rows && cols == ctx->tty_cols)
		return FYAIEA_CONTINUE;

	ctx->tty_rows = rows;
	ctx->tty_cols = cols;
	fyai_terminal_session_resize(ctx, rows, cols);
	fyai_tool_jobs_resize(ctx, rows, cols);
	return FYAIEA_CONTINUE;
}

int fyai_terminal_winch_open(struct fyai_ctx *ctx)
{
	struct fyai_event_loop *el;

	if (!ctx || ctx->winch_src)
		return 0;
	/* A tool child has no terminal of its own; the parent tells it. */
	if (ctx->cfg->tool_child)
		return 0;

	el = fyai_ctx_loop(ctx);
	if (!el)
		return -1;
	return fyai_event_add_signal(el, SIGWINCH, tty_winch, ctx,
				     &ctx->winch_src);
}

void fyai_terminal_winch_close(struct fyai_ctx *ctx)
{
	if (!ctx || !ctx->winch_src)
		return;
	fyai_event_source_remove(ctx->winch_src);
	ctx->winch_src = NULL;
}

void fyai_terminal_result_cleanup(struct fyai_terminal_result *result)
{
	free(result->output);
	memset(result, 0, sizeof(*result));
}

/* Drive a session PTY while the parent interprets and renders its bytes. */
struct fyai_terminal_relay {
	struct fyai_ctx *ctx;
	struct jsonrpc_conn *conn;
	int master;			/* what the program writes; read here */
	int input;			/* what it reads; the same fd on a PTY */
	pid_t pid;
	int status;
	bool reaped;
	bool done;
	bool closing;
	struct fyai_event_source *fdsrc;
	struct fyai_event_source *childsrc;
	struct fyai_event_source *killer;
};

/* A terminal is one descriptor; a pipe session has one for each direction. */
static void relay_fds_close(struct fyai_terminal_relay *rl)
{
	if (rl->input >= 0 && rl->input != rl->master)
		close(rl->input);
	rl->input = -1;
	if (rl->master >= 0)
		close(rl->master);
	rl->master = -1;
}

static void relay_notify(struct fyai_terminal_relay *rl, const char *method,
			 fy_generic params)
{
	if (!rl->conn)
		return;
	(void)jsonrpc_notify(rl->conn, method, params);
}

/* Tell the parent how the program ended, then stop serving. */
static void relay_finish(struct fyai_terminal_relay *rl)
{
	struct fy_generic_builder *gb;

	if (rl->done)
		return;
	rl->done = true;

	gb = fyai_ctx_transient_gb(rl->ctx);
	if (!gb)
		return;
	if (WIFSIGNALED(rl->status))
		relay_notify(rl, "shell/exit",
			     fy_gb_mapping(gb, "signal",
					   (long long)WTERMSIG(rl->status)));
	else
		relay_notify(rl, "shell/exit",
			     fy_gb_mapping(gb, "exit_code",
					   (long long)WEXITSTATUS(rl->status)));
}

static enum fyai_event_action relay_read(const struct fyai_event *ev)
{
	struct fyai_terminal_relay *rl = ev->userdata;
	struct fy_generic_builder *gb;
	char buf[4096];
	ssize_t n;

	gb = fyai_ctx_transient_gb(rl->ctx);

	for (;;) {
		n = read(rl->master, buf, sizeof(buf));
		if (n > 0) {
			relay_notify(rl, "shell/output",
				     fyai_bytes_to_generic(gb, buf,
							   (size_t)n));
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EINTR))
			return FYAIEA_CONTINUE;
		/* EIO is the last slave closing: the end of the stream. */
		if (rl->fdsrc) {
			fyai_event_source_remove(rl->fdsrc);
			rl->fdsrc = NULL;
		}
		if (rl->reaped)
			relay_finish(rl);
		return FYAIEA_CONTINUE;
	}
}

static enum fyai_event_action relay_child(const struct fyai_event *ev)
{
	struct fyai_terminal_relay *rl = ev->userdata;

	rl->status = ev->status;
	rl->reaped = true;
	if (rl->childsrc) {
		fyai_event_source_remove(rl->childsrc);
		rl->childsrc = NULL;
	}
	/* A terminal can still hold output after the program is reaped. */
	if (!rl->fdsrc)
		relay_finish(rl);
	return FYAIEA_CONTINUE;
}

static enum fyai_event_action relay_kill(const struct fyai_event *ev)
{
	struct fyai_terminal_relay *rl = ev->userdata;

	(void)kill(-rl->pid, SIGKILL);
	return FYAIEA_CONTINUE;
}

struct fyai_terminal_relay *
fyai_terminal_relay_start(struct fyai_ctx *ctx, struct jsonrpc_conn *conn,
			  const char *command,
			  const struct fyai_sandbox_spec *sandbox,
			  const struct fyai_terminal_opts *opts)
{
	char why_buf[FYAI_CHILD_START_TEXT_MAX];
	struct fyai_child_start start = {};
	struct fyai_terminal_relay *rl;
	struct fy_generic_builder *gb;
	struct fyai_event_loop *el;
	const char *why;
	int rows, cols, rc;

	rl = calloc(1, sizeof(*rl));
	if (!rl) {
		fyai_error(ctx, "shell: out of memory starting the session");
		return NULL;
	}
	rl->ctx = ctx;
	rl->conn = conn;
	rl->master = -1;
	rl->input = -1;

	rows = opts->rows > 0 ? opts->rows : FYAI_TTY_ROWS_DEFAULT;
	cols = opts->cols > 0 ? opts->cols : FYAI_TTY_COLS_DEFAULT;

	/*
	 * Each step reports its cause. A session that cannot start is reported to the
	 * model and to the user. "It could not be opened" gives neither of them a
	 * cause that they can act on.
	 */
	if (opts->pipes) {
		rc = fyai_terminal_pipe_spawn(ctx, command, sandbox, opts,
					      &rl->master, &rl->input,
					      &rl->pid, &start);
		if (rc)
			why = fyai_child_start_text(&start, opts->shell,
						    opts->workdir, why_buf,
						    sizeof(why_buf));
		fyai_error_check(ctx, !rc, fail,
			"shell: could not start '%s' on pipes: %s",
			command ? command : "", why ? why : strerror(errno));
	} else {
		rc = fyai_terminal_pty_spawn(ctx, command, sandbox, opts, rows,
					     cols, &rl->master, &rl->pid,
					     &start);
		if (rc)
			why = fyai_child_start_text(&start, opts->shell,
						    opts->workdir, why_buf,
						    sizeof(why_buf));
		fyai_error_check(ctx, !rc, fail,
			"shell: could not start '%s' on a "
			"pseudo-terminal (%dx%d): %s",
			command ? command : "", rows, cols,
			why ? why : strerror(errno));
		/* One descriptor is both directions of a terminal. */
		rl->input = rl->master;
	}

	el = fyai_ctx_loop(ctx);
	if (!el) {
		fyai_error(ctx, "shell: no event loop for the session");
		goto fail_child;
	}
	rc = fyai_event_add_fd(el, rl->master, FYAIEV_READ, relay_read, rl,
			       &rl->fdsrc);
	if (rc) {
		fyai_error(ctx, "shell: could not watch the session terminal");
		goto fail_child;
	}
	rc = fyai_event_add_child(el, rl->pid, relay_child, rl, &rl->childsrc);
	if (rc) {
		fyai_error(ctx, "shell: could not watch the session process");
		goto fail_child;
	}
	/* Tell the parent which process to watch for input waits. */
	gb = fyai_ctx_transient_gb(ctx);
	if (gb)
		relay_notify(rl, "shell/started",
			     fy_gb_mapping(gb, "pid", (long long)rl->pid));
	return rl;

fail_child:
	(void)kill(-rl->pid, SIGKILL);
	(void)waitpid(rl->pid, NULL, 0);
fail:
	if (rl->fdsrc)
		fyai_event_source_remove(rl->fdsrc);
	if (rl->childsrc)
		fyai_event_source_remove(rl->childsrc);
	relay_fds_close(rl);
	free(rl);
	return NULL;
}

int fyai_terminal_relay_write(struct fyai_terminal_relay *rl, const char *data,
			      size_t len)
{
	size_t done = 0;
	ssize_t n;

	if (!rl || rl->input < 0 || !data)
		return -1;

	/* Input is a keystroke or a line; a short write is retried at once. */
	while (done < len) {
		n = write(rl->input, data + done, len - done);
		if (n > 0) {
			done += (size_t)n;
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		return -1;
	}
	return 0;
}

void fyai_terminal_relay_resize(struct fyai_terminal_relay *rl, int rows,
				int cols)
{
	struct winsize ws = {};

	/* A pipe has no size, so there is nothing to tell the program. */
	if (!rl || rl->input != rl->master || rl->master < 0 ||
	    rows <= 0 || cols <= 0)
		return;
	ws.ws_row = (unsigned short)rows;
	ws.ws_col = (unsigned short)cols;
	(void)ioctl(rl->master, TIOCSWINSZ, &ws);
}

/* End the session gracefully, or kill it immediately when forced. */
void fyai_terminal_relay_close(struct fyai_terminal_relay *rl, bool force)
{
	struct fyai_event_loop *el;

	if (!rl || rl->reaped || rl->done)
		return;
	if (force) {
		(void)kill(-rl->pid, SIGKILL);
		return;
	}
	if (rl->closing)
		return;
	rl->closing = true;
	/* The session ends, so its terminal hangs up. An interactive shell
	 * ignores SIGTERM and answers only the hangup. */
	(void)kill(-rl->pid, SIGHUP);
	(void)kill(-rl->pid, SIGTERM);

	el = fyai_ctx_loop(rl->ctx);
	if (el && !rl->killer)
		(void)fyai_event_add_timer(el, FYAI_TTY_KILL_GRACE_MS, 0,
					   relay_kill, rl, &rl->killer);
}

bool fyai_terminal_relay_done(const struct fyai_terminal_relay *rl)
{
	return !rl || rl->done;
}

bool fyai_terminal_relay_reaped(const struct fyai_terminal_relay *rl)
{
	return !rl || rl->reaped;
}

void fyai_terminal_relay_destroy(struct fyai_terminal_relay *rl)
{
	if (!rl)
		return;
	if (!rl->reaped && rl->pid > 0) {
		(void)kill(-rl->pid, SIGKILL);
		while (waitpid(rl->pid, NULL, 0) < 0 && errno == EINTR)
			;
	}
	if (rl->killer)
		fyai_event_source_remove(rl->killer);
	if (rl->fdsrc)
		fyai_event_source_remove(rl->fdsrc);
	if (rl->childsrc)
		fyai_event_source_remove(rl->childsrc);
	relay_fds_close(rl);
	free(rl);
}
