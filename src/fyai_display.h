/* SPDX-License-Identifier: MIT */
#ifndef FYAI_DISPLAY_H
#define FYAI_DISPLAY_H

#include "fyai.h"

fy_generic fyai_stats_data(struct fyai_ctx *ctx, struct fy_generic_builder *gb);
int fyai_show_stats(struct fyai_ctx *ctx);
int fyai_display_view(struct fyai_ctx *ctx);
int fyai_display_recap(struct fyai_ctx *ctx, int max_exchanges, int max_rows);
int fyai_export_view(struct fyai_ctx *ctx, const char *path);
int fyai_import_view(struct fyai_ctx *ctx, const char *path);
int fyai_replay_view(struct fyai_ctx *ctx, bool ignore_compact);
int fyai_dump_view(struct fyai_ctx *ctx);
fy_generic fyai_list_turns_data(struct fyai_ctx *ctx,
				struct fy_generic_builder *gb);
fy_generic fyai_list_exchanges_data(struct fyai_ctx *ctx,
				    struct fy_generic_builder *gb);
fy_generic fyai_list_reflog_data(struct fyai_ctx *ctx,
				 struct fy_generic_builder *gb);
int fyai_list_turns(struct fyai_ctx *ctx);
void fyai_interactive_recap(struct fyai_ctx *ctx);
void fyai_echo_user_turn(struct fyai_ctx *ctx, const char *line);
void fyai_render_tool_exchange(struct fyai_ctx *ctx, fy_generic tool_call,
			       fy_generic tool_result, bool tool_ok);
void fyai_render_tool_result_exchange(struct fyai_ctx *ctx,
				      fy_generic tool_call,
				      fy_generic tool_result);
int fyai_record_tool_exchange(struct fyai_ctx *ctx, fy_generic tool_call,
			      fy_generic tool_result, bool tool_ok);
/*
 * Print @lead and @url, as a Markdown link labelled @label on a terminal (which
 * renders to an OSC 8 hyperlink) or as plain text otherwise. Keep @label short:
 * the raw URL does not fit a terminal row, and the transcript clips what does.
 */
void fyai_print_login_url(struct fyai_ctx *ctx, const char *lead,
			  const char *label, const char *url);
void fyai_render_tool_result(struct fyai_sink *sink, struct fyai_cfg *cfg,
			     fy_generic content,
			     const char *lang, int preview_lines);
int fyai_render_display_output(struct fyai_ctx *ctx, const char *tag,
			       const char *markdown);
/* Build the title and marked body of a tool call. */
int fyai_tool_call_view(struct fyai_ctx *ctx, const char *name, fy_generic args,
			int preview_lines, char **title,
			struct response_buffer *body);
/* One fenced tool-body range in emitted Markdown. */
struct fyai_md_block {
	size_t start;
	size_t end;
	char *lang;
};

struct fyai_md_blocks {
	struct fyai_md_block *item;
	size_t count;
	size_t cap;
};

void fyai_md_blocks_free(struct fyai_md_blocks *blocks);
void fyai_emit_tool_call(struct fyai_ctx *ctx, FILE *mf,
			 struct fy_generic_builder *gb,
			 const char *name, fy_generic args, int preview_lines,
			 struct fyai_md_blocks *blocks);
int fyai_tool_preview_lines(const struct fyai_cfg *cfg, const char *name);

#endif
