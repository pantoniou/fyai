/*
 * fyai_waitq.c - the queue of events for the model
 *
 * SPDX-License-Identifier: MIT
 */
#define FYAI_MODULE FYAIEM_UNKNOWN

#include "config.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai_diag.h"
#include "fyai_waitq.h"

void fyai_waitq_init(struct fyai_waitq *q, struct fyai_loop *loop,
		     struct fyai_diag *diag)
{
	memset(q, 0, sizeof(*q));
	q->loop = loop;
	q->diag = diag;
	q->events_tail = &q->events;
}

void fyai_waitq_kick(struct fyai_waitq *q)
{
	if (q->kick)
		q->kick(q->hook_data);
}

/*
 * Events queued for the model in arrival order. A wait event names its
 * owner: the event reaches the model turns after the poll that queued it,
 * and the condition that produced it may be gone by then. The owner pairs
 * the event with the liveness of the job or session it came from: a stale
 * wait is dropped instead of starting a turn for a program that has ended.
 */
struct fyai_pending_event {
	struct fyai_pending_event *next;
	char *text;
	char *from, *message_id, *reply_to;
	long long execution;
	char *owner;
	enum fyai_event_owner_kind owner_kind;
	bool seen;		/* a waiter has it: the turn loop leaves it */
};

/* The context takes @text and @owner; both are freed on every path. */
static int fyai_event_inject_queue(struct fyai_waitq *q, char *text,
				   enum fyai_event_owner_kind owner_kind,
				   char *owner, const char *from,
				   long long execution,
				   const char *message_id,
				   const char *reply_to)
{
	struct fyai_pending_event *ev;

	if (!q || !text) {
		free(text);
		free(owner);
		return -1;
	}
	ev = calloc(1, sizeof(*ev));
	fyai_diag_check(q->diag, ev, err, "could not queue event");
	ev->text = text;
	if (from) {
		ev->from = strdup(from);
		ev->message_id = strdup(message_id ? message_id : "");
		ev->reply_to = strdup(reply_to ? reply_to : "");
		fyai_diag_check(q->diag, ev->from && ev->message_id && ev->reply_to,
				 err_event, "could not copy event sender metadata");
		ev->execution = execution;
	}
	ev->owner_kind = owner_kind;
	ev->owner = owner;
	if (!q->events_tail)
		q->events_tail = &q->events;
	*q->events_tail = ev;
	q->events_tail = &ev->next;
	fyai_waitq_kick(q);
	return 0;

err_event:
	free(ev->from);
	free(ev->message_id);
	free(ev->reply_to);
	free(ev);
err:
	free(text);
	free(owner);
	return -1;
}

int fyai_event_inject(struct fyai_waitq *q, char *text)
{
	return fyai_event_inject_queue(q, text, FYAI_EVENT_OWNER_NONE,
				       NULL, NULL, 0, NULL, NULL);
}

int fyai_event_inject_agent(struct fyai_waitq *q, const char *text,
			    const char *from, long long execution,
			    const char *message_id, const char *reply_to)
{
	char *copy = text ? strdup(text) : NULL;

	return fyai_event_inject_queue(q, copy, FYAI_EVENT_OWNER_NONE,
		NULL, from, execution, message_id, reply_to);
}

int fyai_event_vinjectf(struct fyai_waitq *q, const char *fmt, va_list ap)
{
	char *text;
	int rc;

	rc = vasprintf(&text, fmt, ap);
	fyai_diag_check(q->diag, rc >= 0, err, "could not format an event");
	return fyai_event_inject(q, text);
err:
	return -1;
}

int fyai_event_injectf(struct fyai_waitq *q, const char *fmt, ...)
{
	va_list ap;
	int rc;

	va_start(ap, fmt);
	rc = fyai_event_vinjectf(q, fmt, ap);
	va_end(ap);
	return rc;
}

/* Queue owned @text for the loop owner; the caller gives away @owner. */
int fyai_event_inject_owned(struct fyai_waitq *q, char *text,
			    enum fyai_event_owner_kind owner_kind,
			    char *owner)
{
	return fyai_event_inject_queue(q, text, owner_kind, owner,
		NULL, 0, NULL, NULL);
}

void fyai_event_input_clear(struct fyai_event_input *input)
{
	if (!input)
		return;
	free(input->text);
	free(input->from);
	free(input->message_id);
	free(input->reply_to);
	memset(input, 0, sizeof(*input));
}

static void fyai_pending_event_free(struct fyai_pending_event *ev)
{
	if (!ev)
		return;
	free(ev->text);
	free(ev->from);
	free(ev->message_id);
	free(ev->reply_to);
	free(ev->owner);
	free(ev);
}

char *fyai_event_take(struct fyai_waitq *q)
{
	struct fyai_pending_event *ev;
	char *text;

	if (!q || !q->events)
		return NULL;
	ev = q->events;
	q->events = ev->next;
	if (!q->events)
		q->events_tail = &q->events;
	text = ev->text;
	ev->text = NULL;
	fyai_pending_event_free(ev);
	return text;
}

bool fyai_event_queued(const struct fyai_waitq *q)
{
	const struct fyai_pending_event *ev;

	for (ev = q ? q->events : NULL; ev; ev = ev->next)
		if (!ev->seen)
			return true;
	return false;
}

/*
 * Whether a queued wait event still names a live owner. Unowned events
 * (waits, agent control input) are not wait reports and are always
 * delivered. A wait whose owner settled is dropped with a trace record.
 */
static bool fyai_event_owner_live(struct fyai_waitq *q,
				  const struct fyai_pending_event *ev)
{
	if (!ev || ev->owner_kind == FYAI_EVENT_OWNER_NONE)
		return true;
	/* Without a way to ask, the owner is taken as live. */
	if (!q->owner_live)
		return true;
	return q->owner_live(q->hook_data, ev->owner_kind, ev->owner);
}

bool fyai_event_pending_prefix(struct fyai_waitq *q, const char *prefix)
{
	const struct fyai_pending_event *ev;
	size_t len = strlen(prefix);

	for (ev = q ? q->events : NULL; ev; ev = ev->next)
		if (!strncmp(ev->text, prefix, len) &&
		    fyai_event_owner_live(q, ev))
			return true;
	return false;
}

char *fyai_event_peek_prefix(struct fyai_waitq *q, const char *prefix)
{
	struct fyai_pending_event *ev;
	size_t len = strlen(prefix);
	char *text;

	for (ev = q ? q->events : NULL; ev; ev = ev->next) {
		if (strncmp(ev->text, prefix, len) ||
		    !fyai_event_owner_live(q, ev))
			continue;
		text = strdup(ev->text);
		if (text)
			ev->seen = true;
		return text;
	}
	return NULL;
}

void fyai_events_purge_seen(struct fyai_waitq *q)
{
	struct fyai_pending_event **link, *ev;

	if (!q)
		return;
	link = &q->events;
	while ((ev = *link)) {
		if (!ev->seen) {
			link = &ev->next;
			continue;
		}
		*link = ev->next;
		if (!*link)
			q->events_tail = link;
		fyai_pending_event_free(ev);
	}
}

/* Drop every queued wait event owned by @name of @kind. */
static void fyai_events_drop_owner(struct fyai_waitq *q,
				   enum fyai_event_owner_kind kind,
				   const char *name)
{
	struct fyai_pending_event **link, *ev;

	if (!q || kind == FYAI_EVENT_OWNER_NONE || !name || !*name)
		return;
	link = &q->events;
	while ((ev = *link) != NULL) {
		if (ev->owner_kind != kind || !ev->owner ||
		    strcmp(ev->owner, name)) {
			link = &ev->next;
			continue;
		}
		*link = ev->next;
		if (!*link)
			q->events_tail = link;
		fyai_diag_tracef("event", "dropped stale %s wait from '%s'",
				 kind == FYAI_EVENT_OWNER_AGENT ?
				 "agent" : "session", name);
		fyai_pending_event_free(ev);
	}
}

void fyai_events_release(struct fyai_waitq *q)
{
	char *text;

	while ((text = fyai_event_take(q)))
		free(text);
}

/* Drop the queued wait events of the sub-agent on @branch. */
void fyai_events_drop_agent(struct fyai_waitq *q, const char *branch)
{
	fyai_events_drop_owner(q, FYAI_EVENT_OWNER_AGENT, branch);
}

/* Drop the queued wait events of the shell session @name. */
void fyai_events_drop_session(struct fyai_waitq *q, const char *name)
{
	fyai_events_drop_owner(q, FYAI_EVENT_OWNER_SESSION, name);
}

/*
 * Take the oldest queued event whose owner is still live, dropping stale
 * wait events in front of it. A wait event names a job or session that may
 * have settled in the gap between the poll that queued it and this turn:
 * starting a turn for it would ask the model to answer a program that has
 * ended. Each drop is traced; delivery of an unowned event never changes.
 */
bool fyai_event_take_live_input(struct fyai_waitq *q,
				struct fyai_event_input *input)
{
	struct fyai_pending_event **link, *ev;

	memset(input, 0, sizeof(*input));
	if (!q)
		return false;
	link = &q->events;
	while ((ev = *link)) {
		if (ev->seen) {
			link = &ev->next;
			continue;
		}
		*link = ev->next;
		if (!*link)
			q->events_tail = link;
		if (fyai_event_owner_live(q, ev)) {
			input->text = ev->text;
			input->from = ev->from;
			input->message_id = ev->message_id;
			input->reply_to = ev->reply_to;
			input->execution = ev->execution;
			ev->text = ev->from = ev->message_id = ev->reply_to = NULL;
			fyai_pending_event_free(ev);
			return true;
		}
		fyai_diag_tracef("event", "dropped stale %s wait from '%s'",
				 ev->owner_kind == FYAI_EVENT_OWNER_AGENT ?
				 "agent" : "session",
				 ev->owner ? ev->owner : "?");
		fyai_pending_event_free(ev);
	}
	return false;
}

char *fyai_event_take_live(struct fyai_waitq *q)
{
	struct fyai_event_input input;
	char *text;

	if (!fyai_event_take_live_input(q, &input))
		return NULL;
	text = input.text;
	input.text = NULL;
	fyai_event_input_clear(&input);
	return text;
}

