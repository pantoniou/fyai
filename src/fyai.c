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

#include <fcntl.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "fyai.h"
#include "fyai_agent.h"
#include "fyai_catalog.h"
#include "fyai_config.h"
#include "fyai_curl.h"
#include "fyai_event.h"
#include "fyai_display.h"
#include "fyai_log.h"
#include "fyai_markdown.h"
#include "fyai_model.h"
#include "fyai_provider.h"
#include "fyai_output.h"
#include "fyai_sink.h"
#include "fyai_session.h"
#include "fyai_prof.h"
#include "fyai_ui.h"
#include "fyai_tools.h"
#include "fyai_storage.h"
#include "fyai_stream.h"
#include "fyai_terminal.h"
#include "fyai_terminal_session.h"
#include "fyai_tools.h"
#include "fyai_tool_spec.h"
#include "fyai_turn.h"

static enum fyai_event_action fyai_signal_cb(const struct fyai_event *ev)
{
	struct fyai_ctx *ctx = ev->userdata;

	if (ev->signo == SIGPIPE)
		return FYAIEA_CONTINUE;
	ctx->interrupt_pending = true;
	ctx->terminate_pending = true;
	fyai_ui_signal(ctx, ev->signo);
	return FYAIEA_CONTINUE;
}

/* Maximum time to acknowledge SIGINT. */
#define FYAI_INTERRUPT_WATCHDOG_MS 200

static int fyai_signals_open(struct fyai_ctx *ctx)
{
	/* The watchdog handles SIGINT. */
	static const int signals[] = {
		SIGTERM, SIGHUP, SIGQUIT, SIGPIPE,
	};
	struct fyai_event_loop *el = fyai_ctx_loop(ctx);
	size_t i;
	int rc;

	if (!el) return -1;
	if (sigprocmask(SIG_SETMASK, NULL, &ctx->signal_mask))
		return -1;
	ctx->signal_mask_valid = true;
	for (i = 0; i < ARRAY_SIZE(signals); i++)
		if (fyai_event_add_signal(el, signals[i], fyai_signal_cb, ctx,
					  &ctx->signal_src[i]))
			return -1;

	rc = fyai_event_interrupt_open(ctx, FYAI_INTERRUPT_WATCHDOG_MS);
	if (rc)
		return -1;

	/* Keep a direct diagnostic output when the UI event loop stops. */
	ctx->dump_fd = dup(STDERR_FILENO);
	if (ctx->dump_fd >= 0) {
		(void)fcntl(ctx->dump_fd, F_SETFD, FD_CLOEXEC);
		(void)fyai_event_dump_open(ctx, SIGUSR2, ctx->dump_fd);
	}
	return 0;
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

	if (fyai_agent_delegated(ctx))
		return;
	usage = fyai_extract_usage(ctx, doc);
	input_tokens = fy_get(usage, "input", 0LL);
	cached_tokens = fy_get(usage, "cached", 0LL);
	output_tokens = fy_get(usage, "output", 0LL);
	total_tokens = fy_get(usage, "total", 0LL);

	if (input_tokens)
		ratio = (double)cached_tokens * 100.0 / (double)input_tokens;

	(void)fyai_result(ctx, "cache-info: input=%lld cached=%lld "
			  "cached_ratio=%.1f%% output=%lld total=%lld\n",
			  input_tokens, cached_tokens, ratio, output_tokens,
			  total_tokens);
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
	if (fy_is_invalid(usage))
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

	(void)fyai_report(ctx, "stats: calls=%d input=%lld cached=%lld (%.1f%%)",
			  ctx->usage_calls, ctx->usage_input,
			  ctx->usage_cached, ratio);
	if (ctx->usage_cache_write)
		(void)fyai_report(ctx, " cache_write=%lld",
				  ctx->usage_cache_write);
	(void)fyai_report(ctx, " output=%lld", ctx->usage_output);
	if (ctx->usage_reasoning)
		(void)fyai_report(ctx, " reasoning=%lld", ctx->usage_reasoning);
	(void)fyai_report(ctx, " total=%lld", ctx->usage_total);
	if (ctx->usage_cost > 0.0)
		(void)fyai_report(ctx, " cost=$%.6f", ctx->usage_cost);
	(void)fyai_report(ctx, "\n");
}

static bool fyai_is_tool_marked(const char *name)
{
	return fy_any_equal(name, "shell", "agent", "read_file", "write_file",
			    "apply_patch");
}

static fy_generic fyai_finish_tool_call(struct fyai_ctx *ctx, fy_generic turn,
					fy_generic tool_call,
					fy_generic tool_result, bool tool_ok,
					bool execute)
{
	struct fyai_cfg *cfg = ctx->cfg;
	fy_generic tool_message;
	fy_generic max_output_length;
	fy_generic out;
	fy_generic tool_call_type;
	const char *tool_call_id, *tool_call_output_type;
	const char *name;
	char *cause;
	bool shell;
	bool agent;
	bool banded;
	bool marked;
	bool isolated_tool;
	int rc;

	assert(ctx->transient_gb);

	tool_call_type = fy_get(tool_call, "type");
	if (fy_equal(tool_call_type, "shell_call"))
		name = "shell";
	else if (cfg->api_mode == FYAI_API_CHAT_COMPLETIONS)
		name = fy_get(fy_get(tool_call, "function"), "name", "");
	else
		name = fy_get(tool_call, "name", "");
	shell = fy_equal(name, "shell");
	agent = fy_equal(name, "agent");
	/* Use work bands only in the terminal UI. */
	banded = (shell || agent) && fyai_sink_bands_available(ctx->sink);
	marked = fyai_is_tool_marked(name);
	isolated_tool = fyai_output_renders_live(ctx);

	/*
	 * Providers do not have to terminate assistant prose with a newline, but
	 * the invocation must start on its own row in both the live display and
	 * durable output.
	 */
	rc = fyai_output_start_block(ctx);
	fyai_error_check(ctx, !rc, err,
		"could not separate tool call from assistant output");
	/*
	 * Checkpoint the assistant tail before opening the temporary bounded
	 * shell work-band. The committed band is the live representation of the
	 * tool-result fragment; after it closes, the same Markdown is appended
	 * silently to the durable document and assistant rendering resumes.
	 */
	rc = isolated_tool ? fyai_output_checkpoint(ctx) : 0;
	fyai_error_check(ctx, !rc, err,
		"could not checkpoint output before tool call");
	if (fyai_agent_delegated(ctx) || !cfg->markdown || banded ||
	    (marked && fyai_sink_bands_available(ctx->sink)))
		fyai_print_tool_call(ctx, tool_call);
	if (cfg->debug)
		emit_generic_to_stdout(ctx, "tool-call", tool_call, cfg->pretty);

	if (execute)
		tool_result = fyai_execute_tool_call(ctx, tool_call, &tool_ok);
	if (marked && fyai_sink_bands_available(ctx->sink)) {
		/* The patch resolves while it runs; show what it resolved. */
		fyai_patch_band_refresh(ctx, tool_call);
		cause = tool_ok ? NULL : fyai_tool_error_cause(tool_result);
		fyai_sink_band_close(ctx->sink, tool_ok, cause);
		free(cause);
		if (agent)
			(void)fyai_ui_commit(ctx, "\n", 1);
	}
	if (isolated_tool) {
		/* Store the result in the transcript path. */
		if (marked && fyai_sink_bands_available(ctx->sink))
			fyai_render_tool_result_exchange(ctx, tool_call,
						 tool_result);
		else if (!banded)
			fyai_render_tool_exchange(ctx, tool_call, tool_result,
					  tool_ok);
	}
	rc = fyai_record_tool_exchange(ctx, tool_call, tool_result, tool_ok);
	fyai_error_check(ctx, !rc, err_resume,
		"could not record tool display output");
	rc = isolated_tool ? fyai_output_resume(ctx) : 0;
	fyai_error_check(ctx, !rc, err,
		"could not resume output after tool call");

	if (!isolated_tool && cfg->markdown && !banded)
		fyai_render_tool_exchange(ctx, tool_call, tool_result,
					  tool_ok);
	if (cfg->debug)
		emit_generic_to_stdout(ctx, "tool-result", tool_result,
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
			if (!fy_is_valid(max_output_length))
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
	fyai_error_check(ctx, fy_is_valid(out), err,
			 "could not append the tool result");
	return out;
err_resume:
	if (isolated_tool)
		(void)fyai_output_resume(ctx);
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
	if (fy_is_invalid(gi.diag))
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
	if (fy_is_valid(diag) && !fy_is_null(diag)) {
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
	struct fyai_ctx *ctx;
	bool enabled;
	bool visible;
	struct timespec last;
	unsigned int frame;
};

static void fyai_spinner_erase(struct fyai_spinner *sp)
{
	if (!sp->visible)
		return;
	(void)fyai_report(sp->ctx, "\r\033[K");
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
	if (fyai_interrupt_pending(sp->ctx)) {
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

	(void)fyai_report(sp->ctx, "\r%s",
			  frames[sp->frame++ % ARRAY_SIZE(frames)]);
	sp->visible = true;
	return 0;
}

struct fyai_stream_tool_sink {
	struct fyai_ctx *ctx;
	struct fyai_tool_job_group *group;
	bool separated;
};

struct fyai_model_step {
	struct fyai_ctx *ctx;
	fy_generic turn;
	fy_generic previous;
	struct fyai_tool_job_group *tool_group;
	fyai_model_step_complete_fn complete;
	void *userdata;
	fyai_stream_request *stream_request;
	struct fyai_buffered_request *buffered_request;
	struct fyai_spinner spinner;
	struct fyai_stream_tool_sink tool_sink;
	struct timespec send_started;
	fy_generic result;
	enum fyai_model_step_state state;
	bool want_extents_lp;
	bool response_linked;
	bool notified;
	bool cancel_requested;
};

static bool fyai_model_step_terminal(enum fyai_model_step_state state)
{
	return state == FYAIMSS_COMPLETED ||
	       state == FYAIMSS_CANCELLED ||
	       state == FYAIMSS_FAILED;
}

static const char *
fyai_model_step_state_name(enum fyai_model_step_state state)
{
	switch (state) {
	case FYAIMSS_NEW:
		return "new";
	case FYAIMSS_BUILDING:
		return "building";
	case FYAIMSS_REQUEST_PENDING:
		return "request-pending";
	case FYAIMSS_RETRYING:
		return "retrying";
	case FYAIMSS_COMPLETED:
		return "completed";
	case FYAIMSS_CANCELLED:
		return "cancelled";
	case FYAIMSS_FAILED:
		return "failed";
	}
	return "unknown";
}

static bool
fyai_model_step_transition_valid(enum fyai_model_step_state from,
				 enum fyai_model_step_state to)
{
	switch (from) {
	case FYAIMSS_NEW:
		return to == FYAIMSS_BUILDING || to == FYAIMSS_FAILED;
	case FYAIMSS_BUILDING:
	case FYAIMSS_RETRYING:
		return to == FYAIMSS_REQUEST_PENDING ||
		       to == FYAIMSS_FAILED;
	case FYAIMSS_REQUEST_PENDING:
		return to == FYAIMSS_RETRYING ||
		       to == FYAIMSS_COMPLETED ||
		       to == FYAIMSS_CANCELLED ||
		       to == FYAIMSS_FAILED;
	case FYAIMSS_COMPLETED:
	case FYAIMSS_CANCELLED:
	case FYAIMSS_FAILED:
		return false;
	}
	return false;
}

static int
fyai_model_step_transition(struct fyai_model_step *step,
			   enum fyai_model_step_state state)
{
	if (!fyai_model_step_transition_valid(step->state, state)) {
		fyai_error(step->ctx,
			   "invalid model step transition %s -> %s",
			   fyai_model_step_state_name(step->state),
			   fyai_model_step_state_name(state));
		if (!fyai_model_step_terminal(step->state))
			step->state = FYAIMSS_FAILED;
		return -1;
	}
	if (step->ctx->cfg->debug)
		fyai_debug(step->ctx, "model step state %s -> %s",
			   fyai_model_step_state_name(step->state),
			   fyai_model_step_state_name(state));
	step->state = state;
	return 0;
}

static void fyai_stream_tool_ready(fyai_stream_request *request,
				   fy_generic tool_call, size_t index,
				   void *userdata)
{
	struct fyai_model_step *step;
	struct fyai_stream_tool_sink *sink;
	bool checkpointed;
	int rc;
	int resume_rc;

	(void)index;
	step = userdata;
	sink = &step->tool_sink;
	checkpointed = false;
	rc = 0;
	if (!sink->separated) {
		rc = fyai_output_start_block(sink->ctx);
		if (!rc && fyai_output_renders_live(sink->ctx)) {
			rc = fyai_output_checkpoint(sink->ctx);
			checkpointed = !rc;
		}
		if (!rc)
			sink->separated = true;
	}
	if (!rc)
		rc = fyai_tool_job_group_add(sink->group, tool_call);
	if (checkpointed) {
		resume_rc = fyai_output_resume(sink->ctx);
		if (!rc)
			rc = resume_rc;
	}
	if (rc < 0)
		fyai_stream_request_cancel(request);
}

static void fyai_model_step_stream_complete(fyai_stream_request *request,
					    void *userdata);
static void fyai_model_step_buffered_complete(
		struct fyai_buffered_request *request, void *userdata);
static void fyai_model_step_request_complete(struct fyai_model_step *step,
					     fy_generic response_doc);

static fy_generic fyai_model_step_add_reasoning(struct fy_generic_builder *gb,
						struct fyai_cfg *cfg,
						fy_generic request)
{
	fy_generic reasoning;

	if ((!cfg->reasoning_effort || !*cfg->reasoning_effort) &&
	    (!cfg->reasoning_summary || !*cfg->reasoning_summary))
		return request;

	switch (cfg->api_mode) {
	case FYAI_API_RESPONSES:
		reasoning = fy_map_empty;
		if (cfg->reasoning_effort && *cfg->reasoning_effort)
			reasoning = fy_assoc(gb, reasoning, "effort",
					     cfg->reasoning_effort);
		if (cfg->reasoning_summary && *cfg->reasoning_summary)
			reasoning = fy_assoc(gb, reasoning, "summary",
					     cfg->reasoning_summary);
		return fy_assoc(gb, request, "reasoning", reasoning);

	case FYAI_API_CHAT_COMPLETIONS:
		if (cfg->reasoning_effort && *cfg->reasoning_effort)
			request = fy_assoc(gb, request, "reasoning_effort",
					   cfg->reasoning_effort);
		return request;

	case FYAI_API_MESSAGES:
		return request;

	default:
		assert(0);
		__builtin_unreachable();
	}
}

/* Serialize and submit the fully-built request. */
static int fyai_model_step_submit_request(struct fyai_model_step *step,
						  fy_generic request,
						  bool response_linked,
						  bool want_extents_lp,
						  const struct timespec *t_emit)
{
	struct fyai_ctx *ctx;
	struct fyai_cfg *cfg;
	const char *request_body;
	struct fyai_stream_callbacks callbacks;
	int rc;

	ctx = step->ctx;
	cfg = ctx->cfg;
	request_body = emit_request_body(ctx->transient_gb, request);
	fyai_error_check(ctx, request_body, out,
			 "could not serialize the model request");

	/* Build + serialize done: this is "time to emit the request". */
	fyai_prof_since("request_emit", t_emit);

	curl_easy_setopt(ctx->curl, CURLOPT_POSTFIELDS, request_body);

	step->spinner.enabled = !fyai_ui_active(ctx) &&
				terminal_is_tty(STDERR_FILENO);
	curl_easy_setopt(ctx->curl, CURLOPT_XFERINFOFUNCTION,
			 fyai_spinner_xferinfo);
	curl_easy_setopt(ctx->curl, CURLOPT_XFERINFODATA, &step->spinner);
	/* Always on: the callback also polls the ^C abort flag. */
	curl_easy_setopt(ctx->curl, CURLOPT_NOPROGRESS, 0L);

	/* Everything before the wire transfer, relative to process start. */
	fyai_prof_once_from_base("start_to_first_send");
	fyai_prof_stamp(&step->send_started);

	ctx->auth_retry_done = false;
	ctx->response_chain_linked = response_linked;
	ctx->response_chain_miss = false;
	step->response_linked = response_linked;
	step->want_extents_lp = want_extents_lp;
	rc = fyai_model_step_transition(step, FYAIMSS_REQUEST_PENDING);
	fyai_error_check(ctx, !rc, out,
			 "could not advance model step");
	if (cfg->stream) {
		memset(&callbacks, 0, sizeof(callbacks));
		callbacks.complete = fyai_model_step_stream_complete;
		callbacks.tool = step->tool_group ?
					fyai_stream_tool_ready : NULL;
		callbacks.userdata = step;
		step->stream_request =
			fyai_stream_request_submit(ctx, &callbacks);
		fyai_error_check(ctx, step->stream_request, out,
				 "could not submit streaming model request");
	} else {
		step->buffered_request = fyai_buffered_request_submit(ctx,
				fyai_model_step_buffered_complete, step);
		fyai_error_check(ctx, step->buffered_request, out,
				 "could not submit buffered model request");
	}
	return 0;

out:
	return -1;
}

/* Use the context limit when it reduces the configured allowance. */
static long long fyai_model_step_max_tokens(struct fyai_ctx *ctx,
					    struct fyai_cfg *cfg)
{
	return ctx->context_max_tokens > 0 ?
	       ctx->context_max_tokens : cfg->max_tokens;
}

static fy_generic
fyai_model_step_request_base(struct fyai_cfg *cfg, struct fyai_ctx *ctx,
			     fy_generic messages, const char *instructions)
{
	struct fy_generic_builder *gb = ctx->transient_gb;

	switch (cfg->api_mode) {
	case FYAI_API_RESPONSES:
		return fy_mapping(gb,
			"model", cfg->model,
			"instructions", instructions,
			"input", fyai_responses_input(ctx, messages),
			"store", cfg->response_chain);
	case FYAI_API_CHAT_COMPLETIONS:
		return fy_mapping(gb,
			"model", cfg->model,
			"messages", fyai_chat_input(ctx, messages));
	case FYAI_API_MESSAGES:
		/* Anthropic only caches on explicit cache_control breakpoints. */
		return fy_mapping(gb,
			"model", cfg->model,
			"max_tokens", fyai_model_step_max_tokens(ctx, cfg),
			"system", fy_sequence(gb,
					fy_mapping(gb, "type", "text",
						   "text", instructions,
						   "cache_control", fy_mapping(gb, "type", "ephemeral"))),
			"messages", fyai_messages_input(ctx, messages));
	default:
		assert(0);
		__builtin_unreachable();
	}
}

static fy_generic
fyai_model_step_add_model_options(struct fyai_ctx *ctx, struct fyai_cfg *cfg,
				   fy_generic request, fy_generic cat_model,
				   fy_generic previous_response_id)
{
	struct fy_generic_builder *gb = ctx->transient_gb;
	bool response_linked;

	/* Reasoning models reject an explicit temperature. */
	if (fyai_model_supports_temperature(cat_model) &&
	    (!cfg->reasoning_effort || !*cfg->reasoning_effort) &&
	    (!cfg->reasoning_summary || !*cfg->reasoning_summary) &&
	    cfg->api_mode != FYAI_API_MESSAGES)
		request = fy_assoc(gb, request, "temperature", cfg->temperature);

	response_linked = cfg->api_mode == FYAI_API_RESPONSES &&
		cfg->response_chain &&
		fy_is_valid(previous_response_id) &&
		!fy_is_null(previous_response_id);
	if (response_linked)
		request = fy_assoc(gb, request, "previous_response_id",
				   previous_response_id);

	return request;
}

static fy_generic
fyai_model_step_add_tools(struct fyai_ctx *ctx, struct fyai_cfg *cfg,
				  fy_generic request)
{
	struct fy_generic_builder *gb = ctx->transient_gb;
	fy_generic tool_choice;

	if (!cfg->enable_tools && !cfg->enable_builtin_shell &&
	    !cfg->mcp_enabled)
		return request;

	/* Messages spells tool_choice as an object, not a string. */
	tool_choice = cfg->api_mode == FYAI_API_MESSAGES ?
			fy_mapping(gb, "type", "auto", "disable_parallel_tool_use",
				   cfg->parallel_tool_calls ? fy_false : fy_true) :
			fy_value(gb, "auto");
	request = fy_assoc(gb, request, "tools", ctx->tools,
				  "tool_choice", tool_choice);
	if (cfg->parallel_tool_calls && cfg->api_mode != FYAI_API_MESSAGES)
		request = fy_assoc(gb, request, "parallel_tool_calls", true);
	return request;
}

static fy_generic
fyai_model_step_add_logprobs(struct fyai_ctx *ctx, struct fyai_cfg *cfg,
				     fy_generic request, fy_generic cat_model,
				     bool *want_extents_lp)
{
	struct fy_generic_builder *gb = ctx->transient_gb;

	*want_extents_lp = cfg->token_extents && cfg->stream &&
		!ctx->token_extents_off && cfg->api_mode != FYAI_API_MESSAGES &&
		(!cfg->reasoning_effort || !*cfg->reasoning_effort) &&
		(!cfg->reasoning_summary || !*cfg->reasoning_summary) &&
		fyai_model_supports_logprobs(cat_model);

	if (!cfg->logprobs && !*want_extents_lp)
		return request;

	switch (cfg->api_mode) {
	case FYAI_API_CHAT_COMPLETIONS:
		request = fy_assoc(gb, request, "logprobs", true);
		break;
	case FYAI_API_RESPONSES:
		if (*want_extents_lp)
			request = fy_assoc(gb, request, "top_logprobs", 0,
					  "include", fy_sequence(gb, fy_string(
						  "message.output_text.logprobs")));
		break;
	default:
		break;
	}
	return request;
}

static fy_generic
fyai_model_step_add_stream_options(struct fyai_ctx *ctx, struct fyai_cfg *cfg,
				   fy_generic request)
{
	struct fy_generic_builder *gb = ctx->transient_gb;
	fy_generic stream_options;

	if (!cfg->stream)
		return request;

	switch (cfg->api_mode) {
	case FYAI_API_RESPONSES:
	case FYAI_API_MESSAGES:
		stream_options = fy_map_empty;
		break;
	case FYAI_API_CHAT_COMPLETIONS:
		stream_options = fy_mapping(gb, "include_usage", true);
		break;
	default:
		assert(0);
		__builtin_unreachable();
	}
	if (cfg->no_obfuscation && cfg->api_mode != FYAI_API_MESSAGES)
		stream_options = fy_assoc(gb, stream_options,
					  "include_obfuscation", false);
	request = fy_assoc(gb, request, "stream", true);
	if (fy_len(stream_options))
		request = fy_assoc(gb, request, "stream_options", stream_options);
	return request;
}

/* Reject full prompts and limit output to the remaining context space. */
static int fyai_model_step_context_check(struct fyai_ctx *ctx, fy_generic turn)
{
	struct fyai_context_prompt p;
	long long window;

	ctx->context_max_tokens = 0;
	if (ctx->compacting)
		return 0;
	window = fyai_context_window(ctx);
	if (window <= 0)
		return 0;

	fyai_context_prompt_at(ctx, turn, &p);
	if (p.prompt >= window) {
		/* Explain whether the rejected size is estimated. */
		fyai_error(ctx,
			   "the conversation needs %s%lld prompt tokens but the "
			   "%s context window holds %lld; compact it to "
			   "summarize the history, or clear it to start again",
			   p.from_estimate ? "about " : "",
			   p.prompt, ctx->cfg->model ? ctx->cfg->model : "model",
			   window);
		return -1;
	}

	ctx->context_max_tokens = fyai_context_output_tokens(ctx, p.prompt,
							     window);
	return 0;
}

static int fyai_model_step_start(struct fyai_model_step *step)
{
	struct fyai_ctx *ctx;
	struct fyai_cfg *cfg;
	fy_generic turn;
	fy_generic previous;
	const char *instructions;
	fy_generic previous_response_id;
	fy_generic catalog;
	fy_generic cat_model;
	fy_generic request;
	fy_generic messages;
	fy_generic m;
	fy_generic v;
	struct timespec t_emit;
	bool want_extents_lp;
	bool response_linked;
	int rc;

	ctx = step->ctx;
	cfg = ctx->cfg;
	turn = step->turn;
	previous = step->previous;
	step->spinner.ctx = ctx;
	step->tool_sink.ctx = ctx;
	step->tool_sink.group = step->tool_group;

	rc = fyai_model_step_context_check(ctx, turn);
	fyai_error_check(ctx, !rc, out,
			 "could not fit the request in the context window");

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

	v = fyai_model_step_request_base(cfg, ctx, messages, instructions);

	fyai_error_check(ctx, fy_is_valid(v), out,
			 "could not build the model request");
	request = v;

	catalog = fyai_catalog_effective(cfg->catalog, cfg->gb);
	cat_model = fyai_catalog_resolved_model(catalog, cfg->model);
	request = fyai_model_step_add_model_options(ctx, cfg, request, cat_model,
						     previous_response_id);

	request = fyai_model_step_add_reasoning(ctx->transient_gb, cfg,
						request);

	request = fyai_model_step_add_tools(ctx, cfg, request);
	/*
	 * token_extents wants per-token delimitation, which only logprobs
	 * provide. Gate on the catalogue (reasoning models reject the params)
	 * and on the session fail-soft latch; the Messages API has no
	 * logprobs at all. Gated-off calls still record chunk extents from
	 * the stream.
	 */
	request = fyai_model_step_add_logprobs(ctx, cfg, request, cat_model,
						      &want_extents_lp);

	if (cfg->top_logprobs >= 0)
		request = fy_assoc(ctx->transient_gb, request, "top_logprobs",
				   cfg->top_logprobs);
	request = fyai_model_step_add_stream_options(ctx, cfg, request);
	fyai_error_check(ctx, fy_is_valid(request), out,
			 "could not finish the model request");

	if (cfg->debug)
		emit_generic_to_stdout(ctx, "request", request, cfg->pretty);

	if (cfg->conversation_logging) {
		(void)fyai_log_generic(ctx, "conversation",
			fy_mapping(ctx->transient_gb,
				"kind", "request",
				"api", fyai_api_to_string(cfg->api_mode),
				"url", cfg->api_url,
				"body", request));
	}

	response_linked = cfg->api_mode == FYAI_API_RESPONSES &&
		cfg->response_chain &&
		fy_is_valid(previous_response_id) &&
		!fy_is_null(previous_response_id);
	rc = fyai_model_step_submit_request(step, request, response_linked,
					    want_extents_lp, &t_emit);
	fyai_error_check(ctx, !rc, out, "could not submit the model request");
	return 0;

out:
	return -1;
}

static void fyai_model_step_notify(struct fyai_model_step *step)
{
	fyai_model_step_complete_fn complete;
	void *userdata;

	if (step->notified || !fyai_model_step_terminal(step->state))
		return;
	step->notified = true;
	complete = step->complete;
	userdata = step->userdata;
	if (complete)
		complete(step, userdata);
}

static void
fyai_model_step_finish(struct fyai_model_step *step,
		       enum fyai_model_step_state state, fy_generic result)
{
	int rc;

	step->result = result;
	rc = fyai_model_step_transition(step, state);
	if (rc)
		step->state = FYAIMSS_FAILED;
	fyai_model_step_notify(step);
}

static int fyai_model_step_retry(struct fyai_model_step *step,
				 fy_generic previous)
{
	int rc;

	step->previous = previous;
	rc = fyai_model_step_transition(step, FYAIMSS_RETRYING);
	if (rc)
		return -1;
	return fyai_model_step_start(step);
}

static void fyai_model_step_request_complete(struct fyai_model_step *step,
					     fy_generic response_doc)
{
	struct fyai_ctx *ctx;
	struct fyai_cfg *cfg;
	fy_generic diag;
	long status;
	int rc;

	ctx = step->ctx;
	cfg = ctx->cfg;
	fyai_prof_since("request_roundtrip", &step->send_started);

	/* Failed/empty responses can end the transfer with it still shown. */
	fyai_spinner_erase(&step->spinner);
	curl_easy_setopt(ctx->curl, CURLOPT_NOPROGRESS, 1L);

	if (fy_is_valid(response_doc))
		fyai_accumulate_usage(ctx,
				      fyai_extract_usage(ctx, response_doc));

	if (cfg->debug && fy_is_valid(response_doc))
		emit_generic_to_stdout(ctx, "response", response_doc, cfg->pretty);
	if (cfg->debug && cfg->cache_info &&
	    fy_is_valid(response_doc))
		fyai_print_cache_info(ctx, response_doc);

	if (fy_is_invalid(response_doc)) {
		if (step->cancel_requested) {
			fyai_model_step_finish(step, FYAIMSS_CANCELLED,
					       response_doc);
			return;
		}
		if (step->response_linked && ctx->response_chain_miss) {
			rc = fyai_model_step_retry(step, fy_null);
			if (!rc)
				return;
			fyai_model_step_finish(step, FYAIMSS_FAILED,
					       fy_invalid);
			return;
		}

		diag = fy_generic_get_diag(response_doc);
		if (step->want_extents_lp && !cfg->logprobs &&
		    (fy_is_invalid(diag) ||
		     fy_is_null(diag))) {
			status = 0;
			curl_easy_getinfo(ctx->curl, CURLINFO_RESPONSE_CODE,
					  &status);
			if (status >= 400 && status < 500) {
				ctx->token_extents_off = true;
				rc = fyai_model_step_retry(step,
							  step->previous);
				if (!rc)
					return;
			}
		}
		fyai_model_step_finish(step, FYAIMSS_FAILED, response_doc);
		return;
	}

	response_doc = fy_gb_internalize(ctx->transient_gb, response_doc);
	if (cfg->conversation_logging) {
		(void)fyai_log_generic(ctx, "conversation",
			fy_mapping(ctx->transient_gb,
				"kind", "response",
				"api", fyai_api_to_string(cfg->api_mode),
				"body", response_doc));
	}
	if (fy_is_invalid(response_doc)) {
		fyai_error(ctx, "could not retain the provider response");
		fyai_model_step_finish(step, FYAIMSS_FAILED, fy_invalid);
		return;
	}
	fyai_model_step_finish(step, FYAIMSS_COMPLETED, response_doc);
}

static void fyai_model_step_stream_complete(fyai_stream_request *request,
					    void *userdata)
{
	struct fyai_model_step *step;
	fy_generic response_doc;

	step = userdata;
	response_doc = fyai_stream_request_collect(request);
	fyai_stream_request_destroy(request);
	step->stream_request = NULL;
	fyai_model_step_request_complete(step, response_doc);
}

static void fyai_model_step_buffered_complete(
		struct fyai_buffered_request *request, void *userdata)
{
	struct fyai_model_step *step;
	fy_generic response_doc;

	step = userdata;
	response_doc = fyai_buffered_request_collect(request);
	fyai_buffered_request_destroy(request);
	step->buffered_request = NULL;
	fyai_model_step_request_complete(step, response_doc);
}

struct fyai_model_step *
fyai_model_step_submit(struct fyai_ctx *ctx, fy_generic turn,
		       fy_generic previous,
		       struct fyai_tool_job_group *tool_group,
		       fyai_model_step_complete_fn complete,
		       void *userdata)
{
	struct fyai_model_step *step;
	int rc;

	step = calloc(1, sizeof(*step));
	fyai_error_check(ctx, step, err, "out of memory");
	step->ctx = ctx;
	step->turn = turn;
	step->previous = previous;
	step->tool_group = tool_group;
	step->complete = complete;
	step->userdata = userdata;
	step->result = fy_invalid;
	step->state = FYAIMSS_NEW;
	rc = fyai_model_step_transition(step, FYAIMSS_BUILDING);
	fyai_error_check(ctx, !rc, err_free,
			 "could not start model step");
	rc = fyai_model_step_start(step);
	fyai_error_check(ctx, !rc, err_free,
			 "could not submit model step");
	return step;

err_free:
	fyai_model_step_destroy(step);
err:
	return NULL;
}

void fyai_model_step_cancel(struct fyai_model_step *step)
{
	if (!step || fyai_model_step_terminal(step->state))
		return;
	step->cancel_requested = true;
	if (step->stream_request)
		fyai_stream_request_cancel(step->stream_request);
	if (step->buffered_request)
		fyai_buffered_request_cancel(step->buffered_request);
}

enum fyai_model_step_state
fyai_model_step_state(const struct fyai_model_step *step)
{
	return step ? step->state : FYAIMSS_FAILED;
}

bool fyai_model_step_done(const struct fyai_model_step *step)
{
	return step && fyai_model_step_terminal(step->state);
}

fy_generic fyai_model_step_collect(const struct fyai_model_step *step)
{
	return fyai_model_step_done(step) ? step->result : fy_invalid;
}

void fyai_model_step_destroy(struct fyai_model_step *step)
{
	if (!step)
		return;
	if (step->stream_request)
		fyai_stream_request_destroy(step->stream_request);
	if (step->buffered_request)
		fyai_buffered_request_destroy(step->buffered_request);
	fyai_spinner_erase(&step->spinner);
	curl_easy_setopt(step->ctx->curl, CURLOPT_NOPROGRESS, 1L);
	curl_easy_setopt(step->ctx->curl, CURLOPT_XFERINFODATA, NULL);
	free(step);
}

/* Asynchronous turn state. */
enum fyai_turn_run_state {
	FYAITRS_NEW,
	FYAITRS_MODEL,
	FYAITRS_TOOLS,
	FYAITRS_DONE,
	FYAITRS_FAILED,
};

struct fyai_turn_run {
	struct fyai_ctx *ctx;
	fy_generic turn;
	fy_generic turn_in;
	fy_generic previous;
	fy_generic tool_calls;
	fy_generic result;
	struct fyai_model_step *model_step;
	struct fyai_tool_job_group *tool_group;
	enum fyai_turn_run_state state;
	size_t exclusive_index;
	int iteration;
	bool group_parallel;
	bool cancel_requested;
};

static bool fyai_turn_run_done(const struct fyai_turn_run *run)
{
	return run && (run->state == FYAITRS_DONE ||
		       run->state == FYAITRS_FAILED);
}

static const char *
fyai_turn_run_state_name(enum fyai_turn_run_state state)
{
	switch (state) {
	case FYAITRS_NEW:
		return "new";
	case FYAITRS_MODEL:
		return "model";
	case FYAITRS_TOOLS:
		return "tools";
	case FYAITRS_DONE:
		return "done";
	case FYAITRS_FAILED:
		return "failed";
	}
	return "unknown";
}

static bool
fyai_turn_run_transition_valid(enum fyai_turn_run_state from,
			       enum fyai_turn_run_state to)
{
	switch (from) {
	case FYAITRS_NEW:
		return to == FYAITRS_MODEL || to == FYAITRS_FAILED;
	case FYAITRS_MODEL:
		return to == FYAITRS_MODEL || to == FYAITRS_TOOLS ||
		       to == FYAITRS_DONE || to == FYAITRS_FAILED;
	case FYAITRS_TOOLS:
		return to == FYAITRS_MODEL || to == FYAITRS_TOOLS ||
		       to == FYAITRS_DONE || to == FYAITRS_FAILED;
	case FYAITRS_DONE:
	case FYAITRS_FAILED:
		return false;
	}
	return false;
}

static int
fyai_turn_run_transition(struct fyai_turn_run *run,
			 enum fyai_turn_run_state state)
{
	if (!fyai_turn_run_transition_valid(run->state, state)) {
		fyai_error(run->ctx, "invalid turn transition %s -> %s",
			   fyai_turn_run_state_name(run->state),
			   fyai_turn_run_state_name(state));
		if (!fyai_turn_run_done(run))
			run->state = FYAITRS_FAILED;
		return -1;
	}
	if (run->ctx->cfg->debug)
		fyai_debug(run->ctx, "turn state %s -> %s",
			   fyai_turn_run_state_name(run->state),
			   fyai_turn_run_state_name(state));
	run->state = state;
	return 0;
}

static void fyai_turn_run_service(void *userdata);

static void fyai_turn_run_model_complete(struct fyai_model_step *step,
					 void *userdata)
{
	struct fyai_turn_run *run = userdata;

	(void)step;
	(void)fyai_event_defer(fyai_ctx_loop(run->ctx),
			       fyai_turn_run_service, run);
}

static void
fyai_turn_run_tool_complete(struct fyai_tool_job_group *group, void *userdata)
{
	struct fyai_turn_run *run = userdata;

	(void)group;
	(void)fyai_event_defer(fyai_ctx_loop(run->ctx),
			       fyai_turn_run_service, run);
}

static void fyai_turn_run_drop_tool_group(struct fyai_turn_run *run)
{
	struct fyai_tool_job_group *group;

	group = run->tool_group;
	run->tool_group = NULL;
	fyai_tool_job_group_destroy(group);
}

static void fyai_turn_run_abort_output(struct fyai_turn_run *run)
{
	fyai_output_abort(run->ctx);
	run->result = fy_invalid;
	if (run->state != FYAITRS_FAILED)
		(void)fyai_turn_run_transition(run, FYAITRS_FAILED);
}

static void
fyai_turn_run_finish(struct fyai_turn_run *run, fy_generic result,
		     bool finalize)
{
	struct fyai_ctx *ctx;

	ctx = run->ctx;
	if (finalize) {
		result = fyai_output_finalize(ctx, result, false);
		result = fy_gb_internalize(ctx->gb, result);
		if (fy_is_invalid(result))
			fyai_error(ctx, "could not retain the completed turn");
	}
	run->result = result;
	if (fy_is_invalid(result)) {
		fyai_output_abort(ctx);
		(void)fyai_turn_run_transition(run, FYAITRS_FAILED);
	} else {
		(void)fyai_turn_run_transition(run, FYAITRS_DONE);
	}
}

static void
fyai_turn_run_request_failed(struct fyai_turn_run *run, fy_generic response)
{
	struct fyai_ctx *ctx;
	fy_generic diag;
	fy_generic result;
	const char *msg;

	ctx = run->ctx;
	diag = fy_generic_get_diag(response);
	msg = fy_is_valid(diag) &&
	      !fy_is_null(diag) ?
		fy_castp(&diag, "request failed") : "request failed";
	if (run->turn.v != run->turn_in.v) {
		result = fyai_output_finalize(ctx, run->turn, true);
		result = fy_gb_internalize(ctx->gb, result);
		result = fyai_with_diag(ctx->gb, result, msg);
	} else {
		fyai_output_abort(ctx);
		result = fyai_with_diag(ctx->gb, fy_invalid, msg);
	}
	run->result = result;
	(void)fyai_turn_run_transition(run,
		fy_is_invalid(result) ?
			FYAITRS_FAILED : FYAITRS_DONE);
}

static int fyai_turn_run_submit_model(struct fyai_turn_run *run)
{
	struct fyai_ctx *ctx;
	struct fyai_cfg *cfg;
	fy_generic previous;
	int rc;

	ctx = run->ctx;
	cfg = ctx->cfg;
	if (run->iteration >= cfg->max_tool_iterations) {
		fyai_turn_run_abort_output(run);
		return -1;
	}
	previous = cfg->response_chain ? run->previous :
		(run->iteration ? run->previous : fy_null);
	run->tool_group = cfg->stream ?
		fyai_tool_job_group_create_open(ctx,
			fyai_turn_run_tool_complete, run) : NULL;
	fyai_error_check(ctx, !cfg->stream || run->tool_group, err,
			 "could not create streaming tool group");
	rc = fyai_turn_run_transition(run, FYAITRS_MODEL);
	fyai_error_check(ctx, !rc, err,
			 "could not advance turn to model request");
	run->model_step = fyai_model_step_submit(ctx, run->turn, previous,
						 run->tool_group,
						 fyai_turn_run_model_complete,
						 run);
	fyai_error_check(ctx, run->model_step, err,
			 "could not submit model step");
	return 0;

err:
	fyai_turn_run_drop_tool_group(run);
	fyai_turn_run_abort_output(run);
	return -1;
}

static int
fyai_turn_run_submit_exclusive(struct fyai_turn_run *run, size_t start)
{
	struct fyai_ctx *ctx;
	fy_generic tool_call;
	size_t i;
	size_t total;
	int rc;

	ctx = run->ctx;
	total = fy_len(run->tool_calls);
	for (i = start; i < total; i++) {
		tool_call = fy_get_at(run->tool_calls, i);
		if (!fyai_tool_call_parallel_eligible(ctx, tool_call))
			break;
	}
	if (i >= total) {
		run->iteration++;
		return fyai_turn_run_submit_model(run);
	}
	run->exclusive_index = i;
	run->group_parallel = false;
	run->tool_group = fyai_tool_job_group_create_notify(ctx,
				fyai_turn_run_tool_complete, run);
	fyai_error_check(ctx, run->tool_group, err,
			 "could not create exclusive tool group");
	rc = fyai_tool_job_group_add(run->tool_group, tool_call);
	fyai_error_check(ctx, !rc, err,
			 "could not add exclusive tool call");
	rc = fyai_turn_run_transition(run, FYAITRS_TOOLS);
	fyai_error_check(ctx, !rc, err,
			 "could not advance turn to tool group");
	rc = fyai_tool_job_group_submit(run->tool_group);
	fyai_error_check(ctx, !rc, err,
			 "could not submit exclusive tool group");
	return 0;

err:
	fyai_turn_run_drop_tool_group(run);
	fyai_turn_run_abort_output(run);
	return -1;
}

static int fyai_turn_run_collect_tools(struct fyai_turn_run *run)
{
	struct fyai_ctx *ctx;
	fy_generic tool_call;
	fy_generic result;
	size_t group_index;
	size_t i;
	size_t total;
	bool ok;
	int rc;

	ctx = run->ctx;
	total = fy_len(run->tool_calls);
	group_index = 0;
	for (i = 0; i < total; i++) {
		tool_call = fy_get_at(run->tool_calls, i);
		if (run->group_parallel !=
		    fyai_tool_call_parallel_eligible(ctx, tool_call))
			continue;
		if (!run->group_parallel && i != run->exclusive_index)
			continue;
		result = fy_invalid;
		ok = false;
		rc = fyai_tool_job_group_collect(run->tool_group,
						 group_index++, &result, &ok);
		fyai_error_check(ctx, !rc, err,
				 "could not collect tool job result");
		run->turn = fyai_finish_tool_call(ctx, run->turn, tool_call,
						 result, ok, false);
		fyai_error_check(ctx, fy_is_valid(run->turn), err,
				 "could not append tool job result");
	}
	fyai_turn_run_drop_tool_group(run);
	run->turn = fy_gb_internalize(ctx->transient_gb, run->turn);
	fyai_error_check(ctx, fy_is_valid(run->turn), err,
			 "could not append the tool calls");
	if (ctx->ask_abort) {
		fyai_turn_run_finish(run, fy_null, true);
		return 0;
	}
	if (fyai_interrupt_check(ctx)) {
		result = fy_gb_internalize(ctx->gb, run->turn);
		result = fyai_with_diag(ctx->gb, result, "interrupted");
		fyai_turn_run_finish(run, result, false);
		return 0;
	}
	return run->group_parallel ?
		fyai_turn_run_submit_exclusive(run, 0) :
		fyai_turn_run_submit_exclusive(run,
					       run->exclusive_index + 1);

err:
	fyai_turn_run_drop_tool_group(run);
	fyai_turn_run_abort_output(run);
	return -1;
}

static fy_generic fyai_turn_run_take_model_response(
		struct fyai_turn_run *run)
{
	fy_generic response;

	response = fyai_model_step_collect(run->model_step);
	fyai_model_step_destroy(run->model_step);
	run->model_step = NULL;
	return response;
}

static int
fyai_turn_run_append_response(struct fyai_turn_run *run, fy_generic response)
{
	struct fyai_ctx *ctx;
	struct fyai_cfg *cfg;
	fy_generic response_id;
	fy_generic out_text;
	const char *text;

	ctx = run->ctx;
	cfg = ctx->cfg;
	response_id = fyai_response_id(ctx, response);
	if (!cfg->stream) {
		out_text = fyai_response_output_text(ctx, response);
		text = fy_castp(&out_text, "");
		(void)fyai_output_append_string(ctx, text);
		fyai_tool_progress_emit(ctx, text, strlen(text));
	}
	run->turn = fyai_append_assistant_response(ctx, run->turn, response);
	fyai_error_check(ctx, fy_is_valid(run->turn), err,
			 "could not append assistant response");
	if (cfg->response_chain) {
		run->turn = fyai_turn_set_response_id(ctx, run->turn,
						     response_id);
		fyai_error_check(ctx, fy_is_valid(run->turn), err,
				 "could not retain response id");
	}
	return 0;

err:
	return -1;
}

static size_t
fyai_turn_run_parallel_count(struct fyai_turn_run *run)
{
	fy_generic tool_call;
	size_t count;

	count = 0;
	fy_foreach(tool_call, run->tool_calls)
		if (fyai_tool_call_parallel_eligible(run->ctx, tool_call))
			count++;
	return count;
}

static int fyai_turn_run_submit_parallel(struct fyai_turn_run *run,
					 size_t parallel_total)
{
	struct fyai_ctx *ctx;
	struct fyai_cfg *cfg;
	fy_generic tool_call;
	int rc;

	ctx = run->ctx;
	cfg = ctx->cfg;
	fyai_error_check(ctx, !run->tool_group ||
			 fyai_tool_job_group_count(run->tool_group) ==
				parallel_total,
			 err, "streamed tool group does not match response");
	if (!run->tool_group) {
		run->tool_group = fyai_tool_job_group_create_notify(ctx,
					fyai_turn_run_tool_complete, run);
		fyai_error_check(ctx, run->tool_group, err,
				 "could not create parallel tool group");
		fy_foreach(tool_call, run->tool_calls) {
			if (!fyai_tool_call_parallel_eligible(ctx, tool_call))
				continue;
			rc = fyai_tool_job_group_add(run->tool_group, tool_call);
			fyai_error_check(ctx, !rc, err,
					 "could not add parallel tool call");
		}
	}
	run->group_parallel = true;
	rc = fyai_turn_run_transition(run, FYAITRS_TOOLS);
	fyai_error_check(ctx, !rc, err,
			 "could not advance turn to tool group");
	if (cfg->stream)
		rc = fyai_tool_job_group_seal(run->tool_group);
	else
		rc = fyai_tool_job_group_submit(run->tool_group);
	fyai_error_check(ctx, !rc, err,
			 "could not seal parallel tool group");
	return 0;

err:
	fyai_turn_run_drop_tool_group(run);
	fyai_turn_run_abort_output(run);
	return -1;
}

static int
fyai_turn_run_start_tools(struct fyai_turn_run *run, fy_generic response)
{
	size_t parallel_total;

	if (run->ctx->cfg->response_chain)
		run->previous = run->turn;
	run->tool_calls = fyai_response_tool_calls(run->ctx, response);
	parallel_total = fyai_turn_run_parallel_count(run);
	if (parallel_total)
		return fyai_turn_run_submit_parallel(run, parallel_total);
	fyai_turn_run_drop_tool_group(run);
	return fyai_turn_run_submit_exclusive(run, 0);
}

static int fyai_turn_run_process_model(struct fyai_turn_run *run)
{
	struct fyai_ctx *ctx;
	fy_generic response;
	int rc;

	ctx = run->ctx;
	response = fyai_turn_run_take_model_response(run);
	if (fy_is_invalid(response)) {
		fyai_turn_run_drop_tool_group(run);
		fyai_turn_run_request_failed(run, response);
		return 0;
	}
	rc = fyai_turn_run_append_response(run, response);
	if (rc) {
		fyai_turn_run_drop_tool_group(run);
		fyai_turn_run_abort_output(run);
		return -1;
	}
	if (fyai_response_is_final(ctx, response)) {
		fyai_turn_run_drop_tool_group(run);
		/* Closing the assistant document presents its accumulated text. */
		fyai_turn_run_finish(run, run->turn, true);
		return 0;
	}
	if (fyai_response_needs_tool_calls(ctx, response))
		return fyai_turn_run_start_tools(run, response);
	fyai_turn_run_drop_tool_group(run);
	run->iteration++;
	return fyai_turn_run_submit_model(run);
}

static struct fyai_turn_run *
fyai_turn_run_submit(struct fyai_ctx *ctx, fy_generic turn)
{
	struct fyai_turn_run *run;
	int rc;

	run = calloc(1, sizeof(*run));
	fyai_error_check(ctx, run, err, "out of memory");
	run->ctx = ctx;
	run->turn = turn;
	run->turn_in = turn;
	run->previous = fy_get(turn, "previous", fy_null);
	run->tool_calls = fy_null;
	run->result = fy_invalid;
	run->state = FYAITRS_NEW;
	ctx->tool_output_displayed = false;
	rc = fyai_output_begin(ctx, FYAI_OUTPUT_ASSISTANT);
	fyai_error_check(ctx, !rc, err_free,
			 "could not start assistant display output");
	fyai_interrupt_check(ctx);
	rc = fyai_turn_run_submit_model(run);
	fyai_error_check(ctx, !rc, err_free,
			 "could not submit turn");
	return run;

err_free:
	free(run);
err:
	return NULL;
}

static void fyai_turn_run_service(void *userdata)
{
	struct fyai_turn_run *run = userdata;
	bool progress;

	/*
	 * Service completed operations after event dispatch. Continue through
	 * synchronous transitions until the turn waits or stops.
	 */
	if (!run)
		return;
	do {
		progress = false;
		if (run->state == FYAITRS_MODEL &&
		    fyai_model_step_done(run->model_step)) {
			(void)fyai_turn_run_process_model(run);
			progress = true;
		} else if (run->state == FYAITRS_TOOLS &&
			   fyai_tool_job_group_done(run->tool_group)) {
			(void)fyai_turn_run_collect_tools(run);
			progress = true;
		}
	} while (progress && !fyai_turn_run_done(run));
}

static void fyai_turn_run_cancel(struct fyai_turn_run *run)
{
	if (!run || fyai_turn_run_done(run) || run->cancel_requested)
		return;
	run->cancel_requested = true;
	if (run->model_step)
		fyai_model_step_cancel(run->model_step);
	if (run->tool_group)
		fyai_tool_job_group_cancel(run->tool_group);
}

static fy_generic fyai_turn_run_collect(const struct fyai_turn_run *run)
{
	return fyai_turn_run_done(run) ? run->result : fy_invalid;
}

static void fyai_turn_run_destroy(struct fyai_turn_run *run)
{
	if (!run)
		return;
	/* Drop a deferred service still pointing at the run we are freeing. */
	fyai_event_defer_cancel(fyai_ctx_loop(run->ctx), fyai_turn_run_service,
				run);
	fyai_model_step_destroy(run->model_step);
	fyai_turn_run_drop_tool_group(run);
	free(run);
}

/* Drive an asynchronous turn from a caller that owns the top-level pump. */
fy_generic fyai_run_turn(struct fyai_ctx *ctx, fy_generic turn)
{
	struct fyai_turn_run *run;
	struct fyai_event_loop *el;
	fy_generic result, diag;
	int rc;

	result = fy_invalid;
	run = fyai_turn_run_submit(ctx, turn);
	if (!run)
		return fy_invalid;
	el = fyai_ctx_loop(ctx);
	assert(el);
	while (!fyai_turn_run_done(run)) {
		if (ctx->interrupt_pending) {
			fyai_event_interrupt_ack(ctx);
			fyai_turn_run_cancel(run);
		}
		rc = fyai_event_loop_step(el, -1);
		fyai_error_check(ctx, rc >= 0, done,
				 "the event loop stopped while the turn was running");
	}

done:
	result = fyai_turn_run_collect(run);
	/* Add a cause if the result and the sink do not have one. */
	diag = fy_generic_get_diag(result);
	if (fy_is_invalid(result) && (!fy_is_valid(diag) || fy_is_null(diag)) &&
	    !fyai_diag_got_error(&ctx->cfg->diag))
		fyai_error(ctx, "the turn did not complete");
	fyai_turn_run_destroy(run);
	return result;
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
	ctx->transient_autorelease = false;
}

struct fy_generic_builder *fyai_ctx_transient_gb(struct fyai_ctx *ctx)
{
	if (ctx->transient_gb)
		return ctx->transient_gb;
	if (fyai_setup_transient_builder(ctx))
		return NULL;
	ctx->transient_autorelease = true;
	return ctx->transient_gb;
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

void fyai_cleanup(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg;

	if (!ctx)
		return;
	cfg = ctx->cfg;

	/* Remove the handlers before the context that they name is destroyed. */
	fyai_terminal_winch_close(ctx);
	fyai_event_interrupt_close(ctx);
	fyai_cleanup_transient_builder(ctx);
	fyai_output_cleanup(ctx);
	/* After the output document: its discard still talks to the sink. */
	fyai_sink_destroy(ctx->sink);
	ctx->sink = NULL;

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
	if (ctx->branch) {
		free(ctx->branch);
		ctx->branch = NULL;
	}
	if (ctx->head_branch) {
		free(ctx->head_branch);
		ctx->head_branch = NULL;
	}
	free(ctx->agent_branch);
	ctx->agent_branch = NULL;
	free(ctx->tool_submit_error);
	ctx->tool_submit_error = NULL;
	fyai_patch_display_clear(ctx);
	free(ctx->patch_display);
	ctx->patch_display = NULL;
	if (ctx->shell_stream) {
		fyai_fenced_stream_finish(ctx->shell_stream);
		free(ctx->shell_stream);
		ctx->shell_stream = NULL;
	}
	fyai_mcp_cleanup(ctx);
	fyai_ui_close(ctx);

	/* Before the easy handle: the multi still references it. */
	fyai_curl_cleanup(ctx);
	if (ctx->curl) {
		curl_easy_cleanup(ctx->curl);
		ctx->curl = NULL;
	}
	fyai_auth_cleanup(ctx);

	/* Destroy the loop after every borrower has dropped its sources. */
	if (ctx->el) {
		fyai_event_loop_destroy(ctx->el);
		ctx->el = NULL;
	}

	fyai_event_pool_drain(ctx);

	if (!fyai_cfg_no_storage(cfg))
		fyai_close_storage(ctx);
}

/* Apply the configured MCP state. This operation is idempotent. */
static int fyai_mcp_bringup(struct fyai_ctx *ctx)
{
	if (ctx->cfg->mcp_enabled)
		return fyai_mcp_refresh(ctx);
	fyai_mcp_cleanup(ctx);
	ctx->mcp_tools = fy_invalid;
	return 0;
}

/* Rebuild the curl handle and request state from the current configuration. */
int fyai_curl_easy_reinit(struct fyai_ctx *ctx)
{
	ctx->curl = curl_easy_init();
	if (!ctx->curl)
		return -1;

	/* Reserve SIGALRM for the watchdog. */
	curl_easy_setopt(ctx->curl, CURLOPT_NOSIGNAL, 1L);

	curl_easy_setopt(ctx->curl, CURLOPT_WRITEFUNCTION, write_response);
	curl_easy_setopt(ctx->curl, CURLOPT_TIMEOUT, 600L);	/* 10 minutes */
	curl_easy_setopt(ctx->curl, CURLOPT_USERAGENT, ctx->user_agent);
	curl_easy_setopt(ctx->curl, CURLOPT_VERBOSE, 1L);
	curl_easy_setopt(ctx->curl, CURLOPT_DEBUGFUNCTION, fyai_curl_debug);
	curl_easy_setopt(ctx->curl, CURLOPT_DEBUGDATA, ctx);
	return 0;
}

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

	if (cfg->enable_tools || cfg->enable_builtin_shell || cfg->mcp_enabled) {

		switch (cfg->api_mode) {
		case FYAI_API_RESPONSES:
			ctx->tools = fyai_make_responses_tools(ctx);
			break;
		case FYAI_API_CHAT_COMPLETIONS:
			ctx->tools = cfg->enable_tools || cfg->enable_builtin_shell ?
				make_tools_filtered(ctx) : fy_seq_empty;
			break;
		case FYAI_API_MESSAGES:
			ctx->tools = cfg->enable_tools || cfg->enable_builtin_shell ?
				fyai_make_messages_tools(ctx) : fy_seq_empty;
			break;
		}

		if (cfg->mcp_enabled) {
			fy_generic mt, tool, fn, converted = fy_seq_empty;

			mt = fyai_mcp_tools(ctx);
			if (cfg->api_mode == FYAI_API_CHAT_COMPLETIONS) {
				converted = mt;
			} else {
				fy_foreach(tool, mt) {
					fn = fy_get(tool, "function");
					if (cfg->api_mode == FYAI_API_RESPONSES)
						tool = fy_mapping("type", "function",
							"name", fy_get(fn, "name", ""),
							"description", fy_get(fn, "description", ""),
							"parameters", fy_get(fn, "parameters"));
					else
						tool = fy_mapping("name", fy_get(fn, "name", ""),
							"description", fy_get(fn, "description", ""),
							"input_schema", fy_get(fn, "parameters"));
					converted = fy_append(ctx->gb, converted, tool);
				}
			}
			ctx->tools = fy_concat(ctx->gb, ctx->tools, converted);
		}
	}

	if (fy_is_valid(ctx->tools)) {
		ctx->tools = fy_gb_internalize(ctx->gb, ctx->tools);
		fyai_error_check(ctx, fy_is_valid(ctx->tools), err,
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
	if (!terminal_window_size(STDOUT_FILENO, &ctx->tty_rows, &ctx->tty_cols))
		(void)terminal_window_size(STDERR_FILENO, &ctx->tty_rows,
					   &ctx->tty_cols);

	/* Before anything can render: every later step reports through it. */
	ctx->sink = fyai_sink_create(ctx);
	if (!ctx->sink)
		goto err;

	ctx->tools = fy_invalid;
	ctx->tools_spec = fy_invalid;
	ctx->last_message = fy_invalid;
	ctx->arena_config = fy_invalid;
	ctx->arena_catalog = fy_invalid;
	/* A zeroed fy_generic is an empty sequence, not fy_invalid. */
	ctx->arena_branches = fy_invalid;
	ctx->branch_prev = fy_invalid;
	ctx->branch_desc = fy_invalid;
	ctx->branch_agent = fy_invalid;
	ctx->last_token_extents = fy_invalid;
	if (fyai_signals_open(ctx))
		goto err;

	if (!fyai_cfg_no_storage(cfg)) {
		if (fyai_setup_storage(ctx))
			goto err;
	}

	if (fyai_cfg_no_requests(cfg))
		return 0;

	rc = asprintf(&ctx->user_agent, "%s/%s", "fyai", VERSION);
	if (rc == -1)
		goto err;

	if (fyai_curl_easy_reinit(ctx))
		goto err;

	(void)fyai_setup_transient_builder(ctx);
	if (fyai_auth_resolve(ctx))
		goto err;

	rc = fyai_request_state_apply(ctx);
	if (rc)
		goto err;

	/* Start MCP here for non-interactive modes and apply its tools. */
	if (!cfg->interactive) {
		rc = fyai_mcp_bringup(ctx);
		fyai_error_check(ctx, !rc, err, "could not initialize MCP servers");
		rc = fyai_request_state_apply(ctx);
		if (rc)
			goto err;
	}

	/*
	 * Fold repo-scoped instruction files (AGENTS.md/CLAUDE.md) into the
	 * system prompt before it is frozen into the canonical system turn, so
	 * project guidance travels with a new conversation (continuations keep
	 * the copy they were started with). Only for a fresh conversation -
	 * an existing chain already carries its own system turn.
	 */
	if (fy_is_invalid(ctx->last_message)) {
		if (cfg->parallel_tool_calls &&
		    cfg->parallel_tool_calls_prompt &&
		    *cfg->parallel_tool_calls_prompt) {
			cfg->system_prompt = fy_gb_intern_string(ctx->gb,
				fy_sprintfa("%s%s%s",
					cfg->system_prompt ?
						cfg->system_prompt : "",
					cfg->system_prompt &&
						*cfg->system_prompt ?
						"\n\n" : "",
					cfg->parallel_tool_calls_prompt));
		}
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
	if (fy_is_invalid(ctx->last_message)) {
		ctx->last_message = fyai_turn_append(ctx, ctx->last_message,
			fy_sequence(fyai_make_system_message(ctx, cfg->system_prompt)));
		ctx->last_message = fyai_output_record(ctx, ctx->last_message,
			FYAI_OUTPUT_SYSTEM, cfg->system_prompt);
	}

	if (cfg->prompt && *cfg->prompt) {
		ctx->last_message = fyai_turn_append(ctx, ctx->last_message,
			fy_sequence(fyai_make_user_message(ctx, cfg->prompt)));
		ctx->last_message = fyai_output_record(ctx, ctx->last_message,
			FYAI_OUTPUT_USER, cfg->prompt);
	}

	/* intern all to durable */
	ctx->last_message = fy_gb_internalize(ctx->gb, ctx->last_message);
	fyai_error_check(ctx, fy_is_valid(ctx->last_message), err,
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
	v = fyai_run_turn(ctx, ctx->last_message);
	v = fyai_report_diag(ctx, v);
	if (fy_is_invalid(v))
		goto err_out;
	ctx->last_message = v;

	/*
	 * Close the assistant turn with a blank line so it does not butt up
	 * against the shell prompt. The interactive loop gets this spacing from
	 * its own prompt, so only the batch path needs it, and only on a
	 * terminal - piped/redirected output stays byte-clean for scripting.
	 */
	if (ctx->stdout_tty)
		(void)fyai_sink_write(ctx->sink, FYAI_SINK_TRANSCRIPT, "\n", 1);

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

static void fyai_interactive_finish_output(struct fyai_ctx *ctx);

static int
fyai_interactive_finish_turn(struct fyai_ctx *ctx,
			     struct fyai_turn_run **runp)
{
	struct fyai_turn_run *run;
	fy_generic result;
	int rc;

	run = *runp;
	result = fyai_turn_run_collect(run);
	fyai_turn_run_destroy(run);
	*runp = NULL;
	fyai_ui_set_busy(ctx, false);
	result = fyai_report_diag(ctx, result);
	rc = 0;
	if (fy_is_valid(result)) {
		ctx->last_message = result;
		rc = fyai_publish_state(ctx);
		fyai_error_check(ctx, !rc, out,
				 "could not publish the completed turn");
	}
out:
	fyai_interactive_finish_output(ctx);
	return rc;
}

/* Common display/session work done after an interactive turn completes. */
static void fyai_interactive_finish_output(struct fyai_ctx *ctx)
{
	fyai_cleanup_transient_builder(ctx);
	fyai_ui_diag_drain(ctx, "error");
	fyai_ui_drain_output(ctx);
	fyai_session_banner_update(ctx);
	if (ctx->cfg->markdown && ctx->stdout_tty)
		(void)fyai_sink_write(ctx->sink, FYAI_SINK_TRANSCRIPT, "\n", 1);
}

/* Prepare the common interactive surface before entering its input loop. */
static void fyai_interactive_prepare(struct fyai_ctx *ctx,
				     const char *histfile, bool banner)
{
	fyai_ui_history_load(ctx, histfile);
	fyai_interactive_recap(ctx);
	fyai_ui_drain_output(ctx);
	if (banner)
		fyai_session_banner_update(ctx);
}

/* Handle a slash command. Return -1 when @line is an ordinary user turn. */
static int fyai_interactive_handle_slash(struct fyai_ctx *ctx,
					 const char *histfile, char *line)
{
	int rc;

	if (line[0] != '/' || line[1] == '/')
		return -1;
	fyai_ui_history_save(ctx, histfile, line);
	fyai_echo_user_turn(ctx, line);
	rc = fyai_session_slash(ctx, line);
	fyai_error_check(ctx, rc >= 0, out,
			 "could not run the slash command");
	if (ctx->cfg->markdown && ctx->stdout_tty)
		(void)fyai_sink_write(ctx->sink, FYAI_SINK_TRANSCRIPT, "\n", 1);
out:
	fyai_ui_drain_output(ctx);
	return rc < 0 ? rc : rc > 0 ? 1 : 0;
}

/* Normalize and display a normal interactive user line. */
static void fyai_interactive_prepare_user_line(struct fyai_ctx *ctx,
					       const char *histfile, char *line)
{
	if (line[0] == '/' && line[1] == '/')
		memmove(line, line + 1, strlen(line));
	fyai_ui_history_save(ctx, histfile, line);
	fyai_echo_user_turn(ctx, line);
	fyai_ui_drain_output(ctx);
}

static fy_generic fyai_interactive_append_user_turn(struct fyai_ctx *ctx,
						    char *line)
{
	fy_generic turn;

	turn = fyai_turn_append(ctx, ctx->last_message,
			fy_sequence(fyai_make_user_message(ctx, line)));
	return fyai_output_record(ctx, turn, FYAI_OUTPUT_USER, line);
}

static int
fyai_interactive_start_line(struct fyai_ctx *ctx, const char *histfile,
			    char *line, struct fyai_turn_run **runp)
{
	fy_generic turn;
	int rc;

	rc = fyai_interactive_handle_slash(ctx, histfile, line);
	if (rc >= 0)
		return rc;
	fyai_interactive_prepare_user_line(ctx, histfile, line);
	rc = fyai_setup_transient_builder(ctx);
	fyai_error_check(ctx, !rc, err,
			 "could not create transient turn storage");
	turn = fyai_interactive_append_user_turn(ctx, line);
	fyai_error_check(ctx, fy_is_valid(turn), err_cleanup,
			 "could not append the user turn");
	*runp = fyai_turn_run_submit(ctx, turn);
	fyai_error_check(ctx, *runp, err_cleanup,
			 "could not submit interactive turn");
	fyai_ui_set_busy(ctx, true);
	return 0;

err_cleanup:
	fyai_cleanup_transient_builder(ctx);
err:
	return -1;
}

/* Add discovered MCP tools before the first model step. */
static int fyai_interactive_mcp_gate(struct fyai_ctx *ctx)
{
	int rc;

	if (!ctx->cfg->mcp_enabled)
		return 0;
	fyai_mcp_publish_tools(ctx);
	rc = fyai_setup_transient_builder(ctx);
	if (rc)
		return rc;
	rc = fyai_request_state_apply(ctx);
	fyai_cleanup_transient_builder(ctx);
	return rc;
}

/* Interactive application states around the event pump. */
enum fyai_app_state {
	FYAIAS_STARTING,
	FYAIAS_RUNNING,
	FYAIAS_STOPPING,
	FYAIAS_DONE,
};

enum fyai_line_result {
	FYAILR_NONE,
	FYAILR_QUIT,
	FYAILR_HANDLED,
	FYAILR_ERROR,
};

static int fyai_interactive_config_edit_step(struct fyai_ctx *ctx)
{
	int rc;

	if (!ctx->config_edit ||
	    !fyai_config_edit_done(ctx->config_edit))
		return 0;
	rc = fyai_config_edit_collect(ctx->config_edit);
	fyai_config_edit_destroy(ctx->config_edit);
	ctx->config_edit = NULL;
	fyai_error_check(ctx, !rc, err,
			 "could not collect the configuration edit");
	rc = fyai_config_rederive(ctx);
	fyai_error_check(ctx, !rc, err,
			 "could not apply the configuration edit");
	return 0;
err:
	fyai_ui_diag_drain(ctx, "config");
	return rc;
}

static int fyai_interactive_finish_run(struct fyai_ctx *ctx,
					struct fyai_turn_run **runp, bool *initial,
					bool *done)
{
	int rc;

	*done = false;
	rc = fyai_interactive_finish_turn(ctx, runp);
	if (rc)
		return rc;
	if (*initial) {
		*initial = false;
		if (fy_is_invalid(ctx->last_message))
			*done = true;
	}
	return 0;
}

static void fyai_interactive_process_interrupt(struct fyai_ctx *ctx,
						struct fyai_turn_run *run)
{
	if (!ctx->interrupt_pending)
		return;
	/* Acknowledge SIGINT while the event loop is responsive. */
	fyai_event_interrupt_ack(ctx);
	/* Process each delivered SIGINT one time. */
	if (ctx->interrupt_seq != ctx->interrupt_seen) {
		ctx->interrupt_seen = ctx->interrupt_seq;
		fyai_ui_interrupt(ctx);
	}
	if (run)
		fyai_turn_run_cancel(run);
}

static int fyai_interactive_submit_initial(struct fyai_ctx *ctx,
					   struct fyai_turn_run **runp)
{
	int rc;

	rc = fyai_setup_transient_builder(ctx);
	fyai_error_check(ctx, !rc, err,
			 "could not create transient initial storage");
	*runp = fyai_turn_run_submit(ctx, ctx->last_message);
	fyai_error_check(ctx, *runp, err_cleanup,
			 "could not submit the initial turn");
	fyai_ui_set_busy(ctx, true);
	return 0;
err_cleanup:
	fyai_cleanup_transient_builder(ctx);
err:
	return -1;
}

static enum fyai_line_result fyai_interactive_read_line(struct fyai_ctx *ctx,
					      const char *histfile,
					      struct fyai_turn_run **runp)
{
	char *line;
	int rc;

	line = fyai_ui_take_line(ctx);
	if (!line)
		return FYAILR_NONE;
	rc = fyai_interactive_start_line(ctx, histfile, line, runp);
	free(line);
	if (rc > 0)
		return FYAILR_QUIT;
	if (rc < 0) {
		fyai_ui_diag_drain(ctx, "error");
		return FYAILR_ERROR;
	}
	return FYAILR_HANDLED;
}

static int fyai_prompt_interactive_async(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg;
	struct fyai_event_loop *el;
	struct fyai_turn_run *run;
	enum fyai_app_state state;
	enum fyai_line_result line_result;
	char *histfile;
	char *line;
	bool initial;
	bool gated;
	bool quit;
	bool done;
	int rc;

	cfg = ctx->cfg;
	el = fyai_ctx_loop(ctx);
	run = NULL;
	state = FYAIAS_STARTING;
	histfile = fyai_history_path();
	line = NULL;
	initial = cfg->prompt && *cfg->prompt;
	gated = false;
	quit = false;
	done = false;
	rc = -1;
	fyai_error_check(ctx, el, out,
			 "interactive UI requires an event loop");

	/* Start all MCP servers after the UI opens. */
	fyai_interactive_prepare(ctx, histfile, true);
	if (cfg->mcp_enabled) {
		rc = fyai_mcp_start(ctx);
		fyai_error_check(ctx, !rc, out, "could not start MCP servers");
	} else {
		fyai_mcp_cleanup(ctx);
		ctx->mcp_tools = fy_invalid;
	}
	state = FYAIAS_RUNNING;

	/* No operation below this loop can start a nested event pump. */
	for (;;) {
		/* Release scratch storage that an idle operation created. */
		if (ctx->transient_autorelease)
			fyai_cleanup_transient_builder(ctx);

		rc = fyai_interactive_config_edit_step(ctx);
		fyai_error_check(ctx, !rc, out,
				 "could not finish the configuration edit");

		if (run && fyai_turn_run_done(run)) {
			rc = fyai_interactive_finish_run(ctx, &run, &initial,
						 &done);
			/* The finish path reports and drains its own cause. */
			if (rc)
				goto out;
			if (done)
				goto out;
		}
		fyai_interactive_process_interrupt(ctx, run);
		if (ctx->terminate_pending || fyai_ui_quit_requested(ctx))
			quit = true;
		if (!run && quit) {
			rc = 0;
			break;
		}

		/* Apply MCP tools before the first turn when startup_wait is set. */
		if (!run && !gated &&
		    (!cfg->mcp_enabled || !cfg->mcp_startup_wait ||
		     fyai_mcp_settled(ctx))) {
			rc = fyai_interactive_mcp_gate(ctx);
			fyai_error_check(ctx, !rc, out,
					 "could not apply MCP tools");
			gated = true;
		}

		if (!run && gated && initial) {
			rc = fyai_interactive_submit_initial(ctx, &run);
			fyai_error_check(ctx, !rc, out,
					 "could not submit initial turn");
		}

		if (!run && gated && !ctx->config_edit) {
			line_result = fyai_interactive_read_line(ctx, histfile, &run);
			if (line_result == FYAILR_QUIT) {
				quit = true;
				continue;
			}
			fyai_error_check(ctx, line_result != FYAILR_ERROR, out,
					 "could not process the input line");
			if (line_result == FYAILR_HANDLED)
				continue;
		}
		rc = fyai_event_loop_step(el, -1);
		fyai_error_check(ctx, rc >= 0, out,
				 "interactive event loop failed");
	}
	rc = 0;
out:
	/* Cancel active work and drain MCP shutdown on every exit path. */
	state = FYAIAS_STOPPING;
	if (run) {
		fyai_turn_run_cancel(run);
		fyai_turn_run_destroy(run);
		fyai_ui_set_busy(ctx, false);
	}
	if (ctx->config_edit) {
		fyai_config_edit_cancel(ctx->config_edit);
		fyai_config_edit_destroy(ctx->config_edit);
		ctx->config_edit = NULL;
	}
	/* Reclaim the active turn's builder or a lingering idle scratch one. */
	fyai_cleanup_transient_builder(ctx);
	if (!fyai_mcp_stop(ctx)) {
		while (!fyai_mcp_stop_done(ctx)) {
			rc = fyai_event_loop_step(el, -1);
			if (rc < 0)
				break;
		}
		fyai_mcp_stop_finish(ctx);
	}
	state = FYAIAS_DONE;
	(void)state;
	free(line);
	free(histfile);
	return rc;
}

int fyai_prompt_interactive(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg = ctx->cfg;
	struct fyai_prompt_args *args = &cfg->cmd.args.prompt;
	const char *prompt;
	char *histfile = NULL;
	char *line = NULL;
	fy_generic v;
	int rc = -1;

	(void)args;
	assert(ctx);

	if (fyai_ui_open(ctx)) {
		fyai_error(ctx, "could not initialize the interactive terminal UI");
		goto err_out;
	}
	if (fyai_ui_active(ctx))
		return fyai_prompt_interactive_async(ctx);

	/* Start MCP before the synchronous interactive fallback. */
	if (cfg->mcp_enabled) {
		if (fyai_setup_transient_builder(ctx))
			goto err_out;
		rc = fyai_mcp_bringup(ctx);
		if (!rc)
			rc = fyai_request_state_apply(ctx);
		fyai_cleanup_transient_builder(ctx);
		if (rc) {
			fyai_error(ctx, "could not initialize MCP servers");
			goto err_out;
		}
		rc = -1;
	}

	if (cfg->prompt && *cfg->prompt) {
		v = fyai_run_turn(ctx, ctx->last_message);
		v = fyai_report_diag(ctx, v);
		if (fy_is_invalid(v))
			goto err_out;
		ctx->last_message = v;
		rc = fyai_publish_state(ctx);
		if (rc)
			goto err_out;
	}

	histfile = fyai_history_path();
	fyai_interactive_prepare(ctx, histfile, false);

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
		/* The top row and the bottom status row (both {key} templates)
		 * are installed by the banner update, which frames the input. */
		fyai_session_banner_update(ctx);

		/* Default marker aligns with the markdown "  " indent. */
		if (!(cfg->prompt_marker && *cfg->prompt_marker))
			prompt = "  > ";
	}

	for (;;) {
		if (ctx->terminate_pending)
			break;
		errno = 0;
		line = fyai_ui_active(ctx) ? fyai_ui_readline(ctx) :
			fyai_readline(ctx, prompt);
		if (!line) {
			/* Ctrl-C cancels the line; Ctrl-D / EOF exits. */
			if (errno == EAGAIN)
				continue;
			break;
		}
		if (strspn(line, " \t\r\n") == strlen(line)) {
			free(line);
			line = NULL;
			continue;
		}

		/*
		 * Slash commands: /clear, /compact, /model, ... (see /help).
		 * They never reach the model; "//..." escapes to send a
		 * literal slash-prefixed prompt line.
		 */
		rc = fyai_interactive_handle_slash(ctx, histfile, line);
		if (rc >= 0) {
			free(line);
			line = NULL;
			if (rc > 0)
				break;
			continue;
		}
		fyai_interactive_prepare_user_line(ctx, histfile, line);

		rc = fyai_setup_transient_builder(ctx);
		assert(!rc);

		v = fyai_interactive_append_user_turn(ctx, line);
		free(line);
		line = NULL;
		if (fy_is_invalid(v)) {
			fyai_error(ctx, "could not append the user turn");
			fyai_ui_diag_drain(ctx, "error");
			fyai_cleanup_transient_builder(ctx);
			continue;
		}

		fyai_ui_set_busy(ctx, true);
		v = fyai_run_turn(ctx, v);
		fyai_ui_set_busy(ctx, false);
		/* A failed/interrupted run may carry a diagnostic; print it and
		 * unwrap to the (possibly partial) turn. */
		v = fyai_report_diag(ctx, v);
		if (fy_is_invalid(v)) {
			/* nothing completed: keep the prior state, stay in the
			 * loop so the user can retry or exit */
			fyai_ui_diag_drain(ctx, "error");
			fyai_cleanup_transient_builder(ctx);
			continue;
		}
		/* Interrupted mid-turn: commit the steps that completed. */
		ctx->last_message = v;
		if (fyai_publish_state(ctx))
			goto err_out;

		/*
		 * The turn is over and the render is done: report now, before
		 * the banner repaints the footer under it.
		 */
		fyai_interactive_finish_output(ctx);
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

	/* Not for a delegated sub-agent: its stderr is the parent's. */
	if (cfg->stats && !fyai_agent_delegated(ctx))
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
