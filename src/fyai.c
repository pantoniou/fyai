/*
 * fyai.c - Minimal OpenAI Chat
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

/* The engine speaks for fyai itself, so its diagnostics stay unprefixed. */
#define FYAI_MODULE FYAIEM_UNKNOWN

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <linenoise.h>

#include "fyai.h"
#include "fyai_catalog.h"
#include "fyai_config.h"
#include "fyai_display.h"
#include "fyai_log.h"
#include "fyai_markdown.h"
#include "fyai_provider.h"
#include "fyai_session.h"
#include "fyai_prof.h"
#include "fyai_signal.h"
#include "fyai_storage.h"
#include "fyai_stream.h"
#include "fyai_terminal.h"
#include "fyai_tools.h"
#include "fyai_turn.h"

static void fyai_print_final_response(struct fyai_ctx *ctx,
				      fy_generic response_doc)
{
	struct fyai_cfg *cfg = ctx->cfg;
	const char *text;

	text = fy_cast(fyai_response_output_text(ctx, response_doc), "");
	if (cfg->markdown && !fyai_print_markdown(text, cfg))
		return;

	printf("%s\n", text);
}

static void fyai_print_cache_info(struct fyai_ctx *ctx, fy_generic doc)
{
	fy_generic usage;
	long long input_tokens;
	long long cached_tokens;
	long long output_tokens;
	long long total_tokens;
	double ratio;

	ratio = 0.0;

	usage = fyai_extract_usage(ctx, doc);
	input_tokens = fy_get(usage, "input", 0LL);
	cached_tokens = fy_get(usage, "cached", 0LL);
	output_tokens = fy_get(usage, "output", 0LL);
	total_tokens = fy_get(usage, "total", 0LL);

	if (input_tokens)
		ratio = (double)cached_tokens * 100.0 / (double)input_tokens;

	printf("cache-info: input=%lld cached=%lld cached_ratio=%.1f%% "
	       "output=%lld total=%lld\n",
	       input_tokens, cached_tokens, ratio, output_tokens, total_tokens);
}

/*
 * Normalize one model call's token usage into a canonical, provider-agnostic
 * mapping { input, cached, cache_write, output, reasoning, total, cost }.
 * Responses and Chat Completions name the wire fields differently; both are
 * mapped here. Returns fy_invalid when the response carries no usage. The
 * result is attached to the turn (so `fyai stats` can sum it) and fed to the
 * in-memory run counters.
 */

/* Add a normalized usage mapping to the running in-memory session totals. */
static void fyai_accumulate_usage(struct fyai_ctx *ctx, fy_generic usage)
{
	if (fy_generic_is_invalid(usage))
		return;

	ctx->usage_input += fy_get(usage, "input", 0LL);
	ctx->usage_cached += fy_get(usage, "cached", 0LL);
	ctx->usage_cache_write += fy_get(usage, "cache_write", 0LL);
	ctx->usage_output += fy_get(usage, "output", 0LL);
	ctx->usage_reasoning += fy_get(usage, "reasoning", 0LL);
	ctx->usage_total += fy_get(usage, "total", 0LL);
	ctx->usage_cost += fy_get(usage, "cost", 0.0);
	ctx->usage_calls++;

	/* Last-call snapshot: ground truth for the /context fill report. */
	ctx->last_call_input = fy_get(usage, "input", 0LL);
	ctx->last_call_output = fy_get(usage, "output", 0LL);
	ctx->last_call_total = fy_get(usage, "total", 0LL);
}

void fyai_print_usage_stats(struct fyai_ctx *ctx)
{
	double ratio;

	ratio = 0.0;

	if (ctx->usage_input)
		ratio = (double)ctx->usage_cached * 100.0 /
			(double)ctx->usage_input;

	fprintf(stderr,
		"stats: calls=%d input=%lld cached=%lld (%.1f%%)",
		ctx->usage_calls, ctx->usage_input, ctx->usage_cached, ratio);
	if (ctx->usage_cache_write)
		fprintf(stderr, " cache_write=%lld", ctx->usage_cache_write);
	fprintf(stderr, " output=%lld", ctx->usage_output);
	if (ctx->usage_reasoning)
		fprintf(stderr, " reasoning=%lld", ctx->usage_reasoning);
	fprintf(stderr, " total=%lld", ctx->usage_total);
	if (ctx->usage_cost > 0.0)
		fprintf(stderr, " cost=$%.6f", ctx->usage_cost);
	fprintf(stderr, "\n");
}

static fy_generic fyai_run_tool_call(struct fyai_ctx *ctx, fy_generic turn,
				     fy_generic tool_call)
{
	struct fyai_cfg *cfg = ctx->cfg;
	fy_generic tool_message;
	fy_generic tool_result;
	fy_generic max_output_length;
	fy_generic out;
	fy_generic tool_call_type;
	const char *tool_call_id, *tool_call_output_type;
	const char *name;
	bool shell;

	assert(ctx->transient_gb);

	tool_call_type = fy_get(tool_call, "type");
	if (fy_equal(tool_call_type, "shell_call"))
		name = "shell";
	else if (cfg->api_mode == FYAI_API_CHAT_COMPLETIONS)
		name = fy_get(fy_get(tool_call, "function"), "name", "");
	else
		name = fy_get(tool_call, "name", "");
	shell = fy_equal(name, "shell");

	/*
	 * Shell streams its output live as it runs, so it prints its own header
	 * and its result progressively (bounded + indented, the same fenced
	 * render as history); every other tool renders after the fact through
	 * fyai_render_tool_exchange(). Non-markdown always streams raw.
	 */
	if (!cfg->markdown || shell)
		fyai_print_tool_call(ctx, tool_call);
	if (cfg->debug)
		emit_generic_to_stdout("tool-call", tool_call, cfg->pretty);

	tool_result = fyai_execute_tool_call(ctx, tool_call);

	if (cfg->markdown && !shell)
		fyai_render_tool_exchange(ctx, tool_call, tool_result);
	if (cfg->debug)
		emit_generic_to_stdout("tool-result", tool_result,
				       cfg->pretty);

	switch (cfg->api_mode) {
	case FYAI_API_RESPONSES:
		tool_call_id = fy_get(tool_call, "call_id", "");

		/*
		 * The output item type must match the call item type: a native
		 * `shell_call` is answered with `shell_call_output`, while every
		 * regular function tool (read_file, write_file, shell, ask_user)
		 * arrives as a `function_call` and must be answered with
		 * `function_call_output`. Submitting a shell_call_output for a
		 * function_call is rejected by the API, so the tool reads as
		 * "failed".
		 */
		if (fy_equal(tool_call_type, "shell_call")) {

			tool_call_output_type = "shell_call_output";

			max_output_length = fy_get_at_path(tool_call, "action", "max_output_length");
			if (!fy_generic_is_valid(max_output_length))
				max_output_length = fy_null;
		} else {
			tool_call_output_type = "function_call_output";
			max_output_length = fy_null;
		}


		tool_message = fy_null_filtered_mapping(
				"type", tool_call_output_type,
				"call_id", tool_call_id,
				"output", tool_result,
				"max_output_length", max_output_length);
		break;

	case FYAI_API_CHAT_COMPLETIONS:
		tool_call_id = fy_get(tool_call, "id", "");
		tool_message = fy_mapping(
				"role", "tool",
				"tool_call_id", tool_call_id,
				"content", tool_result);
		break;

	case FYAI_API_MESSAGES:
		/* Tool calls were normalized to function_call items at the
		 * parse boundary; answer with the matching canonical output
		 * item (fyai_messages_input puts a tool_result on the wire). */
		tool_call_id = fy_get(tool_call, "call_id", "");
		tool_message = fy_mapping(
				"type", "function_call_output",
				"call_id", tool_call_id,
				"output", tool_result);
		break;

	default:
		assert(0);
		__builtin_unreachable();
		break;
	}

	out = fyai_turn_append(ctx, turn, fy_sequence(tool_message));

	out = fy_gb_internalize(ctx->transient_gb, out);
	fyai_error_check(ctx, fy_generic_is_valid(out), err,
			 "could not append the tool result");
	return out;
err:
	return fy_invalid;
}

static fy_generic fyai_run_response_tool_calls(struct fyai_ctx *ctx,
					       fy_generic turn,
					       fy_generic response_doc)
{
	fy_generic tool_calls;
	fy_generic tool_call;

	tool_calls = fyai_response_tool_calls(ctx, response_doc);

	fy_foreach(tool_call, tool_calls) {
		if (fy_generic_is_invalid(turn))
			break;
		/* ^C during a tool run: stop issuing further calls; the loop
		 * wraps what completed with an "interrupted" diagnostic. */
		if (fyai_sig_intr_pending())
			break;
		turn = fyai_run_tool_call(ctx, turn, tool_call);
	}

	turn = fy_gb_internalize(ctx->transient_gb, turn);
	fyai_error_check(ctx, fy_generic_is_valid(turn), err,
			 "could not append the tool calls");
	return turn;
err:
	return fy_invalid;
}

/*
 * Canonical messages are stored in whatever shape the provider that produced
 * them used: a Chat Completions turn records a tool request as
 * `{role: assistant, content: null, tool_calls: [...]}` followed by
 * `{role: tool, tool_call_id, content}`, while a Responses turn records native
 * output items (`reasoning`, `function_call`, ...). The Responses API input
 * grammar rejects the Chat shapes outright -- the null assistant content alone
 * is an `invalid_type` error -- so a conversation begun under Chat Completions
 * cannot be continued against the Responses API without translation. Map each
 * canonical message into a valid Responses input item; native Responses items
 * (anything carrying a `type`) pass through verbatim.
 */

/* Join the text parts of a Responses `message` output item into one string. */

/*
 * Symmetric to fyai_responses_input(): translate canonical messages into Chat
 * Completions message shape so a conversation begun under the Responses API can
 * be continued against a Chat Completions provider. Responses native output
 * items are converted: function_call (and builtin shell_call) accumulate into
 * an assistant message carrying tool_calls; function_call_output /
 * shell_call_output -> {role: tool}; a `message` item -> {role, content};
 * reasoning items are provider wire detail with no Chat analogue and are
 * dropped. Chat-shaped messages (role present, no type) pass through.
 */

/*
 * Attach a diagnostic message to a generic: a manual
 * FYGIF_DIAG indirect. @value may be fy_invalid (hard failure - the result
 * still tests invalid everywhere since fy_generic_is_invalid dereferences
 * indirects) or a partial result worth keeping. The message is recovered at
 * the display boundary with fy_generic_get_diag().
 */
fy_generic fyai_with_diag(struct fy_generic_builder *gb, fy_generic value,
			  const char *msg)
{
	fy_generic_indirect gi;
	fy_generic v;

	memset(&gi, 0, sizeof(gi));
	gi.flags = FYGIF_VALUE | FYGIF_DIAG;
	gi.value = value;
	gi.diag = fy_gb_to_generic(gb, msg);
	if (fy_generic_is_invalid(gi.diag))
		return value;
	v = fy_gb_indirect_create(gb, &gi);
	/*
	 * The result is an indirect object that *dereferences* to @value (often
	 * fy_invalid), so is-valid unwraps it - check the indirect word itself
	 * with the direct predicate, else the wrapper (and its diag) is lost.
	 */
	return fy_generic_is_direct_valid(v) ? v : value;
}

/*
 * Move a diagnostic attached to @v into the collected sink and return the
 * unwrapped value. It is raised as the error rather than printed here, so it
 * lands in order with the rest and, being the cause, demotes the caller's own
 * "could not do X" behind it.
 */
fy_generic fyai_report_diag(struct fyai_ctx *ctx, fy_generic v)
{
	fy_generic diag;

	diag = fy_generic_get_diag(v);
	if (fy_generic_is_valid(diag) && !fy_generic_is_null_type(diag)) {
		fyai_error(ctx, "%s", fy_castp(&diag, ""));
		return fy_generic_indirect_get_value(v);
	}
	return v;
}

/*
 * A small stderr spinner covering the wait for the first response byte:
 * connect, TLS, request upload and the model's own think-time. Driven by
 * curl's transfer-info callback (which fires periodically even while no data
 * flows) and erased as soon as the download counter moves - from then on the
 * progressive renderer owns the terminal. Only on a tty; piped stderr stays
 * byte-clean.
 */
struct fyai_spinner {
	bool enabled;
	bool visible;
	struct timespec last;
	unsigned int frame;
};

static void fyai_spinner_erase(struct fyai_spinner *sp)
{
	if (!sp->visible)
		return;
	fprintf(stderr, "\r\033[K");
	fflush(stderr);
	sp->visible = false;
}

static int fyai_spinner_xferinfo(void *p, curl_off_t dltotal, curl_off_t dlnow,
				 curl_off_t ultotal, curl_off_t ulnow)
{
	static const char *const frames[] = {
		"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏",
	};
	struct fyai_spinner *sp = p;
	struct timespec now;
	long long elapsed_ms;

	(void)dltotal;
	(void)ultotal;
	(void)ulnow;

	/* ^C: abort the transfer (curl returns CURLE_ABORTED_BY_CALLBACK).
	 * Checked for the whole transfer, not just while the spinner shows. */
	if (fyai_sig_intr_pending()) {
		fyai_spinner_erase(sp);
		return 1;
	}

	if (!sp->enabled)
		return 0;

	/* First body byte: the stream renderer takes over. */
	if (dlnow > 0) {
		fyai_spinner_erase(sp);
		sp->enabled = false;
		return 0;
	}

	clock_gettime(CLOCK_MONOTONIC, &now);
	elapsed_ms = (now.tv_sec - sp->last.tv_sec) * 1000LL +
		     (now.tv_nsec - sp->last.tv_nsec) / 1000000LL;
	if (sp->visible && elapsed_ms < 100)
		return 0;
	sp->last = now;

	fprintf(stderr, "\r%s", frames[sp->frame++ % ARRAY_SIZE(frames)]);
	fflush(stderr);
	sp->visible = true;
	return 0;
}

static fy_generic fyai_run_model_step(struct fyai_ctx *ctx, fy_generic turn,
				      fy_generic previous)
{
	struct fyai_cfg *cfg = ctx->cfg;
	const char *request_body;
	const char *instructions;
	fy_generic previous_response_id;
	fy_generic catalog;
	fy_generic cat_model;
	fy_generic stream_options;
	fy_generic tool_choice;
	fy_generic response_doc;
	fy_generic reasoning;
	fy_generic request;
	fy_generic messages;
	fy_generic m;
	fy_generic v;
	struct fyai_spinner spinner = {};
	struct timespec t_emit, t_send;
	bool want_extents_lp;
	fy_generic diag;
	long status;

	fyai_prof_stamp(&t_emit);

	/* Never let a stale streamed collection attach to this step's turn. */
	ctx->last_token_extents = fy_invalid;

	messages = fyai_turn_messages_since(ctx, turn, previous);
	previous_response_id = fy_get(fyai_turn_meta(previous), "response_id");

	/* Prefer the conversation's own canonical system turn so the
	 * instructions stay stable even if cfg.system_prompt differs from
	 * what the conversation was started with. Responses surfaces it as
	 * `instructions`, Messages as the top-level `system` field. */
	instructions = cfg->system_prompt;
	fy_foreach(m, messages) {
		if (fy_equal(fy_get(m, "role"), "system")) {
			instructions = fy_get(m, "content", instructions);
			break;
		}
	}

	switch (cfg->api_mode) {
	case FYAI_API_RESPONSES:
		v = fy_mapping(
			"model", cfg->model,
			"instructions", instructions,
			"input", fyai_responses_input(ctx, messages),
			"store", cfg->response_chain);
		break;

	case FYAI_API_CHAT_COMPLETIONS:
		v = fy_mapping(
			"model", cfg->model,
			"messages", fyai_chat_input(ctx, messages));
		break;

	case FYAI_API_MESSAGES:
		/*
		 * Anthropic only caches on explicit cache_control breakpoints.
		 * The system block gets one (covering the tools + system
		 * prefix); fyai_messages_input() places the second on the last
		 * block of the replayed history so each turn extends the
		 * cached span. Markers are added at request build time only -
		 * canonical state never carries them.
		 */
		v = fy_mapping(
			"model", cfg->model,
			"max_tokens", (long long)cfg->max_tokens,
			"system", fy_sequence(
					fy_mapping("type", "text",
						   "text", instructions,
						   "cache_control", fy_mapping("type", "ephemeral"))),
			"messages", fyai_messages_input(ctx, messages));
		break;
	default:
		assert(0);
		__builtin_unreachable();
		break;
	}

	fyai_error_check(ctx, fy_generic_is_valid(v), out,
			 "could not build the model request");
	request = v;

	/*
	 * Reasoning models reject an explicit `temperature` (the Responses API
	 * 400s, and Chat reasoning models ignore or reject it), so only send it
	 * when reasoning is not configured.
	 */
	catalog = fyai_catalog_effective(cfg->catalog, cfg->gb);
	cat_model = fyai_catalog_resolved_model(catalog, cfg->model);

	if (fyai_model_supports_temperature(cat_model) &&
	    (!cfg->reasoning_effort || !*cfg->reasoning_effort) &&
	    (!cfg->reasoning_summary || !*cfg->reasoning_summary) &&
	    cfg->api_mode != FYAI_API_MESSAGES)
		request = fy_assoc(request, "temperature", cfg->temperature);

	if (cfg->response_chain &&
	    fy_generic_is_valid(previous_response_id) &&
	    !fy_generic_is_null_type(previous_response_id)) {

		switch (cfg->api_mode) {
		case FYAI_API_RESPONSES:
			request = fy_assoc(request, "previous_response_id", previous_response_id);
			break;
		default:
			break;
		}
	}

	if ((cfg->reasoning_effort && *cfg->reasoning_effort) ||
	    (cfg->reasoning_summary && *cfg->reasoning_summary)) {

		switch (cfg->api_mode) {
		case FYAI_API_RESPONSES:
			reasoning = fy_map_empty;
			if (cfg->reasoning_effort && *cfg->reasoning_effort)
				reasoning = fy_assoc(reasoning, "effort", cfg->reasoning_effort);
			if (cfg->reasoning_summary && *cfg->reasoning_summary)
				reasoning = fy_assoc(reasoning, "summary", cfg->reasoning_summary);
			request = fy_assoc(request, "reasoning", reasoning);
			break;

		case FYAI_API_CHAT_COMPLETIONS:
			if (cfg->reasoning_effort && *cfg->reasoning_effort)
				request = fy_assoc(request,
					"reasoning_effort", cfg->reasoning_effort);
			break;

		case FYAI_API_MESSAGES:
			break;

		default:
			assert(0);
			__builtin_unreachable();
			break;
		}
	}

	if (cfg->enable_tools || cfg->enable_builtin_shell) {
		/* Messages spells tool_choice as an object, not a string. */
		tool_choice = cfg->api_mode == FYAI_API_MESSAGES ?
				fy_mapping("type", "auto") : fy_value("auto");
		request = fy_assoc(request,
				"tools", ctx->tools,
				"tool_choice", tool_choice);
	}
	/*
	 * token_extents wants per-token delimitation, which only logprobs
	 * provide. Gate on the catalogue (reasoning models reject the params)
	 * and on the session fail-soft latch; the Messages API has no
	 * logprobs at all. Gated-off calls still record chunk extents from
	 * the stream.
	 */
	want_extents_lp = cfg->token_extents && cfg->stream &&
			  !ctx->token_extents_off &&
			  cfg->api_mode != FYAI_API_MESSAGES &&
			  (!cfg->reasoning_effort || !*cfg->reasoning_effort) &&
			  (!cfg->reasoning_summary || !*cfg->reasoning_summary) &&
			  fyai_model_supports_logprobs(cat_model);

	if (cfg->logprobs || want_extents_lp) {
		switch (cfg->api_mode) {
		case FYAI_API_CHAT_COMPLETIONS:
			request = fy_assoc(request, "logprobs", true);
			break;
		case FYAI_API_RESPONSES:
			if (want_extents_lp)
				request = fy_assoc(request,
					"top_logprobs", 0,
					"include", fy_sequence(fy_string(
						"message.output_text.logprobs")));
			break;
		default:
			break;
		}
	}

	if (cfg->top_logprobs >= 0)
		request = fy_assoc(request, "top_logprobs", cfg->top_logprobs);
	if (cfg->stream) {

		switch (cfg->api_mode) {
		case FYAI_API_RESPONSES:
			stream_options = fy_map_empty;
			break;

		case FYAI_API_CHAT_COMPLETIONS:
			stream_options = fy_mapping("include_usage", true);
			break;

		case FYAI_API_MESSAGES:
			stream_options = fy_map_empty;
			break;

		default:
			assert(0);
			__builtin_unreachable();
			break;
		}

		if (cfg->no_obfuscation && cfg->api_mode != FYAI_API_MESSAGES)
			stream_options = fy_assoc(stream_options, "include_obfuscation", false);
		request = fy_assoc(request, "stream", true);
		if (fy_len(stream_options))
			request = fy_assoc(request, "stream_options", stream_options);
	}

	if (cfg->debug)
		emit_generic_to_stdout("request", request, cfg->pretty);

	if (cfg->conversation_logging) {
		(void)fyai_log_generic(ctx, "conversation",
			fy_mapping(ctx->transient_gb,
				"kind", "request",
				"api", fyai_api_to_string(cfg->api_mode),
				"url", cfg->api_url,
				"body", request));
	}

	request_body = emit_request_body(ctx->transient_gb, request);

	/* Build + serialize done: this is "time to emit the request". */
	fyai_prof_since("request_emit", &t_emit);

	curl_easy_setopt(ctx->curl, CURLOPT_POSTFIELDS, request_body);

	spinner.enabled = terminal_is_tty(STDERR_FILENO);
	curl_easy_setopt(ctx->curl, CURLOPT_XFERINFOFUNCTION,
			 fyai_spinner_xferinfo);
	curl_easy_setopt(ctx->curl, CURLOPT_XFERINFODATA, &spinner);
	/* Always on: the callback also polls the ^C abort flag. */
	curl_easy_setopt(ctx->curl, CURLOPT_NOPROGRESS, 0L);

	/* Everything before the wire transfer, relative to process start. */
	fyai_prof_once_from_base("start_to_first_send");
	fyai_prof_stamp(&t_send);

	ctx->auth_retry_done = false;
	if (cfg->stream)
		response_doc = fyai_perform_streaming_request(ctx);
	else
		response_doc = fyai_perform_buffered_request(ctx);

	fyai_prof_since("request_roundtrip", &t_send);

	/* Failed/empty responses can end the transfer with it still shown. */
	fyai_spinner_erase(&spinner);
	curl_easy_setopt(ctx->curl, CURLOPT_NOPROGRESS, 1L);

	if (fy_generic_is_valid(response_doc))
		fyai_accumulate_usage(ctx, fyai_extract_usage(ctx, response_doc));

	if (cfg->debug && fy_generic_is_valid(response_doc))
		emit_generic_to_stdout("response", response_doc, cfg->pretty);
	if (cfg->debug && cfg->cache_info &&
	    fy_generic_is_valid(response_doc))
		fyai_print_cache_info(ctx, response_doc);

	/*
	 * A failed request (HTTP error, malformed body, truncated stream)
	 * yields an invalid generic; propagate it so the run fails cleanly.
	 * An interrupted request carries a diagnostic on the (invalid) result
	 * - pass it through untouched so the loop can surface "interrupted".
	 */
	if (fy_generic_is_invalid(response_doc)) {
		/*
		 * Fail-soft backstop: when the logprobs params were injected
		 * only for token_extents (never for an explicit --logprobs)
		 * and the provider rejected the request with a client error,
		 * latch off for the session and retry the step once without
		 * them. Interrupts carry a diag - pass those through.
		 */
		diag = fy_generic_get_diag(response_doc);
		if (want_extents_lp && !cfg->logprobs &&
		    (fy_generic_is_invalid(diag) ||
		     fy_generic_is_null_type(diag))) {
			status = 0;
			curl_easy_getinfo(ctx->curl, CURLINFO_RESPONSE_CODE,
					  &status);
			if (status >= 400 && status < 500) {
				ctx->token_extents_off = true;
				return fyai_run_model_step(ctx, turn, previous);
			}
		}
		return response_doc;
	}

	response_doc = fy_gb_internalize(ctx->transient_gb, response_doc);
	if (cfg->conversation_logging) {
		(void)fyai_log_generic(ctx, "conversation",
			fy_mapping(ctx->transient_gb,
				"kind", "response",
				"api", fyai_api_to_string(cfg->api_mode),
				"body", response_doc));
	}
	fyai_error_check(ctx, fy_generic_is_valid(response_doc), out,
			 "could not retain the provider response");
	return response_doc;
out:
	return fy_invalid;
}

void fyai_cleanup_transient_builder(struct fyai_ctx *ctx)
{
	if (ctx->transient_gb) {
		fy_generic_builder_destroy(ctx->transient_gb);
		ctx->transient_gb = NULL;
	}
	if (ctx->transient_allocator) {
		fy_allocator_destroy(ctx->transient_allocator);
		ctx->transient_allocator = NULL;
	}
}

int fyai_setup_transient_builder(struct fyai_ctx *ctx)
{
	struct fy_auto_allocator_cfg trans_cfg = {};
	struct fy_generic_builder_cfg gb_cfg = {};

	/* now the transient */
	memset(&trans_cfg, 0, sizeof(trans_cfg));
	trans_cfg.scenario = FYAST_PER_TAG_FREE_DEDUP;
	trans_cfg.estimated_max_size = 0;
	ctx->transient_allocator = fy_allocator_create("auto", &trans_cfg);
	assert(ctx->transient_allocator);

	memset(&gb_cfg, 0, sizeof(gb_cfg));
	gb_cfg.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED | FYGBCF_CREATE_TAG;
	gb_cfg.allocator = ctx->transient_allocator;
	gb_cfg.parent = ctx->gb;
	ctx->transient_gb = fy_generic_builder_create(&gb_cfg);
	assert(ctx->transient_gb);

	return 0;
}

fy_generic fyai_run_model_loop(struct fyai_ctx *ctx, fy_generic turn)
{
	struct fyai_cfg *cfg = ctx->cfg;
	fy_generic previous;
	fy_generic response_id;
	fy_generic response_doc;
	fy_generic previous_turn;
	fy_generic turn_in;
	fy_generic diag;
	fy_generic out;
	const char *msg;
	int i, rc;

	(void)rc;

	ctx->tool_output_displayed = false;
	previous = fy_get(turn, "previous", fy_null);
	turn_in = turn;
	out = fy_invalid;

	/* Drop any ^C that arrived while we were idle at the prompt. */
	fyai_sig_intr_check();

	for (i = 0; i < cfg->max_tool_iterations; i++) {

		previous_turn = cfg->response_chain ? previous : (i ? previous : fy_null);

		response_doc = fyai_run_model_step(ctx, turn, previous_turn);
		if (fy_generic_is_invalid(response_doc)) {
			/*
			 * Request failed or was interrupted: keep the steps
			 * completed so far (assistant/tool appends from
			 * earlier iterations); only the in-flight call is
			 * lost. The step's diagnostic rides on the result.
			 */
			diag = fy_generic_get_diag(response_doc);
			msg = fy_generic_is_valid(diag) &&
			      !fy_generic_is_null_type(diag) ?
				fy_castp(&diag, "request failed") :
				"request failed";
			if (turn.v != turn_in.v) {
				out = fy_gb_internalize(ctx->gb, turn);
				out = fyai_with_diag(ctx->gb, out, msg);
			} else {
				out = fyai_with_diag(ctx->gb, fy_invalid, msg);
			}
			return out;
		}
		response_id = fyai_response_id(ctx, response_doc);

		turn = fyai_append_assistant_response(ctx, turn, response_doc);

		if (cfg->response_chain)
			turn = fyai_turn_set_response_id(ctx, turn, response_id);

		if (fyai_response_is_final(ctx, response_doc)) {
			if (!cfg->stream)
				fyai_print_final_response(ctx, response_doc);
			out = turn;
			break;
		}

		if (fyai_response_needs_tool_calls(ctx, response_doc)) {

			if (cfg->response_chain)
				previous = turn;

			turn = fyai_run_response_tool_calls(ctx, turn, response_doc);
			if (ctx->ask_abort) {
				out = fy_null;
				break;
			}
			/* ^C during the tool run: commit what completed, tagged
			 * interrupted, and stop the loop. */
			if (fyai_sig_intr_check()) {
				out = fy_gb_internalize(ctx->gb, turn);
				return fyai_with_diag(ctx->gb, out, "interrupted");
			}
		}
	}

	if (i >= cfg->max_tool_iterations)
		out = fy_invalid;

	/* A failed run stays invalid; the caller decides how to surface it. */
	if (fy_generic_is_invalid(out))
		return fy_invalid;

	out = fy_gb_internalize(ctx->gb, out);

	if (fy_generic_is_invalid(out))
		fyai_error(ctx, "could not retain the completed turn");
	return out;
}

void fyai_cleanup(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg;

	if (!ctx)
		return;
	cfg = ctx->cfg;

	fyai_cleanup_transient_builder(ctx);

	if (ctx->headers) {
		curl_slist_free_all(ctx->headers);
		ctx->headers = NULL;
	}
	if (ctx->auth_header) {
		free(ctx->auth_header);
		ctx->auth_header = NULL;
	}
	if (ctx->user_agent) {
		free(ctx->user_agent);
		ctx->user_agent = NULL;
	}
	if (ctx->shell_stream) {
		fyai_fenced_stream_finish(ctx->shell_stream);
		free(ctx->shell_stream);
		ctx->shell_stream = NULL;
	}

	if (ctx->curl) {
		curl_easy_cleanup(ctx->curl);
		ctx->curl = NULL;
	}
	fyai_auth_cleanup(ctx);

	if (!fyai_cfg_no_storage(cfg))
		fyai_close_storage(ctx);
}

/*
 * (Re)build every piece of per-session request state derived from cfg: the
 * auth header, the header list, the endpoint URL on the curl handle and the
 * tools document. Idempotent, so a mid-session /model switch just calls it
 * again after re-resolving the model. Requires an active transient builder
 * (the tools construction goes through it).
 */
int fyai_request_state_apply(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg = ctx->cfg;
	int rc;

	assert(ctx->transient_gb);

	if (ctx->headers) {
		curl_slist_free_all(ctx->headers);
		ctx->headers = NULL;
	}
	if (ctx->auth_header) {
		free(ctx->auth_header);
		ctx->auth_header = NULL;
	}
	ctx->tools = fy_invalid;

	/* Anthropic authenticates with x-api-key (no Bearer scheme) and
	 * requires a protocol version header on every request.  Local no-auth
	 * servers skip the API-key header entirely. */
	if (!cfg->chatgpt_auth && !cfg->no_auth) {
		ctx->auth_header = make_header(cfg->api_mode == FYAI_API_MESSAGES ?
					       "x-api-key: " : "Authorization: Bearer ",
					       cfg->api_key);
		if (!ctx->auth_header) {
			if (!cfg->api_key || !*cfg->api_key)
				fyai_error(ctx, "no API key (set one via --api-key, the "
					   "provider's <PROVIDER>_API_KEY env var, or "
					   "the env mapping in config)");
			return -1;
		}
	}

	rc = append_header(&ctx->headers, "Content-Type: application/json");
	if (rc)
		return -1;

	if (cfg->chatgpt_auth)
		rc = fyai_auth_apply_headers(ctx, &ctx->headers);
	else if (ctx->auth_header)
		rc = append_header(&ctx->headers, ctx->auth_header);
	if (rc)
		return -1;

	if (cfg->api_mode == FYAI_API_MESSAGES) {
		rc = append_header(&ctx->headers,
				   "anthropic-version: " ANTHROPIC_VERSION);
		if (rc)
			return -1;
	}

	curl_easy_setopt(ctx->curl, CURLOPT_URL, cfg->api_url);
	curl_easy_setopt(ctx->curl, CURLOPT_HTTPHEADER, ctx->headers);

	if (cfg->enable_tools || cfg->enable_builtin_shell) {

		switch (cfg->api_mode) {
		case FYAI_API_RESPONSES:
			ctx->tools = fyai_make_responses_tools(ctx);
			break;
		case FYAI_API_CHAT_COMPLETIONS:
			ctx->tools = make_tools(ctx->gb);
			break;
		case FYAI_API_MESSAGES:
			ctx->tools = fyai_make_messages_tools(ctx);
			break;
		}
	}

	if (fy_generic_is_valid(ctx->tools)) {
		ctx->tools = fy_gb_internalize(ctx->gb, ctx->tools);
		fyai_error_check(ctx, fy_generic_is_valid(ctx->tools), err,
				 "could not retain the tool definitions");
	}

	return 0;
err:
	return -1;
}

int fyai_setup(struct fyai_ctx *ctx, struct fyai_cfg *cfg)
{
	struct timespec t_setup;
	char *instr;
	int rc;

	fyai_prof_stamp(&t_setup);

	memset(ctx, 0, sizeof(*ctx));
	ctx->cfg = cfg;

	ctx->stdout_tty = terminal_is_tty(STDOUT_FILENO);

	ctx->tools = fy_invalid;
	ctx->last_message = fy_invalid;
	ctx->arena_config = fy_invalid;
	ctx->arena_catalog = fy_invalid;
	ctx->last_token_extents = fy_invalid;

	if (!fyai_cfg_no_storage(cfg)) {
		if (fyai_setup_storage(ctx))
			goto err;
	}

	if (fyai_cfg_no_requests(cfg))
		return 0;

	ctx->curl = curl_easy_init();
	if (!ctx->curl)
		goto err;

	rc = asprintf(&ctx->user_agent, "%s/%s", "fyai", VERSION);
	if (rc == -1)
		goto err;

	curl_easy_setopt(ctx->curl, CURLOPT_WRITEFUNCTION, write_response);
	curl_easy_setopt(ctx->curl, CURLOPT_TIMEOUT, 600L);	/* 10 minutes */
	curl_easy_setopt(ctx->curl, CURLOPT_USERAGENT, ctx->user_agent);
	curl_easy_setopt(ctx->curl, CURLOPT_VERBOSE, 1L);
	curl_easy_setopt(ctx->curl, CURLOPT_DEBUGFUNCTION, fyai_curl_debug);
	curl_easy_setopt(ctx->curl, CURLOPT_DEBUGDATA, ctx);

	(void)fyai_setup_transient_builder(ctx);
	if (fyai_auth_resolve(ctx))
		goto err;

	rc = fyai_request_state_apply(ctx);
	if (rc)
		goto err;

	/*
	 * Fold repo-scoped instruction files (AGENTS.md/CLAUDE.md) into the
	 * system prompt before it is frozen into the canonical system turn, so
	 * project guidance travels with a new conversation (continuations keep
	 * the copy they were started with). Only for a fresh conversation -
	 * an existing chain already carries its own system turn.
	 */
	if (fy_generic_is_invalid(ctx->last_message)) {
		instr = fyai_project_instructions();

		if (instr) {
			cfg->system_prompt = fy_gb_intern_string(ctx->gb,
					fy_sprintfa("%s%s",
						cfg->system_prompt ?
							cfg->system_prompt : "",
						instr));
			free(instr);
		}
	}

	/*
	 * Seed the system prompt as the first canonical turn for both API
	 * modes, so it is provider-agnostic content that survives a provider
	 * switch and shows up in `history`/`dump state`. In Responses mode it is
	 * additionally surfaced as the request `instructions` field (and dropped
	 * from `input` to avoid duplication) by fyai_responses_input().
	 */
	if (fy_generic_is_invalid(ctx->last_message)) {
		ctx->last_message = fyai_turn_append(ctx, ctx->last_message,
			fy_sequence(fyai_make_system_message(ctx, cfg->system_prompt)));
	}

	if (cfg->prompt && *cfg->prompt) {
		ctx->last_message = fyai_turn_append(ctx, ctx->last_message,
			fy_sequence(fyai_make_user_message(ctx, cfg->prompt)));
	}

	/* intern all to durable */
	ctx->last_message = fy_gb_internalize(ctx->gb, ctx->last_message);
	fyai_error_check(ctx, fy_generic_is_valid(ctx->last_message), err,
			 "could not retain the initial turn");

	(void)fyai_cleanup_transient_builder(ctx);

	fyai_prof_since("setup", &t_setup);
	return 0;

err:
	/*
	 * Leave teardown to the caller: fyai_run() always calls fyai_cleanup()
	 * on the same ctx after a failed setup. Cleaning up here too would free
	 * the same handles twice (curl, headers, user_agent), so just unwind to
	 * the caller with the partially-initialized ctx - every field it touches
	 * is either NULL (memset above) or a live handle fyai_cleanup() guards.
	 */
	return -1;
}

int fyai_prompt_batch(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg = ctx->cfg;
	struct fyai_prompt_args *args = &cfg->cmd.args.prompt;
	fy_generic v;
	int rc;

	(void)args;
	assert(ctx);

	rc = fyai_setup_transient_builder(ctx);
	assert(!rc);

	/* not interactive? single run */
	v = fyai_run_model_loop(ctx, ctx->last_message);
	v = fyai_report_diag(ctx, v);
	if (fy_generic_is_invalid(v))
		goto err_out;
	ctx->last_message = v;

	/*
	 * Close the assistant turn with a blank line so it does not butt up
	 * against the shell prompt. The interactive loop gets this spacing from
	 * its own prompt, so only the batch path needs it, and only on a
	 * terminal - piped/redirected output stays byte-clean for scripting.
	 */
	if (ctx->stdout_tty)
		putchar('\n');

	rc = fyai_publish_state(ctx);
	if (rc)
		goto err_out;

	rc = 0;
out:
	fyai_cleanup_transient_builder(ctx);
	return rc;

err_out:
	rc = -1;
	goto out;
}

int fyai_prompt_interactive(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg = ctx->cfg;
	struct fyai_prompt_args *args = &cfg->cmd.args.prompt;
	const char *prompt;
	const char *rev_on;
	const char *rev_off;
	char *histfile = NULL;
	char *line = NULL;
	fy_generic v;
	int rc = -1;

	(void)args;
	assert(ctx);

	if (cfg->prompt && *cfg->prompt) {
		v = fyai_run_model_loop(ctx, ctx->last_message);
		v = fyai_report_diag(ctx, v);
		if (fy_generic_is_invalid(v))
			goto err_out;
		ctx->last_message = v;
		rc = fyai_publish_state(ctx);
		if (rc)
			goto err_out;
	}

	histfile = fyai_history_path();

	fyai_interactive_recap(ctx);

	/* From here on ^C aborts the in-flight turn (not the session) and
	 * SIGWINCH reflows the prompt; batch mode keeps the default exit. */
	fyai_signals_install();

	fyai_session_completion_init(ctx);
	linenoiseSetCompletionCallback(fyai_session_completion);
	linenoiseSetMultiLine(1);
	linenoiseSetEditorCallback(fyai_edit_line);	/* Ctrl-G */
	/*
	 * Style the interactive prompt with the theme's reverse-card colours -
	 * the same pair the echoed user turn uses (markdown_reverse_pair) - so the
	 * input bubble follows the selected libfymd4c theme instead of a hardwired
	 * black/white card. Only when colour is on; linenoise treats the SGR as
	 * zero-width.
	 */
	prompt = cfg->prompt_marker && *cfg->prompt_marker ?
		cfg->prompt_marker : "> ";
	if (cfg->markdown && ctx->stdout_tty) {
		if (markdown_reverse_pair(cfg, &rev_on, &rev_off))
			linenoiseSetPromptStyle(rev_on);
		/* The top row and the bottom status row (both {key} templates)
		 * are installed by the banner update, which frames the input. */
		fyai_session_banner_update(ctx);

		/* Default marker aligns with the markdown "  " indent. */
		if (!(cfg->prompt_marker && *cfg->prompt_marker))
			prompt = "  > ";
	}
	linenoiseHistorySetMaxLen(1000);
	if (histfile)
		linenoiseHistoryLoad(histfile);

	for (;;) {
		errno = 0;
		line = linenoise(prompt);
		if (!line) {
			/* Ctrl-C cancels the line; Ctrl-D / EOF exits. */
			if (errno == EAGAIN)
				continue;
			break;
		}
		if (!*line) {
			linenoiseFree(line);
			line = NULL;
			continue;
		}

		/*
		 * Slash commands: /clear, /compact, /model, ... (see /help).
		 * They never reach the model; "//..." escapes to send a
		 * literal slash-prefixed prompt line.
		 */
		if (line[0] == '/' && line[1] != '/') {
			linenoiseHistoryAdd(line);
			if (histfile)
				linenoiseHistorySave(histfile);
			fyai_echo_user_turn(ctx, line);
			rc = fyai_session_slash(ctx, line);
			if (cfg->markdown && ctx->stdout_tty)
				putchar('\n');
			linenoiseFree(line);
			line = NULL;
			if (rc > 0)
				break;
			continue;
		}
		if (line[0] == '/' && line[1] == '/')
			memmove(line, line + 1, strlen(line));

		linenoiseHistoryAdd(line);
		if (histfile)
			linenoiseHistorySave(histfile);
		fyai_echo_user_turn(ctx, line);

		rc = fyai_setup_transient_builder(ctx);
		assert(!rc);

		v = fyai_turn_append(ctx, ctx->last_message, fy_sequence(fyai_make_user_message(ctx, line)));
		linenoiseFree(line);
		line = NULL;
		if (fy_generic_is_invalid(v)) {
			fyai_error(ctx, "could not append the user turn");
			fyai_diag_drain(&cfg->diag);
			fyai_cleanup_transient_builder(ctx);
			continue;
		}

		v = fyai_run_model_loop(ctx, v);
		/* A failed/interrupted run may carry a diagnostic; print it and
		 * unwrap to the (possibly partial) turn. */
		v = fyai_report_diag(ctx, v);
		if (fy_generic_is_invalid(v)) {
			/* nothing completed: keep the prior state, stay in the
			 * loop so the user can retry or exit */
			fyai_diag_drain(&cfg->diag);
			fyai_cleanup_transient_builder(ctx);
			continue;
		}
		/* Interrupted mid-turn: commit the steps that completed. */
		ctx->last_message = v;
		if (fyai_publish_state(ctx))
			goto err_out;

		fyai_cleanup_transient_builder(ctx);

		/*
		 * The turn is over and the render is done: report now, before
		 * the banner repaints the footer under it.
		 */
		fyai_diag_drain(&cfg->diag);

		/* Usage moved; refresh the context fill in the footer. */
		fyai_session_banner_update(ctx);

		/* Blank line between the reply and the next prompt. */
		if (cfg->markdown && ctx->stdout_tty)
			fputc('\n', stdout);
	}

	rc = 0;

out:
	free(line);
	free(histfile);
	return rc;

err_out:
	rc = -1;
	goto out;
}

int fyai_prompt(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg = ctx->cfg;
	struct fyai_prompt_args *args = &cfg->cmd.args.prompt;
	int rc;

	(void)args;
	assert(ctx);

	if ((!cfg->api_key || !*cfg->api_key) &&
	    !cfg->chatgpt_auth && !cfg->no_auth) {
		fyai_error(ctx, "no API key or ChatGPT login is available");
		return -1;
	}

	rc = !cfg->interactive ?
		fyai_prompt_batch(ctx) :
		fyai_prompt_interactive(ctx);
	if (rc)
		return rc;

	if (cfg->stats)
		fyai_print_usage_stats(ctx);
	return 0;
}

int fyai_execute(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg;
	const struct fyai_verb *v;
	bool cleanup_transient;
	int rc;

	assert(ctx);
	cfg = ctx->cfg;
	assert(cfg);

	v = fyai_cfg_verb(cfg);
	if (!v) {
		fyai_error(ctx, "no verb selected");
		return EXIT_FAILURE;
	}

	cleanup_transient = false;
	if ((v->flags & FYAIVF_NEEDS_TRANSIENT_BUILDER) && !ctx->transient_gb) {
		rc = fyai_setup_transient_builder(ctx);
		if (rc)
			return EXIT_FAILURE;
		cleanup_transient = true;
	}

	rc = v->execute ? v->execute(ctx) : 0;
	if (cleanup_transient)
		fyai_cleanup_transient_builder(ctx);

	/* The verb reported why; printing where it was noticed only buried it. */
	return rc ? EXIT_FAILURE : EXIT_SUCCESS;
}

const char *fyai_api_to_string(enum fyai_api_mode api)
{
	static const char * const apis[] = {
		[FYAI_API_RESPONSES]		= "responses",
		[FYAI_API_CHAT_COMPLETIONS]	= "chat-completions",
		[FYAI_API_MESSAGES]		= "messages",
	};

	if ((unsigned int)api >= ARRAY_SIZE(apis))
		return NULL;
	return apis[(unsigned int)api];
}
