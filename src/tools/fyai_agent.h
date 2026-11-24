/* SPDX-License-Identifier: MIT */
#ifndef FYAI_AGENT_H
#define FYAI_AGENT_H

#include "fyai.h"

extern const char fyai_agent_system_prompt[];

/* Return the sub-agent report, or fy_invalid after an error. */
fy_generic fyai_agent_run(struct fyai_ctx *ctx, fy_generic args, bool *okp);
/* Reopen the arena for a sub-agent call. Return 0, or -1 after an error. */
int fyai_agent_reopen_for_call(struct fyai_ctx *ctx, fy_generic *call);

int fyai_agent_verb(struct fyai_ctx *ctx);

/*
 * Resolve @persona against the configuration of @ctx into @out, as a
 * sub-agent does when it starts, without changing that configuration. Return
 * 0, or -1 with a diagnostic.
 */
int fyai_agent_persona_cfg(struct fyai_ctx *ctx, fy_generic persona,
			   bool fork_mode, struct fyai_cfg *out);

struct fyai_sandbox_path;

/*
 * The paths that an agent runtime of @ctx needs beyond its view: the sandbox
 * grants of the grammar of its configuration and of each persona, since a
 * sub-agent picks its persona after it enters the view. Each path is given
 * once. *@out is a malloc'd array of malloc'd paths, NULL when there is none;
 * free it with fyai_agent_runtime_grants_free(). Return 0, or -1 with a
 * diagnostic.
 */
int fyai_agent_runtime_grants(struct fyai_ctx *ctx,
			      struct fyai_sandbox_path **out, size_t *n);
void fyai_agent_runtime_grants_free(struct fyai_sandbox_path *grants, size_t n);

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
/* Borrow the reported name for an original or listed background handle. */
const char *fyai_agent_background_original_name(struct fyai_ctx *ctx,
						 const char *name);

/* Serialize active background sub-agents into @gb for the pending store. */
fy_generic fyai_agents_pending(struct fyai_ctx *ctx, struct fy_generic_builder *gb);
/* Restart a background sub-agent from @record, reviving its branch. */
int fyai_agent_restore(struct fyai_ctx *ctx, fy_generic record);

/* Cancel and release the background sub-agents of this invocation. */
void fyai_agent_background_close(struct fyai_ctx *ctx);

/* Drop the background sub-agents of the parent in a forked child. */
void fyai_agent_background_abandon(struct fyai_ctx *ctx);

/* Return true when the child must suppress display output. */
bool fyai_agent_delegated(const struct fyai_ctx *ctx);

#endif
