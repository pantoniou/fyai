/* SPDX-License-Identifier: MIT */
#ifndef FYAI_PATCH_H
#define FYAI_PATCH_H

struct fyai_ctx;

char *fyai_apply_patch_text_ctx(struct fyai_ctx *ctx, const char *patch);
char *fyai_apply_patch_text(const char *patch);
/*
 * Convert an envelope to a unified diff for display. Call this before patch
 * application. Return NULL for a unified diff or a conversion error.
 */
char *fyai_patch_to_unified(const char *patch);
char *fyai_patch_to_unified_ctx(struct fyai_ctx *ctx, const char *patch);

#endif
