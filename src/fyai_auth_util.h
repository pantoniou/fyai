/* SPDX-License-Identifier: MIT */
#ifndef FYAI_AUTH_UTIL_H
#define FYAI_AUTH_UTIL_H

#include <stddef.h>
#include <stdbool.h>
#include <time.h>
#include <libfyaml/libfyaml-generic.h>

#define FYAI_AUTH_ISSUER "https://auth.openai.com"

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

bool fyai_auth_scope_has(const char *scopes, const char *scope);
/* The verified claims live in gb; an invalid token returns fy_invalid. */
fy_generic fyai_auth_verify_id_token(struct fy_generic_builder *gb,
		const char *token, fy_generic jwks, const char *client_id,
		const char *nonce, const char *subject, time_t now);

/* Adapt a Responses request to the ChatGPT plan HTTP contract in gb. */
fy_generic fyai_auth_subscription_request(struct fy_generic_builder *gb,
					 fy_generic request);

#endif
