/*
 * fyai_curl.h - run a curl transfer on the event loop
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_CURL_H
#define FYAI_CURL_H

#include <curl/curl.h>

struct fyai_ctx;

/* Drive @easy to completion through curl's multi interface on a fyai_event
 * loop, and return the same CURLcode curl_easy_perform() would have. */
CURLcode fyai_curl_perform(struct fyai_ctx *ctx, CURL *easy);

/* Release the invocation's curl plumbing: the multi handle with its connection
 * pool, and the loop whose sources describe curl's sockets. */
void fyai_curl_cleanup(struct fyai_ctx *ctx);

#endif
