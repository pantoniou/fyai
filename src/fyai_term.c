/*
 * fyai_term.c - the terminal verb
 *
 * A program runs on a pseudo-terminal. libfyvterm interprets what the program
 * draws into a fyai_terminal_view, and the view is published to a libfytimui
 * surface, which draws it. The bytes of the program do not reach the terminal.
 * The user sees the screen that fyai holds, which is the screen that a model
 * reads.
 *
 * Nothing here owns the terminal. A surface is a band like any other, so this
 * verb uses the path of a watched sub-agent. There is one difference: this
 * surface holds the keys, because a user drives it.
 *
 * SPDX-License-Identifier: MIT
 */
#define FYAI_MODULE FYAIEM_TOOLS
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "fyai.h"
#include "fyai_event.h"
#include "fyai_term.h"
#include "fyai_terminal.h"
#include "fyai_terminal_session.h"
#include "fyai_terminal_view.h"
#include "fyai_ui.h"
#include "commands.h"
#include "utils.h"

/* The key that belongs to fyai and not to the program: Ctrl-\. */
#define FYAI_TERM_PREFIX	0x1c
/*
 * Rows that the band uses for something other than the program. A surface that
 * holds the keys has no prompt, no separators and no empty chrome. The state
 * row of the surface is then the only row that the program does not have. A
 * request for too many rows is safe, because the band grants the ceiling.
 */
#define FYAI_TERM_CHROME_ROWS	1

/* How long the loop waits before it looks at the window again. */
#define FYAI_TERM_FRAME_MS	50
/*
 * The maximum time to wait until the band grants the rows that a new window
 * size requested. The granted rows apply to the last layout of the band.
 * Thus before the band answers, they still give the previous window size.
 */
#define FYAI_TERM_SETTLE_MS	1000
/* How long the program is given to go before it is killed outright. */
#define FYAI_TERM_EXIT_WAIT_MS	(FYAI_TERM_KILL_GRACE_MS + 1000)

/* Time the program gets to leave after it is asked to. */
#define FYAI_TERM_KILL_GRACE_MS	2000

struct fyai_term {
	struct fyai_ctx *ctx;
	struct fyai_terminal_view *view;
	struct fytim_surface *surface;
	struct fyai_terminal_opts opts;
	struct response_buffer input;	/* keys the program could not take yet */
	const char *command;
	int master;
	pid_t pid;
	int rows;			/* the rows the program is given */
	int term_rows;			/* the window height that size came from */
	int asked;			/* rows last requested from the band */
	fyai_event_ms_t settle_until;	/* time after which a grant is used */
	int cols;
	int status;
	bool prefix;			/* the next key is for fyai */
	bool reaped;
	bool drained;
	bool done;
	bool cancelling;
	bool hold;			/* wait for a key after the program ends */
	bool ended;			/* the program ended and was reported */
	bool quitting;			/* the user asked to leave */
	char state_row[512];		/* the state row as it was last set */
	struct fyai_event_source *ptysrc;
	struct fyai_event_source *childsrc;
	struct fyai_event_source *killer;
};

/* Describe the state of the program for the status row. */
static void term_status_text(const struct fyai_term *t, char *buf, size_t len)
{
	char state[48];

	if (!t->reaped)
		snprintf(state, sizeof(state), "running");
	else if (WIFSIGNALED(t->status))
		snprintf(state, sizeof(state), "signal %d", WTERMSIG(t->status));
	else
		snprintf(state, sizeof(state), "exit %d",
			 WIFEXITED(t->status) ? WEXITSTATUS(t->status) : 0);

	snprintf(buf, len, " fyai term  %s  %dx%d  %s  %s", state,
		 t->rows, t->cols,
		 t->command && *t->command ? t->command : "shell",
		 t->reaped && t->hold ? "^\\ q or any key to leave"
				      : "^\\ q quit  ^\\ r redraw");
}

/* Write the reply of the view to the terminal of the program. */
static void term_view_reply(const char *data, size_t len, void *user)
{
	struct fyai_term *t = user;

	fyai_terminal_reply_write(t->master, data, len);
}

/*
 * Give the program, the view and the surface one size. The program is told
 * with TIOCSWINSZ, which is what makes the kernel send it SIGWINCH.
 */
static void term_apply_size(struct fyai_term *t, int rows, int cols)
{
	struct winsize ws = {};

	if (rows == t->rows && cols == t->cols)
		return;

	t->rows = rows;
	t->cols = cols;
	ws.ws_row = (unsigned short)t->rows;
	ws.ws_col = (unsigned short)t->cols;
	(void)ioctl(t->master, TIOCSWINSZ, &ws);
	fyai_terminal_view_resize(t->view, t->rows, t->cols);
	(void)fyai_ui_surface_resize(t->surface, t->rows, t->cols);
}

/* Fit the program to the current surface without chasing band-height
 * changes. */
static void term_follow_window(struct fyai_term *t)
{
	int cols = 0, term_rows = 0;
	int rows, granted;

	if (fyai_ui_size(t->ctx, &cols, &term_rows) || cols < 1)
		return;

	rows = t->rows;
	if (term_rows != t->term_rows) {
		/*
		 * The window changed size. The program gets all of it except
		 * the chrome rows. The band did not get this request yet.
		 */
		t->term_rows = term_rows;
		t->asked = term_rows - FYAI_TERM_CHROME_ROWS;
		t->settle_until = fyai_event_now_ms() + FYAI_TERM_SETTLE_MS;
		rows = t->asked;
	} else {
		/*
		 * The granted rows are what the band could supply. They are
		 * an answer only when they agree with the request. Before
		 * that they give the height of the previous window, and no
		 * later change corrects a program that received it.
		 */
		granted = fyai_ui_surface_granted_rows(t->surface);
		if (granted >= 1 && (granted == t->asked ||
				     fyai_event_now_ms() >= t->settle_until)) {
			t->asked = granted;
			rows = granted;
		}
	}
	if (rows < 1)
		rows = 1;
	term_apply_size(t, rows, cols);
}

/* Give the surface what the view now holds, with the state row beneath it. */
static void term_frame(struct fyai_term *t)
{
	char status[sizeof(t->state_row)];
	bool published;

	term_follow_window(t);
	published = fyai_ui_surface_publish(t->surface, t->view) > 0;
	/*
	 * The state row is set apart from the screen: a program that ended
	 * draws nothing more, and that is exactly when the row has news.
	 */
	term_status_text(t, status, sizeof(status));
	if (strcmp(status, t->state_row)) {
		memcpy(t->state_row, status, sizeof(status));
		(void)fyai_ui_surface_set_title(t->surface, NULL, status);
		published = true;
	}
	/* The display paints; this only says that there is something to
	 * paint. */
	if (published)
		fyai_ui_wake(t->ctx);
}

/* Hand queued keys to the program, and watch for writability while any stay. */
static void term_input_flush(struct fyai_term *t)
{
	ssize_t n;

	while (t->input.len) {
		n = write(t->master, t->input.data, t->input.len);
		if (n > 0) {
			memmove(t->input.data, t->input.data + n,
				t->input.len - (size_t)n);
			t->input.len -= (size_t)n;
			t->input.data[t->input.len] = '\0';
			continue;
		}
		if (n < 0 && (errno == EINTR))
			continue;
		break;
	}
	if (!t->ptysrc)
		return;
	(void)fyai_event_fd_modify(t->ptysrc, t->input.len
					      ? FYAIEV_READ | FYAIEV_WRITE
					      : FYAIEV_READ);
}

static void term_input_queue(struct fyai_term *t, const char *data, size_t len)
{
	if (!len)
		return;
	if (response_buffer_append_data(&t->input, data, len))
		return;
	term_input_flush(t);
}

static enum fyai_event_action term_kill(const struct fyai_event *ev);

/* Stop with SIGHUP and SIGTERM, then SIGKILL after the grace period. */
static void term_cancel(struct fyai_term *t)
{
	struct fyai_event_loop *el;

	if (t->cancelling || t->reaped)
		return;
	t->cancelling = true;
	(void)kill(-t->pid, SIGHUP);
	(void)kill(-t->pid, SIGTERM);

	el = fyai_ctx_loop(t->ctx);
	if (el && !t->killer)
		(void)fyai_event_add_timer(el, FYAI_TERM_KILL_GRACE_MS, 0,
					   term_kill, t, &t->killer);
	if (t->killer)
		return;
	(void)kill(-t->pid, SIGKILL);
}

static enum fyai_event_action term_kill(const struct fyai_event *ev)
{
	struct fyai_term *t = ev->userdata;

	(void)kill(-t->pid, SIGKILL);
	return FYAIEA_CONTINUE;
}

/* Interpret the fyai prefix; send every other key to the program. */
static void term_key(struct fyai_term *t, char c)
{
	char both[2];

	if (t->prefix) {
		t->prefix = false;
		switch (c) {
		case 'q':
		case 'Q':
			/* Keep pumping until staged termination completes. */
			t->quitting = true;
			term_cancel(t);
			return;
		case 'r':
		case 'R':
			/* The library owns the repaint; asking the view to
			 * report every row again is all this side can do. */
			fyai_terminal_view_damage_all(t->view);
			return;
		case FYAI_TERM_PREFIX:
			term_input_queue(t, &c, 1);
			return;
		default:
			/* Not a command of fyai: the program gets both keys. */
			both[0] = FYAI_TERM_PREFIX;
			both[1] = c;
			term_input_queue(t, both, sizeof(both));
			return;
		}
	}
	if (c == FYAI_TERM_PREFIX) {
		t->prefix = true;
		return;
	}
	term_input_queue(t, &c, 1);
}

/*
 * The keys of one frame, already the bytes a terminal would send. The prefix
 * is the only one fyai keeps; ^C and Escape are the program's.
 */
static void term_keys(void *user, const char *data, size_t len)
{
	struct fyai_term *t = user;
	size_t i;

	if (t->ended && t->hold) {
		t->done = true;
		return;
	}
	for (i = 0; i < len; i++)
		term_key(t, data[i]);
}

static enum fyai_event_action term_pty(const struct fyai_event *ev)
{
	struct fyai_term *t = ev->userdata;
	char buf[4096];
	ssize_t n;

	if (ev->events & FYAIEV_WRITE)
		term_input_flush(t);

	for (;;) {
		n = read(t->master, buf, sizeof(buf));
		if (n > 0) {
			(void)fyai_terminal_view_feed(t->view, buf, (size_t)n);
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EINTR))
			return FYAIEA_CONTINUE;
		/* EIO is the last slave closing: the end of the stream. */
		if (t->ptysrc) {
			fyai_event_source_remove(t->ptysrc);
			t->ptysrc = NULL;
		}
		t->drained = true;
		return FYAIEA_CONTINUE;
	}
}

/* Read whatever the terminal still holds, without waiting for more. */
static void term_drain(struct fyai_term *t)
{
	char buf[4096];
	ssize_t n;

	for (;;) {
		n = read(t->master, buf, sizeof(buf));
		if (n > 0) {
			(void)fyai_terminal_view_feed(t->view, buf, (size_t)n);
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		return;
	}
}

static enum fyai_event_action term_child(const struct fyai_event *ev)
{
	struct fyai_term *t = ev->userdata;

	t->status = ev->status;
	t->reaped = true;
	/*
	 * The program is gone, so what it drew is all here. A descendant of it
	 * can hold the terminal open, so the end is the child being reaped
	 * and never the stream reaching its end.
	 */
	term_drain(t);
	if (t->childsrc) {
		fyai_event_source_remove(t->childsrc);
		t->childsrc = NULL;
	}
	return FYAIEA_CONTINUE;
}

static void term_sources_remove(struct fyai_term *t)
{
	if (t->killer) {
		fyai_event_source_remove(t->killer);
		t->killer = NULL;
	}
	if (t->ptysrc) {
		fyai_event_source_remove(t->ptysrc);
		t->ptysrc = NULL;
	}
	if (t->childsrc) {
		fyai_event_source_remove(t->childsrc);
		t->childsrc = NULL;
	}
}

/*
 * Keep the screen that the program left, so that a test or a user can read it
 * again.
 */
static int term_screen_save(struct fyai_term *t, const char *path)
{
	size_t len = 0;
	char *text;
	int fd, rc;

	text = fyai_terminal_view_read(t->view, FYAITR_SCREEN, NULL, &len);
	if (!text)
		return -1;
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0) {
		free(text);
		return -1;
	}
	rc = write(fd, text, len) == (ssize_t)len ? 0 : -1;
	if (!rc)
		rc = write(fd, "\n", 1) == 1 ? 0 : -1;
	close(fd);
	free(text);
	return rc;
}

int fyai_term_verb(struct fyai_ctx *ctx)
{
	const struct fyai_term_args *args = &ctx->cfg->cmd.args.term;
	char why_buf[FYAI_CHILD_START_TEXT_MAX];
	struct fyai_child_start start = {};
	struct fyai_event_loop *el;
	struct fyai_term t = {};
	fyai_event_ms_t deadline;
	int rows = 0, cols = 0;
	const char *why;
	int rc, err = -1;

	t.ctx = ctx;
	t.master = -1;
	t.command = args->command;
	t.hold = args->hold;

	/* Give the program the display's usable size, or the fixed default. */
	if (!terminal_window_size(STDOUT_FILENO, &rows, &cols)) {
		rows = 0;
		cols = 0;
	}
	if (rows > 0)
		rows -= FYAI_TERM_CHROME_ROWS;
	t.rows = args->rows > 0 ? args->rows
			        : (rows > 0 ? rows : FYAI_TTY_ROWS_DEFAULT);
	t.cols = args->cols > 0 ? args->cols
			        : (cols > 0 ? cols : FYAI_TTY_COLS_DEFAULT);

	t.opts.rows = t.rows;
	t.opts.cols = t.cols;
	t.opts.shell = args->shell;
	t.opts.login = args->login;

	t.view = fyai_terminal_view_create(ctx, t.rows, t.cols, 0);
	fyai_error_check(ctx, t.view != NULL, err_out,
			 "term: could not create the terminal view");
	fyai_terminal_view_reply_cb(t.view, term_view_reply, &t);

	rc = fyai_ui_open(ctx);
	fyai_error_check(ctx, !rc, err_out,
			 "term: could not open the terminal display");

	t.surface = fyai_ui_surface_open(ctx, t.rows, t.cols);
	fyai_error_check(ctx, t.surface != NULL, err_out,
			 "term: could not open the display surface");

	/* A command run directly by the user is not model-sandboxed. */
	rc = fyai_terminal_pty_spawn(ctx, t.command, NULL, &t.opts, t.rows,
				     t.cols, &t.master, &t.pid, &start);
	why = fyai_child_start_text(&start, t.opts.shell, t.opts.workdir,
				    why_buf, sizeof(why_buf));
	fyai_error_check(ctx, !rc, err_out,
			 "term: could not start the terminal: %s",
			 why ? why : strerror(errno));

	el = fyai_ctx_loop(ctx);
	fyai_error_check(ctx, el != NULL, err_out,
			 "term: no event loop for the terminal");

	rc = fyai_event_add_fd(el, t.master, FYAIEV_READ, term_pty, &t,
			       &t.ptysrc);
	if (!rc)
		rc = fyai_event_add_child(el, t.pid, term_child, &t,
					  &t.childsrc);
	fyai_error_check(ctx, !rc, err_out,
			 "term: could not watch the terminal");

	/* The keys are the program's while it runs. */
	rc = fyai_ui_surface_keys(ctx, t.surface, true, term_keys, &t);
	fyai_error_check(ctx, !rc, err_out,
			 "term: could not take the keys for the terminal");

	while (!t.done) {
		term_frame(&t);
		/* External interrupts stop the program; typed ^C reaches it. */
		if (fyai_interrupt_pending(ctx)) {
			fyai_event_interrupt_ack(ctx);
			t.quitting = true;
			term_cancel(&t);
			continue;
		}
		if (t.reaped && !t.ended) {
			t.ended = true;
			if (!t.hold || t.quitting)
				t.done = true;
			continue;
		}
		/* Wake periodically to sample display-size changes. */
			rc = fyai_event_loop_step(el, FYAI_TERM_FRAME_MS);
		if (rc < 0)
			break;
	}
	term_frame(&t);

	/* Keep pumping until staged termination has reaped the program. */
	term_cancel(&t);
	deadline = fyai_event_now_ms() + FYAI_TERM_EXIT_WAIT_MS;
	while (!t.reaped && fyai_event_now_ms() < deadline) {
		rc = fyai_event_loop_step(el, FYAI_TERM_FRAME_MS);
		if (rc < 0)
			break;
	}
	if (!t.reaped) {
		/* The loop could not finish it: nothing survives this. */
		(void)kill(-t.pid, SIGKILL);
		(void)waitpid(t.pid, &t.status, 0);
		t.reaped = true;
	}
	err = 0;

err_out:
	term_sources_remove(&t);
	/*
	 * The program stopped, but the user was reading its last screen. A commit
	 * keeps that screen in the transcript. A close would remove it with the band.
	 */
	if (t.surface) {
		term_frame(&t);
		fyai_ui_surface_commit(ctx, t.surface);
	}
	if (t.view && args->screen && term_screen_save(&t, args->screen))
		fyai_error(ctx, "term: could not write the screen to '%s'",
			   args->screen);
	fyai_ui_close(ctx);
	if (t.master >= 0)
		close(t.master);
	free(t.input.data);
	fyai_terminal_view_destroy(t.view);
	return err;
}
