/* SPDX-License-Identifier: MIT */
#define FYAI_MODULE FYAIEM_DISPLAY

#include <stdlib.h>
#include <string.h>
#include <libfymermaid.h>
#include <libfyvterm.h>

#include "fyai.h"
#include "fyai_sink.h"
#include "fyai_browser.h"
#include "fyai_test.h"
#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(diagram, branch_topology, diagram_branch_topology)
FYAI_TEST_ENTRY(diagram, modes, diagram_modes)
FYAI_TEST_ENTRY(diagram, selection, diagram_selection)
FYAI_TEST_ENTRY(diagram, measure, diagram_measure)
FYAI_TEST_ENTRY(diagram, legend, diagram_legend)
FYAI_TEST_ENTRY(diagram, locate, diagram_locate)
FYAI_TEST_ENTRY(diagram, navigate, diagram_navigate)
FYAI_TEST_ENTRY(diagram, navigate_gitgraph, diagram_navigate_gitgraph)
FYAI_TEST_ENTRY(diagram, invalid_source, diagram_invalid_source)

static struct fyvt *diagram_screen(const char *text)
{
	struct fyvt_cfg cfg;
	struct fyvt *vt;
	const char *p, *end;
	size_t len, written;

	fyvt_cfg_default(&cfg);
	cfg.rows = 60;
	cfg.cols = 100;
	vt = fyvt_create(&cfg);
	FYAI_TCHECK(vt != NULL);
	fyvt_set_utf8(vt, 1);
	fyvt_screen_reset(fyvt_obtain_screen(vt), 1);
	for (p = text; *p; p += len + (end ? 1 : 0)) {
		end = strchr(p, '\n');
		len = end ? (size_t)(end - p) : strlen(p);
		written = fyvt_input_write(vt, p, len);
		FYAI_TCHECK(written == len);
		written = fyvt_input_write(vt, "\r\n", 2);
		FYAI_TCHECK(written == 2);
	}
	return vt;
}

static void diagram_compare(const char *actual, const char *expected, int width)
{
	struct fyvt *a = diagram_screen(actual), *b = diagram_screen(expected);
	struct fyvt_screen *sa = fyvt_obtain_screen(a), *sb = fyvt_obtain_screen(b);
	struct fyvt_screen_cell ca, cb;
	struct fyvt_pos pos;
	bool got;
	size_t glyph;

	for (pos.row = 0; pos.row < 60; pos.row++) {
		for (pos.col = 0; pos.col < 100; pos.col++) {
			got = fyvt_screen_get_cell(sa, pos, &ca);
			FYAI_TCHECK(got);
			got = fyvt_screen_get_cell(sb, pos, &cb);
			FYAI_TCHECK(got);
			for (glyph = 0; glyph < sizeof(ca.chars) / sizeof(ca.chars[0]); glyph++) {
				FYAI_TCHECK(ca.chars[glyph] == cb.chars[glyph]);
				if (!ca.chars[glyph])
					break;
			}
			FYAI_TCHECK(ca.attrs.bold == cb.attrs.bold && ca.attrs.dim == cb.attrs.dim);
			fyvt_screen_convert_color_to_rgb(sa, &ca.fg);
			fyvt_screen_convert_color_to_rgb(sb, &cb.fg);
			fyvt_screen_convert_color_to_rgb(sa, &ca.bg);
			fyvt_screen_convert_color_to_rgb(sb, &cb.bg);
			FYAI_TCHECK(fyvt_color_is_equal(&ca.fg, &cb.fg));
			FYAI_TCHECK(fyvt_color_is_equal(&ca.bg, &cb.bg));
			if (pos.col >= width)
				FYAI_TCHECK(!ca.chars[0] || ca.chars[0] == ' ');
		}
	}
	fyvt_destroy(a);
	fyvt_destroy(b);
}

int diagram_modes(void)
{
	static const char *sources[] = {
		"treeView-beta\n> main/\n  - 日本語/\n    - child\n  - sibling\n",
		"gitGraph\ncommit id: \"a-long-commit-label\"\nbranch feature\n"
		"checkout feature\ncommit id: \"another-long-label\"\n",
	};
	static const char *charsets[] = { "auto", "ascii", "unicode", "rich" };
	static const enum fymm_charset repertoires[] = {
		FYMM_CHARSET_AUTO, FYMM_CHARSET_ASCII, FYMM_CHARSET_UNICODE, FYMM_CHARSET_RICH,
	};
	static const char *fits[] = { "legend", "shrink", "clip" };
	static const enum fymm_fit policies[] = { FYMM_FIT_LEGEND, FYMM_FIT_SHRINK, FYMM_FIT_CLIP };
	struct fyai_cfg cfg = { .color = "on" };
	struct fymm_render_cfg render;
	struct fymm_diagram *diagram;
	char *actual, *expected;
	size_t source, charset, fit;
	int variant, width;

	for (source = 0; source < sizeof(sources) / sizeof(sources[0]); source++) {
		diagram = fymm_parse(sources[source], FYMM_NT, NULL);
		FYAI_TCHECK(diagram && !fymm_diagram_has_errors(diagram));
		for (charset = 0; charset < 4; charset++) {
			for (fit = 0; fit < 3; fit++) {
				for (variant = 0; variant < 3; variant++) {
					for (width = 16; width <= 80; width += 64) {
						cfg.diagram_charset = charsets[charset];
						cfg.diagram_fit = fits[fit];
						cfg.theme_variant = variant ? "light" : "dark";
						cfg.diagram_theme = variant == 2 ? "mono" : "";
						fymm_render_cfg_default(&render);
						render.color = FYMM_COLOR_TRUECOLOR;
						render.width = width;
						render.charset = repertoires[charset];
						render.fit = policies[fit];
						render.background = variant ? FYMM_BG_LIGHT : FYMM_BG_DARK;
						render.theme = variant == 2 ? "mono" : NULL;
						expected = fymm_render(diagram, &render);
						actual = fyai_sink_diagram_render(&cfg, sources[source], NULL, width);
						FYAI_TCHECK(actual && expected);
						diagram_compare(actual, expected, width);
						fymm_free(actual);
						fymm_free(expected);
					}
				}
			}
		}
		fymm_diagram_destroy(diagram);
	}
	cfg.color = "off";
	actual = fyai_sink_diagram_render(&cfg, sources[0], NULL, 80);
	FYAI_TCHECK(actual && !strchr(actual, '\033'));
	fymm_free(actual);
	return 0;
}

/* The measure answers the rows the render produces, under the fit policy the
 * render uses. */
int diagram_measure(void)
{
	struct fyai_cfg cfg = { .color = "off" };
	static const char *source = "treeView-beta\n- main\n- other\n- third\n";
	static const char *fits[] = { "legend", "shrink", "clip" };
	char *out;
	const char *p;
	int rows, drawn, i;

	for (i = 0; i < 3; i++) {
		cfg.diagram_fit = fits[i];
		FYAI_TCHECK(!fyai_sink_diagram_measure(&cfg, source, 24, &rows, NULL));
		out = fyai_sink_diagram_render(&cfg, source, NULL, 24);
		FYAI_TCHECK(out != NULL);
		for (drawn = 0, p = out; *p; p++)
			drawn += *p == '\n';
		if (p != out && p[-1] != '\n')
			drawn++;
		FYAI_TCHECK(drawn == rows);
		fymm_free(out);
	}
	FYAI_TCHECK(fyai_sink_diagram_measure(&cfg, "not-a-diagram", 24, &rows, NULL));
	return 0;
}

/* A legend says the width stopped carrying the labels, which is what sizes a
 * window of branches. */
int diagram_legend(void)
{
	struct fy_generic_builder_cfg gbcfg = { .flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED };
	struct fy_generic_builder *gb = fy_generic_builder_create(&gbcfg);
	struct fyai_cfg cfg = { .color = "off", .diagram_fit = "legend" };
	fy_generic rows;
	char *source;
	bool legend;
	int r;

	FYAI_TCHECK(gb != NULL);
	rows = fy_gb_sequence(gb,
		fy_gb_mapping(gb, "name", "main"),
		fy_gb_mapping(gb, "name", "main/one-long-branch-name"),
		fy_gb_mapping(gb, "name", "main/two-long-branch-name"),
		fy_gb_mapping(gb, "name", "main/three-long-branch-name"));
	source = fyai_browser_gitgraph_source(rows);
	FYAI_TCHECK(source != NULL);
	legend = true;
	FYAI_TCHECK(!fyai_sink_diagram_measure(&cfg, source, 200, &r, &legend));
	FYAI_TCHECK(!legend);
	FYAI_TCHECK(!fyai_sink_diagram_measure(&cfg, source, 30, &r, &legend));
	FYAI_TCHECK(legend);
	free(source);
	fy_generic_builder_destroy(gb);
	return 0;
}

/* The viewport is panned by where the selected element landed. */
int diagram_locate(void)
{
	struct fyai_cfg cfg = { .color = "off" };
	static const char *source = "treeView-beta\n- main\n- other\n- third\n";
	int first = -1, row = -1, height = -1;

	FYAI_TCHECK(!fyai_sink_diagram_locate(&cfg, source, "entries/0", 40,
					            &first, NULL));
	FYAI_TCHECK(!fyai_sink_diagram_locate(&cfg, source, "entries/2", 40,
					            &row, &height));
	FYAI_TCHECK(row == first + 2 && height == 1);
	FYAI_TCHECK(fyai_sink_diagram_locate(&cfg, source, "entries/9", 40,
					           &row, &height));
	return 0;
}

/* A move is answered by where the renderer drew the elements. */
int diagram_navigate(void)
{
	struct fyai_cfg cfg = { .color = "off" };
	static const char *tree = "treeView-beta\n- main\n- other\n- third\n";
	char *to;

	to = fyai_sink_diagram_navigate(&cfg, tree, "entries/0",
					FYAI_DIAGRAM_DOWN, 40);
	FYAI_TCHECK(to && !strcmp(to, "entries/1"));
	free(to);
	to = fyai_sink_diagram_navigate(&cfg, tree, "entries/2",
					FYAI_DIAGRAM_UP, 40);
	FYAI_TCHECK(to && !strcmp(to, "entries/1"));
	free(to);
	/* A move that leaves the drawing arrives nowhere. */
	to = fyai_sink_diagram_navigate(&cfg, tree, "entries/2",
					FYAI_DIAGRAM_DOWN, 40);
	FYAI_TCHECK(to == NULL);
	free(to);
	to = fyai_sink_diagram_navigate(&cfg, "not-a-diagram", "entries/0",
					FYAI_DIAGRAM_DOWN, 40);
	FYAI_TCHECK(to == NULL);
	return 0;
}

/* The gitgraph lanes are laid out across the pane, so a lateral move reaches
 * another branch. */
int diagram_navigate_gitgraph(void)
{
	struct fy_generic_builder_cfg gbcfg = { .flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED };
	struct fy_generic_builder *gb = fy_generic_builder_create(&gbcfg);
	struct fyai_cfg cfg = { .color = "off" };
	fy_generic rows;
	char *source, *to;

	FYAI_TCHECK(gb != NULL);
	rows = fy_gb_sequence(gb,
		fy_gb_mapping(gb, "name", "main"),
		fy_gb_mapping(gb, "name", "main/one"),
		fy_gb_mapping(gb, "name", "main/two"));
	source = fyai_browser_gitgraph_source(rows);
	FYAI_TCHECK(source != NULL);
	to = fyai_sink_diagram_navigate(&cfg, source, "commits/0",
					FYAI_DIAGRAM_DOWN, 80);
	FYAI_TCHECK(to && strncmp(to, "commits/", 8) == 0 && strcmp(to, "commits/0"));
	free(to);
	to = fyai_sink_diagram_navigate(&cfg, source, "commits/0",
					FYAI_DIAGRAM_UP, 80);
	FYAI_TCHECK(to == NULL);
	free(source);
	fy_generic_builder_destroy(gb);
	return 0;
}

int diagram_invalid_source(void)
{
	struct fyai_cfg cfg = { .color = "off", .diagram_theme = "missing-theme" };
	char *out;

	out = fyai_sink_diagram_render(&cfg, "treeView-beta\nmain\n", NULL, 80);
	FYAI_TCHECK(out == NULL);
	cfg.diagram_theme = "";
	out = fyai_sink_diagram_render(&cfg, "not-a-diagram", NULL, 80);
	FYAI_TCHECK(out == NULL);
	return 0;
}

int diagram_selection(void)
{
	struct fyai_cfg cfg = { .color = "on" };
	char *plain, *selected, *unknown;

	plain = fyai_sink_diagram_render(&cfg,
		"treeView-beta\n- main\n  - child\n", NULL, 80);
	selected = fyai_sink_diagram_render(&cfg,
		"treeView-beta\n- main\n  - child\n", "entries/1", 80);
	/* A path the render does not hold selects nothing, so the drawing is
	 * the one with no selection at all. */
	unknown = fyai_sink_diagram_render(&cfg,
		"treeView-beta\n- main\n  - child\n", "entries/9", 80);
	FYAI_TCHECK(plain && selected && unknown);
	FYAI_TCHECK(strcmp(plain, selected));
	FYAI_TCHECK(!strstr(selected, "\033[7m"));
	FYAI_TCHECK(!strcmp(plain, unknown));
	fymm_free(plain);
	fymm_free(selected);
	fymm_free(unknown);
	return 0;
}

int diagram_branch_topology(void)
{
	struct fy_generic_builder_cfg gbcfg = { .flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED };
	struct fy_generic_builder *gb = fy_generic_builder_create(&gbcfg);
	struct fymm_diagram *diagram;
	fy_generic rows, commits, row, parents;
	char *source;

	FYAI_TCHECK(gb != NULL);
	rows = fy_gb_sequence(gb,
		fy_gb_mapping(gb, "name", "main"),
		fy_gb_mapping(gb, "name", "main/agent:worker"),
		fy_gb_mapping(gb, "name", "other"),
		fy_gb_mapping(gb, "name", "other/日本語"));
	source = fyai_browser_gitgraph_source(rows);
	FYAI_TCHECK(source != NULL);
	diagram = fymm_parse(source, FYMM_NT, NULL);
	FYAI_TCHECK(diagram && !fymm_diagram_has_errors(diagram));
	commits = fy_get(fymm_diagram_model(diagram), "commits", fy_invalid);
	FYAI_TCHECK(fy_len(commits) == 4);
	row = fy_get(commits, 0);
	parents = fy_get(row, "parents", fy_invalid);
	FYAI_TCHECK(fy_empty(parents));
	row = fy_get(commits, 1);
	parents = fy_get(row, "parents", fy_invalid);
	FYAI_TCHECK(fy_len(parents) == 1 && fy_get(parents, 0, -1LL) == 0);
	FYAI_TCHECK(!strcmp(fy_get(row, "id", ""), "main/agent:worker"));
	row = fy_get(commits, 2);
	parents = fy_get(row, "parents", fy_invalid);
	FYAI_TCHECK(fy_empty(parents));
	row = fy_get(commits, 3);
	parents = fy_get(row, "parents", fy_invalid);
	FYAI_TCHECK(fy_len(parents) == 1 && fy_get(parents, 0, -1LL) == 2);
	FYAI_TCHECK(!strcmp(fy_get(row, "id", ""), "other/日本語"));
	fymm_diagram_destroy(diagram);
	free(source);
	fy_generic_builder_destroy(gb);
	return 0;
}
