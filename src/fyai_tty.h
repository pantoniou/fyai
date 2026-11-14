/*
 * fyai_tty.h - the real terminal component
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_TTY_H
#define FYAI_TTY_H

#include <stdbool.h>

struct fyai_diag;
struct fyai_event_source;
struct fyai_loop;

/*
 * The real terminal of the process: whether standard output is one, its size,
 * the source that follows SIGWINCH, and the pseudo-terminal session that runs
 * in this process. The size is recorded by the parent and kept up to date by
 * SIGWINCH. A forked tool child inherits it, because it calls setsid() and can
 * no longer ask the kernel itself. The component stands on the loop and the
 * diagnostics. The owner is struct fyai_app.
 */
struct fyai_tty {
	/* Dependencies. */
	struct fyai_loop *loop;
	struct fyai_diag *diag;

	bool stdout_tty;		/* stdout is a terminal (cached) */
	int rows;			/* 0 = unknown */
	int cols;			/* 0 = unknown */
	void *session;			/* the PTY session running in this process */
	struct fyai_event_source *winch_src;
};

void fyai_tty_init(struct fyai_tty *tty, struct fyai_loop *loop,
		   struct fyai_diag *diag);

#endif
