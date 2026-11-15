/*
 * fyai_scope.c - the scope of an exchange and the processes that it keeps
 *
 * SPDX-License-Identifier: MIT
 */
#define FYAI_MODULE FYAIEM_UNKNOWN

#include "config.h"

#include <stdbool.h>

#include "fyai.h"
#include "fyai_cover.h"
#include "fyai_orphan.h"
#include "fyai_scope.h"

/* End the scope when no exchange runs and no object keeps it. */
static void fyai_scope_settle(struct fyai_scope *scope)
{
	if (scope->open || scope->pins > 0 ||
	    scope->cfg->sandbox_scope_kind == FYAI_SCOPE_SESSION)
		return;
	/* A process that did not start the holder does not end it. */
	if (scope->cover.owner) {
		fyai_diag_tracef("scope", "end, holder %ld", (long)scope->cover.holder);
		fyai_cover_release(&scope->cover);
	}
	if (fyai_orphan_count(scope))
		fyai_diag_tracef("scope", "stop %zu tracked daemons",
				 fyai_orphan_count(scope));
	fyai_orphan_sweep(scope);
}

void fyai_scope_exchange(struct fyai_scope *scope, bool begin)
{
	scope->open = begin;
	if (!begin)
		fyai_scope_settle(scope);
}

void fyai_scope_pin(struct fyai_scope *scope, struct fyai_scope_pin *pin)
{
	if (pin->held)
		return;
	pin->held = true;
	scope->pins++;
}

void fyai_scope_unpin(struct fyai_scope *scope, struct fyai_scope_pin *pin)
{
	if (!pin->held)
		return;
	pin->held = false;
	/* A forked child resets the count and drops the objects of its parent. */
	if (scope->pins > 0)
		scope->pins--;
	fyai_scope_settle(scope);
}

/* The processes of the scope: the ones its holder holds, and the ones that it tracks. */
fy_generic fyai_scope_rows(struct fyai_scope *scope, struct fy_generic_builder *gb)
{
	pid_t pids[64];
	char name[32], label[64];
	size_t n, i;
	fy_generic rows;

	rows = fyai_orphan_rows(scope, gb);
	n = fyai_cover_members(&scope->cover, pids, ARRAY_SIZE(pids));
	for (i = 0; i < n; i++) {
		fyai_orphan_name(pids[i], name, sizeof(name));
		fyai_orphan_label(pids[i], label, sizeof(label));
		rows = fy_append(gb, rows, fy_mapping(gb, "pid", (long long)pids[i],
						      "name", fy_value(gb, name),
						      "label", fy_value(gb, label),
						      "held", "scope"));
	}
	return rows;
}

