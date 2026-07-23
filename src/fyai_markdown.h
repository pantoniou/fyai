/* SPDX-License-Identifier: MIT */
#ifndef FYAI_MARKDOWN_H
#define FYAI_MARKDOWN_H

#include <libfyaml/libfyaml-generic.h>

#include "utils.h"

struct fyai_cfg;
struct fymd_renderer;

struct markdown_renderer {
	struct fymd_renderer *renderer;
	bool active;
};

struct markdown_update {
	size_t backtrack;
	const char *content;
	size_t content_len;
	size_t freeze;
};

bool markdown_available(struct fyai_cfg *cfg);

/*
 * True if @name is a selectable libfymd4c theme (or NULL/empty, meaning the
 * library default). Used to validate display/markdown_theme.
 */
bool markdown_theme_valid(const char *name);
/* The valid theme names, comma separated, into @buf; returns @buf. */
const char *markdown_theme_names(char *buf, size_t bufsz);

/*
 * Highlighter language name for a file path (from its extension), using the same
 * catalogue the fenced-code highlighter uses so the result is a valid info
 * string. Returns a newly allocated string the caller frees, or NULL when the
 * extension is unknown/unsupported.
 */
char *markdown_lang_for_path(const char *path);

/*
 * Install the shared markdown styling (a parsed libfymd4c style generic) and the
 * fenced-code theme name/path. Both are applied to every subsequent render, so
 * fyai's colours - and the fenced-code highlighter - come from YAML, not from
 * escapes in C. Locate and load the shipped styling YAML into @cfg->gb; safe to
 * skip (renderer falls back to the library default theme) if it is missing.
 */
void fyai_markdown_load_style(struct fyai_cfg *cfg);
/*
 * The reverse-card foreground/background escape pair for the effective theme,
 * read from the loaded styling generic (elements.reverse, with the .light
 * override when cfg->theme is "light"). Returns false if no styling is loaded.
 */
bool markdown_reverse_pair(struct fyai_cfg *cfg, const char **on,
			   const char **off);

int markdown_renderer_start(struct fyai_cfg *cfg,
			    struct markdown_renderer *renderer, bool color,
			    const char *theme);
int markdown_renderer_push(struct markdown_renderer *renderer,
			   const char *text, size_t len,
			   struct markdown_update *update);
int markdown_renderer_finish(struct markdown_renderer *renderer,
			     struct response_buffer *out);
void markdown_renderer_destroy(struct markdown_renderer *renderer);
int markdown_render(struct fyai_cfg *cfg, const char *text, size_t len,
		    struct response_buffer *out, bool color, const char *theme);
int markdown_render_margins(struct fyai_cfg *cfg, const char *text, size_t len,
			    struct response_buffer *out,
			    const char *first_margin,
			    const char *next_margin);
int markdown_render_reverse(struct fyai_cfg *cfg, const char *text, size_t len,
			    struct response_buffer *out, bool color,
			    const char *theme);
int fyai_print_markdown(const char *text, struct fyai_cfg *cfg);
/* Like fyai_print_markdown(), but render to @fp (e.g. stderr for live tool
 * headers) instead of stdout. */
int fyai_fprint_markdown(FILE *fp, const char *text, struct fyai_cfg *cfg);
/*
 * Like fyai_print_markdown(), but bound the rendered view to @max_lines terminal
 * rows (head + omission separator + tail), counted after wrapping/layout. A zero
 * limit renders unbounded. Used to cap tool-output previews to the configured
 * tool_preview_lines without truncating the markdown source.
 */
int fyai_print_markdown_limited(const char *text, struct fyai_cfg *cfg,
				size_t max_lines);
/*
 * Print @text/@len as a single fenced block via libfymd4c's fenced-block
 * renderer (no Markdown parsing, no theme fence decoration), bounded to
 * @max_lines rendered rows. @lang selects the highlighter (NULL/empty => plain,
 * for tool errors); @template_vars supplies {key} decoration values or may be
 * fy_invalid. Returns 0 on success, -1 on renderer failure (raw text printed).
 */
int fyai_print_fenced(struct fyai_cfg *cfg, const char *text, size_t len,
		      const char *lang, fy_generic template_vars,
		      size_t max_lines);
/*
 * Write @data/@len to @fp with each non-empty line prefixed by the @ind indent
 * string. Used to manually decorate raw fenced tool output with a uniform
 * indent (content and the row-limit separator alike), in one-shot and
 * progressive rendering.
 */
void fyai_fwrite_indented(FILE *fp, const char *ind, const char *data,
			  size_t len);

/*
 * Progressive raw fenced-block renderer: stream tool output into a bounded,
 * indented, in-place-updating region (the live-shell counterpart of
 * fyai_print_fenced, using the same libfymd4c fenced render + row limit so the
 * live view and the history view match). Drive it start -> push* -> finish.
 */
struct fyai_fenced_stream {
	struct fyai_ctx *ctx;
	struct fymd_renderer *r;
	struct response_buffer accum;	/* raw source accumulated so far */
	struct response_buffer shown;	/* last rendered (un-indented) output */
	const char *lang;		/* highlighter language, NULL => plain */
	const char *indent;		/* per-line indent decoration */
	FILE *fp;
	bool live;			/* true: repaint in place; false: buffer */
	bool active;
};

int fyai_fenced_stream_start(struct fyai_fenced_stream *fs, struct fyai_ctx *ctx,
			     struct fyai_cfg *cfg,
			     const char *lang, size_t max_lines,
			     const char *indent, FILE *fp, bool live);
int fyai_fenced_stream_push(struct fyai_fenced_stream *fs, const char *data,
			    size_t len);
void fyai_fenced_stream_finish(struct fyai_fenced_stream *fs);
size_t fyai_common_complete_lines(const char *a, size_t alen,
				  const char *b, size_t blen);
size_t fyai_count_newlines(const char *data, size_t len);

#endif
