/* SPDX-License-Identifier: MIT */
#ifndef FYAI_SINK_H
#define FYAI_SINK_H

#include <stdbool.h>
#include <stddef.h>

struct fyai_ctx;
struct fyai_sink;
struct fyai_sink_band;

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
};

/* Create the configured sink. A discard-only backend is still valid. */
struct fyai_sink *fyai_sink_create(struct fyai_ctx *ctx);
void fyai_sink_destroy(struct fyai_sink *s);

int fyai_sink_doc_begin(struct fyai_sink *s, enum fyai_sink_doc_kind kind);
int fyai_sink_doc_append(struct fyai_sink *s, const char *text, size_t len);
int fyai_sink_doc_end(struct fyai_sink *s, bool aborted);
void fyai_sink_doc_discard(struct fyai_sink *s);
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
void fyai_sink_band_destroy(struct fyai_sink_band *b);
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
