/*
 * fyai_cmd_catalog.c - handlers of the catalog command
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * A handler returns what the catalogue holds for the caller to present, and
 * applies a change to a live session.
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
#include "fyai_catalog.h"
#include "fyai_config.h"

#define FYAI_MODULE FYAIEM_CATALOG

/* The model_info block and the model resolution follow the catalogue. */
static int catalog_changed(struct fyai_cmd_call *call, int rc)
{
	if (rc || call->surface != FYAI_CMD_SESSION)
		return rc;
	return fyai_config_rederive(call->ctx);
}

int fyai_cmd_catalog_show(struct fyai_cmd_call *call, fy_generic *result)
{
	return fyai_catalog_document(call->ctx, result);
}

int fyai_cmd_catalog_list(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *what = fyai_cmd_arg_str(call, "what");

	*result = fyai_catalog_list_data(call->ctx, call->gb, what,
					 fyai_cmd_arg_bool(call, "available"));
	if (!fy_is_valid(*result))
		return -1;
	call->renderopts = !strcmp(what, "providers") ?
		fy_mapping(call->gb, "title", "Providers",
			   "keys", fy_sequence(call->gb, "name", "root_url",
					       "protocols"),
			   "columns", fy_mapping(call->gb,
				"root_url", fy_mapping(call->gb, "name",
						       "Root URL"))) :
		fy_mapping(call->gb, "title", "Models",
			   "keys", fy_sequence(call->gb, "name",
					       "context_window",
					       "max_output_tokens",
					       "key_provider"));
	return 0;
}

/* The tools are prose: the result is their Markdown. */
int fyai_cmd_catalog_tools(struct fyai_cmd_call *call, fy_generic *result)
{
	char *md;

	md = fyai_catalog_tools_markdown(call->ctx,
					 fyai_cmd_arg_str(call, "agent"),
					 fyai_cmd_arg_bool(call, "full"));
	if (!md)
		return -1;
	*result = fy_value(call->gb, (const char *)md);
	free(md);
	return fy_is_valid(*result) ? 0 : -1;
}

int fyai_cmd_catalog_get(struct fyai_cmd_call *call, fy_generic *result)
{
	return fyai_catalog_value(call->ctx, fyai_cmd_arg_str(call, "path"),
				  result);
}

int fyai_cmd_catalog_set(struct fyai_cmd_call *call, fy_generic *result)
{
	(void)result;
	return catalog_changed(call, fyai_catalog_set(call->ctx,
					fyai_cmd_arg_str(call, "path"),
					fyai_cmd_arg_str(call, "value")));
}

int fyai_cmd_catalog_delete(struct fyai_cmd_call *call, fy_generic *result)
{
	(void)result;
	return catalog_changed(call, fyai_catalog_delete(call->ctx,
					fyai_cmd_arg_str(call, "path")));
}

int fyai_cmd_catalog_import(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *file = fyai_cmd_arg_str(call, "file");
	fy_generic cat;

	if (catalog_changed(call, fyai_catalog_import(call->ctx, file)))
		return -1;
	cat = call->ctx->bstate->catalog;
	*result = fy_mapping(call->gb, "file", file,
			     "models", (long long)fy_len(fy_get(cat, "models",
								fy_seq_empty)),
			     "providers", (long long)fy_len(fy_get(cat,
					"providers", fy_seq_empty)));
	return 0;
}

int fyai_cmd_catalog_export(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *file = fyai_cmd_arg_str(call, "file");

	/* Standard output takes the catalogue as a document. */
	if (!file) {
		*result = fyai_catalog_effective(call->ctx->bstate->catalog,
						 call->ctx->cfg->gb);
		return fy_is_valid(*result) ? 0 : -1;
	}
	return fyai_catalog_export(call->ctx, file);
}

int fyai_cmd_catalog_validate(struct fyai_cmd_call *call, fy_generic *result)
{
	if (fyai_catalog_validate(call->ctx))
		return -1;
	*result = fy_mapping(call->gb, "valid", true);
	return 0;
}

int fyai_cmd_catalog_schema(struct fyai_cmd_call *call, fy_generic *result)
{
	*result = fyai_catalog_schema(call->gb);
	fyai_error_check(call->ctx, fy_is_valid(*result), err,
			 "the catalogue schema does not load");
	return 0;
err:
	return -1;
}

int fyai_cmd_catalog_reset(struct fyai_cmd_call *call, fy_generic *result)
{
	(void)result;
	if (catalog_changed(call, fyai_catalog_reset(call->ctx)))
		return -1;
	*result = fy_mapping(call->gb, "catalog", "embedded");
	return 0;
}

/* A session edits in a tile of the work pane and applies between turns. */
int fyai_cmd_catalog_edit(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = call->ctx;

	(void)result;
	if (call->surface == FYAI_CMD_CLI)
		return fyai_catalog_edit(ctx);
	fyai_error_check(ctx, !ctx->front->config_edit, err,
			 "an editor is already active");
	ctx->front->config_edit = fyai_catalog_edit_submit(ctx);
	return ctx->front->config_edit ? 0 : -1;
err:
	return -1;
}

/*
 * The verb waits for the update program. A session runs it in a tile and
 * commits the output between turns.
 */
int fyai_cmd_catalog_update(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *providers[FYAI_CATALOG_UPDATE_PROVIDERS_MAX];
	struct fyai_ctx *ctx = call->ctx;
	bool curated = fyai_cmd_arg_bool(call, "curated");
	fy_generic p;
	size_t n;

	(void)result;
	n = 0;
	/* A provider is named with --provider, or as a word. */
	fy_foreach(p, fy_concat(call->gb,
				fy_get(call->args, "provider", fy_seq_empty),
				fy_get(call->args, "names", fy_seq_empty))) {
		fyai_error_check(ctx, n < ARRAY_SIZE(providers), err,
				 "catalog: at most %zu providers in one update",
				 ARRAY_SIZE(providers));
		providers[n++] = fy_gb_intern_string(ctx->cfg->gb,
						     fy_castp(&p, ""));
	}
	if (call->surface == FYAI_CMD_CLI)
		return fyai_catalog_update(ctx, providers, n, curated);
	fyai_error_check(ctx, !ctx->front->catalog_update, err,
			 "catalog: an update is already running");
	ctx->front->catalog_update = fyai_catalog_update_submit(ctx, providers, n,
							 curated);
	return ctx->front->catalog_update ? 0 : -1;
err:
	return -1;
}
