/* SPDX-License-Identifier: MIT */
#ifndef FYAI_AGENT_H
#define FYAI_AGENT_H

#include "fyai.h"

extern const char fyai_agent_system_prompt[];

/* Return the sub-agent report, or fy_invalid after an error. */
fy_generic fyai_agent_run(struct fyai_ctx *ctx, fy_generic args, bool *okp);

int fyai_agent_verb(struct fyai_ctx *ctx);

/*
 * Resolve @persona against the configuration of @ctx into @out, as a
 * sub-agent does when it starts, without changing that configuration. Return
 * 0, or -1 with a diagnostic.
 */
int fyai_agent_persona_cfg(struct fyai_ctx *ctx, fy_generic persona,
			   bool fork_mode, struct fyai_cfg *out);

/*
 * Adopt the configuration that a parent sent to an executed sub-agent child.
 * The call arguments are parsed in the grammar of that configuration.
 */
int fyai_agent_spawn_config(struct fyai_ctx *ctx, fy_generic spawn);

/* Return true when the child must suppress display output. */
bool fyai_agent_delegated(const struct fyai_ctx *ctx);

#endif
