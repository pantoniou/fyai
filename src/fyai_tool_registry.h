/* SPDX-License-Identifier: MIT */
#ifndef FYAI_TOOL_REGISTRY_H
#define FYAI_TOOL_REGISTRY_H

#include <stdbool.h>
#include <stdio.h>
#include <stddef.h>

#include <libfyaml/libfyaml-generic.h>

struct fyai_ctx;
struct fyai_md_blocks;

/*
 * Run a tool and return its result. Set *okp to the outcome. A tool that
 * builds a structure gives it as it is: the caller formats it for the model.
 */
typedef fy_generic (*fyai_tool_run_fn)(struct fyai_ctx *ctx, fy_generic args,
				       bool *okp);
/* Write the title row of a call as Markdown to @mf. */
typedef void (*fyai_tool_head_fn)(struct fyai_ctx *ctx, FILE *mf,
				  struct fy_generic_builder *gb,
				  fy_generic args, int preview_lines,
				  struct fyai_md_blocks *blocks);

/*
 * Write the result of a call as Markdown for the user, for a result that is
 * a structure and reads badly as the text the model is given. @args are the
 * arguments of the call, or fy_invalid when the call is not known. Return a
 * malloc'd string, or NULL to show the result as it is. The model always
 * gets the result as it is.
 */
typedef char *(*fyai_tool_format_fn)(struct fyai_ctx *ctx,
				     struct fy_generic_builder *gb,
				     fy_generic args, const char *result);

/*
 * Adapt the wire specification @tool of the tool to this run, built in @gb.
 * Return fy_invalid to leave the tool out of the run.
 */
typedef fy_generic (*fyai_tool_describe_fn)(struct fyai_ctx *ctx,
					    struct fy_generic_builder *gb,
					    fy_generic tool);

/*
 * Decide whether a call runs as the call of another tool. Given the
 * arguments and the id of the call, set *@namep and *@argsp, built in the
 * transient builder, and return true; return false to run the call as it
 * is. The decision must depend only on the call and on the stored state,
 * because the scheduler asks it more than once for one call.
 */
typedef bool (*fyai_tool_expand_fn)(struct fyai_ctx *ctx, fy_generic args,
				    const char *call_id, const char **namep,
				    fy_generic *argsp);

/* Ordering classes of fyai_tool_def.effect. */
enum {
	FYAI_TOOL_EFFECT_NONE,		/* reads nothing that others write */
	FYAI_TOOL_EFFECT_FILES,		/* writes project files, named in the call */
	FYAI_TOOL_EFFECT_PROCESS,	/* starts or ends processes, or writes unknown files */
};

enum {
	/* The tool reads or changes state of the parent: it never runs in a job. */
	FYAI_TOOL_PARENT	= 1 << 0,
	/* Its screen is a session or a tile: the call draws no head of its own. */
	FYAI_TOOL_SILENT	= 1 << 1,
	/* A tool call row carries the state mark of the call. */
	FYAI_TOOL_MARKED	= 1 << 2,
	/* A sub-agent manages agents and views through it: it is not offered to one. */
	FYAI_TOOL_NOT_FOR_CHILD	= 1 << 3,
	/* The provider runs it: there is no local run, only a head. */
	FYAI_TOOL_HOSTED	= 1 << 4,
	/*
	 * A call kept in the parent returns at once and starts work that ends
	 * later. It runs in order inside a group, so a later call of the same
	 * response can wait for it. When the entry has `in_parent`, only a call
	 * that function accepts is instant.
	 */
	FYAI_TOOL_INSTANT	= 1 << 5,
	/* The root cannot call a tool meant for its delegating parent. */
	FYAI_TOOL_CHILD_ONLY	= 1 << 6,
	/* A delegated agent cannot call a tool reserved for the user-owned root. */
	FYAI_TOOL_ROOT_ONLY	= 1 << 7,
	/*
	 * The result for the user is read as a whole, as a patch is: the
	 * formatter keeps it short, and the preview does not cut it.
	 */
	FYAI_TOOL_WHOLE_RESULT	= 1 << 8,
};

/*
 * Everything the program knows about one tool, by canonical name. The file
 * that implements a tool defines its entry; fyai_tool_registry.c only joins
 * the tables. A tool needs `run` unless it is hosted, and a
 * `head` unless it is silent.
 */
struct fyai_tool_def {
	const char *name;
	fyai_tool_run_fn run;
	fyai_tool_head_fn head;
	/* When set, the result of a call that succeeded is shown through it. */
	fyai_tool_format_fn format_result;
	/* When set, a call is kept in the parent if this says so of its arguments. */
	bool (*in_parent)(fy_generic args);
	/* When set, the specification of the tool is adapted to the run. */
	fyai_tool_describe_fn describe;
	/* When set, a call can run as the call of another tool. */
	fyai_tool_expand_fn expand;
	unsigned int flags;
	int effect;
};

/* The tables of the files that implement tools. */
extern const struct fyai_tool_def fyai_tools_defs[];
extern const size_t fyai_tools_defs_count;
extern const struct fyai_tool_def fyai_wait_defs[];
extern const size_t fyai_wait_defs_count;
extern const struct fyai_tool_def fyai_agent_defs[];
extern const size_t fyai_agent_defs_count;
extern const struct fyai_tool_def fyai_monitor_defs[];
extern const size_t fyai_monitor_defs_count;
extern const struct fyai_tool_def fyai_display_defs[];
extern const size_t fyai_display_defs_count;
extern const struct fyai_tool_def fyai_skill_defs[];
extern const size_t fyai_skill_defs_count;
extern const struct fyai_tool_def fyai_mcp_defs[];
extern const struct fyai_tool_def fyai_structured_defs[];
extern const size_t fyai_structured_defs_count;
extern const size_t fyai_mcp_defs_count;
extern const struct fyai_tool_def fyai_settings_defs[];
extern const size_t fyai_settings_defs_count;

/* The entry for @name, or NULL. @name may be a wire name of the shell. */
const struct fyai_tool_def *fyai_tool_find(const char *name);

/* Entry @index of the whole registry, or NULL past the end. */
const struct fyai_tool_def *fyai_tool_at(size_t index);

/* Whether @name has @flag. A name that is not registered has none. */
bool fyai_tool_has(const char *name, unsigned int flag);

#endif
