/* SPDX-License-Identifier: MIT */
#ifndef FYAI_AGENTS_H
#define FYAI_AGENTS_H

#include "fyai.h"

struct jsonrpc_conn;
struct fytim_surface;

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
const char *fyai_agents_attached(const struct fyai_ctx *ctx);
const char *fyai_agents_model(const struct fyai_ctx *ctx);
/*
 * Read the model, execution id, and start time of @branch from the registry.
 * The model is borrowed and is valid until the record changes. Return false
 * and set NULL and zero values when the registry has no record.
 */
bool fyai_agents_branch_identity(struct fyai_ctx *ctx, const char *branch,
				 const char **model, long long *execution,
				 long long *started_ms);
const char *fyai_agents_zoom(struct fyai_ctx *ctx, const char *name, bool attach);
void fyai_agents_detach(struct fyai_ctx *ctx);
bool fyai_agents_input(struct fyai_ctx *ctx, const char *line);
int fyai_agents_kill(struct fyai_ctx *ctx, const char *name);
bool fyai_agents_surface(struct fyai_ctx *ctx, const struct fytim_surface *sf);
bool fyai_agents_keys(struct fyai_ctx *ctx, const char *data, size_t len);
void fyai_agents_present(struct fyai_ctx *ctx);
unsigned int fyai_agents_detail(unsigned int relative_depth);

#endif
