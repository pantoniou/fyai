/*
 * fyai_monitor.c - the monitor tool
 *
 * A monitor runs a shell command in the background. Each line that the
 * command prints reaches the model as an event, and the end of the command
 * is the last event. The command is an ordinary shell job in a job group of
 * its own, because the group of the turn is cancelled when the turn ends. It
 * has the confinement, the environment and the time limit of a shell call.
 *
 * SPDX-License-Identifier: MIT
 */
#define FYAI_MODULE FYAIEM_TOOLS
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai.h"
#include "fyai_event.h"
#include "fyai_monitor.h"
#include "fyai_tool_registry.h"
#include "fyai_tools.h"
#include "fyai_wait.h"
#include "utils.h"

#define FYAI_MONITOR_NAME_MAX	32
#define FYAI_MONITOR_DEFAULT_MS	(5 * 60 * 1000LL)
#define FYAI_MONITOR_MAX_MS	(30 * 60 * 1000LL)
/* A monitor that sends more events than this is a firehose, and is stopped. */
#define FYAI_MONITOR_MAX_EVENTS	200
/* The longest line kept; a longer one is cut. */
#define FYAI_MONITOR_LINE_MAX	4096

struct fyai_monitor_run {
	struct fyai_monitor_run *next;
	struct fyai_ctx *ctx;
	struct fyai_tool_job_group *group;
	char *name;
	char *partial;		/* the line that has no end yet */
	size_t partial_len;
	unsigned int events;
	bool stopped;		/* too many events: later output is dropped */
};

static struct fyai_monitor_run *monitor_find(struct fyai_ctx *ctx,
					     const char *name)
{
	struct fyai_monitor_run *run;

	for (run = ctx->monitors; run; run = run->next)
		if (!strcmp(run->name, name))
			return run;
	return NULL;
}

static void monitor_unlink(struct fyai_monitor_run *run)
{
	struct fyai_monitor_run **link = &run->ctx->monitors;

	while (*link && *link != run)
		link = &(*link)->next;
	if (*link)
		*link = run->next;
}

static void monitor_free(struct fyai_monitor_run *run)
{
	fyai_tool_job_group_destroy(run->group);
	free(run->partial);
	free(run->name);
	free(run);
}

static bool monitor_name_valid(const char *name)
{
	size_t i;

	if (!name || !*name || strlen(name) > FYAI_MONITOR_NAME_MAX)
		return false;
	for (i = 0; name[i]; i++)
		if (!isalnum((unsigned char)name[i]) && !strchr("-_", name[i]))
			return false;
	return true;
}

/* Queue @text for the model; the run reports a loss and goes on. */
static void monitor_event(struct fyai_monitor_run *run, char *text)
{
	if (!text || fyai_event_inject(run->ctx, text))
		fyai_warning(run->ctx, "an event of the monitor '%s' was lost",
			     run->name);
}

void fyai_monitor_output(struct fyai_ctx *ctx, const char *name,
			 const char *data, size_t len)
{
	struct fyai_monitor_run *run = monitor_find(ctx, name);
	struct response_buffer lines = {0};
	char *nl, *buf, *line;
	size_t total;

	if (!run || run->stopped || !len)
		return;
	/* Join the kept part and the chunk, then cut at each line end. */
	total = run->partial_len + len;
	buf = malloc(total + 1);
	fyai_error_check(ctx, buf, out, "monitor: could not keep the output");
	if (run->partial_len)
		memcpy(buf, run->partial, run->partial_len);
	memcpy(buf + run->partial_len, data, len);
	buf[total] = '\0';
	free(run->partial);
	run->partial = NULL;
	run->partial_len = 0;

	for (line = buf; (nl = memchr(line, '\n', buf + total - line));
	     line = nl + 1) {
		*nl = '\0';
		if (nl - line > FYAI_MONITOR_LINE_MAX)
			line[FYAI_MONITOR_LINE_MAX] = '\0';
		if (response_buffer_append(&lines, line) ||
		    response_buffer_append(&lines, "\n"))
			goto done;
	}
	if (line < buf + total) {
		run->partial_len = (size_t)(buf + total - line);
		if (run->partial_len > FYAI_MONITOR_LINE_MAX)
			run->partial_len = FYAI_MONITOR_LINE_MAX;
		run->partial = strndup(line, run->partial_len);
		if (!run->partial)
			run->partial_len = 0;
	}
	if (lines.len) {
		if (++run->events > FYAI_MONITOR_MAX_EVENTS) {
			run->stopped = true;
			monitor_event(run, strdup(fy_sprintfa(
				"[monitor '%s' stopped: more than %d events; "
				"start it again with a narrower filter]",
				run->name, FYAI_MONITOR_MAX_EVENTS)));
			fyai_tool_job_group_cancel(run->group);
		} else {
			monitor_event(run, strdup(fy_sprintfa("[monitor '%s'] %s",
						run->name, lines.data)));
		}
	}
done:
	free(buf);
	free(lines.data);
	return;
out:
	return;
}

/* The command ended: report how, after the last complete line. */
static void monitor_finish(void *userdata)
{
	struct fyai_monitor_run *run = userdata;
	struct fyai_ctx *ctx = run->ctx;
	fy_generic result = fy_invalid;
	const char *text;
	bool ok = false;
	int rc;

	fyai_error_check(ctx, fyai_ctx_transient_gb(ctx), release,
			 "monitor: could not collect the end of '%s'", run->name);
	rc = fyai_tool_job_group_collect(run->group, 0, &result, &ok);
	text = fy_is_string(result) ? fy_castp(&result, "") : "";
	if (run->partial && run->partial_len)
		monitor_event(run, strdup(fy_sprintfa("[monitor '%s'] %s",
						      run->name, run->partial)));
	if (run->stopped)
		goto release;
	if (strstr(text, "timed out after"))
		monitor_event(run, strdup(fy_sprintfa("[monitor '%s' ended: "
			"its time limit passed; start it again to go on]",
			run->name)));
	else if (rc || !ok)
		monitor_event(run, strdup(fy_sprintfa("[monitor '%s' ended with "
			"a failure]", run->name)));
	else
		monitor_event(run, strdup(fy_sprintfa("[monitor '%s' ended]",
						      run->name)));
release:
	monitor_unlink(run);
	monitor_free(run);
}

static void monitor_complete(struct fyai_tool_job_group *group, void *userdata)
{
	struct fyai_monitor_run *run = userdata;

	(void)group;
	(void)fyai_event_defer(fyai_ctx_loop(run->ctx), monitor_finish, run);
}

static fy_generic tool_monitor(struct fyai_ctx *ctx, fy_generic args, bool *okp)
{
	struct fy_generic_builder *gb = ctx->transient_gb;
	struct fyai_monitor_run *run = NULL;
	fy_generic gname, call, shell_args, result = fy_invalid;
	long long ms;
	const char *name, *json;
	bool ok = false;
	int rc;

	*okp = false;
	gname = fy_get(args, "name", fy_invalid);
	name = fy_is_string(gname) ? fy_castp(&gname, "") : "";
	if (!monitor_name_valid(name))
		return fy_value(gb, "tool error: name the monitor with letters, "
				"digits, '-' or '_'");
	if (monitor_find(ctx, name))
		return fy_stringf(gb, "tool error: a monitor named '%s' is "
				  "already running; use another name or cancel "
				  "it", name);
	if (!fy_is_string(fy_get(args, "command", fy_invalid)))
		return fy_value(gb, "tool error: give the command to monitor");
	ms = fy_get(args, "timeout", FYAI_MONITOR_DEFAULT_MS);
	if (ms < 1000)
		ms = 1000;
	if (ms > FYAI_MONITOR_MAX_MS)
		ms = FYAI_MONITOR_MAX_MS;

	shell_args = fy_mapping(gb,
		"command", fy_get(args, "command"),
		"timeout", ms,
		"description", fy_get(args, "description", "monitor"),
		"_fyai_background", true,
		"_fyai_monitor", gname);
	if (fy_is_string(fy_get(args, "workdir", fy_invalid)))
		shell_args = fy_assoc(gb, shell_args, "workdir",
				      fy_get(args, "workdir"));
	json = emit_json_string(gb, shell_args);
	fyai_error_check(ctx, json, err,
			 "monitor: could not encode the command");
	if (ctx->cfg->api_mode == FYAI_API_CHAT_COMPLETIONS)
		call = fy_mapping(gb, "type", "function",
			"function", fy_mapping(gb,
				"name", "shell", "arguments", json));
	else
		call = fy_mapping(gb, "type", "function_call",
			"name", "shell", "arguments", json);

	run = calloc(1, sizeof(*run));
	fyai_error_check(ctx, run, err, "monitor: could not allocate the run");
	run->ctx = ctx;
	run->name = strdup(name);
	fyai_error_check(ctx, run->name, err,
			 "monitor: could not keep the name");
	run->group = fyai_tool_job_group_create_notify(ctx, monitor_complete,
						       run);
	fyai_error_check(ctx, run->group, err,
			 "monitor: could not create the job group");
	/* The first output can come before the run is linked: link it first. */
	run->next = ctx->monitors;
	ctx->monitors = run;
	rc = fyai_tool_job_group_add(run->group, call);
	fyai_error_check(ctx, !rc, err_linked,
			 "monitor: could not add the command");
	rc = fyai_tool_job_group_submit(run->group);
	fyai_error_check(ctx, !rc, err_linked,
			 "monitor: could not start the command");
	if (fyai_tool_job_group_done(run->group)) {
		/* A job that ends at once did not start: this is its cause. */
		fyai_event_defer_cancel(fyai_ctx_loop(ctx), monitor_finish, run);
		(void)fyai_tool_job_group_collect(run->group, 0, &result, &ok);
		if (fy_is_string(result))
			result = fy_gb_internalize(gb, result);
		monitor_unlink(run);
		monitor_free(run);
		return fy_is_string(result) ? result :
			fy_value(gb, "tool error: the monitor did not start");
	}
	*okp = true;
	return fy_stringf(gb, "[monitor '%s' started: %lld seconds]\n"
			  "Each line the command prints reaches you as an "
			  "event, and its end is the last one. It does not "
			  "hold your turn. Only complete lines are sent, from "
			  "both output streams; filter the output to the lines "
			  "you would act on.", name, ms / 1000);

err_linked:
	monitor_unlink(run);
err:
	if (run)
		monitor_free(run);
	return fy_value(gb, "tool error: the monitor could not start");
}

static void tool_head_monitor(struct fyai_ctx *ctx, FILE *mf,
			      struct fy_generic_builder *gb, fy_generic args,
			      int preview_lines, struct fyai_md_blocks *blocks)
{
	fy_generic gname = fy_get(args, "name"), gdesc = fy_get(args, "description");
	const char *name = fy_castp(&gname, ""), *desc = fy_castp(&gdesc, "");

	(void)ctx;
	(void)gb;
	(void)preview_lines;
	(void)blocks;
	fprintf(mf, "**monitor**%s%s%s%s%s\n\n", *name ? " [" : "", name,
		*name ? "]" : "", *desc ? " " : "", desc);
}

bool fyai_monitor_running(struct fyai_ctx *ctx, const char *name)
{
	return monitor_find(ctx, name) != NULL;
}

bool fyai_monitor_cancel(struct fyai_ctx *ctx, const char *name)
{
	struct fyai_monitor_run *run = monitor_find(ctx, name);

	if (!run)
		return false;
	fyai_event_defer_cancel(fyai_ctx_loop(ctx), monitor_finish, run);
	monitor_unlink(run);
	monitor_free(run);
	fyai_waiters_kick(ctx);
	return true;
}

fy_generic fyai_monitors_rows(struct fyai_ctx *ctx,
			      struct fy_generic_builder *gb)
{
	const struct fyai_monitor_run *run;
	fy_generic rows = fy_sequence(gb);

	for (run = ctx->monitors; run; run = run->next)
		rows = fy_append(gb, rows, fy_mapping(gb, "name", run->name,
				"events", (long long)run->events));
	return rows;
}

void fyai_monitor_close(struct fyai_ctx *ctx)
{
	struct fyai_monitor_run *run, *next;

	for (run = ctx->monitors; run; run = next) {
		next = run->next;
		fyai_event_defer_cancel(fyai_ctx_loop(ctx), monitor_finish, run);
		monitor_free(run);
	}
	ctx->monitors = NULL;
}

void fyai_monitor_abandon(struct fyai_ctx *ctx)
{
	ctx->monitors = NULL;
}

/* The command is a job of the parent, so the tool is a call of the parent. */
const struct fyai_tool_def fyai_monitor_defs[] = {
	{ .name = "monitor", .run = tool_monitor, .head = tool_head_monitor,
	  .flags = FYAI_TOOL_PARENT | FYAI_TOOL_INSTANT,
	  .effect = FYAI_TOOL_EFFECT_PROCESS },
};
const size_t fyai_monitor_defs_count = sizeof(fyai_monitor_defs) /
				       sizeof(fyai_monitor_defs[0]);
