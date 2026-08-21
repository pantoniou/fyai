/*
 * fyai_wait.c - the time and wait tools
 *
 * A model that comes back to something later ran a shell that sleeps, which
 * costs a process, a sandbox and a terminal. These two tools do the work
 * directly. `time` gives the clock, because the system turn is frozen at the
 * start of a conversation. `wait` waits on the event loop, so the display
 * keeps painting and an interrupt stops it.
 *
 * A named wait does not hold the turn. It returns immediately and fires later,
 * and the firing reaches the model as a turn of its own. A model can therefore
 * start work and come back to it, and no process stands still in between. Like
 * a terminal session, it lives for one invocation, because no daemon outlives
 * it.
 *
 * SPDX-License-Identifier: MIT
 */
#define FYAI_MODULE FYAIEM_TOOLS
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fyai.h"
#include "fyai_event.h"
#include "fyai_wait.h"
#include "utils.h"

/* A name says what is being waited for, as a session name does. */
#define FYAI_WAIT_NAME_MAX	32
/* A wait that holds the turn cannot hold it for ever. */
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

/*
 * Seconds until @text, which is a time of day, or a complete date and time, in
 * local time. A time that has passed gives no wait. To read it as tomorrow
 * would be a guess, and a wait of nearly one day is not the request. Returns
 * -1 when the text cannot be read.
 */
static double fyai_wait_until_seconds(const char *text)
{
	struct tm tm;
	time_t now, then;
	int y, mo, d, h, mi, se;
	double delta;
	int n;

	now = time(NULL);
	if (!localtime_r(&now, &tm))
		return -1;

	/*
	 * Read into scratch first, and take the result after that. A failed scan has
	 * already written the part that it read, and a date that is half read names a
	 * time in another century.
	 */
	n = sscanf(text, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se);
	if (n >= 5) {
		if (n < 6)
			se = 0;
		tm.tm_year = y - 1900;
		tm.tm_mon = mo - 1;
		tm.tm_mday = d;
	} else {
		n = sscanf(text, "%d:%d:%d", &h, &mi, &se);
		if (n < 2)
			return -1;
		if (n < 3)
			se = 0;
	}
	if (h < 0 || h > 23 || mi < 0 || mi > 59 || se < 0 || se > 60)
		return -1;
	tm.tm_hour = h;
	tm.tm_min = mi;
	tm.tm_sec = se;
	tm.tm_isdst = -1;
	then = mktime(&tm);
	if (then == (time_t)-1)
		return -1;
	delta = difftime(then, now);
	return delta > 0 ? delta : 0;
}

/*
 * How long to wait, from the arguments of the call. Returns -1 when the
 * arguments make no sense.
 *
 * An empty argument is not an argument. A model that fills in each property of
 * the schema sends `until: ""` beside the seconds that it means. To refuse
 * that call as "both" would refuse a call that asked for one thing.
 */
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
		*whyp = strdup("say how long to wait: seconds or until");
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
		if ((name[i] >= 'a' && name[i] <= 'z') ||
		    (name[i] >= 'A' && name[i] <= 'Z') ||
		    (name[i] >= '0' && name[i] <= '9') ||
		    name[i] == '-' || name[i] == '_')
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
 * starts the turn that carries the event, because only the owner knows if a
 * turn runs already.
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
	if (!el)
		return strdup("tool error: no event loop for a wait");

	w = calloc(1, sizeof(*w));
	if (!w)
		return strdup("tool error: could not start the wait");
	w->ctx = ctx;
	w->name = strdup(name);
	w->reason = reason ? strdup(reason) : NULL;
	ms = (int64_t)(seconds * 1000.0);
	if (ms < 1)
		ms = 1;
	w->due_ms = fyai_event_now_ms() + ms;
	rc = fyai_event_add_timer(el, ms, 0, fyai_wait_fired, w, &w->timer);
	if (!w->name || rc) {
		fyai_wait_free(w);
		return strdup("tool error: could not start the wait");
	}
	w->next = ctx->waits;
	ctx->waits = w;
	return strdup(fy_sprintfa("[wait '%s' started: %.3g seconds]\nIt does "
				  "not hold your turn. Keep working; you are "
				  "told when it fires.", name, seconds));
}

char *fyai_wait_tool(struct fyai_ctx *ctx, fy_generic args, bool *okp)
{
	struct fyai_event_loop *el;
	fy_generic name, reason;
	const char *reason_text;
	double seconds;
	char *why;
	char *out;
	int rc;

	*okp = false;
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

	name = fy_get(args, "name", fy_invalid);
	if (fy_is_string(name) && !fy_empty(name)) {
		out = fyai_wait_start(ctx, fy_castp(&name, ""), reason_text,
				      seconds);
		*okp = out && strncmp(out, "tool error:", 11);
		return out;
	}

	/*
	 * A wait that holds the turn runs on the event loop, so the display
	 * keeps painting and an interrupt reaches it. Nothing else in this
	 * process stands still.
	 */
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
