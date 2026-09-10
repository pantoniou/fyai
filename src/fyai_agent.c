/* SPDX-License-Identifier: MIT */
#include <stdbool.h>

#include <curl/curl.h>

#include <libfyaml.h>

#include "fyai_sink.h"
#include "fyai.h"
#include "fyai_agent.h"
#include "fyai_agents.h"
#include "fyai_ui.h"
#include "fyai_prof.h"
#include "fyai_turn.h"
#include "fyai_output.h"
#include "fyai_event.h"
#include "fyai_jsonrpc.h"
#include "fyai_provider.h"
#include "fyai_catalog.h"
#include "fyai_branch.h"
#include "fyai_config.h"
#include "fyai_storage.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define FYAI_MODULE FYAIEM_UNKNOWN

const char fyai_agent_system_prompt[] =
	"You are a fyai sub-agent: an autonomous coding assistant delegated a "
	"single, self-contained task. Work in the current workspace using the "
	"tools available to you. Use apply_patch for manual code edits. Do not "
	"create or edit files with exec_command, Python, or other shell write "
	"tricks; formatting commands and bulk mechanical rewrites are exempt. "
	"Investigate before acting, make the smallest change that satisfies the "
	"task, and verify your work. Use ask_user when a decision truly requires "
	"the user; the delegating agent will forward the answer. Otherwise decide "
	"with sensible defaults and state any assumptions. When the task is "
	"complete, stop and reply with a concise final report of what you did "
	"and what you found; that report is your entire return value.";

bool fyai_agent_delegated(const struct fyai_ctx *ctx)
{
	if (!ctx || !ctx->cfg->agent_child)
		return false;
	return ctx->tool_rpc || ctx->cfg->agent_rpc;
}

static fy_generic fyai_agent_final_text(struct fyai_ctx *ctx, fy_generic turn)
{
	fy_generic msgs, m, role, type, text;
	size_t n, i;

	msgs = fy_get(turn, "messages");
	n = fy_len(msgs);
	for (i = n; i-- > 0; ) {
		m = fy_get(msgs, i);
		role = fy_get(m, "role");
		type = fy_get(m, "type");
		if (fy_equal(role, "assistant") || fy_equal(type, "message")) {
			text = fyai_item_text(ctx, m);
			if (fy_is_string(text))
				return text;
			break;
		}
	}
	return fy_gb_internalize(ctx->transient_gb, fy_value(""));
}

/* Return the configured personas. */
static fy_generic fyai_agent_personas(struct fyai_ctx *ctx)
{
	return fy_get(fy_get(ctx->cfg->config_doc, "agent"), "personas",
		      fy_invalid);
}

/* The names of the configured personas, as "a, b, c", for a diagnostic. */
static char *fyai_agent_persona_names(struct fyai_ctx *ctx)
{
	fy_generic personas, key;
	const char *name;
	char *next, *out;
	size_t len, name_len;

	personas = fyai_agent_personas(ctx);
	if (!fy_is_mapping(personas))
		return strdup("none configured");
	out = NULL;
	len = 0;
	fy_foreach(key, personas) {
		name = fy_castp(&key, "?");
		name_len = strlen(name);
		next = realloc(out, len + (len ? 2 : 0) + name_len + 1);
		if (!next) {
			free(out);
			return NULL;
		}
		out = next;
		if (len) {
			memcpy(out + len, ", ", 2);
			len += 2;
		}
		memcpy(out + len, name, name_len + 1);
		len += name_len;
	}
	return out ? out : strdup("none configured");
}

/*
 * Apply a persona to a shallow copy. fyai_config_apply() must handle each
 * field that must not keep the parent value. Forks keep the parent model.
 */
static int fyai_agent_persona_apply(struct fyai_ctx *ctx, fy_generic persona,
				    bool fork_mode)
{
	struct fyai_cfg *cfg = ctx->cfg;
	struct fyai_cfg tmp;
	fy_generic model, overlay, thinking;
	fy_generic target;
	const char *model_text, *model_slash, *target_name;
	size_t prefix_len;
	bool reset_api_url;
	int rc;

	tmp = *cfg;
	overlay = persona;
	if (fork_mode)
		overlay = fy_disassoc(ctx->transient_gb, overlay, "model");
	thinking = fy_get(persona, "thinking", fy_invalid);
	if (fy_generic_is_bool(thinking))
		overlay = fy_assoc(ctx->transient_gb, overlay,
			fy_value(ctx->transient_gb, "display"),
			fy_mapping(ctx->transient_gb, "thinking", thinking));
	model = fy_get(overlay, "model", fy_invalid);
	reset_api_url = !fy_is_invalid(fy_get(overlay, "api", fy_invalid));
	if (fy_is_string(model)) {
		model_text = fy_castp(&model, "");
		model_slash = strchr(model_text, '/');
		/*
		 * A qualified model names its provider. An unqualified model
		 * resolves to a provider through the catalogue.
		 */
		if (model_slash) {
			prefix_len = model_slash - model_text;
			if (!cfg->provider ||
			    strncmp(model_text, cfg->provider, prefix_len) ||
			    cfg->provider[prefix_len])
				reset_api_url = true;
		} else {
			target = fyai_catalog_provider_for_model(
				fyai_catalog_effective(cfg->catalog, cfg->gb),
				model_text, NULL);
			target_name = fy_get(target, "name", "");
			if (*target_name && (!cfg->provider ||
					     strcmp(target_name, cfg->provider)))
				reset_api_url = true;
		}
		/*
		 * The api_url of config_doc can be catalogue-derived. A provider
		 * or API change invalidates it. An api_url on the persona is
		 * explicit and fyai_config_apply() keeps it.
		 */
		if (reset_api_url &&
		    fy_is_invalid(fy_get(overlay, "api_url", fy_invalid)))
			tmp.api_url = NULL;
		if (fy_is_invalid(fy_get(cfg->config_doc, "max_tokens",
						 fy_invalid)))
			tmp.max_tokens = DEFAULT_MAX_TOKENS;
		if (!tmp.api_key_explicit)
			tmp.api_key = NULL;
	}
	rc = fyai_config_apply(&tmp, overlay);
	fyai_error_check(ctx, !rc, err,
			 "could not apply the sub-agent persona");
	if (fy_is_string(model)) {
		rc = fyai_config_resolve_model(&tmp);
		fyai_error_check(ctx, !rc, err,
				 "persona model '%s' cannot be resolved",
				 fy_castp(&model, ""));
		/* A qualified persona model is an explicit wire-model choice when
		 * its provider has no catalogue offering for the suffix. */
		if (model_slash && tmp.model &&
		    !strchr(tmp.model, '/') &&
		    !strcmp(tmp.model, model_slash + 1))
			tmp.model = fy_gb_intern_string(tmp.gb, model_text);
	}
	*cfg = tmp;
	return 0;

err:
	return -1;
}

/* fy_delete_at_pathstr() fails for a key that is not present. */
static fy_generic config_drop_keys(struct fy_generic_builder *gb, fy_generic doc,
				   const char * const *keys, size_t count)
{
	size_t i;

	for (i = 0; i < count && fy_is_valid(doc); i++) {
		if (fy_is_valid(fy_get_at_pathstr(gb, doc, keys[i])))
			doc = fy_delete_at_pathstr(gb, doc, keys[i]);
	}
	return doc;
}

/*
 * Adopt the configuration stored on the sub-agent branch. It holds the
 * provider and model that the earlier delegation resolved. Without it the
 * revived agent derives the endpoint from the parent branch.
 */
static int fyai_agent_revive_config(struct fyai_ctx *ctx, fy_generic stored)
{
	static const char * const endpoint_keys[] = { "api", "api_url" };
	static const char * const parent_keys[] = { "model", "api", "api_url" };
	struct fyai_cfg *cfg = ctx->cfg;
	fy_generic stored_model;
	const char *model_text, *slash, *bare;
	int rc;

	if (!fy_is_valid(stored))
		return 0;
	ctx->arena_config = stored;
	stored_model = fy_get(stored, "model", fy_invalid);
	model_text = fy_castp(&stored_model, "");
	slash = strrchr(model_text, '/');
	bare = slash ? slash + 1 : model_text;
	/*
	 * The stored model takes precedence over the parent model, API and
	 * endpoint. Drop those keys so the endpoint derives from the catalogue
	 * for the stored model. fyai_config_apply() keeps an endpoint and a key
	 * that the document does not set, so clear the derived ones.
	 */
	if (*model_text && (!cfg->model || strcmp(bare, cfg->model))) {
		ctx->arena_config = config_drop_keys(ctx->gb, ctx->arena_config,
				endpoint_keys, ARRAY_SIZE(endpoint_keys));
		cfg->config_doc = config_drop_keys(cfg->gb, cfg->config_doc,
				parent_keys, ARRAY_SIZE(parent_keys));
		fyai_error_check(ctx, fy_is_valid(ctx->arena_config) &&
				 fy_is_valid(cfg->config_doc), err,
			"could not drop the parent endpoint for the revived sub-agent");
		cfg->api_url = NULL;
		if (!cfg->api_key_explicit)
			cfg->api_key = NULL;
	}
	rc = fyai_config_adopt_arena(ctx);
	fyai_error_check(ctx, !rc, err,
			 "could not adopt the sub-agent branch configuration");
	return 0;

err:
	return -1;
}

fy_generic fyai_agent_run(struct fyai_ctx *ctx, fy_generic args, bool *okp)
{
	struct fyai_cfg *cfg = ctx->cfg;
	struct timespec t_intern;
	fy_generic name, description;
	fy_generic task_v, context_v;
	fy_generic persona_v, persona, persona_model;
	fy_generic turn;
	fy_generic report;
	char *input;
	struct fyai_branch stored;
	bool revive = false;
	const char *json;
	char *args_json = NULL;
	const char *task;
	const char *model_bare;
	char *persona_names;
	bool fork_mode;
	int rc;

	*okp = false;
	task = NULL;

	if (!ctx->transient_gb) {
		rc = fyai_setup_transient_builder(ctx);
		fyai_error_check(ctx, !rc, err,
				 "could not prepare sub-agent storage");
	}

	ctx->last_message = fy_invalid;

	/* Re-open the arena before the forked child writes to it. */
	if (ctx->agent_branch) {
		/*
		 * The transient builder is parented by the arena builder. Preserve
		 * the call while both builders are replaced.
		 */
		json = emit_json_string(ctx->transient_gb, args);
		fyai_error_check(ctx, json, err,
				 "could not preserve the sub-agent call");
		args_json = strdup(json);
		fyai_error_check(ctx, args_json, err,
				 "could not preserve the sub-agent call");
		fyai_cleanup_transient_builder(ctx);
		rc = fyai_arena_reopen(ctx);
		fyai_error_check(ctx, !rc, err,
				 "could not re-open the arena");
		rc = fyai_setup_transient_builder(ctx);
		fyai_error_check(ctx, !rc, err,
				 "could not prepare sub-agent storage");
		args = parse_json_string(ctx->transient_gb, args_json);
		fyai_error_check(ctx, fy_is_mapping(args), err,
				 "could not restore the sub-agent call");
		free(args_json);
		args_json = NULL;
		rc = fyai_ctx_set_branch(ctx, ctx->agent_branch);
		fyai_error_check(ctx, !rc, err,
				 "could not select the sub-agent branch");
		/* Add the sub-agent branch to subsequent trace records. */
		fyai_diag_trace_tag(ctx->agent_branch);
		/* Keep last_message as the fork point unless the call is fresh. */
		ctx->branch_prev = fy_invalid;
		ctx->branch_desc = fy_invalid;
		ctx->branch_agent = fy_invalid;
		/* Continue the stored conversation identified by this name. */
		revive = fyai_branch_lookup(ctx->arena_branches,
					    ctx->agent_branch, &stored) &&
			 fy_is_valid(stored.head);
		if (revive) {
			ctx->branch_prev = stored.entry;
			ctx->branch_desc = stored.description;
			ctx->branch_agent = stored.agent;
			ctx->last_message = stored.head;
			rc = fyai_agent_revive_config(ctx, stored.config);
			fyai_error_check(ctx, !rc, err,
				"could not adopt the sub-agent branch configuration");
		}
	} else {
		fork_mode = false;
	}

	persona_v = fy_get(args, "persona", fy_invalid);
	persona = fy_is_string(persona_v) && *fy_castp(&persona_v, "") ?
		fy_get(fyai_agent_personas(ctx), persona_v, fy_invalid) :
		fy_invalid;
	if (fy_is_string(persona_v) &&
	    *fy_castp(&persona_v, "") &&
	    !fy_is_mapping(persona)) {
		persona_names = fyai_agent_persona_names(ctx);
		fyai_error(ctx, "no persona '%s'; configured: %s",
			   fy_castp(&persona_v, ""),
			   persona_names ? persona_names : "unknown");
		free(persona_names);
		goto err;
	}

	/* The call overrides the persona's conversation mode. */
	context_v = fy_get(args, "context", fy_invalid);
	if (!fy_is_string(context_v))
		context_v = fy_get(persona, "context", fy_invalid);
	if (ctx->agent_branch)
		fork_mode = !fy_equal(context_v, "fresh");
	/* A revived agent keeps its existing conversation. */
	if (revive)
		fork_mode = false;
	else if (!fork_mode)
		ctx->last_message = fy_invalid;
	name = fy_get(args, "name", fy_invalid);
	description = fy_get(args, "description", fy_invalid);

	/* Apply child settings after the arena restores branch configuration. */
	cfg->agent_child = true;
	cfg->mcp_enabled = false;
	cfg->system_prompt = fyai_agent_system_prompt;
	if (fy_is_mapping(persona)) {
		rc = fyai_agent_persona_apply(ctx, persona, fork_mode);
		fyai_error_check(ctx, !rc, err,
				 "could not apply the sub-agent persona");
	}

	if (!ctx->tool_rpc && ctx->curl) {
		curl_easy_cleanup(ctx->curl);
		ctx->curl = NULL;
	}
	rc = fyai_curl_easy_reinit(ctx);
	fyai_error_check(ctx, !rc, err,
			 "could not initialize the sub-agent request");

	rc = fyai_request_state_apply(ctx);
	fyai_error_check(ctx, !rc, err,
			 "could not apply sub-agent request settings");

	/* Add a persona to a fork as a user instruction. */
	if (fork_mode && !revive) {
		ctx->last_message = fyai_turn_append(ctx, ctx->last_message,
			fy_sequence(fyai_make_user_message(ctx,
					cfg->system_prompt)));
		ctx->last_message = fyai_output_record(ctx, ctx->last_message,
			FYAI_OUTPUT_USER, cfg->system_prompt);
	}
	if (!fork_mode && !revive) {
		ctx->last_message = fyai_turn_append(ctx, ctx->last_message,
			fy_sequence(fyai_make_system_message(ctx,
					cfg->system_prompt)));
		ctx->last_message = fyai_output_record(ctx, ctx->last_message,
			FYAI_OUTPUT_SYSTEM, cfg->system_prompt);
	}
	task_v = fy_get(args, "task", fy_invalid);
	task = fy_castp(&task_v, "");
	ctx->last_message = fyai_turn_append(ctx, ctx->last_message,
		fy_sequence(fyai_make_user_message(ctx, task)));
	ctx->last_message = fyai_output_record(ctx, ctx->last_message,
		FYAI_OUTPUT_USER, task);
	fyai_prof_stamp(&t_intern);
	ctx->last_message = fy_gb_internalize(ctx->gb, ctx->last_message);
	fyai_prof_since("agent_input_internalize", &t_intern);
	fyai_error_check(ctx, fy_is_valid(ctx->last_message), err,
			 "could not store the sub-agent input");

	/* Render only when the parent supplied a terminal. */
	ctx->stdout_tty = cfg->agent_pty;

	/* Open a promptless display on the supplied terminal. */
	if (cfg->agent_pty && !fyai_ui_open(ctx))
		fyai_ui_prompt_enabled(ctx, false);

	turn = fyai_run_turn(ctx, ctx->last_message);
	while (fy_is_valid(turn) && !ctx->terminate_pending && fyai_event_queued(ctx)) {
		input = fyai_event_take(ctx);

		ctx->last_message = turn;
		ctx->last_message = fyai_turn_append(ctx, turn,
			fy_sequence(fyai_make_user_message(ctx, input)));
		ctx->last_message = fyai_output_record(ctx, ctx->last_message,
			FYAI_OUTPUT_USER, input);
		free(input);
		turn = fyai_run_turn(ctx, ctx->last_message);
	}
	turn = fyai_report_diag(ctx, turn);
	/* Identify the failed sub-agent after its own cause. */
	fyai_error_check(ctx, fy_is_valid(turn), err,
			 "sub-agent '%s' did not complete its turn (model %s, "
			 "%s)", fy_castp(&name, ""),
			 cfg->model ? cfg->model : "unset",
			 fyai_api_to_string(cfg->api_mode));
	ctx->last_message = turn;
	report = fyai_agent_final_text(ctx, turn);

	/* Publish the durable sub-agent conversation. */
	if (ctx->agent_branch) {
		/* Preserve the original agent metadata when reviving it. */
		if (!revive)
			ctx->branch_agent = fy_gb_mapping(ctx->gb,
				"name", fyai_generic_or_null(name),
				"description", fyai_generic_or_null(description),
				"context", fork_mode ? "fork" : "fresh",
				"persona", fyai_generic_or_null(persona_v));
		/*
		 * Store the resolved provider and model on the branch. A revive
		 * without a persona resolves the same offering from them. The
		 * persona path can keep the provider prefix on the wire model.
		 * Store the bare model to prevent a doubled prefix. A fork keeps
		 * the parent model, and an agent with no persona keeps the parent
		 * configuration. Neither stores a model key.
		 */
		persona_model = fy_get(persona, "model", fy_invalid);
		if (!fork_mode && fy_is_string(persona_model) &&
		    !fy_str_empty(cfg->model) && !fy_str_empty(cfg->provider) &&
		    fy_is_valid(fyai_catalog_provider(
			    fyai_catalog_effective(cfg->catalog, cfg->gb),
			    cfg->provider))) {
			model_bare = strchr(cfg->model, '/');
			model_bare = model_bare ? model_bare + 1 : cfg->model;
			ctx->arena_config = fy_set_at_pathstr(ctx->gb,
				ctx->arena_config, "model",
				fy_stringf(ctx->gb, "%s/%s", cfg->provider,
					   model_bare));
			fyai_error_check(ctx, fy_is_valid(ctx->arena_config), err,
					 "could not persist the sub-agent model");
		}
		fyai_branch_op_set(ctx, revive ? FYAI_BRANCH_OP_TURN :
					 FYAI_BRANCH_OP_CREATE, NULL);
		rc = fyai_publish_state(ctx);
		fyai_error_check(ctx, !rc, err,
			"could not publish the sub-agent conversation on '%s'",
			ctx->agent_branch);
	}
	/*
	 * The turn and its final report say whether the sub-agent completed.
	 * A tool call it recovered from raises a diagnostic of its own, and
	 * the run that recovered is not a failed one.
	 */
	fyai_error_check(ctx,
			 fy_is_string(report) &&
			 *fy_castp(&report, ""),
			 err, "the sub-agent returned no final report");

	*okp = true;
	fyai_ui_close(ctx);
	return report;

err:
	fyai_ui_close(ctx);
	free(args_json);
	return fy_invalid;
}

static int fyai_agent_rpc_verb(struct fyai_ctx *ctx);

int fyai_agent_verb(struct fyai_ctx *ctx)
{
	fy_generic report;
	const char *task;
	bool ok = false;

	if (ctx->cfg->agent_rpc)
		return fyai_agent_rpc_verb(ctx);

	task = ctx->cfg->prompt ? ctx->cfg->prompt : "";
	report = fyai_agent_run(ctx,
			fy_mapping(fyai_ctx_transient_gb(ctx),
				   "task", task),
			&ok);
	if (!ok || fy_is_invalid(report)) {
		fyai_error(ctx, "agent: the sub-agent did not complete");
		return -1;
	}
	fyai_result(ctx, "%s\n", fy_castp(&report, ""));
	return 0;
}

struct agent_rpc {
	struct fyai_ctx *ctx;
	struct jsonrpc_conn *conn;
	fy_generic run_id;
	const char *task;
	const char *name;
	const char *description;
	bool run_pending;
	bool ran;
	bool quit;
};

static fy_generic agent_rpc_error(struct fyai_ctx *ctx, long long code,
				  const char *message)
{
	return fy_mapping(fyai_ctx_transient_gb(ctx),
			  "code", code, "message", message);
}

/* Build an agent/run error that includes the collected cause. */
static fy_generic agent_rpc_failure(struct fyai_ctx *ctx)
{
	fy_generic error;
	char *why;

	why = fyai_diag_string(&ctx->cfg->diag);
	error = fy_gb_mapping(fyai_ctx_transient_gb(ctx),
			      "code", -32000LL,
			      "message", "the sub-agent did not complete",
			      "data", !fy_str_empty(why) ? fy_value(why) :
				      fy_null);
	free(why);
	return error;
}

static fy_generic agent_rpc_serve(struct jsonrpc_conn *conn, const char *method,
				  fy_generic params, fy_generic id,
				  void *userdata, fy_generic *errorp)
{
	struct agent_rpc *rpc = userdata;
	struct fyai_ctx *ctx = rpc->ctx;
	const char *task;

	if (!strcmp(method, "initialize")) {
		return fy_mapping(fyai_ctx_transient_gb(ctx),
				  "agentId", fy_get(params, "name", "agent"),
				  "protocol", 1LL);
	}
	if (!strcmp(method, "shutdown")) {
		rpc->quit = true;
		return fy_null;
	}
	if (!strcmp(method, "agent/run")) {
		if (!fy_is_valid(id)) {
			return fy_invalid;
		}
		if (rpc->ran || rpc->run_pending) {
			*errorp = agent_rpc_error(ctx, -32003,
						  "agent is already running");
			return fy_invalid;
		}
		task = fy_get(params, "task", "");
		if (!task || !*task) {
			*errorp = agent_rpc_error(ctx, -32602,
						  "task is required");
			return fy_invalid;
		}
		rpc->task = task;
		rpc->name = fy_get(params, "name", "agent");
		rpc->description = fy_get(params, "description", "");
		rpc->run_id = id;
		rpc->run_pending = true;
		jsonrpc_conn_defer(conn);
		return fy_invalid;
	}

	*errorp = agent_rpc_error(ctx, -32601, "method not found");
	return fy_invalid;
}

static int fyai_agent_rpc_verb(struct fyai_ctx *ctx)
{
	struct agent_rpc rpc;
	struct fyai_event_loop *el;
	struct jsonrpc_conn *conn;
	fy_generic report;
	bool ok;
	int rc = -1;

	memset(&rpc, 0, sizeof(rpc));
	rpc.ctx = ctx;

	el = fyai_ctx_loop(ctx);
	fyai_error_check(ctx, el, out,
			 "agent: could not acquire the application event loop");

	conn = jsonrpc_conn_stdio(ctx, STDOUT_FILENO, STDIN_FILENO,
				  0, "agent", NULL);
	fyai_error_check(ctx, conn, out,
			 "agent: could not open the control channel");
	rc = jsonrpc_conn_serve(conn, agent_rpc_serve, &rpc);
	fyai_error_check(ctx, !rc, out_conn,
			 "agent: could not serve the control channel");
	rpc.conn = conn;

	while (!rpc.quit) {
		if (rpc.run_pending) {
			rpc.run_pending = false;
			rpc.ran = true;
			report = fyai_agent_run(ctx,
					fy_mapping(fyai_ctx_transient_gb(ctx),
						   "task", rpc.task,
						   "name", rpc.name ?
							rpc.name : "agent",
						   "description",
						   rpc.description ?
							rpc.description : ""),
					&ok);
			if (ok && fy_is_valid(report))
				jsonrpc_conn_respond(conn, rpc.run_id,
					fy_mapping(fyai_ctx_transient_gb(ctx),
						   "report", report),
					fy_invalid);
			else
				jsonrpc_conn_respond(conn, rpc.run_id,
					fy_invalid,
					agent_rpc_failure(ctx));
			continue;
		}
		if (fyai_event_loop_step(el, -1) < 0)
			break;
	}

	while (jsonrpc_conn_has_output(conn))
		if (fyai_event_loop_step(el, 1000) <= 0)
			break;
	rc = 0;

out_conn:
	jsonrpc_conn_destroy(conn);
out:
	return rc;
}
