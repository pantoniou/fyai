/*
 * fyai_cmd_query.c - the `query` command
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * Ask the owner of the arena for a page of rows (src/store/fyai_query.c). The
 * process that owns the arena answers at once. A client of a worker asks the
 * worker, and the command ends when the answer comes.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <string.h>

#include "fyai.h"
#include "fyai_cmd.h"
#include "fyai_cmd_int.h"
#include "fyai_uimodel.h"

#define FYAI_MODULE FYAIEM_UNKNOWN

struct query_state {
	struct fyai_cmd_call *call;
	unsigned long long id;		/* the query in flight, or 0 */
	bool answering;			/* the handler has not returned yet */
	bool answered;
	fy_generic answer;
};

/* The call is in flight: it ends with the answer. */
static void query_finish(struct fyai_cmd_call *call, fy_generic answer)
{
	fy_generic error = fy_get(answer, "error", fy_invalid);
	fy_generic result;

	if (fy_is_mapping(error)) {
		fyai_error(fyai_cmd_call_ctx(call), "query: %s: %s",
			   fy_get(error, "code", "failed"),
			   fy_get(error, "message", "no cause"));
		fyai_cmd_done(call, -1, fy_invalid);
		return;
	}
	result = fy_gb_internalize(call->gb, answer);
	fyai_cmd_done(call, fy_is_valid(result) ? 0 : -1, result);
}

static void query_answer(void *arg, fy_generic answer)
{
	struct query_state *st = arg;

	st->id = 0;
	/* The answer is borrowed for this call. */
	if (st->answering) {
		st->answer = fy_gb_internalize(st->call->gb, answer);
		st->answered = true;
		return;
	}
	query_finish(st->call, answer);
}

static void query_cancel(struct fyai_cmd_call *call)
{
	struct query_state *st = call->priv;

	if (st && st->id)
		fyai_uimodel_query_cancel(fyai_cmd_call_ctx(call), st->id);
}

static void query_cleanup(struct fyai_cmd_call *call)
{
	free(call->priv);
	call->priv = NULL;
}

int fyai_cmd_query(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = fyai_cmd_call_ctx(call);
	struct query_state *st;
	fy_generic args, request, error;
	const char *text;
	int rc;

	args = fy_mapping(call->gb, "all", fyai_cmd_arg_bool(call, "all"));
	text = fyai_cmd_arg_str(call, "dir");
	if (text)
		args = fy_assoc(call->gb, args, "dir", text);
	request = fy_mapping(call->gb, "what", fyai_cmd_arg_str(call, "what"),
			     "params", args);
	text = fyai_cmd_arg_str(call, "after");
	if (text)
		request = fy_assoc(call->gb, request, "after", text);
	if (fy_get(call->args, "limit", 0LL) > 0)
		request = fy_assoc(call->gb, request, "limit",
				   fy_get(call->args, "limit", 0LL));
	if (fy_is_valid(fy_get(call->args, "if_rev", fy_invalid)))
		request = fy_assoc(call->gb, request, "if_rev",
				   fy_get(call->args, "if_rev", 0LL));
	st = calloc(1, sizeof(*st));
	fyai_error_check(ctx, st, err, "query: cannot keep the query");
	st->call = call;
	st->answering = true;
	call->priv = st;
	rc = fyai_uimodel_query(ctx, request, query_answer, st, &st->id);
	st->answering = false;
	if (rc)			/* the model says why */
		goto err_state;
	if (st->answered) {
		error = fy_get(st->answer, "error", fy_invalid);
		fyai_error_check(ctx, !fy_is_mapping(error), err_state,
				 "query: %s: %s", fy_get(error, "code", "failed"),
				 fy_get(error, "message", "no cause"));
		*result = st->answer;
		query_cleanup(call);
		return 0;
	}
	call->cancel = query_cancel;
	call->cleanup = query_cleanup;
	return FYAI_CMD_PENDING;
err_state:
	query_cleanup(call);
err:
	return -1;
}
