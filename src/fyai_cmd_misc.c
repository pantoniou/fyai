/*
 * fyai_cmd_misc.c - handlers of the smaller commands
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * The backends present what they read; a handler sets the arguments that
 * the backend takes from the configuration and calls it.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai.h"
#include "fyai_transport_boot.h"
#include "fyai_cmd.h"
#include "fyai_cmd_int.h"
#include "fyai_agents.h"
#include "fyai_branch.h"
#include "fyai_config.h"
#include "fyai_display.h"
#include "fyai_session.h"
#include "fyai_tools.h"
#include "fyai_ui.h"
#include "fyai_auth.h"
#include "fyai_catalog.h"
#include "fyai_log.h"
#include "fyai_merge.h"
#include "fyai_secret.h"
#include "fyai_storage.h"
#include "utils.h"

#define FYAI_MODULE FYAIEM_UNKNOWN

int fyai_cmd_reset(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = call->ctx;
	const char *ref = fyai_cmd_arg_str(call, "ref");
	const char *branch;
	long long turns;
	int rc;

	if (call->surface == FYAI_CMD_SESSION)
		fyai_error_check(ctx, !fyai_ui_busy(ctx) &&
				 !fyai_tools_active(ctx) &&
				 !fyai_agents_attached(ctx), err,
				 "branch changes require idle model and tool "
				 "work");
	rc = fyai_branch_reset(ctx, ref);
	fyai_error_check(ctx, !rc, err, "reset: could not move the branch "
			 "head");
	if (call->surface == FYAI_CMD_SESSION) {
		fyai_session_banner_update(ctx);
		fyai_ui_repaint(ctx);
	}
	branch = fyai_ctx_branch(ctx);
	turns = fyai_branch_turn_count(ctx->last_message,
				       FYAI_BRANCH_WALK_MAX);
	*result = fy_mapping(call->gb, "branch", branch, "ref", ref,
			     "turns", turns,
			     "previous", fy_sprintfa("%s@{1}", branch));
	return 0;
err:
	return -1;
}

int fyai_cmd_clear(struct fyai_cmd_call *call, fy_generic *result)
{
	if (fyai_session_clear(call->ctx))
		return -1;
	*result = fy_mapping(call->gb, "branch", fyai_ctx_branch(call->ctx));
	return 0;
}

/*
 * The API grammar in use, after a switch to MODE when one is given. The
 * switch stores the grammar; a live session also needs a credential.
 */
int fyai_cmd_api(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = call->ctx;
	struct fyai_cfg *cfg = ctx->cfg;
	const char *mode = fyai_cmd_arg_str(call, "mode");
	enum fyai_api_mode before = cfg->api_mode;

	if (fyai_session_api(ctx, mode, call->surface == FYAI_CMD_SESSION))
		return -1;
	*result = fy_mapping(call->gb,
		"api", fyai_api_to_string(cfg->api_mode),
		"model", cfg->model ? cfg->model : "",
		"provider", cfg->provider ? cfg->provider : "?",
		"url", cfg->api_url ? cfg->api_url : "?",
		"max_tokens", (long long)cfg->max_tokens,
		"unchanged", mode && before == cfg->api_mode,
		"shown", !mode || before != cfg->api_mode);
	return 0;
}

int fyai_cmd_context(struct fyai_cmd_call *call, fy_generic *result)
{
	*result = fyai_session_context_data(call->ctx, call->gb);
	fyai_error_check(call->ctx, fy_is_valid(*result), err,
			 "context: cannot build the report");
	return 0;
err:
	return -1;
}

/*
 * The table of stats has formats of its own, currency and percent, that the
 * table options do not express; it keeps its renderer. The machine formats
 * take the data.
 */
int fyai_cmd_stats(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_stats_args *args = &call->ctx->cfg->cmd.args.stats;
	struct fyai_stats_args saved = *args;
	int rc;

	if (call->format != FYAI_CMD_OUT_MARKDOWN) {
		*result = fyai_stats_data(call->ctx, call->gb);
		fyai_error_check(call->ctx, fy_is_valid(*result), err,
				 "stats: cannot build the report");
		return 0;
	}
	args->format = fyai_cmd_arg_bool(call, "raw") ? FYAIOF_RAW :
		       FYAIOF_MARKDOWN;
	rc = fyai_show_stats(call->ctx);
	*args = saved;
	return rc;
err:
	return -1;
}

static bool list_provider_active(struct fyai_cfg *cfg, const char *name)
{
	return cfg->provider && name && !strcmp(cfg->provider, name);
}

/* The providers of the catalogue and the models each serves. */
static fy_generic list_providers(struct fyai_cmd_call *call)
{
	struct fyai_cfg *cfg = call->ctx->cfg;
	struct fy_generic_builder *gb = call->gb;
	fy_generic p, o, rows, models, active_model, name, canon;
	bool active;

	rows = fy_seq_empty;
	fy_foreach(p, fy_get(fyai_catalog_effective(cfg->catalog, gb),
			     "providers", fy_invalid)) {
		name = fy_get(p, "name", fy_invalid);
		active = list_provider_active(cfg, fy_castp(&name, ""));
		models = fy_seq_empty;
		active_model = fy_null;
		fy_foreach(o, fy_get(p, "models", fy_invalid)) {
			canon = fy_get(o, "canonical_id", fy_invalid);
			models = fy_append(gb, models, canon);
			if (active && cfg->model &&
			    (fy_equal(canon, cfg->model) ||
			     fy_equal(fy_get(o, "provider_model_id",
					     fy_invalid), cfg->model)))
				active_model = canon;
		}
		rows = fy_append(gb, rows, fy_mapping(gb,
			"name", name, "active", active, "models", models,
			"active_model", active_model));
	}
	return rows;
}

/* True when @model of @provider is the model of the session. */
static bool list_model_active(struct fyai_cfg *cfg, fy_generic provider,
			      fy_generic model)
{
	fy_generic name, o;

	if (!cfg->model)
		return false;
	if (fy_equal(model, cfg->model))
		return true;
	name = fy_get(provider, "name", fy_invalid);
	if (!list_provider_active(cfg, fy_castp(&name, "")))
		return false;
	fy_foreach(o, fy_get(provider, "models", fy_invalid))
		if (fy_equal(fy_get(o, "canonical_id", fy_invalid), model))
			return fy_equal(fy_get(o, "provider_model_id",
					       fy_invalid), cfg->model);
	return false;
}

/* The models of the catalogue and the providers that serve each. */
static fy_generic list_models(struct fyai_cmd_call *call, bool full)
{
	struct fyai_cfg *cfg = call->ctx->cfg;
	struct fy_generic_builder *gb = call->gb;
	fy_generic cat, m, p, o, rows, names, name;
	bool active;

	cat = fyai_catalog_effective(cfg->catalog, gb);
	rows = fy_seq_empty;
	fy_foreach(m, fy_get(cat, "models", fy_invalid)) {
		name = fy_get(m, "name", fy_invalid);
		active = false;
		names = fy_seq_empty;
		fy_foreach(p, fy_get(cat, "providers", fy_invalid)) {
			fy_foreach(o, fy_get(p, "models", fy_invalid)) {
				if (!fy_equal(fy_get(o, "canonical_id",
						     fy_invalid), name))
					continue;
				names = fy_append(gb, names,
						  fy_get(p, "name", fy_invalid));
				active |= list_model_active(cfg, p, name);
				break;
			}
		}
		rows = fy_append(gb, rows, fy_null_filtered_mapping(gb,
			"name", name,
			"active", active,
			"providers", names,
			"context_window", fy_get(m, "context_window", 0LL),
			"max_output_tokens", fy_get(m, "max_output_tokens", 0LL),
			"open_source", fy_get(m, "open_source", false),
			"display_name", full ? fy_get(m, "display_name", fy_null) :
				fy_null,
			"modalities", full ? fy_get(m, "modalities", fy_seq_empty) :
				fy_null,
			"capabilities", full ? fy_get(m, "capabilities",
						      fy_seq_empty) : fy_null));
	}
	return rows;
}

/* Strong text for what the session uses, in the Markdown table only. */
static fy_generic list_strong(struct fy_generic_builder *gb, fy_generic v)
{
	const char *s = fy_sprintfa("**%s**", fy_str(v) ? : "");

	return fy_value(gb, s);
}

static fy_generic list_decorate(struct fyai_cmd_call *call, fy_generic rows,
				const char *what)
{
	struct fyai_cfg *cfg = call->ctx->cfg;
	struct fy_generic_builder *gb = call->gb;
	fy_generic row, out, items, item, list;
	const char *key, *active_provider;

	active_provider = cfg->auth_mode == FYAI_AUTH_CHATGPT ? "chatgpt" :
			  cfg->provider;
	key = !strcmp(what, "providers") ? "models" : "providers";
	out = fy_seq_empty;
	fy_foreach(row, rows) {
		items = fy_seq_empty;
		fy_foreach(item, fy_get(row, key, fy_invalid)) {
			if (!strcmp(what, "providers") ?
			    fy_equal(item, fy_get(row, "active_model",
						  fy_invalid)) :
			    (active_provider && fy_equal(item,
							 active_provider)))
				item = list_strong(gb, item);
			items = fy_append(gb, items, item);
		}
		list = fy_assoc(gb, row, key, items);
		if (fy_get(row, "active", false))
			list = fy_assoc(gb, list, "name",
					list_strong(gb, fy_get(row, "name",
							       fy_invalid)));
		out = fy_append(gb, out, list);
	}
	return out;
}

static fy_generic list_renderopts(struct fy_generic_builder *gb,
				  const char *what, bool full)
{
	fy_generic api = fy_mapping(gb, "api", fy_mapping(gb, "name", "API"));

	if (!strcmp(what, "turns"))
		return fy_mapping(gb, "empty", "no turns",
			"keys", fy_sequence(gb, "index", "role", "provider",
					    "api", "tokens"),
			"columns", api);
	if (!strcmp(what, "exchanges"))
		return fy_mapping(gb, "empty", "no exchanges",
			"keys", fy_sequence(gb, "index", "provider", "api",
					    "tokens"),
			"columns", api);
	if (!strcmp(what, "reflog"))
		return fy_mapping(gb, "empty", "no ref log",
			"keys", fy_sequence(gb, "index", "ref", "kind", "from",
					    "model", "created", "project"),
			"columns", fy_mapping(gb,
				"ref", fy_mapping(gb, "name", "Ref",
						  "align", "left"),
				"from", fy_mapping(gb, "name", "From",
						   "align", "left"),
				"created", fy_mapping(gb, "name", "When",
						      "align", "left",
						      "format", "time")));
	if (!strcmp(what, "models"))
		return fy_mapping(gb, "empty", "no models",
			"keys", full ?
				fy_sequence(gb, "name", "display_name",
					    "providers", "context_window",
					    "max_output_tokens", "open_source",
					    "modalities", "capabilities") :
				fy_sequence(gb, "name", "providers",
					    "context_window",
					    "max_output_tokens", "open_source"),
			"columns", fy_mapping(gb,
				"name", fy_mapping(gb, "name", "Model"),
				"context_window", fy_mapping(gb, "name",
							     "Context"),
				"max_output_tokens", fy_mapping(gb, "name",
								"Max Output"),
				"open_source", fy_mapping(gb, "name", "Open",
							  "align", "left"),
				"display_name", fy_mapping(gb, "name",
							   "Display")));
	return fy_mapping(gb, "empty", "no providers configured",
		"keys", fy_sequence(gb, "name", "models"),
		"columns", fy_mapping(gb,
			"name", fy_mapping(gb, "name", "Provider")));
}

int fyai_cmd_list(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = call->ctx;
	const char *what = fyai_cmd_arg_str(call, "what");
	bool full = fyai_cmd_arg_bool(call, "full");
	fy_generic rows;

	if (!strcmp(what, "models"))
		rows = ctx->cfg->auth_mode == FYAI_AUTH_CHATGPT ?
		       fyai_auth_models(ctx, call->gb, full) :
		       list_models(call, full);
	else if (!strcmp(what, "turns"))
		rows = fyai_list_turns_data(ctx, call->gb);
	else if (!strcmp(what, "exchanges"))
		rows = fyai_list_exchanges_data(ctx, call->gb);
	else if (!strcmp(what, "reflog"))
		rows = fyai_list_reflog_data(ctx, call->gb);
	else
		rows = list_providers(call);
	fyai_error_check(ctx, fy_is_valid(rows), err, "list: cannot build "
			 "the %s", what);
	call->renderopts = list_renderopts(call->gb, what, full);
	if (fyai_cmd_arg_bool(call, "raw"))
		call->renderopts = fy_assoc(call->gb, call->renderopts, "raw",
					    true);
	if (call->format == FYAI_CMD_OUT_MARKDOWN &&
	    (!strcmp(what, "models") || !strcmp(what, "providers")))
		rows = list_decorate(call, rows, what);
	*result = rows;
	return 0;
err:
	return -1;
}

int fyai_cmd_diff(struct fyai_cmd_call *call, fy_generic *result)
{
	(void)result;
	return fyai_export_diff(call->ctx, fyai_cmd_arg_str(call, "from"),
				fyai_cmd_arg_str(call, "to"),
				fyai_cmd_arg_bool(call, "unified"));
}

int fyai_cmd_root(struct fyai_cmd_call *call, fy_generic *result)
{
	return fyai_root_data(call->ctx, call->gb,
			      fyai_cmd_arg_str(call, "ref"),
			      fy_equal(fy_get(call->def, "command", fy_invalid),
				       "show"), result);
}

int fyai_cmd_join(struct fyai_cmd_call *call, fy_generic *result)
{
	bool rebase;

	(void)result;
	rebase = fy_equal(fy_get(call->def, "command", fy_invalid), "rebase");
	return fyai_branch_join(call->ctx, fyai_cmd_arg_str(call, "branch"),
				rebase ? FYAI_JOIN_REBASE : FYAI_JOIN_MERGE,
				fyai_cmd_arg_bool(call, "allow_unrelated"));
}

int fyai_cmd_gc(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_gc_args *args = &call->ctx->cfg->cmd.args.gc;
	int rc;

	args->keep_reflogs = fy_get(call->args, "keep_reflogs", -1LL);
	rc = fyai_gc_storage(call->ctx);
	if (rc < 0)
		return -1;
	*result = fy_mapping(call->gb, "arena", call->ctx->cfg->arena_dir,
			     "compacted", rc == 0, "missing", rc == 1);
	return 0;
}

int fyai_cmd_export(struct fyai_cmd_call *call, fy_generic *result)
{
	(void)result;
	return fyai_export_view(call->ctx, fyai_cmd_arg_str(call, "file"),
				fyai_cmd_arg_str(call, "ref"));
}

/* A status line of the logs: on and off are words, not true and false. */
static fy_generic log_words(struct fy_generic_builder *gb, fy_generic data)
{
	fy_generic out, key, value;

	out = fy_map_empty;
	fy_foreach_key_value(key, value, data)
		out = fy_assoc(gb, out, key, fy_equal(value, true) ? "on" :
			       "off");
	return out;
}

/* The action is the subcommand; the target is the argument of the group. */
int fyai_cmd_log(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = call->ctx;
	fy_generic action = fy_get(call->def, "command", fy_invalid);
	const char *target = fyai_cmd_arg_str(call, "target");
	fy_generic data;

	if (fy_equal(action, "view"))
		return fyai_log_view(ctx, target);
	if (fy_equal(action, "clear")) {
		if (fyai_log_clear_log(ctx, target))
			return -1;
		*result = fy_mapping(call->gb, "cleared", target);
		return 0;
	}
	if (fy_any_equal(action, "start", "stop"))
		fyai_log_set(ctx->cfg, target, fy_equal(action, "start"));
	/* The wire and the transport logs are written by the transport. */
	fyai_transport_sync_logging(ctx);
	data = fyai_log_status_data(ctx, call->gb);
	*result = call->format == FYAI_CMD_OUT_MARKDOWN ?
		  log_words(call->gb, data) : data;
	return 0;
}

int fyai_cmd_secret(struct fyai_cmd_call *call, fy_generic *result)
{
	fy_generic cmd = fy_get(call->def, "command", fy_invalid);
	enum fyai_secret_command command;

	command = fy_equal(cmd, "set") ? FYAI_SECRET_SET :
		  fy_equal(cmd, "delete") ? FYAI_SECRET_DELETE :
		  FYAI_SECRET_STATUS;
	if (command == FYAI_SECRET_STATUS) {
		/* A backend that is not there is the answer, not a failure:
		 * the status says so. */
		*result = fyai_secret_status_data(call->ctx, call->gb,
						  fyai_cmd_arg_str(call,
								   "name"));
		fyai_error_check(call->ctx, fy_is_valid(*result), err,
				 "secret: cannot build the status");
		return 0;
	}
	/* A value is read from the terminal, or from standard input with
	 * --stdin: never from the command line. */
	return fyai_secret_action(call->ctx, command,
				  fyai_cmd_arg_str(call, "name"),
				  fyai_cmd_arg_bool(call, "stdin"));
err:
	return -1;
}

int fyai_cmd_compact(struct fyai_cmd_call *call, fy_generic *result)
{
	(void)result;
	return fyai_session_compact(call->ctx, fyai_cmd_arg_str(call, "hint"));
}

int fyai_cmd_render(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *file = fyai_cmd_arg_str(call, "file");
	char *md;

	if (fy_str_empty(file) || !strcmp(file, "-"))
		md = read_all_stdin();
	else
		md = read_text_file(file);
	fyai_error_check(call->ctx, md, err,
			 "render: cannot read %s", fy_str_empty(file) ||
			 !strcmp(file, "-") ? "standard input" : file);
	*result = fy_value(call->gb, (const char *)md);
	free(md);
	fyai_error_check(call->ctx, fy_is_valid(*result), err,
			 "render: cannot keep the Markdown text");
	return 0;
err:
	return -1;
}
