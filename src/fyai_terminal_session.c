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

#include <libfyvterm.h>
#include "fyai.h"
#include "fyai_event.h"
#include "fyai_sandbox.h"
#include "fyai_terminal.h"
#include "fyai_tools.h"
#include "fyai_terminal_session.h"
#include "utils.h"

/* Time a program gets to restore the terminal after the limit expires. */
#define FYAI_TTY_KILL_GRACE_MS	2000

struct tty_run {
	struct fyai_ctx *ctx;
	const struct fyai_terminal_opts *opts;
	struct fyvt *vt;
	struct fyvt_screen *screen;
	struct response_buffer sb;	/* lines that left the top of the screen */
	size_t sb_max;			/* compaction threshold, 0 = no limit */
	int rows;
	int cols;
	int master;
	pid_t pid;
	size_t raw_bytes;		/* bytes the program wrote to the PTY */
	bool binary;			/* the raw stream is not text */
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

/* Encode one code point. @out holds at least 4 bytes. */
static size_t tty_put_utf8(char *out, uint32_t cp)
{
	if (cp < 0x80) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = (char)(0xc0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3f));
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = (char)(0xe0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
		out[2] = (char)(0x80 | (cp & 0x3f));
		return 3;
	}
	out[0] = (char)(0xf0 | (cp >> 18));
	out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
	out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
	out[3] = (char)(0x80 | (cp & 0x3f));
	return 4;
}

/*
 * Append @cells as text, without the blank run at the end of the line. A cell
 * holds a base character and up to FYVT_MAX_CHARS_PER_CELL - 1 combining
 * characters, and the text keeps all of them. A double-width glyph occupies
 * its own cell and leaves the next cell unused. Step over that filler cell,
 * and do not make it a space.
 */
static int tty_append_cells(struct response_buffer *out,
			    const struct fyvt_screen_cell *cells, int cols)
{
	char utf8[4];
	size_t n;
	int col, last, i;
	int rc;

	last = -1;
	for (col = 0; col < cols; col++)
		if (cells[col].chars[0])
			last = col;

	for (col = 0; col <= last; col++) {
		if (!cells[col].chars[0]) {
			rc = response_buffer_append_data(out, " ", 1);
			if (rc)
				return -1;
			continue;
		}
		for (i = 0; i < FYVT_MAX_CHARS_PER_CELL; i++) {
			if (!cells[col].chars[i])
				break;
			n = tty_put_utf8(utf8, cells[col].chars[i]);
			rc = response_buffer_append_data(out, utf8, n);
			if (rc)
				return -1;
		}
		if (cells[col].width > 1)
			col += cells[col].width - 1;
	}
	return 0;
}

/*
 * Keep the retained scrollback bounded. A program that never stops must not
 * grow this buffer without end, thus the front half is dropped at a line
 * boundary once the buffer is twice the budget. The end is what the caller
 * keeps anyway.
 */
static void tty_sb_compact(struct tty_run *r)
{
	char *nl;
	size_t keep;

	if (!r->sb_max || r->sb.len <= r->sb_max * 2 || !r->sb.data)
		return;

	nl = memchr(r->sb.data + r->sb.len - r->sb_max, '\n',
		    r->sb_max);
	keep = nl ? r->sb.len - (size_t)(nl + 1 - r->sb.data) : r->sb_max;
	memmove(r->sb.data, r->sb.data + r->sb.len - keep, keep);
	r->sb.len = keep;
	r->sb.data[keep] = '\0';
}

/*
 * A line left the top of the screen: keep it, and show it to the user.
 *
 * One screen row is not one line. A line too long for the screen is stored as
 * several rows, and every row after the first is a continuation. Joining them
 * again keeps the result searchable, thus the separating newline is written
 * before a row that starts a line and not after the row before it.
 */
static int tty_sb_pushline4(int cols, const struct fyvt_screen_cell *cells,
			    bool continuation, void *user)
{
	struct tty_run *r = user;
	size_t start;

	start = r->sb.len;
	if (!continuation && r->sb.len &&
	    response_buffer_append_data(&r->sb, "\n", 1))
		return 0;
	if (tty_append_cells(&r->sb, cells, cols))
		return 0;

	if (r->opts->output_fn && r->sb.len > start)
		r->opts->output_fn(r->opts->output_data, SHELL_OUTPUT_STDOUT,
				   r->sb.data + start, r->sb.len - start);
	tty_sb_compact(r);
	return 1;
}

static const struct fyvt_screen_callbacks tty_screen_callbacks = {
	.sb_pushline4 = tty_sb_pushline4,
};

static enum fyai_event_action tty_read(const struct fyai_event *ev)
{
	struct tty_run *r = ev->userdata;
	char buf[4096];
	ssize_t n;

	for (;;) {
		n = read(r->master, buf, sizeof(buf));
		if (n > 0) {
			/*
			 * A terminal screen is text by construction, thus the
			 * assembled result can never look binary. Test the raw
			 * stream instead, so that a command that writes a blob
			 * is reported and does not become a mangled screen.
			 */
			if (!r->binary && data_is_binary(buf, (size_t)n))
				r->binary = true;
			r->raw_bytes += (size_t)n;
			(void)fyvt_input_write(r->vt, buf, (size_t)n);
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

/* True when screen row @row continues the line above it. */
static bool tty_row_continues(struct tty_run *r, int row)
{
	const struct fyvt_line_info *info;

	info = fyvt_state_get_lineinfo(fyvt_obtain_state(r->vt), row);
	return info && info->continuation;
}

/*
 * Assemble the retained scrollback and the visible screen. A row that
 * continues the line above it is joined to it, so that one long line is one
 * line in the result and not one line for each screen row it needed.
 */
static char *tty_assemble(struct tty_run *r, size_t *lenp)
{
	struct response_buffer out = {};
	struct fyvt_screen_cell *cells;
	struct fyvt_pos pos;
	int row, col;
	int rc;

	cells = calloc((size_t)r->cols, sizeof(*cells));
	if (!cells)
		return NULL;

	if (r->sb.data && r->sb.len &&
	    response_buffer_append_data(&out, r->sb.data, r->sb.len))
		goto fail;

	for (row = 0; row < r->rows; row++) {
		if (out.len && !tty_row_continues(r, row) &&
		    response_buffer_append_data(&out, "\n", 1))
			goto fail;
		for (col = 0; col < r->cols; col++) {
			pos.row = row;
			pos.col = col;
			if (!fyvt_screen_get_cell(r->screen, pos, &cells[col]))
				memset(&cells[col], 0, sizeof(cells[col]));
		}
		rc = tty_append_cells(&out, cells, r->cols);
		if (rc)
			goto fail;
	}
	free(cells);

	/* A screen is mostly blank at the bottom; do not send empty rows. */
	response_buffer_trim(&out);
	/* A command that printed nothing leaves an empty screen. That is a
	 * result, not a failure, thus it must not be a null buffer. */
	if (!out.data) {
		*lenp = 0;
		return strdup("");
	}
	*lenp = out.len;
	return out.data;

fail:
	free(cells);
	free(out.data);
	return NULL;
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
	execl("/bin/sh", "sh", "-c", command, (char *)NULL);
	_exit(FYAI_SHELL_EXIT_EXEC);
}

int fyai_terminal_session_run(struct fyai_ctx *ctx, const char *command,
			      const struct fyai_sandbox_spec *sandbox,
			      const struct fyai_terminal_opts *opts,
			      struct fyai_terminal_result *result)
{
	struct fyai_terminal_opts defaults = {};
	struct fyai_event_loop *el;
	struct winsize ws = {};
	struct tty_run r = {};
	struct fyvt_cfg vtcfg;
	int slave = -1;
	int flags, rc;
	pid_t pid;

	if (!opts)
		opts = &defaults;

	memset(result, 0, sizeof(*result));
	r.ctx = ctx;
	r.opts = opts;
	r.rows = opts->rows > 0 ? opts->rows : FYAI_TTY_ROWS_DEFAULT;
	r.cols = opts->cols > 0 ? opts->cols : FYAI_TTY_COLS_DEFAULT;
	r.sb_max = opts->max_bytes;
	r.master = -1;

	rc = openpty(&r.master, &slave, NULL, NULL, NULL);
	if (rc)
		return -1;

	ws.ws_row = (unsigned short)r.rows;
	ws.ws_col = (unsigned short)r.cols;
	rc = ioctl(slave, TIOCSWINSZ, &ws);
	if (rc)
		goto fail_pty;

	pid = fork();
	if (pid < 0)
		goto fail_pty;
	if (!pid) {
		close(r.master);
		tty_child_exec(ctx, command, sandbox, opts, slave);
	}
	close(slave);
	slave = -1;
	r.pid = pid;

	flags = fcntl(r.master, F_GETFL, 0);
	if (flags >= 0)
		(void)fcntl(r.master, F_SETFL, flags | O_NONBLOCK);

	fyvt_cfg_default(&vtcfg);
	vtcfg.rows = r.rows;
	vtcfg.cols = r.cols;
	r.vt = fyvt_create(&vtcfg);
	if (!r.vt)
		goto fail_child;
	/* fyvt_create() starts in 8-bit mode; without this every byte above 0x7f
	 * is taken as a separate character and text is mangled. */
	fyvt_set_utf8(r.vt, 1);
	r.screen = fyvt_obtain_screen(r.vt);
	fyvt_screen_set_callbacks(r.screen, &tty_screen_callbacks, &r);
	/* Without this the ABI-compatible sb_pushline is called instead, and a
	 * line that needed several rows cannot be joined again. */
	fyvt_screen_callbacks_has_pushline4(r.screen);
	/*
	 * Reflow a line that a resize makes fit differently. It is what the
	 * program sees, and it keeps a joined line joined across a resize.
	 */
	fyvt_screen_enable_reflow(r.screen, true);
	fyvt_screen_reset(r.screen, 1);

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

	result->output = tty_assemble(&r, &result->output_len);
	result->rows = r.rows;
	result->cols = r.cols;
	result->timed_out = r.timed_out;
	result->binary = r.binary;
	result->cancelled = r.cancelling && !r.timed_out;
	result->raw_bytes = r.raw_bytes;
	if (r.reaped && WIFSIGNALED(r.status)) {
		result->signaled = true;
		result->signal = WTERMSIG(r.status);
	} else if (r.reaped && WIFEXITED(r.status)) {
		result->exit_code = WEXITSTATUS(r.status);
	}

	rc = result->output ? 0 : -1;
	free(r.sb.data);
	close(r.master);
	fyvt_destroy(r.vt);
	return rc;

fail_child:
	(void)kill(-pid, SIGKILL);
	tty_sources_remove(&r);
	(void)waitpid(pid, NULL, 0);
	if (r.vt)
		fyvt_destroy(r.vt);
fail_pty:
	free(r.sb.data);
	if (slave >= 0)
		close(slave);
	if (r.master >= 0)
		close(r.master);
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
	fyvt_set_size(r->vt, rows, cols);
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
