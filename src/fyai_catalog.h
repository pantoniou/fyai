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

/* The effective catalogue: the arena document when valid, else the embedded
 * snapshot parsed into @gb (cached per process). fy_invalid only on parse
 * failure. */
fy_generic fyai_catalog_effective(fy_generic arena_catalog,
				  struct fy_generic_builder *gb);

/* models[] entry with name == @model, or fy_invalid */
fy_generic fyai_catalog_model(fy_generic cat, const char *model);

/* models[] entry, also accepting provider_model_id via providers[].models[] */
fy_generic fyai_catalog_resolved_model(fy_generic cat, const char *model);

/* true when @model_entry lists @cap in capabilities[] */
bool fyai_catalog_model_has_cap(fy_generic model_entry, const char *cap);

static inline bool fyai_model_supports_temperature(fy_generic model_entry)
{
	return fy_generic_is_invalid(model_entry) ||
	       (!fyai_catalog_model_has_cap(model_entry, "thinking") &&
		!fyai_catalog_model_has_cap(model_entry, "effort") &&
		!fyai_catalog_model_has_cap(model_entry, "reasoning") &&
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

/* verb backends */
int fyai_catalog_import(struct fyai_ctx *ctx, const char *path);
int fyai_catalog_show(struct fyai_ctx *ctx);
int fyai_catalog_list(struct fyai_ctx *ctx, const char *what);

#endif
