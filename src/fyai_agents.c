/* SPDX-License-Identifier: MIT */
#define FYAI_MODULE FYAIEM_UNKNOWN

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "fyai_agents.h"
#include "fyai_branch.h"
#include "fyai_browser.h"
#include "fyai_display.h"
#include "fyai_event.h"
#include "fyai_jsonrpc.h"
#include "fyai_output.h"
#include "fyai_session.h"
#include "fyai_sink.h"
#include "fyai_storage.h"
#include "fyai_tools.h"
#include "fyai_ui.h"
#include "fyai_workpane.h"

struct agent_record {
	struct agent_record *next;
	struct jsonrpc_conn *route;
	long long execution, parent, sequence, started;
	char *branch, *model, *source;
	char state[24];
	bool active, subscribed;
};

struct agent_forward {
	struct agent_forward *next;
	struct fyai_ctx *ctx;
	struct jsonrpc_conn *from, *to;
	struct jsonrpc_request *request;
	struct fy_generic_builder *gb;
	fy_generic id;
};

struct agent_question {
	struct agent_question *next;
	struct jsonrpc_conn *from;
	struct fy_generic_builder *gb;
	fy_generic id, params;
};

/*
 * The parent reports through the context that owns the registry, and a
 * forward through the context it recorded.
 */
#define agents_error_check(_a, _cond, _label, _fmt, ...) \
	fyai_error_check((_a)->ctx, (_cond), _label, (_fmt) , ## __VA_ARGS__)

/* The view a zoomed agent opens in, before the work pane grants its share. */
#define AGENT_VIEW_ROWS 16
#define AGENT_VIEW_COLS 80

/* How often an agent reports its progress to its parent. */
#define AGENT_TICK_MS 250

/* Agents named in one progress band; the rest are reported as a count. */
#define AGENT_PROGRESS_SHOWN 3

/* The tail of a transcript one event carries. A subscriber is shown the end
 * of the output, so an agent that printed more sends the last of it. */
#define AGENT_SOURCE_MAX (64 * 1024)

/* An agent chain is bounded by the branch hierarchy it names, so a walk that
 * takes more hops than that has followed a cycle. */
#define AGENT_CHAIN_MAX FYAI_BRANCH_NAME_MAX

/*
 * The state of an agent as it goes over the wire, and the class the parent
 * makes of it. The wire keeps the string: a child names the work it is doing
 * and the parent presents that text. The class is what code decides on.
 */
enum agent_class {
	AGENT_CLASS_WAITING,	/* the agent wants the user */
	AGENT_CLASS_STOPPED,	/* it ended without completing */
	AGENT_CLASS_ENDED,	/* it completed or was cancelled */
	AGENT_CLASS_RUNNING,
};

struct fyai_agents {
	struct fyai_ctx *ctx;
	struct agent_record *records;
	struct agent_forward *forwards;
	struct agent_question *questions, **question_tail;
	struct fyai_event_source *timer;
	struct fytim_surface *surface;
	struct fyai_sink_band *progress;
	long long next_execution, selected, sequence;
	unsigned long generation, painted;
	bool subscribed, attached, question_presented;
	char *draft;
	char *question_draft;
	char *history;
	size_t offset;
	char activity[24];
};

static struct fyai_agents *agents_get(struct fyai_ctx *ctx)
{
	struct fyai_agents *a;

	if (ctx->agents)
		return ctx->agents;
	a = calloc(1, sizeof(*a));
	fyai_error_check(ctx, a, err_out, "cannot allocate the agent registry");
	a->ctx = ctx;
	a->question_tail = &a->questions;
	snprintf(a->activity, sizeof(a->activity), "running");
	ctx->agents = a;
	return a;

err_out:
	return NULL;
}

/* The class of a wire state. An unknown state is work in progress. */
static enum agent_class agent_state_class(const char *state)
{
	if (!strcmp(state, "waiting for input"))
		return AGENT_CLASS_WAITING;
	if (!strcmp(state, "failed") || !strcmp(state, "interrupted"))
		return AGENT_CLASS_STOPPED;
	if (!strcmp(state, "completed") || !strcmp(state, "cancelled"))
		return AGENT_CLASS_ENDED;
	return AGENT_CLASS_RUNNING;
}

/* An agent is active until its state says it has stopped. */
static bool agent_state_active(const char *state)
{
	enum agent_class class;

	class = agent_state_class(state);
	return class == AGENT_CLASS_WAITING || class == AGENT_CLASS_RUNNING;
}

/*
 * The name an agent was delegated under: the leaf of "main/agent:greeter" is
 * "greeter". NULL when @branch does not name an agent.
 */
static const char *agent_leaf_name(const char *branch)
{
	const size_t marklen = sizeof(FYAI_BRANCH_AGENT_PREFIX) - 1;
	const char *leaf;

	leaf = strrchr(branch, '/');
	if (!leaf || strncmp(leaf + 1, FYAI_BRANCH_AGENT_PREFIX, marklen))
		return NULL;
	return leaf + 1 + marklen;
}

static struct agent_record *agents_record(struct fyai_agents *a, long long id)
{
	struct agent_record *r;

	for (r = a ? a->records : NULL; r; r = r->next)
		if (r->execution == id)
			return r;
	return NULL;
}

static struct agent_record *agents_named(struct fyai_ctx *ctx, const char *name)
{
	struct agent_record *r, *found = NULL;
	const char *leaf;

	if (!ctx->agents || fy_str_empty(name))
		return NULL;
	for (r = ctx->agents->records; r; r = r->next) {
		if (!r->active)
			continue;
		if (!strcmp(r->branch, name))
			return r;
		leaf = agent_leaf_name(r->branch);
		if (!leaf || strcmp(leaf, name))
			continue;
		if (found)
			return NULL;
		found = r;
	}
	return found;
}

bool fyai_agents_ambiguous(struct fyai_ctx *ctx, const char *name)
{
	struct agent_record *r;
	const char *leaf;
	unsigned int matches = 0;

	if (!ctx->agents || fy_str_empty(name))
		return false;
	for (r = ctx->agents->records; r; r = r->next) {
		if (!r->active)
			continue;
		if (!strcmp(r->branch, name))
			return false;
		leaf = agent_leaf_name(r->branch);
		if (leaf && !strcmp(leaf, name))
			matches++;
	}
	return matches > 1;
}

unsigned int fyai_agents_detail(unsigned int relative_depth)
{
	return relative_depth <= 1 ? 2 : relative_depth == 2 ? 1 : 0;
}

static void agents_forward_free(struct agent_forward *f)
{
	struct agent_forward **p;

	p = &f->ctx->agents->forwards;
	while (*p && *p != f)
		p = &(*p)->next;
	if (*p)
		*p = f->next;
	jsonrpc_request_destroy(f->request);
	fy_generic_builder_destroy(f->gb);
	free(f);
}

static void agents_forward_done(struct jsonrpc_request *req, void *user)
{
	struct agent_forward *f;
	fy_generic result, error;

	f = user;
	result = jsonrpc_request_result(req);
	error = fy_invalid;
	if (!jsonrpc_request_ok(req))
		error = fy_mapping(f->gb, "code", -32000LL,
			"message", "agent route disconnected");
	if (jsonrpc_conn_respond(f->from, f->id, result, error))
		fyai_warning(f->ctx, "an agent is still waiting for its answer");
	agents_forward_free(f);
}

static int agents_forward(struct fyai_ctx *ctx, struct jsonrpc_conn *from,
			  struct jsonrpc_conn *to, const char *method,
			  fy_generic params, fy_generic id)
{
	struct fy_generic_builder_cfg cfg = {
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED,
	};
	struct fyai_agents *a;
	struct agent_forward *f;

	a = agents_get(ctx);
	if (!a || !to || from == to)
		return -1;
	if (!fy_is_valid(id))
		return jsonrpc_notify(to, method, params);
	f = calloc(1, sizeof(*f));
	fyai_error_check(ctx, f, err_out, "cannot allocate an agent route");
	/*
	 * A forward outlives the transient builder: it is settled when the
	 * far end answers, which is after this turn has released it.
	 */
	f->gb = fy_generic_builder_create(&cfg);
	if (!f->gb) {
		fyai_error(ctx, "cannot create the builder of an agent route");
		free(f);
		return -1;
	}
	f->ctx = ctx;
	f->from = from;
	f->to = to;
	f->id = fy_gb_internalize(f->gb, id);
	f->next = a->forwards;
	a->forwards = f;
	f->request = jsonrpc_request_submit(to, method, params,
		jsonrpc_conn_next_id(to), false, agents_forward_done, f);
	if (!f->request) {
		fyai_error(ctx, "cannot forward %s to the agent route", method);
		agents_forward_free(f);
		return -1;
	}
	jsonrpc_conn_defer(from);
	return 0;

err_out:
	return -1;
}

static void agent_record_destroy(struct agent_record *r)
{
	if (!r)
		return;
	free(r->branch);
	free(r->model);
	free(r->source);
	free(r);
}

static struct agent_record *agents_add(struct fyai_agents *a,
				      struct jsonrpc_conn *route,
				      fy_generic params, long long execution)
{
	struct agent_record *r;
	const char *branch;

	branch = fy_get(params, "branch", "");
	agents_error_check(a, fyai_branch_name_ref_valid(branch) && execution >= 1,
			   err_out, "agent admission names no valid branch");
	r = calloc(1, sizeof(*r));
	agents_error_check(a, r, err_out, "cannot allocate an agent record");
	r->branch = strdup(branch);
	r->model = strdup(fy_get(params, "model", ""));
	if (!r->branch || !r->model) {
		fyai_error(a->ctx, "cannot record agent %s", branch);
		agent_record_destroy(r);
		return NULL;
	}
	r->execution = execution;
	r->parent = fy_get(params, "parent", 0LL);
	r->started = fyai_event_now_ms();
	r->route = route;
	r->active = true;
	snprintf(r->state, sizeof(r->state), "starting");
	r->next = a->records;
	a->records = r;
	a->generation++;
	return r;

err_out:
	return NULL;
}

static void agents_event(struct fyai_ctx *ctx, struct jsonrpc_conn *from,
			 fy_generic params)
{
	struct fyai_agents *a;
	struct agent_record *r;
	long long execution, sequence;
	fy_generic source;
	const char *state, *model, *text;
	char *copy;
	bool changed;
	int rc;

	a = agents_get(ctx);
	if (!a)
		return;
	execution = fy_get(params, "execution", 0LL);
	sequence = fy_get(params, "sequence", 0LL);
	r = agents_record(a, execution);
	if (!r && ctx->agent_execution)
		r = agents_add(a, from, params, execution);
	if (!r || r->route != from || sequence <= r->sequence || !r->active)
		return;
	r->sequence = sequence;
	state = fy_get(params, "state", "running");
	changed = strcmp(r->state, state) != 0;
	snprintf(r->state, sizeof(r->state), "%s", state);
	r->active = agent_state_active(r->state);
	model = fy_get(params, "model", "");
	if (!fy_str_empty(model) && strcmp(model, r->model)) {
		copy = strdup(model);
		fyai_error_check(ctx, copy, err_out,
				 "cannot record the model of agent %s", r->branch);
		free(r->model);
		r->model = copy;
		changed = true;
	}
	source = fy_get(params, "source", fy_invalid);
	text = fy_castp(&source, (const char *)NULL);
	if (text) {
		copy = strdup(text);
		fyai_error_check(ctx, copy, err_out,
				 "cannot record the output of agent %s", r->branch);
		free(r->source);
		r->source = copy;
	}
	a->generation++;
	if (ctx->agent_execution && ctx->tool_rpc) {
		rc = jsonrpc_notify(ctx->tool_rpc, "agent/event", params);
		fyai_error_check(ctx, !rc, err_out,
				 "cannot pass on the event of agent %s", r->branch);
	}
	/* A new execution or model changes the live tile header. */
	if (r->branch)
		fyai_tool_agent_title_refresh(ctx, r->branch);
	if (changed && a->attached && a->selected == execution)
		fyai_session_banner_update(ctx);
err_out:
	fyai_ui_wake(ctx);
}

/* Tell the parent what this agent is doing. */
static int agents_publish(struct fyai_ctx *ctx, const char *state)
{
	struct fyai_agents *a;
	struct fy_generic_builder *gb;
	fy_generic params;
	const char *source, *line;
	size_t len;
	int rc;

	a = agents_get(ctx);
	gb = fyai_ctx_transient_gb(ctx);
	if (!a || !gb || !ctx->agent_execution || !ctx->tool_rpc)
		return 0;
	params = fy_mapping(gb, "execution", ctx->agent_execution,
		"parent", ctx->agent_parent, "branch", ctx->agent_branch,
		"model", ctx->cfg->model ? ctx->cfg->model : "",
		"sequence", ++a->sequence, "state", state);
	if (a->subscribed) {
		source = ctx->display_output ? fyai_output_markdown(ctx, &len) : NULL;
		if (source && len > AGENT_SOURCE_MAX) {
			/* Resume at a row boundary so the tail is legible. */
			line = strchr(source + len - AGENT_SOURCE_MAX, '\n');
			source = line ? line + 1 : "Output is available in the stored transcript.";
		}
		if (source)
			params = fy_assoc(gb, params, "source", source);
	}
	rc = jsonrpc_notify(ctx->tool_rpc, "agent/event", params);
	fyai_error_check(ctx, !rc, err_out, "cannot report the agent state to the parent");
	return 0;

err_out:
	return -1;
}

static enum fyai_event_action agents_tick(const struct fyai_event *event)
{
	struct fyai_ctx *ctx = event->userdata;

	if (jsonrpc_conn_closed(ctx->tool_rpc)) {
		ctx->interrupt_pending = ctx->terminate_pending = true;
		return FYAIEA_CONTINUE;
	}
	(void)agents_publish(ctx, ctx->terminate_pending ? "cancelled" : ctx->agents->activity);
	fyai_agents_present(ctx);
	return FYAIEA_CONTINUE;
}

void fyai_agents_activity(struct fyai_ctx *ctx, const char *state)
{
	if (!ctx->agent_execution || !ctx->agents)
		return;
	snprintf(ctx->agents->activity, sizeof(ctx->agents->activity), "%s", state);
	(void)agents_publish(ctx, state);
}

void fyai_agents_output(struct fyai_ctx *ctx)
{
	if (ctx->agents && ctx->agents->subscribed)
		(void)agents_publish(ctx, ctx->agents->activity);
}

static int agents_question(struct fyai_ctx *ctx, struct jsonrpc_conn *conn,
			   fy_generic params, fy_generic id)
{
	struct fy_generic_builder_cfg cfg = {
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED,
	};
	struct fyai_agents *a;
	struct agent_question *q;

	a = agents_get(ctx);
	if (!a)
		return -1;
	q = calloc(1, sizeof(*q));
	fyai_error_check(ctx, q, err_out, "cannot allocate an agent question");
	/*
	 * A question waits for the user, which is past the turn that carried
	 * it, so it keeps the question and its answer in a builder of its own.
	 */
	q->gb = fy_generic_builder_create(&cfg);
	if (!q->gb) {
		fyai_error(ctx, "cannot create the builder of an agent question");
		free(q);
		return -1;
	}
	q->from = conn;
	q->id = fy_gb_internalize(q->gb, id);
	q->params = fy_gb_internalize(q->gb, params);
	*a->question_tail = q;
	a->question_tail = &q->next;
	jsonrpc_conn_defer(conn);
	a->generation++;
	fyai_ui_wake(ctx);
	return 0;

err_out:
	return -1;
}

/* Whether one method was this component's to answer, and how it went. */
enum agents_served {
	AGENTS_SERVED_NO,	/* another component owns the method */
	AGENTS_SERVED_OK,
	AGENTS_SERVED_FAIL,
};

/*
 * A question from a descendant. An agent passes it to its own parent; the
 * invocation that owns the terminal puts it to the user. Nobody else can
 * answer one, so the method is left unserved.
 */
static enum agents_served agents_serve_ask(struct fyai_ctx *ctx,
					   struct jsonrpc_conn *conn,
					   fy_generic params, fy_generic id)
{
	int rc;

	if (ctx->agent_execution && ctx->tool_rpc)
		rc = agents_forward(ctx, conn, ctx->tool_rpc, "user/ask", params, id);
	else if (fyai_ui_active(ctx) && ctx->answer_next >= ctx->cfg->answer_count)
		rc = agents_question(ctx, conn, params, id);
	else
		return AGENTS_SERVED_NO;
	return rc ? AGENTS_SERVED_FAIL : AGENTS_SERVED_OK;
}

/*
 * Admit one delegated agent. An agent passes the request up: the invocation
 * holds the one registry, so the limit counts every agent of the invocation
 * and a name is active once.
 */
static enum agents_served agents_serve_admit(struct fyai_ctx *ctx,
					     struct fyai_agents *a,
					     struct fy_generic_builder *gb,
					     struct jsonrpc_conn *conn,
					     fy_generic params, fy_generic id,
					     fy_generic *result)
{
	struct agent_record *r;
	const char *branch;
	int active = 0, rc;

	if (ctx->agent_execution && ctx->tool_rpc) {
		rc = agents_forward(ctx, conn, ctx->tool_rpc, "agent/admit", params, id);
		return rc ? AGENTS_SERVED_FAIL : AGENTS_SERVED_OK;
	}
	branch = fy_get(params, "branch", "");
	for (r = a->records; r; r = r->next) {
		if (!r->active)
			continue;
		active++;
		if (!strcmp(r->branch, branch)) {
			fyai_error(ctx, "agent %s is already active", branch);
			return AGENTS_SERVED_FAIL;
		}
	}
	fyai_error_check(ctx, active < ctx->cfg->agent_max_live_agents, err_out,
			 "agent/max_live_agents (%d) reached",
			 ctx->cfg->agent_max_live_agents);
	r = agents_add(a, conn, params, ++a->next_execution);
	if (!r)
		return AGENTS_SERVED_FAIL;
	*result = fy_mapping(gb, "execution", r->execution);
	/* The execution id completes the live tile header. */
	if (r->branch)
		fyai_tool_agent_title_refresh(ctx, r->branch);
	return AGENTS_SERVED_OK;

err_out:
	return AGENTS_SERVED_FAIL;
}

/* Act on a control addressed to this agent. */
static enum agents_served agents_serve_own_control(struct fyai_ctx *ctx,
						   struct fyai_agents *a,
						   struct fy_generic_builder *gb,
						   const char *action,
						   fy_generic params,
						   fy_generic *result)
{
	fy_generic input;
	const char *text;
	char *copy;
	int rc;

	if (!strcmp(action, "subscribe")) {
		a->subscribed = fy_get(params, "enabled", true);
		rc = agents_publish(ctx, a->activity);
		if (rc)
			return AGENTS_SERVED_FAIL;
	} else if (!strcmp(action, "cancel")) {
		ctx->interrupt_pending = ctx->terminate_pending = true;
	} else if (!strcmp(action, "input")) {
		input = fy_get(params, "text", fy_invalid);
		text = fy_castp(&input, (const char *)NULL);
		fyai_error_check(ctx, text, err_out, "agent input carries no text");
		/* The queue owns the text it is given. */
		copy = strdup(text);
		fyai_error_check(ctx, copy, err_out, "cannot queue the agent input");
		rc = fyai_event_inject(ctx, copy);
		if (rc)
			return AGENTS_SERVED_FAIL;
	} else {
		fyai_error(ctx, "unknown agent control \"%s\"", action);
		return AGENTS_SERVED_FAIL;
	}
	*result = fy_mapping(gb, "ok", true);
	return AGENTS_SERVED_OK;

err_out:
	return AGENTS_SERVED_FAIL;
}

/*
 * A control names one execution. It is this agent's own, or it belongs to a
 * descendant and travels down the route that agent was admitted on.
 */
static enum agents_served agents_serve_control(struct fyai_ctx *ctx,
					       struct fyai_agents *a,
					       struct fy_generic_builder *gb,
					       struct jsonrpc_conn *conn,
					       fy_generic params, fy_generic id,
					       fy_generic *result)
{
	struct agent_record *r;
	long long execution;
	const char *action;
	int rc;

	execution = fy_get(params, "execution", 0LL);
	action = fy_get(params, "action", "");
	if (execution > 0 && execution == ctx->agent_execution)
		return agents_serve_own_control(ctx, a, gb, action, params, result);
	r = agents_record(a, execution);
	fyai_error_check(ctx, r && r->active && r->route, err_out,
			 "no live agent answers execution %lld", execution);
	rc = agents_forward(ctx, conn, r->route, "agent/control", params, id);
	return rc ? AGENTS_SERVED_FAIL : AGENTS_SERVED_OK;

err_out:
	return AGENTS_SERVED_FAIL;
}

bool fyai_agents_serve(struct fyai_ctx *ctx, struct jsonrpc_conn *conn,
		       const char *method, fy_generic params, fy_generic id,
		       fy_generic *result, fy_generic *error)
{
	struct fy_generic_builder *gb;
	struct fyai_agents *a;
	enum agents_served served;

	/* Settled before any return, so a caller reads no stale value. */
	*result = fy_invalid;
	*error = fy_invalid;
	if (strcmp(method, "user/ask") && strncmp(method, "agent/", 6))
		return false;
	a = agents_get(ctx);
	gb = fyai_ctx_transient_gb(ctx);
	if (!a || !gb)
		return true;
	if (!strcmp(method, "agent/event")) {
		agents_event(ctx, conn, params);
		return true;
	}
	if (!strcmp(method, "user/ask"))
		served = agents_serve_ask(ctx, conn, params, id);
	else if (!strcmp(method, "agent/admit"))
		served = agents_serve_admit(ctx, a, gb, conn, params, id, result);
	else if (!strcmp(method, "agent/control"))
		served = agents_serve_control(ctx, a, gb, conn, params, id, result);
	else {
		fyai_error(ctx, "unknown agent method \"%s\"", method);
		served = AGENTS_SERVED_FAIL;
	}
	if (served == AGENTS_SERVED_NO)
		return false;
	if (served == AGENTS_SERVED_FAIL)
		*error = fy_mapping(gb, "code", -32000LL,
			"message", "agent is unavailable, name is already active, or live-agent limit reached");
	return true;
}

int fyai_agents_enter(struct fyai_ctx *ctx)
{
	struct fyai_agents *a;
	struct fy_generic_builder *gb;
	struct jsonrpc_request *request;
	fy_generic result;
	int rc;

	a = agents_get(ctx);
	gb = fyai_ctx_transient_gb(ctx);
	if (!a || !gb || !ctx->tool_rpc || !ctx->agent_branch)
		return -1;
	request = jsonrpc_request_submit(ctx->tool_rpc, "agent/admit",
		fy_mapping(gb, "branch", ctx->agent_branch,
			"parent", ctx->agent_parent, "model", ctx->cfg->model),
		jsonrpc_conn_next_id(ctx->tool_rpc), false, NULL, NULL);
	fyai_error_check(ctx, request, err_out, "cannot ask the parent for admission");
	while (!jsonrpc_request_done(request) && !ctx->terminate_pending) {
		rc = fyai_event_loop_step(fyai_ctx_loop(ctx), -1);
		if (rc < 0) {
			fyai_error(ctx, "the event loop stopped while waiting for admission");
			break;
		}
	}
	result = jsonrpc_request_result(request);
	ctx->agent_execution = jsonrpc_request_ok(request) ?
		fy_get(result, "execution", 0LL) : 0;
	jsonrpc_request_destroy(request);
	fyai_error_check(ctx, ctx->agent_execution, err_out,
			 "agent admission refused: name is active or agent/max_live_agents reached");
	rc = fyai_event_add_timer(fyai_ctx_loop(ctx), AGENT_TICK_MS, AGENT_TICK_MS,
				 agents_tick, ctx, &a->timer);
	fyai_error_check(ctx, !rc, err_out, "cannot arm the agent progress timer");
	return agents_publish(ctx, "running");

err_out:
	return -1;
}

void fyai_agents_leave(struct fyai_ctx *ctx, bool ok)
{
	if (!ctx->agents)
		return;
	(void)agents_publish(ctx, ctx->terminate_pending ? "cancelled" :
			     ok ? "completed" : "failed");
	fyai_event_source_remove(ctx->agents->timer);
	ctx->agents->timer = NULL;
}

/*
 * A route has gone. Settle everything that named it: a question nobody can
 * answer now, a forward nobody will respond to, and the agents it carried.
 */
void fyai_agents_conn_closed(struct fyai_ctx *ctx, struct jsonrpc_conn *conn)
{
	struct fyai_agents *a = ctx->agents;
	struct agent_record *r;
	struct agent_forward *f, *next;
	struct agent_question **qp, *q;

	if (!a || !conn)
		return;
	if (a->questions && a->questions->from == conn && a->question_presented) {
		fyai_ui_input_set(ctx, a->question_draft);
		free(a->question_draft);
		a->question_draft = NULL;
		a->question_presented = false;
	}
	qp = &a->questions;
	while ((q = *qp)) {
		if (q->from != conn) {
			qp = &q->next;
			continue;
		}
		*qp = q->next;
		fy_generic_builder_destroy(q->gb);
		free(q);
	}
	a->question_tail = qp;
	for (f = a->forwards; f; f = next) {
		next = f->next;
		if (f->from != conn && f->to != conn)
			continue;
		if (f->from != conn)
			(void)jsonrpc_conn_respond(f->from, f->id, fy_invalid,
				fy_mapping(f->gb, "code", -32000LL,
					"message", "agent route disconnected"));
		agents_forward_free(f);
	}
	for (r = a->records; r; r = r->next) {
		if (r->route != conn)
			continue;
		r->route = NULL;
		if (r->active) {
			r->active = false;
			snprintf(r->state, sizeof(r->state), "interrupted");
		}
	}
	a->generation++;
}

unsigned long fyai_agents_generation(const struct fyai_ctx *ctx)
{
	return ctx->agents ? ctx->agents->generation : 0;
}

/* The record of @branch, or NULL. */
static struct agent_record *agents_branch_record(struct fyai_ctx *ctx,
						 const char *branch)
{
	struct agent_record *r;

	if (!ctx->agents || fy_str_empty(branch))
		return NULL;
	for (r = ctx->agents->records; r; r = r->next)
		if (!strcmp(r->branch, branch))
			return r;
	return NULL;
}

const char *fyai_agents_state(struct fyai_ctx *ctx, const char *branch)
{
	struct agent_record *r;

	r = agents_branch_record(ctx, branch);
	return r ? r->state : NULL;
}

bool fyai_agents_branch_identity(struct fyai_ctx *ctx, const char *branch,
				 const char **model, long long *execution,
				 long long *started_ms)
{
	struct agent_record *r;

	r = agents_branch_record(ctx, branch);
	*model = r ? r->model : NULL;
	*execution = r ? r->execution : 0;
	*started_ms = r ? r->started : 0;
	return r != NULL;
}

fy_generic fyai_agents_rows(struct fyai_ctx *ctx, struct fy_generic_builder *gb)
{
	struct agent_record *r;
	fy_generic rows;

	rows = fy_sequence(gb);
	for (r = ctx->agents ? ctx->agents->records : NULL; r; r = r->next)
		rows = fy_append(gb, rows, fy_mapping(gb,
			"branch", r->branch, "state", r->state,
			"execution", r->execution, "parent", r->parent,
			"model", r->model, "active", r->active));
	return rows;
}

static int agents_control(struct fyai_ctx *ctx, struct agent_record *r,
			  const char *action, const char *text, bool enabled)
{
	struct fy_generic_builder *gb;

	gb = fyai_ctx_transient_gb(ctx);
	if (!r || !r->route || !r->active || !gb)
		return -1;
	return jsonrpc_notify(r->route, "agent/control", fy_mapping(gb,
		"execution", r->execution, "action", action,
		"text", text ? text : "", "enabled", enabled));
}

static void agents_grant(void *owner, int rows, int cols)
{
	struct fyai_agents *a = owner;

	if (fyai_ui_surface_resize(a->surface, rows, cols))
		fyai_warning(a->ctx, "the agent view did not take its %dx%d grant",
			     cols, rows);
	fyai_workpane_grid_resized(a->ctx->workpane, a->surface, rows, cols);
	a->painted = 0;
}

/* An agent that wants the user is named before one that stopped, and both
 * before one that is working. */
static int agents_priority(const struct agent_record *r)
{
	switch (agent_state_class(r->state)) {
	case AGENT_CLASS_WAITING:
		return 0;
	case AGENT_CLASS_STOPPED:
		return 1;
	default:
		return 2;
	}
}

static size_t agents_descendants(struct fyai_agents *a, struct agent_record *parent)
{
	struct agent_record *r, *p;
	size_t count = 0, hops;

	for (r = a->records; r; r = r->next) {
		if (!r->active)
			continue;
		for (p = r, hops = 0; p && hops < AGENT_CHAIN_MAX; hops++) {
			if (p->parent == parent->execution) {
				count++;
				break;
			}
			p = agents_record(a, p->parent);
		}
	}
	return count;
}

/*
 * Whether @r is named before @held, which is the agent already holding a
 * place: the one that wants the user first, and the older of two alike.
 */
static bool agents_precedes(const struct agent_record *r,
			    const struct agent_record *held)
{
	if (agents_priority(r) != agents_priority(held))
		return agents_priority(r) < agents_priority(held);
	return r->execution < held->execution;
}

/*
 * Report the children of @parent, naming the first few and counting the rest.
 * Returns how many there are.
 */
static size_t agents_progress(FILE *fp, struct fyai_agents *a, long long parent)
{
	struct agent_record *r, *shown[AGENT_PROGRESS_SHOWN] = {};
	size_t count = 0, i, j;
	char *name;

	for (r = a->records; r; r = r->next) {
		if (r->parent != parent ||
		    (!r->active && (!r->route || agents_priority(r) != 1)))
			continue;
		count++;
		for (i = 0; i < AGENT_PROGRESS_SHOWN; i++) {
			if (shown[i] && !agents_precedes(r, shown[i]))
				continue;
			for (j = AGENT_PROGRESS_SHOWN - 1; j > i; j--)
				shown[j] = shown[j - 1];
			shown[i] = r;
			break;
		}
	}
	for (i = 0; i < AGENT_PROGRESS_SHOWN && shown[i]; i++) {
		r = shown[i];
		name = fyai_prompt_literal(r->branch);
		fprintf(fp, "%s · %s · %llds · %zu descendants  \n",
			name ? name : "agent", r->state,
			(long long)((fyai_event_now_ms() - r->started) / 1000),
			agents_descendants(a, r));
		free(name);
	}
	if (count > AGENT_PROGRESS_SHOWN)
		fprintf(fp, "%zu more agents · /branches shows the complete tree\n",
			count - AGENT_PROGRESS_SHOWN);
	return count;
}

static const struct fyai_workpane_tile_ops agents_ops = {
	.apply_grant = agents_grant,
};

const char *fyai_agents_attached(const struct fyai_ctx *ctx)
{
	struct agent_record *r;

	if (!ctx->agents || !ctx->agents->attached)
		return NULL;
	r = agents_record(ctx->agents, ctx->agents->selected);
	return r ? r->branch : NULL;
}

const char *fyai_agents_model(const struct fyai_ctx *ctx)
{
	struct agent_record *r;

	if (!ctx->agents || !ctx->agents->attached)
		return NULL;
	r = agents_record(ctx->agents, ctx->agents->selected);
	return r ? r->model : NULL;
}

static char *agents_history(struct fyai_ctx *ctx, const char *name)
{
	struct fyai_ctx view = *ctx;
	struct fyai_cfg cfg = *ctx->cfg;
	struct fyai_branch branch;
	struct fy_generic_builder_cfg gbcfg = {
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED,
	};
	struct fy_generic_builder *gb;
	fy_generic root;
	char *text = NULL;

	root = fyai_branches_snapshot(ctx);
	if (!fyai_branch_lookup(fy_get(root, "branches", fy_invalid), name, &branch))
		return NULL;
	gb = fy_generic_builder_create(&gbcfg);
	if (!gb)
		return NULL;
	view.cfg = &cfg;
	view.ui = NULL;
	view.display_output = NULL;
	view.last_message = branch.head;
	view.transient_gb = gb;
	view.sink = fyai_sink_create_capture(&view);
	if (view.sink) {
		if (fyai_display_recap(&view, 10, 0))
			fyai_warning(ctx, "the stored history of %s is incomplete", name);
		text = strdup(fyai_sink_captured(view.sink, NULL));
		if (!text)
			fyai_warning(ctx, "cannot keep the history of %s", name);
		fyai_sink_destroy(view.sink);
	}
	fy_generic_builder_destroy(gb);
	return text;
}

const char *fyai_agents_zoom(struct fyai_ctx *ctx, const char *name, bool attach)
{
	struct fyai_agents *a;
	struct agent_record *r;
	int rc;

	a = agents_get(ctx);
	r = agents_named(ctx, name);
	if (!a || !r || !fyai_ui_active(ctx))
		return NULL;
	fyai_agents_detach(ctx);
	a->selected = r->execution;
	a->attached = attach;
	a->history = agents_history(ctx, r->branch);
	a->offset = 0;
	a->draft = attach ? fyai_ui_input_copy(ctx) : NULL;
	a->surface = fyai_ui_surface_open(ctx, AGENT_VIEW_ROWS, AGENT_VIEW_COLS);
	fyai_error_check(ctx, a->surface, fail, "cannot open the agent view");
	rc = fyai_workpane_register(ctx->workpane, a->surface,
		FYAI_WORKPANE_TILE_AGENT_VIEW, a, &agents_ops, AGENT_VIEW_ROWS, 0);
	fyai_error_check(ctx, !rc, fail, "cannot place the agent view in the work pane");
	(void)fyai_ui_surface_zoom(ctx, a->surface);
	if (attach)
		fyai_ui_input_set(ctx, "");
	else {
		(void)fyai_tools_focus_next(ctx);
		fyai_workpane_set_focus(ctx->workpane, a->surface);
	}
	rc = agents_control(ctx, r, "subscribe", NULL, true);
	fyai_error_check(ctx, !rc, fail, "agent %s does not answer", r->branch);
	r->subscribed = true;
	a->painted = 0;
	fyai_session_banner_update(ctx);
	fyai_ui_wake(ctx);
	return r->branch;
fail:
	fyai_agents_detach(ctx);
	return NULL;
}

void fyai_agents_detach(struct fyai_ctx *ctx)
{
	struct fyai_agents *a = ctx->agents;
	struct agent_record *r;

	if (!a)
		return;
	for (r = a->records; r; r = r->next) {
		if (r->subscribed && agents_control(ctx, r, "subscribe", NULL, false))
			fyai_warning(ctx, "agent %s keeps reporting its output",
				     r->branch);
		r->subscribed = false;
	}
	if (a->surface)
		fyai_ui_surface_close(ctx, a->surface);
	a->surface = NULL;
	if (a->attached)
		fyai_ui_input_set(ctx, a->draft);
	free(a->draft);
	a->draft = NULL;
	free(a->history);
	a->history = NULL;
	a->attached = false;
	a->selected = 0;
	fyai_session_banner_update(ctx);
	fyai_ui_wake(ctx);
}

/*
 * Answer the question at the head of the queue with @line. A line that names
 * one of the offered options answers with that option; any other line is the
 * answer as typed.
 */
static void agents_answer(struct fyai_ctx *ctx, struct fyai_agents *a,
			  const char *line)
{
	struct agent_question *q;
	fy_generic answer, options;
	char *end;
	long selected;
	int rc;

	q = a->questions;
	answer = fy_value(q->gb, line);
	options = fy_get(q->params, "options", fy_invalid);
	selected = strtol(line, &end, 10);
	if (end != line && !*end && selected >= 1 &&
	    (size_t)selected <= fy_len(options))
		answer = fy_get(options, selected - 1);
	rc = jsonrpc_conn_respond(q->from, q->id,
		fy_mapping(q->gb, "answer", answer), fy_invalid);
	if (rc)
		fyai_warning(ctx, "the agent that asked did not take the answer");
	a->questions = q->next;
	if (!a->questions)
		a->question_tail = &a->questions;
	fy_generic_builder_destroy(q->gb);
	free(q);
	fyai_ui_input_set(ctx, a->question_draft);
	free(a->question_draft);
	a->question_draft = NULL;
	a->question_presented = false;
	a->generation++;
}

bool fyai_agents_input(struct fyai_ctx *ctx, const char *line)
{
	struct fyai_agents *a = ctx->agents;
	struct agent_record *r;

	if (!a)
		return false;
	if (a->questions && a->question_presented) {
		agents_answer(ctx, a, line);
		return true;
	}
	if (!a->attached)
		return false;
	if (!strcmp(line, "/branch detach")) {
		fyai_agents_detach(ctx);
		return true;
	}
	r = agents_record(a, a->selected);
	if (*line == '/')
		fyai_report(ctx, "attached view: use /branch detach to return");
	else if (agents_control(ctx, r, "input", line, true))
		fyai_report(ctx, "attached agent has stopped; use /branch detach to return");
	return true;
}

int fyai_agents_kill(struct fyai_ctx *ctx, const char *name)
{
	return agents_control(ctx, agents_named(ctx, name), "cancel", NULL, false);
}

bool fyai_agents_surface(struct fyai_ctx *ctx, const struct fytim_surface *sf)
{
	return ctx->agents && ctx->agents->surface && ctx->agents->surface == sf;
}

bool fyai_agents_keys(struct fyai_ctx *ctx, const char *data, size_t len)
{
	struct fyai_agents *a = ctx->agents;
	size_t i;

	if (!fyai_agents_surface(ctx, fyai_workpane_focused(ctx->workpane)))
		return false;
	for (i = 0; i < len; i++) {
		if (data[i] == 'j' || data[i] == 'k') {
			if (data[i] == 'j')
				a->offset++;
			else if (a->offset)
				a->offset--;
			a->generation++;
			fyai_ui_wake(ctx);
			continue;
		}
		if (data[i] == FYAI_FOCUS_NEXT_KEY) {
			fyai_tools_focus_next(ctx);
			if (i + 1 < len && fyai_ui_keys_return(ctx, data + i + 1,
							       len - i - 1))
				fyai_warning(ctx, "input typed after ^T was lost");
			break;
		}
		/* The view is this program's, so the keys that leave a tool
		 * tile leave it too. */
		if (data[i] == FYAI_KEY_ESC || data[i] == FYAI_KEY_INTR ||
		    data[i] == FYAI_FOCUS_PROMPT_KEY) {
			fyai_agents_detach(ctx);
			if (i + 1 < len && fyai_ui_keys_return(ctx, data + i + 1,
							       len - i - 1))
				fyai_warning(ctx, "input typed after the leave key was lost");
			break;
		}
	}
	return true;
}

/* Put the question at the head of the queue to the user. */
static void agents_present_question(struct fyai_ctx *ctx, struct fyai_agents *a)
{
	fy_generic question, option;
	size_t index;

	question = a->questions->params;
	(void)fyai_browser_cancel_input(ctx);
	a->question_draft = fyai_ui_input_copy(ctx);
	a->question_presented = true;
	fyai_workpane_clear_focus(ctx->workpane);
	fyai_ui_input_set(ctx, "");
	fyai_report(ctx, "[%s] %s\n", fy_get(question, "branch", "agent"),
		    fy_get(question, "question", ""));
	fy_foreach_idx_item(index, option, fy_get(question, "options", fy_invalid))
		fyai_report(ctx, "%zu) %s\n", index + 1, fy_castp(&option, ""));
}

/*
 * Draw the selected agent in its tile: what it has stored, what it is
 * writing now, and a heading for each child an attached view subscribes to.
 */
static void agents_present_view(struct fyai_ctx *ctx, struct fyai_agents *a,
				struct agent_record *selected)
{
	struct agent_record *r;
	char *md = NULL, *name;
	size_t size = 0;
	FILE *fp;
	int rc;

	if (fyai_ui_surface_set_title(a->surface, selected->branch, selected->state))
		fyai_warning(ctx, "the agent view is not named");
	fp = open_memstream(&md, &size);
	fyai_error_check(ctx, fp, err_out, "cannot build the agent view");
	if (a->history)
		fprintf(fp, "%s\n\n", a->history);
	fprintf(fp, "%s\n\n", selected->source ? selected->source :
		"Waiting for agent output.");
	if (!selected->active)
		fprintf(fp, "Agent %s. This view is read-only. Use /branch detach to return.\n\n",
			selected->state);
	for (r = a->records; a->attached && r; r = r->next) {
		if (r->parent != selected->execution)
			continue;
		if (r->active && !r->subscribed)
			r->subscribed = !agents_control(ctx, r, "subscribe", NULL, true);
		name = fyai_prompt_literal(r->branch);
		fprintf(fp, "### %s · %s\n\n%s\n\n%zu active descendants\n\n",
			name ? name : "agent", r->state, r->source ? r->source : "",
			agents_descendants(a, r));
		free(name);
		(void)agents_progress(fp, a, r->execution);
	}
	fclose(fp);
	rc = fyai_sink_page(ctx->sink, a->surface, md, a->offset);
	if (rc)
		fyai_warning(ctx, "the agent view did not render");
err_out:
	free(md);
}

/*
 * Report the children of this agent in one band. The band stands while there
 * is work in it and goes with the last of it, so an invocation whose agents
 * have all finished shows none.
 */
static void agents_present_progress(struct fyai_ctx *ctx, struct fyai_agents *a)
{
	char *md = NULL;
	size_t size = 0, count;
	FILE *fp;

	fp = open_memstream(&md, &size);
	fyai_error_check(ctx, fp, err_out, "cannot build the agent progress band");
	count = agents_progress(fp, a, ctx->agent_execution);
	fclose(fp);
	if (!count) {
		fyai_sink_band_destroy(a->progress);
		a->progress = NULL;
		goto err_out;
	}
	if (!a->progress)
		a->progress = fyai_sink_band_open(ctx->sink, false, "Agent progress", NULL);
	if (a->progress)
		fyai_sink_band_paint(a->progress, "Agent progress", NULL, md, size, NULL);
err_out:
	free(md);
}

void fyai_agents_present(struct fyai_ctx *ctx)
{
	struct fyai_agents *a = ctx->agents;
	struct agent_record *selected;

	if (!a || a->painted == a->generation)
		return;
	a->painted = a->generation;
	if (a->questions && !a->question_presented)
		agents_present_question(ctx, a);
	selected = agents_record(a, a->selected);
	if (a->surface && selected)
		agents_present_view(ctx, a, selected);
	if (ctx->agent_execution && fyai_sink_bands_available(ctx->sink))
		agents_present_progress(ctx, a);
}

void fyai_agents_cleanup(struct fyai_ctx *ctx)
{
	struct fyai_agents *a = ctx->agents;
	struct agent_record *r, *next;
	struct agent_question *q;

	if (!a)
		return;
	fyai_agents_detach(ctx);
	fyai_event_source_remove(a->timer);
	while (a->forwards)
		agents_forward_free(a->forwards);
	while ((q = a->questions)) {
		a->questions = q->next;
		(void)jsonrpc_conn_respond(q->from, q->id,
			fy_mapping(q->gb, "answer", ""), fy_invalid);
		fy_generic_builder_destroy(q->gb);
		free(q);
	}
	free(a->question_draft);
	fyai_sink_band_destroy(a->progress);
	for (r = a->records; r; r = next) {
		next = r->next;
		agent_record_destroy(r);
	}
	free(a);
	ctx->agents = NULL;
}
