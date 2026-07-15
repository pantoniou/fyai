/* SPDX-License-Identifier: MIT */
#ifndef FYAI_AUTH_H
#define FYAI_AUTH_H

#include <stdbool.h>
#include <time.h>
#include <libfyaml/libfyaml-generic.h>

struct fyai_ctx;

enum fyai_auth_mode {
	FYAI_AUTH_AUTO,
	FYAI_AUTH_API_KEY,
	FYAI_AUTH_CHATGPT,
};

enum fyai_auth_command {
	FYAI_AUTH_STATUS,
	FYAI_AUTH_INFO,
	FYAI_AUTH_USAGE,
	FYAI_AUTH_LOGIN,
	FYAI_AUTH_LOGOUT,
};

struct fyai_auth_args {
	enum fyai_auth_command command;
	const char *provider;
	bool device_code;
	bool no_browser;
	bool manual;
	bool json;
};

/* all pointer are stable in the cfg builder */
struct fyai_credentials {
	const char *access_token;
	const char *refresh_token;
	const char *id_token;
	const char *account_id;
	const char *email;
	const char *plan;
	bool fedramp;
	time_t expires_at;
	const char *storage;
};

const char *fyai_auth_mode_string(enum fyai_auth_mode mode);
int fyai_auth_execute(struct fyai_ctx *ctx);
fy_generic fyai_auth_status_data(struct fyai_ctx *ctx,
				 struct fy_generic_builder *gb, bool info);
int fyai_auth_status(struct fyai_ctx *ctx, bool json, bool info);
/* Fetch and display the live limits for the active subscription. */
int fyai_auth_usage(struct fyai_ctx *ctx, bool json);
int fyai_auth_resolve(struct fyai_ctx *ctx);
int fyai_auth_refresh(struct fyai_ctx *ctx, bool force);
int fyai_auth_apply_headers(struct fyai_ctx *ctx,
			    struct curl_slist **headers);
bool fyai_auth_should_retry(struct fyai_ctx *ctx, long status);
int fyai_auth_prepare_retry(struct fyai_ctx *ctx);
fy_generic fyai_auth_models(struct fyai_ctx *ctx,
			    struct fy_generic_builder *gb, bool full);
void fyai_auth_cleanup(struct fyai_ctx *ctx);

#endif
