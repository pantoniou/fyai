/* SPDX-License-Identifier: MIT */
#ifndef FYAI_ACP_H
#define FYAI_ACP_H

#include <stddef.h>

struct fyai_ctx;

/* Report provider text from an ACP worker without terminal presentation. */
void fyai_acp_worker_text(struct fyai_ctx *ctx, const char *text, size_t len);
int fyai_acp_worker_tool(struct fyai_ctx *ctx, const char *tool, bool ok,
			 const char *cause);

#endif
