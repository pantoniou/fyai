/*
 * fyai_waitq.h - the waits component
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_WAITQ_H
#define FYAI_WAITQ_H

#include <stdbool.h>

struct fyai_diag;
struct fyai_event_source;
struct fyai_loop;
struct fyai_pending_event;
struct fyai_wait;
struct fyai_waiter;

/*
 * The named waits of a run, the waits that hold a call, the timer of the
 * earliest deadline and the queue of events for the model. The component
 * stands on the loop for its timer and sources and on the diagnostics. The
 * owner is struct fyai_app. The queue tail points into the component, so the
 * component does not move.
 */
struct fyai_waitq {
	/* Dependencies. */
	struct fyai_loop *loop;
	struct fyai_diag *diag;

	struct fyai_wait *waits;	/* named waits, live for this run */
	struct fyai_waiter *waiters;	/* waits that hold a call, active */
	struct fyai_event_source *waiter_tick;	/* earliest deadline of a waiter */
	bool waiter_kick;		/* a look at the waiters is queued */
	/* Events queued for model turns in arrival order. */
	struct fyai_pending_event *events;
	struct fyai_pending_event **events_tail;
};

void fyai_waitq_init(struct fyai_waitq *q, struct fyai_loop *loop,
		     struct fyai_diag *diag);

#endif
