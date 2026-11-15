/*
 * fyai_cmd_view.c - handlers of the conversation views
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai.h"
#include "fyai_cmd.h"
#include "fyai_cmd_int.h"
#include "fyai_display.h"

#define FYAI_MODULE FYAIEM_DISPLAY

/*
 * The exchanges to show: at most one of --first, --last, or --range, or the
 * session words `all`, `first N`, `last N`, and `range A,B`.
 */
int fyai_cmd_turn_selection(struct fyai_cmd_call *call,
			    struct fyai_turn_selector_args *sel)
{
	fy_generic words, w;
	const char *kind, *range, *count;
	unsigned long a, b;
	char *end;
	size_t n;
	int given;

	memset(sel, 0, sizeof(*sel));
	sel->type = FYAITST_ALL;
	kind = NULL;
	count = NULL;
	range = fyai_cmd_arg_str(call, "range");
	given = !!range;
	if (fy_is_valid(fy_get(call->args, "first", fy_invalid))) {
		sel->type = FYAITST_FIRST;
		sel->first = fy_get(call->args, "first", 0LL);
		given++;
	}
	if (fy_is_valid(fy_get(call->args, "last", fy_invalid))) {
		sel->type = FYAITST_LAST;
		sel->last = fy_get(call->args, "last", 0LL);
		given++;
	}
	words = fy_get(call->args, "select", fy_invalid);
	n = 0;
	fy_foreach(w, words) {
		if (!n)
			kind = fy_gb_intern_string(call->gb, fy_castp(&w, ""));
		else if (n == 1)
			count = fy_gb_intern_string(call->gb, fy_castp(&w, ""));
		n++;
	}
	if (n)
		given++;
	fyai_error_check(fyai_cmd_call_ctx(call), given <= 1, usage, "%s: give one "
			 "selection", call->path);
	if (!n && !range)
		return 0;
	if (n) {
		fyai_error_check(fyai_cmd_call_ctx(call), n <= 2, usage, "%s: too many words",
				 call->path);
		if (!strcmp(kind, "all") && n == 1)
			return 0;
		fyai_error_check(fyai_cmd_call_ctx(call), n == 2, usage,
				 "%s: '%s' needs a count", call->path, kind);
		if (!strcmp(kind, "range")) {
			range = count;
		} else {
			a = strtoul(count, &end, 10);
			fyai_error_check(fyai_cmd_call_ctx(call), *count && !*end, usage,
					 "%s: invalid count '%s'", call->path,
					 count);
			if (!strcmp(kind, "first")) {
				sel->type = FYAITST_FIRST;
				sel->first = a;
			} else if (!strcmp(kind, "last")) {
				sel->type = FYAITST_LAST;
				sel->last = a;
			} else {
				goto usage;
			}
			return 0;
		}
	}
	a = strtoul(range, &end, 10);
	fyai_error_check(fyai_cmd_call_ctx(call), end != range && (*end == ',' ||
						      *end == ':'),
			 usage, "%s: invalid range '%s'", call->path, range);
	range = end + 1;
	b = strtoul(range, &end, 10);
	fyai_error_check(fyai_cmd_call_ctx(call), end != range && !*end, usage,
			 "%s: invalid range", call->path);
	sel->type = FYAITST_RANGE;
	sel->range_lo = a;
	sel->range_hi = b;
	return 0;
usage:
	fyai_error(fyai_cmd_call_ctx(call), "%s: use all, first N, last N, or range A,B",
		   call->path);
	return -1;
}

static int history_emit_turn(void *arg, fy_generic turn)
{
	struct fyai_cmd_call *call = arg;
	fy_generic m;

	fy_foreach(m, fy_get(turn, "messages", fy_seq_empty))
		if (fyai_cmd_emit(call, m))
			return -1;
	return 0;
}

/*
 * A rendered transcript goes through the one transcript renderer. The
 * machine formats stream the stored messages, one document each.
 */
int fyai_cmd_history(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = fyai_cmd_call_ctx(call);
	struct fyai_display_args *args = &ctx->cfg->cmd.args.display;
	struct fyai_display_args saved = *args;
	struct fyai_turn_selector_args sel;
	int rc;

	(void)result;
	if (fyai_cmd_turn_selection(call, &sel))
		return -1;
	if (call->format != FYAI_CMD_OUT_MARKDOWN)
		return fyai_display_foreach_turn(ctx, &sel, history_emit_turn,
						 call);
	args->raw = fyai_cmd_arg_bool(call, "raw");
	args->tool_detail = fyai_cmd_arg_str(call, "tool_detail");
	args->turn_sel = sel;
	rc = fyai_display_view(ctx);
	*args = saved;
	return rc;
}
