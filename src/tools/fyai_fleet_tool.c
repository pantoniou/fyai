/* SPDX-License-Identifier: MIT */
/*
 * The `fleet` tool: a root session reads and drives the sessions of the
 * projects below it (doc/fyai-fleet-sdd.md, section 10). The work is done by
 * the operations of the fleet (src/remote/fyai_fleet_ops.c), which the
 * `fleet` commands also use.
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
#include "fyai_branch.h"
#include "fyai_fleet.h"
#include "fyai_fleet_ops.h"
#include "fyai_tool_registry.h"
#include "utils.h"

#define FYAI_MODULE FYAIEM_TOOLS

/* The longest text of the model that a title row shows, in characters. */
#define FLEET_SPAN_MAX	60

/* A scan stops at the second project: the root has one below it. */
static int fleet_tool_found(const char *dir, void *data)
{
	int *count = data;

	(void)dir;
	return ++*count > 1;
}

/*
 * The tool is offered to a session of a root project that has a project
 * below it. The answer is kept for the run: the tree does not change under a
 * session often enough to scan it for each request.
 */
static bool fleet_tool_offered(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg = ctx->cfg;
	fy_generic scan = fy_get(fy_get(cfg->config_doc, "fleet", fy_invalid), "scan",
				 fy_invalid);
	char *cwd, *project = NULL, *root = NULL;
	int count = 0;

	if (cfg->fleet_tool)
		return cfg->fleet_tool > 0;
	cfg->fleet_tool = -1;
	cwd = getcwd(NULL, 0);
	if (cwd) {
		project = fyai_fleet_project_of(cwd);
		root = fyai_fleet_root_of(cwd);
	}
	if (project && root && !strcmp(project, root) &&
	    fyai_fleet_scan(root, (int)fy_get(scan, "depth",
					      (long long)DEFAULT_FLEET_SCAN_DEPTH),
			    fy_get(scan, "exclude", fy_seq_empty),
			    fleet_tool_found, &count) > 0)
		cfg->fleet_tool = 1;
	free(root);
	free(project);
	free(cwd);
	return cfg->fleet_tool > 0;
}

static fy_generic fleet_tool_describe(struct fyai_ctx *ctx,
				      struct fy_generic_builder *gb, fy_generic tool)
{
	(void)gb;
	return fleet_tool_offered(ctx) ? tool : fy_invalid;
}

/* This session as a sender: `.:BRANCH` in the root. */
static const char *fleet_tool_self(struct fyai_ctx *ctx, struct fy_generic_builder *gb)
{
	return fy_gb_intern_string(gb, fy_sprintfa(".:%s", fyai_ctx_branch(ctx)));
}

/* The cause of a failure, from the diagnostics, as the answer to the model. */
static fy_generic fleet_tool_failed(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
				    const char *what)
{
	char *diag = fyai_diag_string(fyai_ctx_diag(ctx));
	fy_generic text;

	text = fy_stringf(gb, "fleet: %s%s%s", what, diag && *diag ? ": " : "",
			  diag ? diag : "");
	free(diag);
	/* The model gets the cause; the session goes on. */
	fyai_diag_reset(fyai_ctx_diag(ctx));
	return text;
}

/* An argument that the action needs, or NULL. */
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
	bool busy = false;
	int rc;

	*okp = false;
	if (fyai_agent_delegated(ctx))
		return fy_value(gb, "fleet: only the session of the user has the "
				"fleet tool");
	if (fy_any_equal(action, "send", "interrupt", "answer", "stop", "read") && !name)
		return fy_value(gb, "fleet: name the session, as PROJECT:BRANCH");
	if (fy_equal(action, "list")) {
		rc = fyai_fleet_rows_list(ctx, gb, &rows);
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

/* Model text for a title row: one line, no backtick, at most FLEET_SPAN_MAX characters. */
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
		"tasks", "start", "send", "interrupt", "answer", "stop", "task") ?
		fy_castp(&action, "") : "?");
	what = fy_equal(action, "search") ? fy_get(args, "query", "") :
	       fy_equal(action, "start") ? fy_get(args, "project", "") :
	       fy_get(args, "name", "");
	if (*what)
		fprintf(mf, " %s", fleet_tool_span(gb, what));
	if (fy_equal(action, "start") && *fy_get(args, "task", ""))
		fprintf(mf, " %s", fleet_tool_span(gb, fy_get(args, "task", "")));
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
