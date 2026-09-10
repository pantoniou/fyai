/* SPDX-License-Identifier: MIT */
#ifndef FYAI_AGENTS_H
#define FYAI_AGENTS_H

#include "fyai.h"

struct jsonrpc_conn;

/* Called by a tool child from its top-level loop, before running an agent. */
int fyai_agents_enter(struct fyai_ctx *ctx);
void fyai_agents_leave(struct fyai_ctx *ctx, bool ok);
void fyai_agents_activity(struct fyai_ctx *ctx, const char *state);
void fyai_agents_output(struct fyai_ctx *ctx);
bool fyai_agents_serve(struct fyai_ctx *ctx, struct jsonrpc_conn *conn,
		       const char *method, fy_generic params, fy_generic id,
		       fy_generic *result, fy_generic *error);
/* Settle everything that named @conn; call it when that route has gone. */
void fyai_agents_conn_closed(struct fyai_ctx *ctx, struct jsonrpc_conn *conn);
void fyai_agents_cleanup(struct fyai_ctx *ctx);
fy_generic fyai_agents_rows(struct fyai_ctx *ctx, struct fy_generic_builder *gb);
unsigned long fyai_agents_generation(const struct fyai_ctx *ctx);
const char *fyai_agents_state(struct fyai_ctx *ctx, const char *branch);
bool fyai_agents_ambiguous(struct fyai_ctx *ctx, const char *name);
void fyai_agents_present(struct fyai_ctx *ctx);
unsigned int fyai_agents_detail(unsigned int relative_depth);

#endif
