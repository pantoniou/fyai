/*
 * fyai_auth_util.c - small OAuth encoding helpers
 *
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_AUTH

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/evp.h>
#include <openssl/core_names.h>
#include <openssl/param_build.h>

#include "fyai_auth_util.h"
#include "fyai.h"
#include "utils.h"

const char *fyai_auth_state_path(struct fyai_ctx *ctx, const char *name,
				 bool create)
{
	struct fyai_cfg *cfg;
	struct fy_generic_builder *gb;
	const char *base;
	const char *home;
	const char *dir;

	cfg = ctx->cfg;
	dir = cfg->auth_state_dir;
	if (!dir) {
		base = getenv("XDG_STATE_HOME");
		if (base && *base)
			dir = fy_gb_intern_string(cfg->gb,
				fy_sprintfa("%s/fyai", base));
		if (!dir) {
			home = getenv("HOME");
			if (home && *home)
				dir = fy_gb_intern_string(cfg->gb,
					fy_sprintfa("%s/.local/state/fyai",
						    home));
		}
		cfg->auth_state_dir = dir ? dir : "";
	}
	dir = cfg->auth_state_dir;
	if (!dir || !*dir ||
	    (create && (fyai_mkdir_p(dir) || mkdir_private(dir))))
		return "";
	gb = fyai_ctx_transient_gb(ctx);
	if (!gb)
		return "";
	return fy_gb_intern_string(gb, fy_sprintfa("%s/%s", dir, name));
}

int fyai_auth_store_lock(struct fyai_ctx *ctx, bool nonblock)
{
	const char *path;
	int operation;
	int saved_errno;
	int fd;

	path = fyai_auth_state_path(ctx, "auth.lock", true);
	if (!path || !*path)
		return -1;
	fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0)
		return -1;
	operation = LOCK_EX | (nonblock ? LOCK_NB : 0);
	if (!flock(fd, operation))
		return fd;
	saved_errno = errno;
	close(fd);
	if (nonblock &&
	    (saved_errno == EWOULDBLOCK || saved_errno == EAGAIN))
		return -2;
	errno = saved_errno;
	return -1;
}

void fyai_auth_store_unlock(int fd)
{
	if (fd < 0)
		return;
	flock(fd, LOCK_UN);
	close(fd);
}

char *fyai_auth_store_read(struct fyai_ctx *ctx, const char *name)
{
	const char *path;
	struct stat st;
	char *text;
	size_t total;
	ssize_t count;
	int fd;

	text = NULL;
	fd = -1;
	path = fyai_auth_state_path(ctx, name, false);
	if (!path || !*path)
		goto out;
	fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		goto out;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (st.st_mode & 077) ||
	    st.st_size < 0 || st.st_size > 1024 * 1024)
		goto out;
	text = malloc((size_t)st.st_size + 1);
	if (!text)
		goto out;
	total = 0;
	while (total < (size_t)st.st_size) {
		count = read(fd, text + total, (size_t)st.st_size - total);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			free(text);
			text = NULL;
			goto out;
		}
		total += (size_t)count;
	}
	text[total] = '\0';

out:
	if (fd >= 0)
		close(fd);
	return text;
}

int fyai_auth_store_write(struct fyai_ctx *ctx, const char *name,
			  const char *text)
{
	const char *path;
	const char *tmp;
	const char *dir;
	size_t total;
	size_t len;
	ssize_t count;
	int fd;
	int dfd;
	int rc;

	fd = -1;
	dfd = -1;
	rc = -1;
	path = fyai_auth_state_path(ctx, name, true);
	if (!path || !*path)
		goto out;
	tmp = fy_sprintfa("%s.tmp.%ld", path, (long)getpid());
	len = strlen(text);
	fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
		  0600);
	if (fd < 0)
		goto out;
	total = 0;
	while (total < len) {
		count = write(fd, text + total, len - total);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			goto unlink_out;
		total += (size_t)count;
	}
	if (fsync(fd) || close(fd))
		goto unlink_closed;
	fd = -1;
	if (rename(tmp, path))
		goto unlink_closed;
	dir = fy_sprintfa("%.*s", (int)(strrchr(path, '/') - path), path);
	dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dfd >= 0)
		rc = fsync(dfd);
	else
		rc = 0;
	goto out;

unlink_out:
	close(fd);
	fd = -1;
unlink_closed:
	unlink(tmp);
out:
	if (fd >= 0)
		close(fd);
	if (dfd >= 0)
		close(dfd);
	return rc;
}

int fyai_auth_store_delete(struct fyai_ctx *ctx, const char *name)
{
	const char *path;

	path = fyai_auth_state_path(ctx, name, false);
	if (!path || !*path)
		return -1;
	if (!unlink(path) || errno == ENOENT)
		return 0;
	fyai_error(ctx, "could not remove authentication state %s: %s",
		   path, strerror(errno));
	return -1;
}

char *fyai_base64url_encode(const unsigned char *data, size_t len)
{
	size_t cap = 4 * ((len + 2) / 3) + 1;
	char *out = malloc(cap);
	int n;

	if (!out)
		return NULL;
	n = EVP_EncodeBlock((unsigned char *)out, data, (int)len);
	if (n < 0) {
		free(out);
		return NULL;
	}
	while (n && out[n - 1] == '=')
		n--;
	for (int i = 0; i < n; i++) {
		if (out[i] == '+')
			out[i] = '-';
		else if (out[i] == '/')
			out[i] = '_';
	}
	out[n] = '\0';
	return out;
}

unsigned char *fyai_base64url_decode(const char *text, size_t *lenp)
{
	size_t len, pad, i;
	char *tmp;
	unsigned char *out;
	int n;

	if (!text || !lenp)
		return NULL;
	len = strlen(text);
	if (len > 1024 * 1024 || len % 4 == 1)
		return NULL;
	for (i = 0; i < len; i++) {
		if (!((text[i] >= 'A' && text[i] <= 'Z') ||
		      (text[i] >= 'a' && text[i] <= 'z') ||
		      (text[i] >= '0' && text[i] <= '9') ||
		      text[i] == '-' || text[i] == '_'))
			return NULL;
	}
	pad = (4 - len % 4) % 4;
	tmp = malloc(len + pad + 1);
	if (!tmp)
		return NULL;
	memcpy(tmp, text, len);
	for (i = 0; i < len; i++) {
		if (tmp[i] == '-')
			tmp[i] = '+';
		else if (tmp[i] == '_')
			tmp[i] = '/';
	}
	memset(tmp + len, '=', pad);
	tmp[len + pad] = '\0';
	out = malloc(3 * ((len + pad) / 4) + 1);
	if (!out) {
		free(tmp);
		return NULL;
	}
	n = EVP_DecodeBlock(out, (unsigned char *)tmp, (int)(len + pad));
	free(tmp);
	if (n < (int)pad) {
		free(out);
		return NULL;
	}
	n -= (int)pad;
	out[n] = '\0';
	*lenp = (size_t)n;
	return out;
}

bool fyai_auth_scope_has(const char *scopes, const char *scope)
{
	const char *end;
	size_t len;

	if (!scopes || !scope || !*scope)
		return false;
	len = strlen(scope);
	while (*scopes) {
		while (*scopes == ' ')
			scopes++;
		end = strchr(scopes, ' ');
		if (!end)
			end = scopes + strlen(scopes);
		if ((size_t)(end - scopes) == len && !memcmp(scopes, scope, len))
			return true;
		scopes = end;
	}
	return false;
}

static fy_generic auth_jwt_part(struct fy_generic_builder *gb,
				const char *start, size_t len)
{
	char *encoded;
	unsigned char *decoded;
	size_t size;
	fy_generic value;

	encoded = strndup(start, len);
	if (!encoded)
		return fy_invalid;
	decoded = fyai_base64url_decode(encoded, &size);
	free(encoded);
	if (!decoded)
		return fy_invalid;
	value = parse_json_string_size(gb, (const char *)decoded, size);
	free(decoded);
	return value;
}

static EVP_PKEY *auth_jwk_key(fy_generic jwk)
{
	unsigned char *bytes;
	size_t len;
	BIGNUM *n = NULL, *e = NULL;
	OSSL_PARAM_BLD *builder = NULL;
	OSSL_PARAM *params = NULL;
	EVP_PKEY_CTX *ctx = NULL;
	EVP_PKEY *key = NULL;

	bytes = fyai_base64url_decode(fy_get(jwk, "n", ""), &len);
	if (!bytes || len > 1024) {
		free(bytes);
		goto out;
	}
	n = BN_bin2bn(bytes, (int)len, NULL);
	free(bytes);
	bytes = fyai_base64url_decode(fy_get(jwk, "e", ""), &len);
	if (!bytes || len > 8) {
		free(bytes);
		goto out;
	}
	e = BN_bin2bn(bytes, (int)len, NULL);
	free(bytes);
	if (!n || !e || BN_num_bits(n) < 2048)
		goto out;
	builder = OSSL_PARAM_BLD_new();
	if (!builder ||
	    !OSSL_PARAM_BLD_push_BN(builder, OSSL_PKEY_PARAM_RSA_N, n) ||
	    !OSSL_PARAM_BLD_push_BN(builder, OSSL_PKEY_PARAM_RSA_E, e))
		goto out;
	params = OSSL_PARAM_BLD_to_param(builder);
	ctx = EVP_PKEY_CTX_new_from_name(NULL, "RSA", NULL);
	if (!params || !ctx || EVP_PKEY_fromdata_init(ctx) <= 0 ||
	    EVP_PKEY_fromdata(ctx, &key, EVP_PKEY_PUBLIC_KEY, params) <= 0) {
		EVP_PKEY_free(key);
		key = NULL;
	}
out:
	EVP_PKEY_CTX_free(ctx);
	OSSL_PARAM_free(params);
	OSSL_PARAM_BLD_free(builder);
	BN_free(n);
	BN_free(e);
	return key;
}

fy_generic fyai_auth_verify_id_token(struct fy_generic_builder *gb,
		const char *token, fy_generic jwks, const char *client_id,
		const char *nonce, const char *subject, time_t now)
{
	const char *first, *second;
	fy_generic header, claims, keys, jwk, aud, item;
	EVP_PKEY *key = NULL;
	EVP_MD_CTX *md = NULL;
	unsigned char *signature = NULL;
	size_t len;
	bool audience = false;
	int rc;

	if (fy_str_empty(token) || fy_str_empty(client_id) || strlen(token) > 65536)
		return fy_invalid;
	first = strchr(token, '.');
	second = first ? strchr(first + 1, '.') : NULL;
	if (!second || strchr(second + 1, '.'))
		return fy_invalid;
	header = auth_jwt_part(gb, token, (size_t)(first - token));
	claims = auth_jwt_part(gb, first + 1, (size_t)(second - first - 1));
	if (!fy_is_mapping(header) || !fy_is_mapping(claims) ||
	    fy_not_equal(fy_get(header, "alg", ""), "RS256") ||
	    fy_is_valid(fy_get(header, "crit", fy_invalid)) ||
	    fy_str_empty(fy_get(header, "kid", "")))
		return fy_invalid;
	keys = fy_get(jwks, "keys", fy_invalid);
	if (!fy_is_sequence(keys))
		return fy_invalid;
	fy_foreach(jwk, keys) {
		if (fy_not_equal(fy_get(jwk, "kid", ""), fy_get(header, "kid", "")) ||
		    fy_not_equal(fy_get(jwk, "kty", ""), "RSA") ||
		    fy_not_equal(fy_get(jwk, "use", "sig"), "sig") ||
		    fy_not_equal(fy_get(jwk, "alg", "RS256"), "RS256"))
			continue;
		key = auth_jwk_key(jwk);
		break;
	}
	if (!key)
		return fy_invalid;
	signature = fyai_base64url_decode(second + 1, &len);
	md = EVP_MD_CTX_new();
	if (!signature || !md)
		goto invalid;
	rc = EVP_DigestVerifyInit(md, NULL, EVP_sha256(), NULL, key);
	if (rc != 1)
		goto invalid;
	rc = EVP_DigestVerify(md, signature, len, (const unsigned char *)token,
			      (size_t)(second - token));
	if (rc != 1)
		goto invalid;
	aud = fy_get(claims, "aud", fy_invalid);
	if (fy_is_string(aud))
		audience = fy_equal(aud, client_id);
	else if (fy_is_sequence(aud)) {
		fy_foreach(item, aud) {
			if (fy_equal(item, client_id))
				audience = true;
		}
	}
	if (!audience ||
	    fy_not_equal(fy_get(claims, "iss", ""), FYAI_AUTH_ISSUER) ||
	    fy_str_empty(fy_get(claims, "sub", "")) ||
	    fy_get(claims, "exp", 0LL) <= (long long)now ||
	    fy_get(claims, "iat", 0LL) <= 0 ||
	    fy_get(claims, "iat", 0LL) > (long long)now + 5 ||
	    fy_get(claims, "nbf", 0LL) > (long long)now + 5 ||
	    (nonce && fy_not_equal(fy_get(claims, "nonce", ""), nonce)) ||
	    (!fy_str_empty(subject) && fy_not_equal(fy_get(claims, "sub", ""), subject)) ||
	    (fy_is_valid(fy_get(claims, "azp", fy_invalid)) &&
	     fy_not_equal(fy_get(claims, "azp", ""), client_id)))
		goto invalid;
	goto out;
invalid:
	claims = fy_invalid;
out:
	EVP_MD_CTX_free(md);
	EVP_PKEY_free(key);
	free(signature);
	return claims;
}

fy_generic fyai_auth_subscription_request(struct fy_generic_builder *gb,
					 fy_generic request)
{
	fy_generic key, value, item, type;
	fy_generic out = fy_map_empty;
	fy_generic input = fy_seq_empty;
	fy_generic tools = fy_seq_empty;
	fy_generic functions = fy_seq_empty;

	fy_foreach_key_value(key, value, request) {
		if (fy_any_equal(key, "background", "conversation", "max_output_tokens",
			"max_tool_calls", "metadata", "moderation", "multi_agent", "prompt",
			"prompt_cache_retention", "safety_identifier", "temperature",
			"top_logprobs", "top_p", "truncation", "user", "previous_response_id",
			"store", "stream", "input", "tools"))
			continue;
		out = fy_assoc(gb, out, key, value);
	}
	fy_foreach(item, fy_get(request, "input", fy_seq_empty)) {
		type = fy_get(item, "type", fy_invalid);
		if (fy_equal(fy_get(item, "role", ""), "system"))
			item = fy_assoc(gb, item, "role", "developer");
		if (fy_any_equal(type, "function_call", "custom_tool_call"))
			item = fy_assoc(gb, item, "namespace", "fyai");
		input = fy_append(gb, input, item);
	}
	fy_foreach(item, fy_get(request, "tools", fy_seq_empty)) {
		type = fy_get(item, "type", fy_invalid);
		if (fy_any_equal(type, "function", "custom"))
			functions = fy_append(gb, functions, item);
		else if (fy_equal(type, "web_search"))
			tools = fy_append(gb, tools, item);
		else
			return fy_invalid;
	}
	if (!fy_empty(functions))
		tools = fy_append(gb, tools, fy_mapping(gb,
			"type", "namespace", "name", "fyai",
			"description", "Local coding assistant tools", "tools", functions));
	out = fy_assoc(gb, out, "input", input, "store", false, "stream", true);
	if (!fy_empty(tools))
		out = fy_assoc(gb, out, "tools", tools);
	return out;
}
