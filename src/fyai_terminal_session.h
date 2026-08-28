/* SPDX-License-Identifier: MIT */
#ifndef FYAI_TERMINAL_SESSION_H
#define FYAI_TERMINAL_SESSION_H

#include <stdbool.h>
#include <stddef.h>

#include "utils.h"
#include "fyai_terminal_view.h"

struct fyai_ctx;
struct fyai_sandbox_spec;

/* One command on a pseudo-terminal. */
struct fyai_terminal_opts {
	const char *workdir;		/* chdir in the child before exec */
	unsigned int timeout_ms;	/* 0 = no limit */
	int rows;			/* 0 = FYAI_TTY_ROWS_DEFAULT */
	int cols;			/* 0 = FYAI_TTY_COLS_DEFAULT */
	const char *term;		/* terminal type exposed to the child */
	size_t max_bytes;		/* retained scrollback, 0 = no limit */
	shell_output_fn output_fn;	/* a line that left the screen */
	void *output_data;
	const char *shell;		/* the program to run; NULL = /bin/sh */
	bool login;			/* start @shell as a login shell */
	bool pipes;			/* run on pipes, with no terminal */
};

struct fyai_terminal_result {
	char *output;			/* scrollback then the visible screen */
	size_t output_len;
	int rows;			/* the size the program was given */
	int cols;
	size_t raw_bytes;		/* bytes the program wrote to the PTY */
	int exit_code;
	int signal;
	bool binary;			/* the raw stream was not text */
	bool screen_mode;		/* the program drew a screen */
	bool cancelled;			/* an interrupt stopped the command */
	bool signaled;
	bool timed_out;
	/* Set when the command did not start; stage gives the failure point. */
	struct fyai_child_start start;
};

/* Start a command or interactive shell on a pseudo-terminal. */
int fyai_terminal_pty_spawn(struct fyai_ctx *ctx, const char *command,
			    const struct fyai_sandbox_spec *sandbox,
			    const struct fyai_terminal_opts *opts, int rows,
			    int cols, int *masterp, pid_t *pidp,
			    struct fyai_child_start *startp);

/* Start @command on pipes with standard error joined to standard output. */
int fyai_terminal_pipe_spawn(struct fyai_ctx *ctx, const char *command,
			     const struct fyai_sandbox_spec *sandbox,
			     const struct fyai_terminal_opts *opts,
			     int *readp, int *writep, pid_t *pidp,
			     struct fyai_child_start *startp);

/*
 * Run one command on a pseudo-terminal and return its interpreted screen.
 * The alternate screen is deliberately not enabled, so that the last screen a
 * full-screen program draws stays visible after it exits.
 */
int fyai_terminal_session_run(struct fyai_ctx *ctx, const char *command,
			      const struct fyai_sandbox_spec *sandbox,
			      const struct fyai_terminal_opts *opts,
			      struct fyai_terminal_result *result);
void fyai_terminal_result_cleanup(struct fyai_terminal_result *result);

/* Give the session running in this process a new size. */
void fyai_terminal_session_resize(struct fyai_ctx *ctx, int rows, int cols);

/* Keep the active pseudo-terminal at the user's window size. */
/* Drive a session PTY and notify the parent of output and exit. */
struct fyai_terminal_relay;
struct jsonrpc_conn;

struct fyai_terminal_relay *
fyai_terminal_relay_start(struct fyai_ctx *ctx, struct jsonrpc_conn *conn,
			  const char *command,
			  const struct fyai_sandbox_spec *sandbox,
			  const struct fyai_terminal_opts *opts);
int fyai_terminal_relay_write(struct fyai_terminal_relay *rl, const char *data,
			      size_t len);
void fyai_terminal_relay_resize(struct fyai_terminal_relay *rl, int rows,
				int cols);
void fyai_terminal_relay_resume(struct fyai_terminal_relay *rl);
void fyai_terminal_relay_close(struct fyai_terminal_relay *rl, bool force);
bool fyai_terminal_relay_done(const struct fyai_terminal_relay *rl);
/* True once the program is reaped, whether or not its output is drained. */
bool fyai_terminal_relay_reaped(const struct fyai_terminal_relay *rl);
void fyai_terminal_relay_destroy(struct fyai_terminal_relay *rl);

int fyai_terminal_winch_open(struct fyai_ctx *ctx);
void fyai_terminal_winch_close(struct fyai_ctx *ctx);

#endif
