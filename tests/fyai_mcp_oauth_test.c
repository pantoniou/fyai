/*
 * fyai_mcp_oauth_test.c - the token refresh of an MCP server
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_UNKNOWN

#include "fyai_auth_mock.h"
#include "fyai_mcp_oauth.h"

FYAI_TEST_ENTRY(mcp_oauth, refreshes_and_saves, mcp_oauth_refreshes)
FYAI_TEST_ENTRY(mcp_oauth, adopts_a_sibling_token, mcp_oauth_adopts)
FYAI_TEST_ENTRY(mcp_oauth, sends_the_stored_refresh_token, mcp_oauth_stored_refresh)
FYAI_TEST_ENTRY(mcp_oauth, failure_leaves_the_record, mcp_oauth_failure)
FYAI_TEST_ENTRY(mcp_oauth, cancel_while_waiting, mcp_oauth_cancel_wait)
FYAI_TEST_ENTRY(mcp_oauth, cancel_in_token_request_saves, mcp_oauth_cancel_token)
FYAI_TEST_ENTRY(mcp_oauth, owner_destroys_in_cancel, mcp_oauth_owner_destroys)
FYAI_TEST_ENTRY(mcp_oauth, abort_drops_the_request, mcp_oauth_abort)
FYAI_TEST_ENTRY(mcp_oauth, destroy_while_waiting, mcp_oauth_destroy_wait)

#define TOKEN_BODY(access, refresh) \
	"{\"token_type\":\"Bearer\",\"expires_in\":3600," \
	"\"access_token\":\"" access "\",\"refresh_token\":\"" refresh "\"}"

struct done {
	unsigned int count;
	enum fyai_mcp_oauth_outcome outcome;
	char access[64], refresh[64], message[160];
	time_t expires_at;
	bool destroy;
};

static void on_done(struct fyai_mcp_oauth_refresh *r,
		    const struct fyai_mcp_oauth_result *res, void *ud)
{
	struct done *d = ud;

	d->count++;
	d->outcome = res->outcome;
	snprintf(d->access, sizeof(d->access), "%s",
		 res->access_token ? res->access_token : "");
	snprintf(d->refresh, sizeof(d->refresh), "%s",
		 res->refresh_token ? res->refresh_token : "");
	snprintf(d->message, sizeof(d->message), "%s",
		 res->message ? res->message : "");
	d->expires_at = res->expires_at;
	if (d->destroy)
		fyai_mcp_oauth_refresh_destroy(r);
}

/* The record of the server in the store, as another process left it. */
static void record_write(const char *access, const char *refresh,
			 long long expires_at)
{
	fy_generic doc;
	const char *json;

	doc = fy_mapping(cfg.gb, "server", "srv", "issuer", "https://issuer.test",
		"resource", "https://mcp.test/", "client_id", "client-1",
		"client_secret", "", "scopes", "read",
		"access_token", access, "refresh_token", refresh,
		"expires_at", expires_at);
	json = emit_json_string(cfg.gb, doc);
	FYAI_TCHECK(!fyai_auth_store_write(&ctx, "mcp-auth-srv.json", json));
}

/* The stored value of a field of the record, or "". */
static const char *record_field(const char *field)
{
	char *text = fyai_auth_store_read(&ctx, "mcp-auth-srv.json");
	fy_generic doc;
	const char *v;

	FYAI_TCHECK(text);
	doc = parse_json_string(cfg.gb, text);
	free(text);
	v = fy_get(doc, field, "");
	return fy_gb_intern_string(cfg.gb, v);
}

static struct fyai_mcp_oauth_spec spec_for(const struct mock *m,
					   const char *access, const char *refresh)
{
	struct fyai_mcp_oauth_spec spec = {
		.server = "srv", .issuer = "https://issuer.test",
		.resource = "https://mcp.test/", .client_id = "client-1",
		.client_secret = "", .store_secret = "", .scopes = "read",
		.access_token = access, .refresh_token = refresh,
		.auth = FYAI_MCP_CLIENT_NONE, .timeout = 10,
	};

	spec.token_endpoint = fy_gb_intern_string(cfg.gb,
		fy_sprintfa("http://%s/oauth/token", m->bound));
	return spec;
}

/* The endpoint gives new tokens; the record holds them when the owner is told. */
int mcp_oauth_refreshes(void)
{
	static const struct mock_answer script[] = {
		{ 200, TOKEN_BODY("access-2", "refresh-2"), false } };
	struct fyai_mcp_oauth_refresh *r;
	struct fyai_event_loop *el;
	struct fyai_mcp_oauth_spec spec;
	struct done d = { 0 };
	struct mock m;
	char dir[] = "/tmp/fyai-mcp-oauth-XXXXXX";

	store_setup(dir);
	record_write("access-1", "refresh-1", 1);
	el = fyai_ctx_loop(&ctx);
	mock_start(&m, script, 1);
	spec = spec_for(&m, "access-1", "refresh-1");
	r = fyai_mcp_oauth_refresh_submit(&ctx, &spec, on_done, &d);
	FYAI_TCHECK(r && d.count == 0);
	STEP_UNTIL(el, d.count > 0);
	FYAI_TCHECK(d.count == 1 && d.outcome == FYAI_MCP_OAUTH_REFRESHED);
	FYAI_TCHECK(!strcmp(d.access, "access-2") && !strcmp(d.refresh, "refresh-2"));
	FYAI_TCHECK(d.expires_at > time(NULL) + 3000);
	FYAI_TCHECK(m.nseen == 1 && strstr(m.seen[0].target, "oauth/token"));
	FYAI_TCHECK(strstr(m.seen[0].body, "grant_type=refresh_token") &&
		    strstr(m.seen[0].body, "refresh_token=refresh-1") &&
		    strstr(m.seen[0].body, "client_id=client-1"));
	FYAI_TCHECK(!strcmp(record_field("access_token"), "access-2"));
	FYAI_TCHECK(!strcmp(record_field("refresh_token"), "refresh-2"));
	FYAI_TCHECK(lock_free(dir));
	fyai_mcp_oauth_refresh_destroy(r);
	mock_stop(&m);
	store_remove(dir);
	printf("ok - an MCP refresh saves the new tokens\n");
	return 0;
}

/* Another process renewed the token: take it, and send nothing. */
int mcp_oauth_adopts(void)
{
	struct fyai_mcp_oauth_refresh *r;
	struct fyai_event_loop *el;
	struct fyai_mcp_oauth_spec spec;
	struct done d = { 0 };
	struct mock m;
	char dir[] = "/tmp/fyai-mcp-oauth-XXXXXX";

	store_setup(dir);
	record_write("sibling-access", "sibling-refresh", (long long)time(NULL) + 3600);
	el = fyai_ctx_loop(&ctx);
	mock_start(&m, NULL, 0);
	spec = spec_for(&m, "access-1", "refresh-1");
	r = fyai_mcp_oauth_refresh_submit(&ctx, &spec, on_done, &d);
	FYAI_TCHECK(r);
	STEP_UNTIL(el, d.count > 0);
	FYAI_TCHECK(d.count == 1 && d.outcome == FYAI_MCP_OAUTH_ADOPTED);
	FYAI_TCHECK(!strcmp(d.access, "sibling-access") &&
		    !strcmp(d.refresh, "sibling-refresh"));
	FYAI_TCHECK(m.nseen == 0);
	FYAI_TCHECK(lock_free(dir));
	fyai_mcp_oauth_refresh_destroy(r);
	mock_stop(&m);
	store_remove(dir);
	printf("ok - an MCP refresh adopts the token of another process\n");
	return 0;
}

/* The sibling rotated the refresh token and its access token is due: use it. */
int mcp_oauth_stored_refresh(void)
{
	static const struct mock_answer script[] = {
		{ 200, TOKEN_BODY("access-3", "refresh-3"), false } };
	struct fyai_mcp_oauth_refresh *r;
	struct fyai_event_loop *el;
	struct fyai_mcp_oauth_spec spec;
	struct done d = { 0 };
	struct mock m;
	char dir[] = "/tmp/fyai-mcp-oauth-XXXXXX";

	store_setup(dir);
	record_write("access-1", "rotated-refresh", 1);
	el = fyai_ctx_loop(&ctx);
	mock_start(&m, script, 1);
	spec = spec_for(&m, "access-1", "stale-refresh");
	r = fyai_mcp_oauth_refresh_submit(&ctx, &spec, on_done, &d);
	FYAI_TCHECK(r);
	STEP_UNTIL(el, d.count > 0);
	FYAI_TCHECK(d.outcome == FYAI_MCP_OAUTH_REFRESHED);
	FYAI_TCHECK(m.nseen == 1 && strstr(m.seen[0].body, "refresh_token=rotated-refresh") &&
		    !strstr(m.seen[0].body, "stale-refresh"));
	fyai_mcp_oauth_refresh_destroy(r);
	mock_stop(&m);
	store_remove(dir);
	printf("ok - an MCP refresh sends the stored refresh token\n");
	return 0;
}

/* The endpoint refuses: the owner is told once, and the record stays. */
int mcp_oauth_failure(void)
{
	static const struct mock_answer script[] = { { 400, "{\"error\":\"invalid_grant\"}", false } };
	struct fyai_mcp_oauth_refresh *r;
	struct fyai_event_loop *el;
	struct fyai_mcp_oauth_spec spec;
	struct done d = { 0 };
	struct mock m;
	char dir[] = "/tmp/fyai-mcp-oauth-XXXXXX";

	store_setup(dir);
	record_write("access-1", "refresh-1", 1);
	el = fyai_ctx_loop(&ctx);
	mock_start(&m, script, 1);
	spec = spec_for(&m, "access-1", "refresh-1");
	r = fyai_mcp_oauth_refresh_submit(&ctx, &spec, on_done, &d);
	FYAI_TCHECK(r);
	STEP_UNTIL(el, d.count > 0);
	FYAI_TCHECK(d.count == 1 && d.outcome == FYAI_MCP_OAUTH_FAILED);
	FYAI_TCHECK(strstr(d.message, "HTTP 400"));
	FYAI_TCHECK(!strcmp(record_field("refresh_token"), "refresh-1"));
	FYAI_TCHECK(lock_free(dir));
	fyai_mcp_oauth_refresh_destroy(r);
	mock_stop(&m);
	store_remove(dir);
	printf("ok - a refused MCP refresh leaves the record\n");
	return 0;
}

/* A refresh that waits for the store lock is cancelled: it ends once. */
int mcp_oauth_cancel_wait(void)
{
	struct fyai_mcp_oauth_refresh *r;
	struct fyai_event_loop *el;
	struct fyai_mcp_oauth_spec spec;
	struct done d = { 0 };
	struct mock m;
	char dir[] = "/tmp/fyai-mcp-oauth-XXXXXX", path[300];
	int held, i;

	store_setup(dir);
	record_write("access-1", "refresh-1", 1);
	snprintf(path, sizeof(path), "%s/auth.lock", dir);
	held = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	FYAI_TCHECK(held >= 0 && !flock(held, LOCK_EX | LOCK_NB));
	el = fyai_ctx_loop(&ctx);
	mock_start(&m, NULL, 0);
	spec = spec_for(&m, "access-1", "refresh-1");
	r = fyai_mcp_oauth_refresh_submit(&ctx, &spec, on_done, &d);
	FYAI_TCHECK(r);
	for (i = 0; i < 3; i++)
		FYAI_TCHECK(fyai_event_loop_step(el, 20) >= 0);
	FYAI_TCHECK(d.count == 0);
	fyai_mcp_oauth_refresh_cancel(r);
	FYAI_TCHECK(d.count == 1 && d.outcome == FYAI_MCP_OAUTH_CANCELLED);
	fyai_mcp_oauth_refresh_cancel(r);
	FYAI_TCHECK(!flock(held, LOCK_UN));
	close(held);
	for (i = 0; i < 3; i++)
		FYAI_TCHECK(fyai_event_loop_step(el, 60) >= 0);
	FYAI_TCHECK(d.count == 1 && m.nseen == 0 && lock_free(dir));
	fyai_mcp_oauth_refresh_destroy(r);
	mock_stop(&m);
	store_remove(dir);
	printf("ok - an MCP refresh that waits for the lock cancels once\n");
	return 0;
}

/*
 * The token request carries the refresh token. A cancel tells the owner at
 * once, and the answer is still read and saved.
 */
static void token_cancel_case(bool owner_destroys)
{
	static const struct mock_answer script[] = {
		{ 200, TOKEN_BODY("access-2", "refresh-2"), true } };
	struct fyai_mcp_oauth_refresh *r;
	struct fyai_event_loop *el;
	struct fyai_mcp_oauth_spec spec;
	struct done d = { .destroy = owner_destroys };
	struct mock m;
	char dir[] = "/tmp/fyai-mcp-oauth-XXXXXX";
	int i;

	store_setup(dir);
	record_write("access-1", "refresh-1", 1);
	el = fyai_ctx_loop(&ctx);
	mock_start(&m, script, 1);
	spec = spec_for(&m, "access-1", "refresh-1");
	r = fyai_mcp_oauth_refresh_submit(&ctx, &spec, on_done, &d);
	FYAI_TCHECK(r);
	STEP_UNTIL(el, m.nheld == 1);

	fyai_mcp_oauth_refresh_cancel(r);
	FYAI_TCHECK(d.count == 1 && d.outcome == FYAI_MCP_OAUTH_CANCELLED);
	for (i = 0; i < 5; i++)
		FYAI_TCHECK(fyai_event_loop_step(el, 20) >= 0);
	FYAI_TCHECK(m.dropped == 0 && m.nheld == 1);
	mock_release(&m);
	STEP_UNTIL(el, lock_free(dir));
	FYAI_TCHECK(d.count == 1);
	/* The rotated tokens reached the store. */
	FYAI_TCHECK(!strcmp(record_field("access_token"), "access-2"));
	FYAI_TCHECK(!strcmp(record_field("refresh_token"), "refresh-2"));
	if (!owner_destroys)
		fyai_mcp_oauth_refresh_destroy(r);
	mock_stop(&m);
	store_remove(dir);
}

int mcp_oauth_cancel_token(void)
{
	token_cancel_case(false);
	printf("ok - a cancelled MCP token request still saves its answer\n");
	return 0;
}

int mcp_oauth_owner_destroys(void)
{
	token_cancel_case(true);
	printf("ok - the end of an MCP token request frees a destroyed refresh\n");
	return 0;
}

/* A logout aborts: the token request is dropped and the record stays. */
int mcp_oauth_abort(void)
{
	static const struct mock_answer script[] = {
		{ 200, TOKEN_BODY("access-2", "refresh-2"), true } };
	struct fyai_mcp_oauth_refresh *r;
	struct fyai_event_loop *el;
	struct fyai_mcp_oauth_spec spec;
	struct done d = { 0 };
	struct mock m;
	char dir[] = "/tmp/fyai-mcp-oauth-XXXXXX";

	store_setup(dir);
	record_write("access-1", "refresh-1", 1);
	el = fyai_ctx_loop(&ctx);
	mock_start(&m, script, 1);
	spec = spec_for(&m, "access-1", "refresh-1");
	r = fyai_mcp_oauth_refresh_submit(&ctx, &spec, on_done, &d);
	FYAI_TCHECK(r);
	STEP_UNTIL(el, m.nheld == 1);
	fyai_mcp_oauth_refresh_abort(r);
	STEP_UNTIL(el, m.dropped == 1);
	FYAI_TCHECK(d.count == 0);
	FYAI_TCHECK(lock_free(dir));
	FYAI_TCHECK(!strcmp(record_field("refresh_token"), "refresh-1"));
	mock_stop(&m);
	store_remove(dir);
	printf("ok - an aborted MCP refresh leaves the record\n");
	return 0;
}

/* Destroy while the refresh waits for the lock: no callback, nothing left. */
int mcp_oauth_destroy_wait(void)
{
	struct fyai_mcp_oauth_refresh *r;
	struct fyai_event_loop *el;
	struct fyai_mcp_oauth_spec spec;
	struct done d = { 0 };
	struct mock m;
	char dir[] = "/tmp/fyai-mcp-oauth-XXXXXX", path[300];
	unsigned int base;
	int held, i;

	store_setup(dir);
	record_write("access-1", "refresh-1", 1);
	snprintf(path, sizeof(path), "%s/auth.lock", dir);
	held = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	FYAI_TCHECK(held >= 0 && !flock(held, LOCK_EX | LOCK_NB));
	el = fyai_ctx_loop(&ctx);
	base = fyai_event_loop_source_count(el);
	mock_start(&m, NULL, 0);
	spec = spec_for(&m, "access-1", "refresh-1");
	base = fyai_event_loop_source_count(el);
	r = fyai_mcp_oauth_refresh_submit(&ctx, &spec, on_done, &d);
	FYAI_TCHECK(r);
	fyai_mcp_oauth_refresh_destroy(r);
	FYAI_TCHECK(!flock(held, LOCK_UN));
	close(held);
	for (i = 0; i < 3; i++)
		FYAI_TCHECK(fyai_event_loop_step(el, 60) >= 0);
	FYAI_TCHECK(d.count == 0 && m.nseen == 0);
	FYAI_TCHECK(fyai_event_loop_source_count(el) == base);
	FYAI_TCHECK(lock_free(dir));
	mock_stop(&m);
	store_remove(dir);
	printf("ok - an MCP refresh destroyed while it waits leaves nothing\n");
	return 0;
}
