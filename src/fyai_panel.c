/* SPDX-License-Identifier: MIT */
#define FYAI_MODULE FYAIEM_DISPLAY

#include <stdlib.h>
#include <string.h>

#include "fyai.h"
#include "fyai_markdown.h"
#include "fyai_panel.h"
#include "fyai_terminal.h"

struct fyai_panel {
	struct fyai_ctx *ctx;
	enum fyai_panel_type type;
	char *source, *rendered;
	size_t *offsets;
	int cols, rows, total, top;
};

static void panel_render_free(struct fyai_panel *p, char *text)
{
	if (p->type == FYAI_PANEL_DIFF)
		fymd_free(text);
	else
		free(text);
}

struct fyai_panel *fyai_panel_create(struct fyai_ctx *ctx,
				    enum fyai_panel_type type)
{
	struct fyai_panel *p = calloc(1, sizeof(*p));

	if (p) {
		p->ctx = ctx;
		p->type = type;
	}
	return p;
}

void fyai_panel_destroy(struct fyai_panel *p)
{
	if (!p)
		return;
	free(p->source);
	panel_render_free(p, p->rendered);
	free(p->offsets);
	free(p);
}

static void panel_clamp(struct fyai_panel *p)
{
	int last = p->total > p->rows ? p->total - p->rows : 0;

	if (p->top > last)
		p->top = last;
	if (p->top < 0)
		p->top = 0;
}

int fyai_panel_update(struct fyai_panel *p, const char *source,
		      int cols, int rows)
{
	struct fyai_cfg *cfg = p->ctx->cfg;
	struct fyai_buf out = {0};
	char *copy;
	size_t *offsets, i;
	int saved, rc, total, n;
	bool changed;

	if (!source || cols < 1 || rows < 1)
		return -1;
	changed = !p->source || strcmp(p->source, source);
	p->rows = rows;
	if (!changed && p->cols == cols) {
		panel_clamp(p);
		return 0;
	}
	saved = cfg->render_width;
	cfg->render_width = cols;
	if (p->type == FYAI_PANEL_DIFF)
		rc = markdown_diff_render(cfg, source, strlen(source),
					  &out.data, &out.len);
	else
		rc = markdown_render(cfg, source, strlen(source), &out,
			markdown_color_enabled(cfg->color), cfg->theme_variant);
	cfg->render_width = saved;
	if (rc) {
		panel_render_free(p, out.data);
		return -1;
	}
	total = out.len && out.data[out.len - 1] != '\n';
	for (i = 0; i < out.len; i++)
		if (out.data[i] == '\n')
			total++;
	offsets = calloc((size_t)total + 1, sizeof(*offsets));
	copy = strdup(source);
	if (!offsets || !copy) {
		free(offsets);
		free(copy);
		panel_render_free(p, out.data);
		return -1;
	}
	for (i = 0, n = 1; i < out.len && n <= total; i++)
		if (out.data[i] == '\n')
			offsets[n++] = i + 1;
	free(p->source);
	panel_render_free(p, p->rendered);
	free(p->offsets);
	p->source = copy;
	p->rendered = out.data;
	p->offsets = offsets;
	p->cols = cols;
	p->total = total;
	if (changed)
		p->top = 0;
	panel_clamp(p);
	return 0;
}

void fyai_panel_scroll(struct fyai_panel *p, int delta, bool page)
{
	int step = page && p->rows > 1 ? p->rows - 1 : 1;

	p->top += delta * step;
	panel_clamp(p);
}

const char *fyai_panel_text(const struct fyai_panel *p)
{
	return p->rendered ? p->rendered + p->offsets[p->top] : "";
}

void fyai_panel_extent(const struct fyai_panel *p, int *total, int *top,
		       int *rows)
{
	*total = p->total;
	*top = p->top;
	*rows = p->rows;
}
