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

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

#include "fyai.h"
#include "fyai_cmd.h"
#include "fyai_cmd_int.h"
#include "fyai_config.h"
#include "fyai_sandbox_profile.h"
#include "fyai_skill.h"
#include "fyai_sink.h"
#include "fyai_settings.h"
#include "fyai_agents.h"
#include "fyai_event.h"
#include "fyai_transport_ctl.h"
#include "fyai_sandbox.h"
#include "fyai_tools.h"
#include "fyai_ui.h"
#include "fyai_transport_boot.h"
#include "fyai_view.h"
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

int fyai_cmd_config_reset(struct fyai_cmd_call *call, fy_generic *result)
{
	if (fyai_config_reset(call->ctx) || config_changed(call))
		return -1;
	*result = fy_mapping(call->gb, "reset", true);
	return 0;
}

int fyai_cmd_config_undo(struct fyai_cmd_call *call, fy_generic *result)
{
	long long n = fy_get(call->args, "count", 1LL), found;

	if (fyai_config_undo(call->ctx, n, &found) || config_changed(call))
		return -1;
	*result = fy_mapping(call->gb, "count", n);
	return 0;
}

/* The rows of a log of the settings tool, newest first. */
static int config_settings_rows(struct fyai_cmd_call *call, const char *member,
				fy_generic *result)
{
	fy_generic log, item, rows, p;
	struct response_buffer keys = { 0 };
	size_t i, n;
	int rc;

	if (config_need_arena(call))
		return -1;
	log = fyai_settings_log(call->ctx, member);
	rows = fy_sequence(call->gb);
	n = fy_len(log);
	for (i = n; i > 0; i--) {
		item = fy_get_at(log, i - 1, fy_invalid);
		keys.len = 0;
		rc = 0;
		fy_foreach(p, fy_get(item, "paths", fy_invalid))
			rc |= response_buffer_append(&keys, keys.len ?
						     ", " : "") |
			      response_buffer_append(&keys, fy_castp(&p, ""));
		fyai_error_check(call->ctx, !rc, err,
				 "config: cannot list the keys of a change");
		rows = fy_append(call->gb, rows, fy_mapping(call->gb,
			"id", fy_get(item, "name", ""),
			"document", fy_get(item, "document", "config"),
			"status", fy_get(item, "status", ""),
			"access", fy_get(item, "access", ""),
			"keys", fy_value(call->gb, keys.data ? keys.data : ""),
			"origin", fy_get(item, "origin", ""),
			"time", fy_get(item, "time", 0LL)));
	}
	free(keys.data);
	*result = rows;
	return 0;
err:
	free(keys.data);
	return -1;
}

int fyai_cmd_config_changes(struct fyai_cmd_call *call, fy_generic *result)
{
	return config_settings_rows(call, "config_changes", result);
}

int fyai_cmd_config_proposals(struct fyai_cmd_call *call, fy_generic *result)
{
	return config_settings_rows(call, "config_proposals", result);
}

/*
 * The text the model wrote, as a block quote: a heading or a fence in it
 * stays inside the quote, so it cannot pass for the diff above it.
 */
static const char *config_quote(struct fy_generic_builder *gb,
				const char *text)
{
	struct response_buffer out = { 0 };
	const char *line, *nl, *quoted;
	int rc = 0;

	for (line = text; *line && !rc; line = nl ? nl + 1 : line +
							   strlen(line)) {
		nl = strchr(line, '\n');
		rc = response_buffer_append(&out, "> ") ||
		     response_buffer_append_data(&out, line, nl ?
				(size_t)(nl - line) : strlen(line)) ||
		     response_buffer_append(&out, "\n");
	}
	quoted = rc ? NULL : fy_gb_intern_string(gb, out.data ? out.data :
						 "> _no explanation_\n");
	free(out.data);
	return quoted;
}

int fyai_cmd_config_review(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *id = fyai_cmd_arg_str(call, "id");
	const char *quoted;
	fy_generic shown;

	if (config_need_arena(call))
		return -1;
	shown = fyai_settings_proposal_show(call->ctx, call->gb, id);
	if (!fy_is_mapping(shown))
		return -1;
	quoted = config_quote(call->gb, fy_get(shown, "explanation", ""));
	fyai_error_check(call->ctx, quoted, err,
			 "config: cannot quote the explanation of '%s'", id);
	*result = fy_assoc(call->gb, shown, "explanation", quoted);
	return 0;
err:
	return -1;
}

/* The panel answered the review of the proposal of an approve. */
static void config_approve_answered(void *user,
				    const struct fyai_ask_reply *replies,
				    size_t n)
{
	struct fyai_cmd_call *call = user;
	fy_generic result = fy_invalid;
	static const struct fyai_ask_reply none = { 0 };
	int rc;

	/* No answer at all, as from Escape, leaves the proposal pending. */
	rc = fyai_settings_review_apply(call->priv, replies ? replies : &none,
					replies ? n : 1, true, &result);
	fyai_cmd_done(call, rc, result);
}

static void config_approve_cancel(struct fyai_cmd_call *call)
{
	fyai_ui_ask_withdraw(call->ctx, call);
	fyai_cmd_done(call, 0, fy_mapping(call->gb, "id",
		fyai_cmd_arg_str(call, "id"), "status", "pending"));
}

static void config_approve_cleanup(struct fyai_cmd_call *call)
{
	fyai_settings_review_free(call->priv);
	call->priv = NULL;
}

/*
 * A session puts the proposal to the user in the review panel and waits in
 * the loop of the session; a verb applies it, because the user named it.
 */
int fyai_cmd_config_approve(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_settings_review *r;
	int rc;

	if (config_need_arena(call))
		return -1;
	r = fyai_settings_review_open(call->ctx, call->gb,
				      fyai_cmd_arg_str(call, "id"));
	if (!r)
		return -1;
	if (call->surface != FYAI_CMD_SESSION ||
	    !fyai_ui_ask_available(call->ctx)) {
		rc = fyai_settings_review_apply(r, NULL, 0,
				call->surface == FYAI_CMD_SESSION, result);
		fyai_settings_review_free(r);
		return rc;
	}
	call->priv = r;
	call->cleanup = config_approve_cleanup;
	call->cancel = config_approve_cancel;
	rc = fyai_ui_ask(call->ctx, fyai_settings_review_questions(r), NULL,
			 config_approve_answered, call);
	fyai_error_check(call->ctx, !rc, err,
			 "config approve: cannot show the review panel");
	return FYAI_CMD_PENDING;
err:
	return -1;
}

int fyai_cmd_config_reject(struct fyai_cmd_call *call, fy_generic *result)
{
	if (config_need_arena(call))
		return -1;
	return fyai_settings_proposal_decide(call->ctx, call->gb,
					     fyai_cmd_arg_str(call, "id"),
					     false,
					     call->surface == FYAI_CMD_SESSION,
					     result);
}

int fyai_cmd_config_revert(struct fyai_cmd_call *call, fy_generic *result)
{
	if (config_need_arena(call))
		return -1;
	return fyai_settings_revert(call->ctx, call->gb,
				    fyai_cmd_arg_str(call, "id"),
				    fyai_cmd_arg_bool(call, "force"), result);
}

int fyai_cmd_config_keep(struct fyai_cmd_call *call, fy_generic *result)
{
	if (config_need_arena(call))
		return -1;
	return fyai_settings_keep(call->ctx, call->gb,
				  fyai_cmd_arg_str(call, "id"), result);
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
			       const char *path, fy_generic node, bool required,
			       fy_generic desc)
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
		"model", fyai_model_access_name(fyai_config_model_access(path)),
		"description", fy_is_string(desc) ?
			fy_castp(&desc, "") : "");
}

/*
 * `config describe [path]`: the rows (name, type, constraint, default,
 * description) of a config.schema.yaml node, resolved by a slash path into
 * `properties`, oneOf and anyOf branches included ("mcp/enabled"). An
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
			if (fyai_path_segment_decode(p, seglen, seg,
						     sizeof(seg)) < 0)
				seg[0] = '\0';
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
				 describe_row(gb, last, path ? path : "", node,
					      false, desc));
	} else {
		fy_foreach_key_value(key, sub, props) {
			rows = fy_append(gb, rows,
				describe_row(gb, fy_castp(&key, ""),
					     fy_sprintfa("%s%s%s",
						(path && *path) ? path : "",
						(path && *path) ? "/" : "",
						fy_castp(&key, "")), sub,
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
				    "default", "model", "description"));
	*result = rows;
	return 0;
err:
	return -1;
}

/*
 * Without an arena no configuration was applied: an empty document gives the
 * built-in defaults of every sandbox key, and the profiles are built in.
 */
static int sandbox_config_ready(struct fyai_ctx *ctx)
{
	if (ctx->cfg->sandbox_scope)
		return 0;
	ctx->cfg->config_doc = fy_map_empty;
	return fyai_config_apply(ctx->cfg, ctx->cfg->config_doc);
}

/*
 * `none` is the name that confines nothing, so it has no policy to show or to
 * enter.
 */
static int sandbox_named_check(struct fyai_ctx *ctx, const char *name)
{
	if (strcmp(name, FYAI_SANDBOX_PROFILE_NONE))
		return 0;
	fyai_error(ctx, "sandbox profile 'none' confines nothing: it has no policy");
	return -1;
}

/*
 * The policy that a profile resolves to: `extends` applied, placeholders
 * expanded, and the built-in profiles included. Without a name, the profile of
 * the configuration.
 */
int fyai_cmd_sandbox_show(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = call->ctx;
	const char *name = fyai_cmd_arg_str(call, "profile");
	fy_generic v;

	if (sandbox_config_ready(ctx))
		return -1;
	if (!name || !*name)
		name = ctx->cfg->sandbox_profile;
	fyai_error_check(ctx, name, err,
			 "sandbox_profile is none: nothing confines the tools; "
			 "`sandbox profiles` lists the profiles");
	if (sandbox_named_check(ctx, name))
		return -1;
	v = fyai_sandbox_profile_policy(ctx->cfg, ctx->cfg->config_doc, name);
	if (fy_is_invalid(v))
		return -1;
	*result = v;
	return 0;
err:
	return -1;
}

/* The embedded data/apparmor.in profile, with @FYAI_BIN@ for the binary that runs. */
#include "embedded_apparmor.inc"

/*
 * Write the AppArmor profile of this binary. The profile names an absolute path,
 * so it is for the file that runs: /proc/self/exe on Linux, which a link or a
 * relative argv[0] does not change.
 */
int fyai_cmd_sandbox_apparmor(struct fyai_cmd_call *call, fy_generic *result)
{
	static const char marker[] = "@FYAI_BIN@", deleted[] = " (deleted)";
	const char *tpl = (const char *)FYAI_EMBEDDED_APPARMOR;
	size_t tpl_len = FYAI_EMBEDDED_APPARMOR_LEN, mark_len = sizeof(marker) - 1;
	size_t exe_len, count = 0, i, o;
	char exe[PATH_MAX], *out;
	ssize_t got;

	(void)result;
	got = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	fyai_error_check(call->ctx, got > 0, err,
			 "sandbox apparmor: cannot find the binary that runs: %s",
			 strerror(errno));
	exe[got] = '\0';
	/* The kernel appends this to a path that was removed since the program started. */
	if ((size_t)got > sizeof(deleted) - 1 &&
	    !strcmp(exe + got - (sizeof(deleted) - 1), deleted))
		exe[got - (sizeof(deleted) - 1)] = '\0';
	fyai_error_check(call->ctx, !strchr(exe, ' ') && !strchr(exe, '"'), err,
			 "sandbox apparmor: the path '%s' has a blank or a quote, which the "
			 "profile cannot name here: write it by hand", exe);
	exe_len = strlen(exe);
	for (i = 0; i + mark_len <= tpl_len; i++)
		if (!memcmp(tpl + i, marker, mark_len))
			count++;
	out = malloc(tpl_len + count * exe_len + 1);
	fyai_error_check(call->ctx, out, err, "sandbox apparmor: out of memory");
	for (i = 0, o = 0; i < tpl_len;) {
		if (i + mark_len <= tpl_len && !memcmp(tpl + i, marker, mark_len)) {
			memcpy(out + o, exe, exe_len);
			o += exe_len;
			i += mark_len;
		} else {
			out[o++] = tpl[i++];
		}
	}
	/* A profile is written as it stands, for a pipe to tee. */
	(void)fyai_sink_write(call->ctx->sink, FYAI_SINK_MACHINE, out, o);
	free(out);
	return 0;
err:
	return -1;
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
	fy_generic v;

	v = fy_get_at_pathstr(call->gb, call->ctx->cfg->config_doc, key);
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

/*
 * /session lockdown and /session yolo set a group of isolation keys at once. A session-scoped
 * key goes into the session layer, any other key is stored, as for a setting.
 * Values are YAML flow documents.
 */
static int preset_set(struct fyai_cmd_call *call, const char *key, const char *text)
{
	struct fyai_ctx *ctx = call->ctx;
	fy_generic value;

	if (!fyai_config_session_scoped(key))
		return fyai_config_set(ctx, key, text);
	value = fy_parse(call->gb, text, FYAI_YAML_PARSE_FLAGS |
			 FYOPPF_INPUT_TYPE_STRING, NULL);
	fyai_error_check(ctx, fy_is_valid(value), err, "%s: invalid value '%s'",
			 key, text);
	return fyai_config_session_set(ctx, key, value);
err:
	return -1;
}

/* Whether the configuration puts the session itself in a view. */
static bool preset_view_session(struct fyai_ctx *ctx)
{
	return fy_get(fy_get(ctx->cfg->config_doc, "view", fy_invalid),
		      "isolate_session", false);
}

/* Whether this session runs in a view now; a restart cannot leave it. */
static bool preset_in_view(struct fyai_ctx *ctx)
{
	return fyai_view_session_name(ctx) != NULL;
}

/*
 * A switch ends the processes that hold the old isolation, so nothing may run:
 * the check comes before any key changes, and a refused switch changes nothing.
 */
static int preset_idle(struct fyai_cmd_call *call)
{
	struct fyai_ctx *ctx = call->ctx;

	fyai_error_check(ctx, !fyai_tools_active(ctx), err,
			 "%s: close live shells and sub-agents first", call->path);
	fyai_error_check(ctx, !fyai_agents_attached(ctx), err,
			 "%s: close attached agents first", call->path);
	fyai_error_check(ctx, !fyai_ui_has_line(ctx), err,
			 "%s: process queued input first", call->path);
	return 0;
err:
	return -1;
}

/* Apply the change and restart when the credential transport must follow. */
static int preset_commit(struct fyai_cmd_call *call, bool in_view)
{
	if (in_view)
		return fyai_config_rederive(call->ctx);
	return config_changed(call);
}

/* Where sub-agents run: in the view of the session, or as `agent/isolation` says. */
static const char *preset_agents(struct fyai_ctx *ctx, bool session_view)
{
	if (session_view)
		return "share the view of the session";
	return !strcmp(fy_get(fy_get(ctx->cfg->config_doc, "agent", fy_invalid),
			      "isolation", "none"), "view") ?
	       "in a view each" : "share the project";
}

/*
 * The strongest isolation that this host can enforce: the credential
 * transport, the sandbox with no network egress, and a view for the session,
 * which its sub-agents share. The views of sub-agents (`agent/isolation`) stay
 * as the user set them. A session that runs in a view already cannot start
 * the transport itself, and only asks for a restart of fyai. Nothing changes when
 * the transport cannot run.
 */
int fyai_cmd_lockdown(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = call->ctx;
	bool in_view = preset_in_view(ctx);
	bool net = fyai_sandbox_net_restrictable(-1);
	bool was_view = preset_view_session(ctx);
	bool want_view = fyai_view_session_available(ctx);
	const char *note = NULL;
	fy_generic none, policy;
	int rc;

	if (preset_idle(call) || fyai_transport_preflight(ctx))
		return -1;
	/* The profile is stored, so the next image of the session confines its tools too. */
	rc = preset_set(call, "sandbox_profile", "lockdown");
	/* A session already in its view keeps it; this host may not make another. */
	if (!rc && !in_view && want_view != was_view)
		rc = preset_set(call, "view/isolate_session", want_view ? "true" : "false");
	if (!rc)
		rc = preset_set(call, "agent/transport_isolation", "auto");
	if (rc || preset_commit(call, in_view))
		return -1;
	/* The view alone also needs a restart: the transport may be running already. */
	if (!in_view && !ctx->cfg->reload_branch && want_view != was_view &&
	    fyai_cmd_reload(call, &none))
		fyai_warning(ctx, "view/isolate_session is stored; it applies after the next restart");
	if (in_view)
		note = "this session runs in a view already: restart fyai to start the "
		       "credential transport";
	else if (ctx->cfg->reload_branch)
		note = want_view ? "restarting to start the credential transport and the "
				   "view of the session" :
				   "restarting to start the credential transport";
	else if (!want_view)
		note = "no view for the session here: the arena must be in the .fyai "
		       "directory of the project, the scratch directory outside the "
		       "project, and user namespaces available";
	else if (!fyai_sandbox_available())
		note = "this kernel has no Landlock: the sandbox cannot confine anything";
	else if (!net)
		note = "this kernel cannot restrict network egress: it stays open";
	policy = fy_get(ctx->cfg->sandbox, "network", fy_invalid);
	*result = fy_mapping(call->gb, "mode", "lockdown",
			     "transport", "auto",
			     "sandbox", !net || !fy_is_valid(policy) ? "on" :
					fy_empty(fy_get(policy, "ports", fy_invalid)) ?
					"on, no network egress" : "on, network restricted",
			     "agents", preset_agents(ctx, in_view || want_view));
	if (note)
		*result = fy_assoc(call->gb, *result, "note", fy_value(call->gb, note));
	return 0;
}

/*
 * Take off the isolation that lockdown added: no transport and no view. The
 * sandbox profile is @profile; the profile `none` confines nothing.
 * A restart cannot end the transport of a live session: the key lived only in
 * the transport, and the next image would start with none. The keys are stored
 * and the transport ends when fyai starts again, with the key of that run.
 */
static int preset_relax(struct fyai_cmd_call *call, fy_generic *result,
			const char *profile)
{
	struct fyai_ctx *ctx = call->ctx;
	bool in_view = preset_in_view(ctx);
	bool none = !strcmp(profile, FYAI_SANDBOX_PROFILE_NONE);
	const char *note = NULL;
	int rc;

	if (preset_idle(call))
		return -1;
	rc = preset_set(call, "sandbox_profile", profile);
	if (!rc && preset_view_session(ctx))
		rc = preset_set(call, "view/isolate_session", "false");
	if (!rc)
		rc = preset_set(call, "agent/transport_isolation", "none");
	if (rc || preset_commit(call, in_view || ctx->tclient))
		return -1;
	if (ctx->tclient)
		note = "the credential transport of this session ends when fyai starts "
		       "again: start it with the key, from the environment or `fyai auth`";
	else if (in_view)
		note = "this session stays in its view until fyai restarts";
	*result = fy_mapping(call->gb, "mode", none ? "yolo" : "dev", "transport", "none",
			     "sandbox", none ? "off" : "dev", "agents",
			     preset_agents(ctx, in_view));
	if (note)
		*result = fy_assoc(call->gb, *result, "note", fy_value(call->gb, note));
	return 0;
}

/* The opposite of lockdown: no transport, no sandbox and no view. */
int fyai_cmd_yolo(struct fyai_cmd_call *call, fy_generic *result)
{
	return preset_relax(call, result, FYAI_SANDBOX_PROFILE_NONE);
}

/*
 * The default: no transport and no view, and the tools confined by the dev
 * profile, which lets them do project work.
 */
int fyai_cmd_dev(struct fyai_cmd_call *call, fy_generic *result)
{
	return preset_relax(call, result, "dev");
}

/* Show the isolation of the session as it runs, and what is stored beyond that. */
int fyai_cmd_session_status(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = call->ctx;
	struct fyai_cfg *cfg = ctx->cfg;
	fy_generic net = fy_get(cfg->sandbox, "network", fy_invalid);
	const char *running = fyai_transport_effective_level(ctx), *name = fyai_view_session_name(ctx);
	const char *stored = cfg->agent_transport_isolation ? cfg->agent_transport_isolation : "none";
	const char *note = NULL;
	bool want_view = preset_view_session(ctx), stored_on, running_on, view_on, restricted;

	restricted = fy_is_valid(net) && fyai_sandbox_net_restrictable(-1);
	/* A key that is stored and not yet in force waits for the next start. */
	stored_on = strcmp(stored, "none") != 0;
	running_on = strcmp(running, "none") != 0;
	view_on = name != NULL;
	if (stored_on != running_on)
		note = "the credential transport changes when fyai starts again";
	else if (want_view != view_on)
		note = "the view of the session changes when fyai starts again";
	*result = fy_mapping(call->gb,
			     "transport", running_on ? running : "off",
			     "sandbox", !cfg->enable_sandbox ? "off" :
					fy_sprintfa("on, %s%s", cfg->sandbox_profile,
						    restricted ? ", network restricted" : ""),
			     "agents", preset_agents(ctx, view_on),
			     "view", name ? name : "none");
	if (note)
		*result = fy_assoc(call->gb, *result, "note", fy_value(call->gb, note));
	return 0;
}

/*
 * push and pull ask the supervisor of the session, which stays outside the view
 * and so reaches the project. The request goes on the descriptor that the
 * supervisor passed (FYAI_SESSION_FD), and the reply comes back on the event loop.
 */
struct session_request {
	struct fyai_cmd_call *call;
	struct fyai_event_source *source;
	bool ends_session;	/* a pull that went through: the supervisor starts a new session */
	char *revert_profile;	/* the stored values to put back when the supervisor refuses */
	int revert_view;	/* -1: none; else the value of view/isolate_session */
};

static void session_request_cleanup(struct fyai_cmd_call *call)
{
	struct session_request *r = call->priv;

	if (!r)
		return;
	if (r->source)
		fyai_event_source_remove(r->source);
	free(r->revert_profile);
	free(r);
	call->priv = NULL;
}

/* Put back the stored values that a refused request had changed. */
static void session_request_revert(struct fyai_cmd_call *call, struct session_request *r)
{
	if (r->revert_profile &&
	    preset_set(call, "sandbox_profile", fy_sprintfa("'%s'", r->revert_profile)))
		fyai_warning(call->ctx, "%s: could not put back the sandbox profile '%s'",
			     call->path, r->revert_profile);
	if (r->revert_view >= 0 &&
	    preset_set(call, "view/isolate_session", r->revert_view ? "true" : "false"))
		fyai_warning(call->ctx, "%s: could not put back view/isolate_session", call->path);
	(void)fyai_config_rederive(call->ctx);
}

static void session_quit(void *userdata)
{
	fyai_ui_quit_request(userdata);
}

static enum fyai_event_action session_reply(const struct fyai_event *ev)
{
	struct session_request *r = ev->userdata;
	struct fyai_cmd_call *call = r->call;
	struct fyai_ctx *ctx = call->ctx;
	bool ends = r->ends_session;
	fy_generic m, op, why;
	int fd = ev->fd, rc;

	rc = fyai_ctl_recv(fd, call->gb, &m, NULL);
	if (rc == -EAGAIN)
		return FYAIEA_CONTINUE;
	if (rc) {
		fyai_error(ctx, "%s: the supervisor of the session did not answer", call->path);
		fyai_cmd_done(call, -1, fy_invalid);
		return FYAIEA_CONTINUE;
	}
	op = fy_get(m, "op", fy_invalid);
	if (!fy_equal(op, "ok")) {
		why = fy_get(m, "message", fy_invalid);
		fyai_error(ctx, "%s: %s", call->path, fy_is_string(why) ? fy_castp(&why, "") :
			   "the supervisor refused");
		session_request_revert(call, r);
		fyai_cmd_done(call, -1, fy_invalid);
		return FYAIEA_CONTINUE;
	}
	fyai_cmd_done(call, 0, fy_get(m, "result", fy_map_empty));
	/*
	 * The supervisor starts the session again on the fresh view once this one ends.
	 * The result is drawn first, so the quit waits for the next turn of the loop.
	 */
	if (ends)
		(void)fyai_event_defer(fyai_ctx_loop(ctx), session_quit, ctx);
	return FYAIEA_CONTINUE;
}

static int session_request(struct fyai_cmd_call *call, const char *op, fy_generic request,
			   bool ends_session)
{
	struct fyai_ctx *ctx = call->ctx;
	struct session_request *r;
	const char *env = getenv("FYAI_SESSION_FD");
	int fd = env ? atoi(env) : -1, rc;

	fyai_error_check(ctx, fyai_view_session_name(ctx) && fd > 2 && fcntl(fd, F_GETFD) >= 0, err,
			 "%s: this session does not run in a view; `/session lockdown` puts it in one",
			 call->path);
	fyai_error_check(ctx, !call->priv, err, "%s: a request is in flight", call->path);
	r = calloc(1, sizeof(*r));
	fyai_error_check(ctx, r, err, "%s: out of memory", call->path);
	r->call = call;
	r->ends_session = ends_session;
	r->revert_view = -1;
	call->priv = r;
	call->cleanup = session_request_cleanup;
	request = fy_assoc(call->gb, request, "op", fy_value(call->gb, op));
	request = fy_assoc(call->gb, request, "seq", 1LL);
	rc = fyai_ctl_send(fd, request, -1, 0);
	fyai_error_check(ctx, !rc, err_priv, "%s: cannot reach the supervisor of the session: %s",
			 call->path, strerror(-rc));
	rc = fyai_event_add_fd(fyai_ctx_loop(ctx), fd, FYAIEV_READ, session_reply, r, &r->source);
	fyai_error_check(ctx, !rc, err_priv, "%s: cannot wait for the supervisor of the session",
			 call->path);
	return FYAI_CMD_PENDING;
err_priv:
	session_request_cleanup(call);
err:
	return -1;
}

/* Apply the changes of the session to the project. */
int fyai_cmd_session_push(struct fyai_cmd_call *call, fy_generic *result)
{
	(void)result;
	return session_request(call, "push",
			       fy_mapping(call->gb, "dry_run", fyai_cmd_arg_bool(call, "dry_run"),
					  "paths", fy_get(call->args, "paths", fy_seq_empty)),
			       false);
}

/* Replace the view with the project as it is now, and start the session again on it. */
int fyai_cmd_session_pull(struct fyai_cmd_call *call, fy_generic *result)
{
	(void)result;
	return session_request(call, "pull",
			       fy_mapping(call->gb, "discard", fyai_cmd_arg_bool(call, "discard")),
			       true);
}

struct profile_list {
	struct fy_generic_builder *gb;
	const char *current;
	fy_generic names;
};

static void profile_list_add(void *arg, const char *name, const char *desc)
{
	struct profile_list *l = arg;

	(void)desc;
	l->names = fy_append(l->gb, l->names, fy_stringf(l->gb, "%s%s", name,
			     !strcmp(name, l->current) ? " (current)" : ""));
}

/*
 * Select the sandbox profile of the session by name, or list the profiles. The
 * profile is stored, with the view of the session that it asks for, and the
 * session starts again to apply it. Outside a view the session runs this program
 * again. In a view the supervisor does it: it applies the view to the project
 * first, refuses when a path conflicts, and starts the session again in a view
 * or outside one. The credential transport is not changed: `/session dev`,
 * `/session lockdown` and `/session yolo` set that too.
 */
int fyai_cmd_session_profile(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = call->ctx;
	struct fyai_cfg *cfg = ctx->cfg;
	const char *name = fyai_cmd_arg_str(call, "name");
	struct profile_list list = { .gb = call->gb,
		.current = cfg->sandbox_profile ? cfg->sandbox_profile : FYAI_SANDBOX_PROFILE_NONE };
	bool in_view = preset_in_view(ctx), was_view = preset_view_session(ctx);
	bool pview = false, want_view;
	const char *c, *note = NULL, *old = list.current;
	fy_generic none, policy;
	struct session_request *r;
	int rc;

	list.names = fy_sequence(call->gb);
	if (name) {
		for (c = name; *c; c++)
			fyai_error_check(ctx, isalnum((unsigned char)*c) || *c == '_' ||
					 *c == '-', err,
					 "profile: '%s' is not a profile name", name);
		if (strcmp(name, FYAI_SANDBOX_PROFILE_NONE)) {
			policy = fyai_sandbox_profile_policy(cfg, cfg->config_doc, name);
			fyai_error_check(ctx, fy_is_valid(policy), err,
					 "profile: could not select '%s'", name);
			pview = fy_get(policy, "view", false);
		}
		/* A session in a view cannot make another: the supervisor decides. */
		want_view = pview && (in_view || fyai_view_session_available(ctx));
		if (pview && !want_view)
			fyai_warning(ctx, "profile '%s' asks for a view of the session, which "
				     "this host cannot make: the tools are confined, and the "
				     "session runs in the project", name);
		if (strcmp(name, old) || want_view != (in_view || was_view)) {
			/* A restart follows, and it ends the live work. */
			if (preset_idle(call))
				return -1;
			rc = preset_set(call, "sandbox_profile", fy_sprintfa("'%s'", name));
			if (!rc && want_view != was_view)
				rc = preset_set(call, "view/isolate_session",
						want_view ? "true" : "false");
			if (rc || preset_commit(call, in_view))
				return -1;
			if (in_view) {
				rc = session_request(call, "restart",
						     fy_mapping(call->gb, "view", want_view), true);
				if (rc == FYAI_CMD_PENDING && (r = call->priv)) {
					r->revert_profile = strdup(old);
					r->revert_view = was_view;
				}
				return rc;
			}
			if (fyai_cmd_reload(call, &none))
				fyai_warning(ctx, "sandbox_profile is stored; it applies after the next restart");
			else
				note = "restarting to apply the profile";
		}
		list.current = name;
	}
	fyai_sandbox_profile_names(cfg, cfg->config_doc, profile_list_add, &list);
	*result = fy_mapping(call->gb, "profile", list.current,
			     "available", fyai_skill_join(call->gb, list.names, ", "));
	if (note)
		*result = fy_assoc(call->gb, *result, "note", fy_value(call->gb, note));
	return 0;
err:
	return -1;
}
