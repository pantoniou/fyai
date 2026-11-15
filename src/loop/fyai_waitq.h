/*
 * fyai_waitq.h - the waits component
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_WAITQ_H
#define FYAI_WAITQ_H

#include <stdarg.h>
#include <stdbool.h>

struct fyai_diag;
struct fyai_event_source;
struct fyai_loop;
struct fyai_pending_event;
struct fyai_wait;
struct fyai_waiter;
struct fyai_waitq;

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
	/*
	 * Called up: a queued event wakes the waiters that hold a call, and
	 * @owner_live says whether the job or session that an event names is
	 * still there. Without them the queue wakes nobody and takes every
	 * owner as live.
	 */
	void (*kick)(void *data);
	bool (*owner_live)(void *data, int owner_kind, const char *owner);
	void *hook_data;
};

void fyai_waitq_init(struct fyai_waitq *q, struct fyai_loop *loop,
		     struct fyai_diag *diag);
/* A forked child forgets the named waits of its parent and what they queued. */
void fyai_waitq_abandon(struct fyai_waitq *q);
/* Wake the waiters that hold a call, if the run said how. */
void fyai_waitq_kick(struct fyai_waitq *q);

/* Queue owned @text for the event-loop owner to submit between turns. */
int fyai_event_inject(struct fyai_waitq *q, char *text);
/* The same, with the text made from a format. Return 0, or -1 on failure. */
int fyai_event_injectf(struct fyai_waitq *q, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
int fyai_event_vinjectf(struct fyai_waitq *q, const char *fmt, va_list ap)
	__attribute__((format(printf, 2, 0)));
/*
 * What a queued event reports for. A wait event names its owner: the
 * report reaches the model after the poll that queued it, and the wait
 * may be gone by then. The owner pairs the report with the liveness of
 * the job or session it came from: a stale wait is dropped instead of
 * starting a turn for a program that has ended.
 */
enum fyai_event_owner_kind {
	FYAI_EVENT_OWNER_NONE,		/* owned by nobody: always delivered */
	FYAI_EVENT_OWNER_AGENT,		/* branch of a live sub-agent job */
	FYAI_EVENT_OWNER_SESSION,	/* name of a live shell session */
};
/* Queue owned @text with its owner; the caller gives away @owner. */
int fyai_event_inject_owned(struct fyai_waitq *q, char *text,
			    enum fyai_event_owner_kind owner_kind,
			    char *owner);
/* Agent input keeps its sender separate from model-visible text. */
struct fyai_event_input {
	char *text;
	char *from;
	char *message_id;
	char *reply_to;
	long long execution;
};
int fyai_event_inject_agent(struct fyai_waitq *q, const char *text,
			    const char *from, long long execution,
			    const char *message_id, const char *reply_to);
bool fyai_event_take_live_input(struct fyai_waitq *q,
				struct fyai_event_input *input);
void fyai_event_input_clear(struct fyai_event_input *input);
/* Take the oldest queued event, or NULL. The caller owns it. */
char *fyai_event_take(struct fyai_waitq *q);
bool fyai_event_queued(const struct fyai_waitq *q);
/* Whether a live event whose text starts with @prefix is queued. */
bool fyai_event_pending_prefix(struct fyai_waitq *q, const char *prefix);
/*
 * Copy the oldest live event whose text starts with @prefix, or return NULL.
 * The event stays queued and is marked as seen by a waiter: every waiter that
 * asks while the mark stands gets the same event, and the turn loop does not
 * deliver it. fyai_events_purge_seen() removes the marked events.
 */
char *fyai_event_peek_prefix(struct fyai_waitq *q, const char *prefix);
/* Remove the events that waiters have seen. */
void fyai_events_purge_seen(struct fyai_waitq *q);
/* Take the oldest event with a live owner, dropping stale waits first. */
char *fyai_event_take_live(struct fyai_waitq *q);
void fyai_events_release(struct fyai_waitq *q);
/* Drop the queued wait events of one settled owner. */
void fyai_events_drop_agent(struct fyai_waitq *q, const char *branch);
void fyai_events_drop_session(struct fyai_waitq *q, const char *name);



#endif
