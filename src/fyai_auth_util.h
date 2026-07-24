/* SPDX-License-Identifier: MIT */
#ifndef FYAI_AUTH_UTIL_H
#define FYAI_AUTH_UTIL_H

#include <stddef.h>
#include <stdbool.h>

struct fyai_ctx;

char *fyai_base64url_encode(const unsigned char *data, size_t len);
unsigned char *fyai_base64url_decode(const char *text, size_t *lenp);
const char *fyai_auth_state_path(struct fyai_ctx *ctx, const char *name,
				 bool create);
int fyai_auth_store_lock(struct fyai_ctx *ctx, bool nonblock);
void fyai_auth_store_unlock(int fd);
char *fyai_auth_store_read(struct fyai_ctx *ctx, const char *name);
int fyai_auth_store_write(struct fyai_ctx *ctx, const char *name,
			  const char *text);
int fyai_auth_store_delete(struct fyai_ctx *ctx, const char *name);

#endif
