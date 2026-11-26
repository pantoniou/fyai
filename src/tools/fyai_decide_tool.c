/*
 * fyai_decide_tool.c - the decide tool: the model asks a decider
 *
 * A session with deciders configured (`decisions/NAME`) offers the tool
 * `decide {decider, input}`. A call runs `fyai decide` as a child of this
 * process, as a route of an event source does, so the decider runs on its
 * own model, decision model or chat model, outside the tool sandbox and
 * through the credential channels. The call waits on the event loop, as an
 * MCP call does: the turn goes on with the other work of the loop, and the
 * result is the decision line.
 *
 * SPDX-License-Identifier: MIT
 */
#define FYAI_MODULE FYAIEM_UNKNOWN
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai.h"
#include "fyai_decider.h"
#include "fyai_selfjob.h"
#include "fyai_tool_registry.h"
#include "fyai_tools_int.h"
#include "utils.h"

/* A decision that takes longer than this has failed. */
#define DECIDE_TOOL_TIMEOUT_MS	(120 * 1000)

struct fyai_decide_call {
	struct fyai_ctx *ctx;
	struct fyai_selfjob *job;
	void (*notify)(void *userdata);
	void *userdata;
	char *result;		/* the text the model gets */
	bool ok;
	bool done;
};

/* The context that this object stands on. */
static inline struct fyai_ctx *
fyai_decide_call_ctx(const struct fyai_decide_call *o)
{
	return o->ctx;
}

/* The deciders of the configuration; the tool offers only these. */
static fy_generic decide_tool_deciders(struct fyai_ctx *ctx)
{
	fy_generic v = fy_get(ctx->cfg->config_doc, "decisions", fy_invalid);

	return fy_is_mapping(v) ? v : fy_map_empty;
}

static fy_generic decide_tool_describe(struct fyai_ctx *ctx,
				       struct fy_generic_builder *gb,
				       fy_generic tool)
{
	fy_generic deciders = decide_tool_deciders(ctx), names = fy_seq_empty;
	fy_generic name, spec, d, desc, fn, params;
	const char *list = "", *why;

	fy_foreach_key_value(name, spec, deciders) {
		(void)spec;
		why = NULL;
		d = fyai_decider_get(ctx->cfg, gb, fy_castp(&name, ""), &why);
		if (!fy_is_mapping(d))
			continue;
		desc = fy_get(d, "description", fy_invalid);
		list = fy_gb_intern_string(gb, fy_sprintfa("%s\n- %s: %s", list,
			fy_castp(&name, ""), fy_is_string(desc) ?
			fy_castp(&desc, "") : "(no description)"));
		names = fy_append(gb, names, name);
	}
	if (!fy_len(names))
		return fy_invalid;
	fn = fy_get(tool, "function", fy_invalid);
	desc = fy_get(fn, "description", fy_invalid);
	params = fy_get(fn, "parameters", fy_invalid);
	params = fy_assoc(gb, params, "properties",
		fy_assoc(gb, fy_get(params, "properties", fy_map_empty),
			 "decider", fy_mapping(gb, "type", "string",
				"enum", names,
				"description", "The decider to ask.")));
	fn = fy_assoc(gb, fn, "description", fy_stringf(gb, "%s The deciders:%s",
		fy_castp(&desc, ""), list), "parameters", params);
	return fy_assoc(gb, tool, "function", fn);
}

static void decide_call_finish(struct fyai_decide_call *call, const char *text,
			       bool ok)
{
	call->result = strdup(text);
	call->ok = ok && call->result;
	call->done = true;
	if (call->notify)
		call->notify(call->userdata);
}

static void decide_call_done(void *data, int status, const char *out)
{
	struct fyai_decide_call *call = data;
	const char *end, *line;
	fy_generic answer;
	size_t len;

	call->job = NULL;		/* the job frees itself after this */
	if (status) {
		decide_call_finish(call, status < 0 ?
			"tool error: the decider took too long" :
			"tool error: the decider failed", false);
		return;
	}
	/* One line: {decision, arguments, ...} or {error}. */
	len = strlen(out);
	end = out + len;
	while (end > out && (end[-1] == '\n' || end[-1] == '\r'))
		end--;
	line = fy_sprintfa("%.*s", (int)(end - out), out);
	answer = parse_json_string(
		fyai_ctx_transient_gb(fyai_decide_call_ctx(call)), line);
	decide_call_finish(call, line, fy_is_mapping(answer) &&
			   fy_is_invalid(fy_get(answer, "error", fy_invalid)));
}

struct fyai_decide_call *fyai_decide_call_start(struct fyai_ctx *ctx,
						fy_generic args,
						void (*notify)(void *),
						void *userdata, char **whyp)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	struct fyai_decide_call *call;
	fy_generic name = fy_get(args, "decider", fy_invalid), input, d;
	const char *spec, *json, *why = NULL;

	*whyp = NULL;
	input = fy_get(args, "input", fy_invalid);
	if (!fy_is_string(name) || fy_is_invalid(input)) {
		*whyp = strdup("tool error: give a decider and an input");
		return NULL;
	}
	if (fy_is_invalid(fy_get(decide_tool_deciders(ctx), name,
				 fy_invalid))) {
		*whyp = strdup(fy_sprintfa("tool error: no decider '%s' is "
					   "configured", fy_castp(&name, "")));
		return NULL;
	}
	d = fyai_decider_get(ctx->cfg, gb, fy_castp(&name, ""), &why);
	spec = fy_is_mapping(d) ? emit_json_string(gb, d) : NULL;
	json = emit_json_string(gb, input);
	if (!spec || !json) {
		*whyp = strdup(fy_sprintfa("tool error: the decider '%s': %s",
			fy_castp(&name, ""), why ? why : "cannot be resolved"));
		return NULL;
	}
	call = calloc(1, sizeof(*call));
	if (!call)
		return NULL;
	call->ctx = ctx;
	call->notify = notify;
	call->userdata = userdata;
	call->job = fyai_selfjob_decide_start(ctx, spec, json, fy_seq_empty,
					      DECIDE_TOOL_TIMEOUT_MS,
					      decide_call_done, call);
	if (!call->job) {
		free(call);
		*whyp = strdup("tool error: the decider could not start");
		return NULL;
	}
	return call;
}

bool fyai_decide_call_done(const struct fyai_decide_call *call)
{
	return !call || call->done;
}

fy_generic fyai_decide_call_collect(struct fyai_ctx *ctx,
				    struct fyai_decide_call *call, bool *okp)
{
	*okp = call && call->ok;
	return fy_value(fyai_ctx_transient_gb(ctx), call && call->result ?
			call->result : "tool error: the decision was stopped");
}

void fyai_decide_call_cancel(struct fyai_decide_call *call)
{
	if (!call || call->done)
		return;
	fyai_selfjob_cancel(call->job);
	call->job = NULL;
	call->done = true;
	call->ok = false;
}

void fyai_decide_call_destroy(struct fyai_decide_call *call)
{
	if (!call)
		return;
	if (call->job)
		fyai_selfjob_cancel(call->job);
	free(call->result);
	free(call);
}

/* The group runs a call; one that does not reach it is refused. */
static fy_generic decide_tool_run(struct fyai_ctx *ctx, fy_generic args,
				  bool *okp)
{
	(void)args;
	*okp = false;
	return fy_value(fyai_ctx_transient_gb(ctx),
			"tool error: decide runs only as a tool call of a turn");
}

static void decide_tool_head(struct fyai_ctx *ctx, FILE *mf,
			     struct fy_generic_builder *gb, fy_generic args,
			     int preview_lines, struct fyai_md_blocks *blocks)
{
	fy_generic name = fy_get(args, "decider", fy_invalid);

	(void)ctx;
	(void)gb;
	(void)preview_lines;
	(void)blocks;
	fprintf(mf, "**decide** %s\n\n", fy_castp(&name, "?"));
}

const struct fyai_tool_def fyai_decide_defs[] = {
	{ .name = "decide", .run = decide_tool_run, .head = decide_tool_head,
	  .describe = decide_tool_describe,
	  .flags = FYAI_TOOL_NOT_FOR_CHILD,
	  .effect = FYAI_TOOL_EFFECT_NONE },
};
const size_t fyai_decide_defs_count = ARRAY_SIZE(fyai_decide_defs);
