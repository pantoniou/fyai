/* SPDX-License-Identifier: MIT */
#ifndef FYAI_TOOL_SPEC_H
#define FYAI_TOOL_SPEC_H

#include "fyai.h"

/* Return the built-in tools from data/tools.yaml. */
fy_generic make_tools(struct fyai_ctx *ctx);

/* Return the built-in tools that the context can use. */
fy_generic make_tools_filtered(struct fyai_ctx *ctx);

#endif
