/* SPDX-License-Identifier: MIT */
#ifndef FYAI_WAIT_H
#define FYAI_WAIT_H

#include <stdbool.h>

#include <libfyaml/libfyaml-generic.h>

struct fyai_ctx;

/* The `time` tool: what the clock says now. */
char *fyai_time_tool(struct fyai_ctx *ctx, bool *okp);
/* The same text, for anything else that has to say what the time is. */
char *fyai_time_now_text(void);

/* Wait synchronously, or schedule a named asynchronous wait. */
char *fyai_wait_tool(struct fyai_ctx *ctx, fy_generic args, bool *okp);

/* Cancel the pending wait @name; false when no such wait is pending. */
bool fyai_wait_cancel(struct fyai_ctx *ctx, const char *name);

/* True when a named wait @name is pending. */
bool fyai_wait_exists(struct fyai_ctx *ctx, const char *name);

/*
 * A wait for an event (`for`) that holds a tool call. Waiters run beside each
 * other, and an event that several of them wait for reaches each of them.
 */
struct fyai_waiter;

/* Whether the arguments of a wait ask to wait for an event. */
bool fyai_wait_for_requested(fy_generic args);
/* The registry test: a wait with no `for` stays in the parent. */
bool fyai_wait_in_parent(fy_generic args);
/*
 * Start a waiter. @complete runs from the event loop when the waiter ends,
 * but not when it ends at once: read fyai_waiter_done() after the start.
 * Return NULL with the cause in *errp (malloc'd) when it cannot start.
 */
struct fyai_waiter *fyai_waiter_start(struct fyai_ctx *ctx, fy_generic args,
				      void (*complete)(void *userdata),
				      void *userdata, char **errp);
bool fyai_waiter_done(const struct fyai_waiter *w);
/* Take the report of an ended waiter; the caller frees it. */
char *fyai_waiter_result(struct fyai_waiter *w, bool *okp);
/* End a waiter that is active, with an interruption report. */
void fyai_waiter_cancel(struct fyai_waiter *w);
void fyai_waiter_destroy(struct fyai_waiter *w);
/*
 * Something a waiter can wait for changed: look at the waiters on the next
 * pass of the loop. An event that is queued does this itself.
 */
void fyai_waiters_kick(struct fyai_ctx *ctx);
/* Drop every waiter at the end of the invocation. */
void fyai_waiters_release(struct fyai_ctx *ctx);
/* Drop the waiters of the parent in a forked child, without ending them. */
void fyai_waiters_abandon(struct fyai_ctx *ctx);

/* True while a named wait is pending. */
bool fyai_wait_pending(const struct fyai_ctx *ctx);

/*
 * The pending waits of this run, as rows of name, reason and the seconds that
 * remain. A wait belongs to the run that made it: a child has none of its parent.
 */
fy_generic fyai_waits_rows(struct fyai_ctx *ctx, struct fy_generic_builder *gb);

/* Drop every wait. They live for one invocation, as a session does. */
void fyai_waits_release(struct fyai_ctx *ctx);

/* Drop the waits of the parent in a forked child, without ending them. */
void fyai_waits_abandon(struct fyai_ctx *ctx);

#endif
