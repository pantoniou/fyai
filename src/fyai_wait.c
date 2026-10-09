/*
 * fyai_wait.c - the time and wait tools
 *
 * The time tool reports the current clock. The wait tool uses the event loop,
 * keeping the UI responsive and allowing interruption. Named waits return
 * immediately and later notify the model in a new turn. They last only for
 * the current invocation.
 *
 * SPDX-License-Identifier: MIT
 */
#define FYAI_MODULE FYAIEM_TOOLS
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fyai.h"
#include "fyai_event.h"
#include "fyai_wait.h"
#include "fyai_agent.h"
#include "fyai_asks.h"
#include "fyai_tools.h"
#include "fyai_monitor.h"
#include "fyai_tool_registry.h"
#include "utils.h"

/* A name says what is being waited for, as a session name does. */
#define FYAI_WAIT_NAME_MAX	32
/* A synchronous wait must have a finite duration. */
#define FYAI_WAIT_MAX_MS	(6 * 60 * 60 * 1000LL)

struct fyai_wait {
	struct fyai_wait *next;
	struct fyai_ctx *ctx;
	char *name;
	char *reason;
	struct fyai_event_source *timer;
	int64_t due_ms;			/* when it fires, monotonic */
	bool fired;
};

/*
 * The time in the form that a user writes, and in the form that a machine
 * reads.
 */
char *fyai_time_now_text(void)
{
	char local[64], utc[64];
	struct tm tm;
	time_t now;

	now = time(NULL);
	if (!localtime_r(&now, &tm) ||
	    !strftime(local, sizeof(local), "%Y-%m-%dT%H:%M:%S%z", &tm))
		return NULL;
	if (!gmtime_r(&now, &tm) ||
	    !strftime(utc, sizeof(utc), "%Y-%m-%dT%H:%M:%SZ", &tm))
		return NULL;
	return strdup(fy_sprintfa("%s (local)\n%s\nepoch %lld", local, utc,
				  (long long)now));
}

/* Parse a local time and return seconds until it, or -1 if invalid. */
static double fyai_wait_until_seconds(const char *text)
{
	struct tm tm, requested;
	time_t now, then;
	int y, mo, d, h, mi, se;
	double delta;
	int n, used;

	now = time(NULL);
	if (!localtime_r(&now, &tm))
		return -1;

	/* Commit date fields only after the complete date was parsed. */
	used = 0;
	n = sscanf(text, "%d-%d-%dT%d:%d:%d%n", &y, &mo, &d, &h, &mi, &se,
		   &used);
	if (n != 6 || text[used]) {
		used = 0;
		n = sscanf(text, "%d-%d-%dT%d:%d%n", &y, &mo, &d, &h, &mi,
			   &used);
		if (n != 5 || text[used])
			n = 0;
		else
			se = 0;
	}
	if (n >= 5) {
		if (mo < 1 || mo > 12 || d < 1 || d > 31)
			return -1;
		tm.tm_year = y - 1900;
		tm.tm_mon = mo - 1;
		tm.tm_mday = d;
	} else {
		used = 0;
		n = sscanf(text, "%d:%d:%d%n", &h, &mi, &se, &used);
		if (n != 3 || text[used]) {
			used = 0;
			n = sscanf(text, "%d:%d%n", &h, &mi, &used);
			if (n != 2 || text[used])
				return -1;
			se = 0;
		}
	}
	if (h < 0 || h > 23 || mi < 0 || mi > 59 || se < 0 || se > 59)
		return -1;
	tm.tm_hour = h;
	tm.tm_min = mi;
	tm.tm_sec = se;
	tm.tm_isdst = -1;
	requested = tm;
	then = mktime(&tm);
	if (then == (time_t)-1)
		return -1;
	/* mktime normalizes values such as February 30; do not wait for the
	 * different date that normalization produced. */
	if (tm.tm_year != requested.tm_year || tm.tm_mon != requested.tm_mon ||
	    tm.tm_mday != requested.tm_mday || tm.tm_hour != requested.tm_hour ||
	    tm.tm_min != requested.tm_min || tm.tm_sec != requested.tm_sec)
		return -1;
	delta = difftime(then, now);
	return delta > 0 ? delta : 0;
}

/* Read a seconds or nonempty until argument, returning -1 if invalid. */
static double fyai_wait_seconds(fy_generic args, char **whyp)
{
	fy_generic until;
	const char *until_text;
	double seconds;

	*whyp = NULL;
	seconds = fy_number(fy_get(args, "seconds", fy_invalid), -1.0);

	until = fy_get(args, "until", fy_invalid);
	until_text = fy_is_string(until) ? fy_castp(&until, "") : NULL;
	while (until_text && (*until_text == ' ' || *until_text == '\t'))
		until_text++;
	if (until_text && !*until_text)
		until_text = NULL;

	if (seconds >= 0 && until_text) {
		*whyp = strdup(fy_sprintfa("give seconds or until, not both; "
					   "this asked for %.3g seconds and "
					   "until '%s'", seconds, until_text));
		return -1;
	}
	if (until_text) {
		seconds = fyai_wait_until_seconds(until_text);
		if (seconds < 0)
			*whyp = strdup(fy_sprintfa("until must be HH:MM, "
						   "HH:MM:SS or "
						   "YYYY-MM-DDTHH:MM:SS; this "
						   "asked for '%s'",
						   until_text));
		return seconds;
	}
	if (seconds < 0) {
		*whyp = strdup("say how long to wait with seconds or until, or "
				       "give for to hold the turn for work you "
				       "started");
		return -1;
	}
	return seconds;
}

static bool fyai_wait_name_valid(const char *name)
{
	size_t i;

	if (!name || !*name || strlen(name) > FYAI_WAIT_NAME_MAX)
		return false;
	for (i = 0; name[i]; i++) {
		if (isalnum((unsigned char)name[i]) ||
		    strchr("-/_", name[i]))
			continue;
		return false;
	}
	return true;
}

static struct fyai_wait *fyai_wait_find(struct fyai_ctx *ctx, const char *name)
{
	struct fyai_wait *w;

	for (w = ctx->waits; w; w = w->next)
		if (!strcmp(w->name, name))
			return w;
	return NULL;
}

static void fyai_wait_free(struct fyai_wait *w)
{
	if (!w)
		return;
	if (w->timer)
		fyai_event_source_remove(w->timer);
	free(w->name);
	free(w->reason);
	free(w);
}

/* What the model is told when a wait fires. */
static char *fyai_wait_fired_text(const struct fyai_wait *w)
{
	return strdup(fy_sprintfa("[wait '%s' fired%s%s]", w->name,
				  w->reason && *w->reason ? ": " : "",
				  w->reason ? w->reason : ""));
}

/*
 * The wait fired. This function asks the model nothing. The owner of the loop
 * starts the turn that carries the event, because only the owner can tell
 * whether a turn runs already.
 */
static enum fyai_event_action fyai_wait_fired(const struct fyai_event *ev)
{
	struct fyai_wait *w = ev->userdata;

	w->fired = true;
	if (w->timer) {
		fyai_event_source_remove(w->timer);
		w->timer = NULL;
	}
	(void)fyai_event_inject(w->ctx, fyai_wait_fired_text(w));
	return FYAIEA_CONTINUE;
}

/* Drop the waits that have fired: their event is queued already. */
static void fyai_waits_reap(struct fyai_ctx *ctx)
{
	struct fyai_wait **pp, *w;

	for (pp = &ctx->waits; *pp; ) {
		if (!(*pp)->fired) {
			pp = &(*pp)->next;
			continue;
		}
		w = *pp;
		*pp = w->next;
		fyai_wait_free(w);
	}
}

bool fyai_wait_exists(struct fyai_ctx *ctx, const char *name)
{
	fyai_waits_reap(ctx);
	return fyai_wait_find(ctx, name) != NULL;
}

bool fyai_wait_cancel(struct fyai_ctx *ctx, const char *name)
{
	struct fyai_wait **pp, *w;

	fyai_waits_reap(ctx);
	for (pp = &ctx->waits; (w = *pp); pp = &w->next) {
		if (strcmp(w->name, name))
			continue;
		*pp = w->next;
		fyai_wait_free(w);
		fyai_waiters_kick(ctx);
		return true;
	}
	return false;
}

bool fyai_wait_pending(const struct fyai_ctx *ctx)
{
	const struct fyai_wait *w;

	if (!ctx)
		return false;
	fyai_waits_reap((struct fyai_ctx *)ctx);
	for (w = ctx->waits; w; w = w->next)
		if (!w->fired)
			return true;
	return false;
}

fy_generic fyai_waits_rows(struct fyai_ctx *ctx, struct fy_generic_builder *gb)
{
	const struct fyai_wait *w;
	fy_generic rows, row;
	int64_t left;

	rows = fy_sequence(gb);
	fyai_waits_reap(ctx);
	for (w = ctx->waits; w; w = w->next) {
		left = w->due_ms - fyai_event_now_ms();
		row = fy_mapping(gb, "name", w->name,
				 "remaining_seconds", (long long)(left > 0 ? (left + 999) / 1000 : 0));
		if (w->reason && *w->reason)
			row = fy_assoc(gb, row, "reason", fy_value(gb, w->reason));
		rows = fy_append(gb, rows, row);
	}
	return rows;
}

void fyai_waits_release(struct fyai_ctx *ctx)
{
	struct fyai_wait *w, *next;

	if (!ctx)
		return;
	for (w = ctx->waits; w; w = next) {
		next = w->next;
		fyai_wait_free(w);
	}
	ctx->waits = NULL;
}

/* Drop inherited wait records without removing the parent's timers. */
void fyai_waits_abandon(struct fyai_ctx *ctx)
{
	struct fyai_wait *w, *next;

	if (!ctx)
		return;
	for (w = ctx->waits; w; w = next) {
		next = w->next;
		w->timer = NULL;
		fyai_wait_free(w);
	}
	ctx->waits = NULL;
}

/* Start a wait that does not hold the turn. */
static char *fyai_wait_start(struct fyai_ctx *ctx, const char *name,
			     const char *reason, double seconds)
{
	struct fyai_event_loop *el;
	struct fyai_wait *w;
	int64_t ms;
	int rc;

	if (!fyai_wait_name_valid(name))
		return strdup(fy_sprintfa("tool error: '%s' is not a usable "
					  "wait name; use letters, digits, "
					  "'-' or '_'", name));
	if (fyai_wait_find(ctx, name))
		return strdup(fy_sprintfa("tool error: a wait named '%s' is "
					  "already open; use another name",
					  name));

	el = fyai_ctx_loop(ctx);
	assert(el);

	w = calloc(1, sizeof(*w));
	fyai_error_check(ctx, w, out, "wait: could not allocate wait");
	w->ctx = ctx;
	w->name = strdup(name);
	w->reason = reason ? strdup(reason) : NULL;
	fyai_error_check(ctx, w->name && (!reason || w->reason), out_free,
			 "wait: could not allocate wait fields");
	ms = (int64_t)(seconds * 1000.0);
	if (ms < 1)
		ms = 1;
	w->due_ms = fyai_event_now_ms() + ms;
	rc = fyai_event_add_timer(el, ms, 0, fyai_wait_fired, w, &w->timer);
	fyai_error_check(ctx, !rc, out_free, "wait: could not arm timer");
	w->next = ctx->waits;
	ctx->waits = w;
	return strdup(fy_sprintfa("[wait '%s' started: %.3g seconds]\nIt does "
				  "not hold your turn. Keep working; you are "
				  "told when it fires.", name, seconds));

out_free:
	fyai_wait_free(w);
out:
	return NULL;
}

/* The most things one wait holds the turn for. */
#define FYAI_WAIT_FOR_MAX	16

enum fyai_wait_for_kind {
	FYAI_WAIT_FOR_NONE,
	FYAI_WAIT_FOR_AGENT,
	FYAI_WAIT_FOR_SHELL,
	FYAI_WAIT_FOR_WAIT,
	FYAI_WAIT_FOR_MONITOR,
	FYAI_WAIT_FOR_QUESTION,
};

struct fyai_wait_for_item {
	enum fyai_wait_for_kind kind;
	char *name;		/* owned */
	char *text;		/* its report, once it has one; owned */
};

/*
 * A wait that holds a tool call. Several can be active at once, and each one
 * is woken by every event it waits for: a waiter reads an event without
 * removing it, and the events that waiters have read leave the queue when no
 * waiter is active. The model thus gets an event once for each waiter.
 */
struct fyai_waiter {
	struct fyai_waiter *next;
	struct fyai_ctx *ctx;
	struct fyai_wait_for_item items[FYAI_WAIT_FOR_MAX];
	size_t n;
	bool all;
	bool poll;		/* report what has ended and return at once */
	bool done;
	double seconds;
	fyai_event_ms_t deadline;
	char *result;
	bool ok;
	void (*complete)(void *userdata);
	void *userdata;
};

/*
 * Look for the end of one target. Return its report, copied from the event
 * queue, or NULL while the target goes on.
 */
static char *fyai_wait_for_check(struct fyai_ctx *ctx,
				 const struct fyai_wait_for_item *it)
{
	char *text;
	bool known;

	switch (it->kind) {
	case FYAI_WAIT_FOR_AGENT:
		/* An end and a request for input both start this way. */
		return fyai_event_peek_prefix(ctx,
				fy_sprintfa("[agent '%s' ", it->name));
	case FYAI_WAIT_FOR_SHELL:
		text = fyai_shell_session_ended_text(ctx, it->name, &known);
		if (text)
			return text;
		return fyai_event_peek_prefix(ctx,
				fy_sprintfa("[shell '%s' is waiting", it->name));
	case FYAI_WAIT_FOR_WAIT:
		return fyai_event_peek_prefix(ctx,
				fy_sprintfa("[wait '%s' fired", it->name));
	case FYAI_WAIT_FOR_MONITOR:
		return fyai_event_peek_prefix(ctx,
				fy_sprintfa("[monitor '%s'", it->name));
	case FYAI_WAIT_FOR_QUESTION:
		return fyai_event_peek_prefix(ctx,
				fy_sprintfa("[question '%s' ", it->name));
	default:
		return NULL;
	}
}

/* Whether a target can still produce the report that a wait is for. */
static bool fyai_wait_for_alive(struct fyai_ctx *ctx,
				const struct fyai_wait_for_item *it)
{
	bool known;

	switch (it->kind) {
	case FYAI_WAIT_FOR_AGENT:
		return fyai_agent_background_running(ctx, it->name);
	case FYAI_WAIT_FOR_SHELL:
		(void)fyai_shell_session_ended_text(ctx, it->name, &known);
		return known;
	case FYAI_WAIT_FOR_WAIT:
		return fyai_wait_exists(ctx, it->name);
	case FYAI_WAIT_FOR_MONITOR:
		return fyai_monitor_running(ctx, it->name);
	case FYAI_WAIT_FOR_QUESTION:
		return fyai_asks_exists(ctx, it->name);
	default:
		return false;
	}
}

/* Pick what @target names: a sub-agent, a monitor, a session or a named wait. */
static enum fyai_wait_for_kind fyai_wait_for_resolve(struct fyai_ctx *ctx,
						     const char *target)
{
	bool known;

	if (fyai_agent_background_running(ctx, target) ||
	    fyai_event_pending_prefix(ctx, fy_sprintfa("[agent '%s' ", target)))
		return FYAI_WAIT_FOR_AGENT;
	if (fyai_monitor_running(ctx, target) ||
	    fyai_event_pending_prefix(ctx, fy_sprintfa("[monitor '%s'", target)))
		return FYAI_WAIT_FOR_MONITOR;
	(void)fyai_shell_session_ended_text(ctx, target, &known);
	if (known)
		return FYAI_WAIT_FOR_SHELL;
	if (fyai_wait_exists(ctx, target) ||
	    fyai_event_pending_prefix(ctx, fy_sprintfa("[wait '%s' fired",
						       target)))
		return FYAI_WAIT_FOR_WAIT;
	if (fyai_asks_exists(ctx, target) ||
	    fyai_event_pending_prefix(ctx, fy_sprintfa("[question '%s' ",
						       target)))
		return FYAI_WAIT_FOR_QUESTION;
	return FYAI_WAIT_FOR_NONE;
}

/* Join the reports that a wait collected, each one as its own paragraph. */
static char *fyai_waiter_report(const struct fyai_waiter *w)
{
	struct response_buffer out = {0}, pending = {0};
	const char *sep = "";
	size_t i;

	for (i = 0; i < w->n; i++) {
		if (!w->items[i].text) {
			if ((pending.len && response_buffer_append(&pending, ", ")) ||
			    response_buffer_append(&pending, "'") ||
			    response_buffer_append(&pending, w->items[i].name) ||
			    response_buffer_append(&pending, "'"))
				goto fail;
			continue;
		}
		if (response_buffer_append(&out, sep) ||
		    response_buffer_append(&out, w->items[i].text))
			goto fail;
		sep = "\n\n";
	}
	if (pending.len) {
		if (response_buffer_append(&out, sep) ||
		    response_buffer_append(&out, w->poll ?
				fy_sprintfa("[still running: %s]", pending.data) :
				fy_sprintfa("[still waiting for %s after %.3g seconds]",
					    pending.data, w->seconds)))
			goto fail;
	}
	free(pending.data);
	return out.data ? out.data : strdup("");
fail:
	free(pending.data);
	free(out.data);
	return NULL;
}

/* Look at the targets of one waiter; finish it when its condition holds. */
static void fyai_waiter_poll(struct fyai_waiter *w)
{
	struct fyai_ctx *ctx = w->ctx;
	size_t i, ended = 0;

	if (w->done)
		return;
	for (i = 0; i < w->n; i++) {
		struct fyai_wait_for_item *it = &w->items[i];

		if (!it->text)
			it->text = fyai_wait_for_check(ctx, it);
		if (!it->text && !fyai_wait_for_alive(ctx, it))
			it->text = strdup(fy_sprintfa(
				"[%s ended without a report]", it->name));
		ended += it->text != NULL;
	}
	if (!(w->all ? ended == w->n : ended > 0) &&
	    fyai_event_now_ms() < w->deadline && !fyai_interrupt_pending(ctx))
		return;
	w->result = fyai_waiter_report(w);
	w->ok = w->result != NULL;
	if (!w->result)
		w->result = strdup("tool error: out of memory");
	w->done = true;
}

static bool fyai_waiters_active(const struct fyai_ctx *ctx)
{
	const struct fyai_waiter *w;

	for (w = ctx->waiters; w; w = w->next)
		if (!w->done)
			return true;
	return false;
}

/* Remove the events that waiters have read, once no waiter can still want them. */
static void fyai_waiters_purge(void *userdata)
{
	struct fyai_ctx *ctx = userdata;

	if (!fyai_waiters_active(ctx))
		fyai_events_purge_seen(ctx);
}

/*
 * Arm the one timer to the earliest deadline of the active waiters, or remove
 * it when none is active. A waiter has no other clock: events wake it through
 * fyai_waiters_kick(), and a cancelled turn cancels its group.
 */
static enum fyai_event_action fyai_waiters_timer(const struct fyai_event *ev);

static void fyai_waiters_arm(struct fyai_ctx *ctx)
{
	struct fyai_event_loop *el = fyai_ctx_loop(ctx);
	const struct fyai_waiter *w;
	fyai_event_ms_t first = -1, now = fyai_event_now_ms(), left;

	for (w = ctx->waiters; w; w = w->next) {
		if (w->done)
			continue;
		left = w->deadline - now;
		if (first < 0 || left < first)
			first = left;
	}
	if (first < 0) {
		fyai_event_source_remove(ctx->waiter_tick);
		ctx->waiter_tick = NULL;
		(void)fyai_event_defer(el, fyai_waiters_purge, ctx);
		return;
	}
	if (first < 1)
		first = 1;
	if (ctx->waiter_tick)
		(void)fyai_event_timer_rearm(ctx->waiter_tick, first, 0);
	else if (el)
		(void)fyai_event_add_timer(el, first, 0, fyai_waiters_timer, ctx,
					   &ctx->waiter_tick);
}

/* Look at every active waiter; finish those whose condition holds. */
static void fyai_waiters_poll(struct fyai_ctx *ctx)
{
	struct fyai_waiter *w, *next;
	bool was_done;

	for (w = ctx->waiters; w; w = next) {
		next = w->next;
		was_done = w->done;
		fyai_waiter_poll(w);
		if (!was_done && w->done && w->complete)
			w->complete(w->userdata);
	}
	fyai_waiters_arm(ctx);
}

static enum fyai_event_action fyai_waiters_timer(const struct fyai_event *ev)
{
	fyai_waiters_poll(ev->userdata);
	return FYAIEA_CONTINUE;
}

static void fyai_waiters_kicked(void *userdata)
{
	struct fyai_ctx *ctx = userdata;

	ctx->waiter_kick = false;
	fyai_waiters_poll(ctx);
}

void fyai_waiters_kick(struct fyai_ctx *ctx)
{
	if (!ctx || ctx->waiter_kick || !fyai_waiters_active(ctx))
		return;
	/* Many changes in one pass of the loop make one look. */
	ctx->waiter_kick = !fyai_event_defer(fyai_ctx_loop(ctx),
					     fyai_waiters_kicked, ctx);
}

static void fyai_waiter_unlink(struct fyai_waiter *w)
{
	struct fyai_waiter **pp;

	for (pp = &w->ctx->waiters; *pp; pp = &(*pp)->next) {
		if (*pp != w)
			continue;
		*pp = w->next;
		return;
	}
}

void fyai_waiter_destroy(struct fyai_waiter *w)
{
	size_t i;

	if (!w)
		return;
	fyai_waiter_unlink(w);
	fyai_waiters_arm(w->ctx);
	for (i = 0; i < w->n; i++) {
		free(w->items[i].name);
		free(w->items[i].text);
	}
	free(w->result);
	free(w);
}

bool fyai_waiter_done(const struct fyai_waiter *w)
{
	return w && w->done;
}

char *fyai_waiter_result(struct fyai_waiter *w, bool *okp)
{
	char *result = w->result;

	*okp = w->ok;
	w->result = NULL;
	return result;
}

void fyai_waiter_cancel(struct fyai_waiter *w)
{
	if (!w || w->done)
		return;
	w->result = strdup("[wait interrupted]");
	w->ok = false;
	w->done = true;
}

bool fyai_wait_for_requested(fy_generic args)
{
	fy_generic forv = fy_get(args, "for", fy_invalid);

	return fy_is_string(forv) || fy_is_sequence(forv);
}

/* A wait without `for` holds the call in the parent; one with it is a waiter. */
bool fyai_wait_in_parent(fy_generic args)
{
	return !fyai_wait_for_requested(args);
}

/* Give the refusal @msg to the model as *errp. Always return -1. */
static int fyai_waiter_refuse(struct fyai_ctx *ctx, char **errp,
			      const char *msg)
{
	*errp = strdup(msg);
	fyai_error_check(ctx, *errp, err, "wait: could not copy the refusal");
err:
	return -1;
}

/*
 * Read the `for` arguments of a wait. A name is copied: a short string lives
 * in the generic that it was read from, which the loop variable of the list
 * replaces at each item. Return 0, or -1 with the cause in *errp.
 */
static int fyai_waiter_parse(struct fyai_waiter *w, fy_generic args,
			     char **errp)
{
	struct fyai_ctx *ctx = w->ctx;
	fy_generic forv = fy_get(args, "for", fy_invalid), item, mode;
	char *why;

	if (fy_is_string(forv)) {
		w->items[w->n].name = strdup(fy_castp(&forv, ""));
		fyai_error_check(ctx, w->items[w->n].name, oom,
				 "wait: could not copy a name");
		w->n++;
	} else {
		fy_foreach(item, forv) {
			if (w->n == FYAI_WAIT_FOR_MAX || !fy_is_string(item))
				return fyai_waiter_refuse(ctx, errp,
					fy_sprintfa("tool error: for takes up "
						    "to %d names",
						    FYAI_WAIT_FOR_MAX));
			w->items[w->n].name = strdup(fy_castp(&item, ""));
			fyai_error_check(ctx, w->items[w->n].name, oom,
					 "wait: could not copy a name");
			w->n++;
		}
	}
	if (!w->n)
		return fyai_waiter_refuse(ctx, errp,
					  "tool error: for names nothing");
	/* A poll looks once: what has ended is reported, the rest is running. */
	w->poll = fy_get(args, "poll", false);
	/* The time is a limit here, and is optional. */
	if (w->poll) {
		w->seconds = 0;
	} else if (fy_is_valid(fy_get(args, "seconds", fy_invalid)) ||
	    fy_is_valid(fy_get(args, "until", fy_invalid))) {
		w->seconds = fyai_wait_seconds(args, &why);
		if (w->seconds < 0) {
			fyai_waiter_refuse(ctx, errp,
				fy_sprintfa("tool error: %s", why ? why :
					    "the wait could not be read"));
			free(why);
			return -1;
		}
	} else {
		w->seconds = FYAI_WAIT_MAX_MS / 1000.0;
	}
	mode = fy_get(args, "mode", fy_invalid);
	if (fy_is_string(mode) && !fy_any_equal(mode, "any", "all"))
		return fyai_waiter_refuse(ctx, errp,
					  "tool error: mode is any or all");
	w->all = fy_equal(mode, "all");
	return 0;
oom:
	return fyai_waiter_refuse(ctx, errp, "tool error: out of memory");
}

struct fyai_waiter *fyai_waiter_start(struct fyai_ctx *ctx, fy_generic args,
				      void (*complete)(void *userdata),
				      void *userdata, char **errp)
{
	struct fyai_event_loop *el;
	struct fyai_waiter *w;
	size_t i;

	*errp = NULL;
	w = calloc(1, sizeof(*w));
	if (!w) {
		fyai_waiter_refuse(ctx, errp, "tool error: out of memory");
		return NULL;
	}
	w->ctx = ctx;
	w->complete = complete;
	w->userdata = userdata;
	if (fyai_waiter_parse(w, args, errp))
		goto err;
	for (i = 0; i < w->n; i++) {
		w->items[i].kind = fyai_wait_for_resolve(ctx, w->items[i].name);
		if (w->items[i].kind == FYAI_WAIT_FOR_NONE) {
			fyai_waiter_refuse(ctx, errp, fy_sprintfa(
				"tool error: nothing named '%s' is running to "
				"wait for; use list to see what is open",
				w->items[i].name));
			goto err;
		}
	}
	el = fyai_ctx_loop(ctx);
	if (!el) {
		fyai_waiter_refuse(ctx, errp,
				   "tool error: no event loop for a wait");
		goto err;
	}
	w->deadline = fyai_event_now_ms() + (fyai_event_ms_t)(w->seconds * 1000.0);
	w->next = ctx->waiters;
	ctx->waiters = w;
	/* The condition can hold at once; the owner reads it from done. */
	fyai_waiter_poll(w);
	fyai_waiters_arm(ctx);
	return w;
err:
	for (i = 0; i < w->n; i++)
		free(w->items[i].name);
	free(w);
	return NULL;
}

void fyai_waiters_release(struct fyai_ctx *ctx)
{
	if (!ctx)
		return;
	if (ctx->waiter_kick)
		fyai_event_defer_cancel(fyai_ctx_loop(ctx), fyai_waiters_kicked,
					ctx);
	ctx->waiter_kick = false;
	while (ctx->waiters)
		fyai_waiter_destroy(ctx->waiters);
	fyai_event_source_remove(ctx->waiter_tick);
	ctx->waiter_tick = NULL;
}

/* Drop the waiters of the parent in a forked child, without ending them. */
void fyai_waiters_abandon(struct fyai_ctx *ctx)
{
	if (!ctx)
		return;
	ctx->waiters = NULL;
	ctx->waiter_tick = NULL;
	ctx->waiter_kick = false;
}

/*
 * Hold the turn until the wait ends, on the event loop. Only a call that no
 * group runs asks for this; a group holds the waiter and goes on.
 */
static void fyai_wait_for_sync_done(void *userdata)
{
	*(volatile bool *)userdata = true;
}

static char *fyai_wait_for_sync(struct fyai_ctx *ctx, fy_generic args,
				bool *okp)
{
	struct fyai_event_loop *el = fyai_ctx_loop(ctx);
	volatile bool finished = false;
	struct fyai_waiter *w;
	char *err, *result;

	w = fyai_waiter_start(ctx, args, fyai_wait_for_sync_done,
			      (void *)&finished, &err);
	if (!w)
		return err;
	if (!fyai_waiter_done(w) && el)
		(void)fyai_event_loop_run_until(el, &finished, -1);
	fyai_waiter_cancel(w);
	result = fyai_waiter_result(w, okp);
	fyai_waiter_destroy(w);
	return result ? result : strdup("tool error: out of memory");
}

char *fyai_wait_tool(struct fyai_ctx *ctx, fy_generic args, bool *okp)
{
	struct fyai_event_loop *el;
	fy_generic reason;
	const char *name, *reason_text;
	double seconds;
	char *why;
	char *out;
	int rc;

	*okp = false;
	if (fyai_wait_for_requested(args))
		return fyai_wait_for_sync(ctx, args, okp);
	seconds = fyai_wait_seconds(args, &why);
	if (seconds < 0) {
		out = strdup(fy_sprintfa("tool error: %s", why ? why :
					 "the wait could not be read"));
		free(why);
		return out;
	}
	if (seconds * 1000.0 > (double)FYAI_WAIT_MAX_MS)
		return strdup(fy_sprintfa("tool error: a wait of %.0f seconds "
					  "is longer than this program lives; "
					  "wait for less, or ask the user",
					  seconds));

	reason = fy_get(args, "reason", fy_invalid);
	reason_text = fy_is_string(reason) ? fy_castp(&reason, "") : NULL;

	name = fy_get(args, "name", "");
	if (!fy_str_empty(name)) {
		out = fyai_wait_start(ctx, name, reason_text, seconds);
		*okp = out && strncmp(out, "tool error:", 11);
		return out;
	}

	/* Sleep on the event loop so UI and interrupts remain active. */
	el = fyai_ctx_loop(ctx);
	if (!el)
		return strdup("tool error: no event loop for a wait");
	rc = fyai_event_sleep(el, (fyai_event_ms_t)(seconds * 1000.0));
	if (rc || fyai_interrupt_pending(ctx))
		return strdup(fy_sprintfa("[wait interrupted after less than "
					  "%.3g seconds]", seconds));
	*okp = true;
	return strdup(fy_sprintfa("[waited %.3g seconds]", seconds));
}

char *fyai_time_tool(struct fyai_ctx *ctx, bool *okp)
{
	char *text;

	(void)ctx;
	text = fyai_time_now_text();
	*okp = text != NULL;
	return text ? text : strdup("tool error: could not read the clock");
}

static char *tool_time(struct fyai_ctx *ctx, fy_generic args, bool *okp)
{
	(void)args;
	return fyai_time_tool(ctx, okp);
}

static void tool_head_time(struct fyai_ctx *ctx, FILE *mf,
			   struct fy_generic_builder *gb, fy_generic args,
			   int preview_lines, struct fyai_md_blocks *blocks)
{
	(void)ctx;
	(void)gb;
	(void)args;
	(void)preview_lines;
	(void)blocks;
	fprintf(mf, "**time**\n\n");
}

/* What the wait is for, when the model said, and its name when it fires alone. */
static void tool_head_wait(struct fyai_ctx *ctx, FILE *mf,
			   struct fy_generic_builder *gb, fy_generic args,
			   int preview_lines, struct fyai_md_blocks *blocks)
{
	fy_generic greason = fy_get(args, "reason"), gname = fy_get(args, "name");
	fy_generic gfor;
	const char *reason = fy_castp(&greason, ""), *name = fy_castp(&gname, "");

	(void)ctx;
	(void)gb;
	(void)preview_lines;
	(void)blocks;
	fprintf(mf, "**wait**");
	gfor = fy_get(args, "for");
	if (fy_is_string(gfor) && *fy_castp(&gfor, ""))
		fprintf(mf, " for %s", fy_castp(&gfor, ""));
	if (*name)
		fprintf(mf, " [%s]", name);
	if (*reason)
		fprintf(mf, " %s", reason);
	fprintf(mf, "\n\n");
}

/* The waits and the clock belong to the parent, which has the event loop. */
const struct fyai_tool_def fyai_wait_defs[] = {
	{ .name = "time", .run_text = tool_time, .head = tool_head_time,
	  .flags = FYAI_TOOL_PARENT },
	{ .name = "wait", .run_text = fyai_wait_tool, .head = tool_head_wait,
	  .in_parent = fyai_wait_in_parent },
};
const size_t fyai_wait_defs_count = sizeof(fyai_wait_defs) /
				    sizeof(fyai_wait_defs[0]);
