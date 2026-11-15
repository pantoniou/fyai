/*
 * fyai_loop.h - the event loop component
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_LOOP_H
#define FYAI_LOOP_H

#include <signal.h>
#include <stdbool.h>
#include <sys/time.h>

struct fyai_diag;
struct fyai_event_loop;
struct fyai_event_source;

/*
 * The event loop and everything that stands on it: its timers and sources,
 * the pools that recycle them, the signals of the invocation, and the
 * interrupt state that a signal handler sets and the pumps read. The
 * component depends on the diagnostics alone. It is made by fyai_loop_init()
 * and its pointers stay valid until fyai_loop_cleanup().
 *
 * The signal handlers and the dump handler are process-wide, so a process has
 * one loop component, as it has one set of signals: the handlers name the
 * component that installed them, and only that one removes them.
 */
struct fyai_loop {
	/* Dependencies. */
	struct fyai_diag *diag;			/* where the loop reports */
	/*
	 * Called on the loop when an interrupt is pending, so that a wait that
	 * stands on the loop looks at it. NULL: nothing to tell.
	 */
	void (*on_interrupt)(void *data);
	void *on_interrupt_data;

	/* The one application loop, made on first use. */
	struct fyai_event_loop *el;
	/* Recycled loops and sources; FYAI_EVENT_NO_POOL turns them off. */
	struct fyai_event_loop *event_loop_pool;
	struct fyai_event_source *event_source_pool;
	/* The signals of the invocation and the mask that they stand on. */
	struct fyai_event_source *signal_src[4];
	sigset_t signal_mask;
	bool signal_mask_valid;
	/* Signal sources: the claims on each signal and what a backend saved. */
	unsigned int signal_refs[NSIG];
	bool signal_was_blocked[NSIG];
	struct sigaction signal_saved_sa[NSIG];		/* kqueue backends */
	/*
	 * The interrupt handlers (SIGINT, and SIGALRM for the watchdog) and the
	 * handler of the dump read this state through the one owner pointer of
	 * fyai_event.c. They compose nothing: the actions that they take are
	 * here, made when the handlers are installed.
	 */
	struct sigaction interrupt_prev_int, interrupt_prev_alrm;
	struct itimerval interrupt_arm;
	volatile sig_atomic_t interrupt_wake_fd;	/* the wake pipe, or -1 */
	volatile sig_atomic_t interrupt_escalated;
	bool interrupt_installed;
	volatile sig_atomic_t dump_out_fd;		/* where the dump goes, or -1 */
	volatile sig_atomic_t dump_crlf;		/* probed when the dump is armed */
	volatile sig_atomic_t dump_busy;
	bool dump_armed;
	/* The SIGINT handler can set this value. */
	volatile sig_atomic_t interrupt_pending;
	/* Count SIGINT edges while interrupt_pending remains set. */
	volatile sig_atomic_t interrupt_seq;
	sig_atomic_t interrupt_seen;
	/* A request that ends the invocation: SIGTERM, or a cancel of an agent. */
	bool terminate_pending;
	/* The diagnostic output of the loop dump (SIGUSR2). */
	int dump_fd;
	char *dump_dir, *dump_file;	/* the dump file; NULL for dump_fd */
};

/* Make the component. @diag is the sink that it reports into. */
void fyai_loop_init(struct fyai_loop *loop, struct fyai_diag *diag);

/*
 * Release what stands on the loop: the interrupt handlers, the application
 * loop, the pools and the dump handler, and drop the hook. The dump output
 * stays open, because a crash report and a late dump still write to it.
 * Call it when every borrower has dropped its sources.
 */
void fyai_loop_release(struct fyai_loop *loop);

/*
 * Release everything, the dump output included. The owner calls it last.
 * Safe to call again, and after fyai_loop_release().
 */
void fyai_loop_cleanup(struct fyai_loop *loop);

/* The one application loop, made on first use and owned by @loop. */
struct fyai_event_loop *fyai_loop_get(struct fyai_loop *loop);

/*
 * Forget what a forked child inherited and does not own: the application
 * loop, which is not destroyed, the sources of the signals of the parent, its
 * copy of the dump output and the dump handler. The child builds its own loop
 * on first use. Safe to call again.
 */
void fyai_loop_abandon(struct fyai_loop *loop);

/* Clear the interrupt state and the terminate request, as a new turn starts. */
void fyai_loop_reset_interrupt(struct fyai_loop *loop);

/* Put back the signal mask that the invocation started with, if it was kept. */
void fyai_loop_restore_signal_mask(const struct fyai_loop *loop);

#endif
