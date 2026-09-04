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
/* A configured colour as 0xRRGGBB, with or without a leading hash. */
bool fyai_ui_color_parse(const char *text, uint32_t *out);
/* That, or `reverse` for the ground the terminal draws text in. */
bool fyai_ui_ground_parse(const char *text, uint32_t *out);
/* Re-read the display configuration a live session holds. */
void fyai_ui_config_changed(struct fyai_ctx *ctx);
bool fyai_ui_active(const struct fyai_ctx *ctx);
char *fyai_ui_readline(struct fyai_ctx *ctx);
char *fyai_ui_take_line(struct fyai_ctx *ctx);
bool fyai_ui_quit_requested(const struct fyai_ctx *ctx);
void fyai_ui_drain_output(struct fyai_ctx *ctx);
void fyai_ui_history_load(struct fyai_ctx *ctx, const char *path);
void fyai_ui_history_save(struct fyai_ctx *ctx, const char *path,
			  const char *line);
/* Return the display terminal descriptor, or -1. */
int fyai_ui_tty_fd(const struct fyai_ctx *ctx);
/* Schedule a frame after a terminal resize. */
void fyai_ui_resized(struct fyai_ctx *ctx);
/* Clear the display screen without clearing terminal scrollback. */
void fyai_ui_clear_screen(struct fyai_ctx *ctx);
int fyai_ui_commit(struct fyai_ctx *ctx, const char *buf, size_t len);
int fyai_ui_tail_apply(struct fyai_ctx *ctx, const struct markdown_update *upd);
void fyai_ui_tail_finish(struct fyai_ctx *ctx, const char *buf, size_t len);
void fyai_ui_set_busy(struct fyai_ctx *ctx, bool busy);
/* An interrupt reached the session (Escape, or SIGINT from ^C). Discards a
 * half-typed line on an idle prompt, ends the session when there is nothing to
 * discard, and cancels the turn while busy. */
/*
 * Act on an interrupt. Returns true when it was given to the program that
 * holds the keys, which is what a ^C typed into a tile is: the turn of this
 * process is not what the user was interrupting.
 */
bool fyai_ui_interrupt(struct fyai_ctx *ctx);

void fyai_ui_signal(struct fyai_ctx *ctx, int signo);
void fyai_ui_update_banner(struct fyai_ctx *ctx, const char *top,
			   const char *bottom);
int fyai_ui_update_prompt_style(struct fyai_ctx *ctx);
/*
 * Shape the input pane for a pending model question: a distinct prompt
 * marker, @question pinned in the header row above the prompt in place of
 * the model/provider banner, and @options (one numbered choice per line, or
 * NULL) as a live band under it. Escape declines the question alone -
 * neither the turn nor the session ends.
 */
void fyai_ui_ask_begin(struct fyai_ctx *ctx, const char *question,
		       const char *options);
/* Restore the marker and banner an ask_user question replaced. Returns
 * true when the question was declined with Escape rather than answered. */
bool fyai_ui_ask_end(struct fyai_ctx *ctx);
int fyai_ui_external_begin(struct fyai_ctx *ctx);
int fyai_ui_external_end(struct fyai_ctx *ctx);
/* Create an independent text tile in the work pane. */
struct fytim_workband *fyai_ui_work_tile_create(struct fyai_ctx *ctx);
void fyai_ui_work_tile_destroy(struct fyai_ctx *ctx,
			       struct fytim_workband *band, bool commit);
/* Return the granted tile width, or zero before layout. */
int fyai_ui_work_tile_cols(const struct fytim_workband *band);
void fyai_ui_workband_update(struct fyai_ctx *ctx,
			     struct fytim_workband *band,
			     const char *title, const char *body, size_t len,
			     const char *first_margin);
void fyai_ui_shell_workband_update(struct fyai_ctx *ctx,
				   struct fytim_workband *band,
				   const char *title, const char *command,
				   const char *body, size_t len,
				   const char *first_margin);
void fyai_ui_tool_begin(struct fyai_ctx *ctx, const char *title);
void fyai_ui_shell_begin(struct fyai_ctx *ctx, const char *title,
			 const char *command);
void fyai_ui_tool_update(struct fyai_ctx *ctx, const char *body, size_t len);
void fyai_ui_tool_end(struct fyai_ctx *ctx, bool ok, const char *cause);
void fyai_ui_pane_begin(struct fyai_ctx *ctx);
void fyai_ui_pane_end(struct fyai_ctx *ctx, const char *title, bool error,
		      bool show_output);
void fyai_ui_diag_drain(struct fyai_ctx *ctx, const char *title);

/* A grid of terminal cells displayed in a work band. */
struct fytim_surface *fyai_ui_surface_open(struct fyai_ctx *ctx, int rows,
					   int cols);
void fyai_ui_surface_close(struct fyai_ctx *ctx, struct fytim_surface *sf);
int fyai_ui_surface_resize(struct fytim_surface *sf, int rows, int cols);
int fyai_ui_surface_request_rows(struct fytim_surface *sf, int rows);
int fyai_ui_surface_granted_rows(const struct fytim_surface *sf);
/* The columns the grid was given: the width less the margin. */
int fyai_ui_surface_granted_cols(const struct fytim_surface *sf);
/* Chrome at the left of every row of @sf. */
int fyai_ui_surface_set_margin(struct fytim_surface *sf, const char *text);
/* Blank the grid: the tile is no longer drawing its program. */
int fyai_ui_surface_clear(struct fytim_surface *sf);
/* Limit grid height; zero accepts all granted rows. */
int fyai_ui_surface_set_max_rows(struct fytim_surface *sf, int rows);
/* Update keyboard-focus chrome. */
void fyai_ui_surface_focus(struct fyai_ctx *ctx, struct fytim_surface *sf,
			   bool focused);
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

/* Set the marked title and optional command chrome for @sf. */
int fyai_ui_surface_set_head(struct fyai_ctx *ctx, struct fytim_surface *sf,
			     const char *title, const char *command,
			     const char *cause, enum fyai_ui_mark mark);
/* Set one animation frame and return its interval through @interval_msp. */
int fyai_ui_surface_set_head_frame(struct fyai_ctx *ctx,
				   struct fytim_surface *sf,
				   const char *title, const char *command,
				   const char *cause,
				   enum fyai_ui_mark mark, size_t frame,
				   unsigned int *interval_msp);

/* Keep the last screen: it goes into the transcript and @sf is retired. */
void fyai_ui_surface_commit(struct fyai_ctx *ctx, struct fytim_surface *sf);
/* Zoom one tile to the pane; NULL restores the grid. */
int fyai_ui_surface_zoom(struct fyai_ctx *ctx, struct fytim_surface *sf);
struct fytim_surface *fyai_ui_surface_zoomed(const struct fyai_ctx *ctx);
/* Publish emulator scroll extent to the surface. */
int fyai_ui_surface_scroll_extent(struct fytim_surface *sf, int total_rows,
				  int top_row);

/* Give the keys to @sf; @cb receives the bytes a terminal would send. */
int fyai_ui_surface_keys(struct fyai_ctx *ctx, struct fytim_surface *sf,
			 bool take, fyai_ui_keys_fn cb, void *user);

/* Return an unconsumed input-frame suffix to the current input owner. */
int fyai_ui_keys_return(struct fyai_ctx *ctx, const char *data, size_t len);

#endif
