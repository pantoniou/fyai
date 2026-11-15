/* SPDX-License-Identifier: MIT */
#ifndef FYAI_PANEL_H
#define FYAI_PANEL_H

#include <stdbool.h>

struct fyai_ctx;
struct fyai_panel;

enum fyai_panel_type {
	FYAI_PANEL_DIFF,
	FYAI_PANEL_MARKDOWN,
};

struct fyai_panel *fyai_panel_create(struct fyai_ctx *ctx,
				    enum fyai_panel_type type);
void fyai_panel_destroy(struct fyai_panel *panel);
/* The panel copies @source and renders at the width of its assigned region. */
int fyai_panel_update(struct fyai_panel *panel, const char *source,
		      int cols, int rows);
/* Positive @delta moves toward the end; the position stops at either end. */
void fyai_panel_scroll(struct fyai_panel *panel, int delta, bool page);
/* Borrowed rendered text, valid until the panel is updated or destroyed. */
const char *fyai_panel_text(const struct fyai_panel *panel);
void fyai_panel_extent(const struct fyai_panel *panel, int *total, int *top,
		       int *rows);

#endif
