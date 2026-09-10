/* SPDX-License-Identifier: MIT */
#define FYAI_MODULE FYAIEM_UNKNOWN

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "fyai_browser.h"
#include "fyai_branch.h"
#include "fyai_agents.h"
#include "fyai_display.h"
#include "fyai_event.h"
#include "fyai_merge.h"
#include "fyai_render.h"
#include "fyai_session.h"
#include "fyai_sink.h"
#include "fyai_markdown.h"
#include "fyai_terminal.h"
#include "fyai_storage.h"
#include "fyai_tools.h"
#include "fyai_ui.h"
#include "fyai_workpane.h"

struct browser_row {
	char *name;
	bool branch;
	bool collapsed;
};

struct fyai_browser {
	struct fyai_ctx *ctx;
	struct fytim_surface *surface;
	struct browser_row *rows;
	size_t count, selected, top, offset;
	fy_generic root, branches;
	int64_t refreshed;
	unsigned long generation;
	bool dirty, closing, filtering;
	unsigned int view, configured_view;
	char filter[256], input[1024], target[FYAI_BRANCH_NAME_MAX + 1];
	char reference[1024];
	char action, pending;
	unsigned int field;
	char escape[16];
	size_t escape_len;
	char *details;
	char *message;
	/* The diagram of the last paint, the width it was drawn at, and the
	 * rows it drew, in the order it drew them. A move is answered against
	 * that drawing, so it must be the one on the screen. */
	char *diagram;
	int diagram_cols;
	/* The fit of the last paint: the largest row count the pane held and
	 * the smallest it did not, for the geometry they were measured in. */
	size_t fit_lo, fit_hi;
	int fit_rows, fit_cols, fit_avail;
	unsigned int fit_view;
	int pan;		/* the first row of the drawing on the pane */
	size_t *drawn, drawn_count;
	char *draft;
	char *preview;
	char preview_branch[FYAI_BRANCH_NAME_MAX + 1];
	unsigned long preview_generation;
	fy_generic preview_root;
	int preview_rows, preview_cols;
	enum fyai_sink_split preview_split;
	unsigned int preview_mode, configured_preview;
};

/* Cycled by p; index into the branch_preview enum order. */
static const char *const browser_preview_names[] = { "off", "right", "bottom", "auto" };

static int browser_present(struct fyai_browser *b, const char *md,
			   const char *diagram, const char *selection,
			   int pan, size_t offset);

static unsigned int browser_preview_mode(const char *name)
{
	unsigned int i;

	for (i = 0; i < sizeof(browser_preview_names) / sizeof(*browser_preview_names); i++)
		if (name && !strcmp(name, browser_preview_names[i]))
			return i;
	return 3;
}

static void browser_message(struct fyai_browser *b, const char *message)
{
	free(b->message);
	b->message = strdup(message);
	if (!b->message)
		fyai_warning(b->ctx, "cannot show \"%s\" in the browser", message);
	b->dirty = true;
}

static int browser_compare(const void *a, const void *b)
{
	const struct browser_row *ra = a, *rb = b;
	const unsigned char *pa = (const unsigned char *)ra->name;
	const unsigned char *pb = (const unsigned char *)rb->name;

	while (*pa && *pa == *pb) {
		pa++;
		pb++;
	}
	if (!*pa || !*pb)
		return (int)*pa - (int)*pb;
	if (*pa == '/' || *pb == '/')
		return *pa == '/' ? -1 : 1;
	return (int)*pa - (int)*pb;
}

/*
 * The browser draws a page. A failure here costs rows on that page and not
 * the turn, so it is reported as a warning and the page is drawn without
 * them.
 */
#define browser_warn_check(_b, _cond, _label, _fmt, ...) \
	do { \
		if (!(_cond)) { \
			fyai_warning((_b)->ctx, (_fmt) , ## __VA_ARGS__); \
			goto _label; \
		} \
	} while (0)

static int browser_add(struct fyai_browser *b, const char *name, bool branch)
{
	struct browser_row *rows;
	size_t i;
	char *copy;

	for (i = 0; i < b->count; i++) {
		if (strcmp(b->rows[i].name, name))
			continue;
		b->rows[i].branch |= branch;
		return 0;
	}
	copy = strdup(name);
	if (!copy)
		return -1;
	rows = realloc(b->rows, (b->count + 1) * sizeof(*rows));
	if (!rows) {
		free(copy);
		return -1;
	}
	b->rows = rows;
	rows[b->count++] = (struct browser_row){ .name = copy, .branch = branch };
	return 0;
}

static void browser_refresh(struct fyai_browser *b)
{
	struct browser_row *old = b->rows;
	size_t old_count = b->count, old_selected = b->selected, i, j;
	fy_generic root, name, entry;
	fy_generic live;
	struct fy_generic_builder_cfg config = {
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED,
	};
	struct fy_generic_builder *gb;
	char *copy, *p;
	const char *selected;

	root = fyai_branches_snapshot(b->ctx);
	b->refreshed = fyai_event_now_ms();
	if (fy_is_valid(root) && root.v == b->root.v &&
	    !b->ctx->cfg->transient && b->generation == fyai_agents_generation(b->ctx))
		return;
	b->generation = fyai_agents_generation(b->ctx);
	b->root = root;
	b->branches = fy_is_valid(root) && !b->ctx->cfg->transient ?
		fy_get(root, "branches", fy_invalid) : b->ctx->arena_branches;
	selected = old_selected < old_count ? old[old_selected].name :
		fyai_ctx_branch(b->ctx);
	b->rows = NULL;
	b->count = b->selected = 0;
	fy_foreach_key_value(name, entry, b->branches) {
		(void)entry;
		copy = strdup(fy_castp(&name, ""));
		if (!copy)
			continue;
		(void)browser_add(b, copy, true);
		for (p = copy; (p = strchr(p, '/')); p++) {
			*p = '\0';
			(void)browser_add(b, copy, false);
			*p = '/';
		}
		free(copy);
	}
	gb = fy_generic_builder_create(&config);
	if (gb) {
		live = fyai_agents_rows(b->ctx, gb);
		fy_foreach(entry, live) {
			name = fy_get(entry, "branch", fy_invalid);
			(void)browser_add(b, fy_castp(&name, ""), true);
		}
		fy_generic_builder_destroy(gb);
	}
	qsort(b->rows, b->count, sizeof(*b->rows), browser_compare);
	for (i = 0; i < b->count; i++) {
		if (!strcmp(b->rows[i].name, selected))
			b->selected = i;
		for (j = 0; j < old_count; j++)
			if (!strcmp(b->rows[i].name, old[j].name))
				b->rows[i].collapsed = old[j].collapsed;
	}
	for (i = 0; i < old_count; i++)
		free(old[i].name);
	free(old);
	b->dirty = true;
}

static bool browser_visible(struct fyai_browser *b, size_t n)
{
	size_t i;

	if (*b->filter)
		return strstr(b->rows[n].name, b->filter) != NULL;
	for (i = 0; i < n; i++)
		if (b->rows[i].collapsed &&
		    fyai_branch_is_below(b->rows[n].name, b->rows[i].name))
			return false;
	return true;
}

static void browser_move(struct fyai_browser *b, int delta)
{
	size_t i = b->selected;

	while (delta && b->count) {
		if ((delta < 0 && !i) || (delta > 0 && i + 1 >= b->count))
			break;
		i = delta < 0 ? i - 1 : i + 1;
		if (browser_visible(b, i)) {
			b->selected = i;
			delta += delta < 0 ? 1 : -1;
		}
	}
	b->dirty = true;
}

/* Move the selection by where the renderer drew the diagram, so a move reads
 * the way the drawing looks. Returns false when the move arrives nowhere:
 * there is no drawing, or the destination is off the page the drawing holds.
 * The caller then moves through the rows, which also scrolls the page. */
static bool browser_navigate(struct fyai_browser *b, enum fyai_diagram_move dir)
{
	char from[32];
	char *to;
	size_t i;
	long n;

	if (!b->diagram || !b->drawn_count || b->details)
		return false;
	for (i = 0; i < b->drawn_count && b->drawn[i] != b->selected; i++)
		;
	if (i >= b->drawn_count)
		return false;
	snprintf(from, sizeof(from), "%s/%zu",
		 b->view == 1 ? "commits" : "entries", i);
	to = fyai_sink_diagram_navigate(b->ctx->cfg, b->diagram, from, dir,
					b->diagram_cols);
	if (!to)
		return false;
	n = strtol(strrchr(to, '/') + 1, NULL, 10);
	free(to);
	if (n < 0 || (size_t)n >= b->drawn_count)
		return false;
	b->selected = b->drawn[n];
	b->dirty = true;
	return true;
}

static const char *browser_form(struct fyai_browser *b)
{
	switch (b->action) {
	case 'n': case 'N':
		return b->field ? "Start reference (empty uses current branch):" :
			"New branch name:";
	case 'r': return b->field ? "Rename branch and descendants? Type yes:" :
		"New branch name (descendants move with it):";
	case 'e': return "Description (empty clears it):";
	case 'x': return b->field ? "Reset current branch? Type yes:" : "Reset current branch to reference:";
	case 'd': return "Delete this branch and reparent descendants? Type yes:";
	case 'm': return "Merge selected branch into current branch? Type yes:";
	case 'b': return "Rebase current branch onto selected branch? Type yes:";
	default: return "";
	}
}

char *fyai_browser_gitgraph_source(fy_generic rows)
{
	struct fy_generic_builder_cfg gbcfg = { .flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED };
	struct fy_generic_builder *gb;
	fy_generic config, row, name, label;
	const char *encoded, *child, *parent;
	char *source = NULL;
	FILE *fp;
	size_t len = 0, i, j, count = fy_len(rows);
	long *parents;

	if (!count)
		return strdup("gitGraph\n");
	gb = fy_generic_builder_create(&gbcfg);
	parents = malloc(count * sizeof(*parents));
	fp = open_memstream(&source, &len);
	if (!gb || !parents || !fp)
		goto out;	/* The caller draws the list instead. */
	config = fy_mapping(gb, "gitGraph", fy_mapping(gb,
		"mainBranchName", "b0", "showBranches", false));
	encoded = emit_json_string(gb, config);
	if (!encoded)
		goto out;
	fprintf(fp, "%%%%{init: %s}%%%%\ngitGraph\n", encoded);
	for (i = 0; i < count; i++) {
		row = fy_get(rows, i);
		child = fy_get(row, "name", "");
		parents[i] = -1;
		for (j = 0; j < i; j++) {
			parent = fy_get(fy_get(rows, j), "name", "");
			if (fyai_branch_is_below(child, parent))
				parents[i] = (long)j;
		}
		/* Root lanes start before any marker, so they have no shared parent. */
		if (i && parents[i] < 0)
			fprintf(fp, "branch b%zu\n", i);
	}
	for (i = 0; i < count; i++) {
		if (parents[i] >= 0)
			fprintf(fp, "checkout b%ld\nbranch b%zu\n", parents[i], i);
		else
			fprintf(fp, "checkout b%zu\n", i);
		row = fy_get(rows, i);
		name = fy_get(row, "name", fy_invalid);
		label = fy_stringf(gb, "%s%s%s%s%s%s%s", fy_castp(&name, ""),
			fy_get(row, "current", false) ? " [current]" : "",
			fy_get(row, "head", false) ? " [HEAD]" : "",
			fy_get(row, "collapsed", false) ? " [+]" : "",
			fy_get(row, "group", false) ? " [group]" : "",
			*fy_get(row, "state", "") ? " - " : "",
			fy_get(row, "state", ""));
		encoded = emit_json_string(gb, label);
		if (!encoded)
			goto out;
		fprintf(fp, "commit id: %s\n", encoded);
	}
out:
	if (fp)
		fclose(fp);
	free(parents);
	fy_generic_builder_destroy(gb);
	return source;
}

static enum fyai_sink_split browser_split(const struct fyai_browser *b);

/* One built page: the list, the diagram source drawn in its place, and how
 * much of the view it holds. */
struct browser_page {
	char *md;
	size_t header_len;
	char *tree;		/* the diagram source, or NULL for the list */
	long selected;		/* the ordinal of the selection, or -1 */
	size_t drawn;		/* the visible rows the page holds */
	size_t more;		/* the visible rows it left out */
	int rows;		/* the rows the drawing takes */
	bool fallback;		/* a name the diagram cannot carry */
};

static void browser_page_free(struct browser_page *p)
{
	free(p->md);
	free(p->tree);
	memset(p, 0, sizeof(*p));
}

/* The grid the page itself is given, which is what a diagram must fit in. The
 * aside probe stands for the preview this pass is about to build. */
static void browser_pane(struct fyai_browser *b, int *rowsp, int *colsp)
{
	struct fyai_sink_page probe = {
		.markdown = "",
		.aside = "?",
		.split = browser_split(b),
		.extent = b->ctx->cfg->branch_preview_size,
	};
	int rows = fyai_ui_surface_granted_rows(b->surface);
	int cols = fyai_ui_surface_granted_cols(b->surface);

	*rowsp = fyai_sink_page_rows(&probe, rows, cols);
	*colsp = fyai_sink_page_cols(&probe, rows, cols);
}

/* Build the page from at most @limit visible rows. The map from an element
 * ordinal to a row is kept on @b, because a move is answered against the page
 * that is presented. */
static void browser_build(struct fyai_browser *b, size_t limit,
			  struct browser_page *p)
{
	struct fyai_branch branch;
	struct fy_generic_builder_cfg gbcfg = { .flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED };
	struct fy_generic_builder *gb = NULL;
	fy_generic graph_rows = fy_invalid;
	char *name, *model, *description, *input;
	const char *leaf, *head, *state;
	FILE *fp, *diagram = NULL;
	size_t len = 0, tree_len = 0, i, j, shown, total, depth;
	bool child;

	memset(p, 0, sizeof(*p));
	p->selected = -1;
	fp = open_memstream(&p->md, &len);
	if (!fp)
		return;
	if (b->selected < b->top)
		b->top = b->selected;
	shown = 0;
	for (i = b->top; i < b->selected; i++)
		shown += browser_visible(b, i);
	while (shown >= limit && b->top < b->selected)
		shown -= browser_visible(b, b->top++);
	for (i = b->top, total = 0; i < b->count; i++)
		total += browser_visible(b, i);
	input = fyai_prompt_literal(b->filter);
	fprintf(fp, "**Branches** · %s · / filter%s%s",
		b->view == 0 ? "tree" : b->view == 1 ? "gitgraph overview" : "list",
		*b->filter || b->filtering ? ": " : "", input ? input : "");
	free(input);
	/* The page states what it left out, so a view that holds a part of a
	 * long list does not read as the whole of it. */
	p->more = total > limit ? total - limit : 0;
	if (p->more)
		fprintf(fp, " · %zu more", p->more);
	fprintf(fp, "  \n%s  \n", fyai_ui_surface_granted_cols(b->surface) < 50 ?
		"Enter switch · i info" :
		"Enter switch · i info · g view · p preview · a actions · Esc close");
	if (b->view != 2 && b->count) {
		free(b->drawn);
		b->drawn = malloc(b->count * sizeof(*b->drawn));
		b->drawn_count = 0;
		diagram = open_memstream(&p->tree, &tree_len);
		if (diagram)
			fprintf(diagram, "treeView-beta\n");
		if (b->view == 1) {
			gb = fy_generic_builder_create(&gbcfg);
			if (gb)
				graph_rows = fy_gb_sequence(gb);
		}
	}
	if (b->message) {
		input = fyai_prompt_literal(b->message);
		fprintf(fp, "%s  \n", input ? input : "");
		free(input);
	}
	fflush(fp);
	p->header_len = len;
	head = fy_get(b->root, "HEAD", "");
	for (i = b->top, shown = 0; i < b->count && shown < limit; i++) {
		if (!browser_visible(b, i))
			continue;
		/* The renderer names an element by its ordinal in the model,
		 * which is the order the rows are emitted in. */
		if (i == b->selected)
			p->selected = (long)shown;
		if (b->drawn)
			b->drawn[shown] = i;
		shown++;
		if (gb)
			graph_rows = fy_append(gb, graph_rows, fy_gb_mapping(gb,
				"name", b->rows[i].name, "group", !b->rows[i].branch,
				"current", !strcmp(b->rows[i].name, fyai_ctx_branch(b->ctx)),
				"head", !strcmp(b->rows[i].name, head),
				"collapsed", b->rows[i].collapsed,
				"state", fyai_agents_state(b->ctx, b->rows[i].name) ?
					fyai_agents_state(b->ctx, b->rows[i].name) : ""));
		name = fyai_prompt_literal(b->rows[i].name);
		child = i + 1 < b->count &&
			fyai_branch_is_below(b->rows[i + 1].name, b->rows[i].name);
		(void)fyai_branch_lookup(b->branches, b->rows[i].name, &branch);
		state = fyai_agents_state(b->ctx, b->rows[i].name);
		model = fyai_prompt_literal(fy_get(branch.config, "model", ""));
		description = fyai_prompt_literal(fy_get(branch.entry, "description", ""));
		fprintf(fp, "%s%s %s%s%s%s%s%s%s%s  \n",
			i == b->selected ? "**> " : "  ",
			child ? (b->rows[i].collapsed ? "+" : "−") : "·",
			name ? name : "", i == b->selected ? "**" : "",
			!strcmp(b->rows[i].name, fyai_ctx_branch(b->ctx)) ? " [current]" : "",
			!strcmp(b->rows[i].name, head) ? " [HEAD]" : "",
			!b->rows[i].branch ? " [group]" :
			state ? state : fy_is_valid(branch.agent) ? " [agent · stored]" : "",
			model && *model ? " · " : "", model ? model : "",
			description && *description ? description : "");
		if (diagram) {
			depth = 0;
			for (j = b->top; j < i; j++)
				depth += browser_visible(b, j) &&
					fyai_branch_is_below(b->rows[i].name, b->rows[j].name);
			leaf = strrchr(b->rows[i].name, '/');
			/* treeView has no literal-label escape for these delimiters. */
			if (strstr(b->rows[i].name, "##") || strstr(b->rows[i].name, "%%") ||
			    strpbrk(b->rows[i].name, "\"<>")) {
				fclose(diagram);
				diagram = NULL;
				p->fallback = true;
				free(name);
				free(model);
				free(description);
				continue;
			}
			fprintf(diagram, "%*s- %s%s%s%s%s\n", (int)(depth * 2), "",
				leaf ? leaf + 1 : b->rows[i].name,
				child && b->rows[i].collapsed ? " [+]" : "",
				!strcmp(b->rows[i].name, fyai_ctx_branch(b->ctx)) ? " [current]" : "",
				!strcmp(b->rows[i].name, head) ? " [HEAD]" : "",
				!b->rows[i].branch ? " [group]" : state ? state :
				fy_is_valid(branch.agent) ? " [agent]" : "");
		}
		free(name);
		free(model);
		free(description);
	}
	p->drawn = shown;
	b->drawn_count = b->drawn ? shown : 0;
	if (!shown) {
		fprintf(fp, "No matching branches.  \n");
		if (diagram)
			fprintf(diagram, "No matching branches.\n");
	}
	fclose(fp);
	if (diagram) {
		fclose(diagram);
		if (b->view == 1) {
			free(p->tree);
			p->tree = gb ? fyai_browser_gitgraph_source(graph_rows) : NULL;
		}
	} else {
		free(p->tree);
		p->tree = NULL;
	}
	fy_generic_builder_destroy(gb);
}

/* The panes of rows a page holds. The viewport pans on the window; a window
 * of one pane is the page that fits, which never pans. */
#define FYAI_BROWSER_WINDOW 4

/* Fit the diagram to the pane. The rows one row of the view costs are an
 * estimate: the fit policy can add a legend the estimate does not carry, and a
 * drawing short of the pane leaves rows nobody uses. The measure is the
 * render, so take it and settle on the largest page the pane holds. */
static void browser_fit(struct fyai_browser *b, struct browser_page *p)
{
	size_t limit, cur, lo = 0, hi;
	char *header;
	int rows, cols, avail, budget, used, attempt;
	bool fits = false, legend = false;

	browser_pane(b, &rows, &cols);
	/* The header stands above the drawing, and it wraps, so the rows it
	 * takes are measured and not assumed. */
	avail = rows > 4 ? rows - 4 : 1;
	/* The drawing is a window over the view, and the pane is a viewport
	 * that pans on it, so the page holds several panes of rows. */
	budget = avail * FYAI_BROWSER_WINDOW;
	limit = (size_t)budget;
	if (b->view == 1)
		limit = budget > 3 ? (size_t)budget / 3 : 1;
	/* What the pane held on the last paint is the better first guess: the
	 * view moves by one row at a time. A geometry that changes drops it. */
	if (b->fit_rows != rows || b->fit_cols != cols || b->fit_view != b->view) {
		b->fit_lo = b->fit_hi = 0;
		b->pan = 0;
		b->fit_rows = rows;
		b->fit_cols = cols;
		b->fit_view = b->view;
	}
	hi = b->fit_hi;
	if (b->fit_lo)
		limit = b->fit_lo;
	for (attempt = 0; attempt < 4; attempt++) {
		if (attempt)
			browser_page_free(p);
		browser_build(b, limit, p);
		cur = limit;
		if (p->md) {
			header = strndup(p->md, p->header_len);
			used = header ? fyai_sink_markdown_measure(b->ctx->sink,
								   header, cols) : -1;
			free(header);
			if (used > 0 && used < rows) {
				avail = rows - used;
				budget = avail * FYAI_BROWSER_WINDOW;
			}
		}
		/* A list view and a source that cannot be measured are
		 * presented as they were built. */
		if (!p->tree || !p->drawn ||
		    fyai_sink_diagram_measure(b->ctx->cfg, p->tree, cols, &used,
					      &legend) ||
		    used < 1)
			return;
		p->rows = used;
		/* The window grows while the drawing stays legible: a legend
		 * means the labels no longer fit beside the drawing, and the
		 * page that names its branches is worth more than the page
		 * that holds more of them. */
		fits = used <= budget && !legend;
		if (fits) {
			lo = cur;
			/* The whole view is on the page, or the page above it
			 * is known not to fit. */
			if (!p->more || (hi && hi <= cur + 1))
				goto out;
			/* The rows to spare carry this many rows of the view;
			 * between a page that fits and one that does not,
			 * halve what is left. */
			limit = hi ? cur + (hi - cur) / 2 :
				cur + 1 + (size_t)(budget - used) * p->drawn / (size_t)used;
		} else {
			hi = cur;
			if (cur <= 1)
				goto out;
			/* Rows overrun by a measurable amount; a legend says
			 * only that this many lanes are too many. */
			if (lo)
				limit = lo + (cur - lo) / 2;
			else if (used > budget)
				limit = (size_t)budget * p->drawn / (size_t)used;
			else
				limit = cur / 2;
			if (!limit)
				limit = 1;
			if (limit >= cur)
				limit = cur - 1;
		}
		if (limit == cur)
			break;
	}
	/* The last try need not be the best one: present the largest page the
	 * measure accepted. */
	if (!fits && lo && lo != cur) {
		browser_page_free(p);
		browser_build(b, lo, p);
		fits = true;
	}
out:
	b->fit_lo = fits ? lo : 0;
	b->fit_hi = hi;
	b->fit_avail = avail;
}

/* Pan the viewport so the whole selected element is on the pane. The origin
 * is kept otherwise, so a move that stays on the pane moves nothing under
 * it. */
static int browser_pan(struct fyai_browser *b, const struct browser_page *p,
		       const char *selection)
{
	int row = 0, height = 1, margin, origin = b->pan, avail = b->fit_avail;

	if (!selection || avail < 1 || p->rows <= avail ||
	    fyai_sink_diagram_locate(b->ctx->cfg, p->tree, selection,
				     b->fit_cols, &row, &height)) {
		b->pan = 0;
		return 0;
	}
	if (height < 1)
		height = 1;
	/* A row of context beyond the element, where the pane has one to
	 * give: an element on the edge of the pane reads as the end of the
	 * drawing. */
	margin = avail > height + 2 ? 1 : 0;
	if (origin > p->rows - avail)
		origin = p->rows - avail;
	if (row - margin < origin)
		origin = row - margin;
	if (row + height + margin > origin + avail)
		origin = row + height + margin - avail;
	if (origin > p->rows - avail)
		origin = p->rows - avail;
	if (origin < 0)
		origin = 0;
	b->pan = origin;
	return origin;
}

static void browser_paint(struct fyai_browser *b)
{
	struct browser_page page;
	char selection[32];
	char *md = NULL, *name, *input;
	size_t len = 0, i, shown;
	FILE *fp;
	int rc;

	if (!b->dirty || !b->surface)
		return;
	b->dirty = false;
	if (b->details && !b->action && !b->filtering) {
		rc = browser_present(b, b->details, NULL, NULL, 0, b->offset);
		browser_warn_check(b, !rc, out, "the branch details did not render");
		return;
	}
	if (b->action) {
		fp = open_memstream(&md, &len);
		browser_warn_check(b, fp, out, "cannot build the branch action form");
		name = fyai_prompt_literal(b->target);
		input = fyai_prompt_literal(b->input);
		fprintf(fp, "**%s**  \n%s  \n%s\n\nType in the prompt · Enter accepts · Escape cancels\n",
			name ? name : "", browser_form(b), input ? input : "");
		free(name);
		free(input);
		if (b->field && (b->action == 'r' || b->action == 'x')) {
			input = fyai_prompt_literal(b->reference);
			fprintf(fp, "Destination: %s  \n", input ? input : "");
			free(input);
		}
		if (b->action == 'r' || b->action == 'd') {
			shown = 0;
			for (i = 0; i < b->count; i++) {
				if (!strcmp(b->rows[i].name, b->target) ||
				    !fyai_branch_is_below(b->rows[i].name, b->target))
					continue;
				if (shown++ >= 3)
					continue;
				name = fyai_prompt_literal(b->rows[i].name);
				fprintf(fp, "Affected descendant: %s  \n", name ? name : "");
				free(name);
			}
			if (shown > 3)
				fprintf(fp, "%zu more descendants are affected.\n", shown - 3);
		}
		if (b->message) {
			input = fyai_prompt_literal(b->message);
			fprintf(fp, "\n%s\n", input ? input : "");
			free(input);
		}
		fclose(fp);
		rc = md ? browser_present(b, md, NULL, NULL, 0, 0) : -1;
		free(md);
		browser_warn_check(b, !rc, out, "the branch action form did not render");
		return;
	}
	browser_fit(b, &page);
	if (!page.md) {
		browser_page_free(&page);
		browser_warn_check(b, false, out, "cannot build the branch page");
		return;
	}
	if (page.tree) {
		snprintf(selection, sizeof(selection), "%s/%ld",
			 b->view == 1 ? "commits" : "entries", page.selected);
		input = strndup(page.md, page.header_len);
		rc = input ? browser_present(b, input, page.tree,
					     page.selected >= 0 ? selection : NULL,
					     browser_pan(b, &page,
							 page.selected >= 0 ? selection : NULL),
					     0) : -1;
		free(input);
		if (rc) {
			/* The drawing did not render; the list says the same
			 * thing and is what the reader gets instead. */
			/* fy_sprintfa() is stack storage: it lives to the end
			 * of this frame and is not freed. */
			input = fy_sprintfa("Diagram unavailable; showing list.\n\n%s", page.md);
			rc = browser_present(b, input, NULL, NULL, 0, 0);
		}
	} else if (page.fallback) {
		input = fy_sprintfa("Literal branch labels require the list view.\n\n%s", page.md);
		rc = browser_present(b, input, NULL, NULL, 0, 0);
	} else
		rc = browser_present(b, page.md, NULL, NULL, 0, 0);
	browser_page_free(&page);
	browser_warn_check(b, !rc, out, "the branch page did not render");
out:
	return;
}


static void browser_grant(void *owner, int rows, int cols)
{
	struct fyai_browser *b = owner;

	(void)fyai_ui_surface_resize(b->surface, rows, cols);
	fyai_workpane_grid_resized(b->ctx->workpane, b->surface, rows, cols);
	b->dirty = true;
	browser_paint(b);
}

static const struct fyai_workpane_tile_ops browser_ops = {
	.apply_grant = browser_grant,
};

bool fyai_browser_surface(struct fyai_ctx *ctx, const struct fytim_surface *sf)
{
	return ctx && ctx->browser && ctx->browser->surface == sf;
}

int fyai_browser_open(struct fyai_ctx *ctx)
{
	struct fyai_browser *b;
	int rc;

	if (!fyai_ui_active(ctx))
		return fyai_branch_list(ctx, NULL, true);
	b = ctx->browser;
	if (!b) {
		b = calloc(1, sizeof(*b));
		if (!b)
			return -1;
		b->ctx = ctx;
		b->view = ctx->cfg->branch_view && !strcmp(ctx->cfg->branch_view, "list") ? 2 :
			ctx->cfg->branch_view && !strcmp(ctx->cfg->branch_view, "gitgraph") ? 1 : 0;
		b->configured_view = b->view;
		b->preview_mode = browser_preview_mode(ctx->cfg->branch_preview);
		b->configured_preview = b->preview_mode;
		b->root = b->branches = fy_invalid;
		b->preview_root = fy_invalid;
		ctx->browser = b;
		b->surface = fyai_ui_surface_open(ctx, 16, 80);
		if (!b->surface)
			goto fail;
		rc = fyai_workpane_register(ctx->workpane, b->surface,
			FYAI_WORKPANE_TILE_BROWSER, b, &browser_ops, 16, 0);
		if (rc)
			goto fail;
		browser_refresh(b);
	}
	fyai_tools_focus_next(ctx);
	fyai_workpane_set_focus(ctx->workpane, b->surface);
	fyai_ui_wake(ctx);
	return 0;
fail:
	fyai_browser_close(ctx);
	return -1;
}

void fyai_browser_close(struct fyai_ctx *ctx)
{
	struct fyai_browser *b = ctx ? ctx->browser : NULL;
	size_t i;

	if (!b)
		return;
	fyai_ui_surface_close(ctx, b->surface);
	for (i = 0; i < b->count; i++)
		free(b->rows[i].name);
	free(b->rows);
	free(b->drawn);
	free(b->diagram);
	free(b->details);
	free(b->message);
	free(b->preview);
	if (b->draft)
		fyai_ui_input_set(ctx, b->draft);
	free(b->draft);
	free(b);
	ctx->browser = NULL;
	fyai_ui_wake(ctx);
}

void fyai_browser_service(struct fyai_ctx *ctx)
{
	struct fyai_browser *b = ctx->browser;

	if (!b)
		return;
	if (fyai_event_now_ms() - b->refreshed >= 1000)
		browser_refresh(b);
	/* Read-only views and live controls do not start an engine loop. */
	if (b->pending && strchr("itlczAK", b->pending))
		fyai_browser_step(ctx);
	browser_paint(b);
}

static void browser_accept(struct fyai_browser *b)
{
	if ((b->action == 'x' || b->action == 'r') && !b->field) {
		memcpy(b->reference, b->input, strlen(b->input) + 1);
		b->input[0] = '\0';
		b->field = 1;
	} else if ((b->action == 'n' || b->action == 'N') && !b->field) {
		if (strlen(b->input) >= sizeof(b->target) || !fyai_branch_name_valid(b->input)) {
			browser_message(b, "Enter a valid branch name.");
			return;
		}
		memcpy(b->target, b->input, strlen(b->input) + 1);
		b->input[0] = '\0';
		b->field = 1;
	} else if ((b->action == 'd' || b->action == 'm' || b->action == 'b' ||
		    b->action == 'x' || b->action == 'r') &&
		   strcmp(b->input, "yes")) {
		browser_message(b, "Type yes to confirm, or Escape to cancel.");
	} else {
		b->pending = b->action;
		b->action = 0;
	}
}

static void browser_edit(struct fyai_browser *b)
{
	free(b->draft);
	b->draft = fyai_ui_input_copy(b->ctx);
	fyai_workpane_clear_focus(b->ctx->workpane);
	fyai_ui_input_set(b->ctx, b->filtering ? b->filter : "");
}

static void browser_edit_end(struct fyai_browser *b)
{
	fyai_ui_input_set(b->ctx, b->draft);
	free(b->draft);
	b->draft = NULL;
	fyai_workpane_set_focus(b->ctx->workpane, b->surface);
	b->dirty = true;
	fyai_ui_wake(b->ctx);
}

bool fyai_browser_cancel_input(struct fyai_ctx *ctx)
{
	struct fyai_browser *b = ctx->browser;

	if (!b || (!b->action && !b->filtering))
		return false;
	b->action = 0;
	b->filtering = false;
	browser_edit_end(b);
	return true;
}

bool fyai_browser_input(struct fyai_ctx *ctx, const char *line)
{
	struct fyai_browser *b = ctx->browser;
	size_t len = strlen(line);

	if (!b || (!b->action && !b->filtering))
		return false;
	if (len >= (b->filtering ? sizeof(b->filter) : sizeof(b->input))) {
		browser_message(b, "The value is too long. Enter a shorter value.");
		fyai_ui_input_set(ctx, line);
		return true;
	}
	if (b->filtering) {
		memcpy(b->filter, line, len + 1);
		b->filtering = false;
		b->selected = b->top = 0;
		if (b->count && !browser_visible(b, 0))
			browser_move(b, 1);
	} else {
		memcpy(b->input, line, len + 1);
		browser_accept(b);
	}
	if (!b->action)
		browser_edit_end(b);
	else
		fyai_ui_input_set(ctx, "");
	b->dirty = true;
	fyai_ui_wake(ctx);
	return true;
}

static void browser_key(struct fyai_browser *b, unsigned char c)
{
	b->dirty = true;
	if (c == 27 || c == 3) {
		if (b->action)
			b->action = 0;
		else if (b->filtering)
			b->filtering = false;
		else if (b->details) {
			free(b->details);
			b->details = NULL;
		} else
			b->closing = true;
		return;
	}
	if (c == 'j' || c == 'k') {
		if (b->details) {
			if (c == 'j') b->offset++;
			else if (b->offset) b->offset--;
		} else if (!browser_navigate(b, c == 'j' ? FYAI_DIAGRAM_DOWN :
						FYAI_DIAGRAM_UP))
			browser_move(b, c == 'j' ? 1 : -1);
		return;
	}
	if (c == '/') {
		b->filtering = true;
		browser_edit(b);
		return;
	}
	if (c == 'a') {
		free(b->details);
		b->details = strdup("**Branch actions**\n\n"
			"Enter switch and close · i info · s switch  \n"
			"n create · N create and switch  \n"
			"r rename · d delete · e describe  \n"
			"m merge · b rebase · x reset current branch  \n"
			"t transcript · l reflog · c configuration  \n"
			"z zoom · A attach · K stop live agent  \n"
			"p session preview · R refresh  \n"
			"Escape returns to the branch tree.\n");
		b->offset = 0;
		return;
	}
	if (c == 'g') {
		b->view = (b->view + 1) % 3;
		return;
	}
	if (c == 'p') {
		b->preview_mode = (b->preview_mode + 1) %
			(sizeof(browser_preview_names) / sizeof(*browser_preview_names));
		browser_message(b, browser_preview_names[b->preview_mode][0] == 'o' ?
			"Session preview off." :
			b->preview_mode == 1 ? "Session preview at the right." :
			b->preview_mode == 2 ? "Session preview at the foot." :
			"Session preview follows the pane.");
		return;
	}
	if (c == 'R') {
		b->root = fy_invalid;
		browser_refresh(b);
		return;
	}
	if (!b->count || !browser_visible(b, b->selected))
		return;
	if (c == 'h' || c == 'v') {
		b->rows[b->selected].collapsed = c == 'h';
		return;
	}
	if (!b->rows[b->selected].branch && c != 'n' && c != 'N')
		return;
	if (strchr("nNredmbx", c)) {
		b->action = c;
		b->field = 0;
		b->input[0] = '\0';
		browser_edit(b);
	} else if (c == '\r' || c == '\n')
		b->pending = 'S';
	else if (strchr("istlczAK", c))
		b->pending = c;
	snprintf(b->target, sizeof(b->target), "%s", b->rows[b->selected].name);
}

bool fyai_browser_keys(struct fyai_ctx *ctx, const char *data, size_t len)
{
	struct fyai_browser *b = ctx->browser;
	size_t i;
	char c;

	if (!b || fyai_workpane_focused(ctx->workpane) != b->surface)
		return false;
	for (i = 0; i < len; i++) {
		c = data[i];
		if (b->escape_len || (c == 27 && i + 1 < len && data[i + 1] == '[')) {
			if (b->escape_len + 1 < sizeof(b->escape))
				b->escape[b->escape_len++] = c;
			if (b->escape_len < 3)
				continue;
			if ((c >= 'A' && c <= 'Z') || c == '~') {
				if (c == 'A') browser_key(b, 'k');
				if (c == 'B') browser_key(b, 'j');
				/* Gitgraph lanes run across the pane, so the
				 * lateral keys move on the drawing there. The
				 * tree keeps them for collapse and expand. */
				if ((c == 'D' || c == 'C') &&
				    b->view == 1 && !b->details)
					(void)browser_navigate(b, c == 'C' ?
						FYAI_DIAGRAM_RIGHT : FYAI_DIAGRAM_LEFT);
				else if (c == 'D') browser_key(b, 'h');
				else if (c == 'C') browser_key(b, 'v');
				if (c == 'H') { b->selected = b->top = b->offset = 0; }
				if (c == 'F' && b->count) b->selected = b->count - 1;
				if (c == '~') {
					browser_move(b, b->escape[2] == '5' ? -8 : 8);
					if (b->details)
						b->offset = b->escape[2] == '5' ?
							(b->offset > 8 ? b->offset - 8 : 0) : b->offset + 8;
				}
				b->escape_len = 0;
				b->dirty = true;
			}
		} else
			browser_key(b, c);
		if (b->closing) {
			fyai_browser_close(ctx);
			if (i + 1 < len)
				(void)fyai_ui_keys_return(ctx, data + i + 1, len - i - 1);
			return true;
		}
		if (b->action || b->filtering) {
			if (i + 1 < len)
				(void)fyai_ui_keys_return(ctx, data + i + 1, len - i - 1);
			break;
		}
	}
	fyai_ui_wake(ctx);
	return true;
}

/* Render a read-only view of one branch through a capture sink. Returns the
 * captured Markdown source, which the caller owns. */
static char *browser_capture(struct fyai_browser *b, char *name,
			     char action, int cols, int rows)
{
	struct fyai_ctx view = *b->ctx;
	struct fyai_cfg cfg = *b->ctx->cfg;
	struct fyai_branch branch;
	struct fy_generic_builder_cfg gbcfg = {
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED,
	};
	struct fy_generic_builder *gb;
	fy_generic data;
	const char *captured;
	char *out = NULL;
	size_t len = 0;
	FILE *fp = NULL;
	int rc;

	if (!fyai_branch_lookup(b->branches, name, &branch))
		return NULL;
	gb = fy_generic_builder_create(&gbcfg);
	browser_warn_check(b, gb, none, "cannot build the branch preview of %s", name);
	view.cfg = &cfg;
	view.ui = NULL;
	view.display_output = NULL;
	view.browser = NULL;
	view.branch = name;
	view.arena_branches = b->branches;
	view.branch_prev = branch.entry;
	view.last_message = branch.head;
	view.transient_gb = gb;
	if (cols > 0)
		cfg.render_width = cols;
	/* The preview is a render, not a source: a recap mixes Markdown with
	 * rows another path already drew, and only the renderer can place
	 * both at this width. */
	if (action == 'p') {
		fp = open_memstream(&out, &len);
		browser_warn_check(b, fp, out, "cannot build the preview of %s", name);
		view.sink = fyai_sink_create_render(&view, fp);
	} else
		view.sink = fyai_sink_create_capture(&view);
	browser_warn_check(b, view.sink, out, "cannot render the preview of %s", name);
	if (action == 't')
		rc = fyai_display_recap(&view, 10, 0);
	else if (action == 'p')
		rc = fyai_display_recap(&view, -1, rows);
	else if (action == 'i')
		rc = fyai_branch_show(&view, name);
	else {
		data = action == 'l' ? fyai_list_reflog_data(&view, gb) : branch.config;
		rc = fyai_generic_to_markdown(&view, fy_invalid, data);
	}
	if (rc)
		fyai_warning(b->ctx, "the preview of %s is incomplete", name);
	if (!fp) {
		captured = fyai_sink_captured(view.sink, NULL);
		out = captured ? strdup(captured) : NULL;
		if (!out)
			fyai_warning(b->ctx, "cannot keep the preview of %s", name);
	}
	fyai_sink_destroy(view.sink);
out:
	/* The memstream must be closed before @out names its bytes. */
	if (fp && fclose(fp))
		fyai_warning(b->ctx, "the preview of %s was not completed", name);
	fy_generic_builder_destroy(gb);
	return out;
none:
	return NULL;
}

static void browser_inspect(struct fyai_browser *b, char action)
{
	char *text;

	text = browser_capture(b, b->target, action, 0, 0);
	if (!text)
		return;
	free(b->details);
	b->details = text;
	b->offset = 0;
}

static enum fyai_sink_split browser_split(const struct fyai_browser *b)
{
	switch (b->preview_mode) {
	case 1: return FYAI_SINK_SPLIT_RIGHT;
	case 2: return FYAI_SINK_SPLIT_BOTTOM;
	case 3: return FYAI_SINK_SPLIT_AUTO;
	default: return FYAI_SINK_SPLIT_NONE;
	}
}

/* Rebuild the preview when the selection, the stored state, or the pane
 * geometry it was measured at changes. The renderer is fast; the recap walk
 * over stored turns is not free. */
static void browser_preview_update(struct fyai_browser *b, int cols, int rows)
{
	char *name;

	if (browser_split(b) == FYAI_SINK_SPLIT_NONE || !b->count ||
	    b->selected >= b->count || !b->rows[b->selected].branch) {
		free(b->preview);
		b->preview = NULL;
		b->preview_branch[0] = '\0';
		return;
	}
	name = b->rows[b->selected].name;
	if (b->preview && !strcmp(b->preview_branch, name) &&
	    b->preview_generation == b->generation &&
	    b->preview_root.v == b->root.v &&
	    b->preview_cols == cols && b->preview_rows == rows)
		return;
	free(b->preview);
	b->preview = browser_capture(b, name, 'p', cols, rows);
	snprintf(b->preview_branch, sizeof(b->preview_branch), "%s", name);
	b->preview_generation = b->generation;
	b->preview_root = b->root;
	b->preview_cols = cols;
	b->preview_rows = rows;
}

/* Present the page and, when one is configured, the end of the selected
 * session beside it. */
static int browser_present(struct fyai_browser *b, const char *md,
			   const char *diagram, const char *selection,
			   int pan, size_t offset)
{
	struct fyai_sink_page page = {
		.markdown = md,
		.diagram = diagram,
		.diagram_selection = selection,
		.diagram_row = pan,
		.offset = offset,
		.split = browser_split(b),
		.extent = b->ctx->cfg->branch_preview_size,
	};
	enum fyai_sink_split split;
	int extent, cols, rows;
	int preview_width = b->ctx->cfg->branch_preview_width;
	char *heading = NULL;
	bool color = markdown_color_enabled(b->ctx->cfg->color);

	if (preview_width > 0)
		page.aside_cols = preview_width;
	else if (preview_width < 0)
		page.extent = -preview_width;

	cols = fyai_ui_surface_granted_cols(b->surface);
	rows = fyai_ui_surface_granted_rows(b->surface);
	/* Ask for the geometry the sink will use, so the preview is rendered
	 * at the width it is placed at. The probe stands for the preview this
	 * pass is about to build. */
	page.aside = "?";
	extent = fyai_sink_page_extent(&page, rows, cols, &split);
	page.aside = NULL;
	if (split != FYAI_SINK_SPLIT_NONE)
		browser_preview_update(b,
			split == FYAI_SINK_SPLIT_BOTTOM ? cols : extent,
			split == FYAI_SINK_SPLIT_BOTTOM ? extent : rows);
	if (b->preview) {
		heading = fyai_prompt_literal(b->preview_branch);
		page.aside = fy_sprintfa("%s%s%s · session end\n\n%s",
					 color ? FYAI_ANSI_BOLD : "",
					 heading ? heading : "",
					 color ? FYAI_ANSI_RESET : "",
					 b->preview);
		page.aside_rendered = true;
		page.aside_offset = FYAI_SINK_OFFSET_TAIL;
		free(heading);
	}
	/* Keep the drawing a move is answered against: the source presented
	 * and the width the sink renders it at. */
	free(b->diagram);
	b->diagram = diagram ? strdup(diagram) : NULL;
	if (b->diagram)
		b->diagram_cols = fyai_sink_page_cols(&page, rows, cols);
	else
		b->drawn_count = 0;
	return fyai_sink_page_split(b->ctx->sink, b->surface, &page);
}

void fyai_browser_step(struct fyai_ctx *ctx)
{
	struct fyai_browser *b = ctx->browser;
	char action;
	int rc = 0;
	struct fyai_branch current;
	fy_generic root;

	if (!b || !b->pending)
		return;
	action = b->pending;
	b->pending = 0;
	if (strchr("itlc", action)) {
		browser_inspect(b, action);
		goto out;
	}
	if (action == 'z') {
		if (!fyai_tools_zoom(ctx, b->target))
			browser_message(b, "This branch has no live screen.");
		goto out;
	}
	if (action == 'A') {
		if (!fyai_agents_zoom(ctx, b->target, true))
			browser_message(b, "This branch has no reachable live owner.");
		goto out;
	}
	if (action == 'K') {
		if (fyai_agents_kill(ctx, b->target))
			browser_message(b, "This branch has no reachable live owner.");
		goto out;
	}
	if (fyai_ui_busy(ctx) || fyai_tools_active(ctx) || fyai_agents_attached(ctx)) {
		browser_message(b, "Branch changes require idle model and tool work.");
		goto out;
	}
	if (ctx->cfg->root_pinned) {
		browser_message(b, "The selected root is read-only.");
		goto out;
	}
	if (!fyai_ctx_transient_gb(ctx))
		goto out;
	root = fyai_branches_snapshot(ctx);
	if (fy_is_valid(root) && action != 's' && action != 'S') {
		/* A branch that is not in the published root reads as empty,
		 * which is the comparison this wants. */
		(void)fyai_branch_lookup(fy_get(root, "branches", fy_invalid),
			fyai_ctx_branch(ctx), &current);
		if (current.entry.v != ctx->branch_prev.v) {
			browser_message(b, "The current branch changed in another invocation. Switch to it again before editing.");
			goto out;
		}
	}
	rc = fyai_branches_refresh(ctx);
	if (rc)
		goto report;
	switch (action) {
	case 's': case 'S':
		rc = fyai_session_branch_switch(ctx, b->target, false);
		break;
	case 'n': case 'N':
		rc = fyai_branch_create(ctx, b->target, *b->input ? b->input : NULL,
				NULL, false);
		if (!rc && action == 'N')
			rc = fyai_session_branch_switch(ctx, b->target, false);
		break;
	case 'r': rc = fyai_branch_rename(ctx, b->target, b->reference); break;
	case 'd': rc = fyai_branch_delete(ctx, b->target, true); break;
	case 'e': rc = fyai_branch_describe(ctx, b->target, b->input); break;
	case 'm': case 'b':
		rc = fyai_branch_join(ctx, b->target,
			action == 'm' ? FYAI_JOIN_MERGE : FYAI_JOIN_REBASE, false);
		break;
	case 'x': rc = fyai_branch_reset(ctx, b->reference); break;
	}
report:
	if (!rc) {
		free(b->details);
		b->details = NULL;
	}
	browser_message(b, rc ? "The operation failed; see the diagnostic." : "Done.");
	fyai_ui_diag_drain(ctx, "branch");
	fyai_session_banner_update(ctx);
	if (!rc && action == 'S') {
		fyai_browser_close(ctx);
		return;
	}
	if (!rc && (action == 'x' || action == 'm' || action == 'b'))
		fyai_ui_repaint(ctx);
	b->root = fy_invalid;
	browser_refresh(b);
out:
	b->dirty = true;
	fyai_ui_wake(ctx);
}

void fyai_browser_config_changed(struct fyai_ctx *ctx)
{
	struct fyai_browser *b = ctx ? ctx->browser : NULL;
	unsigned int view;

	if (!b)
		return;
	view = ctx->cfg->branch_view && !strcmp(ctx->cfg->branch_view, "list") ? 2 :
		ctx->cfg->branch_view && !strcmp(ctx->cfg->branch_view, "gitgraph") ? 1 : 0;
	if (view != b->configured_view)
		b->view = b->configured_view = view;
	view = browser_preview_mode(ctx->cfg->branch_preview);
	if (view != b->configured_preview)
		b->preview_mode = b->configured_preview = view;
	b->dirty = true;
	fyai_ui_wake(ctx);
}
