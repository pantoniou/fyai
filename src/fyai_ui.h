/* SPDX-License-Identifier: MIT */
#ifndef FYAI_UI_H
#define FYAI_UI_H

#include <stdbool.h>
#include <stddef.h>

struct fyai_ctx;
struct markdown_update;
struct fytim_workband;
struct fytim_surface;
struct fyai_terminal_view;

/* Bytes the user typed for the surface holding the keys. */
typedef void (*fyai_ui_keys_fn)(void *user, const char *data, size_t len);

int fyai_ui_open(struct fyai_ctx *ctx);
void fyai_ui_close(struct fyai_ctx *ctx);

/*
 * Say if a user types here. A sub-agent draws on a terminal that only the
 * parent reads. It therefore asks for no prompt, and the rows go to its work.
 */
void fyai_ui_prompt_enabled(struct fyai_ctx *ctx, bool enabled);
bool fyai_ui_active(const struct fyai_ctx *ctx);
char *fyai_ui_readline(struct fyai_ctx *ctx);
char *fyai_ui_take_line(struct fyai_ctx *ctx);
bool fyai_ui_quit_requested(const struct fyai_ctx *ctx);
void fyai_ui_drain_output(struct fyai_ctx *ctx);
void fyai_ui_history_load(struct fyai_ctx *ctx, const char *path);
void fyai_ui_history_save(struct fyai_ctx *ctx, const char *path,
			  const char *line);
int fyai_ui_commit(struct fyai_ctx *ctx, const char *buf, size_t len);
int fyai_ui_tail_apply(struct fyai_ctx *ctx, const struct markdown_update *upd);
void fyai_ui_tail_finish(struct fyai_ctx *ctx, const char *buf, size_t len);
void fyai_ui_set_busy(struct fyai_ctx *ctx, bool busy);
/* An interrupt reached the session (Escape, or SIGINT from ^C). Discards a
 * half-typed line on an idle prompt, ends the session when there is nothing to
 * discard, and cancels the turn while busy. */
void fyai_ui_interrupt(struct fyai_ctx *ctx);

void fyai_ui_signal(struct fyai_ctx *ctx, int signo);
void fyai_ui_update_banner(struct fyai_ctx *ctx, const char *top,
			   const char *bottom);
int fyai_ui_update_prompt_style(struct fyai_ctx *ctx);
int fyai_ui_external_begin(struct fyai_ctx *ctx);
int fyai_ui_external_end(struct fyai_ctx *ctx);
struct fytim_workband *fyai_ui_workband_create(struct fyai_ctx *ctx);
void fyai_ui_workband_update(struct fyai_ctx *ctx,
			     struct fytim_workband *band,
			     const char *title, const char *body, size_t len,
			     const char *first_margin);
void fyai_ui_shell_workband_update(struct fyai_ctx *ctx,
				   struct fytim_workband *band,
				   const char *title, const char *command,
				   const char *body, size_t len,
				   const char *first_margin);
void fyai_ui_workband_destroy(struct fytim_workband *band);
void fyai_ui_tool_begin(struct fyai_ctx *ctx, const char *title);
void fyai_ui_shell_begin(struct fyai_ctx *ctx, const char *title,
			 const char *command);
void fyai_ui_tool_update(struct fyai_ctx *ctx, const char *body, size_t len);
void fyai_ui_tool_end(struct fyai_ctx *ctx, bool ok, const char *cause);
void fyai_ui_pane_begin(struct fyai_ctx *ctx);
void fyai_ui_pane_end(struct fyai_ctx *ctx, const char *title, bool error,
		      bool show_output);
void fyai_ui_diag_drain(struct fyai_ctx *ctx, const char *title);

/*
 * A surface: a grid of cells in the band, for a program on a pseudo-terminal.
 * The host publishes what its terminal view holds and the library draws it, so
 * several programs can be watched at once above the prompt.
 */
struct fytim_surface *fyai_ui_surface_open(struct fyai_ctx *ctx, int rows,
					   int cols);
void fyai_ui_surface_close(struct fyai_ctx *ctx, struct fytim_surface *sf);
int fyai_ui_surface_resize(struct fytim_surface *sf, int rows, int cols);
int fyai_ui_surface_granted_rows(const struct fytim_surface *sf);
/* The columns the grid was given: the width less the margin. */
int fyai_ui_surface_granted_cols(const struct fytim_surface *sf);
/* Chrome at the left of every row of @sf. */
int fyai_ui_surface_set_margin(struct fytim_surface *sf, const char *text);
int fyai_ui_surface_set_title(struct fytim_surface *sf, const char *top,
			      const char *bottom);
/* Copy what changed in @view onto @sf. Returns 1 when it published. */
int fyai_ui_surface_publish(struct fytim_surface *sf,
			    struct fyai_terminal_view *view);
/* Ask for a frame: the content of a surface changed. */
void fyai_ui_wake(struct fyai_ctx *ctx);

/* The terminal geometry the display last sampled, in cells. */
int fyai_ui_size(struct fyai_ctx *ctx, int *cols, int *rows);
/* The state a surface's title row shows. */
enum fyai_ui_mark {
	FYAI_UI_MARK_RUNNING,	/* the program is still there */
	FYAI_UI_MARK_OK,
	FYAI_UI_MARK_FAILED
};

/* Set the title row of @sf: a rendered tool head with its state mark. */
int fyai_ui_surface_set_head(struct fyai_ctx *ctx, struct fytim_surface *sf,
			     const char *title, const char *cause,
			     enum fyai_ui_mark mark);

/* Keep the last screen: it goes into the transcript and @sf is retired. */
void fyai_ui_surface_commit(struct fyai_ctx *ctx, struct fytim_surface *sf);
/* Give the keys to @sf; @cb receives the bytes a terminal would send. */
int fyai_ui_surface_keys(struct fyai_ctx *ctx, struct fytim_surface *sf,
			 bool take, fyai_ui_keys_fn cb, void *user);

#endif
