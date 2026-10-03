/*
 * fyai_cmd_int.h - internal interfaces of the command engine
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_CMD_INT_H
#define FYAI_CMD_INT_H

#include "fyai_cmd.h"

#define FYAI_CMD_MAX_PROPS	32

/* The command that a list of words resolves to. */
struct fyai_cmd_walk {
	fy_generic def;		/* registry storage */
	fy_generic surfaces;	/* effective surfaces of @def */
	char path[256];		/* "branch new" */
	size_t consumed;	/* words that name the command */
	fy_generic group;	/* the group whose default gave @def */
	fy_generic group_surfaces;
	/*
	 * The positional arguments of the groups on the path: a word that
	 * names no subcommand fills the next one ("auth openai login").
	 */
	struct {
		fy_generic group;
		const char *name;	/* registry storage */
		const char *word;	/* the caller's words */
	} gargs[4];
	size_t ngargs;
	fy_generic groups[4];	/* the groups on the path, outermost first */
	size_t ngroups;
	fy_generic last_group;	/* the innermost group on the path */
	long long last_group_pos;	/* its next positional */
};

/* One property of the arguments of a command. */
struct fyai_cmd_prop {
	const char *name;	/* registry storage */
	fy_generic p;
	long long pos;		/* positional index, or -1 for an option */
	bool rest;
	bool repeat;
	bool negate;
	bool hidden;
	bool boolean;
	bool array;
	char lname[64];		/* long option name */
	char sname;		/* short option letter, or 0 */
	char meta[32];
};

struct fyai_cmd_parsed {
	fy_generic def;
	const char *path;
	fy_generic args;
	enum fyai_cmd_format format;
	bool help;
};

struct fyai_cmd_parse_state {
	struct fyai_cfg *cfg;
	struct fy_generic_builder *gb;
	enum fyai_cmd_surface surface;
	size_t nwords;
	const char *const *words;
	const char *line;	/* the session line, for a rest argument */
	const size_t *offs;
	struct fyai_cmd_parsed *out;
	fy_generic args;
};

const char *fyai_cmd_surface_name(enum fyai_cmd_surface surface);
bool fyai_cmd_seq_has(fy_generic seq, const char *word);
bool fyai_cmd_def_on(fy_generic def, fy_generic inherited,
		     enum fyai_cmd_surface surface);
fy_generic fyai_cmd_def_surfaces(fy_generic def, fy_generic inherited);
bool fyai_cmd_def_names(fy_generic def, const char *word);
fy_generic fyai_cmd_def_find(fy_generic defs, fy_generic inherited,
			     const char *word, size_t len,
			     enum fyai_cmd_surface surface, size_t *hitsp);

/*
 * Resolve the command that @words names. Only the first @limit words are
 * taken as subcommand words. Returns 0, or -1 when no command matches.
 */
int fyai_cmd_walk(enum fyai_cmd_surface surface, size_t nwords,
		  const char *const *words, size_t limit,
		  struct fyai_cmd_walk *w);

/* Fill @props with the properties of @def on @surface; returns the count. */
int fyai_cmd_props(fy_generic def, enum fyai_cmd_surface surface,
		   struct fyai_cmd_prop *props, size_t max);
void fyai_cmd_long_name(const char *name, char *buf, size_t size);
struct fyai_cmd_prop *fyai_cmd_prop_positional(struct fyai_cmd_prop *props,
					       int n, long long pos);

int fyai_cmd_parse(struct fyai_cfg *cfg, struct fy_generic_builder *gb,
		   enum fyai_cmd_surface surface, size_t nwords,
		   const char *const *words, const char *line,
		   const size_t *offs, struct fyai_cmd_parsed *out);

int fyai_cmd_call_init(struct fyai_cmd_call *call, struct fyai_ctx *ctx,
		       struct fyai_cmd_parsed *parsed,
		       enum fyai_cmd_surface surface);
int fyai_cmd_call_run(struct fyai_cmd_call *call);
void fyai_cmd_call_release(struct fyai_cmd_call *call);

/* Handlers of config and sandbox. */
int fyai_cmd_config_show(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_config_effective(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_config_get(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_config_set(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_config_delete(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_config_import(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_config_export(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_config_validate(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_config_schema(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_config_edit(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_config_describe(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_sandbox_show(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_sandbox_set(struct fyai_cmd_call *call, fy_generic *result);

int fyai_cmd_history(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_reset(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_clear(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_api(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_context(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_stats(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_list(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_diff(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_root(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_join(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_gc(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_export(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_log(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_secret(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_compact(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_dump(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_import(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_replay(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_tool(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_term(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_init(struct fyai_cmd_call *call, fy_generic *result);

/* The exchanges that --first, --last, --range, or the session words select. */
int fyai_cmd_turn_selection(struct fyai_cmd_call *call,
			    struct fyai_turn_selector_args *sel);
int fyai_cmd_mcp_status(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_mcp_login(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_mcp_enable(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_mcp_import_client(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_exit(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_reload(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_btw(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_branches(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_resume(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_zoom(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_page(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_page_review(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_page_review_sample(struct fyai_cmd_call *call,
				fy_generic *result);
int fyai_cmd_sessions(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_kill(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_status(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_profiles(struct fyai_cmd_call *call, fy_generic *result);

/* Handlers of the filesystem views. */
int fyai_cmd_view_mount(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_view_unmount(struct fyai_cmd_call *call, fy_generic *result);

/* Handlers of the filesystem views. */
int fyai_cmd_view_create(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_view_update(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_view_show(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_view_list(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_view_enter(struct fyai_cmd_call *call, fy_generic *result);

fyai_cmd_prepare_fn fyai_cmd_prepare(const char *name);
fyai_cmd_prepare_fn fyai_cmd_early_hook(const char *name);
int fyai_cmd_resume_early(struct fyai_cfg *cfg, fy_generic args);
int fyai_cmd_resume_prepare(struct fyai_cfg *cfg, fy_generic args);
int fyai_cmd_term_prepare(struct fyai_cfg *cfg, fy_generic args);
int fyai_cmd_init_prepare(struct fyai_cfg *cfg, fy_generic args);
int fyai_cmd_agent_prepare(struct fyai_cfg *cfg, fy_generic args);
int fyai_cmd_transport_early(struct fyai_cfg *cfg, fy_generic args);
int fyai_cmd_transport(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_agent(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_setting(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_catalog_show(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_catalog_list(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_render(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_catalog_tools(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_catalog_get(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_catalog_set(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_catalog_delete(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_catalog_import(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_catalog_export(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_catalog_validate(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_catalog_schema(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_catalog_reset(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_catalog_edit(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_catalog_update(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_auth_accounts(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_auth_status(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_auth_usage(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_auth_login(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_auth_logout(struct fyai_cmd_call *call, fy_generic *result);

/* Session-only branch handlers. */
int fyai_cmd_branch_switch(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_branch_attach(struct fyai_cmd_call *call, fy_generic *result);
int fyai_cmd_branch_detach(struct fyai_cmd_call *call, fy_generic *result);

#endif
