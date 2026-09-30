/* SPDX-License-Identifier: MIT */
#ifndef FYAI_AUTH_H
#define FYAI_AUTH_H

#include <stdbool.h>
#include <time.h>
#include <libfyaml/libfyaml-generic.h>

struct fyai_ctx;
struct fyai_auth_refresh_request;
struct fyai_auth_login_request;

typedef void (*fyai_auth_refresh_complete_fn)(
		struct fyai_auth_refresh_request *request, void *userdata);
typedef void (*fyai_auth_login_complete_fn)(
		struct fyai_auth_login_request *request, void *userdata);

enum fyai_auth_mode {
	FYAI_AUTH_AUTO,
	FYAI_AUTH_API_KEY,
	FYAI_AUTH_CHATGPT,
};

/* Credential strings live in the authentication builder. */
struct fyai_credentials {
	const char *client_id;
	const char *host_id;
	const char *subject;
	const char *scope;
	fy_generic registrations;
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

bool fyai_auth_credentials_ready(const struct fyai_credentials *c);
/* Token strings are copied into ctx's authentication builder. */
int fyai_auth_parse_tokens(struct fyai_ctx *ctx, struct fyai_credentials *c,
			  fy_generic doc, bool refresh);
const char *fyai_auth_mode_string(enum fyai_auth_mode mode);
fy_generic fyai_auth_status_data(struct fyai_ctx *ctx,
				 struct fy_generic_builder *gb, bool info);
int fyai_auth_login(struct fyai_ctx *ctx, bool device_code,
		    bool no_browser, bool manual, const char *account, bool new_account);
fy_generic fyai_auth_accounts_data(struct fyai_ctx *ctx,
				   struct fy_generic_builder *gb);
int fyai_auth_logout(struct fyai_ctx *ctx);
/* Return recorded conversation usage and the plan-settings link in out_gb. */
int fyai_auth_usage(struct fyai_ctx *ctx, struct fy_generic_builder *out_gb,
		    bool raw, fy_generic *datap);
int fyai_auth_resolve(struct fyai_ctx *ctx);
int fyai_auth_refresh(struct fyai_ctx *ctx, bool force);
struct fyai_auth_refresh_request *
fyai_auth_refresh_submit(struct fyai_ctx *ctx, bool force,
			 fyai_auth_refresh_complete_fn complete,
			 void *userdata);
void fyai_auth_refresh_cancel(struct fyai_auth_refresh_request *request);
bool fyai_auth_refresh_done(
		const struct fyai_auth_refresh_request *request);
int fyai_auth_refresh_collect(
		const struct fyai_auth_refresh_request *request);
void fyai_auth_refresh_destroy(struct fyai_auth_refresh_request *request);
struct fyai_auth_login_request *
fyai_auth_login_submit(struct fyai_ctx *ctx, bool device_code,
		       bool no_browser, const char *account, bool new_account,
		       fyai_auth_login_complete_fn complete,
		       void *userdata);
/* Validate a pasted complete callback URL without storing it. */
int fyai_auth_login_redirect(struct fyai_auth_login_request *request,
			     const char *url);
void fyai_auth_login_cancel(struct fyai_auth_login_request *request);
bool fyai_auth_login_done(const struct fyai_auth_login_request *request);
int fyai_auth_login_collect(const struct fyai_auth_login_request *request);
void fyai_auth_login_destroy(struct fyai_auth_login_request *request);
int fyai_auth_apply_headers(struct fyai_ctx *ctx,
			    struct curl_slist **headers);

/*
 * Load stored credentials into ctx; return -1 if no login is available.
 * Token pointers are borrowed from ctx until the next load.
 * Fresh tokens do not need a refresh; valid tokens have not expired.
 */
int fyai_auth_store_load(struct fyai_ctx *ctx);
bool fyai_auth_store_fresh(const struct fyai_ctx *ctx);
bool fyai_auth_store_valid(const struct fyai_ctx *ctx);
const char *fyai_auth_store_token(const struct fyai_ctx *ctx);
/* True when the loaded login holds a registration that authorizes the plan. */
bool fyai_auth_store_ready(const struct fyai_ctx *ctx);
/* The credential source that names the login held by the transport. */
#define FYAI_AUTH_CHATGPT_REF "oauth:chatgpt"

struct fyai_cfg;

/* Is the login at the transport for this configuration? */
bool fyai_auth_uses_transport(const struct fyai_cfg *cfg);
/*
 * Can the ChatGPT login serve this configuration? Return 0, -ENOTSUP for a
 * provider it does not serve, or -EINVAL for a grammar or endpoint it cannot
 * use; @why names the reason.
 */
int fyai_auth_chatgpt_eligible(const struct fyai_cfg *cfg, const char **why);

/* The Responses endpoint of the subscription. */
const char *fyai_auth_chatgpt_url(void);
bool fyai_auth_should_retry(struct fyai_ctx *ctx, long status);
fy_generic fyai_auth_models(struct fyai_ctx *ctx,
			    struct fy_generic_builder *gb, bool full);
void fyai_auth_cleanup(struct fyai_ctx *ctx);

#endif
