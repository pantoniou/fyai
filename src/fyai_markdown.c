/*
 * fyai_markdown.c - libfymd4c markdown rendering
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libfymd4c.h>

#include "fyai_markdown.h"
#include "fyai_ui.h"
#include "fyai.h"
#include "fyai_terminal.h"

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

	width = fcfg->render_width > 0 ? fcfg->render_width :
		markdown_render_width();
	cfg->flags = FYMD_RF_DEFAULT | FYMD_RF_TABLE_FIT | extra;
	if (!color)
		cfg->flags |= FYMD_RF_NO_COLOR;
	cfg->width = width > 0 ? width : FYMD_WIDTH_AUTO;
	cfg->background = markdown_background(theme);
	/*
	 * Keep SGR (colour/attribute) escapes embedded in the input, stripping
	 * only dangerous ones (cursor moves, screen clears, OSC). This lets
	 * colours placed in decorator/separator config values survive rendering,
	 * while untrusted content still cannot smuggle control sequences.
	 */
	cfg->sgr_input = FYMD_SGR_SAFE;
	/* Theming is surrendered to libfymd4c: the palette is one of its embedded
	 * theme names (NULL/empty => the library "default"); the fenced-code
	 * highlighter theme still comes from our code_theme override. */
	cfg->theme = fcfg->markdown_theme;
	cfg->code_theme = fcfg->markdown_code_theme;
	/* Table-border override on top of the theme: 1 grid, 2 none, else theme. */
	cfg->table_border = fcfg->table_border == 1 ? FYMD_TB_GRID :
			    fcfg->table_border == 2 ? FYMD_TB_NONE :
			    FYMD_TB_THEME;
}

/*
 * Query the active theme's reverse-card pair for a given background from a probe
 * renderer and intern the escapes in @gb so the cached pointers stay valid for
 * the whole run (a raw library pointer would die with its renderer; interning
 * gives it stable arena storage). @index selects the cache slot (0 dark, 1
 * light). Every theme resolves a pair, so this normally fills both slots.
 */
static void markdown_probe_reverse(struct fyai_cfg *cfg, int index,
				   const char *theme)
{
	struct fymd_renderer_cfg rcfg;
	struct fymd_renderer *r;
	const char *on;
	const char *off;

	markdown_renderer_cfg(cfg, &rcfg, true, theme, 0);
	r = fymd_renderer_create(&rcfg);
	if (!r)
		return;
	on = "";
	off = "";
	if (!fymd_renderer_get_reverse_pair(r, &on, &off)) {
		cfg->markdown_rev_on[index] = fy_gb_intern_string(cfg->gb, on);
		cfg->markdown_rev_off[index] = fy_gb_intern_string(cfg->gb, off);
	}
	fymd_renderer_destroy(r);
}

void fyai_markdown_load_style(struct fyai_cfg *cfg)
{
	/* The fenced-code highlighter theme is our override; the element palette
	 * is now the libfymd4c theme selected by cfg->markdown_theme. */
	cfg->markdown_code_theme = cfg->code_theme;
	memset(cfg->markdown_rev_on, 0, sizeof(cfg->markdown_rev_on));
	memset(cfg->markdown_rev_off, 0, sizeof(cfg->markdown_rev_off));

	/* Cache the theme's user-turn bubble pair for both backgrounds so
	 * fyai_bubble_fence() can draw the card's own top/bottom rows. */
	markdown_probe_reverse(cfg, 0, "dark");
	markdown_probe_reverse(cfg, 1, "light");
}

bool markdown_theme_valid(const char *name)
{
	size_t i;
	size_t n;

	if (!name || !*name)
		return true;	/* NULL/empty => the library "default" */
	n = fymd_theme_count();
	for (i = 0; i < n; i++)
		if (!strcmp(name, fymd_theme_name(i)))
			return true;
	return false;
}

/*
 * The embedded theme names, comma separated, into @buf. Queried from
 * libfymd4c so the list cannot drift from what markdown_theme_valid()
 * accepts - note only some themes carry a `-borderless` variant.
 */
const char *markdown_theme_names(char *buf, size_t bufsz)
{
	size_t i, n, off;
	int rc;

	off = 0;
	n = fymd_theme_count();
	for (i = 0; i < n; i++) {
		rc = snprintf(buf + off, off < bufsz ? bufsz - off : 0,
			      "%s%s", off ? ", " : "", fymd_theme_name(i));
		if (rc <= 0)
			break;
		off += (size_t)rc;
		if (off >= bufsz)
			break;
	}
	if (bufsz)
		buf[bufsz - 1] = '\0';
	return buf;
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

/*
 * Bound the renderer to @max_lines rendered terminal rows (head + omission
 * separator + tail), counted after wrapping/layout. A zero limit leaves the
 * renderer unbounded. The separator mirrors the "N more lines" trailer fyai
 * used to print by hand; the library substitutes the omitted-row count for %d.
 */
static void markdown_set_line_limit(struct fymd_renderer *r, size_t max_lines)
{
	struct fymd_line_limit_opts opts;

	if (!max_lines)
		return;
	memset(&opts, 0, sizeof(opts));
	opts.mode = FYMD_LLM_HEAD_TAIL;
	opts.max_lines = max_lines;
	opts.split = FYMD_LLS_BALANCED;
	opts.separator_format = "⋯ %d more lines";
	fymd_renderer_set_line_limit(r, &opts);
}

static int markdown_render_flags(struct fyai_cfg *fcfg, const char *text,
				 size_t len,
				 struct response_buffer *out, bool color,
				 const char *theme, enum fymd_cfg_flags extra,
				 size_t max_lines)
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
	markdown_set_line_limit(r, max_lines);
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
	return markdown_render_flags(cfg, text, len, out, color, theme, 0, 0);
}

struct markdown_margin_ctx {
	const char *first;
	const char *next;
};

static const char *markdown_margin_cb(void *userdata, size_t row)
{
	struct markdown_margin_ctx *m = userdata;

	return row ? m->next : m->first;
}

int markdown_render_margins(struct fyai_cfg *fcfg, const char *text, size_t len,
			    struct response_buffer *out,
			    const char *first_margin,
			    const char *next_margin)
{
	struct markdown_margin_ctx margins = { first_margin, next_margin };
	struct fymd_renderer_cfg cfg;
	struct fymd_renderer *r;
	char *s = NULL;
	size_t slen = 0;

	markdown_renderer_cfg(fcfg, &cfg,
			      markdown_color_enabled(fcfg->color),
			      fcfg->theme, 0);
	r = fymd_renderer_create(&cfg);
	if (!r)
		return -1;
	if (fymd_render_with_margins(r, text, len, markdown_margin_cb, &margins,
				     &s, &slen)) {
		fymd_renderer_destroy(r);
		return -1;
	}
	if (response_buffer_reserve(out, out->len + slen + 1)) {
		fymd_free(s);
		fymd_renderer_destroy(r);
		return -1;
	}
	memcpy(out->data + out->len, s, slen);
	out->len += slen;
	out->data[out->len] = '\0';
	fymd_free(s);
	fymd_renderer_destroy(r);
	return 0;
}

int markdown_render_reverse(struct fyai_cfg *cfg, const char *text, size_t len,
			    struct response_buffer *out, bool color,
			    const char *theme)
{
	return markdown_render_flags(cfg, text, len, out, color, theme,
				     FYMD_RF_REVERSE, 0);
}

static int markdown_fprint(FILE *fp, const char *text, struct fyai_cfg *cfg,
			   size_t max_lines)
{
	struct response_buffer out = {0};
	bool color;
	size_t start;
	size_t end;
	int rc;

	start = 0;
	color = markdown_color_enabled(cfg->color);
	fflush(fp);
	rc = markdown_render_flags(cfg, text, strlen(text), &out, color,
				   cfg->theme, 0, max_lines);
	end = out.len;
	if (rc) {
		/* Fallback: raw input was appended, emit it untouched. */
		if (out.len)
			fwrite(out.data, 1, out.len, fp);
		fflush(fp);
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
		fwrite(out.data + start, 1, end - start, fp);
	fputc('\n', fp);
	fflush(fp);
	free(out.data);
	return rc;
}

int fyai_print_markdown_limited(const char *text, struct fyai_cfg *cfg,
				size_t max_lines)
{
	return markdown_fprint(stdout, text, cfg, max_lines);
}

int fyai_print_markdown(const char *text, struct fyai_cfg *cfg)
{
	return markdown_fprint(stdout, text, cfg, 0);
}

int fyai_fprint_markdown(FILE *fp, const char *text, struct fyai_cfg *cfg)
{
	return markdown_fprint(fp, text, cfg, 0);
}

/*
 * Render @text as a single fenced block through libfymd4c's fenced-block
 * renderer, bypassing Markdown parsing. Tool output is presented without the
 * theme's fence decoration (rules/margin): FYMD_FBF_HIGHLIGHT alone emits the
 * highlighted content, so no FYMD_FBF_STYLE decoration frames it. @lang selects
 * the highlighter language (NULL/empty => a plain, unhighlighted block, used for
 * tool errors); @template_vars supplies {key} values for any decoration
 * templates that remain, and may be fy_invalid. The rendered rows are bounded to
 * @max_lines (0 => unbounded) with the same head/tail viewport as
 * fyai_print_markdown_limited(). ANSI is appended to @out; on failure the raw
 * text is appended instead and -1 returned.
 */
static int markdown_render_fenced(struct fyai_cfg *fcfg, const char *text,
				  size_t len, const char *lang,
				  fy_generic template_vars, size_t max_lines,
				  struct response_buffer *out, bool color)
{
	struct fymd_fenced_block_opts opts;
	struct fymd_renderer_cfg cfg;
	struct fymd_renderer *r;
	char *s;
	size_t slen;
	size_t before;

	before = out->len;
	s = NULL;

	markdown_renderer_cfg(fcfg, &cfg, color, fcfg->theme, 0);
	r = fymd_renderer_create(&cfg);
	if (!r)
		goto raw;
	markdown_set_line_limit(r, max_lines);

	/*
	 * Render the raw content frameless (highlighting only, no theme fence
	 * rules or margin) at column 0. fyai decorates it manually - a uniform
	 * indent applied by the callers below - so content and the row-limit
	 * separator line up, in both one-shot and progressive rendering.
	 */
	memset(&opts, 0, sizeof(opts));
	opts.language = lang;
	opts.flags = (lang && *lang) ? FYMD_FBF_HIGHLIGHT : 0;
	opts.template_vars = template_vars;

	if (fymd_render_fenced_block(r, text, len, &opts, &s, &slen))
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

void fyai_fwrite_indented(FILE *fp, const char *ind, const char *data,
			  size_t len)
{
	size_t ilen = ind ? strlen(ind) : 0;
	size_t ls;
	size_t i;

	for (ls = 0, i = 0; i < len; i++) {
		if (data[i] != '\n')
			continue;
		if (i > ls && ilen)		/* prefix non-empty lines only */
			fwrite(ind, 1, ilen, fp);
		fwrite(data + ls, 1, i - ls + 1, fp);
		ls = i + 1;
	}
	if (ls < len) {
		if (ilen)
			fwrite(ind, 1, ilen, fp);
		fwrite(data + ls, 1, len - ls, fp);
	}
}

int fyai_print_fenced(struct fyai_cfg *cfg, const char *text, size_t len,
		      const char *lang, fy_generic template_vars,
		      size_t max_lines)
{
	struct response_buffer out = {0};
	bool color;
	size_t start;
	size_t end;
	int rc;

	if (!len)
		return 0;
	start = 0;
	color = markdown_color_enabled(cfg->color);
	fflush(stdout);
	rc = markdown_render_fenced(cfg, text, len, lang, template_vars,
				    max_lines, &out, color);
	end = out.len;
	if (rc) {
		if (out.len)
			fwrite(out.data, 1, out.len, stdout);
		fflush(stdout);
		free(out.data);
		return rc;
	}
	while (start < out.len && (out.data[start] == '\n' ||
				   out.data[start] == '\r'))
		start++;
	while (end > start && (out.data[end - 1] == '\n' ||
			       out.data[end - 1] == '\r'))
		end--;
	if (end > start)
		fyai_fwrite_indented(stdout, FYAI_TOOL_OUTPUT_INDENT,
				     out.data + start, end - start);
	fputc('\n', stdout);
	fflush(stdout);
	free(out.data);
	return rc;
}

/* Bytes of the common prefix of @a and @b that end on a complete line (the last
 * shared newline). The tail past this point is what a progressive update must
 * repaint. */
size_t fyai_common_complete_lines(const char *a, size_t alen,
				  const char *b, size_t blen)
{
	size_t i = 0;
	size_t last_nl = 0;

	while (i < alen && i < blen && a[i] == b[i]) {
		if (a[i] == '\n')
			last_nl = i + 1;
		i++;
	}
	return last_nl;
}

size_t fyai_count_newlines(const char *data, size_t len)
{
	size_t i;
	size_t n = 0;

	for (i = 0; i < len; i++)
		if (data[i] == '\n')
			n++;
	return n;
}

/*
 * Progressive raw fenced-block rendering, following the libfymd4c CLI model:
 * accumulate the raw source, re-render the whole bounded block on each push,
 * line-diff it against what is on screen, and repaint only the changed tail.
 * The row limit (HEAD_TAIL) keeps the on-screen region bounded exactly as the
 * one-shot/history render, so a live shell run and its history look identical -
 * only here it updates in place as output streams in. fyai decorates the raw
 * output manually with a uniform indent.
 */
int fyai_fenced_stream_start(struct fyai_fenced_stream *fs, struct fyai_ctx *ctx,
			     struct fyai_cfg *cfg,
			     const char *lang, size_t max_lines,
			     const char *indent, FILE *fp, bool live)
{
	struct fymd_renderer_cfg rcfg;

	memset(fs, 0, sizeof(*fs));
	fs->ctx = ctx;
	markdown_renderer_cfg(cfg, &rcfg, markdown_color_enabled(cfg->color),
			      cfg->theme, 0);
	fs->r = fymd_renderer_create(&rcfg);
	if (!fs->r)
		return -1;
	markdown_set_line_limit(fs->r, max_lines);
	fs->lang = lang;
	fs->indent = indent;
	fs->fp = fp;
	fs->live = live;
	fs->active = true;
	return 0;
}

int fyai_fenced_stream_push(struct fyai_fenced_stream *fs,
			    const char *data, size_t len)
{
	struct fymd_fenced_block_opts opts;
	char *rendered = NULL;
	size_t rlen = 0;
	size_t common;
	size_t backtrack;

	if (!fs->active)
		return -1;
	if (len) {
		if (response_buffer_reserve(&fs->accum, fs->accum.len + len + 1))
			return -1;
		memcpy(fs->accum.data + fs->accum.len, data, len);
		fs->accum.len += len;
		fs->accum.data[fs->accum.len] = '\0';
	}
	/* Off a terminal there is nothing to repaint; just accumulate and render
	 * once at finish (avoids re-rendering the whole block on every chunk). */
	if (!fs->live)
		return 0;

	memset(&opts, 0, sizeof(opts));
	opts.language = fs->lang;
	opts.flags = (fs->lang && *fs->lang) ? FYMD_FBF_HIGHLIGHT : 0;
	if (fymd_render_fenced_block(fs->r, fs->accum.data, fs->accum.len,
				     &opts, &rendered, &rlen))
		return -1;
	if (fyai_ui_active(fs->ctx)) {
		char *indented = NULL;
		size_t ilen = 0;
		FILE *mf = open_memstream(&indented, &ilen);

		if (!mf) {
			fymd_free(rendered);
			return -1;
		}
		fyai_fwrite_indented(mf, fs->indent, rendered, rlen);
		if (fclose(mf)) {
			free(indented);
			fymd_free(rendered);
			return -1;
		}
		fyai_ui_tool_update(fs->ctx, indented, ilen);
		free(indented);
		fymd_free(rendered);
		return 0;
	}
	/*
	 * Keep the trailing newline: each repaint must leave the cursor on a
	 * fresh line so the next diff's backtrack lands at a line start. Trimming
	 * it strands the cursor mid-line and the next push appends there,
	 * duplicating the last row.
	 */

	common = fyai_common_complete_lines(fs->shown.data ? fs->shown.data : "",
					    fs->shown.len, rendered, rlen);
	backtrack = fyai_count_newlines(fs->shown.data + common,
					fs->shown.len - common);
	if (backtrack)	/* up N rows, column 0, erase to end of screen */
		fprintf(fs->fp, "\033[%zuA\r\033[J", backtrack);
	fyai_fwrite_indented(fs->fp, fs->indent, rendered + common,
			     rlen - common);
	fflush(fs->fp);

	fs->shown.len = 0;
	if (response_buffer_reserve(&fs->shown, rlen + 1)) {
		fymd_free(rendered);
		return -1;
	}
	memcpy(fs->shown.data, rendered, rlen);
	fs->shown.len = rlen;
	fs->shown.data[rlen] = '\0';
	fymd_free(rendered);
	return 0;
}

void fyai_fenced_stream_finish(struct fyai_fenced_stream *fs)
{
	struct fymd_fenced_block_opts opts;
	char *rendered = NULL;
	size_t rlen = 0;

	if (!fs->active)
		return;
	if (fs->live && !fyai_ui_active(fs->ctx)) {
		fputc('\n', fs->fp);
	} else if (!fs->live) {
		/* Non-terminal: render the whole accumulated block once, bounded,
		 * and write it indented (matches the history view). */
		memset(&opts, 0, sizeof(opts));
		opts.language = fs->lang;
		opts.flags = (fs->lang && *fs->lang) ? FYMD_FBF_HIGHLIGHT : 0;
		if (fs->accum.len &&
		    !fymd_render_fenced_block(fs->r, fs->accum.data,
					      fs->accum.len, &opts,
					      &rendered, &rlen)) {
			while (rlen && (rendered[rlen - 1] == '\n' ||
					rendered[rlen - 1] == '\r'))
				rlen--;
			if (rlen)
				fyai_fwrite_indented(fs->fp, fs->indent,
						     rendered, rlen);
			fputc('\n', fs->fp);
			fymd_free(rendered);
		}
	}
	fflush(fs->fp);
	if (fs->r)
		fymd_renderer_destroy(fs->r);
	free(fs->accum.data);
	free(fs->shown.data);
	memset(fs, 0, sizeof(*fs));
}
