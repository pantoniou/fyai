/* SPDX-License-Identifier: MIT */
#ifndef FYAI_RENDER_H
#define FYAI_RENDER_H

#include "fyai.h"

/*
 * Render @data as a Markdown table and print it through the configured
 * Markdown renderer (falling back to the raw text when rendering fails).
 *
 * @data is either a mapping with string keys - rendered as a two column
 * key/value table, one row per pair - or a sequence of such mappings,
 * rendered as one column per key and one row per item.
 *
 * @renderopts is an optional (fy_invalid for none) mapping:
 *
 *	title		string, emitted as a level 1 heading
 *	preamble	string, Markdown emitted verbatim between the title
 *			and the table (a heading, a lead paragraph, ...)
 *	raw		bool, emit the Markdown text verbatim instead of
 *			rendering it
 *	empty		string, italic note emitted when there are no rows
 *	keys		sequence of keys, selecting the columns and their
 *			order; without it every key of the data is a column,
 *			in first seen order
 *	key_header	two column mode: the key column header ("Metric")
 *	value_header	two column mode: the value column header ("Value")
 *	columns		mapping of data key to a per column override mapping:
 *
 *				name	column header; defaults to the key
 *					with underscores turned into spaces
 *					and the first letter capitalized
 *				align	"left", "right" or "center"; defaults
 *					to right for numbers and booleans,
 *					left for everything else
 *				format	"yesno" (default for booleans),
 *					"mark" (bool as "*" / ""), "join"
 *					(default for sequences), "humanize"
 *					(underscores to spaces, capitalized)
 *					or "plain"
 *
 * Cells are escaped and truncated, so no value can break the table.
 * Returns 0 on success, -1 on failure.
 */
int fyai_generic_to_markdown(struct fyai_ctx *ctx, fy_generic renderopts,
			     fy_generic data);

#endif
