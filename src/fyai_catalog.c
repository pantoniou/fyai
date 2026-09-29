/*
 * fyai_catalog.c - provider/model catalogue (scrape-providers document)
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_CATALOG

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "fyai_transport_boot.h"
#include "fyai_sink.h"
#include "fyai_catalog.h"
#include "fyai_config.h"
#include "fyai_markdown.h"
#include "fyai_schema.h"
#include "fyai_storage.h"
#include "fyai_tool_spec.h"
#include "fyai_tools.h"
#include "utils.h"

/* FYAI_EMBEDDED_CATALOG[] / FYAI_EMBEDDED_CATALOG_LEN - the vendored
 * data/catalog.yaml snapshot, generated at configure time. */
#include "embedded_catalog.inc"
/* FYAI_EMBEDDED_CATALOG_SCHEMA[] - data/catalog.schema.yaml. */
#include "embedded_catalog_schema.inc"

static fy_generic embedded_catalog = fy_invalid;

fy_generic fyai_catalog_effective(fy_generic arena_catalog,
				  struct fy_generic_builder *gb)
{
	struct fy_generic_builder_cfg cfg = {
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED,
	};
	static struct fy_generic_builder *embedded_gb;
	fy_generic_sized_string embedded;

	if (fy_is_valid(arena_catalog))
		return arena_catalog;
	if (fy_is_valid(embedded_catalog))
		return embedded_catalog;
	/*
	 * The parse is cached for the process, so it lives in a builder of its
	 * own: the builder of the first caller can be released before the
	 * next call.
	 */
	if (!embedded_gb)
		embedded_gb = fy_generic_builder_create(&cfg);
	if (!embedded_gb)
		return fy_invalid;
	gb = embedded_gb;
	embedded.data = (const char *)FYAI_EMBEDDED_CATALOG;
	embedded.size = FYAI_EMBEDDED_CATALOG_LEN;
	embedded_catalog = fy_parse(gb, embedded,
				    FYAI_YAML_PARSE_FLAGS |
				    FYOPPF_INPUT_TYPE_STRING, NULL);
	return embedded_catalog;
}

fy_generic fyai_catalog_model(fy_generic cat, const char *model)
{
	fy_generic models, m;
	fy_generic name;

	if (!model)
		return fy_invalid;
	models = fy_get(cat, "models");
	fy_foreach(m, models) {
		name = fy_get(m, "name");
		if (fy_equal(name, model))
			return m;
	}
	return fy_invalid;
}

fy_generic fyai_catalog_resolved_model(fy_generic cat, const char *model)
{
	fy_generic cat_model, cat_offer;

	cat_model = fyai_catalog_model(cat, model);
	if (fy_is_valid(cat_model))
		return cat_model;

	fyai_catalog_provider_for_model(cat, model, &cat_offer);
	if (fy_is_valid(cat_offer))
		return fyai_catalog_model(cat, fy_get(cat_offer, "canonical_id", ""));

	return fy_invalid;
}

bool fyai_catalog_model_has_cap(fy_generic model_entry, const char *cap)
{
	fy_generic caps, c;

	if (!cap)
		return false;
	caps = fy_get(model_entry, "capabilities");
	fy_foreach(c, caps) {
		if (fy_equal(c, cap))
			return true;
	}
	return false;
}

fy_generic fyai_catalog_provider_for_model(fy_generic cat, const char *model,
					   fy_generic *offeringp)
{
	fy_generic providers, p, offers, o;
	fy_generic canon, pmid;

	if (offeringp)
		*offeringp = fy_invalid;
	if (!model)
		return fy_invalid;
	providers = fy_get(cat, "providers");
	fy_foreach(p, providers) {
		offers = fy_get(p, "models");
		fy_foreach(o, offers) {
			canon = fy_get(o, "canonical_id");
			pmid = fy_get(o, "provider_model_id");
			if (fy_not_equal(canon, model) && fy_not_equal(pmid, model))
				continue;
			if (offeringp)
				*offeringp = o;
			return p;
		}
	}
	return fy_invalid;
}

fy_generic fyai_catalog_provider(fy_generic cat, const char *name)
{
	fy_generic providers, p;
	const char *provider_name;

	if (!name)
		return fy_invalid;
	providers = fy_get(cat, "providers");
	fy_foreach(p, providers) {
		provider_name = fy_get(p, "name", "");
		if (!strcasecmp(provider_name, name))
			return p;
	}
	return fy_invalid;
}

fy_generic fyai_catalog_offering(fy_generic provider, const char *model,
				 fy_generic *offeringp)
{
	fy_generic offers, o;
	fy_generic canon, pmid;

	if (offeringp)
		*offeringp = fy_invalid;
	if (!model)
		return fy_invalid;
	offers = fy_get(provider, "models");
	fy_foreach(o, offers) {
		canon = fy_get(o, "canonical_id");
		pmid = fy_get(o, "provider_model_id");
		if (fy_not_equal(canon, model) && fy_not_equal(pmid, model))
			continue;
		if (offeringp)
			*offeringp = o;
		return o;
	}
	return fy_invalid;
}

/* The catalogue names protocols with underscores. */
static const char *api_to_protocol(enum fyai_api_mode api)
{
	switch (api) {
	case FYAI_API_RESPONSES:
		return "responses";
	case FYAI_API_CHAT_COMPLETIONS:
		return "chat_completions";
	case FYAI_API_MESSAGES:
		return "messages";
	}
	return "";
}

fy_generic fyai_catalog_endpoint(fy_generic provider, enum fyai_api_mode api)
{
	fy_generic eps, e;
	const char *proto;

	eps = fy_get(provider, "endpoints");
	proto = api_to_protocol(api);
	fy_foreach(e, eps) {
		if (fy_equal(fy_get(e, "protocol"), proto))
			return e;
	}
	return fy_invalid;
}

bool fyai_catalog_endpoint_has_hosted_tool(fy_generic endpoint,
						const char *tool)
{
	fy_generic hosted, item;

	if (!tool || !*tool)
		return false;
	hosted = fy_get(endpoint, "hosted_tools");
	fy_foreach(item, hosted)
		if (fy_equal(item, tool))
			return true;
	return false;
}

static fy_generic embedded_catalog_schema = fy_invalid;

fy_generic fyai_catalog_schema(struct fy_generic_builder *gb)
{
	struct fy_generic_builder_cfg cfg = {
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED,
	};
	static struct fy_generic_builder *schema_gb;
	fy_generic_sized_string embedded;
	fy_generic schema;

	/*
	 * The parse is cached for the process, so it lives in a builder of its
	 * own: the builder of the first caller can be released before the
	 * next call.
	 */
	if (fy_is_valid(embedded_catalog_schema))
		return embedded_catalog_schema;
	if (!schema_gb)
		schema_gb = fy_generic_builder_create(&cfg);
	if (!schema_gb)
		return fy_invalid;
	gb = schema_gb;
	embedded.data = (const char *)FYAI_EMBEDDED_CATALOG_SCHEMA;
	embedded.size = FYAI_EMBEDDED_CATALOG_SCHEMA_LEN;
	schema = fy_parse(gb, embedded,
			  FYAI_YAML_PARSE_FLAGS | FYOPPF_INPUT_TYPE_STRING, NULL);
	/*
	 * The schema is vendored from scrape-providers. Its $id names it and
	 * resolves nothing, and the validator refuses the keyword.
	 */
	if (fy_is_mapping(schema) &&
	    fy_is_valid(fy_get(schema, "$id", fy_invalid)))
		schema = fy_disassoc(gb, schema, "$id");
	embedded_catalog_schema = schema;
	return embedded_catalog_schema;
}

/* Check @doc against the catalogue schema; one diagnostic says why not. */
static int catalog_check(struct fyai_ctx *ctx, fy_generic doc,
			 const char *origin)
{
	struct response_buffer msg = {0};
	fy_generic schema, report, problem;

	if (!fy_is_mapping(doc)) {
		fyai_error(ctx, "%s is not a YAML mapping", origin);
		return -1;
	}
	schema = fyai_catalog_schema(ctx->cfg->gb);
	if (fy_is_invalid(schema)) {
		fyai_error(ctx, "cannot read the embedded catalogue schema");
		return -1;
	}
	report = fyai_schema_validate(ctx->cfg->gb, schema, doc);
	if (fyai_schema_valid(report))
		return 0;
	fy_foreach(problem, fy_get(report, "problems", fy_seq_empty)) {
		if (response_buffer_append(&msg, "\n  ") ||
		    response_buffer_append(&msg, fy_castp(&problem, "")))
			break;
	}
	fyai_error(ctx, "%s does not match the catalogue schema:%s", origin,
		   msg.len ? msg.data : " (no details)");
	free(msg.data);
	return -1;
}

int fyai_catalog_commit(struct fyai_ctx *ctx, fy_generic doc,
			const char *origin)
{
	fy_generic effective, new_config;

	if (!ctx->durable_gb) {
		fyai_error(ctx, "no arena; run fyai init");
		return -1;
	}
	if (!fy_is_null(doc) && catalog_check(ctx, doc, origin))
		return -1;
	/*
	 * The model_info block of the configuration follows the catalogue it
	 * was derived from, so derive it again for the new one.
	 */
	effective = fy_is_null(doc) ?
		fyai_catalog_effective(fy_invalid, ctx->cfg->gb) : doc;
	new_config = fy_is_valid(ctx->arena_config) ?
		fyai_config_sync_catalog(ctx->gb, effective, ctx->arena_config) :
		fy_invalid;
	/* A null catalogue removes the member; the branch uses the embedded one. */
	if (fyai_publish_root(ctx, new_config, doc, fy_invalid))
		return -1;
	if (fy_is_null(ctx->arena_catalog))
		ctx->arena_catalog = fy_invalid;
	return fyai_config_adopt_catalog(ctx);
}

int fyai_catalog_import(struct fyai_ctx *ctx, const char *path)
{
	fy_generic doc;

	doc = fy_parse_file(ctx->gb, FYAI_YAML_PARSE_FLAGS, path);
	if (!fy_is_mapping(doc)) {
		fyai_error(ctx, "cannot parse %s", path);
		return -1;
	}
	return fyai_catalog_commit(ctx, doc, path);
}

/*
 * Catalogue paths are slash-separated. A component that selects an item of a
 * sequence is its index, or the value of the key that names the item: "name",
 * or "canonical_id" for the models that a provider offers.
 */
#define CATALOG_PATH_MAX_DEPTH	32

static const char *catalog_item_key(fy_generic seq)
{
	fy_generic item;

	fy_foreach(item, seq) {
		if (fy_is_string(fy_get(item, "name", fy_invalid)))
			return "name";
		if (fy_is_string(fy_get(item, "canonical_id", fy_invalid)))
			return "canonical_id";
	}
	return "name";
}

static long catalog_seq_find(fy_generic seq, const char *seg)
{
	fy_generic item;
	const char *key;
	char *end;
	long idx, i;

	idx = strtol(seg, &end, 10);
	if (*seg && !*end)
		return idx >= 0 && (size_t)idx < fy_len(seq) ? idx : -1;
	key = catalog_item_key(seq);
	i = 0;
	fy_foreach(item, seq) {
		if (!strcmp(fy_get(item, key, ""), seg))
			return i;
		i++;
	}
	return -1;
}

static int catalog_path_split(char *path, char **segs)
{
	char *seg, *save;
	int n;

	n = 0;
	for (seg = strtok_r(path, "/", &save); seg;
	     seg = strtok_r(NULL, "/", &save)) {
		if (n == CATALOG_PATH_MAX_DEPTH)
			return -1;
		segs[n++] = seg;
	}
	return n;
}

static fy_generic catalog_path_get(fy_generic node, char **segs, int n)
{
	long idx;
	int i;

	for (i = 0; i < n && fy_is_valid(node); i++) {
		if (fy_is_mapping(node)) {
			node = fy_get(node, segs[i], fy_invalid);
		} else if (fy_is_sequence(node)) {
			idx = catalog_seq_find(node, segs[i]);
			node = idx < 0 ? fy_invalid : fy_get_at(node, idx);
		} else {
			node = fy_invalid;
		}
	}
	return node;
}

/*
 * Return @node with the value at @segs replaced by @value, or removed when
 * @value is fy_invalid. A new item of a sequence is appended and named by
 * the path component. fy_invalid when the path does not lead anywhere.
 */
static fy_generic catalog_path_put(struct fy_generic_builder *gb,
				   fy_generic node, char **segs, int n,
				   fy_generic value)
{
	fy_generic child, item, out;
	const char *key;
	long idx, i;

	if (!n)
		return value;
	if (fy_is_mapping(node)) {
		child = fy_get(node, segs[0], fy_invalid);
		if (n == 1 && fy_is_invalid(value))
			return fy_is_invalid(child) ? fy_invalid :
				fy_disassoc(gb, node, segs[0]);
		if (fy_is_invalid(child)) {
			if (fy_is_invalid(value))
				return fy_invalid;
			child = fy_map_empty;
		}
		child = catalog_path_put(gb, child, segs + 1, n - 1, value);
		return fy_is_invalid(child) ? child :
			fy_assoc(gb, node, segs[0], child);
	}
	if (!fy_is_sequence(node))
		return fy_invalid;
	idx = catalog_seq_find(node, segs[0]);
	if (idx < 0) {
		/* A missing item can be made, not removed. */
		if (fy_is_invalid(value))
			return fy_invalid;
		key = catalog_item_key(node);
		child = fy_mapping(gb, key, fy_value(gb, segs[0]));
		if (n == 1 && fy_is_mapping(value) &&
		    fy_is_invalid(fy_get(value, key, fy_invalid)))
			child = fy_assoc(gb, value, key, fy_value(gb, segs[0]));
		else if (n == 1)
			child = value;
		else
			child = catalog_path_put(gb, child, segs + 1, n - 1,
						 value);
		return fy_is_invalid(child) ? child :
			fy_append(gb, node, child);
	}
	out = fy_seq_empty;
	i = 0;
	fy_foreach(item, node) {
		child = item;
		if (i++ == idx) {
			if (n == 1 && fy_is_invalid(value))
				continue;
			child = catalog_path_put(gb, item, segs + 1, n - 1,
						 value);
			if (fy_is_invalid(child))
				return child;
		}
		out = fy_append(gb, out, child);
		if (fy_is_invalid(out))
			return out;
	}
	return out;
}

static fy_generic catalog_current(struct fyai_ctx *ctx)
{
	return fyai_catalog_effective(ctx->arena_catalog, ctx->cfg->gb);
}

int fyai_catalog_value(struct fyai_ctx *ctx, const char *path,
		       fy_generic *valuep)
{
	char *segs[CATALOG_PATH_MAX_DEPTH];
	char *copy;
	fy_generic v;
	int n;

	copy = strdup(path);
	fyai_error_check(ctx, copy, err_out, "out of memory reading '%s'", path);
	n = catalog_path_split(copy, segs);
	v = n < 0 ? fy_invalid : catalog_path_get(catalog_current(ctx), segs, n);
	free(copy);
	fyai_error_check(ctx, n >= 0, err_out, "catalogue path '%s' is too deep",
			 path);
	fyai_error_check(ctx, fy_is_valid(v), err_out,
			 "catalogue has nothing at '%s'", path);
	*valuep = v;
	return 0;

err_out:
	return -1;
}

/* Set @value (fy_invalid removes) at @path of the catalogue of the branch. */
static int catalog_put(struct fyai_ctx *ctx, const char *path, fy_generic value)
{
	char *segs[CATALOG_PATH_MAX_DEPTH];
	char *copy;
	fy_generic doc;
	int n, rc;

	copy = strdup(path);
	fyai_error_check(ctx, copy, err_out, "out of memory reading '%s'", path);
	n = catalog_path_split(copy, segs);
	doc = fy_invalid;
	if (n > 0)
		doc = catalog_path_put(ctx->gb, catalog_current(ctx), segs, n,
				       value);
	free(copy);
	fyai_error_check(ctx, n > 0, err_out, "invalid catalogue path '%s'",
			 path);
	fyai_error_check(ctx, fy_is_valid(doc), err_out,
			 fy_is_invalid(value) ?
			 "catalogue has nothing at '%s'" :
			 "catalogue path '%s' does not lead to a mapping or "
			 "a sequence", path);
	rc = fyai_catalog_commit(ctx, doc, "the edited catalogue");
	return rc;

err_out:
	return -1;
}

int fyai_catalog_set(struct fyai_ctx *ctx, const char *path, const char *value)
{
	fy_generic v;

	if (!ctx->durable_gb) {
		fyai_error(ctx, "no arena; run fyai init");
		return -1;
	}
	v = fy_parse(ctx->gb, value,
		     FYAI_YAML_PARSE_FLAGS | FYOPPF_INPUT_TYPE_STRING, NULL);
	if (fy_is_invalid(v)) {
		fyai_error(ctx, "cannot parse value '%s'", value);
		return -1;
	}
	return catalog_put(ctx, path, v);
}

int fyai_catalog_delete(struct fyai_ctx *ctx, const char *path)
{
	if (!ctx->durable_gb) {
		fyai_error(ctx, "no arena; run fyai init");
		return -1;
	}
	return catalog_put(ctx, path, fy_invalid);
}

int fyai_catalog_validate(struct fyai_ctx *ctx)
{
	if (catalog_check(ctx, catalog_current(ctx),
			  fy_is_valid(ctx->arena_catalog) ?
			  "the catalogue of the branch" :
			  "the embedded catalogue"))
		return -1;
	return 0;
}

int fyai_catalog_reset(struct fyai_ctx *ctx)
{
	if (fy_is_invalid(ctx->arena_catalog))
		return 0;
	return fyai_catalog_commit(ctx, fy_null, "the embedded catalogue");
}

/* Quote @s for /bin/sh into @out. */
static int catalog_shell_quote(struct response_buffer *out, const char *s)
{
	if (response_buffer_append(out, " '"))
		return -1;
	for (; *s; s++) {
		if (*s == '\'' ? response_buffer_append(out, "'\\''") :
				 response_buffer_append_data(out, s, 1))
			return -1;
	}
	return response_buffer_append(out, "'");
}

/* Return @seq with item @idx replaced by @item. */
static fy_generic catalog_seq_replace(struct fy_generic_builder *gb,
				      fy_generic seq, long idx, fy_generic item)
{
	fy_generic cur, out;
	long i;

	out = fy_seq_empty;
	i = 0;
	fy_foreach(cur, seq) {
		out = fy_append(gb, out, i++ == idx ? item : cur);
		if (fy_is_invalid(out))
			break;
	}
	return out;
}

/* Replace or add each item of @add in @seq, matched by the key that names it. */
static fy_generic catalog_seq_merge(struct fy_generic_builder *gb,
				    fy_generic seq, fy_generic add)
{
	fy_generic item, out;
	const char *key, *name;
	long idx;

	if (!fy_is_sequence(seq))
		return add;
	key = catalog_item_key(fy_len(seq) ? seq : add);
	out = seq;
	fy_foreach(item, add) {
		name = fy_get(item, key, "");
		idx = *name ? catalog_seq_find(out, name) : -1;
		out = idx < 0 ? fy_append(gb, out, item) :
			catalog_seq_replace(gb, out, idx, item);
		if (fy_is_invalid(out))
			return out;
	}
	return out;
}

/*
 * Fill @names with the credentials that catalog_update/credentials names,
 * NULL terminated. The names point into the configuration.
 */
static void catalog_update_env_keep(struct fyai_ctx *ctx, const char **names)
{
	const char *name;
	size_t n;

	n = 0;
	fy_foreach(name, ctx->cfg->catalog_update_credentials) {
		if (n == FYAI_CATALOG_ENV_KEEP_MAX)
			break;
		if (!fy_str_empty(name))
			names[n++] = name;
	}
	names[n] = NULL;
}

/* The command line of catalog_update/command with the selection; heap. */
static char *catalog_update_command(struct fyai_ctx *ctx,
				    const char *const *providers,
				    size_t count, bool curated)
{
	struct fyai_cfg *cfg = ctx->cfg;
	struct response_buffer cmd = {0};
	size_t i;
	int rc;

	fyai_error_check(ctx, !fy_str_empty(cfg->catalog_update_command), err,
			 "catalog_update/command is empty");
	rc = response_buffer_append(&cmd, cfg->catalog_update_command) ||
	     response_buffer_append(&cmd, " --format yaml") ||
	     (curated && response_buffer_append(&cmd, " --curated"));
	for (i = 0; !rc && i < count; i++)
		rc = response_buffer_append(&cmd, " --provider") ||
		     catalog_shell_quote(&cmd, providers[i]);
	fyai_error_check(ctx, !rc, err, "out of memory building the "
			 "catalogue command");
	return cmd.data;

err:
	free(cmd.data);
	return NULL;
}

/*
 * Parse the catalogue that @cmd wrote and commit it. With @selected, merge
 * only the providers and models that it describes.
 */
static int catalog_update_apply(struct fyai_ctx *ctx, const char *cmd,
				const char *data, size_t len, bool selected)
{
	fy_generic_sized_string text;
	fy_generic doc, cur, merged, models;

	fyai_error_check(ctx, len, err, "%s wrote no catalogue", cmd);
	text.data = data;
	text.size = len;
	doc = fy_parse(ctx->gb, text, FYAI_YAML_PARSE_FLAGS |
		       FYOPPF_INPUT_TYPE_STRING | FYOPPF_COLLECT_DIAG, NULL);
	if (fy_is_invalid(doc)) {
		parse_diag_report(ctx, doc, "not YAML",
				  fy_sprintfa("the output of %s", cmd));
		goto err;
	}
	fyai_error_check(ctx, fy_is_mapping(doc), err,
			 "the output of %s is not a YAML mapping", cmd);

	/*
	 * A selection of providers replaces those providers and the models
	 * that the scrape describes; the rest of the catalogue stays.
	 */
	if (selected) {
		cur = catalog_current(ctx);
		merged = catalog_seq_merge(ctx->gb, fy_get(cur, "providers"),
				fy_get(doc, "providers", fy_seq_empty));
		if (fy_is_valid(merged))
			merged = fy_assoc(ctx->gb, cur, "providers", merged);
		models = catalog_seq_merge(ctx->gb, fy_get(cur, "models"),
				fy_get(doc, "models", fy_seq_empty));
		if (fy_is_valid(merged) && fy_is_valid(models))
			merged = fy_assoc(ctx->gb, merged, "models", models);
		fyai_error_check(ctx, fy_is_valid(merged) &&
				 fy_is_valid(models), err,
				 "could not merge the output of %s into the "
				 "catalogue", cmd);
		doc = merged;
	}
	if (fyai_catalog_commit(ctx, doc, cmd))
		goto err;
	fyai_result(ctx, "catalog: updated (%zu models, %zu providers)\n",
		    fy_len(fy_get(doc, "models")),
		    fy_len(fy_get(doc, "providers")));
	return 0;

err:
	return -1;
}

int fyai_catalog_update(struct fyai_ctx *ctx, const char *const *providers,
			size_t count, bool curated)
{
	const char *env_keep[FYAI_CATALOG_ENV_KEEP_MAX + 1];
	struct shell_command_result res = {0};
	struct shell_command_opts opts = {0};
	const char *stderr_text;
	char *cmd = NULL;
	int rc;

	rc = -1;
	fyai_error_check(ctx, ctx->durable_gb, out, "no arena; run fyai init");
	cmd = catalog_update_command(ctx, providers, count, curated);
	if (!cmd)
		goto out;

	fyai_report(ctx, "catalog: running %s\n", cmd);
	catalog_update_env_keep(ctx, env_keep);
	opts.timeout_ms = ctx->cfg->catalog_update_timeout_ms;
	opts.env_keep = env_keep;
	rc = run_shell_command_capture_cb(ctx, cmd, &res, NULL, NULL, NULL, &opts);
	if (rc) {
		rc = -1;
		goto out;	/* run_shell_command_capture_cb() says why */
	}
	rc = -1;
	/* The last line of the error output says why; keep it one line. */
	if (res.stderr_data)
		while (res.stderr_len && (res.stderr_data[res.stderr_len - 1] == '\n' ||
					  res.stderr_data[res.stderr_len - 1] == '\r'))
			res.stderr_data[--res.stderr_len] = '\0';
	stderr_text = res.stderr_data && *res.stderr_data ?
		res.stderr_data : "no error output";
	fyai_error_check(ctx, !res.timed_out, out,
			 "%s did not finish in %u ms", cmd, opts.timeout_ms);
	fyai_error_check(ctx, !res.signaled, out, "%s was stopped by signal "
			 "%d: %s", cmd, res.signal, stderr_text);
	fyai_error_check(ctx, !res.exit_code, out, "%s exited with status "
			 "%d: %s", cmd, res.exit_code, stderr_text);
	rc = catalog_update_apply(ctx, cmd, res.stdout_data, res.stdout_len,
				  count > 0);
out:
	shell_command_result_cleanup(&res);
	free(cmd);
	return rc;
}

/*
 * An update started from a session. The program runs in a tile of the work
 * pane and writes the catalogue to a private file; the session collects the
 * request between turns and commits it there.
 */
struct fyai_catalog_update_request {
	struct fyai_ctx *ctx;
	struct fyai_shell_session *session;
	char *cmd;			/* the catalogue command, for reports */
	char dir[64];			/* private directory of the output */
	char out[96];			/* the catalogue the program writes */
	bool selected;
	bool done;
	bool cancelled;
	int exit_code;
	int signal;
};

static void catalog_update_exited(void *userdata, int exit_code, int signal)
{
	struct fyai_catalog_update_request *request = userdata;

	request->session = NULL;
	request->exit_code = exit_code;
	request->signal = signal;
	request->done = true;
}

struct fyai_catalog_update_request *
fyai_catalog_update_submit(struct fyai_ctx *ctx, const char *const *providers,
			   size_t count, bool curated)
{
	struct fyai_catalog_update_request *request;
	struct response_buffer line = {0};
	const char *env_names[FYAI_CATALOG_ENV_KEEP_MAX + 1];
	const char *tmp;
	int rc;

	request = calloc(1, sizeof(*request));
	fyai_error_check(ctx, request, err, "could not allocate the "
			 "catalogue update");
	request->ctx = ctx;
	request->selected = count > 0;
	fyai_error_check(ctx, ctx->durable_gb, err, "no arena; run fyai init");
	request->cmd = catalog_update_command(ctx, providers, count, curated);
	if (!request->cmd)
		goto err;

	tmp = getenv("TMPDIR");
	if (fy_str_empty(tmp) || strlen(tmp) > 32)
		tmp = "/tmp";
	snprintf(request->dir, sizeof(request->dir), "%s/fyai-catalog-XXXXXX",
		 tmp);
	fyai_error_check(ctx, mkdtemp(request->dir), err_dir,
			 "could not create a directory for the catalogue: %s",
			 strerror(errno));
	snprintf(request->out, sizeof(request->out), "%s/catalog.yaml",
		 request->dir);

	/* Standard error stays on the tile; the catalogue goes to the file. */
	rc = response_buffer_append(&line, request->cmd) ||
	     response_buffer_append(&line, " >") ||
	     catalog_shell_quote(&line, request->out);
	fyai_error_check(ctx, !rc, err_line, "out of memory building the "
			 "catalogue command");

	fyai_report(ctx, "catalog: running %s\n", request->cmd);
	/* The credentials are at the transport; only the tool child that
	 * starts the program gets them. */
	catalog_update_env_keep(ctx, env_names);
	if (fyai_transport_env_grant(ctx, env_names))
		goto err_line;
	request->session = fyai_tools_config_program(ctx, line.data, "catalog",
				ctx->cfg->catalog_update_credentials,
				catalog_update_exited, request);
	fyai_transport_env_release(ctx);
	if (!request->session)
		goto err_line;	/* fyai_tools_config_program() says why */
	free(line.data);
	return request;

err_line:
	free(line.data);
	(void)rmdir(request->dir);
err_dir:
	request->dir[0] = '\0';
err:
	if (request)
		free(request->cmd);
	free(request);
	return NULL;
}

bool fyai_catalog_update_done(
		const struct fyai_catalog_update_request *request)
{
	return request && request->done;
}

int fyai_catalog_update_collect(struct fyai_catalog_update_request *request)
{
	struct response_buffer buf = {0};
	struct fyai_ctx *ctx;
	char chunk[65536];
	size_t n;
	FILE *fp;
	int rc;

	if (!request || !request->done || request->cancelled)
		return -1;
	ctx = request->ctx;
	rc = -1;
	fyai_error_check(ctx, !request->signal, out, "%s was stopped by "
			 "signal %d; its output is in its tile", request->cmd,
			 request->signal);
	fyai_error_check(ctx, !request->exit_code, out, "%s exited with "
			 "status %d; its output is in its tile", request->cmd,
			 request->exit_code);
	fp = fopen(request->out, "r");
	fyai_error_check(ctx, fp, out, "could not read the catalogue that %s "
			 "wrote: %s", request->cmd, strerror(errno));
	rc = 0;
	while (!rc && (n = fread(chunk, 1, sizeof(chunk), fp)) > 0)
		rc = response_buffer_append_data(&buf, chunk, n);
	if (!rc && ferror(fp))
		rc = -1;
	fclose(fp);
	fyai_error_check(ctx, !rc, out, "could not read the catalogue that %s "
			 "wrote", request->cmd);
	rc = catalog_update_apply(ctx, request->cmd, buf.data ? buf.data : "",
				  buf.len, request->selected);
out:
	free(buf.data);
	return rc;
}

void fyai_catalog_update_cancel(struct fyai_catalog_update_request *request)
{
	if (!request || request->done || request->cancelled)
		return;
	request->cancelled = true;
	if (request->session)
		fyai_tools_user_program_close(request->session);
}

void fyai_catalog_update_destroy(struct fyai_catalog_update_request *request)
{
	if (!request)
		return;
	fyai_catalog_update_cancel(request);
	/* A program that is still running must not reach a freed request. */
	if (request->session)
		fyai_tools_user_program_forget(request->session);
	if (request->dir[0]) {
		(void)unlink(request->out);
		(void)rmdir(request->dir);
	}
	free(request->cmd);
	free(request);
}

int fyai_catalog_export(struct fyai_ctx *ctx, const char *path)
{
	fy_generic cat, emitted;
	const char *text;

	cat = fyai_catalog_effective(ctx->arena_catalog, ctx->cfg->gb);
	if (fy_is_invalid(cat)) {
		fyai_error(ctx, "none available");
		return -1;
	}
	emitted = fy_emit(cat,
			  FYAI_YAML_EMIT_FLAGS, NULL);
	if (fy_is_invalid(emitted))
		return -1;
	text = fy_castp(&emitted, "");
	if (write_text_file(path, text)) {
		fyai_error(ctx, "cannot write %s", path);
		return -1;
	}
	return 0;
}

int fyai_catalog_document(struct fyai_ctx *ctx, fy_generic *catp)
{
	*catp = fyai_catalog_effective(ctx->arena_catalog, ctx->cfg->gb);
	fyai_error_check(ctx, fy_is_valid(*catp), err, "none available");
	if (fy_is_invalid(ctx->arena_catalog))
		fyai_report(ctx, "# embedded snapshot (no catalog on this "
			    "branch)\n");
	return 0;
err:
	return -1;
}

static void catalog_md_cell(FILE *fp, const char *s)
{
	if (!s)
		return;
	for (; *s; s++) {
		if (*s == '|') {
			fputc('\\', fp);
		}
		fputc(*s == '\n' || *s == '\r' ? ' ' : *s, fp);
	}
}

static void catalog_tool_description(FILE *fp, const char *desc, bool full)
{
	const char *p;

	if (!desc)
		return;
	if (full) {
		catalog_md_cell(fp, desc);
		return;
	}
	p = strchr(desc, '.');
	if (p)
		p++;
	else
		p = desc + strlen(desc);
	while (desc < p) {
		if (*desc == '|')
			fputc('\\', fp);
		fputc(*desc == '\n' || *desc == '\r' || *desc == '\t' ?
		       ' ' : *desc, fp);
		desc++;
	}
}

static void catalog_tool_full_description(FILE *fp, const char *desc)
{
	if (!desc)
		return;
	fputs("  > ", fp);
	for (; *desc; desc++) {
		if (*desc == '\n' || *desc == '\r')
			fputs("\n  > ", fp);
		else
			fputc(*desc, fp);
	}
	fputs("\n", fp);
}

static void catalog_tool_schema(FILE *fp, struct fy_generic_builder *gb,
				fy_generic schema)
{
	fy_generic emitted;
	const char *json;

	if (fy_is_invalid(schema) || fy_generic_is_null(schema))
		return;
	emitted = fy_emit(gb, schema,
		FYOPEF_DISABLE_DIRECTORY |
		FYOPEF_OUTPUT_TYPE_STRING |
		FYOPEF_MODE_YAML_1_2 |
		FYOPEF_STYLE_PRETTY |
		FYOPEF_WIDTH_80 |
		FYOPEF_NO_ENDING_NEWLINE,
		NULL);
	if (fy_is_invalid(emitted))
		return;
	json = fy_castp(&emitted, "");
	fputs("\n  ```yaml\n  ", fp);
	for (; *json; json++) {
		fputc(*json == '\n' ? '\n' : *json, fp);
		if (json[0] == '\n' && json[1])
			fputs("  ", fp);
	}
	fputs("\n  ```\n", fp);
}

static void catalog_full_heading(FILE *fp, const char *name)
{
	fprintf(fp, "%s\n%.*s\n\n", name, (int)strlen(name),
		"--------------------------------------------------------------------------------");
}

static void catalog_agent_tools_markdown(FILE *mf, struct fy_generic_builder *gb,
					 fy_generic agent, bool full)
{
	fy_generic tools, key, tool, desc, schema;
	const char *name;

	if (full)
		catalog_full_heading(mf, fy_cast(fy_get(agent, "name", ""), ""));
	else
		fprintf(mf, "## %s\n\n", fy_get(agent, "name", ""));
	if (!full) {
		fprintf(mf, "| Tool | Description |\n");
		fprintf(mf, "|---|---|\n");
	}

	tools = fy_get(agent, "tools");
	if (!fy_is_mapping(tools)) {
		fprintf(mf, "\n_no tools_\n");
		return;
	}
	fy_foreach_key_value(key, tool, tools) {
		name = fy_castp(&key, "");
		desc = fy_get(tool, "description");
		schema = fy_get(tool, "schema");
		if (full) {
			fprintf(mf, "- **%s**\n\n", name);
			catalog_tool_full_description(mf, fy_castp(&desc, ""));
			catalog_tool_schema(mf, gb, schema);
			fprintf(mf, "\n");
		} else {
			fprintf(mf, "| `");
			catalog_md_cell(mf, name);
			fprintf(mf, "` | ");
			catalog_tool_description(mf, fy_castp(&desc, ""), false);
			fprintf(mf, " |\n");
		}
	}
}

static void catalog_builtin_tools_markdown(FILE *mf,
					   struct fyai_ctx *ctx,
					   bool full)
{
	struct fy_generic_builder *gb = ctx->cfg->gb;
	fy_generic tools, tool, function, name, desc, schema;

	if (full)
		catalog_full_heading(mf, "fyai");
	else {
		fprintf(mf, "## fyai tools\n\n");
		fprintf(mf, "| Tool | Description |\n");
		fprintf(mf, "|---|---|\n");
	}
	tools = make_tools(ctx);
	fy_foreach(tool, tools) {
		function = fy_get(tool, "function");
		name = fy_get(function, "name");
		desc = fy_get(function, "description");
		schema = fy_get(function, "parameters");
		if (full) {
			fprintf(mf, "- **%s**\n\n", fy_castp(&name, ""));
			catalog_tool_full_description(mf, fy_castp(&desc, ""));
			catalog_tool_schema(mf, gb, schema);
			fprintf(mf, "\n");
		} else {
			fprintf(mf, "| `");
			catalog_md_cell(mf, fy_castp(&name, ""));
			fprintf(mf, "` | ");
			catalog_tool_description(mf, fy_castp(&desc, ""), false);
			fprintf(mf, " |\n");
		}
	}
}

char *fyai_catalog_tools_markdown(struct fyai_ctx *ctx, const char *agent_name,
				  bool full)
{
	fy_generic cat, agents, a;
	char *md;
	size_t mdlen;
	FILE *mf;
	bool found;

	cat = fyai_catalog_effective(ctx->arena_catalog, ctx->cfg->gb);
	fyai_error_check(ctx, fy_is_valid(cat), err, "none available");
	md = NULL;
	mdlen = 0;
	mf = open_memstream(&md, &mdlen);
	fyai_error_check(ctx, mf, err, "cannot build the tool list");
	if (!agent_name || !*agent_name || !strcmp(agent_name, "fyai")) {
		catalog_builtin_tools_markdown(mf, ctx, full);
		fclose(mf);
		return md;
	}
	agents = fy_get(cat, "agents");
	found = false;
	fy_foreach(a, agents) {
		if (!fy_equal(fy_get(a, "name"), agent_name))
			continue;
		if (found)
			fprintf(mf, "\n");
		catalog_agent_tools_markdown(mf, ctx->cfg->gb, a, full);
		found = true;
	}
	fclose(mf);
	if (!found) {
		free(md);
		fyai_error(ctx, fy_is_sequence(agents) ?
			   "no such agent '%s'" : "no agents section; no "
			   "agent '%s'", agent_name);
		return NULL;
	}
	return md;
err:
	return NULL;
}

fy_generic fyai_catalog_list_data(struct fyai_ctx *ctx,
				  struct fy_generic_builder *gb,
				  const char *what)
{
	fy_generic cat, rows, m, p, e, protocols;

	cat = fyai_catalog_effective(ctx->arena_catalog, ctx->cfg->gb);
	fyai_error_check(ctx, fy_is_valid(cat), err, "none available");
	rows = fy_seq_empty;
	if (!strcmp(what, "providers")) {
		fy_foreach(p, fy_get(cat, "providers", fy_invalid)) {
			protocols = fy_seq_empty;
			fy_foreach(e, fy_get(p, "endpoints", fy_invalid))
				protocols = fy_append(gb, protocols,
					fy_get(e, "protocol", fy_invalid));
			rows = fy_append(gb, rows, fy_mapping(gb,
				"name", fy_get(p, "name", fy_invalid),
				"root_url", fy_get(p, "root_url", ""),
				"protocols", protocols));
		}
		return rows;
	}
	fy_foreach(m, fy_get(cat, "models", fy_invalid))
		rows = fy_append(gb, rows, fy_mapping(gb,
			"name", fy_get(m, "name", fy_invalid),
			"context_window", fy_get(m, "context_window", 0LL),
			"max_output_tokens", fy_get(m, "max_output_tokens",
						    0LL)));
	return rows;
err:
	return fy_invalid;
}
