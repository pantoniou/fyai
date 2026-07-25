/* SPDX-License-Identifier: MIT */
#ifndef FYAI_AGENT_H
#define FYAI_AGENT_H

#include "fyai.h"

extern const char fyai_agent_system_prompt[];

/* Return the sub-agent report, or fy_invalid after an error. */
fy_generic fyai_agent_run(struct fyai_ctx *ctx, const char *task, bool *okp);

int fyai_agent_verb(struct fyai_ctx *ctx);

/* Return true when the child must suppress display output. */
bool fyai_agent_delegated(const struct fyai_ctx *ctx);

#endif
