/*
 * fyai_cmd.c - schema-defined commands: registry, parser, and dispatch
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>

#include "fyai.h"
#include "fyai_cmd.h"
#include "fyai_cmd_int.h"
#include "fyai_event.h"
#include "fyai_schema.h"
#include "fyai_config.h"
#include "fyai_sink.h"
#include "fyai_render.h"
#include "fyai_ui.h"
#include "commands.h"
#include "utils.h"

#define FYAI_MODULE FYAIEM_UNKNOWN

/* The embedded definitions, data/commands.yaml, and their schema. */
#include "embedded_commands.inc"
#include "embedded_command_schema.inc"

static const struct {
	const char *name;
	fyai_cmd_fn fn;
} cmd_handlers[] = {
	{ "help",		fyai_cmd_help },
	{ "completion",		fyai_cmd_completion },
	{ "complete",		fyai_cmd_complete_verb },
	{ "branch_list",	fyai_cmd_branch_list },
	{ "branch_new",		fyai_cmd_branch_new },
	{ "branch_delete",	fyai_cmd_branch_delete },
	{ "branch_rename",	fyai_cmd_branch_rename },
	{ "branch_show",	fyai_cmd_branch_show },
	{ "branch_describe",	fyai_cmd_branch_describe },
	{ "branch_switch",	fyai_cmd_branch_switch },
	{ "branch_attach",	fyai_cmd_branch_attach },
	{ "branch_detach",	fyai_cmd_branch_detach },
	{ "checkout",		fyai_cmd_checkout },
	{ "model",		fyai_cmd_model },
	{ "config_show",	fyai_cmd_config_show },
	{ "config_effective",	fyai_cmd_config_effective },
	{ "config_get",		fyai_cmd_config_get },
	{ "config_set",		fyai_cmd_config_set },
	{ "config_delete",	fyai_cmd_config_delete },
	{ "config_import",	fyai_cmd_config_import },
	{ "config_export",	fyai_cmd_config_export },
	{ "config_validate",	fyai_cmd_config_validate },
	{ "config_schema",	fyai_cmd_config_schema },
	{ "config_edit",	fyai_cmd_config_edit },
	{ "config_describe",	fyai_cmd_config_describe },
	{ "sandbox_show",	fyai_cmd_sandbox_show },
	{ "sandbox_set",	fyai_cmd_sandbox_set },
	{ "history",		fyai_cmd_history },
	{ "reset",		fyai_cmd_reset },
	{ "clear",		fyai_cmd_clear },
	{ "api",		fyai_cmd_api },
	{ "context",		fyai_cmd_context },
	{ "stats",		fyai_cmd_stats },
	{ "list",		fyai_cmd_list },
	{ "diff",		fyai_cmd_diff },
	{ "root",		fyai_cmd_root },
	{ "join",		fyai_cmd_join },
	{ "gc",			fyai_cmd_gc },
	{ "export",		fyai_cmd_export },
	{ "log",		fyai_cmd_log },
	{ "secret",		fyai_cmd_secret },
	{ "compact",		fyai_cmd_compact },
	{ "dump",		fyai_cmd_dump },
	{ "import",		fyai_cmd_import },
	{ "replay",		fyai_cmd_replay },
	{ "tool",		fyai_cmd_tool },
	{ "term",		fyai_cmd_term },
	{ "init",		fyai_cmd_init },
	{ "mcp_status",		fyai_cmd_mcp_status },
	{ "mcp_login",		fyai_cmd_mcp_login },
	{ "mcp_enable",		fyai_cmd_mcp_enable },
	{ "mcp_import_client",	fyai_cmd_mcp_import_client },
	{ "exit",		fyai_cmd_exit },
	{ "reload",		fyai_cmd_reload },
	{ "btw",		fyai_cmd_btw },
	{ "branches",		fyai_cmd_branches },
	{ "resume",		fyai_cmd_resume },
	{ "zoom",		fyai_cmd_zoom },
	{ "page",		fyai_cmd_page },
	{ "page_review",	fyai_cmd_page_review },
	{ "page_review_sample",	fyai_cmd_page_review_sample },
	{ "sessions",		fyai_cmd_sessions },
	{ "kill",		fyai_cmd_kill },
	{ "status",		fyai_cmd_status },
	{ "agent",		fyai_cmd_agent },
	{ "transport",		fyai_cmd_transport },
	{ "setting",		fyai_cmd_setting },
	{ "catalog_show",	fyai_cmd_catalog_show },
	{ "catalog_list",	fyai_cmd_catalog_list },
	{ "render",		fyai_cmd_render },
	{ "catalog_tools",	fyai_cmd_catalog_tools },
	{ "catalog_get",	fyai_cmd_catalog_get },
	{ "catalog_set",	fyai_cmd_catalog_set },
	{ "catalog_delete",	fyai_cmd_catalog_delete },
	{ "catalog_import",	fyai_cmd_catalog_import },
	{ "catalog_export",	fyai_cmd_catalog_export },
	{ "catalog_validate",	fyai_cmd_catalog_validate },
	{ "catalog_schema",	fyai_cmd_catalog_schema },
	{ "catalog_reset",	fyai_cmd_catalog_reset },
	{ "catalog_edit",	fyai_cmd_catalog_edit },
	{ "catalog_update",	fyai_cmd_catalog_update },
	{ "auth_accounts",	fyai_cmd_auth_accounts },
	{ "auth_status",	fyai_cmd_auth_status },
	{ "auth_usage",		fyai_cmd_auth_usage },
	{ "auth_login",		fyai_cmd_auth_login },
	{ "auth_logout",	fyai_cmd_auth_logout },
};

static const struct {
	const char *name;
	fyai_cmd_prepare_fn fn;
} cmd_earlies[] = {
	{ "resume",		fyai_cmd_resume_early },
	{ "transport",		fyai_cmd_transport_early },
};

static const struct {
	const char *name;
	fyai_cmd_prepare_fn fn;
} cmd_prepares[] = {
	{ "term",		fyai_cmd_term_prepare },
	{ "init",		fyai_cmd_init_prepare },
	{ "agent",		fyai_cmd_agent_prepare },
	{ "resume",		fyai_cmd_resume_prepare },
};

fyai_cmd_prepare_fn fyai_cmd_early_hook(const char *name)
{
	size_t i;

	for (i = 0; name && i < ARRAY_SIZE(cmd_earlies); i++)
		if (!strcmp(cmd_earlies[i].name, name))
			return cmd_earlies[i].fn;
	return NULL;
}

fyai_cmd_prepare_fn fyai_cmd_prepare(const char *name)
{
	size_t i;

	for (i = 0; name && i < ARRAY_SIZE(cmd_prepares); i++)
		if (!strcmp(cmd_prepares[i].name, name))
			return cmd_prepares[i].fn;
	return NULL;
}

fyai_cmd_fn fyai_cmd_handler(const char *name)
{
	size_t i;

	for (i = 0; name && i < ARRAY_SIZE(cmd_handlers); i++)
		if (!strcmp(cmd_handlers[i].name, name))
			return cmd_handlers[i].fn;
	return NULL;
}

/* ---- registry ------------------------------------------------------------ */

/*
 * The registry is parsed and checked one time. The builder lives as long as
 * the process and the document is read only, as the embedded page is.
 */
static struct fy_generic_builder *reg_gb;
static fy_generic reg_doc = fy_invalid;
static bool reg_tried;
static char reg_why[512];

static void reg_fail(const char *fmt, ...)
	__attribute__((format(printf, 1, 2)));

static void reg_fail(const char *fmt, ...)
{
	va_list ap;

	if (reg_why[0])
		return;
	va_start(ap, fmt);
	vsnprintf(reg_why, sizeof(reg_why), fmt, ap);
	va_end(ap);
}

static int reg_check_props(fy_generic def, const char *path)
{
	fy_generic props, p, kind, rest_p;
	const char *name, *k;
	long long pos, npos, rest_pos, array_pos;
	bool seen[FYAI_CMD_MAX_PROPS];

	props = fy_get(fy_get(def, "arguments", fy_invalid), "properties",
		       fy_invalid);
	memset(seen, 0, sizeof(seen));
	npos = 0;
	rest_pos = -1;
	array_pos = -1;
	fy_foreach_key_value(name, p, props) {
		kind = fy_get(p, "x-fyai-complete", fy_invalid);
		k = fy_castp(&kind, "");
		if (*k && !fyai_cmd_kind_known(k)) {
			reg_fail("%s: %s: unknown completion kind '%s'", path,
				 name, k);
			return -1;
		}
		pos = fy_get(p, "x-fyai-positional", -1LL);
		if (pos < 0)
			continue;
		if (pos >= FYAI_CMD_MAX_PROPS || seen[pos]) {
			reg_fail("%s: %s: positional %lld is not unique", path,
				 name, pos);
			return -1;
		}
		seen[pos] = true;
		npos++;
		rest_p = fy_get(p, "x-fyai-rest", fy_invalid);
		if (fy_equal(rest_p, true))
			rest_pos = pos;
		if (fy_equal(fy_get(p, "type", fy_invalid), "array"))
			array_pos = pos;
	}
	for (pos = 0; pos < npos; pos++)
		if (!seen[pos]) {
			reg_fail("%s: positional %lld is missing", path, pos);
			return -1;
		}
	if ((rest_pos >= 0 && rest_pos != npos - 1) ||
	    (array_pos >= 0 && array_pos != npos - 1)) {
		reg_fail("%s: a rest or array positional must be the last",
			 path);
		return -1;
	}
	return 0;
}

static int reg_check_command(fy_generic def, const char *parent)
{
	fy_generic subs, sub, handler, name_v, dflt;
	const char *name, *h, *path;
	bool group;

	name_v = fy_get(def, "command", fy_invalid);
	name = fy_castp(&name_v, "");
	path = parent ? fy_sprintfa("%s %s", parent, name) : name;
	subs = fy_get(def, "commands", fy_invalid);
	handler = fy_get(def, "handler", fy_invalid);
	h = fy_castp(&handler, "");
	group = fy_is_valid(subs);
	if (group == !!*h) {
		reg_fail("%s: a command has either a handler or commands",
			 path);
		return -1;
	}
	if (*h && !fyai_cmd_handler(h)) {
		reg_fail("%s: unknown handler '%s'", path, h);
		return -1;
	}
	handler = fy_get(def, "prepare", fy_invalid);
	if (fy_is_string(handler) &&
	    !fyai_cmd_prepare(fy_castp(&handler, ""))) {
		reg_fail("%s: unknown prepare hook '%s'", path,
			 fy_castp(&handler, ""));
		return -1;
	}
	handler = fy_get(def, "early", fy_invalid);
	if (fy_is_string(handler) &&
	    !fyai_cmd_early_hook(fy_castp(&handler, ""))) {
		reg_fail("%s: unknown early hook '%s'", path,
			 fy_castp(&handler, ""));
		return -1;
	}
	if (reg_check_props(def, path))
		return -1;
	fy_foreach(sub, subs)
		if (reg_check_command(sub, path))
			return -1;
	dflt = fy_get(def, "default", fy_invalid);
	if (fy_is_valid(dflt) && !group) {
		reg_fail("%s: only a group has a default", path);
		return -1;
	}
	return 0;
}

static fy_generic reg_parse(const unsigned char *data, size_t len)
{
	fy_generic_sized_string text;

	text.data = (const char *)data;
	text.size = len;
	return fy_parse(reg_gb, text,
			FYAI_YAML_PARSE_FLAGS | FYOPPF_INPUT_TYPE_STRING, NULL);
}

fy_generic fyai_cmd_registry(void)
{
	struct fy_generic_builder_cfg cfg = {
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED,
	};
	fy_generic doc, schema, report, def;
	char *problems;

	if (reg_tried)
		return reg_doc;
	reg_tried = true;
	reg_gb = fy_generic_builder_create(&cfg);
	if (!reg_gb) {
		reg_fail("cannot create the command registry builder");
		return fy_invalid;
	}
	doc = reg_parse(FYAI_EMBEDDED_COMMANDS, FYAI_EMBEDDED_COMMANDS_LEN);
	schema = reg_parse(FYAI_EMBEDDED_COMMAND_SCHEMA,
			   FYAI_EMBEDDED_COMMAND_SCHEMA_LEN);
	if (!fy_is_mapping(doc) || !fy_is_mapping(schema)) {
		reg_fail("the command definitions do not parse");
		return fy_invalid;
	}
	report = fyai_schema_validate(reg_gb, schema, doc);
	if (!fyai_schema_valid(report)) {
		problems = fyai_schema_report_string(report);
		reg_fail("the command definitions do not match their schema: %s",
			 problems ? problems : "no problem was reported");
		free(problems);
		return fy_invalid;
	}
	if (reg_check_props(fy_get(doc, "global", fy_invalid), "global"))
		return fy_invalid;
	fy_foreach(def, fy_get(doc, "commands", fy_invalid))
		if (reg_check_command(def, NULL))
			return fy_invalid;
	reg_doc = doc;
	return reg_doc;
}

const char *fyai_cmd_registry_why(void)
{
	return reg_why;
}

/* ---- definitions ---------------------------------------------------------- */

const char *fyai_cmd_surface_name(enum fyai_cmd_surface surface)
{
	return surface == FYAI_CMD_SESSION ? "session" : "cli";
}

bool fyai_cmd_def_on(fy_generic def, fy_generic inherited,
		     enum fyai_cmd_surface surface)
{
	fy_generic surfaces;

	surfaces = fy_get(def, "surfaces", fy_invalid);
	if (!fy_is_valid(surfaces))
		surfaces = inherited;
	if (!fy_is_valid(surfaces))
		return true;
	return fyai_cmd_seq_has(surfaces, fyai_cmd_surface_name(surface));
}

bool fyai_cmd_seq_has(fy_generic seq, const char *word)
{
	fy_generic item;

	fy_foreach(item, seq)
		if (fy_equal(item, word))
			return true;
	return false;
}

fy_generic fyai_cmd_def_surfaces(fy_generic def, fy_generic inherited)
{
	fy_generic surfaces;

	surfaces = fy_get(def, "surfaces", fy_invalid);
	return fy_is_valid(surfaces) ? surfaces : inherited;
}

bool fyai_cmd_def_names(fy_generic def, const char *word)
{
	fy_generic name, alias;

	name = fy_get(def, "command", fy_invalid);
	if (fy_equal(name, word))
		return true;
	fy_foreach(alias, fy_get(def, "aliases", fy_invalid))
		if (fy_equal(alias, word))
			return true;
	return false;
}

static bool def_name_has_prefix(fy_generic def, const char *word, size_t len)
{
	fy_generic name;
	const char *s;

	name = fy_get(def, "command", fy_invalid);
	s = fy_castp(&name, "");
	return !strncmp(s, word, len);
}

/*
 * The command of the list @defs that @word names on @surface. The session
 * also takes a unique prefix of a name. *@hitsp receives the number of
 * prefix matches when there is no exact match.
 */
fy_generic fyai_cmd_def_find(fy_generic defs, fy_generic inherited,
			     const char *word, size_t len,
			     enum fyai_cmd_surface surface, size_t *hitsp)
{
	fy_generic def, found;
	char *exact;
	size_t hits;

	if (hitsp)
		*hitsp = 0;
	exact = alloca(len + 1);
	memcpy(exact, word, len);
	exact[len] = '\0';
	fy_foreach(def, defs) {
		if (!fyai_cmd_def_on(def, inherited, surface))
			continue;
		if (fyai_cmd_def_names(def, exact))
			return def;
	}
	if (surface != FYAI_CMD_SESSION || !len)
		return fy_invalid;
	hits = 0;
	found = fy_invalid;
	fy_foreach(def, defs) {
		if (!fyai_cmd_def_on(def, inherited, surface) ||
		    fy_get(def, "hidden", false))
			continue;
		if (def_name_has_prefix(def, word, len)) {
			found = def;
			hits++;
		}
	}
	if (hitsp)
		*hitsp = hits;
	return hits == 1 ? found : fy_invalid;
}

/* The subcommand name that @key ("default" or "fallback") selects. */
static fy_generic def_default(fy_generic group, const char *key,
			      enum fyai_cmd_surface surface)
{
	fy_generic d;

	d = fy_get(group, key, fy_invalid);
	if (fy_is_mapping(d))
		d = fy_get(d, fyai_cmd_surface_name(surface), fy_invalid);
	return fy_is_string(d) ? d : fy_invalid;
}

static fy_generic def_sub_named(fy_generic group, fy_generic inherited,
				fy_generic name, enum fyai_cmd_surface surface)
{
	const char *s;

	s = fy_castp(&name, "");
	return fyai_cmd_def_find(fy_get(group, "commands", fy_invalid),
				 inherited, s, strlen(s), surface, NULL);
}

static bool word_is_help(const char *w)
{
	return !strcmp(w, "--help") || !strcmp(w, "-h");
}

int fyai_cmd_walk(enum fyai_cmd_surface surface, size_t nwords,
		  const char *const *words, size_t limit,
		  struct fyai_cmd_walk *w)
{
	struct fyai_cmd_prop props[FYAI_CMD_MAX_PROPS], *cp;
	fy_generic reg, def, sub, dflt, name_v;
	const char *name;
	long long gpos;
	size_t i, len;
	int n, np, k;

	memset(w, 0, sizeof(*w));
	w->def = fy_invalid;
	w->surfaces = fy_invalid;
	w->group = fy_invalid;
	w->group_surfaces = fy_invalid;
	w->last_group = fy_invalid;
	reg = fyai_cmd_registry();
	if (!nwords || !fy_is_valid(reg))
		return -1;
	def = fyai_cmd_def_find(fy_get(reg, "commands", fy_invalid),
				fy_invalid, words[0], strlen(words[0]),
				surface, NULL);
	if (!fy_is_valid(def))
		return -1;
	w->surfaces = fyai_cmd_def_surfaces(def, fy_invalid);
	name_v = fy_get(def, "command", fy_invalid);
	name = fy_castp(&name_v, "");
	n = snprintf(w->path, sizeof(w->path), "%s", name);
	gpos = 0;
	for (i = 1; fy_is_valid(fy_get(def, "commands", fy_invalid)); ) {
		if (!fy_equal(w->last_group, def) &&
		    w->ngroups < ARRAY_SIZE(w->groups))
			w->groups[w->ngroups++] = def;
		w->last_group = def;
		w->last_group_pos = gpos;
		if (i < limit && !word_is_help(words[i])) {
			len = strlen(words[i]);
			sub = fyai_cmd_def_find(fy_get(def, "commands",
						       fy_invalid),
						w->surfaces, words[i], len,
						surface, NULL);
			cp = NULL;
			if (!fy_is_valid(sub) && words[i][0] != '-' &&
			    w->ngargs < ARRAY_SIZE(w->gargs)) {
				np = fyai_cmd_props(def, surface, props,
						    ARRAY_SIZE(props));
				for (k = 0; k < np; k++)
					if (props[k].pos == gpos)
						cp = &props[k];
			}
			if (cp) {
				w->gargs[w->ngargs].group = def;
				w->gargs[w->ngargs].name = cp->name;
				w->gargs[w->ngargs].word = words[i];
				w->ngargs++;
				gpos++;
				i++;
				continue;
			}
			if (fy_is_valid(sub)) {
				gpos = 0;
				def = sub;
				w->surfaces = fyai_cmd_def_surfaces(def,
							w->surfaces);
				name_v = fy_get(def, "command", fy_invalid);
				name = fy_castp(&name_v, "");
				n += snprintf(w->path + n, sizeof(w->path) - n,
					      " %s", name);
				i++;
				continue;
			}
		}
		/* No subcommand word: the default, else the fallback. */
		if (i < limit && word_is_help(words[i]))
			break;
		dflt = i >= limit || words[i][0] == '-' ?
		       def_default(def, "default", surface) :
		       def_default(def, "fallback", surface);
		if (!fy_is_valid(dflt))
			break;
		sub = def_sub_named(def, w->surfaces, dflt, surface);
		if (!fy_is_valid(sub))
			break;
		w->group = def;
		w->group_surfaces = w->surfaces;
		gpos = 0;
		def = sub;
		w->surfaces = fyai_cmd_def_surfaces(def, w->surfaces);
		name_v = fy_get(def, "command", fy_invalid);
		name = fy_castp(&name_v, "");
		n += snprintf(w->path + n, sizeof(w->path) - n, " %s", name);
	}
	w->def = def;
	w->consumed = i;
	return 0;
}

/* ---- properties ----------------------------------------------------------- */

int fyai_cmd_props(fy_generic def, enum fyai_cmd_surface surface,
		   struct fyai_cmd_prop *props, size_t max)
{
	fy_generic map, p, v;
	const char *name;
	struct fyai_cmd_prop *cp;
	size_t n;

	map = fy_get(fy_get(def, "arguments", fy_invalid), "properties",
		     fy_invalid);
	n = 0;
	fy_foreach_key_value(name, p, map) {
		v = fy_get(p, "x-fyai-surfaces", fy_invalid);
		if (fy_is_valid(v) &&
		    !fyai_cmd_seq_has(v, fyai_cmd_surface_name(surface)))
			continue;
		if (n >= max)
			return -1;
		cp = &props[n++];
		memset(cp, 0, sizeof(*cp));
		cp->name = name;
		cp->p = p;
		cp->pos = fy_get(p, "x-fyai-positional", -1LL);
		cp->rest = fy_get(p, "x-fyai-rest", false);
		cp->repeat = fy_get(p, "x-fyai-repeat", false);
		cp->negate = fy_get(p, "x-fyai-negate", false);
		cp->hidden = fy_get(p, "x-fyai-hidden", false);
		v = fy_get(p, "type", fy_invalid);
		cp->boolean = fy_equal(v, "boolean");
		cp->array = fy_equal(v, "array");
		v = fy_get(p, "x-fyai-long", fy_invalid);
		if (fy_is_string(v))
			snprintf(cp->lname, sizeof(cp->lname), "%s",
				 fy_castp(&v, ""));
		else
			fyai_cmd_long_name(name, cp->lname, sizeof(cp->lname));
		v = fy_get(p, "x-fyai-short", fy_invalid);
		cp->sname = *fy_castp(&v, "");
		v = fy_get(p, "x-fyai-meta", fy_invalid);
		snprintf(cp->meta, sizeof(cp->meta), "%s",
			 fy_is_string(v) ? fy_castp(&v, "") : "VALUE");
	}
	return (int)n;
}

void fyai_cmd_long_name(const char *name, char *buf, size_t size)
{
	size_t i;

	for (i = 0; name[i] && i + 1 < size; i++)
		buf[i] = name[i] == '_' ? '-' : name[i];
	buf[i] = '\0';
}

static struct fyai_cmd_prop *prop_long(struct fyai_cmd_prop *props, int n,
				       const char *name, size_t len,
				       bool *negated)
{
	int i;

	*negated = false;
	for (i = 0; i < n; i++) {
		if (props[i].pos >= 0)
			continue;
		if (strlen(props[i].lname) == len &&
		    !strncmp(props[i].lname, name, len))
			return &props[i];
	}
	if (len > 3 && !strncmp(name, "no-", 3))
		for (i = 0; i < n; i++)
			if (props[i].pos < 0 && props[i].negate &&
			    strlen(props[i].lname) == len - 3 &&
			    !strncmp(props[i].lname, name + 3, len - 3)) {
				*negated = true;
				return &props[i];
			}
	return NULL;
}

static struct fyai_cmd_prop *prop_short(struct fyai_cmd_prop *props, int n,
					char c)
{
	int i;

	for (i = 0; i < n; i++)
		if (props[i].pos < 0 && props[i].sname == c)
			return &props[i];
	return NULL;
}

struct fyai_cmd_prop *fyai_cmd_prop_positional(struct fyai_cmd_prop *props,
					       int n, long long pos)
{
	int i;

	for (i = 0; i < n; i++)
		if (props[i].pos == pos)
			return &props[i];
	/* A variadic last positional takes every later word. */
	for (i = 0; i < n; i++)
		if (props[i].pos >= 0 && props[i].pos < pos &&
		    (props[i].array || props[i].rest))
			return &props[i];
	return NULL;
}

static const char *const bool_words[] = {
	"true", "false", "on", "off", "yes", "no", "1", "0", NULL,
};

/* The value of @text for the item type of @cp. */
static int prop_value(struct fy_generic_builder *gb, struct fyai_cmd_prop *cp,
		      const char *text, fy_generic *out)
{
	fy_generic type;
	long long ll;
	double d;
	char *end;

	type = cp->array ? fy_get(fy_get(cp->p, "items", fy_invalid), "type",
				  fy_invalid) :
			   fy_get(cp->p, "type", fy_invalid);
	if (fy_equal(type, "integer")) {
		errno = 0;
		ll = strtoll(text, &end, 0);
		if (errno || !*text || *end)
			return -1;
		*out = fy_value(gb, ll);
	} else if (fy_equal(type, "number")) {
		errno = 0;
		d = strtod(text, &end);
		if (errno || !*text || *end)
			return -1;
		*out = fy_value(gb, d);
	} else if (fy_equal(type, "boolean")) {
		ll = str_in_set(text, bool_words);
		if (ll < 0)
			return -1;
		*out = ll & 1 ? fy_false : fy_true;
	} else {
		*out = fy_value(gb, text);
	}
	return fy_is_valid(*out) ? 0 : -1;
}

/* ---- parser ---------------------------------------------------------------- */

char *fyai_cmd_usage_of(struct fyai_cmd_parsed *p, enum fyai_cmd_surface s)
{
	return fyai_cmd_usage(p->def, p->path, s);
}

/* Report @fmt for the command at @path, with its usage, in one diagnostic. */
static void parse_error(struct fyai_cfg *cfg, fy_generic def, const char *path,
			enum fyai_cmd_surface surface, const char *fmt, ...)
	__attribute__((format(printf, 5, 6)));

static void parse_error(struct fyai_cfg *cfg, fy_generic def, const char *path,
			enum fyai_cmd_surface surface, const char *fmt, ...)
{
	char msg[512];
	char *usage;
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	usage = fy_is_valid(def) ? fyai_cmd_usage(def, path, surface) : NULL;
	if (usage)
		fyai_cfg_error(cfg, "%s: %s; usage: %s", path, msg, usage);
	else
		fyai_cfg_error(cfg, "%s: %s", path, msg);
	free(usage);
}

static int parse_set(struct fyai_cmd_parse_state *st, struct fyai_cmd_prop *cp,
		     fy_generic value)
{
	fy_generic cur;

	cur = fy_get(st->args, cp->name, fy_invalid);
	if (cp->array) {
		if (!fy_is_valid(cur))
			cur = fy_seq_empty;
		value = fy_append(st->gb, cur, value);
	} else if (fy_is_valid(cur)) {
		return -1;
	}
	st->args = fy_assoc(st->gb, st->args, cp->name, value);
	return fy_is_valid(st->args) ? 0 : -1;
}

static int parse_format(const char *s, enum fyai_cmd_format *fmtp)
{
	if (!strcmp(s, "markdown"))
		*fmtp = FYAI_CMD_OUT_MARKDOWN;
	else if (!strcmp(s, "json"))
		*fmtp = FYAI_CMD_OUT_JSON;
	else if (!strcmp(s, "yaml"))
		*fmtp = FYAI_CMD_OUT_YAML;
	else
		return -1;
	return 0;
}

/* The text of the rest of a line that starts at word @i. */
static const char *parse_rest(struct fyai_cmd_parse_state *st, size_t i)
{
	struct response_buffer buf = { 0 };
	const char *text;
	size_t len;

	if (st->line) {
		text = st->line + st->offs[i];
		len = strlen(text);
		while (len && isspace((unsigned char)text[len - 1]))
			len--;
		return fy_gb_intern_string(st->gb,
					   fy_sprintfa("%.*s", (int)len, text));
	}
	for (; i < st->nwords; i++) {
		if (buf.len && response_buffer_append(&buf, " "))
			break;
		if (response_buffer_append(&buf, st->words[i]))
			break;
	}
	text = fy_gb_intern_string(st->gb, buf.data ? buf.data : "");
	free(buf.data);
	return text;
}

static int parse_args(struct fyai_cmd_parse_state *st, size_t i)
{
	struct fyai_cmd_prop props[FYAI_CMD_MAX_PROPS], *cp;
	fy_generic value, report, dflt, argschema, req;
	const char *w, *eq, *text;
	char *problems;
	bool opts_done, negated;
	long long npos;
	size_t len, j;
	int n, k;

	n = fyai_cmd_props(st->out->def, st->surface, props,
			   ARRAY_SIZE(props));
	if (n < 0) {
		parse_error(st->cfg, st->out->def, st->out->path, st->surface,
			    "too many arguments are defined");
		return -1;
	}
	opts_done = false;
	npos = 0;
	for (; i < st->nwords; i++) {
		w = st->words[i];
		if (!opts_done && !strcmp(w, "--")) {
			opts_done = true;
			continue;
		}
		if (!opts_done && word_is_help(w)) {
			st->out->help = true;
			continue;
		}
		if (!opts_done && w[0] == '-' && w[1] == '-') {
			eq = strchr(w + 2, '=');
			len = eq ? (size_t)(eq - (w + 2)) : strlen(w + 2);
			if (st->surface == FYAI_CMD_CLI && len == 6 &&
			    !strncmp(w + 2, "output", 6)) {
				text = eq ? eq + 1 :
				       i + 1 < st->nwords ? st->words[++i] : NULL;
				if (!text || parse_format(text,
							  &st->out->format)) {
					parse_error(st->cfg, st->out->def,
						    st->out->path, st->surface,
						    "--output takes markdown, "
						    "json, or yaml");
					return -1;
				}
				continue;
			}
			cp = prop_long(props, n, w + 2, len, &negated);
			if (!cp) {
				parse_error(st->cfg, st->out->def,
					    st->out->path, st->surface,
					    "unknown option '%.*s'",
					    (int)(len + 2), w);
				return -1;
			}
			if (cp->boolean && !eq) {
				value = negated ? fy_false : fy_true;
			} else {
				text = eq ? eq + 1 :
				       i + 1 < st->nwords ? st->words[++i] : NULL;
				if (!text) {
					parse_error(st->cfg, st->out->def,
						    st->out->path, st->surface,
						    "--%s needs a value",
						    cp->lname);
					return -1;
				}
				if (prop_value(st->gb, cp, text, &value)) {
					parse_error(st->cfg, st->out->def,
						    st->out->path, st->surface,
						    "--%s: invalid value '%s'",
						    cp->lname, text);
					return -1;
				}
			}
			if (parse_set(st, cp, value)) {
				parse_error(st->cfg, st->out->def,
					    st->out->path, st->surface,
					    "--%s is given more than once",
					    cp->lname);
				return -1;
			}
			continue;
		}
		if (!opts_done && w[0] == '-' && w[1]) {
			for (j = 1; w[j]; j++) {
				cp = prop_short(props, n, w[j]);
				if (!cp) {
					parse_error(st->cfg, st->out->def,
						    st->out->path, st->surface,
						    "unknown option '-%c'",
						    w[j]);
					return -1;
				}
				if (cp->boolean) {
					value = fy_true;
				} else {
					text = w[j + 1] ? w + j + 1 :
					       i + 1 < st->nwords ?
					       st->words[++i] : NULL;
					if (!text || prop_value(st->gb, cp,
								text,
								&value)) {
						parse_error(st->cfg,
							    st->out->def,
							    st->out->path,
							    st->surface,
							    "-%c needs a value",
							    cp->sname);
						return -1;
					}
				}
				if (parse_set(st, cp, value)) {
					parse_error(st->cfg, st->out->def,
						    st->out->path, st->surface,
						    "-%c is given more than "
						    "once", cp->sname);
					return -1;
				}
				if (!cp->boolean)
					break;
			}
			continue;
		}
		cp = fyai_cmd_prop_positional(props, n, npos);
		if (!cp) {
			parse_error(st->cfg, st->out->def, st->out->path,
				    st->surface, "unexpected argument '%s'", w);
			return -1;
		}
		if (cp->rest) {
			value = fy_value(st->gb, parse_rest(st, i));
			if (parse_set(st, cp, value))
				return -1;
			break;
		}
		if (prop_value(st->gb, cp, w, &value)) {
			parse_error(st->cfg, st->out->def, st->out->path,
				    st->surface, "%s: invalid value '%s'",
				    cp->meta, w);
			return -1;
		}
		if (parse_set(st, cp, value))
			return -1;
		if (!cp->array)
			npos++;
	}
	if (st->out->help)
		return 0;

	for (k = 0; k < n; k++) {
		dflt = fy_get(props[k].p, "default", fy_invalid);
		if (fy_is_valid(dflt) &&
		    !fy_is_valid(fy_get(st->args, props[k].name, fy_invalid)))
			st->args = fy_assoc(st->gb, st->args, props[k].name,
					    dflt);
	}
	/* A missing argument is named as the command line spells it. */
	fy_foreach(req, fy_get(fy_get(st->out->def, "arguments", fy_invalid),
			       "required", fy_invalid)) {
		for (k = 0; k < n; k++)
			if (fy_equal(req, props[k].name))
				break;
		if (k == n || fy_is_valid(fy_get(st->args, props[k].name,
						 fy_invalid)))
			continue;
		if (props[k].pos >= 0)
			parse_error(st->cfg, st->out->def, st->out->path,
				    st->surface, "%s is required",
				    props[k].meta);
		else
			parse_error(st->cfg, st->out->def, st->out->path,
				    st->surface, "--%s is required",
				    props[k].lname);
		return -1;
	}
	argschema = fy_get(st->out->def, "arguments", fy_invalid);
	if (fy_is_valid(argschema)) {
		report = fyai_schema_validate(st->gb, argschema, st->args);
		if (!fyai_schema_valid(report)) {
			problems = fyai_schema_report_string(report);
			parse_error(st->cfg, st->out->def, st->out->path,
				    st->surface, "%s",
				    problems ? problems : "invalid arguments");
			free(problems);
			return -1;
		}
	}
	st->out->args = st->args;
	return 0;
}

/*
 * Merge the arguments of the groups on the path into the arguments of the
 * command, each group checked against its own schema.
 */
static int parse_group_args(struct fyai_cmd_parse_state *st,
			    struct fyai_cmd_walk *w)
{
	struct fyai_cmd_prop props[FYAI_CMD_MAX_PROPS];
	fy_generic group, gargs, value, report, dflt, schema;
	char *problems;
	size_t g, j;
	int n, k;

	for (g = 0; g < w->ngroups; g++) {
		group = w->groups[g];
		schema = fy_get(group, "arguments", fy_invalid);
		if (!fy_is_valid(schema))
			continue;
		n = fyai_cmd_props(group, st->surface, props,
				   ARRAY_SIZE(props));
		gargs = fy_map_empty;
		for (j = 0; j < w->ngargs; j++) {
			if (!fy_equal(w->gargs[j].group, group))
				continue;
			for (k = 0; k < n; k++)
				if (!strcmp(props[k].name, w->gargs[j].name))
					break;
			if (k == n || prop_value(st->gb, &props[k],
						 w->gargs[j].word, &value)) {
				parse_error(st->cfg, st->out->def,
					    st->out->path, st->surface,
					    "invalid value '%s'",
					    w->gargs[j].word);
				return -1;
			}
			gargs = fy_assoc(st->gb, gargs, w->gargs[j].name,
					 value);
		}
		for (k = 0; k < n; k++) {
			dflt = fy_get(props[k].p, "default", fy_invalid);
			if (fy_is_valid(dflt) &&
			    !fy_is_valid(fy_get(gargs, props[k].name,
						fy_invalid)))
				gargs = fy_assoc(st->gb, gargs, props[k].name,
						 dflt);
		}
		report = fyai_schema_validate(st->gb, schema, gargs);
		if (!fyai_schema_valid(report)) {
			problems = fyai_schema_report_string(report);
			parse_error(st->cfg, st->out->def, st->out->path,
				    st->surface, "%s",
				    problems ? problems : "invalid arguments");
			free(problems);
			return -1;
		}
		st->out->args = fy_merge(st->gb, st->out->args, gargs);
		if (!fy_is_valid(st->out->args)) {
			parse_error(st->cfg, st->out->def, st->out->path,
				    st->surface, "cannot store the arguments");
			return -1;
		}
	}
	return 0;
}

int fyai_cmd_parse(struct fyai_cfg *cfg, struct fy_generic_builder *gb,
		   enum fyai_cmd_surface surface, size_t nwords,
		   const char *const *words, const char *line,
		   const size_t *offs, struct fyai_cmd_parsed *out)
{
	struct fyai_cmd_parse_state st;
	struct fyai_cmd_walk w;
	const char *top;
	size_t i;

	memset(out, 0, sizeof(*out));
	out->def = fy_invalid;
	out->args = fy_map_empty;
	top = nwords ? words[0] : "";
	if (!fy_is_valid(fyai_cmd_registry())) {
		fyai_cfg_error(cfg, "%s: %s", top, fyai_cmd_registry_why());
		return -1;
	}
	if (fyai_cmd_walk(surface, nwords, words, nwords, &w)) {
		fyai_cfg_error(cfg, "unknown command '%s'", top);
		return -1;
	}
	out->def = w.def;
	out->path = fy_gb_intern_string(gb, w.path);
	i = w.consumed;
	if (fy_is_valid(fy_get(w.def, "commands", fy_invalid))) {
		/* A group that no word or default resolved. */
		for (; i < nwords; i++)
			if (word_is_help(words[i])) {
				out->help = true;
				return 0;
			}
		if (w.consumed < nwords)
			parse_error(cfg, w.def, out->path, surface,
				    "unknown subcommand '%s'",
				    words[w.consumed]);
		else
			parse_error(cfg, w.def, out->path, surface,
				    "a subcommand is required");
		return -1;
	}
	memset(&st, 0, sizeof(st));
	st.cfg = cfg;
	st.gb = gb;
	st.surface = surface;
	st.nwords = nwords;
	st.words = words;
	/* The raw line holds a rest argument only with the word offsets. */
	st.line = offs ? line : NULL;
	st.offs = offs;
	st.out = out;
	st.args = fy_map_empty;
	if (parse_args(&st, i))
		return -1;
	return out->help ? 0 : parse_group_args(&st, &w);
}

/* ---- word splitting ------------------------------------------------------- */

int fyai_cmd_split(const char *line, bool partial, char ***wordsp,
		   size_t **offsp)
{
	struct response_buffer word = { 0 };
	char **words = NULL, **nw;
	size_t *offs = NULL, *no;
	const char *p;
	char quote;
	bool in_word;
	int n = 0;

	*wordsp = NULL;
	*offsp = NULL;
	p = line;
	for (;;) {
		while (*p && isspace((unsigned char)*p))
			p++;
		if (!*p)
			break;
		nw = realloc(words, (n + 2) * sizeof(*words));
		if (!nw)
			goto err;
		words = nw;
		no = realloc(offs, (n + 2) * sizeof(*offs));
		if (!no)
			goto err;
		offs = no;
		offs[n] = (size_t)(p - line);
		word.len = 0;
		if (response_buffer_append(&word, ""))
			goto err;
		quote = 0;
		in_word = true;
		while (*p && in_word) {
			if (quote == '\'') {
				if (*p == '\'')
					quote = 0;
				else if (response_buffer_append_data(&word, p, 1))
					goto err;
				p++;
			} else if (quote == '"') {
				if (*p == '"') {
					quote = 0;
					p++;
					continue;
				}
				if (*p == '\\' && (p[1] == '"' || p[1] == '\\'))
					p++;
				if (response_buffer_append_data(&word, p, 1))
					goto err;
				p++;
			} else if (*p == '\'' || *p == '"') {
				quote = *p++;
			} else if (isspace((unsigned char)*p)) {
				in_word = false;
			} else {
				if (*p == '\\' && p[1])
					p++;
				if (response_buffer_append_data(&word, p, 1))
					goto err;
				p++;
			}
		}
		words[n] = strndup(word.data ? word.data : "", word.len);
		if (!words[n])
			goto err;
		n++;
		/* An open quote at the end is the word being completed. */
		if (quote && partial) {
			free(word.data);
			*wordsp = words;
			*offsp = offs;
			return n;
		}
	}
	if (partial && (!n || isspace((unsigned char)p[-1]))) {
		nw = realloc(words, (n + 1) * sizeof(*words));
		if (!nw)
			goto err;
		words = nw;
		no = realloc(offs, (n + 1) * sizeof(*offs));
		if (!no)
			goto err;
		offs = no;
		offs[n] = (size_t)(p - line);
		words[n] = strdup("");
		if (!words[n])
			goto err;
		n++;
	}
	free(word.data);
	*wordsp = words;
	*offsp = offs;
	return n;

err:
	free(word.data);
	fyai_cmd_split_free(words, n, offs);
	return -1;
}

void fyai_cmd_split_free(char **words, int n, size_t *offs)
{
	int i;

	for (i = 0; words && i < n; i++)
		free(words[i]);
	free(words);
	free(offs);
}

/* ---- arguments ------------------------------------------------------------ */

const char *fyai_cmd_arg_str(struct fyai_cmd_call *call, const char *name)
{
	fy_generic v;

	v = fy_get(call->args, name, fy_invalid);
	if (!fy_is_string(v))
		return NULL;
	/* The value lives in a builder; intern a short string for a stable
	 * pointer. */
	return fy_gb_intern_string(call->gb, fy_castp(&v, ""));
}

bool fyai_cmd_arg_bool(struct fyai_cmd_call *call, const char *name)
{
	return fy_get(call->args, name, false);
}

/* ---- presentation ---------------------------------------------------------- */

static int present_machine(struct fyai_cmd_call *call, fy_generic value)
{
	enum fy_op_emit_flags flags;
	fy_generic emitted;
	const char *text;
	struct fyai_sink *sink = call->ctx->sink;

	flags = FYOPEF_DISABLE_DIRECTORY | FYOPEF_WIDTH_INF;
	if (call->format == FYAI_CMD_OUT_JSON)
		flags |= FYOPEF_MODE_JSON | FYOPEF_STYLE_COMPACT;
	else
		flags |= FYOPEF_MODE_YAML_1_2 | FYOPEF_STYLE_PRETTY;
	emitted = fy_emit(call->gb, value, flags, NULL);
	if (!fy_is_string(emitted)) {
		fyai_error(call->ctx, "%s: cannot write the result as %s",
			   call->path, call->format == FYAI_CMD_OUT_JSON ?
			   "JSON" : "YAML");
		return -1;
	}
	text = fy_castp(&emitted, "");
	/* Machine output is written as emitted, never rendered. */
	if (call->format == FYAI_CMD_OUT_YAML && call->mode ==
	    FYAI_CMD_MODE_STREAM)
		(void)fyai_sink_write(sink, FYAI_SINK_MACHINE, "---\n", 4);
	(void)fyai_sink_write(sink, FYAI_SINK_MACHINE, text, strlen(text));
	if (!*text || text[strlen(text) - 1] != '\n')
		(void)fyai_sink_write(sink, FYAI_SINK_MACHINE, "\n", 1);
	return 0;
}

/*
 * Expand {key} in @tmpl from the mapping @result into @buf. A line that names
 * a key the result does not set, or sets to false or null, is left out;
 * {?key} is such a test that writes nothing.
 * Returns -1 when @buf cannot grow.
 */
static int present_template(struct response_buffer *buf, const char *tmpl,
			    fy_generic result)
{
	size_t start = buf->len, len;
	const char *p, *end, *s;
	fy_generic v;
	char key[64];
	bool test;

	for (p = tmpl; *p; ) {
		end = *p == '{' ? strchr(p, '}') : NULL;
		len = end ? (size_t)(end - p - 1) : 0;
		if (!end || !len || len >= sizeof(key)) {
			if (response_buffer_append_data(buf, p, 1))
				return -1;
			p++;
			continue;
		}
		memcpy(key, p + 1, len);
		key[len] = '\0';
		test = key[0] == '?';
		v = fy_get(result, key + test, fy_invalid);
		if (!fy_is_valid(v) || fy_is_null(v) || fy_equal(v, false)) {
			buf->len = start;
			if (buf->data)
				buf->data[start] = '\0';
			return 0;
		}
		p = end + 1;
		/* {?key} tests the key and writes nothing. */
		if (test)
			continue;
		s = fy_is_string(v) ? fy_castp(&v, "") : fy_str(v);
		if (response_buffer_append(buf, s ? s : "?"))
			return -1;
	}
	return response_buffer_append(buf, "\n") ? -1 : 0;
}

/*
 * A result that is a YAML document. A verb writes it as it stands, so the
 * output can be read back; a session shows it as a YAML block.
 */
static int present_document(struct fyai_cmd_call *call, fy_generic value,
			    bool flow)
{
	enum fy_op_emit_flags flags;
	fy_generic emitted;
	const char *text;
	char *md;
	int rc;

	/* A scalar is one line either way. */
	if (!fy_is_mapping(value) && !fy_is_sequence(value))
		flow = true;
	flags = FYOPEF_DISABLE_DIRECTORY | FYOPEF_MODE_YAML_1_2 |
		FYOPEF_WIDTH_INF;
	flags |= flow ? FYOPEF_STYLE_ONELINE : FYOPEF_STYLE_PRETTY;
	emitted = fy_emit(call->gb, value, flags, NULL);
	fyai_error_check(call->ctx, fy_is_string(emitted), err,
			 "%s: cannot write the result as YAML", call->path);
	text = fy_castp(&emitted, "");
	if (call->surface == FYAI_CMD_CLI) {
		(void)fyai_sink_write(call->ctx->sink, FYAI_SINK_MACHINE, text,
				      strlen(text));
		if (!*text || text[strlen(text) - 1] != '\n')
			(void)fyai_sink_write(call->ctx->sink,
					      FYAI_SINK_MACHINE, "\n", 1);
		return 0;
	}
	md = NULL;
	rc = asprintf(&md, "```yaml\n%s%s```\n", text,
		      *text && text[strlen(text) - 1] == '\n' ? "" : "\n");
	fyai_error_check(call->ctx, rc >= 0, err,
			 "%s: cannot format the result", call->path);
	rc = fyai_result_md(call->ctx, md) < 0 ? -1 : 0;
	free(md);
	return rc;
err:
	return -1;
}

static int present_result(struct fyai_cmd_call *call, fy_generic result)
{
	struct fyai_ctx *ctx = call->ctx;
	struct response_buffer text = { 0 };
	fy_generic render, table, message, line, document;
	const char *s;
	int rc;

	if (!fy_is_valid(result) || fy_is_null(result))
		return 0;
	if (call->format != FYAI_CMD_OUT_MARKDOWN)
		return present_machine(call, result);

	render = fy_get(call->def, "render", fy_invalid);
	message = fy_get(render, "message", fy_invalid);
	if (fy_is_valid(message) && fy_is_mapping(result)) {
		if (fy_is_string(message)) {
			rc = present_template(&text, fy_castp(&message, ""),
					      result);
		} else {
			rc = 0;
			fy_foreach(line, message)
				if (!rc)
					rc = present_template(&text,
						fy_castp(&line, ""), result);
		}
		if (!rc && text.len)
			rc = fyai_result(ctx, "%s", text.data) < 0 ? -1 : 0;
		free(text.data);
		fyai_error_check(ctx, !rc, err, "%s: cannot present the result",
				 call->path);
		return 0;
	}
	document = fy_get(render, "document", fy_invalid);
	if (fy_is_valid(document))
		return present_document(call, result, fy_equal(document,
							       "flow"));
	if (fy_is_string(result)) {
		/* A string result is Markdown that the command wrote; with
		 * Markdown off it is written as its source. */
		s = fy_castp(&result, "");
		if (!ctx->cfg->markdown)
			return fyai_result(ctx, "%s%s", s,
					   *s && s[strlen(s) - 1] != '\n' ?
					   "\n" : "") < 0 ? -1 : 0;
		return fyai_result_md(ctx, s) < 0 ? -1 : 0;
	}
	table = fy_is_valid(call->renderopts) ? call->renderopts :
		fy_get(render, "table", fy_map_empty);
	if (fy_is_mapping(result) || fy_is_sequence(result))
		return fyai_generic_to_markdown(ctx, table, result);
	return fyai_result(ctx, "%s\n", fy_str(result) ? : "") < 0 ? -1 : 0;
err:
	return -1;
}

int fyai_cmd_emit(struct fyai_cmd_call *call, fy_generic item)
{
	fy_generic stream, how;
	const char *s;

	if (call->stopped)
		return -1;
	if (call->format != FYAI_CMD_OUT_MARKDOWN) {
		if (!present_machine(call, item))
			return 0;
		call->stopped = true;
		return -1;
	}
	stream = fy_get(call->def, "stream", fy_invalid);
	how = fy_get(stream, "render", fy_invalid);
	if (fy_equal(how, "rows")) {
		/* A table sizes its columns from every row: collect them. */
		call->rows = fy_append(call->gb, call->rows, item);
		return fy_is_valid(call->rows) ? 0 : -1;
	}
	s = fy_is_string(item) ? fy_castp(&item, "") : fy_str(item);
	if (fy_equal(how, "markdown"))
		return fyai_result_md(call->ctx, s ? s : "") < 0 ? -1 : 0;
	return fyai_result(call->ctx, "%s\n", s ? s : "") < 0 ? -1 : 0;
}

void fyai_cmd_done(struct fyai_cmd_call *call, int rc, fy_generic result)
{
	call->done = true;
	call->rc = rc;
	call->result = result;
	call->cancel = NULL;
	if (call->finish)
		call->finish(call);
}

static enum fyai_cmd_mode def_mode(fy_generic def)
{
	fy_generic m;

	m = fy_get(def, "mode", fy_invalid);
	if (fy_equal(m, "stream"))
		return FYAI_CMD_MODE_STREAM;
	if (fy_equal(m, "async"))
		return FYAI_CMD_MODE_ASYNC;
	return FYAI_CMD_MODE_RESULT;
}

/* Present what a finished call left: its stream rows, then its result. */
static int call_present(struct fyai_cmd_call *call)
{
	fy_generic table;

	if (call->rc)
		return -1;
	if (call->mode == FYAI_CMD_MODE_STREAM &&
	    call->format == FYAI_CMD_OUT_MARKDOWN &&
	    fy_len(call->rows)) {
		table = fy_get(fy_get(call->def, "render", fy_invalid), "table",
			       fy_map_empty);
		if (fyai_generic_to_markdown(call->ctx, table, call->rows))
			return -1;
	}
	return present_result(call, call->result);
}

int fyai_cmd_call_init(struct fyai_cmd_call *call, struct fyai_ctx *ctx,
		       struct fyai_cmd_parsed *parsed,
		       enum fyai_cmd_surface surface)
{
	struct fy_generic_builder_cfg cfg = {
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED,
	};

	memset(call, 0, sizeof(*call));
	call->gb = fy_generic_builder_create(&cfg);
	fyai_error_check(ctx, call->gb, err, "%s: cannot create the result "
			 "builder", parsed->path);
	call->ctx = ctx;
	call->def = parsed->def;
	call->path = fy_gb_intern_string(call->gb, parsed->path);
	call->surface = surface;
	call->format = parsed->format;
	call->mode = def_mode(parsed->def);
	/* The arguments move to the builder of the call, which outlives the
	 * parse. */
	call->args = fy_gb_internalize(call->gb, parsed->args);
	call->rows = fy_seq_empty;
	call->result = fy_invalid;
	call->renderopts = fy_invalid;
	fyai_error_check(ctx, call->path && fy_is_valid(call->args), err,
			 "%s: cannot store the arguments", parsed->path);
	return 0;
err:
	if (call->gb)
		fy_generic_builder_destroy(call->gb);
	call->gb = NULL;
	return -1;
}

void fyai_cmd_call_release(struct fyai_cmd_call *call)
{
	if (call->cleanup)
		call->cleanup(call);
	call->cleanup = NULL;
	if (call->gb)
		fy_generic_builder_destroy(call->gb);
	call->gb = NULL;
}

/* Run @call. Returns 0 or FYAI_CMD_PENDING, or -1 with the cause raised. */
int fyai_cmd_call_run(struct fyai_cmd_call *call)
{
	fy_generic handler, result;
	fyai_cmd_fn fn;
	int rc;

	handler = fy_get(call->def, "handler", fy_invalid);
	fn = fyai_cmd_handler(fy_castp(&handler, ""));
	fyai_error_check(call->ctx, fn, err, "%s: no handler", call->path);
	result = fy_invalid;
	rc = fn(call, &result);
	if (rc == FYAI_CMD_PENDING) {
		fyai_error_check(call->ctx, call->mode == FYAI_CMD_MODE_ASYNC,
				 err, "%s: the command is not async",
				 call->path);
		return FYAI_CMD_PENDING;
	}
	call->done = true;
	call->rc = rc ? -1 : 0;
	call->result = result;
	return call->rc;
err:
	call->done = true;
	call->rc = -1;
	return -1;
}

/* ---- the CLI surface ------------------------------------------------------ */

bool fyai_cmd_state_is(const struct fyai_cmd_state *st, const char *path)
{
	return st && st->path && !strcmp(st->path, path);
}

bool fyai_cmd_is_verb(const char *word)
{
	fy_generic reg, def;

	reg = fyai_cmd_registry();
	if (!word || !fy_is_valid(reg))
		return false;
	def = fyai_cmd_def_find(fy_get(reg, "commands", fy_invalid),
				fy_invalid, word, strlen(word), FYAI_CMD_CLI,
				NULL);
	return fy_is_valid(def);
}

/*
 * The verb flags of a definition. A verb talks to no model unless it says
 * `model`: only such a verb sets up the request state, and needs a provider
 * credential.
 */
static enum fyai_verb_flags def_verb_flags(fy_generic def, bool help)
{
	enum fyai_verb_flags flags = FYAIVF_NO_REQUESTS;
	fy_generic f;

	if (help)
		return FYAIVF_NO_STORAGE | FYAIVF_NO_REQUESTS;
	fy_foreach(f, fy_get(def, "flags", fy_invalid)) {
		if (fy_equal(f, "model"))
			flags &= ~FYAIVF_NO_REQUESTS;
		else if (fy_equal(f, "no-storage"))
			flags |= FYAIVF_NO_STORAGE;
		else if (fy_equal(f, "storage-optional"))
			flags |= FYAIVF_STORAGE_OPTIONAL;
		else if (fy_equal(f, "transient"))
			flags |= FYAIVF_NEEDS_TRANSIENT_BUILDER;
		else if (fy_equal(f, "interactive"))
			flags |= FYAIVF_INTERACTIVE;
	}
	return flags;
}

/* Parse the verb into @cfg one time; the stages below share the parse. */
static int cmd_parse_verb(struct fyai_cfg *cfg, int argc, char **argv)
{
	struct fyai_cmd_parsed parsed;

	if (cfg->cmd.reg.path)
		return 0;
	if (fyai_cmd_parse(cfg, cfg->gb, FYAI_CMD_CLI, (size_t)argc,
			   (const char *const *)argv, NULL, NULL, &parsed))
		return -1;
	cfg->cmd.reg.def = parsed.def;
	cfg->cmd.reg.path = parsed.path;
	cfg->cmd.reg.args = parsed.args;
	cfg->cmd.reg.format = parsed.format;
	cfg->cmd.reg.help = parsed.help;
	return 0;
}

/* Run the hook that @key names in the definition of the parsed verb. */
static int cmd_run_hook(struct fyai_cfg *cfg, const char *key,
			fyai_cmd_prepare_fn (*lookup)(const char *name))
{
	fyai_cmd_prepare_fn fn;
	fy_generic hook;

	hook = fy_get(cfg->cmd.reg.def, key, fy_invalid);
	if (cfg->cmd.reg.help || !fy_is_string(hook))
		return 0;
	fn = lookup(fy_castp(&hook, ""));
	return fn ? fn(cfg, cfg->cmd.reg.args) : -1;
}

int fyai_cmd_early(struct fyai_cfg *cfg, int argc, char **argv)
{
	if (cmd_parse_verb(cfg, argc, argv))
		return -1;
	return cmd_run_hook(cfg, "early", fyai_cmd_early_hook);
}

int fyai_cmd_configure(struct fyai_cfg *cfg, int argc, char **argv)
{
	struct fyai_verb *v = &cfg->cmd.reg.verb;

	if (cmd_parse_verb(cfg, argc, argv) ||
	    cmd_run_hook(cfg, "prepare", fyai_cmd_prepare))
		return -1;
	cfg->cmd.run = FYAI_RUN_CMD;
	memset(v, 0, sizeof(*v));
	v->name = fy_gb_intern_string(cfg->gb, argv[0]);
	v->execute = fyai_cmd_execute;
	v->flags = def_verb_flags(cfg->cmd.reg.def, cfg->cmd.reg.help);
	return 0;
}

static int cmd_help_present(struct fyai_ctx *ctx, enum fyai_cmd_surface s,
			    const char *path)
{
	struct response_buffer out = { 0 };
	char **words;
	size_t *offs;
	int n, rc;

	n = fyai_cmd_split(path, false, &words, &offs);
	fyai_error_check(ctx, n >= 0, err, "%s: cannot split the command path",
			 path);
	rc = fyai_cmd_help_source(ctx->cfg, s, (size_t)n,
				  (const char *const *)words, &out);
	fyai_cmd_split_free(words, n, offs);
	if (!rc)
		rc = fyai_result_md(ctx, out.data ? out.data : "") < 0 ? -1 : 0;
	free(out.data);
	return rc;
err:
	return -1;
}

int fyai_cmd_execute(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg = ctx->cfg;
	struct fyai_cmd_parsed parsed;
	struct fyai_cmd_call call;
	struct fyai_event_loop *el;
	int rc, step;

	if (cfg->cmd.reg.help)
		return cmd_help_present(ctx, FYAI_CMD_CLI, cfg->cmd.reg.path);

	memset(&parsed, 0, sizeof(parsed));
	parsed.def = cfg->cmd.reg.def;
	parsed.path = cfg->cmd.reg.path;
	parsed.args = cfg->cmd.reg.args;
	parsed.format = cfg->cmd.reg.format;
	if (fyai_cmd_call_init(&call, ctx, &parsed, FYAI_CMD_CLI))
		return -1;
	rc = fyai_cmd_call_run(&call);
	if (rc == FYAI_CMD_PENDING) {
		/*
		 * A standalone verb owns the top-level loop. An interrupt
		 * cancels the work, which then completes.
		 */
		el = fyai_ctx_loop(ctx);
		fyai_error_check(ctx, el, err_release, "%s: no event loop",
				 call.path);
		step = 0;
		while (!call.done && step >= 0) {
			if (fyai_interrupt_pending(ctx)) {
				fyai_event_interrupt_ack(ctx);
				if (call.cancel)
					call.cancel(&call);
				call.cancel = NULL;
			}
			step = fyai_event_loop_step(el, -1);
		}
		fyai_error_check(ctx, call.done, err_release,
				 "%s: the command did not finish", call.path);
		rc = call.rc;
	}
	if (!rc)
		rc = call_present(&call);
	fyai_cmd_call_release(&call);
	return rc ? -1 : 0;

err_release:
	if (call.cancel)
		call.cancel(&call);
	fyai_cmd_call_release(&call);
	return -1;
}

/* ---- the session surface --------------------------------------------------- */

fy_generic fyai_cmd_session_lookup(const char *line)
{
	fy_generic reg;
	size_t len;

	reg = fyai_cmd_registry();
	if (!line || !fy_is_valid(reg))
		return fy_invalid;
	len = strcspn(line, " \t");
	return fyai_cmd_def_find(fy_get(reg, "commands", fy_invalid),
				 fy_invalid, line, len, FYAI_CMD_SESSION,
				 NULL);
}

bool fyai_cmd_session_exact(const char *name, size_t len)
{
	fy_generic def;
	char *word;

	word = alloca(len + 1);
	memcpy(word, name, len);
	word[len] = '\0';
	fy_foreach(def, fy_get(fyai_cmd_registry(), "commands", fy_invalid))
		if (fyai_cmd_def_on(def, fy_invalid, FYAI_CMD_SESSION) &&
		    fyai_cmd_def_names(def, word))
			return true;
	return false;
}

size_t fyai_cmd_session_prefix(const char *name, size_t len)
{
	size_t hits;

	(void)fyai_cmd_def_find(fy_get(fyai_cmd_registry(), "commands",
				       fy_invalid),
				fy_invalid, name, len, FYAI_CMD_SESSION,
				&hits);
	return hits;
}

/*
 * A read runs beside a turn; a mutation waits behind it. `bare` is a command
 * that reads with no argument and changes state with one.
 */
bool fyai_cmd_session_ends(const char *line)
{
	struct fyai_cmd_walk w;
	char **words;
	size_t *offs;
	bool ends;
	int n;

	n = fyai_cmd_split(line, false, &words, &offs);
	ends = n > 0 && !fyai_cmd_walk(FYAI_CMD_SESSION, (size_t)n,
				       (const char *const *)words, (size_t)n,
				       &w) &&
	       fy_get(w.def, "ends_session", false);
	fyai_cmd_split_free(words, n, offs);
	return ends;
}

bool fyai_cmd_session_immediate(const char *line)
{
	struct fyai_cmd_walk w;
	fy_generic busy;
	char **words;
	size_t *offs;
	bool run;
	int n;

	n = fyai_cmd_split(line, false, &words, &offs);
	if (n <= 0 || fyai_cmd_walk(FYAI_CMD_SESSION, (size_t)n,
				    (const char *const *)words, (size_t)n,
				    &w)) {
		fyai_cmd_split_free(words, n, offs);
		return false;
	}
	busy = fy_get(w.def, "while_busy", fy_invalid);
	run = fy_equal(busy, "run") ||
	      (fy_equal(busy, "bare") && w.consumed == (size_t)n);
	fyai_cmd_split_free(words, n, offs);
	return run;
}

static void session_call_finish(struct fyai_cmd_call *call)
{
	struct fyai_ctx *ctx = call->ctx;

	/* The session presents between turns; wake its loop. */
	fyai_ui_wake(ctx);
}

int fyai_cmd_session_run(struct fyai_ctx *ctx, const char *line, bool *viewp)
{
	struct fyai_cmd_parsed parsed;
	struct fyai_cmd_call *call;
	char **words;
	size_t *offs;
	int n, rc;

	*viewp = false;
	n = fyai_cmd_split(line, false, &words, &offs);
	fyai_error_check(ctx, n > 0, err, "cannot split the command line");
	rc = fyai_cmd_parse(ctx->cfg, ctx->cfg->gb, FYAI_CMD_SESSION,
			    (size_t)n, (const char *const *)words, line, offs,
			    &parsed);
	fyai_cmd_split_free(words, n, offs);
	if (rc)
		return -1;
	if (parsed.help) {
		*viewp = true;
		return cmd_help_present(ctx, FYAI_CMD_SESSION, parsed.path);
	}
	*viewp = fy_get(parsed.def, "view", false);

	fyai_error_check(ctx, !ctx->cmd_call, err,
			 "%s: another command is running", parsed.path);
	call = calloc(1, sizeof(*call));
	fyai_error_check(ctx, call, err, "%s: cannot allocate the call",
			 parsed.path);
	if (fyai_cmd_call_init(call, ctx, &parsed, FYAI_CMD_SESSION)) {
		free(call);
		return -1;
	}
	rc = fyai_cmd_call_run(call);
	if (rc == FYAI_CMD_PENDING) {
		call->finish = session_call_finish;
		ctx->cmd_call = call;
		return 0;
	}
	if (!rc)
		rc = call_present(call);
	fyai_cmd_call_release(call);
	free(call);
	return rc ? -1 : 0;
err:
	return -1;
}

bool fyai_cmd_session_input(struct fyai_ctx *ctx, const char *line)
{
	struct fyai_cmd_call *call = ctx->cmd_call;

	if (!call || call->done || !call->input)
		return false;
	call->input(call, line);
	return true;
}

bool fyai_cmd_session_step(struct fyai_ctx *ctx)
{
	struct fyai_cmd_call *call = ctx->cmd_call;
	bool error;
	int rc;

	if (!call || !call->done)
		return false;
	ctx->cmd_call = NULL;
	fyai_ui_pane_begin(ctx);
	rc = call_present(call);
	error = rc || fyai_diag_got_error(&ctx->cfg->diag);
	fyai_diag_drain(&ctx->cfg->diag);
	fyai_ui_pane_end(ctx, call->path, error,
			 !fy_get(call->def, "view", false));
	fyai_cmd_call_release(call);
	free(call);
	return true;
}

void fyai_cmd_session_interrupt(struct fyai_ctx *ctx)
{
	struct fyai_cmd_call *call = ctx->cmd_call;

	/* The work completes as cancelled; the step presents that. */
	if (!call || call->done || !call->cancel)
		return;
	call->cancel(call);
	call->cancel = NULL;
}

void fyai_cmd_session_cancel(struct fyai_ctx *ctx)
{
	struct fyai_cmd_call *call = ctx->cmd_call;

	if (!call)
		return;
	ctx->cmd_call = NULL;
	call->finish = NULL;
	if (!call->done && call->cancel)
		call->cancel(call);
	fyai_cmd_call_release(call);
	free(call);
}
