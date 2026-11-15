/*
 * fyai_editor.h - the external editor child
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_EDITOR_H
#define FYAI_EDITOR_H

#include <stdbool.h>

struct fyai_ctx;

struct fyai_editor_request;
typedef void (*fyai_editor_complete_fn)(
		struct fyai_editor_request *request, void *userdata);

/*
 * External editor child operation. Submission never pumps; completion is
 * latched before @complete runs, cancellation is idempotent, and collection
 * never blocks.
 */
struct fyai_editor_request *
fyai_editor_submit(struct fyai_ctx *ctx, const char *path, bool readonly,
		   fyai_editor_complete_fn complete, void *userdata);
/* Whether an editor runs in a tile of the work pane, not on the terminal:
 * display/editor is not terminal and an interactive UI runs. A caller that
 * gets true does not suspend the UI. */
bool fyai_editor_in_pane(struct fyai_ctx *ctx);
void fyai_editor_cancel(struct fyai_editor_request *request);
bool fyai_editor_done(const struct fyai_editor_request *request);
int fyai_editor_collect(const struct fyai_editor_request *request);
void fyai_editor_destroy(struct fyai_editor_request *request);

int fyai_spawn_editor_readonly(struct fyai_ctx *ctx, const char *path);

#endif
