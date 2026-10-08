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

/* Whether the arguments of an agent call ask for a background run. */
bool fyai_agent_background_requested(fy_generic args);

/*
 * Start the sub-agent of @args in the background and return the text that
 * says so, without waiting for it. The report reaches the model as an event
 * when the sub-agent ends. On failure, set *okp to false and return the
 * cause as the tool result.
 */
fy_generic fyai_agent_background(struct fyai_ctx *ctx, fy_generic args,
				 bool *okp);

/* Whether the background sub-agent @name has not ended. */
bool fyai_agent_background_running(struct fyai_ctx *ctx, const char *name);

/* Cancel and release the background sub-agents of this invocation. */
void fyai_agent_background_close(struct fyai_ctx *ctx);

/* Drop the background sub-agents of the parent in a forked child. */
void fyai_agent_background_abandon(struct fyai_ctx *ctx);

/* Return true when the child must suppress display output. */
bool fyai_agent_delegated(const struct fyai_ctx *ctx);

#endif
