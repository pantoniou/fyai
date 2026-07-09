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
#include "fyai_storage.h"

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
	fy_generic doc, models, providers;

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
	if (fyai_publish_root(ctx, fy_invalid, doc, fy_invalid))
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
