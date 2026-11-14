/*
 * fyai_xfer.c - one HTTP transfer of the model request path
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_STREAM

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "fyai.h"
#include "fyai_auth.h"
#include "fyai_buf.h"
#include "fyai_cred.h"
#include "fyai_curl.h"
#include "fyai_event.h"
#include "fyai_grammar.h"
#include "fyai_secret.h"
#include "fyai_transport_boot.h"
#include "fyai_transport_cfg.h"
#include "fyai_transport_client.h"
#include "fyai_xfer.h"

struct fyai_xfer {
	struct fyai_ctx *ctx;
	fyai_xfer_write_fn write;
	void *write_ud;
	fyai_xfer_done_fn done;
	void *done_ud;
	bool finished;
	CURLcode result;

	struct fyai_curl_transfer *curl;	/* the curl backend */

	/*
	 * The curl backend with the ChatGPT login: the credential is read for
	 * each send, a request waits while the login refreshes, and one 401 is
	 * answered with a renewal and a second send. States of the request:
	 * NEW, PARKED (@waiter queued), SENT, HELD (a 401 whose body is kept
	 * back) and RETRIED (the second send). doc/credential-sources-plan.md
	 * section 4.3.
	 */
	bool login;
	bool rekey;			/* a stored API key; see xfer_rekeyed() */
	bool parked;
	bool retried;
	bool started;			/* the body reached the caller */
	bool hold;			/* a 401 body is kept back */
	bool cancelled;
	bool deferred;			/* xfer_defer_cb() is queued on the loop */
	uint64_t tag;			/* of the token that this send used */
	struct fyai_cred_waiter waiter;
	struct curl_slist *headers;	/* the headers of this send */
	struct fyai_buf held;

	struct fyai_tcall *call;		/* the transport backend */
	long status;
	long retry_after;
};

static void xfer_finish(struct fyai_xfer *x, CURLcode result, long status,
			long retry_after, const char *error);
static int xfer_login_send(struct fyai_xfer *x);
static int xfer_curl_send(struct fyai_xfer *x);
static void xfer_headers_free(struct fyai_xfer *x);
static void xfer_curl_complete(struct fyai_curl_transfer *transfer, void *ud);

void fyai_xfer_set_body(struct fyai_ctx *ctx, const char *body)
{
	ctx->xfer_body = body;
	if (ctx->curl)
		curl_easy_setopt(ctx->curl, CURLOPT_POSTFIELDS, body);
}

void fyai_xfer_set_endpoint(struct fyai_ctx *ctx, const char *url,
			    const char *kind)
{
	ctx->xfer_url = url;
	ctx->xfer_profile = kind;
	if (ctx->curl)
		curl_easy_setopt(ctx->curl, CURLOPT_URL,
				 url ? url : ctx->cfg->api_url);
}

long fyai_xfer_last_status(const struct fyai_ctx *ctx)
{
	return ctx->xfer_status;
}

long fyai_xfer_last_retry_after(const struct fyai_ctx *ctx)
{
	return ctx->xfer_retry_after;
}

const char *fyai_xfer_last_error(const struct fyai_ctx *ctx)
{
	return ctx->xfer_error;
}

static void xfer_finish(struct fyai_xfer *x, CURLcode result, long status,
			long retry_after, const char *error)
{
	struct fyai_ctx *ctx = x->ctx;

	x->result = result;
	x->finished = true;
	xfer_headers_free(x);
	fyai_buf_free(&x->held);
	ctx->xfer_status = status;
	ctx->xfer_retry_after = retry_after;
	snprintf(ctx->xfer_error, sizeof(ctx->xfer_error), "%s",
		 error ? error : "");
	if (x->done)
		x->done(x, x->done_ud);
}

/* The curl backend. */

#define XFER_HELD_MAX (64 * 1024)

/* The login is the credential of this request, and this process holds it. */
static bool xfer_uses_login(const struct fyai_ctx *ctx)
{
	return ctx->cfg->chatgpt_auth && !ctx->transport.client &&
	       !fyai_auth_uses_transport(ctx->cfg);
}

/*
 * An API key that the secret store holds can be replaced by another process.
 * A 401 for such a key is held back, and the key is read again.
 */
static bool xfer_uses_rekey(const struct fyai_ctx *ctx)
{
	const struct fyai_cfg *cfg = ctx->cfg;

	return !ctx->transport.client && cfg->api_key_explicit &&
	       !fy_str_empty(cfg->api_key) && cfg->api_key_ref &&
	       !strncmp(cfg->api_key_ref, "secret:", 7);
}

static void xfer_headers_free(struct fyai_xfer *x)
{
	curl_slist_free_all(x->headers);
	x->headers = NULL;
}

/* Raise the cause of a login that cannot serve this request. */
static void xfer_login_cause(struct fyai_ctx *ctx, int rc)
{
	if (rc == -ENOENT)
		fyai_error(ctx, "not logged in; run `fyai auth login`");
	else if (rc == -ESTALE)
		fyai_error(ctx, "the saved login has expired; run `fyai auth login`");
	else
		fyai_error(ctx, "the ChatGPT login could not be renewed");
}

static void xfer_defer_cb(void *ud)
{
	struct fyai_xfer *x = ud;

	x->deferred = false;
	if (x->cancelled) {
		xfer_finish(x, CURLE_ABORTED_BY_CALLBACK, 0, 0, NULL);
		return;
	}
	if (x->login ? xfer_login_send(x) : xfer_curl_send(x))
		xfer_finish(x, CURLE_LOGIN_DENIED, 0, 0, NULL);
}

static void xfer_defer(struct fyai_xfer *x)
{
	struct fyai_event_loop *el = fyai_ctx_loop(x->ctx);

	if (x->deferred)
		return;
	x->deferred = !fyai_event_defer(el, xfer_defer_cb, x);
	if (!x->deferred)
		xfer_finish(x, CURLE_FAILED_INIT, 0, 0, NULL);
}

/* The write callback of the caller sees every body except a held 401. */
static size_t xfer_write(char *ptr, size_t size, size_t nmemb, void *ud)
{
	struct fyai_xfer *x = ud;
	long status = 0;

	if (!(x->login || x->rekey) || x->started)
		return x->write(ptr, size, nmemb, x->write_ud);
	if (!x->hold) {
		curl_easy_getinfo(x->ctx->curl, CURLINFO_RESPONSE_CODE, &status);
		if (status == 401 && !x->retried &&
		    (x->rekey ||
		     fyai_cred_reject(x->ctx, "oauth:chatgpt", NULL, 0, true)))
			x->hold = true;
		else
			x->started = true;
	}
	if (!x->hold)
		return x->write(ptr, size, nmemb, x->write_ud);
	/* Keep the start of it: it is the answer if the login cannot renew. */
	if (fyai_buf_pending(&x->held) < XFER_HELD_MAX &&
	    fyai_buf_append_data(&x->held, ptr, size * nmemb)) {
		fyai_error(x->ctx, "cannot keep the body of the refused request");
		return 0;
	}
	return size * nmemb;
}

/* Send the request: the headers are built for this send from the login. */
static int xfer_curl_send(struct fyai_xfer *x)
{
	struct fyai_ctx *ctx = x->ctx;
	struct curl_slist *src;
	int rc = 0;

	xfer_headers_free(x);
	x->started = false;
	x->hold = false;
	fyai_buf_free(&x->held);
	if (x->login || x->rekey) {
		for (src = ctx->headers; !rc && src; src = src->next)
			rc = append_header(&x->headers, src->data);
		if (!rc && x->login)
			rc = fyai_auth_apply_headers(ctx, &x->headers);
		fyai_error_check(ctx, !rc, err_out,
				 "the credential cannot authenticate this request");
		if (x->login)
			x->tag = fyai_cred_tag(ctx->auth.access_token);
		curl_easy_setopt(ctx->curl, CURLOPT_HTTPHEADER, x->headers);
	}
	curl_easy_setopt(ctx->curl, CURLOPT_WRITEFUNCTION, xfer_write);
	curl_easy_setopt(ctx->curl, CURLOPT_WRITEDATA, x);
	x->curl = fyai_curl_submit(ctx, ctx->curl, xfer_curl_complete, x);
	fyai_error_check(ctx, x->curl, err_out, "could not submit the request");
	return 0;
err_out:
	return -1;
}

/* The login is current, or the request waits for its refresh. */
static int xfer_login_send(struct fyai_xfer *x)
{
	int rc;

	rc = fyai_cred_prepare(x->ctx, "oauth:chatgpt", &x->waiter, x->retried);
	if (rc < 0) {
		xfer_login_cause(x->ctx, rc);
		return -1;
	}
	if (rc > 0) {
		x->parked = true;
		return 0;
	}
	return xfer_curl_send(x);
}

/* The refresh that this request waited for has ended. */
static void xfer_waiter_run(struct fyai_cred_waiter *w, int result)
{
	struct fyai_xfer *x = w->userdata;

	x->parked = false;
	if (x->cancelled) {
		xfer_finish(x, CURLE_ABORTED_BY_CALLBACK, 0, 0, NULL);
		return;
	}
	if (result < 0) {
		xfer_login_cause(x->ctx, result);
		xfer_finish(x, CURLE_LOGIN_DENIED, 0, 0, NULL);
		return;
	}
	if (xfer_login_send(x))
		xfer_finish(x, CURLE_LOGIN_DENIED, 0, 0, NULL);
}

/*
 * The provider refused the token: renew the login once and send again. The
 * body of the refusal stays with the request. When the login cannot be
 * renewed the refusal is the answer, and the caller gets its body.
 */
static bool xfer_rejected(struct fyai_xfer *x, long status)
{
	int rc;

	rc = fyai_cred_reject(x->ctx, "oauth:chatgpt", &x->waiter, x->tag, false);
	if (rc < 0)
		return false;
	x->retried = true;
	if (rc > 0)
		x->parked = true;
	else
		xfer_defer(x);
	(void)status;
	return true;
}

/*
 * Read the stored key again. A key that differs from the one this request
 * used replaces it, and the request is sent again with it. The same key is
 * the answer of the provider.
 */
static bool xfer_rekeyed(struct fyai_xfer *x)
{
	struct fyai_ctx *ctx = x->ctx;
	struct fyai_cfg *cfg = ctx->cfg;
	const struct fyai_grammar *grammar = fyai_grammar(cfg);
	struct curl_slist *list = NULL, *src;
	char *secret = NULL, *header = NULL;
	const char *key;
	size_t slen;
	int rc;

	rc = fyai_cred_get(ctx, cfg->api_key_ref, &secret, NULL);
	if (rc || fy_str_empty(secret) ||
	    fyai_cred_tag(secret) == fyai_cred_tag(cfg->api_key))
		goto err_same;
	key = fy_gb_intern_string(cfg->gb, secret);
	header = grammar->key_header ?
		make_header(fy_sprintfa("%s: ", grammar->key_header), key) :
		make_header("Authorization: Bearer ", key);
	if (!key || !header)
		goto err_same;
	for (src = ctx->headers; src; src = src->next) {
		if (ctx->auth_header && !strcmp(src->data, ctx->auth_header))
			continue;
		if (append_header(&list, src->data))
			goto err_same;
	}
	if (append_header(&list, header))
		goto err_same;
	curl_slist_free_all(ctx->headers);
	ctx->headers = list;
	free(ctx->auth_header);
	ctx->auth_header = header;
	cfg->api_key = key;
	slen = strlen(secret);
	fyai_secret_clear_and_free(&secret, &slen);
	x->retried = true;
	xfer_defer(x);
	return true;
err_same:
	curl_slist_free_all(list);
	free(header);
	if (secret) {
		slen = strlen(secret);
		fyai_secret_clear_and_free(&secret, &slen);
	}
	return false;
}

static void xfer_curl_complete(struct fyai_curl_transfer *transfer, void *ud)
{
	struct fyai_xfer *x = ud;
	CURLcode res = fyai_curl_collect(transfer);
	curl_off_t retry = 0;
	long status = 0;

	curl_easy_getinfo(x->ctx->curl, CURLINFO_RESPONSE_CODE, &status);
	if (curl_easy_getinfo(x->ctx->curl, CURLINFO_RETRY_AFTER, &retry) != CURLE_OK)
		retry = 0;
	fyai_curl_transfer_destroy(transfer);
	x->curl = NULL;
	if (x->login && res == CURLE_OK && status == 401 && !x->retried &&
	    !x->started && xfer_rejected(x, status))
		return;
	if (x->rekey && res == CURLE_OK && status == 401 && !x->retried &&
	    !x->started && xfer_rekeyed(x))
		return;
	/* A refusal that was held and cannot be renewed goes to the caller. */
	if (x->hold && fyai_buf_pending(&x->held))
		(void)x->write(x->held.data, 1, fyai_buf_pending(&x->held),
			       x->write_ud);
	xfer_finish(x, res, status, retry > 0 ? (long)retry : 0, NULL);
}

/* The transport backend. */

static size_t xfer_tp_body(const void *data, size_t len, void *ud)
{
	struct fyai_xfer *x = ud;

	return x->write((void *)data, 1, len, x->write_ud);
}

static void xfer_tp_start(struct fyai_tcall *call,
			  const struct fyai_tcall_start *st, void *ud)
{
	struct fyai_xfer *x = ud;

	(void)call;
	x->status = st->status;
	x->retry_after = st->retry_after_s > 0 ? st->retry_after_s : 0;
}

/* Say how the call ended in the terms of a curl transfer. */
static void xfer_tp_end(struct fyai_tcall *call,
			const struct fyai_tcall_result *r, void *ud)
{
	struct fyai_xfer *x = ud;
	CURLcode res;

	(void)call;
	switch (r->end) {
	case FYAI_TC_OK:
		res = CURLE_OK;
		break;
	case FYAI_TC_FAILED:
		res = (CURLcode)r->code;
		break;
	case FYAI_TC_CANCELLED:
		res = CURLE_ABORTED_BY_CALLBACK;
		break;
	case FYAI_TC_ABORTED:
		/* As curl reports a write callback that stopped the transfer. */
		res = CURLE_WRITE_ERROR;
		break;
	case FYAI_TC_REFUSED:
		res = CURLE_BAD_FUNCTION_ARGUMENT;
		break;
	case FYAI_TC_LOST:
	default:
		/* Nothing retries a lost transport; it is not transient. */
		res = CURLE_FAILED_INIT;
		break;
	}
	xfer_finish(x, res, x->status, x->retry_after, r->message);
}

static struct fyai_xfer *xfer_tp_submit(struct fyai_ctx *ctx, struct fyai_xfer *x)
{
	char name[FYAI_TPC_NAME_MAX];
	struct fyai_transport_request rq = {
		.profile = name,
		.method = "POST",
		.content_type = "application/json",
		.body = (const uint8_t *)(ctx->xfer_body ? ctx->xfer_body : ""),
		.body_len = ctx->xfer_body ? strlen(ctx->xfer_body) : 0,
	};
	struct fyai_tcall_callbacks cb = {
		.start = xfer_tp_start, .body = xfer_tp_body,
		.end = xfer_tp_end, .userdata = x,
	};

	/* This agent may need profiles that its parent did not state. */
	if (fyai_transport_ensure(ctx)) {
		free(x);
		return NULL;
	}
	/* The kind names the profile of this configuration; see fyai_transport_cfg.h. */
	if (!fyai_transport_profile_name(ctx->cfg, ctx->xfer_profile ?
					 ctx->xfer_profile : FYAI_TPC_MODEL,
					 name, sizeof(name))) {
		fyai_error(ctx, "this configuration has no transport profile "
			   "for a %s request", ctx->xfer_profile ?
			   ctx->xfer_profile : FYAI_TPC_MODEL);
		free(x);
		return NULL;
	}
	x->call = fyai_tclient_submit(ctx->transport.client, &rq, &cb);
	if (!x->call) {
		free(x);
		return NULL;
	}
	return x;
}

/* Submit. */

struct fyai_xfer *fyai_xfer_submit(struct fyai_ctx *ctx, fyai_xfer_write_fn write,
				   void *write_userdata, fyai_xfer_done_fn done,
				   void *done_userdata)
{
	struct fyai_xfer *x;
	int rc;

	x = calloc(1, sizeof(*x));
	fyai_error_check(ctx, x, err, "could not allocate the HTTP transfer");
	x->ctx = ctx;
	x->write = write;
	x->write_ud = write_userdata;
	x->done = done;
	x->done_ud = done_userdata;

	if (ctx->transport.client)
		return xfer_tp_submit(ctx, x);
	x->waiter.run = xfer_waiter_run;
	x->waiter.userdata = x;
	x->login = xfer_uses_login(ctx);
	x->rekey = !x->login && xfer_uses_rekey(ctx);
	if (x->login) {
		rc = xfer_login_send(x);
		fyai_error_check(ctx, !rc, err_free,
				 "could not start the request");
		return x;
	}
	rc = xfer_curl_send(x);
	fyai_error_check(ctx, !rc, err_free, "could not submit the request");
	return x;
err_free:
	fyai_xfer_destroy(x);
	return NULL;
err:
	return NULL;
}

void fyai_xfer_cancel(struct fyai_xfer *x)
{
	if (!x)
		return;
	x->cancelled = true;
	if (x->curl) {
		fyai_curl_cancel(x->curl);
	} else if (x->parked) {
		/* No transfer runs: the request ends from the loop. */
		fyai_cred_waiter_cancel(x->ctx, &x->waiter);
		x->parked = false;
		xfer_defer(x);
	}
	if (x->call)
		fyai_tcall_cancel(x->call);
}

CURLcode fyai_xfer_result(const struct fyai_xfer *x)
{
	return x && x->finished ? x->result : CURLE_FAILED_INIT;
}

void fyai_xfer_destroy(struct fyai_xfer *x)
{
	if (!x)
		return;
	if (x->parked || x->waiter.queued)
		fyai_cred_waiter_cancel(x->ctx, &x->waiter);
	if (x->deferred)
		fyai_event_defer_cancel(fyai_ctx_loop(x->ctx), xfer_defer_cb, x);
	if (x->curl)
		fyai_curl_transfer_destroy(x->curl);
	if (x->call)
		fyai_tcall_destroy(x->call);
	xfer_headers_free(x);
	fyai_buf_free(&x->held);
	free(x);
}
