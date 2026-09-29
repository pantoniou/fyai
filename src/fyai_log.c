/*
 * fyai_log.c - YAML trace logs
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_LOG

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "fyai_sink.h"
#include "fyai_diag.h"
#include "fyai_log.h"
#include "fyai_redact.h"
#include "fyai_ui.h"

static char *fyai_log_path(struct fyai_ctx *ctx, const char *name)
{
	struct fyai_cfg *cfg = ctx->cfg;
	const char *arena;
	char *base, *slash, *path;

	if (!cfg->arena_dir || !name)
		return NULL;

	arena = cfg->arena_dir;
	base = strdup(arena);
	if (!base)
		return NULL;
	slash = strrchr(base, '/');
	if (slash)
		*slash = '\0';

	if (fyai_mkdir_p(fy_sprintfa("%s/logs", base))) {
		free(base);
		return NULL;
	}

	if (asprintf(&path, "%s/logs/%s.yaml", base, name) < 0)
		path = NULL;
	free(base);
	return path;
}

static int fyai_log_truncate(struct fyai_ctx *ctx, const char *name)
{
	char *path;
	FILE *fp;

	path = fyai_log_path(ctx, name);
	if (!path)
		return -1;
	fp = fopen(path, "w");
	free(path);
	if (!fp)
		return -1;
	fclose(fp);
	return 0;
}

static int fyai_log_view_target(struct fyai_ctx *ctx, const char *target)
{
	char *path;
	FILE *fp;
	int ret;
	int rc;
	bool ui_external = false;

	path = fyai_log_path(ctx, target);
	fyai_error_check(ctx, path, err_out, "cannot resolve log path");
	fp = fopen(path, "a");
	fyai_error_check(ctx, fp, err_path, "cannot open log %s", path);
	fclose(fp);
	/* A viewer in the work pane keeps the UI; one on the terminal takes it. */
	if (!fyai_editor_in_pane(ctx)) {
		ret = fyai_ui_external_begin(ctx);
		fyai_error_check(ctx, !ret, err_path,
				 "cannot suspend UI for log viewer");
		ui_external = true;
	}
	ret = fyai_spawn_editor_readonly(ctx, path);
	if (ui_external) {
		rc = fyai_ui_external_end(ctx);
		ui_external = false;
		fyai_error_check(ctx, !rc, err_path,
				 "cannot resume UI after log viewer");
	}
	fyai_error_check(ctx, !ret, err_path, "log viewer failed");
	free(path);
	return 0;
err_path:
	if (ui_external)
		(void)fyai_ui_external_end(ctx);
	free(path);
err_out:
	return -1;
}

int fyai_log_clear(struct fyai_ctx *ctx)
{
	int ret;

	ret = fyai_log_truncate(ctx, "wire");
	if (fyai_log_truncate(ctx, "stream"))
		ret = -1;
	if (fyai_log_truncate(ctx, "conversation"))
		ret = -1;
	if (fyai_log_truncate(ctx, "mcp"))
		ret = -1;
	if (fyai_log_truncate(ctx, "transport"))
		ret = -1;
	return ret;
}

static int fyai_log_clear_target(struct fyai_ctx *ctx, const char *target)
{
	if (!strcmp(target, "all"))
		return fyai_log_clear(ctx);
	return fyai_log_truncate(ctx, target);
}

fy_generic fyai_log_status_data(struct fyai_ctx *ctx,
				struct fy_generic_builder *gb)
{
	const struct fyai_cfg *cfg = ctx->cfg;

	return fy_mapping(gb,
		"wire", cfg->wire_logging,
		"stream", cfg->stream_logging,
		"conversation", cfg->conversation_logging,
		"mcp", cfg->mcp_logging,
		"transport", cfg->transport_logging);
}

void fyai_log_set(struct fyai_cfg *cfg, const char *target, bool on)
{
	if (!strcmp(target, "wire") || !strcmp(target, "all"))
		cfg->wire_logging = on;
	if (!strcmp(target, "stream") || !strcmp(target, "all"))
		cfg->stream_logging = on;
	if (!strcmp(target, "conversation") || !strcmp(target, "all"))
		cfg->conversation_logging = on;
	if (!strcmp(target, "mcp") || !strcmp(target, "all"))
		cfg->mcp_logging = on;
	if (!strcmp(target, "transport") || !strcmp(target, "all"))
		cfg->transport_logging = on;
}

int fyai_log_view(struct fyai_ctx *ctx, const char *target)
{
	if (strcmp(target, "all"))
		return fyai_log_view_target(ctx, target);
	if (fyai_log_view_target(ctx, "wire") ||
	    fyai_log_view_target(ctx, "stream") ||
	    fyai_log_view_target(ctx, "conversation") ||
	    fyai_log_view_target(ctx, "mcp") ||
	    fyai_log_view_target(ctx, "transport"))
		return -1;
	return 0;
}

int fyai_log_clear_log(struct fyai_ctx *ctx, const char *target)
{
	if (fyai_log_clear_target(ctx, target)) {
		fyai_error(ctx, "clear failed");
		return -1;
	}
	return 0;
}

int fyai_log_generic(struct fyai_ctx *ctx, const char *name, fy_generic doc)
{
	struct fyai_cfg *cfg = ctx->cfg;
	char *path;
	const char *out;
	FILE *fp;
	struct fy_generic_builder *gb;
	fy_generic emitted;
	int ret = -1;

	if (!strcmp(name, "wire") && !cfg->wire_logging)
		return 0;
	if (!strcmp(name, "stream") && !cfg->stream_logging)
		return 0;
	if (!strcmp(name, "conversation") && !cfg->conversation_logging)
		return 0;
	if (!strcmp(name, "mcp") && !cfg->mcp_logging)
		return 0;
	if (!strcmp(name, "transport") && !cfg->transport_logging)
		return 0;

	path = fyai_log_path(ctx, name);
	if (!path)
		return -1;

	gb = ctx->transient_gb ? ctx->transient_gb : cfg->gb;
	emitted = fy_emit(gb, doc,
		FYOPEF_DISABLE_DIRECTORY |
		FYOPEF_OUTPUT_TYPE_STRING |
		FYOPEF_MODE_YAML_1_2 |
		FYOPEF_STYLE_PRETTY |
		FYOPEF_WIDTH_INF, &out);
	if (fy_is_invalid(emitted))
		goto out;
	out = fy_castp(&emitted, "");

	fp = fopen(path, "a");
	if (!fp)
		goto out;
	if (fprintf(fp, "---\n") < 0)
		goto close;
	if (out && fputs(out, fp) < 0)
		goto close;
	ret = ferror(fp) ? -1 : 0;
close:
	fclose(fp);
out:
	free(path);
	return ret;
}

void fyai_log_wire_text(struct fyai_ctx *ctx, const char *type,
			const char *data, size_t size)
{
	struct fyai_cfg *cfg = ctx->cfg;
	struct fy_generic_builder *gb;
	struct fyai_redactor redactor;
	fy_generic doc;
	char *copy = NULL;
	const char *log_data;

	if (!data)
		return;
	log_data = data;
	if (cfg->whitewash_api_keys) {
		copy = malloc(size + 1);
		if (!copy)
			return;
		memcpy(copy, data, size);
		copy[size] = '\0';
		/* Every record can carry a key: a header, an echo, an error. */
		fyai_redactor_init(&redactor);
		if (cfg->api_key)
			(void)fyai_redactor_add_secret(&redactor, cfg->api_key);
		fyai_redact(&redactor, copy, size);
		fyai_redactor_clear(&redactor);
		log_data = copy;
	}
	gb = ctx->transient_gb ? ctx->transient_gb : cfg->gb;
	doc = fy_mapping(gb,
		"kind", "curl",
		"type", type ? type : "",
		"data", fy_string_size(log_data, size));
	(void)fyai_log_generic(ctx, "wire", doc);
	free(copy);
}

static const char *curl_debug_type_name(curl_infotype type)
{
	switch (type) {
	case CURLINFO_TEXT:
		return "text";
	case CURLINFO_HEADER_IN:
		return "header_in";
	case CURLINFO_HEADER_OUT:
		return "header_out";
	case CURLINFO_DATA_IN:
		return "data_in";
	case CURLINFO_DATA_OUT:
		return "data_out";
	case CURLINFO_SSL_DATA_IN:
		return "ssl_data_in";
	case CURLINFO_SSL_DATA_OUT:
		return "ssl_data_out";
	case CURLINFO_END:
		return "end";
	}
	return "unknown";
}

int fyai_curl_debug(CURL *curl, curl_infotype type, char *data,
		    size_t size, void *userdata)
{
	struct fyai_ctx *ctx = userdata;
	struct fyai_cfg *cfg = ctx->cfg;

	(void)curl;
	if (cfg->wire_logging)
		fyai_log_wire_text(ctx, curl_debug_type_name(type), data, size);
	return 0;
}
