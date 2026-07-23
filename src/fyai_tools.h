/* SPDX-License-Identifier: MIT */
#ifndef FYAI_TOOLS_H
#define FYAI_TOOLS_H

#include "fyai.h"

void fyai_print_tool_call(struct fyai_ctx *ctx, fy_generic tool_call);
fy_generic fyai_execute_tool_call(struct fyai_ctx *ctx, fy_generic tool_call,
				  bool *okp);

int fyai_mcp_refresh(struct fyai_ctx *ctx);
fy_generic fyai_mcp_tools(struct fyai_ctx *ctx);
bool fyai_mcp_tool_name(const char *name);
fy_generic fyai_mcp_call(struct fyai_ctx *ctx, const char *name,
			 fy_generic args);
void fyai_mcp_cleanup(struct fyai_ctx *ctx);

/*
 * Execute a single named built-in tool (read_file, write_file, apply_patch,
 * shell, ask_user) with already-parsed @args, returning the result generic in
 * ctx->transient_gb. Shared by the model tool-use loop, the fork-per-tool
 * sandbox path, and the `fyai tool` verb.
 */
fy_generic fyai_tool_run_one(struct fyai_ctx *ctx, const char *name,
			     fy_generic args, bool *okp);

/*
 * `fyai tool <name> [json]` verb: a one-shot sandboxed tool sub-execution of
 * self. Sets up a transient builder, parses the JSON arguments (from argv or
 * stdin), sanitizes the environment, applies the arena's sandbox policy to this
 * process irreversibly, runs the single tool, and prints the result. Because
 * the process is the confined context, no fork is needed.
 */
int fyai_run_tool_verb(struct fyai_ctx *ctx);

#endif
