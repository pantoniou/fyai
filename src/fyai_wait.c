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

/* A wait for an event looks again this often; no source wakes it earlier. */
#define FYAI_WAIT_FOR_SLICE_MS	50
/* The most things one wait holds the turn for. */
#define FYAI_WAIT_FOR_MAX	16

enum fyai_wait_for_kind {
	FYAI_WAIT_FOR_NONE,
	FYAI_WAIT_FOR_AGENT,
	FYAI_WAIT_FOR_SHELL,
	FYAI_WAIT_FOR_WAIT,
	FYAI_WAIT_FOR_MONITOR,
};

struct fyai_wait_for_item {
	enum fyai_wait_for_kind kind;
	const char *name;
	char *text;		/* its report, once it has one */
};

/*
 * Look for the end of one target. Take its report out of the event queue so
 * that the model gets it once. Return NULL while the target goes on.
 */
static char *fyai_wait_for_check(struct fyai_ctx *ctx,
				 const struct fyai_wait_for_item *it)
{
	char *text;
	bool known;

	switch (it->kind) {
	case FYAI_WAIT_FOR_AGENT:
		/* An end and a request for input both start this way. */
		return fyai_event_take_prefix(ctx,
				fy_sprintfa("[agent '%s' ", it->name));
	case FYAI_WAIT_FOR_SHELL:
		text = fyai_shell_session_ended_text(ctx, it->name, &known);
		if (text)
			return text;
		return fyai_event_take_prefix(ctx,
				fy_sprintfa("[shell '%s' is waiting", it->name));
	case FYAI_WAIT_FOR_WAIT:
		return fyai_event_take_prefix(ctx,
				fy_sprintfa("[wait '%s' fired", it->name));
	case FYAI_WAIT_FOR_MONITOR:
		return fyai_event_take_prefix(ctx,
				fy_sprintfa("[monitor '%s'", it->name));
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
	default:
		return false;
	}
}

/* Pick what @target names: a sub-agent, a session or a named wait. */
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
	return FYAI_WAIT_FOR_NONE;
}

/* Join the reports that a wait collected, each one as its own paragraph. */
static char *fyai_wait_for_report(struct fyai_wait_for_item *items, size_t n,
				  double seconds)
{
	struct response_buffer out = {0};
	const char *sep = "", *pending = "", *text;
	size_t i;

	for (i = 0; i < n; i++) {
		if (!items[i].text) {
			/* At most FYAI_WAIT_FOR_MAX short names: the frame holds them. */
			pending = fy_sprintfa("%s%s'%s'", pending,
					      *pending ? ", " : "", items[i].name);
			continue;
		}
		if (response_buffer_append(&out, sep) ||
		    response_buffer_append(&out, items[i].text))
			goto out;
		sep = "\n\n";
	}
	if (*pending) {
		text = fy_sprintfa("%s[still waiting for %s after %.3g seconds]",
				   sep, pending, seconds);
		if (response_buffer_append(&out, text))
			goto out;
	}
	return out.data ? out.data : strdup("");
out:
	free(out.data);
	return NULL;
}

/*
 * Hold the turn until the targets end or @seconds pass. With @all, every
 * target must end; otherwise the first one to end is enough. A timeout is not
 * an error: the reports collected so far are the result.
 */
static char *fyai_wait_for(struct fyai_ctx *ctx, const char **targets, size_t n,
			   bool all, double seconds, bool *okp)
{
	struct fyai_wait_for_item items[FYAI_WAIT_FOR_MAX] = {0};
	struct fyai_event_loop *el;
	fyai_event_ms_t deadline, left;
	char *result = NULL;
	size_t i, ended;

	for (i = 0; i < n; i++) {
		items[i].name = targets[i];
		items[i].kind = fyai_wait_for_resolve(ctx, targets[i]);
		if (items[i].kind == FYAI_WAIT_FOR_NONE)
			return strdup(fy_sprintfa("tool error: nothing named "
				"'%s' is running to wait for; use list to see "
				"what is open", targets[i]));
	}
	el = fyai_ctx_loop(ctx);
	if (!el)
		return strdup("tool error: no event loop for a wait");
	deadline = fyai_event_now_ms() + (fyai_event_ms_t)(seconds * 1000.0);
	for (;;) {
		ended = 0;
		for (i = 0; i < n; i++) {
			if (!items[i].text)
				items[i].text = fyai_wait_for_check(ctx, &items[i]);
			if (!items[i].text && !fyai_wait_for_alive(ctx, &items[i]))
				items[i].text = strdup(fy_sprintfa(
					"[%s ended without a report]",
					items[i].name));
			ended += items[i].text != NULL;
		}
		if (all ? ended == n : ended > 0)
			break;
		left = deadline - fyai_event_now_ms();
		if (left <= 0 || fyai_interrupt_pending(ctx))
			break;
		if (fyai_event_sleep(el, left < FYAI_WAIT_FOR_SLICE_MS ? left :
					 FYAI_WAIT_FOR_SLICE_MS))
			break;
	}
	result = fyai_wait_for_report(items, n, seconds);
	for (i = 0; i < n; i++)
		free(items[i].text);
	*okp = result != NULL;
	return result ? result : strdup("tool error: out of memory");
}

/*
 * Read the `for` arguments of a wait and hold the turn. A name is copied: a
 * short string lives in the generic that it was read from, which the loop
 * variable of the list replaces at each item.
 */
static char *fyai_wait_for_args(struct fyai_ctx *ctx, fy_generic args,
				fy_generic forv, bool *okp)
{
	const char *targets[FYAI_WAIT_FOR_MAX];
	char *names[FYAI_WAIT_FOR_MAX];
	fy_generic item, mode;
	const char *msg;
	double seconds;
	size_t n = 0, i;
	char *why, *out = NULL;

	if (fy_is_string(forv)) {
		names[n] = strdup(fy_castp(&forv, ""));
		fyai_error_check(ctx, names[n], oom, "wait: could not copy a name");
		n++;
	} else {
		fy_foreach(item, forv) {
			if (n == FYAI_WAIT_FOR_MAX || !fy_is_string(item)) {
				msg = fy_sprintfa("tool error: for takes up to "
						  "%d names", FYAI_WAIT_FOR_MAX);
				goto reject;
			}
			names[n] = strdup(fy_castp(&item, ""));
			fyai_error_check(ctx, names[n], oom,
					 "wait: could not copy a name");
			n++;
		}
	}
	if (!n) {
		msg = "tool error: for names nothing";
		goto reject;
	}
	/* The time is a limit here, and is optional. */
	if (fy_is_valid(fy_get(args, "seconds", fy_invalid)) ||
	    fy_is_valid(fy_get(args, "until", fy_invalid))) {
		seconds = fyai_wait_seconds(args, &why);
		if (seconds < 0) {
			msg = fy_sprintfa("tool error: %s", why ? why :
					  "the wait could not be read");
			free(why);
			goto reject;
		}
	} else {
		seconds = FYAI_WAIT_MAX_MS / 1000.0;
	}
	mode = fy_get(args, "mode", fy_invalid);
	if (fy_is_string(mode) && !fy_any_equal(mode, "any", "all")) {
		msg = "tool error: mode is any or all";
		goto reject;
	}
	for (i = 0; i < n; i++)
		targets[i] = names[i];
	out = fyai_wait_for(ctx, targets, n, fy_equal(mode, "all"), seconds,
			    okp);
	goto free_names;
oom:
	msg = "tool error: out of memory";
reject:
	out = strdup(msg);
	fyai_error_check(ctx, out, free_names,
			 "wait: could not copy the refusal");
free_names:
	for (i = 0; i < n; i++)
		free(names[i]);
	return out;
}

char *fyai_wait_tool(struct fyai_ctx *ctx, fy_generic args, bool *okp)
{
	struct fyai_event_loop *el;
	fy_generic reason, forv;
	const char *name, *reason_text;
	double seconds;
	char *why;
	char *out;
	int rc;

	*okp = false;
	forv = fy_get(args, "for", fy_invalid);
	if (fy_is_string(forv) || fy_is_sequence(forv))
		return fyai_wait_for_args(ctx, args, forv, okp);
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
	  .flags = FYAI_TOOL_PARENT },
};
const size_t fyai_wait_defs_count = sizeof(fyai_wait_defs) /
				    sizeof(fyai_wait_defs[0]);
