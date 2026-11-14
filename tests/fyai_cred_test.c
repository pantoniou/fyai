/*
 * fyai_cred_test.c - the credential sources and their shared refresh
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_UNKNOWN

#include "fyai_auth_mock.h"
#include "fyai_cred.h"

FYAI_TEST_ENTRY(cred, static_source, cred_static_source)
FYAI_TEST_ENTRY(cred, schedule_armed, cred_schedule_armed)
FYAI_TEST_ENTRY(cred, schedule_skips_short_tokens, cred_schedule_short)
FYAI_TEST_ENTRY(cred, schedule_refreshes, cred_schedule_refreshes)
FYAI_TEST_ENTRY(cred, schedule_sibling_renewed, cred_schedule_sibling)
FYAI_TEST_ENTRY(cred, schedule_failure_retries_later, cred_schedule_failure)
FYAI_TEST_ENTRY(cred, schedule_ends_with_release, cred_schedule_release)
FYAI_TEST_ENTRY(cred, get_chain, cred_get_chain)
FYAI_TEST_ENTRY(cred, get_mem, cred_get_mem)
FYAI_TEST_ENTRY(cred, get_login, cred_get_login)
FYAI_TEST_ENTRY(cred, login_states, cred_login_states)
FYAI_TEST_ENTRY(cred, refresh_is_shared, cred_refresh_is_shared)
FYAI_TEST_ENTRY(cred, refresh_renews, cred_refresh_renews)
FYAI_TEST_ENTRY(cred, reject_tag, cred_reject_tag)
FYAI_TEST_ENTRY(cred, reject_replaced_in_lock_wait, cred_reject_replaced_in_lock_wait)
FYAI_TEST_ENTRY(cred, reject_untagged_forces, cred_reject_untagged_forces)
FYAI_TEST_ENTRY(cred, waiter_cancel, cred_waiter_cancel)

#define LOGIN "oauth:chatgpt"
#define DIR_TEMPLATE "/tmp/fyai-cred-XXXXXX"

/* mkdtemp() changes its template: start each store from the template. */
static void dir_new(char *dir)
{
	strcpy(dir, DIR_TEMPLATE);
}

struct waiter {
	struct fyai_cred_waiter w;
	unsigned int count;
	int result;
};

static void waiter_run(struct fyai_cred_waiter *w, int result)
{
	struct waiter *x = w->userdata;

	x->count++;
	x->result = result;
}

static void waiter_init(struct waiter *x)
{
	memset(x, 0, sizeof(*x));
	x->w.run = waiter_run;
	x->w.userdata = x;
}

/* A source that cannot go stale is always ready and has no renewal. */
int cred_static_source(void)
{
	char dir[] = "/tmp/fyai-cred-XXXXXX";

	store_setup(dir);
	FYAI_TCHECK(fyai_cred_prepare(&ctx, "env:KEY", NULL, false) == 0);
	FYAI_TCHECK(fyai_cred_prepare(&ctx, "secret:KEY", NULL, true) == 0);
	FYAI_TCHECK(fyai_cred_reject(&ctx, "env:KEY", NULL, 0, true) == 0);
	FYAI_TCHECK(fyai_cred_reject(&ctx, "env:KEY", NULL, 7, false) == -ENOTSUP);
	FYAI_TCHECK(fyai_cred_reject(&ctx, LOGIN, NULL, 0, true) == 1);
	store_remove(dir);
	printf("ok - a static source is ready and has no renewal\n");
	return 0;
}

/* The states of the stored login that need no network. */
int cred_login_states(void)
{
	struct fyai_event_loop *el;
	char dir[] = "/tmp/fyai-cred-XXXXXX", path[300];

	/* No record. */
	dir_new(dir);
	store_setup(dir);
	snprintf(path, sizeof(path), "%s/auth.json", dir);
	unlink(path);
	FYAI_TCHECK(fyai_cred_prepare(&ctx, LOGIN, NULL, false) == -ENOENT);
	FYAI_TCHECK(fyai_cred_reject(&ctx, LOGIN, NULL, 5, false) == -ENOENT);
	store_remove(dir);
	dir_new(dir);
	/* A fresh token: ready, and a rejected token that was replaced. */
	store_setup_expiring(dir, (long long)time(NULL) + 3600);
	el = fyai_ctx_loop(&ctx);
	FYAI_TCHECK(el);
	FYAI_TCHECK(fyai_cred_prepare(&ctx, LOGIN, NULL, false) == 0);
	FYAI_TCHECK(fyai_cred_prepare(&ctx, LOGIN, NULL, true) == 0);
	/* Another token than the one that the request used: retry at once. */
	FYAI_TCHECK(fyai_cred_reject(&ctx, LOGIN, NULL,
				     fyai_cred_tag("some-other-token"), false) == 0);
	store_remove(dir);
	dir_new(dir);
	/* An expired token that cannot be renewed and a waiter that waited. */
	store_setup(dir);
	FYAI_TCHECK(fyai_cred_prepare(&ctx, LOGIN, NULL, true) == -ESTALE);
	store_remove(dir);
	printf("ok - the stored login answers from its state\n");
	return 0;
}

/*
 * Two requests find the token due: one refresh serves both. When the refresh
 * fails, each waiter is told once, and a request that waited starts none.
 */
int cred_refresh_is_shared(void)
{
	static const struct mock_answer script[] = { { 500, "{}", true } };
	struct fyai_event_loop *el;
	struct waiter a, b;
	struct mock m;
	char dir[] = "/tmp/fyai-cred-XXXXXX";

	store_setup(dir);
	el = fyai_ctx_loop(&ctx);
	FYAI_TCHECK(el);
	mock_start(&m, script, 1);
	waiter_init(&a);
	waiter_init(&b);

	FYAI_TCHECK(fyai_cred_prepare(&ctx, LOGIN, &a.w, false) == 1);
	STEP_UNTIL(el, m.nheld == 1);
	FYAI_TCHECK(fyai_cred_prepare(&ctx, LOGIN, &b.w, false) == 1);
	FYAI_TCHECK(a.count == 0 && b.count == 0);

	/* The key endpoint answers with an error: the refresh fails. */
	mock_release(&m);
	STEP_UNTIL(el, a.count > 0 && b.count > 0);
	FYAI_TCHECK(a.count == 1 && b.count == 1);
	FYAI_TCHECK(a.result < 0 && b.result < 0);
	/* A request that waited gets the state, and starts no refresh. */
	FYAI_TCHECK(fyai_cred_prepare(&ctx, LOGIN, NULL, true) == -ESTALE);
	/* One refresh made one request. */
	FYAI_TCHECK(m.nseen == 1 && m.extra == 0);

	mock_stop(&m);
	store_remove(dir);
	printf("ok - one refresh serves every waiting request\n");
	return 0;
}

/*
 * A refresh that succeeds: the issuer signs a token, the store takes the new
 * tokens, and every waiter is told once.
 */
int cred_refresh_renews(void)
{
	struct mock_answer script[2] = { { 200, NULL, false }, { 200, NULL, false } };
	struct fyai_event_loop *el;
	struct waiter a, b;
	struct issuer is;
	struct mock m;
	char dir[] = "/tmp/fyai-cred-XXXXXX", *token;

	store_setup(dir);
	el = fyai_ctx_loop(&ctx);
	FYAI_TCHECK(el);
	issuer_open(&is);
	token = issuer_token_body(&is, "renewed-access", "renewed-refresh");
	script[0].body = is.jwks;
	script[1].body = token;
	mock_start(&m, script, 2);
	waiter_init(&a);
	waiter_init(&b);

	FYAI_TCHECK(fyai_cred_prepare(&ctx, LOGIN, &a.w, false) == 1);
	FYAI_TCHECK(fyai_cred_prepare(&ctx, LOGIN, &b.w, false) == 1);
	STEP_UNTIL(el, a.count > 0 && b.count > 0);
	FYAI_TCHECK(a.count == 1 && b.count == 1 && a.result == 0 && b.result == 0);
	/* The keys first, then the token request, once. */
	FYAI_TCHECK(m.nseen == 2 && m.extra == 0);
	FYAI_TCHECK(strstr(m.seen[0].target, "jwks"));
	FYAI_TCHECK(strstr(m.seen[1].target, "oauth/token"));
	/* The store holds the new token, and a request is ready. */
	FYAI_TCHECK(fyai_cred_prepare(&ctx, LOGIN, NULL, true) == 0);
	FYAI_TCHECK(!strcmp(fyai_auth_store_token(&ctx), "renewed-access"));

	free(token);
	issuer_close(&is);
	mock_stop(&m);
	store_remove(dir);
	printf("ok - a refresh renews the stored login\n");
	return 0;
}

/* A 401 renews the login only when the stored token is the rejected one. */
int cred_reject_tag(void)
{
	static const struct mock_answer script[] = { { 500, "{}", true } };
	struct fyai_event_loop *el;
	struct waiter a;
	struct mock m;
	char dir[] = "/tmp/fyai-cred-XXXXXX";

	/* The stored token is fresh, the rejected one is not: retry, no refresh. */
	store_setup_expiring(dir, (long long)time(NULL) + 3600);
	el = fyai_ctx_loop(&ctx);
	FYAI_TCHECK(el);
	waiter_init(&a);
	FYAI_TCHECK(fyai_cred_reject(&ctx, LOGIN, &a.w,
				     fyai_cred_tag("old-token"), false) == 0);
	FYAI_TCHECK(a.count == 0);
	store_remove(dir);
	strcpy(dir, "/tmp/fyai-cred-XXXXXX");

	/* The stored token is the rejected one: a refresh starts. */
	store_setup_expiring(dir, (long long)time(NULL) + 3600);
	mock_start(&m, script, 1);
	FYAI_TCHECK(fyai_cred_reject(&ctx, LOGIN, &a.w,
				     fyai_cred_tag("old-access"), false) == 1);
	STEP_UNTIL(el, m.nheld == 1);
	mock_release(&m);
	STEP_UNTIL(el, a.count > 0);
	FYAI_TCHECK(a.count == 1 && a.result < 0);
	mock_stop(&m);
	store_remove(dir);
	printf("ok - a rejected token renews the login once\n");
	return 0;
}

/* The last waiter that leaves ends a refresh that has sent nothing secret. */
int cred_waiter_cancel(void)
{
	static const struct mock_answer script[] = {
		{ 500, "{}", true }, { 500, "{}", true } };
	struct fyai_event_loop *el;
	struct waiter a, b;
	struct mock m;
	char dir[] = "/tmp/fyai-cred-XXXXXX";

	store_setup(dir);
	el = fyai_ctx_loop(&ctx);
	FYAI_TCHECK(el);
	mock_start(&m, script, 2);
	waiter_init(&a);
	waiter_init(&b);
	FYAI_TCHECK(fyai_cred_prepare(&ctx, LOGIN, &a.w, false) == 1);
	FYAI_TCHECK(fyai_cred_prepare(&ctx, LOGIN, &b.w, false) == 1);
	STEP_UNTIL(el, m.nheld == 1);

	/* One waiter leaves: the refresh goes on for the other. */
	fyai_cred_waiter_cancel(&ctx, &a.w);
	FYAI_TCHECK(!a.w.queued && a.count == 0);
	FYAI_TCHECK(fyai_event_loop_step(el, 0) >= 0);
	FYAI_TCHECK(b.count == 0 && m.dropped == 0);
	/* The last one leaves: the key fetch is dropped, nobody is told. */
	fyai_cred_waiter_cancel(&ctx, &b.w);
	STEP_UNTIL(el, m.dropped == 1);
	STEP_UNTIL(el, lock_free(dir));
	FYAI_TCHECK(a.count == 0 && b.count == 0);
	/* A new request starts a new refresh. */
	FYAI_TCHECK(fyai_cred_prepare(&ctx, LOGIN, &a.w, false) == 1);
	STEP_UNTIL(el, m.nheld == 1);
	fyai_cred_waiter_cancel(&ctx, &a.w);
	STEP_UNTIL(el, m.dropped == 2);
	mock_stop(&m);
	store_remove(dir);
	printf("ok - the last waiter that leaves ends the refresh\n");
	return 0;
}

/*
 * A sibling renews the login while this process waits for the store lock. The
 * token that the provider rejected is gone by then, so no request is made.
 */
int cred_reject_replaced_in_lock_wait(void)
{
	struct fyai_event_loop *el;
	struct waiter a;
	char dir[] = DIR_TEMPLATE, path[300];
	int held;

	store_setup_expiring(dir, (long long)time(NULL) + 3600);
	el = fyai_ctx_loop(&ctx);
	FYAI_TCHECK(el);
	snprintf(path, sizeof(path), "%s/auth.lock", dir);
	held = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	FYAI_TCHECK(held >= 0 && !flock(held, LOCK_EX | LOCK_NB));
	waiter_init(&a);
	/* The stored token is the rejected one: a refresh starts and waits. */
	FYAI_TCHECK(fyai_cred_reject(&ctx, LOGIN, &a.w,
				     fyai_cred_tag("old-access"), false) == 1);
	FYAI_TCHECK(fyai_event_loop_step(el, 0) >= 0);
	FYAI_TCHECK(a.count == 0);
	/* The sibling saves a new token and lets go of the lock. */
	store_write_token("new-access", (long long)time(NULL) + 3600);
	FYAI_TCHECK(!flock(held, LOCK_UN));
	close(held);
	/* No server runs: a request to the issuer would fail the refresh. */
	STEP_UNTIL(el, a.count > 0);
	FYAI_TCHECK(a.count == 1 && a.result == 0);
	store_remove(dir);
	printf("ok - a token replaced during the lock wait needs no refresh\n");
	return 0;
}

/* A rejection with no tag renews whatever the store holds, fresh or not. */
int cred_reject_untagged_forces(void)
{
	static const struct mock_answer script[] = { { 500, "{}", true } };
	struct fyai_event_loop *el;
	struct waiter a;
	struct mock m;
	char dir[] = "/tmp/fyai-cred-XXXXXX";

	store_setup_expiring(dir, (long long)time(NULL) + 3600);
	el = fyai_ctx_loop(&ctx);
	FYAI_TCHECK(el);
	mock_start(&m, script, 1);
	waiter_init(&a);
	FYAI_TCHECK(fyai_cred_reject(&ctx, LOGIN, &a.w, 0, false) == 1);
	STEP_UNTIL(el, m.nheld == 1);
	mock_release(&m);
	STEP_UNTIL(el, a.count > 0);
	FYAI_TCHECK(a.count == 1 && a.result < 0);
	mock_stop(&m);
	store_remove(dir);
	printf("ok - a rejection with no tag forces the refresh\n");
	return 0;
}

/* The value of a source or a chain of sources: the first one that holds a value. */
int cred_get_chain(void)
{
	char *secret = NULL;

	setenv("FYAI_CRED_TEST_A", "alpha", 1);
	setenv("FYAI_CRED_TEST_B", "beta", 1);
	setenv("FYAI_CRED_TEST_EMPTY", "", 1);
	unsetenv("FYAI_CRED_TEST_NONE");

	FYAI_TCHECK(fyai_cred_get(NULL, "env:FYAI_CRED_TEST_A", &secret, NULL) == 0);
	FYAI_TCHECK(secret && !strcmp(secret, "alpha"));
	free(secret);
	FYAI_TCHECK(fyai_cred_get(NULL, "env:FYAI_CRED_TEST_NONE|env:FYAI_CRED_TEST_B",
				  &secret, NULL) == 0);
	FYAI_TCHECK(secret && !strcmp(secret, "beta"));
	free(secret);
	/* An empty value is none, and the first source that holds one wins. */
	FYAI_TCHECK(fyai_cred_get(NULL, "env:FYAI_CRED_TEST_EMPTY|env:FYAI_CRED_TEST_A|env:FYAI_CRED_TEST_B",
				  &secret, NULL) == 0);
	FYAI_TCHECK(secret && !strcmp(secret, "alpha"));
	free(secret);
	FYAI_TCHECK(fyai_cred_get(NULL, "env:FYAI_CRED_TEST_NONE", &secret, NULL) ==
		    -ENOENT && !secret);
	FYAI_TCHECK(fyai_cred_get(NULL, "env:FYAI_CRED_TEST_NONE|env:FYAI_CRED_TEST_EMPTY",
				  &secret, NULL) == -ENOENT);
	FYAI_TCHECK(fyai_cred_get(NULL, "bogus:x", &secret, NULL) == -EINVAL);
	FYAI_TCHECK(fyai_cred_get(NULL, NULL, &secret, NULL) == -ENOENT);
	FYAI_TCHECK(fyai_cred_get(NULL, "", &secret, NULL) == -ENOENT);
	printf("ok - a chain of sources gives the first value\n");
	return 0;
}

/* A secret kept in memory: set, replaced, and cleared with the state. */
int cred_get_mem(void)
{
	char *secret = NULL;
	char dir[] = DIR_TEMPLATE;

	store_setup(dir);
	FYAI_TCHECK(fyai_cred_get(&ctx, "mem:key", &secret, NULL) == -ENOENT);
	FYAI_TCHECK(fyai_cred_mem_set(&ctx, "key", "first") == 0);
	FYAI_TCHECK(fyai_cred_get(&ctx, "mem:key", &secret, NULL) == 0);
	FYAI_TCHECK(secret && !strcmp(secret, "first"));
	free(secret);
	FYAI_TCHECK(fyai_cred_mem_set(&ctx, "key", "second") == 0);
	FYAI_TCHECK(fyai_cred_mem_set(&ctx, "other", "third") == 0);
	FYAI_TCHECK(fyai_cred_get(&ctx, "mem:key", &secret, NULL) == 0);
	FYAI_TCHECK(secret && !strcmp(secret, "second"));
	free(secret);
	FYAI_TCHECK(fyai_cred_get(&ctx, "mem:other", &secret, NULL) == 0);
	FYAI_TCHECK(secret && !strcmp(secret, "third"));
	free(secret);
	/* No context, no memory. */
	FYAI_TCHECK(fyai_cred_get(NULL, "mem:key", &secret, NULL) == -ENOENT);
	/* A forked child keeps the secrets and loses the refresh. */
	fyai_cred_abandon(&ctx);
	FYAI_TCHECK(fyai_cred_get(&ctx, "mem:key", &secret, NULL) == 0);
	free(secret);
	fyai_cred_release(&ctx);
	FYAI_TCHECK(fyai_cred_get(&ctx, "mem:key", &secret, NULL) == -ENOENT);
	store_remove(dir);
	printf("ok - a secret in memory is set, replaced and released\n");
	return 0;
}

/* The login as a source: its token, a stale login, and no login. */
int cred_get_login(void)
{
	char *secret = NULL;
	char dir[] = DIR_TEMPLATE, path[300];

	store_setup_expiring(dir, (long long)time(NULL) + 3600);
	FYAI_TCHECK(fyai_cred_get(&ctx, "oauth:chatgpt", &secret, NULL) == 0);
	FYAI_TCHECK(secret && !strcmp(secret, "old-access"));
	free(secret);
	FYAI_TCHECK(fyai_cred_get(&ctx, "oauth:other", &secret, NULL) == -EINVAL);
	/* A chain falls through a login that is absent, not one that is stale. */
	setenv("FYAI_CRED_TEST_A", "alpha", 1);
	store_write_token("old-access", 1);
	FYAI_TCHECK(fyai_cred_get(&ctx, "oauth:chatgpt|env:FYAI_CRED_TEST_A",
				  &secret, NULL) == 0);
	FYAI_TCHECK(secret && !strcmp(secret, "alpha"));
	free(secret);
	FYAI_TCHECK(fyai_cred_get(&ctx, "oauth:chatgpt", &secret, NULL) == -ESTALE);
	snprintf(path, sizeof(path), "%s/auth.json", dir);
	unlink(path);
	FYAI_TCHECK(fyai_cred_get(&ctx, "oauth:chatgpt", &secret, NULL) == -ENOENT);
	FYAI_TCHECK(fyai_cred_get(NULL, "oauth:chatgpt", &secret, NULL) == -EINVAL);
	store_remove(dir);
	printf("ok - the login is a source with its own states\n");
	return 0;
}

/* A fresh token arms one timer for the time it is due. */
int cred_schedule_armed(void)
{
	struct fyai_event_loop *el;
	long long expires = (long long)time(NULL) + 3600;
	char dir[] = DIR_TEMPLATE;
	time_t at;

	store_setup_expiring(dir, expires);
	el = fyai_ctx_loop(&ctx);
	FYAI_TCHECK(el);
	FYAI_TCHECK(fyai_cred_scheduled_at(&ctx) == 0);
	FYAI_TCHECK(fyai_cred_prepare(&ctx, LOGIN, NULL, false) == 0);
	at = fyai_cred_scheduled_at(&ctx);
	/* Due five minutes before it expires. */
	FYAI_TCHECK(at == (time_t)expires - 300);
	/* A second request arms no second timer. */
	FYAI_TCHECK(fyai_cred_prepare(&ctx, LOGIN, NULL, false) == 0);
	FYAI_TCHECK(fyai_cred_scheduled_at(&ctx) == at);
	/* A static source arms none. */
	FYAI_TCHECK(fyai_cred_prepare(&ctx, "env:X", NULL, false) == 0);
	store_remove(dir);
	printf("ok - a fresh token arms one timer for its due time\n");
	return 0;
}

/* A token that lives hardly longer than its window is left to the requests. */
int cred_schedule_short(void)
{
	char dir[] = DIR_TEMPLATE;

	store_setup_expiring(dir, (long long)time(NULL) + 330);
	FYAI_TCHECK(fyai_cred_prepare(&ctx, LOGIN, NULL, false) == 0);
	FYAI_TCHECK(fyai_cred_scheduled_at(&ctx) == 0);
	store_remove(dir);
	printf("ok - a short token arms no timer\n");
	return 0;
}

/* The timer fires when the token is due: the refresh runs, and arms the next. */
int cred_schedule_refreshes(void)
{
	struct mock_answer script[2] = { { 200, NULL, false }, { 200, NULL, false } };
	struct fyai_event_loop *el;
	struct issuer is;
	struct mock m;
	char dir[] = DIR_TEMPLATE, *token;
	time_t at;

	store_setup_expiring(dir, (long long)time(NULL) + 3600);
	el = fyai_ctx_loop(&ctx);
	FYAI_TCHECK(el);
	issuer_open(&is);
	token = issuer_token_body(&is, "renewed-access", "renewed-refresh");
	script[0].body = is.jwks;
	script[1].body = token;
	mock_start(&m, script, 2);
	FYAI_TCHECK(fyai_cred_prepare(&ctx, LOGIN, NULL, false) == 0);
	FYAI_TCHECK(fyai_cred_scheduled_at(&ctx) != 0);

	/* The time passes: the stored token is now inside its window. */
	store_write_token("old-access", (long long)time(NULL) + 100);
	fyai_cred_timer_fire(&ctx);
	FYAI_TCHECK(fyai_cred_refresh_pending(&ctx));
	STEP_UNTIL(el, !fyai_cred_refresh_pending(&ctx));
	/* The keys and the token request, with no request waiting for them. */
	FYAI_TCHECK(m.nseen == 2 && m.extra == 0);
	FYAI_TCHECK(!fyai_auth_store_load(&ctx));
	FYAI_TCHECK(!strcmp(fyai_auth_store_token(&ctx), "renewed-access"));
	/* The next refresh is armed for the new token. */
	at = fyai_cred_scheduled_at(&ctx);
	FYAI_TCHECK(at > time(NULL) + 3000);

	free(token);
	issuer_close(&is);
	mock_stop(&m);
	store_remove(dir);
	printf("ok - the timer refreshes the login and arms the next one\n");
	return 0;
}

/* Another process renewed the token: the timer sends nothing and waits again. */
int cred_schedule_sibling(void)
{
	struct fyai_event_loop *el;
	struct mock m;
	char dir[] = DIR_TEMPLATE;
	long long later = (long long)time(NULL) + 7200;

	store_setup_expiring(dir, (long long)time(NULL) + 3600);
	el = fyai_ctx_loop(&ctx);
	FYAI_TCHECK(el);
	mock_start(&m, NULL, 0);
	FYAI_TCHECK(fyai_cred_prepare(&ctx, LOGIN, NULL, false) == 0);
	store_write_token("sibling-access", later);
	fyai_cred_timer_fire(&ctx);
	FYAI_TCHECK(!fyai_cred_refresh_pending(&ctx));
	FYAI_TCHECK(fyai_cred_scheduled_at(&ctx) == (time_t)later - 300);
	FYAI_TCHECK(m.nseen == 0);
	/* No login at all: nothing to keep fresh, and no timer. */
	{
		char path[300];

		snprintf(path, sizeof(path), "%s/auth.json", dir);
		FYAI_TCHECK(!unlink(path));
	}
	fyai_cred_timer_fire(&ctx);
	FYAI_TCHECK(fyai_cred_scheduled_at(&ctx) == 0 && !fyai_cred_refresh_pending(&ctx));
	mock_stop(&m);
	store_remove(dir);
	printf("ok - the timer waits again when another process renewed the token\n");
	return 0;
}

/* A refresh that fails is tried again later, not at once. */
int cred_schedule_failure(void)
{
	static const struct mock_answer script[] = { { 500, "{}", false } };
	struct fyai_event_loop *el;
	struct mock m;
	char dir[] = DIR_TEMPLATE;
	time_t at;

	store_setup_expiring(dir, (long long)time(NULL) + 3600);
	el = fyai_ctx_loop(&ctx);
	FYAI_TCHECK(el);
	mock_start(&m, script, 1);
	FYAI_TCHECK(fyai_cred_prepare(&ctx, LOGIN, NULL, false) == 0);
	store_write_token("old-access", (long long)time(NULL) + 100);
	fyai_cred_timer_fire(&ctx);
	STEP_UNTIL(el, !fyai_cred_refresh_pending(&ctx));
	FYAI_TCHECK(m.nseen == 1);
	at = fyai_cred_scheduled_at(&ctx);
	FYAI_TCHECK(at > time(NULL) + 200 && at <= time(NULL) + 300);
	mock_stop(&m);
	store_remove(dir);
	printf("ok - a failed scheduled refresh is tried again later\n");
	return 0;
}

/* The timer is a source of the loop, and it ends with the state. */
int cred_schedule_release(void)
{
	struct fyai_event_loop *el;
	char dir[] = DIR_TEMPLATE;
	unsigned int base;

	store_setup_expiring(dir, (long long)time(NULL) + 3600);
	el = fyai_ctx_loop(&ctx);
	FYAI_TCHECK(el);
	base = fyai_event_loop_source_count(el);
	FYAI_TCHECK(fyai_cred_prepare(&ctx, LOGIN, NULL, false) == 0);
	FYAI_TCHECK(fyai_event_loop_source_count(el) == base + 1);
	fyai_cred_release(&ctx);
	FYAI_TCHECK(fyai_event_loop_source_count(el) == base);
	FYAI_TCHECK(fyai_cred_scheduled_at(&ctx) == 0);
	store_remove(dir);
	printf("ok - the scheduled refresh ends with the state\n");
	return 0;
}
