/* SPDX-License-Identifier: MIT */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fyai.h"
#include "fyai_branch.h"
#include "fyai_query.h"
#include "fyai_storage.h"

#define FYAI_MODULE FYAIEM_UNKNOWN

static const char *const query_names[] = { "branches" };

const char *const *fyai_query_names(size_t *n)
{
	*n = ARRAY_SIZE(query_names);
	return query_names;
}

static fy_generic query_error(struct fy_generic_builder *gb, const char *code,
			      const char *message)
{
	fy_generic inner;

	inner = fy_mapping(gb, "code", code, "message", message);
	return fy_mapping(gb, "error", inner);
}

/* Encode the arena word as an opaque revision. */
static long long query_rev(fy_generic value)
{
	return (long long)value.v;
}

/* Read "REV:OFFSET". */
static bool query_cursor_parse(const char *text, long long *revp, size_t *offp)
{
	unsigned long long off;
	char *end;

	*revp = strtoll(text, &end, 10);
	if (end == text || *end != ':')
		return false;
	text = end + 1;
	off = strtoull(text, &end, 10);
	if (end == text || *end)
		return false;
	*offp = (size_t)off;
	return true;
}

/* Estimate the serialized size of a row. */
static size_t query_row_bytes(fy_generic row)
{
	static const char *const keys[] = { "branch", "cwd", "model",
					    "description" };
	size_t n = 96, i;

	for (i = 0; i < ARRAY_SIZE(keys); i++)
		n += fy_len(fy_get(row, keys[i], fy_invalid));
	return n;
}

static fy_generic query_branches(struct fy_generic_builder *gb,
				 const struct fyai_query_source *src,
				 fy_generic request)
{
	fy_generic args = fy_get(request, "params", fy_map_empty);
	fy_generic after = fy_get(request, "after", fy_invalid);
	fy_generic if_rev = fy_get(request, "if_rev", fy_invalid);
	fy_generic table = fy_is_mapping(src->branches) ? src->branches :
			   fy_map_empty;
	fy_generic dirv = fy_get(args, "dir", fy_invalid);
	fy_generic rows, row, next, result;
	const char *dir = fy_is_string(dirv) ? fy_castp(&dirv, "") : src->cwd;
	const char *text;
	long long rev = query_rev(table), cursor_rev, limit;
	size_t first = 0, total = 0, kept = 0, bytes = 0, n;
	char cursor[64];

	limit = fy_get(request, "limit", 0LL);
	if (limit < 1)
		limit = FYAI_QUERY_LIMIT_DEFAULT;
	if (limit > FYAI_QUERY_LIMIT_MAX)
		limit = FYAI_QUERY_LIMIT_MAX;
	if (fy_is_valid(after) && !fy_is_null(after)) {
		text = fy_is_string(after) ? fy_castp(&after, "") : NULL;
		if (!text || !query_cursor_parse(text, &cursor_rev, &first))
			return query_error(gb, FYAI_QUERY_BAD_REQUEST,
					   "the cursor is not one that an answer gave");
		if (cursor_rev != rev)
			return query_error(gb, FYAI_QUERY_STALE_CURSOR,
					   "the data changed since the cursor was made");
	} else if (fy_is_int(if_rev) && fy_get(request, "if_rev", 0LL) == rev) {
		return fy_mapping(gb, "unchanged", true, "rev", fy_value(gb, rev));
	}
	rows = fyai_branch_select_page(NULL, gb, table, dir,
				       fy_get(args, "all", false), first,
				       (size_t)limit, true, &total);
	/* Keep each page within the byte budget. */
	n = fy_len(rows);
	fy_foreach(row, rows) {
		if (kept && bytes + query_row_bytes(row) > FYAI_QUERY_BYTES_MAX)
			break;
		bytes += query_row_bytes(row);
		kept++;
	}
	if (n && !kept)
		return query_error(gb, FYAI_QUERY_TOO_LARGE,
				   "one row is larger than an answer may be");
	if (kept < n) {
		result = fy_seq_empty;
		for (n = 0; n < kept; n++)
			result = fy_append(gb, result, fy_get_at(rows, n));
		rows = result;
	}
	next = fy_null;
	if (first + kept < total) {
		snprintf(cursor, sizeof(cursor), "%lld:%zu", rev, first + kept);
		next = fy_value(gb, cursor);
	}
	return fy_mapping(gb, "what", "branches", "rows", rows, "next", next,
			  "total", fy_value(gb, (long long)total),
			  "rev", fy_value(gb, rev));
}

fy_generic fyai_query_answer(struct fy_generic_builder *gb,
			     const struct fyai_query_source *src,
			     fy_generic request)
{
	const char *what;

	if (!fy_is_mapping(request))
		return query_error(gb, FYAI_QUERY_BAD_REQUEST,
				   "a query is a mapping");
	what = fy_get(request, "what", "");
	if (!strcmp(what, "branches"))
		return query_branches(gb, src, request);
	return query_error(gb, FYAI_QUERY_UNSUPPORTED,
			   "this worker does not answer that query");
}

fy_generic fyai_query(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
		      fy_generic request)
{
	struct fyai_query_source src = { .branches = fy_invalid };
	char cwd[PATH_MAX];
	fy_generic root;

	/* The latest root, which another process may have published, and no
	 * change of the baseline of this context. */
	root = fyai_branches_snapshot(ctx);
	src.branches = fy_is_valid(root) ? fy_get(root, "branches", fy_invalid) :
		       ctx->bstate->branches;
	src.cwd = getcwd(cwd, sizeof(cwd)) ? cwd : NULL;
	return fyai_query_answer(gb, &src, request);
}
