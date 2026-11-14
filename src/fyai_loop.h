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

struct fyai_diag;
struct fyai_event_loop;
struct fyai_event_source;
struct fyai_curl_state;

/*
 * The event loop and everything that stands on it: its timers and sources,
 * the pools that recycle them, the signals of the invocation, and the
 * interrupt state that a signal handler sets and the pumps read. The
 * component depends on the diagnostics alone. It is made by fyai_loop_init()
 * and its pointers stay valid until fyai_loop_cleanup().
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
	/* The SIGINT handler can set this value. */
	volatile sig_atomic_t interrupt_pending;
	/* Count SIGINT edges while interrupt_pending remains set. */
	volatile sig_atomic_t interrupt_seq;
	sig_atomic_t interrupt_seen;
	/* A request that ends the invocation: SIGTERM, or a cancel of an agent. */
	bool terminate_pending;
	/* The curl multi state of the invocation; it stands on the loop. */
	struct fyai_curl_state *curl_state;
	/* The diagnostic output of the loop dump (SIGUSR2). */
	int dump_fd;
	char *dump_dir, *dump_file;	/* the dump file; NULL for dump_fd */
};

/* Make the component. @diag is the sink that it reports into. */
void fyai_loop_init(struct fyai_loop *loop, struct fyai_diag *diag);

/*
 * Release what the loop holds: the interrupt handlers, the application loop,
 * the pools and the dump output. Safe to call again.
 */
void fyai_loop_cleanup(struct fyai_loop *loop);

/* The one application loop, made on first use and owned by @loop. */
struct fyai_event_loop *fyai_loop_get(struct fyai_loop *loop);

/* Drop the inherited loop in a freshly forked child, so it builds its own. */
void fyai_loop_abandon(struct fyai_loop *loop);

/*
 * Forget what a forked child inherited and does not own: the sources of the
 * parent and its copy of the diagnostic output. Ends nothing.
 */
void fyai_loop_disown(struct fyai_loop *loop);

/* Put back the signal mask that the invocation started with, if it was kept. */
void fyai_loop_restore_signal_mask(const struct fyai_loop *loop);

#endif
