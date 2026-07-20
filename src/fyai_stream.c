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

#include "fyai_markdown.h"
#include "fyai_log.h"
#include "fyai_provider.h"
#include "fyai_stream.h"
#include "fyai_terminal.h"

/* Min interval between streamed-markdown redraws. */
#define STREAM_REDRAW_MS 50

struct stream_spinner {
	size_t frame;
	bool active;
};

struct stream_response {
	struct fyai_ctx *ctx;
	struct fy_generic_builder *gb;
	struct markdown_renderer markdown;
	struct stream_spinner spinner;
	struct response_buffer raw;
	struct response_buffer line;
	struct response_buffer md_full;		/* used for oneshot/off-tty */
	struct response_buffer reasoning_text;
	size_t md_active_rows;
	size_t reasoning_active_rows;
	bool md_tty;				/* stdout is a terminal (redraw ok) */
	long md_last_ms;			/* monotonic ms of last stream redraw */
	fy_generic content_chunks;
	fy_generic tool_calls;
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
	size_t received_bytes;
	size_t content_bytes;
	size_t content_chunks_count;
	/*
	 * Token extents collected for turn metadata (cfg->token_extents):
	 * {text, pos, lp} per logprob token, or {text, pos} per content chunk
	 * on the fallback paths. extents_pos is the running byte offset into
	 * the joined content, advanced only where extent entries are added -
	 * not the display counters above, which follow the markdown branch.
	 */
	fy_generic token_extents;
	size_t extents_pos;
	bool extents_lp;
};

static int append_mem(struct response_buffer *buf, const char *data, size_t len)
{
	if (response_buffer_reserve(buf, buf->len + len + 1))
		return -1;

	memcpy(buf->data + buf->len, data, len);
	buf->len += len;
	buf->data[buf->len] = '\0';
	return 0;
}

static void stream_spinner_clear(struct stream_spinner *spinner)
{
	if (!spinner->active)
		return;

	fputs(FYAI_ANSI_ERASE_LINE, stderr);
	fflush(stderr);
	spinner->active = false;
}

static void stream_response_cleanup(struct stream_response *stream)
{
	stream_spinner_clear(&stream->spinner);
	free(stream->raw.data);
	free(stream->line.data);
	free(stream->md_full.data);
	free(stream->reasoning_text.data);
	markdown_renderer_destroy(&stream->markdown);
	if (stream->gb)
		fy_generic_builder_destroy(stream->gb);
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

/*
 * Apply one renderer line-delta: it is authoritative - backtrack N lines over
 * the active region, erase down, and print only the changed tail (from the
 * first changed line). We mirror the renderer's own active-region line count in
 * md_active_rows so the final flush knows how much to rewind; backtrack is
 * always <= that count, and max_active_lines keeps the active region within the
 * viewport so the cursor-up always lands on a visible row. Do not clamp
 * backtrack to our own count - that under-rewinds at a freeze boundary and
 * leaves duplicated lines in the scrollback.
 */
static void stream_md_apply(FILE *fp, size_t *active_rows,
			    const struct markdown_update *update)
{
	size_t backtrack = update->backtrack;

	if (backtrack) {
		fprintf(fp, FYAI_ANSI_CURSOR_UP_FMT, backtrack);
		fputs(FYAI_ANSI_ERASE_DOWN, fp);
		*active_rows -= backtrack;
	}
	if (update->content_len)
		fwrite(update->content, 1, update->content_len, fp);
	*active_rows += count_newlines(update->content, update->content_len);
	if (update->freeze >= *active_rows)
		*active_rows = 0;
	else
		*active_rows -= update->freeze;
}

static void stream_write_content(struct stream_response *stream,
				 const char *text)
{
	struct fyai_ctx *ctx = stream->ctx;
	struct fyai_cfg *cfg = ctx->cfg;
	size_t len;
	const char *mode;
	bool draw;
	struct timespec ts;
	struct markdown_update update;
	long now;
	bool tty_stream;

	len = strlen(text);
	if (stream->markdown.active) {
		mode = cfg->markdown_mode;
		tty_stream = stream->md_tty && strcmp(mode, "oneshot");
		if (response_buffer_append(&stream->md_full, text)) {
			stream->failed = true;
		} else {
			draw = false;

			stream->content_bytes += len;
			stream->content_chunks_count++;
			/*
			 * Redraw cadence is terminal-only. Off-tty and oneshot
			 * accumulate and render once at finish.
			 * "line" redraws when a source line completes; "oneshot"
			 * never until finish.
			 */
			if (tty_stream && !strcmp(mode, "stream")) {
				clock_gettime(CLOCK_MONOTONIC, &ts);
				now = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
				if (now - stream->md_last_ms >= STREAM_REDRAW_MS) {
					draw = true;
					stream->md_last_ms = now;
				}
			} else if (tty_stream && !strcmp(mode, "line")) {
				draw = strchr(text, '\n') != NULL;
			}
			if (draw) {
				if (markdown_renderer_push(&stream->markdown,
							   stream->md_full.data,
							   stream->md_full.len,
							   &update)) {
					stream->failed = true;
				} else {
					stream_md_apply(stdout,
							&stream->md_active_rows,
							&update);
					stream->md_full.len = 0;
					stream->md_full.data[0] = '\0';
				}
			}
		}
	} else {
		fputs(text, stdout);
		fflush(stdout);
	}
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
				     cfg->theme);
	free(md.data);
	if (rc) {
		free(out.data);
		return -1;
	}

	if (stream->reasoning_active_rows) {
		fprintf(stderr, FYAI_ANSI_CURSOR_UP_FMT,
			stream->reasoning_active_rows);
		fputs(FYAI_ANSI_ERASE_DOWN, stderr);
	}
	rows = count_newlines(out.data, out.len);
	if (out.len)
		fwrite(out.data, 1, out.len, stderr);
	stream->reasoning_active_rows = rows;
	fflush(stderr);
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
	bool color;

	if (!cfg->thinking)
		return;

	color = ansi_color_on(cfg->color, STDERR_FILENO);
	if (!*text)
		return;
	if (!stream->printed_reasoning) {
		if (cfg->markdown && markdown_available(cfg) &&
		    terminal_is_tty(STDERR_FILENO)) {
			stream->printed_reasoning = true;
		} else {
			fputs(color ? FYAI_ANSI_DIM "reasoning ▸ " :
				     "reasoning > ", stderr);
			stream->printed_reasoning = true;
		}
	}
	if (cfg->markdown && markdown_available(cfg) &&
	    terminal_is_tty(STDERR_FILENO)) {
		if (response_buffer_append(&stream->reasoning_text, text) ||
		    stream_write_reasoning_md(stream))
			stream->failed = true;
	} else {
		fputs(text, stderr);
		fflush(stderr);
	}
}

static void stream_finish_reasoning(struct stream_response *stream)
{
	struct fyai_ctx *ctx = stream->ctx;
	struct fyai_cfg *cfg = ctx->cfg;
	bool color;

	color = ansi_color_on(cfg->color, STDERR_FILENO);
	if (stream->reasoning_active_rows) {
		if (*cfg->section_separator)
			fputs(cfg->section_separator, stderr);
		fputs("\n\n", stderr);
		fflush(stderr);
		stream->reasoning_active_rows = 0;
		stream->printed_reasoning = false;
		return;
	}
	if (stream->printed_reasoning) {
		if (color)
			fputs(FYAI_ANSI_RESET, stderr);
		if (*cfg->section_separator)
			fputs(cfg->section_separator, stderr);
		fputs("\n\n", stderr);
		fflush(stderr);
		stream->printed_reasoning = false;
	}
}

static void stream_finish_content(struct stream_response *stream)
{
	struct fyai_ctx *ctx = stream->ctx;
	struct fyai_cfg *cfg = ctx->cfg;
	struct markdown_update update;
	struct response_buffer out = {0};
	const char *mode;
	size_t end;

	stream_finish_reasoning(stream);
	if (!stream->printed_content)
		return;

	if (stream->markdown.active) {
		stream_spinner_clear(&stream->spinner);
		mode = cfg->markdown_mode;
		if (stream->md_tty && strcmp(mode, "oneshot")) {
			if (stream->md_full.len &&
			    !markdown_renderer_push(&stream->markdown,
						    stream->md_full.data,
						    stream->md_full.len,
						    &update))
				stream_md_apply(stdout, &stream->md_active_rows,
						&update);
			stream->md_full.len = 0;
			if (stream->md_full.data)
				stream->md_full.data[0] = '\0';
			/*
			 * markdown_renderer_finish() emits the healed final form
			 * of the still-active (unfrozen) region - the same rows
			 * the progressive redraw already drew. Rewind over them
			 * first, exactly as stream_md_apply() does, so the final
			 * text replaces the active region instead of appending a
			 * duplicate copy below it.
			 */
			if (stream->md_active_rows) {
				fprintf(stdout, FYAI_ANSI_CURSOR_UP_FMT,
					stream->md_active_rows);
				fputs(FYAI_ANSI_ERASE_DOWN, stdout);
				stream->md_active_rows = 0;
			}
			if (!markdown_renderer_finish(&stream->markdown, &out) &&
			    out.len) {
				end = out.len;
				while (end && (out.data[end - 1] == '\n' ||
					       out.data[end - 1] == '\r'))
					end--;
				if (end)
					fwrite(out.data, 1, end, stdout);
			}
			free(out.data);
			putchar('\n');
		} else if (stream->md_full.len) {
			fyai_print_markdown(stream->md_full.data, cfg);
		}
	} else {
		putchar('\n');
	}
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
		if (fy_generic_is_invalid(stream->tool_calls))
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

	stream->tool_calls = fy_replace(gb, stream->tool_calls, (size_t)index, tool_call);
	return fy_generic_is_invalid(stream->tool_calls) ? -1 : 0;
}

static int chat_stream_apply_chunk(struct stream_response *stream, fy_generic chunk)
{
	struct fy_generic_builder *gb = stream->gb;
	struct fyai_cfg *cfg = stream->ctx->cfg;
	fy_generic choice, delta, tool_calls, tool_call, usage, logprobs;
	const char *text;

	usage = fy_get(chunk, "usage");
	if (!fy_generic_is_invalid(usage))
		stream->usage = usage;

	stream->metadata = fy_assoc(gb, stream->metadata,
		"id", fy_get(chunk, "id", ""),
		"created", fy_get(chunk, "created", 0LL),
		"model", fy_get(chunk, "model", ""),
		"service_tier", fy_get(chunk, "service_tier", ""),
		"system_fingerprint", fy_get(chunk, "system_fingerprint", ""));

	choice = fy_get_at_path(chunk, "choices", 0);
	if (fy_generic_is_invalid(choice))
		return 0;

	logprobs = fy_get(choice, "logprobs");
	if (!fy_generic_is_invalid(logprobs) &&
	    !fy_generic_is_null_type(logprobs)) {
		fy_generic content, refusal;

		content = fy_get(logprobs, "content");
		if (!fy_generic_is_invalid(content)) {
			stream->logprob_content = fy_concat(gb,
					stream->logprob_content, content);
			if (cfg->token_extents &&
			    fy_generic_is_valid(stream->token_extents)) {
				stream->token_extents =
					fyai_token_extents_append(gb,
						stream->token_extents, content,
						&stream->extents_pos);
				stream->extents_lp = true;
			}
		}
		refusal = fy_get(logprobs, "refusal");
		if (!fy_generic_is_invalid(refusal))
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
		if (fy_generic_is_invalid(stream->content_chunks))
			return -1;
		stream_write_content(stream, text);
	}

	tool_calls = fy_get(delta, "tool_calls");
	fy_foreach(tool_call, tool_calls) {
		if (stream_apply_tool_call_delta(stream, tool_call))
			return -1;
	}

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

	if (fy_generic_is_invalid(stream->token_extents))
		return;

	entries = fy_get(event, "logprobs");
	if (fy_generic_is_valid(entries) && fy_len(entries)) {
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

	msg = fy_get(event, "message", fy_invalid);
	if (fy_generic_is_invalid(msg)) {
		err = fy_get(event, "error", fy_invalid);
		msg = fy_get(err, "message", fy_invalid);
	}
	if (fy_generic_is_invalid(msg)) {
		resp = fy_get(event, "response", fy_invalid);
		err = fy_get(resp, "error", fy_invalid);
		msg = fy_get(err, "message", fy_invalid);
		if (fy_generic_is_invalid(msg))
			msg = fy_get(fy_get(resp, "incomplete_details",
					    fy_invalid), "reason", fy_invalid);
	}

	what = fy_castp(&type, "error");
	if (fy_generic_is_valid(msg) && !fy_generic_is_null_type(msg))
		fyai_error(ctx, "%s: %s", what, fy_castp(&msg, ""));
	else
		fyai_error(ctx, "provider sent %s and no reason", what);
}

static int responses_stream_apply_event(struct stream_response *stream,
					fy_generic event)
{
	struct fyai_cfg *cfg = stream->ctx->cfg;
	fy_generic type;
	const char *delta;

	type = fy_get(event, "type");

	if (fy_equal(type, "response.reasoning_summary_text.delta") ||
	    fy_equal(type, "response.reasoning_text.delta")) {
		stream_write_reasoning(stream, fy_get(event, "delta", ""));
		return 0;
	}

	if (fy_equal(type, "response.output_text.delta")) {
		delta = fy_get(event, "delta", "");
		if (*delta) {
			stream_finish_reasoning(stream);
			if (cfg->token_extents)
				responses_collect_extents(stream, event, delta);
			stream_write_content(stream, delta);
		}
		return 0;
	}

	if (fy_equal(type, "response.completed")) {
		stream->completed_response = fy_get(event, "response");
		stream->done = true;
		stream_finish_content(stream);
		return fy_generic_is_invalid(stream->completed_response) ? -1 : 0;
	}

	/*
	 * The provider says why on the event itself: an error object, or
	 * incomplete_details naming the limit that stopped it (commonly
	 * max_output_tokens). Report it - dropping it leaves the engine with
	 * nothing but a bare "request failed".
	 */
	if (fy_equal(type, "response.failed") ||
	    fy_equal(type, "response.incomplete") ||
	    fy_equal(type, "error")) {
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
	const char *text;
	long long index;

	type = fy_get(event, "type");

	if (fy_equal(type, "message_start")) {
		message = fy_get(event, "message");
		stream->metadata = fy_assoc(gb, stream->metadata,
			"id", fy_get(message, "id", ""),
			"model", fy_get(message, "model", ""));
		usage = fy_get(message, "usage");
		if (fy_generic_is_valid(usage))
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
		return fy_generic_is_invalid(stream->tool_calls) ? -1 : 0;
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
				if (fy_generic_is_invalid(stream->content_chunks))
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
			return fy_generic_is_invalid(stream->tool_calls) ? -1 : 0;
		}

		return 0;
	}

	if (fy_equal(type, "message_delta")) {
		text = fy_cast(fy_get_at_path(event, "delta", "stop_reason"), "");
		if (*text)
			stream->finish_reason = fy_value(gb, text);
		usage = fy_get(event, "usage");
		if (fy_generic_is_valid(usage))
			stream->usage = fy_assoc(gb,
				fy_generic_is_valid(stream->usage) ?
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
	if (fy_generic_is_invalid(chunk))
		return -1;

	if (cfg->debug > 1)
		emit_generic_to_stdout("stream", chunk, true);

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
	if (fy_generic_is_invalid(tool_calls))
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
				"refusal", fy_generic_is_valid(stream->logprob_refusal) ?
					stream->logprob_refusal : fy_null) :
			fy_null,
		"finish_reason", fy_generic_is_valid(stream->finish_reason) ?
			stream->finish_reason : fy_string("stop"));

	doc = fy_mapping(
		"id", fy_get(stream->metadata, "id", ""),
		"object", "chat.completion",
		"created", fy_get(stream->metadata, "created", 0LL),
		"model", fy_get(stream->metadata, "model", ""),
		"choices", fy_sequence(choice));
	if (!fy_generic_is_invalid(stream->usage))
		doc = fy_assoc(doc, "usage", stream->usage);
	doc = fy_assoc(doc,
		"service_tier", fy_get(stream->metadata, "service_tier", ""),
		"system_fingerprint", fy_get(stream->metadata,
					     "system_fingerprint", ""));

	if (fy_generic_is_invalid(doc)) {
		fyai_error(ctx, "could not build the streamed response");
		return fy_invalid;
	}
	doc = fy_gb_internalize(gb, doc);
	if (fy_generic_is_invalid(doc))
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
		if (fy_generic_is_invalid(input))
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
		"stop_reason", fy_generic_is_valid(stream->finish_reason) ?
			stream->finish_reason : fy_string("end_turn"));
	if (!fy_generic_is_invalid(stream->usage))
		doc = fy_assoc(doc, "usage", stream->usage);

	if (fy_generic_is_invalid(doc)) {
		fyai_error(stream->ctx, "could not build the streamed message");
		return fy_invalid;
	}
	doc = fy_gb_internalize(gb, doc);
	if (fy_generic_is_invalid(doc))
		fyai_error(stream->ctx, "could not retain the streamed message");
	return doc;
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

	if (fy_generic_is_invalid(extents) || !fy_len(extents))
		return;

	ctx->last_token_extents = fy_gb_internalize(ctx->transient_gb, extents);
}

fy_generic fyai_perform_streaming_request(struct fyai_ctx *ctx)
{
	struct stream_response stream = {
		.ctx = ctx,
		.content_chunks = fy_seq_empty,
		.tool_calls = fy_seq_empty,
		.metadata = fy_map_empty,
		.logprob_content = fy_seq_empty,
		.logprob_refusal = fy_invalid,
		.finish_reason = fy_invalid,
		.usage = fy_invalid,
		.completed_response = fy_invalid,
		.token_extents = fy_seq_empty,
	};
	struct fy_generic_builder_cfg gb_cfg = {
		.flags = FYGBCF_CREATE_ALLOCATOR |
			 FYGBCF_SCOPE_LEADER |
			 FYGBCF_DEDUP_ENABLED,
		.parent = ctx->gb,
	};
	CURLcode res;
	long status = 0;
	fy_generic ret = fy_invalid;
	struct fyai_cfg *cfg = ctx->cfg;

	stream.gb = fy_generic_builder_create(&gb_cfg);
	if (!stream.gb)
		return fy_invalid;

	if (cfg->markdown && markdown_available(cfg)) {
		stream.md_tty = ctx->stdout_tty;
		if (stream.md_tty && strcmp(cfg->markdown_mode, "oneshot")) {
			if (markdown_renderer_start(cfg, &stream.markdown,
						    markdown_color_enabled(cfg->color),
						    cfg->theme))
				stream.markdown.active = false;
		} else {
			stream.markdown.active = true;
		}
	}

	curl_easy_setopt(ctx->curl, CURLOPT_WRITEFUNCTION, write_stream_response);
	curl_easy_setopt(ctx->curl, CURLOPT_WRITEDATA, &stream);

	res = curl_easy_perform(ctx->curl);
	if (res == CURLE_ABORTED_BY_CALLBACK) {
		/* ^C: carry the reason on the (invalid) result, don't print. */
		ret = fyai_with_diag(ctx->transient_gb, fy_invalid, "interrupted");
		goto out;
	}
	fyai_error_check(ctx, res == CURLE_OK, out, "request failed: %s",
			 curl_easy_strerror(res));

	curl_easy_getinfo(ctx->curl, CURLINFO_RESPONSE_CODE, &status);
	if (status < 200 || status >= 300) {
		if (fyai_auth_should_retry(ctx, status)) {
			stream_response_cleanup(&stream);
			if (!fyai_auth_prepare_retry(ctx))
				return fyai_perform_streaming_request(ctx);
			return fy_invalid;
		}
		ctx->response_chain_miss =
			response_chain_miss(ctx, status, stream.raw.data);
		if (ctx->response_chain_miss)
			goto out;
		/*
		 * The body is detail on this failure, not a second one: raised
		 * separately it would be demoted behind the status and lost.
		 */
		fyai_error(ctx, "request returned HTTP %ld%s%s", status,
			   stream.raw.data ? "\n" : "",
			   stream.raw.data ? stream.raw.data : "");
		goto out;
	}

	/*
	 * A failure event already said why (and demotes this); a stream that
	 * simply stopped never did. Say so rather than leave the engine to
	 * report a bare "request failed".
	 */
	if (stream.failed)
		goto out;
	if (!stream.done) {
		fyai_error(ctx, "the response stream ended before it completed "
			   "(after %zu bytes)", stream.received_bytes);
		goto out;
	}

	switch (cfg->api_mode) {
	case FYAI_API_RESPONSES:
		ret = stream.completed_response;
		break;
	case FYAI_API_CHAT_COMPLETIONS:
		ret = stream_build_response_doc(&stream);
		break;
	case FYAI_API_MESSAGES:
		ret = messages_build_response_doc(&stream);
		break;
	}

	if (fy_generic_is_valid(ret))
		stream_store_token_extents(&stream, ret);

out:

	/*
	 * A failed request (transport error, non-2xx, provider failure event,
	 * stream cut before completion) leaves ret invalid; return it as-is so
	 * the engine can fail the run cleanly instead of asserting.
	 */
	if (fy_generic_is_valid(ret))
		ret = fy_gb_internalize(ctx->transient_gb, ret);
	stream_response_cleanup(&stream);

	return ret;
}

fy_generic fyai_perform_buffered_request(struct fyai_ctx *ctx)
{
	struct response_buffer response = {};
	fy_generic response_doc;
	CURLcode res;
	long status = 0;
	fy_generic ret = fy_invalid;

	curl_easy_setopt(ctx->curl, CURLOPT_WRITEFUNCTION, write_response);
	curl_easy_setopt(ctx->curl, CURLOPT_WRITEDATA, &response);

	res = curl_easy_perform(ctx->curl);
	if (res == CURLE_ABORTED_BY_CALLBACK) {
		ret = fyai_with_diag(ctx->transient_gb, fy_invalid, "interrupted");
		goto out;
	}
	fyai_error_check(ctx, res == CURLE_OK, out, "request failed: %s",
			 curl_easy_strerror(res));

	curl_easy_getinfo(ctx->curl, CURLINFO_RESPONSE_CODE, &status);
	if (status < 200 || status >= 300) {
		if (fyai_auth_should_retry(ctx, status)) {
			free(response.data);
			if (!fyai_auth_prepare_retry(ctx))
				return fyai_perform_buffered_request(ctx);
			return fy_invalid;
		}
		ctx->response_chain_miss =
			response_chain_miss(ctx, status, response.data);
		if (ctx->response_chain_miss)
			goto out;
		/* One diagnostic; see the streaming path above. */
		fyai_error(ctx, "request returned HTTP %ld%s%s", status,
			   response.data ? "\n" : "",
			   response.data ? response.data : "");
		goto out;
	}

	if (!response.data)
		goto out;

	response_doc = parse_response(ctx->transient_gb, response.data);
	ret = response_doc;
out:
	free(response.data);

	return ret;
}
