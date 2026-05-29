/*
 * fyai_log.h - YAML trace logs
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_LOG_H
#define FYAI_LOG_H

#include "fyai.h"

int fyai_log_generic(struct fyai_ctx *ctx, const char *name, fy_generic doc);
int fyai_log_clear(struct fyai_ctx *ctx);
int fyai_log_control(struct fyai_ctx *ctx, const char *arg);
void fyai_log_wire_text(struct fyai_ctx *ctx, const char *type,
			const char *data, size_t size);
int fyai_curl_debug(CURL *curl, curl_infotype type, char *data,
		    size_t size, void *userdata);

#endif
