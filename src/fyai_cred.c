/*
 * fyai_cred.c - the credential sources and their shared refresh
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#include "config.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

#include "fyai.h"
#include "fyai_auth.h"
#include "fyai_cred.h"
#include "fyai_event.h"
#include "fyai_secret.h"
#include "fyai_transport_server.h"

#define FYAI_MODULE FYAIEM_UNKNOWN

/* A secret that the process was given in memory, "mem:NAME". */
struct fyai_cred_mem {
	char *name;
	char *value;
};

/* The one refresh of the login in this process, and who waits for it. */
struct fyai_cred {
	struct fyai_ctx *ctx;
	struct fyai_event_source *timer;	/* the scheduled refresh */
	time_t scheduled;			/* when the timer fires, or 0 */
	struct fyai_auth_refresh_request *refresh;
	struct fyai_cred_waiter *head;
	struct fyai_cred_waiter *tail;
	struct fyai_cred_mem *mem;
	size_t nmem;
};

struct fyai_cred_ops {
	const char *name;
	int (*prepare)(struct fyai_ctx *ctx, struct fyai_cred_waiter *w,
		       bool resumed);
	int (*reject)(struct fyai_ctx *ctx, struct fyai_cred_waiter *w,
		      uint64_t tag, bool probe);
};

uint64_t fyai_cred_tag(const char *secret)
{
	return fyai_transport_credential_tag(secret);
}

static struct fyai_cred *cred_get(struct fyai_ctx *ctx)
{
	if (!ctx->cred) {
		ctx->cred = calloc(1, sizeof(*ctx->cred));
		if (ctx->cred)
			ctx->cred->ctx = ctx;
	}
	fyai_error_check(ctx, ctx->cred, err_out,
			 "could not allocate the credential state");
	return ctx->cred;
err_out:
	return NULL;
}

bool fyai_cred_refresh_pending(const struct fyai_ctx *ctx)
{
	return ctx->cred && ctx->cred->refresh;
}

static void cred_queue(struct fyai_cred *cred, struct fyai_cred_waiter *w)
{
	if (!w || w->queued)
		return;
	w->next = NULL;
	w->queued = true;
	if (cred->tail)
		cred->tail->next = w;
	else
		cred->head = w;
	cred->tail = w;
}

/*
 * The scheduled refresh. A holder that lives long keeps the token fresh with
 * one timer at the time the token is due, so a request seldom waits for a
 * refresh. The timer only starts the shared refresh, with no waiter; every
 * state that a request meets still holds when the timer did not run. A token
 * that lives no longer than CRED_SCHEDULE_MIN_S past its due time is left to
 * the requests: a timer for it would refresh in a loop.
 */
#define CRED_SCHEDULE_MIN_S 60
#define CRED_SCHEDULE_RETRY_S 300

static enum fyai_event_action cred_timer_cb(const struct fyai_event *ev);

static void cred_arm(struct fyai_ctx *ctx, time_t at)
{
	struct fyai_cred *cred;
	struct fyai_event_loop *el;
	time_t now = time(NULL);
	fyai_event_ms_t ms;
	int rc;

	cred = cred_get(ctx);
	if (!cred || cred->timer)
		return;
	if (at < now + CRED_SCHEDULE_MIN_S)
		return;
	el = fyai_ctx_loop(ctx);
	if (!el)
		return;
	ms = (fyai_event_ms_t)(at - now) * 1000;
	rc = fyai_event_add_timer(el, ms, 0, cred_timer_cb, ctx, &cred->timer);
	if (rc) {
		cred->timer = NULL;
		return;
	}
	cred->scheduled = at;
}

static int login_refresh(struct fyai_ctx *ctx, struct fyai_cred_waiter *w,
			 bool force, uint64_t tag);

void fyai_cred_timer_fire(struct fyai_ctx *ctx)
{
	struct fyai_cred *cred = ctx->cred;
	time_t due;
	int rc;

	if (!cred)
		return;
	/* Called by a test with the timer still armed: it must not fire twice. */
	fyai_event_source_remove(cred->timer);
	cred->timer = NULL;
	cred->scheduled = 0;
	/* A refresh that runs arms the timer again when it ends. */
	if (cred->refresh)
		return;
	/* No login, or a store that cannot be read: nothing to keep fresh. */
	if (fyai_auth_store_load(ctx))
		return;
	due = fyai_auth_store_due_at(ctx);
	if (!due)
		return;
	/* Another process renewed it meanwhile: wait for the new due time. */
	if (fyai_auth_store_fresh(ctx)) {
		cred_arm(ctx, due);
		return;
	}
	rc = login_refresh(ctx, NULL, false, 0);
	/* A refresh that cannot start is tried again later, not at once. */
	if (rc < 0)
		cred_arm(ctx, time(NULL) + CRED_SCHEDULE_RETRY_S);
	else if (!rc)
		cred_arm(ctx, fyai_auth_store_due_at(ctx));
}

static enum fyai_event_action cred_timer_cb(const struct fyai_event *ev)
{
	struct fyai_ctx *ctx = ev->userdata;

	/* A timer of one shot ends with its callback. */
	if (ctx->cred)
		ctx->cred->timer = NULL;
	fyai_cred_timer_fire(ctx);
	return FYAIEA_CONTINUE;
}

time_t fyai_cred_scheduled_at(const struct fyai_ctx *ctx)
{
	return ctx->cred ? ctx->cred->scheduled : 0;
}

/*
 * The refresh ended. Take the waiters first: one of them can ask for the
 * credential again, and that is a new refresh or an answer from the state.
 */
static void cred_refresh_done(struct fyai_auth_refresh_request *request,
			      void *userdata)
{
	struct fyai_cred *cred = userdata;
	struct fyai_cred_waiter *list, *w;
	int rc;

	rc = fyai_auth_refresh_collect(request) ? -EIO : 0;
	fyai_auth_refresh_destroy(request);
	cred->refresh = NULL;
	/* Keep the new token fresh; a failed refresh waits before it is tried again. */
	if (!rc)
		cred_arm(cred->ctx, fyai_auth_store_due_at(cred->ctx));
	else
		cred_arm(cred->ctx, time(NULL) + CRED_SCHEDULE_RETRY_S);
	list = cred->head;
	cred->head = cred->tail = NULL;
	while (list) {
		w = list;
		list = w->next;
		w->next = NULL;
		w->queued = false;
		w->run(w, rc);
	}
}

/*
 * Start the refresh of the login, or find the one that runs. Returns 1 when
 * the caller is queued, 0 when the refresh ended at once, with the answer in
 * the store, and a negative errno when it could not start.
 */
static int login_refresh(struct fyai_ctx *ctx, struct fyai_cred_waiter *w,
			 bool force, uint64_t tag)
{
	struct fyai_cred *cred;

	cred = cred_get(ctx);
	if (!cred)
		return -ENOMEM;
	if (!cred->refresh) {
		cred->refresh = tag ?
			fyai_auth_refresh_submit_rejected(ctx, tag,
							  cred_refresh_done, cred) :
			fyai_auth_refresh_submit(ctx, force, cred_refresh_done,
						 cred);
		if (!cred->refresh)
			return -EIO;
		/* A login that needs no refresh ends at once, with no callback. */
		if (fyai_auth_refresh_done(cred->refresh)) {
			fyai_auth_refresh_destroy(cred->refresh);
			cred->refresh = NULL;
			return 0;
		}
	}
	cred_queue(cred, w);
	return 1;
}

static int login_prepare(struct fyai_ctx *ctx, struct fyai_cred_waiter *w,
			 bool resumed)
{
	int rc;

	if (fyai_auth_store_load(ctx))
		return -ENOENT;
	if (fyai_auth_store_fresh(ctx)) {
		cred_arm(ctx, fyai_auth_store_due_at(ctx));
		return 0;
	}
	if (resumed)
		return fyai_auth_store_valid(ctx) ? 0 : -ESTALE;
	rc = login_refresh(ctx, w, false, 0);
	if (rc < 0)
		return fyai_auth_store_valid(ctx) ? 0 : -EIO;
	if (rc > 0)
		return 1;
	if (fyai_auth_store_load(ctx))
		return -EIO;
	return fyai_auth_store_valid(ctx) ? 0 : -ESTALE;
}

static int login_reject(struct fyai_ctx *ctx, struct fyai_cred_waiter *w,
			uint64_t tag, bool probe)
{
	int rc;

	if (probe)
		return 1;
	if (fyai_auth_store_load(ctx))
		return -ENOENT;
	if (tag && fyai_cred_tag(fyai_auth_store_token(ctx)) != tag)
		return fyai_auth_store_valid(ctx) ? 0 : -ESTALE;
	rc = login_refresh(ctx, w, true, tag);
	if (rc < 0)
		return -EIO;
	if (rc > 0)
		return 1;
	if (fyai_auth_store_load(ctx))
		return -EIO;
	return fyai_auth_store_valid(ctx) &&
	       (!tag || fyai_cred_tag(fyai_auth_store_token(ctx)) != tag) ?
	       0 : -ESTALE;
}

static const struct fyai_cred_ops cred_ops[] = {
	{ "oauth:chatgpt", login_prepare, login_reject },
};

static const struct fyai_cred_ops *cred_find(const char *source)
{
	size_t i;

	for (i = 0; source && i < sizeof(cred_ops) / sizeof(cred_ops[0]); i++)
		if (!strcmp(source, cred_ops[i].name))
			return &cred_ops[i];
	return NULL;
}

int fyai_cred_prepare(struct fyai_ctx *ctx, const char *source,
		      struct fyai_cred_waiter *waiter, bool resumed)
{
	const struct fyai_cred_ops *ops = cred_find(source);

	if (!ops || !ops->prepare)
		return 0;
	return ops->prepare(ctx, waiter, resumed);
}

int fyai_cred_reject(struct fyai_ctx *ctx, const char *source,
		     struct fyai_cred_waiter *waiter, uint64_t tag, bool probe)
{
	const struct fyai_cred_ops *ops = cred_find(source);

	if (!ops || !ops->reject)
		return probe ? 0 : -ENOTSUP;
	return ops->reject(ctx, waiter, tag, probe);
}

void fyai_cred_waiter_cancel(struct fyai_ctx *ctx,
			     struct fyai_cred_waiter *waiter)
{
	struct fyai_cred *cred = ctx->cred;
	struct fyai_cred_waiter **pp;
	struct fyai_auth_refresh_request *refresh;

	if (!cred || !waiter || !waiter->queued)
		return;
	for (pp = &cred->head; *pp; pp = &(*pp)->next) {
		if (*pp != waiter)
			continue;
		*pp = waiter->next;
		if (cred->tail == waiter) {
			cred->tail = NULL;
			for (cred->tail = cred->head;
			     cred->tail && cred->tail->next;
			     cred->tail = cred->tail->next)
				;
		}
		break;
	}
	waiter->next = NULL;
	waiter->queued = false;
	refresh = cred->refresh;
	/* The last waiter left: stop what has sent nothing secret yet. */
	if (!cred->head && refresh)
		fyai_auth_refresh_cancel(refresh);
}

/*
 * Resolve one source, "env:NAME", "secret:NAME" (the secret store, as the
 * api_key setting reads it), "mem:NAME" or "oauth:chatgpt". Each returns 0 and a
 * malloc'd value, -ENOENT when the source holds none, or -ESTALE for a login
 * that is out of date.
 */
static int kind_env(struct fyai_ctx *ctx, const char *name, char **secret,
		    struct curl_slist **extra)
{
	const char *value;

	(void)ctx;
	(void)extra;
	value = getenv(name);
	if (!value || !*value)
		return -ENOENT;
	*secret = strdup(value);
	return *secret ? 0 : -ENOMEM;
}

static const struct fyai_cred_mem *mem_find(const struct fyai_cred *cred,
					    const char *name)
{
	size_t i;

	for (i = 0; cred && i < cred->nmem; i++)
		if (!strcmp(cred->mem[i].name, name))
			return &cred->mem[i];
	return NULL;
}

static int kind_mem(struct fyai_ctx *ctx, const char *name, char **secret,
		    struct curl_slist **extra)
{
	const struct fyai_cred_mem *m;

	(void)extra;
	m = mem_find(ctx ? ctx->cred : NULL, name);
	if (!m || !m->value || !*m->value)
		return -ENOENT;
	*secret = strdup(m->value);
	return *secret ? 0 : -ENOMEM;
}

static int kind_secret(struct fyai_ctx *ctx, const char *name, char **secret,
		       struct curl_slist **extra)
{
	char key[300], *found = NULL;
	size_t flen = 0;
	int rc, n;

	(void)ctx;
	(void)extra;
	n = snprintf(key, sizeof(key), "fyai:%s", name);
	if (n < 0 || (size_t)n >= sizeof(key))
		return -EINVAL;
	if (fyai_secret_kernel_get(key, &found, &flen) != FYAI_SECRET_OK)
		return -ENOENT;
	rc = found && *found ? 0 : -ENOENT;
	if (!rc) {
		*secret = strdup(found);
		if (!*secret)
			rc = -ENOMEM;
	}
	fyai_secret_clear_and_free(&found, &flen);
	return rc;
}

/*
 * "oauth:chatgpt": the access token of the subscription login. A token that
 * cannot be used is reported, not refreshed here: fyai_cred_prepare() does
 * that before a request reads it.
 */
static int kind_oauth(struct fyai_ctx *ctx, const char *name, char **secret,
		      struct curl_slist **extra)
{
	(void)extra;
	if (strcmp(name, "chatgpt") || !ctx)
		return -EINVAL;
	if (fyai_auth_store_load(ctx))
		return -ENOENT;
	if (!fyai_auth_store_ready(ctx))
		return -ENOENT;
	if (!fyai_auth_store_valid(ctx))
		return -ESTALE;
	*secret = strdup(fyai_auth_store_token(ctx));
	return *secret ? 0 : -ENOMEM;
}

struct fyai_cred_kind {
	const char *prefix;
	int (*get)(struct fyai_ctx *, const char *, char **,
		   struct curl_slist **);
};

static const struct fyai_cred_kind cred_kinds[] = {
	{ "env:", kind_env },
	{ "mem:", kind_mem },
	{ "secret:", kind_secret },
	{ "oauth:", kind_oauth },
};

static int cred_one(struct fyai_ctx *ctx, const char *src, size_t len,
		    char **secret, struct curl_slist **extra)
{
	char tmp[256];
	size_t i, n;

	if (len >= sizeof(tmp))
		return -EINVAL;
	memcpy(tmp, src, len);
	tmp[len] = '\0';
	for (i = 0; i < sizeof(cred_kinds) / sizeof(cred_kinds[0]); i++) {
		n = strlen(cred_kinds[i].prefix);
		if (!strncmp(tmp, cred_kinds[i].prefix, n))
			return cred_kinds[i].get(ctx, tmp + n, secret, extra);
	}
	return -EINVAL;
}

/*
 * The source can be a chain, "A|B": the first source that holds a value wins,
 * as the provider default tries the environment and then the secret store.
 */
int fyai_cred_get(struct fyai_ctx *ctx, const char *chain, char **secret,
		  struct curl_slist **extra)
{
	const char *src = chain;
	const char *end;
	size_t len;
	int rc = -ENOENT;

	*secret = NULL;
	while (src && *src) {
		end = strchr(src, '|');
		len = end ? (size_t)(end - src) : strlen(src);

		rc = cred_one(ctx, src, len, secret, extra);
		if (!rc || rc == -ENOMEM)
			return rc;
		src = end ? end + 1 : NULL;
	}
	return rc == -EINVAL || rc == -ESTALE ? rc : -ENOENT;
}

int fyai_cred_mem_set(struct fyai_ctx *ctx, const char *name, const char *value)
{
	struct fyai_cred *cred;
	struct fyai_cred_mem *m, *nm;
	char *val, *nn;

	cred = cred_get(ctx);
	if (!cred)
		return -ENOMEM;
	val = strdup(value);
	if (!val)
		return -ENOMEM;
	m = (struct fyai_cred_mem *)mem_find(cred, name);
	if (m) {
		if (m->value)
			fyai_secret_clear(m->value, strlen(m->value));
		free(m->value);
		m->value = val;
		return 0;
	}
	nn = strdup(name);
	nm = nn ? realloc(cred->mem, (cred->nmem + 1) * sizeof(*nm)) : NULL;
	if (!nm) {
		fyai_secret_clear(val, strlen(val));
		free(val);
		free(nn);
		return -ENOMEM;
	}
	cred->mem = nm;
	cred->mem[cred->nmem].name = nn;
	cred->mem[cred->nmem].value = val;
	cred->nmem++;
	return 0;
}

void fyai_cred_release(struct fyai_ctx *ctx)
{
	struct fyai_cred *cred = ctx->cred;
	struct fyai_cred_waiter *w;
	size_t i;

	if (!cred)
		return;
	for (w = cred->head; w; w = w->next)
		w->queued = false;
	cred->head = cred->tail = NULL;
	fyai_event_source_remove(cred->timer);
	cred->timer = NULL;
	if (cred->refresh) {
		fyai_auth_refresh_destroy(cred->refresh);
		cred->refresh = NULL;
	}
	for (i = 0; i < cred->nmem; i++) {
		fyai_secret_clear(cred->mem[i].value, strlen(cred->mem[i].value));
		free(cred->mem[i].value);
		free(cred->mem[i].name);
	}
	free(cred->mem);
	free(cred);
	ctx->cred = NULL;
}

void fyai_cred_abandon(struct fyai_ctx *ctx)
{
	struct fyai_cred *cred = ctx->cred;

	if (!cred)
		return;
	/*
	 * The refresh and the waiters belong to the parent: its transfer, its
	 * lock and its requests are not ours. The secrets kept in memory
	 * describe the work and stay.
	 */
	cred->refresh = NULL;
	ctx->auth_detached = 0;
	cred->head = cred->tail = NULL;
	/* The timer is a source of the loop of the parent. */
	cred->timer = NULL;
	cred->scheduled = 0;
}
