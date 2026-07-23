/*
 * fyai_curl.h - run a curl transfer on the event loop
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_CURL_H
#define FYAI_CURL_H

#include <stdbool.h>

#include <curl/curl.h>

struct fyai_ctx;
struct fyai_curl_transfer;

typedef void (*fyai_curl_complete_fn)(struct fyai_curl_transfer *transfer,
				      void *userdata);

/*
 * Submit @easy to the context-owned curl multi handle. Completion is latched
 * before @complete is called. Completion is deferred through the event loop,
 * so @complete cannot run before this function returns. The caller owns @easy
 * and must keep it alive until the transfer is destroyed.
 */
struct fyai_curl_transfer *
fyai_curl_submit(struct fyai_ctx *ctx, CURL *easy,
		 fyai_curl_complete_fn complete, void *userdata);

void fyai_curl_cancel(struct fyai_curl_transfer *transfer);
bool fyai_curl_done(const struct fyai_curl_transfer *transfer);
CURLcode fyai_curl_collect(const struct fyai_curl_transfer *transfer);
void fyai_curl_transfer_destroy(struct fyai_curl_transfer *transfer);

/* Synchronous compatibility wrapper over the submitted transfer lifecycle. */
CURLcode fyai_curl_perform(struct fyai_ctx *ctx, CURL *easy);

/* Release the invocation's curl plumbing: the multi handle with its connection
 * pool, and the loop whose sources describe curl's sockets. */
void fyai_curl_cleanup(struct fyai_ctx *ctx);

#endif
