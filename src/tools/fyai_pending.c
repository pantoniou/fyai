/*
 * fyai_pending.c - background work in the branch store
 *
 * SPDX-License-Identifier: MIT
 */
#define FYAI_MODULE FYAIEM_TOOLS
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fyai.h"
#include "fyai_agent.h"
#include "fyai_branch.h"
#include "fyai_host.h"
#include "fyai_monitor.h"
#include "fyai_pending.h"
#include "fyai_storage.h"
#include "fyai_wait.h"
#include "fyai_waitq.h"

/* The owner of a record: the process that made it. */
static void pending_owner_self(long long *pid, long long *start)
{
	uint64_t t = 0;

	*pid = (long long)getpid();
	*start = fyai_host_process_start(getpid(), &t) ? 0 : (long long)t;
}

static bool pending_owner_ours(fy_generic row)
{
	long long pid, start;

	pending_owner_self(&pid, &start);
	return fy_get(row, "pid", 0LL) == pid && fy_get(row, "start", 0LL) == start;
}

/* Whether the process that wrote @row still runs. */
static bool pending_owner_alive(fy_generic row)
{
	long long pid = fy_get(row, "pid", 0LL), start = fy_get(row, "start", 0LL);
	uint64_t now;
	int rc;

	if (pid <= 0)
		return false;
	if (pid == (long long)getpid())
		return true;
	if (kill((pid_t)pid, 0) && errno == ESRCH)
		return false;
	rc = fyai_host_process_start((pid_t)pid, &now);
	if (rc == -ESRCH)
		return false;
	/* A process of the same number that started later is another one. */
	return rc || !start || (long long)now == start;
}

static fy_generic pending_stored(struct fyai_ctx *ctx)
{
	struct fyai_branch b;

	if (fy_is_invalid(ctx->bstate->prev) ||
	    !fyai_branch_decode(ctx->bstate->prev, &b) ||
	    !fy_is_sequence(b.pending) || fy_empty(b.pending))
		return fy_invalid;
	return b.pending;
}

/* The work of this run, each record with its owner. */
static fy_generic pending_rows(struct fyai_ctx *ctx, struct fy_generic_builder *gb)
{
	fy_generic rows, row, out;
	long long pid, start;

	pending_owner_self(&pid, &start);
	/* Events first: they came before the work that restarts. */
	rows = fyai_events_pending(ctx->waitq, gb);
	fy_foreach(row, fyai_waits_pending(ctx->waitq, gb))
		rows = fy_append(gb, rows, row);
	fy_foreach(row, fyai_monitors_pending(ctx, gb))
		rows = fy_append(gb, rows, row);
	fy_foreach(row, fyai_agents_pending(ctx, gb))
		rows = fy_append(gb, rows, row);
	out = fy_sequence(gb);
	fy_foreach(row, rows)
		out = fy_append(gb, out, fy_assoc(gb, fy_assoc(gb, row, "pid", pid),
						  "start", start));
	return out;
}

/*
 * @take: the stored table is that of a lost owner, which this run takes over,
 * so an empty set of work removes it.
 */
static void pending_stage(struct fyai_ctx *ctx, bool take)
{
	struct fy_generic_builder *gb;
	fy_generic stored, rows, kept;

	if (!ctx->cfg->durable || !ctx->arena->durable_gb)
		return;
	gb = fyai_ctx_transient_gb(ctx);
	if (!gb)
		return;
	stored = pending_stored(ctx);
	rows = pending_rows(ctx, gb);
	if (fy_empty(rows)) {
		if (fy_is_valid(stored) &&
		    (take || pending_owner_ours(fy_get_at(stored, 0))))
			ctx->bstate->pending = fy_null;
		return;
	}
	kept = fy_gb_internalize(ctx->arena->gb, rows);
	if (fy_is_invalid(kept) ||
	    (fy_is_valid(stored) && fy_equal(stored, kept)))
		return;
	ctx->bstate->pending = kept;
}

void fyai_pending_stage(struct fyai_ctx *ctx)
{
	pending_stage(ctx, false);
}

static void pending_publish(struct fyai_ctx *ctx)
{
	fyai_branch_op_set(ctx, FYAI_BRANCH_OP_AMEND, NULL);
	/* The publish reported a failure; the caller drains it. */
	(void)fyai_publish_state(ctx);
}

int fyai_pending_restore(struct fyai_ctx *ctx)
{
	fy_generic stored, row, name, reason, gkind;
	const char *kind;
	char *text;
	bool failed = false;

	if (!ctx->cfg->durable)
		return 0;
	stored = pending_stored(ctx);
	if (fy_is_invalid(stored) || pending_owner_alive(fy_get_at(stored, 0)))
		return 0;
	/* The commands that restart build their arguments in the transient builder. */
	if (fyai_setup_transient_builder(ctx)) {
		fyai_error(ctx, "durable: could not make room to restart the work");
		return -1;
	}
	fy_foreach(row, stored) {
		gkind = fy_get(row, "kind", fy_invalid);
		kind = fy_castp(&gkind, "");
		if (!strcmp(kind, "event")) {
			name = fy_get(row, "text", fy_invalid);
			if (!fy_is_string(name) ||
			    fyai_event_inject(ctx->waitq, strdup(fy_castp(&name, ""))))
				failed = true;
			continue;
		}
		name = fy_get(row, "name", fy_invalid);
		if (!fy_is_string(name))
			continue;
		if (!strcmp(kind, "wait")) {
			reason = fy_get(row, "reason", fy_invalid);
			text = fyai_wait_restore(ctx->waitq, fy_castp(&name, ""),
						 fy_is_string(reason) ?
						 fy_castp(&reason, "") : NULL,
						 fy_get(row, "due", 0LL));
			if (!text)
				failed = true;
			free(text);
		} else if (!strcmp(kind, "agent")) {
			if (fyai_agent_restore(ctx, row))
				failed = true;
		} else if (fyai_monitor_restore(ctx, row)) {
			failed = true;
		}
	}
	pending_stage(ctx, true);
	pending_publish(ctx);
	return failed ? -1 : 0;
}

void fyai_pending_clear(struct fyai_ctx *ctx)
{
	fy_generic stored;

	if (!ctx->cfg->durable)
		return;
	stored = pending_stored(ctx);
	if (fy_is_invalid(stored) || !pending_owner_ours(fy_get_at(stored, 0)))
		return;
	pending_publish(ctx);
}
