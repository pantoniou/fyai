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

/* True while a named wait is pending. */
bool fyai_wait_pending(const struct fyai_ctx *ctx);

/* Drop every wait. They live for one invocation, as a session does. */
void fyai_waits_release(struct fyai_ctx *ctx);

/* Drop the waits of the parent in a forked child, without ending them. */
void fyai_waits_abandon(struct fyai_ctx *ctx);

#endif
