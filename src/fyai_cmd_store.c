/*
 * fyai_cmd_store.c - handlers of init, dump, import, replay, tool, term, and
 * agent
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * These backends read their arguments from the command state of the
 * configuration; a handler sets that state and calls the backend.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fyai.h"
#include "fyai_cmd.h"
#include "fyai_cmd_int.h"
#include "fyai_agent.h"
#include "fyai_config.h"
#include "fyai_display.h"
#include "fyai_foreign_import.h"
#include "fyai_storage.h"
#include "fyai_term.h"
#include "fyai_tools.h"
#include "utils.h"

#define FYAI_MODULE FYAIEM_UNKNOWN

/* A string argument of @args, in the builder of the configuration. */
static const char *prep_str(struct fyai_cfg *cfg, fy_generic args,
			    const char *name)
{
	fy_generic v = fy_get(args, name, fy_invalid);

	return fy_is_string(v) ? fy_gb_intern_string(cfg->gb,
						     fy_castp(&v, "")) : NULL;
}

/*
 * init resolves the arena directory and the configuration file before setup:
 * the storage that setup opens does not exist yet.
 */
int fyai_cmd_init_prepare(struct fyai_cfg *cfg, fy_generic args)
{
	struct fyai_init_args *a = &cfg->cmd.args.init;
	const char *file = prep_str(cfg, args, "file");
	char *cwd, *real;

	memset(a, 0, sizeof(*a));
	a->force = fy_get(args, "force", false);
	cwd = getcwd(NULL, 0);
	fyai_cfg_error_check(cfg, cwd, err, "init: cannot read the current "
			     "directory");
	real = realpath(cwd, NULL);
	free(cwd);
	fyai_cfg_error_check(cfg, real, err, "init: cannot resolve the "
			     "current directory");
	a->dir = fy_gb_intern_string(cfg->gb, real);
	free(real);
	fyai_cfg_error_check(cfg, is_writable_directory(a->dir), err,
			     "init: cannot write the directory %s", a->dir);
	/* The default config.yaml is taken when it exists; a named file
	 * must. */
	if (!file && access("config.yaml", R_OK))
		return 0;
	if (!file)
		file = "config.yaml";
	fyai_cfg_error_check(cfg, is_readable_file(file), err,
			     "init: cannot read the configuration %s", file);
	a->config = fy_gb_intern_string(cfg->gb, file);
	return 0;
err:
	return -1;
}

int fyai_cmd_init(struct fyai_cmd_call *call, fy_generic *result)
{
	(void)result;
	return fyai_init_storage(call->ctx);
}

int fyai_cmd_dump(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_dump_args *args = &call->ctx->cfg->cmd.args.dump;
	const char *what = fyai_cmd_arg_str(call, "what");

	(void)result;
	memset(args, 0, sizeof(*args));
	if (fyai_cmd_turn_selection(call, &args->turn_sel))
		return -1;
	args->decorate = fyai_cmd_arg_bool(call, "decorate");
	args->state = !strcmp(what, "state");
	args->anchors = !strcmp(what, "anchors");
	args->provider_stream = !strcmp(what, "providers");
	return fyai_dump_view(call->ctx);
}

static int import_source(struct fyai_cmd_call *call, const char *from,
			 enum fyai_foreign_source *sourcep)
{
	if (!strcmp(from, "auto"))
		*sourcep = FYAI_FOREIGN_AUTO;
	else if (!strcmp(from, "claude-code"))
		*sourcep = FYAI_FOREIGN_CLAUDE_CODE;
	else if (!strcmp(from, "codex"))
		*sourcep = FYAI_FOREIGN_CODEX;
	else {
		fyai_error(call->ctx, "import: unknown source '%s'", from);
		return -1;
	}
	return 0;
}

/*
 * A native export is read from standard input or --input. A foreign session
 * (--from) is read from a file, listed, or found by its ID.
 */
int fyai_cmd_import(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = call->ctx;
	const char *path = fyai_cmd_arg_str(call, "input");
	const char *from = fyai_cmd_arg_str(call, "from");
	const char *root = fyai_cmd_arg_str(call, "source_root");
	const char *session = fyai_cmd_arg_str(call, "session");
	bool dry_run = fyai_cmd_arg_bool(call, "dry_run");
	bool list = fyai_cmd_arg_bool(call, "list");
	bool all = fyai_cmd_arg_bool(call, "all");
	bool json = fyai_cmd_arg_bool(call, "json");
	bool native = !from || !strcmp(from, "fyai");
	enum fyai_foreign_source source;

	(void)result;
	fyai_error_check(ctx, from || !(dry_run || list || session || json),
			 err, "import: foreign options need --from");
	fyai_error_check(ctx, !root || (from && strcmp(from, "auto")), err,
			 "import: --source-root needs a concrete source");
	fyai_error_check(ctx, !(list && path), err,
			 "import: --list does not take an input file");
	fyai_error_check(ctx, !(list && session), err,
			 "import: --list and --session are mutually exclusive");
	fyai_error_check(ctx, !(session && path), err,
			 "import: --session and --input are mutually exclusive");
	fyai_error_check(ctx, !session || strcmp(from, "auto"), err,
			 "import: --session needs a concrete source");
	fyai_error_check(ctx, !from || !fyai_cmd_arg_bool(call,
							   "ignore_compact"),
			 err, "import: foreign input does not use "
			 "--ignore-compact");
	fyai_error_check(ctx, !from || strcmp(from, "fyai") ||
			 !(dry_run || list || session || json || root || all),
			 err, "import: fyai input does not use foreign options");
	if (native) {
		ctx->cfg->cmd.args.import.ignore_compact =
			fyai_cmd_arg_bool(call, "ignore_compact");
		return fyai_import_view(ctx, path);
	}
	if (import_source(call, from, &source))
		return -1;
	if (list)
		return fyai_foreign_import_list(ctx, source, root, all, json);
	if (session)
		return fyai_foreign_import_session(ctx, source, root, session,
						   dry_run, json);
	if (dry_run)
		return fyai_foreign_import_dry_run(ctx, path, source, json);
	return fyai_foreign_import_view(ctx, path, source, NULL);
err:
	return -1;
}

int fyai_cmd_replay(struct fyai_cmd_call *call, fy_generic *result)
{
	(void)result;
	return fyai_replay_view(call->ctx,
				fyai_cmd_arg_bool(call, "ignore_compact"));
}

int fyai_cmd_tool(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_tool_args *args = &call->ctx->cfg->cmd.args.tool;

	(void)result;
	args->name = fyai_cmd_arg_str(call, "name");
	args->args_json = fyai_cmd_arg_str(call, "json");
	return fyai_run_tool_verb(call->ctx);
}

/* term draws on the terminal, so the display is interactive from setup. */
int fyai_cmd_term_prepare(struct fyai_cfg *cfg, fy_generic args)
{
	struct fyai_term_args *a = &cfg->cmd.args.term;

	memset(a, 0, sizeof(*a));
	cfg->interactive = true;
	a->command = prep_str(cfg, args, "command");
	a->shell = prep_str(cfg, args, "shell");
	if (!a->shell)
		a->shell = getenv("SHELL");
	a->screen = prep_str(cfg, args, "screen");
	a->rows = fy_get(args, "rows", 0LL);
	a->cols = fy_get(args, "cols", 0LL);
	a->login = fy_get(args, "login", false);
	a->hold = fy_get(args, "hold", false);
	return 0;
}

int fyai_cmd_term(struct fyai_cmd_call *call, fy_generic *result)
{
	(void)result;
	return fyai_term_verb(call->ctx);
}

/*
 * A sub-agent runs without the MCP servers of a parent, and stores its run
 * unless --transient is given. An
 * executed tool child (--tool-child) is written by fyai_tool_child_exec():
 * it keeps the arena of its parent, publishes to it, and takes its call on
 * descriptors 3 and 4.
 */
int fyai_cmd_agent_prepare(struct fyai_cfg *cfg, fy_generic args)
{
	const char *task = prep_str(cfg, args, "task");
	bool rpc = fy_get(args, "rpc", false);

	cfg->interactive = false;
	cfg->enable_tools = true;
	cfg->agent_child = true;
	cfg->mcp_enabled = false;
	if (fy_get(args, "tool_child", false)) {
		cfg->diag.mask &= ~(1u << FYAIET_NOTICE);
		cfg->agent_pty = fy_get(args, "pty", false);
		if (prep_str(cfg, args, "arena"))
			cfg->arena_dir = prep_str(cfg, args, "arena");
		cfg->tool_exec = true;
		cfg->tool_child = true;
		return 0;
	}
	fyai_cfg_error_check(cfg, rpc || task, err,
			     "agent: a task description is required");
	fyai_cfg_error_check(cfg, !rpc || !task, err,
			     "agent: --rpc takes no task");
	/* In RPC mode the task arrives through agent/run. */
	cfg->agent_rpc = rpc;
	if (task)
		cfg->prompt = task;
	return 0;
err:
	return -1;
}

int fyai_cmd_agent(struct fyai_cmd_call *call, fy_generic *result)
{
	(void)result;
	return fyai_agent_verb(call->ctx);
}

/*
 * resume selects its session before the configuration loads, because the
 * branch holds that configuration.
 */
int fyai_cmd_resume_early(struct fyai_cfg *cfg, fy_generic args)
{
	struct fyai_resume_args *a = &cfg->cmd.args.resume;

	a->branch = prep_str(cfg, args, "session");
	a->last = fy_get(args, "last", false);
	a->all = fy_get(args, "all", false);
	fyai_cfg_error_check(cfg, !(a->branch && a->last), err,
			     "resume: --last does not take a session");
	return fyai_config_select_resume(cfg);
err:
	return -1;
}

/* Resume continues a session at the prompt. */
int fyai_cmd_resume_prepare(struct fyai_cfg *cfg, fy_generic args)
{
	(void)args;
	cfg->interactive = true;
	return 0;
}
