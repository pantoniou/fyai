/* SPDX-License-Identifier: MIT */
#ifndef FYAI_MONITOR_H
#define FYAI_MONITOR_H

#include <stdbool.h>
#include <stddef.h>

#include <libfyaml/libfyaml-generic.h>

struct fyai_ctx;

/*
 * Give the complete lines that the monitor @name printed to the model as one
 * event, and keep a partial line until its end arrives. The owner of the job
 * that runs the command calls it for each chunk of output.
 */
void fyai_monitor_output(struct fyai_ctx *ctx, const char *name,
			 const char *data, size_t len);

/* Whether the monitor @name has not ended. */
bool fyai_monitor_running(struct fyai_ctx *ctx, const char *name);

/* Stop the monitor @name without an end event; false when none is running. */
bool fyai_monitor_cancel(struct fyai_ctx *ctx, const char *name);

/* The running monitors as rows of name and events so far. */
fy_generic fyai_monitors_rows(struct fyai_ctx *ctx,
			      struct fy_generic_builder *gb);

/* Cancel and release every monitor of this invocation. */
void fyai_monitor_close(struct fyai_ctx *ctx);

/* Drop the monitors of the parent in a forked child, without ending them. */
void fyai_monitor_abandon(struct fyai_ctx *ctx);

#endif
