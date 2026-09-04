/*
 * fyai_stream.c - HTTP response transport and SSE assembly
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_STREAM

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "fyai_sink.h"
#include "fyai_markdown.h"
#include "fyai_output.h"
#include "fyai_tools.h"
#include "fyai_log.h"
#include "fyai_provider.h"
#include "fyai_curl.h"
#include "fyai_event.h"
#include "fyai_agent.h"
#include "fyai_stream.h"
#include "fyai_terminal.h"
#include "fyai_ui.h"

struct stream_spinner {
	size_t frame;
	bool active;
};

struct stream_response {
	struct fyai_ctx *ctx;
	struct fyai_curl_transfer *transfer;
	struct fyai_auth_refresh_request *auth_refresh;
	struct fyai_event_source *retry_timer;
	int attempt;			/* attempts made, survives a resubmit */
	/* A transient provider error received inside an HTTP 200 stream. */
	bool retry_hint;
	char *retry_cause;
	bool retry_band;		/* the shared band is ours, across resubmits */
	struct fyai_stream_callbacks callbacks;
	fy_generic result;
	enum fyai_stream_state state;
	struct fy_generic_builder *gb;
	struct stream_spinner spinner;
	struct response_buffer raw;
	struct response_buffer line;
	struct response_buffer reasoning_text;
	size_t reasoning_active_rows;
	fy_generic content_chunks;
	fy_generic tool_calls;
	fy_generic response_items;
	fy_generic emitted_tools;
	fy_generic metadata;
	fy_generic logprob_content;
	fy_generic logprob_refusal;
	fy_generic finish_reason;
	fy_generic usage;
	fy_generic completed_response;
	bool done;
	bool printed_content;
	bool printed_reasoning;
	bool failed;
	bool cancel_requested;
	size_t received_bytes;
	size_t content_bytes;
	size_t content_chunks_count;
	/*
	 * Token extents collected for turn metadata (cfg->token_extents):
	 * {text, pos, lp} per logprob token, or {text, pos} per content chunk
	 * on the fallback paths. extents_pos is the running byte offset into
	 * the joined content, advanced only where extent entries are added -
	 * not the content counters above, which follow every chunk received.
	 */
	fy_generic token_extents;
	size_t extents_pos;
	bool extents_lp;
};

static bool stream_state_terminal(enum fyai_stream_state state)
{
	return state == FYAISS_COMPLETED ||
	       state == FYAISS_CANCELLED ||
	       state == FYAISS_FAILED;
}

static const char *stream_state_name(enum fyai_stream_state state)
{
	static const char * const names[] = {
		[FYAISS_NEW] = "new",
		[FYAISS_SUBMITTED] = "submitted",
		[FYAISS_STREAMING] = "streaming",
		[FYAISS_RETRYING] = "retrying",
		[FYAISS_CANCELLING] = "cancelling",
		[FYAISS_COMPLETED] = "completed",
		[FYAISS_CANCELLED] = "cancelled",
		[FYAISS_FAILED] = "failed",
	};

	if ((unsigned int)state >= ARRAY_SIZE(names) ||
	    !names[state])
		return "unknown";
	return names[state];
}

static bool stream_state_transition_valid(enum fyai_stream_state from,
					  enum fyai_stream_state to)
{
	switch (from) {
	case FYAISS_NEW:
		return to == FYAISS_SUBMITTED || to == FYAISS_FAILED;
	case FYAISS_SUBMITTED:
	case FYAISS_STREAMING:
		return to == FYAISS_STREAMING ||
		       to == FYAISS_RETRYING ||
		       to == FYAISS_CANCELLING ||
		       to == FYAISS_COMPLETED ||
		       to == FYAISS_CANCELLED ||
		       to == FYAISS_FAILED;
	case FYAISS_RETRYING:
		return to == FYAISS_SUBMITTED ||
		       to == FYAISS_CANCELLING ||
		       to == FYAISS_FAILED;
	case FYAISS_CANCELLING:
		return to == FYAISS_CANCELLED || to == FYAISS_FAILED;
	case FYAISS_COMPLETED:
	case FYAISS_CANCELLED:
	case FYAISS_FAILED:
		return false;
	}
	return false;
}

static void stream_state_transition(struct stream_response *stream,
				    enum fyai_stream_state state)
{
	if (!stream_state_transition_valid(stream->state, state)) {
		fyai_error(stream->ctx, "invalid response stream transition "
			   "from %s to %s",
			   stream_state_name(stream->state),
			   stream_state_name(state));
		if (!stream_state_terminal(stream->state))
			stream->state = FYAISS_FAILED;
		return;
	}
	if (stream->ctx->cfg->debug)
		fyai_debug(stream->ctx, "response stream state %s -> %s",
			   stream_state_name(stream->state),
			   stream_state_name(state));
	stream->state = state;
}

static int append_mem(struct response_buffer *buf, const char *data, size_t len)
{
	if (response_buffer_reserve(buf, buf->len + len + 1))
		return -1;

	memcpy(buf->data + buf->len, data, len);
	buf->len += len;
	buf->data[buf->len] = '\0';
	return 0;
}

static void stream_spinner_clear(struct fyai_ctx *ctx,
				 struct stream_spinner *spinner)
{
	if (!spinner->active)
		return;

	(void)fyai_sink_write(ctx->sink, FYAI_SINK_STATUS,
			      FYAI_ANSI_ERASE_LINE,
			      sizeof(FYAI_ANSI_ERASE_LINE) - 1);
	spinner->active = false;
}

static void stream_response_cleanup(struct stream_response *stream)
{
	stream_spinner_clear(stream->ctx, &stream->spinner);
	free(stream->retry_cause);
	free(stream->raw.data);
	free(stream->line.data);
	free(stream->reasoning_text.data);
	if (stream->gb)
		fy_generic_builder_destroy(stream->gb);
}

static int stream_response_init(struct stream_response *stream,
				struct fyai_ctx *ctx)
{
	struct fy_generic_builder_cfg gb_cfg;

	memset(&gb_cfg, 0, sizeof(gb_cfg));
	gb_cfg.flags = FYGBCF_CREATE_ALLOCATOR |
		       FYGBCF_SCOPE_LEADER |
		       FYGBCF_DEDUP_ENABLED;
	gb_cfg.parent = ctx->gb;

	stream->ctx = ctx;
	stream->content_chunks = fy_seq_empty;
	stream->tool_calls = fy_seq_empty;
	stream->response_items = fy_seq_empty;
	stream->emitted_tools = fy_seq_empty;
	stream->metadata = fy_map_empty;
	stream->logprob_content = fy_seq_empty;
	stream->logprob_refusal = fy_invalid;
	stream->finish_reason = fy_invalid;
	stream->usage = fy_invalid;
	stream->completed_response = fy_invalid;
	stream->token_extents = fy_seq_empty;
	stream->result = fy_invalid;
	stream->gb = fy_generic_builder_create(&gb_cfg);
	if (!stream->gb)
		return -1;
	return 0;
}

static bool response_chain_miss(struct fyai_ctx *ctx, long status,
				const char *body)
{
	if (!ctx->response_chain_linked ||
	    ctx->cfg->api_mode != FYAI_API_RESPONSES ||
	    (status != 400 && status != 404) || !body)
		return false;

	/* OpenAI identifies this either through error.param or in the message.
	 * Keep the match narrow so an unrelated endpoint 404 is not replayed. */
	return strstr(body, "previous_response_id") ||
	       strstr(body, "Previous response");
}

static size_t count_newlines(const char *s, size_t len)
{
	size_t i;
	size_t count;

	count = 0;
	for (i = 0; i < len; i++)
		if (s[i] == '\n')
			count++;
	return count;
}

static void stream_write_content(struct stream_response *stream,
				 const char *text)
{
	struct fyai_ctx *ctx = stream->ctx;
	size_t len;

	len = strlen(text);
	/* Record and present the response through the active output document. */
	if (fyai_output_append(ctx, text, len))
		stream->failed = true;
	/* Send sub-agent prose to the parent tool band through JSON-RPC. */
	fyai_tool_progress_emit(ctx, text, len);
	stream->content_bytes += len;
	stream->content_chunks_count++;
	stream->printed_content = true;
}

static int stream_reasoning_markdown(struct stream_response *stream,
				     struct response_buffer *md)
{
	const char *nl;
	const char *p;
	int len;

	if (response_buffer_append(md, "**💭 reasoning**\n\n"))
		return -1;

	p = stream->reasoning_text.data ? stream->reasoning_text.data : "";
	do {
		nl = strchr(p, '\n');
		len = nl ? (int)(nl - p) : (int)strlen(p);
		if (len) {
			if (response_buffer_append(md, "*") ||
			    append_mem(md, p, (size_t)len) ||
			    response_buffer_append(md, "*\n"))
				return -1;
		} else if (response_buffer_append(md, "\n")) {
			return -1;
		}
		p = nl ? nl + 1 : NULL;
	} while (p && *p);
	return response_buffer_append(md, "\n");
}

static int stream_write_reasoning_md(struct stream_response *stream)
{
	struct fyai_ctx *ctx = stream->ctx;
	struct fyai_cfg *cfg = ctx->cfg;
	struct response_buffer md = {0};
	struct response_buffer out = {0};
	size_t rows;
	int rc;

	rc = stream_reasoning_markdown(stream, &md);
	if (!rc)
		rc = markdown_render(cfg, md.data, md.len, &out,
				     ansi_color_on(cfg->color, STDERR_FILENO),
				     cfg->theme_variant);
	free(md.data);
	if (rc) {
		free(out.data);
		return -1;
	}
	if (stream->reasoning_active_rows) {
		(void)fyai_sink_printf(ctx->sink, FYAI_SINK_STATUS,
				       FYAI_ANSI_CURSOR_UP_FMT,
				       stream->reasoning_active_rows);
		(void)fyai_sink_write(ctx->sink, FYAI_SINK_STATUS,
				      FYAI_ANSI_ERASE_DOWN,
				      sizeof(FYAI_ANSI_ERASE_DOWN) - 1);
	}
	rows = count_newlines(out.data, out.len);
	(void)fyai_sink_write(ctx->sink, FYAI_SINK_STATUS, out.data, out.len);
	stream->reasoning_active_rows = rows;
	free(out.data);
	return 0;
}

/*
 * Stream reasoning-summary text live. Reasoning is meta, not the answer, so it
 * goes to stderr, keeping stdout the clean deliverable. Encrypted reasoning
 * blobs are never text and never reach here. No-op when there is no summary
 * text to show.
 */
static void stream_write_reasoning(struct stream_response *stream,
				   const char *text)
{
	struct fyai_ctx *ctx = stream->ctx;
	struct fyai_cfg *cfg = ctx->cfg;
	const char *prefix;
	bool color;

	if (!cfg->thinking)
		return;

	color = ansi_color_on(cfg->color, STDERR_FILENO);
	if (!*text)
		return;
	if (fyai_output_reasoning_append(ctx, text))
		stream->failed = true;
	/* Record delegated reasoning, but do not write it to the terminal. */
	if (fyai_agent_delegated(ctx)) {
		stream->printed_reasoning = true;
		return;
	}
	if (fyai_output_renders_live(ctx)) {
		stream->printed_reasoning = true;
		return;
	}
	if (!stream->printed_reasoning) {
		if (cfg->markdown && markdown_available(cfg) &&
		    terminal_is_tty(STDERR_FILENO)) {
			stream->printed_reasoning = true;
		} else {
			prefix = color ? FYAI_ANSI_DIM "reasoning \xe2\x96\xb8 " :
					 "reasoning > ";

			(void)fyai_sink_write(ctx->sink, FYAI_SINK_STATUS,
					      prefix, strlen(prefix));
			stream->printed_reasoning = true;
		}
	}
	if (cfg->markdown && markdown_available(cfg) &&
	    terminal_is_tty(STDERR_FILENO)) {
		if (response_buffer_append(&stream->reasoning_text, text) ||
		    stream_write_reasoning_md(stream))
			stream->failed = true;
	} else {
		(void)fyai_sink_write(ctx->sink, FYAI_SINK_STATUS, text,
				      strlen(text));
	}
}

/* Close the reasoning section: reset any colour, then the separator. */
static void stream_reasoning_trailer(struct stream_response *stream, bool color)
{
	struct fyai_ctx *ctx = stream->ctx;
	const char *sep = ctx->cfg->section_separator;

	if (color)
		(void)fyai_sink_write(ctx->sink, FYAI_SINK_STATUS,
				      FYAI_ANSI_RESET,
				      sizeof(FYAI_ANSI_RESET) - 1);
	if (*sep)
		(void)fyai_sink_write(ctx->sink, FYAI_SINK_STATUS, sep,
				      strlen(sep));
	(void)fyai_sink_write(ctx->sink, FYAI_SINK_STATUS, "\n\n", 2);
}

static void stream_finish_reasoning(struct stream_response *stream)
{
	struct fyai_ctx *ctx = stream->ctx;
	struct fyai_cfg *cfg = ctx->cfg;
	bool color;

	color = ansi_color_on(cfg->color, STDERR_FILENO);
	if (fyai_output_reasoning_finish(ctx))
		stream->failed = true;
	/* Nothing was printed, so there is no trailer to close. */
	if (fyai_agent_delegated(ctx)) {
		stream->reasoning_active_rows = 0;
		stream->printed_reasoning = false;
		return;
	}
	if (fyai_output_renders_live(ctx)) {
		stream->reasoning_active_rows = 0;
		stream->printed_reasoning = false;
		return;
	}
	if (stream->reasoning_active_rows) {
		stream_reasoning_trailer(stream, false);
		stream->reasoning_active_rows = 0;
		stream->printed_reasoning = false;
		return;
	}
	if (stream->printed_reasoning) {
		stream_reasoning_trailer(stream, color);
		stream->printed_reasoning = false;
	}
}

/* Retire the spinner after the output document receives the content. */
static void stream_finish_content(struct stream_response *stream)
{
	stream_finish_reasoning(stream);
	if (!stream->printed_content)
		return;
	stream_spinner_clear(stream->ctx, &stream->spinner);
	stream->printed_content = false;
}

static int stream_ensure_tool_call(struct stream_response *stream, size_t index)
{
	struct fy_generic_builder *gb = stream->gb;
	fy_generic tool_call;

	while (fy_len(stream->tool_calls) <= index) {
		tool_call = fy_mapping(
			"function", fy_mapping(
				"name_chunks", fy_seq_empty,
				"argument_chunks", fy_seq_empty));
		stream->tool_calls = fy_append(gb, stream->tool_calls, tool_call);
		if (fy_is_invalid(stream->tool_calls))
			return -1;
	}

	return 0;
}

static int stream_apply_tool_call_delta(struct stream_response *stream,
					fy_generic delta_tool_call)
{
	struct fy_generic_builder *gb = stream->gb;
	fy_generic tool_call;
	fy_generic function, delta_function;
	fy_generic chunks;
	const char *text;
	long long index;

	index = fy_get(delta_tool_call, "index", 0LL);
	if (index < 0)
		return -1;

	if (stream_ensure_tool_call(stream, (size_t)index))
		return -1;

	tool_call = fy_get(stream->tool_calls, (size_t)index);
	function = fy_get(tool_call, "function");
	delta_function = fy_get(delta_tool_call, "function");

	text = fy_get(delta_tool_call, "id", "");
	if (*text)
		tool_call = fy_assoc(gb, tool_call, "id", text);

	text = fy_get(delta_tool_call, "type", "");
	if (*text)
		tool_call = fy_assoc(gb, tool_call, "type", text);

	text = fy_get(delta_function, "name", "");
	if (*text) {
		function = fy_get(tool_call, "function");
		chunks = fy_get(function, "name_chunks", fy_seq_empty);
		chunks = fy_append(gb, chunks, text);
		function = fy_assoc(gb, function, "name_chunks", chunks);
		tool_call = fy_assoc(gb, tool_call, "function", function);
	}

	text = fy_get(delta_function, "arguments", "");
	if (*text) {
		function = fy_get(tool_call, "function");
		chunks = fy_get(function, "argument_chunks", fy_seq_empty);
		chunks = fy_append(gb, chunks, text);
		function = fy_assoc(gb, function, "argument_chunks", chunks);
		tool_call = fy_assoc(gb, tool_call, "function", function);
	}

	stream->tool_calls = fy_replace(gb, stream->tool_calls,
					(size_t)index, tool_call);
	return fy_is_invalid(stream->tool_calls) ? -1 : 0;
}

static int stream_emit_tool(struct stream_response *stream, size_t index,
			    fy_generic tool_call)
{
	struct fy_generic_builder *gb;
	fy_generic emitted;
	fy_generic slot;
	const char *key;

	if (!stream->callbacks.tool)
		return 0;
	if (stream->ctx->cfg->api_mode == FYAI_API_CHAT_COMPLETIONS)
		key = fy_get(tool_call, "id", "");
	else
		key = fy_get(tool_call, "call_id", "");
	fy_foreach(emitted, stream->emitted_tools)
		if (*key && fy_equal(emitted, key))
			return 0;
	if (index < fy_len(stream->tool_calls)) {
		slot = fy_get(stream->tool_calls, index);
		if (fy_get(slot, "_emitted", false))
			return 0;
		slot = fy_assoc(stream->gb, slot, "_emitted", true);
		stream->tool_calls = fy_replace(stream->gb, stream->tool_calls,
						index, slot);
		if (fy_is_invalid(stream->tool_calls))
			return -1;
	}
	gb = stream->gb;
	tool_call = fy_gb_internalize(gb, tool_call);
	if (fy_is_invalid(tool_call))
		return -1;
	if (*key)
		stream->emitted_tools = fy_append(gb, stream->emitted_tools,
						  key);
	if (fy_is_invalid(stream->emitted_tools))
		return -1;
	stream->callbacks.tool(stream, tool_call, index,
			       stream->callbacks.userdata);
	return 0;
}

static fy_generic stream_chat_tool_call(struct stream_response *stream,
					size_t index)
{
	struct fy_generic_builder *gb;
	fy_generic tool_call;
	fy_generic function;
	fy_generic name;
	fy_generic arguments;

	gb = stream->gb;
	tool_call = fy_get(stream->tool_calls, index);
	function = fy_get(tool_call, "function");
	name = fyai_join_strings(gb,
			fy_get(function, "name_chunks", fy_seq_empty));
	arguments = fyai_join_strings(gb,
			fy_get(function, "argument_chunks", fy_seq_empty));
	return fy_mapping(gb,
		"id", fy_get(tool_call, "id", ""),
		"type", fy_get(tool_call, "type", "function"),
		"function", fy_mapping(
			"name", name,
			"arguments", arguments));
}

static fy_generic stream_messages_tool_call(struct stream_response *stream,
					    size_t index)
{
	struct fy_generic_builder *gb;
	fy_generic tool_call;
	fy_generic function;
	fy_generic arguments;

	gb = stream->gb;
	tool_call = fy_get(stream->tool_calls, index);
	function = fy_get(tool_call, "function");
	arguments = fyai_join_strings(gb,
			fy_get(function, "argument_chunks", fy_seq_empty));
	return fy_mapping(gb,
		"type", "function_call",
		"call_id", fy_get(tool_call, "id", ""),
		"name", fy_get(tool_call, "name", ""),
		"arguments", arguments);
}

static int stream_emit_chat_tools(struct stream_response *stream)
{
	size_t i;

	for (i = 0; i < fy_len(stream->tool_calls); i++)
		if (stream_emit_tool(stream, i,
				     stream_chat_tool_call(stream, i)))
			return -1;
	return 0;
}

static void stream_report_failure(struct stream_response *stream,
				  fy_generic type, fy_generic event);

static int chat_stream_apply_chunk(struct stream_response *stream,
				   fy_generic chunk)
{
	struct fy_generic_builder *gb = stream->gb;
	struct fyai_cfg *cfg = stream->ctx->cfg;
	fy_generic choice, delta, tool_calls, tool_call, usage, logprobs;
	const char *text;

	/* Handle an error object that arrives with HTTP status 200. */
	if (fy_is_valid(fy_get(chunk, "error"))) {
		stream_report_failure(stream, fy_get(chunk, "type"), chunk);
		stream->failed = true;
		return -1;
	}

	usage = fy_get(chunk, "usage");
	if (!fy_is_invalid(usage))
		stream->usage = usage;

	stream->metadata = fy_assoc(gb, stream->metadata,
		"id", fy_get(chunk, "id", ""),
		"created", fy_get(chunk, "created", 0LL),
		"model", fy_get(chunk, "model", ""),
		"service_tier", fy_get(chunk, "service_tier", ""),
		"system_fingerprint", fy_get(chunk, "system_fingerprint", ""));

	choice = fy_get_at_path(chunk, "choices", 0);
	if (fy_is_invalid(choice))
		return 0;

	logprobs = fy_get(choice, "logprobs");
	if (!fy_is_invalid(logprobs) &&
	    !fy_is_null(logprobs)) {
		fy_generic content, refusal;

		content = fy_get(logprobs, "content");
		if (!fy_is_invalid(content)) {
			stream->logprob_content = fy_concat(gb,
					stream->logprob_content, content);
			if (cfg->token_extents &&
			    fy_is_valid(stream->token_extents)) {
				stream->token_extents =
					fyai_token_extents_append(gb,
						stream->token_extents, content,
						&stream->extents_pos);
				stream->extents_lp = true;
			}
		}
		refusal = fy_get(logprobs, "refusal");
		if (!fy_is_invalid(refusal))
			stream->logprob_refusal = refusal;
	}

	text = fy_get(choice, "finish_reason", "");
	if (*text)
		stream->finish_reason = fy_value(gb, text);

	delta = fy_get(choice, "delta");

	/*
	 * Some Chat Completions providers (deepseek, openrouter) stream a
	 * reasoning trace alongside the answer under `reasoning_content`. Show
	 * it live (dim, on stderr); it is provider wire detail dropped from
	 * canonical content, not part of the answer.
	 */
	text = fy_get(delta, "reasoning_content", "");
	if (*text)
		stream_write_reasoning(stream, text);

	text = fy_get(delta, "content", "");
	if (*text) {
		stream_finish_reasoning(stream);
		stream->content_chunks = fy_append(gb,
					stream->content_chunks, text);
		if (fy_is_invalid(stream->content_chunks))
			return -1;
		stream_write_content(stream, text);
	}

	tool_calls = fy_get(delta, "tool_calls");
	fy_foreach(tool_call, tool_calls) {
		if (stream_apply_tool_call_delta(stream, tool_call))
			return -1;
	}
	if (fy_equal(stream->finish_reason, "tool_calls"))
		return stream_emit_chat_tools(stream);

	return 0;
}

/*
 * Collect extents for one Responses output-text delta. When logprobs were
 * included the event's `logprobs` array delimits the tokens exactly; without
 * them the delta itself becomes one chunk extent. Both advance extents_pos by
 * byte length, so mixed streams stay position-consistent.
 */
static void responses_collect_extents(struct stream_response *stream,
				      fy_generic event, const char *delta)
{
	struct fy_generic_builder *gb = stream->gb;
	fy_generic entries;

	if (fy_is_invalid(stream->token_extents))
		return;

	entries = fy_get(event, "logprobs");
	if (fy_is_valid(entries) && fy_len(entries)) {
		stream->token_extents = fyai_token_extents_append(gb,
				stream->token_extents, entries,
				&stream->extents_pos);
		stream->extents_lp = true;
		return;
	}

	stream->token_extents = fy_append(gb, stream->token_extents,
			fy_mapping(gb,
				"text", delta,
				"pos", (long long)stream->extents_pos));
	stream->extents_pos += strlen(delta);
}

/*
 * Report a provider failure event. The reason sits in one of a few places
 * depending on the grammar and the kind of failure:
 *
 *	{ type: error, message }                          - a stream error
 *	{ type: error, error: { message } }               - the Messages form
 *	{ response: { error: { message } } }              - response.failed
 *	{ response: { incomplete_details: { reason } } }  - response.incomplete
 *
 * None of them is guaranteed, so fall back to naming the event: even that
 * beats the bare "request failed" the engine reports when the result carries
 * nothing.
 */
static void stream_report_failure(struct stream_response *stream,
				  fy_generic type, fy_generic event)
{
	struct fyai_ctx *ctx = stream->ctx;
	fy_generic err, msg, resp;
	const char *what;
	char *text;

	msg = fy_get(event, "message", fy_invalid);
	err = fy_get(event, "error", fy_invalid);
	if (fy_is_invalid(msg))
		msg = fy_get(err, "message", fy_invalid);
	if (fy_is_invalid(msg)) {
		resp = fy_get(event, "response", fy_invalid);
		err = fy_get(resp, "error", fy_invalid);
		msg = fy_get(err, "message", fy_invalid);
		if (fy_is_invalid(msg))
			msg = fy_get(fy_get(resp, "incomplete_details",
					    fy_invalid), "reason", fy_invalid);
	}

	what = fy_castp(&type, "error");
	if (fy_is_valid(msg) && !fy_is_null(msg))
		text = fy_sprintfa("%s: %s", what, fy_castp(&msg, ""));
	else
		text = fy_sprintfa("provider sent %s and no reason", what);
	/* Keep the cause until all retry attempts fail. */
	if (fyai_provider_error_transient(err) ||
	    fyai_provider_error_transient(event)) {
		free(stream->retry_cause);
		stream->retry_cause = strdup(text);
		stream->retry_hint = true;
		return;
	}
	fyai_error(ctx, "%s", text);
}

static int responses_stream_apply_event(struct stream_response *stream,
					fy_generic event)
{
	struct fy_generic_builder *gb = stream->gb;
	struct fyai_cfg *cfg = stream->ctx->cfg;
	fy_generic item;
	fy_generic type;
	const char *delta;
	long long index;

	type = fy_get(event, "type");

	if (fy_any_equal(type, "response.reasoning_summary_text.delta",
			 "response.reasoning_text.delta")) {
		stream_write_reasoning(stream, fy_get(event, "delta", ""));
		return 0;
	}

	if (fy_equal(type, "response.output_text.delta")) {
		delta = fy_get(event, "delta", "");
		if (*delta) {
			stream_finish_reasoning(stream);
			if (cfg->token_extents)
				responses_collect_extents(stream, event, delta);
			stream->content_chunks = fy_append(gb,
						stream->content_chunks, delta);
			if (fy_is_invalid(stream->content_chunks))
				return -1;
			stream_write_content(stream, delta);
		}
		return 0;
	}

	if (fy_equal(type, "response.output_item.done")) {
		item = fy_get(event, "item");
		index = fy_get(event, "output_index", 0LL);
		if (index < 0)
			return -1;
		while (fy_len(stream->response_items) <= (size_t)index) {
			stream->response_items = fy_append(gb,
						stream->response_items, fy_null);
			if (fy_is_invalid(stream->response_items))
				return -1;
		}
		stream->response_items = fy_replace(gb, stream->response_items,
						    (size_t)index, item);
		if (fy_is_invalid(stream->response_items))
			return -1;
		type = fy_get(item, "type");
		if (fy_not_equal(type, "function_call") &&
		    fy_not_equal(type, "shell_call"))
			return 0;
		return stream_emit_tool(stream, (size_t)index, item);
	}

	if (fy_equal(type, "response.completed")) {
		stream->completed_response = fy_get(event, "response");
		stream->done = true;
		stream_finish_content(stream);
		return fy_is_invalid(stream->completed_response) ? -1 : 0;
	}

	/*
	 * The provider says why on the event itself: an error object, or
	 * incomplete_details naming the limit that stopped it (commonly
	 * max_output_tokens). Report it - dropping it leaves the engine with
	 * nothing but a bare "request failed".
	 */
	if (fy_any_equal(type, "response.failed", "response.incomplete",
			 "error")) {
		stream_report_failure(stream, type, event);
		stream->failed = true;
		return -1;
	}

	return 0;
}

/*
 * Anthropic Messages SSE: content arrives as indexed block events. Text
 * deltas stream straight to the renderer; tool_use blocks reuse the chat
 * tool-call slots (indexed by content-block index, id/name at block start,
 * input_json_delta fragments accumulated as argument chunks). Usage arrives
 * split across message_start (input) and message_delta (output). There is no
 * [DONE]; message_stop ends the stream.
 */
static int messages_stream_apply_event(struct stream_response *stream,
				       fy_generic event)
{
	struct fy_generic_builder *gb = stream->gb;
	fy_generic message, block, tool_call, function, chunks, usage;
	fy_generic type;
	fy_generic reason;
	const char *text;
	long long index;

	type = fy_get(event, "type");

	if (fy_equal(type, "message_start")) {
		message = fy_get(event, "message");
		stream->metadata = fy_assoc(gb, stream->metadata,
			"id", fy_get(message, "id", ""),
			"model", fy_get(message, "model", ""));
		usage = fy_get(message, "usage");
		if (fy_is_valid(usage))
			stream->usage = usage;
		return 0;
	}

	if (fy_equal(type, "content_block_start")) {
		block = fy_get(event, "content_block");
		if (fy_not_equal(fy_get(block, "type"), "tool_use"))
			return 0;
		index = fy_get(event, "index", 0LL);
		if (index < 0 || stream_ensure_tool_call(stream, (size_t)index))
			return -1;
		tool_call = fy_get(stream->tool_calls, (size_t)index);
		tool_call = fy_assoc(gb, tool_call,
			"id", fy_get(block, "id", ""),
			"name", fy_get(block, "name", ""));
		stream->tool_calls = fy_replace(gb, stream->tool_calls,
						(size_t)index, tool_call);
		return fy_is_invalid(stream->tool_calls) ? -1 : 0;
	}

	if (fy_equal(type, "content_block_delta")) {
		block = fy_get(event, "delta");
		type = fy_get(block, "type");

		if (fy_equal(type, "text_delta")) {
			text = fy_get(block, "text", "");
			if (*text) {
				stream_finish_reasoning(stream);
				stream->content_chunks = fy_append(gb,
						stream->content_chunks, text);
				if (fy_is_invalid(stream->content_chunks))
					return -1;
				stream_write_content(stream, text);
			}
			return 0;
		}

		if (fy_equal(type, "thinking_delta")) {
			stream_write_reasoning(stream,
					fy_get(block, "thinking", ""));
			return 0;
		}

		if (fy_equal(type, "input_json_delta")) {
			text = fy_get(block, "partial_json", "");
			if (!*text)
				return 0;
			index = fy_get(event, "index", 0LL);
			if (index < 0 ||
			    stream_ensure_tool_call(stream, (size_t)index))
				return -1;
			tool_call = fy_get(stream->tool_calls, (size_t)index);
			function = fy_get(tool_call, "function");
			chunks = fy_append(gb,
					fy_get(function, "argument_chunks",
					       fy_seq_empty), text);
			function = fy_assoc(gb, function,
					    "argument_chunks", chunks);
			tool_call = fy_assoc(gb, tool_call,
					     "function", function);
			stream->tool_calls = fy_replace(gb, stream->tool_calls,
							(size_t)index, tool_call);
			return fy_is_invalid(stream->tool_calls) ? -1 : 0;
		}

		return 0;
	}

	if (fy_equal(type, "content_block_stop")) {
		index = fy_get(event, "index", -1LL);
		if (index < 0 || (size_t)index >= fy_len(stream->tool_calls))
			return 0;
		tool_call = fy_get(stream->tool_calls, (size_t)index);
		if (!*fy_get(tool_call, "id", ""))
			return 0;
		return stream_emit_tool(stream, (size_t)index,
			stream_messages_tool_call(stream, (size_t)index));
	}

	if (fy_equal(type, "message_delta")) {
		reason = fy_get_at_path(event, "delta", "stop_reason");
		text = fy_castp(&reason, "");
		if (*text)
			stream->finish_reason = fy_value(gb, text);
		usage = fy_get(event, "usage");
		if (fy_is_valid(usage))
			stream->usage = fy_assoc(gb,
				fy_is_valid(stream->usage) ?
					stream->usage : fy_map_empty,
				"output_tokens",
				fy_get(usage, "output_tokens", 0LL));
		return 0;
	}

	if (fy_equal(type, "message_stop")) {
		stream->done = true;
		stream_finish_content(stream);
		return 0;
	}

	if (fy_equal(type, "error")) {
		stream_report_failure(stream, type, event);
		stream->failed = true;
		return -1;
	}

	/* ping, content_block_stop, unknown future events: ignore. */
	return 0;
}

static int stream_handle_data(struct stream_response *stream,
			      const char *data)
{
	struct fyai_ctx *ctx = stream->ctx;
	struct fyai_cfg *cfg = ctx->cfg;
	fy_generic chunk;

	while (*data == ' ')
		data++;

	if (!strcmp(data, "[DONE]")) {
		stream->done = true;
		stream_finish_content(stream);
		return 0;
	}

	chunk = parse_response(stream->gb, data);
	if (fy_is_invalid(chunk)) {
		parse_diag_report(ctx, chunk, "malformed JSON",
				  "could not read a stream event");
		return -1;
	}

	if (cfg->debug > 1)
		emit_generic_to_stdout(stream->ctx, "stream", chunk, true);

	if (cfg->stream_logging) {
		(void)fyai_log_generic(ctx, "stream",
				fy_mapping(stream->gb,
					"kind", "event",
					"api", fyai_api_to_string(cfg->api_mode),
					"data", chunk));
	}

	switch (cfg->api_mode) {

	case FYAI_API_RESPONSES:
		return responses_stream_apply_event(stream, chunk);

	case FYAI_API_CHAT_COMPLETIONS:
		return chat_stream_apply_chunk(stream, chunk);

	case FYAI_API_MESSAGES:
		return messages_stream_apply_event(stream, chunk);
	}
	return -1;
}

static int stream_handle_line(struct stream_response *stream,
			      const char *line, size_t len)
{
	char *data;
	int ret;

	if (len && line[len - 1] == '\r')
		len--;

	if (len > 5 && !memcmp(line, "data:", 5)) {
		data = malloc(len - 5 + 1);
		if (!data)
			return -1;
		memcpy(data, line + 5, len - 5);
		data[len - 5] = '\0';
		ret = stream_handle_data(stream, data);
		free(data);
		return ret;
	}

	return 0;
}

static size_t write_stream_response(void *ptr, size_t size, size_t nmemb,
				    void *userdata)
{
	struct stream_response *stream = userdata;
	const char *data = ptr;
	size_t bytes = size * nmemb;
	size_t pos = 0;
	const char *newline;
	size_t len;

	if (stream->state == FYAISS_SUBMITTED)
		stream_state_transition(stream, FYAISS_STREAMING);
	if (stream->state == FYAISS_FAILED)
		return 0;
	if (!bytes)
		return 0;

	stream->received_bytes += bytes;

	if (append_mem(&stream->raw, data, bytes))
		return 0;

	while (pos < bytes) {
		newline = memchr(data + pos, '\n', bytes - pos);
		if (!newline) {
			if (append_mem(&stream->line, data + pos, bytes - pos))
				return 0;
			return bytes;
		}

		len = newline - (data + pos);
		if (append_mem(&stream->line, data + pos, len))
			return 0;
		if (stream_handle_line(stream, stream->line.data,
				       stream->line.len)) {
			stream->failed = true;
			return 0;
		}
		stream->line.len = 0;
		if (stream->line.data)
			stream->line.data[0] = '\0';
		pos += len + 1;
	}

	return bytes;
}

static fy_generic stream_build_tool_calls(struct stream_response *stream)
{
	struct fy_generic_builder *gb = stream->gb;
	fy_generic tool_calls, tool_call, function;
	fy_generic name, arguments;

	tool_calls = fy_seq_empty;

	fy_foreach(tool_call, stream->tool_calls) {
		function = fy_get(tool_call, "function");
		name = fyai_join_strings(gb,
				fy_get(function, "name_chunks", fy_seq_empty));
		arguments = fyai_join_strings(gb,
				fy_get(function, "argument_chunks", fy_seq_empty));

		tool_call = fy_mapping(
			"id", fy_get(tool_call, "id", ""),
			"type", fy_get(tool_call, "type", "function"),
			"function", fy_mapping(
				"name", name,
				"arguments", arguments));
		tool_calls = fy_append(tool_calls, tool_call);
	}

	tool_calls = fy_gb_internalize(gb, tool_calls);
	if (fy_is_invalid(tool_calls))
		fyai_error(stream->ctx, "could not build the streamed tool calls");
	return tool_calls;
}

static fy_generic stream_build_response_doc(struct stream_response *stream)
{
	struct fyai_ctx *ctx = stream->ctx;
	struct fyai_cfg *cfg = ctx->cfg;
	struct fy_generic_builder *gb = stream->gb;
	fy_generic message, choice, doc;

	message = fy_mapping("role", "assistant");
	if (fy_len(stream->content_chunks))
		message = fy_assoc(message,
			"content", fyai_join_strings(gb, stream->content_chunks));
	if (!fy_len(stream->tool_calls))
		message = fy_assoc(message,
			"refusal", fy_null,
			"annotations", fy_seq_empty);
	if (fy_len(stream->tool_calls))
		message = fy_assoc(message,
			"tool_calls", stream_build_tool_calls(stream));

	choice = fy_mapping(
		"index", 0,
		"message", message,
		"logprobs", cfg->logprobs ?
			fy_mapping(
				"content", stream->logprob_content,
				"refusal", fy_is_valid(stream->logprob_refusal) ?
					stream->logprob_refusal : fy_null) :
			fy_null,
		"finish_reason", fy_is_valid(stream->finish_reason) ?
			stream->finish_reason : fy_string("stop"));

	doc = fy_mapping(
		"id", fy_get(stream->metadata, "id", ""),
		"object", "chat.completion",
		"created", fy_get(stream->metadata, "created", 0LL),
		"model", fy_get(stream->metadata, "model", ""),
		"choices", fy_sequence(choice));
	if (!fy_is_invalid(stream->usage))
		doc = fy_assoc(doc, "usage", stream->usage);
	doc = fy_assoc(doc,
		"service_tier", fy_get(stream->metadata, "service_tier", ""),
		"system_fingerprint", fy_get(stream->metadata,
					     "system_fingerprint", ""));

	if (fy_is_invalid(doc)) {
		fyai_error(ctx, "could not build the streamed response");
		return fy_invalid;
	}
	doc = fy_gb_internalize(gb, doc);
	if (fy_is_invalid(doc))
		fyai_error(ctx, "could not retain the streamed response");
	return doc;
}

/*
 * Assemble a full Anthropic message document from the stream state so the
 * streamed path hands the engine the same shape a buffered /v1/messages
 * reply has: {id, model, role, content: [text?, tool_use...], stop_reason,
 * usage}. tool_use input is the parsed JSON of the accumulated
 * input_json_delta fragments.
 */
static fy_generic messages_build_response_doc(struct stream_response *stream)
{
	struct fy_generic_builder *gb = stream->gb;
	fy_generic content, tool_call, function, arguments, input, doc, text;

	content = fy_seq_empty;
	if (fy_len(stream->content_chunks)) {
		text = fyai_join_strings(gb, stream->content_chunks);
		content = fy_append(gb, content, fy_mapping(
				"type", "text",
				"text", text));
	}

	fy_foreach(tool_call, stream->tool_calls) {
		/* skip slots never opened as tool_use blocks (text indexes) */
		if (!*fy_get(tool_call, "id", ""))
			continue;
		function = fy_get(tool_call, "function");
		arguments = fyai_join_strings(gb,
				fy_get(function, "argument_chunks", fy_seq_empty));
		input = parse_json_string(gb, fy_cast(arguments, "{}"));
		if (fy_is_invalid(input))
			input = fy_map_empty;
		content = fy_append(gb, content, fy_mapping(
				"type", "tool_use",
				"id", fy_get(tool_call, "id", ""),
				"name", fy_get(tool_call, "name", ""),
				"input", input));
	}

	doc = fy_mapping(
		"id", fy_get(stream->metadata, "id", ""),
		"type", "message",
		"role", "assistant",
		"model", fy_get(stream->metadata, "model", ""),
		"content", content,
		"stop_reason", fy_is_valid(stream->finish_reason) ?
			stream->finish_reason : fy_string("end_turn"));
	if (!fy_is_invalid(stream->usage))
		doc = fy_assoc(doc, "usage", stream->usage);

	if (fy_is_invalid(doc)) {
		fyai_error(stream->ctx, "could not build the streamed message");
		return fy_invalid;
	}
	doc = fy_gb_internalize(gb, doc);
	if (fy_is_invalid(doc))
		fyai_error(stream->ctx, "could not retain the streamed message");
	return doc;
}

/*
 * Some Responses-compatible streaming endpoints leave `output` empty in the
 * response.completed event. Rebuild it from the output-item events, with the
 * text deltas as a fallback when the endpoint omits the message item too.
 */
static fy_generic responses_complete_response(struct stream_response *stream)
{
	struct fy_generic_builder *gb = stream->gb;
	fy_generic response, output, item, text;

	response = stream->completed_response;
	output = fy_get(response, "output", fy_invalid);
	if (fy_is_valid(output) && !fy_is_null(output) &&
	    fy_len(output))
		return response;

	output = fy_seq_empty;
	fy_foreach(item, stream->response_items) {
		if (!fy_is_null(item))
			output = fy_append(gb, output, item);
	}
	if (!fy_len(output) && fy_len(stream->content_chunks)) {
		text = fyai_join_strings(gb, stream->content_chunks);
		item = fy_mapping(
			"type", "message",
			"role", "assistant",
			"content", fy_sequence(
				fy_mapping(
					"type", "output_text",
					"text", text)));
		output = fy_append(gb, output, item);
	}
	if (fy_is_invalid(output))
		return fy_invalid;
	return fy_assoc(gb, response, "output", output);
}

/*
 * Hand the collected token extents to the engine for the turn-metadata
 * attach. Fail-soft by design: any inconsistency (offset drift against the
 * final content, collection failure) silently drops the extents - the call,
 * canonical content and usage are never affected. Chat without logprob
 * entries and Messages (no logprobs at all) rebuild {text, pos} chunk extents
 * from the accumulated content chunks, whose offsets are consistent with the
 * join by construction.
 */
static void stream_store_token_extents(struct stream_response *stream,
				       fy_generic response)
{
	struct fyai_ctx *ctx = stream->ctx;
	struct fyai_cfg *cfg = ctx->cfg;
	struct fy_generic_builder *gb = stream->gb;
	fy_generic extents;
	fy_generic chunk;
	fy_generic out;
	const char *text;
	size_t total;

	if (!cfg->token_extents)
		return;

	extents = stream->token_extents;
	switch (cfg->api_mode) {
	case FYAI_API_CHAT_COMPLETIONS:
		if (!stream->extents_lp) {
			extents = fyai_chunk_extents(gb,
						     stream->content_chunks);
			break;
		}
		total = 0;
		fy_foreach(chunk, stream->content_chunks) {
			text = fy_castp(&chunk, "");
			total += strlen(text);
		}
		if (stream->extents_pos != total)
			extents = fy_invalid;
		break;

	case FYAI_API_MESSAGES:
		extents = fyai_chunk_extents(gb, stream->content_chunks);
		break;

	case FYAI_API_RESPONSES:
		out = fyai_response_output_text(ctx, response);
		text = fy_castp(&out, "");
		if (stream->extents_pos != strlen(text))
			extents = fy_invalid;
		break;
	}

	if (fy_is_invalid(extents) || !fy_len(extents))
		return;

	ctx->last_token_extents = fy_gb_internalize(ctx->transient_gb, extents);
}

/* Read jitter from the operating system entropy source. */
static uint64_t stream_jitter_next(void)
{
#if !defined(__APPLE__)
	struct timespec ts;
#endif
	uint64_t value;
#if !defined(__APPLE__)
	int rc;
#endif

#if defined(__APPLE__)
	arc4random_buf(&value, sizeof(value));
	return value;
#else
	rc = getentropy(&value, sizeof(value));
	if (!rc)
		return value;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec << 32 ^ (uint64_t)ts.tv_nsec ^
	       ((uint64_t)getpid() << 16);
#endif
}

/* Apply exponential backoff, jitter, and the configured delay limit. */
fyai_event_ms_t fyai_retry_delay_ms(struct fyai_cfg *cfg, int attempt,
				    long retry_after_s)
{
	fyai_event_ms_t base, cap;
	int shift;

	cap = cfg->retry_max_delay_ms > 0 ? cfg->retry_max_delay_ms : 0;
	if (retry_after_s > 0) {
		base = (fyai_event_ms_t)retry_after_s * 1000;
		return cap && base > cap ? cap : base;
	}
	base = cfg->retry_initial_delay_ms > 0 ?
		cfg->retry_initial_delay_ms : 0;
	/* Bound the shift: an attempt count cannot overflow the delay. */
	shift = attempt > 0 ? attempt - 1 : 0;
	if (shift > 20)
		shift = 20;
	base <<= shift;
	if (cap && base > cap)
		base = cap;
	if (base <= 1)
		return base;
	return base / 2 + (fyai_event_ms_t)(stream_jitter_next() %
					    (uint64_t)(base / 2 + 1));
}

/* Present a retry in the shared work band or as plain status output. */
static void stream_retry_report(struct fyai_ctx *ctx, long status,
				CURLcode res, const char *cause, int attempt,
				int max_attempts, fyai_event_ms_t delay_ms,
				bool *bandp)
{
	struct fyai_sink_band *band;
	const char *reason;
	char *body;

	if (!fy_str_empty(cause))
		reason = cause;
	else if (status)
		reason = fy_sprintfa("HTTP %ld", status);
	else
		reason = curl_easy_strerror(res);

	/* Route delegated waits to the parent work band. */
	if (fyai_agent_delegated(ctx)) {
		body = fy_sprintfa("retrying in %.1f s (attempt %d of %d): "
				   "%s\n", (double)delay_ms / 1000.0,
				   attempt + 1, max_attempts, reason);
		fyai_tool_progress_emit(ctx, body, strlen(body));
		return;
	}
	if (!bandp || !fyai_sink_bands_available(ctx->sink)) {
		/* Keep the reason last: a provider message can be long. */
		fyai_report(ctx, "retrying in %.1f s (attempt %d of %d): %s\n",
			    (double)delay_ms / 1000.0, attempt + 1,
			    max_attempts, reason);
		return;
	}
	/*
	 * Never take a band another producer is drawing in. A model request
	 * runs with no tool band open, so this is a guard, not the usual path.
	 */
	if (!*bandp && fyai_sink_band_shared(ctx->sink))
		return;
	if (!*bandp) {
		if (!fyai_sink_band_open(ctx->sink, true, "**provider**", NULL))
			return;
		*bandp = true;
	}
	band = fyai_sink_band_shared(ctx->sink);
	if (!band)
		return;
	body = fy_sprintfa("%s\n\nretrying in %.1f s (attempt %d of %d)\n",
			   reason, (double)delay_ms / 1000.0, attempt + 1,
			   max_attempts);
	fyai_sink_band_paint(band, NULL, NULL, body, strlen(body), NULL);
}

/* Settle the retry band with the final request outcome. */
static void stream_retry_band_close(struct fyai_ctx *ctx, bool *bandp, bool ok,
				    const char *cause)
{
	if (!bandp || !*bandp)
		return;
	*bandp = false;
	fyai_sink_band_close(ctx->sink, ok, cause);
}

/* Return the Retry-After delay in seconds. */
static long stream_retry_after(struct fyai_ctx *ctx)
{
	curl_off_t secs = 0;

	if (curl_easy_getinfo(ctx->curl, CURLINFO_RETRY_AFTER, &secs) != CURLE_OK)
		return 0;
	return secs > 0 ? (long)secs : 0;
}

static void stream_retry_band_close(struct fyai_ctx *ctx, bool *bandp, bool ok,
				    const char *cause);

static void stream_request_notify(struct stream_response *stream)
{
	fyai_stream_complete_fn complete;
	void *userdata;

	/*
	 * Every terminal path arrives here, so the band the retries opened is
	 * settled once, with the outcome the turn actually reached.
	 */
	stream_retry_band_close(stream->ctx, &stream->retry_band,
				stream->state == FYAISS_COMPLETED, NULL);
	complete = stream->callbacks.complete;
	userdata = stream->callbacks.userdata;
	if (complete)
		complete(stream, userdata);
}

static void stream_request_complete(struct fyai_curl_transfer *transfer,
				    void *userdata);
static void stream_auth_complete(
		struct fyai_auth_refresh_request *request, void *userdata);

static int stream_request_resubmit(struct stream_response *stream)
{
	struct fyai_ctx *ctx;
	struct fyai_stream_callbacks callbacks;
	bool band;
	int attempt;
	int rc;

	ctx = stream->ctx;
	callbacks = stream->callbacks;
	attempt = stream->attempt;
	band = stream->retry_band;
	if (stream->state != FYAISS_RETRYING)
		return -1;
	stream_response_cleanup(stream);
	memset(stream, 0, sizeof(*stream));
	stream->callbacks = callbacks;
	stream->attempt = attempt;
	stream->retry_band = band;
	rc = stream_response_init(stream, ctx);
	if (rc)
		return -1;
	stream->state = FYAISS_RETRYING;
	curl_easy_setopt(ctx->curl, CURLOPT_WRITEFUNCTION,
			 write_stream_response);
	curl_easy_setopt(ctx->curl, CURLOPT_WRITEDATA, stream);
	stream_state_transition(stream, FYAISS_SUBMITTED);
	if (stream->state != FYAISS_SUBMITTED)
		return -1;
	stream->transfer = fyai_curl_submit(ctx, ctx->curl,
					    stream_request_complete, stream);
	return stream->transfer ? 0 : -1;
}

static void stream_auth_complete(
		struct fyai_auth_refresh_request *request, void *userdata)
{
	struct stream_response *stream;
	struct fyai_ctx *ctx;
	fy_generic result;
	int rc;

	stream = userdata;
	ctx = stream->ctx;
	rc = fyai_auth_refresh_collect(request);
	fyai_auth_refresh_destroy(request);
	stream->auth_refresh = NULL;
	if (stream->cancel_requested) {
		result = fyai_with_diag(ctx->transient_gb, fy_invalid,
				       "interrupted");
		stream_state_transition(stream, FYAISS_CANCELLED);
		stream->result = result;
		stream_request_notify(stream);
		return;
	}
	if (!rc)
		rc = fyai_request_state_apply(ctx);
	if (!rc)
		rc = stream_request_resubmit(stream);
	if (!rc)
		return;
	stream_state_transition(stream, FYAISS_FAILED);
	stream->result = fy_invalid;
	stream_request_notify(stream);
}

/* Arm the timer that resubmits the request. */
static enum fyai_event_action stream_retry_fire(const struct fyai_event *ev);

static int stream_retry_arm(struct stream_response *stream, long status,
			    CURLcode res)
{
	struct fyai_ctx *ctx = stream->ctx;
	struct fyai_cfg *cfg = ctx->cfg;
	struct fyai_event_loop *el;
	fyai_event_ms_t delay;
	int rc;

	if (stream->cancel_requested)
		return 0;
	if (stream->attempt >= cfg->retry_max_attempts)
		return 0;
	/* Check transport failures and provider errors inside HTTP 200 streams. */
	if (!fyai_http_transient(res, status) && !stream->retry_hint)
		return 0;
	/* Do not repeat content that the user has already seen. */
	if (stream->printed_content || stream->printed_reasoning)
		return 0;
	el = fyai_ctx_loop(ctx);
	assert(el);
	delay = fyai_retry_delay_ms(cfg, stream->attempt,
				   status ? stream_retry_after(ctx) : 0);
	stream_state_transition(stream, FYAISS_RETRYING);
	if (stream->state != FYAISS_RETRYING)
		return -1;
	rc = fyai_event_add_timer(el, delay, 0, stream_retry_fire, stream,
				  &stream->retry_timer);
	fyai_error_check(ctx, !rc, err_out,
			 "could not arm the provider retry timer");
	stream_retry_report(ctx, status, res, stream->retry_cause,
			    stream->attempt, cfg->retry_max_attempts, delay,
			    &stream->retry_band);
	return 1;

err_out:
	stream->retry_timer = NULL;
	return -1;
}

static enum fyai_event_action stream_retry_fire(const struct fyai_event *ev)
{
	struct stream_response *stream = ev->userdata;
	struct fyai_ctx *ctx;
	int rc;

	ctx = stream->ctx;
	fyai_event_source_remove(stream->retry_timer);
	stream->retry_timer = NULL;
	if (stream->cancel_requested) {
		/* CANCELLING is the only state a cancel may settle from. */
		stream_state_transition(stream, FYAISS_CANCELLING);
		stream_state_transition(stream, FYAISS_CANCELLED);
		stream->result = fyai_with_diag(ctx->transient_gb,
						fy_invalid, "interrupted");
		stream_request_notify(stream);
		return FYAIEA_CONTINUE;
	}
	rc = stream_request_resubmit(stream);
	fyai_error_check(ctx, !rc, err_out,
			 "could not resubmit the provider request");
	return FYAIEA_CONTINUE;

err_out:
	stream_state_transition(stream, FYAISS_FAILED);
	stream->result = fy_invalid;
	stream_request_notify(stream);
	return FYAIEA_CONTINUE;
}

static void stream_request_complete(struct fyai_curl_transfer *transfer,
				    void *userdata)
{
	struct stream_response *stream;
	struct fyai_ctx *ctx;
	struct fyai_cfg *cfg;
	CURLcode res;
	fy_generic ret;
	fy_generic tool_calls;
	fy_generic tool_call;
	long status;
	int rc;
	int retry_rc;
	size_t tool_index;

	stream = userdata;
	ctx = stream->ctx;
	cfg = ctx->cfg;
	status = 0;
	ret = fy_invalid;
	res = fyai_curl_collect(transfer);
	fyai_curl_transfer_destroy(transfer);
	stream->transfer = NULL;

	if (stream->cancel_requested || res == CURLE_ABORTED_BY_CALLBACK) {
		ret = fyai_with_diag(ctx->transient_gb, fy_invalid, "interrupted");
		stream_state_transition(stream, FYAISS_CANCELLED);
		goto done;
	}

	curl_easy_getinfo(ctx->curl, CURLINFO_RESPONSE_CODE, &status);
	/* Use the stream error before the curl callback error. */
	if (stream->failed && (!status || (status >= 200 && status < 300))) {
		stream->attempt++;
		retry_rc = stream_retry_arm(stream, 0, CURLE_OK);
		if (retry_rc > 0)
			return;
		if (retry_rc < 0) {
			stream_state_transition(stream, FYAISS_FAILED);
			goto done;
		}
		if (stream->retry_hint && stream->retry_cause)
			fyai_error(ctx, "%s", stream->retry_cause);
		stream_state_transition(stream, FYAISS_FAILED);
		goto done;
	}
	if (res != CURLE_OK) {
		stream->attempt++;
		retry_rc = stream_retry_arm(stream, 0, res);
		if (retry_rc > 0)
			return;
		if (retry_rc < 0) {
			stream_state_transition(stream, FYAISS_FAILED);
			goto done;
		}
		fyai_error(ctx, "request failed: %s", curl_easy_strerror(res));
		stream_state_transition(stream, FYAISS_FAILED);
		goto done;
	}

	if (status < 200 || status >= 300) {
		stream->attempt++;
		retry_rc = stream_retry_arm(stream, status, res);
		if (retry_rc > 0)
			return;
		if (retry_rc < 0) {
			stream_state_transition(stream, FYAISS_FAILED);
			goto done;
		}
		if (fyai_auth_should_retry(ctx, status)) {
			ctx->auth_retry_done = true;
			stream_state_transition(stream, FYAISS_RETRYING);
			stream->auth_refresh = fyai_auth_refresh_submit(
					ctx, true, stream_auth_complete,
					stream);
			if (stream->auth_refresh)
				return;
			stream_state_transition(stream, FYAISS_FAILED);
			goto done;
		}
		ctx->response_chain_miss =
			response_chain_miss(ctx, status, stream->raw.data);
		if (ctx->response_chain_miss) {
			stream_state_transition(stream, FYAISS_FAILED);
			goto done;
		}
		fyai_error(ctx, "request returned HTTP %ld%s%s", status,
			   stream->raw.data ? "\n" : "",
			   stream->raw.data ? stream->raw.data : "");
		stream_state_transition(stream, FYAISS_FAILED);
		goto done;
	}

	if (stream->failed) {
		stream_state_transition(stream, FYAISS_FAILED);
		goto done;
	}
	if (!stream->done) {
		fyai_error(ctx, "the response stream ended before it completed "
			   "(after %zu bytes)", stream->received_bytes);
		stream_state_transition(stream, FYAISS_FAILED);
		goto done;
	}

	switch (cfg->api_mode) {
	case FYAI_API_RESPONSES:
		ret = responses_complete_response(stream);
		break;
	case FYAI_API_CHAT_COMPLETIONS:
		ret = stream_build_response_doc(stream);
		break;
	case FYAI_API_MESSAGES:
		ret = messages_build_response_doc(stream);
		break;
	}

	if (fy_is_valid(ret))
		stream_store_token_extents(stream, ret);
	if (fy_is_valid(ret) && stream->callbacks.tool) {
		tool_calls = fyai_response_tool_calls(ctx, ret);
		tool_index = 0;
		fy_foreach(tool_call, tool_calls) {
			rc = stream_emit_tool(stream, tool_index++, tool_call);
			if (rc) {
				ret = fy_invalid;
				break;
			}
		}
	}
	if (fy_is_valid(ret))
		stream_state_transition(stream, FYAISS_COMPLETED);
	else
		stream_state_transition(stream, FYAISS_FAILED);

done:
	if (fy_is_valid(ret))
		ret = fy_gb_internalize(ctx->transient_gb, ret);
	stream->result = ret;
	stream_request_notify(stream);
}

fyai_stream_request *
fyai_stream_request_submit(struct fyai_ctx *ctx,
			   const struct fyai_stream_callbacks *callbacks)
{
	struct stream_response *stream;

	stream = calloc(1, sizeof(*stream));
	fyai_error_check(ctx, stream, err_out, "out of memory");
	if (callbacks)
		stream->callbacks = *callbacks;
	fyai_error_check(ctx, !stream_response_init(stream, ctx), err_free,
			 "could not initialize response stream");

	curl_easy_setopt(ctx->curl, CURLOPT_WRITEFUNCTION, write_stream_response);
	curl_easy_setopt(ctx->curl, CURLOPT_WRITEDATA, stream);
	stream_state_transition(stream, FYAISS_SUBMITTED);
	fyai_error_check(ctx, stream->state == FYAISS_SUBMITTED, err_cleanup,
			 "could not submit response stream");
	stream->transfer = fyai_curl_submit(ctx, ctx->curl,
					    stream_request_complete, stream);
	fyai_error_check(ctx, stream->transfer, err_cleanup,
			 "could not submit response stream");
	return stream;

err_cleanup:
	stream_response_cleanup(stream);
err_free:
	free(stream);
err_out:
	return NULL;
}

void fyai_stream_request_cancel(fyai_stream_request *request)
{
	if (!request || stream_state_terminal(request->state))
		return;
	request->cancel_requested = true;
	if (request->transfer) {
		stream_state_transition(request, FYAISS_CANCELLING);
		fyai_curl_cancel(request->transfer);
	} else if (request->auth_refresh) {
		stream_state_transition(request, FYAISS_CANCELLING);
		fyai_auth_refresh_cancel(request->auth_refresh);
	} else if (request->retry_timer) {
		/*
		 * A request that waits out a backoff has no transfer to stop.
		 * Retire the timer and settle here: nothing else will run.
		 */
		fyai_event_source_remove(request->retry_timer);
		request->retry_timer = NULL;
		stream_state_transition(request, FYAISS_CANCELLING);
		stream_state_transition(request, FYAISS_CANCELLED);
		request->result = fyai_with_diag(request->ctx->transient_gb,
						 fy_invalid, "interrupted");
		stream_request_notify(request);
	}
}

enum fyai_stream_state
fyai_stream_request_state(const fyai_stream_request *request)
{
	return request ? request->state : FYAISS_FAILED;
}

bool fyai_stream_request_done(const fyai_stream_request *request)
{
	return request && stream_state_terminal(request->state);
}

fy_generic fyai_stream_request_collect(const fyai_stream_request *request)
{
	return fyai_stream_request_done(request) ?
		request->result : fy_invalid;
}

void fyai_stream_request_destroy(fyai_stream_request *request)
{
	if (!request)
		return;
	if (request->retry_timer) {
		fyai_event_source_remove(request->retry_timer);
		request->retry_timer = NULL;
	}
	if (request->transfer)
		fyai_curl_transfer_destroy(request->transfer);
	if (request->auth_refresh)
		fyai_auth_refresh_destroy(request->auth_refresh);
	stream_response_cleanup(request);
	free(request);
}

struct fyai_buffered_request {
	struct fyai_ctx *ctx;
	struct fyai_curl_transfer *transfer;
	struct fyai_auth_refresh_request *auth_refresh;
	struct fyai_event_source *retry_timer;
	int attempt;			/* attempts made for this request */
	bool retry_band;		/* the shared band is ours */
	fyai_buffered_complete_fn complete;
	void *userdata;
	struct response_buffer response;
	fy_generic result;
	bool done;
	bool cancel_requested;
};

static void buffered_request_complete(struct fyai_curl_transfer *transfer,
				      void *userdata);
static void buffered_auth_complete(
		struct fyai_auth_refresh_request *request, void *userdata);

static int
buffered_request_start(struct fyai_buffered_request *request)
{
	struct fyai_ctx *ctx;

	ctx = request->ctx;
	curl_easy_setopt(ctx->curl, CURLOPT_WRITEFUNCTION, write_response);
	curl_easy_setopt(ctx->curl, CURLOPT_WRITEDATA, &request->response);
	request->transfer = fyai_curl_submit(ctx, ctx->curl,
					     buffered_request_complete,
					     request);
	return request->transfer ? 0 : -1;
}

static enum fyai_event_action buffered_retry_fire(const struct fyai_event *ev);
static void buffered_request_notify(struct fyai_buffered_request *request);

/* Arm a retry for a request that has not presented its buffered response. */
static int buffered_retry_arm(struct fyai_buffered_request *request,
			      long status, CURLcode res)
{
	struct fyai_ctx *ctx = request->ctx;
	struct fyai_cfg *cfg = ctx->cfg;
	struct fyai_event_loop *el;
	fyai_event_ms_t delay;
	int rc;

	if (request->cancel_requested)
		return 0;
	if (request->attempt >= cfg->retry_max_attempts)
		return 0;
	if (!fyai_http_transient(res, status))
		return 0;
	el = fyai_ctx_loop(ctx);
	assert(el);
	delay = fyai_retry_delay_ms(cfg, request->attempt,
				   status ? stream_retry_after(ctx) : 0);
	rc = fyai_event_add_timer(el, delay, 0, buffered_retry_fire, request,
				  &request->retry_timer);
	fyai_error_check(ctx, !rc, err_out,
			 "could not arm the provider retry timer");
	free(request->response.data);
	memset(&request->response, 0, sizeof(request->response));
	stream_retry_report(ctx, status, res, NULL, request->attempt,
			    cfg->retry_max_attempts, delay,
			    &request->retry_band);
	return 1;

err_out:
	request->retry_timer = NULL;
	return -1;
}

static enum fyai_event_action buffered_retry_fire(const struct fyai_event *ev)
{
	struct fyai_buffered_request *request = ev->userdata;

	fyai_event_source_remove(request->retry_timer);
	request->retry_timer = NULL;
	if (!request->cancel_requested && !buffered_request_start(request))
		return FYAIEA_CONTINUE;
	if (request->cancel_requested)
		request->result = fyai_with_diag(request->ctx->transient_gb,
						 fy_invalid, "interrupted");
	request->done = true;
	buffered_request_notify(request);
	return FYAIEA_CONTINUE;
}

static void buffered_request_notify(struct fyai_buffered_request *request)
{
	fyai_buffered_complete_fn complete;
	void *userdata;

	stream_retry_band_close(request->ctx, &request->retry_band,
				fy_is_valid(request->result), NULL);
	complete = request->complete;
	userdata = request->userdata;
	if (complete)
		complete(request, userdata);
}

static void buffered_auth_complete(
		struct fyai_auth_refresh_request *refresh, void *userdata)
{
	struct fyai_buffered_request *request;
	struct fyai_ctx *ctx;
	int rc;

	request = userdata;
	ctx = request->ctx;
	rc = fyai_auth_refresh_collect(refresh);
	fyai_auth_refresh_destroy(refresh);
	request->auth_refresh = NULL;
	if (request->cancel_requested) {
		request->result = fyai_with_diag(ctx->transient_gb,
						 fy_invalid,
						 "interrupted");
		goto done;
	}
	if (!rc)
		rc = fyai_request_state_apply(ctx);
	if (!rc)
		rc = buffered_request_start(request);
	if (!rc)
		return;
done:
	request->done = true;
	buffered_request_notify(request);
}

static void buffered_request_complete(struct fyai_curl_transfer *transfer,
				      void *userdata)
{
	struct fyai_buffered_request *request;
	struct fyai_ctx *ctx;
	fy_generic response_doc;
	CURLcode res;
	long status;
	int retry_rc;

	request = userdata;
	ctx = request->ctx;
	status = 0;
	request->result = fy_invalid;
	res = fyai_curl_collect(transfer);
	fyai_curl_transfer_destroy(transfer);
	request->transfer = NULL;

	if (res == CURLE_ABORTED_BY_CALLBACK) {
		request->result = fyai_with_diag(ctx->transient_gb,
						 fy_invalid,
						 "interrupted");
		goto done;
	}
	if (res != CURLE_OK) {
		request->attempt++;
		retry_rc = buffered_retry_arm(request, 0, res);
		if (retry_rc > 0)
			return;
		if (retry_rc < 0)
			goto done;
		fyai_error(ctx, "request failed: %s", curl_easy_strerror(res));
		goto done;
	}

	curl_easy_getinfo(ctx->curl, CURLINFO_RESPONSE_CODE, &status);
	if (status < 200 || status >= 300) {
		request->attempt++;
		retry_rc = buffered_retry_arm(request, status, res);
		if (retry_rc > 0)
			return;
		if (retry_rc < 0)
			goto done;
		if (fyai_auth_should_retry(ctx, status)) {
			free(request->response.data);
			memset(&request->response, 0,
			       sizeof(request->response));
			ctx->auth_retry_done = true;
			request->auth_refresh = fyai_auth_refresh_submit(
					ctx, true, buffered_auth_complete,
					request);
			if (request->auth_refresh)
				return;
			goto done;
		}
		ctx->response_chain_miss =
			response_chain_miss(ctx, status,
					    request->response.data);
		if (ctx->response_chain_miss)
			goto done;
		fyai_error(ctx, "request returned HTTP %ld%s%s", status,
			   request->response.data ? "\n" : "",
			   request->response.data ?
				request->response.data : "");
		goto done;
	}

	if (!request->response.data)
		goto done;

	response_doc = parse_response(ctx->transient_gb,
				      request->response.data);
	if (fy_is_invalid(response_doc)) {
		parse_diag_report(ctx, response_doc, "malformed JSON",
				  "could not read the provider response");
		goto done;
	}
	request->result = response_doc;
done:
	request->done = true;
	buffered_request_notify(request);
}

struct fyai_buffered_request *
fyai_buffered_request_submit(struct fyai_ctx *ctx,
			     fyai_buffered_complete_fn complete,
			     void *userdata)
{
	struct fyai_buffered_request *request;
	int rc;

	request = calloc(1, sizeof(*request));
	fyai_error_check(ctx, request, err_out, "out of memory");
	request->ctx = ctx;
	request->complete = complete;
	request->userdata = userdata;
	request->result = fy_invalid;
	rc = buffered_request_start(request);
	fyai_error_check(ctx, !rc, err_free,
			 "could not submit buffered request");
	return request;

err_free:
	free(request);
err_out:
	return NULL;
}

void fyai_buffered_request_cancel(struct fyai_buffered_request *request)
{
	if (!request || request->done)
		return;
	request->cancel_requested = true;
	if (request->transfer)
		fyai_curl_cancel(request->transfer);
	else if (request->auth_refresh)
		fyai_auth_refresh_cancel(request->auth_refresh);
	else if (request->retry_timer) {
		/* Waiting out a backoff: retire the timer and settle here. */
		fyai_event_source_remove(request->retry_timer);
		request->retry_timer = NULL;
		request->result = fyai_with_diag(request->ctx->transient_gb,
						 fy_invalid, "interrupted");
		request->done = true;
		buffered_request_notify(request);
	}
}

bool fyai_buffered_request_done(
		const struct fyai_buffered_request *request)
{
	return request && request->done;
}

fy_generic fyai_buffered_request_collect(
		const struct fyai_buffered_request *request)
{
	return fyai_buffered_request_done(request) ?
		request->result : fy_invalid;
}

void fyai_buffered_request_destroy(struct fyai_buffered_request *request)
{
	if (!request)
		return;
	if (request->retry_timer) {
		fyai_event_source_remove(request->retry_timer);
		request->retry_timer = NULL;
	}
	if (request->transfer)
		fyai_curl_transfer_destroy(request->transfer);
	if (request->auth_refresh)
		fyai_auth_refresh_destroy(request->auth_refresh);
	free(request->response.data);
	free(request);
}
