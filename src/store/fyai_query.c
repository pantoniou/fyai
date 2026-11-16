/* SPDX-License-Identifier: MIT */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fyai.h"
#include "fyai_branch.h"
#include "fyai_agents.h"
#include "fyai_browser.h"
#include "fyai_cmd.h"
#include "fyai_query.h"
#include "fyai_turn.h"
#include "fyai_storage.h"

#define FYAI_MODULE FYAIEM_UNKNOWN

static const char *const query_names[] = { "branches", "branch_tree",
					   "branch_preview", "branch_turns",
					   "complete" };

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

/*
 * The rows of one query from @first, at most @count, and the number of rows
 * in all in *@totalp. They live in @gb.
 */
typedef fy_generic (*query_rows_fn)(struct fy_generic_builder *gb,
				    const struct fyai_query_source *src,
				    fy_generic params, size_t first,
				    size_t count, size_t *totalp);

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

/*
 * Answer a query of rows: the cursor, if_rev, the limits of rows and of
 * bytes. @rev is the generation of the data, and @extra the members that the
 * answer holds besides the page.
 */
static fy_generic query_paged(struct fy_generic_builder *gb,
			      const struct fyai_query_source *src,
			      fy_generic request, const char *what,
			      long long rev, fy_generic extra,
			      query_rows_fn rows_fn)
{
	fy_generic params = fy_get(request, "params", fy_map_empty);
	fy_generic after = fy_get(request, "after", fy_invalid);
	fy_generic if_rev = fy_get(request, "if_rev", fy_invalid);
	fy_generic rows, row, next, result, key, value;
	const char *text;
	long long cursor_rev, limit;
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
	rows = rows_fn(gb, src, params, first, (size_t)limit, &total);
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
	result = fy_mapping(gb, "what", what, "rows", rows, "next", next,
			    "total", fy_value(gb, (long long)total),
			    "rev", fy_value(gb, rev));
	fy_foreach_key_value(key, value, extra)
		result = fy_assoc(gb, result, fy_castp(&key, ""), value);
	return result;
}

static fy_generic branches_rows(struct fy_generic_builder *gb,
				const struct fyai_query_source *src,
				fy_generic params, size_t first, size_t count,
				size_t *totalp)
{
	fy_generic table = fy_is_mapping(src->branches) ? src->branches :
			   fy_map_empty;
	fy_generic dirv = fy_get(params, "dir", fy_invalid);
	const char *dir = fy_is_string(dirv) ? fy_castp(&dirv, "") : src->cwd;

	return fyai_branch_select_page(NULL, gb, table, dir,
				       fy_get(params, "all", false), first,
				       count, true, totalp);
}

static fy_generic query_branches(struct fy_generic_builder *gb,
				 const struct fyai_query_source *src,
				 fy_generic request)
{
	fy_generic table = fy_is_mapping(src->branches) ? src->branches :
			   fy_map_empty;

	return query_paged(gb, src, request, "branches", query_rev(table),
			   fy_invalid, branches_rows);
}

/* Project the browser row for @name. */
static fy_generic tree_row(struct fy_generic_builder *gb,
			   const struct fyai_query_source *src,
			   const char *name, fy_generic entry)
{
	struct fyai_branch b;
	fy_generic row, mapping, source, id, title, model, description;
	const char *state = src->state ? src->state(src->state_arg, name) : NULL;
	bool known = fy_is_valid(entry) && fyai_branch_decode(entry, &b);
	long long updated = known ? (long long)fyai_branch_updated(&b) : 0LL;
	/* A branch that only runs has no entry yet: it is an agent. */
	bool agent = known ? fy_is_valid(b.agent) : true;

	model = known ? fy_value(gb, fy_get(b.config, "model", "")) : fy_value(gb, "");
	description = known ? fy_value(gb, fy_castp(&b.description, "")) :
		      fy_value(gb, "");
	row = fy_mapping(gb, "branch", name, "model", model,
			 "description", description,
			 "agent", agent,
			 "updated", fy_value(gb, updated),
			 "state", state ? state : "");
	if (known && fy_is_mapping(b.import)) {
		source = fy_value(gb, fy_get(b.import, "source", ""));
		id = fy_value(gb, fy_get(b.import, "session_id", ""));
		title = fy_value(gb, fy_get(b.import, "title", ""));
		mapping = fy_mapping(gb, "source", source, "session_id", id,
				     "title", title);
		row = fy_assoc(gb, row, "import", mapping);
	}
	if (known)
		row = fy_assoc(gb, row, "rev", fy_value(gb, (long long)entry.v));
	return row;
}

static fy_generic tree_rows(struct fy_generic_builder *gb,
			    const struct fyai_query_source *src,
			    fy_generic params, size_t first, size_t count,
			    size_t *totalp)
{
	fy_generic table = fy_is_mapping(src->branches) ? src->branches :
			   fy_map_empty;
	fy_generic rows = fy_seq_empty, name, entry, live, branch;
	const char *nm;
	size_t idx = 0;

	(void)params;
	/* Every branch of the table, a stored sub-agent too, and then the live
	 * agents that the table does not hold yet. */
	fy_foreach_key_value(name, entry, table) {
		nm = fy_castp(&name, "");
		if (fy_str_empty(nm))
			continue;
		if (idx >= first && idx - first < count)
			rows = fy_append(gb, rows, tree_row(gb, src, nm, entry));
		idx++;
	}
	if (fy_is_sequence(src->live))
		fy_foreach(live, src->live) {
			branch = fy_get(live, "branch", fy_invalid);
			nm = fy_is_string(branch) ? fy_castp(&branch, "") : "";
			if (fy_str_empty(nm) ||
			    fy_is_valid(fy_get(table, nm, fy_invalid)))
				continue;
			if (idx >= first && idx - first < count)
				rows = fy_append(gb, rows,
						 tree_row(gb, src, nm, fy_invalid));
			idx++;
		}
	*totalp = idx;
	return rows;
}

static fy_generic query_tree(struct fy_generic_builder *gb,
			     const struct fyai_query_source *src,
			     fy_generic request)
{
	/* The root moves with HEAD, and the live agents have a generation: one
	 * word says that none of them moved. */
	fy_generic root = fy_is_valid(src->root) ? src->root : src->branches;
	long long rev = query_rev(root) +
			(long long)src->generation * 0x9e3779b97f4a7c15LL;
	fy_generic extra;

	extra = fy_mapping(gb, "head", src->head ? src->head : "");
	return query_paged(gb, src, request, "branch_tree", rev, extra,
			   tree_rows);
}

/* Estimate the size of a stored turn for the byte budget. */
static size_t turn_bytes(fy_generic wire)
{
	fy_generic item;
	size_t n = 64;

	fy_foreach(item, fy_get(wire, "display_outputs", fy_seq_empty))
		n += fy_len(fy_get(item, "markdown", fy_invalid)) +
		     fy_len(fy_get(item, "text", fy_invalid)) + 64;
	fy_foreach(item, fy_get(wire, "messages", fy_seq_empty))
		n += fy_len(fy_get(item, "content", fy_invalid)) +
		     fy_len(fy_get(item, "output", fy_invalid)) + 64;
	return n;
}

/*
 * The last turns of a branch, oldest first, as a client that has no arena
 * renders its history from them: `branch_turns` {branch, depth}. The `rev`
 * is the word of the entry of the branch, and `if_rev` answers a word.
 */
static fy_generic query_branch_turns(struct fy_generic_builder *gb,
				     const struct fyai_query_source *src,
				     fy_generic request)
{
	fy_generic params = fy_get(request, "params", fy_map_empty);
	fy_generic if_rev = fy_get(request, "if_rev", fy_invalid);
	fy_generic table = fy_is_mapping(src->branches) ? src->branches :
			   fy_map_empty;
	fy_generic turns = fy_seq_empty, wire, answer, display;
	struct fyai_turn_stack stack = { 0 };
	struct fyai_branch b;
	const char *name = fy_get(params, "branch", "");
	long long depth = fy_get(params, "depth", 8LL);
	size_t i, begin, bytes = 0;
	bool more;
	int rc;

	if (fy_str_empty(name))
		return query_error(gb, FYAI_QUERY_BAD_REQUEST,
				   "a query of turns names a branch");
	if (!fyai_branch_lookup(table, name, &b))
		return fy_mapping(gb, "what", "branch_turns", "found", false,
				  "turns", fy_seq_empty);
	if (fy_is_int(if_rev) && fy_get(request, "if_rev", 0LL) ==
	    (long long)b.entry.v)
		return fy_mapping(gb, "unchanged", true, "rev",
				  fy_value(gb, (long long)b.entry.v));
	if (depth < 1)
		depth = 1;
	if (depth > 64)
		depth = 64;
	rc = fyai_turn_stack_init(&stack, b.head, fy_invalid);
	if (rc)
		return query_error(gb, FYAI_QUERY_BAD_REQUEST,
				   "the turns of the branch cannot be read");
	begin = stack.count > (size_t)depth ? stack.count - (size_t)depth : 0;
	/* The newest turns stay when the answer would pass its limit. */
	for (i = stack.count; i > begin; i--) {
		wire = fyai_turn_wire(gb, stack.items[i - 1]);
		bytes += turn_bytes(wire);
		if (i < stack.count && bytes > FYAI_QUERY_BYTES_MAX) {
			begin = i;
			break;
		}
	}
	more = begin > 0;
	for (i = begin; i < stack.count; i++)
		turns = fy_append(gb, turns, fyai_turn_wire(gb, stack.items[i]));
	fyai_turn_stack_cleanup(&stack);
	/* Include the branch display settings for the client recap. */
	answer = fy_mapping(gb, "what", "branch_turns", "found", true, "turns",
			    turns, "more", more,
			    "rev", fy_value(gb, (long long)b.entry.v));
	display = fy_get(b.config, "display", fy_invalid);
	return fy_is_mapping(display) ?
	       fy_assoc(gb, answer, "display", display) : answer;
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
	if (!strcmp(what, "branch_tree"))
		return query_tree(gb, src, request);
	if (!strcmp(what, "branch_turns"))
		return query_branch_turns(gb, src, request);
	return query_error(gb, FYAI_QUERY_UNSUPPORTED,
			   "this worker does not answer that query");
}

static const char *query_agent_state(void *arg, const char *branch)
{
	return fyai_agents_state(arg, branch);
}

fy_generic fyai_query(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
		      fy_generic request)
{
	struct fyai_query_source src = { .branches = fy_invalid };
	char cwd[PATH_MAX];
	fy_generic root;

	/* Resolve completion candidates on the worker. */
	if (!strcmp(fy_get(request, "what", ""), "complete"))
		return fyai_cmd_complete_answer(ctx, gb, request);
	if (!strcmp(fy_get(request, "what", ""), "branch_preview"))
		return fyai_browser_preview_answer(ctx, gb, request);
	/* The latest root, which another process may have published, and no
	 * change of the baseline of this context. */
	root = fyai_branches_snapshot(ctx);
	src.branches = fy_is_valid(root) ? fy_get(root, "branches", fy_invalid) :
		       ctx->bstate->branches;
	src.cwd = getcwd(cwd, sizeof(cwd)) ? cwd : NULL;
	/* The tree of the browser: HEAD, and the agents that run. */
	src.root = root;
	src.head = fy_is_valid(root) ? fy_get(root, "HEAD", (const char *)NULL) :
		   NULL;
	if (ctx->astate) {
		src.live = fyai_agents_rows(ctx, gb);
		src.generation = fyai_agents_generation(ctx);
		src.state = query_agent_state;
		src.state_arg = ctx;
	}
	return fyai_query_answer(gb, &src, request);
}
