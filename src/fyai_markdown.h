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
int markdown_render_reverse(struct fyai_cfg *cfg, const char *text, size_t len,
			    struct response_buffer *out, bool color,
			    const char *theme);
int fyai_print_markdown(const char *text, struct fyai_cfg *cfg);

#endif
