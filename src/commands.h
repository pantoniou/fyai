/*
 * commands.h - fyai verb dispatch
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
struct fyai_verb;
struct fyai_cfg;
struct fyai_ctx;

enum fyai_verb_id {
	FYAIVID_INVALID = -1,
	FYAIVID_PROMPT = 0,	/* this is the default, prompt mode */
	FYAIVID_INIT,
	FYAIVID_DUMP,
	FYAIVID_DISPLAY,
	FYAIVID_HISTORY,
	FYAIVID_STATS,
	FYAIVID_CONFIG,
	FYAIVID_LIST,
	FYAIVID_CATALOG,
	FYAIVID_CLEAR,
	FYAIVID_COMPACT,
	FYAIVID_CONTEXT,
	FYAIVID_API,
	FYAIVID_LOG,
	FYAIVID_SANDBOX,
	FYAIVID_AUTH,
	FYAIVID_SECRET,
	FYAIVID_GC,
	FYAIVID_TOOL,
	FYAIVID_HELP,
};
#define FYAI_VERB_COUNT (FYAIVID_HELP + 1)

/* find the fyai_verb that matches the name or NULL */
const struct fyai_verb *fyai_find_verb(const char *name);

/* return the verb id or FYAIVID_INVALID if invalid */
enum fyai_verb_id fyai_get_verb_id(const char *name);

/* True if @name is a known verb (init, dump, stats, config, gc, init). */
bool fyai_is_verb(const char *name);

enum fyai_output_format {
	FYAIOF_MARKDOWN,
	FYAIOF_RAW,
	FYAIOF_JSON,
	FYAIOF_YAML,
};

const struct fyai_verb *fyai_id_to_verb(enum fyai_verb_id id);

/*
 * Run a single model invocation (prompt or interactive) to completion:
 * setup, execute, print --stats, cleanup. @cfg must already carry the prompt
 * and any run options. Returns 0 on success, -1 on failure.
 */
int fyai_run(struct fyai_cfg *cfg);

/*
 * Configure a verb. @argc/@argv are the verb's own slice (argv[0] is the verb
 * name, argv[1..] its arguments). @cfg carries the resolved global options.
 * Returns 0 on success, -1 on error
 */
int fyai_configure(struct fyai_cfg *cfg, int argc, char *argv[]);

/* Print the top-level usage (verbs + global options) to @fp. */
void fyai_usage(FILE *fp, const char *progname, const char *color_mode);
int fyai_execute_list(struct fyai_ctx *ctx);
int fyai_execute_config(struct fyai_ctx *ctx);

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
	struct fyai_turn_selector_args turn_sel;
};

struct fyai_stats_args {
	enum fyai_output_format format;
};

enum fyai_config_type {
	FYAICT_SHOW,
	FYAICT_EFFECTIVE,
	FYAICT_GET,
	FYAICT_SET,
	FYAICT_DELETE,
	FYAICT_EDIT,
	FYAICT_IMPORT,
	FYAICT_EXPORT,
	FYAICT_VALIDATE,
	FYAICT_SCHEMA,
	FYAICT_DESCRIBE,
	/*
	 * Not user-typable: synthesized for a bare --set/--get/--delete run with
	 * no verb, so storage opens (no API key, no requests) and the global ops
	 * run in fyai_run while the verb itself does nothing. Kept last so it does
	 * not perturb the types[] subcommand table.
	 */
	FYAICT_NOOP,
};

struct fyai_config_args {
	enum fyai_config_type type;
	const char *key;
	const char *value;
};

enum fyai_catalog_type {
	FYAICAT_SHOW,
	FYAICAT_LIST,
	FYAICAT_TOOLS,
	FYAICAT_IMPORT,
	FYAICAT_EXPORT,
};

struct fyai_catalog_args {
	enum fyai_catalog_type type;
	const char *arg;	/* import file or list selector */
	bool full;		/* --full: show complete tool descriptions */
};

enum fyai_list_type {
	FYAILT_PROVIDERS,
	FYAILT_MODELS,
	FYAILT_TURNS,
	FYAILT_EXCHANGES,
	FYAILT_REFLOG,
};

struct fyai_list_args {
	enum fyai_list_type type;
	enum fyai_output_format format;
	bool full;	/* --full: include per-item detail; --brief (default): summary */
};

struct fyai_gc_args {
	/* Retain at most this many ref-log entries (current root + N-1
	 * predecessors); the rest are cut from the chain and freed. -1 keeps
	 * the whole chain. */
	int keep_reflogs;
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

struct fyai_help_args {
	const char *verb;
};

struct fyai_tool_args {
	const char *name;	/* tool name, e.g. read_file */
	const char *args_json;	/* JSON args, or NULL to read stdin */
};

/* everything */
union fyai_cmd_args {
	struct fyai_prompt_args prompt;
	struct fyai_init_args init;
	struct fyai_dump_args dump;
	struct fyai_display_args display;
	struct fyai_stats_args stats;
	struct fyai_config_args config;
	struct fyai_catalog_args catalog;
	struct fyai_list_args list;
	struct fyai_gc_args gc;
	struct fyai_clear_args clear;
	struct fyai_compact_args compact;
	struct fyai_context_args context;
	struct fyai_api_args api;
	struct fyai_log_args log;
	struct fyai_help_args help;
	struct fyai_tool_args tool;
	struct fyai_auth_args auth;
	struct fyai_secret_args secret;
};

/* combined */
struct fyai_cmd_info {
	enum fyai_verb_id id;
	union fyai_cmd_args args;
};

/* finally declare the verb */
enum fyai_verb_flags {
	FYAIVF_BATCH		= 0,		/* is batch only */
	FYAIVF_INTERACTIVE	= FY_BIT(0),	/* is interactive */
	FYAIVF_NEEDS_API_KEYS	= FY_BIT(1),	/* will use api keys if available, will fail without */
	FYAIVF_NO_STORAGE	= FY_BIT(2),	/* does not need storage */
	FYAIVF_NO_REQUESTS	= FY_BIT(3),	/* does not make requests */
};

struct fyai_verb {
	int id;
	const char *name;
	int (*configure)(int argc, char **argv, struct fyai_cfg *cfg);
	int (*execute)(struct fyai_ctx *ctx);
	const char *synopsis;
	const char *help;
	enum fyai_verb_flags flags;
	union fyai_cmd_args default_args;	/* the default args */
};


#endif
