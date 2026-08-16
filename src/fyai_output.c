/*
 * fyai_output.c - context-owned tagged transcript documents
 *
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_DISPLAY

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai_output.h"
#include "fyai_sink.h"
#include "fyai_turn.h"

/* Durable Markdown source and fragment markers for one transcript document. */
struct fyai_display_output {
	enum fyai_output_tag tag;
	struct response_buffer markdown;
	fy_generic fragments;
	bool reasoning;
};

static enum fyai_sink_doc_kind fyai_output_doc_kind(enum fyai_output_tag tag)
{
	switch (tag) {
	case FYAI_OUTPUT_SYSTEM:
		return FYAI_SINK_DOC_SYSTEM;
	case FYAI_OUTPUT_USER:
		return FYAI_SINK_DOC_USER;
	case FYAI_OUTPUT_ASSISTANT:
		break;
	}
	return FYAI_SINK_DOC_ASSISTANT;
}

const char *fyai_output_tag_name(enum fyai_output_tag tag)
{
	switch (tag) {
	case FYAI_OUTPUT_SYSTEM:
		return "system";
	case FYAI_OUTPUT_USER:
		return "user";
	case FYAI_OUTPUT_ASSISTANT:
		return "assistant";
	}
	return "assistant";
}

void fyai_output_cleanup(struct fyai_ctx *ctx)
{
	if (!ctx || !ctx->display_output)
		return;
	fyai_sink_doc_discard(ctx->sink);
	free(ctx->display_output->markdown.data);
	free(ctx->display_output);
	ctx->display_output = NULL;
}

int fyai_output_begin(struct fyai_ctx *ctx, enum fyai_output_tag tag)
{
	struct fyai_display_output *output;
	int rc;

	if (!ctx)
		return -1;
	fyai_output_cleanup(ctx);
	output = calloc(1, sizeof(*output));
	fyai_error_check(ctx, output, err,
			 "could not allocate display output");
	output->tag = tag;
	output->fragments = fy_seq_empty;
	ctx->display_output = output;
	rc = fyai_sink_doc_begin(ctx->sink, fyai_output_doc_kind(tag));
	fyai_error_check(ctx, !rc,
		err_output, "could not start display output");
	return 0;
err_output:
	fyai_output_cleanup(ctx);
err:
	return -1;
}

static int fyai_output_record_source(struct fyai_ctx *ctx, const char *text,
				     size_t len)
{
	struct fyai_display_output *output = ctx->display_output;
	int rc;

	rc = response_buffer_reserve(&output->markdown,
				     output->markdown.len + len + 1);
	fyai_error_check(ctx, !rc, err,
		"could not grow display output");
	memcpy(output->markdown.data + output->markdown.len, text, len);
	output->markdown.len += len;
	output->markdown.data[output->markdown.len] = '\0';
	return 0;
err:
	return -1;
}

int fyai_output_append(struct fyai_ctx *ctx, const char *text, size_t len)
{
	int rc;

	if (!ctx || !ctx->display_output || (!text && len))
		return -1;
	if (!len)
		return 0;
	rc = fyai_output_record_source(ctx, text, len);
	fyai_error_check(ctx, !rc, err,
			 "could not grow display output");
	rc = fyai_sink_doc_append(ctx->sink, text, len);
	fyai_error_check(ctx, !rc, err,
			 "could not render display output");
	return 0;
err:
	return -1;
}

int fyai_output_append_recorded(struct fyai_ctx *ctx, const char *text,
				size_t len)
{
	if (!ctx || !ctx->display_output || (!text && len))
		return -1;
	if (!len)
		return 0;
	return fyai_output_record_source(ctx, text, len);
}

int fyai_output_append_string(struct fyai_ctx *ctx, const char *text)
{
	return fyai_output_append(ctx, text ? text : "",
				  text ? strlen(text) : 0);
}

int fyai_output_printf(struct fyai_ctx *ctx, const char *fmt, ...)
{
	va_list ap;
	char *text;
	int len;
	int rc;

	va_start(ap, fmt);
	len = vasprintf(&text, fmt, ap);
	va_end(ap);
	fyai_error_check(ctx, len >= 0, err,
			 "could not format display output");
	rc = fyai_output_append(ctx, text, (size_t)len);
	free(text);
	fyai_error_check(ctx, !rc, err,
			 "could not append display output");
	return 0;
err:
	return -1;
}

int fyai_output_start_block(struct fyai_ctx *ctx)
{
	struct fyai_display_output *output;
	size_t len;

	if (!ctx || !ctx->display_output)
		return -1;
	output = ctx->display_output;
	len = output->markdown.len;

	if (!len)
		return 0;
	if (output->markdown.data[len - 1] != '\n')
		return fyai_output_append_string(ctx, "\n\n");
	if (len < 2 || output->markdown.data[len - 2] != '\n')
		return fyai_output_append_string(ctx, "\n");
	return 0;
}

/*
 * Reasoning is ordinary Markdown inside the assistant document. A blockquote
 * is deliberately used instead of per-line emphasis: it remains well-formed
 * while arbitrary provider chunks split in the middle of a line.
 */
int fyai_output_reasoning_append(struct fyai_ctx *ctx, const char *text)
{
	struct fyai_display_output *output;
	const char *p, *nl;
	size_t len;

	if (!ctx || !ctx->display_output || !text || !*text)
		return 0;
	output = ctx->display_output;
	if (!output->reasoning) {
		if (fyai_output_start_block(ctx))
			return -1;
		if (fyai_output_printf(ctx, "> **%s**\n>\n> ",
				       "💭 reasoning"))
			return -1;
		output->reasoning = true;
	}
	p = text;
	while (*p) {
		nl = strchr(p, '\n');
		len = nl ? (size_t)(nl - p) : strlen(p);
		if (len && fyai_output_append(ctx, p, len))
			return -1;
		if (!nl)
			break;
		if (fyai_output_append_string(ctx, "\n> "))
			return -1;
		p = nl + 1;
	}
	return 0;
}

int fyai_output_reasoning_finish(struct fyai_ctx *ctx)
{
	struct fyai_display_output *output;

	if (!ctx || !ctx->display_output)
		return 0;
	output = ctx->display_output;
	if (!output->reasoning)
		return 0;
	output->reasoning = false;
	return fyai_output_append_string(ctx, "\n\n");
}

const char *fyai_output_markdown(const struct fyai_ctx *ctx, size_t *len)
{
	if (len)
		*len = ctx && ctx->display_output ?
			ctx->display_output->markdown.len : 0;
	return ctx && ctx->display_output &&
	       ctx->display_output->markdown.data ?
		ctx->display_output->markdown.data : "";
}

bool fyai_output_renders_live(const struct fyai_ctx *ctx)
{
	return ctx && ctx->display_output && fyai_sink_doc_is_live(ctx->sink);
}

int fyai_output_add_fragment(struct fyai_ctx *ctx, const char *kind,
			     size_t start, size_t end, const char *lang,
			     const char *tool)
{
	struct fyai_display_output *output;
	fy_generic fragment;

	if (!ctx || !ctx->display_output || end < start)
		return -1;
	output = ctx->display_output;
	fragment = fy_null_filtered_mapping(
		"kind", kind,
		"start", (long long)start,
		"end", (long long)end,
		"lang", lang && *lang ? fy_value(lang) : fy_null,
		"tool", tool && *tool ? fy_value(tool) : fy_null);
	output->fragments = fy_append(ctx->transient_gb, output->fragments,
				      fragment);
	fyai_error_check(ctx, fy_is_valid(output->fragments), err,
			 "could not append display fragment");
	return 0;
err:
	return -1;
}

/* Store a tool title fragment and its outcome. */
int fyai_output_add_tool_head_fragment(struct fyai_ctx *ctx, size_t start,
				       size_t end, const char *tool, bool ok,
				       const char *cause)
{
	struct fyai_display_output *output;
	fy_generic fragment;

	if (!ctx || !ctx->display_output || end < start)
		return -1;
	output = ctx->display_output;
	fragment = fy_null_filtered_mapping(
		"kind", "tool_head",
		"start", (long long)start,
		"end", (long long)end,
		"tool", !fy_str_empty(tool) ? fy_value(tool) : fy_null,
		"ok", ok ? fy_true : fy_false,
		"cause", !fy_str_empty(cause) ? fy_value(cause) : fy_null);
	output->fragments = fy_append(ctx->transient_gb, output->fragments,
				      fragment);
	fyai_error_check(ctx, fy_is_valid(output->fragments), err,
			 "could not append tool head fragment");
	return 0;
err:
	return -1;
}

int fyai_output_checkpoint(struct fyai_ctx *ctx)
{
	int rc;

	if (!fyai_output_renders_live(ctx))
		return 0;
	rc = fyai_sink_doc_pause(ctx->sink);
	fyai_error_check(ctx, !rc, err,
			 "could not checkpoint display output");
	return 0;
err:
	return -1;
}

int fyai_output_resume(struct fyai_ctx *ctx)
{
	int rc;

	if (!ctx || !ctx->display_output)
		return -1;
	rc = fyai_sink_doc_resume(ctx->sink);
	fyai_error_check(ctx, !rc, err,
			 "could not resume display output");
	return 0;
err:
	return -1;
}

fy_generic fyai_output_finalize(struct fyai_ctx *ctx, fy_generic turn,
				bool aborted)
{
	struct fyai_display_output *output;
	fy_generic record;
	int rc;

	if (!ctx || !ctx->display_output || fy_is_invalid(turn))
		return turn;
	if (fy_is_null(turn)) {
		fyai_output_cleanup(ctx);
		return turn;
	}
	output = ctx->display_output;
	(void)fyai_output_reasoning_finish(ctx);
	rc = fyai_sink_doc_end(ctx->sink, aborted);
	fyai_error_check(ctx, !rc, err,
			 "could not finalize display renderer");
	record = fy_mapping(
		"tag", fyai_output_tag_name(output->tag),
		"markdown", output->markdown.data ? output->markdown.data : "",
		"state", aborted ? "aborted" : "finalized",
		"fragments", output->fragments);
	turn = fyai_turn_append_display_output(ctx, turn, record);
	fyai_output_cleanup(ctx);
	return turn;
err:
	fyai_output_cleanup(ctx);
	return fy_invalid;
}

fy_generic fyai_output_record(struct fyai_ctx *ctx, fy_generic turn,
			      enum fyai_output_tag tag, const char *markdown)
{
	if (fyai_output_begin(ctx, tag) ||
	    fyai_output_append_string(ctx, markdown)) {
		fyai_error(ctx, "could not build display output");
		fyai_output_abort(ctx);
		return fy_invalid;
	}
	return fyai_output_finalize(ctx, turn, false);
}

void fyai_output_abort(struct fyai_ctx *ctx)
{
	fyai_output_cleanup(ctx);
}
