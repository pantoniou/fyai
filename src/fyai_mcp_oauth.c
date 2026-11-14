/*
 * fyai_mcp_oauth.c - the token refresh of an MCP server that uses OAuth
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#include "config.h"

#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

#include "fyai.h"
#include "fyai_auth_util.h"
#include "fyai_buf.h"
#include "fyai_cred.h"
#include "fyai_curl.h"
#include "fyai_event.h"
#include "fyai_mcp_oauth.h"
#include "fyai_secret.h"
#include "utils.h"

#define FYAI_MODULE FYAIEM_UNKNOWN

/* A token that is valid for this long more is not renewed. */
#define OAUTH_ADOPT_WINDOW 30

enum refresh_state {
	RS_WAIT_LOCK,		/* the timer tries the store lock */
	RS_PENDING,		/* the token request is out */
	RS_DONE,		/* the owner has the answer */
};

struct fyai_mcp_oauth_refresh {
	struct fyai_ctx *ctx;
	fyai_mcp_oauth_done_fn done;
	void *userdata;
	enum refresh_state state;
	struct fyai_event_source *timer;
	int lockfd;
	struct fyai_curl_transfer *transfer;
	CURL *curl;
	struct curl_slist *headers;
	struct fyai_buf response;
	char *body;
	/*
	 * The owner cancelled while the token request was out; the answer is
	 * still saved. @owner_gone: the owner destroyed the request meanwhile,
	 * so the end of the transfer frees it.
	 */
	bool detached;
	bool owner_gone;
	/* Copies of the spec. The tokens are cleared when they are freed. */
	char *server, *issuer, *resource, *endpoint, *client_id, *client_secret;
	char *store_secret, *scopes, *access_token, *refresh_token;
	enum fyai_mcp_oauth_client_auth auth;
	long timeout;
};

static void secret_free(char **p)
{
	size_t len;

	if (!*p)
		return;
	len = strlen(*p);
	fyai_secret_clear_and_free(p, &len);
}

static char *dup_or_empty(const char *s)
{
	return strdup(s ? s : "");
}

static void refresh_free(struct fyai_mcp_oauth_refresh *r)
{
	if (r->transfer)
		fyai_curl_transfer_destroy(r->transfer);
	fyai_event_source_remove(r->timer);
	fyai_auth_store_unlock(r->lockfd);
	curl_easy_cleanup(r->curl);
	curl_slist_free_all(r->headers);
	free(r->response.data);
	if (r->body) {
		fyai_secret_clear(r->body, strlen(r->body));
		free(r->body);
	}
	free(r->server);
	free(r->issuer);
	free(r->resource);
	free(r->endpoint);
	free(r->client_id);
	secret_free(&r->client_secret);
	secret_free(&r->store_secret);
	free(r->scopes);
	secret_free(&r->access_token);
	secret_free(&r->refresh_token);
	free(r);
}

/* Give the owner the answer, once. The request may be gone on return. */
static void refresh_finish(struct fyai_mcp_oauth_refresh *r,
			   enum fyai_mcp_oauth_outcome outcome,
			   const char *access, const char *refresh,
			   time_t expires_at, const char *message)
{
	struct fyai_mcp_oauth_result res = {
		.outcome = outcome, .access_token = access,
		.refresh_token = refresh, .expires_at = expires_at,
		.message = message,
	};

	r->state = RS_DONE;
	fyai_auth_store_unlock(r->lockfd);
	r->lockfd = -1;
	if (r->detached) {
		/* The owner has its answer already. */
		if (r->owner_gone)
			refresh_free(r);
		return;
	}
	if (r->done)
		r->done(r, &res, r->userdata);
}

static void refresh_fail(struct fyai_mcp_oauth_refresh *r, const char *message)
{
	if (!r->detached)
		fyai_error(r->ctx, "MCP server '%s' OAuth refresh failed: %s",
			   r->server, message);
	refresh_finish(r, FYAI_MCP_OAUTH_FAILED, NULL, NULL, 0, message);
}

static int record_save(struct fyai_mcp_oauth_refresh *r, const char *access,
		       const char *refresh, time_t expires_at)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(r->ctx);
	const char *json, *name;
	fy_generic doc;

	if (!gb)
		return -1;
	doc = fy_mapping(gb,
		"server", r->server, "issuer", r->issuer,
		"resource", r->resource, "client_id", r->client_id,
		"client_secret", r->store_secret, "scopes", r->scopes,
		"access_token", access, "refresh_token", refresh,
		"expires_at", (long long)expires_at);
	json = emit_json_string(gb, doc);
	if (!json)
		return -1;
	name = fy_sprintfa("mcp-auth-%s.json", r->server);
	return fyai_auth_store_write(r->ctx, name, json);
}

static void token_complete(struct fyai_curl_transfer *transfer, void *ud)
{
	struct fyai_mcp_oauth_refresh *r = ud;
	struct fy_generic_builder *gb;
	fy_generic doc;
	const char *access, *refresh;
	char *msg = NULL;
	CURLcode code;
	long status = 0;
	time_t expires_at;
	bool ok = false;

	code = fyai_curl_collect(transfer);
	curl_easy_getinfo(r->curl, CURLINFO_RESPONSE_CODE, &status);
	fyai_curl_transfer_destroy(transfer);
	r->transfer = NULL;
	if (code == CURLE_ABORTED_BY_CALLBACK) {
		refresh_finish(r, FYAI_MCP_OAUTH_CANCELLED, NULL, NULL, 0, NULL);
		return;
	}
	if (code != CURLE_OK || status < 200 || status >= 300) {
		msg = fy_sprintfa("token request failed: %s (HTTP %ld)",
				  curl_easy_strerror(code), status);
		goto fail;
	}
	gb = fyai_ctx_transient_gb(r->ctx);
	doc = gb ? parse_json_string(gb, r->response.data ?
				     r->response.data : "") : fy_invalid;
	access = "";
	if (fy_is_mapping(doc))
		access = fy_get(doc, "access_token", "");
	if (!*access) {
		msg = "token response has no access_token";
		goto fail;
	}
	/* A response with no new refresh token keeps the one that was sent. */
	refresh = fy_get(doc, "refresh_token", "");
	if (!*refresh)
		refresh = r->refresh_token;
	expires_at = time(NULL) + (time_t)fy_get(doc, "expires_in", 3600LL);
	if (record_save(r, access, refresh, expires_at)) {
		msg = "could not save the OAuth tokens";
		goto fail;
	}
	ok = true;
	refresh_finish(r, FYAI_MCP_OAUTH_REFRESHED, access, refresh, expires_at,
		       NULL);
fail:
	if (!ok)
		refresh_fail(r, msg);
}

static int token_submit(struct fyai_mcp_oauth_refresh *r)
{
	char *e_refresh = NULL, *e_secret = NULL, *e_client = NULL;
	char *e_resource = NULL;
	const char *form;
	int rc = -1;

	e_refresh = curl_easy_escape(NULL, r->refresh_token, 0);
	e_client = curl_easy_escape(NULL, r->client_id, 0);
	e_resource = curl_easy_escape(NULL, r->resource, 0);
	if (r->auth == FYAI_MCP_CLIENT_POST)
		e_secret = curl_easy_escape(NULL, r->client_secret, 0);
	if (!e_refresh || !e_client || !e_resource ||
	    (r->auth == FYAI_MCP_CLIENT_POST && !e_secret))
		goto out;
	form = fy_sprintfa(
		"grant_type=refresh_token&refresh_token=%s&client_id=%s&"
		"resource=%s%s%s", e_refresh, e_client, e_resource,
		e_secret ? "&client_secret=" : "", e_secret ? e_secret : "");
	r->body = strdup(form);
	if (!r->body)
		goto out;
	r->headers = curl_slist_append(r->headers,
		"Content-Type: application/x-www-form-urlencoded");
	r->curl = curl_easy_init();
	if (!r->headers || !r->curl)
		goto out;
	/* Reserve SIGALRM for the watchdog. */
	curl_easy_setopt(r->curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(r->curl, CURLOPT_URL, r->endpoint);
	curl_easy_setopt(r->curl, CURLOPT_WRITEFUNCTION, write_response);
	curl_easy_setopt(r->curl, CURLOPT_WRITEDATA, &r->response);
	curl_easy_setopt(r->curl, CURLOPT_TIMEOUT, r->timeout);
	if (r->ctx->user_agent)
		curl_easy_setopt(r->curl, CURLOPT_USERAGENT, r->ctx->user_agent);
	if (r->auth == FYAI_MCP_CLIENT_BASIC) {
		curl_easy_setopt(r->curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
		curl_easy_setopt(r->curl, CURLOPT_USERNAME, r->client_id);
		curl_easy_setopt(r->curl, CURLOPT_PASSWORD, r->client_secret);
	}
	curl_easy_setopt(r->curl, CURLOPT_POSTFIELDS, r->body);
	curl_easy_setopt(r->curl, CURLOPT_HTTPHEADER, r->headers);
	r->transfer = fyai_curl_submit(r->ctx, r->curl, token_complete, r);
	rc = r->transfer ? 0 : -1;
out:
	curl_free(e_refresh);
	curl_free(e_secret);
	curl_free(e_client);
	curl_free(e_resource);
	return rc;
}

/*
 * With the lock: read the record again. A token that differs from the one in
 * memory was renewed by another process; take it and send nothing. Else send
 * the refresh token that the store holds, which a rotation by another process
 * may have changed.
 */
static void refresh_locked(struct fyai_mcp_oauth_refresh *r)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(r->ctx);
	const char *name, *s_access, *s_refresh;
	char *text;
	fy_generic doc;
	long long s_expires;

	name = fy_sprintfa("mcp-auth-%s.json", r->server);
	text = fyai_auth_store_read(r->ctx, name);
	doc = text && gb ? parse_json_string(gb, text) : fy_invalid;
	free(text);
	if (fy_is_mapping(doc) &&
	    fy_equal(fy_get(doc, "issuer", ""), r->issuer) &&
	    fy_equal(fy_get(doc, "resource", ""), r->resource)) {
		s_access = fy_get(doc, "access_token", "");
		s_refresh = fy_get(doc, "refresh_token", "");
		s_expires = fy_get(doc, "expires_at", 0LL);
		if (*s_access && s_expires > (long long)time(NULL) +
		    OAUTH_ADOPT_WINDOW &&
		    fyai_cred_tag(s_access) != fyai_cred_tag(r->access_token)) {
			refresh_finish(r, FYAI_MCP_OAUTH_ADOPTED, s_access,
				       *s_refresh ? s_refresh : r->refresh_token,
				       (time_t)s_expires, NULL);
			return;
		}
		if (*s_refresh) {
			secret_free(&r->refresh_token);
			r->refresh_token = strdup(s_refresh);
			if (!r->refresh_token) {
				refresh_fail(r, "out of memory");
				return;
			}
		}
	}
	if (!*r->refresh_token) {
		refresh_fail(r, "the server has no refresh token");
		return;
	}
	r->state = RS_PENDING;
	if (token_submit(r))
		refresh_fail(r, "could not start the token request");
}

static enum fyai_event_action refresh_timer(const struct fyai_event *ev)
{
	struct fyai_mcp_oauth_refresh *r = ev->userdata;
	int rc;

	r->timer = NULL;
	r->lockfd = fyai_auth_store_lock(r->ctx, true);
	if (r->lockfd == -2) {
		rc = fyai_event_add_timer(ev->loop, 50, 0, refresh_timer, r,
					  &r->timer);
		if (rc)
			refresh_fail(r, "could not wait for the store");
		return FYAIEA_CONTINUE;
	}
	if (r->lockfd < 0) {
		refresh_fail(r, "could not lock the store");
		return FYAIEA_CONTINUE;
	}
	refresh_locked(r);
	return FYAIEA_CONTINUE;
}

struct fyai_mcp_oauth_refresh *
fyai_mcp_oauth_refresh_submit(struct fyai_ctx *ctx,
			      const struct fyai_mcp_oauth_spec *spec,
			      fyai_mcp_oauth_done_fn done, void *userdata)
{
	struct fyai_mcp_oauth_refresh *r;
	struct fyai_event_loop *el;
	int rc;

	r = calloc(1, sizeof(*r));
	fyai_error_check(ctx, r, err_out, "could not allocate the OAuth refresh");
	r->ctx = ctx;
	r->done = done;
	r->userdata = userdata;
	r->lockfd = -1;
	r->auth = spec->auth;
	r->timeout = spec->timeout > 0 ? spec->timeout : 30;
	r->server = dup_or_empty(spec->server);
	r->issuer = dup_or_empty(spec->issuer);
	r->resource = dup_or_empty(spec->resource);
	r->endpoint = dup_or_empty(spec->token_endpoint);
	r->client_id = dup_or_empty(spec->client_id);
	r->client_secret = dup_or_empty(spec->client_secret);
	r->store_secret = dup_or_empty(spec->store_secret);
	r->scopes = dup_or_empty(spec->scopes);
	r->access_token = dup_or_empty(spec->access_token);
	r->refresh_token = dup_or_empty(spec->refresh_token);
	fyai_error_check(ctx, r->server && r->issuer && r->resource &&
			 r->endpoint && r->client_id && r->client_secret &&
			 r->store_secret && r->scopes && r->access_token &&
			 r->refresh_token, err_free,
			 "could not copy the OAuth refresh");
	el = fyai_ctx_loop(ctx);
	fyai_error_check(ctx, el, err_free,
			 "could not acquire the application event loop");
	r->state = RS_WAIT_LOCK;
	/* The work starts from the loop, so the owner never sees it inside submit. */
	rc = fyai_event_add_timer(el, 1, 0, refresh_timer, r, &r->timer);
	fyai_error_check(ctx, !rc, err_free,
			 "could not start the OAuth refresh");
	return r;

err_free:
	refresh_free(r);
err_out:
	return NULL;
}

void fyai_mcp_oauth_refresh_cancel(struct fyai_mcp_oauth_refresh *r)
{
	if (!r || r->state == RS_DONE || r->detached)
		return;
	if (r->state == RS_PENDING) {
		/* The refresh token is on the wire: read the answer. */
		r->detached = true;
		if (r->done)
			r->done(r, &(struct fyai_mcp_oauth_result){
				.outcome = FYAI_MCP_OAUTH_CANCELLED }, r->userdata);
		return;
	}
	fyai_event_source_remove(r->timer);
	r->timer = NULL;
	refresh_finish(r, FYAI_MCP_OAUTH_CANCELLED, NULL, NULL, 0, NULL);
}

void fyai_mcp_oauth_refresh_destroy(struct fyai_mcp_oauth_refresh *r)
{
	if (!r)
		return;
	if (r->detached && r->transfer) {
		/* The end of the transfer saves the tokens and frees it. */
		r->owner_gone = true;
		return;
	}
	refresh_free(r);
}

void fyai_mcp_oauth_refresh_abort(struct fyai_mcp_oauth_refresh *r)
{
	if (r)
		refresh_free(r);
}
