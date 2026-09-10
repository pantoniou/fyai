/* SPDX-License-Identifier: MIT */
#ifndef FYAI_SINK_H
#define FYAI_SINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "fyai_flow.h"

struct fyai_ctx;
struct fyai_cfg;
struct fyai_sink;
struct fyai_sink_band;
struct fytim_surface;

/* Start at the final rows that fit in the pane. */
#define FYAI_SINK_OFFSET_TAIL ((size_t)-1)

/* Where the aside of a page stands. Auto follows the granted geometry. */
enum fyai_sink_split {
	FYAI_SINK_SPLIT_NONE,
	FYAI_SINK_SPLIT_RIGHT,
	FYAI_SINK_SPLIT_BOTTOM,
	FYAI_SINK_SPLIT_AUTO,
};

/* A page and the aside beside it. Each block is rendered at its pane width. */
struct fyai_sink_page {
	const char *markdown;
	const char *diagram;		/* Mermaid source under the page. */
	const char *diagram_selection;	/* Element path selected in the diagram. */
	int diagram_row;		/* First row of the drawing to present. */
	size_t offset;			/* Page scroll, in rendered rows. */
	const char *aside;
	size_t aside_offset;
	bool aside_rendered;		/* final bytes; do not render again */
	enum fyai_sink_split split;
	int extent;			/* Aside share of the grid, percent. */
	int aside_cols;			/* Fixed columns for a right split. */
};

/* Render a scrollable Markdown page into a UI-owned surface. */
int fyai_sink_page(struct fyai_sink *s, struct fytim_surface *surface,
		   const char *markdown, size_t offset);

/* Where a move goes on a rendered diagram. */
enum fyai_diagram_move {
	FYAI_DIAGRAM_UP,
	FYAI_DIAGRAM_DOWN,
	FYAI_DIAGRAM_LEFT,
	FYAI_DIAGRAM_RIGHT,
};

/* Render Mermaid at the granted width, with @selection drawn as the selected
 * element, or NULL for none. Release the result with fymm_free(). */
char *fyai_sink_diagram_render(const struct fyai_cfg *cfg, const char *source,
			       const char *selection, int cols);

/* The rows a render at @cols would occupy, under the same fit policy the
 * render uses. With @legendp, also states whether the render moved labels
 * into a legend, which says the width stopped carrying them. Returns 0 and
 * stores the rows, or -1. */
int fyai_sink_diagram_measure(const struct fyai_cfg *cfg, const char *source,
			      int cols, int *rowsp, bool *legendp);

/* Where the element at @path landed in a render at @cols. Returns 0 and
 * stores its first row and its height, or -1 when the render holds no such
 * element. */
int fyai_sink_diagram_locate(const struct fyai_cfg *cfg, const char *source,
			     const char *path, int cols, int *rowp, int *heightp);

/* The element a move from @from arrives at, decided by where the renderer
 * drew the diagram at @cols. Returns the path, which the caller frees, or
 * NULL when the move leaves the drawing. */
char *fyai_sink_diagram_navigate(const struct fyai_cfg *cfg, const char *source,
				 const char *from, enum fyai_diagram_move dir,
				 int cols);
int fyai_sink_diagram_page(struct fyai_sink *s, struct fytim_surface *surface,
			   const char *header, const char *source);
int fyai_sink_page_split(struct fyai_sink *s, struct fytim_surface *surface,
			 const struct fyai_sink_page *page);
/* Split the grid between the page and its aside. Returns the aside extent, in
 * columns or rows, and zero when the grid cannot carry one. */
int fyai_sink_page_extent(const struct fyai_sink_page *page, int rows, int cols,
			  enum fyai_sink_split *split);
/* The rows a Markdown source occupies when it is rendered at @cols. */
int fyai_sink_markdown_measure(struct fyai_sink *s, const char *md, int cols);

/* The grid the page itself is rendered in, once the aside is taken out. */
int fyai_sink_page_cols(const struct fyai_sink_page *page, int rows, int cols);
int fyai_sink_page_rows(const struct fyai_sink_page *page, int rows, int cols);

/* The sink owns presentation. Producers provide Markdown source or final bytes. */

/* A content role; the backend selects its destination and presentation. */
enum fyai_sink_stream {
	FYAI_SINK_TRANSCRIPT,
	FYAI_SINK_NOTICE,
	FYAI_SINK_STATUS,
	FYAI_SINK_DIAG,
	FYAI_SINK_MACHINE,
};

/* A transcript document role; fyai_output.c owns its durable record. */
enum fyai_sink_doc_kind {
	FYAI_SINK_DOC_SYSTEM,
	FYAI_SINK_DOC_USER,
	FYAI_SINK_DOC_ASSISTANT,
};

struct fyai_sink_doc {
	enum fyai_sink_doc_kind kind;
};

/* NULL operations discard that presentation. Paused documents retain source. */
struct fyai_sink_ops {
	const char *name;
	int (*doc_begin)(struct fyai_sink *s, const struct fyai_sink_doc *doc);
	int (*doc_append)(struct fyai_sink *s, const char *text, size_t len);
	int (*doc_end)(struct fyai_sink *s, bool aborted);
	/* Drop the open document without presenting anything further. */
	void (*doc_discard)(struct fyai_sink *s);
	/* Rerender the live region at the current width. */
	void (*doc_reflow)(struct fyai_sink *s);
	int (*doc_pause)(struct fyai_sink *s);
	int (*doc_resume)(struct fyai_sink *s);
	/* True while the open document repaints in place. */
	bool (*doc_is_live)(const struct fyai_sink *s);
	/* Work-band operations are NULL when the backend cannot repaint. */
	bool (*bands_available)(const struct fyai_sink *s);
	struct fyai_sink_band *(*band_open)(struct fyai_sink *s, bool shared,
					    const char *title,
					    const char *command);
	void (*band_paint)(struct fyai_sink_band *b, const char *title,
			   const char *command, const char *body, size_t len,
			   const char *margin);
	void (*band_close)(struct fyai_sink *s, bool ok, const char *cause);
	void (*band_commit)(struct fyai_sink_band *b);
	void (*band_destroy)(struct fyai_sink_band *b);
	struct fyai_sink_band *(*band_shared)(struct fyai_sink *s);
	/* Render @md as Markdown on @stream. */
	int (*markdown)(struct fyai_sink *s, enum fyai_sink_stream stream,
			const char *md);
	/* Present bytes already rendered for the destination. */
	int (*write)(struct fyai_sink *s, enum fyai_sink_stream stream,
		     const char *buf, size_t len);
	void (*flush)(struct fyai_sink *s);
	void (*destroy)(struct fyai_sink *s);
};

struct fyai_sink {
	const struct fyai_sink_ops *ops;
	struct fyai_ctx *ctx;
	void *state;
	struct fyai_flow flow;		/* separation state for this medium */
};

/* The separation manager for this medium. */
struct fyai_flow *fyai_sink_flow(struct fyai_sink *s);

/* Present the separation before @unit and record it. Call it before @unit. */
int fyai_sink_unit(struct fyai_sink *s, enum fyai_sink_stream stream,
		   enum fyai_flow_unit unit);

/* Create the configured sink. A discard-only backend is still valid. */
struct fyai_sink *fyai_sink_create(struct fyai_ctx *ctx);
/*
 * A sink that keeps what it was asked to present instead of drawing it. It is
 * the substrate a document backend is written against, and it lets a test read
 * back exactly what a run would have shown.
 */
struct fyai_sink *fyai_sink_create_capture(struct fyai_ctx *ctx);

/*
 * A terminal backend that renders into @out instead of the terminal. It
 * presents every document at cfg->render_width and carries no status or
 * diagnostic stream. The caller owns @out.
 */
struct fyai_sink *fyai_sink_create_render(struct fyai_ctx *ctx, FILE *out);
/* The captured text, NUL terminated. @lenp may be NULL. */
const char *fyai_sink_captured(const struct fyai_sink *s, size_t *lenp);
void fyai_sink_capture_reset(struct fyai_sink *s);
void fyai_sink_destroy(struct fyai_sink *s);

int fyai_sink_doc_begin(struct fyai_sink *s, enum fyai_sink_doc_kind kind);
int fyai_sink_doc_append(struct fyai_sink *s, const char *text, size_t len);
int fyai_sink_doc_end(struct fyai_sink *s, bool aborted);
void fyai_sink_doc_discard(struct fyai_sink *s);
/* Rerender the open live document at the current width. */
void fyai_sink_reflow(struct fyai_sink *s);
int fyai_sink_doc_pause(struct fyai_sink *s);
int fyai_sink_doc_resume(struct fyai_sink *s);
bool fyai_sink_doc_is_live(const struct fyai_sink *s);

/* Test if the sink can present a repainting work band. */
bool fyai_sink_bands_available(const struct fyai_sink *s);
/* Open a sink-owned shared band or a caller-owned independent band. */
struct fyai_sink_band *fyai_sink_band_open(struct fyai_sink *s, bool shared,
					   const char *title,
					   const char *command);
/* Repaint a band. A NULL title retains its current title. */
void fyai_sink_band_paint(struct fyai_sink_band *b, const char *title,
			  const char *command, const char *body, size_t len,
			  const char *margin);
/* Commit the shared band to the transcript with a success indicator. */
void fyai_sink_band_close(struct fyai_sink *s, bool ok, const char *cause);
/* Commit and retire an independently owned band. */
void fyai_sink_band_commit(struct fyai_sink_band *b);
void fyai_sink_band_destroy(struct fyai_sink_band *b);
/* Return the granted tile width, or zero for a full-width band. */
int fyai_sink_band_cols(const struct fyai_sink_band *b);
/* The shared band, or NULL when none is open. */
struct fyai_sink_band *fyai_sink_band_shared(struct fyai_sink *s);

int fyai_sink_markdown(struct fyai_sink *s, enum fyai_sink_stream stream,
		       const char *md);
int fyai_sink_write(struct fyai_sink *s, enum fyai_sink_stream stream,
		    const char *buf, size_t len);
/*
 * Formatted plain text on @stream. The text is written as it stands, not
 * rendered: a status line carries no Markdown, and a value inside it - a model
 * name, a path, a branch - must not be reinterpreted as markup.
 */
int fyai_sink_printf(struct fyai_sink *s, enum fyai_sink_stream stream,
		     const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));
void fyai_sink_flush(struct fyai_sink *s);

/* Present command results as output and reports as commentary. */
int fyai_result(struct fyai_ctx *ctx, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
int fyai_result_md(struct fyai_ctx *ctx, const char *md);
int fyai_report(struct fyai_ctx *ctx, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

#endif
