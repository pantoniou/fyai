/* SPDX-License-Identifier: MIT */
#ifndef FYAI_UITRANSCRIPT_H
#define FYAI_UITRANSCRIPT_H

#include <stdbool.h>
#include <stddef.h>

#include <libfyaml.h>
#include <libfyaml/libfyaml-generic.h>

struct fyai_ctx;

/*
 * The live document of the transcript, in the transcript store of the UI
 * model (src/fyai_uimodel.h). The output document gives it at its input:
 *
 *   live: {tag, source, length, fragments, recorded} | null
 *   last: {tag, state}
 *
 * @source is the append-only text of the model that holds the Markdown source
 * of the document, of @length bytes. @fragments are the fragments of the
 * document. @recorded holds the [start, end) ranges that another path, such as
 * the band of a tool call, presented already. @last is the document that
 * ended last and how: finalized, aborted or discarded. The stored exchanges
 * are the arena's: a renderer reads them from the head of the branch.
 */

void fyai_uitranscript_begin(struct fyai_ctx *ctx, const char *tag);
/* @present is false for source that another path presented already. */
void fyai_uitranscript_append(struct fyai_ctx *ctx, const char *text,
			      size_t len, bool present);
void fyai_uitranscript_fragment(struct fyai_ctx *ctx, fy_generic fragment);
void fyai_uitranscript_end(struct fyai_ctx *ctx, const char *state);
void fyai_uitranscript_release(struct fyai_ctx *ctx);

#endif
