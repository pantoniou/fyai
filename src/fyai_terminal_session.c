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
#include <pty.h>
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
	/* A PTY can still hold output after the child has been reaped. */
	if (!r->fdsrc)
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
 * Stop the command as the pipe path stops it. Send SIGTERM to the group, then
 * SIGKILL after a grace time, so that a full-screen program can restore the
 * terminal. Call this one time.
 */
static void tty_cancel(struct tty_run *r)
{
	struct fyai_event_loop *el;

	if (r->cancelling || r->reaped)
		return;
	r->cancelling = true;
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

/* Describe the terminal the child is given. */
static void tty_child_setenv(const struct fyai_terminal_opts *opts)
{
	char num[16];

	setenv("TERM", "xterm-256color", 1);
	snprintf(num, sizeof(num), "%d",
		 opts->rows > 0 ? opts->rows : FYAI_TTY_ROWS_DEFAULT);
	setenv("LINES", num, 1);
	snprintf(num, sizeof(num), "%d",
		 opts->cols > 0 ? opts->cols : FYAI_TTY_COLS_DEFAULT);
	setenv("COLUMNS", num, 1);
}

/*
 * Run the program. With a command it is `sh -c`, as for each other command
 * that this program runs. Without a command it is the shell itself, which a
 * user drives. A login shell then starts as a terminal starts one, with a
 * leading dash in argv[0], which makes it read the profile.
 */
static void tty_child_shell(const char *command,
			    const struct fyai_terminal_opts *opts)
{
	const char *shell = opts->shell && *opts->shell ? opts->shell
							: "/bin/sh";
	char argv0[64];
	const char *base;

	base = strrchr(shell, '/');
	base = base ? base + 1 : shell;

	if (command && *command) {
		execl(shell, base, "-c", command, (char *)NULL);
		return;
	}
	snprintf(argv0, sizeof(argv0), "%s%s", opts->login ? "-" : "", base);
	execl(shell, argv0, (char *)NULL);
}

/* The child side of the fork. It never returns. */
static void tty_child_exec(struct fyai_ctx *ctx, const char *command,
			   const struct fyai_sandbox_spec *sandbox,
			   const struct fyai_terminal_opts *opts, int slave)
{
	fyai_ctx_loop_abandon(ctx);

	if (setsid() < 0)
		_exit(FYAI_SHELL_EXIT_EXEC);
	if (ioctl(slave, TIOCSCTTY, 0) < 0)
		_exit(FYAI_SHELL_EXIT_EXEC);
	if (dup2(slave, STDIN_FILENO) < 0 ||
	    dup2(slave, STDOUT_FILENO) < 0 ||
	    dup2(slave, STDERR_FILENO) < 0)
		_exit(FYAI_SHELL_EXIT_EXEC);
	if (slave > STDERR_FILENO)
		close(slave);
	fyai_close_fds_from(3);
	if (fyai_env_sanitize())
		_exit(FYAI_SHELL_EXIT_EXEC);
	/*
	 * Name the terminal the program is actually given. An inherited TERM
	 * describes the terminal of the user, which libfyvterm does not emulate;
	 * `dumb` would also make a program give up formatting for no reason.
	 */
	tty_child_setenv(opts);
	if (opts->workdir && *opts->workdir && chdir(opts->workdir))
		_exit(FYAI_SHELL_EXIT_WORKDIR);
	if (sandbox && fyai_sandbox_apply(sandbox))
		_exit(FYAI_SHELL_EXIT_SANDBOX);
	tty_child_shell(command, opts);
	_exit(FYAI_SHELL_EXIT_EXEC);
}

int fyai_terminal_pty_spawn(struct fyai_ctx *ctx, const char *command,
			    const struct fyai_sandbox_spec *sandbox,
			    const struct fyai_terminal_opts *opts, int rows,
			    int cols, int *masterp, pid_t *pidp)
{
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

	pid = fork();
	if (pid < 0)
		goto fail;
	if (!pid) {
		close(master);
		tty_child_exec(ctx, command, sandbox, opts, slave);
	}
	close(slave);

	flags = fcntl(master, F_GETFL, 0);
	if (flags >= 0)
		(void)fcntl(master, F_SETFL, flags | O_NONBLOCK);

	*masterp = master;
	*pidp = pid;
	return 0;

fail:
	if (slave >= 0)
		close(slave);
	if (master >= 0)
		close(master);
	return -1;
}

/*
 * The child of a pipe session. It leads its own process group, as the terminal
 * child does, so that a close stops the program and everything it started.
 */
static void pipe_child_exec(struct fyai_ctx *ctx, const char *command,
			    const struct fyai_sandbox_spec *sandbox,
			    const struct fyai_terminal_opts *opts,
			    int in_fd, int out_fd)
{
	fyai_ctx_loop_abandon(ctx);

	if (setsid() < 0)
		_exit(FYAI_SHELL_EXIT_EXEC);
	if (dup2(in_fd, STDIN_FILENO) < 0 ||
	    dup2(out_fd, STDOUT_FILENO) < 0 ||
	    dup2(out_fd, STDERR_FILENO) < 0)
		_exit(FYAI_SHELL_EXIT_EXEC);
	if (in_fd > STDERR_FILENO)
		close(in_fd);
	if (out_fd > STDERR_FILENO)
		close(out_fd);
	fyai_close_fds_from(3);
	if (fyai_env_sanitize())
		_exit(FYAI_SHELL_EXIT_EXEC);
	/*
	 * There is no terminal here, and TERM must say so: a program that
	 * reads it before it asks the descriptor would otherwise draw for a
	 * terminal that is not there.
	 */
	setenv("TERM", "dumb", 1);
	unsetenv("LINES");
	unsetenv("COLUMNS");
	if (opts->workdir && *opts->workdir && chdir(opts->workdir))
		_exit(FYAI_SHELL_EXIT_WORKDIR);
	if (sandbox && fyai_sandbox_apply(sandbox))
		_exit(FYAI_SHELL_EXIT_SANDBOX);
	tty_child_shell(command, opts);
	_exit(FYAI_SHELL_EXIT_EXEC);
}

int fyai_terminal_pipe_spawn(struct fyai_ctx *ctx, const char *command,
			     const struct fyai_sandbox_spec *sandbox,
			     const struct fyai_terminal_opts *opts,
			     int *readp, int *writep, pid_t *pidp)
{
	int out_pipe[2] = { -1, -1 };
	int in_pipe[2] = { -1, -1 };
	int flags;
	pid_t pid;

	if (pipe(out_pipe)) {
		fyai_error(ctx, "pipe: %s", strerror(errno));
		return -1;
	}
	if (pipe(in_pipe)) {
		fyai_error(ctx, "pipe: %s", strerror(errno));
		goto fail;
	}

	pid = fork();
	if (pid < 0) {
		fyai_error(ctx, "fork: %s", strerror(errno));
		goto fail;
	}
	if (!pid) {
		close(out_pipe[0]);
		close(in_pipe[1]);
		pipe_child_exec(ctx, command, sandbox, opts, in_pipe[0],
				out_pipe[1]);
	}
	close(out_pipe[1]);
	close(in_pipe[0]);

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
	struct tty_run r = {};
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

	r.view = fyai_terminal_view_create(r.rows, r.cols, opts->max_bytes);
	if (!r.view)
		return -1;
	fyai_terminal_view_line_cb(r.view, opts->output_fn, opts->output_data);

	rc = fyai_terminal_pty_spawn(ctx, command, sandbox, opts, r.rows,
				     r.cols, &r.master, &pid);
	if (rc)
		goto fail_view;
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

/*
 * The relay: this process drives the pseudo-terminal, the parent renders it.
 *
 * A session is one process running one program on a terminal, the way a remote
 * shell is. This side moves bytes and applies the window size; it interprets
 * nothing, thus the terminal state lives in the parent and survives this
 * process. Output goes up as a `shell/output` notification, as text when the
 * program writes text and as base64 when it draws.
 */
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

	for (;;) {
		n = read(rl->master, buf, sizeof(buf));
		if (n > 0) {
			gb = fyai_ctx_transient_gb(rl->ctx);
			if (gb)
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
	struct fyai_terminal_relay *rl;
	struct fyai_event_loop *el;
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
					      &rl->pid);
		if (rc) {
			fyai_error(ctx,
				   "shell: could not start '%s' on pipes: %s",
				   command ? command : "", strerror(errno));
			goto fail;
		}
	} else {
		rc = fyai_terminal_pty_spawn(ctx, command, sandbox, opts, rows,
					     cols, &rl->master, &rl->pid);
		if (rc) {
			fyai_error(ctx,
				   "shell: could not start '%s' on a "
				   "pseudo-terminal (%dx%d): %s",
				   command ? command : "", rows, cols,
				   strerror(errno));
			goto fail;
		}
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

	/* A pipe has no size, thus nothing to tell the program. */
	if (!rl || rl->input != rl->master || rl->master < 0 ||
	    rows <= 0 || cols <= 0)
		return;
	ws.ws_row = (unsigned short)rows;
	ws.ws_col = (unsigned short)cols;
	(void)ioctl(rl->master, TIOCSWINSZ, &ws);
}

/*
 * End the session. A program is asked first and killed after the grace time,
 * so that it can restore the terminal; @force does not wait.
 */
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
