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

#include "fyai_markdown.h"
#include "fyai_output.h"
#include "fyai_terminal.h"
#include "fyai_turn.h"
#include "fyai_ui.h"

struct fyai_display_output {
	enum fyai_output_tag tag;
	struct response_buffer markdown;
	struct response_buffer pending;
	struct markdown_renderer renderer;
	fy_generic fragments;
	size_t active_rows;
	bool render_live;
	bool reasoning;
};

static int fyai_output_renderer_start(struct fyai_ctx *ctx,
				      struct fyai_display_output *output)
{
	if (output->tag != FYAI_OUTPUT_ASSISTANT || !ctx->cfg->markdown ||
	    !ctx->stdout_tty || !markdown_available(ctx->cfg))
		return 0;
	fyai_error_check(ctx,
		!markdown_renderer_start(ctx->cfg, &output->renderer,
			markdown_color_enabled(ctx->cfg->color),
			ctx->cfg->theme), err,
		"could not start display renderer");
	output->render_live = true;
	return 0;
err:
	return -1;
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
	markdown_renderer_destroy(&ctx->display_output->renderer);
	free(ctx->display_output->markdown.data);
	free(ctx->display_output->pending.data);
	free(ctx->display_output);
	ctx->display_output = NULL;
}

int fyai_output_begin(struct fyai_ctx *ctx, enum fyai_output_tag tag)
{
	struct fyai_display_output *output;

	if (!ctx)
		return -1;
	fyai_output_cleanup(ctx);
	output = calloc(1, sizeof(*output));
	fyai_error_check(ctx, output, err,
			 "could not allocate display output");
	output->tag = tag;
	output->fragments = fy_seq_empty;
	fyai_error_check(ctx, !fyai_output_renderer_start(ctx, output),
			 err_output, "could not start display output");
	ctx->display_output = output;
	return 0;
err_output:
	free(output);
err:
	return -1;
}

static size_t output_newlines(const char *text, size_t len)
{
	size_t i, rows = 0;

	for (i = 0; i < len; i++)
		if (text[i] == '\n')
			rows++;
	return rows;
}

static void output_apply(struct fyai_ctx *ctx,
			 const struct markdown_update *update)
{
	struct fyai_display_output *output = ctx->display_output;
	size_t backtrack = update->backtrack;

	if (fyai_ui_active(ctx)) {
		(void)fyai_ui_tail_apply(ctx, update);
		return;
	}
	if (backtrack) {
		fprintf(stdout, FYAI_ANSI_CURSOR_UP_FMT, backtrack);
		fputs(FYAI_ANSI_ERASE_DOWN, stdout);
		output->active_rows -= backtrack;
	}
	if (update->content_len)
		fwrite(update->content, 1, update->content_len, stdout);
	output->active_rows += output_newlines(update->content,
					      update->content_len);
	if (update->freeze >= output->active_rows)
		output->active_rows = 0;
	else
		output->active_rows -= update->freeze;
	fflush(stdout);
}

static int fyai_output_render_pending(struct fyai_ctx *ctx)
{
	struct fyai_display_output *output = ctx->display_output;
	struct markdown_update update;

	if (!output->render_live || !output->pending.len)
		return 0;
	if (markdown_renderer_push(&output->renderer, output->pending.data,
				   output->pending.len, &update))
		return -1;
	output_apply(ctx, &update);
	output->pending.len = 0;
	output->pending.data[0] = '\0';
	return 0;
}

int fyai_output_append(struct fyai_ctx *ctx, const char *text, size_t len)
{
	struct fyai_display_output *output;

	if (!ctx || !ctx->display_output || (!text && len))
		return -1;
	output = ctx->display_output;
	if (!len)
		return 0;
	fyai_error_check(ctx,
		!response_buffer_reserve(&output->markdown,
			output->markdown.len + len + 1), err,
		"could not grow display output");
	memcpy(output->markdown.data + output->markdown.len, text, len);
	output->markdown.len += len;
	output->markdown.data[output->markdown.len] = '\0';
	if (output->render_live) {
		fyai_error_check(ctx,
			!response_buffer_reserve(&output->pending,
				output->pending.len + len + 1), err,
			"could not grow progressive display output");
		memcpy(output->pending.data + output->pending.len, text, len);
		output->pending.len += len;
		output->pending.data[output->pending.len] = '\0';
		fyai_error_check(ctx, !fyai_output_render_pending(ctx), err,
				 "could not render display output");
	}
	return 0;
err:
	return -1;
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
	return ctx && ctx->display_output &&
	       ctx->display_output->render_live;
}

int fyai_output_add_fragment(struct fyai_ctx *ctx, const char *kind,
			     size_t start, size_t end, const char *lang)
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
		"lang", lang && *lang ? fy_value(lang) : fy_null);
	output->fragments = fy_append(ctx->transient_gb, output->fragments,
				      fragment);
	fyai_error_check(ctx, fy_generic_is_valid(output->fragments), err,
			 "could not append display fragment");
	return 0;
err:
	return -1;
}

static int fyai_output_render_finish(struct fyai_ctx *ctx)
{
	struct fyai_display_output *output = ctx->display_output;
	struct response_buffer rendered = {0};
	size_t end;

	if (!output || !output->render_live)
		return 0;
	if (fyai_output_render_pending(ctx) ||
	    markdown_renderer_finish(&output->renderer, &rendered)) {
		free(rendered.data);
		return -1;
	}
	end = rendered.len;
	while (end && (rendered.data[end - 1] == '\n' ||
		       rendered.data[end - 1] == '\r'))
		end--;
	if (fyai_ui_active(ctx))
		fyai_ui_tail_finish(ctx, rendered.data, end);
	else {
		if (output->active_rows) {
			fprintf(stdout, FYAI_ANSI_CURSOR_UP_FMT,
				output->active_rows);
			fputs(FYAI_ANSI_ERASE_DOWN, stdout);
		}
		if (end)
			fwrite(rendered.data, 1, end, stdout);
		fputc('\n', stdout);
		fflush(stdout);
	}
	output->active_rows = 0;
	output->render_live = false;
	free(rendered.data);
	return 0;
}

int fyai_output_checkpoint(struct fyai_ctx *ctx)
{
	if (!fyai_output_renders_live(ctx))
		return 0;
	fyai_error_check(ctx, !fyai_output_render_finish(ctx), err,
			 "could not checkpoint display output");
	markdown_renderer_destroy(&ctx->display_output->renderer);
	return 0;
err:
	return -1;
}

int fyai_output_resume(struct fyai_ctx *ctx)
{
	struct fyai_display_output *output;

	if (!ctx || !ctx->display_output)
		return -1;
	output = ctx->display_output;
	if (output->render_live)
		return 0;
	fyai_error_check(ctx, !fyai_output_renderer_start(ctx, output), err,
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

	if (!ctx || !ctx->display_output || fy_generic_is_invalid(turn))
		return turn;
	if (fy_generic_is_null_type(turn)) {
		fyai_output_cleanup(ctx);
		return turn;
	}
	output = ctx->display_output;
	(void)fyai_output_reasoning_finish(ctx);
	if (fyai_output_render_finish(ctx)) {
		fyai_error(ctx, "could not finalize display renderer");
		fyai_output_cleanup(ctx);
		return fy_invalid;
	}
	record = fy_mapping(
		"tag", fyai_output_tag_name(output->tag),
		"markdown", output->markdown.data ? output->markdown.data : "",
		"state", aborted ? "aborted" : "finalized",
		"fragments", output->fragments);
	turn = fyai_turn_append_display_output(ctx, turn, record);
	fyai_output_cleanup(ctx);
	return turn;
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
