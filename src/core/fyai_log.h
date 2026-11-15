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
/*
 * The logs of a target: wire, stream, conversation, mcp, transport, or all. Set turns
 * them on or off for this run, clear empties them, and view opens them.
 */
void fyai_log_set(struct fyai_cfg *cfg, const char *target, bool on);
int fyai_log_clear_log(struct fyai_ctx *ctx, const char *target);
int fyai_log_view(struct fyai_ctx *ctx, const char *target);
/* Which logs are on, built in @gb. */
fy_generic fyai_log_status_data(struct fyai_ctx *ctx,
				struct fy_generic_builder *gb);
void fyai_log_wire_text(struct fyai_ctx *ctx, const char *type,
			const char *data, size_t size);
int fyai_curl_debug(CURL *curl, curl_infotype type, char *data,
		    size_t size, void *userdata);

#endif
