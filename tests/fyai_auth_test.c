/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/core_names.h>

#include "fyai.h"
#include "fyai_auth.h"
#include "fyai_auth_util.h"
#include "fyai_provider.h"
#include "fyai_cmd.h"
#include "fyai_test.h"
#include "fyai_test_registry.h"
#include "utils.h"

FYAI_TEST_ENTRY(auth, id_token_signature, auth_id_token_signature)
FYAI_TEST_ENTRY(auth, id_token_claims, auth_id_token_claims)
FYAI_TEST_ENTRY(auth, plan_permission, auth_plan_permission)
FYAI_TEST_ENTRY(auth, subscription_request, auth_subscription_request)
FYAI_TEST_ENTRY(auth, subscription_errors, auth_subscription_errors)
FYAI_TEST_ENTRY(auth, malformed_base64, auth_malformed_base64)
FYAI_TEST_ENTRY(auth, billing_route, auth_billing_route)
FYAI_TEST_ENTRY(auth, recorded_usage, auth_recorded_usage)
FYAI_TEST_ENTRY(auth, manual_input, auth_manual_input)

struct token_fixture {
	struct fy_generic_builder *gb;
	EVP_PKEY *key;
	fy_generic jwks;
	fy_generic claims;
};

static char *bn_encoded(BIGNUM *bn)
{
	unsigned char bytes[512];
	int len;

	len = BN_num_bytes(bn);
	FYAI_TCHECK(len > 0 && (size_t)len <= sizeof(bytes));
	BN_bn2bin(bn, bytes);
	return fyai_base64url_encode(bytes, (size_t)len);
}

static void fixture_open(struct token_fixture *f)
{
	struct fy_generic_builder_cfg cfg = { .flags = FYGBCF_SCOPE_LEADER };
	EVP_PKEY_CTX *keyctx;
	BIGNUM *n = NULL, *e = NULL;
	char *modulus, *exponent;
	int rc;

	memset(f, 0, sizeof(*f));
	f->gb = fy_generic_builder_create(&cfg);
	FYAI_TCHECK(f->gb);
	keyctx = EVP_PKEY_CTX_new_from_name(NULL, "RSA", NULL);
	FYAI_TCHECK(keyctx);
	rc = EVP_PKEY_keygen_init(keyctx);
	FYAI_TCHECK(rc == 1);
	rc = EVP_PKEY_CTX_set_rsa_keygen_bits(keyctx, 2048);
	FYAI_TCHECK(rc == 1);
	rc = EVP_PKEY_generate(keyctx, &f->key);
	FYAI_TCHECK(rc == 1);
	EVP_PKEY_CTX_free(keyctx);
	rc = EVP_PKEY_get_bn_param(f->key, OSSL_PKEY_PARAM_RSA_N, &n);
	FYAI_TCHECK(rc == 1);
	rc = EVP_PKEY_get_bn_param(f->key, OSSL_PKEY_PARAM_RSA_E, &e);
	FYAI_TCHECK(rc == 1);
	modulus = bn_encoded(n);
	exponent = bn_encoded(e);
	FYAI_TCHECK(modulus && exponent);
	f->jwks = fy_mapping(f->gb, "keys", fy_sequence(f->gb,
		fy_mapping(f->gb, "kty", "RSA", "kid", "test-key", "alg", "RS256",
			"use", "sig", "n", fy_value(f->gb, modulus), "e", fy_value(f->gb, exponent))));
	free(modulus);
	free(exponent);
	BN_free(n);
	BN_free(e);
	f->claims = fy_mapping(f->gb, "iss", "https://auth.openai.com",
		"aud", "oaiapp_fyai", "sub", "user-test", "nonce", "nonce-test",
		"iat", 1000LL, "exp", 2000LL);
}

static void fixture_close(struct token_fixture *f)
{
	EVP_PKEY_free(f->key);
	fy_generic_builder_destroy(f->gb);
}

static char *sign_token(struct token_fixture *f, fy_generic claims, const char *alg)
{
	EVP_MD_CTX *md;
	const char *json;
	char *header, *payload, *unsigned_token, *encoded, *token;
	unsigned char signature[512];
	size_t len = sizeof(signature);
	int rc;

	json = emit_json_string(f->gb, fy_mapping(f->gb, "alg", alg, "kid", "test-key"));
	header = fyai_base64url_encode((const unsigned char *)json, strlen(json));
	json = emit_json_string(f->gb, claims);
	payload = fyai_base64url_encode((const unsigned char *)json, strlen(json));
	rc = asprintf(&unsigned_token, "%s.%s", header, payload);
	FYAI_TCHECK(rc >= 0);
	free(header);
	free(payload);
	md = EVP_MD_CTX_new();
	FYAI_TCHECK(md);
	rc = EVP_DigestSignInit(md, NULL, EVP_sha256(), NULL, f->key);
	FYAI_TCHECK(rc == 1);
	rc = EVP_DigestSign(md, signature, &len,
		(const unsigned char *)unsigned_token, strlen(unsigned_token));
	FYAI_TCHECK(rc == 1);
	EVP_MD_CTX_free(md);
	encoded = fyai_base64url_encode(signature, len);
	rc = asprintf(&token, "%s.%s", unsigned_token, encoded);
	FYAI_TCHECK(rc >= 0);
	free(unsigned_token);
	free(encoded);
	return token;
}

static fy_generic verify(struct token_fixture *f, const char *token,
			 fy_generic jwks, const char *client, const char *nonce,
			 const char *subject)
{
	return fyai_auth_verify_id_token(f->gb, token, jwks, client, nonce, subject, 1500);
}

int auth_id_token_signature(void)
{
	struct token_fixture f;
	fy_generic result;
	char *token;
	char *signature;

	fixture_open(&f);
	token = sign_token(&f, f.claims, "RS256");
	result = verify(&f, token, f.jwks, "oaiapp_fyai", "nonce-test", NULL);
	FYAI_TCHECK(fy_equal(fy_get(result, "sub", ""), "user-test"));
	signature = strrchr(token, '.') + 1;
	signature[0] = signature[0] == 'A' ? 'B' : 'A';
	result = verify(&f, token, f.jwks, "oaiapp_fyai", "nonce-test", NULL);
	FYAI_TCHECK(fy_is_invalid(result));
	free(token);
	token = sign_token(&f, f.claims, "none");
	result = verify(&f, token, f.jwks, "oaiapp_fyai", "nonce-test", NULL);
	FYAI_TCHECK(fy_is_invalid(result));
	free(token);
	token = sign_token(&f, f.claims, "RS256");
	result = verify(&f, token, fy_mapping(f.gb, "keys", fy_seq_empty),
		"oaiapp_fyai", "nonce-test", NULL);
	FYAI_TCHECK(fy_is_invalid(result));
	free(token);
	fixture_close(&f);
	return 0;
}

int auth_id_token_claims(void)
{
	struct token_fixture f;
	fy_generic claims, result, change;
	fy_generic cases, key, value;
	char *token;

	fixture_open(&f);
	cases = fy_sequence(f.gb,
		fy_mapping(f.gb, "iss", "https://attacker.invalid"),
		fy_mapping(f.gb, "aud", "another-client"),
		fy_mapping(f.gb, "exp", 1500LL),
		fy_mapping(f.gb, "iat", 1510LL),
		fy_mapping(f.gb, "nbf", 1510LL),
		fy_mapping(f.gb, "nonce", "wrong-nonce"),
		fy_mapping(f.gb, "sub", ""),
		fy_mapping(f.gb, "azp", "another-client"));
	fy_foreach(change, cases) {
		claims = f.claims;
		fy_foreach_key_value(key, value, change)
			claims = fy_assoc(f.gb, claims, key, value);
		token = sign_token(&f, claims, "RS256");
		result = verify(&f, token, f.jwks, "oaiapp_fyai", "nonce-test", NULL);
		FYAI_TCHECK(fy_is_invalid(result));
		free(token);
	}
	token = sign_token(&f, f.claims, "RS256");
	result = verify(&f, token, f.jwks, "oaiapp_fyai", "nonce-test", "another-user");
	FYAI_TCHECK(fy_is_invalid(result));
	result = verify(&f, token, f.jwks, "oaiapp_fyai", NULL, "user-test");
	FYAI_TCHECK(fy_is_valid(result));
	free(token);
	claims = fy_assoc(f.gb, f.claims, "aud", fy_sequence(f.gb, "other", "oaiapp_fyai"),
		"azp", "oaiapp_fyai");
	token = sign_token(&f, claims, "RS256");
	result = verify(&f, token, f.jwks, "oaiapp_fyai", "nonce-test", NULL);
	FYAI_TCHECK(fy_is_valid(result));
	free(token);
	fixture_close(&f);
	return 0;
}

int auth_plan_permission(void)
{
	struct fy_generic_builder_cfg config = { .flags = FYGBCF_SCOPE_LEADER };
	struct fyai_cfg cfg = {};
	struct fyai_ctx ctx = { .cfg = &cfg };
	struct fyai_credentials c = { .registrations = fy_invalid };
	fy_generic tokens, invalid;
	const char *old_access;
	int rc;
	bool ready;

	cfg.gb = fy_generic_builder_create(&config);
	ctx.transient_gb = cfg.gb;
	tokens = fy_mapping(cfg.gb, "access_token", "oauth-access",
		"refresh_token", "refresh-1", "id_token", "id-token",
		"expires_in", 3600LL, "token_type", "Bearer",
		"scope", "openid offline_access resource.invoke chatgpt.tokens.use.direct");
	rc = fyai_auth_parse_tokens(&ctx, &c, tokens, false);
	FYAI_TCHECK(!rc);
	ready = fyai_auth_credentials_ready(&c);
	FYAI_TCHECK(!ready);
	c.client_id = "oaiapp_fyai";
	c.host_id = "urn:uuid:00000000-0000-4000-8000-000000000000";
	c.subject = "user-test";
	ready = fyai_auth_credentials_ready(&c);
	FYAI_TCHECK(ready);
	old_access = c.access_token;
	invalid = fy_assoc(cfg.gb, tokens, "scope", "openid resource.invoke chatgpt.tokens.use.direct.extra");
	rc = fyai_auth_parse_tokens(&ctx, &c, invalid, false);
	FYAI_TCHECK(rc && c.access_token == old_access);
	invalid = fy_delete_at_path(cfg.gb, tokens, "scope");
	rc = fyai_auth_parse_tokens(&ctx, &c, invalid, false);
	FYAI_TCHECK(rc);
	invalid = fy_assoc(cfg.gb, invalid, "refresh_token", "refresh-2");
	rc = fyai_auth_parse_tokens(&ctx, &c, invalid, true);
	FYAI_TCHECK(!rc && !strcmp(c.refresh_token, "refresh-2"));
	invalid = fy_assoc(cfg.gb, tokens, "scope", "openid");
	rc = fyai_auth_parse_tokens(&ctx, &c, invalid, true);
	FYAI_TCHECK(rc);
	c.client_id = "dynamic_agent_client";
	ready = fyai_auth_credentials_ready(&c);
	FYAI_TCHECK(!ready);
	fyai_auth_cleanup(&ctx);
	fy_generic_builder_destroy(cfg.gb);
	return 0;
}

int auth_subscription_request(void)
{
	struct fy_generic_builder_cfg cfg = { .flags = FYGBCF_SCOPE_LEADER };
	struct fy_generic_builder *gb;
	fy_generic request, result, tool, input, fields, key;

	gb = fy_generic_builder_create(&cfg);
	request = fy_mapping(gb, "model", "test-model", "store", true, "stream", false,
		"input", fy_sequence(gb,
			fy_mapping(gb, "type", "message", "role", "system", "content", "prompt"),
			fy_mapping(gb, "type", "function_call", "name", "shell", "call_id", "call-1"),
			fy_mapping(gb, "type", "function_call_output", "call_id", "call-1", "output", "done")),
		"tools", fy_sequence(gb,
			fy_mapping(gb, "type", "function", "name", "shell", "parameters", fy_map_empty),
			fy_mapping(gb, "type", "web_search")));
	fields = fy_sequence(gb, "background", "conversation", "max_output_tokens",
		"max_tool_calls", "metadata", "moderation", "multi_agent", "prompt",
		"prompt_cache_retention", "safety_identifier", "temperature", "top_logprobs",
		"top_p", "truncation", "user", "previous_response_id");
	fy_foreach(key, fields)
		request = fy_assoc(gb, request, key, "unsupported");
	result = fyai_auth_subscription_request(gb, request);
	FYAI_TCHECK(fy_is_valid(result));
	FYAI_TCHECK(!fy_get(result, "store", true) && fy_get(result, "stream", false));
	fy_foreach(key, fields) {
		tool = fy_get(result, key, fy_invalid);
		FYAI_TCHECK(fy_is_invalid(tool));
	}
	input = fy_get(result, "input", fy_invalid);
	FYAI_TCHECK(fy_len(input) == 3);
	FYAI_TCHECK(fy_equal(fy_get(fy_get_at(input, 0), "role", ""), "developer"));
	FYAI_TCHECK(fy_equal(fy_get(fy_get_at(input, 1), "namespace", ""), "fyai"));
	tool = fy_get_at(fy_get(result, "tools", fy_invalid), 1);
	FYAI_TCHECK(fy_equal(fy_get(tool, "type", ""), "namespace"));
	FYAI_TCHECK(fy_len(fy_get(tool, "tools", fy_invalid)) == 1);
	request = fy_assoc(gb, request, "tools", fy_sequence(gb, fy_mapping(gb, "type", "tool_search")));
	result = fyai_auth_subscription_request(gb, request);
	FYAI_TCHECK(fy_is_invalid(result));
	fy_generic_builder_destroy(gb);
	return 0;
}

int auth_subscription_errors(void)
{
	bool transient;

	transient = fyai_provider_error_transient(fy_mapping("code", "subscription_sharing_usage_limit_exceeded",
		"type", "rate_limit_error"));
	FYAI_TCHECK(!transient);
	transient = fyai_provider_error_transient(fy_mapping("code", "subscription_sharing_usage_unavailable"));
	FYAI_TCHECK(transient);
	transient = fyai_provider_error_transient(fy_mapping("code", "subscription_sharing_user_unavailable"));
	FYAI_TCHECK(transient);
	return 0;
}

int auth_malformed_base64(void)
{
	const char *cases[] = { "A", "!!!!", "AB=C", "A.AA", "AA+_", NULL };
	unsigned char *decoded;
	size_t len, i;

	for (i = 0; cases[i]; i++) {
		decoded = fyai_base64url_decode(cases[i], &len);
		FYAI_TCHECK(!decoded);
	}
	decoded = fyai_base64url_decode("SGVsbG8", &len);
	FYAI_TCHECK(decoded && len == 5 && !memcmp(decoded, "Hello", 5));
	free(decoded);
	return 0;
}

int auth_billing_route(void)
{
	struct fy_generic_builder_cfg config = { .flags = FYGBCF_SCOPE_LEADER };
	struct fyai_cfg cfg = { .auth_mode = FYAI_AUTH_CHATGPT,
		.api_mode = FYAI_API_RESPONSES, .provider = "openai",
		.response_compaction_supported = true, .token_extents = true,
		.api_key = "unused-api-key", .api_url = OPENAI_RESPONSES_URL };
	struct fyai_ctx ctx = { .cfg = &cfg };
	fy_generic record;
	char dir[] = "/tmp/fyai-auth-test-XXXXXX";
	char path[256];
	const char *created, *json;
	int rc;

	created = mkdtemp(dir);
	FYAI_TCHECK(created);
	cfg.auth_state_dir = created;
	cfg.gb = fy_generic_builder_create(&config);
	ctx.transient_gb = cfg.gb;
	record = fy_mapping(cfg.gb, "type", "chatgpt", "client_id", "oaiapp_test",
		"ext_agent_host_id", "urn:uuid:00000000-0000-4000-8000-000000000000",
		"subject", "user-test", "scope", "resource.invoke chatgpt.tokens.use.direct",
		"access_token", "subscription-token", "refresh_token", "refresh-token",
		"id_token", "retained-token", "expires_at", (long long)time(NULL) + 3600);
	json = emit_json_string(cfg.gb, record);
	rc = fyai_auth_store_write(&ctx, "auth.json", json);
	FYAI_TCHECK(!rc);
	rc = fyai_auth_resolve(&ctx);
	FYAI_TCHECK(!rc && cfg.chatgpt_auth && cfg.stream);
	FYAI_TCHECK(!strcmp(cfg.api_url, OPENAI_RESPONSES_URL));
	FYAI_TCHECK(!cfg.response_compaction_supported && !cfg.token_extents);
	FYAI_TCHECK(!strcmp(ctx.auth.access_token, "subscription-token"));
	cfg.chatgpt_auth = false;
	cfg.auth_mode = FYAI_AUTH_AUTO;
	rc = fyai_auth_resolve(&ctx);
	FYAI_TCHECK(!rc && !cfg.chatgpt_auth);
	cfg.api_key = NULL;
	record = fy_mapping(cfg.gb, "access_token", "legacy-token", "id_token", "legacy-id",
		"refresh_token", "legacy-refresh");
	json = emit_json_string(cfg.gb, record);
	rc = fyai_auth_store_write(&ctx, "auth.json", json);
	FYAI_TCHECK(!rc);
	rc = fyai_auth_resolve(&ctx);
	FYAI_TCHECK(rc && !cfg.chatgpt_auth);
	fyai_auth_cleanup(&ctx);
	fy_generic_builder_destroy(cfg.gb);
	snprintf(path, sizeof(path), "%s/auth.json", created);
	unlink(path);
	rmdir(created);
	return 0;
}

int auth_recorded_usage(void)
{
	struct fy_generic_builder_cfg gbcfg = { .flags = FYGBCF_SCOPE_LEADER };
	struct fy_generic_builder *gb = fy_generic_builder_create(&gbcfg);
	struct fyai_ctx ctx = { .last_message = fy_invalid };
	fy_generic result;

	FYAI_TCHECK(gb != NULL);
	FYAI_TCHECK(!fyai_auth_usage(&ctx, gb, true, &result));
	FYAI_TCHECK(fy_get(result, "total_tokens", -1LL) == 0);
	FYAI_TCHECK(fy_equal(fy_get(result, "scope"), "Selected conversation"));
	FYAI_TCHECK(fy_is_string(fy_get(result, "plan_remaining")));
	ctx.last_message = fy_mapping(gb, "metadata", fy_mapping(gb,
		"usage", fy_mapping(gb, "input", 120, "output", 30, "total", 150)),
		"previous", fy_mapping(gb, "metadata", fy_mapping(gb,
			"usage", fy_mapping(gb, "input", 80, "cached", 40,
				"output", 20, "total", 100))));
	FYAI_TCHECK(!fyai_auth_usage(&ctx, gb, true, &result));
	FYAI_TCHECK(fy_get(result, "calls", -1LL) == 2);
	FYAI_TCHECK(fy_get(result, "input_tokens", -1LL) == 200);
	FYAI_TCHECK(fy_get(result, "cached_tokens", -1LL) == 40);
	FYAI_TCHECK(fy_get(result, "output_tokens", -1LL) == 50);
	FYAI_TCHECK(fy_get(result, "total_tokens", -1LL) == 250);
	fy_generic_builder_destroy(gb);
	return 0;
}

static void manual_input_record(struct fyai_cmd_call *call, const char *line)
{
	FYAI_TCHECK(!strcmp(line, "sensitive-callback"));
	(*(int *)call->priv)++;
}

int auth_manual_input(void)
{
	struct fyai_ctx ctx = { 0 };
	struct fyai_cmd_call call = { .input = manual_input_record };
	int inputs = 0;

	call.priv = &inputs;
	FYAI_TCHECK(!fyai_cmd_session_input(&ctx, "sensitive-callback"));
	ctx.cmd_call = &call;
	FYAI_TCHECK(fyai_cmd_session_input(&ctx, "sensitive-callback"));
	FYAI_TCHECK(inputs == 1);
	call.done = true;
	FYAI_TCHECK(!fyai_cmd_session_input(&ctx, "sensitive-callback"));
	FYAI_TCHECK(inputs == 1);
	return 0;
}
