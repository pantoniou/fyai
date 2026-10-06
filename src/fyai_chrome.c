/*
 * fyai_chrome.c - the chrome of a tile of the work pane
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_DISPLAY

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libfytimui.h>
#include <libfymd4c.h>

#include "fyai.h"
#include "fyai_chrome.h"
#include "fyai_diag.h"
#include "fyai_markdown.h"
#include "fyai_terminal.h"
#include "fyai_tools.h"
#include "fyai_workpane.h"
#include "utils.h"

enum fyai_chrome_level fyai_chrome_level(const struct fyai_ctx *ctx)
{
	const char *v = ctx && ctx->cfg ? ctx->cfg->work_controls : NULL;

	if (!v || !strcmp(v, "none"))
		return FYAI_CHROME_NONE;
	return !strcmp(v, "full") ? FYAI_CHROME_FULL : FYAI_CHROME_ZOOM;
}

/* True when a tile of @kind draws a screen of cells, which scrolls. */
static bool chrome_kind_scrolls(enum fyai_workpane_tile_kind kind)
{
	switch (kind) {
	case FYAI_WORKPANE_TILE_SHELL:
	case FYAI_WORKPANE_TILE_AGENT:
	case FYAI_WORKPANE_TILE_AGENT_VIEW:
	case FYAI_WORKPANE_TILE_BROWSER:
		return true;
	default:
		return false;
	}
}

unsigned int fyai_chrome_items(const struct fyai_ctx *ctx,
			       enum fyai_workpane_tile_kind kind,
			       enum fyai_chrome_phase phase)
{
	enum fyai_chrome_level level = fyai_chrome_level(ctx);
	unsigned int items = 0;

	if (level == FYAI_CHROME_NONE)
		return 0;
	items |= FYAI_CHROME_WHEEL;
	/* A button acts on a program. A screen that goes into the transcript
	 * has none, and a button there acts on nothing. */
	if (phase != FYAI_CHROME_COMMITTED)
		items |= FYAI_CHROME_BUTTONS;
	if (level == FYAI_CHROME_FULL && chrome_kind_scrolls(kind))
		items |= FYAI_CHROME_SCROLL;
	return items;
}

int fyai_chrome_scroll_cols(unsigned int items)
{
	return items & FYAI_CHROME_SCROLL ? 1 : 0;
}

struct fyai_chrome_frame fyai_chrome_frame(unsigned int items, int margin_cols,
					   int width)
{
	struct fyai_chrome_frame f = { 0 };

	if (width < 0)
		width = 0;
	f.margin = margin_cols > 0 ? (margin_cols < width ? margin_cols : width) : 0;
	f.bar = (items & FYAI_CHROME_SCROLL) && width - f.margin >= 2 ? 1 : 0;
	f.body = width - f.margin - f.bar;
	return f;
}

/*
 * The buttons at the right of the head of a tile, as UI Markdown: minimize,
 * maximize and close. The owner decides what each does. The caller frees it.
 */
static char *chrome_buttons(struct fyai_ctx *ctx)
{
	static const struct {
		const char *id, *glyph, *fallback;
	} buttons[] = {
		{ "tile:minimize", "tile.minimize", "\xe2\x96\x81" },	/* ▁ */
		{ "tile:maximize", "tile.maximize", "\xe2\x96\xa1" },	/* □ */
		{ "tile:close", "tile.close", "\xc3\x97" },		/* × */
	};
	struct response_buffer out = {0};
	char *glyph;
	size_t i;
	int rc = 0;

	for (i = 0; i < ARRAY_SIZE(buttons) && !rc; i++) {
		/* A glyph of the theme is configuration: escape it. */
		glyph = markdown_ui_escape(markdown_glyph(ctx->cfg,
							  buttons[i].glyph,
							  buttons[i].fallback));
		rc = !glyph || response_buffer_append(&out,
			fy_sprintfa(" <fy-act id=\"%s\">%s</fy-act>",
				    buttons[i].id, glyph));
		free(glyph);
	}
	if (rc) {
		fyai_warning(ctx, "cannot write the buttons of a tile");
		free(out.data);
		return NULL;
	}
	return out.data;
}

/*
 * The columns that the text of the UI Markdown @s takes on a row: no tag, no
 * SGR sequence and no emphasis marker counts. With @plain, the text itself
 * goes to @plain, and a codepoint that would take it past @max columns ends
 * it. Returns the columns.
 */
static int chrome_markup_cols(const char *s, struct response_buffer *plain,
			      int max)
{
	unsigned int cp, prev = 0;
	size_t n, len = s ? strlen(s) : 0;
	int cols = 0, w;
	const char *p = s, *end = s + len;

	while (p && p < end) {
		if (*p == '<') {
			while (p < end && *p != '>')
				p++;
			p += p < end;
			continue;
		}
		if (*p == '\x1b') {
			for (p++; p < end && !(*p >= '@' && *p <= '~' &&
					       p[-1] != '\x1b'); p++)
				;
			p += p < end;
			continue;
		}
		if (*p == '*' || *p == '`' || *p == '_' || *p == '\\') {
			p++;
			continue;
		}
		n = fymd_utf8_decode(p, (size_t)(end - p), &cp);
		if (!n)
			break;
		w = fymd_cp_width_next(prev, cp);
		if (plain && cols + w > max)
			break;
		if (plain && response_buffer_append_data(plain, p, n))
			break;
		cols += w;
		prev = cp;
		p += n;
	}
	return cols;
}

/* The phase of the tile @sf whose mark is @mark. */
static enum fyai_chrome_phase chrome_phase(struct fyai_ctx *ctx,
					   struct fytim_surface *sf,
					   enum fyai_ui_mark mark)
{
	if (mark == FYAI_UI_MARK_RUNNING)
		return FYAI_CHROME_RUNNING;
	return fyai_tools_kept_surface(ctx, sf) ? FYAI_CHROME_KEPT :
						  FYAI_CHROME_COMMITTED;
}

int fyai_chrome_update(struct fyai_ctx *ctx, struct fytim_surface *sf,
		       const struct fyai_chrome_spec *spec,
		       unsigned int *interval_msp)
{
	enum fyai_workpane_tile_kind kind = FYAI_WORKPANE_TILE_SHELL;
	struct response_buffer out = {0}, cut = {0};
	struct markdown_region *regions = NULL;
	size_t nregions = 0;
	const char *short_title = NULL;
	char *escaped = NULL, *buttons = NULL, *head = NULL, *margin;
	unsigned int items;
	size_t tlen;
	int saved_width, margin_cols = 0;
	int cols, granted, room;
	int rc;

	if (!ctx || !ctx->ui || !sf || !spec || !spec->title)
		return -1;
	(void)fyai_workpane_tile_owner(ctx->workpane, sf, &kind);
	items = fyai_chrome_items(ctx, kind,
				  chrome_phase(ctx, sf, spec->mark));
	/* fyai writes @right and the buttons, so they take the right edge as
	 * they are. */
	if (items & FYAI_CHROME_BUTTONS)
		buttons = chrome_buttons(ctx);
	/*
	 * The title row is one row: a title that would wrap takes the buttons
	 * with it. The title keeps the columns that the gutter, @right and the
	 * buttons leave, and loses the rest to an ellipsis.
	 */
	granted = fyai_ui_surface_granted_cols(ctx, sf);
	room = granted - markdown_gutter_cols(ctx->cfg) - 1 -
	       chrome_markup_cols(spec->right, NULL, 0) -
	       chrome_markup_cols(buttons, NULL, 0) - 1;
	if (granted > 0 && room > 1 &&
	    chrome_markup_cols(spec->title, NULL, 0) > room) {
		(void)chrome_markup_cols(spec->title, &cut, room - 1);
		if (!response_buffer_append(&cut, "\xe2\x80\xa6"))
			short_title = cut.data;
	}
	/*
	 * The title holds what a model or a program wrote. Escape it, then
	 * make it the label that gives the tile the keys.
	 */
	escaped = markdown_ui_escape(short_title ? short_title : spec->title);
	if (escaped) {
		tlen = strlen(escaped);
		while (tlen && (escaped[tlen - 1] == '\n' ||
				escaped[tlen - 1] == '\r'))
			tlen--;
		if (asprintf(&head,
			     "<fy-act id=\"tile:focus\">%.*s</fy-act>%s%s%s\n",
			     (int)tlen, escaped,
			     spec->right || buttons ? "<fy-fill/>" : "",
			     spec->right ? spec->right : "",
			     buttons ? buttons : "") < 0)
			head = NULL;
	}

	/* Render the head at the granted width of the tile. */
	saved_width = ctx->cfg->render_width;
	cols = granted;
	if (cols > 0) {
		(void)fytim_surface_margin(sf, &margin_cols);
		cols += margin_cols + fyai_chrome_scroll_cols(items);
		ctx->cfg->render_width = cols;
	}
	margin = fyai_ui_indicator(ctx, spec->mark, spec->frame, interval_msp);
	rc = -1;
	if (head)
		rc = markdown_render_tool_head_ui(ctx->cfg, head, strlen(head),
				spec->cause,
				margin ? margin : markdown_gutter_blank(ctx->cfg),
				markdown_gutter_blank(ctx->cfg), &out,
				&regions, &nregions);
	free(margin);
	free(head);
	free(buttons);
	free(escaped);
	free(cut.data);
	/* The tile keeps the regions of the head it shows. */
	fyai_workpane_tile_set_regions(ctx->workpane, sf, regions, nregions);
	if (!rc) {
		response_buffer_trim(&out);
		if (!fy_str_empty(spec->command))
			rc = fyai_ui_append_shell_command(ctx->cfg, &out,
							  spec->command);
	}
	if (!rc) {
		response_buffer_trim(&out);
		rc = fyai_workpane_tile_set_head(ctx->workpane, sf,
						 out.data ? out.data : "");
		fyai_diag_tracef("page", "tile=%p head=%zu", (void *)sf,
				 out.len);
		/* The commit of the surface writes its top into the
		 * transcript; the tile page draws the head by itself. */
		if (!rc && fytim_surface_set_top(sf, out.data ? out.data :
						 spec->title) != FYTIM_OK)
			rc = -1;
	}
	ctx->cfg->render_width = saved_width;
	free(out.data);
	return rc;
}

const char *fyai_chrome_text(const char *v)
{
	if (!v || !strcmp(v, "none"))
		return NULL;
	return v;
}

void fyai_chrome_body(struct fyai_ctx *ctx, struct fytim_surface *sf)
{
	if (!ctx || !ctx->cfg || !sf)
		return;
	(void)fytim_surface_set_margin(sf, ctx->cfg->session_margin);
	(void)fytim_surface_set_bottom(sf, fyai_chrome_text(ctx->cfg->tile_frame));
}

/* The way back, which the status row says while @sf holds the keys. */
static const char *chrome_hint(struct fyai_ctx *ctx, struct fytim_surface *sf)
{
	if (fyai_tools_btw_surface(ctx, sf))
		return "Esc closes \xc2\xb7 PgUp/PgDn scroll \xc2\xb7 Ctrl-] returns to the prompt";
	if (fyai_tools_kept_surface(ctx, sf))
		return "Esc closes \xc2\xb7 Ctrl-] returns to the prompt \xc2\xb7 Ctrl-Tab/Ctrl-T moves focus";
	return "Ctrl-] returns to the prompt \xc2\xb7 Ctrl-Tab/Ctrl-T moves focus";
}

void fyai_chrome_focus(struct fyai_ctx *ctx, struct fytim_surface *sf,
		       bool focused)
{
	const char *on, *off;
	char edge[256];
	uint32_t bg = 0;
	bool reversed;

	if (!ctx || !ctx->cfg || !sf || !ctx->ui)
		return;
	/* Without a configured ground, mark focus by reversing the margin. */
	reversed = fyai_ui_focus_ground(ctx, focused, &bg);
	(void)fytim_surface_set_bg(sf, bg, ctx->cfg->focus_bg_mix);
	if (reversed && markdown_reverse_pair(ctx->cfg, &on, &off))
		(void)fytim_surface_set_margin(sf,
				fy_sprintfa("%s%s%s", on,
					    ctx->cfg->session_margin, off));
	else
		(void)fytim_surface_set_margin(sf, focused ?
				fyai_ui_focus_margin(ctx, edge, sizeof(edge)) :
				ctx->cfg->session_margin);
	(void)fytim_surface_set_bottom(sf, fyai_chrome_text(ctx->cfg->tile_frame));
	fyai_ui_set_hint(ctx, focused ? chrome_hint(ctx, sf) : NULL);
}

int fyai_chrome_panel(struct fyai_ctx *ctx, struct response_buffer *out,
		      int *colsp, int *buttonp)
{
	/* Each kind takes a role of the palette theme, else a style of the
	 * theme of the chrome. */
	static const struct {
		const char *glyph, *fallback, *role;
		enum fymd_style_element element;
	} kinds[] = {
		{ "panel.user", "!", "tile.sigil.work", FYMD_STYLE_HEADING },
		{ "tile.shell", "$", "tile.sigil.view", FYMD_STYLE_STRONG },
		{ "tile.agent", "@", "tile.state.asks",
		  FYMD_STYLE_INDICATOR_PENDING },
	};
	const char *glyph, *on, *off;
	int counts[3], i, n, cols, rc = 0;
	bool hidden, color;

	fyai_tools_counts(ctx, &counts[0], &counts[1], &counts[2]);
	hidden = fyai_workpane_hidden(ctx->workpane);
	if (!hidden && !counts[0] && !counts[1] && !counts[2])
		return 0;
	color = markdown_color_enabled(ctx->cfg->color);
	/* The button: a full box while the pane shows, an empty one while it
	 * is hidden. */
	glyph = hidden ? markdown_glyph(ctx->cfg, "panel.hidden", "\xe2\x96\xa2") :
			 markdown_glyph(ctx->cfg, "panel.shown", "\xe2\x96\xa3");
	on = off = NULL;
	if (color)
		fyai_ui_theme_pair(ctx, "text", FYMD_STYLE_STRONG, &on, &off);
	rc = response_buffer_append(out, fy_sprintfa("%s%s%s", on ? on : "",
			glyph, off ? off : ""));
	n = fymd_str_width(glyph, strlen(glyph));
	cols = n > 0 ? n : 1;
	*buttonp = cols;
	for (i = 0; i < 3 && !rc; i++) {
		if (!counts[i])
			continue;
		glyph = markdown_glyph(ctx->cfg, kinds[i].glyph,
				       kinds[i].fallback);
		on = off = NULL;
		if (color)
			fyai_ui_theme_pair(ctx, kinds[i].role, kinds[i].element,
					   &on, &off);
		rc = response_buffer_append(out, fy_sprintfa(" %s%s%d%s",
				on ? on : "", glyph, counts[i], off ? off : ""));
		n = fymd_str_width(glyph, strlen(glyph));
		cols += 1 + (n > 0 ? n : 1) +
			(int)strlen(fy_sprintfa("%d", counts[i]));
	}
	if (rc) {
		free(out->data);
		memset(out, 0, sizeof(*out));
		return -1;
	}
	*colsp = cols;
	return 1;
}

int fyai_chrome_cap_source(const struct fyai_ctx *ctx,
			   const struct fyai_chrome_cap *cap, char *buf,
			   size_t size)
{
	const char *rule;
	char hidden[32];

	hidden[0] = '\0';
	if (cap->hidden)
		snprintf(hidden, sizeof(hidden), " \u00b7 +%d hidden",
			 cap->hidden);
	rule = markdown_glyph(ctx->cfg, "md.rule", "\u2500");
	return snprintf(buf, size,
			"<fy-role name=\"chrome\">%s%swork%s%s</fy-role> "
			"%s%s \u00b7 %d %s \u00b7 %d shown%s "
			"<fy-role name=\"chrome\"><fy-fill char=\"%s\"/></fy-role>"
			" ^T focus \u00b7 ^] prompt\n",
			rule, rule, rule, rule, cap->height,
			cap->zoomed ? " zoomed" : "", cap->tiles,
			cap->tiles == 1 ? "tile" : "tiles",
			cap->tiles - cap->hidden, hidden, rule);
}
