/*
 * fyai_render.c - the single generic to Markdown table renderer
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai_markdown.h"
#include "fyai_render.h"

/*
 * A schema default (system_prompt's, notably) can be arbitrarily long in a
 * live config; cap every cell so one oversized value cannot blow up the
 * table.
 */
#define RENDER_CELL_MAX	160
#define RENDER_NAME_MAX	64

enum render_format {
	RENDER_FMT_PLAIN,
	RENDER_FMT_HUMANIZE,
	RENDER_FMT_YESNO,
	RENDER_FMT_MARK,
	RENDER_FMT_JOIN,
};

struct render_col {
	fy_generic key;			/* stable storage: fy_castp() safe */
	char name[RENDER_NAME_MAX];
	const char *align;
	enum render_format fmt;
	bool fmt_set;
};

static bool render_seq_has(fy_generic seq, fy_generic key)
{
	fy_generic v;

	fy_foreach(v, seq) {
		if (!fy_generic_compare(v, key))
			return true;
	}
	return false;
}

static const char *render_align_str(const char *align)
{
	if (!strcmp(align, "right"))
		return "---:";
	if (!strcmp(align, "center"))
		return ":-:";
	return "---";
}

/* Right align numbers and booleans, left align everything else. */
static const char *render_align_default(fy_generic v)
{
	switch (fy_generic_get_type(v)) {
	case FYGT_INT:
	case FYGT_FLOAT:
	case FYGT_BOOL:
		return "---:";
	default:
		return "---";
	}
}

static enum render_format render_format_default(fy_generic v)
{
	switch (fy_generic_get_type(v)) {
	case FYGT_BOOL:
		return RENDER_FMT_YESNO;
	case FYGT_SEQUENCE:
		return RENDER_FMT_JOIN;
	default:
		return RENDER_FMT_PLAIN;
	}
}

static enum render_format render_format_parse(const char *s)
{
	if (!strcmp(s, "humanize"))
		return RENDER_FMT_HUMANIZE;
	if (!strcmp(s, "yesno"))
		return RENDER_FMT_YESNO;
	if (!strcmp(s, "mark"))
		return RENDER_FMT_MARK;
	if (!strcmp(s, "join"))
		return RENDER_FMT_JOIN;
	return RENDER_FMT_PLAIN;
}

/* Underscores to spaces, first letter capitalized, into a fixed buffer. */
static void render_humanize(char *buf, size_t bufsz, const char *s)
{
	size_t i;

	for (i = 0; i + 1 < bufsz && *s; i++, s++)
		buf[i] = *s == '_' ? ' ' : *s;
	buf[i] = '\0';
	if (buf[0])
		buf[0] = toupper((unsigned char)buf[0]);
}

/* Escape the cell separator and fold newlines, capped at RENDER_CELL_MAX. */
static void render_escape(FILE *fp, const char *s, size_t *used)
{
	if (!s)
		return;
	for (; *s; s++) {
		if (*used >= RENDER_CELL_MAX) {
			fputs("...", fp);
			*used += 3;
			return;
		}
		if (*s == '|') {
			fputc('\\', fp);
			(*used)++;
		}
		fputc(*s == '\n' ? ' ' : *s, fp);
		(*used)++;
	}
}

static void render_cell(FILE *fp, fy_generic v, enum render_format fmt)
{
	char buf[RENDER_CELL_MAX + 1];
	fy_generic item;
	size_t used;
	int i;

	used = 0;
	if (fy_generic_is_invalid(v))
		return;
	switch (fmt) {
	case RENDER_FMT_YESNO:
		fputs(fy_cast(v, false) ? "yes" : "no", fp);
		return;
	case RENDER_FMT_MARK:
		fputs(fy_cast(v, false) ? "*" : "", fp);
		return;
	case RENDER_FMT_JOIN:
		i = 0;
		fy_foreach(item, v) {
			if (i++)
				fputs(", ", fp);
			render_escape(fp, fy_castp(&item, ""), &used);
		}
		return;
	case RENDER_FMT_HUMANIZE:
		if (fy_generic_is_string(v)) {
			render_humanize(buf, sizeof(buf), fy_castp(&v, ""));
			render_escape(fp, buf, &used);
			return;
		}
		break;
	case RENDER_FMT_PLAIN:
	default:
		break;
	}
	switch (fy_generic_get_type(v)) {
	case FYGT_STRING:
		render_escape(fp, fy_castp(&v, ""), &used);
		break;
	case FYGT_INT:
		fprintf(fp, "%lld", fy_cast(v, 0LL));
		break;
	case FYGT_FLOAT:
		fprintf(fp, "%g", fy_cast(v, 0.0));
		break;
	case FYGT_BOOL:
		fputs(fy_cast(v, false) ? "yes" : "no", fp);
		break;
	case FYGT_NULL:
		fputs("null", fp);
		break;
	case FYGT_SEQUENCE:
		i = 0;
		fy_foreach(item, v) {
			if (i++)
				fputs(", ", fp);
			render_escape(fp, fy_castp(&item, ""), &used);
		}
		break;
	default:
		fputs("(complex value)", fp);
		break;
	}
}

/*
 * Fill in @col's header, alignment and format: the "columns" override for
 * @col's key wins, otherwise they are derived from the key text and from a
 * sample value of the column.
 */
static void render_col_setup(struct render_col *col, fy_generic columns,
			     fy_generic sample)
{
	fy_generic opts, v;

	render_humanize(col->name, sizeof(col->name), fy_castp(&col->key, ""));
	col->align = render_align_default(sample);
	col->fmt = render_format_default(sample);
	col->fmt_set = false;

	opts = fy_get(columns, fy_castp(&col->key, ""), fy_invalid);
	if (!fy_generic_is_mapping(opts))
		return;
	v = fy_get(opts, "name", fy_invalid);
	if (fy_generic_is_string(v))
		snprintf(col->name, sizeof(col->name), "%s", fy_castp(&v, ""));
	v = fy_get(opts, "align", fy_invalid);
	if (fy_generic_is_string(v))
		col->align = render_align_str(fy_castp(&v, ""));
	v = fy_get(opts, "format", fy_invalid);
	if (fy_generic_is_string(v)) {
		col->fmt = render_format_parse(fy_castp(&v, ""));
		col->fmt_set = true;
	}
}

/*
 * Collect the columns of a sequence of mappings: the "keys" override selects
 * them (and their order) verbatim, otherwise they are the union of every
 * row's keys, in first seen order.
 */
static struct render_col *render_cols_collect(fy_generic data,
					      fy_generic renderopts,
					      size_t *ncolsp)
{
	fy_generic keys, columns, row, key, sample;
	struct render_col *cols, *ncols_mem;
	size_t ncols, alloc, i, j, n;
	bool seen;

	cols = NULL;
	ncols = 0;
	alloc = 0;
	columns = fy_get(renderopts, "columns", fy_invalid);
	keys = fy_get(renderopts, "keys", fy_invalid);

	fy_foreach(row, data) {
		if (!fy_generic_is_mapping(row))
			continue;
		n = fy_generic_mapping_get_pair_count(row);
		for (i = 0; i < n; i++) {
			key = fy_generic_mapping_get_at_key(row, i);
			if (!fy_generic_is_string(key))
				continue;
			if (fy_generic_is_sequence(keys) &&
			    !render_seq_has(keys, key))
				continue;
			seen = false;
			for (j = 0; j < ncols && !seen; j++)
				seen = fy_generic_compare(cols[j].key, key) == 0;
			if (seen)
				continue;
			if (ncols == alloc) {
				alloc = alloc ? alloc * 2 : 8;
				ncols_mem = realloc(cols,
						    alloc * sizeof(*cols));
				if (!ncols_mem) {
					free(cols);
					return NULL;
				}
				cols = ncols_mem;
			}
			cols[ncols].key = key;
			sample = fy_generic_mapping_get_at_value(row, i);
			render_col_setup(&cols[ncols], columns, sample);
			ncols++;
		}
	}

	/* honour the requested column order, not the first seen one */
	if (fy_generic_is_sequence(keys) && ncols > 1) {
		j = 0;
		fy_foreach(key, keys) {
			for (i = j; i < ncols; i++) {
				struct render_col tmp;

				if (fy_generic_compare(cols[i].key, key))
					continue;
				tmp = cols[j];
				cols[j] = cols[i];
				cols[i] = tmp;
				j++;
				break;
			}
		}
	}
	*ncolsp = ncols;
	return cols;
}

static void render_seq_table(FILE *fp, fy_generic data, fy_generic renderopts)
{
	struct render_col *cols;
	fy_generic row, v;
	const char *empty;
	size_t ncols, j;
	bool any;

	cols = render_cols_collect(data, renderopts, &ncols);
	if (!cols || !ncols) {
		free(cols);
		ncols = 0;
	}

	if (ncols) {
		fputc('|', fp);
		for (j = 0; j < ncols; j++)
			fprintf(fp, " %s |", cols[j].name);
		fputs("\n|", fp);
		for (j = 0; j < ncols; j++)
			fprintf(fp, "%s|", cols[j].align);
		fputc('\n', fp);
	}

	any = false;
	fy_foreach(row, data) {
		if (!fy_generic_is_mapping(row))
			continue;
		any = true;
		fputc('|', fp);
		for (j = 0; j < ncols; j++) {
			fputc(' ', fp);
			v = fy_get(row, fy_castp(&cols[j].key, ""), fy_invalid);
			render_cell(fp, v, cols[j].fmt_set ? cols[j].fmt :
				    render_format_default(v));
			fputs(" |", fp);
		}
		fputc('\n', fp);
	}
	free(cols);

	if (!any) {
		empty = fy_get(renderopts, "empty", "");
		fprintf(fp, "\n_%s_\n", *empty ? empty : "nothing to show");
	}
}

/*
 * A plain mapping is the degenerate table: one row per pair, a key column and
 * a value column.  The per key "columns" override still applies, so a caller
 * can rename a key or humanize its value.
 */
static void render_kv_table(FILE *fp, fy_generic data, fy_generic renderopts)
{
	struct render_col col;
	fy_generic columns, key, v;
	const char *empty;
	size_t i, n;

	n = fy_generic_mapping_get_pair_count(data);
	if (!n) {
		empty = fy_get(renderopts, "empty", "");
		fprintf(fp, "\n_%s_\n", *empty ? empty : "nothing to show");
		return;
	}
	columns = fy_get(renderopts, "columns", fy_invalid);
	fprintf(fp, "| %s | %s |\n|---|---|\n",
		fy_get(renderopts, "key_header", "Metric"),
		fy_get(renderopts, "value_header", "Value"));
	for (i = 0; i < n; i++) {
		key = fy_generic_mapping_get_at_key(data, i);
		if (!fy_generic_is_string(key))
			continue;
		v = fy_generic_mapping_get_at_value(data, i);
		col.key = key;
		render_col_setup(&col, columns, v);
		fprintf(fp, "| %s | ", col.name);
		render_cell(fp, v, col.fmt);
		fputs(" |\n", fp);
	}
}

int fyai_generic_to_markdown(struct fyai_ctx *ctx, fy_generic renderopts,
			     fy_generic data)
{
	const char *preamble;
	const char *title;
	size_t mdlen;
	char *md;
	FILE *mf;
	int rc;

	if (!fy_generic_is_mapping(data) && !fy_generic_is_sequence(data))
		return -1;
	md = NULL;
	mdlen = 0;
	mf = open_memstream(&md, &mdlen);
	if (!mf)
		return -1;
	title = fy_get(renderopts, "title", "");
	if (*title)
		fprintf(mf, "# %s\n\n", title);
	preamble = fy_get(renderopts, "preamble", "");
	if (*preamble)
		fprintf(mf, "%s\n\n", preamble);
	if (fy_generic_is_mapping(data))
		render_kv_table(mf, data, renderopts);
	else
		render_seq_table(mf, data, renderopts);
	fclose(mf);
	rc = fy_get(renderopts, "raw", false) ? -1 :
	     fyai_print_markdown(md, ctx->cfg);
	if (rc) {
		fputs(md, stdout);
		rc = 0;
	}
	free(md);
	return rc;
}
