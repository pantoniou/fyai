/* SPDX-License-Identifier: MIT */
#ifndef FYAI_PENDING_H
#define FYAI_PENDING_H

#include <stdbool.h>

#include <libfyaml.h>
#include <libfyaml/libfyaml-generic.h>

struct fyai_ctx;

/*
 * Background work that survives a lost process. With `durable`, each publish
 * records the named waits, the monitors and the commands in the background of
 * the run in the branch store (`pending`), with the process that owns them.
 * The table exists only while its owner lives: a clean end of the run removes
 * it. A run that finds a table with a dead owner takes the work over.
 */

/*
 * Stage the table for the next publish: the work of this run, or a removal
 * when this run owned the stored table and has no work left. A publish calls
 * it; nothing else needs to.
 */
void fyai_pending_stage(struct fyai_ctx *ctx);

/*
 * Take over the work of a table that a lost process left on the branch: open
 * each wait again with the time that remains and start each monitor and
 * background command again, and say so to the model as events. A table whose
 * owner lives is left alone. Returns 0, or -1 with the cause reported.
 */
int fyai_pending_restore(struct fyai_ctx *ctx);

/*
 * Whether the process that wrote the record @row ({pid, start}) still runs. A
 * process of the same number that started later is another one.
 */
bool fyai_pending_owner_alive(fy_generic row);

/* At the end of a run: remove the table that this run owns. Call it after the work is released. */
void fyai_pending_clear(struct fyai_ctx *ctx);

#endif
