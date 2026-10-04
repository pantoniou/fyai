/*
 * commands.h - what an invocation runs, and the arguments backends read
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef COMMANDS_H
#define COMMANDS_H

#include <stdio.h>

#include <libfyaml/libfyaml-generic.h>

#include "utils.h"
#include "fyai_auth.h"
#include "fyai_secret.h"

/* fwd decl */
struct fyai_cfg;
struct fyai_ctx;

/* What one invocation runs. */
enum fyai_run {
	FYAI_RUN_NONE = -1,
	FYAI_RUN_PROMPT = 0,	/* a prompt, or the interactive session */
	FYAI_RUN_CONFIG,	/* only the global --set, --get, and --delete */
	FYAI_RUN_CMD,		/* a command of the registry, fyai_cmd.h */
};

enum fyai_output_format {
	FYAIOF_MARKDOWN,
	FYAIOF_RAW,
	FYAIOF_JSON,
	FYAIOF_YAML,
};

/*
 * Run a single model invocation (prompt or interactive) to completion:
 * setup, execute, print --stats, cleanup. @cfg must already carry the prompt
 * and any run options. Returns 0 on success, -1 on failure.
 */
int fyai_run(struct fyai_cfg *cfg);

/* per verb arguments */

struct fyai_prompt_args {
	bool interactive;
	const char *prompt;
	int answer_count;
	const char **answers;
};

struct fyai_init_args {
	const char *dir;
	bool force;
	const char *config;
};

enum fyai_turn_selector_type {
	FYAITST_ALL,
	FYAITST_FIRST,
	FYAITST_LAST,
	FYAITST_RANGE,
};

struct fyai_turn_selector_args {
	enum fyai_turn_selector_type type;
	size_t first;
	size_t last;
	size_t range_lo;
	size_t range_hi;
};

struct fyai_dump_args {
	bool decorate;
	bool state;
	bool anchors;
	bool provider_stream;
	struct fyai_turn_selector_args turn_sel;
};

struct fyai_display_args {
	bool raw;
	const char *tool_detail;
	struct fyai_turn_selector_args turn_sel;
};

struct fyai_export_args {
	const char *path;	/* NULL is standard output */
	const char *ref;	/* <branch>[@{N}]; NULL is the active branch */
};

/* `fyai resume`: continue a stored session instead of starting a fresh one. */
struct fyai_resume_args {
	const char *branch;	/* resume this branch, without moving HEAD */
	bool last;		/* resume the most recently updated branch */
	bool all;		/* every starting directory, not only this one */
};

struct fyai_replay_args {
	bool ignore_compact;
};

struct fyai_import_args {
	const char *path;	/* NULL is standard input */
	bool ignore_compact;	/* skip compaction markers instead of issuing */
	const char *from;	/* NULL for native import */
	const char *source_root; /* override the foreign application state root */
	const char *session;	/* discover and import this source session ID */
	bool dry_run;		/* inspect foreign input without publication */
	bool list;		/* list discoverable foreign sessions */
	bool all;		/* include sessions from other directories */
	bool json;		/* machine-readable dry-run report */
};

struct fyai_stats_args {
	enum fyai_output_format format;
};

struct fyai_gc_args {
	/* Retain at most this many ref-log entries (current root + N-1
	 * predecessors); the rest are cut from the chain and freed. -1 keeps
	 * the whole chain. */
	int keep_reflogs;
	/* A file of the project storage younger than this many seconds stays. */
	unsigned int grace;
	/* What the collection of the project storage removed. */
	size_t manifests, objects, runtimes;
	unsigned long long bytes;
};

struct fyai_diff_args {
	const char *from;	/* ref-log entry; HEAD@{1} by default */
	const char *to;		/* ref-log entry; HEAD by default */
	bool unified;		/* unified rows, coloured on a terminal */
};

struct fyai_reset_args {
	const char *ref;	/* symbolic start point, e.g. HEAD~2 */
};

enum fyai_root_cmd_type {
	FYAIRCT_PRINT,
	FYAIRCT_SHOW,
};

struct fyai_root_args {
	enum fyai_root_cmd_type type;
	const char *ref;	/* optional: the root behind this reference */
};

struct fyai_join_args {
	const char *source;
	bool allow_unrelated;
};

struct fyai_clear_args {
	/* nothing */
};

struct fyai_compact_args {
	const char *hint;	/* optional summary focus */
};

struct fyai_context_args {
	/* nothing */
};

struct fyai_api_args {
	const char *mode;	/* responses|chat-completions|messages, or NULL */
};

struct fyai_log_args {
	const char *arg;
};

/* `fyai term`: one program on a terminal that fyai draws. */
struct fyai_term_args {
	const char *command;	/* NULL runs the shell interactively */
	const char *shell;	/* the shell to run; NULL = $SHELL or /bin/sh */
	const char *screen;	/* write the final screen as text here */
	int rows;		/* force a size, for a run with no terminal */
	int cols;
	bool login;
	bool hold;		/* stay on the last screen until a key */
};

struct fyai_tool_args {
	const char *name;	/* tool name, e.g. read_file */
	const char *args_json;	/* JSON args, or NULL to read stdin */
};

struct fyai_mcp_args {
	const char *name;
	const char *file;
	const char *endpoint;
	const char *secret_env;
	const char **scopes;
	size_t scope_count;
	bool force;
};

/* everything */
union fyai_cmd_args {
	struct fyai_prompt_args prompt;
	struct fyai_init_args init;
	struct fyai_dump_args dump;
	struct fyai_display_args display;
	struct fyai_stats_args stats;
	struct fyai_gc_args gc;
	struct fyai_reset_args reset;
	struct fyai_root_args root;
	struct fyai_join_args join;
	struct fyai_clear_args clear;
	struct fyai_compact_args compact;
	struct fyai_context_args context;
	struct fyai_api_args api;
	struct fyai_log_args log;
	struct fyai_tool_args tool;
	struct fyai_secret_args secret;
	struct fyai_mcp_args mcp;
	struct fyai_export_args export;
	struct fyai_diff_args diff;
	struct fyai_import_args import;
	struct fyai_replay_args replay;
	struct fyai_resume_args resume;
	struct fyai_term_args term;
};

/* finally declare the verb */
enum fyai_verb_flags {
	FYAIVF_BATCH		= 0,		/* is batch only */
	FYAIVF_INTERACTIVE	= FY_BIT(0),	/* is interactive */
	FYAIVF_NO_STORAGE	= FY_BIT(2),	/* does not need storage */
	FYAIVF_NO_REQUESTS	= FY_BIT(3),	/* does not make requests */
	FYAIVF_NEEDS_TRANSIENT_BUILDER = FY_BIT(4),
	FYAIVF_STORAGE_OPTIONAL	= FY_BIT(5),	/* storage only when it exists */
};

/* The properties of what an invocation runs, which setup reads. */
struct fyai_verb {
	const char *name;
	int (*execute)(struct fyai_ctx *ctx);
	enum fyai_verb_flags flags;
};

/*
 * A command of the registry, parsed for this invocation: its definition, its
 * validated arguments, and the verb properties that setup reads. The values
 * live in cfg->gb. See fyai_cmd.h.
 */
struct fyai_cmd_state {
	fy_generic def;
	const char *path;		/* "branch new" */
	fy_generic args;
	int format;			/* enum fyai_cmd_format */
	bool help;			/* --help: show the help, run nothing */
	struct fyai_verb verb;		/* flags from the definition */
};

/* combined */
struct fyai_cmd_info {
	enum fyai_run run;
	union fyai_cmd_args args;

	struct fyai_cmd_state reg;	/* FYAI_RUN_CMD */
};



#endif
