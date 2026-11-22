/* SPDX-License-Identifier: MIT */
#ifndef FYAI_PENDING_H
#define FYAI_PENDING_H

struct fyai_ctx;

/*
 * Crash-resilient persistence for background work. Under `durable`, each
 * publish saves active waits, monitors, background commands, sub-agents, and
 * queued events in the branch `pending` sequence, tagged with owner {pid,
 * start}. Clean shutdown clears it; a later process restores pending work from
 * a dead process.
 */

/*
 * Stage the current pending table (or fy_null to clear our empty table) for
 * the next publish. Called internally by fyai_publish_state().
 */
void fyai_pending_stage(struct fyai_ctx *ctx);

/*
 * Restore pending work from a dead process on the current branch: re-arm waits
 * with remaining duration, restart monitors/commands/sub-agents, and inject
 * notification events. No-op if the recorded owner is still alive.
 * Returns 0 on success, or -1 if any item failed to restore (with diag).
 */
int fyai_pending_restore(struct fyai_ctx *ctx);

/* Clear our owned pending table at shutdown; call after releasing work. */
void fyai_pending_clear(struct fyai_ctx *ctx);

#endif
