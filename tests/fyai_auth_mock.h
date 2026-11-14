/*
 * fyai_auth_mock.h - a store of the login and an in-process provider and
 * issuer for the tests of the credential component. The test file that
 * includes it holds the cases.
 *
 * The server is fyai_httpd on the loop of the test: no process, no pipe. A
 * request takes the next answer of the script, and an answer can be held
 * until the test releases it, so a test acts between a request and its answer.
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_AUTH_MOCK_H
#define FYAI_AUTH_MOCK_H

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>

#include "fyai.h"
#include "fyai_auth.h"
#include "fyai_auth_util.h"
#include "fyai_event.h"
#include "fyai_httpd.h"
#include "fyai_test.h"
#include "fyai_test_registry.h"
#include "utils.h"

#define STEP_BOUND 2000

static struct fyai_cfg cfg;
static struct fyai_ctx ctx = { .cfg = &cfg };
static bool diag_ready;

/* Write the record of the login with @access as its access token. */
__attribute__((unused)) static void store_write_token(const char *access, long long expires)
{
	fy_generic record;
	const char *json;
	int rc;

	record = fy_mapping(cfg.gb, "type", "chatgpt", "client_id", "oaiapp_test",
		"ext_agent_host_id", "urn:uuid:00000000-0000-4000-8000-000000000000",
		"subject", "user-test",
		"scope", "resource.invoke chatgpt.tokens.use.direct",
		"access_token", access, "refresh_token", "old-refresh",
		"id_token", "old-id", "expires_at", expires);
	json = emit_json_string(cfg.gb, record);
	rc = fyai_auth_store_write(&ctx, "auth.json", json);
	FYAI_TCHECK(!rc);
}

__attribute__((unused)) static void store_setup_expiring(char *dir, long long expires)
{
	struct fy_generic_builder_cfg config = { .flags = FYGBCF_SCOPE_LEADER };

	if (!cfg.gb) {
		cfg.gb = fy_generic_builder_create(&config);
		ctx.transient_gb = cfg.gb;
	}
	/* The tests own the store: never the keyring or the keychain of the host. */
	FYAI_TCHECK(!setenv("FYAI_AUTH_STORAGE", "file", 1));
	FYAI_TCHECK(mkdtemp(dir));
	cfg.auth_state_dir = fy_gb_intern_string(cfg.gb, dir);
	/* A refresh that is not meant to happen must not reach the real issuer. */
	cfg.auth_refresh_base = "http://127.0.0.1:1";
	store_write_token("old-access", expires);
}

__attribute__((unused)) static void store_setup(char *dir)
{
	store_setup_expiring(dir, 1);
}

__attribute__((unused)) static void store_remove(const char *dir)
{
	char path[300];

	snprintf(path, sizeof(path), "%s/auth.json", dir);
	unlink(path);
	snprintf(path, sizeof(path), "%s/auth.lock", dir);
	unlink(path);
	rmdir(dir);
}

/* Step the loop until @cond holds; a bound on the steps, not a delay. */
#define STEP_UNTIL(el, cond)						\
	do {								\
		int steps_ = 0;						\
		while (!(cond) && steps_++ < STEP_BOUND)		\
			FYAI_TCHECK(fyai_event_loop_step((el), 100) >= 0); \
		FYAI_TCHECK(cond);					\
	} while (0)

/* True when no process holds the store lock. */
__attribute__((unused)) static bool lock_free(const char *dir)
{
	char path[300];
	int fd;
	bool free_;

	snprintf(path, sizeof(path), "%s/auth.lock", dir);
	fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	FYAI_TCHECK(fd >= 0);
	free_ = !flock(fd, LOCK_EX | LOCK_NB);
	close(fd);
	return free_;
}

/* ---- the provider and the issuer ------------------------------------------ */

/* One answer of the script. A held answer waits for mock_release(). */
struct mock_answer {
	int status;
	const char *body;
	bool hold;
};

struct mock_seen {
	char target[96];
	char bearer[96];
	char body[320];
};

#define MOCK_SEEN_MAX 16
#define MOCK_HELD_MAX 4

struct mock {
	struct fyai_httpd *h;
	char bound[64];
	const struct mock_answer *script;
	size_t n, next;
	struct mock_seen seen[MOCK_SEEN_MAX];
	size_t nseen;
	struct {
		struct fyai_httpd_conn *c;
		const struct mock_answer *a;
	} held[MOCK_HELD_MAX];
	size_t nheld;
	size_t dropped;		/* held requests that the client closed */
	size_t extra;		/* requests after the end of the script */
};

static void mock_answer_send(struct fyai_httpd_conn *c, const struct mock_answer *a)
{
	fyai_httpd_respond(c, a->status, NULL, "Content-Type: application/json\r\n",
			   a->body, strlen(a->body));
}

static void mock_request(void *data, struct fyai_httpd_conn *c,
			 const struct fyai_http_request *req)
{
	struct mock *m = data;
	const struct mock_answer *a;

	if (m->nseen < MOCK_SEEN_MAX) {
		snprintf(m->seen[m->nseen].target, sizeof(m->seen[0].target), "%s",
			 req->target ? req->target : "");
		snprintf(m->seen[m->nseen].bearer, sizeof(m->seen[0].bearer), "%s",
			 req->bearer ? req->bearer : "");
		snprintf(m->seen[m->nseen].body, sizeof(m->seen[0].body), "%s",
			 req->body ? req->body : "");
		m->nseen++;
	}
	if (m->next >= m->n) {
		m->extra++;
		fyai_httpd_respond(c, 500, NULL, NULL, "{}", 2);
		return;
	}
	a = &m->script[m->next++];
	if (a->hold && m->nheld < MOCK_HELD_MAX) {
		fyai_httpd_conn_hold(c);
		m->held[m->nheld].c = c;
		m->held[m->nheld].a = a;
		m->nheld++;
		return;
	}
	mock_answer_send(c, a);
}

static void mock_closed(void *data, struct fyai_httpd_conn *c)
{
	struct mock *m = data;
	size_t i;

	for (i = 0; i < m->nheld; i++) {
		if (m->held[i].c != c)
			continue;
		/* Closed before it was answered: the client dropped it. */
		m->dropped++;
		memmove(&m->held[i], &m->held[i + 1],
			(m->nheld - i - 1) * sizeof(m->held[0]));
		m->nheld--;
		return;
	}
}

/*
 * Serve @script, and point the refresh endpoints of the login at the same
 * server. Pass n = 0 for a server that answers nothing right.
 */
__attribute__((unused)) static void mock_start(struct mock *m,
					       const struct mock_answer *script, size_t n)
{
	struct fyai_httpd_opts opts = {
		.max_conns = 16, .max_output = 1u << 20, .max_body = 65536,
	};
	struct fyai_httpd_ops ops = {
		.request = mock_request, .closed = mock_closed, .data = m,
	};
	sigset_t mask;
	int rc;

	/* The program keeps SIGPIPE blocked; a write to a client that left is an error. */
	sigemptyset(&mask);
	sigaddset(&mask, SIGPIPE);
	sigprocmask(SIG_BLOCK, &mask, NULL);
	if (!diag_ready) {
		rc = fyai_diag_setup(&cfg.diag);
		FYAI_TCHECK(!rc);
		diag_ready = true;
	}
	memset(m, 0, sizeof(*m));
	m->script = script;
	m->n = n;
	m->h = fyai_httpd_new(&ctx, &opts, &ops);
	FYAI_TCHECK(m->h);
	rc = fyai_httpd_listen(m->h, "127.0.0.1:0", NULL, m->bound, sizeof(m->bound));
	FYAI_TCHECK(!rc);
	cfg.auth_refresh_base = fy_gb_intern_string(cfg.gb,
		fy_sprintfa("http://%s", m->bound));
}

__attribute__((unused)) static const char *mock_url(const struct mock *m)
{
	return fy_gb_intern_string(cfg.gb, fy_sprintfa("http://%s/", m->bound));
}

/* Answer the oldest held request. */
__attribute__((unused)) static void mock_release(struct mock *m)
{
	FYAI_TCHECK(m->nheld > 0);
	mock_answer_send(m->held[0].c, m->held[0].a);
	memmove(&m->held[0], &m->held[1], (m->nheld - 1) * sizeof(m->held[0]));
	m->nheld--;
}

__attribute__((unused)) static void mock_stop(struct mock *m)
{
	fyai_httpd_close(m->h);
	m->h = NULL;
}

/* ---- tokens that the issuer signs ------------------------------------------ */

struct issuer {
	EVP_PKEY *key;
	char *jwks;		/* the body of the key endpoint */
};

static char *issuer_bn(BIGNUM *bn)
{
	unsigned char bytes[512];
	int len = BN_num_bytes(bn);

	FYAI_TCHECK(len > 0 && (size_t)len <= sizeof(bytes));
	BN_bn2bin(bn, bytes);
	return fyai_base64url_encode(bytes, (size_t)len);
}

/* A key and its key set. */
__attribute__((unused)) static void issuer_open(struct issuer *is)
{
	EVP_PKEY_CTX *kc;
	BIGNUM *n = NULL, *e = NULL;
	char *mod, *exp;
	fy_generic key, set;
	int rc;

	memset(is, 0, sizeof(*is));
	kc = EVP_PKEY_CTX_new_from_name(NULL, "RSA", NULL);
	FYAI_TCHECK(kc && EVP_PKEY_keygen_init(kc) == 1);
	FYAI_TCHECK(EVP_PKEY_CTX_set_rsa_keygen_bits(kc, 2048) == 1);
	rc = EVP_PKEY_generate(kc, &is->key);
	FYAI_TCHECK(rc == 1);
	EVP_PKEY_CTX_free(kc);
	FYAI_TCHECK(EVP_PKEY_get_bn_param(is->key, OSSL_PKEY_PARAM_RSA_N, &n) == 1);
	FYAI_TCHECK(EVP_PKEY_get_bn_param(is->key, OSSL_PKEY_PARAM_RSA_E, &e) == 1);
	mod = issuer_bn(n);
	exp = issuer_bn(e);
	key = fy_mapping(cfg.gb, "kty", "RSA", "kid", "test-key", "alg", "RS256",
			 "use", "sig", "n", fy_value(cfg.gb, mod),
			 "e", fy_value(cfg.gb, exp));
	set = fy_mapping(cfg.gb, "keys", fy_sequence(cfg.gb, key));
	is->jwks = strdup(emit_json_string(cfg.gb, set));
	FYAI_TCHECK(is->jwks);
	free(mod);
	free(exp);
	BN_free(n);
	BN_free(e);
}

__attribute__((unused)) static void issuer_close(struct issuer *is)
{
	EVP_PKEY_free(is->key);
	free(is->jwks);
}

/* An ID token for the subject of the stored login, valid for an hour. */
static char *issuer_id_token(struct issuer *is)
{
	EVP_MD_CTX *md;
	fy_generic claims;
	const char *json;
	char *header, *payload, *unsigned_token, *encoded, *token;
	unsigned char sig[512];
	size_t len = sizeof(sig);
	long long now = (long long)time(NULL);

	claims = fy_mapping(cfg.gb, "iss", "https://auth.openai.com",
		"aud", "oaiapp_test", "sub", "user-test",
		"iat", now, "exp", now + 3600);
	json = emit_json_string(cfg.gb, fy_mapping(cfg.gb, "alg", "RS256",
						   "kid", "test-key"));
	header = fyai_base64url_encode((const unsigned char *)json, strlen(json));
	json = emit_json_string(cfg.gb, claims);
	payload = fyai_base64url_encode((const unsigned char *)json, strlen(json));
	FYAI_TCHECK(asprintf(&unsigned_token, "%s.%s", header, payload) >= 0);
	md = EVP_MD_CTX_new();
	FYAI_TCHECK(md && EVP_DigestSignInit(md, NULL, EVP_sha256(), NULL, is->key) == 1);
	FYAI_TCHECK(EVP_DigestSign(md, sig, &len,
		(const unsigned char *)unsigned_token, strlen(unsigned_token)) == 1);
	EVP_MD_CTX_free(md);
	encoded = fyai_base64url_encode(sig, len);
	FYAI_TCHECK(asprintf(&token, "%s.%s", unsigned_token, encoded) >= 0);
	free(header);
	free(payload);
	free(unsigned_token);
	free(encoded);
	return token;
}

/*
 * The body that the token endpoint answers a refresh with. The caller frees
 * it.
 */
__attribute__((unused)) static char *issuer_token_body(struct issuer *is,
						       const char *access,
						       const char *refresh)
{
	char *id = issuer_id_token(is), *body;

	FYAI_TCHECK(asprintf(&body,
		"{\"token_type\":\"Bearer\",\"expires_in\":3600,"
		"\"scope\":\"openid resource.invoke chatgpt.tokens.use.direct\","
		"\"access_token\":\"%s\",\"refresh_token\":\"%s\","
		"\"id_token\":\"%s\"}", access, refresh, id) >= 0);
	free(id);
	return body;
}


#endif
