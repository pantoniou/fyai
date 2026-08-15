/* SPDX-License-Identifier: MIT */
#ifndef FYAI_SINK_H
#define FYAI_SINK_H

#include <stdbool.h>
#include <stddef.h>

struct fyai_ctx;
struct fyai_sink;

/* The sink owns presentation. Producers provide Markdown source or final bytes. */

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

#endif
