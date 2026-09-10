/*
 * fyai_sink.c - the one rendering component and its backends
 *
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_DISPLAY

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libfymermaid.h>

#include "fyai.h"
#include "fyai_agent.h"
#include "fyai_event.h"
#include "fyai_markdown.h"
#include "fyai_sink.h"
#include "fyai_terminal.h"
#include "fyai_terminal_view.h"
#include "fyai_ui.h"

/* The render configuration one diagram source is drawn under. Every path
 * that renders or navigates a diagram must use the same one, because a move
 * is decided by where the elements landed. */
static void sink_diagram_cfg(const struct fyai_cfg *cfg, int cols,
			     const char *selection,
			     struct fymm_render_cfg *render)
{
	fymm_render_cfg_default(render);
	render->width = cols;
	render->color = markdown_color_enabled(cfg->color) ?
		FYMM_COLOR_TRUECOLOR : FYMM_COLOR_NONE;
	render->background = cfg->theme_variant && !strcmp(cfg->theme_variant, "light") ?
		FYMM_BG_LIGHT : FYMM_BG_DARK;
	render->theme = fy_str_empty(cfg->diagram_theme) ? NULL : cfg->diagram_theme;
	if (cfg->diagram_charset) {
		if (!strcmp(cfg->diagram_charset, "ascii")) render->charset = FYMM_CHARSET_ASCII;
		else if (!strcmp(cfg->diagram_charset, "unicode")) render->charset = FYMM_CHARSET_UNICODE;
		else if (!strcmp(cfg->diagram_charset, "rich")) render->charset = FYMM_CHARSET_RICH;
	}
	render->selection = fy_str_empty(selection) ? NULL : selection;
	render->selection_style = FYMM_SEL_AUTO;
	render->fit = FYMM_FIT_LEGEND;
	if (cfg->diagram_fit) {
		if (!strcmp(cfg->diagram_fit, "shrink")) render->fit = FYMM_FIT_SHRINK;
		else if (!strcmp(cfg->diagram_fit, "clip")) render->fit = FYMM_FIT_CLIP;
	}
}

char *fyai_sink_diagram_render(const struct fyai_cfg *cfg, const char *source,
			       const char *selection, int cols)
{
	struct fymm_render_cfg render;
	struct fymm_diagram *diagram;
	char *out;

	if (!cfg || !source || cols < 1)
		return NULL;
	sink_diagram_cfg(cfg, cols, selection, &render);
	diagram = fymm_parse(source, FYMM_NT, NULL);
	if (!diagram)
		return NULL;
	out = fymm_diagram_has_errors(diagram) ? NULL : fymm_render(diagram, &render);
	fymm_diagram_destroy(diagram);
	return out;
}

int fyai_sink_diagram_measure(const struct fyai_cfg *cfg, const char *source,
			      int cols, int *rowsp, bool *legendp)
{
	struct fymm_render_cfg render;
	struct fymm_diagram *diagram;
	struct fymm_render_result *result = NULL;
	const struct fymm_element *e;
	size_t i, count;
	int rc = -1;

	if (!cfg || !source || cols < 1)
		return -1;
	sink_diagram_cfg(cfg, cols, NULL, &render);
	diagram = fymm_parse(source, FYMM_NT, NULL);
	if (!diagram)
		return -1;
	if (fymm_diagram_has_errors(diagram))
		goto out;
	if (!legendp) {
		rc = fymm_measure(diagram, &render, NULL, rowsp);
		goto out;
	}
	/* A legend is the renderer saying the width stopped carrying the
	 * labels, which is what a caller sizing a drawing needs to know. */
	result = fymm_render_ex(diagram, &render);
	if (!result)
		goto out;
	*legendp = false;
	count = fymm_render_result_count(result);
	for (i = 0; i < count; i++) {
		e = fymm_render_result_element(result, i);
		if (e && e->kind == FYMM_EL_LEGEND) {
			*legendp = true;
			break;
		}
	}
	rc = fymm_measure(diagram, &render, NULL, rowsp);
out:
	fymm_render_result_destroy(result);
	fymm_diagram_destroy(diagram);
	return rc;
}

int fyai_sink_diagram_locate(const struct fyai_cfg *cfg, const char *source,
			     const char *path, int cols, int *rowp, int *heightp)
{
	struct fymm_render_cfg render;
	struct fymm_diagram *diagram;
	struct fymm_render_result *result = NULL;
	const struct fymm_element *e;
	int rc = -1;

	if (!cfg || !source || !path || cols < 1)
		return -1;
	sink_diagram_cfg(cfg, cols, NULL, &render);
	diagram = fymm_parse(source, FYMM_NT, NULL);
	if (!diagram)
		return -1;
	if (fymm_diagram_has_errors(diagram))
		goto out;
	result = fymm_render_ex(diagram, &render);
	if (!result)
		goto out;
	e = fymm_render_result_find(result, path);
	if (!e)
		goto out;
	if (rowp)
		*rowp = e->row;
	if (heightp)
		*heightp = e->height;
	rc = 0;
out:
	fymm_render_result_destroy(result);
	fymm_diagram_destroy(diagram);
	return rc;
}

static const enum fymm_direction sink_diagram_dirs[] = {
	[FYAI_DIAGRAM_UP] = FYMM_DIR_UP,
	[FYAI_DIAGRAM_DOWN] = FYMM_DIR_DOWN,
	[FYAI_DIAGRAM_LEFT] = FYMM_DIR_LEFT,
	[FYAI_DIAGRAM_RIGHT] = FYMM_DIR_RIGHT,
};

char *fyai_sink_diagram_navigate(const struct fyai_cfg *cfg, const char *source,
				 const char *from, enum fyai_diagram_move dir,
				 int cols)
{
	struct fymm_render_cfg render;
	struct fymm_diagram *diagram;
	struct fymm_render_result *result = NULL;
	const struct fymm_element *e;
	char *path = NULL;
	size_t steps;

	if (!cfg || !source || cols < 1)
		return NULL;
	sink_diagram_cfg(cfg, cols, NULL, &render);
	diagram = fymm_parse(source, FYMM_NT, NULL);
	if (!diagram)
		return NULL;
	if (fymm_diagram_has_errors(diagram))
		goto out;
	result = fymm_render_ex(diagram, &render);
	if (!result)
		goto out;
	/* The title and the legend are drawn among the nodes. A move that
	 * arrives at one carries on, so every step lands on a branch. */
	for (steps = fymm_render_result_count(result); steps; steps--) {
		e = fymm_navigate(result, from, sink_diagram_dirs[dir]);
		if (!e)
			break;
		if (e->kind == FYMM_EL_NODE) {
			path = strdup(e->path);
			break;
		}
		from = e->path;
	}
out:
	fymm_render_result_destroy(result);
	fymm_diagram_destroy(diagram);
	return path;
}

/* One rendered block placed at a cell origin of the page grid. */
struct sink_pane {
	const char *text;
	size_t offset;
	int row, col, rows, cols;
};

/* Returns 0, or -1 when a row did not reach the view. */
static int sink_pane_feed(struct fyai_terminal_view *view,
			  const struct sink_pane *pane)
{
	const char *p, *e;
	char cup[32];
	int row, n, rc;
	size_t len, lines, offset;

	p = pane->text;
	offset = pane->offset;
	if (offset == FYAI_SINK_OFFSET_TAIL) {
		lines = 0;
		for (e = p; *e; e++)
			lines += *e == '\n';
		if (e != p && e[-1] != '\n')
			lines++;
		offset = lines > (size_t)pane->rows ?
			 lines - (size_t)pane->rows : 0;
	}
	for (len = 0; len < offset && *p; len++) {
		e = strchr(p, '\n');
		p = e ? e + 1 : p + strlen(p);
	}
	for (row = 0; row < pane->rows && *p; row++) {
		e = strchr(p, '\n');
		len = e ? (size_t)(e - p) : strlen(p);
		n = snprintf(cup, sizeof(cup), "\033[%d;%dH\033[0m",
			     pane->row + row + 1, pane->col + 1);
		rc = fyai_terminal_view_feed(view, cup, (size_t)n);
		if (!rc)
			rc = fyai_terminal_view_feed(view, p, len);
		/* One row is enough to say the pane is wrong; the caller
		 * reports it one time. */
		if (rc)
			return -1;
		p += len + (e ? 1 : 0);
	}
	return 0;
}

/* The line between two panes; a zero count draws nothing. */
struct sink_rule {
	const char *glyph;
	int row, col, count;
	bool vertical;
};

/* Returns 0, or -1 when the rule did not reach the view. */
static int sink_rule_feed(struct fyai_terminal_view *view,
			  const struct sink_rule *rule)
{
	char cup[32];
	int i, n, rc;

	for (i = 0; i < rule->count; i++) {
		n = snprintf(cup, sizeof(cup), "\033[%d;%dH\033[0m",
			     (rule->vertical ? rule->row + i : rule->row) + 1,
			     (rule->vertical ? rule->col : rule->col + i) + 1);
		rc = fyai_terminal_view_feed(view, cup, (size_t)n);
		if (!rc)
			rc = fyai_terminal_view_feed(view, rule->glyph,
						     strlen(rule->glyph));
		if (rc)
			return -1;
	}
	return 0;
}

static int sink_page_publish(struct fyai_sink *s, struct fytim_surface *surface,
			     const struct sink_pane *panes, size_t count,
			     const struct sink_rule *rule, int rows, int cols)
{
	struct fyai_terminal_view *view;
	size_t i;
	int rc;

	view = fyai_terminal_view_create(s->ctx, rows, cols, 0);
	fyai_error_check(s->ctx, view, err_out, "cannot make the page view");
	/* Rows are hard-wrapped at their pane width; autowrap would leak into
	 * the pane beside them. */
	rc = fyai_terminal_view_feed(view, "\033[?7l", 5);
	for (i = 0; !rc && i < count; i++)
		rc = sink_pane_feed(view, &panes[i]);
	if (!rc && rule && rule->count > 0)
		rc = sink_rule_feed(view, rule);
	if (!rc)
		rc = fyai_terminal_view_feed(view, "\033[0m", 4);
	if (rc)
		fyai_warning(s->ctx, "the page is drawn without some of its rows");
	fyai_terminal_view_damage_all(view);
	rc = fyai_ui_surface_publish(surface, view);
	fyai_terminal_view_destroy(view);
	return rc < 0 ? -1 : 0;

err_out:
	return -1;
}

/* Render one Markdown source, with an optional diagram appended. */
/* Render one Markdown source, with an optional diagram under it presented
 * from row @skip of the drawing. */
static int sink_render(struct fyai_sink *s, const char *md, const char *source,
		       const char *selection, int skip, int cols,
		       struct response_buffer *out)
{
	struct fyai_cfg cfg;
	char *diagram = NULL;
	const char *p, *e;
	int rc;

	cfg = *s->ctx->cfg;
	cfg.render_width = cols;
	rc = markdown_render(&cfg, md, strlen(md), out,
			     markdown_color_enabled(cfg.color), cfg.theme_variant);
	if (rc || !source)
		return rc;
	diagram = fyai_sink_diagram_render(&cfg, source, selection, cols);
	if (!diagram)
		return -1;
	/* The drawing is panned, not the page: the header stands above it. */
	for (p = diagram; skip > 0 && *p; skip--) {
		e = strchr(p, '\n');
		if (!e)
			break;
		p = e + 1;
	}
	rc = response_buffer_append(out, p);
	fymm_free(diagram);
	return rc;
}

int fyai_sink_page_extent(const struct fyai_sink_page *page, int rows, int cols,
			  enum fyai_sink_split *split)
{
	int extent, room, whole;

	*split = page->split;
	if (*split == FYAI_SINK_SPLIT_AUTO)
		*split = cols >= 100 ? FYAI_SINK_SPLIT_RIGHT :
			rows >= 16 ? FYAI_SINK_SPLIT_BOTTOM : FYAI_SINK_SPLIT_NONE;
	if (!page->aside || !*page->aside || *split == FYAI_SINK_SPLIT_NONE)
		goto none;
	whole = *split == FYAI_SINK_SPLIT_RIGHT ? cols : rows;
	room = *split == FYAI_SINK_SPLIT_RIGHT ? 44 : 6;
	/* The rule and the columns each side of it come out of the whole. */
	whole -= *split == FYAI_SINK_SPLIT_RIGHT ? 2 : 0;
	extent = page->aside_cols > 0 && *split == FYAI_SINK_SPLIT_RIGHT ?
		 page->aside_cols : whole * (page->extent > 0 ? page->extent : 40) / 100;
	if (extent < (*split == FYAI_SINK_SPLIT_RIGHT ? 28 : 4))
		goto none;
	/* The separator column or row is taken from the aside. */
	if (whole - extent - 1 < room)
		goto none;
	return extent;
none:
	*split = FYAI_SINK_SPLIT_NONE;
	return 0;
}

int fyai_sink_markdown_measure(struct fyai_sink *s, const char *md, int cols)
{
	struct response_buffer out = {};
	struct fyai_cfg cfg;
	size_t i;
	int rows = 0;

	if (!s || !md || cols < 1)
		return -1;
	cfg = *s->ctx->cfg;
	cfg.render_width = cols;
	if (markdown_render(&cfg, md, strlen(md), &out,
			    markdown_color_enabled(cfg.color), cfg.theme_variant)) {
		free(out.data);
		return -1;
	}
	for (i = 0; i < out.len; i++)
		rows += out.data[i] == '\n';
	if (out.len && out.data[out.len - 1] != '\n')
		rows++;
	free(out.data);
	return rows;
}

int fyai_sink_page_cols(const struct fyai_sink_page *page, int rows, int cols)
{
	enum fyai_sink_split split;
	int extent;

	extent = fyai_sink_page_extent(page, rows, cols, &split);
	return split == FYAI_SINK_SPLIT_RIGHT ? cols - extent - 3 : cols;
}

int fyai_sink_page_rows(const struct fyai_sink_page *page, int rows, int cols)
{
	enum fyai_sink_split split;
	int extent;

	extent = fyai_sink_page_extent(page, rows, cols, &split);
	return split == FYAI_SINK_SPLIT_BOTTOM ? rows - extent - 1 : rows;
}

static int sink_page(struct fyai_sink *s, struct fytim_surface *surface,
		     const struct fyai_sink_page *page)
{
	struct response_buffer out = {}, aside = {};
	struct sink_pane panes[2];
	struct sink_rule rule = {};
	bool ascii;
	enum fyai_sink_split split;
	int rows, cols, extent, rc;
	size_t count = 1;

	if (!s || !surface || !page || !page->markdown)
		return -1;
	rows = fyai_ui_surface_granted_rows(surface);
	cols = fyai_ui_surface_granted_cols(surface);
	if (rows < 1 || cols < 1)
		return 0;
	extent = fyai_sink_page_extent(page, rows, cols, &split);
	panes[0] = (struct sink_pane){ .offset = page->offset,
				       .rows = fyai_sink_page_rows(page, rows, cols),
				       .cols = fyai_sink_page_cols(page, rows, cols) };
	rc = sink_render(s, page->markdown, page->diagram,
			 page->diagram_selection, page->diagram_row,
			 panes[0].cols, &out);
	if (rc)
		goto out;
	panes[0].text = out.data ? out.data : "";
	if (split != FYAI_SINK_SPLIT_NONE) {
		if (page->aside_rendered)
			rc = response_buffer_append(&aside, page->aside);
		else
			rc = sink_render(s, page->aside, NULL, NULL, 0,
					 split == FYAI_SINK_SPLIT_RIGHT ? extent : cols,
					 &aside);
		if (rc)
			goto out;
		panes[count++] = (struct sink_pane){
			.text = aside.data ? aside.data : "",
			.offset = page->aside_offset,
			.row = split == FYAI_SINK_SPLIT_BOTTOM ? panes[0].rows + 1 : 0,
			.col = split == FYAI_SINK_SPLIT_RIGHT ? cols - extent : 0,
			.rows = split == FYAI_SINK_SPLIT_BOTTOM ? extent : rows,
			.cols = split == FYAI_SINK_SPLIT_RIGHT ? extent : cols,
		};
		ascii = s->ctx->cfg->diagram_charset &&
			!strcmp(s->ctx->cfg->diagram_charset, "ascii");
		rule.vertical = split == FYAI_SINK_SPLIT_RIGHT;
		rule.glyph = rule.vertical ? (ascii ? "|" : "\u2502") :
			(ascii ? "-" : "\u2500");
		rule.row = rule.vertical ? 0 : panes[0].rows;
		rule.col = rule.vertical ? panes[0].cols + 1 : 0;
		rule.count = rule.vertical ? rows : cols;
	}
	rc = sink_page_publish(s, surface, panes, count, &rule, rows, cols);
out:
	free(out.data);
	free(aside.data);
	return rc < 0 ? -1 : 0;
}

int fyai_sink_page(struct fyai_sink *s, struct fytim_surface *surface,
		   const char *md, size_t offset)
{
	struct fyai_sink_page page = { .markdown = md, .offset = offset };

	return sink_page(s, surface, &page);
}

int fyai_sink_diagram_page(struct fyai_sink *s, struct fytim_surface *surface,
			   const char *header, const char *source)
{
	struct fyai_sink_page page = { .markdown = header, .diagram = source };

	return sink_page(s, surface, &page);
}

int fyai_sink_page_split(struct fyai_sink *s, struct fytim_surface *surface,
			 const struct fyai_sink_page *page)
{
	return sink_page(s, surface, page);
}

/* The only progressive Markdown renderer in the process. */
struct sink_term {
	struct markdown_renderer renderer;
	struct response_buffer source;	/* the document as given so far */
	struct response_buffer pending;	/* appended, not yet pushed */
	size_t active_rows;		/* rows the live region occupies */
	int render_cols;		/* width used to wrap the rows */
	int64_t last_draw_ms;		/* monotonic time of the last repaint */
	struct fyai_sink_band *shared_band;
	enum fyai_sink_doc_kind kind;
	bool render_live;
	bool doc_open;
	bool present;			/* this document may reach the user */
	bool passthrough;		/* no renderer: write bytes as they come */
	bool oneshot;			/* accumulate, render when it closes */
	bool wrote;			/* the passthrough path wrote something */
	bool write_failed;		/* a write failed; report it once */
	FILE *out;			/* NULL presents on standard output */
};

/* Where this backend presents. An offscreen sink renders into a stream of the
 * caller, which owns it. */
static FILE *sink_term_out(const struct sink_term *t)
{
	return t->out ? t->out : stdout;
}

static int sink_term_write_failed(struct fyai_sink *s, FILE *fp);

/* Tool children reserve standard output for JSON-RPC frames. */
/* Present only when descriptor 1 is not a JSON-RPC channel. */
static bool sink_may_present(const struct fyai_sink *s)
{
	if (s->ctx->cfg->agent_pty)
		return true;
	return !s->ctx->cfg->tool_child && !fyai_agent_delegated(s->ctx);
}

static struct sink_term *sink_term_state(const struct fyai_sink *s)
{
	return s ? s->state : NULL;
}

static bool sink_term_wants_live(const struct fyai_sink *s,
				 enum fyai_sink_doc_kind kind)
{
	struct fyai_ctx *ctx = s->ctx;
	const struct sink_term *t = s->state;

	return t->present && !t->out && kind == FYAI_SINK_DOC_ASSISTANT &&
	       ctx->cfg->markdown && ctx->stdout_tty &&
	       markdown_available(ctx->cfg) &&
	       strcmp(ctx->cfg->markdown_mode, "oneshot");
}

static int sink_term_renderer_start(struct fyai_sink *s)
{
	struct sink_term *t = sink_term_state(s);
	struct fyai_ctx *ctx = s->ctx;
	int rc;

	if (!sink_term_wants_live(s, t->kind))
		return 0;
	rc = markdown_renderer_start(ctx->cfg, &t->renderer,
			markdown_color_enabled(ctx->cfg->color),
			ctx->cfg->theme_variant);
	fyai_error_check(ctx, !rc, err,
		"could not start the display renderer");
	t->render_live = true;
	t->render_cols = markdown_effective_width(ctx->cfg);
	return 0;
err:
	return -1;
}

/* Replace the mutable rows and retain the rows that the renderer freezes. */
static void sink_term_apply(struct fyai_sink *s,
			    const struct markdown_update *update)
{
	struct sink_term *t = sink_term_state(s);
	size_t backtrack = update->backtrack;
	bool ui = fyai_ui_active(s->ctx);

	/* Track active rows for direct output and display-owned tails. */
	if (ui)
		(void)fyai_ui_tail_apply(s->ctx, update);
	else if (backtrack) {
		fprintf(sink_term_out(t), FYAI_ANSI_CURSOR_UP_FMT, backtrack);
		fputs(FYAI_ANSI_ERASE_DOWN, sink_term_out(t));
	}
	if (backtrack <= t->active_rows)
		t->active_rows -= backtrack;
	else
		t->active_rows = 0;
	if (!ui && update->content_len)
		fwrite(update->content, 1, update->content_len, sink_term_out(t));
	t->active_rows += fyai_count_newlines(update->content,
					      update->content_len);
	if (update->freeze >= t->active_rows)
		t->active_rows = 0;
	else
		t->active_rows -= update->freeze;
	if (!ui)
		fflush(sink_term_out(t));
}

static int sink_term_push_pending(struct fyai_sink *s)
{
	struct sink_term *t = sink_term_state(s);
	struct markdown_update update;

	if (!t->render_live || !t->pending.len)
		return 0;
	if (markdown_renderer_push(&t->renderer, t->pending.data,
				   t->pending.len, &update))
		return -1;
	sink_term_apply(s, &update);
	t->pending.len = 0;
	t->pending.data[0] = '\0';
	return 0;
}

/* Replace the progressive region with the healed final rendering. */
static int sink_term_render_finish(struct fyai_sink *s)
{
	struct sink_term *t = sink_term_state(s);
	struct response_buffer rendered = {0};
	size_t end;
	bool line_start;

	if (!t->render_live)
		return 0;
	if (sink_term_push_pending(s) ||
	    markdown_renderer_finish(&t->renderer, &rendered)) {
		free(rendered.data);
		return -1;
	}
	end = terminal_trim_blank_rows(rendered.data, rendered.len);
	line_start = terminal_text_at_line_start(rendered.data, end);
	if (fyai_ui_active(s->ctx)) {
		fyai_ui_tail_finish(s->ctx, rendered.data, end);
		if (!line_start)
			(void)fyai_ui_commit(s->ctx, "\n", 1);
	} else {
		if (t->active_rows) {
			fprintf(sink_term_out(t), FYAI_ANSI_CURSOR_UP_FMT,
				t->active_rows);
			fputs(FYAI_ANSI_ERASE_DOWN, sink_term_out(t));
		}
		if (end)
			fwrite(rendered.data, 1, end, sink_term_out(t));
		if (!line_start)
			fputc('\n', sink_term_out(t));
		fflush(sink_term_out(t));
	}
	t->active_rows = 0;
	t->render_live = false;
	free(rendered.data);
	return 0;
}

static int sink_term_doc_begin(struct fyai_sink *s,
			       const struct fyai_sink_doc *doc)
{
	struct sink_term *t = sink_term_state(s);
	struct fyai_ctx *ctx = s->ctx;

	t->kind = doc->kind;
	t->active_rows = 0;
	t->pending.len = 0;
	if (t->pending.data)
		t->pending.data[0] = '\0';
	t->source.len = 0;
	if (t->source.data)
		t->source.data[0] = '\0';
	t->last_draw_ms = 0;
	/* The display path presents user and system cards; an offscreen render
	 * has no display, so it presents every document itself. */
	t->present = sink_may_present(s) &&
		     (t->out || doc->kind == FYAI_SINK_DOC_ASSISTANT);
	/*
	 * With no Markdown to render, the source is the presentation: write it
	 * through as it arrives so the reader still sees the model type.
	 */
	t->passthrough = t->present && (!ctx->cfg->markdown ||
					!markdown_available(ctx->cfg));
	t->wrote = false;
	t->doc_open = true;
	if (sink_term_renderer_start(s))
		return -1;
	/* Retain a document only when closing it is the first presentation. */
	t->oneshot = t->present && !t->passthrough && !t->render_live;
	return 0;
}

/* Apply the configured line or timed stream redraw cadence. */
static bool sink_term_wants_draw(struct fyai_sink *s, const char *text,
				 size_t len)
{
	struct sink_term *t = sink_term_state(s);
	int64_t now;

	if (!strcmp(s->ctx->cfg->markdown_mode, "line"))
		return memchr(text, '\n', len) != NULL;
	now = fyai_event_now_ms();
	if (now - t->last_draw_ms <
	    s->ctx->cfg->markdown_update_interval_ms)
		return false;
	t->last_draw_ms = now;
	return true;
}

static int sink_term_doc_append(struct fyai_sink *s, const char *text,
				size_t len)
{

	struct sink_term *t = sink_term_state(s);
	struct fyai_ctx *ctx = s->ctx;
	int rc;

	if (!t->present || !len)
		return 0;
	if (t->passthrough) {
		t->wrote = true;
		if (fwrite(text, 1, len, sink_term_out(t)) != len ||
		    fflush(sink_term_out(t)))
			return sink_term_write_failed(s, sink_term_out(t));
		return 0;
	}
	if (t->oneshot) {
		rc = response_buffer_append_data(&t->source, text, len);
		fyai_error_check(ctx, !rc, err,
			"could not grow the display buffer");
		return 0;
	}
	if (!t->render_live)
		return 0;
	/* Retain live source for width reflow. */
	rc = response_buffer_append_data(&t->source, text, len);
	fyai_error_check(ctx, !rc, err,
		"could not grow the display source buffer");
	rc = response_buffer_reserve(&t->pending, t->pending.len + len + 1);
	fyai_error_check(ctx, !rc,
		err, "could not grow the progressive display buffer");
	memcpy(t->pending.data + t->pending.len, text, len);
	t->pending.len += len;
	t->pending.data[t->pending.len] = '\0';
	if (!sink_term_wants_draw(s, text, len))
		return 0;
	rc = sink_term_push_pending(s);
	fyai_error_check(ctx, !rc, err,
			 "could not render the display output");
	return 0;
err:
	return -1;
}

/* Present retained source when the document closes. */
static void sink_term_present_oneshot(struct fyai_sink *s)
{
	struct sink_term *t = sink_term_state(s);
	struct fyai_cfg *cfg = s->ctx->cfg;

	if (t->passthrough) {
		/* Close the row the passthrough text was written on. */
		if (t->wrote)
			fputc('\n', sink_term_out(t));
		fflush(sink_term_out(t));
		return;
	}
	if (!t->oneshot || !t->source.len)
		return;
	/* Only an assistant document is presented here, unless this sink is
	 * the whole presentation. */
	(void)fyai_sink_unit(s, FYAI_SINK_TRANSCRIPT, FYAI_FLOW_PROSE);
	if (fyai_fprint_markdown(sink_term_out(t), t->source.data, cfg, 0))
		fwrite(t->source.data, 1, t->source.len, sink_term_out(t));
	fflush(sink_term_out(t));
}

static int sink_term_doc_end(struct fyai_sink *s, bool aborted)
{
	struct sink_term *t = sink_term_state(s);
	int rc;

	(void)aborted;
	if (t->render_live)
		rc = sink_term_render_finish(s);
	else {
		sink_term_present_oneshot(s);
		rc = 0;
	}
	markdown_renderer_destroy(&t->renderer);
	t->doc_open = false;
	return rc;
}

/*
 * Drop the document. The renderer is released without asking it for the healed
 * final form, so nothing further reaches the display; whatever the progressive
 * repaint already drew stays on screen as the record of what happened.
 */
static void sink_term_doc_discard(struct fyai_sink *s)
{
	struct sink_term *t = sink_term_state(s);

	if (!t)
		return;
	markdown_renderer_destroy(&t->renderer);
	t->pending.len = 0;
	t->active_rows = 0;
	t->render_live = false;
	t->doc_open = false;
}

/* Rerender the live region at the current width. */
static void sink_term_reflow(struct fyai_sink *s)
{
	struct sink_term *t = sink_term_state(s);
	struct fyai_ctx *ctx = s->ctx;
	struct markdown_renderer r;
	struct markdown_update update;
	struct markdown_update repaint;
	int cols;

	if (!t || !t->render_live || !t->source.len)
		return;
	cols = markdown_effective_width(ctx->cfg);
	if (cols <= 0 || cols == t->render_cols)
		return;
	/* Apply pending source before replacing the renderer. */
	if (sink_term_push_pending(s))
		return;
	/* Retain existing rows if renderer creation fails. */
	if (markdown_renderer_start(ctx->cfg, &r,
				    markdown_color_enabled(ctx->cfg->color),
				    ctx->cfg->theme_variant))
		return;
	if (markdown_renderer_push(&r, t->source.data, t->source.len,
				   &update)) {
		markdown_renderer_destroy(&r);
		return;
	}
	/* Replace the complete active region with the new render. */
	repaint = update;
	repaint.backtrack = t->active_rows;
	sink_term_apply(s, &repaint);
	markdown_renderer_destroy(&t->renderer);
	t->renderer = r;
	t->render_cols = cols;
}

static int sink_term_doc_pause(struct fyai_sink *s)
{
	struct sink_term *t = sink_term_state(s);
	struct fyai_ctx *ctx = s->ctx;
	int rc;

	if (!t->render_live)
		return 0;
	rc = sink_term_render_finish(s);
	fyai_error_check(ctx, !rc, err,
			 "could not checkpoint the display output");
	markdown_renderer_destroy(&t->renderer);
	return 0;
err:
	return -1;
}

static int sink_term_doc_resume(struct fyai_sink *s)
{
	struct sink_term *t = sink_term_state(s);
	struct fyai_ctx *ctx = s->ctx;
	int rc;

	if (!t->doc_open || t->render_live)
		return 0;
	rc = sink_term_renderer_start(s);
	fyai_error_check(ctx, !rc, err,
			 "could not resume the display output");
	return 0;
err:
	return -1;
}

static bool sink_term_doc_is_live(const struct fyai_sink *s)
{
	const struct sink_term *t = sink_term_state(s);

	return t && t->render_live;
}

static void sink_term_destroy(struct fyai_sink *s)
{
	struct sink_term *t = sink_term_state(s);

	if (!t)
		return;
	markdown_renderer_destroy(&t->renderer);
	free(t->shared_band);
	free(t->source.data);
	free(t->pending.data);
	free(t);
	s->state = NULL;
}

/* A shared sequential band or an independently owned parallel band. */
struct fyai_sink_band {
	struct fyai_ctx *ctx;
	struct fytim_workband *wb;	/* NULL for the shared band */
	bool shared;
	bool tile;			/* independent work-pane tile */
};

static bool sink_term_bands_available(const struct fyai_sink *s)
{
	return fyai_ui_active(s->ctx);
}

static struct fyai_sink_band *sink_term_band_open(struct fyai_sink *s,
						  bool shared,
						  const char *title,
						  const char *command)
{
	struct sink_term *t = sink_term_state(s);
	struct fyai_sink_band *b;

	if (!fyai_ui_active(s->ctx))
		return NULL;
	if (shared && t->shared_band) {
		/* One shared band at a time: the previous one is replaced. */
		free(t->shared_band);
		t->shared_band = NULL;
	}
	b = calloc(1, sizeof(*b));
	if (!b)
		return NULL;
	b->ctx = s->ctx;
	b->shared = shared;
	if (shared) {
		if (command)
			fyai_ui_shell_begin(s->ctx, title, command);
		else
			fyai_ui_tool_begin(s->ctx, title);
		t->shared_band = b;
		return b;
	}
	/* Place independent work in a work-pane tile. */
	b->wb = fyai_ui_work_tile_create(s->ctx);
	if (!b->wb) {
		free(b);
		return NULL;
	}
	b->tile = true;
	return b;
}

static void sink_term_band_paint(struct fyai_sink_band *b, const char *title,
				 const char *command, const char *body,
				 size_t len, const char *margin)
{
	if (!b)
		return;
	/* The shared band keeps the title it opened with; only the body moves. */
	if (b->shared) {
		if (len)
			fyai_ui_tool_update(b->ctx, body, len);
		return;
	}
	if (command)
		fyai_ui_shell_workband_update(b->ctx, b->wb, title, command,
					      body, len, margin);
	else
		fyai_ui_workband_update(b->ctx, b->wb, title, body, len, margin);
}

static void sink_term_band_close(struct fyai_sink *s, bool ok,
				 const char *cause)
{
	struct sink_term *t = sink_term_state(s);

	if (!fyai_ui_active(s->ctx))
		return;
	fyai_ui_tool_end(s->ctx, ok, cause);
	free(t->shared_band);
	t->shared_band = NULL;
}

static struct fyai_sink_band *sink_term_band_shared(struct fyai_sink *s)
{
	struct sink_term *t = sink_term_state(s);

	return t ? t->shared_band : NULL;
}

static void sink_term_band_commit(struct fyai_sink_band *b)
{
	if (!b)
		return;
	if (b->wb)
		fyai_ui_work_tile_destroy(b->ctx, b->wb, true);
	free(b);
}

static void sink_term_band_destroy(struct fyai_sink_band *b)
{
	if (!b)
		return;
	if (b->wb)
		fyai_ui_work_tile_destroy(b->ctx, b->wb, false);
	free(b);
}

/* Select the stream file, or discard output that can corrupt JSON-RPC. */
static FILE *sink_term_stream_file(const struct fyai_sink *s,
				   enum fyai_sink_stream stream)
{
	const struct sink_term *t = s->state;

	switch (stream) {
	case FYAI_SINK_STATUS:
	case FYAI_SINK_DIAG:
		/* An offscreen render carries content only. */
		return t->out ? NULL : stderr;
	case FYAI_SINK_TRANSCRIPT:
	case FYAI_SINK_NOTICE:
	case FYAI_SINK_MACHINE:
		break;
	}
	return sink_may_present(s) ? sink_term_out(t) : NULL;
}

static int sink_term_markdown(struct fyai_sink *s, enum fyai_sink_stream stream,
			      const char *md)
{
	struct fyai_cfg *cfg = s->ctx->cfg;
	FILE *fp = sink_term_stream_file(s, stream);

	if (!md || !*md || !fp)
		return 0;
	/*
	 * Machine data is parsed by another program: never decorate it.
	 * Everything else is rendered, and falls back to its own source when
	 * no renderer is available. display/markdown is deliberately not
	 * consulted here: it governs the assistant transcript, not a table or
	 * a notice, which stay readable either way.
	 */
	if (stream == FYAI_SINK_MACHINE ||
	    fyai_fprint_markdown(fp, md, cfg, 0)) {
		if (fputs(md, fp) == EOF || fflush(fp))
			return sink_term_write_failed(s, fp);
	}
	return 0;
}

/* Report only the first output failure. */
static int sink_term_write_failed(struct fyai_sink *s, FILE *fp)
{
	struct sink_term *t = sink_term_state(s);
	int err = errno;

	clearerr(fp);
	if (t->write_failed)
		return -1;
	t->write_failed = true;
	fyai_error(s->ctx, "output could not be written: %s", strerror(err));
	return -1;
}

static int sink_term_write(struct fyai_sink *s, enum fyai_sink_stream stream,
			   const char *buf, size_t len)
{
	FILE *fp;
	size_t written;
	int rc;

	fp = sink_term_stream_file(s, stream);
	if (!buf || !len || !fp)
		return 0;
	written = fwrite(buf, 1, len, fp);
	rc = fflush(fp);
	if (written != len || rc)
		return sink_term_write_failed(s, fp);
	return 0;
}

static void sink_term_flush(struct fyai_sink *s)
{
	struct sink_term *t = s->state;

	fflush(sink_term_out(t));
	if (!t->out)
		fflush(stderr);
}

static const struct fyai_sink_ops sink_terminal_ops = {
	.name		= "terminal",
	.doc_begin	= sink_term_doc_begin,
	.doc_append	= sink_term_doc_append,
	.doc_end	= sink_term_doc_end,
	.doc_discard	= sink_term_doc_discard,
	.doc_reflow	= sink_term_reflow,
	.doc_pause	= sink_term_doc_pause,
	.doc_resume	= sink_term_doc_resume,
	.doc_is_live	= sink_term_doc_is_live,
	.bands_available = sink_term_bands_available,
	.band_open	= sink_term_band_open,
	.band_paint	= sink_term_band_paint,
	.band_close	= sink_term_band_close,
	.band_commit	= sink_term_band_commit,
	.band_destroy	= sink_term_band_destroy,
	.band_shared	= sink_term_band_shared,
	.markdown	= sink_term_markdown,
	.write		= sink_term_write,
	.flush		= sink_term_flush,
	.destroy	= sink_term_destroy,
};

/*
 * The capture backend. It has no terminal, so no document repaints: the source
 * is kept as it arrives and presented when the document closes, which is the
 * same order a document format would write it in.
 */
struct sink_capture {
	struct response_buffer text;	/* everything presented so far */
	struct response_buffer source;	/* the open document */
	bool doc_open;
};

static int sink_capture_doc_begin(struct fyai_sink *s,
				  const struct fyai_sink_doc *doc)
{
	struct sink_capture *c = s->state;

	(void)doc;
	c->source.len = 0;
	if (c->source.data)
		c->source.data[0] = '\0';
	c->doc_open = true;
	return 0;
}

static int sink_capture_doc_append(struct fyai_sink *s, const char *text,
				   size_t len)
{
	struct sink_capture *c = s->state;

	if (!c->doc_open || !len)
		return 0;
	return response_buffer_append_data(&c->source, text, len);
}

static int sink_capture_doc_end(struct fyai_sink *s, bool aborted)
{
	struct sink_capture *c = s->state;
	int rc = 0;

	(void)aborted;
	if (c->source.len)
		rc = response_buffer_append_data(&c->text, c->source.data,
						 c->source.len);
	c->source.len = 0;
	c->doc_open = false;
	return rc;
}

static void sink_capture_doc_discard(struct fyai_sink *s)
{
	struct sink_capture *c = s->state;

	c->source.len = 0;
	c->doc_open = false;
}

static int sink_capture_text(struct fyai_sink *s, enum fyai_sink_stream stream,
			     const char *buf, size_t len)
{
	struct sink_capture *c = s->state;

	(void)stream;
	if (!buf || !len)
		return 0;
	return response_buffer_append_data(&c->text, buf, len);
}

static int sink_capture_markdown(struct fyai_sink *s,
				 enum fyai_sink_stream stream, const char *md)
{
	return sink_capture_text(s, stream, md, md ? strlen(md) : 0);
}

static void sink_capture_destroy(struct fyai_sink *s)
{
	struct sink_capture *c = s->state;

	if (!c)
		return;
	free(c->text.data);
	free(c->source.data);
	free(c);
	s->state = NULL;
}

static const struct fyai_sink_ops sink_capture_ops = {
	.name		= "capture",
	.doc_begin	= sink_capture_doc_begin,
	.doc_append	= sink_capture_doc_append,
	.doc_end	= sink_capture_doc_end,
	.doc_discard	= sink_capture_doc_discard,
	.markdown	= sink_capture_markdown,
	.write		= sink_capture_text,
	.destroy	= sink_capture_destroy,
};

struct fyai_sink *fyai_sink_create_capture(struct fyai_ctx *ctx)
{
	struct fyai_sink *s;
	struct sink_capture *c;

	if (!ctx)
		return NULL;
	s = calloc(1, sizeof(*s));
	if (!s)
		return NULL;
	c = calloc(1, sizeof(*c));
	if (!c) {
		free(s);
		return NULL;
	}
	s->ctx = ctx;
	s->ops = &sink_capture_ops;
	s->state = c;
	fyai_flow_reset(&s->flow, ctx->cfg);
	return s;
}

const char *fyai_sink_captured(const struct fyai_sink *s, size_t *lenp)
{
	const struct sink_capture *c;

	if (!s || s->ops != &sink_capture_ops) {
		if (lenp)
			*lenp = 0;
		return "";
	}
	c = s->state;
	if (lenp)
		*lenp = c->text.len;
	return c->text.data ? c->text.data : "";
}

void fyai_sink_capture_reset(struct fyai_sink *s)
{
	struct sink_capture *c;

	if (!s || s->ops != &sink_capture_ops)
		return;
	c = s->state;
	c->text.len = 0;
	if (c->text.data)
		c->text.data[0] = '\0';
}

struct fyai_sink *fyai_sink_create(struct fyai_ctx *ctx)
{
	struct fyai_sink *s;
	struct sink_term *t;

	if (!ctx)
		return NULL;
	s = calloc(1, sizeof(*s));
	if (!s)
		return NULL;
	t = calloc(1, sizeof(*t));
	if (!t) {
		free(s);
		return NULL;
	}
	s->ctx = ctx;
	s->ops = &sink_terminal_ops;
	s->state = t;
	fyai_flow_reset(&s->flow, ctx->cfg);
	return s;
}

struct fyai_sink *fyai_sink_create_render(struct fyai_ctx *ctx, FILE *out)
{
	struct fyai_sink *s;

	if (!out)
		return NULL;
	s = fyai_sink_create(ctx);
	if (!s)
		return NULL;
	((struct sink_term *)s->state)->out = out;
	return s;
}

struct fyai_flow *fyai_sink_flow(struct fyai_sink *s)
{
	return s ? &s->flow : NULL;
}

int fyai_sink_unit(struct fyai_sink *s, enum fyai_sink_stream stream,
		   enum fyai_flow_unit unit)
{
	struct fyai_flow_sep sep;
	unsigned i;
	int rc;

	if (!s)
		return 0;
	sep = fyai_flow_before(&s->flow, unit);
	fyai_diag_tracef("sinkunit", "prev=%s next=%s rows=%u blank=%u ls=%d",
			 fyai_flow_unit_name(s->flow.prev),
			 fyai_flow_unit_name(unit), sep.rows,
			 s->flow.blank_rows, (int)s->flow.at_line_start);
	/* Close a partial row before the separation is drawn. */
	if (!s->flow.at_line_start) {
		rc = fyai_sink_write(s, stream, "\n", 1);
		if (rc)
			return rc;
	}
	if (sep.markdown) {
		rc = fyai_sink_markdown(s, stream, sep.markdown);
		if (rc)
			return rc;
	}
	for (i = 0; i < sep.rows; i++) {
		rc = fyai_sink_write(s, stream, "\n", 1);
		if (rc)
			return rc;
	}
	fyai_flow_emitted(&s->flow, unit, true);
	return 0;
}

void fyai_sink_destroy(struct fyai_sink *s)
{
	if (!s)
		return;
	if (s->ops->destroy)
		s->ops->destroy(s);
	free(s);
}

int fyai_sink_doc_begin(struct fyai_sink *s, enum fyai_sink_doc_kind kind)
{
	struct fyai_sink_doc doc;

	if (!s || !s->ops->doc_begin)
		return 0;
	memset(&doc, 0, sizeof(doc));
	doc.kind = kind;
	return s->ops->doc_begin(s, &doc);
}

int fyai_sink_doc_append(struct fyai_sink *s, const char *text, size_t len)
{
	if (!s || !s->ops->doc_append)
		return 0;
	return s->ops->doc_append(s, text, len);
}

int fyai_sink_doc_end(struct fyai_sink *s, bool aborted)
{
	if (!s || !s->ops->doc_end)
		return 0;
	return s->ops->doc_end(s, aborted);
}

void fyai_sink_reflow(struct fyai_sink *s)
{
	if (s && s->ops->doc_reflow)
		s->ops->doc_reflow(s);
}

void fyai_sink_doc_discard(struct fyai_sink *s)
{
	if (s && s->ops->doc_discard)
		s->ops->doc_discard(s);
}

int fyai_sink_doc_pause(struct fyai_sink *s)
{
	if (!s || !s->ops->doc_pause)
		return 0;
	return s->ops->doc_pause(s);
}

int fyai_sink_doc_resume(struct fyai_sink *s)
{
	if (!s || !s->ops->doc_resume)
		return 0;
	return s->ops->doc_resume(s);
}

bool fyai_sink_doc_is_live(const struct fyai_sink *s)
{
	if (!s || !s->ops->doc_is_live)
		return false;
	return s->ops->doc_is_live(s);
}

bool fyai_sink_bands_available(const struct fyai_sink *s)
{
	if (!s || !s->ops->bands_available)
		return false;
	return s->ops->bands_available(s);
}

struct fyai_sink_band *fyai_sink_band_open(struct fyai_sink *s, bool shared,
					   const char *title,
					   const char *command)
{
	if (!s || !s->ops->band_open)
		return NULL;
	return s->ops->band_open(s, shared, title, command);
}

void fyai_sink_band_paint(struct fyai_sink_band *b, const char *title,
			  const char *command, const char *body, size_t len,
			  const char *margin)
{
	if (b && b->ctx && b->ctx->sink && b->ctx->sink->ops->band_paint)
		b->ctx->sink->ops->band_paint(b, title, command, body, len,
					      margin);
}

void fyai_sink_band_close(struct fyai_sink *s, bool ok, const char *cause)
{
	if (s && s->ops->band_close)
		s->ops->band_close(s, ok, cause);
}

/* Return the granted tile width, or zero for a full-width band. */
int fyai_sink_band_cols(const struct fyai_sink_band *b)
{
	if (!b || !b->tile || !b->wb)
		return 0;
	return fyai_ui_work_tile_cols(b->wb);
}

void fyai_sink_band_commit(struct fyai_sink_band *b)
{
	if (b && b->ctx && b->ctx->sink && b->ctx->sink->ops->band_commit)
		b->ctx->sink->ops->band_commit(b);
}

void fyai_sink_band_destroy(struct fyai_sink_band *b)
{
	if (b && b->ctx && b->ctx->sink && b->ctx->sink->ops->band_destroy)
		b->ctx->sink->ops->band_destroy(b);
}

struct fyai_sink_band *fyai_sink_band_shared(struct fyai_sink *s)
{
	if (!s || !s->ops->band_shared)
		return NULL;
	return s->ops->band_shared(s);
}

int fyai_sink_markdown(struct fyai_sink *s, enum fyai_sink_stream stream,
		       const char *md)
{
	int rc;

	if (!s || !s->ops->markdown)
		return 0;
	rc = s->ops->markdown(s, stream, md);
	/* A rendered block closes its last row. */
	if (!rc && (stream == FYAI_SINK_TRANSCRIPT || stream == FYAI_SINK_NOTICE)) {
		s->flow.at_line_start = true;
		fyai_flow_blank_rows(&s->flow, 0);
	}
	return rc;
}

int fyai_sink_write(struct fyai_sink *s, enum fyai_sink_stream stream,
		    const char *buf, size_t len)
{
	int rc;

	if (!s || !s->ops->write)
		return 0;
	rc = s->ops->write(s, stream, buf, len);
	/* Only the scrollback streams share this medium. */
	if (!rc && len && (stream == FYAI_SINK_TRANSCRIPT ||
			   stream == FYAI_SINK_NOTICE))
		fyai_flow_observe(&s->flow, buf, len);
	return rc;
}

int fyai_sink_printf(struct fyai_sink *s, enum fyai_sink_stream stream,
		     const char *fmt, ...)
{
	va_list ap;
	char *text;
	int len;
	int rc;

	va_start(ap, fmt);
	len = vasprintf(&text, fmt, ap);
	va_end(ap);
	if (len < 0)
		return -1;
	rc = fyai_sink_write(s, stream, text, (size_t)len);
	free(text);
	return rc;
}

int fyai_result(struct fyai_ctx *ctx, const char *fmt, ...)
{
	va_list ap;
	char *text;
	int len;
	int rc;

	va_start(ap, fmt);
	len = vasprintf(&text, fmt, ap);
	va_end(ap);
	if (len < 0)
		return -1;
	rc = fyai_sink_write(ctx ? ctx->sink : NULL, FYAI_SINK_NOTICE, text,
			     (size_t)len);
	free(text);
	return rc;
}

int fyai_result_md(struct fyai_ctx *ctx, const char *md)
{
	return fyai_sink_markdown(ctx ? ctx->sink : NULL, FYAI_SINK_NOTICE, md);
}

int fyai_report(struct fyai_ctx *ctx, const char *fmt, ...)
{
	va_list ap;
	char *text;
	int len;
	int rc;

	va_start(ap, fmt);
	len = vasprintf(&text, fmt, ap);
	va_end(ap);
	if (len < 0)
		return -1;
	rc = fyai_sink_write(ctx ? ctx->sink : NULL, FYAI_SINK_STATUS, text,
			     (size_t)len);
	free(text);
	return rc;
}

void fyai_sink_flush(struct fyai_sink *s)
{
	if (s && s->ops->flush)
		s->ops->flush(s);
}
