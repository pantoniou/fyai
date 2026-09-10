/* SPDX-License-Identifier: MIT */
#ifndef FYAI_BROWSER_H
#define FYAI_BROWSER_H

#include "fyai.h"
struct fytim_surface;

char *fyai_browser_gitgraph_source(fy_generic rows);
int fyai_browser_open(struct fyai_ctx *ctx);
void fyai_browser_close(struct fyai_ctx *ctx);
void fyai_browser_config_changed(struct fyai_ctx *ctx);
void fyai_browser_service(struct fyai_ctx *ctx);
void fyai_browser_step(struct fyai_ctx *ctx);
bool fyai_browser_input(struct fyai_ctx *ctx, const char *line);
bool fyai_browser_cancel_input(struct fyai_ctx *ctx);
bool fyai_browser_keys(struct fyai_ctx *ctx, const char *data, size_t len);
bool fyai_browser_surface(struct fyai_ctx *ctx, const struct fytim_surface *sf);

#endif
