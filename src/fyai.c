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
#include "fyai_cmd.h"
#include "fyai_agent.h"
#include "fyai_monitor.h"
#include "fyai_tool_registry.h"
#include "fyai_catalog.h"
#include "fyai_config.h"
#include "fyai_grammar.h"
#include "fyai_crash.h"
#include "fyai_curl.h"
#include "fyai_event.h"
#include "fyai_display.h"
#include "fyai_log.h"
#include "fyai_markdown.h"
#include "fyai_model.h"
#include "fyai_provider.h"
#include "fyai_auth_util.h"
#include "fyai_output.h"
#include "fyai_acp.h"
#include "fyai_sink.h"
#include "fyai_session.h"
#include "fyai_browser.h"
#include "fyai_agents.h"
#include "fyai_prof.h"
#include "fyai_ui.h"
#include "fyai_tools.h"
#include "fyai_home.h"
#include "fyai_orphan.h"
#include "fyai_view.h"
#include "fyai_asks.h"
#include "fyai_wait.h"
#include "fyai_storage.h"
#include "fyai_stream.h"
#include "fyai_transport_boot.h"
#include "fyai_xfer.h"
#include "fyai_terminal.h"
#include "fyai_terminal_session.h"
#include "fyai_tool_spec.h"
#include "fyai_turn.h"
#include "fyai_branch.h"
#include "fyai_skill.h"
#include "fyai_settings.h"

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

/*
 * SIGUSR2 appends the event-loop dump to ~/.fyai/dump.log, which the handler
 * creates. A full-screen terminal garbles text written to it. Without a home
 * directory the dump goes to the original standard error.
 */
static void fyai_dump_setup(struct fyai_ctx *ctx)
{
	const char *home = getenv("HOME");
	int rc;

	if (home && *home) {
		rc = asprintf(&ctx->dump_dir, "%s/.fyai", home);
		if (rc < 0)
			ctx->dump_dir = NULL;
		else if (asprintf(&ctx->dump_file, "%s/dump.log",
				  ctx->dump_dir) < 0)
			ctx->dump_file = NULL;
	}
	if (ctx->dump_fd >= 0)
		(void)fyai_event_dump_open(ctx, SIGUSR2, ctx->dump_fd);
}

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
		/* Past the UI stdio spool, so a crash still reaches it. */
		fyai_crash_set_fd(ctx->dump_fd);
	}
	fyai_dump_setup(ctx);
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
	if (fy_get(usage, "est", (_Bool)false))
		ctx->usage_cost_est += fy_get(usage, "cost", 0.0);
	ctx->usage_calls++;
	/* The request that paid for the change of model is over. */
	ctx->switch_pending = false;

	/* Last-call snapshot: ground truth for the /context fill report. */
	ctx->last_call_input = fy_get(usage, "input", 0LL);
	ctx->last_call_output = fy_get(usage, "output", 0LL);
	ctx->last_call_total = fy_get(usage, "total", 0LL);

	/* The input pane reports usage from every completed model call. */
	fyai_session_banner_update(ctx);
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
		(void)fyai_report(ctx, " cost=%s$%.6f",
				  ctx->usage_cost_est > 0.0 ? "~" : "",
				  ctx->usage_cost);
	(void)fyai_report(ctx, "\n");
}

static bool fyai_is_tool_marked(const char *name)
{
	return fyai_tool_has(name, FYAI_TOOL_MARKED);
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
	bool session_display;
	bool agent_display;
	bool isolated_tool;
	int rc;

	assert(ctx->transient_gb);

	tool_call_type = fy_get(tool_call, "type");
	name = fyai_tool_call_name(ctx, tool_call);
	shell = fy_equal(name, "shell");
	agent = fy_equal(name, "agent");
	/*
	 * A call that drives a terminal session shows nothing of its own: the
	 * session has one screen, and what these calls did is on it.
	 */
	session_display = fyai_shell_session_display(ctx, tool_call);
	/* Do not redraw calls already presented live. */
	banded = (shell || agent) && !session_display &&
		 (fyai_sink_bands_available(ctx->sink) ||
		  (shell && cfg->agent_pty));
	/* A displayed sub-agent's surface already carries the call header. */
	agent_display = agent && fyai_ui_active(ctx);
	marked = fyai_is_tool_marked(name) && !session_display;
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
	/* Do not repeat a job invocation after its live output. */
	if (!session_display && !agent_display &&
	    !(shell && cfg->agent_pty && !execute) &&
	    (fyai_agent_delegated(ctx) || !cfg->markdown || banded ||
	     (marked && fyai_sink_bands_available(ctx->sink))))
		fyai_print_tool_call(ctx, tool_call, execute);
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
	if (isolated_tool && !session_display) {
		/* Store the result in the transcript path. */
		if (marked && fyai_sink_bands_available(ctx->sink))
			fyai_render_tool_result_exchange(ctx, tool_call,
						 tool_result);
		else if (!banded)
			fyai_render_tool_exchange(ctx, tool_call, tool_result,
					  tool_ok);
	}
	/*
	 * The session commits its own screen when its program goes, so the
	 * calls that drive it add nothing to the transcript here.
	 */
	rc = session_display ? 0 :
		fyai_record_tool_exchange(ctx, tool_call, tool_result, tool_ok);
	fyai_error_check(ctx, !rc, err_resume,
		"could not record tool display output");
	rc = isolated_tool ? fyai_output_resume(ctx) : 0;
	fyai_error_check(ctx, !rc, err,
		"could not resume output after tool call");

	if (!isolated_tool && !session_display && cfg->markdown && !banded)
		fyai_render_tool_exchange(ctx, tool_call, tool_result,
					  tool_ok);
	if (cfg->debug)
		emit_generic_to_stdout(ctx, "tool-result", tool_result,
				       cfg->pretty);
	tool_call_id = fy_get(tool_call, "call_id", "");

	/*
	 * The output item type must match the call item type: a native
	 * `shell_call` is answered with `shell_call_output`, while every
	 * function tool (read_file, write_file, shell, ask_user) arrives as a
	 * `function_call` and is answered with `function_call_output`. The
	 * Responses API rejects a shell_call_output for a function_call, so
	 * the tool reads as "failed".
	 */
	if (fy_equal(tool_call_type, "shell_call")) {
		tool_call_output_type = "shell_call_output";
		max_output_length = fy_get_at_path(tool_call, "action",
						   "max_output_length");
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

	fyai_xfer_set_body(ctx, request_body);

	/* The parent surface supplies a delegated sub-agent's activity mark. */
	step->spinner.enabled = !fyai_ui_active(ctx) &&
				!fyai_agent_delegated(ctx) &&
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
	const struct fyai_grammar *grammar;
	struct fyai_grammar_request req;
	fy_generic turn;
	fy_generic previous;
	fy_generic previous_response_id;
	fy_generic catalog;
	fy_generic cat_model;
	fy_generic request;
	fy_generic messages;
	fy_generic m;
	struct timespec t_emit;
	bool response_linked;
	bool reasoning;
	int rc;

	ctx = step->ctx;
	cfg = ctx->cfg;
	grammar = fyai_grammar(cfg);
	turn = step->turn;
	previous = step->previous;
	step->spinner.ctx = ctx;
	step->tool_sink.ctx = ctx;
	step->tool_sink.group = step->tool_group;

	if (cfg->chatgpt_auth) {
		fyai_error_check(ctx, grammar->chatgpt &&
			!cfg->response_chain && cfg->api_url &&
			!strcmp(cfg->api_url, OPENAI_RESPONSES_URL), out,
			"ChatGPT plan usage requires the public, stateless Responses endpoint");
		cfg->stream = true;
	}

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
	 * what the conversation was started with. */
	req.instructions = cfg->system_prompt;
	fy_foreach(m, messages) {
		if (fy_equal(fy_get(m, "role"), "system")) {
			req.instructions = fy_get(m, "content", req.instructions);
			break;
		}
	}

	catalog = fyai_catalog_effective(cfg->catalog, cfg->gb);
	cat_model = fyai_catalog_resolved_model(catalog, cfg->model);
	reasoning = fyai_grammar_reasoning_set(cfg);
	response_linked = grammar->chaining && cfg->response_chain &&
		fy_is_valid(previous_response_id) &&
		!fy_is_null(previous_response_id);

	req.messages = messages;
	req.previous_response_id = response_linked ? previous_response_id :
						     fy_invalid;
	req.max_tokens = fyai_model_step_max_tokens(ctx, cfg);
	/* Reasoning models reject an explicit temperature. */
	req.temperature = fyai_model_supports_temperature(cat_model) &&
			  !reasoning;
	/*
	 * token_extents wants per-token delimitation, which only logprobs
	 * provide. Gate on the catalogue (reasoning models reject the params)
	 * and on the session fail-soft latch. Gated-off calls still record
	 * chunk extents from the stream.
	 */
	req.extents_lp = cfg->token_extents && cfg->stream &&
		!ctx->token_extents_off && grammar->logprobs && !reasoning &&
		fyai_model_supports_logprobs(cat_model);

	request = fyai_grammar_build_request(ctx, &req);
	fyai_error_check(ctx, fy_is_valid(request), out,
			 "could not build the model request");
	if (cfg->chatgpt_auth)
		request = fyai_auth_subscription_request(ctx->transient_gb, request);
	fyai_error_check(ctx, fy_is_valid(request), out,
			 "could not finish the model request");

	if (cfg->debug)
		emit_generic_to_stdout(ctx, "request", request, cfg->pretty);

	if (cfg->conversation_logging) {
		(void)fyai_log_generic(ctx, "conversation",
			fy_mapping(ctx->transient_gb,
				"kind", "request",
				"api", grammar->name,
				"url", cfg->api_url,
				"body", request));
	}

	rc = fyai_model_step_submit_request(step, request, response_linked,
					    req.extents_lp, &t_emit);
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
			status = fyai_xfer_last_status(ctx);
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
static int fyai_interactive_prepare_user_line(struct fyai_ctx *ctx,
					      const char *histfile, char *line);

static bool fyai_interactive_is_user_line(const char *line)
{
	return line && line[0] != '!' &&
		(line[0] != '/' || line[1] == '/');
}

enum fyai_turn_run_state {
	FYAITRS_NEW,
	FYAITRS_MODEL,
	FYAITRS_TOOLS,
	FYAITRS_GATE,
	FYAITRS_DONE,
	FYAITRS_FAILED,
};

struct fyai_turn_run {
	struct fyai_ctx *ctx;
	fy_generic turn;
	fy_generic turn_in;
	fy_generic settled;
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
	bool accept_pending_user;
	const char *histfile;
	/* The project state before the next group of tool calls, when the diffs are shown. */
	fy_generic diff_before;
	/*
	 * The open round of the tool gate: its ID, its calls, what each came
	 * to, and the answers that it waits for. An after round resumes the
	 * exclusive calls at @gate_next.
	 */
	uint64_t gate_id;
	fy_generic gate_items;
	struct fyai_gate_answer *gate;
	size_t gate_count;
	size_t gate_pending;
	size_t gate_next;
	bool gate_after;
};

/* The answer of the tool gate for one call of a round. */
struct fyai_gate_answer {
	bool answered;
	bool allowed;
	bool stop;
	fy_generic args;
	char *text;
};

/* A diff that is longer than this is cut: the human reads it, and a full tree is not that. */
#define FYAI_TOOL_DIFF_LIMIT (256 * 1024)

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
	case FYAITRS_GATE:
		return "gate";
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
	case FYAITRS_TOOLS:
	case FYAITRS_GATE:
		return to == FYAITRS_MODEL || to == FYAITRS_TOOLS ||
		       to == FYAITRS_GATE || to == FYAITRS_DONE ||
		       to == FYAITRS_FAILED;
	case FYAITRS_DONE:
	case FYAITRS_FAILED:
		return false;
	}
	return false;
}

static void fyai_turn_run_observe(struct fyai_turn_run *run)
{
	struct fyai_ctx *ctx = run->ctx;

	if (ctx->turn_observe)
		ctx->turn_observe(ctx, run->turn, ctx->turn_observe_data);
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
		run->ctx->tool_diff_tracking = false;
		return -1;
	}
	if (run->ctx->cfg->debug)
		fyai_debug(run->ctx, "turn state %s -> %s",
			   fyai_turn_run_state_name(run->state),
			   fyai_turn_run_state_name(state));
	run->state = state;
	if (state == FYAITRS_DONE || state == FYAITRS_FAILED)
		run->ctx->tool_diff_tracking = false;
	if (state == FYAITRS_MODEL)
		fyai_agents_activity(run->ctx, "running");
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

/*
 * End a turn that failed. The turn keeps the steps up to the last point where
 * every tool call had its result: the work of a long turn is not lost to one
 * failure, and the stored conversation stays valid for the next request.
 * Only a turn with no such step is discarded.
 */
static void fyai_turn_run_abort_output(struct fyai_turn_run *run)
{
	struct fyai_ctx *ctx = run->ctx;
	fy_generic result;

	if (!fyai_turn_run_done(run) && fy_is_valid(run->settled) &&
	    run->settled.v != run->turn_in.v) {
		result = fyai_output_finalize(ctx, run->settled, true);
		result = fy_gb_internalize(ctx->gb, result);
		if (fy_is_valid(result)) {
			result = fyai_with_diag(ctx->gb, result,
				"the turn stopped on an error; the steps "
				"before it are kept");
			run->result = result;
			(void)fyai_turn_run_transition(run, FYAITRS_DONE);
			return;
		}
	}
	fyai_output_abort(ctx);
	run->result = fy_invalid;
	if (run->state != FYAITRS_FAILED)
		(void)fyai_turn_run_transition(run, FYAITRS_FAILED);
}

/* stop a turn that made progress */
static void fyai_turn_run_stop(struct fyai_turn_run *run, const char *msg)
{
	struct fyai_ctx *ctx;
	fy_generic result;

	ctx = run->ctx;
	if (fy_is_valid(run->turn) && run->turn.v != run->turn_in.v) {
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
	fy_generic diag;
	const char *msg;

	run->ctx->turn_stop = FYAI_TURN_STOP_REQUEST;
	diag = fy_generic_get_diag(response);
	msg = fy_is_valid(diag) &&
	      !fy_is_null(diag) ?
		fy_castp(&diag, "request failed") : "request failed";
	fyai_turn_run_stop(run, msg);
}

/* Append an event with its sender metadata to ctx->gb storage. */
fy_generic fyai_turn_append_event(struct fyai_ctx *ctx, fy_generic turn,
				  const struct fyai_event_input *input)
{
	const char *display;

	if (input->from) {
		turn = fyai_turn_append(ctx, turn,
			fy_sequence(fyai_make_agent_message(ctx, input->text,
				input->from, input->execution, input->message_id,
				input->reply_to)));
		display = fy_sprintfa("**Agent %s** (message %s%s%s)\n\n%s",
			input->from, input->message_id,
			input->reply_to && *input->reply_to ? ", reply to " : "",
			input->reply_to ? input->reply_to : "", input->text);
		return fyai_output_record(ctx, turn, FYAI_OUTPUT_AGENT, display);
	}
	turn = fyai_turn_append(ctx, turn,
		fy_sequence(fyai_make_instruction_message(ctx, input->text)));
	return fyai_output_record(ctx, turn, FYAI_OUTPUT_USER, input->text);
}

static int fyai_turn_run_append_user(struct fyai_turn_run *run, const char *text)
{
	struct fyai_ctx *ctx = run->ctx;
	char *shown;

	shown = fyai_media_pending_display(ctx, text);
	run->turn = fyai_turn_append(ctx, run->turn,
		fy_sequence(fyai_make_user_message(ctx, text)));
	if (fy_is_valid(run->turn))
		run->turn = fyai_output_record(ctx, run->turn,
					       FYAI_OUTPUT_USER,
					       shown ? shown : text);
	free(shown);
	return fy_is_valid(run->turn) ? 0 : -1;
}

/*
 * Add queued events and user lines before the next model request. An event
 * enters first, as it was queued before the line that is read now. Commands
 * keep order.
 */
static int fyai_turn_run_take_pending_user(struct fyai_turn_run *run)
{
	struct fyai_ctx *ctx = run->ctx;
	const char *head;
	char *line;
	struct fyai_event_input event = {};
	bool have_event;
	int rc;

	if (!run->accept_pending_user)
		return 0;
	/* A stale event is dropped here, so a live one is taken before output ends. */
	have_event = fyai_event_take_live_input(ctx, &event);
	head = fyai_ui_peek_line(ctx);
	if (!have_event && !fyai_interactive_is_user_line(head))
		return 0;
	run->turn = fyai_output_finalize(ctx, run->turn, false);
	fyai_error_check(ctx, fy_is_valid(run->turn), err,
			 "could not finish output before pending input");
	while (have_event) {
		if (!event.from)
			fyai_echo_user_turn(ctx, event.text);
		run->turn = fyai_turn_append_event(ctx, run->turn, &event);
		fyai_event_input_clear(&event);
		fyai_error_check(ctx, fy_is_valid(run->turn), err,
				 "could not append the pending event");
		have_event = fyai_event_take_live_input(ctx, &event);
	}
	while (fyai_interactive_is_user_line(head = fyai_ui_peek_line(ctx))) {
		line = fyai_ui_take_line(ctx);
		fyai_error_check(ctx, line, err,
				 "could not take pending input");
		if (fyai_interactive_prepare_user_line(ctx, run->histfile,
						       line)) {
			free(line);
			continue;
		}
		rc = fyai_turn_run_append_user(run, line);
		free(line);
		fyai_error_check(ctx, !rc, err,
				 "could not append pending user input");
	}
	rc = fyai_output_begin(ctx, FYAI_OUTPUT_ASSISTANT);
	fyai_error_check(ctx, !rc, err, "could not resume assistant output");
	return 0;
err:
	fyai_event_input_clear(&event);
	return -1;
}

static int fyai_turn_run_submit_model(struct fyai_turn_run *run)
{
	struct fyai_ctx *ctx;
	struct fyai_cfg *cfg;
	fy_generic previous;
	const char *test_abort;
	bool open_group;
	int rc;

	ctx = run->ctx;
	cfg = ctx->cfg;
	/* Every call of the turn so far has its result. */
	if (fy_is_valid(run->turn))
		run->settled = run->turn;
	/* Test hook for the failure of a turn after a round of tools. */
	test_abort = getenv("FYAI_TEST_TURN_ABORT");
	if (test_abort && run->iteration == atoi(test_abort)) {
		fyai_error(ctx, "injected failure of the turn");
		fyai_turn_run_abort_output(run);
		return -1;
	}
	/* A tool that answers for the turn ends it, as a final response does. */
	if (ctx->turn_end_requested) {
		ctx->turn_end_requested = false;
		fyai_turn_run_finish(run, run->turn, true);
		return 0;
	}
	if (run->iteration >= cfg->max_tool_iterations) {
		/* tool loop limit reached */
		ctx->turn_stop = FYAI_TURN_STOP_LIMIT;
		fyai_turn_run_stop(run,
			fy_sprintfa("the tool loop reached its limit of %d iterations",
				    cfg->max_tool_iterations));
		return -1;
	}
	if (ctx->turn_cost_limit > 0 && ctx->usage_cost >= ctx->turn_cost_limit) {
		ctx->turn_stop = FYAI_TURN_STOP_COST;
		fyai_turn_run_stop(run,
			fy_sprintfa("the cost reached its limit of $%.4f",
				    ctx->turn_cost_limit));
		return -1;
	}
	if (fyai_turn_run_take_pending_user(run)) {
		fyai_turn_run_abort_output(run);
		return -1;
	}
	/*
	 * Only a response-chained request omits the history the provider
	 * already holds. Every other request carries the full conversation,
	 * so it must start the walk at the root and not at the head this
	 * run began from.
	 */
	previous = cfg->response_chain ? run->previous : fy_null;
	/* A gate decides on the calls of a response before any of them runs. */
	open_group = cfg->stream && !ctx->tool_gate;
	run->tool_group = open_group ?
		fyai_tool_job_group_create_open(ctx,
			fyai_turn_run_tool_complete, run) : NULL;
	fyai_error_check(ctx, !open_group || run->tool_group, err,
			 "could not create streaming tool group");
	rc = fyai_turn_run_transition(run, FYAITRS_MODEL);
	fyai_error_check(ctx, !rc, err,
			 "could not advance turn to model request");
	fyai_diag_tracef("model", "request, round %d", run->iteration + 1);
	run->model_step = fyai_model_step_submit(ctx, run->turn, previous,
						 run->tool_group,
						 fyai_turn_run_model_complete,
						 run);
	fyai_error_check(ctx, run->model_step, err,
			 "could not submit model step");
	return 0;

err:
	fyai_turn_run_drop_tool_group(run);
	fyai_turn_run_stop(run, "the turn stopped before the model request");
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

/*
 * Record the project after a group of tool calls that can change files and show
 * what the group changed. A patch or a file write does not display its own diff
 * while the turn is tracked, so this one is the only place that shows it. A
 * failure stops the diffs of the turn and not the turn.
 */
static void fyai_turn_run_tool_diff(struct fyai_turn_run *run)
{
	struct fyai_ctx *ctx = run->ctx;
	fy_generic after, text;
	const char *patch;
	int rc;

	rc = fyai_project_state_capture(ctx, &after);
	if (rc || !fy_is_mapping(after)) {
		ctx->tool_diff_tracking = false;
		return;
	}
	if (!fyai_project_state_diff(ctx, ctx->transient_gb, run->diff_before, after,
					     FYAI_TOOL_DIFF_LIMIT, &text)) {
		patch = fy_castp(&text, "");
		if (*patch) {
			(void)fyai_present_tool_diff(ctx, patch);
			fyai_tool_change_record(ctx, run->diff_before, after);
		}
	}
	run->diff_before = after;
}

static int fyai_turn_run_dispatch_tools(struct fyai_turn_run *run);

static void fyai_turn_run_gate_clear(struct fyai_turn_run *run)
{
	size_t i;

	for (i = 0; i < run->gate_count; i++)
		free(run->gate[i].text);
	free(run->gate);
	run->gate = NULL;
	run->gate_count = 0;
	run->gate_pending = 0;
	run->gate_items = fy_invalid;
	if (run->ctx->tool_gate_run == run)
		run->ctx->tool_gate_run = NULL;
}

/*
 * Open a round of the tool gate over @items: the calls of a response, or after
 * a group of calls {call, result, ok} for each of them. An after round goes on
 * with the exclusive calls at @next. The items live in the transient builder,
 * which the turn keeps.
 */
static int fyai_turn_run_gate_open(struct fyai_turn_run *run, fy_generic items,
				   bool after, size_t next)
{
	struct fyai_ctx *ctx = run->ctx;
	const struct fyai_tool_gate_ops *ops = ctx->tool_gate;
	fy_generic item;
	uint64_t id;
	size_t i, n;
	int rc;

	n = fy_len(items);
	run->gate = calloc(n ? n : 1, sizeof(*run->gate));
	fyai_error_check(ctx, run->gate, err, "could not open the tool gate");
	for (i = 0; i < n; i++)
		run->gate[i].args = fy_invalid;
	run->gate_items = items;
	run->gate_count = n;
	run->gate_pending = n;
	run->gate_after = after;
	run->gate_next = next;
	id = ++ctx->tool_gate_seq;
	run->gate_id = id;
	ctx->tool_gate_run = run;
	rc = fyai_turn_run_transition(run, FYAITRS_GATE);
	fyai_error_check(ctx, !rc, err,
			 "could not advance turn to the tool gate");
	/* An answer can come during the call, and the last one ends the round. */
	for (i = 0; i < n && ctx->tool_gate_run == run && run->gate_id == id;
	     i++) {
		item = fy_get_at(items, i);
		if (after)
			ops->after(ctx, id, i, fy_get(item, "call", fy_invalid),
				   fy_get(item, "result", fy_invalid),
				   fy_get(item, "ok", false), ctx->tool_gate_data);
		else
			ops->before(ctx, id, i, item, ctx->tool_gate_data);
	}
	if (!n)
		(void)fyai_event_defer(fyai_ctx_loop(ctx), fyai_turn_run_service,
				       run);
	return 0;
err:
	fyai_turn_run_gate_clear(run);
	fyai_turn_run_stop(run, "the tool gate could not open");
	return -1;
}

static struct fyai_gate_answer *
fyai_tool_gate_slot(struct fyai_ctx *ctx, uint64_t id, size_t index,
		    struct fyai_turn_run **runp)
{
	struct fyai_turn_run *run;

	run = ctx ? ctx->tool_gate_run : NULL;
	if (!run || run->gate_id != id || index >= run->gate_count ||
	    run->gate[index].answered)
		return NULL;
	*runp = run;
	return &run->gate[index];
}

static void fyai_tool_gate_answered(struct fyai_turn_run *run,
				    struct fyai_gate_answer *a,
				    const char *text, bool stop)
{
	a->answered = true;
	a->stop = stop;
	if (text && *text) {
		a->text = strdup(text);
		if (!a->text)
			fyai_error(run->ctx, "could not keep the answer of the "
				   "tool gate");
	}
	if (!--run->gate_pending)
		(void)fyai_event_defer(fyai_ctx_loop(run->ctx),
				       fyai_turn_run_service, run);
}

/* A round that a delegated sub-agent asked for: one call of its turn. */
struct fyai_gate_relay {
	struct fyai_gate_relay *next;
	uint64_t id;
	bool after;
	char *branch;		/* the job that asked */
	long long rpc_id;	/* and its request */
};

static void fyai_gate_relay_free(struct fyai_gate_relay *r)
{
	free(r->branch);
	free(r);
}

void fyai_tool_gate_relays_drop(struct fyai_ctx *ctx)
{
	struct fyai_gate_relay *r;

	while ((r = ctx->gate_relays)) {
		ctx->gate_relays = r->next;
		fyai_gate_relay_free(r);
	}
}

/* Take the relayed round @id of the kind @after, or NULL. */
static struct fyai_gate_relay *fyai_gate_relay_take(struct fyai_ctx *ctx,
						    uint64_t id, bool after)
{
	struct fyai_gate_relay **pp, *r;

	for (pp = &ctx->gate_relays; (r = *pp); pp = &r->next)
		if (r->id == id) {
			if (r->after != after)
				return NULL;
			*pp = r->next;
			return r;
		}
	return NULL;
}

/* Send @answer to the sub-agent that asked for round @r, and free it. */
static void fyai_gate_relay_answer(struct fyai_ctx *ctx,
				   struct fyai_gate_relay *r, fy_generic answer)
{
	int rc;

	rc = fyai_tool_agent_respond(ctx, r->branch, r->rpc_id, answer);
	if (rc)
		fyai_warning(ctx, "could not answer the tool call of the "
			     "sub-agent '%s'", r->branch);
	fyai_gate_relay_free(r);
}

int fyai_tool_gate_relay(struct fyai_ctx *ctx, const char *branch,
			 long long rpc_id, fy_generic params)
{
	const struct fyai_tool_gate_ops *ops = ctx->tool_gate;
	struct fyai_gate_relay *r;
	fy_generic call;

	if (!ops)
		return -1;
	r = calloc(1, sizeof(*r));
	fyai_error_check(ctx, r, err, "could not relay a tool call");
	r->branch = strdup(branch ? branch : "");
	fyai_error_check(ctx, r->branch, err_free,
			 "could not relay a tool call");
	r->id = ++ctx->tool_gate_seq;
	r->after = fy_equal(fy_get(params, "phase", fy_invalid), "after");
	r->rpc_id = rpc_id;
	/* The gate can answer during the call. */
	r->next = ctx->gate_relays;
	ctx->gate_relays = r;
	call = fy_get(params, "call", fy_invalid);
	if (r->after)
		ops->after(ctx, r->id, 0, call,
			   fy_get(params, "result", fy_invalid),
			   fy_get(params, "ok", false), ctx->tool_gate_data);
	else
		ops->before(ctx, r->id, 0, call, ctx->tool_gate_data);
	return 0;
err_free:
	free(r);
err:
	return -1;
}

void fyai_tool_gate_allow(struct fyai_ctx *ctx, uint64_t id, size_t index,
			  fy_generic args)
{
	struct fyai_gate_answer *a;
	struct fyai_gate_relay *r;
	struct fyai_turn_run *run;

	r = fyai_gate_relay_take(ctx, id, false);
	if (r) {
		fyai_gate_relay_answer(ctx, r, fy_mapping(fyai_ctx_transient_gb(ctx),
			"allow", true, "args", fyai_generic_or_null(args)));
		return;
	}
	a = fyai_tool_gate_slot(ctx, id, index, &run);
	if (!a || run->gate_after)
		return;
	a->allowed = true;
	if (fy_is_mapping(args))
		a->args = fy_gb_internalize(ctx->transient_gb, args);
	fyai_tool_gate_answered(run, a, NULL, false);
}

void fyai_tool_gate_deny(struct fyai_ctx *ctx, uint64_t id, size_t index,
			 const char *message, bool stop)
{
	struct fyai_gate_answer *a;
	struct fyai_gate_relay *r;
	struct fyai_turn_run *run;

	if (!message || !*message)
		message = "the tool call was denied";
	r = fyai_gate_relay_take(ctx, id, false);
	if (r) {
		fyai_gate_relay_answer(ctx, r, fy_mapping(fyai_ctx_transient_gb(ctx),
			"allow", false, "message", message,
			"stop", (_Bool)stop));
		return;
	}
	a = fyai_tool_gate_slot(ctx, id, index, &run);
	if (!a || run->gate_after)
		return;
	fyai_tool_gate_answered(run, a, message, stop);
}

void fyai_tool_gate_done(struct fyai_ctx *ctx, uint64_t id, size_t index,
			 const char *context, bool stop)
{
	struct fyai_gate_answer *a;
	struct fyai_gate_relay *r;
	struct fyai_turn_run *run;

	r = fyai_gate_relay_take(ctx, id, true);
	if (r) {
		fyai_gate_relay_answer(ctx, r, fy_mapping(fyai_ctx_transient_gb(ctx),
			"context", context ? context : "",
			"stop", (_Bool)stop));
		return;
	}
	a = fyai_tool_gate_slot(ctx, id, index, &run);
	if (!a || !run->gate_after)
		return;
	fyai_tool_gate_answered(run, a, context, stop);
}

/* Answer the calls of an open before round that have no result. */
static int fyai_turn_run_gate_refuse(struct fyai_turn_run *run,
				     const char *why)
{
	struct fyai_ctx *ctx = run->ctx;
	size_t i;

	for (i = 0; i < run->gate_count && !run->gate_after; i++) {
		if (run->gate[i].answered && !run->gate[i].allowed)
			continue;
		run->turn = fyai_finish_tool_call(ctx, run->turn,
			fy_get_at(run->gate_items, i),
			fy_value(ctx->transient_gb, why), false, false);
		fyai_error_check(ctx, fy_is_valid(run->turn), err,
				 "could not answer a held tool call");
		fyai_turn_run_observe(run);
	}
	return 0;
err:
	return -1;
}

/* Finish the turn that a round of the gate ends, with @cause. */
static void fyai_turn_run_gate_finish(struct fyai_turn_run *run,
				      const char *cause)
{
	struct fyai_ctx *ctx = run->ctx;
	fy_generic result;

	fyai_turn_run_gate_clear(run);
	result = fy_gb_internalize(ctx->gb, run->turn);
	result = fyai_with_diag(ctx->gb, result, cause);
	fyai_turn_run_finish(run, result, false);
}

/* Act on the answers of a round that has all of them. */
static int fyai_turn_run_gate_resolve(struct fyai_turn_run *run)
{
	struct fyai_ctx *ctx = run->ctx;
	struct fyai_gate_answer *a;
	fy_generic item, calls;
	char *cause = NULL;
	bool after, stop, context;
	size_t i, next;
	int rc;

	calls = fy_seq_empty;
	stop = false;
	context = false;
	for (i = 0; i < run->gate_count; i++) {
		a = &run->gate[i];
		item = fy_get_at(run->gate_items, i);
		if (a->stop && !stop) {
			stop = true;
			cause = a->text ? strdup(a->text) : NULL;
		}
		if (run->gate_after) {
			if (!a->text || a->stop)
				continue;
			/* Context enters as pending input does: between documents. */
			if (!context) {
				run->turn = fyai_output_finalize(ctx, run->turn,
								 false);
				fyai_error_check(ctx, fy_is_valid(run->turn),
						 err, "could not finish output "
						 "before the context of a tool "
						 "gate");
				context = true;
			}
			rc = fyai_turn_run_append_user(run, a->text);
			fyai_error_check(ctx, !rc, err,
					 "could not add the context of a tool "
					 "gate");
			continue;
		}
		if (a->allowed) {
			if (fy_is_mapping(a->args))
				item = fyai_tool_call_set_args(ctx, item,
							       a->args);
			fyai_error_check(ctx, fy_is_valid(item), err,
					 "could not change the arguments of "
					 "a tool call");
			calls = fy_append(ctx->transient_gb, calls, item);
			continue;
		}
		run->turn = fyai_finish_tool_call(ctx, run->turn, item,
			fy_value(ctx->transient_gb, a->text), false, false);
		fyai_error_check(ctx, fy_is_valid(run->turn), err,
				 "could not answer a denied tool call");
		fyai_turn_run_observe(run);
	}
	if (stop) {
		/* A call that a stop holds back still needs its result. */
		rc = fyai_turn_run_gate_refuse(run, cause ? cause :
					       "the turn stopped");
		fyai_error_check(ctx, !rc, err,
				 "could not stop at the tool gate");
		fyai_turn_run_gate_finish(run, cause ? cause :
					  "a tool gate stopped the turn");
		free(cause);
		return 0;
	}
	if (context) {
		rc = fyai_output_begin(ctx, FYAI_OUTPUT_ASSISTANT);
		fyai_error_check(ctx, !rc, err,
				 "could not resume assistant output");
	}
	after = run->gate_after;
	next = run->gate_next;
	fyai_turn_run_gate_clear(run);
	if (after)
		return fyai_turn_run_submit_exclusive(run, next);
	run->tool_calls = calls;
	if (fy_empty(calls)) {
		run->iteration++;
		return fyai_turn_run_submit_model(run);
	}
	return fyai_turn_run_dispatch_tools(run);
err:
	free(cause);
	fyai_turn_run_gate_clear(run);
	fyai_turn_run_abort_output(run);
	return -1;
}

static int fyai_turn_run_collect_tools(struct fyai_turn_run *run)
{
	struct fyai_ctx *ctx;
	fy_generic tool_call;
	fy_generic result, reports;
	size_t group_index;
	size_t i, next;
	size_t total;
	bool interrupted;
	bool ok;
	int rc, effect;

	ctx = run->ctx;
	total = fy_len(run->tool_calls);
	group_index = 0;
	effect = 0;
	reports = fy_seq_empty;
	for (i = 0; i < total; i++) {
		tool_call = fy_get_at(run->tool_calls, i);
		if (run->group_parallel !=
		    fyai_tool_call_parallel_eligible(ctx, tool_call))
			continue;
		if (!run->group_parallel && i != run->exclusive_index)
			continue;
		if (ctx->tool_diff_tracking &&
		    fyai_tool_call_file_effect(ctx, tool_call) > effect)
			effect = fyai_tool_call_file_effect(ctx, tool_call);
		result = fy_invalid;
		ok = false;
		rc = fyai_tool_job_group_collect(run->tool_group,
						 group_index++, &result, &ok);
		/* Test hook for a call that has no result. */
		if (!rc && getenv("FYAI_TEST_COLLECT_FAIL"))
			rc = -1;
		/* The model answers for a call that has no result. */
		if (rc || fy_is_invalid(result)) {
			fyai_warning(ctx, "a tool call returned no result");
			result = fy_value(ctx->transient_gb,
					  "tool error: the call returned no result");
			ok = false;
		}
		run->turn = fyai_finish_tool_call(ctx, run->turn, tool_call,
						 result, ok, false);
		fyai_error_check(ctx, fy_is_valid(run->turn), err,
				 "could not append tool job result");
		fyai_turn_run_observe(run);
		if (ctx->tool_gate)
			reports = fy_append(ctx->transient_gb, reports,
				fy_mapping(ctx->transient_gb, "call", tool_call,
					   "result", result, "ok", (_Bool)ok));
		/* Present the completed exchange before opening the next band. */
		fyai_ui_drain_output(ctx);
	}
	fyai_turn_run_drop_tool_group(run);
	run->turn = fy_gb_internalize(ctx->transient_gb, run->turn);
	fyai_error_check(ctx, fy_is_valid(run->turn), err,
			 "could not append the tool calls");
	if (effect)
		fyai_turn_run_tool_diff(run);
	if (ctx->ask_abort) {
		fyai_turn_run_finish(run, fy_null, true);
		return 0;
	}
	interrupted = fyai_interrupt_check(ctx);
	if (run->cancel_requested || interrupted) {
		result = fy_gb_internalize(ctx->gb, run->turn);
		result = fyai_with_diag(ctx->gb, result, "interrupted");
		fyai_turn_run_finish(run, result, false);
		return 0;
	}
	next = run->group_parallel ? 0 : run->exclusive_index + 1;
	if (ctx->tool_gate && !fy_empty(reports))
		return fyai_turn_run_gate_open(run, reports, true, next);
	return fyai_turn_run_submit_exclusive(run, next);

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
		fyai_acp_worker_text(ctx, text, strlen(text));
		fyai_text_observe(ctx, text, strlen(text));
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
	fyai_turn_run_observe(run);
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
	fy_generic tool_call;
	bool streamed;
	int rc;

	ctx = run->ctx;
	fyai_error_check(ctx, !run->tool_group ||
			 fyai_tool_job_group_count(run->tool_group) ==
				parallel_total,
			 err, "streamed tool group does not match response");
	streamed = run->tool_group != NULL;
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
	if (streamed)
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

static int fyai_turn_run_dispatch_tools(struct fyai_turn_run *run)
{
	size_t parallel_total;

	parallel_total = fyai_turn_run_parallel_count(run);
	if (parallel_total)
		return fyai_turn_run_submit_parallel(run, parallel_total);
	fyai_turn_run_drop_tool_group(run);
	return fyai_turn_run_submit_exclusive(run, 0);
}

static int
fyai_turn_run_start_tools(struct fyai_turn_run *run, fy_generic response)
{
	if (run->ctx->cfg->response_chain)
		run->previous = run->turn;
	run->tool_calls = fyai_response_tool_calls(run->ctx, response);
	if (run->ctx->tool_gate) {
		fyai_turn_run_drop_tool_group(run);
		return fyai_turn_run_gate_open(run, run->tool_calls, false, 0);
	}
	return fyai_turn_run_dispatch_tools(run);
}

static int fyai_turn_run_process_model(struct fyai_turn_run *run)
{
	struct fyai_ctx *ctx;
	fy_generic response;
	int rc;

	ctx = run->ctx;
	response = fyai_turn_run_take_model_response(run);
	if (run->cancel_requested) {
		fyai_turn_run_drop_tool_group(run);
		fyai_turn_run_stop(run, "interrupted");
		return 0;
	}
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

	fyai_ui_marker(ctx, "turn");

	if (!fyai_credential_available(ctx)) {
		fyai_error(ctx, "no API key or ChatGPT login is available");
		return NULL;
	}
	run = calloc(1, sizeof(*run));
	fyai_error_check(ctx, run, err, "out of memory");
	run->ctx = ctx;
	run->turn = turn;
	run->turn_in = turn;
	run->settled = turn;
	fyai_scope_exchange(ctx, true);
	run->previous = fy_get(turn, "previous", fy_null);
	run->tool_calls = fy_null;
	run->result = fy_invalid;
	run->gate_items = fy_invalid;
	run->state = FYAITRS_NEW;
	/* A request of a turn that stopped does not end this one. */
	ctx->turn_end_requested = false;
	ctx->tool_output_displayed = false;
	rc = fyai_output_begin(ctx, FYAI_OUTPUT_ASSISTANT);
	fyai_error_check(ctx, !rc, err_free,
			 "could not start assistant display output");
	fyai_interrupt_check(ctx);
	/* What the user or another program changed between turns is not the turn's. */
	if (fyai_tool_diff_enabled(ctx)) {
		rc = fyai_project_state_capture(ctx, &run->diff_before);
		ctx->tool_diff_tracking = !rc && fy_is_mapping(run->diff_before);
	}
	rc = fyai_turn_run_submit_model(run);
	fyai_error_check(ctx, !rc, err_free,
			 "could not submit turn");
	return run;

err_free:
	ctx->tool_diff_tracking = false;
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
		} else if (run->state == FYAITRS_GATE && !run->gate_pending) {
			(void)fyai_turn_run_gate_resolve(run);
			progress = true;
		}
	} while (progress && !fyai_turn_run_done(run));
}

static void fyai_turn_run_cancel(struct fyai_turn_run *run)
{
	if (!run || fyai_turn_run_done(run) || run->cancel_requested)
		return;
	run->cancel_requested = true;
	/* A round of the gate waits on nothing that a cancel can stop. */
	if (run->state == FYAITRS_GATE) {
		if (!fyai_turn_run_gate_refuse(run, "interrupted"))
			fyai_turn_run_gate_finish(run, "interrupted");
		else
			fyai_turn_run_stop(run, "interrupted");
		return;
	}
	if (run->model_step)
		fyai_model_step_cancel(run->model_step);
	if (run->tool_group)
		fyai_tool_job_group_cancel(run->tool_group);
	/* Stop terminal sessions owned by the interrupted turn. */
	fyai_shell_sessions_release_turn(run->ctx);
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
	fyai_turn_run_gate_clear(run);
	run->ctx->tool_diff_tracking = false;
	fyai_diag_tracef("turn", "end, %s, %d rounds",
			 fyai_turn_run_state_name(run->state), run->iteration);
	fyai_scope_exchange(run->ctx, false);
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
	run->accept_pending_user = ctx->cfg->agent_child;
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

	/* An explicit owner adopts a builder that fyai_ctx_transient_gb()
	 * made: the idle loop must not release it while the owner works. */
	if (ctx->transient_gb) {
		ctx->transient_autorelease = false;
		return 0;
	}
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

	fyai_cover_release(&ctx->cover);
	fyai_orphan_release(ctx);
	/* The holder gives what is left in the scope its time before the process ends. */
	fyai_cover_drain(&ctx->cover);

	/* Remove the handlers before the context that they name is destroyed. */
	/* A session cannot outlive the invocation that opened it, and neither
	 * can a wait: there is no daemon for either to be handed to. */
	fyai_shell_sessions_release(ctx, false);
	fyai_asks_release(ctx);
	fyai_waits_release(ctx);
	fyai_waiters_release(ctx);
	fyai_tool_gate_relays_drop(ctx);
	fyai_agent_background_close(ctx);
	fyai_monitor_close(ctx);
	fyai_tool_diff_cleanup(ctx);
	fyai_events_release(ctx);
	fyai_terminal_winch_close(ctx);
	fyai_event_interrupt_close(ctx);
	fyai_output_cleanup(ctx);
	/* After the output document: its discard still talks to the sink. */
	fyai_agents_cleanup(ctx);
	fyai_skill_dirs_release(ctx);
	fyai_settings_release(ctx);
	fyai_home_release(ctx);
	fyai_cleanup_transient_builder(ctx);
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
	free(ctx->session_unstored);
	ctx->session_unstored = NULL;
	free(ctx->agent_branch);
	ctx->agent_branch = NULL;
	free(ctx->agent_spawn_json);
	ctx->agent_spawn_json = NULL;
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
	fyai_transport_detach(ctx);
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

	if (ctx->dump_fd >= 0) {
		close(ctx->dump_fd);
		ctx->dump_fd = -1;
	}
	free(ctx->dump_dir);
	free(ctx->dump_file);
	ctx->dump_dir = ctx->dump_file = NULL;
}

/* Apply the configured MCP state. This operation is idempotent. */
static int fyai_mcp_bringup(struct fyai_ctx *ctx)
{
	if (ctx->cfg->mcp_enabled)
		return fyai_mcp_refresh(ctx);
	fyai_mcp_cleanup(ctx);
	ctx->mcp_tools = fy_invalid;
	ctx->mcp_active_tools = fy_invalid;
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

/* A run with no transport has no request state: there is nothing to apply. */
int fyai_request_state_apply(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg = ctx->cfg;
	const struct fyai_grammar *grammar = fyai_grammar(cfg);
	const struct fyai_grammar_header *hdr;
	fy_generic native, mt, tool, fn, converted;
	const char *name;
	int rc;

	if (!ctx->curl)
		return 0;
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

	/* A local server without authentication gets no API-key header. */
	if (!cfg->chatgpt_auth && !cfg->no_auth && !ctx->tclient &&
	    (!cfg->interactive || (cfg->api_key && *cfg->api_key))) {
		ctx->auth_header = grammar->key_header ?
			make_header(fy_sprintfa("%s: ", grammar->key_header),
				    cfg->api_key) :
			make_header("Authorization: Bearer ", cfg->api_key);
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

	for (hdr = grammar->headers; hdr->name; hdr++) {
		rc = append_header(&ctx->headers,
				   fy_sprintfa("%s: %s", hdr->name, hdr->value));
		if (rc)
			return -1;
	}

	fyai_xfer_set_endpoint(ctx, NULL, NULL);
	curl_easy_setopt(ctx->curl, CURLOPT_HTTPHEADER, ctx->headers);

	if (cfg->enable_tools || cfg->enable_builtin_shell ||
	    fyai_mcp_enabled(ctx) || cfg->web_search) {

		ctx->tools = fyai_grammar_make_tools(ctx);
		/* MCP discovery and resources remain available in an MCP-only run. */
		if (fyai_mcp_enabled(ctx) && !cfg->enable_tools &&
		    !cfg->enable_builtin_shell) {
			native = make_tools_filtered(ctx);

			fy_foreach(tool, native) {
				fn = fy_get(tool, "function");
				name = fy_get(fn, "name", "");
				if (strcmp(name, "tool_search") &&
				    strcmp(name, "mcp_resource"))
					continue;
				ctx->tools = fy_append(ctx->gb, ctx->tools,
						fyai_grammar_tool_def(ctx, tool));
			}
		}

		if (fyai_mcp_enabled(ctx)) {
			converted = fy_seq_empty;
			mt = fyai_mcp_tools(ctx);
			fy_foreach(tool, mt) {
				fn = fy_get(tool, "function");
				name = fy_get(fn, "name", "");
				if (!fyai_skill_tool_allowed(ctx, name) ||
				    (fyai_mcp_deferred(ctx) &&
				     !fyai_mcp_tool_active(ctx, name)))
					continue;
				converted = fy_append(ctx->gb, converted,
						fyai_grammar_tool_def(ctx, tool));
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
	fy_generic skill_msg;
	char *prompt = NULL, *shown = NULL;
	char *instr;
	size_t i;
	int rc;

	fyai_prof_stamp(&t_setup);

	memset(ctx, 0, sizeof(*ctx));
	ctx->cfg = cfg;
	ctx->auth.registrations = fy_invalid;
	/* A holder that an ancestor started is in the environment, which a tool clears. */
	fyai_cover_init(&ctx->cover);

	ctx->stdout_tty = terminal_is_tty(STDOUT_FILENO);
	if (!terminal_window_size(STDOUT_FILENO, &ctx->tty_rows, &ctx->tty_cols))
		(void)terminal_window_size(STDERR_FILENO, &ctx->tty_rows,
					   &ctx->tty_cols);

	/* Before anything can render: every later step reports through it. */
	ctx->sink = fyai_sink_create(ctx);
	if (!ctx->sink)
		goto err;

	ctx->tools = fy_invalid;
	ctx->mcp_active_tools = fy_invalid;
	ctx->tools_spec = fy_invalid;
	ctx->last_message = fy_invalid;
	ctx->usage_head = fy_invalid;
	ctx->usage_agents_root = fy_invalid;
	ctx->arena_config = fy_invalid;
	ctx->arena_catalog = fy_invalid;
	ctx->arena_skills = fy_invalid;
	ctx->tools_allow = fy_invalid;
	ctx->output_schema = fy_invalid;
	ctx->structured_output = fy_invalid;
	/* A zeroed fy_generic is an empty sequence, not fy_invalid. */
	ctx->arena_branches = fy_invalid;
	ctx->branch_prev = fy_invalid;
	ctx->branch_desc = fy_invalid;
	ctx->branch_agent = fy_invalid;
	ctx->branch_store = fy_invalid;
	ctx->project_state = fy_invalid;
	ctx->tool_changes = fy_seq_empty;
	ctx->last_token_extents = fy_invalid;
	ctx->media_pending = fy_seq_empty;
	if (fyai_signals_open(ctx))
		goto err;

	/* A verb whose storage is optional runs without an arena that is not
	 * there, and asks no question about making one. */
	if (!fyai_cfg_no_storage(cfg) &&
	    !(fyai_cfg_storage_optional(cfg) && access(cfg->arena_dir, F_OK))) {
		if (fyai_setup_storage(ctx))
			goto err;
	}
	/*
	 * The images that --image names wait for the first message, so both
	 * the prompt of a verb and the first line of a session carry them.
	 * They are attached here, after the arena is open: the bytes are
	 * published beside that arena.
	 */
	for (i = 0; i < cfg->image_count; i++) {
		if (fyai_media_attach_path(ctx, cfg->images[i], NULL, NULL))
			goto err;
	}

	if (fyai_cfg_no_requests(cfg))
		return 0;

	rc = asprintf(&ctx->user_agent, "%s/%s", "fyai", VERSION);
	if (rc == -1)
		goto err;

	if (fyai_curl_easy_reinit(ctx))
		goto err;

	/* A supervisor sends its model requests through the transport. */
	if (fyai_transport_attach(ctx))
		goto err;

	(void)fyai_setup_transient_builder(ctx);
	/* An executed tool child resolves when the parent state arrives. */
	if (cfg->tool_exec)
		return 0;
	if (fyai_auth_resolve(ctx))
		goto err;

	rc = fyai_request_state_apply(ctx);
	if (rc)
		goto err;

	/* Start MCP here for non-interactive modes and apply its tools. */
	if (!cfg->interactive && !cfg->agent_child) {
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
	 * switch and shows up in `history`/`dump state`. A grammar that carries
	 * it in a field of its own, such as `instructions` or `system`, drops it
	 * from the input.
	 */
	if (fy_is_invalid(ctx->last_message)) {
		ctx->last_message = fyai_turn_append(ctx, ctx->last_message,
			fy_sequence(fyai_make_system_message(ctx, cfg->system_prompt)));
		ctx->last_message = fyai_output_record(ctx, ctx->last_message,
			FYAI_OUTPUT_SYSTEM, cfg->system_prompt);
	}

	if (cfg->prompt && *cfg->prompt) {
		prompt = strdup(cfg->prompt);
		fyai_error_check(ctx, prompt, err, "out of memory");
		/*
		 * The @PATH mentions attach only in the prompt the user typed:
		 * the task of a sub-agent is not a line of the user, and an
		 * attachment never leaves with the instruction of one. A prompt
		 * that starts with "/" names a command and has none.
		 */
		if (cfg->cmd.run == FYAI_RUN_PROMPT && !cfg->agent_child &&
		    !cfg->agent_rpc && !cfg->tool_exec && prompt[0] != '/' &&
		    fyai_media_take_mentions(ctx, prompt))
			goto err;
		/* A prompt "/NAME ARGS" that names a skill runs the skill. */
		skill_msg = fyai_skill_user_message(ctx,
				fyai_ctx_transient_gb(ctx), prompt);
		fyai_error_check(ctx, fy_is_string(skill_msg) ||
				 !fyai_skill_exec_line(prompt), err,
				 "no enabled skill for '%s' on branch '%s'",
				 prompt, fyai_ctx_branch(ctx));
		shown = fyai_media_pending_display(ctx, prompt);
		ctx->last_message = fyai_turn_append(ctx, ctx->last_message,
			fy_sequence(fyai_make_user_message(ctx,
				fy_is_string(skill_msg) ?
				fy_castp(&skill_msg, prompt) : prompt)));
		ctx->last_message = fyai_output_record(ctx, ctx->last_message,
			FYAI_OUTPUT_USER, shown ? shown : prompt);
		free(shown);
		shown = NULL;
		free(prompt);
		prompt = NULL;
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
	free(shown);
	free(prompt);
	return -1;
}

/*
 * Events queued for the model in arrival order. A wait event names its
 * owner: the event reaches the model turns after the poll that queued it,
 * and the condition that produced it may be gone by then. The owner pairs
 * the event with the liveness of the job or session it came from: a stale
 * wait is dropped instead of starting a turn for a program that has ended.
 */
struct fyai_pending_event {
	struct fyai_pending_event *next;
	char *text;
	char *from, *message_id, *reply_to;
	long long execution;
	char *owner;
	enum fyai_event_owner_kind owner_kind;
	bool seen;		/* a waiter has it: the turn loop leaves it */
};

/* The context takes @text and @owner; both are freed on every path. */
static int fyai_event_inject_queue(struct fyai_ctx *ctx, char *text,
				   enum fyai_event_owner_kind owner_kind,
				   char *owner, const char *from,
				   long long execution,
				   const char *message_id,
				   const char *reply_to)
{
	struct fyai_pending_event *ev;

	if (!ctx || !text) {
		free(text);
		free(owner);
		return -1;
	}
	ev = calloc(1, sizeof(*ev));
	fyai_error_check(ctx, ev, err, "could not queue event");
	ev->text = text;
	if (from) {
		ev->from = strdup(from);
		ev->message_id = strdup(message_id ? message_id : "");
		ev->reply_to = strdup(reply_to ? reply_to : "");
		fyai_error_check(ctx, ev->from && ev->message_id && ev->reply_to,
				 err_event, "could not copy event sender metadata");
		ev->execution = execution;
	}
	ev->owner_kind = owner_kind;
	ev->owner = owner;
	if (!ctx->events_tail)
		ctx->events_tail = &ctx->events;
	*ctx->events_tail = ev;
	ctx->events_tail = &ev->next;
	fyai_waiters_kick(ctx);
	return 0;

err_event:
	free(ev->from);
	free(ev->message_id);
	free(ev->reply_to);
	free(ev);
err:
	free(text);
	free(owner);
	return -1;
}

int fyai_event_inject(struct fyai_ctx *ctx, char *text)
{
	return fyai_event_inject_queue(ctx, text, FYAI_EVENT_OWNER_NONE,
				       NULL, NULL, 0, NULL, NULL);
}

int fyai_event_inject_agent(struct fyai_ctx *ctx, const char *text,
			    const char *from, long long execution,
			    const char *message_id, const char *reply_to)
{
	char *copy = text ? strdup(text) : NULL;

	return fyai_event_inject_queue(ctx, copy, FYAI_EVENT_OWNER_NONE,
		NULL, from, execution, message_id, reply_to);
}

int fyai_event_injectf(struct fyai_ctx *ctx, const char *fmt, ...)
{
	va_list ap;
	char *text;
	int rc;

	va_start(ap, fmt);
	rc = vasprintf(&text, fmt, ap);
	va_end(ap);
	fyai_error_check(ctx, rc >= 0, err, "could not format an event");
	return fyai_event_inject(ctx, text);
err:
	return -1;
}

/* Queue owned @text for the loop owner; the caller gives away @owner. */
int fyai_event_inject_owned(struct fyai_ctx *ctx, char *text,
			    enum fyai_event_owner_kind owner_kind,
			    char *owner)
{
	return fyai_event_inject_queue(ctx, text, owner_kind, owner,
		NULL, 0, NULL, NULL);
}

void fyai_event_input_clear(struct fyai_event_input *input)
{
	if (!input)
		return;
	free(input->text);
	free(input->from);
	free(input->message_id);
	free(input->reply_to);
	memset(input, 0, sizeof(*input));
}

static void fyai_pending_event_free(struct fyai_pending_event *ev)
{
	if (!ev)
		return;
	free(ev->text);
	free(ev->from);
	free(ev->message_id);
	free(ev->reply_to);
	free(ev->owner);
	free(ev);
}

char *fyai_event_take(struct fyai_ctx *ctx)
{
	struct fyai_pending_event *ev;
	char *text;

	if (!ctx || !ctx->events)
		return NULL;
	ev = ctx->events;
	ctx->events = ev->next;
	if (!ctx->events)
		ctx->events_tail = &ctx->events;
	text = ev->text;
	ev->text = NULL;
	fyai_pending_event_free(ev);
	return text;
}

bool fyai_event_queued(const struct fyai_ctx *ctx)
{
	const struct fyai_pending_event *ev;

	for (ev = ctx ? ctx->events : NULL; ev; ev = ev->next)
		if (!ev->seen)
			return true;
	return false;
}

/*
 * Whether a queued wait event still names a live owner. Unowned events
 * (waits, agent control input) are not wait reports and are always
 * delivered. A wait whose owner settled is dropped with a trace record.
 */
static bool fyai_event_owner_live(struct fyai_ctx *ctx,
				  const struct fyai_pending_event *ev)
{
	if (!ev || ev->owner_kind == FYAI_EVENT_OWNER_NONE)
		return true;
	if (ev->owner_kind == FYAI_EVENT_OWNER_AGENT)
		return fyai_event_agent_live(ctx, ev->owner);
	return fyai_event_session_live(ctx, ev->owner);
}

bool fyai_event_pending_prefix(struct fyai_ctx *ctx, const char *prefix)
{
	const struct fyai_pending_event *ev;
	size_t len = strlen(prefix);

	for (ev = ctx ? ctx->events : NULL; ev; ev = ev->next)
		if (!strncmp(ev->text, prefix, len) &&
		    fyai_event_owner_live(ctx, ev))
			return true;
	return false;
}

char *fyai_event_peek_prefix(struct fyai_ctx *ctx, const char *prefix)
{
	struct fyai_pending_event *ev;
	size_t len = strlen(prefix);
	char *text;

	for (ev = ctx ? ctx->events : NULL; ev; ev = ev->next) {
		if (strncmp(ev->text, prefix, len) ||
		    !fyai_event_owner_live(ctx, ev))
			continue;
		text = strdup(ev->text);
		if (text)
			ev->seen = true;
		return text;
	}
	return NULL;
}

void fyai_events_purge_seen(struct fyai_ctx *ctx)
{
	struct fyai_pending_event **link, *ev;

	if (!ctx)
		return;
	link = &ctx->events;
	while ((ev = *link)) {
		if (!ev->seen) {
			link = &ev->next;
			continue;
		}
		*link = ev->next;
		if (!*link)
			ctx->events_tail = link;
		fyai_pending_event_free(ev);
	}
}

/* Drop every queued wait event owned by @name of @kind. */
static void fyai_events_drop_owner(struct fyai_ctx *ctx,
				   enum fyai_event_owner_kind kind,
				   const char *name)
{
	struct fyai_pending_event **link, *ev;

	if (!ctx || kind == FYAI_EVENT_OWNER_NONE || !name || !*name)
		return;
	link = &ctx->events;
	while ((ev = *link) != NULL) {
		if (ev->owner_kind != kind || !ev->owner ||
		    strcmp(ev->owner, name)) {
			link = &ev->next;
			continue;
		}
		*link = ev->next;
		if (!*link)
			ctx->events_tail = link;
		fyai_diag_tracef("event", "dropped stale %s wait from '%s'",
				 kind == FYAI_EVENT_OWNER_AGENT ?
				 "agent" : "session", name);
		fyai_pending_event_free(ev);
	}
}

void fyai_background_reset(struct fyai_ctx *ctx)
{
	if (!ctx)
		return;
	fyai_agent_background_close(ctx);
	fyai_monitor_close(ctx);
	fyai_asks_release(ctx);
	fyai_waits_release(ctx);
	fyai_events_release(ctx);
}

void fyai_events_release(struct fyai_ctx *ctx)
{
	char *text;

	while ((text = fyai_event_take(ctx)))
		free(text);
}

/* Drop the queued wait events of the sub-agent on @branch. */
void fyai_events_drop_agent(struct fyai_ctx *ctx, const char *branch)
{
	fyai_events_drop_owner(ctx, FYAI_EVENT_OWNER_AGENT, branch);
}

/* Drop the queued wait events of the shell session @name. */
void fyai_events_drop_session(struct fyai_ctx *ctx, const char *name)
{
	fyai_events_drop_owner(ctx, FYAI_EVENT_OWNER_SESSION, name);
}

/*
 * Take the oldest queued event whose owner is still live, dropping stale
 * wait events in front of it. A wait event names a job or session that may
 * have settled in the gap between the poll that queued it and this turn:
 * starting a turn for it would ask the model to answer a program that has
 * ended. Each drop is traced; delivery of an unowned event never changes.
 */
bool fyai_event_take_live_input(struct fyai_ctx *ctx,
				struct fyai_event_input *input)
{
	struct fyai_pending_event **link, *ev;

	memset(input, 0, sizeof(*input));
	if (!ctx)
		return false;
	link = &ctx->events;
	while ((ev = *link)) {
		if (ev->seen) {
			link = &ev->next;
			continue;
		}
		*link = ev->next;
		if (!*link)
			ctx->events_tail = link;
		if (fyai_event_owner_live(ctx, ev)) {
			input->text = ev->text;
			input->from = ev->from;
			input->message_id = ev->message_id;
			input->reply_to = ev->reply_to;
			input->execution = ev->execution;
			ev->text = ev->from = ev->message_id = ev->reply_to = NULL;
			fyai_pending_event_free(ev);
			return true;
		}
		fyai_diag_tracef("event", "dropped stale %s wait from '%s'",
				 ev->owner_kind == FYAI_EVENT_OWNER_AGENT ?
				 "agent" : "session",
				 ev->owner ? ev->owner : "?");
		fyai_pending_event_free(ev);
	}
	return false;
}

char *fyai_event_take_live(struct fyai_ctx *ctx)
{
	struct fyai_event_input input;
	char *text;

	if (!fyai_event_take_live_input(ctx, &input))
		return NULL;
	text = input.text;
	input.text = NULL;
	fyai_event_input_clear(&input);
	return text;
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
	fy_generic result, diag;
	int rc;

	run = *runp;
	result = fyai_turn_run_collect(run);
	fyai_turn_run_destroy(run);
	*runp = NULL;
	fyai_ui_set_busy(ctx, false);
	/* add a cause if neither the result nor the sink has one */
	diag = fy_generic_get_diag(result);
	if (fy_is_invalid(result) && (!fy_is_valid(diag) || fy_is_null(diag)) &&
	    !fyai_diag_got_error(&ctx->cfg->diag))
		fyai_error(ctx, "the turn did not complete");
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
/*
 * True when this invocation opens the resume picker: the resume verb with no
 * branch and no --last. Direct and --last selection already happened, before
 * the configuration was loaded.
 */
static bool fyai_resume_picker(const struct fyai_cfg *cfg)
{
	const struct fyai_cmd_state *st = &cfg->cmd.reg;

	return cfg->cmd.run == FYAI_RUN_CMD && fyai_cmd_state_is(st, "resume") &&
	       !fy_is_valid(fy_get(st->args, "session", fy_invalid)) &&
	       !fy_get(st->args, "last", false);
}

static void fyai_interactive_prepare(struct fyai_ctx *ctx,
				     const char *histfile, bool banner)
{
	fyai_ui_history_load(ctx, histfile);
	/* The picker is the screen until a session is chosen, so the history
	 * of the branch that HEAD names is not replayed under it. */
	if (!fyai_resume_picker(ctx->cfg))
		fyai_interactive_recap(ctx);
	fyai_ui_drain_output(ctx);
	if (banner)
		fyai_session_banner_update(ctx);
}

/*
 * Store @line in the conversation: a turn with no messages, whose display
 * record draws the card of the command and, with display/command_output set
 * to transcript, the output that the command drew. The model never sees it.
 * A command that moved the head, or changed the branch, is recorded by the
 * operation that it did, and not here.
 */
static void fyai_interactive_record_command(struct fyai_ctx *ctx,
					    const char *line, fy_generic head,
					    const char *branch)
{
	struct fy_generic_builder *gb;
	fy_generic turn, record;
	char *output;

	output = fyai_ui_pane_take(ctx);
	/* A session that holds no exchange yet is not stored for a command. */
	if (ctx->session_unstored ||
	    ctx->last_message.v != head.v || fy_is_invalid(head) ||
	    !ctx->branch || !branch || strcmp(ctx->branch, branch))
		goto out;
	gb = fyai_ctx_transient_gb(ctx);
	fyai_error_check(ctx, gb, out, "could not store the command %s", line);
	record = fy_null_filtered_mapping(gb,
		"tag", "command",
		"markdown", line,
		"output", output ? fy_value(gb, output) : fy_null);
	turn = fyai_turn_append(ctx, head, fy_seq_empty);
	turn = fyai_turn_append_display_output(ctx, turn, record);
	fyai_error_check(ctx, fy_is_valid(turn), out,
			 "could not store the command %s", line);
	ctx->last_message = fy_gb_internalize(ctx->gb, turn);
	fyai_error_check(ctx, fy_is_valid(ctx->last_message), err_restore,
			 "could not store the command %s", line);
	fyai_branch_op_set(ctx, FYAI_BRANCH_OP_COMMAND, NULL);
	if (fyai_publish_state(ctx))
		fyai_error(ctx, "could not publish the command %s on branch %s",
			   line, branch);
out:
	free(output);
	return;
err_restore:
	ctx->last_message = head;
	free(output);
}

/*
 * Handle a slash command. Return -1 when @line is an ordinary user turn.
 * With display/command_output set to transcript, the command is a part of the
 * transcript: its card is drawn and, with @record, the command is stored. A
 * command beside a turn is not stored, because the turn owns the head. With
 * pane, the output goes to the pane and the transcript keeps nothing.
 */
static int fyai_interactive_handle_slash(struct fyai_ctx *ctx,
					 const char *histfile, char *line,
					 bool record)
{
	fy_generic head;
	char *branch;
	bool transcript, status;
	int rc;

	if (line[0] != '/' || line[1] == '/')
		return -1;
	/* A skill that the user runs is a turn of the conversation. */
	if (fyai_skill_user_line(ctx, line))
		return -1;
	status = fyai_cmd_session_is_status(line + 1);
	transcript = !status && !strcmp(ctx->cfg->command_output, "transcript");
	fyai_ui_history_save(ctx, histfile, line);
	if (transcript)
		fyai_echo_user_turn(ctx, line);
	head = ctx->last_message;
	branch = ctx->branch ? strdup(ctx->branch) : NULL;
	fyai_ui_pane_status(ctx, status);
	rc = fyai_session_slash(ctx, line);
	fyai_ui_pane_status(ctx, false);
	/* /exit and /quit end the session and store nothing. */
	if (transcript && record && rc == 0 && branch)
		fyai_interactive_record_command(ctx, line, head, branch);
	else
		free(fyai_ui_pane_take(ctx));
	free(branch);
	fyai_error_check(ctx, rc >= 0, out,
			 "could not run the slash command");
	if (transcript && ctx->cfg->markdown && ctx->stdout_tty)
		(void)fyai_sink_write(ctx->sink, FYAI_SINK_TRANSCRIPT, "\n", 1);
out:
	fyai_ui_drain_output(ctx);
	return rc < 0 ? rc : rc > 0 ? 1 : 0;
}

/*
 * A turn owns ctx->transient_gb and the collected diagnostics. A line
 * that runs beside the turn must use neither. Enter sets both aside and
 * gives the line scratch storage. Leave restores them.
 */
struct fyai_scratch_state {
	struct fy_generic_builder *gb;
	struct fy_allocator *allocator;
	fy_generic held;
	bool autorelease;
	bool entered;
};

static int fyai_scratch_enter(struct fyai_ctx *ctx,
			      struct fyai_scratch_state *st)
{
	int rc;

	st->gb = ctx->transient_gb;
	st->allocator = ctx->transient_allocator;
	st->autorelease = ctx->transient_autorelease;
	st->held = fy_invalid;
	st->entered = false;
	ctx->transient_gb = NULL;
	ctx->transient_allocator = NULL;
	ctx->transient_autorelease = false;
	rc = fyai_setup_transient_builder(ctx);
	if (rc) {
		ctx->transient_gb = st->gb;
		ctx->transient_allocator = st->allocator;
		ctx->transient_autorelease = st->autorelease;
		return rc;
	}
	/* A drain resets the sink storage, so hold the turn's diagnostics. */
	st->held = fyai_diag_take_generic(&ctx->cfg->diag, ctx->transient_gb);
	st->entered = true;
	return 0;
}

/* Restore the turn state. Call after the line drains its own diagnostics. */
static void fyai_scratch_leave(struct fyai_ctx *ctx,
			       struct fyai_scratch_state *st)
{
	if (!st->entered)
		return;
	st->entered = false;
	/* Raise the held diagnostics again before the scratch builder goes. */
	if (fy_is_sequence(st->held))
		fyai_diag_adopt(&ctx->cfg->diag, st->held, NULL);
	st->held = fy_invalid;
	fyai_cleanup_transient_builder(ctx);
	ctx->transient_gb = st->gb;
	ctx->transient_allocator = st->allocator;
	ctx->transient_autorelease = st->autorelease;
}

/* Run a bang command without creating a model turn. It is not a part of the
 * conversation: nothing of it goes into the transcript. */
static int fyai_interactive_handle_bang(struct fyai_ctx *ctx,
					const char *histfile, char *line)
{
	struct fyai_scratch_state st;
	int rc;

	if (line[0] != '!')
		return -1;
	fyai_ui_history_save(ctx, histfile, line);
	rc = fyai_scratch_enter(ctx, &st);
	fyai_error_check(ctx, !rc, out,
			 "could not create transient shell storage");
	rc = fyai_tools_bang(ctx, line + 1);
	fyai_error_check(ctx, !rc, out,
			 "could not start the bang shell");
	rc = 0;
out:
	fyai_ui_diag_drain(ctx, "error");
	fyai_ui_drain_output(ctx);
	fyai_scratch_leave(ctx, &st);
	return rc;
}

/*
 * Normalize and display a normal interactive user line. Its @PATH mentions
 * attach their files; -1 when one cannot, and then the line is not sent. The
 * history keeps the line as typed, so it can be fixed and sent again.
 */
static int fyai_interactive_prepare_user_line(struct fyai_ctx *ctx,
					      const char *histfile, char *line)
{
	if (line[0] == '/' && line[1] == '/')
		memmove(line, line + 1, strlen(line));
	fyai_ui_history_save(ctx, histfile, line);
	if (fyai_media_take_mentions(ctx, line)) {
		fyai_ui_diag_drain(ctx, "error");
		return -1;
	}
	fyai_echo_user_turn(ctx, line);
	fyai_ui_drain_output(ctx);
	return 0;
}

/*
 * Append the user line to the conversation. A line "/NAME ARGS" that names a
 * skill gives the model the loaded skill, and the transcript keeps the line
 * as typed.
 */
static fy_generic fyai_interactive_append_user_turn(struct fyai_ctx *ctx,
						    char *line)
{
	fy_generic turn, message;
	const char *text;
	char *shown;

	shown = fyai_media_pending_display(ctx, line);
	message = fyai_skill_user_message(ctx, fyai_ctx_transient_gb(ctx), line);
	text = fy_is_string(message) ? fy_castp(&message, line) : line;
	turn = fyai_turn_append(ctx, ctx->last_message,
			fy_sequence(fyai_make_user_message(ctx, text)));
	turn = fyai_output_record(ctx, turn, FYAI_OUTPUT_USER,
				  shown ? shown : line);
	free(shown);
	return turn;
}

static int
fyai_interactive_start_line(struct fyai_ctx *ctx, const char *histfile,
			    char *line, struct fyai_turn_run **runp)
{
	struct fyai_turn_run *run;
	fy_generic turn;
	int rc;

	rc = fyai_interactive_handle_slash(ctx, histfile, line, true);
	if (rc >= 0)
		return rc;
	if (!fyai_credential_available(ctx)) {
		fyai_error(ctx, "no API key or ChatGPT login is available");
		fyai_ui_diag_drain(ctx, "error");
		return 0;
	}
	if (fyai_interactive_prepare_user_line(ctx, histfile, line))
		return 0;
	rc = fyai_setup_transient_builder(ctx);
	fyai_error_check(ctx, !rc, err,
			 "could not create transient turn storage");
	turn = fyai_interactive_append_user_turn(ctx, line);
	fyai_error_check(ctx, fy_is_valid(turn), err_cleanup,
			 "could not append the user turn");
	run = fyai_turn_run_submit(ctx, turn);
	fyai_error_check(ctx, run, err_cleanup,
			 "could not submit interactive turn");
	run->accept_pending_user = true;
	run->histfile = histfile;
	*runp = run;
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

/*
 * Commit a finished catalogue update between turns: the commit publishes the
 * branch store and derives the model again, which a turn must not see change.
 */
static void fyai_interactive_catalog_update_step(struct fyai_ctx *ctx)
{
	int rc;

	if (!fyai_catalog_update_done(ctx->catalog_update))
		return;
	rc = fyai_catalog_update_collect(ctx->catalog_update);
	fyai_catalog_update_destroy(ctx->catalog_update);
	ctx->catalog_update = NULL;
	if (!rc)
		rc = fyai_config_rederive(ctx);
	/* A failed update leaves the catalogue as it was; the session goes on. */
	fyai_ui_diag_drain(ctx, "catalog");
}

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
	bool delivered;

	if (!ctx->interrupt_pending)
		return;
	/* Acknowledge SIGINT while the event loop is responsive. */
	fyai_event_interrupt_ack(ctx);
	delivered = false;
	/* Process each delivered SIGINT one time. */
	if (ctx->interrupt_seq != ctx->interrupt_seen) {
		ctx->interrupt_seen = ctx->interrupt_seq;
		/* A tile with the keys was given it: the user was typing into
		 * a program, not stopping the turn. */
		delivered = fyai_ui_interrupt(ctx);
	}
	if (run && !delivered)
		fyai_turn_run_cancel(run);
	/* A command that waits, such as a login, is stopped the same way. */
	if (!delivered)
		fyai_cmd_session_interrupt(ctx);
	/* Keep a signal that arrived while this interrupt was handled. */
	ctx->interrupt_pending = ctx->interrupt_seq != ctx->interrupt_seen;
}

static int fyai_interactive_submit_initial(struct fyai_ctx *ctx,
					   const char *histfile,
					   struct fyai_turn_run **runp)
{
	struct fyai_turn_run *run;
	int rc;

	rc = fyai_setup_transient_builder(ctx);
	fyai_error_check(ctx, !rc, err,
			 "could not create transient initial storage");
	run = fyai_turn_run_submit(ctx, ctx->last_message);
	fyai_error_check(ctx, run, err_cleanup,
			 "could not submit the initial turn");
	run->accept_pending_user = true;
	run->histfile = histfile;
	*runp = run;
	fyai_ui_set_busy(ctx, true);
	return 0;
err_cleanup:
	fyai_cleanup_transient_builder(ctx);
err:
	return -1;
}

/* Submit a queued event with its source. */
static int fyai_interactive_submit_event(struct fyai_ctx *ctx,
					 const struct fyai_event_input *input,
					 const char *histfile,
					 struct fyai_turn_run **runp)
{
	struct fyai_turn_run *run;
	fy_generic turn;
	int rc;

	if (!input->from)
		fyai_echo_user_turn(ctx, input->text);
	fyai_ui_drain_output(ctx);
	rc = fyai_setup_transient_builder(ctx);
	fyai_error_check(ctx, !rc, err,
			 "could not create transient event storage");
	turn = fyai_turn_append_event(ctx, ctx->last_message, input);
	fyai_error_check(ctx, fy_is_valid(turn), err_cleanup,
			 "could not append the event turn");
	run = fyai_turn_run_submit(ctx, turn);
	fyai_error_check(ctx, run, err_cleanup,
			 "could not submit the event turn");
	run->accept_pending_user = true;
	run->histfile = histfile;
	*runp = run;
	fyai_ui_set_busy(ctx, true);
	return 0;

err_cleanup:
	fyai_cleanup_transient_builder(ctx);
err:
	return -1;
}

/*
 * Whether a line may run while a turn is in flight. A bang shell may: it
 * runs on scratch storage and is not part of the turn. A slash may when it
 * only reads stored state.
 */
static bool fyai_interactive_line_runs_beside(struct fyai_ctx *ctx,
					      const char *line)
{
	if (line[0] == '!')
		return true;
	if (line[0] != '/' || line[1] == '/')
		return false;
	/* A skill is a turn: it waits for the turn in flight. */
	if (fyai_skill_user_line(ctx, line))
		return false;
	return fyai_session_slash_immediate(ctx, line, true);
}

/* Run a line beside a live turn. FYAILR_QUIT if it asked to leave. */
static enum fyai_line_result fyai_interactive_read_busy(struct fyai_ctx *ctx,
						const char *histfile,
						char *line)
{
	struct fyai_scratch_state st;
	int rc;

	if (line[0] == '!') {
		rc = fyai_interactive_handle_bang(ctx, histfile, line);
		if (rc)
			return FYAILR_ERROR;
		return FYAILR_HANDLED;
	}
	rc = fyai_scratch_enter(ctx, &st);
	if (rc) {
		fyai_error(ctx, "could not create transient command storage");
		fyai_ui_diag_drain(ctx, "error");
		return FYAILR_ERROR;
	}
	rc = fyai_interactive_handle_slash(ctx, histfile, line, false);
	fyai_scratch_leave(ctx, &st);
	if (rc > 0)
		return FYAILR_QUIT;
	if (rc < 0)
		return FYAILR_ERROR;
	return FYAILR_HANDLED;
}

static enum fyai_line_result fyai_interactive_read_line(struct fyai_ctx *ctx,
					      const char *histfile,
					      struct fyai_turn_run **runp)
{
	enum fyai_line_result busy;
	const char *head;
	char *line;
	int rc;

	/*
	 * A line typed during a turn runs now only if it cannot disturb the
	 * turn. Anything else stays in the queue until the turn ends.
	 */
	if (*runp) {
		head = fyai_ui_peek_line(ctx);
		if (!head || !fyai_interactive_line_runs_beside(ctx, head))
			return FYAILR_NONE;
		line = fyai_ui_take_line(ctx);
		if (!line)
			return FYAILR_NONE;
		busy = fyai_interactive_read_busy(ctx, histfile, line);
		free(line);
		return busy;
	}
	line = fyai_ui_take_line(ctx);
	if (!line)
		return FYAILR_NONE;
	rc = line[0] == '!' ?
		fyai_interactive_handle_bang(ctx, histfile, line) : -1;
	if (line[0] == '!') {
		free(line);
		return rc ? FYAILR_ERROR : FYAILR_HANDLED;
	}
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
	int step_rc;

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
	if (fyai_resume_picker(cfg) &&
	    fyai_browser_open_resume(ctx, cfg->cmd.args.resume.all)) {
		fyai_error(ctx, "could not open the session picker");
		goto out;
	}
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
		fyai_browser_step(ctx);

		rc = fyai_interactive_config_edit_step(ctx);
		fyai_error_check(ctx, !rc, out,
				 "could not finish the configuration edit");
		if (!run)
			fyai_interactive_catalog_update_step(ctx);

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
			rc = fyai_interactive_submit_initial(ctx, histfile, &run);
			fyai_error_check(ctx, !rc, out,
					 "could not submit initial turn");
		}

		/* Submit events only between turns. */
		if (!run && gated && !ctx->config_edit && !ctx->cmd_call &&
		    fyai_event_queued(ctx)) {
			/*
			 * A wait whose owner settled since the poll is
			 * dropped, not submitted: answering it would ask
			 * the model about a program that has ended.
			 */
			struct fyai_event_input event;

			if (!fyai_event_take_live_input(ctx, &event))
				continue;

			rc = fyai_interactive_submit_event(ctx, &event,
						   histfile, &run);
			fyai_event_input_clear(&event);
			fyai_error_check(ctx, !rc, out,
					 "could not submit an event turn");
			continue;
		}

		/*
		 * Read while idle, and while a turn runs when a line waits:
		 * an immediate slash runs beside the turn while a mutation
		 * stays queued behind it. Without a waiting line the read
		 * would only block the pump.
		 */
		/* A finished async command presents between turns. */
		if (!run)
			(void)fyai_cmd_session_step(ctx);
		if ((!run || fyai_ui_has_line(ctx)) && gated &&
		    !ctx->config_edit && !ctx->cmd_call) {
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
		/* A side question may complete after its parent turn releases scratch. */
		if ((ctx->btw_runs || ctx->agent_bg_runs || ctx->monitors) &&
		    !ctx->transient_gb) {
			rc = fyai_setup_transient_builder(ctx);
			fyai_error_check(ctx, !rc, out,
					 "could not retain side question storage");
		}
		rc = fyai_event_loop_step(el, -1);
		fyai_error_check(ctx, rc >= 0, out,
				 "interactive event loop failed");
	}
	rc = 0;
out:
	/* Cancel active work and drain MCP shutdown on every exit path. */
	state = FYAIAS_STOPPING;
	fyai_session_btw_close(ctx);
	fyai_agent_background_close(ctx);
	fyai_monitor_close(ctx);
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
	fyai_catalog_update_destroy(ctx->catalog_update);
	ctx->catalog_update = NULL;
	fyai_cmd_session_cancel(ctx);
	/* Reclaim the active turn's builder or a lingering idle scratch one. */
	fyai_cleanup_transient_builder(ctx);
	if (!fyai_mcp_stop(ctx)) {
		while (!fyai_mcp_stop_done(ctx)) {
			step_rc = fyai_event_loop_step(el, -1);
			if (step_rc < 0) {
				rc = step_rc;
				break;
			}
		}
		fyai_mcp_stop_finish(ctx);
	}
	/* A fullscreen session gives the terminal its screen back as it was,
	 * and prints nothing there: the conversation is stored. A live session
	 * commits its tile when it goes, so it goes before the UI that holds
	 * the tile. */
	if (fyai_ui_fullscreen(ctx)) {
		fyai_shell_sessions_release(ctx, false);
		fyai_ui_close(ctx);
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

	/* The picker is a screen the user chooses on; without a terminal there
	 * is nothing to choose with, and a session must be named instead. */
	if (fyai_resume_picker(cfg)) {
		fyai_error(ctx, "the session picker needs an interactive "
			   "terminal; name a session or use --last");
		goto err_out;
	}

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
		rc = line[0] == '!' ?
			fyai_interactive_handle_bang(ctx, histfile, line) : -1;
		if (line[0] == '!') {
			free(line);
			line = NULL;
			if (rc > 0)
				break;
			continue;
		}
		rc = fyai_interactive_handle_slash(ctx, histfile, line, true);
		if (rc >= 0) {
			free(line);
			line = NULL;
			if (rc > 0)
				break;
			continue;
		}
		if (!fyai_credential_available(ctx)) {
			fyai_error(ctx, "no API key or ChatGPT login is available");
			fyai_ui_diag_drain(ctx, "error");
			free(line);
			line = NULL;
			continue;
		}
		if (fyai_interactive_prepare_user_line(ctx, histfile, line)) {
			free(line);
			line = NULL;
			continue;
		}

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

	if (!cfg->interactive && !fyai_credential_available(ctx)) {
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

/*
 * A bare --set, --get, or --delete run: fyai_run() applied the operations
 * before it executes, so nothing is left to do.
 */
static int execute_config_ops(struct fyai_ctx *ctx)
{
	(void)ctx;
	return 0;
}

static const struct fyai_verb run_prompt = {
	.name	 = "prompt",
	.execute = fyai_prompt,
	.flags	 = FYAIVF_INTERACTIVE,
};

static const struct fyai_verb run_config = {
	.name	 = "config",
	.execute = execute_config_ops,
	.flags	 = FYAIVF_BATCH | FYAIVF_NO_REQUESTS,
};

const struct fyai_verb *fyai_cfg_verb(struct fyai_cfg *cfg)
{
	if (!cfg)
		return NULL;
	switch (cfg->cmd.run) {
	case FYAI_RUN_PROMPT:
		return &run_prompt;
	case FYAI_RUN_CONFIG:
		return &run_config;
	case FYAI_RUN_CMD:
		return &cfg->cmd.reg.verb;
	default:
		return NULL;
	}
}

int fyai_run(struct fyai_cfg *cfg)
{
	struct fyai_ctx ctx;
	const char *config, *session;
	int rc;

	/*
	 * Scoped to cfg, not ctx: a failed setup may not have left one. Each of
	 * these is the stage noticing its callee failed, so it is demoted to
	 * debug whenever the callee said why - and stays the reported error when
	 * nothing else did.
	 */
	rc = fyai_setup(&ctx, cfg);
	fyai_cfg_error_check(cfg, !rc, err_out, "failed to initialize fyai");

	/* Persist/replay any global --set/--delete/--get before the verb runs. */
	rc = fyai_apply_config_ops(&ctx);
	fyai_cfg_error_check(cfg, !rc, err_out, "config operation failed");

	rc = fyai_view_session_bootstrap(&ctx);
	fyai_cfg_error_check(cfg, rc >= 0, err_out, "could not run the session in a view");
	if (rc > 0) {
		rc = 0;
		goto out;
	}
	fyai_config_sandbox_report(&ctx);
	rc = fyai_execute(&ctx);
	fyai_cfg_error_check(cfg, !rc, err_out, "fyai execution failed");
	if (cfg->reload_branch) {
		config = emit_json_string(cfg->gb, cfg->config_doc);
		session = emit_json_string(cfg->gb,
			fy_is_valid(cfg->config_session) ?
			cfg->config_session : fy_null);
		fyai_error_check(&ctx, config && session, err_out,
				 "reload: cannot serialize configuration");
		cfg->reload_config = strdup(config);
		cfg->reload_session = strdup(session);
		cfg->reload_arena = strdup(cfg->arena_dir);
		if (cfg->api_key_explicit && cfg->api_key)
			cfg->reload_key = strdup(cfg->api_key);
		fyai_error_check(&ctx, cfg->reload_config && cfg->reload_session &&
			cfg->reload_arena &&
			(!cfg->api_key_explicit || !cfg->api_key ||
			 cfg->reload_key), err_out,
				 "reload: cannot retain configuration");
	}

out:
	/*
	 * The backstop: err_out falls through here, so one drain covers every
	 * path out of a verb and nothing collected can be dropped unreported.
	 */
	fyai_diag_drain(&cfg->diag);
	fyai_cleanup(&ctx);
	/* Teardown can report after the first drain. */
	fyai_diag_drain(&cfg->diag);
	return rc ? -1 : 0;

err_out:
	rc = -1;
	goto out;
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
