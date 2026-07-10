/*
 * fyai_catalog.c - provider/model catalogue (scrape-providers document)
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai_catalog.h"
#include "fyai_config.h"
#include "fyai_markdown.h"
#include "fyai_storage.h"
#include "utils.h"

/* FYAI_EMBEDDED_CATALOG[] / FYAI_EMBEDDED_CATALOG_LEN - the vendored
 * data/catalog.yaml snapshot, generated at configure time. */
#include "embedded_catalog.inc"

static fy_generic embedded_catalog = fy_invalid;

fy_generic fyai_catalog_effective(fy_generic arena_catalog,
				  struct fy_generic_builder *gb)
{
	fy_generic_sized_string embedded;

	if (fy_generic_is_valid(arena_catalog))
		return arena_catalog;
	if (fy_generic_is_valid(embedded_catalog))
		return embedded_catalog;
	embedded.data = (const char *)FYAI_EMBEDDED_CATALOG;
	embedded.size = FYAI_EMBEDDED_CATALOG_LEN;
	embedded_catalog = fy_parse(gb, embedded,
				    FYOPPF_DISABLE_DIRECTORY |
				    FYOPPF_MODE_YAML_1_2 |
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
	if (fy_generic_is_valid(cat_model))
		return cat_model;

	fyai_catalog_provider_for_model(cat, model, &cat_offer);
	if (fy_generic_is_valid(cat_offer))
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

	if (!name)
		return fy_invalid;
	providers = fy_get(cat, "providers");
	fy_foreach(p, providers) {
		if (fy_equal(fy_get(p, "name"), name))
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

int fyai_catalog_import(struct fyai_ctx *ctx, const char *path)
{
	fy_generic doc, models, providers, new_config;

	if (!ctx->durable_gb) {
		fprintf(stderr, "catalog: no arena; run fyai init\n");
		return -1;
	}
	doc = fy_parse_file(ctx->gb,
			    FYOPPF_DISABLE_DIRECTORY | FYOPPF_MODE_YAML_1_2,
			    path);
	if (!fy_generic_is_mapping(doc)) {
		fprintf(stderr, "catalog: cannot parse %s\n", path);
		return -1;
	}
	models = fy_get(doc, "models");
	providers = fy_get(doc, "providers");
	if (!fy_generic_is_sequence(models) ||
	    !fy_generic_is_sequence(providers)) {
		fprintf(stderr,
			"catalog: %s lacks models:/providers: sections\n",
			path);
		return -1;
	}
	/*
	 * A new catalogue can change or drop the read-only catalog: block
	 * (canonical_provider, open_source) on the currently configured
	 * model, so re-derive it against the incoming catalogue rather than
	 * whatever it was pinned against before.
	 */
	new_config = fy_generic_is_valid(ctx->arena_config) ?
		fyai_config_sync_catalog(ctx->gb, doc, ctx->arena_config) :
		fy_invalid;
	if (fyai_publish_root(ctx, new_config, doc, fy_invalid))
		return -1;
	printf("catalog: imported %s (%zu models, %zu providers)\n",
	       path, fy_len(models), fy_len(providers));
	return 0;
}

int fyai_catalog_export(struct fyai_ctx *ctx, const char *path)
{
	fy_generic cat, emitted;
	const char *text;

	cat = fyai_catalog_effective(ctx->arena_catalog, ctx->cfg->gb);
	if (fy_generic_is_invalid(cat)) {
		fprintf(stderr, "catalog: none available\n");
		return -1;
	}
	if (!path) {
		emit_generic_to_stdout(NULL, cat, true);
		return 0;
	}
	emitted = fy_emit(cat,
			  FYOPEF_DISABLE_DIRECTORY | FYOPEF_MODE_YAML_1_2 |
			  FYOPEF_STYLE_PRETTY | FYOPEF_WIDTH_INF, NULL);
	if (fy_generic_is_invalid(emitted))
		return -1;
	text = fy_castp(&emitted, "");
	if (write_text_file(path, text)) {
		fprintf(stderr, "catalog: cannot write %s\n", path);
		return -1;
	}
	return 0;
}

int fyai_catalog_show(struct fyai_ctx *ctx)
{
	fy_generic cat;

	cat = fyai_catalog_effective(ctx->arena_catalog, ctx->cfg->gb);
	if (fy_generic_is_invalid(cat)) {
		fprintf(stderr, "catalog: none available\n");
		return -1;
	}
	if (fy_generic_is_invalid(ctx->arena_catalog))
		fprintf(stderr, "# embedded snapshot (no catalog in arena)\n");
	emit_generic_to_stdout(NULL, cat, true);
	return 0;
}

static void catalog_list_models(fy_generic cat)
{
	fy_generic models, m;

	models = fy_get(cat, "models");
	printf("%-40s %10s %10s\n", "MODEL", "CONTEXT", "MAX-OUT");
	fy_foreach(m, models) {
		printf("%-40s %10lld %10lld\n",
		       fy_get(m, "name", ""),
		       fy_get(m, "context_window", 0LL),
		       fy_get(m, "max_output_tokens", 0LL));
	}
}

static void catalog_list_providers(fy_generic cat)
{
	fy_generic providers, p, eps, e;
	size_t j;

	providers = fy_get(cat, "providers");
	printf("%-14s %-40s %s\n", "PROVIDER", "ROOT-URL", "PROTOCOLS");
	fy_foreach(p, providers) {
		printf("%-14s %-40s ",
		       fy_get(p, "name", ""), fy_get(p, "root_url", ""));
		eps = fy_get(p, "endpoints");
		j = 0;
		fy_foreach(e, eps) {
			printf("%s%s", j ? "," : "",
			       fy_get(e, "protocol", ""));
			j++;
		}
		printf("\n");
	}
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

	if (fy_generic_is_invalid(schema) || fy_generic_is_null(schema))
		return;
	emitted = fy_emit(gb, schema,
		FYOPEF_DISABLE_DIRECTORY |
		FYOPEF_OUTPUT_TYPE_STRING |
		FYOPEF_MODE_YAML_1_2 |
		FYOPEF_STYLE_PRETTY |
		FYOPEF_WIDTH_80 |
		FYOPEF_NO_ENDING_NEWLINE,
		NULL);
	if (fy_generic_is_invalid(emitted))
		return;
	json = fy_cast(emitted, "");
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
	size_t i, n;

	if (full)
		catalog_full_heading(mf, fy_cast(fy_get(agent, "name", ""), ""));
	else
		fprintf(mf, "## %s\n\n", fy_get(agent, "name", ""));
	if (!full) {
		fprintf(mf, "| Tool | Description |\n");
		fprintf(mf, "|---|---|\n");
	}

	tools = fy_get(agent, "tools");
	if (!fy_generic_is_mapping(tools)) {
		fprintf(mf, "\n_no tools_\n");
		return;
	}
	n = fy_generic_mapping_get_pair_count(tools);
	for (i = 0; i < n; i++) {
		key = fy_generic_mapping_get_at_key(tools, i);
		tool = fy_generic_mapping_get_at_value(tools, i);
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
					   struct fy_generic_builder *gb,
					   bool full)
{
	fy_generic tools, tool, function, name, desc, schema;

	if (full)
		catalog_full_heading(mf, "fyai");
	else {
		fprintf(mf, "## fyai tools\n\n");
		fprintf(mf, "| Tool | Description |\n");
		fprintf(mf, "|---|---|\n");
	}
	tools = make_tools(gb);
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

int fyai_catalog_tools(struct fyai_ctx *ctx, const char *agent_name, bool full)
{
	fy_generic cat, agents, a;
	char *md;
	size_t mdlen;
	FILE *mf;
	bool found;
	int rc;

	cat = fyai_catalog_effective(ctx->arena_catalog, ctx->cfg->gb);
	if (fy_generic_is_invalid(cat)) {
		fprintf(stderr, "catalog: none available\n");
		return -1;
	}
	if (!agent_name || !*agent_name || !strcmp(agent_name, "fyai")) {
		md = NULL;
		mdlen = 0;
		mf = open_memstream(&md, &mdlen);
		if (!mf)
			return -1;
		catalog_builtin_tools_markdown(mf, ctx->cfg->gb, full);
		fclose(mf);
		rc = 0;
		if (ctx->cfg->markdown) {
			rc = fyai_print_markdown(md, ctx->cfg);
			if (rc)
				fputs(md, stdout);
		} else {
			fputs(md, stdout);
		}
		free(md);
		return 0;
	}
	agents = fy_get(cat, "agents");
	if (!fy_generic_is_sequence(agents)) {
		fprintf(stderr, "catalog: no agents section\n");
		return -1;
	}

	md = NULL;
	mdlen = 0;
	mf = open_memstream(&md, &mdlen);
	if (!mf)
		return -1;

	found = false;
	fy_foreach(a, agents) {
		if (agent_name && *agent_name &&
		    !fy_equal(fy_get(a, "name"), agent_name))
			continue;
		if (found)
			fprintf(mf, "\n");
		catalog_agent_tools_markdown(mf, ctx->cfg->gb, a, full);
		found = true;
	}
	fclose(mf);

	if (!found) {
		fprintf(stderr, "catalog: no such agent '%s'\n", agent_name);
		free(md);
		return -1;
	}
	rc = 0;
	if (ctx->cfg->markdown) {
		rc = fyai_print_markdown(md, ctx->cfg);
		if (rc)
			fputs(md, stdout);
	} else {
		fputs(md, stdout);
	}
	free(md);
	return 0;
}

int fyai_catalog_list(struct fyai_ctx *ctx, const char *what)
{
	fy_generic cat;

	cat = fyai_catalog_effective(ctx->arena_catalog, ctx->cfg->gb);
	if (fy_generic_is_invalid(cat)) {
		fprintf(stderr, "catalog: none available\n");
		return -1;
	}
	if (!what || !strcmp(what, "models"))
		catalog_list_models(cat);
	if (!what || !strcmp(what, "providers"))
		catalog_list_providers(cat);
	return 0;
}
