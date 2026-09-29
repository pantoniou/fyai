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
};

void fyai_xfer_set_body(struct fyai_ctx *ctx, const char *body)
{
	ctx->xfer_body = body;
	curl_easy_setopt(ctx->curl, CURLOPT_POSTFIELDS, body);
}

void fyai_xfer_set_endpoint(struct fyai_ctx *ctx, const char *url,
			    const char *profile)
{
	ctx->xfer_url = url;
	ctx->xfer_profile = profile;
	curl_easy_setopt(ctx->curl, CURLOPT_URL, url ? url : ctx->cfg->api_url);
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
	if (x && x->curl)
		fyai_curl_cancel(x->curl);
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
	free(x);
}
