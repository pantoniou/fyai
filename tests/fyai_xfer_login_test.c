/*
 * fyai_xfer_login_test.c - a model request that carries the ChatGPT login
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_UNKNOWN

#include "fyai_auth_mock.h"
#include "fyai_buf.h"
#include "fyai_cred.h"
#include "fyai_secret.h"
#include "fyai_xfer.h"

FYAI_TEST_ENTRY(xfer_login, sends_the_token, xfer_login_sends_the_token)
FYAI_TEST_ENTRY(xfer_login, token_changed_on_401, xfer_login_token_changed)
FYAI_TEST_ENTRY(xfer_login, second_401_is_the_answer, xfer_login_second_401)
FYAI_TEST_ENTRY(xfer_login, unrenewable_401_keeps_its_body, xfer_login_unrenewable)
FYAI_TEST_ENTRY(xfer_login, failed_refresh_ends_request, xfer_login_failed_refresh)
FYAI_TEST_ENTRY(xfer_login, due_token_waits, xfer_login_due_waits)
FYAI_TEST_ENTRY(xfer_login, due_token_is_renewed, xfer_login_due_renews)
FYAI_TEST_ENTRY(xfer_login, renews_after_401, xfer_login_renews_after_401)
FYAI_TEST_ENTRY(xfer_login, cancel_while_parked, xfer_login_cancel_parked)
FYAI_TEST_ENTRY(xfer_login, cancel_while_sent, xfer_login_cancel_sent)
FYAI_TEST_ENTRY(xfer_login, api_key_401_passes, xfer_login_api_key)
FYAI_TEST_ENTRY(xfer_login, stored_key_replaced, xfer_rekey_replaced)
FYAI_TEST_ENTRY(xfer_login, stored_key_same, xfer_rekey_same)

struct result {
	volatile bool done;
	unsigned int count;
	CURLcode code;
	long status;
	struct fyai_buf body;
};

static size_t body_write(void *ptr, size_t size, size_t nmemb, void *ud)
{
	struct result *r = ud;

	FYAI_TCHECK(!fyai_buf_append_data(&r->body, ptr, size * nmemb));
	return size * nmemb;
}

static void xfer_done(struct fyai_xfer *x, void *ud)
{
	struct result *r = ud;

	r->count++;
	r->code = fyai_xfer_result(x);
	r->status = fyai_xfer_last_status(&ctx);
	r->done = true;
}

static void request_setup(const struct mock *m)
{
	if (!ctx.curl) {
		ctx.curl = curl_easy_init();
		FYAI_TCHECK(ctx.curl);
		curl_easy_setopt(ctx.curl, CURLOPT_NOSIGNAL, 1L);
	}
	curl_slist_free_all(ctx.headers);
	ctx.headers = NULL;
	FYAI_TCHECK(!append_header(&ctx.headers, "Content-Type: application/json"));
	curl_easy_setopt(ctx.curl, CURLOPT_HTTPHEADER, ctx.headers);
	cfg.chatgpt_auth = true;
	cfg.api_url = OPENAI_RESPONSES_URL;
	fyai_xfer_set_body(&ctx, "{}");
	fyai_xfer_set_endpoint(&ctx, mock_url(m), NULL);
}

static bool body_is(const struct result *r, const char *text)
{
	return r->body.data && fyai_buf_pending(&r->body) == strlen(text) &&
	       !strncmp(r->body.data, text, strlen(text));
}

/* A fresh token goes out as it is stored, and the body reaches the caller. */
int xfer_login_sends_the_token(void)
{
	static const struct mock_answer script[] = { { 200, "fine", false } };
	struct fyai_event_loop *el;
	struct mock m;
	struct result r = { 0 };
	struct fyai_xfer *x;
	char dir[] = "/tmp/fyai-xfer-XXXXXX";

	store_setup_expiring(dir, (long long)time(NULL) + 3600);
	el = fyai_ctx_loop(&ctx);
	mock_start(&m, script, 1);
	request_setup(&m);
	x = fyai_xfer_submit(&ctx, body_write, &r, xfer_done, &r);
	FYAI_TCHECK(x);
	STEP_UNTIL(el, r.done);
	FYAI_TCHECK(r.count == 1 && r.code == CURLE_OK && r.status == 200);
	FYAI_TCHECK(body_is(&r, "fine"));
	FYAI_TCHECK(m.nseen == 1 && !strcmp(m.seen[0].bearer, "old-access"));
	fyai_xfer_destroy(x);
	fyai_buf_free(&r.body);
	mock_stop(&m);
	store_remove(dir);
	printf("ok - a request carries the stored token\n");
	return 0;
}

/*
 * The provider refuses the token that the request used, and the store holds
 * another token by then: the request goes again with the stored one, with no
 * refresh, and the first answer never reaches the caller.
 */
int xfer_login_token_changed(void)
{
	static const struct mock_answer script[] = {
		{ 401, "unauthorized", true }, { 200, "second", false } };
	struct fyai_event_loop *el;
	struct mock m;
	struct result r = { 0 };
	struct fyai_xfer *x;
	char dir[] = "/tmp/fyai-xfer-XXXXXX";

	store_setup_expiring(dir, (long long)time(NULL) + 3600);
	el = fyai_ctx_loop(&ctx);
	mock_start(&m, script, 2);
	request_setup(&m);
	x = fyai_xfer_submit(&ctx, body_write, &r, xfer_done, &r);
	FYAI_TCHECK(x);
	STEP_UNTIL(el, m.nheld == 1);
	/* A sibling renewed the login while this request was out. */
	store_write_token("new-access", (long long)time(NULL) + 3600);
	mock_release(&m);
	STEP_UNTIL(el, r.done);
	FYAI_TCHECK(r.count == 1 && r.code == CURLE_OK && r.status == 200);
	FYAI_TCHECK(body_is(&r, "second"));
	FYAI_TCHECK(m.nseen == 2 && !strcmp(m.seen[0].bearer, "old-access") &&
		    !strcmp(m.seen[1].bearer, "new-access"));
	fyai_xfer_destroy(x);
	fyai_buf_free(&r.body);
	mock_stop(&m);
	store_remove(dir);
	printf("ok - a refused token is replaced by the stored one\n");
	return 0;
}

/* A second refusal is the answer: one renewal for each request. */
int xfer_login_second_401(void)
{
	static const struct mock_answer script[] = {
		{ 401, "first", true }, { 401, "second", false } };
	struct fyai_event_loop *el;
	struct mock m;
	struct result r = { 0 };
	struct fyai_xfer *x;
	char dir[] = "/tmp/fyai-xfer-XXXXXX";

	store_setup_expiring(dir, (long long)time(NULL) + 3600);
	el = fyai_ctx_loop(&ctx);
	mock_start(&m, script, 2);
	request_setup(&m);
	x = fyai_xfer_submit(&ctx, body_write, &r, xfer_done, &r);
	FYAI_TCHECK(x);
	STEP_UNTIL(el, m.nheld == 1);
	store_write_token("new-access", (long long)time(NULL) + 3600);
	mock_release(&m);
	STEP_UNTIL(el, r.done);
	FYAI_TCHECK(r.count == 1 && r.code == CURLE_OK && r.status == 401);
	FYAI_TCHECK(body_is(&r, "second"));
	FYAI_TCHECK(m.nseen == 2 && m.extra == 0);
	fyai_xfer_destroy(x);
	fyai_buf_free(&r.body);
	mock_stop(&m);
	store_remove(dir);
	printf("ok - a second refusal is the answer\n");
	return 0;
}

/* The login is gone when the refusal comes: the caller gets the refusal. */
int xfer_login_unrenewable(void)
{
	static const struct mock_answer script[] = { { 401, "denied-body", true } };
	struct fyai_event_loop *el;
	struct mock m;
	struct result r = { 0 };
	struct fyai_xfer *x;
	char dir[] = "/tmp/fyai-xfer-XXXXXX", path[300];

	store_setup_expiring(dir, (long long)time(NULL) + 3600);
	el = fyai_ctx_loop(&ctx);
	mock_start(&m, script, 1);
	request_setup(&m);
	x = fyai_xfer_submit(&ctx, body_write, &r, xfer_done, &r);
	FYAI_TCHECK(x);
	STEP_UNTIL(el, m.nheld == 1);
	snprintf(path, sizeof(path), "%s/auth.json", dir);
	FYAI_TCHECK(!unlink(path));
	mock_release(&m);
	STEP_UNTIL(el, r.done);
	FYAI_TCHECK(r.count == 1 && r.code == CURLE_OK && r.status == 401);
	FYAI_TCHECK(body_is(&r, "denied-body"));
	FYAI_TCHECK(m.nseen == 1);
	fyai_xfer_destroy(x);
	fyai_buf_free(&r.body);
	mock_stop(&m);
	store_remove(dir);
	printf("ok - a refusal that cannot be renewed keeps its body\n");
	return 0;
}

/* The token is the refused one and the refresh fails: the request ends. */
int xfer_login_failed_refresh(void)
{
	static const struct mock_answer script[] = {
		{ 401, "denied", true }, { 500, "{}", false } };
	struct fyai_event_loop *el;
	struct mock m;
	struct result r = { 0 };
	struct fyai_xfer *x;
	char dir[] = "/tmp/fyai-xfer-XXXXXX";

	store_setup_expiring(dir, (long long)time(NULL) + 3600);
	el = fyai_ctx_loop(&ctx);
	mock_start(&m, script, 2);
	request_setup(&m);
	x = fyai_xfer_submit(&ctx, body_write, &r, xfer_done, &r);
	FYAI_TCHECK(x);
	STEP_UNTIL(el, m.nheld == 1);
	mock_release(&m);
	STEP_UNTIL(el, r.done);
	FYAI_TCHECK(r.count == 1 && r.code == CURLE_LOGIN_DENIED);
	FYAI_TCHECK(!r.body.data || !fyai_buf_pending(&r.body));
	/* The request, then the key fetch that failed; nothing after it. */
	FYAI_TCHECK(m.nseen == 2 && strstr(m.seen[1].target, "jwks"));
	fyai_xfer_destroy(x);
	fyai_buf_free(&r.body);
	mock_stop(&m);
	store_remove(dir);
	printf("ok - a failed renewal ends the request once\n");
	return 0;
}

/*
 * The token is due when the request starts: the request waits for the
 * refresh and never reaches the provider when the refresh fails.
 */
int xfer_login_due_waits(void)
{
	static const struct mock_answer script[] = { { 500, "{}", false } };
	struct fyai_event_loop *el;
	struct mock m;
	struct result r = { 0 };
	struct fyai_xfer *x;
	char dir[] = "/tmp/fyai-xfer-XXXXXX";

	store_setup(dir);
	el = fyai_ctx_loop(&ctx);
	mock_start(&m, script, 1);
	request_setup(&m);
	x = fyai_xfer_submit(&ctx, body_write, &r, xfer_done, &r);
	FYAI_TCHECK(x);
	FYAI_TCHECK(!r.done);
	STEP_UNTIL(el, r.done);
	FYAI_TCHECK(r.count == 1 && r.code == CURLE_LOGIN_DENIED);
	/* Only the key fetch of the refresh reached the server. */
	FYAI_TCHECK(m.nseen == 1 && strstr(m.seen[0].target, "jwks"));
	fyai_xfer_destroy(x);
	fyai_buf_free(&r.body);
	mock_stop(&m);
	store_remove(dir);
	printf("ok - a due token holds the request for the refresh\n");
	return 0;
}

/* The token is due, the refresh succeeds, and the request goes with the new token. */
int xfer_login_due_renews(void)
{
	struct mock_answer script[3] = {
		{ 200, NULL, false }, { 200, NULL, false }, { 200, "answer", false } };
	struct fyai_event_loop *el;
	struct issuer is;
	struct mock m;
	struct result r = { 0 };
	struct fyai_xfer *x;
	char dir[] = "/tmp/fyai-xfer-XXXXXX", *token;

	store_setup(dir);
	el = fyai_ctx_loop(&ctx);
	issuer_open(&is);
	token = issuer_token_body(&is, "renewed-access", "renewed-refresh");
	script[0].body = is.jwks;
	script[1].body = token;
	mock_start(&m, script, 3);
	request_setup(&m);
	x = fyai_xfer_submit(&ctx, body_write, &r, xfer_done, &r);
	FYAI_TCHECK(x);
	FYAI_TCHECK(!r.done);
	STEP_UNTIL(el, r.done);
	FYAI_TCHECK(r.count == 1 && r.code == CURLE_OK && r.status == 200);
	FYAI_TCHECK(body_is(&r, "answer"));
	FYAI_TCHECK(m.nseen == 3 && !strcmp(m.seen[2].bearer, "renewed-access"));
	fyai_xfer_destroy(x);
	fyai_buf_free(&r.body);
	free(token);
	issuer_close(&is);
	mock_stop(&m);
	store_remove(dir);
	printf("ok - a due token is renewed before the request goes\n");
	return 0;
}

/* The provider refuses the stored token: the login is renewed and the request repeats. */
int xfer_login_renews_after_401(void)
{
	struct mock_answer script[4] = {
		{ 401, "refused", false }, { 200, NULL, false }, { 200, NULL, false },
		{ 200, "answer", false } };
	struct fyai_event_loop *el;
	struct issuer is;
	struct mock m;
	struct result r = { 0 };
	struct fyai_xfer *x;
	char dir[] = "/tmp/fyai-xfer-XXXXXX", *token;

	store_setup_expiring(dir, (long long)time(NULL) + 3600);
	el = fyai_ctx_loop(&ctx);
	issuer_open(&is);
	token = issuer_token_body(&is, "renewed-access", "renewed-refresh");
	script[1].body = is.jwks;
	script[2].body = token;
	mock_start(&m, script, 4);
	request_setup(&m);
	x = fyai_xfer_submit(&ctx, body_write, &r, xfer_done, &r);
	FYAI_TCHECK(x);
	STEP_UNTIL(el, r.done);
	FYAI_TCHECK(r.count == 1 && r.code == CURLE_OK && r.status == 200);
	/* The refusal stayed with the transfer: only the answer came out. */
	FYAI_TCHECK(body_is(&r, "answer"));
	FYAI_TCHECK(m.nseen == 4 && m.extra == 0);
	FYAI_TCHECK(!strcmp(m.seen[0].bearer, "old-access"));
	FYAI_TCHECK(strstr(m.seen[1].target, "jwks") &&
		    strstr(m.seen[2].target, "oauth/token"));
	FYAI_TCHECK(!strcmp(m.seen[3].bearer, "renewed-access"));
	fyai_xfer_destroy(x);
	fyai_buf_free(&r.body);
	free(token);
	issuer_close(&is);
	mock_stop(&m);
	store_remove(dir);
	printf("ok - a refusal renews the login and repeats the request\n");
	return 0;
}

/* A request that waits for the refresh is cancelled: it ends once. */
int xfer_login_cancel_parked(void)
{
	static const struct mock_answer script[] = { { 200, "x", false } };
	struct fyai_event_loop *el;
	struct mock m;
	struct result r = { 0 };
	struct fyai_xfer *x;
	char dir[] = "/tmp/fyai-xfer-XXXXXX", path[300];
	int held;

	store_setup(dir);
	snprintf(path, sizeof(path), "%s/auth.lock", dir);
	held = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	FYAI_TCHECK(held >= 0 && !flock(held, LOCK_EX | LOCK_NB));
	el = fyai_ctx_loop(&ctx);
	mock_start(&m, script, 1);
	request_setup(&m);
	x = fyai_xfer_submit(&ctx, body_write, &r, xfer_done, &r);
	FYAI_TCHECK(x && !r.done);
	fyai_xfer_cancel(x);
	/* The end comes from the loop, not from the cancel. */
	FYAI_TCHECK(!r.done);
	STEP_UNTIL(el, r.done);
	FYAI_TCHECK(r.count == 1 && r.code == CURLE_ABORTED_BY_CALLBACK);
	fyai_xfer_destroy(x);
	FYAI_TCHECK(!flock(held, LOCK_UN));
	close(held);
	STEP_UNTIL(el, lock_free(dir));
	FYAI_TCHECK(m.nseen == 0);
	mock_stop(&m);
	store_remove(dir);
	printf("ok - a parked request cancels once\n");
	return 0;
}

/* A request that is out is cancelled: it ends once, with no second send. */
int xfer_login_cancel_sent(void)
{
	static const struct mock_answer script[] = {
		{ 401, "refused", true }, { 200, "again", false } };
	struct fyai_event_loop *el;
	struct mock m;
	struct result r = { 0 };
	struct fyai_xfer *x;
	char dir[] = "/tmp/fyai-xfer-XXXXXX";

	store_setup_expiring(dir, (long long)time(NULL) + 3600);
	el = fyai_ctx_loop(&ctx);
	mock_start(&m, script, 2);
	request_setup(&m);
	x = fyai_xfer_submit(&ctx, body_write, &r, xfer_done, &r);
	FYAI_TCHECK(x);
	STEP_UNTIL(el, m.nheld == 1);
	store_write_token("new-access", (long long)time(NULL) + 3600);
	fyai_xfer_cancel(x);
	STEP_UNTIL(el, r.done);
	FYAI_TCHECK(r.count == 1 && r.code == CURLE_ABORTED_BY_CALLBACK);
	FYAI_TCHECK(!r.body.data || !fyai_buf_pending(&r.body));
	STEP_UNTIL(el, m.dropped == 1);
	FYAI_TCHECK(m.nseen == 1);
	fyai_xfer_destroy(x);
	fyai_buf_free(&r.body);
	mock_stop(&m);
	store_remove(dir);
	printf("ok - a request that is out cancels once\n");
	return 0;
}

/* Without the login, a 401 is an answer like any other. */
int xfer_login_api_key(void)
{
	static const struct mock_answer script[] = { { 401, "bad-key", false } };
	struct fyai_event_loop *el;
	struct mock m;
	struct result r = { 0 };
	struct fyai_xfer *x;
	char dir[] = "/tmp/fyai-xfer-XXXXXX";

	/* The store only sets up the builders; the request never reads it. */
	store_setup_expiring(dir, (long long)time(NULL) + 3600);
	el = fyai_ctx_loop(&ctx);
	mock_start(&m, script, 1);
	request_setup(&m);
	cfg.chatgpt_auth = false;
	FYAI_TCHECK(!append_header(&ctx.headers, "Authorization: Bearer sk-test"));
	curl_easy_setopt(ctx.curl, CURLOPT_HTTPHEADER, ctx.headers);
	x = fyai_xfer_submit(&ctx, body_write, &r, xfer_done, &r);
	FYAI_TCHECK(x);
	STEP_UNTIL(el, r.done);
	FYAI_TCHECK(r.count == 1 && r.code == CURLE_OK && r.status == 401);
	FYAI_TCHECK(body_is(&r, "bad-key"));
	FYAI_TCHECK(m.nseen == 1 && !strcmp(m.seen[0].bearer, "sk-test"));
	fyai_xfer_destroy(x);
	fyai_buf_free(&r.body);
	mock_stop(&m);
	store_remove(dir);
	printf("ok - a 401 without the login is passed on\n");
	return 0;
}

/*
 * A key of the secret store: a 401 reads it again. The cases skip where the
 * platform has no secret store.
 */
static int rekey_setup(struct mock *m, const struct mock_answer *script,
		       size_t n, char *dir, char *name, size_t namelen)
{
	const char *first = "sk-first-0123456789";
	int rc;

	snprintf(name, namelen, "xfer-rekey-%d", (int)getpid());
	rc = fyai_secret_kernel_set(fy_sprintfa("fyai:%s", name), first,
				    strlen(first));
	if (rc != FYAI_SECRET_OK)
		return -1;
	store_setup_expiring(dir, (long long)time(NULL) + 3600);
	mock_start(m, script, n);
	request_setup(m);
	cfg.chatgpt_auth = false;
	cfg.api_key_explicit = true;
	cfg.api_key = fy_gb_intern_string(cfg.gb, first);
	cfg.api_key_ref = fy_gb_intern_string(cfg.gb,
					      fy_sprintfa("secret:%s", name));
	FYAI_TCHECK(!append_header(&ctx.headers, "Authorization: Bearer sk-first-0123456789"));
	ctx.auth_header = strdup("Authorization: Bearer sk-first-0123456789");
	curl_easy_setopt(ctx.curl, CURLOPT_HTTPHEADER, ctx.headers);
	return 0;
}

static void rekey_end(struct mock *m, const char *dir, const char *name)
{
	(void)fyai_secret_kernel_delete(fy_sprintfa("fyai:%s", name));
	free(ctx.auth_header);
	ctx.auth_header = NULL;
	cfg.api_key_explicit = false;
	cfg.api_key = NULL;
	cfg.api_key_ref = NULL;
	mock_stop(m);
	store_remove(dir);
}

int xfer_rekey_replaced(void)
{
	static const struct mock_answer script[] = {
		{ 401, "bad-key", false }, { 200, "fine", false } };
	const char *second = "sk-second-0123456789";
	struct fyai_event_loop *el;
	struct mock m;
	struct result r = { 0 };
	struct fyai_xfer *x;
	char dir[] = "/tmp/fyai-xfer-XXXXXX";
	char name[64];

	el = fyai_ctx_loop(&ctx);
	if (rekey_setup(&m, script, 2, dir, name, sizeof(name))) {
		printf("ok - skipped, no secret store\n");
		return 0;
	}
	FYAI_TCHECK(!fyai_secret_kernel_set(fy_sprintfa("fyai:%s", name), second,
					    strlen(second)));
	x = fyai_xfer_submit(&ctx, body_write, &r, xfer_done, &r);
	FYAI_TCHECK(x);
	STEP_UNTIL(el, r.done);
	FYAI_TCHECK(r.count == 1 && r.code == CURLE_OK && r.status == 200);
	FYAI_TCHECK(body_is(&r, "fine"));
	FYAI_TCHECK(m.nseen == 2 && !strcmp(m.seen[0].bearer, "sk-first-0123456789") &&
		    !strcmp(m.seen[1].bearer, second));
	FYAI_TCHECK(!strcmp(cfg.api_key, second));
	fyai_xfer_destroy(x);
	fyai_buf_free(&r.body);
	rekey_end(&m, dir, name);
	printf("ok - a replaced stored key is sent after a 401\n");
	return 0;
}

int xfer_rekey_same(void)
{
	static const struct mock_answer script[] = { { 401, "bad-key", false } };
	struct fyai_event_loop *el;
	struct mock m;
	struct result r = { 0 };
	struct fyai_xfer *x;
	char dir[] = "/tmp/fyai-xfer-XXXXXX";
	char name[64];

	el = fyai_ctx_loop(&ctx);
	if (rekey_setup(&m, script, 1, dir, name, sizeof(name))) {
		printf("ok - skipped, no secret store\n");
		return 0;
	}
	x = fyai_xfer_submit(&ctx, body_write, &r, xfer_done, &r);
	FYAI_TCHECK(x);
	STEP_UNTIL(el, r.done);
	FYAI_TCHECK(r.count == 1 && r.code == CURLE_OK && r.status == 401);
	FYAI_TCHECK(body_is(&r, "bad-key"));
	FYAI_TCHECK(m.nseen == 1);
	fyai_xfer_destroy(x);
	fyai_buf_free(&r.body);
	rekey_end(&m, dir, name);
	printf("ok - the same stored key leaves the 401 as the answer\n");
	return 0;
}
