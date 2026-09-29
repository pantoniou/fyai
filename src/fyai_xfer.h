/*
 * fyai_xfer.h - one HTTP transfer of the model request path
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * A model request is one transfer: a body goes out, and the response bytes
 * come back through a write callback. A transfer runs on one of two backends.
 * The curl backend talks to the provider from this process. The transport
 * backend sends the request to the credential transport over the channel of
 * this agent, and the transport talks to the provider. The callers do not know
 * which one runs.
 *
 * The caller sets the endpoint and the body on the context before it submits,
 * as it sets them on the curl handle.
 */

#ifndef FYAI_XFER_H
#define FYAI_XFER_H

#include <stdbool.h>
#include <stddef.h>

#include <curl/curl.h>

struct fyai_ctx;
struct fyai_xfer;

/* The write callback of curl: return @size * @nmemb to take the bytes. */
typedef size_t (*fyai_xfer_write_fn)(void *ptr, size_t size, size_t nmemb,
				     void *userdata);
typedef void (*fyai_xfer_done_fn)(struct fyai_xfer *xfer, void *userdata);

/*
 * Set the body of the next transfer. The caller keeps @body alive until the
 * transfer is done. NULL clears it.
 */
void fyai_xfer_set_body(struct fyai_ctx *ctx, const char *body);

/*
 * Send the next transfer to @url, through the transport profile @profile.
 * NULL restores the endpoint of the configuration and the profile "model".
 */
void fyai_xfer_set_endpoint(struct fyai_ctx *ctx, const char *url,
			    const char *profile);

/*
 * Start a transfer. @done runs from the event loop, never from this call, and
 * after it the caller destroys the transfer. Return NULL, with a diagnostic,
 * if it cannot start.
 */
struct fyai_xfer *fyai_xfer_submit(struct fyai_ctx *ctx, fyai_xfer_write_fn write,
				   void *write_userdata, fyai_xfer_done_fn done,
				   void *done_userdata);

/* Stop a transfer; @done still runs, with CURLE_ABORTED_BY_CALLBACK. */
void fyai_xfer_cancel(struct fyai_xfer *xfer);

bool fyai_xfer_done(const struct fyai_xfer *xfer);

/* The result of a finished transfer, as curl would report it. */
CURLcode fyai_xfer_result(const struct fyai_xfer *xfer);

/*
 * Free a transfer, cancelling it if it is not done. This is the only way to
 * release it, from @done or after it.
 */
void fyai_xfer_destroy(struct fyai_xfer *xfer);

/*
 * The status, the Retry-After delay in seconds (0 if none), and the text of
 * the error of the last transfer that finished on @ctx. A transfer records
 * them before its done callback runs.
 */
long fyai_xfer_last_status(const struct fyai_ctx *ctx);
long fyai_xfer_last_retry_after(const struct fyai_ctx *ctx);
const char *fyai_xfer_last_error(const struct fyai_ctx *ctx);

#endif
