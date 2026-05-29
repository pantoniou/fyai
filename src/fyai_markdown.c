/*
 * fyai_markdown.c - libfymd4c markdown rendering
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libfymd4c.h>

#include "fyai_markdown.h"
#include "fyai.h"
#include "fyai_terminal.h"

/* FYAI_EMBEDDED_STYLING[] / FYAI_EMBEDDED_STYLING_LEN - the shipped markdown
 * styling YAML compiled in as a fallback for relocated/static binaries. */
#include "embedded_styling.inc"

static enum fymd_background markdown_background(const char *theme)
{
	if (theme && !strcmp(theme, "light"))
		return FYMD_BG_LIGHT;
	if (theme && !strcmp(theme, "dark"))
		return FYMD_BG_DARK;
	return FYMD_BG_AUTO;
}

static void markdown_renderer_cfg(struct fyai_cfg *fcfg,
				  struct fymd_renderer_cfg *cfg, bool color,
				  const char *theme, enum fymd_cfg_flags extra)
{
	int width;

	memset(cfg, 0, sizeof(*cfg));

	width = markdown_render_width();
	cfg->flags = FYMD_RF_DEFAULT | FYMD_RF_TABLE_FIT | extra;
	if (!color)
		cfg->flags |= FYMD_RF_NO_COLOR;
	cfg->width = width > 0 ? width : FYMD_WIDTH_AUTO;
	cfg->background = markdown_background(theme);
	if (fy_generic_is_valid(fcfg->markdown_style_doc))
		cfg->style_generic = fcfg->markdown_style_doc;
	cfg->code_theme = fcfg->markdown_code_theme;
}

/*
 * Locate the shipped styling YAML: an explicit config path wins; otherwise try
 * next to the binary (installed tree), then the configure-time install and
 * source directories. Returns the first that exists, or NULL.
 */
static const char *markdown_style_path(struct fyai_cfg *cfg)
{
	static const char *const dirs[] = {
		FYAI_STYLING_INSTALL_DIR,
		FYAI_STYLING_SOURCE_DIR,
	};
	char exe[PATH_MAX];
	const char *dir, *theme;
	ssize_t n;
	size_t i;

	if (cfg->markdown_style && *cfg->markdown_style)
		return cfg->markdown_style;

	/* a named theme selects stylings/fyai-<name>.yaml */
	theme = cfg->markdown_theme && *cfg->markdown_theme ?
		cfg->markdown_theme : "markdown";

	n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	if (n > 0) {
		exe[n] = '\0';
		dir = dirname(exe);	/* may modify exe */
		if (snprintf(cfg->markdown_style_path,
			     sizeof(cfg->markdown_style_path),
			     "%s/../share/fyai/stylings/fyai-%s.yaml",
			     dir, theme) < (int)sizeof(cfg->markdown_style_path) &&
		    !access(cfg->markdown_style_path, R_OK))
			return cfg->markdown_style_path;
	}
	for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
		if (snprintf(cfg->markdown_style_path,
			     sizeof(cfg->markdown_style_path),
			     "%s/fyai-%s.yaml", dirs[i], theme) <
		    (int)sizeof(cfg->markdown_style_path) &&
		    !access(cfg->markdown_style_path, R_OK))
			return cfg->markdown_style_path;
	}
	if (strcmp(theme, "markdown"))
		fprintf(stderr, "markdown theme '%s' not found; using default\n",
			theme);
	return NULL;
}

/*
 * Resolve the reverse-card pair for both backgrounds and intern the escapes in
 * @gb so the cached pointers stay valid for the whole run (a short escape lives
 * inline in the by-value generic, so the raw fy_get() pointer would dangle - see
 * CLAUDE.md; interning gives it stable arena storage).
 */
static void markdown_resolve_reverse(struct fyai_cfg *cfg)
{
	fy_generic elems, rev, light, r;
	int i;

	elems = fy_get(cfg->markdown_style_doc, "elements");
	rev = fy_get(elems, "reverse");
	if (fy_generic_is_invalid(rev))
		return;
	for (i = 0; i < 2; i++) {
		r = rev;
		if (i == 1) {
			light = fy_get(rev, "light");
			if (fy_generic_is_valid(light))
				r = light;
		}
		cfg->markdown_rev_on[i] =
			fy_gb_intern_string(cfg->gb, fy_get(r, "on", ""));
		cfg->markdown_rev_off[i] =
			fy_gb_intern_string(cfg->gb, fy_get(r, "off", ""));
	}
}

void fyai_markdown_load_style(struct fyai_cfg *cfg)
{
	fy_generic_sized_string embedded;
	const char *path;

	/* The code theme applies even without our element styling. */
	cfg->markdown_style_doc = fy_invalid;
	cfg->markdown_code_theme = cfg->code_theme;
	memset(cfg->markdown_rev_on, 0, sizeof(cfg->markdown_rev_on));
	memset(cfg->markdown_rev_off, 0, sizeof(cfg->markdown_rev_off));

	/* A styling file wins when one is reachable (explicit path, installed
	 * tree, or source tree). */
	path = markdown_style_path(cfg);
	if (path) {
		cfg->markdown_style_doc =
			fy_parse_file(cfg->gb, FYOPPF_DISABLE_DIRECTORY |
				      FYOPPF_MODE_YAML_1_2, path);
		if (fy_generic_is_invalid(cfg->markdown_style_doc))
			fprintf(stderr,
				"warning: failed to parse styling: %s\n", path);
	}

	/* No file (or it failed to parse): fall back to the styling embedded at
	 * build time, parsed into the config builder so its strings are interned
	 * for the whole run - so a relocated or fully static binary still renders
	 * with full styling. */
	if (fy_generic_is_invalid(cfg->markdown_style_doc)) {
		embedded.data = (const char *)FYAI_EMBEDDED_STYLING;
		embedded.size = FYAI_EMBEDDED_STYLING_LEN;
		cfg->markdown_style_doc =
			fy_parse(cfg->gb, embedded, FYOPPF_DISABLE_DIRECTORY |
				 FYOPPF_MODE_YAML_1_2 |
				 FYOPPF_INPUT_TYPE_STRING, NULL);
	}

	if (fy_generic_is_invalid(cfg->markdown_style_doc))
		return;
	markdown_resolve_reverse(cfg);
}

bool markdown_reverse_pair(struct fyai_cfg *cfg, const char **on,
			   const char **off)
{
	int i = (cfg->theme && !strcmp(cfg->theme, "light")) ? 1 : 0;

	if (!cfg->markdown_rev_on[i] || !*cfg->markdown_rev_on[i])
		return false;
	*on = cfg->markdown_rev_on[i];
	*off = cfg->markdown_rev_off[i];
	return true;
}

bool markdown_available(struct fyai_cfg *fcfg)
{
	struct fymd_renderer_cfg cfg;
	struct fymd_renderer *r;

	markdown_renderer_cfg(fcfg, &cfg, false, "dark", 0);
	r = fymd_renderer_create(&cfg);
	if (!r)
		return false;
	fymd_renderer_destroy(r);
	return true;
}

char *markdown_lang_for_path(const char *path)
{
	if (!path || !*path)
		return NULL;
	return fymd_detect_language_for_path(path);
}

int markdown_renderer_start(struct fyai_cfg *fcfg,
			    struct markdown_renderer *renderer, bool color,
			    const char *theme)
{
	struct fymd_renderer_cfg cfg;

	memset(renderer, 0, sizeof(*renderer));

	markdown_renderer_cfg(fcfg, &cfg, color, theme, 0);
	renderer->renderer = fymd_renderer_create(&cfg);
	if (!renderer->renderer)
		return -1;
	renderer->active = true;
	return 0;
}

int markdown_renderer_push(struct markdown_renderer *renderer,
			   const char *text, size_t len,
			   struct markdown_update *update)
{
	struct fymd_update upd;

	if (!renderer->active)
		return -1;
	if (fymd_render_push(renderer->renderer, text, len, &upd))
		return -1;
	update->backtrack = upd.backtrack;
	update->content = upd.content;
	update->content_len = upd.content_len;
	update->freeze = upd.freeze;
	return 0;
}

int markdown_renderer_finish(struct markdown_renderer *renderer,
			     struct response_buffer *out)
{
	const char *s;
	size_t len;

	if (!renderer->active)
		return -1;
	if (fymd_render_finish(renderer->renderer, &s, &len))
		return -1;
	if (response_buffer_reserve(out, out->len + len + 1))
		return -1;
	memcpy(out->data + out->len, s, len);
	out->len += len;
	out->data[out->len] = '\0';
	renderer->active = false;
	return 0;
}

void markdown_renderer_destroy(struct markdown_renderer *renderer)
{
	if (!renderer)
		return;
	if (renderer->renderer)
		fymd_renderer_destroy(renderer->renderer);
	memset(renderer, 0, sizeof(*renderer));
}

static int markdown_render_flags(struct fyai_cfg *fcfg, const char *text,
				 size_t len,
				 struct response_buffer *out, bool color,
				 const char *theme, enum fymd_cfg_flags extra)
{
	struct fymd_renderer_cfg cfg;
	struct fymd_renderer *r;
	char *s;
	size_t slen;
	size_t before;

	before = out->len;
	s = NULL;

	markdown_renderer_cfg(fcfg, &cfg, color, theme, extra);
	r = fymd_renderer_create(&cfg);
	if (!r)
		goto raw;
	if (fymd_render(r, text, len, &s, &slen))
		goto raw_reset;
	if (response_buffer_reserve(out, out->len + slen + 1))
		goto raw_reset;
	memcpy(out->data + out->len, s, slen);
	out->len += slen;
	out->data[out->len] = '\0';
	fymd_free(s);
	fymd_renderer_destroy(r);
	return 0;

raw_reset:
	fymd_free(s);
	fymd_renderer_destroy(r);
	out->len = before;
	if (out->data)
		out->data[out->len] = '\0';
raw:
	if (!response_buffer_reserve(out, out->len + len + 1)) {
		memcpy(out->data + out->len, text, len);
		out->len += len;
		out->data[out->len] = '\0';
	}
	return -1;
}

int markdown_render(struct fyai_cfg *cfg, const char *text, size_t len,
		    struct response_buffer *out, bool color, const char *theme)
{
	return markdown_render_flags(cfg, text, len, out, color, theme, 0);
}

int markdown_render_reverse(struct fyai_cfg *cfg, const char *text, size_t len,
			    struct response_buffer *out, bool color,
			    const char *theme)
{
	return markdown_render_flags(cfg, text, len, out, color, theme,
				     FYMD_RF_REVERSE);
}

int fyai_print_markdown(const char *text, struct fyai_cfg *cfg)
{
	struct response_buffer out = {0};
	bool color;
	size_t start;
	size_t end;
	int rc;

	start = 0;
	color = markdown_color_enabled(cfg->color);
	fflush(stdout);
	rc = markdown_render(cfg, text, strlen(text), &out, color, cfg->theme);
	end = out.len;
	if (rc) {
		/* Fallback: raw input was appended, emit it untouched. */
		if (out.len)
			fwrite(out.data, 1, out.len, stdout);
		fflush(stdout);
		free(out.data);
		return rc;
	}
	while (start < out.len && (out.data[start] == '\n' ||
				   out.data[start] == '\r'))
		start++;
	/*
	 * libfymd4c terminates its output with a trailing blank line
	 * (content '\n' plus a document-final '\n'). Trim the surplus so a
	 * rendered view ends exactly where its content does.
	 */
	while (end > start && (out.data[end - 1] == '\n' ||
			       out.data[end - 1] == '\r'))
		end--;
	if (end > start)
		fwrite(out.data + start, 1, end - start, stdout);
	fputc('\n', stdout);
	fflush(stdout);
	free(out.data);
	return rc;
}
