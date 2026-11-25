/*
 * fyai_cmd_complete.c - completion from the command definitions
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * One engine completes a verb line for a shell (`fyai __complete`) and a
 * slash line for the session. An argument names a completion kind with
 * x-fyai-complete; a kind is a provider in the table below.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#include "fyai.h"
#include "fyai_cmd.h"
#include "fyai_fleet_ops.h"
#include "fyai_sandbox_profile.h"
#include "fyai_skill.h"
#include "fyai_cmd_int.h"
#include "fyai_grammar.h"
#include "fyai_branch.h"
#include "fyai_catalog.h"
#include "fyai_config.h"
#include "fyai_markdown.h"
#include "fyai_page.h"
#include "fyai_sink.h"
#include "fyai_ui.h"
#include "fyai_uimodel.h"
#include "fyai_query.h"
#include "utils.h"
#include "fyai_media.h"

#define FYAI_MODULE FYAIEM_UNKNOWN

struct complete_req {
	struct fyai_ctx *ctx;		/* NULL without an arena */
	fy_generic def;			/* the command being completed */
	struct fy_generic_builder *gb;	/* scratch, for this completion */
	enum fyai_cmd_surface surface;
	const char *partial;
	size_t plen;
	fyai_cmd_candidate_fn add;
	void *arg;
	unsigned int directives;
	/* The words that an array argument took before the partial one. */
	const char *const *given;
	size_t ngiven;
	/* The data is the owner's, which this process asks: a mirror. */
	bool remote;
};

/* The context that this object stands on. */
static inline struct fyai_ctx *complete_req_ctx(const struct complete_req *o)
{
	return o->ctx;
}

static void cand(struct complete_req *r, const char *value, const char *desc)
{
	size_t len;

	if (!value || strncmp(value, r->partial, r->plen))
		return;
	len = strlen(value);
	/* A path segment or an assignment goes on in the same word. */
	if (len && (value[len - 1] == '/' || value[len - 1] == '='))
		r->directives |= FYAI_CMD_COMPLETE_NOSPACE;
	r->add(r->arg, value, desc);
}

static const char *gstr(fy_generic *v)
{
	return fy_is_string(*v) ? fy_castp(v, "") : "";
}

/* ---- kinds ------------------------------------------------------------------ */

static void kind_branch_filtered(struct complete_req *r, bool agents,
				 const char *prefix)
{
	struct fyai_branch b;
	fy_generic entry;
	const char *name;

	if (!complete_req_ctx(r) || !fy_is_mapping(complete_req_ctx(r)->bstate->branches))
		return;
	fy_foreach_key_value(name, entry, complete_req_ctx(r)->bstate->branches) {
		if (fy_str_empty(name) || !fyai_branch_decode(entry, &b))
			continue;
		if (fy_is_valid(b.agent) != agents)
			continue;
		if (prefix && strncmp(name, prefix, strlen(prefix)))
			continue;
		cand(r, name, fy_castp(&b.description, ""));
	}
}

static void kind_branch(struct complete_req *r)
{
	kind_branch_filtered(r, false, NULL);
}

static void kind_view(struct complete_req *r)
{
	struct fyai_branch branch;
	fy_generic view;
	const char *name;

	if (!complete_req_ctx(r) || !fyai_branch_decode(complete_req_ctx(r)->bstate->prev, &branch))
		return;
	fy_foreach_key_value(name, view, fy_get(branch.store, "views", fy_invalid)) {
		if (!fy_str_empty(name) && fy_is_mapping(view))
			cand(r, name, fy_get(view, "project", ""));
	}
}

static void kind_agent_branch(struct complete_req *r)
{
	kind_branch_filtered(r, true, NULL);
}

static void kind_session(struct complete_req *r)
{
	kind_branch_filtered(r, false, "session/");
}

/*
 * Complete fleet sessions as PROJECT:BRANCH with state and optional title.
 * Produces no candidates if fleet listing fails.
 */
static void kind_fleet_session(struct complete_req *r)
{
	fy_generic rows, row, title;
	const char *state;

	if (!complete_req_ctx(r) ||
	    fyai_fleet_rows_list(complete_req_ctx(r), r->gb, &rows))
		return;
	fy_foreach(row, rows) {
		title = fy_get(row, "title", fy_invalid);
		state = fy_get(row, "state", "");
		cand(r, fy_gb_intern_string(r->gb, fy_sprintfa("%s:%s",
				fy_get(row, "project", "."),
				fy_get(row, "branch", ""))),
		     fy_is_string(title) ? fy_gb_intern_string(r->gb,
				fy_sprintfa("%s \u00b7 %s", state,
					    fy_castp(&title, ""))) : state);
	}
}

/* A branch, then the reference forms of a branch that the word names. */
static void kind_ref(struct complete_req *r)
{
	struct fyai_branch b;
	char base[FYAI_BRANCH_NAME_MAX + 1];
	const char *mark;
	size_t len;
	int i;

	cand(r, "HEAD", "the current branch");
	kind_branch(r);
	mark = strpbrk(r->partial, "~^@");
	if (!mark || !complete_req_ctx(r))
		return;
	len = (size_t)(mark - r->partial);
	if (len >= sizeof(base))
		return;
	memcpy(base, r->partial, len);
	base[len] = '\0';
	if (strcmp(base, "HEAD") &&
	    !fyai_branch_lookup(complete_req_ctx(r)->bstate->branches, base, &b))
		return;
	for (i = 1; i <= 4; i++) {
		cand(r, fy_sprintfa("%s~%d", base, i),
		     fy_sprintfa("%d turns before the head", i));
		cand(r, fy_sprintfa("%s@{%d}", base, i),
		     fy_sprintfa("ref-log entry %d", i));
	}
}

/* The catalogue of the branch, else the embedded one. */
static fy_generic complete_catalog(struct complete_req *r)
{
	return fyai_catalog_effective(complete_req_ctx(r) ? complete_req_ctx(r)->cfg->catalog :
				      fy_invalid, r->gb);
}

static void kind_provider(struct complete_req *r)
{
	fy_generic p, name;

	fy_foreach(p, fy_get(complete_catalog(r), "providers", fy_invalid)) {
		name = fy_get(p, "name", fy_invalid);
		cand(r, gstr(&name), NULL);
	}
}

/*
 * The models of a grammar that a local program serves, as provider/MODEL: its
 * default, and the offerings of the catalogue provider that lists its models.
 * With @prefix NULL, the provider of each such grammar.
 */
static void model_programs(struct complete_req *r, const char *prefix)
{
	const struct fyai_grammar *grammar;
	struct fyai_ctx *ctx = complete_req_ctx(r);
	struct fyai_cfg defaults;
	const struct fyai_cfg *cfg;
	fy_generic o, canon;
	const char *listed;

	/* With no arena, the configuration is the default one. */
	cfg = ctx ? ctx->cfg : NULL;
	if (!cfg) {
		memset(&defaults, 0, sizeof(defaults));
		fyai_config_set_defaults(&defaults);
		cfg = &defaults;
	}

	fyai_grammar_foreach(grammar) {
		if (!grammar->cli)
			continue;
		if (!prefix) {
			cand(r, fy_sprintfa("%s/", grammar->default_provider),
			     "program");
			continue;
		}
		if (strcmp(prefix, grammar->default_provider))
			continue;
		cand(r, fy_sprintfa("%s/%s", prefix, grammar->default_model),
		     NULL);
		listed = fyai_grammar_catalog_provider(grammar, cfg);
		if (!listed)
			continue;
		fy_foreach(o, fy_get(fyai_catalog_provider(complete_catalog(r),
							   listed),
				     "models", fy_invalid)) {
			canon = fy_get(o, "canonical_id", fy_invalid);
			if (*gstr(&canon))
				cand(r, fy_sprintfa("%s/%s", prefix,
						    gstr(&canon)), NULL);
		}
	}
}

/*
 * A model name, or provider/model for the offering of one provider. A bare
 * provider completes to "provider/" to reach that form. A program that serves
 * models names its own. A decision model is not offered: it cannot chat, and
 * a decider names it.
 */
static void kind_model(struct complete_req *r)
{
	fy_generic cat, m, name, prov, o, canon, p;
	const char *slash;
	char *pfx;

	cat = complete_catalog(r);
	slash = strchr(r->partial, '/');
	if (slash) {
		pfx = strndup(r->partial, (size_t)(slash - r->partial));
		if (!pfx)
			return;
		prov = fyai_catalog_provider(cat, pfx);
		fy_foreach(o, fy_get(prov, "models", fy_invalid)) {
			canon = fy_get(o, "canonical_id", fy_invalid);
			if (fyai_catalog_model_has_cap(fyai_catalog_model(cat,
					gstr(&canon)), "decisions"))
				continue;
			if (*gstr(&canon))
				cand(r, fy_sprintfa("%s/%s", pfx,
						    gstr(&canon)), NULL);
		}
		model_programs(r, pfx);
		free(pfx);
		return;
	}
	fy_foreach(m, fy_get(cat, "models", fy_invalid)) {
		if (fyai_catalog_model_has_cap(m, "decisions"))
			continue;
		name = fy_get(m, "name", fy_invalid);
		cand(r, gstr(&name), NULL);
	}
	fy_foreach(p, fy_get(cat, "providers", fy_invalid)) {
		name = fy_get(p, "name", fy_invalid);
		if (*gstr(&name))
			cand(r, fy_sprintfa("%s/", gstr(&name)), "provider");
	}
	model_programs(r, NULL);
}

static void kind_theme(struct complete_req *r)
{
	const char *const *t;

	for (t = markdown_theme_selectors(); t && *t; t++)
		cand(r, *t, NULL);
}

/*
 * The node of the configuration schema at the directory part of @path;
 * *@basep receives the length of that part, slash included.
 */
static fy_generic config_node(struct complete_req *r, const char *path,
			      size_t *basep)
{
	fy_generic node;
	const char *p, *slash;
	char key[128];
	size_t len;

	node = fyai_config_schema(r->gb);
	*basep = 0;
	for (p = path; (slash = strchr(p, '/')); p = slash + 1) {
		len = (size_t)(slash - p);
		if (len >= sizeof(key))
			return fy_invalid;
		memcpy(key, p, len);
		key[len] = '\0';
		node = fy_get(fy_get(node, "properties", fy_invalid), key,
			      fy_invalid);
		if (!fy_is_valid(node))
			return fy_invalid;
		*basep = (size_t)(slash + 1 - path);
	}
	return node;
}

static bool config_is_object(fy_generic node)
{
	return fy_is_valid(fy_get(node, "properties", fy_invalid));
}

/* Configuration paths, one segment at a time; @suffix ends a leaf. */
static void config_paths(struct complete_req *r, const char *suffix)
{
	fy_generic node, child, d;
	const char *key, *tail;
	size_t base;

	node = config_node(r, r->partial, &base);
	fy_foreach_key_value(key, child, fy_get(node, "properties",
						  fy_invalid)) {
		if (!key)
			continue;
		d = fy_get(child, "description", fy_invalid);
		tail = config_is_object(child) ? "/" : (suffix ? suffix : "");
		cand(r, fy_sprintfa("%.*s%s%s", (int)base, r->partial, key,
				    tail),
		     gstr(&d));
	}
}

static void kind_config_path(struct complete_req *r)
{
	config_paths(r, "");
}

static void complete_kind(struct complete_req *r, const char *kind);
static void complete_remote(struct complete_req *r, const char *kind,
			    const char *prev);

/*
 * The values of a configuration item: its enum, true and false, or the
 * completion kind that the schema names with x-fyai-complete.
 */
static void config_values(struct complete_req *r, fy_generic node,
			  const char *prefix)
{
	fy_generic v, type, kind;
	const char *partial;
	size_t plen;

	fy_foreach(v, fy_get(node, "enum", fy_invalid))
		if (fy_is_string(v))
			cand(r, fy_sprintfa("%s%s", prefix, gstr(&v)), NULL);
	type = fy_get(node, "type", fy_invalid);
	if (fy_equal(type, "boolean")) {
		cand(r, fy_sprintfa("%strue", prefix), NULL);
		cand(r, fy_sprintfa("%sfalse", prefix), NULL);
	}
	kind = fy_get(node, "x-fyai-complete", fy_invalid);
	if (!fy_is_string(kind) || *prefix)
		return;
	/* The kind completes the whole word. */
	partial = r->partial;
	plen = r->plen;
	complete_kind(r, gstr(&kind));
	r->partial = partial;
	r->plen = plen;
}

/* KEY=VALUE: the path before the '=', the values of that item after it. */
static void kind_config_assign(struct complete_req *r)
{
	fy_generic node;
	const char *eq;
	char *path;
	size_t base;

	eq = strchr(r->partial, '=');
	if (!eq) {
		config_paths(r, "=");
		return;
	}
	path = fy_sprintfa("%.*s/", (int)(eq - r->partial), r->partial);
	node = config_node(r, path, &base);
	config_values(r, node, fy_sprintfa("%.*s", (int)(eq + 1 - r->partial),
					   r->partial));
}

/* The value of the configuration item that the previous word names. */
static void kind_config_value(struct complete_req *r, const char *path)
{
	fy_generic node;
	size_t base;

	node = config_node(r, fy_sprintfa("%s/", path), &base);
	config_values(r, node, "");
}

/* A file: the shell lists it; a session lists the directory. */
static void kind_files(struct complete_req *r, bool dirs)
{
	char dir[1024];
	const char *slash;
	struct dirent *de;
	char *full;
	DIR *d;

	r->directives |= dirs ? FYAI_CMD_COMPLETE_DIRS : FYAI_CMD_COMPLETE_FILES;
	if (r->surface != FYAI_CMD_SESSION)
		return;
	slash = strrchr(r->partial, '/');
	snprintf(dir, sizeof(dir), "%.*s", slash ?
		 (int)(slash - r->partial + 1) : 1,
		 slash ? r->partial : ".");
	/* ~/ is read under the home directory; the candidates keep it */
	full = fyai_media_path(dir);
	d = opendir(full ? full : dir);
	free(full);
	if (!d)
		return;
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.' && strncmp(slash ? slash + 1 :
				r->partial, ".", 1))
			continue;
		if (dirs && de->d_type != DT_DIR)
			continue;
		cand(r, fy_sprintfa("%s%s%s", slash ? dir : "", de->d_name,
				    de->d_type == DT_DIR ? "/" : ""), NULL);
	}
	closedir(d);
}

static void kind_file(struct complete_req *r)
{
	kind_files(r, false);
}

static void kind_dir(struct complete_req *r)
{
	kind_files(r, true);
}

static void kind_history_select(struct complete_req *r)
{
	cand(r, "all", "every exchange");
	cand(r, "first", "the first N exchanges");
	cand(r, "last", "the last N exchanges");
	cand(r, "range", "the exchanges A,B");
}


/* ---- the catalogue ---------------------------------------------------------- */

/* A local $ref of the catalogue schema, "#/$defs/NAME". */
static fy_generic catalog_schema_deref(fy_generic root, fy_generic node)
{
	fy_generic ref;
	const char *s;
	int depth;

	for (depth = 0; depth < 8; depth++) {
		ref = fy_get(node, "$ref", fy_invalid);
		s = gstr(&ref);
		if (strncmp(s, "#/$defs/", 8))
			break;
		node = fy_get(fy_get(root, "$defs", fy_invalid), s + 8,
			      fy_invalid);
	}
	return node;
}

/* The key that names an item of a catalogue list, as the catalogue reads it. */
static const char *catalog_item_name(fy_generic item, fy_generic *keep)
{
	*keep = fy_get(item, "name", fy_invalid);
	if (!fy_is_string(*keep))
		*keep = fy_get(item, "canonical_id", fy_invalid);
	return gstr(keep);
}

/*
 * Follow the directory part of @path through the catalogue and its schema.
 * *@basep receives the length of that part, slash included.
 */
static void catalog_walk(struct complete_req *r, const char *path,
			 fy_generic *docp, fy_generic *schemap, size_t *basep)
{
	fy_generic root, doc, schema, item, name;
	const char *p, *slash;
	char seg[128];
	size_t len;

	root = fyai_catalog_schema(r->gb);
	doc = complete_catalog(r);
	schema = root;
	*basep = 0;
	for (p = path; (slash = strchr(p, '/')); p = slash + 1) {
		len = (size_t)(slash - p);
		if (len >= sizeof(seg))
			break;
		memcpy(seg, p, len);
		seg[len] = '\0';
		schema = catalog_schema_deref(root, schema);
		if (fy_is_sequence(doc)) {
			item = fy_invalid;
			fy_foreach(item, doc)
				if (!strcmp(catalog_item_name(item, &name),
					    seg))
					break;
			doc = fy_is_valid(item) &&
			      !strcmp(catalog_item_name(item, &name), seg) ?
			      item : fy_invalid;
			schema = fy_get(schema, "items", fy_invalid);
		} else {
			doc = fy_get(doc, seg, fy_invalid);
			schema = fy_get(fy_get(schema, "properties",
					       fy_invalid), seg, fy_invalid);
		}
		*basep = (size_t)(slash + 1 - path);
	}
	*docp = doc;
	*schemap = catalog_schema_deref(root, schema);
}

static bool catalog_container(fy_generic doc, fy_generic schema)
{
	fy_generic type;

	if (fy_is_mapping(doc) || fy_is_sequence(doc))
		return true;
	type = fy_get(schema, "type", fy_invalid);
	return fy_equal(type, "object") || fy_equal(type, "array");
}

/* Catalogue paths, one segment at a time: keys, and the names of items. */
static void kind_catalog_path(struct complete_req *r)
{
	fy_generic root, doc, schema, child, sub, name, d;
	const char *key;
	size_t base;

	root = fyai_catalog_schema(r->gb);
	catalog_walk(r, r->partial, &doc, &schema, &base);
	if (fy_is_sequence(doc)) {
		fy_foreach(child, doc)
			cand(r, fy_sprintfa("%.*s%s/", (int)base, r->partial,
					    catalog_item_name(child, &name)),
			     NULL);
		return;
	}
	fy_foreach_key_value(key, sub, fy_get(schema, "properties",
					      fy_invalid)) {
		child = fy_get(doc, key, fy_invalid);
		d = fy_get(catalog_schema_deref(root, sub), "description",
			   fy_invalid);
		cand(r, fy_sprintfa("%.*s%s%s", (int)base, r->partial, key,
				    catalog_container(child,
					catalog_schema_deref(root, sub)) ?
				    "/" : ""), gstr(&d));
	}
}

/* The values of the catalogue item that the previous word names. */
static void kind_catalog_value(struct complete_req *r, const char *path)
{
	fy_generic doc, schema;
	size_t base;

	catalog_walk(r, fy_sprintfa("%s/", path), &doc, &schema, &base);
	config_values(r, schema, "");
}

static void kind_catalog_agent(struct complete_req *r)
{
	fy_generic a, name;

	fy_foreach(a, fy_get(complete_catalog(r), "agents", fy_invalid)) {
		name = fy_get(a, "name", fy_invalid);
		cand(r, gstr(&name), NULL);
	}
}

static void kind_mcp_server(struct complete_req *r)
{
	fy_generic key;

	if (!complete_req_ctx(r))
		return;
	fy_foreach(key, complete_req_ctx(r)->cfg->mcp_servers)
		cand(r, gstr(&key), NULL);
}

/* The supporting files of the skill that the previous word names. */
static void kind_skill_file(struct complete_req *r, const char *name)
{
	fy_generic key, files;

	if (!complete_req_ctx(r))
		return;
	files = fy_get(fyai_skill_get(complete_req_ctx(r), name), "files", fy_invalid);
	if (!fy_is_mapping(files))
		return;
	fy_foreach(key, files)
		cand(r, gstr(&key), NULL);
}

/* The skills that the user runs as /NAME, beside the commands of the session. */
static void complete_skills(struct complete_req *r)
{
	fy_generic names, item, name, desc, hint;
	const char *n;

	if (!complete_req_ctx(r))
		return;
	names = fyai_skill_user_names(complete_req_ctx(r), r->gb);
	fy_foreach(item, names) {
		name = fy_get(item, "name", fy_invalid);
		desc = fy_get(item, "description", fy_invalid);
		hint = fy_get(item, "hint", fy_invalid);
		n = gstr(&name);
		/* A command of the same name takes /NAME. */
		if (fyai_cmd_session_exact(n, strlen(n)))
			continue;
		cand(r, n, fy_gb_intern_string(r->gb, fy_sprintfa("skill%s%s: %.*s",
			fy_empty(hint) ? "" : " ", gstr(&hint),
			(int)strcspn(gstr(&desc), "\n"), gstr(&desc))));
	}
}

/* The stored skills of the branch. */
static void kind_skill(struct complete_req *r)
{
	fy_generic key, skills;

	if (!complete_req_ctx(r))
		return;
	skills = fyai_skills(complete_req_ctx(r));
	if (fy_is_invalid(skills))
		return;
	fy_foreach(key, skills)
		cand(r, gstr(&key), NULL);
}

/* The values of the configuration key that the setting names. */
static void kind_setting_value(struct complete_req *r)
{
	fy_generic key, node;

	key = fy_get(fy_get(r->def, "setting", fy_invalid), "key",
		     fy_invalid);
	node = fyai_config_schema_node(gstr(&key));
	if (!fy_is_valid(node) || fy_equal(fy_get(node, "type", fy_invalid),
					   "boolean")) {
		cand(r, "on", NULL);
		cand(r, "off", NULL);
		return;
	}
	config_values(r, node, "");
}

static void page_layout_cand(void *arg, const char *name)
{
	cand(arg, name, "a layout of the page document");
}

/* auto, then the layouts of the page document in use. */
static void kind_page_layout(struct complete_req *r)
{
	cand(r, "auto", "the first layout the terminal is large enough for");
	(void)fyai_page_layout_names(complete_req_ctx(r) ? fyai_ui_page(complete_req_ctx(r)) : NULL,
				     page_layout_cand, r);
}

static void sandbox_profile_cand(void *arg, const char *name, const char *desc)
{
	cand(arg, name, desc);
}

static void kind_sandbox_profile(struct complete_req *r)
{
	if (complete_req_ctx(r))
		fyai_sandbox_profile_names(complete_req_ctx(r)->cfg, complete_req_ctx(r)->cfg->config_doc,
					   sandbox_profile_cand, r);
}

static void help_topic_cand(void *arg, const char *value, const char *desc)
{
	cand(arg, value, desc);
}

static void kind_help_topic(struct complete_req *r)
{
	fyai_cmd_complete_help_topics(r->given, r->ngiven, r->partial,
				      help_topic_cand, r);
}

/*
 * A kind reads data that is local to this process, or data of the process
 * that owns the arena and the project: a mirror asks that one (`worker`).
 */
static const struct {
	const char *name;
	void (*fn)(struct complete_req *r);
	bool worker;
} complete_kinds[] = {
	{ "branch",		kind_branch,		true },
	{ "view",		kind_view,		true },
	{ "agent-branch",	kind_agent_branch,	true },
	{ "session",		kind_session,		true },
	{ "fleet-session",	kind_fleet_session,	true },
	{ "ref",		kind_ref,		true },
	{ "model",		kind_model,		true },
	{ "provider",		kind_provider,		true },
	{ "theme",		kind_theme,		false },
	{ "config-path",	kind_config_path,	false },
	{ "config-assign",	kind_config_assign,	false },
	{ "file",		kind_file,		true },
	{ "dir",		kind_dir,		true },
	{ "help-topic",		kind_help_topic,	false },
	{ "history-select",	kind_history_select,	false },
	{ "catalog-path",	kind_catalog_path,	true },
	{ "catalog-agent",	kind_catalog_agent,	true },
	{ "mcp-server",		kind_mcp_server,	true },
	{ "setting-value",	kind_setting_value,	false },
	{ "page-layout",	kind_page_layout,	false },
	{ "skill",		kind_skill,		true },
	{ "sandbox-profile",	kind_sandbox_profile,	true },
	/* The skills that the user runs as /NAME: asked for by the engine. */
	{ "user-skill",		complete_skills,	true },
};

bool fyai_cmd_kind_known(const char *name)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(complete_kinds); i++)
		if (!strcmp(complete_kinds[i].name, name))
			return true;
	/* The value of the item that a path argument named. */
	return !strcmp(name, "config-value") || !strcmp(name, "catalog-value") ||
	       !strcmp(name, "skill-file");
}

/* ---- candidates of the worker ------------------------------------------------ */

/*
 * A client that has no arena asks its worker for the candidates of a kind
 * that reads data. An answer is kept, so that the keys that follow a word
 * are filtered here and ask nothing: the answer for "ma" has every candidate
 * that "mai" can name. The popup of the library asks for the candidates of
 * a word at once, so the first ask of a word finds none, and the answer is
 * there for the next. The cache is forgotten when a line is sent.
 */
#define COMPLETE_CACHE_MAX	32
#define COMPLETE_ROWS_MAX	FYAI_QUERY_LIMIT_MAX

struct complete_cand {
	char *value;
	char *desc;
};

struct complete_entry {
	struct complete_entry *next;
	struct fyai_ctx *ctx;
	char *kind, *prev, *given, *partial;
	struct complete_cand *cands;
	size_t n;
	unsigned long long id;		/* the query in flight, or 0 */
	bool asking;			/* the query is being made: answered now is in time */
	bool ready;			/* the answer came */
	bool complete;			/* it holds every candidate of the partial */
};

struct fyai_complete_cache {
	struct complete_entry *head;
	size_t n;
};

/* The context that this object stands on. */
static inline struct fyai_ctx *complete_entry_ctx(const struct complete_entry *o)
{
	return o->ctx;
}

static void complete_entry_free(struct complete_entry *e)
{
	size_t i;

	if (e->id)
		fyai_uimodel_query_cancel(complete_entry_ctx(e), e->id);
	for (i = 0; i < e->n; i++) {
		free(e->cands[i].value);
		free(e->cands[i].desc);
	}
	free(e->cands);
	free(e->kind);
	free(e->prev);
	free(e->given);
	free(e->partial);
	free(e);
}

void fyai_cmd_complete_reset(struct fyai_ctx *ctx)
{
	struct fyai_complete_cache *cc;
	struct complete_entry *e;

	cc = ctx ? ctx->screen->complete : NULL;
	if (!cc)
		return;
	while ((e = cc->head)) {
		cc->head = e->next;
		complete_entry_free(e);
	}
	free(cc);
	ctx->screen->complete = NULL;
}

/* The given words of an array argument, one to a line. */
static char *complete_given(const struct complete_req *r)
{
	struct fyai_buf b = { 0 };
	char *joined;
	size_t i;

	for (i = 0; i < r->ngiven; i++)
		if (fyai_buf_append_data(&b, r->given[i], strlen(r->given[i])) ||
		    fyai_buf_append_data(&b, "\n", 1)) {
			free(b.data);
			return NULL;
		}
	joined = strdup(b.data ? b.data : "");
	free(b.data);
	return joined;
}

static void complete_answered(void *arg, fy_generic answer)
{
	struct complete_entry *e = arg;
	fy_generic row, rows = fy_get(answer, "rows", fy_invalid);
	const char *value, *desc;
	size_t n = 0;

	e->id = 0;
	e->ready = true;
	/* An error leaves the entry with no candidates: the same word does not
	 * ask again until the next line. */
	if (!fy_is_sequence(rows) || !fy_len(rows))
		goto out;
	e->cands = calloc(fy_len(rows), sizeof(*e->cands));
	if (!e->cands)
		goto out;
	fy_foreach(row, rows) {
		value = fy_get(row, "value", "");
		desc = fy_get(row, "description", "");
		e->cands[n].value = strdup(value);
		e->cands[n].desc = *desc ? strdup(desc) : NULL;
		if (e->cands[n].value)
			n++;
	}
	e->n = n;
	e->complete = fy_is_null(fy_get(answer, "next", fy_null));
out:
	/* An answer that comes after the popup asked is given to it again. */
	if (!e->asking)
		fyai_ui_completion_refresh(complete_entry_ctx(e));
}

static void complete_entry_emit(struct complete_req *r,
				const struct complete_entry *e)
{
	size_t i;

	for (i = 0; i < e->n; i++)
		cand(r, e->cands[i].value, e->cands[i].desc);
}

static void complete_remote(struct complete_req *r, const char *kind,
			    const char *prev)
{
	struct fyai_ctx *ctx = complete_req_ctx(r);
	struct fyai_complete_cache *cc = ctx->screen->complete;
	struct complete_entry *e, *oldest, **pp;
	fy_generic given = fy_seq_empty, params, request;
	char *joined;
	size_t i;
	int rc;

	joined = complete_given(r);
	if (!joined)
		return;
	if (!cc) {
		cc = calloc(1, sizeof(*cc));
		if (!cc)
			goto out;
		ctx->screen->complete = cc;
	}
	for (e = cc->head; e; e = e->next) {
		if (strcmp(e->kind, kind) || strcmp(e->given, joined) ||
		    (e->prev ? !prev || strcmp(e->prev, prev) : prev != NULL) ||
		    strncmp(r->partial, e->partial, strlen(e->partial)))
			continue;
		/* One that waits will serve, and one that has the whole set
		 * serves any longer word. */
		if (!e->ready)
			goto out;
		/* A model word with a slash names another set of candidates
		 * than one without it. */
		if (!strcmp(e->partial, r->partial) ||
		    (e->complete && (strcmp(kind, "model") ||
				     !!strchr(e->partial, '/') ==
				     !!strchr(r->partial, '/')))) {
			complete_entry_emit(r, e);
			goto out;
		}
	}
	if (cc->n >= COMPLETE_CACHE_MAX) {
		/* The oldest is the last: the new one goes in front. */
		for (pp = &cc->head; (*pp)->next; pp = &(*pp)->next)
			;
		oldest = *pp;
		*pp = NULL;
		complete_entry_free(oldest);
		cc->n--;
	}
	e = calloc(1, sizeof(*e));
	if (!e)
		goto out;
	e->ctx = ctx;
	e->kind = strdup(kind);
	e->prev = prev ? strdup(prev) : NULL;
	e->given = joined;
	joined = NULL;
	e->partial = strdup(r->partial);
	if (!e->kind || !e->partial || (prev && !e->prev)) {
		complete_entry_free(e);
		goto out;
	}
	e->next = cc->head;
	cc->head = e;
	cc->n++;
	for (i = 0; i < r->ngiven; i++)
		given = fy_append(r->gb, given, fy_value(r->gb, r->given[i]));
	params = fy_mapping(r->gb, "kind", kind, "partial", r->partial,
			    "given", given);
	if (prev)
		params = fy_assoc(r->gb, params, "prev", prev);
	request = fy_mapping(r->gb, "what", "complete", "params", params,
			     "limit", (long long)COMPLETE_ROWS_MAX);
	e->asking = true;
	rc = fyai_uimodel_query(ctx, request, complete_answered, e, &e->id);
	e->asking = false;
	/* The owner of a mirror that answers at once is served at once. */
	if (!rc && e->ready)
		complete_entry_emit(r, e);
out:
	free(joined);
}

/* What the worker gives: the candidates of a kind, as rows. */
struct complete_rows {
	struct fy_generic_builder *gb;
	fy_generic rows;
	size_t n;
};

static void complete_rows_add(void *arg, const char *value, const char *desc)
{
	struct complete_rows *cr = arg;
	fy_generic row;

	if (cr->n >= COMPLETE_ROWS_MAX)
		return;
	row = fy_mapping(cr->gb, "value", value, "description", desc ? desc : "");
	cr->rows = fy_append(cr->gb, cr->rows, row);
	cr->n++;
}

fy_generic fyai_cmd_complete_answer(struct fyai_ctx *ctx,
				    struct fy_generic_builder *gb,
				    fy_generic request)
{
	struct fy_generic_builder_cfg cfg = {
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED,
	};
	fy_generic params = fy_get(request, "params", fy_map_empty);
	fy_generic gv, word, inner;
	const char *kind = fy_get(params, "kind", "");
	const char *prev = fy_get(params, "prev", (const char *)NULL);
	const char *given[16];
	struct complete_rows cr = { .gb = gb, .rows = fy_seq_empty };
	struct complete_req r;
	bool known = false;
	size_t i, n = 0;

	for (i = 0; i < ARRAY_SIZE(complete_kinds); i++)
		known = known || (complete_kinds[i].worker &&
				  !strcmp(complete_kinds[i].name, kind));
	if (prev)
		known = !strcmp(kind, "catalog-value") ||
			!strcmp(kind, "skill-file");
	if (!known) {
		inner = fy_mapping(gb, "code", FYAI_QUERY_UNSUPPORTED, "message",
				   "this worker does not complete that kind");
		return fy_mapping(gb, "error", inner);
	}
	fy_foreach(gv, fy_get(params, "given", fy_seq_empty)) {
		word = gv;
		if (n < ARRAY_SIZE(given) && fy_is_string(word))
			given[n++] = fy_castp(&gv, "");
	}
	memset(&r, 0, sizeof(r));
	r.ctx = ctx;
	r.def = fy_invalid;
	r.surface = FYAI_CMD_SESSION;
	r.partial = fy_get(params, "partial", "");
	r.plen = strlen(r.partial);
	r.add = complete_rows_add;
	r.arg = &cr;
	r.given = given;
	r.ngiven = n;
	r.gb = fy_generic_builder_create(&cfg);
	if (!r.gb) {
		inner = fy_mapping(gb, "code", "busy", "message",
				   "cannot make a builder for the completion");
		return fy_mapping(gb, "error", inner);
	}
	if (prev && !strcmp(kind, "catalog-value"))
		kind_catalog_value(&r, prev);
	else if (prev)
		kind_skill_file(&r, prev);
	else
		complete_kind(&r, kind);
	fy_generic_builder_destroy(r.gb);
	return fy_mapping(gb, "what", "complete", "rows", cr.rows, "next",
			  cr.n >= COMPLETE_ROWS_MAX ? fy_value(gb, "more") :
			  fy_null, "total", fy_value(gb, (long long)cr.n));
}

/* ---- the engine -------------------------------------------------------------- */

static void complete_value(struct complete_req *r, struct fyai_cmd_prop *cp,
			   const char *prev_value)
{
	fy_generic en, v, kind, items;
	const char *k;

	en = fy_get(cp->p, "enum", fy_invalid);
	if (!fy_is_valid(en) && cp->array) {
		items = fy_get(cp->p, "items", fy_invalid);
		en = fy_get(items, "enum", fy_invalid);
	}
	fy_foreach(v, en)
		if (fy_is_string(v))
			cand(r, gstr(&v), NULL);
	kind = fy_get(cp->p, "x-fyai-complete", fy_invalid);
	k = gstr(&kind);
	if (!strcmp(k, "config-value")) {
		if (prev_value)
			kind_config_value(r, prev_value);
		return;
	}
	if (!strcmp(k, "catalog-value")) {
		if (prev_value && r->remote)
			complete_remote(r, k, prev_value);
		else if (prev_value)
			kind_catalog_value(r, prev_value);
		return;
	}
	if (!strcmp(k, "skill-file")) {
		if (prev_value && r->remote)
			complete_remote(r, k, prev_value);
		else if (prev_value)
			kind_skill_file(r, prev_value);
		return;
	}
	complete_kind(r, k);
}

static void complete_kind(struct complete_req *r, const char *kind)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(complete_kinds); i++) {
		if (strcmp(complete_kinds[i].name, kind))
			continue;
		if (complete_kinds[i].worker && r->remote)
			complete_remote(r, kind, NULL);
		else
			complete_kinds[i].fn(r);
	}
}

static void complete_options(struct complete_req *r, struct fyai_cmd_prop *props,
			     int n, fy_generic given)
{
	fy_generic d;
	int i;

	for (i = 0; i < n; i++) {
		if (props[i].pos >= 0 || props[i].hidden)
			continue;
		if (!props[i].repeat &&
		    fy_is_valid(fy_get(given, props[i].name, fy_invalid)))
			continue;
		d = fy_get(props[i].p, "description", fy_invalid);
		cand(r, fy_sprintfa("--%s", props[i].lname), gstr(&d));
	}
	if (r->surface == FYAI_CMD_CLI)
		cand(r, "--output", "markdown, json, or yaml");
	cand(r, "--help", "show the help of the command");
}

/* A word that names a command, with the title of the command. Both are owned. */
struct cmd_word {
	char *name;
	char *title;
};

struct cmd_words {
	struct cmd_word *v;
	size_t n, cap;
};

static int cmd_words_push(struct cmd_words *ws, const char *name,
			  const char *title)
{
	struct cmd_word *v;
	size_t cap;

	if (ws->n == ws->cap) {
		cap = ws->cap ? ws->cap * 2 : 64;
		v = realloc(ws->v, cap * sizeof(*v));
		if (!v)
			return -1;
		ws->v = v;
		ws->cap = cap;
	}
	ws->v[ws->n].name = strdup(name);
	ws->v[ws->n].title = strdup(title);
	if (!ws->v[ws->n].name || !ws->v[ws->n].title) {
		free(ws->v[ws->n].name);
		free(ws->v[ws->n].title);
		return -1;
	}
	ws->n++;
	return 0;
}

/*
 * Collect the name of @def and its aliases. An alias is a word that names
 * the command, so it completes as one.
 */
static int cmd_words_add(struct cmd_words *ws, fy_generic def)
{
	fy_generic gname, gtitle;
	const char *alias;
	int rc;

	gname = fy_get(def, "command", fy_invalid);
	gtitle = fy_get(def, "title", fy_invalid);
	rc = cmd_words_push(ws, gstr(&gname), gstr(&gtitle));
	fy_foreach(alias, fy_get(def, "aliases", fy_invalid))
		rc |= cmd_words_push(ws, alias, gstr(&gtitle));
	return rc;
}

static int cmd_word_cmp(const void *a, const void *b)
{
	return strcmp(((const struct cmd_word *)a)->name,
		      ((const struct cmd_word *)b)->name);
}

/* Offer the collected words in order, and release them. */
static void cmd_words_emit(struct complete_req *r, struct cmd_words *ws)
{
	size_t i;

	qsort(ws->v, ws->n, sizeof(*ws->v), cmd_word_cmp);
	for (i = 0; i < ws->n; i++) {
		cand(r, ws->v[i].name, ws->v[i].title);
		free(ws->v[i].name);
		free(ws->v[i].title);
	}
	free(ws->v);
	memset(ws, 0, sizeof(*ws));
}

static void complete_subcommands(struct complete_req *r, fy_generic group,
				 fy_generic surfaces)
{
	struct cmd_words ws = { 0 };
	fy_generic sub, *subs;
	size_t n, i;

	subs = fyai_cmd_defs_sorted(fy_get(group, "commands", fy_invalid),
				    "command", &n);
	for (i = 0; i < n; i++) {
		sub = subs[i];
		if (!fyai_cmd_def_on(sub, surfaces, r->surface) ||
		    fy_get(sub, "hidden", false))
			continue;
		if (cmd_words_add(&ws, sub))
			fyai_error(complete_req_ctx(r), "cannot collect the commands to complete");
	}
	free(subs);
	cmd_words_emit(r, &ws);
}

/* The next positional argument of the innermost group on the path. */
static void complete_group_arg(struct complete_req *r, struct fyai_cmd_walk *w)
{
	struct fyai_cmd_prop props[FYAI_CMD_MAX_PROPS];
	int n, k;

	if (!fy_is_valid(w->last_group))
		return;
	n = fyai_cmd_props(w->last_group, r->surface, props,
			   ARRAY_SIZE(props));
	for (k = 0; k < n; k++)
		if (props[k].pos == w->last_group_pos)
			complete_value(r, &props[k], NULL);
}

/* Complete the arguments of the command @def after @i words. */
static void complete_args(struct complete_req *r, fy_generic def,
			  size_t nwords, const char *const *words, size_t i)
{
	struct fyai_cmd_prop props[FYAI_CMD_MAX_PROPS], *cp, *pending;
	fy_generic given;
	const char *w, *last_value;
	bool opts_done, neg;
	long long npos;
	size_t array_start;
	int n, k;

	n = fyai_cmd_props(def, r->surface, props, ARRAY_SIZE(props));
	if (n < 0)
		return;
	r->def = def;
	given = fy_map_empty;
	pending = NULL;
	opts_done = false;
	npos = 0;
	last_value = NULL;
	array_start = SIZE_MAX;
	for (; i + 1 < nwords; i++) {
		w = words[i];
		if (pending) {
			last_value = w;
			pending = NULL;
			continue;
		}
		if (!opts_done && !strcmp(w, "--")) {
			opts_done = true;
			continue;
		}
		if (!opts_done && w[0] == '-' && w[1]) {
			if (!strcmp(w, "--output") && r->surface == FYAI_CMD_CLI) {
				if (i + 2 == nwords) {
					cand(r, "markdown", NULL);
					cand(r, "json", NULL);
					cand(r, "yaml", NULL);
					return;
				}
				i++;
				continue;
			}
			cp = NULL;
			neg = false;
			for (k = 0; k < n; k++) {
				if (props[k].pos >= 0)
					continue;
				if ((w[1] == '-' && !strcmp(w + 2,
							    props[k].lname)) ||
				    (w[1] != '-' && !w[2] &&
				     w[1] == props[k].sname)) {
					cp = &props[k];
					break;
				}
			}
			if (!cp)
				continue;
			given = fy_assoc(r->gb, given, cp->name, fy_true);
			if (!cp->boolean && !strchr(w, '='))
				pending = cp;
			(void)neg;
			continue;
		}
		cp = fyai_cmd_prop_positional(props, n, npos);
		if (cp && (cp->rest))
			return;
		last_value = w;
		if (cp && !cp->array)
			npos++;
		else if (cp && array_start == SIZE_MAX)
			array_start = i;
	}
	if (pending) {
		complete_value(r, pending, last_value);
		return;
	}
	if (!opts_done && r->partial[0] == '-') {
		if (!strcmp(r->partial, "--output=") ||
		    !strncmp(r->partial, "--output=", 9))
			return;
		complete_options(r, props, n, given);
		return;
	}
	cp = fyai_cmd_prop_positional(props, n, npos);
	if (cp && cp->array && array_start != SIZE_MAX) {
		r->given = words + array_start;
		r->ngiven = nwords - 1 - array_start;
	}
	if (cp)
		complete_value(r, cp, last_value);
}

/* Skip the global options of a verb line; returns the index of the verb. */
static size_t complete_globals(struct complete_req *r, size_t nwords,
			       const char *const *words, bool *donep)
{
	struct fyai_cmd_prop props[FYAI_CMD_MAX_PROPS], *cp;
	fy_generic global;
	const char *w;
	size_t i;
	int n, k;

	*donep = false;
	global = fy_get(fyai_cmd_registry(), "global", fy_invalid);
	n = fyai_cmd_props(global, FYAI_CMD_CLI, props, ARRAY_SIZE(props));
	if (n < 0)
		n = 0;
	for (i = 0; i + 1 < nwords && words[i][0] == '-'; i++) {
		w = words[i];
		cp = NULL;
		for (k = 0; k < n; k++)
			if ((w[1] == '-' && !strcmp(w + 2, props[k].lname)) ||
			    (w[1] != '-' && !w[2] && w[1] == props[k].sname))
				cp = &props[k];
		if (!cp || cp->boolean || strchr(w, '='))
			continue;
		if (i + 2 == nwords) {
			complete_value(r, cp, NULL);
			*donep = true;
			return i;
		}
		i++;
	}
	if (i + 1 == nwords && r->partial[0] == '-') {
		complete_options(r, props, n, fy_map_empty);
		*donep = true;
	}
	return i;
}

unsigned int fyai_cmd_complete(struct fyai_ctx *ctx,
			       enum fyai_cmd_surface surface,
			       size_t nwords, const char *const *words,
			       fyai_cmd_candidate_fn add, void *arg)
{
	struct fy_generic_builder_cfg cfg = {
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED,
	};
	struct complete_req r;
	struct fyai_cmd_walk w;
	struct cmd_words ws = { 0 };
	fy_generic reg, def, *defs;
	size_t start, nd, d;
	bool done;

	if (!nwords || !fy_is_valid(fyai_cmd_registry()))
		return 0;
	memset(&r, 0, sizeof(r));
	r.ctx = ctx;
	r.def = fy_invalid;
	r.surface = surface;
	r.partial = words[nwords - 1];
	r.plen = strlen(r.partial);
	r.add = add;
	r.arg = arg;
	/* The session of a client that has no arena asks its worker. */
	r.remote = ctx && fyai_uimodel_mirrored(ctx);
	r.gb = fy_generic_builder_create(&cfg);
	if (!r.gb)
		return 0;

	start = 0;
	if (surface == FYAI_CMD_CLI) {
		start = complete_globals(&r, nwords, words, &done);
		if (done)
			goto out;
	}
	if (start + 1 == nwords) {
		reg = fyai_cmd_registry();
		defs = fyai_cmd_defs_sorted(fy_get(reg, "commands",
						   fy_invalid), "command", &nd);
		for (d = 0; d < nd; d++) {
			def = defs[d];
			if (!fyai_cmd_def_on(def, fy_invalid, surface) ||
			    fy_get(def, "hidden", false))
				continue;
			if (cmd_words_add(&ws, def))
				fyai_error(ctx, "cannot collect the commands to complete");
		}
		free(defs);
		cmd_words_emit(&r, &ws);
		/* A skill runs as /NAME, so the session offers it with the commands. */
		if (surface == FYAI_CMD_SESSION)
			complete_kind(&r, "user-skill");
		goto out;
	}
	if (fyai_cmd_walk(surface, nwords - start, words + start,
			  nwords - start - 1, &w))
		goto out;
	/* A group with no default: its subcommands. */
	if (fy_is_valid(fy_get(w.def, "commands", fy_invalid))) {
		if (start + w.consumed + 1 == nwords) {
			complete_subcommands(&r, w.def, w.surfaces);
			complete_group_arg(&r, &w);
		}
		goto out;
	}
	/* A word that a default took can still name a subcommand. */
	if (fy_is_valid(w.group) && start + w.consumed + 1 == nwords &&
	    r.partial[0] != '-') {
		complete_subcommands(&r, w.group, w.group_surfaces);
		complete_group_arg(&r, &w);
	}
	complete_args(&r, w.def, nwords - start, words + start, w.consumed);
out:
	fy_generic_builder_destroy(r.gb);
	return r.directives;
}

/* ---- the session ------------------------------------------------------------ */

struct session_cands {
	const char *line;
	size_t keep;			/* bytes of the line before the word */
	fyai_cmd_candidate_fn add;
	void *arg;
};

static void session_cand(void *arg, const char *value, const char *desc)
{
	struct session_cands *sc = arg;
	size_t len = strlen(value);
	bool more = len && (value[len - 1] == '/' || value[len - 1] == '=');

	/*
	 * A taken candidate ends the word, so the next one can follow at once. A
	 * path segment or an assignment goes on in the same word.
	 */
	sc->add(sc->arg, fy_sprintfa("%.*s%s%s", (int)sc->keep, sc->line, value,
				     more ? "" : " "), desc);
}

void fyai_cmd_session_complete(struct fyai_ctx *ctx, const char *buf,
			       fyai_cmd_candidate_fn add, void *arg)
{
	struct session_cands sc;
	char **words;
	size_t *offs;
	int n;

	if (!buf || buf[0] != '/')
		return;
	n = fyai_cmd_split(buf + 1, true, &words, &offs);
	if (n <= 0) {
		fyai_cmd_split_free(words, n, offs);
		return;
	}
	sc.line = buf;
	sc.keep = 1 + offs[n - 1];
	sc.add = add;
	sc.arg = arg;
	(void)fyai_cmd_complete(ctx, FYAI_CMD_SESSION, (size_t)n,
				(const char *const *)words, session_cand, &sc);
	fyai_cmd_split_free(words, n, offs);
}

size_t fyai_cmd_session_word(const char *buf)
{
	char **words;
	size_t *offs, off;
	int n;

	if (!buf || buf[0] != '/')
		return 0;
	n = fyai_cmd_split(buf + 1, true, &words, &offs);
	off = n > 0 ? 1 + offs[n - 1] : 0;
	fyai_cmd_split_free(words, n, offs);
	return off;
}

void fyai_cmd_complete_help_topics(const char *const *path, size_t npath,
				   const char *partial,
				   fyai_cmd_candidate_fn add, void *arg)
{
	fy_generic reg, level, def, found, topic, name, title, *defs;
	size_t len = strlen(partial);
	size_t i, nd, d;

	reg = fyai_cmd_registry();
	/* A command path descends into the commands of each group it names. */
	level = reg;
	for (i = 0; i < npath; i++) {
		found = fy_invalid;
		fy_foreach(def, fy_get(level, "commands", fy_invalid)) {
			name = fy_get(def, "command", fy_invalid);
			if (!strcmp(gstr(&name), path[i])) {
				found = def;
				break;
			}
		}
		/* A topic, or a command without subcommands, ends the path. */
		if (!fy_is_valid(found))
			return;
		level = found;
	}
	defs = fyai_cmd_defs_sorted(fy_get(level, "commands", fy_invalid),
				    "command", &nd);
	for (d = 0; d < nd; d++) {
		def = defs[d];
		if (fy_get(def, "hidden", false))
			continue;
		name = fy_get(def, "command", fy_invalid);
		title = fy_get(def, "title", fy_invalid);
		if (!strncmp(gstr(&name), partial, len))
			add(arg, gstr(&name), gstr(&title));
	}
	free(defs);
	/* A topic stands alone: it is not a step of a command path. */
	if (npath)
		return;
	defs = fyai_cmd_defs_sorted(fy_get(reg, "topics", fy_invalid),
				    "topic", &nd);
	for (d = 0; d < nd; d++) {
		topic = defs[d];
		name = fy_get(topic, "topic", fy_invalid);
		title = fy_get(topic, "title", fy_invalid);
		if (!strncmp(gstr(&name), partial, len))
			add(arg, gstr(&name), gstr(&title));
	}
	free(defs);
}

/* ---- the verbs ------------------------------------------------------------ */

struct machine_cands {
	struct fyai_ctx *ctx;
	struct fyai_buf out;
	bool fail;
};

/* The context that this object stands on. */
__attribute__((used)) static inline struct fyai_ctx *
machine_cands_ctx(const struct machine_cands *o)
{
	return o->ctx;
}

static void machine_cand(void *arg, const char *value, const char *desc)
{
	struct machine_cands *mc = arg;
	const char *p;
	size_t n;

	if (mc->fail || fyai_buf_append(&mc->out, value)) {
		mc->fail = true;
		return;
	}
	if (desc && *desc) {
		if (fyai_buf_append(&mc->out, "\t")) {
			mc->fail = true;
			return;
		}
		/* A description is one short line: its first sentence. */
		for (p = desc, n = 0; *p && *p != '\n' && n < 72; p++, n++) {
			if (*p == '.' && (!p[1] || p[1] == ' '))
				break;
			if (fyai_buf_append_data(&mc->out, p, 1)) {
				mc->fail = true;
				return;
			}
		}
	}
	if (fyai_buf_append(&mc->out, "\n"))
		mc->fail = true;
}

int fyai_cmd_complete_verb(struct fyai_cmd_call *call, fy_generic *result)
{
	struct machine_cands mc;
	const char *words[256];
	fy_generic w;
	unsigned int directives;
	size_t n;
	char tail[32];
	int rc;

	(void)result;
	n = 0;
	fy_foreach(w, fy_get(call->args, "words", fy_invalid)) {
		fyai_error_check(fyai_cmd_call_ctx(call), n < ARRAY_SIZE(words), err,
				 "__complete: too many words");
		words[n++] = fy_gb_intern_string(call->gb, fy_castp(&w, ""));
	}
	/* No word yet: the empty word that the shell completes. */
	if (!n)
		words[n++] = "";
	memset(&mc, 0, sizeof(mc));
	mc.ctx = fyai_cmd_call_ctx(call);
	directives = fyai_cmd_complete(fyai_cmd_call_ctx(call), FYAI_CMD_CLI, n, words,
				       machine_cand, &mc);
	snprintf(tail, sizeof(tail), ":%u\n", directives);
	if (!mc.fail && fyai_buf_append(&mc.out, tail))
		mc.fail = true;
	fyai_error_check(fyai_cmd_call_ctx(call), !mc.fail, err_free,
			 "__complete: cannot build the candidates");
	/* The protocol of the completion scripts: never rendered. */
	rc = fyai_cmd_machine_write(call, mc.out.data, mc.out.len);
	free(mc.out.data);
	return rc ? -1 : 0;
err_free:
	free(mc.out.data);
err:
	return -1;
}

static const char completion_bash[] =
"# bash completion for fyai; load with: source <(fyai completion bash)\n"
"_fyai_complete()\n"
"{\n"
"    local cur words cword out line directive\n"
"    local -a cands\n"
"    if declare -F _get_comp_words_by_ref >/dev/null; then\n"
"        _get_comp_words_by_ref -n '=:@' cur words cword\n"
"    else\n"
"        cur=${COMP_WORDS[COMP_CWORD]}\n"
"        words=(\"${COMP_WORDS[@]}\")\n"
"        cword=$COMP_CWORD\n"
"    fi\n"
"    out=$(\"${words[0]}\" __complete -- \"${words[@]:1:cword}\" 2>/dev/null) || return\n"
"    directive=${out##*$'\\n'}\n"
"    directive=${directive#:}\n"
"    cands=()\n"
"    while IFS= read -r line; do\n"
"        [[ $line == :* ]] && continue\n"
"        cands+=(\"${line%%$'\\t'*}\")\n"
"    done <<< \"$out\"\n"
"    COMPREPLY=()\n"
"    if (( directive & 2 )); then\n"
"        COMPREPLY=($(compgen -f -- \"$cur\"))\n"
"    elif (( directive & 4 )); then\n"
"        COMPREPLY=($(compgen -d -- \"$cur\"))\n"
"    fi\n"
"    COMPREPLY+=(\"${cands[@]}\")\n"
"    (( directive & 1 )) && compopt -o nospace 2>/dev/null\n"
"    if declare -F __ltrim_colon_completions >/dev/null; then\n"
"        __ltrim_colon_completions \"$cur\"\n"
"    fi\n"
"}\n"
"complete -F _fyai_complete fyai\n";

static const char completion_zsh[] =
"#compdef fyai\n"
"# zsh completion for fyai; load with: source <(fyai completion zsh)\n"
"_fyai() {\n"
"    local out directive line\n"
"    local -a cands\n"
"    out=$(${words[1]} __complete -- \"${(@)words[2,CURRENT]}\" 2>/dev/null) || return\n"
"    directive=${${(f)out}[-1]#:}\n"
"    for line in ${(f)out}; do\n"
"        [[ $line == :* ]] && continue\n"
"        cands+=(\"${line//:/\\\\:}\")\n"
"    done\n"
"    cands=(\"${(@)cands//$'\\t'/:}\")\n"
"    if (( directive & 2 )); then\n"
"        _files\n"
"    elif (( directive & 4 )); then\n"
"        _files -/\n"
"    fi\n"
"    if (( directive & 1 )); then\n"
"        _describe -t fyai 'fyai' cands -S ''\n"
"    else\n"
"        _describe -t fyai 'fyai' cands\n"
"    fi\n"
"}\n"
"compdef _fyai fyai\n";

static const char completion_fish[] =
"# fish completion for fyai; load with: fyai completion fish | source\n"
"function __fyai_complete\n"
"    set -l words (commandline -opc)\n"
"    set -e words[1]\n"
"    set -l cur (commandline -ct)\n"
"    set -l out (fyai __complete -- $words \"$cur\" 2>/dev/null)\n"
"    or return\n"
"    set -l directive (string replace -r '^:' '' -- $out[-1])\n"
"    for line in $out[1..-2]\n"
"        echo $line\n"
"    end\n"
"    if test (math \"$directive % 4 >= 2\") = 1\n"
"        __fish_complete_path \"$cur\"\n"
"    else if test (math \"$directive % 8 >= 4\") = 1\n"
"        __fish_complete_directories \"$cur\"\n"
"    end\n"
"end\n"
"complete -c fyai -f -a '(__fyai_complete)'\n";

int fyai_cmd_completion(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *shell = fyai_cmd_arg_str(call, "shell");
	const char *script;

	(void)result;
	if (!strcmp(shell, "zsh"))
		script = completion_zsh;
	else if (!strcmp(shell, "fish"))
		script = completion_fish;
	else
		script = completion_bash;
	/* A script is written as it stands. */
	return fyai_cmd_machine_write(call, script, strlen(script)) ? -1 : 0;
}
