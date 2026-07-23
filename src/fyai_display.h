/* SPDX-License-Identifier: MIT */
#ifndef FYAI_DISPLAY_H
#define FYAI_DISPLAY_H

#include "fyai.h"

fy_generic fyai_stats_data(struct fyai_ctx *ctx, struct fy_generic_builder *gb);
int fyai_show_stats(struct fyai_ctx *ctx);
int fyai_display_view(struct fyai_ctx *ctx);
int fyai_dump_view(struct fyai_ctx *ctx);
fy_generic fyai_list_turns_data(struct fyai_ctx *ctx,
				struct fy_generic_builder *gb);
fy_generic fyai_list_exchanges_data(struct fyai_ctx *ctx,
				    struct fy_generic_builder *gb);
fy_generic fyai_list_reflog_data(struct fyai_ctx *ctx,
				 struct fy_generic_builder *gb);
int fyai_list_turns(struct fyai_ctx *ctx);
char *fyai_edit_line(struct fyai_ctx *ctx, const char *current);
void fyai_interactive_recap(struct fyai_ctx *ctx);
void fyai_echo_user_turn(struct fyai_ctx *ctx, const char *line);
void fyai_render_tool_exchange(struct fyai_ctx *ctx, fy_generic tool_call,
			       fy_generic tool_result);
int fyai_record_tool_exchange(struct fyai_ctx *ctx, fy_generic tool_call,
			      fy_generic tool_result);
void fyai_render_tool_result(struct fyai_cfg *cfg, fy_generic content,
			     const char *lang, int preview_lines);
int fyai_render_display_output(struct fyai_ctx *ctx, const char *tag,
			       const char *markdown);
/*
 * Emit a tool-call header as markdown into @mf (bold tool name plus the
 * command/path, with input-body previews for write_file/apply_patch). Shared by
 * the history view and the live loop so a tool's header stays identical in both.
 */
void fyai_emit_tool_call(FILE *mf, struct fy_generic_builder *gb,
			 const char *name, fy_generic args, int preview_lines);

#endif
