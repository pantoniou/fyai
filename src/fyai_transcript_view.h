/* SPDX-License-Identifier: MIT */
/*
 * fyai_transcript_view.h - the transcript of a fullscreen page.
 *
 * A fullscreen page has no scrollback of the terminal: the transcript is a
 * view the program draws into the "transcript" region of its page. The view
 * holds rendered rows - the stored conversation, rendered through the one
 * presentation path at the width of the region one exchange at a time, and
 * the rows of the turn in flight - and the window of them that the region
 * shows.
 */
#ifndef FYAI_TRANSCRIPT_VIEW_H
#define FYAI_TRANSCRIPT_VIEW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libfyaml.h>

struct fyai_ctx;
struct fyai_transcript_view;

struct fyai_transcript_view *fyai_transcript_view_create(void);
void fyai_transcript_view_destroy(struct fyai_transcript_view *v);

/*
 * Replace the stored rows with the rendered @text of @len bytes, one row for
 * each line, as one exchange. The live rows are left as they are. Returns 0,
 * or -1 when memory runs out.
 */
int fyai_transcript_view_set_stored(struct fyai_transcript_view *v,
				    const char *text, size_t len);

/*
 * Render exchange @index at @width columns: an allocated *@textp of *@lenp
 * bytes, one row for each line. Returns 0, or -1.
 */
typedef int (*fyai_transcript_view_render_fn)(void *user, size_t index,
					      int width, char **textp,
					      size_t *lenp);

/* The rows exchange @index takes at @width columns, without rendering it. */
typedef size_t (*fyai_transcript_view_measure_fn)(void *user, size_t index,
						  int width);

/*
 * Make the stored rows the exchanges @keys at @width columns, for a region of
 * @height rows. An exchange whose key and width did not change keeps its rows
 * or its measure. Any other is measured with @measure, and only the exchanges
 * the region shows are rendered with @render; without @measure every exchange
 * is rendered. A view scrolled back keeps the row at its top: the exchange
 * that holds it and the row in that exchange. Returns 0, or -1 when an
 * exchange does not render, which leaves the view as it was.
 */
int fyai_transcript_view_update(struct fyai_transcript_view *v, int width,
				int height, const uintptr_t *keys,
				size_t count,
				fyai_transcript_view_render_fn render,
				fyai_transcript_view_measure_fn measure,
				void *user);

/* Whether a region of @height rows shows an exchange that is only measured. */
bool fyai_transcript_view_needs_render(const struct fyai_transcript_view *v,
				       int height);

/*
 * Append the rendered @text of @len bytes to the live rows. A row that the
 * last append did not end with a newline continues. Returns 0, or -1.
 */
int fyai_transcript_view_append_live(struct fyai_transcript_view *v,
				     const char *text, size_t len);

/* Drop the live rows, as when the turn they belong to is stored. */
void fyai_transcript_view_clear_live(struct fyai_transcript_view *v);

/* Replace the rendered rows of the in-flight tail. The view owns a copy. */
int fyai_transcript_view_set_tail(struct fyai_transcript_view *v,
				  const char *text, size_t len);

/* The rows the view holds: stored, committed live rows, then the tail. */
size_t fyai_transcript_view_rows(const struct fyai_transcript_view *v);

/*
 * Scroll the view by @delta rows for a region of @height rows: back through
 * the transcript when positive, toward its end when negative. The view stops
 * at either end. A view at the end follows the rows that arrive; a view
 * scrolled back keeps its top row.
 */
void fyai_transcript_view_scroll(struct fyai_transcript_view *v, int delta,
				 int height);

/* Whether the view is at the end of the transcript. */
bool fyai_transcript_view_at_end(const struct fyai_transcript_view *v);

/*
 * The rows a region of @height rows shows, from its top, and their count in
 * *@countp. The rows are borrowed until the view changes.
 */
const char *const *fyai_transcript_view_window(struct fyai_transcript_view *v,
					       int height, int *countp);

/*
 * The text of a selection over the rows a region of @height rows shows, from
 * the cell @row0, @col0 to the cell @row1, @col1 in reading order, as the
 * rows are read: without their styles and links, and without the blanks that
 * end a row. Returns an allocated string, or NULL when memory runs out.
 */
char *fyai_transcript_view_copy(struct fyai_transcript_view *v, int height,
				int row0, int col0, int row1, int col1);

/*
 * Bring the stored rows up to the conversation of @ctx at @width columns, for
 * a region of @height rows: an exchange that was stored since, or that
 * changed, is measured, and the exchanges the region shows are rendered; the
 * others keep their rows. The live rows go when the head of the conversation
 * changed. Returns 0, or -1 with the cause reported.
 */
int fyai_transcript_view_refresh(struct fyai_ctx *ctx,
				 struct fyai_transcript_view *v, int width,
				 int height);

#endif
