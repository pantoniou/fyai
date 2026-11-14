/*
 * fyai_cred.h - the credential sources and their shared refresh
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis@konsulko.com>
 * SPDX-License-Identifier: MIT
 *
 * A credential source has a name, "oauth:chatgpt" for the login of the
 * subscription. A source that cannot go stale (env:, secret:, mem:) is never
 * due and has no renewal. A source that refreshes has at most one refresh in a
 * process; every caller that needs it waits on that refresh. The states are
 * in doc/credential-sources-plan.md, section 4.
 */

#ifndef FYAI_CRED_H
#define FYAI_CRED_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

struct fyai_ctx;
struct curl_slist;

/*
 * A caller that waits for a refresh. @run is called once from the event loop
 * when the refresh ends, with 0 or a negative errno. A waiter that was
 * queued stays valid until @run is called or fyai_cred_waiter_cancel().
 */
struct fyai_cred_waiter {
	void (*run)(struct fyai_cred_waiter *waiter, int result);
	void *userdata;
	struct fyai_cred_waiter *next;
	bool queued;
};

/*
 * Make the credential of @source usable before a request reads it. Returns 0
 * when it is ready, 1 when the refresh runs and @waiter is queued, or a
 * negative errno. @resumed is true for a request that waited: the source
 * answers from its state and starts no refresh, so a failed refresh ends each
 * waiting request once. A static source returns 0.
 */
int fyai_cred_prepare(struct fyai_ctx *ctx, const char *source,
		      struct fyai_cred_waiter *waiter, bool resumed);

/*
 * The provider answered a request with HTTP 401. With @probe, return nonzero
 * when the source can renew its credential and do nothing else. Otherwise
 * renew it: @tag names the credential that the request used
 * (fyai_cred_tag()); a credential that changed since needs no refresh.
 * A @tag of 0 renews whatever the store holds. Returns 0 when a request can
 * start now, 1 when the refresh runs and @waiter is queued, or a negative
 * errno when no renewal is possible.
 */
int fyai_cred_reject(struct fyai_ctx *ctx, const char *source,
		     struct fyai_cred_waiter *waiter, uint64_t tag, bool probe);

/*
 * Resolve the credential of @chain, a source or a chain "A|B" of sources
 * (env:, secret:, mem:, oauth:chatgpt). Returns 0 with a malloc'd secret that
 * the caller clears and frees, -ENOENT when no source holds a value, -ESTALE
 * for a login that is out of date, or -EINVAL for a source of another kind.
 * A source that names a header sets @extra, which the caller frees. @ctx may
 * be NULL for env: and secret:.
 */
int fyai_cred_get(struct fyai_ctx *ctx, const char *chain, char **secret,
		  struct curl_slist **extra);

/* Keep a secret in memory under @name, as "mem:NAME" reads it. */
int fyai_cred_mem_set(struct fyai_ctx *ctx, const char *name, const char *value);

/* The tag that tells two values of a credential apart. */
uint64_t fyai_cred_tag(const char *secret);

/*
 * Remove @waiter. When the last waiter leaves a refresh that is still before
 * the token request, the refresh is cancelled; a token request goes on and
 * saves its answer.
 */
void fyai_cred_waiter_cancel(struct fyai_ctx *ctx,
			     struct fyai_cred_waiter *waiter);

/*
 * The time at which the scheduled refresh of the login fires, or 0 when none
 * is armed. fyai_cred_timer_fire() is what the timer runs; a test calls it to
 * act at that time without waiting.
 */
time_t fyai_cred_scheduled_at(const struct fyai_ctx *ctx);
void fyai_cred_timer_fire(struct fyai_ctx *ctx);

/* True while a refresh of the login runs. */
bool fyai_cred_refresh_pending(const struct fyai_ctx *ctx);

/* End the refresh and drop the waiters without running them. */
void fyai_cred_release(struct fyai_ctx *ctx);
/* Forget the refresh and the waiters of the parent in a forked child; end nothing. */
void fyai_cred_abandon(struct fyai_ctx *ctx);

#endif
