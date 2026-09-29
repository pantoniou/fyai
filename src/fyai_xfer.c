/*
 * fyai_xfer.c - one HTTP transfer of the model request path
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_STREAM

#include <stdlib.h>
#include <string.h>

#include "fyai.h"
#include "fyai_curl.h"
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

	struct fyai_tcall *call;		/* the transport backend */
	long status;
	long retry_after;
};

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
	ctx->xfer_status = status;
	ctx->xfer_retry_after = retry_after;
	snprintf(ctx->xfer_error, sizeof(ctx->xfer_error), "%s",
		 error ? error : "");
	if (x->done)
		x->done(x, x->done_ud);
}

/* The curl backend. */

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
	x->call = fyai_tclient_submit(ctx->tclient, &rq, &cb);
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

	x = calloc(1, sizeof(*x));
	fyai_error_check(ctx, x, err, "could not allocate the HTTP transfer");
	x->ctx = ctx;
	x->write = write;
	x->write_ud = write_userdata;
	x->done = done;
	x->done_ud = done_userdata;

	if (ctx->tclient)
		return xfer_tp_submit(ctx, x);
	curl_easy_setopt(ctx->curl, CURLOPT_WRITEFUNCTION, write);
	curl_easy_setopt(ctx->curl, CURLOPT_WRITEDATA, write_userdata);
	x->curl = fyai_curl_submit(ctx, ctx->curl, xfer_curl_complete, x);
	fyai_error_check(ctx, x->curl, err_free, "could not submit the request");
	return x;
err_free:
	free(x);
err:
	return NULL;
}

void fyai_xfer_cancel(struct fyai_xfer *x)
{
	if (!x)
		return;
	if (x->curl)
		fyai_curl_cancel(x->curl);
	if (x->call)
		fyai_tcall_cancel(x->call);
}

bool fyai_xfer_done(const struct fyai_xfer *x)
{
	return x && x->finished;
}

CURLcode fyai_xfer_result(const struct fyai_xfer *x)
{
	return x && x->finished ? x->result : CURLE_FAILED_INIT;
}

void fyai_xfer_destroy(struct fyai_xfer *x)
{
	if (!x)
		return;
	if (x->curl)
		fyai_curl_transfer_destroy(x->curl);
	if (x->call)
		fyai_tcall_destroy(x->call);
	free(x);
}
