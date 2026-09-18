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
/* Reassert runtime values owned by an active terminal UI. */
void fyai_ui_config_reassert(struct fyai_ctx *ctx);
/* Re-read the display configuration a live session holds. */
void fyai_ui_config_changed(struct fyai_ctx *ctx);
bool fyai_ui_active(const struct fyai_ctx *ctx);
/* Whether the UI stands on the alternate screen with a fullscreen page. */
bool fyai_ui_fullscreen(const struct fyai_ctx *ctx);
char *fyai_ui_readline(struct fyai_ctx *ctx);
char *fyai_ui_take_line(struct fyai_ctx *ctx);
/* The head of the input queue. The line stays queued. */
const char *fyai_ui_peek_line(struct fyai_ctx *ctx);
/* Whether the input queue holds an unprocessed line. */
bool fyai_ui_has_line(struct fyai_ctx *ctx);
/* End the session at the next pass, as an end of input does. */
void fyai_ui_quit_request(struct fyai_ctx *ctx);
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
bool fyai_ui_busy(const struct fyai_ctx *ctx);
void fyai_ui_repaint(struct fyai_ctx *ctx);
/* The caller owns the draft copy. Editing remains in the terminal library. */
char *fyai_ui_input_copy(struct fyai_ctx *ctx);
void fyai_ui_input_set(struct fyai_ctx *ctx, const char *text);

/* Called once with the answer to a question, or with NULL when the user gave
 * none. The answer is valid for the call. */
typedef void (*fyai_ui_ask_fn)(void *user, const char *answer);
/* Whether the input area can put a question to the user: the page renderer
 * draws it. */
bool fyai_ui_ask_available(struct fyai_ctx *ctx);

/*
 * Report the page of the live screen as a notice: its document, and the
 * state, source and regions of the last frame.
 */
int fyai_ui_page_report(struct fyai_ctx *ctx);
/*
 * Put @question to the user in the input area, after the questions before it.
 * @from names the sub-agent that asks, or is NULL, and the @n @options are
 * offered. @done is called once, from the event loop, with @user. Returns 0,
 * or -1 after it reported why the question cannot be put.
 */
int fyai_ui_ask(struct fyai_ctx *ctx, const char *question, const char *from,
		const char *const *options, size_t n, fyai_ui_ask_fn done,
		void *user);
/* Take back the questions of @user without an answer: nobody waits for them. */
void fyai_ui_ask_withdraw(struct fyai_ctx *ctx, void *user);
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
/*
 * Set the header and the status of the input area. @top is the rendered
 * header that the band stack draws. @top_source is its Markdown, with the
 * values escaped and in the colours of the palette, which the page draws as
 * UI Markdown.
 */
void fyai_ui_update_banner(struct fyai_ctx *ctx, const char *top,
			   const char *top_source,
			   const char *bottom);
int fyai_ui_update_prompt_style(struct fyai_ctx *ctx);
int fyai_ui_external_begin(struct fyai_ctx *ctx);
int fyai_ui_external_end(struct fyai_ctx *ctx);
/* Create an independent text tile in the work pane. */
struct fytim_workband *fyai_ui_work_tile_create(struct fyai_ctx *ctx);
void fyai_ui_work_tile_destroy(struct fyai_ctx *ctx,
			       struct fytim_workband *band, bool commit);
/* Return the granted tile width, or zero before layout. */
int fyai_ui_work_tile_cols(struct fyai_ctx *ctx,
			   const struct fytim_workband *band);
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
/*
 * Rebuild the right-side input-header panel. It toggles the work pane and
 * reports active user shells, model shells, and sub-agents. Omit it when the
 * pane is visible and no work is active.
 */
void fyai_ui_panel_update(struct fyai_ctx *ctx);
/* Collapse @sf to its header, with no terminal rows. */
int fyai_ui_surface_set_collapsed(struct fytim_surface *sf, bool collapsed);
/* The rows the grid of @sf was given: by the page when the page draws it. */
int fyai_ui_surface_granted_rows(struct fyai_ctx *ctx,
				 const struct fytim_surface *sf);
/* The columns the grid was given: the width less the margin. */
int fyai_ui_surface_granted_cols(struct fyai_ctx *ctx,
				 const struct fytim_surface *sf);
/* The margin, its columns, and the ground and its mix, that @sf stands in. */
void fyai_ui_surface_chrome(const struct fytim_surface *sf,
			    const char **marginp, int *colsp, uint32_t *bgp,
			    int *mixp);
/* Chrome at the left of every row of @sf. */
int fyai_ui_surface_set_margin(struct fytim_surface *sf, const char *text);
/* Blank the grid: the tile is no longer drawing its program. */
int fyai_ui_surface_clear(struct fytim_surface *sf);
/* Limit grid height; zero accepts all granted rows. */
/* Bind the tile of @sf or @band to its page slot "tile:@slot". */
void fyai_ui_tile_bind(struct fytim_surface *sf, struct fytim_workband *band,
		       unsigned int slot);
/* Rows the tile of @sf or @band asks for, head and foot included. */
int fyai_ui_tile_rows(const struct fytim_surface *sf,
		      const struct fytim_workband *band);
/* Draw the part of the tile page of @sf that @present asks for. */
void fyai_ui_surface_set_view(struct fytim_surface *sf, int present);
int fyai_ui_surface_set_max_rows(struct fytim_surface *sf, int rows);
/* Show or hide the surface's emulated program cursor. */
int fyai_ui_surface_cursor_visible(struct fytim_surface *sf, bool visible);
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
/*
 * fyai_ui_surface_set_head_frame() with @right, fyai chrome such as an
 * elapsed time, at the right edge of the title row when the build renders UI
 * Markdown, and after the title otherwise.
 */
int fyai_ui_surface_set_head_right(struct fyai_ctx *ctx,
				   struct fytim_surface *sf,
				   const char *title, const char *right,
				   const char *command, const char *cause,
				   enum fyai_ui_mark mark, size_t frame,
				   unsigned int *interval_msp);
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

/* Give the keys to @sf; @cb receives the bytes a terminal would send. */
int fyai_ui_surface_keys(struct fyai_ctx *ctx, struct fytim_surface *sf,
			 bool take, fyai_ui_keys_fn cb, void *user);

/* Return an unconsumed input-frame suffix to the current input owner. */
int fyai_ui_keys_return(struct fyai_ctx *ctx, const char *data, size_t len);

#endif
