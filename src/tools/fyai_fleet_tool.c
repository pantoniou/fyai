/* SPDX-License-Identifier: MIT */
/*
 * `fleet` tool for root sessions to inspect and drive subordinate project
 * sessions (doc/fyai-fleet-sdd.md, s10). Delegates to
 * src/remote/fyai_fleet_ops.c, shared with the `fleet` CLI commands.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fyai.h"
#include "fyai_agent.h"
#include "fyai_ask.h"
#include "fyai_branch.h"
#include "fyai_fleet.h"
#include "fyai_fleet_ops.h"
#include "fyai_tool_registry.h"
#include "fyai_event.h"
#include "utils.h"

#define FYAI_MODULE FYAIEM_TOOLS

/* Max characters of model-supplied text rendered in a tool header span. */
#define FLEET_SPAN_MAX	60

static fy_generic fleet_tool_describe(struct fyai_ctx *ctx,
				      struct fy_generic_builder *gb, fy_generic tool)
{
	(void)gb;
	return fyai_fleet_is_root(ctx) ? tool : fy_invalid;
}

/* Format this root session as sender spec `.:BRANCH` in @gb. */
static const char *fleet_tool_self(struct fyai_ctx *ctx, struct fy_generic_builder *gb)
{
	return fy_gb_intern_string(gb, fy_sprintfa(".:%s", fyai_ctx_branch(ctx)));
}

/* Format and clear ctx diagnostics as a tool error string for the model. */
static fy_generic fleet_tool_failed(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
				    const char *what)
{
	char *diag = fyai_diag_string(fyai_ctx_diag(ctx));
	fy_generic text;

	text = fy_stringf(gb, "fleet: %s%s%s", what, diag && *diag ? ": " : "",
			  diag ? diag : "");
	free(diag);
	/* Surface failure to the model without aborting the session. */
	fyai_diag_reset(fyai_ctx_diag(ctx));
	return text;
}

/* State for a blocking user approval prompt. */
struct fleet_tool_review {
	bool done;
	bool given;
	unsigned int selected;
	char *other;
};

static void fleet_tool_review_done(void *user, const struct fyai_ask_reply *replies,
				   size_t n)
{
	struct fleet_tool_review *r = user;

	r->done = true;
	if (!replies || !n)
		return;
	r->given = true;
	r->selected = replies[0].selected;
	if (replies[0].other)
		r->other = strdup(replies[0].other);
}

/*
 * Returns true if @action requires user approval per fleet/approve:
 * `start` (default) gates start/create, `always` gates all mutating actions,
 * `never` disables prompts.
 */
static bool fleet_tool_asks(struct fyai_ctx *ctx, fy_generic action)
{
	fy_generic approve = fy_get(fy_get(ctx->cfg->config_doc, "fleet", fy_invalid),
				    "approve", fy_invalid);

	if (fy_equal(approve, "never"))
		return false;
	if (fy_equal(approve, "always"))
		return fy_any_equal(action, "start", "create", "send", "interrupt",
				    "answer", "stop", "adopt");
	return fy_any_equal(action, "start", "create");
}

/*
 * Prompt the user to approve @what. Returns 1 on accept, 0 on reject (optional
 * free-text reason in *@replyp, caller frees), or -1 if prompting fails.
 * Reject is option 0 so an accidental Enter defaults to safe refusal.
 */
static int fleet_tool_review(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
			     const char *what, char **replyp)
{
	struct fleet_tool_review r = { 0 };
	fy_generic reject, accept, question;

	*replyp = NULL;
	reject = fy_mapping(gb, "id", "reject", "label", "Reject",
			    "description", "Do not run it; the model is told.",
			    "preview", "");
	accept = fy_mapping(gb, "id", "accept", "label", "Accept",
			    "description", "Run it.", "preview", "");
	question = fy_mapping(gb, "id", "fleet", "header", "Fleet",
			      "question", fy_stringf(gb, "The model %s. Run it?", what),
			      "multi_select", false,
			      "options", fy_sequence(gb, reject, accept));
	if (!fy_is_valid(question) ||
	    fyai_ask_open(ctx, fy_sequence(gb, question), NULL,
			  fleet_tool_review_done, &r))
		return -1;
	while (!r.done)
		if (fyai_event_loop_step(fyai_ctx_loop(ctx), -1) < 0)
			break;
	if (!r.done) {
		fyai_ask_close(ctx, &r);
		return -1;
	}
	if (r.other && *r.other) {
		*replyp = r.other;
		return 0;
	}
	free(r.other);
	return r.given && (r.selected & 2u) ? 1 : 0;
}

/* Build human-readable summary of a gated action for the approval prompt. */
static const char *fleet_tool_what(struct fy_generic_builder *gb, fy_generic action,
				   fy_generic args)
{
	const char *name = fy_get(args, "name", "");
	const char *project = fy_get(args, "project", "?");
	const char *task = fy_get(args, "task", "");

	if (fy_equal(action, "start"))
		return fy_gb_intern_string(gb, fy_sprintfa("starts a session in %s "
			"with the task: %s", fy_get(args, "project", "?"),
			fy_get(args, "task", "")));
	if (fy_equal(action, "create")) {
		if (!*task)
			return fy_gb_intern_string(gb, fy_sprintfa(
				"makes the project %s", project));
		return fy_gb_intern_string(gb, fy_sprintfa("makes the project "
			"%s and starts a session in it with the task: %s",
			project, task));
	}
	if (fy_equal(action, "send"))
		return fy_gb_intern_string(gb, fy_sprintfa("sends %s: %s", name,
			fy_get(args, "text", "")));
	if (fy_equal(action, "answer"))
		return fy_gb_intern_string(gb, fy_sprintfa("answers the question of "
			"%s: %s", name, fy_get(args, "answer", "")));
	if (fy_equal(action, "adopt"))
		return fy_gb_intern_string(gb, fy_sprintfa("makes the session %s a "
			"member of this one, with the task: %s", name,
			fy_get(args, "text", "")));
	return fy_gb_intern_string(gb, fy_sprintfa("%s %s", fy_equal(action, "stop") ?
		"ends" : "interrupts the turn of", name));
}

/* Returns non-empty string argument @key from @args, or NULL. */
static const char *fleet_tool_arg(fy_generic args, const char *key)
{
	const char *s = fy_get(args, key, (const char *)NULL);

	return s && *s ? s : NULL;
}

static fy_generic fleet_tool_run_generic(struct fyai_ctx *ctx, fy_generic args,
					 bool *okp)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	fy_generic action = fy_get(args, "action", fy_invalid), rows, out;
	const char *name = fleet_tool_arg(args, "name"), *text, *state;
	char *reply = NULL;
	long long id = 0;
	bool busy = false;
	int rc;

	*okp = false;
	if (fyai_agent_delegated(ctx))
		return fy_value(gb, "fleet: only the session of the user has the "
				"fleet tool");
	if (fy_any_equal(action, "send", "interrupt", "answer", "stop", "read", "adopt") && !name)
		return fy_value(gb, "fleet: name the session, as PROJECT:BRANCH");
	/* Enforce fleet/approve confirmation policy before executing. */
	if (fleet_tool_asks(ctx, action)) {
		rc = fleet_tool_review(ctx, gb, fleet_tool_what(gb, action, args), &reply);
		if (rc < 0) {
			fyai_diag_reset(fyai_ctx_diag(ctx));
			return fy_value(gb, "fleet: the call needs the approval of the "
					"person (fleet/approve), and nobody can answer now");
		}
		if (!rc) {
			out = fy_stringf(gb, "fleet: the person rejected the call%s%s",
					 reply ? ": " : "", reply ? reply : "");
			free(reply);
			return out;
		}
	}
	if (fy_equal(action, "list")) {
		rc = fyai_fleet_rows_list(ctx, gb, &rows);
		if (!rc)
			rows = fyai_fleet_rows_in(gb, rows,
						  fleet_tool_arg(args, "project"));
		out = fy_mapping(gb, "sessions", rows);
	} else if (fy_equal(action, "tasks")) {
		rc = fyai_fleet_rows_tasks(ctx, gb, &rows);
		out = fy_mapping(gb, "tasks", rows);
	} else if (fy_equal(action, "search")) {
		text = fleet_tool_arg(args, "query");
		if (!text)
			return fy_value(gb, "fleet: search needs a query");
		rc = fyai_fleet_rows_search(ctx, gb, text,
					    fy_get(args, "limit", 0LL), &rows);
		out = fy_mapping(gb, "found", rows);
	} else if (fy_equal(action, "read")) {
		rc = fyai_fleet_rows_read(ctx, gb, name, fy_get(args, "turns", 0LL),
					  &rows);
		out = fy_mapping(gb, "name", name, "messages", rows);
	} else if (fy_equal(action, "start")) {
		if (!fleet_tool_arg(args, "project") || !fleet_tool_arg(args, "task"))
			return fy_value(gb, "fleet: start needs a project and a task");
		rc = fyai_fleet_start(ctx, gb, fleet_tool_arg(args, "project"),
				      fleet_tool_arg(args, "task"),
				      fleet_tool_arg(args, "branch"), false, &out);
	} else if (fy_equal(action, "create")) {
		if (!fleet_tool_arg(args, "project"))
			return fy_value(gb, "fleet: create needs a project");
		rc = fyai_fleet_create(ctx, gb, fleet_tool_arg(args, "project"),
				       fleet_tool_arg(args, "task"),
				       fleet_tool_arg(args, "branch"),
				       fy_get(args, "default_config", false),
				       false, &out);
	} else if (fy_equal(action, "send")) {
		text = fleet_tool_arg(args, "text");
		if (!text)
			return fy_value(gb, "fleet: send needs a text");
		rc = fyai_fleet_supervise(ctx, name, "fyai_session_send",
			fy_mapping(gb, "text", text, "from", fleet_tool_self(ctx, gb)));
		out = fy_mapping(gb, "sent", name);
	} else if (fy_equal(action, "interrupt")) {
		rc = fyai_fleet_supervise(ctx, name, "fyai_session_interrupt",
					  fy_map_empty);
		out = fy_mapping(gb, "interrupted", name);
	} else if (fy_equal(action, "answer")) {
		text = fleet_tool_arg(args, "answer");
		if (!text)
			return fy_value(gb, "fleet: answer needs an answer");
		rc = fyai_fleet_supervise(ctx, name, "fyai_session_answer",
					  fy_mapping(gb, "answer", text));
		out = fy_mapping(gb, "answered", name);
	} else if (fy_equal(action, "stop")) {
		rc = fyai_fleet_close(ctx, name, fy_get(args, "force", false), &busy);
		out = fy_mapping(gb, "stopped", name, "was_running", busy);
	} else if (fy_equal(action, "adopt")) {
		rc = fyai_fleet_adopt(ctx, gb, name, fleet_tool_arg(args, "text"), false,
				      &id);
		out = fy_mapping(gb, "adopted", name, "task", id);
	} else if (fy_equal(action, "task")) {
		state = fleet_tool_arg(args, "state");
		if (fy_get(args, "id", 0LL) <= 0 || !state)
			return fy_value(gb, "fleet: task needs an id and a state");
		rc = fyai_fleet_task_mark(ctx, fy_sprintfa("%lld", fy_get(args, "id", 0LL)),
					  state, false);
		out = fy_mapping(gb, "task", fy_get(args, "id", 0LL), "state", state);
	} else {
		return fy_value(gb, "fleet: the action is not known");
	}
	if (rc)
		return fleet_tool_failed(ctx, gb, fy_castp(&action, "the action failed"));
	*okp = true;
	return out;
}

static fy_generic fleet_tool_run(struct fyai_ctx *ctx, fy_generic args, bool *okp)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	fy_generic result;
	const char *text;

	result = fleet_tool_run_generic(ctx, args, okp);
	if (!*okp)
		return result;
	text = emit_json_string(gb, result);
	if (!text) {
		*okp = false;
		return fy_value(gb, "fleet: the answer could not be written");
	}
	return fy_value(gb, text);
}

/* Sanitize @text into an inline code span (<= FLEET_SPAN_MAX UTF-8 chars). */
static const char *fleet_tool_span(struct fy_generic_builder *gb, const char *text)
{
	char buf[FLEET_SPAN_MAX * 4 + 8];
	size_t i, n, chars;

	for (i = n = chars = 0; text[i] && n < sizeof(buf) - 8; i++) {
		if (((unsigned char)text[i] & 0xc0) != 0x80 && ++chars > FLEET_SPAN_MAX)
			break;
		buf[n++] = (unsigned char)text[i] < 0x20 || text[i] == '`' ||
			   (unsigned char)text[i] == 0x7f ? ' ' : text[i];
	}
	if (text[i]) {
		memcpy(buf + n, "…", strlen("…"));
		n += strlen("…");
	}
	buf[n] = '\0';
	return fy_gb_intern_string(gb, fy_sprintfa("`%s`", buf));
}

static void fleet_tool_head(struct fyai_ctx *ctx, FILE *mf,
			    struct fy_generic_builder *gb, fy_generic args,
			    int preview_lines, struct fyai_md_blocks *blocks)
{
	fy_generic action = fy_get(args, "action", fy_invalid);
	const char *what;

	(void)ctx;
	(void)preview_lines;
	(void)blocks;
	fprintf(mf, "**fleet** %s", fy_any_equal(action, "search", "list", "read",
		"tasks", "start", "create", "send", "interrupt", "answer",
		"stop", "adopt", "task") ?
		fy_castp(&action, "") : "?");
	what = fy_equal(action, "search") ? fy_get(args, "query", "") :
	       fy_any_equal(action, "start", "create") ?
	       fy_get(args, "project", "") : fy_get(args, "name", "");
	if (*what)
		fprintf(mf, " %s", fleet_tool_span(gb, what));
	if (fy_any_equal(action, "start", "create") &&
	    *fy_get(args, "task", ""))
		fprintf(mf, " %s",
			fleet_tool_span(gb, fy_get(args, "task", "")));
	if (fy_equal(action, "task"))
		fprintf(mf, " %lld %s", fy_get(args, "id", 0LL),
			fy_any_equal(fy_get(args, "state", fy_invalid), "open", "done",
				     "dropped") ? fy_get(args, "state", "") : "?");
}

const struct fyai_tool_def fyai_fleet_defs[] = {
	{ .name = "fleet", .run = fleet_tool_run, .head = fleet_tool_head,
	  .describe = fleet_tool_describe,
	  .flags = FYAI_TOOL_PARENT | FYAI_TOOL_ROOT_ONLY },
};
const size_t fyai_fleet_defs_count = ARRAY_SIZE(fyai_fleet_defs);
