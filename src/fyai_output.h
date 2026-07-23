/* SPDX-License-Identifier: MIT */
#ifndef FYAI_OUTPUT_H
#define FYAI_OUTPUT_H

#include <stdbool.h>
#include <stddef.h>

#include "fyai.h"

enum fyai_output_tag {
	FYAI_OUTPUT_SYSTEM,
	FYAI_OUTPUT_USER,
	FYAI_OUTPUT_ASSISTANT,
};

int fyai_output_begin(struct fyai_ctx *ctx, enum fyai_output_tag tag);
int fyai_output_append(struct fyai_ctx *ctx, const char *text, size_t len);
int fyai_output_append_string(struct fyai_ctx *ctx, const char *text);
int fyai_output_printf(struct fyai_ctx *ctx, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
int fyai_output_reasoning_append(struct fyai_ctx *ctx, const char *text);
int fyai_output_reasoning_finish(struct fyai_ctx *ctx);
const char *fyai_output_markdown(const struct fyai_ctx *ctx, size_t *len);
bool fyai_output_renders_live(const struct fyai_ctx *ctx);
int fyai_output_checkpoint(struct fyai_ctx *ctx);
int fyai_output_resume(struct fyai_ctx *ctx);
int fyai_output_add_fragment(struct fyai_ctx *ctx, const char *kind,
			     size_t start, size_t end, const char *lang);
fy_generic fyai_output_finalize(struct fyai_ctx *ctx, fy_generic turn,
				bool aborted);
fy_generic fyai_output_record(struct fyai_ctx *ctx, fy_generic turn,
			      enum fyai_output_tag tag, const char *markdown);
void fyai_output_abort(struct fyai_ctx *ctx);
void fyai_output_cleanup(struct fyai_ctx *ctx);

const char *fyai_output_tag_name(enum fyai_output_tag tag);

#endif
