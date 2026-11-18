/*
 * fyai_shell_capture.h - shell capture and the start of a child program
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_SHELL_CAPTURE_H
#define FYAI_SHELL_CAPTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

struct fyai_ctx;
struct fyai_sandbox_spec;

/* Stage at which a child failed before exec. */
/*
 * Where a child stopped before it became the program. Each descriptor step
 * has a stage of its own: they fail for reasons that are not the same, and a
 * report that names one of them says which descriptor was not there.
 */
enum fyai_child_stage {
	FYAI_CHILD_STAGE_NONE = 0,	/* it became the program */
	FYAI_CHILD_STAGE_SETUP,		/* descriptors, group, environment */
	FYAI_CHILD_STAGE_SESSION,	/* its own session or process group */
	FYAI_CHILD_STAGE_CTTY,		/* the controlling terminal */
	FYAI_CHILD_STAGE_STDIN,		/* standard input */
	FYAI_CHILD_STAGE_STDOUT,	/* standard output */
	FYAI_CHILD_STAGE_STDERR,	/* standard error */
	FYAI_CHILD_STAGE_STATUS,	/* the status descriptor it reports on */
	FYAI_CHILD_STAGE_ENV,		/* removal of the credentials */
	FYAI_CHILD_STAGE_VIEW,		/* entry to the filesystem view */
	FYAI_CHILD_STAGE_WORKDIR,
	FYAI_CHILD_STAGE_SANDBOX,
	FYAI_CHILD_STAGE_EXEC,		/* the shell itself */
};

struct fyai_child_start {
	enum fyai_child_stage stage;
	int err;			/* errno there, 0 when there is none */
};

struct shell_command_result {
	char *stdout_data;
	char *stderr_data;
	size_t stdout_len;
	size_t stderr_len;
	int exit_code;
	int signal;
	bool signaled;
	bool timed_out;
	/* Set when the command did not start; stage gives the failure point. */
	struct fyai_child_start start;
};

#define FYAI_SHELL_EXIT_WORKDIR	125

/*
 * The text of a shell result for a command that exits with a status. The
 * monitor reads the status back from it, so both use these.
 */
#define FYAI_SHELL_EXIT_PREFIX	"command exited with status "
#define FYAI_SHELL_EXIT_FMT	FYAI_SHELL_EXIT_PREFIX "%d"
#define FYAI_SHELL_EXIT_SANDBOX	126
#define FYAI_SHELL_EXIT_EXEC	127

/* Manage the close-on-exec channel carrying a child-start failure. */
int fyai_child_status_open(int fds[2]);
void fyai_child_status_report(int fd, enum fyai_child_stage stage, int err);
void fyai_child_status_read(int fd, struct fyai_child_start *start);

/* Format a child-start failure in @buf, or return NULL after a successful
 * exec. */
#define FYAI_CHILD_START_TEXT_MAX 512
const char *fyai_child_start_text(const struct fyai_child_start *start,
				  const char *shell, const char *workdir,
				  char *buf, size_t size);

struct fyai_fsview;

/* Options for one shell command. */
struct shell_command_opts {
	const struct fyai_fsview *view; /* optional private project projection */
	const char *workdir;		/* chdir in the child before exec */
	unsigned int timeout_ms;	/* 0 = no limit */
	const char *shell;		/* the program to run; NULL = /bin/sh */
	bool login;			/* start @shell as a login shell */
	const char *const *env_keep;	/* credentials to keep; NULL-terminated */
	/*
	 * Output that a program draws, though it writes to a pipe, is rendered on
	 * a screen of @rows by @cols when the first such byte comes, and the result
	 * is what the screen shows, as a terminal run gives. Both 0 keeps the bytes.
	 * @max_bytes bounds the text that the screen retains; 0 is no bound.
	 */
	int rows, cols;
	size_t max_bytes;
};

/* Exec @command or an interactive shell; return only when exec fails. */
void fyai_exec_shell_command(int status_fd, const char *command,
			     const char *shell,
			     bool login);

enum shell_output_stream {
	SHELL_OUTPUT_STDOUT,
	SHELL_OUTPUT_STDERR,
};

typedef void (*shell_output_fn)(void *userdata, enum shell_output_stream stream,
				const char *data, size_t len);

struct fyai_sandbox_spec;
struct fyai_ctx;
/* @ctx is the diagnostic sink for the capture loop; it must not be NULL. */
int run_shell_command_capture_cb(struct fyai_ctx *ctx, const char *command,
				 struct shell_command_result *result,
				 shell_output_fn output_fn,
				 void *userdata,
				 const struct fyai_sandbox_spec *sandbox,
				 const struct shell_command_opts *opts);
void shell_command_result_cleanup(struct shell_command_result *result);

/* True when @pid or a bounded descendant is blocked reading standard input. */
bool fyai_process_reads_stdin(pid_t pid);

/* Processes looked at in one such walk, of a program and what it started. */
#define FYAI_PROC_WALK_MAX 32

#define FYAI_CHILD_PASS_MAX 5

/* Configuration applied to a forked child before exec. */
struct fyai_child_spec {
	const struct fyai_fsview *view;
	int in_fd;			/* standard input; -1 keeps it */
	int out_fd;			/* standard output; -1 keeps it */
	int err_fd;			/* standard error; -1 keeps it */
	int ctty_fd;			/* >= 0 becomes the controlling tty */
	bool own_session;		/* setsid() instead of a group of its own */
	const char *term;		/* TERM for the child; NULL keeps it */
	int rows;			/* > 0 sets LINES/COLUMNS, else unsets */
	int cols;
	const char *workdir;		/* chdir before confinement; NULL keeps */
	const struct fyai_sandbox_spec *sandbox;
	int status_fd;			/* says why a start stopped; -1 none */
	const char *const *env_keep;	/* credentials to keep; NULL-terminated */
	bool inherit_env;		/* a trusted runtime keeps its environment */
	/*
	 * Descriptors that the child keeps, each under the number in @pass_as.
	 * Every other descriptor above the status descriptor is closed. A kept
	 * descriptor is not close-on-exec in the child.
	 */
	int pass_fd[FYAI_CHILD_PASS_MAX];
	int pass_as[FYAI_CHILD_PASS_MAX];
	size_t pass_n;
	/* The number in the child of the descriptor that announces the view; 0 none. */
	int announce_as;
};

/* Apply @spec before exec, returning the child's failure exit status. */
int fyai_child_exec_prepare(struct fyai_ctx *ctx,
			    const struct fyai_child_spec *spec);

#endif
