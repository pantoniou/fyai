/* SPDX-License-Identifier: MIT */
#ifndef FYAI_UI_H
#define FYAI_UI_H

#include <stdbool.h>
#include <stddef.h>

struct fyai_ctx;
struct markdown_update;
struct fytim_workband;

int fyai_ui_open(struct fyai_ctx *ctx);
void fyai_ui_close(struct fyai_ctx *ctx);
bool fyai_ui_active(const struct fyai_ctx *ctx);
char *fyai_ui_readline(struct fyai_ctx *ctx);
void fyai_ui_drain_output(struct fyai_ctx *ctx);
void fyai_ui_history_load(struct fyai_ctx *ctx, const char *path);
void fyai_ui_history_save(struct fyai_ctx *ctx, const char *path,
			  const char *line);
int fyai_ui_commit(struct fyai_ctx *ctx, const char *buf, size_t len);
int fyai_ui_tail_apply(struct fyai_ctx *ctx, const struct markdown_update *upd);
void fyai_ui_tail_finish(struct fyai_ctx *ctx, const char *buf, size_t len);
void fyai_ui_set_busy(struct fyai_ctx *ctx, bool busy);
void fyai_ui_signal(struct fyai_ctx *ctx, int signo);
void fyai_ui_update_banner(struct fyai_ctx *ctx, const char *top,
			   const char *bottom);
int fyai_ui_update_prompt_style(struct fyai_ctx *ctx);
int fyai_ui_external_begin(struct fyai_ctx *ctx);
int fyai_ui_external_end(struct fyai_ctx *ctx);
struct fytim_workband *fyai_ui_workband_create(struct fyai_ctx *ctx);
void fyai_ui_tool_begin(struct fyai_ctx *ctx, const char *title);
void fyai_ui_tool_update(struct fyai_ctx *ctx, const char *body, size_t len);
void fyai_ui_tool_end(struct fyai_ctx *ctx, bool ok);
void fyai_ui_pane_begin(struct fyai_ctx *ctx);
void fyai_ui_pane_end(struct fyai_ctx *ctx, const char *title, bool error,
		      bool show_output);
void fyai_ui_diag_drain(struct fyai_ctx *ctx, const char *title);

#endif
