/*
 * fyai_catalog.h - provider/model catalogue access
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_CATALOG_H
#define FYAI_CATALOG_H

#include "fyai.h"

/*
 * The catalogue is the scrape-providers YAML document (models / providers /
 * agents sections), stored verbatim as the container root's catalog entry.
 * When the arena carries none, a snapshot embedded at build time is used.
 */

/*
 * The effective catalogue: the arena document when valid, else the embedded
 * snapshot, parsed one time into a builder that lives as long as the process;
 * @gb is not used. fy_invalid only on parse failure.
 */
fy_generic fyai_catalog_effective(fy_generic arena_catalog,
				  struct fy_generic_builder *gb);

/* models[] entry with name == @model, or fy_invalid */
fy_generic fyai_catalog_model(fy_generic cat, const char *model);

/* models[] entry, also accepting provider_model_id via providers[].models[] */
fy_generic fyai_catalog_resolved_model(fy_generic cat, const char *model);

/* True when @model_entry lists @cap in the controlled capabilities[]. */
bool fyai_catalog_model_has_cap(fy_generic model_entry, const char *cap);

static inline bool fyai_model_supports_temperature(fy_generic model_entry)
{
	return fy_is_invalid(model_entry) ||
	       (!fyai_catalog_model_has_cap(model_entry, "reasoning") &&
		!fyai_catalog_model_has_cap(model_entry, "reasoning_effort"));
}

/*
 * Reasoning models reject (or silently omit) logprobs, so token-extents
 * collection must not inject the params for them; the fallback chunk-extents
 * path applies instead. Same capability test as temperature today.
 */
static inline bool fyai_model_supports_logprobs(fy_generic model_entry)
{
	return fyai_model_supports_temperature(model_entry);
}

/*
 * The providers[] entry offering @model (by canonical_id or
 * provider_model_id). Optionally hands back the offering entry itself
 * (canonical_id/provider_model_id/pricing) via @offeringp.
 */
fy_generic fyai_catalog_provider_for_model(fy_generic cat, const char *model,
					   fy_generic *offeringp);

/* providers[] entry named @name, or fy_invalid */
fy_generic fyai_catalog_provider(fy_generic cat, const char *name);

/*
 * The offering of @provider for @model (matched by canonical_id or
 * provider_model_id), also handed back via @offeringp. fy_invalid when the
 * provider does not offer the model.
 */
fy_generic fyai_catalog_offering(fy_generic provider, const char *model,
				 fy_generic *offeringp);

/* endpoints[] entry of @provider for @api mode, or fy_invalid */
fy_generic fyai_catalog_endpoint(fy_generic provider, enum fyai_api_mode api);

/* True when an endpoint advertises a provider-hosted tool named @tool. */
bool fyai_catalog_endpoint_has_hosted_tool(fy_generic endpoint,
						const char *tool);

/*
 * The tool type that an endpoint declares the hosted tool @tool with, or
 * fy_invalid for the default of its protocol.
 */
fy_generic fyai_catalog_endpoint_hosted_tool_type(fy_generic endpoint,
						  const char *tool);

/*
 * The embedded catalogue schema, parsed one time into a builder that lives
 * as long as the process; @gb is not used.
 */
fy_generic fyai_catalog_schema(struct fy_generic_builder *gb);

/*
 * Check @doc against the catalogue schema and make it the catalogue of the
 * branch. fy_null removes the catalogue of the branch, which then uses the
 * embedded one. The model_info block of the configuration follows.
 */
/* @store, when valid, is published with the catalogue in one root. */
int fyai_catalog_commit(struct fyai_ctx *ctx, fy_generic doc,
			const char *origin, fy_generic store);

/* verb backends */
int fyai_catalog_import(struct fyai_ctx *ctx, const char *path);
/* The value at the slash path @path of the catalogue of the branch. */
int fyai_catalog_value(struct fyai_ctx *ctx, const char *path,
		       fy_generic *valuep);
int fyai_catalog_set(struct fyai_ctx *ctx, const char *path, const char *value);
int fyai_catalog_delete(struct fyai_ctx *ctx, const char *path);
/* Build and validate model and offering changes without publishing them. */
int fyai_catalog_patch_models_prepare(struct fyai_ctx *ctx,
		fy_generic changes, fy_generic *docp);
int fyai_catalog_validate(struct fyai_ctx *ctx);
int fyai_catalog_reset(struct fyai_ctx *ctx);
/*
 * Run catalog_update/command and commit what it writes. With @count provider
 * names, merge only the providers and models that it describes.
 */
int fyai_catalog_update(struct fyai_ctx *ctx, const char *const *providers,
			size_t count, bool curated);

/* Most names catalog_update/credentials gives the command. */
#define FYAI_CATALOG_ENV_KEEP_MAX	32
/* Most providers one session update selects. */
#define FYAI_CATALOG_UPDATE_PROVIDERS_MAX	32

/*
 * Start catalog_update/command in a tile of the work pane, for a session. The
 * program ends on its own; fyai_catalog_update_collect() commits what it
 * wrote. Collect it outside an event callback, between turns.
 */
struct fyai_catalog_update_request;
struct fyai_catalog_update_request *
fyai_catalog_update_submit(struct fyai_ctx *ctx, const char *const *providers,
			   size_t count, bool curated);
bool fyai_catalog_update_done(
		const struct fyai_catalog_update_request *request);
int fyai_catalog_update_collect(struct fyai_catalog_update_request *request);
void fyai_catalog_update_cancel(struct fyai_catalog_update_request *request);
void fyai_catalog_update_destroy(struct fyai_catalog_update_request *request);
/* Write the catalogue of the branch as YAML to the file @path. */
int fyai_catalog_export(struct fyai_ctx *ctx, const char *path);
/*
 * The catalogue of the branch, else the embedded snapshot; a notice says
 * which. The value is held by the arena or by the process.
 */
int fyai_catalog_document(struct fyai_ctx *ctx, fy_generic *catp);
/*
 * The value at the slash path @path of the catalogue of the branch, else of
 * the embedded snapshot: an item of a list is named by its name, as in
 * `providers/openai/models/gpt-5`. fy_invalid, with nothing reported, when
 * the path leads nowhere. The value is held by the arena or by the process.
 */
fy_generic fyai_catalog_at(struct fyai_ctx *ctx, const char *path);
/* The value at @path of @doc, borrowed from @doc. */
fy_generic fyai_catalog_doc_at(fy_generic doc, const char *path);

/*
 * The models, or with @what "providers" the providers, of the catalogue.
 * With @available only those with a credential here. Each model row names
 * in key_provider a provider that offers it and has one, else "".
 */
fy_generic fyai_catalog_list_data(struct fyai_ctx *ctx,
				  struct fy_generic_builder *gb,
				  const char *what, bool available);
/*
 * The tools of the coding agent @agent, or of fyai itself, as Markdown; the
 * caller frees it. NULL with the cause raised.
 */
char *fyai_catalog_tools_markdown(struct fyai_ctx *ctx, const char *agent,
				  bool full);

#endif
