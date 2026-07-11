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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "commands.h"
#include "fyai.h"
#include "fyai_catalog.h"
#include "fyai_config.h"
#include "fyai_display.h"
#include "fyai_log.h"
#include "fyai_markdown.h"
#include "fyai_session.h"
#include "fyai_storage.h"
#include "fyai_terminal.h"
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

static long long session_context_window(struct fyai_ctx *ctx)
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
	printf("conversation cleared\n");
	return 0;
}

int fyai_session_compact(struct fyai_ctx *ctx, const char *hint)
{
	struct fyai_cfg *cfg = ctx->cfg;
	fy_generic prev_head, turn, v, msgs, m, meta, content;
	const char *summary;
	bool tools_save, shell_save;
	size_t n;

	if (!cfg->api_key || !*cfg->api_key) {
		fprintf(stderr, "compact: no API key\n");
		return -1;
	}
	if (fy_generic_is_invalid(ctx->last_message) ||
	    fy_generic_is_null_type(ctx->last_message)) {
		printf("compact: nothing to compact\n");
		return 0;
	}
	assert(ctx->transient_gb);

	prev_head = ctx->last_message;
	turn = fyai_turn_append(ctx, prev_head,
		fy_sequence(fy_mapping(ctx->gb,
			"role", "user",
			"content", fy_stringf(
				"Summarize this conversation for continuation: decisions "
				"made, current state, and open items. Be complete but "
				"concise; the summary will replace the conversation "
				"history.%s%s",
				hint && *hint ? " Focus on: " : "",
				hint ? hint : ""))));
	if (fy_generic_is_invalid(turn))
		return -1;

	/* One-off summary call: no tools. */
	tools_save = cfg->enable_tools;
	shell_save = cfg->enable_builtin_shell;
	cfg->enable_tools = false;
	cfg->enable_builtin_shell = false;
	v = fyai_run_model_loop(ctx, turn);
	cfg->enable_tools = tools_save;
	cfg->enable_builtin_shell = shell_save;
	if (fy_generic_is_invalid(v) || fy_generic_is_null_type(v)) {
		fprintf(stderr, "compact: summary request failed\n");
		return -1;
	}

	/* The final assistant message of the final turn is the summary. */
	msgs = fy_get(v, "messages", fy_seq_empty);
	n = fy_len(msgs);
	m = n ? fy_get_at(msgs, n - 1) : fy_invalid;
	content = fy_get(m, "content");
	summary = fy_castp(&content, "");
	if (!*summary) {
		fprintf(stderr, "compact: empty summary\n");
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
	if (fy_generic_is_invalid(turn))
		return -1;

	meta = fyai_turn_meta(turn);
	if (fy_generic_is_invalid(meta) || fy_generic_is_null_type(meta))
		meta = fy_map_empty;
	meta = fy_assoc(meta, "compacted_from", prev_head);
	turn = fy_assoc(turn, "metadata", meta);

	turn = fy_gb_internalize(ctx->gb, turn);
	if (fy_generic_is_invalid(turn))
		return -1;
	ctx->last_message = turn;
	ctx->last_call_input = 0;
	ctx->last_call_output = 0;
	ctx->last_call_total = 0;

	if (fyai_publish_state(ctx))
		return -1;
	printf("conversation compacted\n");
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
		fprintf(stderr, "%s: warning: could not persist to config\n",
			key);
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
	      fy_generic_is_valid(fyai_catalog_provider(catalog,
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

int fyai_session_model(struct fyai_ctx *ctx, const char *name)
{
	struct fyai_cfg *cfg = ctx->cfg;
	struct fyai_cfg tmp;
	long long window;

	if (!name || !*name) {
		window = session_context_window(ctx);
		printf("model: %s (provider %s, api %s, window ",
		       cfg->model ? cfg->model : "",
		       cfg->provider ? cfg->provider : "?",
		       fyai_api_to_string(cfg->api_mode));
		if (window)
			printf("%lld)\n", window);
		else
			printf("unknown)\n");
		return 0;
	}

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
	if ((!tmp.api_key || !*tmp.api_key) &&
	    !session_chatgpt_capable(cfg, &tmp)) {
		fprintf(stderr,
			"model: no API key for provider '%s' (set %s%s)\n",
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
	if (fyai_auth_resolve(ctx))
		return -1;

	/* Rebuild the derived request state when a live session exists. */
	if (ctx->curl && fyai_request_state_apply(ctx))
		return -1;

	session_persist_model(ctx);

	printf("model: %s (provider %s, api %s)\n",
	       cfg->model, cfg->provider ? cfg->provider : "?",
	       fyai_api_to_string(cfg->api_mode));
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

int fyai_session_api(struct fyai_ctx *ctx, const char *arg)
{
	struct fyai_cfg *cfg = ctx->cfg;
	struct fyai_cfg tmp;
	enum fyai_api_mode mode;
	fy_generic catalog;
	fy_generic prov;

	if (!arg || !*arg) {
		printf("api: %s (model %s, provider %s, url %s, max_tokens %d)\n",
		       fyai_api_to_string(cfg->api_mode),
		       cfg->model ? cfg->model : "",
		       cfg->provider ? cfg->provider : "?",
		       cfg->api_url ? cfg->api_url : "?",
		       cfg->max_tokens);
		return 0;
	}

	if (session_api_parse(arg, &mode)) {
		fprintf(stderr, "api: unknown grammar '%s' "
			"(responses|chat-completions|messages)\n", arg);
		return -1;
	}
	if (mode == cfg->api_mode) {
		printf("api: already %s\n", fyai_api_to_string(mode));
		return 0;
	}

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
	if (fy_generic_is_valid(prov))
		tmp.model = fy_gb_intern_string(cfg->gb,
				fy_sprintfa("%s/%s", cfg->provider,
					    cfg->model));
	tmp.provider = NULL;

	if (fyai_config_resolve_model(&tmp))
		return -1;
	/* The resolver falls back to a grammar the provider does offer; an
	 * explicit switch must not silently land somewhere else. */
	if (tmp.api_mode != mode) {
		fprintf(stderr, "api: provider '%s' does not offer %s\n",
			cfg->provider ? cfg->provider : "?",
			fyai_api_to_string(mode));
		return -1;
	}
	if (fyai_config_messages_gate(&tmp))
		return -1;
	if ((!tmp.api_key || !*tmp.api_key) &&
	    !session_chatgpt_capable(cfg, &tmp)) {
		fprintf(stderr,
			"api: no API key for provider '%s' (set %s%s)\n",
			tmp.provider ? tmp.provider : "?",
			tmp.provider ? tmp.provider : "PROVIDER",
			"_API_KEY or use --api-key");
		return -1;
	}

	*cfg = tmp;

	/* Re-derive ChatGPT subscription routing for the new grammar; resolve
	 * returns early and harmlessly when an API key is in use. */
	cfg->chatgpt_auth = false;
	if (fyai_auth_resolve(ctx))
		return -1;

	/* Rebuild the derived request state when a live session exists. */
	if (ctx->curl && fyai_request_state_apply(ctx))
		return -1;

	/*
	 * Persist the grammar, its resolved endpoint and the provider-pinned
	 * model, so the continuation stays on the provider this switch
	 * re-targeted. api_url is persisted explicitly here (rather than via
	 * the model-change catalogue sync, which only fires when the model
	 * itself changes) since an /api switch alone moves the endpoint.
	 */
	session_persist(ctx, "api",
			fy_sprintfa("'%s'", fyai_api_to_string(cfg->api_mode)));
	if (cfg->api_url && *cfg->api_url)
		session_persist(ctx, "api_url",
				fy_sprintfa("'%s'", cfg->api_url));
	session_persist_model(ctx);

	printf("api: %s (model %s, provider %s, url %s)\n",
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

static long long session_estimate_tokens(struct fyai_ctx *ctx)
{
	fy_generic cur, msgs;
	long long bytes, nmsg;

	bytes = 0;
	nmsg = 0;
	fyai_turn_foreach(cur, ctx->last_message) {
		msgs = fy_get(cur, "messages", fy_seq_empty);
		nmsg += (long long)fy_len(msgs);
		bytes += session_est_bytes(msgs);
	}
	if (fy_generic_is_valid(ctx->tools))
		bytes += session_est_bytes(ctx->tools);
	return bytes / 4 + nmsg * 4;
}

/* Last known real context load: this run's last call, else the newest
 * persisted turn carrying a usage record. */
static long long session_last_usage(struct fyai_ctx *ctx, const char **srcp)
{
	fy_generic cur, usage;
	long long total;

	if (ctx->last_call_total) {
		*srcp = "last call";
		return ctx->last_call_total;
	}
	fyai_turn_foreach(cur, ctx->last_message) {
		usage = fy_get(fyai_turn_meta(cur), "usage");
		total = fy_get(usage, "total", 0LL);
		if (total) {
			*srcp = "stored";
			return total;
		}
	}
	*srcp = NULL;
	return 0;
}

int fyai_session_context(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg = ctx->cfg;
	const char *src;
	char md[512];
	long long window, used, est;

	window = session_context_window(ctx);
	used = session_last_usage(ctx, &src);
	est = session_estimate_tokens(ctx);

	if (markdown_available(cfg)) {
		if (!window)
			snprintf(md, sizeof(md),
				 "| Metric | Value |\n"
				 "|---|---|\n"
				 "| Model | %s |\n"
				 "| Provider | %s |\n"
				 "| API | %s |\n"
				 "| Context | unknown |\n"
				 "| Next request | ~%lld tokens |\n",
				 cfg->model ? cfg->model : "",
				 cfg->provider ? cfg->provider : "?",
				 fyai_api_to_string(cfg->api_mode), est);
		else
			snprintf(md, sizeof(md),
				 "| Metric | Value |\n"
				 "|---|---|\n"
				 "| Model | %s |\n"
				 "| Provider | %s |\n"
				 "| API | %s |\n"
				 "| Context | %s%lld / %lld (%.1f%%) |\n"
				 "| Next request | ~%lld tokens |\n",
				 cfg->model ? cfg->model : "",
				 cfg->provider ? cfg->provider : "?",
				 fyai_api_to_string(cfg->api_mode),
				 used ? "" : "~", used ? used : est, window,
				 (double)(used ? used : est) * 100.0 /
				 (double)window, est);
		if (!fyai_print_markdown(md, cfg))
			return 0;
	}

	printf("model: %s (provider %s, api %s)\n",
	       cfg->model ? cfg->model : "",
	       cfg->provider ? cfg->provider : "?",
	       fyai_api_to_string(cfg->api_mode));

	if (!window)
		printf("context: window unknown\n");
	else if (used)
		printf("context: %lld / %lld (%.1f%%) [%s]\n",
		       used, window,
		       (double)used * 100.0 / (double)window, src);
	else
		printf("context: ~%lld / %lld (%.1f%%) [estimate]\n",
		       est, window,
		       (double)est * 100.0 / (double)window);

	printf("next request: ~%lld tokens (estimate)\n", est);

	if (ctx->usage_calls)
		printf("session: calls=%d input=%lld cached=%lld output=%lld "
		       "total=%lld\n",
		       ctx->usage_calls, ctx->usage_input, ctx->usage_cached,
		       ctx->usage_output, ctx->usage_total);
	return 0;
}

/* One {key} -> value binding for the prompt decorator templates. */
struct fyai_tmpl_var {
	const char *key;
	const char *val;
};

/*
 * Expand {key} tokens in @tmpl from @vars into @buf. "{{"/"}}" are literal
 * braces; an unknown {key} expands to empty. Values may carry SGR colour
 * escapes - they are copied verbatim (linenoise treats them as zero width).
 */
static void fyai_expand_template(char *buf, size_t bufsz, const char *tmpl,
				 const struct fyai_tmpl_var *vars, size_t nvars)
{
	const char *p;
	const char *e;
	const char *val;
	size_t off;
	size_t klen;
	size_t i;

	off = 0;
	if (!bufsz)
		return;
	for (p = tmpl; *p && off + 1 < bufsz; ) {
		if ((p[0] == '{' && p[1] == '{') ||
		    (p[0] == '}' && p[1] == '}')) {
			buf[off++] = *p;
			p += 2;
			continue;
		}
		if (*p == '{' && (e = strchr(p, '}')) != NULL) {
			klen = (size_t)(e - p - 1);
			val = "";
			for (i = 0; i < nvars; i++)
				if (!strncmp(vars[i].key, p + 1, klen) &&
				    vars[i].key[klen] == '\0') {
					val = vars[i].val;
					break;
				}
			while (*val && off + 1 < bufsz)
				buf[off++] = *val++;
			p = e + 1;
			continue;
		}
		buf[off++] = *p++;
	}
	buf[off] = '\0';
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
			     markdown_color_enabled(cfg->color), cfg->theme);
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
void fyai_session_banner_update(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg = ctx->cfg;
	struct fyai_tmpl_var vars[7];
	fy_generic model_entry;
	char effort[64], summary[64], temp[32], ctxpct[32];
	char top[256], bottom[256];
	char *top_md;
	char *bottom_md;
	const char *src;
	const char *tmpl;
	long long window, used;

	if (!cfg->interactive || !cfg->markdown || !ctx->stdout_tty)
		return;
	model_entry = session_model_entry(ctx);

	/* Each optional field carries its own " · label" so a template can place
	 * it unconditionally; empty when the field does not apply. */
	effort[0] = summary[0] = temp[0] = ctxpct[0] = '\0';
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

	window = session_context_window(ctx);
	if (window > 0) {
		used = session_last_usage(ctx, &src);
		if (used)
			snprintf(ctxpct, sizeof(ctxpct), " · ctx %.0f%%",
				 (double)used * 100.0 / (double)window);
		else
			snprintf(ctxpct, sizeof(ctxpct), " · ctx ~%.0f%%",
				 (double)session_estimate_tokens(ctx) * 100.0 /
				 (double)window);
	}

	vars[0].key = "model";
	vars[0].val = cfg->model ? cfg->model : "?";
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

	tmpl = cfg->prompt_bottom && *cfg->prompt_bottom ?
		cfg->prompt_bottom : DEFAULT_PROMPT_BOTTOM;
	fyai_expand_template(bottom, sizeof(bottom), tmpl,
			     vars, sizeof(vars) / sizeof(vars[0]));
	bottom_md = fyai_prompt_row_markdown(cfg, bottom);
	linenoiseSetBottomInfo(bottom_md ? bottom_md : bottom);
	free(bottom_md);

	fyai_expand_template(top, sizeof(top),
			     cfg->prompt_top ? cfg->prompt_top : "",
			     vars, sizeof(vars) / sizeof(vars[0]));
	top_md = fyai_prompt_row_markdown(cfg, top);
	linenoiseSetTopInfo(top_md ? top_md : top);
	free(top_md);
}

/* ---- simple config-item slash commands ---------------------------------- */

enum fyai_opt_kind {
	FYAIOK_STR,
	FYAIOK_BOOL,
	FYAIOK_FLOAT,
};

struct fyai_slash_opt {
	const char *name;
	enum fyai_opt_kind kind;
	size_t off;			/* offsetof the field in fyai_cfg */
	const char *const *values;	/* enum for validation/completion */
	bool restyle;			/* reload markdown styling on change */
	bool reasoning;			/* rejected under the Messages API */
	const char *ckey;		/* config path to persist to (or NULL) */
	const char *help;
};

static const char *const effort_vals[] = {
	"minimal", "low", "medium", "high", NULL,
};
static const char *const summary_vals[] = {
	"auto", "concise", "detailed", NULL,
};
static const char *const theme_vals[] = {
	"dark", "light", NULL,
};
/*
 * libfymd4c embedded theme names for tab completion. The authoritative check is
 * markdown_theme_valid() (queried from libfymd4c), so this list may lag the
 * library's catalogue without rejecting a valid name.
 */
static const char *const markdown_theme_vals[] = {
	"default", "catppuccin", "catppuccin-borderless",
	"kanagawa", "kanagawa-borderless", "solarized", "solarized-borderless",
	"tokyonight", "tokyonight-borderless", NULL,
};
static const char *const bool_vals[] = {
	"on", "off", "true", "false", NULL,
};

static const struct fyai_slash_opt fyai_slash_opts[] = {
	{ "reasoning-effort", FYAIOK_STR, offsetof(struct fyai_cfg, reasoning_effort),
	  effort_vals, false, true, "reasoning/effort", "reasoning effort" },
	{ "reasoning-summary", FYAIOK_STR, offsetof(struct fyai_cfg, reasoning_summary),
	  summary_vals, false, true, "reasoning/summary", "reasoning summary" },
	{ "effort", FYAIOK_STR, offsetof(struct fyai_cfg, reasoning_effort),
	  effort_vals, false, true, "reasoning/effort", "reasoning effort (alias)" },
	{ "summary", FYAIOK_STR, offsetof(struct fyai_cfg, reasoning_summary),
	  summary_vals, false, true, "reasoning/summary", "reasoning summary (alias)" },
	{ "theme", FYAIOK_STR, offsetof(struct fyai_cfg, theme),
	  theme_vals, true, false, NULL, "background theme" },
	{ "markdown-theme", FYAIOK_STR, offsetof(struct fyai_cfg, markdown_theme),
	  markdown_theme_vals, true, false, NULL, "markdown palette" },
	{ "code-theme", FYAIOK_STR, offsetof(struct fyai_cfg, code_theme),
	  NULL, true, false, NULL, "fenced-code theme" },
	{ "markdown", FYAIOK_BOOL, offsetof(struct fyai_cfg, markdown),
	  NULL, false, false, NULL, "markdown rendering" },
	{ "stream", FYAIOK_BOOL, offsetof(struct fyai_cfg, stream),
	  NULL, false, false, NULL, "response streaming" },
	{ "thinking", FYAIOK_BOOL, offsetof(struct fyai_cfg, thinking),
	  NULL, false, false, NULL, "display reasoning/thinking output" },
	{ "sandbox", FYAIOK_BOOL, offsetof(struct fyai_cfg, enable_sandbox),
	  NULL, false, false, NULL, "tool sandbox" },
	{ "token-extents", FYAIOK_BOOL, offsetof(struct fyai_cfg, token_extents),
	  NULL, false, false, NULL, "record streamed token extents" },
	{ "print-stats", FYAIOK_BOOL, offsetof(struct fyai_cfg, stats),
	  NULL, false, false, NULL, "end-of-run usage stats" },
	{ "temperature", FYAIOK_FLOAT, offsetof(struct fyai_cfg, temperature),
	  NULL, false, false, "temperature", "sampling temperature" },
};

static void session_opt_print(struct fyai_cfg *cfg,
			      const struct fyai_slash_opt *o)
{
	const void *field = (const char *)cfg + o->off;
	const char *s;

	switch (o->kind) {
	case FYAIOK_STR:
		s = *(const char *const *)field;
		printf("%s: %s\n", o->name, s && *s ? s : "(unset)");
		break;
	case FYAIOK_BOOL:
		printf("%s: %s\n", o->name,
		       *(const bool *)field ? "on" : "off");
		break;
	case FYAIOK_FLOAT:
		printf("%s: %g\n", o->name, (double)*(const float *)field);
		break;
	}
}

static int session_opt_run(struct fyai_ctx *ctx,
			   const struct fyai_slash_opt *o, const char *arg)
{
	struct fyai_cfg *cfg = ctx->cfg;
	void *field = (char *)cfg + o->off;
	const char *const *v;
	char *endp;
	float f;
	int idx;

	if (!arg || !*arg) {
		session_opt_print(cfg, o);
		return 0;
	}
	if (o->reasoning && cfg->api_mode == FYAI_API_MESSAGES) {
		fprintf(stderr,
			"%s: reasoning options are not supported with the "
			"Messages API yet\n", o->name);
		return -1;
	}

	switch (o->kind) {
	case FYAIOK_STR:
		if (o->values && str_in_set(arg, o->values) < 0) {
			fprintf(stderr, "%s: invalid value '%s' (", o->name,
				arg);
			for (v = o->values; *v; v++)
				fprintf(stderr, "%s%s", v == o->values ?
					"" : "|", *v);
			fprintf(stderr, ")\n");
			return -1;
		}
		*(const char **)field = fy_gb_intern_string(cfg->gb, arg);
		break;
	case FYAIOK_BOOL:
		idx = str_in_set(arg, bool_vals);
		if (idx < 0) {
			fprintf(stderr, "%s: invalid value '%s' (on|off)\n",
				o->name, arg);
			return -1;
		}
		*(bool *)field = !(idx & 1);
		break;
	case FYAIOK_FLOAT:
		errno = 0;
		f = strtof(arg, &endp);
		if (errno || *endp) {
			fprintf(stderr, "%s: invalid number '%s'\n",
				o->name, arg);
			return -1;
		}
		*(float *)field = f;
		break;
	}

	if (o->restyle && cfg->markdown)
		fyai_markdown_load_style(cfg);
	if (o->ckey)
		session_persist(ctx, o->ckey, o->kind == FYAIOK_STR ?
				fy_sprintfa("'%s'", arg) : arg);
	session_opt_print(cfg, o);
	return 0;
}

/* ---- slash dispatch ------------------------------------------------------ */

struct fyai_slash_cmd {
	const char *name;
	const char *args;
	const char *help;
	int (*run)(struct fyai_ctx *ctx, const char *arg);
};

static int slash_clear(struct fyai_ctx *ctx, const char *arg)
{
	(void)arg;
	return fyai_session_clear(ctx);
}

static int slash_compact(struct fyai_ctx *ctx, const char *arg)
{
	return fyai_session_compact(ctx, arg);
}

static int slash_model(struct fyai_ctx *ctx, const char *arg)
{
	return fyai_session_model(ctx, arg);
}

static int slash_api(struct fyai_ctx *ctx, const char *arg)
{
	return fyai_session_api(ctx, arg);
}

static int slash_context(struct fyai_ctx *ctx, const char *arg)
{
	(void)arg;
	return fyai_session_context(ctx);
}

static int slash_stats(struct fyai_ctx *ctx, const char *arg)
{
	(void)arg;
	return fyai_show_stats(ctx);
}

static int slash_tools(struct fyai_ctx *ctx, const char *arg)
{
	char input[512], *save, *word;
	const char *agent;
	bool full;

	if (!arg)
		arg = "";
	if (strlen(arg) >= sizeof(input)) {
		fprintf(stderr, "tools: arguments are too long\n");
		return -1;
	}
	strcpy(input, arg);
	agent = NULL;
	full = false;
	word = strtok_r(input, " \t", &save);
	while (word) {
		if (!strcmp(word, "--full"))
			full = true;
		else if (!strcmp(word, "--brief"))
			full = false;
		else if (!agent)
			agent = word;
		else {
			fprintf(stderr, "tools: use /tools [agent] [--brief|--full]\n");
			return -1;
		}
		word = strtok_r(NULL, " \t", &save);
	}
	return fyai_catalog_tools(ctx, agent, full);
}

static int slash_config(struct fyai_ctx *ctx, const char *arg)
{
	struct fyai_config_args *args = &ctx->cfg->cmd.args.config;
	struct fyai_config_args saved = *args;
	const char *key, *value;
	size_t keylen, len;
	int rc;

	while (*arg == ' ' || *arg == '\t')
		arg++;
	len = strcspn(arg, " \t");
	if (!len) {
		args->type = FYAICT_SHOW;
		args->key = NULL;
		args->value = NULL;
		rc = fyai_execute_config(ctx);
		*args = saved;
		return rc;
	}
	key = arg + len;
	while (*key == ' ' || *key == '\t')
		key++;
	value = key + strcspn(key, " \t");
	keylen = (size_t)(value - key);
	while (*value == ' ' || *value == '\t')
		value++;

	if (len == 4 && !strncmp(arg, "show", len)) {
		args->type = FYAICT_SHOW;
		args->key = NULL;
		args->value = NULL;
	} else if (len == 9 && !strncmp(arg, "effective", len)) {
		args->type = FYAICT_EFFECTIVE;
		args->key = NULL;
		args->value = NULL;
	} else if (len == 4 && !strncmp(arg, "edit", len)) {
		args->type = FYAICT_EDIT;
		args->key = NULL;
		args->value = NULL;
	} else if (len == 8 && !strncmp(arg, "validate", len)) {
		args->type = FYAICT_VALIDATE;
		args->key = NULL;
		args->value = NULL;
	} else if (len == 6 && !strncmp(arg, "schema", len)) {
		args->type = FYAICT_SCHEMA;
		args->key = NULL;
		args->value = NULL;
	} else if (len == 8 && !strncmp(arg, "describe", len)) {
		args->type = FYAICT_DESCRIBE;
		args->key = *key ? fy_gb_intern_string(ctx->cfg->gb, key) : NULL;
		args->value = NULL;
	} else if (len == 3 && !strncmp(arg, "get", len)) {
		if (!*key)
			goto usage;
		args->type = FYAICT_GET;
		args->key = fy_gb_intern_string(ctx->cfg->gb, key);
		args->value = NULL;
	} else if (len == 6 && !strncmp(arg, "delete", len)) {
		if (!*key)
			goto usage;
		args->type = FYAICT_DELETE;
		args->key = fy_gb_intern_string(ctx->cfg->gb, key);
		args->value = NULL;
	} else if (len == 3 && !strncmp(arg, "set", len)) {
		if (!*key || !*value)
			goto usage;
		args->type = FYAICT_SET;
		args->key = fy_gb_intern_string(ctx->cfg->gb,
						fy_sprintfa("%.*s",
							(int)keylen, key));
		args->value = fy_gb_intern_string(ctx->cfg->gb, value);
	} else {
		goto usage;
	}

	rc = fyai_execute_config(ctx);
	/*
	 * A successful in-session mutation only touched the arena; refresh the
	 * live derived cache (colours, renderer, model) so the change is visible
	 * on the next prompt instead of only after a restart.
	 */
	if (!rc && (args->type == FYAICT_SET || args->type == FYAICT_DELETE ||
		    args->type == FYAICT_EDIT || args->type == FYAICT_IMPORT))
		rc = fyai_config_rederive(ctx);
	*args = saved;
	return rc;
usage:
	fprintf(stderr, "config: use /config [show|effective|edit|validate|schema|describe [path]|get <key>|set <key> <value>|delete <key>]\n");
	*args = saved;
	return -1;
}

static int slash_list(struct fyai_ctx *ctx, const char *arg)
{
	struct fyai_list_args *args = &ctx->cfg->cmd.args.list;
	struct fyai_list_args saved = *args;
	static const struct {
		const char *name;
		enum fyai_list_type type;
	} targets[] = {
		{ "providers", FYAILT_PROVIDERS },
		{ "models", FYAILT_MODELS },
		{ "turns", FYAILT_TURNS },
		{ "exchanges", FYAILT_EXCHANGES },
	};
	size_t i;
	int rc;

	if (!arg || !*arg)
		arg = "providers";
	for (i = 0; i < ARRAY_SIZE(targets); i++) {
		if (strcmp(arg, targets[i].name))
			continue;
		args->type = targets[i].type;
		args->format = FYAIOF_MARKDOWN;
		args->full = false;
		rc = fyai_execute_list(ctx);
		*args = saved;
		return rc;
	}

	fprintf(stderr, "list: unknown target '%s' (providers|models|turns|exchanges|reflog)\n",
		arg);
	return -1;
}

static int slash_history(struct fyai_ctx *ctx, const char *arg)
{
	struct fyai_display_args *args = &ctx->cfg->cmd.args.display;
	char which[16];
	unsigned long n, hi;
	char *end;
	const char *p;
	bool sep;
	int rc;

	args->raw = false;
	args->turn_sel.type = FYAITST_ALL;
	if (arg && *arg) {
		if (sscanf(arg, "%15s", which) != 1)
			return fyai_display_view(ctx);
		p = arg + strlen(which);
		while (*p == ' ' || *p == '\t')
			p++;
		if (!strcmp(which, "first")) {
			n = strtoul(p, &end, 10);
			args->turn_sel.type = FYAITST_FIRST;
			args->turn_sel.first = n;
		} else if (!strcmp(which, "last")) {
			n = strtoul(p, &end, 10);
			args->turn_sel.type = FYAITST_LAST;
			args->turn_sel.last = n;
		} else if (!strcmp(which, "range")) {
			n = strtoul(p, &end, 10);
			sep = *end == ',' || *end == ':';
			if (sep)
				end++;
			if (*end < '0' || *end > '9') {
				fprintf(stderr, "history: use first N, last N, or range A,B\n");
				return -1;
			}
			hi = strtoul(end, &end, 10);
			args->turn_sel.type = FYAITST_RANGE;
			args->turn_sel.range_lo = n;
			args->turn_sel.range_hi = hi;
			if (!sep) {
				fprintf(stderr, "history: use first N, last N, or range A,B\n");
				return -1;
			}
		} else {
			fprintf(stderr, "history: use first N, last N, or range A,B\n");
			return -1;
		}
		while (*end == ' ' || *end == '\t')
			end++;
		if (*p < '0' || *p > '9' || *end) {
			fprintf(stderr, "history: use first N, last N, or range A,B\n");
			return -1;
		}
	}

	rc = fyai_display_view(ctx);
	args->turn_sel.type = FYAITST_ALL;
	return rc;
}

static int slash_log(struct fyai_ctx *ctx, const char *arg)
{
	return fyai_log_control(ctx, arg);
}

static int slash_secret(struct fyai_ctx *ctx, const char *arg)
{
	char *copy, *action, *name = NULL, *extra;
	enum fyai_secret_command command;
	int rc;

	(void)ctx;
	copy = strdup(arg ? arg : "");
	if (!copy)
		return -1;
	action = strtok(copy, " \t");
	if (!action || fy_equal(action, "status"))
		command = FYAI_SECRET_STATUS;
	else if (fy_equal(action, "set"))
		command = FYAI_SECRET_SET;
	else if (fy_equal(action, "delete"))
		command = FYAI_SECRET_DELETE;
	else {
		fprintf(stderr, "secret: use status [name]|set <name>|delete <name>\n");
		free(copy);
		return -1;
	}
	if (action)
		name = strtok(NULL, " \t");
	extra = strtok(NULL, " \t");
	if (extra || ((command == FYAI_SECRET_SET || command == FYAI_SECRET_DELETE) &&
		      (!name || !*name))) {
		fprintf(stderr, "secret: invalid arguments\n");
		free(copy);
		return -1;
	}
	/* The slash line contains only the logical name. SET prompts separately
	 * on /dev/tty, so secret material never enters linenoise history. */
	rc = fyai_secret_action(command, name, false);
	free(copy);
	return rc;
}

static int slash_help(struct fyai_ctx *ctx, const char *arg);

static const struct fyai_slash_cmd fyai_slash_cmds[] = {
	{ "clear", "", "start a fresh conversation", slash_clear },
	{ "compact", "[hint]", "summarize history into a fresh chain",
	  slash_compact },
	{ "model", "[name]", "show or switch the model", slash_model },
	{ "api", "[mode]", "show or switch the API grammar", slash_api },
	{ "config", "[show|effective|edit|get|set|delete]", "inspect or edit config",
	  slash_config },
	{ "list", "[what]", "list providers, models, turns, exchanges, or reflog",
	  slash_list },
	{ "history", "[last N]", "show conversation history", slash_history },
	{ "log", "[target action]", "control trace logging", slash_log },
	{ "logging", "[target action]", "alias for /log", slash_log },
	{ "secret", "[status [name]|set name|delete name]", "manage secrets (API keys: api-key/<provider>)", slash_secret },
	{ "context", "", "context fill and token estimate", slash_context },
	{ "stats", "", "this session's token usage", slash_stats },
	{ "tools", "[agent] [--brief|--full]", "list catalog agent tools", slash_tools },
	{ "help", "", "list slash commands", slash_help },
	{ "exit", "", "leave the session", NULL },
	{ "quit", "", "leave the session", NULL },
};

/* Render the slash-command reference as markdown, falling back to the plain
 * printer when the markdown renderer or the memory stream is unavailable. */
static void slash_help_plain(void)
{
	const struct fyai_slash_cmd *c;
	const struct fyai_slash_opt *o;
	const char *const *v;
	size_t i;

	printf("commands:\n");
	for (i = 0; i < ARRAY_SIZE(fyai_slash_cmds); i++) {
		c = &fyai_slash_cmds[i];
		printf("  /%-16s %-8s %s\n", c->name, c->args, c->help);
	}
	printf("settings (no value prints the current one; reasoning and "
	       "temperature persist to the config, display settings are "
	       "session-only):\n");
	for (i = 0; i < ARRAY_SIZE(fyai_slash_opts); i++) {
		o = &fyai_slash_opts[i];
		printf("  /%-16s %s", o->name, o->help);
		if (o->values) {
			printf(" (");
			for (v = o->values; *v; v++)
				printf("%s%s", v == o->values ? "" : "|", *v);
			printf(")");
		} else if (o->kind == FYAIOK_BOOL) {
			printf(" (on|off)");
		}
		printf("\n");
	}
	printf("a line starting with // is sent to the model verbatim "
	       "(minus one slash)\n");
}

static int slash_help(struct fyai_ctx *ctx, const char *arg)
{
	const struct fyai_slash_cmd *c;
	const struct fyai_slash_opt *o;
	const char *const *v;
	char *buf = NULL;
	size_t len = 0;
	FILE *fp;
	size_t i;

	(void)arg;

	if (!markdown_available(ctx->cfg)) {
		slash_help_plain();
		return 0;
	}

	fp = open_memstream(&buf, &len);
	if (!fp) {
		slash_help_plain();
		return 0;
	}

	fprintf(fp, "## Commands\n\n");
	fprintf(fp, "| Command | Description |\n");
	fprintf(fp, "| --- | --- |\n");
	for (i = 0; i < ARRAY_SIZE(fyai_slash_cmds); i++) {
		c = &fyai_slash_cmds[i];
		fprintf(fp, "| `/%s%s%s` | %s |\n", c->name,
			*c->args ? " " : "", c->args, c->help);
	}

	fprintf(fp, "\n## Settings\n\n");
	fprintf(fp, "No value prints the current one; reasoning and "
		"temperature persist to the config, display settings are "
		"session-only.\n\n");
	fprintf(fp, "| Setting | Values | Description |\n");
	fprintf(fp, "| --- | --- | --- |\n");
	for (i = 0; i < ARRAY_SIZE(fyai_slash_opts); i++) {
		o = &fyai_slash_opts[i];
		/* Values column: comma-separated (never `|`, which would break
		 * the table cell), or a dash when the setting is free-form. */
		fprintf(fp, "| `/%s` | ", o->name);
		if (o->values) {
			for (v = o->values; *v; v++)
				fprintf(fp, "%s%s", v == o->values ? "" : ", ",
					*v);
		} else if (o->kind == FYAIOK_BOOL) {
			fprintf(fp, "on, off");
		} else {
			fprintf(fp, "—");
		}
		fprintf(fp, " | %s |\n", o->help);
	}
	fprintf(fp, "\nA line starting with `//` is sent to the model "
		"verbatim (minus one slash).\n");
	fclose(fp);

	if (buf && fyai_print_markdown(buf, ctx->cfg))
		slash_help_plain();
	free(buf);
	return 0;
}

/* Find the unique command or option matching @name (@len chars): exact match
 * wins, else a unique prefix. Sets *cmdp or *optp; -1 when unknown/ambiguous. */
static int session_slash_lookup(const char *name, size_t len,
				const struct fyai_slash_cmd **cmdp,
				const struct fyai_slash_opt **optp)
{
	const struct fyai_slash_cmd *pc;
	const struct fyai_slash_opt *po;
	size_t i;
	int hits;

	*cmdp = NULL;
	*optp = NULL;

	for (i = 0; i < ARRAY_SIZE(fyai_slash_cmds); i++)
		if (strlen(fyai_slash_cmds[i].name) == len &&
		    !strncmp(fyai_slash_cmds[i].name, name, len)) {
			*cmdp = &fyai_slash_cmds[i];
			return 0;
		}
	for (i = 0; i < ARRAY_SIZE(fyai_slash_opts); i++)
		if (strlen(fyai_slash_opts[i].name) == len &&
		    !strncmp(fyai_slash_opts[i].name, name, len)) {
			*optp = &fyai_slash_opts[i];
			return 0;
		}

	hits = 0;
	pc = NULL;
	po = NULL;
	for (i = 0; i < ARRAY_SIZE(fyai_slash_cmds); i++)
		if (!strncmp(fyai_slash_cmds[i].name, name, len)) {
			pc = &fyai_slash_cmds[i];
			hits++;
		}
	for (i = 0; i < ARRAY_SIZE(fyai_slash_opts); i++)
		if (!strncmp(fyai_slash_opts[i].name, name, len)) {
			po = &fyai_slash_opts[i];
			hits++;
		}
	if (hits != 1)
		return -1;
	if (pc)
		*cmdp = pc;
	else
		*optp = po;
	return 0;
}

int fyai_session_slash(struct fyai_ctx *ctx, const char *line)
{
	const struct fyai_slash_cmd *cmd;
	const struct fyai_slash_opt *opt;
	const char *name, *arg;
	size_t len;
	bool own_transient;
	int rc;

	name = line + 1;	/* skip '/' */
	len = strcspn(name, " \t");
	if (!len) {
		fprintf(stderr, "unknown command '%s' (try /help)\n", line);
		return 0;
	}
	arg = name + len;
	while (*arg == ' ' || *arg == '\t')
		arg++;

	if (session_slash_lookup(name, len, &cmd, &opt)) {
		fprintf(stderr, "unknown or ambiguous command '%.*s' "
			"(try /help)\n", (int)(len + 1), line);
		return 0;
	}

	if (cmd && !cmd->run)	/* /exit, /quit */
		return 1;

	/* Backends build turns/generics; give them a transient builder. */
	own_transient = !ctx->transient_gb;
	if (own_transient && fyai_setup_transient_builder(ctx))
		return 0;

	if (cmd)
		rc = cmd->run(ctx, arg);
	else
		rc = session_opt_run(ctx, opt, arg);
	(void)rc;	/* errors were reported; the REPL continues */

	if (own_transient)
		fyai_cleanup_transient_builder(ctx);

	/* Settings/model/context may have changed; reflect it in the footer. */
	fyai_session_banner_update(ctx);
	return 0;
}

/* ---- tab completion ------------------------------------------------------ */

static struct fyai_ctx *session_completion_ctx;

void fyai_session_completion_init(struct fyai_ctx *ctx)
{
	session_completion_ctx = ctx;
}

static void session_complete_value(linenoiseCompletions *lc, const char *cmd,
				   size_t cmdlen, const char *word,
				   const char *value)
{
	const char *cand;

	if (strncmp(value, word, strlen(word)))
		return;
	cand = fy_sprintfa("/%.*s %s", (int)cmdlen, cmd, value);
	linenoiseAddCompletion(lc, cand);
}

void fyai_session_completion(const char *buf, linenoiseCompletions *lc)
{
	struct fyai_ctx *ctx = session_completion_ctx;
	const struct fyai_slash_cmd *cmd;
	const struct fyai_slash_opt *opt;
	const char *sp, *word, *s;
	const char *const *v;
	fy_generic cat, models, m, nm, agents, a;
	const char *cand;
	size_t i, len;

	if (!ctx || buf[0] != '/')
		return;

	sp = strchr(buf, ' ');
	if (!sp) {
		/* Complete the command name itself. */
		len = strlen(buf + 1);
		for (i = 0; i < ARRAY_SIZE(fyai_slash_cmds); i++)
			if (!strncmp(fyai_slash_cmds[i].name, buf + 1, len)) {
				cand = fy_sprintfa("/%s",
						   fyai_slash_cmds[i].name);
				linenoiseAddCompletion(lc, cand);
			}
		for (i = 0; i < ARRAY_SIZE(fyai_slash_opts); i++)
			if (!strncmp(fyai_slash_opts[i].name, buf + 1, len)) {
				cand = fy_sprintfa("/%s",
						   fyai_slash_opts[i].name);
				linenoiseAddCompletion(lc, cand);
			}
		return;
	}

	len = (size_t)(sp - buf) - 1;
	word = sp + 1;
	while (*word == ' ')
		word++;

	if (session_slash_lookup(buf + 1, len, &cmd, &opt))
		return;

	if (opt) {
		v = opt->values;
		if (!v && opt->kind == FYAIOK_BOOL)
			v = bool_vals;
		for (; v && *v; v++)
			session_complete_value(lc, buf + 1, len, word, *v);
		return;
	}

	if (cmd && !strcmp(cmd->name, "model")) {
		cat = fyai_catalog_effective(ctx->cfg->catalog, ctx->cfg->gb);
		models = fy_get(cat, "models");
		fy_foreach(m, models) {
			nm = fy_get(m, "name");
			s = fy_castp(&nm, "");
			if (*s)
				session_complete_value(lc, buf + 1, len,
						       word, s);
		}
	} else if (cmd && !strcmp(cmd->name, "history")) {
		session_complete_value(lc, buf + 1, len, word, "last");
		session_complete_value(lc, buf + 1, len, word, "first");
		session_complete_value(lc, buf + 1, len, word, "range");
	} else if (cmd && !strcmp(cmd->name, "list")) {
		session_complete_value(lc, buf + 1, len, word, "models");
		session_complete_value(lc, buf + 1, len, word, "providers");
		session_complete_value(lc, buf + 1, len, word, "turns");
		session_complete_value(lc, buf + 1, len, word, "exchanges");
		session_complete_value(lc, buf + 1, len, word, "reflog");
	} else if (cmd && !strcmp(cmd->name, "tools")) {
		if (word[0] != '-') {
			session_complete_value(lc, buf + 1, len, word, "fyai");
			cat = fyai_catalog_effective(ctx->cfg->catalog, ctx->cfg->gb);
			agents = fy_get(cat, "agents");
			fy_foreach(a, agents) {
				nm = fy_get(a, "name");
				s = fy_castp(&nm, "");
				if (*s)
					session_complete_value(lc, buf + 1, len,
							       word, s);
			}
		}
		session_complete_value(lc, buf + 1, len, word, "--brief");
		session_complete_value(lc, buf + 1, len, word, "--full");
	} else if (cmd && !strcmp(cmd->name, "config")) {
		session_complete_value(lc, buf + 1, len, word, "show");
		session_complete_value(lc, buf + 1, len, word, "effective");
		session_complete_value(lc, buf + 1, len, word, "edit");
		session_complete_value(lc, buf + 1, len, word, "validate");
		session_complete_value(lc, buf + 1, len, word, "schema");
		session_complete_value(lc, buf + 1, len, word, "describe");
		session_complete_value(lc, buf + 1, len, word, "get");
		session_complete_value(lc, buf + 1, len, word, "set");
		session_complete_value(lc, buf + 1, len, word, "delete");
	} else if (cmd && !strcmp(cmd->name, "api")) {
		session_complete_value(lc, buf + 1, len, word, "responses");
		session_complete_value(lc, buf + 1, len, word, "chat-completions");
		session_complete_value(lc, buf + 1, len, word, "messages");
	} else if (cmd && (!strcmp(cmd->name, "log") ||
			   !strcmp(cmd->name, "logging"))) {
		session_complete_value(lc, buf + 1, len, word, "wire");
		session_complete_value(lc, buf + 1, len, word, "stream");
		session_complete_value(lc, buf + 1, len, word, "conversation");
		session_complete_value(lc, buf + 1, len, word, "all");
		session_complete_value(lc, buf + 1, len, word, "start");
		session_complete_value(lc, buf + 1, len, word, "stop");
		session_complete_value(lc, buf + 1, len, word, "clear");
		session_complete_value(lc, buf + 1, len, word, "view");
	}
}
