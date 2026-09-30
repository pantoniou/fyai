/*
 * fyai_cmd_config.c - handlers of the config command
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai.h"
#include "fyai_cmd.h"
#include "fyai_cmd_int.h"
#include "fyai_config.h"
#include "fyai_transport_boot.h"
#include "utils.h"

#define FYAI_MODULE FYAIEM_CONFIG

static int config_need_arena(struct fyai_cmd_call *call)
{
	fyai_error_check(call->ctx, fy_is_valid(call->ctx->arena_config), err,
			 "no config in arena; run fyai init or fyai config "
			 "import");
	return 0;
err:
	return -1;
}

/*
 * A change reaches a live session on the next prompt, not after a restart,
 * except the isolation level: the transport starts and ends with the process,
 * so a change of it restarts the session. A restart that cannot run leaves
 * the stored value for the next one.
 */
static int config_changed(struct fyai_cmd_call *call)
{
	struct fyai_ctx *ctx = call->ctx;
	fy_generic none;
	const char *why = NULL;
	int rc;

	if (call->surface != FYAI_CMD_SESSION)
		return 0;
	if (fyai_config_rederive(ctx))
		return -1;
	rc = fyai_transport_config_changed(ctx, &why);
	if (rc < 0) {
		fyai_warning(ctx, "agent/transport_isolation: %s", why);
		return 0;
	}
	if (rc > 0 && fyai_cmd_reload(call, &none))
		fyai_warning(ctx, "agent/transport_isolation is stored; it "
			     "applies after the next restart");
	return 0;
}

int fyai_cmd_config_show(struct fyai_cmd_call *call, fy_generic *result)
{
	if (config_need_arena(call))
		return -1;
	*result = call->ctx->arena_config;
	return 0;
}

int fyai_cmd_config_effective(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_cfg *cfg = call->ctx->cfg;

	fyai_error_check(call->ctx, fy_is_valid(cfg->config_doc), err,
			 "no configuration; run fyai init or fyai config "
			 "import");
	*result = cfg->config_doc;
	return 0;
err:
	return -1;
}

int fyai_cmd_config_get(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *key = fyai_cmd_arg_str(call, "key");
	fy_generic v;

	if (config_need_arena(call))
		return -1;
	v = fy_get_at_pathstr(call->gb, call->ctx->arena_config, key);
	fyai_error_check(call->ctx, fy_is_valid(v), err, "key '%s' not set",
			 key);
	*result = v;
	return 0;
err:
	return -1;
}

int fyai_cmd_config_set(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *key = fyai_cmd_arg_str(call, "key");
	const char *value = fyai_cmd_arg_str(call, "value");

	if (fyai_config_set(call->ctx, key, value) || config_changed(call))
		return -1;
	*result = fy_mapping(call->gb, "key", key, "value", value);
	return 0;
}

int fyai_cmd_config_delete(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *key = fyai_cmd_arg_str(call, "key");

	if (fyai_config_delete(call->ctx, key) || config_changed(call))
		return -1;
	*result = fy_mapping(call->gb, "key", key);
	return 0;
}

int fyai_cmd_config_import(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *file = fyai_cmd_arg_str(call, "file");

	if (fyai_config_import(call->ctx, file) || config_changed(call))
		return -1;
	*result = fy_mapping(call->gb, "file", file);
	return 0;
}

int fyai_cmd_config_export(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *file = fyai_cmd_arg_str(call, "file");

	if (!file)
		return fyai_cmd_config_show(call, result);
	/* Written to a file: nothing to present. */
	return fyai_config_export(call->ctx, file);
}

int fyai_cmd_config_validate(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = call->ctx;

	if (fyai_config_validate_schema(ctx->cfg, ctx->arena_config, "config"))
		return -1;
	*result = fy_mapping(call->gb, "valid", true);
	return 0;
}

int fyai_cmd_config_schema(struct fyai_cmd_call *call, fy_generic *result)
{
	*result = fyai_config_schema(call->gb);
	fyai_error_check(call->ctx, fy_is_valid(*result), err,
			 "the configuration schema does not load");
	return 0;
err:
	return -1;
}

/*
 * The CLI waits for the editor. A session runs it in a tile of the work pane;
 * the interactive loop collects the edit and applies it between turns.
 */
int fyai_cmd_config_edit(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = call->ctx;

	(void)result;
	if (call->surface == FYAI_CMD_CLI)
		return fyai_config_edit(ctx);
	fyai_error_check(ctx, !ctx->config_edit, err,
			 "configuration editor is already active");
	ctx->config_edit = fyai_config_edit_submit(ctx);
	return ctx->config_edit ? 0 : -1;
err:
	return -1;
}

/*
 * A schema `default` (system_prompt's, notably) can be arbitrarily long in
 * a live config; cap every cell so one oversized value can't blow up the
 * table.
 */
#define DESCRIBE_CELL_MAX 160

/* @node's `default`, stringified into @buf; empty when absent. */
static void schema_default_str(fy_generic node, char *buf, size_t bufsz)
{
	fy_generic v;

	v = fy_get(node, "default");
	if (fy_is_invalid(v)) {
		buf[0] = '\0';
		return;
	}
	/* fy_str()'s result has stack lifetime bound to this frame - copy it
	 * out with snprintf before returning, never cache the pointer. */
	snprintf(buf, bufsz, "%s", fy_str(v) ?: "");
}

static void schema_type_str(fy_generic node, char *buf, size_t bufsz)
{
	fy_generic type, v, branches;
	char sub[64];
	size_t off;
	int n;

	type = fy_get(node, "type");
	if (fy_is_string(type)) {
		snprintf(buf, bufsz, "%s", fy_castp(&type, ""));
		return;
	}
	off = 0;
	if (fy_is_sequence(type)) {
		fy_foreach(v, type) {
			n = snprintf(buf + off, off < bufsz ? bufsz - off : 0,
				    "%s%s", off ? "|" : "", fy_castp(&v, ""));
			if (n > 0)
				off += (size_t)n;
		}
		if (off)
			return;
	}
	branches = fy_get(node, "oneOf");
	if (fy_is_invalid(branches))
		branches = fy_get(node, "anyOf");
	if (fy_is_sequence(branches)) {
		off = 0;
		fy_foreach(v, branches) {
			schema_type_str(v, sub, sizeof(sub));
			n = snprintf(buf + off, off < bufsz ? bufsz - off : 0,
				    "%s%s", off ? " or " : "", sub);
			if (n > 0)
				off += (size_t)n;
		}
		if (off)
			return;
	}
	snprintf(buf, bufsz, "any");
}

static double schema_num(fy_generic v)
{
	if (fy_generic_is_int(v))
		return (double)fy_cast(v, 0LL);
	if (fy_generic_is_float(v))
		return fy_cast(v, 0.0);
	return 0.0;
}

static void schema_constraint_str(fy_generic node, bool required, char *buf,
				  size_t bufsz)
{
	fy_generic v, sub;
	char items[192], itype[64];
	size_t off, ioff;
	bool first, ifirst;
	int n;

	off = 0;
	first = true;

#define ADD(...) do { \
	n = snprintf(buf + off, off < bufsz ? bufsz - off : 0, "%s", \
		    first ? "" : "; "); \
	if (n > 0) \
		off += (size_t)n; \
	n = snprintf(buf + off, off < bufsz ? bufsz - off : 0, __VA_ARGS__); \
	if (n > 0) \
		off += (size_t)n; \
	first = false; \
} while (0)

	if (required)
		ADD("required");

	v = fy_get(node, "const");
	if (fy_is_valid(v))
		ADD("const: %s", fy_castp(&v, ""));

	v = fy_get(node, "enum");
	if (fy_is_sequence(v)) {
		ioff = 0;
		ifirst = true;
		fy_foreach(sub, v) {
			n = snprintf(items + ioff,
				    ioff < sizeof(items) ? sizeof(items) - ioff : 0,
				    "%s%s", ifirst ? "" : ", ", fy_castp(&sub, ""));
			if (n > 0)
				ioff += (size_t)n;
			ifirst = false;
		}
		ADD("one of: %s", items);
	}

	v = fy_get(node, "minimum");
	if (fy_is_valid(v))
		ADD(">= %g", schema_num(v));
	v = fy_get(node, "maximum");
	if (fy_is_valid(v))
		ADD("<= %g", schema_num(v));
	v = fy_get(node, "exclusiveMinimum");
	if (fy_is_valid(v))
		ADD("> %g", schema_num(v));
	v = fy_get(node, "exclusiveMaximum");
	if (fy_is_valid(v))
		ADD("< %g", schema_num(v));

	v = fy_get(node, "minLength");
	if (fy_is_valid(v))
		ADD("minLength %g", schema_num(v));
	v = fy_get(node, "maxLength");
	if (fy_is_valid(v))
		ADD("maxLength %g", schema_num(v));

	v = fy_get(node, "pattern");
	if (fy_is_string(v))
		ADD("pattern: `%s`", fy_castp(&v, ""));

	v = fy_get(node, "minItems");
	if (fy_is_valid(v))
		ADD("minItems %g", schema_num(v));
	v = fy_get(node, "maxItems");
	if (fy_is_valid(v))
		ADD("maxItems %g", schema_num(v));

	v = fy_get(node, "items");
	if (fy_is_mapping(v)) {
		schema_type_str(v, itype, sizeof(itype));
		ADD("items: %s", itype);
	}

#undef ADD
	if (first)
		snprintf(buf, bufsz, "-");
}

/* Descend into `properties`, transparently trying oneOf/anyOf branches. */
static fy_generic schema_child(fy_generic node, const char *key)
{
	fy_generic props, v, branches, b;

	props = fy_get(node, "properties");
	if (fy_is_mapping(props)) {
		v = fy_get(props, key);
		if (fy_is_valid(v))
			return v;
	}
	branches = fy_get(node, "oneOf");
	if (fy_is_invalid(branches))
		branches = fy_get(node, "anyOf");
	fy_foreach(b, branches) {
		v = schema_child(b, key);
		if (fy_is_valid(v))
			return v;
	}
	return fy_invalid;
}

/*
 * @node itself if it has `properties` directly; otherwise the first
 * oneOf/anyOf branch that does (recursively) - so describing a node like
 * `sandbox` (oneOf [boolean, object-with-properties]) still lists the
 * object branch's properties instead of falling back to a single leaf row.
 */
static fy_generic schema_object_node(fy_generic node)
{
	fy_generic props, branches, b, sub;

	props = fy_get(node, "properties");
	if (fy_is_mapping(props))
		return node;
	branches = fy_get(node, "oneOf");
	if (fy_is_invalid(branches))
		branches = fy_get(node, "anyOf");
	fy_foreach(b, branches) {
		sub = schema_object_node(b);
		if (fy_is_valid(sub))
			return sub;
	}
	return fy_invalid;
}

static bool schema_key_required(fy_generic node, const char *key)
{
	fy_generic req, v;

	req = fy_get(node, "required");
	if (!fy_is_sequence(req))
		return false;
	fy_foreach(v, req)
		if (!strcmp(fy_castp(&v, ""), key))
			return true;
	return false;
}

/* One row of the describe table. */
static fy_generic describe_row(struct fy_generic_builder *gb, const char *name,
			       fy_generic node, bool required, fy_generic desc)
{
	char type_buf[128], cons_buf[384];
	char def_buf[DESCRIBE_CELL_MAX + 8];

	schema_type_str(node, type_buf, sizeof(type_buf));
	schema_constraint_str(node, required, cons_buf, sizeof(cons_buf));
	schema_default_str(node, def_buf, sizeof(def_buf));
	return fy_mapping(gb,
		"name", name,
		"type", type_buf,
		"constraint", *cons_buf ? cons_buf : "-",
		"default", *def_buf ? def_buf : "-",
		"description", fy_is_string(desc) ?
			fy_castp(&desc, "") : "");
}

/*
 * `config describe [path]`: the rows (name, type, constraint, default,
 * description) of a config.schema.yaml node, resolved by a slash path into
 * `properties`, oneOf and anyOf branches included ("sandbox/enabled"). An
 * object node gives one row for each property; a leaf gives one row for
 * itself. No path describes the whole document.
 */
int fyai_cmd_config_describe(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = call->ctx;
	struct fy_generic_builder *gb = call->gb;
	fy_generic schema, node, object_node, props, desc, key, sub;
	const char *path = fyai_cmd_arg_str(call, "path");
	const char *p, *slash, *last;
	char seg[256];
	size_t seglen;
	fy_generic rows;

	schema = fyai_config_schema(gb);
	node = schema;
	last = "config";

	if (path && *path) {
		p = path;
		while (*p) {
			slash = strchr(p, '/');
			seglen = slash ? (size_t)(slash - p) : strlen(p);
			if (seglen >= sizeof(seg))
				seglen = sizeof(seg) - 1;
			memcpy(seg, p, seglen);
			seg[seglen] = '\0';
			node = schema_child(node, seg);
			fyai_error_check(ctx, fy_is_valid(node), err,
					 "config describe: unknown property "
					 "'%s'", path);
			last = seg;
			p = slash ? slash + 1 : p + seglen;
		}
	}

	desc = fy_get(node, "description");
	rows = fy_seq_empty;

	/* An object branch of oneOf/anyOf (the object form of sandbox) lists
	 * its properties rather than describing the node as a leaf. */
	object_node = schema_object_node(node);
	props = fy_is_valid(object_node) ?
		fy_get(object_node, "properties") : fy_invalid;
	if (!fy_is_mapping(props)) {
		rows = fy_append(gb, rows,
				 describe_row(gb, last, node, false, desc));
	} else {
		fy_foreach_key_value(key, sub, props) {
			rows = fy_append(gb, rows,
				describe_row(gb, fy_castp(&key, ""), sub,
					     schema_key_required(object_node,
						fy_castp(&key, "")),
					     fy_get(sub, "description")));
		}
	}
	fyai_error_check(ctx, fy_is_valid(rows), err,
			 "config describe: cannot build the rows");
	/* The heading names the node, so the table options are the call's. */
	call->renderopts = fy_mapping(gb,
		"preamble", fy_sprintfa("## %s%s%s",
			(path && *path) ? path : "config",
			fy_is_string(desc) ? "\n\n" : "",
			fy_is_string(desc) ? fy_castp(&desc, "") : ""),
		"keys", fy_sequence(gb, "name", "type", "constraint",
				    "default", "description"));
	*result = rows;
	return 0;
err:
	return -1;
}

/* `sandbox` names the stored sandbox policy of the configuration. */
int fyai_cmd_sandbox_show(struct fyai_cmd_call *call, fy_generic *result)
{
	fy_generic v;

	if (config_need_arena(call))
		return -1;
	v = fy_get(call->ctx->arena_config, "sandbox", fy_invalid);
	*result = fy_is_valid(v) ? v : fy_false;
	return 0;
}

int fyai_cmd_sandbox_set(struct fyai_cmd_call *call, fy_generic *result)
{
	static const char policy[] =
		"{ enabled: true, deny: [secrets, ~/.ssh], "
		"network: { ports: [443] } }";
	bool on = fy_equal(fy_get(call->def, "command", fy_invalid), "on");

	if (fyai_config_set(call->ctx, "sandbox", on ? policy : "false"))
		return -1;
	*result = fy_mapping(call->gb, "sandbox", on ? "on" : "off");
	return 0;
}

static const char *const setting_on[] = { "on", "true", "yes", NULL };
static const char *const setting_off[] = { "off", "false", "no", NULL };

/* The value of a setting as the user typed it, typed by the schema. */
static int setting_value(struct fyai_cmd_call *call, const char *key,
			 const char *text, fy_generic *valuep)
{
	fy_generic node, type;

	node = fyai_config_schema_node(key);
	type = fy_get(node, "type", fy_invalid);
	if (str_in_set(text, setting_on) >= 0 &&
	    (fy_equal(type, "boolean") || !fy_is_valid(node))) {
		*valuep = fy_true;
		return 0;
	}
	if (str_in_set(text, setting_off) >= 0 &&
	    (fy_equal(type, "boolean") || !fy_is_valid(node))) {
		*valuep = fy_false;
		return 0;
	}
	fyai_error_check(call->ctx, !fy_equal(type, "boolean") &&
			 fy_is_valid(node), err, "%s: invalid value '%s' "
			 "(on|off)", call->path, text);
	/* A number is a YAML scalar; any other value is the text itself. */
	*valuep = fy_equal(type, "number") || fy_equal(type, "integer") ?
		  fy_parse(call->gb, text, FYAI_YAML_PARSE_FLAGS |
			   FYOPPF_INPUT_TYPE_STRING, NULL) :
		  fy_value(call->gb, text);
	fyai_error_check(call->ctx, fy_is_valid(*valuep), err,
			 "%s: invalid value '%s'", call->path, text);
	return 0;
err:
	return -1;
}

/* The value that the session uses now, as the setting shows it. */
static const char *setting_show(struct fyai_cmd_call *call, const char *key)
{
	fy_generic v, parent;

	v = fy_get_at_pathstr(call->gb, call->ctx->cfg->config_doc, key);
	/* `sandbox: false` has no enabled member. Any other parent is a
	 * mapping that does not hold the key. */
	if (!fy_is_valid(v) && strrchr(key, '/')) {
		parent = fy_get_at_pathstr(call->gb, call->ctx->cfg->config_doc,
				fy_sprintfa("%.*s",
					    (int)(strrchr(key, '/') - key), key));
		if (fy_is_bool(parent))
			v = parent;
	}
	/* A key that is not set has the default of the schema. */
	if (!fy_is_valid(v))
		v = fy_get(fyai_config_schema_node(key), "default", fy_invalid);
	if (fy_is_bool(v))
		return fy_equal(v, true) ? "on" : "off";
	if (!fy_is_valid(v) || fy_is_null(v) ||
	    (fy_is_string(v) && !*fy_castp(&v, "")))
		return "(unset)";
	return fy_gb_intern_string(call->gb, fy_is_string(v) ?
				   fy_castp(&v, "") : fy_str(v));
}

/*
 * A setting shows or changes one configuration key. The schema scopes the
 * change: a session-scoped key goes into the session layer, any other key is
 * stored.
 */
int fyai_cmd_setting(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = call->ctx;
	fy_generic keyv, name, value, text;
	const char *key, *typed;

	keyv = fy_get(fy_get(call->def, "setting", fy_invalid), "key",
		      fy_invalid);
	key = fy_gb_intern_string(call->gb, fy_castp(&keyv, ""));
	name = fy_get(call->def, "command", fy_invalid);
	typed = fyai_cmd_arg_str(call, "value");
	if (typed) {
		fyai_error_check(ctx, strncmp(key, "reasoning/", 10) ||
				 ctx->cfg->api_mode != FYAI_API_MESSAGES, err,
				 "%s: reasoning options are not supported with "
				 "the Messages API yet", call->path);
		if (setting_value(call, key, typed, &value))
			return -1;
		if (fyai_config_session_scoped(key)) {
			if (fyai_config_session_set(ctx, key, value))
				return -1;
		} else {
			text = fy_emit(call->gb, value,
				       FYOPEF_DISABLE_DIRECTORY |
				       FYOPEF_MODE_YAML_1_2 |
				       FYOPEF_STYLE_ONELINE | FYOPEF_WIDTH_INF |
				       FYOPEF_NO_ENDING_NEWLINE, NULL);
			fyai_error_check(ctx, fy_is_string(text), err,
					 "%s: cannot write the value",
					 call->path);
			if (fyai_config_set(ctx, key, fy_castp(&text, "")) ||
			    fyai_config_rederive(ctx))
				return -1;
		}
	}
	*result = fy_mapping(call->gb, "setting", name,
			     "value", setting_show(call, key));
	return 0;
err:
	return -1;
}
