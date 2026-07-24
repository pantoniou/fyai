/* SPDX-License-Identifier: MIT */
#ifndef FYAI_TOOLS_H
#define FYAI_TOOLS_H

#include "fyai.h"

struct fyai_tool_job;
struct fyai_tool_job_group;
typedef void (*fyai_tool_group_complete_fn)(
		struct fyai_tool_job_group *group, void *userdata);

void fyai_print_tool_call(struct fyai_ctx *ctx, fy_generic tool_call);
fy_generic fyai_execute_tool_call(struct fyai_ctx *ctx, fy_generic tool_call,
				  bool *okp);
bool fyai_tool_call_parallel_eligible(struct fyai_ctx *ctx,
				      fy_generic tool_call);
struct fyai_tool_job *fyai_tool_job_submit(struct fyai_ctx *ctx,
					    fy_generic tool_call);
bool fyai_tool_job_done(const struct fyai_tool_job *job);
void fyai_tool_job_cancel(struct fyai_tool_job *job);
fy_generic fyai_tool_job_collect(struct fyai_ctx *ctx,
				 struct fyai_tool_job *job, bool *okp);

struct fyai_tool_job_group *fyai_tool_job_group_create(struct fyai_ctx *ctx);
struct fyai_tool_job_group *
fyai_tool_job_group_create_notify(struct fyai_ctx *ctx,
				  fyai_tool_group_complete_fn complete,
				  void *userdata);
struct fyai_tool_job_group *
fyai_tool_job_group_create_open(struct fyai_ctx *ctx,
				fyai_tool_group_complete_fn complete,
				void *userdata);
int fyai_tool_job_group_add(struct fyai_tool_job_group *group,
			    fy_generic tool_call);
/*
 * A regular submission seals and starts the FIFO immediately. An open group
 * may accept parallel calls while a model response is still streaming; seal()
 * declares that no more calls will arrive. Job callbacks park completions,
 * fill available FIFO slots, and notify a sealed group exactly once.
 * Collection is non-blocking and valid only after done().
 */
int fyai_tool_job_group_submit(struct fyai_tool_job_group *group);
int fyai_tool_job_group_seal(struct fyai_tool_job_group *group);
void fyai_tool_job_group_service(struct fyai_tool_job_group *group);
bool fyai_tool_job_group_done(const struct fyai_tool_job_group *group);
size_t fyai_tool_job_group_count(const struct fyai_tool_job_group *group);
void fyai_tool_job_group_cancel(struct fyai_tool_job_group *group);
int fyai_tool_job_group_collect(struct fyai_tool_job_group *group,
				size_t index, fy_generic *result, bool *okp);
void fyai_tool_job_group_destroy(struct fyai_tool_job_group *group);

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
