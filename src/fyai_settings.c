/*
 * fyai_settings.c - the settings tool: the model reads the configuration
 *
 * SPDX-License-Identifier: MIT
 *
 * The model asks; fyai answers from the merged configuration by the access
 * that the schema states for each key (x-fyai-model, see
 * doc/declarative-state.md). A hidden key is left out of every answer and
 * named under `hidden`. The model never writes state: it presents a change
 * set and an explanation, and fyai checks the set, makes the diff, and
 * applies it at once (write), after the user accepts it in the review panel
 * (review), or keeps it as a proposal that the user approves later.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fyai.h"
#include "fyai_agent.h"
#include "fyai_ask.h"
#include "fyai_branch.h"
#include "fyai_catalog.h"
#include "fyai_config.h"
#include "fyai_diff.h"
#include "fyai_event.h"
#include "fyai_settings.h"
#include "fyai_storage.h"
#include "fyai_tool_registry.h"
#include "fyai_ui.h"
#include "utils.h"

#define FYAI_MODULE FYAIEM_CONFIG

/* The bound on the depth of a walk of the schema or of a value. */
#define SETTINGS_DEPTH_MAX	64
/* The bound on the keys that one list gives. */
#define SETTINGS_LIST_MAX	100
/* The bound on the rows that a select or an export walks. */
#define SETTINGS_WALK_MAX	5000
/* The bounds on the rows that a select gives, and on its conditions. */
#define SETTINGS_SELECT_LIMIT	20
#define SETTINGS_SELECT_MAX	100
#define SETTINGS_WHERE_MAX	8
/* The bounds on one change set, on its explanation, and on each log. */
#define SETTINGS_CHANGES_MAX	32
#define SETTINGS_EXPLANATION_MAX 16384
#define SETTINGS_LOG_MAX	64
/* The bounds on the words of a search and on the keys that it gives. */
#define SETTINGS_SEARCH_WORDS	8
#define SETTINGS_SEARCH_LIMIT	20
#define SETTINGS_SEARCH_MAX	50

struct settings_walk {
	struct fyai_ctx *ctx;
	struct fy_generic_builder *gb;
	fy_generic hidden;	/* the hidden paths met, a sequence */
	fy_generic keys;	/* the rows of a list, a sequence */
	size_t count;
	size_t max;		/* the bound on the keys that a walk gives */
	bool changed;		/* only keys that are not the default */
	bool full;		/* each key with its scope and its default */
};

/*
 * A path names keys of the schema and of user mappings, such as server,
 * persona and key names; a segment `*` stands for any such name, and "~1"
 * and "~0" write a '/' and a '~' in a name. It holds no control byte and no
 * backtick, so the title row can show it as it is in a code span.
 */
static bool settings_path_valid(const char *path)
{
	const char *p;

	for (p = path; *p; p++)
		if ((unsigned char)*p < 0x20 || *p == 0x7f || *p == '`')
			return false;
	/* A '~' starts an escape: "~1" is '/' and "~0" is '~'. */
	for (p = path; (p = strchr(p, '~')); p++)
		if (p[1] != '0' && p[1] != '1')
			return false;
	p = path + strlen(path);
	/* No empty segment, and none that starts with a dot. */
	return *path != '/' && *path != '.' && (!*path || p[-1] != '/') &&
	       !strstr(path, "//") && !strstr(path, "/.");
}

static bool seq_has(fy_generic seq, const char *s)
{
	fy_generic v;

	fy_foreach(v, seq)
		if (fy_equal(v, s))
			return true;
	return false;
}

static const char *settings_join(struct fy_generic_builder *gb,
				 const char *path, const char *key)
{
	return fyai_path_join(gb, path, key);
}

/*
 * The arena is out of reach of the shell when a view holds it as a
 * descriptor, or when the sandbox is on and denies it: the sandbox denies
 * <project>/.fyai, so an arena elsewhere is not known to be denied.
 */
static bool settings_arena_protected(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg = ctx->cfg;
	char arena[PATH_MAX], denied[PATH_MAX];
	char *root, *deny;
	size_t len;
	bool inside;

	if (cfg->view_arena)
		return true;
	if (!cfg->enable_sandbox || !cfg->arena_dir || cfg->view_project)
		return false;
	root = fyai_discover_project_root();
	if (!root)
		return false;
	deny = NULL;
	inside = asprintf(&deny, "%s/.fyai", root) >= 0 && realpath(deny, denied);
	/*
	 * A tool that has the arena covered cannot resolve a path below it: the
	 * cover is the protection, so the path as given is compared instead.
	 */
	if (inside && !realpath(cfg->arena_dir, arena))
		inside = errno == EACCES &&
			 snprintf(arena, sizeof(arena), "%s", cfg->arena_dir) < (int)sizeof(arena);
	len = inside ? strlen(denied) : 0;
	inside = inside && !strncmp(denied, arena, len) &&
		 (arena[len] == '/' || !arena[len]);
	free(deny);
	free(root);
	return inside;
}

/*
 * The default of the schema node @node: its own, else the mapping of the
 * defaults of its members, so a group such as `retry` shows what applies.
 * fy_invalid when nothing below it has a default.
 */
static fy_generic settings_schema_default(struct fy_generic_builder *gb,
					  fy_generic node, int depth)
{
	fy_generic v, out, key, child;

	v = fy_get(node, "default", fy_invalid);
	if (fy_is_valid(v) || depth > SETTINGS_DEPTH_MAX)
		return v;
	out = fy_invalid;
	fy_foreach_key_value(key, child, fy_get(node, "properties",
						fy_invalid)) {
		v = settings_schema_default(gb, child, depth + 1);
		if (!fy_is_valid(v))
			continue;
		out = fy_assoc(gb, fy_is_valid(out) ? out : fy_mapping(gb),
			       key, v);
	}
	return out;
}

/* The value in force at @path, or the default of the schema. */
static fy_generic settings_value(struct settings_walk *w, const char *path,
				 const char **layerp)
{
	struct fyai_cfg *cfg = w->ctx->cfg;
	fy_generic v, stored;

	v = *path ? fy_get_at_pathstr(w->gb, cfg->config_doc, path) :
		cfg->config_doc;
	if (!fy_is_valid(v)) {
		*layerp = "default";
		return settings_schema_default(w->gb,
					       fyai_config_schema_node(path),
					       0);
	}
	if (fy_is_mapping(cfg->config_session) && *path &&
	    fy_is_valid(fy_get_at_pathstr(w->gb, cfg->config_session, path))) {
		*layerp = "session";
		return v;
	}
	stored = !*path ? w->ctx->arena_config :
		 fy_is_mapping(w->ctx->arena_config) ?
		 fy_get_at_pathstr(w->gb, w->ctx->arena_config, path) :
		 fy_invalid;
	*layerp = fy_is_valid(stored) && !fy_compare(stored, v) ? "branch" :
		  "invocation";
	return v;
}

/*
 * @v at @path without the members that the model must not read. Each such
 * member is named in the hidden list of @w.
 */
static fy_generic settings_redact(struct settings_walk *w, const char *path,
				  fy_generic v, int depth)
{
	fy_generic out, key, member;
	const char *sub;

	if (!fy_is_mapping(v) || depth > SETTINGS_DEPTH_MAX)
		return v;
	out = fy_mapping(w->gb);
	fy_foreach_key_value(key, member, v) {
		sub = settings_join(w->gb, path, fy_castp(&key, ""));
		if (fyai_config_model_access(sub) == FYAI_MODEL_HIDDEN) {
			w->hidden = fy_append(w->gb, w->hidden, sub);
			continue;
		}
		out = fy_assoc(w->gb, out, key,
			       settings_redact(w, sub, member, depth + 1));
	}
	return out;
}

/*
 * The first sentence of the description of @node: the schema keeps it short
 * as a hint, so it names a key without the cost of the whole text.
 */
static const char *settings_summary(struct fy_generic_builder *gb,
				    fy_generic node)
{
	const char *desc = fy_get(node, "description", "");
	const char *end;

	end = strstr(desc, ". ");
	return fy_gb_intern_string(gb, fy_sprintfa("%.*s",
			end ? (int)(end - desc + 1) : (int)strlen(desc), desc));
}

/* The members of the schema node of @path, with the access of each. */
static fy_generic settings_children(struct settings_walk *w, const char *path,
				    fy_generic node)
{
	fy_generic rows, key, child;
	const char *sub;
	enum fyai_model_access access;

	rows = fy_sequence(w->gb);
	fy_foreach_key_value(key, child, fy_get(node, "properties",
						fy_invalid)) {
		sub = settings_join(w->gb, path, fy_castp(&key, ""));
		access = fyai_config_model_access(sub);
		if (access == FYAI_MODEL_HIDDEN) {
			w->hidden = fy_append(w->gb, w->hidden, sub);
			continue;
		}
		rows = fy_append(w->gb, rows, fy_mapping(w->gb,
			"key", sub,
			"access", fyai_model_access_name(access),
			"summary", settings_summary(w->gb, child)));
	}
	return rows;
}

struct settings_search {
	const char *words[SETTINGS_SEARCH_WORDS];
	size_t nwords;
	size_t limit;
	fy_generic by_key;	/* keys whose path holds every word */
	fy_generic by_text;	/* keys whose description holds some word */
	size_t found;
};

/*
 * Match each key of the schema under @path: every word must be in its path
 * or in its description. A member of a mapping of user names, such as a
 * server, is under the segment `*`, which stands for any name.
 */
static void settings_search_walk(struct settings_walk *w,
				 struct settings_search *s, const char *path,
				 fy_generic node, int depth)
{
	static const char *const alts[] = { "oneOf", "anyOf" };
	fy_generic key, child, alt, row;
	const char *sub, *desc;
	size_t i, in_key;

	if (!fy_is_mapping(node) || depth > SETTINGS_DEPTH_MAX)
		return;
	fy_foreach_key_value(key, child, fy_get(node, "properties",
						fy_invalid)) {
		sub = settings_join(w->gb, path, fy_castp(&key, ""));
		if (fyai_config_model_access(sub) == FYAI_MODEL_HIDDEN)
			continue;
		desc = fy_get(child, "description", "");
		in_key = 0;
		for (i = 0; i < s->nwords; i++) {
			if (strcasestr(sub, s->words[i]))
				in_key++;
			else if (!strcasestr(desc, s->words[i]))
				break;
		}
		if (i == s->nwords) {
			s->found++;
			row = fy_mapping(w->gb, "key", sub,
				"access", fyai_model_access_name(
					fyai_config_model_access(sub)),
				"summary", settings_summary(w->gb, child));
			if (in_key == s->nwords && fy_len(s->by_key) < s->limit)
				s->by_key = fy_append(w->gb, s->by_key, row);
			else if (in_key < s->nwords &&
				 fy_len(s->by_text) < s->limit)
				s->by_text = fy_append(w->gb, s->by_text, row);
		}
		settings_search_walk(w, s, sub, child, depth + 1);
	}
	settings_search_walk(w, s, settings_join(w->gb, path, "*"),
			     fy_get(node, "additionalProperties", fy_invalid),
			     depth + 1);
	for (i = 0; i < ARRAY_SIZE(alts); i++)
		fy_foreach(alt, fy_get(node, alts[i], fy_invalid))
			settings_search_walk(w, s, path, alt, depth + 1);
}

/* The keys that match @query, those that name it in their path first. */
static fy_generic settings_search(struct settings_walk *w, const char *path,
				  fy_generic node, const char *query,
				  long long limit)
{
	struct settings_search s;
	fy_generic keys, row;
	char *copy, *word, *save;

	memset(&s, 0, sizeof(s));
	s.limit = limit > 0 && limit <= SETTINGS_SEARCH_MAX ? (size_t)limit :
		  SETTINGS_SEARCH_LIMIT;
	s.by_key = fy_sequence(w->gb);
	s.by_text = fy_sequence(w->gb);
	copy = strdup(query);
	if (!copy)
		return fy_value(w->gb, "settings: the query could not be kept");
	for (word = strtok_r(copy, " \t", &save);
	     word && s.nwords < SETTINGS_SEARCH_WORDS;
	     word = strtok_r(NULL, " \t", &save))
		s.words[s.nwords++] = fy_gb_intern_string(w->gb, word);
	free(copy);
	if (!s.nwords)
		return fy_value(w->gb, "settings: search needs a query");
	settings_search_walk(w, &s, path, node, 0);
	keys = s.by_key;
	fy_foreach(row, s.by_text) {
		if (fy_len(keys) >= s.limit)
			break;
		keys = fy_append(w->gb, keys, row);
	}
	return fy_mapping(w->gb, "query", query, "path", path, "keys", keys,
			  "found", (long long)s.found);
}

/* The schema of a node as the model reads it: no members, no extensions. */
static fy_generic settings_schema(struct fy_generic_builder *gb,
				  fy_generic node)
{
	fy_generic out, key, v;
	const char *name;

	out = fy_mapping(gb);
	fy_foreach_key_value(key, v, node) {
		name = fy_castp(&key, "");
		if (!strncmp(name, "x-fyai-", 7) ||
		    fy_any_equal(key, "properties", "additionalProperties"))
			continue;
		out = fy_assoc(gb, out, key, v);
	}
	return out;
}

static fy_generic settings_describe(struct settings_walk *w, const char *path)
{
	fy_generic node, value, result;
	const char *layer;

	node = *path ? fyai_config_schema_node(path) :
	       fyai_config_schema(NULL);
	value = settings_redact(w, path, settings_value(w, path, &layer), 0);
	result = fy_mapping(w->gb,
		"path", path,
		"access", fyai_model_access_name(
				fyai_config_model_access(path)),
		"scope", "stored",
		"layer", layer);
	if (fy_is_valid(value))
		result = fy_assoc(w->gb, result, "value", value);
	if (fy_is_mapping(node)) {
		result = fy_assoc(w->gb, result, "schema",
				  settings_schema(w->gb, node));
		if (fy_is_mapping(fy_get(node, "properties", fy_invalid)))
			result = fy_assoc(w->gb, result, "keys",
					  settings_children(w, path, node));
	} else {
		result = fy_assoc(w->gb, result, "schema", fy_null);
	}
	return result;
}

/* Add a row for each key under @path that has no members in the schema. */
static void settings_list_walk(struct settings_walk *w, const char *path,
			       fy_generic node, int depth)
{
	fy_generic props, key, child, value, dflt, row;
	enum fyai_model_access access;
	const char *sub, *layer;

	if (w->count >= (w->max ? w->max : SETTINGS_LIST_MAX) ||
	    depth > SETTINGS_DEPTH_MAX)
		return;
	props = fy_get(node, "properties", fy_invalid);
	if (*path && !fy_is_mapping(props)) {
		access = fyai_config_model_access(path);
		value = settings_value(w, path, &layer);
		dflt = fy_get(node, "default", fy_invalid);
		if (w->changed && (!strcmp(layer, "default") ||
				   (fy_is_valid(dflt) &&
				    !fy_compare(value, dflt))))
			return;
		value = settings_redact(w, path, value, 0);
		row = fy_mapping(w->gb,
			"key", path,
			"access", fyai_model_access_name(access),
			"layer", layer,
			"value", fy_is_valid(value) ? value : fy_null);
		if (w->full) {
			dflt = settings_redact(w, path,
				settings_schema_default(w->gb, node, 0), 0);
			row = fy_assoc(w->gb, row, "scope", "stored");
			row = fy_assoc(w->gb, row, "default",
				fy_is_valid(dflt) ? dflt : fy_null);
			/* What a layer set that the default does not have. */
			row = fy_assoc(w->gb, row, "changed",
				(_Bool)(fy_is_valid(value) &&
					(!fy_is_valid(dflt) ||
					 fy_compare(value, dflt))));
		}
		w->keys = fy_append(w->gb, w->keys, row);
		w->count++;
		return;
	}
	fy_foreach_key_value(key, child, props) {
		sub = settings_join(w->gb, path, fy_castp(&key, ""));
		if (fyai_config_model_access(sub) == FYAI_MODEL_HIDDEN) {
			w->hidden = fy_append(w->gb, w->hidden, sub);
			continue;
		}
		settings_list_walk(w, sub, child, depth + 1);
	}
}

/* A change set that the model presented, checked, and diffed by fyai. */
struct settings_change {
	fy_generic changes;	/* [{path, value} or {path, delete: true}] */
	fy_generic paths;	/* the paths, in order */
	fy_generic before;	/* {path: value or null} of its layer */
	fy_generic after;
	fy_generic stored;	/* the branch configuration after */
	fy_generic session;	/* the session layer after, or fy_invalid */
	fy_generic catalog_doc;	/* the validated catalogue after */
	enum fyai_model_access access;	/* the strictest of every path */
	bool catalog;		/* a change of the catalogue */
	const char *source;	/* evidence for catalogue values */
	bool trial;		/* the session layer only, for every scope */
	bool legacy_scope;	/* an older change used schema scope */
	const char *diff;	/* Markdown that fyai made, in the builder */
	const char *unified;	/* the unified diff alone */
	/* The entry of the change log that this change reverts or keeps,
	 * and the status the entry takes when the change applies, or NULL. */
	const char *mark_id;
	const char *mark_status;
};

static int settings_catalog_prepare(struct settings_walk *w,
				    fy_generic changes, const char *source,
				    struct settings_change *c,
				    char *why, size_t why_size);

/*
 * @text without control bytes: a line break and a tab stay, every other
 * byte under a blank becomes `?`. The text then sets no colour and moves no
 * cursor wherever it is drawn.
 */
static const char *settings_clean(struct fy_generic_builder *gb,
				  const char *text)
{
	char *copy, *p;
	const char *out;

	copy = strdup(text);
	if (!copy)
		return NULL;
	for (p = copy; *p; p++)
		if (((unsigned char)*p < 0x20 && *p != '\n' && *p != '\t') ||
		    (unsigned char)*p == 0x7f)
			*p = '?';
	out = fy_gb_intern_string(gb, copy);
	free(copy);
	return out;
}

/* Check the arguments of a change and find the strictest access of it. */
static int settings_change_normalize(struct settings_walk *w,
				     fy_generic list, struct settings_change *c,
				     char *why, size_t why_size)
{
	enum fyai_model_access access;
	fy_generic item, value, out, paths;
	const char *path;
	bool del;
	size_t n;

	n = fy_is_sequence(list) ? fy_len(list) : 0;
	if (n < 1 || n > SETTINGS_CHANGES_MAX) {
		snprintf(why, why_size, "a change needs 1 to %d changes",
			 SETTINGS_CHANGES_MAX);
		return -1;
	}
	c->access = FYAI_MODEL_WRITE;
	out = fy_sequence(w->gb);
	paths = fy_sequence(w->gb);
	fy_foreach(item, list) {
		path = fy_get(item, "path", "");
		if (!*path || !settings_path_valid(path) || strchr(path, '*')) {
			snprintf(why, why_size, "'%s' is not a key", path);
			return -1;
		}
		if (!fy_is_mapping(fyai_config_schema_node(path))) {
			snprintf(why, why_size, "%s is not a key of the schema",
				 path);
			return -1;
		}
		del = fy_get(item, "delete", false);
		value = fy_get(item, "value", fy_invalid);
		if (del == fy_is_valid(value)) {
			snprintf(why, why_size, "%s: give a value, or delete: "
				 "true", path);
			return -1;
		}
		access = fyai_config_model_access_tree(path);
		if (access <= FYAI_MODEL_READ) {
			snprintf(why, why_size, "%s is %s: the model cannot "
				 "change it", path,
				 fyai_model_access_name(access));
			return -1;
		}
		if (access < c->access)
			c->access = access;
		if (seq_has(paths, path)) {
			snprintf(why, why_size, "%s is changed twice", path);
			return -1;
		}
		paths = fy_append(w->gb, paths, path);
		out = fy_append(w->gb, out, del ?
			fy_mapping(w->gb, "path", path, "delete", true) :
			fy_mapping(w->gb, "path", path, "value", value));
	}
	c->changes = out;
	c->paths = paths;
	return 0;
}

/*
 * @doc with @path set to @value, or removed for fy_invalid. A removal also
 * removes each parent that it leaves empty, when the schema makes the parent
 * a plain mapping with no default: such an empty mapping means what its
 * absence means. A parent of a union, such as `sandbox`, stays.
 */
static fy_generic settings_doc_set(struct fy_generic_builder *gb,
				   fy_generic doc, const char *path,
				   fy_generic value)
{
	fy_generic node, parent;
	char *up, *slash;

	if (fy_is_valid(value))
		return fy_set_at_pathstr(gb, doc, path,
					 fy_gb_internalize(gb, value));
	if (!fy_is_valid(fy_get_at_pathstr(gb, doc, path)))
		return doc;
	doc = fy_delete_at_pathstr(gb, doc, path);
	up = strdup(path);
	if (!up)
		return fy_invalid;
	while (fy_is_valid(doc) && (slash = strrchr(up, '/'))) {
		*slash = '\0';
		node = fyai_config_schema_node(up);
		parent = fy_get_at_pathstr(gb, doc, up);
		if (!fy_is_mapping(parent) || !fy_empty(parent) ||
		    !fy_equal(fy_get(node, "type", fy_invalid), "object") ||
		    fy_is_valid(fy_get(node, "default", fy_invalid)))
			break;
		doc = fy_delete_at_pathstr(gb, doc, up);
	}
	free(up);
	return doc;
}

static fy_generic settings_doc_get(struct fy_generic_builder *gb,
				   fy_generic doc, const char *path)
{
	fy_generic v;

	v = fy_is_mapping(doc) ? fy_get_at_pathstr(gb, doc, path) : fy_invalid;
	return fy_is_valid(v) ? v : fy_null;
}

/* The emitted YAML of @v, or "{}" for a document that has nothing. */
static const char *settings_emit(struct fy_generic_builder *gb, fy_generic v)
{
	fy_generic text;

	if (!fy_is_mapping(v) || fy_empty(v))
		return "{}\n";
	text = fy_emit(gb, v, FYAI_YAML_EMIT_FLAGS, NULL);
	return fy_is_string(text) ? fy_gb_intern_string(gb,
					fy_castp(&text, "")) : NULL;
}

/* Copy only paths already approved for a change into a display document. */
static fy_generic settings_changed_values(struct settings_walk *w,
					   fy_generic doc, fy_generic changes)
{
	fy_generic out, item, value;
	const char *path;

	out = fy_mapping(w->gb);
	fy_foreach(item, changes) {
		path = fy_get(item, "path", "");
		value = fy_is_mapping(doc) ?
			fy_get_at_pathstr(w->gb, doc, path) : fy_invalid;
		if (fy_is_valid(value))
			out = settings_doc_set(w->gb, out, path,
				settings_redact(w, path, value, 0));
		if (!fy_is_valid(out))
			break;
	}
	return out;
}

/*
 * The fence of a block that holds @text: more backticks than any run in
 * it, so that no text can close the block and write Markdown of its own.
 * NULL for a run too long to outdo.
 */
static const char *settings_fence(struct fy_generic_builder *gb,
				  const char *text)
{
	char fence[64];
	size_t run, longest = 0;
	const char *p;

	for (p = text; *p; p++) {
		for (run = 0; p[run] == '`'; run++)
			;
		if (run > longest)
			longest = run;
		if (run)
			p += run - 1;
	}
	if (longest >= sizeof(fence) - 1)
		return NULL;
	memset(fence, '`', longest < 3 ? 3 : longest + 1);
	fence[longest < 3 ? 3 : longest + 1] = '\0';
	return fy_gb_intern_string(gb, fence);
}

/* Keep file headers so the diff renderer detects the YAML language. */
static const char *settings_diff_block(struct fy_generic_builder *gb,
				       const char *unified)
{
	const char *fence;

	fence = settings_fence(gb, unified);
	if (!fence)
		return NULL;
	return fy_gb_intern_string(gb, fy_sprintfa("%sdiff\n%s%s\n", fence,
					   unified, fence));
}

/*
 * Make the diff of @c from the effective values of changed paths, grouped by
 * the layer written. The fence must contain arbitrary values of those paths.
 */
static int settings_change_diff(struct settings_walk *w,
				struct settings_change *c)
{
	struct fyai_buf all = { 0 };
	fy_generic shown0, stored, session, old_layer, new_layer, item, value;
	const char *a, *b, *p, *scope, *body;
	const char *path, *layer;
	char *part;
	size_t added = 0, removed = 0;
	int rc = -1;

	/* Keep hidden keys of the active document out of the diff. */
	shown0 = fy_mapping(w->gb);
	fy_foreach(item, c->changes) {
		path = fy_get(item, "path", "");
		value = settings_value(w, path, &layer);
		if (fy_is_valid(value))
			shown0 = settings_doc_set(w->gb, shown0, path,
					settings_redact(w, path, value, 0));
		if (!fy_is_valid(shown0))
			goto out;
	}
	stored = shown0;
	session = shown0;
	fy_foreach(item, c->changes) {
		path = fy_get(item, "path", "");
		value = fy_get(item, "value", fy_invalid);
		if (fy_is_valid(value))
			value = settings_redact(w, path, value, 0);
		if (c->trial || (c->legacy_scope &&
				 fyai_config_session_scoped(path)))
			session = settings_doc_set(w->gb, session, path, value);
		else
			stored = settings_doc_set(w->gb, stored, path, value);
		if (!fy_is_valid(stored) || !fy_is_valid(session))
			goto out;
	}

	if (fy_is_valid(c->stored)) {
		a = settings_emit(w->gb, shown0);
		b = settings_emit(w->gb, stored);
		if (!a || !b || fyai_diff_unified(a, strlen(a), b, strlen(b),
				"a/config.yaml", "b/config.yaml",
				2, &part))
			goto out;
		if (!*part) {
			free(part);
			old_layer = settings_changed_values(w,
				w->ctx->arena_config, c->changes);
			new_layer = settings_changed_values(w, c->stored,
				c->changes);
			if (!fy_is_valid(old_layer) || !fy_is_valid(new_layer))
				goto out;
			a = settings_emit(w->gb, old_layer);
			b = settings_emit(w->gb, new_layer);
			if (!a || !b || fyai_diff_unified(a, strlen(a), b,
					strlen(b), "a/config.yaml",
					"b/config.yaml", 2, &part))
				goto out;
		}
		rc = fyai_buf_append(&all, part);
		free(part);
		if (rc)
			goto out;
		rc = -1;
	}
	if (fy_is_valid(c->session)) {
		a = settings_emit(w->gb, shown0);
		b = settings_emit(w->gb, session);
		if (!a || !b || fyai_diff_unified(a, strlen(a), b, strlen(b),
				"a/session.yaml", "b/session.yaml", 2, &part))
			goto out;
		if (!*part) {
			free(part);
			old_layer = settings_changed_values(w,
				w->ctx->cfg->config_session, c->changes);
			new_layer = settings_changed_values(w, c->session,
				c->changes);
			if (!fy_is_valid(old_layer) || !fy_is_valid(new_layer))
				goto out;
			a = settings_emit(w->gb, old_layer);
			b = settings_emit(w->gb, new_layer);
			if (!a || !b || fyai_diff_unified(a, strlen(a), b,
					strlen(b), "a/session.yaml",
					"b/session.yaml", 2, &part))
				goto out;
		}
		rc = fyai_buf_append(&all, part);
		free(part);
		if (rc)
			goto out;
		rc = -1;
	}
	c->unified = settings_clean(w->gb, all.data ? all.data : "");
	if (!c->unified)
		goto out;
	for (p = c->unified; *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 :
						       p + strlen(p)) {
		if (*p == '+' && strncmp(p, "+++", 3))
			added++;
		else if (*p == '-' && strncmp(p, "---", 3))
			removed++;
	}
	scope = c->trial ? "this session only, as a trial" :
		fy_is_valid(c->stored) && fy_is_valid(c->session) ?
			"the branch configuration and this session" :
		fy_is_valid(c->stored) ? "the branch configuration" :
			"this session";
	body = settings_diff_block(w->gb, c->unified);
	if (!body)
		goto out;
	c->diff = fy_gb_intern_string(w->gb, fy_sprintfa(
		"**%zu key%s \u00b7 +%zu \u2212%zu lines \u00b7 %s**\n\n%s",
		fy_len(c->paths), fy_len(c->paths) == 1 ? "" : "s",
		added, removed, scope, body));
	rc = c->diff ? 0 : -1;
out:
	free(all.data);
	return rc;
}

/*
 * Build the configuration after @c in the branch, or in the session layer
 * for a trial. Both are checked; the diff is made from them.
 */
static int settings_change_build(struct settings_walk *w,
				 struct settings_change *c,
				 char *why, size_t why_size)
{
	struct fyai_ctx *ctx = w->ctx;
	struct fyai_cfg *cfg = ctx->cfg;
	fy_generic stored0, session0, stored, session, item, value, problems, p;
	bool in_session, any_stored = false, any_session = false;
	const char *path;
	size_t len;

	stored0 = fy_is_mapping(ctx->arena_config) ? ctx->arena_config :
		  fy_map_empty;
	session0 = fy_is_mapping(cfg->config_session) ? cfg->config_session :
		   fy_map_empty;
	stored = stored0;
	session = session0;
	c->before = fy_mapping(w->gb);
	c->after = fy_mapping(w->gb);
	fy_foreach(item, c->changes) {
		path = fy_get(item, "path", "");
		value = fy_get(item, "value", fy_invalid);
		in_session = c->trial || (c->legacy_scope &&
					    fyai_config_session_scoped(path));
		c->before = fy_assoc(w->gb, c->before, path, settings_doc_get(
			w->gb, in_session ? session0 : stored0, path));
		if (in_session) {
			session = settings_doc_set(cfg->gb, session, path,
						   value);
			any_session = true;
		} else {
			stored = settings_doc_set(ctx->gb, stored, path, value);
			any_stored = true;
		}
		if (!fy_is_valid(stored) || !fy_is_valid(session)) {
			snprintf(why, why_size, "%s: the value cannot be set",
				 path);
			return -1;
		}
	}
	c->stored = fy_invalid;
	c->session = fy_invalid;
	if (any_stored) {
		problems = fyai_config_check(cfg, &stored);
		if (!fy_empty(problems)) {
			len = (size_t)snprintf(why, why_size,
				"the configuration would not be valid:");
			fy_foreach(p, problems)
				if (len < why_size)
					len += (size_t)snprintf(why + len,
						why_size - len, " %s;",
						fy_castp(&p, ""));
			return -1;
		}
		c->stored = stored;
	}
	if (any_session) {
		if (!fyai_config_session_layer_valid(ctx, session)) {
			snprintf(why, why_size,
				 "the session layer would not be valid");
			return -1;
		}
		c->session = session;
	}
	fy_foreach(item, c->changes) {
		path = fy_get(item, "path", "");
		in_session = c->trial || (c->legacy_scope &&
					    fyai_config_session_scoped(path));
		c->after = fy_assoc(w->gb, c->after, path, settings_doc_get(
			w->gb, in_session ? session : stored, path));
	}
	if (settings_change_diff(w, c)) {
		snprintf(why, why_size, "the diff could not be made");
		return -1;
	}
	if (!*c->unified) {
		snprintf(why, why_size, "the change changes nothing");
		return -1;
	}
	return 0;
}

/* The next id of a log: @prefix and one more than the highest number. */
static const char *settings_next_id(struct fy_generic_builder *gb,
				    fy_generic log, char prefix)
{
	fy_generic item;
	const char *name;
	long long n, top = 0;

	fy_foreach(item, log) {
		name = fy_get(item, "name", "");
		if (*name != prefix)
			continue;
		n = strtoll(name + 1, NULL, 10);
		if (n > top)
			top = n;
	}
	return fy_gb_intern_string(gb, fy_sprintfa("%c%lld", prefix, top + 1));
}

/* @log with @entry, without its oldest entries past the bound. */
static fy_generic settings_log_append(struct fy_generic_builder *gb,
				      fy_generic log, fy_generic entry)
{
	fy_generic out, item;
	size_t n, skip;

	n = fy_len(log) + 1;
	skip = n > SETTINGS_LOG_MAX ? n - SETTINGS_LOG_MAX : 0;
	out = fy_sequence(gb);
	fy_foreach(item, log) {
		if (skip) {
			skip--;
			continue;
		}
		out = fy_append(gb, out, item);
	}
	return fy_append(gb, out, entry);
}

/* Build the store with one or two settings logs for one publication. */
static fy_generic settings_store_build(struct fyai_ctx *ctx,
				       const char *member, fy_generic log,
				       const char *member2, fy_generic log2)
{
	struct fyai_branch prev;
	fy_generic store;

	fyai_branch_decode(ctx->branch_prev, &prev);
	store = fyai_branch_store_build(ctx->gb, &prev);
	store = fy_assoc(ctx->gb, store, member,
			 fy_gb_internalize(ctx->gb, log));
	if (member2)
		store = fy_assoc(ctx->gb, store, member2,
				 fy_gb_internalize(ctx->gb, log2));
	return store;
}

static int settings_publish(struct fyai_ctx *ctx, const char *member,
			    fy_generic log, const char *member2,
			    fy_generic log2)
{
	fy_generic store;

	store = settings_store_build(ctx, member, log, member2, log2);
	fyai_error_check(ctx, fy_is_valid(store), err_out,
			 "settings: cannot build the store of branch '%s'",
			 fyai_ctx_branch(ctx));
	ctx->branch_store = store;
	fyai_branch_op_set(ctx, FYAI_BRANCH_OP_SETTINGS, NULL);
	return fyai_publish_state(ctx);
err_out:
	return -1;
}

fy_generic fyai_settings_log(struct fyai_ctx *ctx, const char *member)
{
	struct fyai_branch prev;

	if (!fyai_branch_decode(ctx->branch_prev, &prev))
		return fy_seq_empty;
	return fy_get(prev.store, member, fy_seq_empty);
}

/* The entry of a log for @c, built in @gb. */
static fy_generic settings_entry(struct fyai_ctx *ctx,
				 struct fy_generic_builder *gb,
				 const struct settings_change *c,
				 const char *id, const char *explanation,
				 const char *status)
{
	fy_generic entry;

	entry = fy_mapping(gb,
		"name", id,
		"status", status,
		"access", fyai_model_access_name(c->access),
		"paths", c->paths,
		"changes", c->changes,
		"trial", c->trial,
		"persistent", !c->trial && !c->legacy_scope,
		"before", c->before,
		"after", c->after,
		"diff", c->unified ? c->unified : "",
		"explanation", explanation,
		"origin", fyai_ctx_branch(ctx),
		"time", (long long)fyai_branch_timestamp());
	if (c->catalog)
		entry = fy_assoc(gb, entry, "document", "catalog",
				 "source", c->source ? c->source : "");
	if (c->mark_id)
		entry = fy_assoc(gb, entry, "of", fy_mapping(gb,
			"id", c->mark_id, "status", c->mark_status));
	return entry;
}

/* The entry @id of @log, or fy_invalid. */
static fy_generic settings_log_find(fy_generic log, const char *id)
{
	fy_generic item;

	fy_foreach(item, log)
		if (fy_equal(fy_get(item, "name", fy_invalid), id))
			return item;
	return fy_invalid;
}

/* @log with the status of entry @id replaced by @status. */
static fy_generic settings_log_status(struct fy_generic_builder *gb,
				      fy_generic log, const char *id,
				      const char *status)
{
	fy_generic out, item;

	out = fy_sequence(gb);
	fy_foreach(item, log) {
		if (fy_equal(fy_get(item, "name", fy_invalid), id))
			item = fy_assoc(gb, item, "status", status);
		out = fy_append(gb, out, item);
	}
	return out;
}

/*
 * Apply @c: the session layer first, then the branch configuration with the
 * entry of the change log in one publish. Returns 0, or -1 with the cause
 * reported.
 */
static int settings_apply(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
			  struct settings_change *c, const char *explanation,
			  const char **idp, const char *member2,
			  fy_generic log2)
{
	fy_generic log, store;
	const char *id;

	log = fyai_settings_log(ctx, "config_changes");
	id = settings_next_id(gb, log, 'c');
	if (c->mark_id)
		log = settings_log_status(gb, log, c->mark_id, c->mark_status);
	log = settings_log_append(gb, log, settings_entry(ctx, gb, c, id,
		explanation, c->catalog ? "applied" :
		fy_is_valid(c->stored) ? "applied" : "session"));
	if (c->catalog) {
		store = settings_store_build(ctx, "config_changes", log,
					     member2, log2);
		if (!fy_is_valid(store))
			return -1;
		fyai_branch_op_set(ctx, FYAI_BRANCH_OP_SETTINGS, NULL);
		if (fyai_catalog_commit(ctx, c->catalog_doc,
					      "the approved catalogue change",
					      store))
			return -1;
		*idp = id;
		return 0;
	}
	if (fy_is_valid(c->session) &&
	    fyai_config_session_layer_adopt(ctx, c->session))
		return -1;
	if (fy_is_valid(c->stored))
		ctx->arena_config = c->stored;
	if (settings_publish(ctx, "config_changes", log, member2, log2))
		return -1;
	if (fy_is_valid(c->stored) && fyai_config_rederive(ctx))
		return -1;
	*idp = id;
	return 0;
}

/* Keep @c as a proposal of this branch for the user to approve later. */
static int settings_propose(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
			    struct settings_change *c, const char *explanation,
			    const char **idp)
{
	fy_generic log;
	const char *id;

	log = fyai_settings_log(ctx, "config_proposals");
	id = settings_next_id(gb, log, 'p');
	log = settings_log_append(gb, log, settings_entry(ctx, gb, c, id,
		explanation, "pending"));
	if (settings_publish(ctx, "config_proposals", log, NULL, fy_invalid))
		return -1;
	*idp = id;
	return 0;
}

/* The answer of the review panel. */
struct settings_review {
	bool done;
	bool given;
	unsigned int selected;
	char *other;
};

static void settings_review_done(void *user,
				 const struct fyai_ask_reply *replies, size_t n)
{
	struct settings_review *r = user;

	r->done = true;
	if (!replies || !n)
		return;
	r->given = true;
	r->selected = replies[0].selected;
	if (replies[0].other)
		r->other = strdup(replies[0].other);
}

/*
 * Put @c to the user in the review panel: the proposal that the model wrote
 * and the diff that fyai made, under headings of their own. Returns 1 when
 * the user accepts, 0 when the user rejects, with the typed reply in
 * *@replyp, and -1 when the panel could not be shown.
 */
static fy_generic settings_review_questions(struct fy_generic_builder *gb,
					    const struct settings_change *c,
					    const char *explanation)
{
	fy_generic reject, accept, options, question;
	const char *text;

	text = fy_sprintfa("The model presents a change of %zu %s%s (%s). Apply it?",
			   fy_len(c->paths), c->catalog ? "catalogue field" : "key",
			   fy_len(c->paths) == 1 ? "" : "s",
			   fyai_model_access_name(c->access));
	/* Reject comes first and is under the cursor: a stray Enter, or one
	 * pasted with a reply, must not apply a change. Each level is built
	 * apart: a nested builder call expands to megabytes. */
	reject = fy_mapping(gb, "id", "reject", "label", "Reject",
			    "description", c->catalog ?
			    "Leave the catalogue as it is." :
			    "Leave the configuration as it is.",
			    "preview", "");
	accept = fy_mapping(gb, "id", "accept", "label", "Accept",
			    "description", "Apply the change that the diff shows.",
			    "preview", "");
	options = fy_sequence(gb, reject, accept);
	question = fy_mapping(gb,
		"id", "settings",
		"header", c->catalog ? "Catalogue" : "Settings",
		"question", text,
		"multi_select", false,
		"options", options,
		"proposal", explanation,
		"diff", c->unified);
	return fy_sequence(gb, question);
}

/*
 * The decision of the review panel: 1 when the user accepts, 0 when the
 * user rejects, with the typed reply in *@replyp when there is one. No
 * answer at all, as from Escape, rejects.
 */
static int settings_review_answer(const struct fyai_ask_reply *replies,
				  size_t n, char **replyp)
{
	*replyp = NULL;
	if (!replies || !n)
		return 0;
	if (replies[0].other && *replies[0].other) {
		*replyp = strdup(replies[0].other);
		return 0;
	}
	return (replies[0].selected & 2u) ? 1 : 0;
}

static int settings_review(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
			   const struct settings_change *c,
			   const char *explanation, char **replyp)
{
	struct settings_review r = { 0 };
	fy_generic questions;

	*replyp = NULL;
	questions = settings_review_questions(gb, c, explanation);
	fyai_error_check(ctx, fy_is_valid(questions), err_out,
			 "settings: cannot build the review of the change");
	if (fyai_ask_open(ctx, questions, NULL, settings_review_done, &r))
		return -1;
	while (!r.done)
		if (fyai_event_loop_step(fyai_ctx_loop(ctx), -1) < 0)
			break;
	if (!r.done) {
		fyai_ask_close(ctx, &r);
		fyai_error(ctx, "settings: the event loop stopped during the "
			   "review of a change");
		return -1;
	}
	if (r.other && *r.other) {
		*replyp = r.other;
		return 0;
	}
	free(r.other);
	return r.given && (r.selected & 2u) ? 1 : 0;
err_out:
	return -1;
}

static void settings_walk_init(struct settings_walk *w, struct fyai_ctx *ctx,
			       struct fy_generic_builder *gb)
{
	memset(w, 0, sizeof(*w));
	w->ctx = ctx;
	w->gb = gb;
	w->hidden = fy_sequence(gb);
	w->keys = fy_sequence(gb);
}

/*
 * Check the pending proposal @id again over the configuration in force and
 * make its diff. The configuration may have changed since the model
 * presented it, so the diff is always made now.
 */
static int settings_proposal_prepare(struct settings_walk *w, const char *id,
				     struct settings_change *c,
				     const char **explanationp)
{
	fy_generic entry, of;
	char why[1024];

	memset(c, 0, sizeof(*c));
	entry = settings_log_find(fyai_settings_log(w->ctx,
					"config_proposals"), id);
	fyai_error_check(w->ctx, fy_is_mapping(entry), err_out,
			 "no proposal '%s' on branch '%s'", id,
			 fyai_ctx_branch(w->ctx));
	fyai_error_check(w->ctx, fy_equal(fy_get(entry, "status", fy_invalid),
					  "pending"), err_out,
			 "proposal '%s' is %s, not pending", id,
			 fy_get(entry, "status", "?"));
	c->trial = fy_get(entry, "trial", false);
	*explanationp = fy_gb_intern_string(w->gb,
					    fy_get(entry, "explanation", ""));
	of = fy_get(entry, "of", fy_invalid);
	if (fy_is_mapping(of)) {
		c->mark_id = fy_gb_intern_string(w->gb, fy_get(of, "id", ""));
		c->mark_status = fy_gb_intern_string(w->gb,
						     fy_get(of, "status", ""));
	}
	if (fy_equal(fy_get(entry, "document", fy_invalid), "catalog")) {
		if (settings_catalog_prepare(w,
			fy_get(entry, "changes", fy_invalid),
			fy_get(entry, "source", ""), c, why, sizeof(why))) {
			fyai_error(w->ctx, "proposal '%s' cannot be applied: %s",
				   id, why);
			return -1;
		}
		return 0;
	}
	if (settings_change_normalize(w, fy_get(entry, "changes", fy_invalid),
				      c, why, sizeof(why)) ||
	    settings_change_build(w, c, why, sizeof(why))) {
		fyai_error(w->ctx, "proposal '%s' cannot be applied: %s", id,
			   why);
		return -1;
	}
	return 0;
err_out:
	return -1;
}

fy_generic fyai_settings_proposal_show(struct fyai_ctx *ctx,
				       struct fy_generic_builder *gb,
				       const char *id)
{
	struct settings_walk w;
	struct settings_change c;
	const char *explanation;

	settings_walk_init(&w, ctx, gb);
	if (settings_proposal_prepare(&w, id, &c, &explanation))
		return fy_invalid;
	return fy_mapping(gb, "id", id, "explanation", explanation,
			  "diff", c.diff, "document",
			  c.catalog ? "catalog" : "config");
}

/*
 * Tell the model how the user decided proposal @id: an event that the next
 * turn reads, as the answer of a background question is.
 */
static void settings_proposal_event(struct fyai_ctx *ctx,
				    struct fy_generic_builder *gb,
				    const char *id, fy_generic result)
{
	const char *json;

	json = emit_json_string(gb, result);
	if (!json || fyai_event_injectf(ctx, "[config '%s' %s]\n%s", id,
					fy_get(result, "status", "decided"),
					json))
		fyai_warning(ctx, "the model is not told that proposal '%s' "
			     "was decided", id);
}

int fyai_settings_proposal_decide(struct fyai_ctx *ctx,
				  struct fy_generic_builder *gb,
				  const char *id, bool approve, bool notify,
				  fy_generic *resultp)
{
	struct settings_walk w;
	struct settings_change c;
	fy_generic proposals;
	const char *explanation, *cid = NULL;

	settings_walk_init(&w, ctx, gb);
	fyai_error_check(ctx, ctx->gb, err_out, "no arena; run fyai init");
	if (settings_proposal_prepare(&w, id, &c, &explanation))
		return -1;
	proposals = fyai_settings_log(ctx, "config_proposals");
	if (!approve) {
		proposals = settings_log_status(gb, proposals, id, "rejected");
		if (settings_publish(ctx, "config_proposals", proposals, NULL,
				     fy_invalid))
			return -1;
		*resultp = fy_mapping(gb, "id", id, "status", "rejected");
		if (c.catalog)
			*resultp = fy_assoc(gb, *resultp, "document", "catalog");
		if (notify)
			settings_proposal_event(ctx, gb, id, *resultp);
		return 0;
	}
	proposals = settings_log_status(gb, proposals, id, "approved");
	if (settings_apply(ctx, gb, &c, explanation, &cid,
			   "config_proposals", proposals))
		return -1;
	*resultp = fy_mapping(gb, "id", id, "status", "approved",
			      "change", cid, "diff", c.diff);
	if (c.catalog)
		*resultp = fy_assoc(gb, *resultp, "document", "catalog");
	if (notify)
		settings_proposal_event(ctx, gb, id, fy_mapping(gb,
			"id", id, "status", "approved", "change", cid,
			"diff", c.unified, "document",
			c.catalog ? "catalog" : "config"));
	return 0;
err_out:
	return -1;
}

/*
 * Apply, review or propose the checked change @c that the model presents:
 * fyai applies a change of write keys unasked only when the setting and the
 * shell both allow it; any other change goes to the review panel, or waits
 * as a proposal when nobody can review it now.
 */
static fy_generic settings_present(struct settings_walk *w,
				   struct settings_change *c,
				   const char *explanation, bool *okp)
{
	struct fyai_ctx *ctx = w->ctx;
	struct fyai_cfg *cfg = ctx->cfg;
	enum fyai_model_access access;
	const char *id = NULL, *status = "rejected";
	char *reply = NULL;
	fy_generic result;
	int rc;

	*okp = false;
	access = c->access;
	if (access == FYAI_MODEL_WRITE &&
	    (cfg->model_access < FYAI_MODEL_WRITE ||
	     !settings_arena_protected(ctx)))
		access = FYAI_MODEL_REVIEW;

	if (access == FYAI_MODEL_WRITE) {
		rc = settings_apply(ctx, w->gb, c, explanation, &id, NULL,
				    fy_invalid);
		status = "applied";
	} else if (!fyai_ui_ask_available(ctx)) {
		rc = settings_propose(ctx, w->gb, c, explanation, &id);
		status = "pending";
	} else {
		rc = settings_review(ctx, w->gb, c, explanation, &reply);
		if (rc > 0) {
			rc = settings_apply(ctx, w->gb, c, explanation, &id,
					    NULL, fy_invalid);
			status = "applied";
		}
	}
	if (rc < 0) {
		free(reply);
		return fy_value(w->gb, "settings: the change was not applied");
	}
	result = fy_mapping(w->gb, "status", status, "access",
			    fyai_model_access_name(access), "diff", c->unified);
	if (c->catalog)
		result = fy_assoc(w->gb, result, "document", "catalog",
				  "source", c->source);
	if (!strcmp(status, "applied") && !c->catalog &&
	    !fy_is_valid(c->stored))
		result = fy_assoc(w->gb, result, "scope", "session",
			"note", "This change lasts only for this invocation.");
	if (id)
		result = fy_assoc(w->gb, result, "id", id);
	if (reply) {
		result = fy_assoc(w->gb, result, "reply",
				  fy_value(w->gb, reply));
		free(reply);
	}
	if (!strcmp(status, "pending"))
		result = fy_assoc(w->gb, result, "note", "Nobody can review "
			"the change now. It is kept as a proposal; the user "
			"applies it with `config approve`.");
	*okp = strcmp(status, "rejected") != 0;
	return result;
}

/*
 * Build in @c the change that takes entry @entry of the change log back:
 * each path to its value before the change. A path whose value is no
 * longer the one the change left is a conflict; with @force the other paths
 * are taken back, else nothing is. Returns 0, 1 for a conflict, or -1.
 */
static int settings_revert_build(struct settings_walk *w, fy_generic entry,
				 bool force, struct settings_change *c,
				 fy_generic *conflictsp, char *why,
				 size_t why_size)
{
	struct fyai_ctx *ctx = w->ctx;
	fy_generic before, after, item, cur, out, paths, conflicts, b;
	const char *path;
	bool in_session;
	size_t len;
	int access;

	memset(c, 0, sizeof(*c));
	c->trial = fy_get(entry, "trial", false);
	c->legacy_scope = !fy_get(entry, "persistent", false);
	before = fy_get(entry, "before", fy_invalid);
	after = fy_get(entry, "after", fy_invalid);
	out = fy_sequence(w->gb);
	paths = fy_sequence(w->gb);
	conflicts = fy_sequence(w->gb);
	fy_foreach(item, fy_get(entry, "paths", fy_invalid)) {
		path = fy_gb_intern_string(w->gb, fy_castp(&item, ""));
		in_session = c->trial || (c->legacy_scope &&
					    fyai_config_session_scoped(path));
		cur = settings_doc_get(w->gb, in_session ?
				       ctx->cfg->config_session :
				       ctx->arena_config, path);
		if (fy_compare(cur, fy_get(after, path, fy_null))) {
			conflicts = fy_append(w->gb, conflicts, path);
			continue;
		}
		b = fy_get(before, path, fy_null);
		out = fy_append(w->gb, out, fy_is_null(b) ?
			fy_mapping(w->gb, "path", path, "delete", true) :
			fy_mapping(w->gb, "path", path, "value", b));
		paths = fy_append(w->gb, paths, path);
	}
	*conflictsp = conflicts;
	if (!fy_empty(conflicts) && !force) {
		len = (size_t)snprintf(why, why_size, "changed since the "
				       "change, so nothing is taken back:");
		fy_foreach(item, conflicts)
			if (len < why_size)
				len += (size_t)snprintf(why + len,
					why_size - len, " %s",
					fy_castp(&item, ""));
		return 1;
	}
	if (fy_empty(out)) {
		snprintf(why, why_size, "every path changed since the change");
		return -1;
	}
	access = fyai_model_access_parse(fy_get(entry, "access", fy_invalid));
	c->access = access < 0 ? FYAI_MODEL_HIDDEN :
		    (enum fyai_model_access)access;
	c->changes = out;
	c->paths = paths;
	c->mark_id = fy_gb_intern_string(w->gb, fy_get(entry, "name", ""));
	c->mark_status = "reverted";
	return settings_change_build(w, c, why, why_size) ? -1 : 0;
}

/* The entry @id of the change log that can be reverted or kept. */
static fy_generic settings_change_entry(struct fyai_ctx *ctx, const char *id)
{
	fy_generic entry;

	entry = settings_log_find(fyai_settings_log(ctx, "config_changes"),
				  id);
	fyai_error_check(ctx, fy_is_mapping(entry), err_out,
			 "no change '%s' on branch '%s'", id,
			 fyai_ctx_branch(ctx));
	fyai_error_check(ctx, fy_any_equal(fy_get(entry, "status",
						  fy_invalid),
					   "applied", "session"), err_out,
			 "change '%s' is %s", id,
			 fy_get(entry, "status", "?"));
	return entry;
err_out:
	return fy_invalid;
}

/* The model takes back one of its own changes of write keys. */
static fy_generic settings_revert_run(struct settings_walk *w,
				      fy_generic args, bool *okp)
{
	struct fyai_ctx *ctx = w->ctx;
	struct settings_change c;
	fy_generic entry, conflicts;
	const char *id = fy_get(args, "id", "");
	char why[1024];
	int rc;

	*okp = false;
	if (ctx->cfg->model_access < FYAI_MODEL_REVIEW)
		return fy_value(w->gb, "settings: model_access allows reads "
				"only");
	entry = settings_log_find(fyai_settings_log(ctx, "config_changes"),
				  id);
	if (!fy_is_mapping(entry) ||
	    !fy_any_equal(fy_get(entry, "status", fy_invalid), "applied",
			  "session"))
		return fy_value(w->gb, fy_sprintfa("settings: no change '%s' "
				"to take back", id));
	if (fy_equal(fy_get(entry, "document", fy_invalid), "catalog"))
		return fy_value(w->gb, "settings: catalogue changes are "
				"reverted with catalog commands");
	if (!fy_equal(fy_get(entry, "access", fy_invalid), "write"))
		return fy_value(w->gb, fy_sprintfa("settings: change %s has "
				"review keys; only the user takes it back",
				id));
	rc = settings_revert_build(w, entry, false, &c, &conflicts, why,
				   sizeof(why));
	if (rc)
		return fy_value(w->gb, fy_sprintfa("settings: %s", why));
	return settings_present(w, &c, fy_sprintfa("Take back change %s.", id),
				okp);
}

int fyai_settings_revert(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
			 const char *id, bool force, fy_generic *resultp)
{
	struct settings_walk w;
	struct settings_change c;
	fy_generic entry, conflicts;
	const char *cid;
	char why[1024];

	settings_walk_init(&w, ctx, gb);
	fyai_error_check(ctx, ctx->gb, err_out, "no arena; run fyai init");
	entry = settings_change_entry(ctx, id);
	if (!fy_is_mapping(entry))
		return -1;
	fyai_error_check(ctx, !fy_equal(fy_get(entry, "document", fy_invalid),
					   "catalog"), err_out,
			 "change '%s' is of the catalogue; use catalog commands",
			 id);
	if (settings_revert_build(&w, entry, force, &c, &conflicts, why,
				  sizeof(why))) {
		fyai_error(ctx, "change '%s' is not taken back: %s", id, why);
		return -1;
	}
	if (settings_apply(ctx, gb, &c, fy_sprintfa("Take back change %s.",
						    id), &cid, NULL,
			   fy_invalid))
		return -1;
	*resultp = fy_mapping(gb, "id", id, "status", "reverted",
			      "change", cid, "conflicts", conflicts,
			      "diff", c.diff);
	return 0;
err_out:
	return -1;
}

int fyai_settings_keep(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
		       const char *id, fy_generic *resultp)
{
	struct settings_walk w;
	struct settings_change c;
	fy_generic entry, after, item, v, out, paths, session;
	fy_generic session0;
	const char *path, *cid;
	char why[1024];

	settings_walk_init(&w, ctx, gb);
	memset(&c, 0, sizeof(c));
	fyai_error_check(ctx, ctx->gb, err_out, "no arena; run fyai init");
	entry = settings_change_entry(ctx, id);
	if (!fy_is_mapping(entry))
		return -1;
	fyai_error_check(ctx, fy_get(entry, "trial", false) ||
			 (fy_equal(fy_get(entry, "status", fy_invalid),
				   "session") &&
			  !fy_get(entry, "persistent", false)), err_out,
			 "change '%s' is not a trial or session-only change", id);
	/* Store each trial key in the branch and remove its session override. */
	after = fy_get(entry, "after", fy_invalid);
	out = fy_sequence(gb);
	paths = fy_sequence(gb);
	session = fy_is_mapping(ctx->cfg->config_session) ?
		  ctx->cfg->config_session : fy_map_empty;
	fy_foreach(item, fy_get(entry, "paths", fy_invalid)) {
		path = fy_gb_intern_string(gb, fy_castp(&item, ""));
		v = fy_get(after, path, fy_null);
		out = fy_append(gb, out, fy_is_null(v) ?
			fy_mapping(gb, "path", path, "delete", true) :
			fy_mapping(gb, "path", path, "value", v));
		paths = fy_append(gb, paths, path);
		session = settings_doc_set(ctx->cfg->gb, session, path,
					   fy_invalid);
	}
	fyai_error_check(ctx, !fy_empty(out), err_out,
			 "change '%s' has no key that a branch stores", id);
	c.changes = out;
	c.paths = paths;
	c.access = FYAI_MODEL_REVIEW;
	c.mark_id = id;
	c.mark_status = "kept";
	if (settings_change_build(&w, &c, why, sizeof(why))) {
		fyai_error(ctx, "change '%s' is not kept: %s", id, why);
		return -1;
	}
	session0 = fy_is_mapping(ctx->cfg->config_session) ?
		   ctx->cfg->config_session : fy_map_empty;
	c.session = fy_compare(session, session0) ? session : fy_invalid;
	if ((fy_is_valid(c.session) &&
	     !fyai_config_session_layer_valid(ctx, session)) ||
	    settings_change_diff(&w, &c)) {
		fyai_error(ctx, "change '%s' is not kept: the session layer "
			   "cannot give up its keys", id);
		return -1;
	}
	if (settings_apply(ctx, gb, &c, fy_sprintfa("Keep change %s.", id),
			   &cid, NULL, fy_invalid))
		return -1;
	*resultp = fy_mapping(gb, "id", id, "status", "kept", "change", cid,
			      "diff", c.diff);
	return 0;
err_out:
	return -1;
}

/* The proposals of this branch, as the model reads them. */
static fy_generic settings_proposals_run(struct settings_walk *w)
{
	fy_generic rows, item;

	rows = fy_sequence(w->gb);
	fy_foreach(item, fyai_settings_log(w->ctx, "config_proposals"))
		rows = fy_append(w->gb, rows, fy_mapping(w->gb,
			"id", fy_get(item, "name", ""),
			"document", fy_get(item, "document", "config"),
			"status", fy_get(item, "status", ""),
			"paths", fy_get(item, "paths", fy_invalid),
			"changes", fy_get(item, "changes", fy_invalid)));
	return fy_mapping(w->gb, "proposals", rows);
}

/* A pending proposal that waits for the answer of the review panel. */
struct fyai_settings_review {
	struct fyai_ctx *ctx;
	struct fy_generic_builder *gb;
	struct settings_change c;
	const char *id;
	const char *explanation;
	fy_generic questions;
};

struct fyai_settings_review *
fyai_settings_review_open(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
			  const char *id)
{
	struct fyai_settings_review *r;
	struct settings_walk w;

	fyai_error_check(ctx, ctx->gb, err_out, "no arena; run fyai init");
	r = calloc(1, sizeof(*r));
	fyai_error_check(ctx, r, err_out,
			 "cannot keep the review of proposal '%s'", id);
	settings_walk_init(&w, ctx, gb);
	r->ctx = ctx;
	r->gb = gb;
	r->id = fy_gb_intern_string(gb, id);
	if (!r->id ||
	    settings_proposal_prepare(&w, id, &r->c, &r->explanation)) {
		free(r);
		return NULL;
	}
	r->questions = settings_review_questions(gb, &r->c, r->explanation);
	if (!fy_is_valid(r->questions)) {
		fyai_error(ctx, "cannot build the review of proposal '%s'", id);
		free(r);
		return NULL;
	}
	return r;
err_out:
	return NULL;
}

fy_generic fyai_settings_review_questions(struct fyai_settings_review *r)
{
	return r->questions;
}

int fyai_settings_review_apply(struct fyai_settings_review *r,
			       const struct fyai_ask_reply *replies, size_t n,
			       bool notify, fy_generic *resultp)
{
	struct fyai_ctx *ctx = r->ctx;
	struct fy_generic_builder *gb = r->gb;
	fy_generic proposals, event;
	const char *cid = NULL;
	char *reply = NULL;

	/* No panel answers a verb: the user approved it by naming it. */
	if (replies && !settings_review_answer(replies, n, &reply)) {
		*resultp = fy_mapping(gb, "id", r->id, "status", "pending");
		if (reply)
			*resultp = fy_assoc(gb, *resultp, "reply",
					    fy_value(gb, reply));
		free(reply);
		return 0;
	}
	proposals = settings_log_status(gb, fyai_settings_log(ctx,
					"config_proposals"), r->id, "approved");
	if (settings_apply(ctx, gb, &r->c, r->explanation, &cid,
			   "config_proposals", proposals))
		return -1;
	*resultp = fy_mapping(gb, "id", r->id, "status", "approved",
			      "change", cid, "diff", r->c.diff);
	if (r->c.catalog)
		*resultp = fy_assoc(gb, *resultp, "document", "catalog");
	if (notify) {
		event = fy_mapping(gb, "id", r->id, "status", "approved",
				   "change", cid, "diff", r->c.unified,
				   "document", r->c.catalog ? "catalog" :
					"config");
		settings_proposal_event(ctx, gb, r->id, event);
	}
	return 0;
}

void fyai_settings_review_free(struct fyai_settings_review *r)
{
	free(r);
}

/* Present, review, and apply a change set of the model. */
static fy_generic settings_change_run(struct settings_walk *w, fy_generic args,
				      bool *okp)
{
	struct fyai_ctx *ctx = w->ctx;
	struct fyai_cfg *cfg = ctx->cfg;
	struct settings_change c;
	const char *explanation;
	char why[1024];

	*okp = false;
	memset(&c, 0, sizeof(c));
	if (!ctx->gb)
		return fy_value(w->gb, "settings: no arena; the change cannot "
				"be kept");
	if (cfg->model_access < FYAI_MODEL_REVIEW)
		return fy_value(w->gb, "settings: model_access allows reads "
				"only");
	explanation = fy_get(args, "explanation", "");
	if (strlen(explanation) > SETTINGS_EXPLANATION_MAX)
		return fy_value(w->gb, fy_sprintfa("settings: the explanation "
				"is longer than %d bytes",
				SETTINGS_EXPLANATION_MAX));
	explanation = settings_clean(w->gb, explanation);
	c.trial = fy_get(args, "trial", false);
	if (!explanation ||
	    settings_change_normalize(w, fy_get(args, "changes", fy_invalid),
				      &c, why, sizeof(why)) ||
	    settings_change_build(w, &c, why, sizeof(why)))
		return fy_value(w->gb, fy_sprintfa("settings: %s",
				explanation ? why : "the explanation could "
				"not be kept"));

	return settings_present(w, &c, explanation, okp);
}

/* A catalogue path names an item of a list by its name. */

static fy_generic settings_catalog_select(struct settings_walk *w,
					  fy_generic cat, fy_generic args);
static fy_generic settings_export(struct settings_walk *w, bool catalog,
				  fy_generic args);

/* The providers that offer each model: {canonical id: [provider, ...]}. */
static fy_generic catalog_offered_by(struct fy_generic_builder *gb,
				     fy_generic cat)
{
	fy_generic out, provider, offering, id, names;

	out = fy_mapping(gb);
	fy_foreach(provider, fy_get(cat, "providers", fy_invalid)) {
		fy_foreach(offering, fy_get(provider, "models", fy_invalid)) {
			id = fy_get(offering, "canonical_id", fy_invalid);
			if (!fy_is_string(id))
				continue;
			names = fy_get(out, id, fy_seq_empty);
			out = fy_assoc(gb, out, id, fy_append(gb, names,
				fy_get(provider, "name", fy_invalid)));
		}
	}
	return out;
}

/* Whether @word is in @text, or in a member of the sequence @seq. */
static bool catalog_word_in(const char *word, const char *text,
			    fy_generic seq)
{
	fy_generic v;

	if (text && strcasestr(text, word))
		return true;
	fy_foreach(v, seq)
		if (fy_is_string(v) && strcasestr(fy_castp(&v, ""), word))
			return true;
	return false;
}

static fy_generic catalog_search(struct settings_walk *w, fy_generic cat,
				 const char *query, long long limit)
{
	const char *words[SETTINGS_SEARCH_WORDS], *name, *current;
	fy_generic offered, model, by_name, by_text, row, providers, keys;
	char *copy, *word, *save;
	size_t nwords = 0, i, in_name, max, found = 0;

	max = limit > 0 && limit <= SETTINGS_SEARCH_MAX ? (size_t)limit :
	      SETTINGS_SEARCH_LIMIT;
	copy = strdup(query);
	if (!copy)
		return fy_value(w->gb, "settings: the query could not be kept");
	for (word = strtok_r(copy, " \t", &save);
	     word && nwords < SETTINGS_SEARCH_WORDS;
	     word = strtok_r(NULL, " \t", &save))
		words[nwords++] = fy_gb_intern_string(w->gb, word);
	free(copy);
	if (!nwords)
		return fy_value(w->gb, "settings: search needs a query");
	offered = catalog_offered_by(w->gb, cat);
	current = w->ctx->cfg->model ? w->ctx->cfg->model : "";
	by_name = fy_sequence(w->gb);
	by_text = fy_sequence(w->gb);
	fy_foreach(model, fy_get(cat, "models", fy_invalid)) {
		name = fy_get(model, "name", "");
		providers = fy_get(offered, name, fy_seq_empty);
		in_name = 0;
		for (i = 0; i < nwords; i++) {
			if (strcasestr(name, words[i]))
				in_name++;
			else if (!catalog_word_in(words[i],
					fy_get(model, "display_name", ""),
					fy_get(model, "capabilities",
					       fy_invalid)) &&
				 !catalog_word_in(words[i], NULL,
					fy_get(model, "modalities",
					       fy_invalid)) &&
				 !catalog_word_in(words[i], NULL, providers))
				break;
		}
		if (i < nwords)
			continue;
		found++;
		row = fy_mapping(w->gb,
			"path", fyai_path_join(w->gb, "models", name),
			"display_name", fy_get(model, "display_name", ""),
			"context_window", fy_get(model, "context_window", 0LL),
			"providers", providers,
			"capabilities", fy_get(model, "capabilities",
					       fy_seq_empty),
			"current", (_Bool)!strcmp(name, current));
		if (in_name == nwords && fy_len(by_name) < max)
			by_name = fy_append(w->gb, by_name, row);
		else if (in_name < nwords && fy_len(by_text) < max)
			by_text = fy_append(w->gb, by_text, row);
	}
	keys = by_name;
	fy_foreach(row, by_text) {
		if (fy_len(keys) >= max)
			break;
		keys = fy_append(w->gb, keys, row);
	}
	return fy_mapping(w->gb, "document", "catalog", "query", query,
			  "models", keys, "found", (long long)found);
}

/*
 * @v at @path, kept short: a provider gives the ids of its models, not each
 * offering, which `providers/NAME/models/ID` reads one at a time.
 */
static fy_generic catalog_short(struct settings_walk *w, const char *path,
				fy_generic v)
{
	fy_generic ids, offering;

	if (strncmp(path, "providers/", 10) || strchr(path + 10, '/') ||
	    !fy_is_mapping(v))
		return v;
	ids = fy_sequence(w->gb);
	fy_foreach(offering, fy_get(v, "models", fy_invalid))
		ids = fy_append(w->gb, ids, fy_get(offering, "canonical_id",
						   fy_invalid));
	return fy_assoc(w->gb, v, "models", ids);
}

/* The names of the items at @path: a list by name, a mapping by key. */
static fy_generic catalog_names(struct settings_walk *w, fy_generic v)
{
	fy_generic out, item, key;

	out = fy_sequence(w->gb);
	if (fy_is_sequence(v)) {
		fy_foreach(item, v) {
			key = fy_get(item, "name", fy_invalid);
			if (!fy_is_string(key))
				key = fy_get(item, "canonical_id", fy_invalid);
			if (fy_is_string(key))
				out = fy_append(w->gb, out, key);
		}
	} else {
		fy_foreach_key_value(key, item, v)
			out = fy_append(w->gb, out, key);
	}
	return out;
}

static int settings_catalog_prepare(struct settings_walk *w,
				    fy_generic changes, const char *source,
				    struct settings_change *c,
				    char *why, size_t why_size)
{
	fy_generic item, paths, old, now;
	const char *path, *a, *b, *block;
	const unsigned char *p;
	char *unified = NULL;
	bool del;

	if (!fy_is_sequence(changes) || fy_empty(changes) ||
	    fy_len(changes) > SETTINGS_CHANGES_MAX) {
		snprintf(why, why_size, "give 1 to 32 catalogue changes");
		return -1;
	}
	if (!*source || strlen(source) > 2048) {
		snprintf(why, why_size, "give a source for the catalogue values");
		return -1;
	}
	for (p = (const unsigned char *)source; *p; p++)
		if (*p < 0x20 || *p == 0x7f || *p == '`') {
			snprintf(why, why_size, "source must be one line of text");
			return -1;
		}
	memset(c, 0, sizeof(*c));
	c->catalog = true;
	c->access = FYAI_MODEL_WRITE;
	c->stored = fy_invalid;
	c->session = fy_invalid;
	c->changes = changes;
	c->source = settings_clean(w->gb, source);
	if (!c->source)
		return -1;
	paths = fy_sequence(w->gb);
	c->before = fy_mapping(w->gb);
	c->after = fy_mapping(w->gb);
	fy_foreach(item, changes) {
		path = fy_get(item, "path", "");
		del = fy_get(item, "delete", false);
		if (!*path || !settings_path_valid(path) ||
		    strchr(path, '*') || seq_has(paths, path) ||
		    (del == fy_is_valid(fy_get(item, "value", fy_invalid)))) {
			snprintf(why, why_size, "each catalogue change needs a "
				 "unique slash path and either a value or delete: true");
			return -1;
		}
		paths = fy_append(w->gb, paths, path);
		old = fyai_catalog_at(w->ctx, path);
		c->before = fy_assoc(w->gb, c->before, path,
				     fyai_generic_or_null(old));
	}
	c->paths = paths;
	if (fyai_catalog_patch_models_prepare(w->ctx, changes,
					      &c->catalog_doc)) {
		snprintf(why, why_size, "the catalogue change is invalid");
		return -1;
	}
	fy_foreach(item, changes) {
		path = fy_get(item, "path", "");
		now = fyai_catalog_doc_at(c->catalog_doc, path);
		c->after = fy_assoc(w->gb, c->after, path,
				    fyai_generic_or_null(now));
	}
	a = settings_emit(w->gb, c->before);
	b = settings_emit(w->gb, c->after);
	if (!a || !b || fyai_diff_unified(a, strlen(a), b, strlen(b),
				    "a/catalog.yaml", "b/catalog.yaml", 2,
				    &unified))
		goto diff_error;
	c->unified = settings_clean(w->gb, unified);
	free(unified);
	unified = NULL;
	if (!c->unified || !*c->unified)
		goto diff_error;
	block = settings_diff_block(w->gb, c->unified);
	if (!block)
		goto diff_error;
	c->diff = fy_gb_intern_string(w->gb, block);
	return c->diff ? 0 : -1;
diff_error:
	free(unified);
	snprintf(why, why_size, "the catalogue diff could not be made");
	return -1;
}

static fy_generic settings_catalog_change(struct settings_walk *w,
					  fy_generic args, bool *okp)
{
	struct settings_change c;
	const char *explanation;
	char why[1024];

	*okp = false;
	if (!w->ctx->gb)
		return fy_value(w->gb, "settings: no arena; run fyai init");
	if (w->ctx->cfg->model_access < FYAI_MODEL_REVIEW)
		return fy_value(w->gb, "settings: model_access allows reads only");
	explanation = fy_get(args, "explanation", "");
	if (strlen(explanation) > SETTINGS_EXPLANATION_MAX)
		return fy_value(w->gb, "settings: the explanation is too long");
	explanation = settings_clean(w->gb, explanation);
	if (!explanation)
		return fy_value(w->gb, "settings: the explanation could not "
				"be kept");
	explanation = fy_gb_intern_string(w->gb, fy_sprintfa("%s%sSource: %s",
		*explanation ? explanation : "Catalogue update",
		*explanation ? "\n\n" : ".\n\n",
		fy_get(args, "source", "")));
	if (!explanation)
		return fy_value(w->gb, "settings: the explanation could not "
				"be kept");
	if (settings_catalog_prepare(w,
		fy_get(args, "changes", fy_invalid),
		fy_get(args, "source", ""), &c, why, sizeof(why)))
		return fy_value(w->gb, fy_sprintfa("settings: %s", why));
	return settings_present(w, &c, explanation, okp);
}

static fy_generic settings_catalog_run(struct settings_walk *w,
				       fy_generic action, const char *path,
				       fy_generic args, bool *okp)
{
	fy_generic cat, v;

	cat = fyai_catalog_effective(w->ctx->arena_catalog, NULL);
	if (!fy_is_mapping(cat))
		return fy_value(w->gb, "settings: the catalogue cannot be "
				"read");
	if (fy_equal(action, "change"))
		return settings_catalog_change(w, args, okp);
	if (fy_equal(action, "search"))
		return catalog_search(w, cat, fy_get(args, "query", ""),
				      fy_get(args, "limit", 0LL));
	if (fy_equal(action, "select"))
		return settings_catalog_select(w, cat, args);
	if (fy_equal(action, "export"))
		return settings_export(w, true, args);
	if (!fy_any_equal(action, "get", "list"))
		return fy_value(w->gb, "settings: catalogue actions are search, "
				"get, list, select, export and change");
	v = *path ? fyai_catalog_at(w->ctx, path) : cat;
	if (!fy_is_valid(v))
		return fy_value(w->gb, fy_sprintfa("settings: the catalogue "
				"has nothing at %s", path));
	if (fy_equal(action, "list")) {
		if (!fy_is_sequence(v) && !fy_is_mapping(v))
			return fy_value(w->gb, fy_sprintfa("settings: %s has "
					"no items", path));
		return fy_mapping(w->gb, "document", "catalog", "path", path,
				  "names", catalog_names(w, v));
	}
	if (!*path)
		return fy_value(w->gb, "settings: get a part of the catalogue, "
				"such as models/NAME or providers/NAME");
	return fy_mapping(w->gb, "document", "catalog", "path", path,
			  "value", catalog_short(w, path, v));
}

/*
 * select: rows that match every condition of `where`, sorted, and cut to
 * a limit; each row only with the fields asked for. The rows of the
 * configuration are its keys; the rows of the catalogue are the items at a
 * path, where a segment `*` takes every item and names it in a column.
 */

/* The value of @field of @row, or fy_invalid. `pricing/blended` is made. */
static fy_generic select_field(struct fy_generic_builder *gb, fy_generic row,
			       const char *field)
{
	fy_generic in, out;

	if (!strcmp(field, "pricing/blended")) {
		in = fy_get_at_pathstr(gb, row, "pricing/input");
		out = fy_get_at_pathstr(gb, row, "pricing/output");
		if (!fy_is_valid(in) || !fy_is_valid(out) ||
		    fy_is_string(in) || fy_is_string(out))
			return fy_invalid;
		/* Three tokens read for each token written. */
		return fy_value(gb, (3.0 * fy_cast(in, 0.0) +
				     fy_cast(out, 0.0)) / 4.0);
	}
	return fy_is_mapping(row) ? fy_get_at_pathstr(gb, row, field) :
	       fy_invalid;
}

static bool select_number(fy_generic v, double *dp)
{
	if (!fy_is_valid(v) || fy_is_string(v) || fy_is_mapping(v) ||
	    fy_is_sequence(v) || fy_is_null(v) || fy_is_bool(v))
		return false;
	*dp = fy_cast(v, 0.0);
	return true;
}

/* -1, 0 or 1; numbers by value, strings by text, a missing value last. */
static int select_cmp(fy_generic a, fy_generic b)
{
	double da, db;

	if (!fy_is_valid(a) || fy_is_null(a))
		return fy_is_valid(b) && !fy_is_null(b) ? 1 : 0;
	if (!fy_is_valid(b) || fy_is_null(b))
		return -1;
	if (select_number(a, &da) && select_number(b, &db))
		return da < db ? -1 : da > db;
	if (fy_is_string(a) && fy_is_string(b))
		return strcmp(fy_castp(&a, ""), fy_castp(&b, ""));
	return fy_compare(a, b) < 0 ? -1 : fy_compare(a, b) > 0;
}

/* Whether @row meets the condition @cond, checked before. */
static bool select_meets(struct fy_generic_builder *gb, fy_generic row,
			 fy_generic cond)
{
	fy_generic v, want, item;
	const char *op = fy_get(cond, "op", "");
	int c;

	v = select_field(gb, row, fy_get(cond, "field", ""));
	want = fy_get(cond, "value", fy_invalid);
	if (!strcmp(op, "exists"))
		return fy_is_valid(v) && !fy_is_null(v);
	if (!strcmp(op, "has")) {
		fy_foreach(item, v)
			if (!fy_compare(item, want))
				return true;
		return false;
	}
	if (!strcmp(op, "contains"))
		return fy_is_string(v) && fy_is_string(want) &&
		       strcasestr(fy_castp(&v, ""), fy_castp(&want, ""));
	if (!fy_is_valid(v))
		return !strcmp(op, "ne");
	c = select_cmp(v, want);
	if (!strcmp(op, "eq"))
		return !c && !fy_compare(v, want) ? true : !c;
	if (!strcmp(op, "ne"))
		return c != 0;
	if (!strcmp(op, "lt"))
		return c < 0;
	if (!strcmp(op, "le"))
		return c <= 0;
	if (!strcmp(op, "gt"))
		return c > 0;
	return c >= 0;
}

/* Check the conditions and the fields of a select. */
static int select_check(fy_generic where, fy_generic fields,
			const char *sort, char *why, size_t why_size)
{
	static const char *const ops[] = {
		"eq", "ne", "lt", "le", "gt", "ge", "has", "contains",
		"exists",
	};
	fy_generic cond, f;
	const char *op, *field;
	size_t i;

	if (fy_is_valid(where) && (!fy_is_sequence(where) ||
				   fy_len(where) > SETTINGS_WHERE_MAX)) {
		snprintf(why, why_size, "where is a list of up to %d "
			 "conditions", SETTINGS_WHERE_MAX);
		return -1;
	}
	fy_foreach(cond, where) {
		field = fy_get(cond, "field", "");
		op = fy_get(cond, "op", "");
		if (!*field || !settings_path_valid(field)) {
			snprintf(why, why_size, "a condition needs a field");
			return -1;
		}
		for (i = 0; i < ARRAY_SIZE(ops); i++)
			if (!strcmp(op, ops[i]))
				break;
		if (i == ARRAY_SIZE(ops)) {
			snprintf(why, why_size, "%s: op must be eq, ne, lt, "
				 "le, gt, ge, has, contains or exists", field);
			return -1;
		}
		if (strcmp(op, "exists") &&
		    !fy_is_valid(fy_get(cond, "value", fy_invalid))) {
			snprintf(why, why_size, "%s: %s needs a value", field,
				 op);
			return -1;
		}
	}
	fy_foreach(f, fields) {
		if (!fy_is_string(f) ||
		    !settings_path_valid(fy_castp(&f, ""))) {
			snprintf(why, why_size, "fields are slash paths");
			return -1;
		}
	}
	if (!settings_path_valid(sort)) {
		snprintf(why, why_size, "sort is a slash path");
		return -1;
	}
	return 0;
}

struct select_sort {
	fy_generic row;
	fy_generic key;
	bool descending;
};

static int select_sort_cmp(const void *a, const void *b)
{
	const struct select_sort *sa = a, *sb = b;
	int c;

	c = select_cmp(sa->key, sb->key);
	/* A missing value stays last in either order. */
	if (sa->descending && fy_is_valid(sa->key) && !fy_is_null(sa->key) &&
	    fy_is_valid(sb->key) && !fy_is_null(sb->key))
		c = -c;
	return c;
}

/*
 * Filter, sort and project @rows. @fields names the columns of each row
 * given; without it, @dflt_fields does. Builds the answer in @w->gb.
 */
static fy_generic settings_select(struct settings_walk *w, fy_generic rows,
				  fy_generic args, fy_generic dflt_fields)
{
	fy_generic where, fields, row, cond, out, f, proj;
	struct select_sort *kept;
	const char *sort, *field;
	char why[256];
	size_t n, i, max;
	long long limit;

	where = fy_get(args, "where", fy_invalid);
	fields = fy_get(args, "fields", fy_invalid);
	sort = fy_gb_intern_string(w->gb, fy_get(args, "sort", ""));
	if (!sort ||
	    select_check(where, fields, sort, why, sizeof(why)))
		return fy_value(w->gb, fy_sprintfa("settings: %s",
				sort ? why : "the sort could not be kept"));
	if (!fy_is_sequence(fields) || fy_empty(fields))
		fields = dflt_fields;
	limit = fy_get(args, "limit", 0LL);
	max = limit > 0 && limit <= SETTINGS_SELECT_MAX ? (size_t)limit :
	      SETTINGS_SELECT_LIMIT;
	kept = calloc(fy_len(rows) + 1, sizeof(*kept));
	if (!kept)
		return fy_value(w->gb, "settings: the rows could not be kept");
	n = 0;
	fy_foreach(row, rows) {
		fy_foreach(cond, where)
			if (!select_meets(w->gb, row, cond))
				goto next;
		kept[n++].row = row;
next:
		;
	}
	if (*sort) {
		for (i = 0; i < n; i++) {
			kept[i].key = select_field(w->gb, kept[i].row, sort);
			kept[i].descending = fy_equal(fy_get(args, "order",
						       fy_invalid), "desc");
		}
		qsort(kept, n, sizeof(*kept), select_sort_cmp);
	}
	out = fy_sequence(w->gb);
	for (i = 0; i < n && i < max; i++) {
		proj = fy_mapping(w->gb);
		fy_foreach(f, fields) {
			field = fy_castp(&f, "");
			proj = fy_assoc(w->gb, proj, f, fyai_generic_or_null(
				select_field(w->gb, kept[i].row, field)));
		}
		out = fy_append(w->gb, out, proj);
	}
	free(kept);
	return fy_mapping(w->gb, "rows", out, "matched", (long long)n,
			  "fields", fields);
}

/* The configuration as rows: each key under @path with every column. */
static fy_generic settings_config_rows(struct settings_walk *w,
				       const char *path, fy_generic node)
{
	w->keys = fy_sequence(w->gb);
	w->count = 0;
	w->max = SETTINGS_WALK_MAX;
	w->full = true;
	w->changed = false;
	settings_list_walk(w, path, node, 0);
	return w->keys;
}

/* The item @seg of a list by name or id, or of a mapping by key. */
static fy_generic catalog_child(fy_generic node, const char *seg)
{
	fy_generic item;

	if (fy_is_mapping(node))
		return fy_get(node, seg, fy_invalid);
	fy_foreach(item, node)
		if (fy_equal(fy_get(item, "name", fy_invalid), seg) ||
		    fy_equal(fy_get(item, "canonical_id", fy_invalid), seg))
			return item;
	/* An offering is also named by the id its provider gives it. */
	fy_foreach(item, node)
		if (fy_equal(fy_get(item, "provider_model_id", fy_invalid),
			     seg))
			return item;
	return fy_invalid;
}

/* The column that a segment `*` after @seg names: `providers` names a
 * `provider`. */
static const char *catalog_column(struct fy_generic_builder *gb,
				  const char *seg)
{
	size_t len = strlen(seg);

	if (len > 1 && seg[len - 1] == 's')
		return fy_gb_intern_string(gb, fy_sprintfa("%.*s",
					   (int)(len - 1), seg));
	return seg;
}

/*
 * The rows of the catalogue at @segs from @i: the items of the list or the
 * mapping that the path ends at, each with the columns that a `*` bound.
 */
static fy_generic catalog_rows(struct settings_walk *w, fy_generic node,
			       char **segs, int n, int i, fy_generic bound,
			       fy_generic out)
{
	fy_generic item, key, row, name;
	const char *col;

	if (fy_len(out) >= SETTINGS_WALK_MAX || !fy_is_valid(node))
		return out;
	if (i == n) {
		if (fy_is_sequence(node)) {
			fy_foreach(item, node) {
				row = fy_is_mapping(item) ? item :
				      fy_mapping(w->gb, "value", item);
				fy_foreach_key_value(key, name, bound)
					row = fy_assoc(w->gb, row, key, name);
				out = fy_append(w->gb, out, row);
			}
		} else if (fy_is_mapping(node)) {
			fy_foreach_key_value(key, item, node) {
				row = fy_is_mapping(item) ? item :
				      fy_mapping(w->gb, "value", item);
				row = fy_assoc(w->gb, row, "key", key);
				fy_foreach_key_value(name, key, bound)
					row = fy_assoc(w->gb, row, name, key);
				out = fy_append(w->gb, out, row);
			}
		}
		return out;
	}
	if (strcmp(segs[i], "*"))
		return catalog_rows(w, catalog_child(node, segs[i]), segs, n,
				    i + 1, bound, out);
	col = catalog_column(w->gb, i ? segs[i - 1] : "item");
	if (fy_is_sequence(node)) {
		fy_foreach(item, node) {
			name = fy_get(item, "name", fy_invalid);
			if (!fy_is_string(name))
				name = fy_get(item, "canonical_id", fy_null);
			out = catalog_rows(w, item, segs, n, i + 1,
				fy_assoc(w->gb, bound, col, name), out);
		}
	} else if (fy_is_mapping(node)) {
		fy_foreach_key_value(key, item, node)
			out = catalog_rows(w, item, segs, n, i + 1,
				fy_assoc(w->gb, bound, col, key), out);
	}
	return out;
}

/* The fields a row of the catalogue shows when none are asked for. */
static fy_generic catalog_dflt_fields(struct settings_walk *w,
				      fy_generic args, char **segs, int n)
{
	fy_generic out, cond;
	const char *sort = fy_get(args, "sort", "");
	int i;

	out = fy_sequence(w->gb);
	for (i = 0; i < n; i++)
		if (!strcmp(segs[i], "*"))
			out = fy_append(w->gb, out, catalog_column(w->gb,
				i ? segs[i - 1] : "item"));
	out = fy_append(w->gb, out, n && !strcmp(segs[n - 1], "models") ?
			"canonical_id" : "name");
	fy_foreach(cond, fy_get(args, "where", fy_invalid))
		out = fy_append(w->gb, out, fy_get(cond, "field", fy_invalid));
	if (*sort)
		out = fy_append(w->gb, out, sort);
	return out;
}

static fy_generic settings_catalog_select(struct settings_walk *w,
					  fy_generic cat, fy_generic args)
{
	char *segs[32], *copy, *seg, *save;
	const char *from = fy_get(args, "from", "");
	fy_generic rows, result;
	int n = 0;

	if (!*from || !settings_path_valid(from))
		return fy_value(w->gb, "settings: select needs from, a path "
				"such as providers/*/models");
	copy = strdup(from);
	if (!copy)
		return fy_value(w->gb, "settings: the path could not be kept");
	for (seg = strtok_r(copy, "/", &save); seg && n < 32;
	     seg = strtok_r(NULL, "/", &save)) {
		/* "*" is the fan-out; any other segment is decoded. */
		if (strcmp(seg, "*") &&
		    fyai_path_segment_decode(seg, strlen(seg), seg,
					     strlen(seg) + 1) < 0) {
			free(copy);
			return fy_value(w->gb, "settings: a '~' in from must be "
					"~0 or ~1");
		}
		segs[n++] = seg;
	}
	rows = catalog_rows(w, cat, segs, n, 0, fy_mapping(w->gb),
			    fy_sequence(w->gb));
	result = settings_select(w, rows, args,
				 catalog_dflt_fields(w, args, segs, n));
	free(copy);
	if (!fy_is_mapping(result))
		return result;
	return fy_assoc(w->gb, fy_assoc(w->gb, result, "document", "catalog"),
			"from", from);
}

/*
 * export: write the document, as the model may read it, to a file that a
 * program can search. fyai names the file, in a directory of its own under
 * $TMPDIR that it removes at cleanup; the model names no path. The
 * configuration leaves its hidden keys out.
 */

/* The names of the files that an export writes. */
static const char *const settings_export_files[] = {
	"config.json", "config.yaml", "config-rows.json", "config-rows.yaml",
	"catalog.json", "catalog.yaml",
};

static const char *settings_export_dir(struct fyai_ctx *ctx)
{
	const char *tmp, *made;
	char tmpl[PATH_MAX];
	int rc;

	if (ctx->settings_export_dir)
		return ctx->settings_export_dir;
	tmp = getenv("TMPDIR");
	if (fy_str_empty(tmp))
		tmp = "/tmp";
	rc = snprintf(tmpl, sizeof(tmpl), "%s/fyai-settings-XXXXXX", tmp);
	fyai_error_check(ctx, rc > 0 && rc < (int)sizeof(tmpl), err_out,
			 "settings: the temporary directory %s is too long",
			 tmp);
	/* mkdtemp makes the directory 0700: only this user reads the files. */
	made = mkdtemp(tmpl);
	fyai_error_check(ctx, made, err_out,
			 "settings: cannot make a directory in %s: %s", tmp,
			 strerror(errno));
	ctx->settings_export_dir = strdup(tmpl);
	fyai_error_check(ctx, ctx->settings_export_dir, err_out,
			 "settings: cannot keep the name of %s", tmpl);
	return ctx->settings_export_dir;
err_out:
	return NULL;
}

void fyai_settings_release(struct fyai_ctx *ctx)
{
	size_t i;

	if (!ctx || !ctx->settings_export_dir)
		return;
	/* Cleanup cannot act on a failure: the files are temporary. */
	for (i = 0; i < ARRAY_SIZE(settings_export_files); i++)
		(void)unlinkat(AT_FDCWD, fy_sprintfa("%s/%s",
				ctx->settings_export_dir,
				settings_export_files[i]), 0);
	(void)rmdir(ctx->settings_export_dir);
	fyai_settings_abandon(ctx);
}

void fyai_settings_abandon(struct fyai_ctx *ctx)
{
	if (!ctx)
		return;
	free(ctx->settings_export_dir);
	ctx->settings_export_dir = NULL;
}

static int settings_write_file(const char *path, const char *text)
{
	size_t len = strlen(text), done;
	ssize_t n;
	int fd, rc;

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
		  0600);
	if (fd < 0)
		return -1;
	for (done = 0; done < len; done += (size_t)n) {
		n = write(fd, text + done, len - done);
		if (n < 0 && errno == EINTR) {
			n = 0;
			continue;
		}
		if (n <= 0)
			break;
	}
	rc = close(fd);
	return done == len && !rc ? 0 : -1;
}

static fy_generic settings_export(struct settings_walk *w, bool catalog,
				  fy_generic args)
{
	const char *format = fy_get(args, "format", "json");
	const char *shape = fy_get(args, "shape", "tree");
	const char *dir, *text, *path;
	fy_generic doc, emitted, result;

	if (strcmp(format, "json") && strcmp(format, "yaml"))
		return fy_value(w->gb, "settings: format must be json or "
				"yaml");
	if (strcmp(shape, "tree") && strcmp(shape, "rows"))
		return fy_value(w->gb, "settings: shape must be tree or rows");
	if (catalog && !strcmp(shape, "rows"))
		return fy_value(w->gb, "settings: the catalogue is a tree; "
				"select gives rows of it");
	if (catalog)
		doc = fyai_catalog_effective(w->ctx->arena_catalog, NULL);
	else if (!strcmp(shape, "rows"))
		doc = settings_config_rows(w, "", fyai_config_schema(NULL));
	else
		doc = settings_redact(w, "", w->ctx->cfg->config_doc, 0);
	if (!fy_is_valid(doc))
		return fy_value(w->gb, "settings: the document cannot be read");
	if (!strcmp(format, "json")) {
		text = emit_json_string(w->gb, doc);
	} else {
		emitted = fy_emit(w->gb, doc, FYAI_YAML_EMIT_FLAGS, NULL);
		text = fy_is_string(emitted) ?
		       fy_gb_intern_string(w->gb, fy_castp(&emitted, "")) :
		       NULL;
	}
	if (!text)
		return fy_value(w->gb, "settings: the document could not be "
				"written");
	dir = settings_export_dir(w->ctx);
	if (!dir)
		return fy_value(w->gb, "settings: there is no directory to "
				"write to");
	path = fy_gb_intern_string(w->gb, fy_sprintfa("%s/%s%s.%s", dir,
		catalog ? "catalog" : "config",
		!strcmp(shape, "rows") ? "-rows" : "", format));
	if (!path || settings_write_file(path, text))
		return fy_value(w->gb, fy_sprintfa("settings: cannot write %s: "
				"%s", path ? path : dir, strerror(errno)));
	result = fy_mapping(w->gb, "document", catalog ? "catalog" : "config",
			    "path", path, "format", format, "shape", shape,
			    "bytes", (long long)strlen(text));
	if (!fy_empty(w->hidden))
		result = fy_assoc(w->gb, result, "hidden", w->hidden);
	return result;
}

static fy_generic settings_run_generic(struct fyai_ctx *ctx, fy_generic args,
				       bool *okp)
{
	struct settings_walk w;
	fy_generic action, result, node, value;
	const char *path, *layer;

	memset(&w, 0, sizeof(w));
	w.ctx = ctx;
	w.gb = fyai_ctx_transient_gb(ctx);
	w.hidden = fy_sequence(w.gb);
	w.keys = fy_sequence(w.gb);
	*okp = false;

	if (ctx->cfg->model_access < 0)
		return fy_value(w.gb, "settings: model_access is none");
	if (fyai_agent_delegated(ctx))
		return fy_value(w.gb, "settings: only the session of the user "
				"has the settings tool");
	action = fy_get(args, "action", fy_invalid);
	path = fy_gb_intern_string(w.gb, fy_get(args, "path", ""));
	if (!path)
		return fy_value(w.gb, "settings: the path could not be kept");
	if (!settings_path_valid(path))
		return fy_value(w.gb, fy_sprintfa("settings: '%s' is not a "
				"slash path of the configuration", path));
	if (*path && !fy_equal(fy_get(args, "document", fy_invalid),
			       "catalog") &&
	    fyai_config_model_access(path) == FYAI_MODEL_HIDDEN)
		return fy_value(w.gb, fy_sprintfa("settings: %s is hidden "
				"from the model", path));
	if (fy_equal(fy_get(args, "document", fy_invalid), "catalog")) {
		result = settings_catalog_run(&w, action, path, args, okp);
		if (!fy_is_mapping(result))
			return result;
		if (!fy_equal(action, "change"))
			*okp = true;
		else
			result = fy_assoc(w.gb, result, "arena_protected",
					  settings_arena_protected(ctx));
		return result;
	}
	if (!fy_any_equal(fy_get(args, "document", "config"), "config")) {
		return fy_value(w.gb, "settings: document must be config or "
				"catalog");
	}
	node = *path ? fyai_config_schema_node(path) :
	       fyai_config_schema(NULL);

	if (fy_any_equal(action, "change", "revert")) {
		result = fy_equal(action, "change") ?
			 settings_change_run(&w, args, okp) :
			 settings_revert_run(&w, args, okp);
		if (!fy_is_mapping(result))
			return result;
		result = fy_assoc(w.gb, result, "arena_protected",
				  settings_arena_protected(ctx));
		return result;
	} else if (fy_equal(action, "select")) {
		if (!fy_is_mapping(node))
			return fy_value(w.gb, fy_sprintfa("settings: %s is not "
					"a key of the schema", path));
		result = settings_select(&w, settings_config_rows(&w, path,
					 node), args, fy_sequence(w.gb, "key",
					 "access", "layer", "value"));
		if (!fy_is_mapping(result))
			return result;
		result = fy_assoc(w.gb, result, "path", path);
	} else if (fy_equal(action, "export")) {
		result = settings_export(&w, false, args);
		if (!fy_is_mapping(result))
			return result;
	} else if (fy_equal(action, "proposals")) {
		result = settings_proposals_run(&w);
	} else if (fy_equal(action, "search")) {
		if (!fy_is_mapping(node))
			return fy_value(w.gb, fy_sprintfa("settings: %s is not "
					"a key of the schema", path));
		result = settings_search(&w, path, node,
					 fy_get(args, "query", ""),
					 fy_get(args, "limit", 0LL));
		if (!fy_is_mapping(result))
			return result;
	} else if (fy_equal(action, "describe")) {
		result = settings_describe(&w, path);
	} else if (fy_equal(action, "get")) {
		value = settings_redact(&w, path,
					settings_value(&w, path, &layer), 0);
		result = fy_mapping(w.gb, "path", path,
			"access", fyai_model_access_name(
					fyai_config_model_access(path)),
			"layer", layer,
			"value", fy_is_valid(value) ? value : fy_null);
	} else if (fy_equal(action, "list")) {
		if (!fy_is_mapping(node))
			return fy_value(w.gb, fy_sprintfa("settings: %s is not "
					"a key of the schema", path));
		w.changed = fy_get(args, "changed", false);
		settings_list_walk(&w, path, node, 0);
		result = fy_mapping(w.gb, "path", path, "keys", w.keys);
		if (w.count >= SETTINGS_LIST_MAX)
			result = fy_assoc(w.gb, result, "truncated", true);
	} else {
		return fy_value(w.gb, "settings: action must be search, "
				"describe, get, list, select, export, change, "
				"revert or proposals");
	}
	if (!fy_empty(w.hidden))
		result = fy_assoc(w.gb, result, "hidden", w.hidden);
	result = fy_assoc(w.gb, result, "arena_protected",
			  settings_arena_protected(ctx));
	if (!fy_is_valid(result))
		return fy_value(w.gb,
				"settings: the answer could not be built");
	*okp = true;
	return result;
}

/*
 * The result of the tool is text, as every function result is: a structure
 * goes to the model as its compact JSON, which the formatter reads back for
 * the user.
 */
static fy_generic settings_run(struct fyai_ctx *ctx, fy_generic args,
			       bool *okp)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	fy_generic result;
	const char *text;

	result = settings_run_generic(ctx, args, okp);
	if (!fy_is_mapping(result))
		return result;
	text = emit_json_string(gb, result);
	if (!text) {
		*okp = false;
		return fy_value(gb, "settings: the answer could not be "
				"written");
	}
	return fy_value(gb, text);
}

/* The tool is not offered when model_access is none. */
static fy_generic settings_describe_tool(struct fyai_ctx *ctx,
					 struct fy_generic_builder *gb,
					 fy_generic tool)
{
	(void)gb;
	return ctx->cfg->model_access < 0 ? fy_invalid : tool;
}

/* The bound on the characters of a value or a query in a row. */
#define SETTINGS_SPAN_MAX	60
/* The bound on the keys that a result shows the user. */
#define SETTINGS_ROWS_MAX	8

/*
 * @text as a code span of one row: control bytes and line breaks become
 * blanks, a long text ends in an ellipsis, and the span is fenced with more
 * backticks than any run in it. The model or a configuration wrote it.
 */
static const char *settings_span(struct fy_generic_builder *gb,
				 const char *text)
{
	char buf[SETTINGS_SPAN_MAX * 4 + 8];
	const char *fence;
	size_t i, n, chars;

	for (i = n = chars = 0; text[i] && n < sizeof(buf) - 8; i++) {
		if (((unsigned char)text[i] & 0xc0) != 0x80 &&
		    ++chars > SETTINGS_SPAN_MAX)
			break;
		buf[n++] = (unsigned char)text[i] < 0x20 ||
			   (unsigned char)text[i] == 0x7f ? ' ' : text[i];
	}
	if (text[i]) {
		memcpy(buf + n, "\u2026", strlen("\u2026"));
		n += strlen("\u2026");
	}
	buf[n] = '\0';
	fence = settings_fence(gb, buf);
	if (!fence)
		return "`?`";
	/* A one-backtick span suffices unless the text holds a backtick. */
	if (!strchr(buf, '`'))
		fence = "`";
	return fy_gb_intern_string(gb, fy_sprintfa("%s%s%s%s%s", fence,
		buf[0] == '`' ? " " : "", buf,
		n && buf[n - 1] == '`' ? " " : "", fence));
}

/* A value as a span: a string as it is, any other value as flow YAML. */
static const char *settings_value_span(struct fy_generic_builder *gb,
				       fy_generic v)
{
	fy_generic text;

	if (fy_is_string(v))
		return settings_span(gb, fy_castp(&v, ""));
	text = fy_emit(gb, v, FYOPEF_DISABLE_DIRECTORY | FYOPEF_MODE_YAML_1_2 |
		       FYOPEF_STYLE_ONELINE | FYOPEF_WIDTH_INF |
		       FYOPEF_NO_ENDING_NEWLINE, NULL);
	return settings_span(gb, fy_is_string(text) ? fy_castp(&text, "") :
			     "?");
}

static void settings_head(struct fyai_ctx *ctx, FILE *mf,
			  struct fy_generic_builder *gb, fy_generic args,
			  int preview_lines, struct fyai_md_blocks *blocks)
{
	fy_generic action = fy_get(args, "action", fy_invalid);
	fy_generic changes = fy_get(args, "changes", fy_invalid);
	const char *path = fy_get(args, "path", "");
	const char *query = fy_get(args, "query", "");
	const char *id = fy_get(args, "id", "");
	const char *first;

	(void)ctx;
	(void)preview_lines;
	(void)blocks;
	/* The model wrote the path: one that is not valid is not shown. */
	if (!settings_path_valid(path))
		path = "?";
	if (fy_equal(fy_get(args, "document", fy_invalid), "catalog")) {
		fprintf(mf, "**catalog** %s ", fy_any_equal(action, "search",
			"get", "list", "select", "export",
			"change") ?
			fy_castp(&action, "") : "?");
		if (fy_equal(action, "search"))
			fprintf(mf, "%s", settings_span(gb, query));
		else if (fy_equal(action, "select"))
			fprintf(mf, "%s", settings_span(gb,
				fy_get(args, "from", "")));
		else if (fy_equal(action, "export"))
			fprintf(mf, "%s", fy_get(args, "format", "json"));
		else if (fy_equal(action, "change")) {
			first = fy_get(fy_get_at(changes, 0, fy_invalid),
					"path", "");
			fprintf(mf, "%s", settings_span(gb, first));
			if (fy_len(changes) > 1)
				fprintf(mf, " (+%zu more)", fy_len(changes) - 1);
		} else
			fprintf(mf, "`%s`", *path ? path : "catalog");
	} else if (fy_equal(action, "search")) {
		fprintf(mf, "**settings** search %s", settings_span(gb, query));
		if (*path)
			fprintf(mf, " in `%s`", path);
	} else if (fy_equal(action, "change")) {
		first = fy_get(fy_get_at(changes, 0, fy_invalid), "path", "");
		fprintf(mf, "**settings** change `%s`",
			settings_path_valid(first) && *first ? first : "?");
		if (fy_len(changes) > 1)
			fprintf(mf, " (+%zu more)", fy_len(changes) - 1);
		if (fy_get(args, "trial", false))
			fprintf(mf, " \u00b7 trial");
	} else if (fy_equal(action, "revert")) {
		fprintf(mf, "**settings** revert %s", settings_span(gb, id));
	} else if (fy_equal(action, "proposals")) {
		fprintf(mf, "**settings** proposals");
	} else if (fy_equal(action, "export")) {
		fprintf(mf, "**settings** export %s",
			fy_equal(fy_get(args, "format", fy_invalid), "yaml") ?
			"yaml" : "json");
	} else if (fy_equal(action, "select")) {
		fprintf(mf, "**settings** select `%s`",
			*path ? path : "config");
	} else {
		fprintf(mf, "**settings** %s `%s`",
			fy_any_equal(action, "describe", "get", "list") ?
				fy_castp(&action, "") : "?",
			*path ? path : "config");
	}
	fprintf(mf, "\n\n");
}

/* A row for each key of @rows, at most a bound, with what follows. */
static void settings_format_keys(FILE *mf, struct fy_generic_builder *gb,
				 fy_generic rows, bool values)
{
	fy_generic row;
	size_t n = 0;

	fy_foreach(row, rows) {
		if (n++ == SETTINGS_ROWS_MAX)
			break;
		fprintf(mf, "- %s \u00b7 %s", settings_span(gb,
			fy_get(row, "key", "")), fy_get(row, "access", ""));
		if (values)
			fprintf(mf, " \u00b7 %s", settings_value_span(gb,
				fy_get(row, "value", fy_null)));
		else
			fprintf(mf, " \u2014 %s", settings_span(gb,
				fy_get(row, "summary", "")));
		fprintf(mf, "\n");
	}
	if (fy_len(rows) > SETTINGS_ROWS_MAX)
		fprintf(mf, "- _%zu more_\n", fy_len(rows) - SETTINGS_ROWS_MAX);
	if (!n)
		fprintf(mf, "_No key._\n");
}

/* The rows of a select: each field of a row as a span. */
static void settings_format_select(FILE *mf, struct fy_generic_builder *gb,
				   fy_generic data)
{
	fy_generic row, key, v;
	size_t n = 0;
	bool first;

	fprintf(mf, "%lld matched\n\n", fy_get(data, "matched", 0LL));
	fy_foreach(row, fy_get(data, "rows", fy_invalid)) {
		if (n++ == SETTINGS_ROWS_MAX)
			break;
		fprintf(mf, "-");
		first = true;
		fy_foreach_key_value(key, v, row) {
			fprintf(mf, "%s %s", first ? "" : " \u00b7",
				settings_value_span(gb, v));
			first = false;
		}
		fprintf(mf, "\n");
	}
	if (fy_len(fy_get(data, "rows", fy_seq_empty)) > SETTINGS_ROWS_MAX)
		fprintf(mf, "- _%zu more_\n",
			fy_len(fy_get(data, "rows", fy_seq_empty)) -
			SETTINGS_ROWS_MAX);
}

/* The file an export wrote. */
static void settings_format_export(FILE *mf, struct fy_generic_builder *gb,
				   fy_generic data)
{
	fprintf(mf, "Wrote %s, %lld bytes\n", settings_span(gb,
		fy_get(data, "path", "")), fy_get(data, "bytes", 0LL));
}

/* The rows of a catalogue call for the user. */
static void settings_format_catalog(FILE *mf, struct fy_generic_builder *gb,
				    fy_generic action, fy_generic data)
{
	fy_generic row, v;
	const char *diff, *block;
	size_t n = 0;

	if (fy_equal(action, "change")) {
		const char *status = fy_get(data, "status", "");
		const char *id = fy_get(data, "id", "");

		fprintf(mf, "**%s** catalogue change from %s\n\n",
			!strcmp(status, "pending") ? "Pending" :
			!strcmp(status, "applied") ? "Applied" : "Rejected",
			settings_span(gb, fy_get(data, "source", "")));
		if (*id)
			fprintf(mf, "Id: %s\n\n", settings_span(gb, id));
		diff = fy_get(data, "diff", "");
		block = *diff ? settings_diff_block(gb, diff) : NULL;
		if (block)
			fprintf(mf, "%s\n", block);
		if (!strcmp(status, "pending"))
			fprintf(mf, "Review with `config review %s`; apply with "
				"`config approve %s`.\n", id, id);
		return;
	}

	if (fy_equal(action, "select")) {
		settings_format_select(mf, gb, data);
		return;
	}
	if (fy_equal(action, "export")) {
		settings_format_export(mf, gb, data);
		return;
	}
	if (fy_equal(action, "search")) {
		fprintf(mf, "%lld found\n\n", fy_get(data, "found", 0LL));
		fy_foreach(row, fy_get(data, "models", fy_invalid)) {
			if (n++ == SETTINGS_ROWS_MAX)
				break;
			fprintf(mf, "- %s \u00b7 %s \u00b7 %lld tokens "
				"\u00b7 %s%s\n",
				settings_span(gb, fy_get(row, "path", "")),
				settings_span(gb, fy_get(row, "display_name",
							 "")),
				fy_get(row, "context_window", 0LL),
				settings_value_span(gb, fy_get(row, "providers",
							       fy_seq_empty)),
				fy_get(row, "current", false) ?
					" \u00b7 **in use**" : "");
		}
		if (fy_len(fy_get(data, "models", fy_seq_empty)) >
		    SETTINGS_ROWS_MAX)
			fprintf(mf, "- _%zu more_\n",
				fy_len(fy_get(data, "models", fy_seq_empty)) -
				SETTINGS_ROWS_MAX);
		return;
	}
	if (fy_equal(action, "list")) {
		v = fy_get(data, "names", fy_seq_empty);
		fprintf(mf, "%zu \u00b7 %s\n", fy_len(v),
			settings_value_span(gb, v));
		return;
	}
	fprintf(mf, "%s \u00b7 %s\n",
		settings_span(gb, fy_get(data, "path", "")),
		settings_value_span(gb, fy_get(data, "value", fy_null)));
}

/*
 * The result of a call for the user: short rows for the reads, and for a
 * change the outcome and the whole diff that fyai made, so no change is
 * silent. The model gets the result as it is.
 */
static char *settings_format(struct fyai_ctx *ctx,
			     struct fy_generic_builder *gb, fy_generic args,
			     const char *result)
{
	fy_generic data, action, v;
	const char *status, *diff, *block;
	char *out = NULL;
	size_t len = 0;
	FILE *mf;

	(void)ctx;
	data = parse_json_string(gb, result);
	if (!fy_is_mapping(data))
		return NULL;
	action = fy_get(args, "action", fy_invalid);
	mf = open_memstream(&out, &len);
	if (!mf)
		return NULL;
	if (fy_equal(fy_get(data, "document", fy_invalid), "catalog")) {
		settings_format_catalog(mf, gb, action, data);
	} else if (fy_equal(action, "select")) {
		settings_format_select(mf, gb, data);
	} else if (fy_equal(action, "export")) {
		settings_format_export(mf, gb, data);
	} else if (fy_equal(action, "search")) {
		fprintf(mf, "%lld found\n\n", fy_get(data, "found", 0LL));
		settings_format_keys(mf, gb, fy_get(data, "keys", fy_invalid),
				     false);
	} else if (fy_equal(action, "list")) {
		settings_format_keys(mf, gb, fy_get(data, "keys", fy_invalid),
				     true);
	} else if (fy_any_equal(action, "describe", "get")) {
		fprintf(mf, "%s \u00b7 %s", settings_span(gb,
			fy_get(data, "path", "")), fy_get(data, "access", ""));
		if (fy_is_string(fy_get(data, "scope", fy_invalid)))
			fprintf(mf, " \u00b7 %s", fy_get(data, "scope", ""));
		v = fy_get(data, "value", fy_invalid);
		if (fy_is_valid(v))
			fprintf(mf, " \u00b7 %s (%s)",
				settings_value_span(gb, v),
				fy_get(data, "layer", "?"));
		fprintf(mf, "\n");
	} else if (fy_any_equal(action, "change", "revert")) {
		status = fy_get(data, "status", "");
		if (!strcmp(status, "applied"))
			fprintf(mf, "**%s** as change %s\n",
				fy_equal(action, "revert") ? "Taken back" :
				!strcmp(fy_get(data, "scope", ""), "session") ?
				"Applied for this session" : "Applied",
				settings_span(gb, fy_get(data, "id", "")));
		else if (!strcmp(status, "pending"))
			fprintf(mf, "**Waits** as proposal %s for `config "
				"approve`\n", settings_span(gb,
						fy_get(data, "id", "")));
		else
			fprintf(mf, "**Rejected**\n");
		if (fy_is_string(fy_get(data, "reply", fy_invalid)))
			fprintf(mf, "\nThe user replied %s\n", settings_span(gb,
				fy_get(data, "reply", "")));
		diff = fy_get(data, "diff", "");
		block = *diff ? settings_diff_block(gb, diff) : NULL;
		if (block)
			fprintf(mf, "\n%s", block);
	} else if (fy_equal(action, "proposals")) {
		fy_foreach(v, fy_get(data, "proposals", fy_invalid))
			fprintf(mf, "- %s \u00b7 %s\n", settings_span(gb,
				fy_get(v, "id", "")), fy_get(v, "status", ""));
		if (fy_empty(fy_get(data, "proposals", fy_seq_empty)))
			fprintf(mf, "_No proposal._\n");
	} else {
		fclose(mf);
		free(out);
		return NULL;
	}
	if (fclose(mf)) {
		free(out);
		return NULL;
	}
	return out;
}

const struct fyai_tool_def fyai_settings_defs[] = {
	{ .name = "settings", .run = settings_run, .head = settings_head,
	  .format_result = settings_format,
	  .describe = settings_describe_tool,
	  .flags = FYAI_TOOL_PARENT | FYAI_TOOL_ROOT_ONLY |
		   FYAI_TOOL_WHOLE_RESULT },
};
const size_t fyai_settings_defs_count = ARRAY_SIZE(fyai_settings_defs);
