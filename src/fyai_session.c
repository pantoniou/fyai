/*
 * fyai_session.c - session commands (/clear, /compact, /model, /context)
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * One backend per command, shared between the interactive slash dispatcher
 * and the CLI verb forms (fyai clear|compact|context). A second, data-driven
 * family handles simple session settings (/effort, /theme, ...): each entry
 * points at a fyai_cfg field, optionally with an enum value list used for
 * both validation and tab completion. Request-shaping switches (/model, /api,
 * the reasoning options, /temperature) persist into the arena config through
 * the one commit path, so a continuation resumes on them; display settings
 * stay session-only and `config set` / `--set` remain the durable forms.
 */

/*
 * Many verbs report from here (compact, api, list, history, secret), so each
 * message names its own rather than taking one module prefix for the file.
 */
#define FYAI_MODULE FYAIEM_UNKNOWN

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>
#include <alloca.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libfytimui.h>

#include "commands.h"
#include "fyai_sink.h"
#include "fyai.h"
#include "fyai_auth.h"
#include "fyai_catalog.h"
#include "fyai_config.h"
#include "fyai_display.h"
#include "fyai_event.h"
#include "fyai_render.h"
#include "fyai_log.h"
#include "fyai_markdown.h"
#include "fyai_output.h"
#include "fyai_provider.h"
#include "fyai_branch.h"
#include "fyai_browser.h"
#include "fyai_agent.h"
#include "fyai_agents.h"
#include "fyai_session.h"
#include "fyai_cmd.h"
#include "fyai_stream.h"
#include "fyai_xfer.h"
#include "fyai_ui.h"
#include "fyai_storage.h"
#include "fyai_terminal.h"
#include "fyai_tools.h"
#include "fyai_turn.h"
#include "utils.h"

/*
 * The catalogue entry for the active model, tolerating the wire-id rewrite
 * done at resolution (look up by name, then via the canonical id of the
 * offering). fy_invalid when the catalogue does not know the model.
 */
static fy_generic session_model_entry(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg = ctx->cfg;
	fy_generic catalog;

	catalog = fyai_catalog_effective(cfg->catalog, cfg->gb);
	return fyai_catalog_resolved_model(catalog, cfg->model);
}

long long fyai_context_window(struct fyai_ctx *ctx)
{
	return fy_get(session_model_entry(ctx), "context_window", 0LL);
}

static void session_reset_usage(struct fyai_ctx *ctx)
{
	ctx->usage_input = 0;
	ctx->usage_cached = 0;
	ctx->usage_cache_write = 0;
	ctx->usage_output = 0;
	ctx->usage_reasoning = 0;
	ctx->usage_total = 0;
	ctx->usage_cost = 0.0;
	ctx->usage_calls = 0;
	ctx->last_call_input = 0;
	ctx->last_call_output = 0;
	ctx->last_call_total = 0;
}

int fyai_session_clear(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg = ctx->cfg;

	ctx->last_message = fy_invalid;
	fyai_branch_op_set(ctx, FYAI_BRANCH_OP_CLEAR, NULL);
	session_reset_usage(ctx);

	/*
	 * In a live request session re-seed the system prompt so the next
	 * turn starts a well-formed chain; the verb form has no transient
	 * builder and leaves seeding to the next prompt run's setup.
	 */
	if (ctx->transient_gb && fyai_cfg_makes_requests(cfg)) {
		ctx->last_message = fyai_turn_append(ctx, fy_invalid,
			fy_sequence(fyai_make_system_message(ctx,
							     cfg->system_prompt)));
		ctx->last_message = fy_gb_internalize(ctx->gb,
						      ctx->last_message);
	}

	if (fyai_publish_state(ctx))
		return -1;
	return 0;
}

static int session_compact_responses(struct fyai_ctx *ctx, const char *hint)
{
	struct fyai_cfg *cfg = ctx->cfg;
	struct fyai_buffered_request *req;
	struct fyai_event_loop *el;
	fy_generic prev_head, messages, input, request, response, output;
	fy_generic turn, meta;
	const char *body;
	const char *url;
	size_t url_len;
	bool endpoint_valid;
	int rc;

	req = NULL;
	rc = -1;
	response = fy_invalid;
	prev_head = ctx->last_message;
	messages = fyai_turn_messages_since(ctx, prev_head, fy_invalid);
	input = fyai_responses_input(ctx, messages);
	fyai_error_check(ctx, fy_is_valid(input), out,
			 "compact: cannot build the Responses input");

	request = fy_mapping(ctx->transient_gb,
		"model", cfg->model,
		"instructions", hint && *hint ?
			fy_stringf("%s\n\nWhen compacting, preserve information "
				   "relevant to: %s", cfg->system_prompt, hint) :
			fy_value(cfg->system_prompt),
		"input", input);
	if (fy_is_valid(ctx->tools) && fy_len(ctx->tools))
		request = fy_assoc(request, "tools", ctx->tools);
	fyai_error_check(ctx, fy_is_valid(request), out,
			 "compact: cannot build the Responses request");

	url_len = strlen(cfg->api_url);
	endpoint_valid = url_len >= strlen("/responses") &&
		!strcmp(cfg->api_url + url_len - strlen("/responses"),
			"/responses");
	fyai_error_check(ctx, endpoint_valid, out,
			 "compact: cannot derive the Responses compact endpoint from %s",
			 cfg->api_url);
	url = fy_sprintfa("%s/compact", cfg->api_url);
	body = emit_request_body(ctx->transient_gb, request);
	fyai_error_check(ctx, url && body, out,
			 "compact: cannot encode the Responses request");

	if (cfg->conversation_logging)
		(void)fyai_log_generic(ctx, "conversation",
			fy_mapping(ctx->transient_gb,
				"kind", "request",
				"api", "responses-compact",
				"url", url,
				"body", request));

	fyai_xfer_set_endpoint(ctx, url, "compact");
	fyai_xfer_set_body(ctx, body);
	req = fyai_buffered_request_submit(ctx, NULL, NULL);
	fyai_error_check(ctx, req, err_restore,
			 "compact: cannot submit the Responses request");

	el = fyai_ctx_loop(ctx);
	assert(el);
	while (!fyai_buffered_request_done(req)) {
		if (ctx->interrupt_pending) {
			fyai_event_interrupt_ack(ctx);
			fyai_buffered_request_cancel(req);
		}
		rc = fyai_event_loop_step(el, -1);
		fyai_error_check(ctx, rc >= 0, err_collect,
				 "compact: the event loop failed");
	}
	rc = 0;
collect:
	response = fyai_buffered_request_collect(req);
	fyai_buffered_request_destroy(req);
	req = NULL;
restore:
	fyai_xfer_set_endpoint(ctx, NULL, NULL);
	fyai_xfer_set_body(ctx, NULL);
	if (rc < 0)
		goto out;

	response = fyai_report_diag(ctx, response);
	fyai_error_check(ctx, fy_is_valid(response) &&
			 !fy_is_null(response), out,
			 "compact: Responses compact request failed");
	if (cfg->conversation_logging)
		(void)fyai_log_generic(ctx, "conversation",
			fy_mapping(ctx->transient_gb,
				"kind", "response",
				"api", "responses-compact",
				"body", response));

	output = fy_get(response, "output");
	fyai_error_check(ctx, fy_is_sequence(output) && fy_len(output), out,
			 "compact: Responses compact returned no output");

	turn = fyai_turn_append(ctx, fy_invalid,
		fy_sequence(fyai_make_system_message(ctx, cfg->system_prompt)));
	fyai_error_check(ctx, fy_is_valid(turn), out,
			 "compact: cannot build the system turn");
	turn = fyai_turn_append(ctx, turn, output);
	fyai_error_check(ctx, fy_is_valid(turn), out,
			 "compact: cannot build the compacted turn");

	meta = fyai_turn_meta(turn);
	if (fy_is_invalid(meta) || fy_is_null(meta))
		meta = fy_map_empty;
	meta = fy_assoc(meta, "compacted_from", prev_head);
	/* Store the instructions needed to repeat the compaction. */
	if (hint && *hint)
		meta = fy_assoc(meta, "compact_instructions", hint);
	fyai_branch_op_set(ctx, FYAI_BRANCH_OP_COMPACT, NULL);
	turn = fy_assoc(turn, "metadata", meta);
	turn = fy_gb_internalize(ctx->gb, turn);
	fyai_error_check(ctx, fy_is_valid(turn), out,
			 "compact: cannot store the compacted turn");

	ctx->last_message = turn;
	session_reset_usage(ctx);
	rc = fyai_publish_state(ctx);
	fyai_error_check(ctx, !rc, out,
			 "compact: cannot publish the compacted turn");
	fyai_result(ctx, "conversation compacted (Responses API)\n");
	return 0;
err_collect:
	rc = -1;
	goto collect;
err_restore:
	rc = -1;
	goto restore;
out:
	if (req)
		fyai_buffered_request_destroy(req);
	return -1;
}

bool fyai_session_compact_v2(const struct fyai_cfg *cfg)
{
	return cfg->api_mode == FYAI_API_RESPONSES &&
		cfg->response_compaction_supported && cfg->chatgpt_auth;
}

static int session_compact_responses_v2(struct fyai_ctx *ctx,
					const char *hint)
{
	struct fyai_cfg *cfg = ctx->cfg;
	fy_generic prev_head, messages, compact_messages, message;
	fy_generic turn, result, provider_stream, provider, output, item, meta;
	const char *provider_name;
	const char *instructions;
	int rc;

	prev_head = ctx->last_message;
	messages = fyai_turn_messages_since(ctx, prev_head, fy_invalid);
	fyai_error_check(ctx, fy_is_valid(messages), out,
			 "compact: cannot collect the Responses input");

	instructions = hint && *hint ?
		fy_sprintfa("%s\n\nWhen compacting, preserve information "
			    "relevant to: %s", cfg->system_prompt, hint) :
		cfg->system_prompt;
	compact_messages = fy_sequence(
		fyai_make_system_message(ctx, instructions));
	fy_foreach(message, messages) {
		if (fy_equal(fy_get(message, "role"), "system"))
			continue;
		compact_messages = fy_append(ctx->transient_gb,
					     compact_messages, message);
	}
	compact_messages = fy_append(ctx->transient_gb, compact_messages,
				     fy_mapping("type", "compaction_trigger"));
	fyai_error_check(ctx, fy_is_valid(compact_messages), out,
			 "compact: cannot build the Responses input trigger");

	turn = fyai_turn_append(ctx, fy_invalid, compact_messages);
	fyai_error_check(ctx, fy_is_valid(turn), out,
			 "compact: cannot build the Responses compaction turn");
	ctx->compacting = true;
	result = fyai_run_turn(ctx, turn);
	ctx->compacting = false;
	result = fyai_report_diag(ctx, result);
	fyai_error_check(ctx, fy_is_valid(result) &&
			 !fy_is_null(result), out,
			 "compact: Responses compaction request failed");

	provider_stream = fy_get(result, "provider_stream");
	provider = fyai_turn_provider(result);
	provider_name = fy_castp(&provider, "");
	output = fy_get(provider_stream, provider_name);
	fyai_error_check(ctx, fy_is_sequence(output) &&
			 fy_len(output) == 1, out,
			 "compact: Responses compaction returned invalid output");
	item = fy_get_at(output, 0);
	fyai_error_check(ctx, fy_equal(fy_get(item, "type"), "compaction"), out,
			 "compact: Responses compaction returned no compaction item");

	turn = fyai_turn_append(ctx, fy_invalid,
		fy_sequence(fyai_make_system_message(ctx, cfg->system_prompt)));
	fyai_error_check(ctx, fy_is_valid(turn), out,
			 "compact: cannot build the system turn");
	turn = fyai_turn_append(ctx, turn, output);
	fyai_error_check(ctx, fy_is_valid(turn), out,
			 "compact: cannot build the compacted turn");

	meta = fyai_turn_meta(turn);
	if (fy_is_invalid(meta) || fy_is_null(meta))
		meta = fy_map_empty;
	meta = fy_assoc(meta, "compacted_from", prev_head);
	if (hint && *hint)
		meta = fy_assoc(meta, "compact_instructions", hint);
	fyai_branch_op_set(ctx, FYAI_BRANCH_OP_COMPACT, NULL);
	turn = fy_assoc(turn, "metadata", meta);
	turn = fy_gb_internalize(ctx->gb, turn);
	fyai_error_check(ctx, fy_is_valid(turn), out,
			 "compact: cannot store the compacted turn");

	ctx->last_message = turn;
	session_reset_usage(ctx);
	rc = fyai_publish_state(ctx);
	fyai_error_check(ctx, !rc, out,
			 "compact: cannot publish the compacted turn");
	fyai_result(ctx, "conversation compacted (Responses API)\n");
	return 0;
out:
	return -1;
}

static fy_generic session_compact_source(struct fyai_ctx *ctx,
					 fy_generic messages);

int fyai_session_compact(struct fyai_ctx *ctx, const char *hint)
{
	struct fyai_cfg *cfg = ctx->cfg;
	fy_generic prev_head, turn, v, msgs, m, meta, content;
	fy_generic all, source, base;
	const char *summary;
	bool tools_save, shell_save;
	size_t n;

	if ((!cfg->api_key || !*cfg->api_key) &&
	    !cfg->chatgpt_auth && !cfg->no_auth) {
		fyai_error(ctx, "compact: no API key or ChatGPT login is available");
		return -1;
	}
	if (fy_is_invalid(ctx->last_message) ||
	    fy_is_null(ctx->last_message)) {
		fyai_result(ctx, "compact: nothing to compact\n");
		return 0;
	}
	assert(ctx->transient_gb);

	if (fyai_session_compact_v2(cfg))
		return session_compact_responses_v2(ctx, hint);

	if (cfg->api_mode == FYAI_API_RESPONSES &&
	    cfg->response_compaction_supported)
		return session_compact_responses(ctx, hint);

	prev_head = ctx->last_message;

	/* Bound the history but retain the real head in compacted_from. */
	base = prev_head;
	all = fyai_turn_messages_since(ctx, prev_head, fy_invalid);
	if (fy_is_valid(all)) {
		source = session_compact_source(ctx, all);
		if (fy_not_equal(source, all)) {
			base = fyai_turn_append(ctx, fy_invalid, source);
			if (fy_is_invalid(base))
				base = prev_head;
		}
	}

	turn = fyai_turn_append(ctx, base,
		fy_sequence(fy_mapping(ctx->gb,
			"role", "user",
			"content", fy_stringf(
				"Summarize this conversation for continuation: decisions "
				"made, current state, and open items. Be complete but "
				"concise; the summary will replace the conversation "
				"history.%s%s",
				hint && *hint ? " Focus on: " : "",
				hint ? hint : ""))));
	if (fy_is_invalid(turn))
		return -1;

	/* One-off summary call: no tools. */
	tools_save = cfg->enable_tools;
	shell_save = cfg->enable_builtin_shell;
	cfg->enable_tools = false;
	cfg->enable_builtin_shell = false;
	ctx->compacting = true;
	v = fyai_run_turn(ctx, turn);
	ctx->compacting = false;
	cfg->enable_tools = tools_save;
	cfg->enable_builtin_shell = shell_save;
	/* The loop carries why it failed on the result; without this a ^C here
	 * reported the generic failure below instead of "interrupted". */
	v = fyai_report_diag(ctx, v);
	if (fy_is_invalid(v) || fy_is_null(v)) {
		fyai_error(ctx, "compact: summary request failed");
		return -1;
	}

	/* The final assistant message of the final turn is the summary. */
	msgs = fy_get(v, "messages", fy_seq_empty);
	n = fy_len(msgs);
	m = n ? fy_get_at(msgs, n - 1) : fy_invalid;
	content = fy_get(m, "content");
	summary = fy_castp(&content, "");
	if (!*summary) {
		fyai_error(ctx, "compact: empty summary");
		return -1;
	}

	/*
	 * Restart the chain: system turn, then the summary as canonical user
	 * content (provider-agnostic, replays anywhere). The old head is kept
	 * reachable in the metadata-events layer for provenance.
	 */
	turn = fyai_turn_append(ctx, fy_invalid,
		fy_sequence(fyai_make_system_message(ctx, cfg->system_prompt)));
	turn = fyai_turn_append(ctx, turn,
		fy_sequence(fy_mapping(ctx->gb,
			"role", "user",
			"content", fy_stringf(
				"Summary of prior conversation:\n\n%s", summary))));
	if (fy_is_invalid(turn))
		return -1;

	meta = fyai_turn_meta(turn);
	if (fy_is_invalid(meta) || fy_is_null(meta))
		meta = fy_map_empty;
	meta = fy_assoc(meta, "compacted_from", prev_head);
	/* Store the instructions needed to repeat the compaction. */
	if (hint && *hint)
		meta = fy_assoc(meta, "compact_instructions", hint);
	fyai_branch_op_set(ctx, FYAI_BRANCH_OP_COMPACT, NULL);
	turn = fy_assoc(turn, "metadata", meta);

	turn = fy_gb_internalize(ctx->gb, turn);
	if (fy_is_invalid(turn))
		return -1;
	ctx->last_message = turn;
	ctx->last_call_input = 0;
	ctx->last_call_output = 0;
	ctx->last_call_total = 0;

	if (fyai_publish_state(ctx))
		return -1;
	fyai_result(ctx, "conversation compacted\n");
	return 0;
}

/*
 * Persist a session switch into the arena config through the one commit
 * path (validate -> commit -> publish root); under --transient the publish
 * stays in the in-memory overlay. The session already runs on the new
 * value, so a failed persist only warns. @value is spliced verbatim into a
 * YAML flow document, so string values must arrive single-quoted.
 */
static void session_persist(struct fyai_ctx *ctx, const char *key,
			    const char *value)
{
	if (fyai_config_set(ctx, key, value))
		fyai_warning(ctx, "%s: could not persist to config", key);
}

/*
 * Persist the active model, pinned to the resolved provider (provider/model
 * form) when the catalogue knows it, so the continuation re-resolves onto
 * the same provider's offering. `api`/`api_url` are re-derived from the
 * catalogue as part of the same commit (config_doc_sync_derived_api, hooked
 * into catalog_sync_config_doc) whenever the model lands on a known
 * catalogue entry, so they need no persisting here.
 */
static void session_persist_model(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg = ctx->cfg;
	fy_generic catalog;
	bool pin;

	if (!cfg->model || !*cfg->model)
		return;
	catalog = fyai_catalog_effective(cfg->catalog, cfg->gb);
	pin = cfg->provider && *cfg->provider &&
	      fy_is_valid(fyai_catalog_provider(catalog,
							cfg->provider));
	session_persist(ctx, "model", pin ?
			fy_sprintfa("'%s/%s'", cfg->provider, cfg->model) :
			fy_sprintfa("'%s'", cfg->model));
}

/*
 * ChatGPT subscription auth stands in for an API key, but only for the OpenAI
 * provider on the Responses grammar. When a session is already running on it
 * (cfg->chatgpt_auth), or the user pinned it (auth_mode CHATGPT), a switch to
 * another such model must not be rejected for lacking an API key.
 */
static bool session_chatgpt_capable(const struct fyai_cfg *cfg,
				    const struct fyai_cfg *tmp)
{
	if (!tmp->provider || fy_not_equal(tmp->provider, "openai"))
		return false;
	if (tmp->api_mode != FYAI_API_RESPONSES)
		return false;
	return cfg->auth_mode == FYAI_AUTH_CHATGPT || cfg->chatgpt_auth;
}

int fyai_session_model(struct fyai_ctx *ctx, const char *name, bool live)
{
	struct fyai_cfg *cfg = ctx->cfg;
	struct fyai_cfg tmp;

	/*
	 * Resolve into a scratch copy so a failed switch leaves the session
	 * untouched. Endpoint/provider/max_tokens are re-derived; the api key
	 * only when it was not explicitly supplied.
	 */
	tmp = *cfg;
	tmp.model = fy_gb_intern_string(cfg->gb, name);
	tmp.api_url = NULL;
	tmp.provider = NULL;
	tmp.max_tokens = DEFAULT_MAX_TOKENS;
	if (!tmp.api_key_explicit)
		tmp.api_key = NULL;

	if (fyai_config_resolve_model(&tmp))
		return -1;
	if (fyai_config_messages_gate(&tmp))
		return -1;
	/* Only a live session sends the next request with it. */
	if (live && (!tmp.api_key || !*tmp.api_key) &&
	    !session_chatgpt_capable(cfg, &tmp)) {
		fyai_error(ctx, "model: no API key for provider '%s' (set %s%s)",
			   tmp.provider ? tmp.provider : "?",
			tmp.provider ? tmp.provider : "PROVIDER",
			"_API_KEY or use --api-key");
		return -1;
	}

	*cfg = tmp;

	/* Re-derive ChatGPT subscription routing (endpoint + token) for the
	 * new model; resolve returns early and harmlessly when an API key is
	 * in use. */
	cfg->chatgpt_auth = false;
	if (live && fyai_auth_resolve(ctx))
		return -1;

	/* Rebuild the derived request state when a live session exists. */
	if (ctx->curl && fyai_request_state_apply(ctx))
		return -1;

	session_persist_model(ctx);
	return 0;
}

static int session_api_parse(const char *s, enum fyai_api_mode *modep)
{
	if (!strcmp(s, "responses"))
		*modep = FYAI_API_RESPONSES;
	else if (!strcmp(s, "chat-completions") || !strcmp(s, "chat"))
		*modep = FYAI_API_CHAT_COMPLETIONS;
	else if (!strcmp(s, "messages"))
		*modep = FYAI_API_MESSAGES;
	else
		return -1;
	return 0;
}

int fyai_session_api(struct fyai_ctx *ctx, const char *arg, bool live)
{
	struct fyai_cfg *cfg = ctx->cfg;
	struct fyai_cfg tmp;
	enum fyai_api_mode mode;
	fy_generic catalog;
	fy_generic prov;

	if (!arg || !*arg)
		return 0;

	if (session_api_parse(arg, &mode)) {
		fyai_error(ctx, "api: unknown grammar '%s' "
			   "(responses|chat-completions|messages)", arg);
		return -1;
	}
	if (mode == cfg->api_mode)
		return 0;

	/*
	 * Resolve into a scratch copy so a failed switch leaves the session
	 * untouched. Pin the current provider (when the catalogue knows it) so
	 * the switch re-targets the same provider's endpoint for the new
	 * grammar instead of re-routing the model to its canonical provider;
	 * the key is re-derived only when it was not explicitly supplied.
	 */
	tmp = *cfg;
	tmp.api_mode = mode;
	tmp.api_url = NULL;
	if (!tmp.api_key_explicit)
		tmp.api_key = NULL;

	catalog = fyai_catalog_effective(cfg->catalog, cfg->gb);
	prov = fyai_catalog_provider(catalog,
				     cfg->provider ? cfg->provider : "");
	if (fy_is_valid(prov))
		tmp.model = fy_gb_intern_string(cfg->gb,
				fy_sprintfa("%s/%s", cfg->provider,
					    cfg->model));
	tmp.provider = NULL;

	if (fyai_config_resolve_model(&tmp))
		return -1;
	/* The resolver falls back to a grammar the provider does offer; an
	 * explicit switch must not silently land somewhere else. */
	if (tmp.api_mode != mode) {
		fyai_error(ctx, "api: provider '%s' does not offer %s",
			   cfg->provider ? cfg->provider : "?",
			   fyai_api_to_string(mode));
		return -1;
	}
	if (fyai_config_messages_gate(&tmp))
		return -1;
	/* Only a live session sends the next request with it. */
	if (live && (!tmp.api_key || !*tmp.api_key) &&
	    !session_chatgpt_capable(cfg, &tmp)) {
		fyai_error(ctx, "api: no API key for provider '%s' (set %s%s)",
			   tmp.provider ? tmp.provider : "?",
			tmp.provider ? tmp.provider : "PROVIDER",
			"_API_KEY or use --api-key");
		return -1;
	}

	*cfg = tmp;

	/* Re-derive ChatGPT subscription routing for the new grammar; resolve
	 * returns early and harmlessly when an API key is in use. */
	cfg->chatgpt_auth = false;
	if (live && fyai_auth_resolve(ctx))
		return -1;

	/* Rebuild the derived request state when a live session exists. */
	if (ctx->curl && fyai_request_state_apply(ctx))
		return -1;

	/*
	 * Persist the provider-pinned model, then the grammar and its resolved
	 * endpoint, so the continuation stays on the provider this switch
	 * re-targeted. api_url is persisted explicitly here (rather than via
	 * the model-change catalogue sync, which only fires when the model
	 * itself changes) since an /api switch alone moves the endpoint.
	 *
	 * The model goes first because each persist is its own commit: pinning
	 * the model is a model change, and that re-derives the grammar and the
	 * endpoint from the provider. Persisted after it, this switch's own
	 * values are what remain.
	 */
	session_persist_model(ctx);
	session_persist(ctx, "api",
			fy_sprintfa("'%s'", fyai_api_to_string(cfg->api_mode)));
	if (cfg->api_url && *cfg->api_url)
		session_persist(ctx, "api_url",
				fy_sprintfa("'%s'", cfg->api_url));

	fyai_result(ctx, "api: %s (model %s, provider %s, url %s)\n",
	       fyai_api_to_string(cfg->api_mode),
	       cfg->model ? cfg->model : "",
	       cfg->provider ? cfg->provider : "?",
	       cfg->api_url ? cfg->api_url : "?");
	return 0;
}

/*
 * Cheap token estimate with no tokenizer: canonical bytes / 4, plus a small
 * per-message overhead. Good enough for a fill gauge.
 */
static long long session_est_bytes(fy_generic v)
{
	enum fy_generic_type t;
	const char *s;
	long long sum;
	size_t i, n;
	fy_generic item;
	fy_generic key;

	t = fy_generic_get_type(v);
	switch (t) {
	case FYGT_STRING:
		s = fy_castp(&v, "");
		return (long long)strlen(s);
	case FYGT_SEQUENCE:
		sum = 0;
		fy_foreach(item, v)
			sum += 2 + session_est_bytes(item);
		return sum;
	case FYGT_MAPPING:
		sum = 0;
		n = fy_generic_mapping_get_pair_count(v);
		for (i = 0; i < n; i++) {
			key = fy_generic_mapping_get_at_key(v, i);
			s = fy_castp(&key, "");
			sum += 4 + (long long)strlen(s) +
			       session_est_bytes(
					fy_generic_mapping_get_at_value(v, i));
		}
		return sum;
	default:
		return 8;
	}
}

static long long session_estimate_tokens_at(fy_generic head)
{
	fy_generic cur, msgs;
	long long bytes, nmsg;

	bytes = 0;
	nmsg = 0;
	fyai_turn_foreach(cur, head) {
		msgs = fy_get(cur, "messages", fy_seq_empty);
		nmsg += (long long)fy_len(msgs);
		bytes += session_est_bytes(msgs);
	}
	return bytes / 4 + nmsg * 4;
}

/*
 * Bound compaction input. Elide oversized content, then drop the oldest
 * messages. Preserve message roles and tool-call keys.
 */
static fy_generic session_compact_source(struct fyai_ctx *ctx,
					 fy_generic messages)
{
	struct fy_generic_builder *gb = ctx->transient_gb;
	fy_generic out, trimmed, m, role, marker;
	long long window, budget, cap, total, bytes;
	size_t i, count, first;
	bool has_system;

	window = fyai_context_window(ctx);
	if (window <= 0)
		return messages;

	/*
	 * Leave the output allowance plus a wide margin: the estimate is
	 * bytes/4, which understates a token-dense history.
	 */
	budget = window - (ctx->cfg->max_tokens > 0 ? ctx->cfg->max_tokens : 0);
	budget = budget * 3 / 4;
	if (budget <= 0)
		return messages;
	if (session_est_bytes(messages) / 4 <= budget)
		return messages;

	cap = budget / 8;
	out = fy_seq_empty;
	fy_foreach(m, messages) {
		bytes = session_est_bytes(m);
		if (bytes / 4 > cap) {
			role = fy_get(m, "role");
			marker = fy_stringf(
				"[fyai: %lld bytes of %s content were elided to "
				"compact an over-full conversation]",
				bytes, fy_castp(&role, "message"));
			m = fy_assoc(gb, m, "content", marker);
		}
		out = fy_append(gb, out, m);
	}

	count = fy_len(out);
	has_system = count &&
		     fy_equal(fy_get(fy_get_at(out, 0), "role"), "system");
	first = has_system ? 1 : 0;

	total = session_est_bytes(out) / 4;
	i = first;
	while (i < count && total > budget) {
		total -= session_est_bytes(fy_get_at(out, i)) / 4;
		i++;
	}
	/* Never start the remainder on a tool result whose call was dropped. */
	while (i < count &&
	       fy_equal(fy_get(fy_get_at(out, i), "role"), "tool"))
		i++;

	if (i > first) {
		trimmed = fy_seq_empty;
		if (has_system)
			trimmed = fy_append(gb, trimmed, fy_get_at(out, 0));
		trimmed = fy_append(gb, trimmed, fy_mapping(gb,
			"role", "user",
			"content", fy_stringf(
				"[fyai: %zu earlier messages were dropped to "
				"compact an over-full conversation]",
				i - first)));
		for (; i < count; i++)
			trimmed = fy_append(gb, trimmed, fy_get_at(out, i));
		out = trimmed;
	}

	out = fy_gb_internalize(gb, out);
	return fy_is_invalid(out) ? messages : out;
}

/* Return the latest measured input-token count, and where it came from. */
static long long session_last_usage(struct fyai_ctx *ctx,
				    enum fyai_context_source *srcp)
{
	enum fyai_context_source source;
	fy_generic cur, usage;
	long long input;

	source = FYAICS_NONE;
	input = 0;
	if (ctx->last_call_input) {
		source = FYAICS_LAST_CALL;
		input = ctx->last_call_input;
		goto out;
	}
	fyai_turn_foreach(cur, ctx->last_message) {
		usage = fy_get(fyai_turn_meta(cur), "usage");
		input = fy_get(usage, "input", 0LL);
		if (input) {
			source = FYAICS_STORED;
			goto out;
		}
	}
	input = 0;
out:
	if (srcp)
		*srcp = source;
	return input;
}

const char *fyai_context_source_name(enum fyai_context_source source)
{
	switch (source) {
	case FYAICS_LAST_CALL:
		return "last call";
	case FYAICS_STORED:
		return "stored";
	default:
		return "none";
	}
}

/* Add the output allowance to the best available prompt size. */
static long long session_projected_tokens(struct fyai_ctx *ctx,
					  const struct fyai_context_prompt *p)
{
	return p->prompt + fyai_context_output_tokens(ctx, p->prompt,
						      fyai_context_window(ctx));
}

long long fyai_context_output_tokens(struct fyai_ctx *ctx, long long prompt,
				     long long window)
{
	long long room, max;

	max = ctx->cfg->max_tokens > 0 ? ctx->cfg->max_tokens : 0;
	if (window <= 0 || max <= 0)
		return max;

	/* Keep the configured allowance when the prompt does not fit. */
	room = window - prompt;
	if (room > 0 && max > room)
		return room;
	return max;
}

void fyai_context_prompt_at(struct fyai_ctx *ctx, fy_generic head,
			    struct fyai_context_prompt *out)
{
	out->measured = session_last_usage(ctx, &out->source);
	out->estimated = session_estimate_tokens_at(head);
	/* Prefer a measured value on ties; the estimate overshoots it. */
	out->from_estimate = out->source == FYAICS_NONE ||
			     out->estimated > out->measured;
	out->prompt = out->from_estimate ? out->estimated : out->measured;
}

long long fyai_context_projected_at(struct fyai_ctx *ctx, fy_generic head)
{
	struct fyai_context_prompt p;

	fyai_context_prompt_at(ctx, head, &p);
	return session_projected_tokens(ctx, &p);
}

/* Render the measured and estimated prompt sizes. */
static fy_generic session_prompt_text(struct fyai_ctx *ctx,
				      const struct fyai_context_prompt *p)
{
	fy_generic text;

	if (p->source == FYAICS_NONE)
		text = fy_stringf("~%lld tokens (estimate; none measured yet)",
				  p->estimated);
	else if (p->from_estimate)
		text = fy_stringf("~%lld tokens (estimate; %lld measured %s)",
				  p->estimated, p->measured,
				  fyai_context_source_name(p->source));
	else
		text = fy_stringf("%lld tokens (measured %s; ~%lld estimated)",
				  p->measured,
				  fyai_context_source_name(p->source), p->estimated);
	return fy_gb_internalize(ctx->transient_gb, text);
}

long long fyai_context_projected(struct fyai_ctx *ctx)
{
	return fyai_context_projected_at(ctx, ctx->last_message);
}

/* The output allowance; the window, not the configuration, can set it. */
static fy_generic session_allowance(struct fy_generic_builder *gb,
				    struct fyai_cfg *cfg, long long out_tokens)
{
	return out_tokens < cfg->max_tokens ?
	       fy_stringf(gb, "%lld tokens (reduced from %d to fit)",
			  out_tokens, cfg->max_tokens) :
	       fy_stringf(gb, "%lld tokens", out_tokens);
}

fy_generic fyai_session_status_data(struct fyai_ctx *ctx,
				    struct fy_generic_builder *gb)
{
	struct fyai_cfg *cfg = ctx->cfg;
	fy_generic context, reasoning;
	struct fyai_context_prompt p;
	long long window, shown, out_tokens;

	window = fyai_context_window(ctx);
	fyai_context_prompt_at(ctx, ctx->last_message, &p);
	shown = session_projected_tokens(ctx, &p);
	out_tokens = fyai_context_output_tokens(ctx, p.prompt, window);
	if (cfg->reasoning_effort && *cfg->reasoning_effort)
		reasoning = fy_stringf(gb, "%s%s%s", cfg->reasoning_effort,
			cfg->reasoning_summary && *cfg->reasoning_summary ?
			" / " : "", cfg->reasoning_summary ?
			cfg->reasoning_summary : "");
	else
		reasoning = fy_value(gb, "off");
	if (window)
		context = fy_stringf(gb, "~%lld / %lld (%.1f%%)", shown, window,
				     (double)shown * 100.0 / (double)window);
	else
		context = fy_stringf(gb, "~%lld / unknown", shown);
	return fy_mapping(gb,
		"model", cfg->model ? cfg->model : "",
		"provider", cfg->provider ? cfg->provider : "?",
		"api", fyai_api_to_string(cfg->api_mode),
		"endpoint", cfg->api_url ? cfg->api_url : "(derived)",
		"reasoning", reasoning,
		"temperature", cfg->temperature,
		"context", context,
		"prompt", fy_gb_internalize(gb, session_prompt_text(ctx, &p)),
		"output_allowance", session_allowance(gb, cfg, out_tokens),
		"auth", fyai_auth_status_data(ctx, gb, false),
		"usage", fyai_stats_data(ctx, gb));
}

fy_generic fyai_session_context_data(struct fyai_ctx *ctx,
				     struct fy_generic_builder *gb)
{
	struct fyai_cfg *cfg = ctx->cfg;
	fy_generic context, estimated, measured;
	struct fyai_context_prompt p;
	long long window, projected, out_tokens;

	window = fyai_context_window(ctx);
	fyai_context_prompt_at(ctx, ctx->last_message, &p);
	projected = session_projected_tokens(ctx, &p);
	out_tokens = fyai_context_output_tokens(ctx, p.prompt, window);

	/* Report both token sources and mark the source used. */
	estimated = fy_stringf(gb, "~%lld tokens%s", p.estimated,
			       p.from_estimate ? "  <- used" : "");
	measured = p.source == FYAICS_NONE ?
		fy_value(gb, "none yet") :
		fy_stringf(gb, "%lld tokens, %s%s", p.measured,
			   fyai_context_source_name(p.source),
			   p.from_estimate ? "" : "  <- used");
	if (window)
		context = fy_stringf(gb, "~%lld / %lld (%.1f%%)", projected,
			window, (double)projected * 100.0 / (double)window);
	else
		context = fy_value(gb, "unknown");
	return fy_mapping(gb,
		"model", cfg->model ? cfg->model : "",
		"provider", cfg->provider ? cfg->provider : "?",
		"api", fyai_api_to_string(cfg->api_mode),
		"context", context,
		"prompt_estimated", estimated,
		"prompt_measured", measured,
		"output_max", session_allowance(gb, cfg, out_tokens));
}

char *fyai_prompt_literal(const char *text)
{
	struct response_buffer out = {};
	const unsigned char *p;
	int rc;

	for (p = (const unsigned char *)(text ? text : ""); *p; p++) {
		if (*p < 32 || *p == 127)
			continue;
		if (*p < 128 && ispunct(*p)) {
			rc = response_buffer_append_data(&out, "\\", 1);
			if (rc)
				goto fail;
		}
		rc = response_buffer_append_data(&out, (const char *)p, 1);
		if (rc)
			goto fail;
	}
	return out.data ? out.data : strdup("");
fail:
	free(out.data);
	return NULL;
}

/* Escaped braces are literal; unknown variables expand to an empty string. */
char *fyai_prompt_expand(const char *tmpl, const struct fyai_tmpl_var *vars,
			size_t nvars)
{
	struct response_buffer out = {};
	const char *p;
	const char *e;
	const char *val;
	size_t klen;
	size_t i;
	int rc;

	for (p = tmpl ? tmpl : ""; *p; ) {
		if ((p[0] == '{' && p[1] == '{') ||
		    (p[0] == '}' && p[1] == '}')) {
			rc = response_buffer_append_data(&out, p, 1);
			if (rc)
				goto fail;
			p += 2;
			continue;
		}
		if (*p == '{' && (e = strchr(p, '}')) != NULL) {
			klen = (size_t)(e - p - 1);
			val = "";
			for (i = 0; i < nvars; i++)
				if (!strncmp(vars[i].key, p + 1, klen) &&
				    vars[i].key[klen] == '\0') {
					val = vars[i].val ? vars[i].val : "";
					break;
				}
			rc = response_buffer_append_data(&out, val, strlen(val));
			if (rc)
				goto fail;
			p = e + 1;
			continue;
		}
		rc = response_buffer_append_data(&out, p++, 1);
		if (rc)
			goto fail;
	}
	return out.data ? out.data : strdup("");
fail:
	free(out.data);
	return NULL;
}

/*
 * prompt_top/prompt_bottom are configured as markdown, but linenoise exposes
 * them as a single status row each. Render through libfymd4c, then fold any
 * markdown block layout (newlines) into spaces so tables/lists do not corrupt
 * the prompt block accounting. The returned string is heap-owned by the caller;
 * NULL means "use the unrendered template expansion".
 */
static char *fyai_prompt_row_markdown(struct fyai_cfg *cfg, const char *text)
{
	struct response_buffer out = {0};
	const char *start;
	const char *end;
	char *row;
	size_t len;
	size_t i;
	int rc;

	if (!text || !*text)
		return strdup("");

	rc = markdown_render(cfg, text, strlen(text), &out,
			     markdown_color_enabled(cfg->color),
			     cfg->theme_variant);
	if (rc || !out.data)
		goto err;

	start = out.data;
	end = out.data + out.len;
	while (start < end && (*start == '\n' || *start == '\r'))
		start++;
	while (end > start && (end[-1] == '\n' || end[-1] == '\r'))
		end--;

	len = (size_t)(end - start);
	row = malloc(len + 1);
	if (!row)
		goto err;
	memcpy(row, start, len);
	row[len] = '\0';
	for (i = 0; i < len; i++)
		if (row[i] == '\n' || row[i] == '\r')
			row[i] = ' ';
	free(out.data);
	return row;

err:
	free(out.data);
	return NULL;
}

/*
 * The REPL prompt bubble decorations: a top row (display/prompt_top), and a
 * bottom status row (display/prompt_bottom) that replaces the built-in banner.
 * Both are {key} templates over the session variables built below (the default
 * bottom template reproduces the classic "model · provider · api · ..." banner),
 * then rendered as markdown into linenoise's top/bottom info rows. Linenoise
 * rows are single-line, so block markdown is folded to one row after rendering.
 */
/*
 * @text in the colour of series @n of the palette theme, as the series of a
 * diagram cycle. @escape escapes @text as Markdown first; pass false for text
 * that is escaped already. Without a palette the text has no colour. Returns
 * a string the caller owns, or NULL.
 */
static char *session_series_text(struct fyai_cfg *cfg, unsigned int n,
				 const char *text, bool escape)
{
	char role[32];
	const char *on, *off;
	char *lit, *out;

	lit = escape ? fyai_prompt_literal(text ? text : "") :
		       strdup(text ? text : "");
	if (!lit || !*lit)
		return lit;
	snprintf(role, sizeof(role), "mermaid.series.%u", n % 8);
	on = markdown_role_on(cfg, role, "");
	off = markdown_role_off(cfg, role, "");
	if (!*on)
		return lit;
	if (asprintf(&out, "%s%s%s", on, lit, off) < 0)
		out = NULL;
	free(lit);
	return out;
}

/* Abbreviate a token count so that the status row stays short. */
/*
 * @path under @home as "~/...", else NULL. A home of "/" abbreviates nothing.
 * The caller owns the result.
 */
static char *session_home_relative(const char *path, const char *home)
{
	size_t n;
	char *out;

	if (!path || !home || *home != '/')
		return NULL;
	n = strlen(home);
	while (n > 1 && home[n - 1] == '/')
		n--;
	if (n <= 1 || strncmp(path, home, n) || (path[n] && path[n] != '/'))
		return NULL;
	if (asprintf(&out, "~%s", path + n) < 0)
		return NULL;
	return out;
}

static void session_token_count(char *buf, size_t size, long long tokens)
{
	if (tokens >= 1000000)
		snprintf(buf, size, "%.1fM", (double)tokens / 1000000.0);
	else if (tokens >= 1000)
		snprintf(buf, size, "%.1fk", (double)tokens / 1000.0);
	else
		snprintf(buf, size, "%lld", tokens);
}

void fyai_session_banner_update(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg = ctx->cfg;
	struct fyai_tmpl_var vars[14], top_vars[14];
	char *coloured[14];
	const struct fyai_tmpl_var *header_vars;
	bool colour_ok;
	fy_generic model_entry;
	char effort[64], summary[64], temp[32], ctxpct[32];
	char tokens[64], cost[32], cache[64];
	/* Wide enough for any long long, so the abbreviation never truncates. */
	char used_str[24], window_str[24], cached_str[24];
	long long used;
	char *top, *bottom, *cwd, *directory, *branch, *location;
	char *tilde, *home_real, *cwd_real;
	const char *home, *pwd;
	struct stat pwd_st, cwd_st;
	size_t i;
	struct fyai_context_prompt prompt;
	char *top_md;
	const char *tmpl;
	long long window;

	if (!cfg->interactive || !cfg->markdown || !ctx->stdout_tty)
		return;
	/* A delegated sub-agent has no header: the head of its tile in the
	 * parent names it. */
	if (fyai_agent_delegated(ctx))
		return;
	cwd = getcwd(NULL, 0);
	/*
	 * The shell names a directory that it reached through a symbolic link
	 * as the user typed it. Use that name when it is the same directory, so
	 * that a home directory behind a link still reads as ~.
	 */
	pwd = getenv("PWD");
	if (cwd && pwd && *pwd == '/' && strcmp(pwd, cwd) &&
	    !stat(pwd, &pwd_st) && !stat(cwd, &cwd_st) &&
	    pwd_st.st_dev == cwd_st.st_dev && pwd_st.st_ino == cwd_st.st_ino) {
		free(cwd);
		cwd = strdup(pwd);
	}
	home = getenv("HOME");
	tilde = session_home_relative(cwd, home);
	if (!tilde) {
		/*
		 * $HOME can name the home directory through a symbolic link
		 * while getcwd() answers with the canonical path. Compare the
		 * canonical form of both, so the two name the same directory.
		 */
		home_real = home ? realpath(home, NULL) : NULL;
		cwd_real = cwd ? realpath(cwd, NULL) : NULL;
		tilde = session_home_relative(cwd_real ? cwd_real : cwd,
					      home_real ? home_real : home);
		free(home_real);
		free(cwd_real);
	}
	if (tilde) {
		free(cwd);
		cwd = tilde;
	}
	directory = fyai_prompt_literal(cwd ? cwd : "?");
	branch = fyai_prompt_literal(fyai_agents_attached(ctx) ?
		fyai_agents_attached(ctx) : fyai_ctx_branch(ctx));
	free(cwd);
	location = NULL;
	if (!directory || !branch ||
	    asprintf(&location, "%s: %s · %s", fyai_agents_attached(ctx) ?
		     "attached" : "fyai", branch, directory) < 0) {
		fyai_warning(ctx, "cannot build the prompt location");
		free(directory);
		free(branch);
		return;
	}
	model_entry = session_model_entry(ctx);

	/* Each optional field carries its own " · label" so a template can place
	 * it unconditionally; empty when the field does not apply. */
	effort[0] = summary[0] = temp[0] = ctxpct[0] = '\0';
	tokens[0] = cost[0] = cache[0] = '\0';
	if (cfg->reasoning_effort && *cfg->reasoning_effort)
		snprintf(effort, sizeof(effort), " · effort %s",
			 cfg->reasoning_effort);
	if (cfg->reasoning_summary && *cfg->reasoning_summary)
		snprintf(summary, sizeof(summary), " · summary %s",
			 cfg->reasoning_summary);
	if (fyai_model_supports_temperature(model_entry) &&
	    (!cfg->reasoning_effort || !*cfg->reasoning_effort) &&
	    (!cfg->reasoning_summary || !*cfg->reasoning_summary))
		snprintf(temp, sizeof(temp), " · temp %g", (double)cfg->temperature);

	window = fyai_context_window(ctx);
	if (window > 0) {
		fyai_context_prompt_at(ctx, ctx->last_message, &prompt);
		/* Streamed bytes are already in the estimate; do not add them twice. */
		used = session_projected_tokens(ctx, &prompt);
		snprintf(ctxpct, sizeof(ctxpct), " · ctx ~%.0f%%",
			 (double)used * 100.0 / (double)window);
		session_token_count(used_str, sizeof(used_str), used);
		session_token_count(window_str, sizeof(window_str), window);
		snprintf(tokens, sizeof(tokens), " · ctx %s/%s (%.0f%%)",
			 used_str, window_str,
			 (double)used * 100.0 / (double)window);
	}
	if (ctx->usage_cost > 0.0)
		snprintf(cost, sizeof(cost), " · $%.4f", ctx->usage_cost);
	if (ctx->usage_input > 0) {
		session_token_count(cached_str, sizeof(cached_str),
				    ctx->usage_cached);
		snprintf(cache, sizeof(cache), " · cache %s (%.0f%%)",
			 cached_str, (double)ctx->usage_cached * 100.0 /
			 (double)ctx->usage_input);
	}

	vars[0].key = "model";
	vars[0].val = fyai_agents_model(ctx) ? fyai_agents_model(ctx) :
		cfg->model ? cfg->model : "?";
	vars[1].key = "provider";
	vars[1].val = cfg->provider ? cfg->provider : "?";
	vars[2].key = "api";
	vars[2].val = fyai_api_to_string(cfg->api_mode);
	vars[3].key = "effort";
	vars[3].val = effort;
	vars[4].key = "summary";
	vars[4].val = summary;
	vars[5].key = "temp";
	vars[5].val = temp;
	vars[6].key = "ctx";
	vars[6].val = ctxpct;
	vars[7].key = "tokens";
	vars[7].val = tokens;
	vars[8].key = "cost";
	vars[8].val = cost;
	vars[9].key = "cache";
	vars[9].val = cache;
	vars[10] = (struct fyai_tmpl_var){ "branch", branch };
	vars[11] = (struct fyai_tmpl_var){ "cwd", directory };
	vars[12] = (struct fyai_tmpl_var){ "location", location };
	vars[13] = (struct fyai_tmpl_var){ "layout", fyai_ui_page_layout(ctx) };
	if (fyai_agents_attached(ctx)) {
		vars[1].val = "agent";
		vars[2].val = fyai_agents_state(ctx, fyai_agents_attached(ctx));
		for (i = 3; i < 10; i++)
			vars[i].val = "";
	}

	/*
	 * Each value of the header and of the status takes a colour of the
	 * palette series in turn: the branch and the directory first, so the two
	 * parts of the location differ. The values are escaped as Markdown, so
	 * the same source is the Markdown of the band stack and the UI Markdown
	 * of the page.
	 */
	memset(coloured, 0, sizeof(coloured));
	colour_ok = true;
	for (i = 0; i < 12 && colour_ok; i++) {
		coloured[i] = session_series_text(cfg, i == 10 ? 0 :
						  i == 11 ? 1 : (unsigned int)i + 2,
						  vars[i].val, i < 10);
		colour_ok = coloured[i] != NULL;
		top_vars[i].key = vars[i].key;
		top_vars[i].val = coloured[i];
	}
	if (colour_ok)
		colour_ok = asprintf(&coloured[12], "%s: %s · %s",
				     fyai_agents_attached(ctx) ?
				     "attached" : "fyai",
				     coloured[10], coloured[11]) >= 0;
	if (!colour_ok) {
		coloured[12] = NULL;
		fyai_warning(ctx, "cannot colour the prompt header");
	}
	top_vars[12] = (struct fyai_tmpl_var){ "location", coloured[12] };
	if (colour_ok) {
		coloured[13] = session_series_text(cfg, 12, vars[13].val, true);
		colour_ok = coloured[13] != NULL;
		if (!colour_ok)
			fyai_warning(ctx, "cannot colour the layout of the "
				     "prompt header");
	}
	top_vars[13] = (struct fyai_tmpl_var){ "layout", coloured[13] };
	header_vars = colour_ok ? top_vars : vars;

	tmpl = cfg->prompt_bottom && *cfg->prompt_bottom ?
		cfg->prompt_bottom : DEFAULT_PROMPT_BOTTOM;
	bottom = fyai_prompt_expand(tmpl, header_vars,
				    sizeof(vars) / sizeof(vars[0]));
	top = fyai_prompt_expand(fy_str_empty(cfg->prompt_top) ?
				DEFAULT_PROMPT_TOP : cfg->prompt_top, header_vars,
				sizeof(vars) / sizeof(vars[0]));
	top_md = fyai_prompt_row_markdown(cfg, top);
	if (fyai_ui_active(ctx))
		fyai_ui_update_banner(ctx, top_md ? top_md : top, top,
				      bottom);
	free(top_md);
	free(top);
	free(bottom);
	for (i = 0; i < 14; i++)
		free(coloured[i]);
	free(location);
	free(directory);
	free(branch);
}

/* ---- slash dispatch ------------------------------------------------------ */

/*
 * Switch branch inside a live session. The conversation and the configuration
 * both belong to the branch, so this re-points the head *and* re-applies the
 * new branch's config, re-resolving the model, api and key the way /model
 * does. A failure leaves the session on the branch it was on.
 */
int fyai_session_branch_switch(struct fyai_ctx *ctx, const char *name,
				 bool create, bool keep_head)
{
	struct fyai_branch b;
	const char *test_fail;
	char *old;
	size_t old_len;
	bool found;
	int rc;

	if (fyai_ui_busy(ctx) || fyai_tools_active(ctx) || fyai_agents_attached(ctx)) {
		fyai_error(ctx, "branch changes require idle model and tool work");
		return -1;
	}
	old_len = strlen(fyai_ctx_branch(ctx));
	old = alloca(old_len + 1);
	memcpy(old, fyai_ctx_branch(ctx), old_len + 1);
	found = fyai_branch_lookup(ctx->arena_branches, name, &b);
	fyai_error_check(ctx, found || create, err_out,
			 "branch: no such branch '%s'", name);
	if (!found) {
		rc = fyai_branch_create(ctx, name, NULL, NULL, false);
		fyai_error_check(ctx, !rc, err_out,
				 "branch: could not create '%s'", name);
	}
	rc = fyai_branch_adopt(ctx, name, keep_head);
	fyai_error_check(ctx, !rc, rollback,
			 "branch: could not stage '%s'", name);
	/* Test hook for the rollback path. */
	test_fail = getenv("FYAI_TEST_BRANCH_SWITCH_FAIL");
	fyai_error_check(ctx, !test_fail || strcmp(test_fail, name), rollback,
			 "branch: injected switch failure for '%s'", name);

	/* Rebuild the derived cache from the branch configuration. */
	rc = fyai_config_rederive(ctx);
	fyai_error_check(ctx, !rc, rollback,
			 "branch: could not apply '%s' configuration", name);
	rc = fyai_auth_resolve(ctx);
	fyai_error_check(ctx, !rc, rollback,
			 "branch: could not resolve '%s' authentication", name);
	rc = ctx->curl ? fyai_request_state_apply(ctx) : 0;
	fyai_error_check(ctx, !rc, rollback,
			 "branch: could not apply '%s' request state", name);

	/* Selecting a session for this invocation changes nothing durable, so
	 * there is nothing to publish and nothing to lose if it is left. */
	if (!keep_head) {
		fyai_branch_op_set(ctx, FYAI_BRANCH_OP_CHECKOUT, NULL);
		rc = fyai_publish_state(ctx);
		fyai_error_check(ctx, !rc, rollback,
				 "branch: could not publish checkout of '%s'",
				 name);
	}

	fyai_session_banner_update(ctx);
	fyai_ui_repaint(ctx);
	return 0;

rollback:
	/* Restore the branch and its derived request state. */
	rc = fyai_branch_adopt(ctx, old, keep_head);
	if (!rc)
		rc = fyai_config_rederive(ctx);
	if (!rc)
		rc = fyai_auth_resolve(ctx);
	if (!rc && ctx->curl)
		rc = fyai_request_state_apply(ctx);
	fyai_session_banner_update(ctx);
	return -1;

err_out:
	return -1;
}

/* /diff [from [to]]: the exports of two ref-log entries, compared. */
struct fyai_btw_run {
	struct fyai_btw_run *next;
	struct fyai_ctx *ctx;
	struct fyai_tool_job_group *group;
	char *branch;
};

static void session_btw_finish(void *userdata)
{
	struct fyai_btw_run *run = userdata;
	struct fyai_btw_run **link = &run->ctx->btw_runs;
	struct fy_generic_builder *gb;
	fy_generic result = fy_invalid;
	bool ok = false;

	gb = fyai_ctx_transient_gb(run->ctx);
	fyai_error_check(run->ctx, gb, release,
			 "btw: could not collect the side answer");
	if (fyai_tool_job_group_collect(run->group, 0, &result, &ok) || !ok) {
		if (fy_is_string(result))
			fyai_warning(run->ctx, "btw: %s", fy_castp(&result, "failed"));
		else
			fyai_warning(run->ctx, "btw: side question failed");
	}
	(void)fyai_tools_zoom(run->ctx, run->branch);
release:
	fyai_ui_diag_drain(run->ctx, "btw");
	while (*link && *link != run)
		link = &(*link)->next;
	if (*link)
		*link = run->next;
	fyai_tool_job_group_destroy(run->group);
	free(run->branch);
	free(run);
}

static void session_btw_complete(struct fyai_tool_job_group *group,
				 void *userdata)
{
	struct fyai_btw_run *run = userdata;

	(void)group;
	(void)fyai_event_defer(fyai_ctx_loop(run->ctx), session_btw_finish, run);
}

void fyai_session_btw_close(struct fyai_ctx *ctx)
{
	struct fyai_btw_run *run, *next;

	for (run = ctx->btw_runs; run; run = next) {
		next = run->next;
		fyai_event_defer_cancel(fyai_ctx_loop(ctx), session_btw_finish,
					 run);
		fyai_tool_job_group_destroy(run->group);
		free(run->branch);
		free(run);
	}
	ctx->btw_runs = NULL;
	fyai_tools_btw_panels_close(ctx);
}

/* Reserve a name absent from stored branches and running side questions. */
static int session_btw_branch_name(struct fyai_ctx *ctx, char *name,
				   size_t name_size, char *branch,
				   size_t branch_size)
{
	struct fyai_btw_run *live;
	unsigned int serial;
	bool taken;
	int rc;

	rc = fyai_branches_refresh(ctx);
	fyai_error_check(ctx, !rc, err,
			 "btw: could not refresh branches");
	for (serial = 1; serial < 1000000; serial++) {
		snprintf(name, name_size, "btw-%u", serial);
		rc = fyai_branch_alloc_child(ctx, fyai_ctx_branch(ctx), name,
			(unsigned int)ctx->cfg->agent_max_branch_depth,
			branch, branch_size, &taken);
		fyai_error_check(ctx, !rc, err,
				 "btw: could not allocate a side branch");
		for (live = ctx->btw_runs; live; live = live->next)
			if (!strcmp(live->branch, branch))
				break;
		if (!taken && !live)
			return 0;
	}
	fyai_error(ctx, "btw: no side branch name is available");
err:
	return -1;
}

int fyai_session_btw(struct fyai_ctx *ctx, const char *arg)
{
	struct fyai_btw_run *run;
	fy_generic args, call;
	const char *json;
	char name[32], branch[FYAI_BRANCH_NAME_MAX + 1];
	int rc;

	fyai_error_check(ctx, arg && *arg, err,
			 "btw: give a question");
	rc = fyai_setup_transient_builder(ctx);
	fyai_error_check(ctx, !rc, err,
			 "btw: could not create side question storage");
	rc = session_btw_branch_name(ctx, name, sizeof(name),
				     branch, sizeof(branch));
	fyai_error_check(ctx, !rc, err,
			 "btw: could not name the side branch");
	args = fy_mapping(ctx->transient_gb,
		"task", fy_value(ctx->transient_gb, arg),
		"name", fy_value(ctx->transient_gb, name),
		"description", "side question",
		"context", "fork",
		"_fyai_btw", true);
	json = emit_json_string(ctx->transient_gb, args);
	fyai_error_check(ctx, json, err,
			 "btw: could not encode the question");
	if (ctx->cfg->api_mode == FYAI_API_CHAT_COMPLETIONS)
		call = fy_mapping(ctx->transient_gb, "type", "function",
			"function", fy_mapping(ctx->transient_gb,
				"name", "agent", "arguments", json));
	else
		call = fy_mapping(ctx->transient_gb, "type", "function_call",
			"name", "agent", "arguments", json);
	run = calloc(1, sizeof(*run));
	fyai_error_check(ctx, run, err,
			 "btw: could not allocate the side question");
	run->ctx = ctx;
	run->branch = strdup(branch);
	fyai_error_check(ctx, run->branch, err_run,
			 "btw: could not retain the branch name");
	run->group = fyai_tool_job_group_create_notify(ctx,
			session_btw_complete, run);
	fyai_error_check(ctx, run->group, err_run,
			 "btw: could not create the side question group");
	rc = fyai_tool_job_group_add(run->group, call);
	fyai_error_check(ctx, !rc, err_group,
			 "btw: could not add the side question");
	rc = fyai_tool_job_group_submit(run->group);
	fyai_error_check(ctx, !rc, err_group,
			 "btw: could not start the side question");
	run->next = ctx->btw_runs;
	ctx->btw_runs = run;
	fyai_result(ctx, "btw: %s", branch);
	return 0;
err_group:
	fyai_tool_job_group_destroy(run->group);
err_run:
	free(run->branch);
	free(run);
err:
	return -1;
}

int fyai_session_slash(struct fyai_ctx *ctx, const char *line)
{
	const char *name;
	char title[64];
	size_t len;
	bool own_transient;
	bool error;
	bool pane_output;
	bool view;
	int rc;

	name = line + 1;	/* skip '/' */
	len = strcspn(name, " \t");
	if (!len) {
		fyai_error(ctx, "unknown command '%s' (try /help)", line);
		goto out_report;
	}
	/* The registry takes an exact name, else a unique prefix. */
	if (!fyai_cmd_session_exact(name, len) &&
	    fyai_cmd_session_prefix(name, len) != 1) {
		fyai_error(ctx, "unknown or ambiguous command '%.*s' (try /help)",
			   (int)(len + 1), line);
		goto out_report;
	}
	/* /exit and /quit end the session; they present nothing. */
	if (fyai_cmd_session_ends(name))
		return 1;

	snprintf(title, sizeof(title), "%.*s", (int)len, name);
	fyai_ui_pane_begin(ctx);

	/* Backends build turns/generics; give them a transient builder. */
	own_transient = !ctx->transient_gb;
	if (own_transient && fyai_setup_transient_builder(ctx)) {
		fyai_ui_pane_end(ctx, title, true, true);
		return 0;
	}

	view = false;
	rc = fyai_cmd_session_run(ctx, name, &view);
	if (own_transient)
		fyai_cleanup_transient_builder(ctx);
	if (!rc && ctx->cfg->reload_branch) {
		fyai_diag_drain(&ctx->cfg->diag);
		fyai_ui_pane_end(ctx, title, false, true);
		return 1;
	}

	/*
	 * A backend collects rather than prints, so report here - before the
	 * banner repaints the footer - or the command would fail silently.
	 */
	error = rc || fyai_diag_got_error(&ctx->cfg->diag);
	fyai_diag_drain(&ctx->cfg->diag);
	/*
	 * A view is a record the user reads and scrolls back to, not a
	 * transient status: commit it to the scrollback instead of a pane.
	 */
	pane_output = !view;
	fyai_ui_pane_end(ctx, title, error, pane_output);

	/* Settings/model/context may have changed; reflect it in the footer. */
	fyai_session_banner_update(ctx);
	return 0;

out_report:
	/*
	 * Nothing ran, so the footer is still good; report and go straight back
	 * to the prompt rather than leaving the complaint until the next drain.
	 */
	fyai_ui_pane_begin(ctx);
	fyai_diag_drain(&ctx->cfg->diag);
	fyai_ui_pane_end(ctx, "error", true, true);
	return 0;
}

/*
 * A slash line typed while a model turn is in flight runs now only when it
 * cannot disturb that turn. Anything else stays queued behind it: the line
 * waits in the input queue and the prompt runs it once the turn is done.
 * Reads of stored state and the work-pane controls are immediate; a mutation
 * of the session, the configuration, or live work waits. A bare setting
 * prints its value (immediate); a value changes it (waits). Return true
 * when @line may run while @busy.
 */
bool fyai_session_slash_immediate(struct fyai_ctx *ctx, const char *line,
				  bool busy)
{
	if (!busy)
		return true;
	if (!ctx || !line || line[0] != '/' || line[1] == '/')
		return false;
	return fyai_cmd_session_immediate(line + 1);
}

/* ---- tab completion ------------------------------------------------------ */

char *fyai_readline(struct fyai_ctx *ctx, const char *prompt)
{
	char *line = NULL;
	size_t cap = 0;
	ssize_t len;

	assert(ctx);
	if (fyai_ui_active(ctx))
		return fyai_ui_readline(ctx);
	if (isatty(STDIN_FILENO) && prompt) {
		fputs(prompt, stderr);
		fflush(stderr);
	}
	len = getline(&line, &cap, stdin);
	if (len < 0) { free(line); return NULL; }
	while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
		line[--len] = '\0';
	return line;
}

struct session_complete {
	struct fytim_completions *lc;
	size_t word;
};

/* The popup shows the word a candidate puts in the line, and its title. */
static void session_complete_add(void *arg, const char *value,
				 const char *description)
{
	struct session_complete *sc = arg;
	const char *word = value + sc->word;
	size_t len = strlen(word);

	while (len && word[len - 1] == ' ')
		len--;
	(void)fytim_completion_add_item(sc->lc, value,
					fy_sprintfa("%.*s", (int)len, word),
					description);
}

void fyai_session_completion(struct fyai_ctx *ctx, const char *buf,
				     struct fytim_completions *lc)
{
	struct session_complete sc;

	if (!ctx || buf[0] != '/')
		return;
	sc.lc = lc;
	sc.word = fyai_cmd_session_word(buf);
	(void)fytim_completion_set_anchor(lc, sc.word);
	fyai_cmd_session_complete(ctx, buf, session_complete_add, &sc);
}
