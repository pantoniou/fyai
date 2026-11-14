/*
 * fyai_auth_cancel_test.c - the cancel and destroy paths of a token refresh
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_UNKNOWN

#include "fyai_auth_mock.h"

FYAI_TEST_ENTRY(auth_cancel, wait_lock, auth_cancel_wait_lock)
FYAI_TEST_ENTRY(auth_cancel, discover, auth_cancel_discover)
FYAI_TEST_ENTRY(auth_cancel, token_request_finishes, auth_cancel_token)
FYAI_TEST_ENTRY(auth_cancel, token_owner_destroys, auth_cancel_token_owner_destroys)
FYAI_TEST_ENTRY(auth_cancel, drain_waits_for_token, auth_cancel_drain)
FYAI_TEST_ENTRY(auth_cancel, models_wait, auth_cancel_models)
FYAI_TEST_ENTRY(auth_cancel, models_refresh_fails, auth_models_refresh_fails)
FYAI_TEST_ENTRY(auth_cancel, destroy_pending, auth_cancel_destroy)

typedef void (*refresh_done_fn_t)(struct fyai_auth_refresh_request *, void *);

struct done {
	unsigned int count;
	int result;
};

static void refresh_done(struct fyai_auth_refresh_request *request, void *ud)
{
	struct done *d = ud;

	d->count++;
	d->result = fyai_auth_refresh_collect(request);
}

/* The refresh waits for the store lock held by another process. */
int auth_cancel_wait_lock(void)
{
	struct fyai_auth_refresh_request *request;
	struct fyai_event_loop *el;
	struct done d = { 0 };
	char dir[] = "/tmp/fyai-auth-cancel-XXXXXX", path[300];
	int held, other, i;

	store_setup(dir);
	el = fyai_ctx_loop(&ctx);
	FYAI_TCHECK(el);
	snprintf(path, sizeof(path), "%s/auth.lock", dir);
	held = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	FYAI_TCHECK(held >= 0 && !flock(held, LOCK_EX | LOCK_NB));

	request = fyai_auth_refresh_submit(&ctx, true, refresh_done, &d);
	FYAI_TCHECK(request);
	FYAI_TCHECK(!fyai_auth_refresh_done(request));

	fyai_auth_refresh_cancel(request);
	FYAI_TCHECK(fyai_auth_refresh_done(request));
	FYAI_TCHECK(d.count == 1 && d.result != 0);
	/* A second cancel and a later turn of the loop add nothing. */
	fyai_auth_refresh_cancel(request);
	for (i = 0; i < 3; i++)
		FYAI_TCHECK(fyai_event_loop_step(el, 60) >= 0);
	FYAI_TCHECK(d.count == 1);

	/* The cancelled request holds no lock: the other holder lets go and the
	 * lock is free for a third taker. */
	FYAI_TCHECK(!flock(held, LOCK_UN));
	close(held);
	other = open(path, O_RDWR | O_CLOEXEC);
	FYAI_TCHECK(other >= 0 && !flock(other, LOCK_EX | LOCK_NB));
	close(other);

	fyai_auth_refresh_destroy(request);
	store_remove(dir);
	printf("ok - a refresh that waits for the lock cancels once\n");
	return 0;
}

/* The refresh is fetching the signing keys; nothing secret has left yet. */
int auth_cancel_discover(void)
{
	static const struct mock_answer script[] = { { 200, "{}", true } };
	struct fyai_auth_refresh_request *request;
	struct fyai_event_loop *el;
	struct mock m;
	struct done d = { 0 };
	char dir[] = "/tmp/fyai-auth-cancel-XXXXXX";

	store_setup(dir);
	el = fyai_ctx_loop(&ctx);
	FYAI_TCHECK(el);
	mock_start(&m, script, 1);
	request = fyai_auth_refresh_submit(&ctx, true, refresh_done, &d);
	FYAI_TCHECK(request);
	STEP_UNTIL(el, m.nheld == 1);

	fyai_auth_refresh_cancel(request);
	STEP_UNTIL(el, d.count > 0);
	FYAI_TCHECK(d.count == 1 && d.result != 0);
	FYAI_TCHECK(fyai_auth_refresh_done(request));
	/* The request to the keys endpoint was dropped. */
	STEP_UNTIL(el, m.dropped == 1);

	fyai_auth_refresh_destroy(request);
	mock_stop(&m);
	store_remove(dir);
	printf("ok - a refresh in the key fetch cancels once\n");
	return 0;
}

/*
 * Run a refresh to the token request, hold the answer, and cancel. The token
 * request carries the refresh token, and the provider can rotate it when it
 * reads the request, so the answer must be read: a cancel ends the wait of the
 * owner at once, and the request goes on and saves the new tokens.
 */
static void token_cancel_case(refresh_done_fn_t fn)
{
	struct fyai_auth_refresh_request *request;
	struct fyai_event_loop *el;
	struct issuer is;
	struct mock m;
	struct done d = { 0 };
	struct mock_answer script[2] = { { 200, NULL, false }, { 200, NULL, true } };
	char dir[] = "/tmp/fyai-auth-cancel-XXXXXX", *token;
	int i;

	store_setup(dir);
	el = fyai_ctx_loop(&ctx);
	FYAI_TCHECK(el);
	issuer_open(&is);
	token = issuer_token_body(&is, "new-access", "new-refresh");
	script[0].body = is.jwks;
	script[1].body = token;
	mock_start(&m, script, 2);
	request = fyai_auth_refresh_submit(&ctx, true, fn, &d);
	FYAI_TCHECK(request);
	STEP_UNTIL(el, m.nheld == 1);

	fyai_auth_refresh_cancel(request);
	FYAI_TCHECK(d.count == 1 && d.result != 0);
	/* The client keeps the connection open for the answer. */
	for (i = 0; i < 5; i++)
		FYAI_TCHECK(fyai_event_loop_step(el, 20) >= 0);
	FYAI_TCHECK(m.dropped == 0 && m.nheld == 1);
	mock_release(&m);
	/* The detached request ends on its own and lets go of the lock. */
	STEP_UNTIL(el, lock_free(dir));
	FYAI_TCHECK(d.count == 1);
	/* The rotated tokens reached the store. */
	FYAI_TCHECK(!fyai_auth_store_load(&ctx));
	FYAI_TCHECK(!strcmp(fyai_auth_store_token(&ctx), "new-access"));
	if (fn == refresh_done)
		fyai_auth_refresh_destroy(request);

	free(token);
	issuer_close(&is);
	mock_stop(&m);
	store_remove(dir);
}

int auth_cancel_token(void)
{
	token_cancel_case(refresh_done);
	printf("ok - a refresh in the token request is read to its end\n");
	return 0;
}

/* The owner destroys the request in its callback, as the stream does. */
static void refresh_done_destroy(struct fyai_auth_refresh_request *request,
				 void *ud)
{
	struct done *d = ud;

	d->count++;
	d->result = fyai_auth_refresh_collect(request);
	fyai_auth_refresh_destroy(request);
}

int auth_cancel_token_owner_destroys(void)
{
	token_cancel_case(refresh_done_destroy);
	printf("ok - the end of a token request frees a destroyed refresh\n");
	return 0;
}



/* Destroy with a refresh pending: no callback, no lock, no source left. */
int auth_cancel_destroy(void)
{
	static const struct mock_answer script[] = { { 200, "{}", true } };
	struct fyai_auth_refresh_request *request;
	struct fyai_event_loop *el;
	struct mock m;
	struct done d = { 0 };
	char dir[] = "/tmp/fyai-auth-cancel-XXXXXX", path[300];
	unsigned int base;
	int held, other, i;

	store_setup(dir);
	el = fyai_ctx_loop(&ctx);
	FYAI_TCHECK(el);
	base = fyai_event_loop_source_count(el);

	/* While it waits for the lock. */
	snprintf(path, sizeof(path), "%s/auth.lock", dir);
	held = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	FYAI_TCHECK(held >= 0 && !flock(held, LOCK_EX | LOCK_NB));
	request = fyai_auth_refresh_submit(&ctx, true, refresh_done, &d);
	FYAI_TCHECK(request);
	fyai_auth_refresh_destroy(request);
	FYAI_TCHECK(!flock(held, LOCK_UN));
	close(held);
	for (i = 0; i < 3; i++)
		FYAI_TCHECK(fyai_event_loop_step(el, 60) >= 0);
	FYAI_TCHECK(d.count == 0);
	FYAI_TCHECK(fyai_event_loop_source_count(el) == base);
	other = open(path, O_RDWR | O_CLOEXEC);
	FYAI_TCHECK(other >= 0 && !flock(other, LOCK_EX | LOCK_NB));
	close(other);

	/* While it fetches the keys. */
	mock_start(&m, script, 1);
	request = fyai_auth_refresh_submit(&ctx, true, refresh_done, &d);
	FYAI_TCHECK(request);
	STEP_UNTIL(el, m.nheld == 1);
	fyai_auth_refresh_destroy(request);
	STEP_UNTIL(el, m.dropped == 1);
	FYAI_TCHECK(d.count == 0);
	FYAI_TCHECK(lock_free(dir));

	mock_stop(&m);
	store_remove(dir);
	printf("ok - a refresh destroyed while pending leaves nothing\n");
	return 0;
}

static void models_done(struct fyai_auth_models_request *request, void *ud)
{
	struct done *d = ud;

	d->count++;
	d->result = fyai_auth_models_done(request) &&
		    fy_is_valid(fyai_auth_models_collect(request, cfg.gb, false)) ?
		    0 : -1;
}

/*
 * Model discovery waits for a token refresh and is cancelled there: its owner
 * is told once, and the request holds no lock afterwards.
 */
int auth_cancel_models(void)
{
	struct fyai_auth_models_request *request;
	struct fyai_event_loop *el;
	struct done d = { 0 };
	char dir[] = "/tmp/fyai-auth-cancel-XXXXXX", path[300];
	int held, i;

	cfg.auth_mode = FYAI_AUTH_CHATGPT;
	cfg.api_mode = FYAI_API_RESPONSES;
	cfg.provider = "openai";
	cfg.api_url = OPENAI_RESPONSES_URL;
	store_setup(dir);
	el = fyai_ctx_loop(&ctx);
	FYAI_TCHECK(el);
	snprintf(path, sizeof(path), "%s/auth.lock", dir);
	held = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	FYAI_TCHECK(held >= 0 && !flock(held, LOCK_EX | LOCK_NB));

	request = fyai_auth_models_submit(&ctx, models_done, &d);
	FYAI_TCHECK(request);
	FYAI_TCHECK(!fyai_auth_models_done(request));
	fyai_auth_models_cancel(request);
	FYAI_TCHECK(d.count == 1 && d.result != 0);
	for (i = 0; i < 3; i++)
		FYAI_TCHECK(fyai_event_loop_step(el, 60) >= 0);
	FYAI_TCHECK(d.count == 1);
	FYAI_TCHECK(!flock(held, LOCK_UN));
	close(held);
	FYAI_TCHECK(lock_free(dir));

	fyai_auth_models_destroy(request);
	store_remove(dir);
	printf("ok - model discovery cancels while it waits for a refresh\n");
	return 0;
}

/*
 * Model discovery finds the token due, waits for the refresh, and the refresh
 * fails: the owner is told once and nothing reaches the models endpoint.
 */
int auth_models_refresh_fails(void)
{
	static const struct mock_answer script[] = { { 500, "{}", false } };
	struct fyai_auth_models_request *request;
	struct fyai_event_loop *el;
	struct mock m;
	struct done d = { 0 };
	char dir[] = "/tmp/fyai-auth-cancel-XXXXXX";

	cfg.auth_mode = FYAI_AUTH_CHATGPT;
	cfg.api_mode = FYAI_API_RESPONSES;
	cfg.provider = "openai";
	cfg.api_url = OPENAI_RESPONSES_URL;
	store_setup(dir);
	el = fyai_ctx_loop(&ctx);
	FYAI_TCHECK(el);
	mock_start(&m, script, 1);
	request = fyai_auth_models_submit(&ctx, models_done, &d);
	FYAI_TCHECK(request);
	FYAI_TCHECK(!fyai_auth_models_done(request) && d.count == 0);
	STEP_UNTIL(el, d.count > 0);
	FYAI_TCHECK(d.count == 1 && d.result != 0);
	FYAI_TCHECK(fyai_auth_models_done(request));
	/* Only the key fetch of the refresh reached the server. */
	FYAI_TCHECK(m.nseen == 1 && strstr(m.seen[0].target, "jwks"));
	fyai_auth_models_destroy(request);
	mock_stop(&m);
	store_remove(dir);
	printf("ok - model discovery ends once when the refresh fails\n");
	return 0;
}

/* The invocation waits at exit for the token request that a cancel left out. */
int auth_cancel_drain(void)
{
	struct fyai_auth_refresh_request *request;
	struct fyai_event_loop *el;
	struct issuer is;
	struct mock m;
	struct done d = { 0 };
	struct mock_answer script[2] = { { 200, NULL, false }, { 200, NULL, true } };
	char dir[] = "/tmp/fyai-auth-cancel-XXXXXX", *token;

	store_setup(dir);
	el = fyai_ctx_loop(&ctx);
	FYAI_TCHECK(el);
	issuer_open(&is);
	token = issuer_token_body(&is, "new-access", "new-refresh");
	script[0].body = is.jwks;
	script[1].body = token;
	mock_start(&m, script, 2);
	request = fyai_auth_refresh_submit(&ctx, true, refresh_done_destroy, &d);
	FYAI_TCHECK(request);
	STEP_UNTIL(el, m.nheld == 1);
	fyai_auth_refresh_cancel(request);
	FYAI_TCHECK(ctx.auth_detached == 1);

	mock_release(&m);
	fyai_auth_drain(&ctx);
	FYAI_TCHECK(ctx.auth_detached == 0 && lock_free(dir));
	FYAI_TCHECK(!fyai_auth_store_load(&ctx));
	FYAI_TCHECK(!strcmp(fyai_auth_store_token(&ctx), "new-access"));

	free(token);
	issuer_close(&is);
	mock_stop(&m);
	store_remove(dir);
	printf("ok - the exit waits for a detached token request\n");
	return 0;
}
