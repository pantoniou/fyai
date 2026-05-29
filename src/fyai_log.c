/*
 * fyai_log.c - YAML trace logs
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
#include <strings.h>

#include "fyai_log.h"

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

	path = fyai_log_path(ctx, target);
	if (!path)
		return -1;
	fp = fopen(path, "a");
	if (!fp) {
		free(path);
		return -1;
	}
	fclose(fp);
	ret = fyai_spawn_editor_readonly(path);
	free(path);
	return ret;
}

int fyai_log_clear(struct fyai_ctx *ctx)
{
	int ret;

	ret = fyai_log_truncate(ctx, "wire");
	if (fyai_log_truncate(ctx, "stream"))
		ret = -1;
	if (fyai_log_truncate(ctx, "conversation"))
		ret = -1;
	return ret;
}

static int fyai_log_clear_target(struct fyai_ctx *ctx, const char *target)
{
	if (!strcmp(target, "all"))
		return fyai_log_clear(ctx);
	return fyai_log_truncate(ctx, target);
}

static void fyai_log_print(struct fyai_cfg *cfg)
{
	printf("logging: wire %s, stream %s, conversation %s\n",
	       cfg->wire_logging ? "on" : "off",
	       cfg->stream_logging ? "on" : "off",
	       cfg->conversation_logging ? "on" : "off");
}

static void fyai_log_set(struct fyai_cfg *cfg, const char *target, bool on)
{
	if (!strcmp(target, "wire") || !strcmp(target, "all"))
		cfg->wire_logging = on;
	if (!strcmp(target, "stream") || !strcmp(target, "all"))
		cfg->stream_logging = on;
	if (!strcmp(target, "conversation") || !strcmp(target, "all"))
		cfg->conversation_logging = on;
}

int fyai_log_control(struct fyai_ctx *ctx, const char *arg)
{
	struct fyai_cfg *cfg = ctx->cfg;
	char first[32], second[32], extra[2];
	const char *target, *action;
	int n;

	if (!arg || !*arg) {
		fyai_log_print(cfg);
		return 0;
	}

	first[0] = second[0] = extra[0] = '\0';
	n = sscanf(arg, "%31s %31s %1s", first, second, extra);
	if (n < 1 || n > 2 || extra[0]) {
		fprintf(stderr, "logging: use [wire|stream|conversation|all] start|stop|clear|view\n");
		return -1;
	}

	if (!strcmp(first, "wire") || !strcmp(first, "stream") ||
	    !strcmp(first, "conversation") || !strcmp(first, "all")) {
		target = first;
		action = n == 2 ? second : "";
	} else {
		if (n == 2) {
			fprintf(stderr, "logging: use [wire|stream|conversation|all] start|stop|clear|view\n");
			return -1;
		}
		target = "all";
		action = first;
	}

	if (!strcmp(action, "start") || !strcmp(action, "on")) {
		fyai_log_set(cfg, target, true);
		fyai_log_print(cfg);
		return 0;
	}
	if (!strcmp(action, "stop") || !strcmp(action, "off")) {
		fyai_log_set(cfg, target, false);
		fyai_log_print(cfg);
		return 0;
	}
	if (!strcmp(action, "clear")) {
		if (fyai_log_clear_target(ctx, target)) {
			fprintf(stderr, "logging: clear failed\n");
			return -1;
		}
		printf("logging: cleared %s\n", target);
		return 0;
	}
	if (!strcmp(action, "view")) {
		if (!strcmp(target, "all")) {
			if (fyai_log_view_target(ctx, "wire") ||
			    fyai_log_view_target(ctx, "stream") ||
			    fyai_log_view_target(ctx, "conversation"))
				return -1;
		} else if (fyai_log_view_target(ctx, target)) {
			return -1;
		}
		return 0;
	}

	fprintf(stderr, "logging: use [wire|stream|conversation|all] start|stop|clear|view\n");
	return -1;
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
	if (fy_generic_is_invalid(emitted))
		goto out;
	out = fy_cast(emitted, "");

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
	fy_generic doc;
	char *copy = NULL, *p, *line, *eol, *value, *tail;
	const char *log_data;
	size_t n;

	if (!data)
		return;
	log_data = data;
	if (cfg->whitewash_api_keys &&
	    type && !strcmp(type, "header_out")) {
		copy = malloc(size + 1);
		if (!copy)
			return;
		memcpy(copy, data, size);
		copy[size] = '\0';
		for (p = copy; p < copy + size; p = eol ? eol + 1 : copy + size) {
			line = p;
			eol = memchr(line, '\n', (copy + size) - line);
			tail = eol ? eol : copy + size;
			if (!strncasecmp(line, "Authorization:", 14))
				value = line + 14;
			else if (!strncasecmp(line, "x-api-key:", 10))
				value = line + 10;
			else
				continue;
			while (value < tail && (*value == ' ' || *value == '\t'))
				value++;
			if (value < tail) {
				n = (size_t)(tail - value);
				memset(value, ' ', n);
				memcpy(value, "[redacted]",
				       n < sizeof("[redacted]") - 1 ?
					       n : sizeof("[redacted]") - 1);
			}
		}
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
