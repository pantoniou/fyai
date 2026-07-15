/*
 * fyai_auth.c - machine-local ChatGPT subscription authentication
 *
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_AUTH

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <openssl/rand.h>
#include <openssl/sha.h>

#ifdef HAVE_LIBSECRET
#include <libsecret/secret.h>
#endif
#ifdef __APPLE__
#include <Security/Security.h>
#endif

#include "fyai.h"
#include "fyai_auth.h"
#include "fyai_auth_util.h"
#include "fyai_display.h"
#include "fyai_markdown.h"
#include "fyai_render.h"
#include "fyai_config.h"

#if defined(__APPLE__) || defined(HAVE_LIBSECRET)
#define HAVE_KEYRING
#else
#undef HAVE_KEYRING
#endif

static char *query_value(CURL *curl, const char *request, const char *key)
{
	const char *q, *end, *p;
	size_t klen;
	const char *v, *amp;
	char *decoded;
	char *result;
	int n;

	klen = strlen(key);
	q = strchr(request, '?');
	if (!q)
		return NULL;
	q++;
	end = strchr(q, ' ');
	if (!end)
		return NULL;

	/* find the key */
	v = NULL;
	for (p = q; p && p < end; p++) {
		if ((size_t)(end - p) > klen && !strncmp(p, key, klen) && p[klen] == '=') {
			v = p + klen + 1;
			break;
		}
		p = memchr(p, '&', (size_t)(end - p));
	}
	if (!v)
		return NULL;

	/* find the next & or end */
	amp = memchr(v, '&', (size_t)(end - v));
	if (amp)
		end = amp;

	/* decode */
	decoded = curl_easy_unescape(curl, v, (size_t)(end - v), &n);
	if (!decoded)
		return NULL;

	result = strndup(decoded, (size_t)n);
	curl_free(decoded);

	return result;
}

#define CHATGPT_BASE_URL "https://chatgpt.com/backend-api/codex"
#define CHATGPT_BACKEND_URL "https://chatgpt.com/backend-api"
#define AUTH_ISSUER "https://auth.openai.com"
/* Public native-client identifier used by the upstream Codex login flow.
 * It is not a secret. Keep this compatibility contract compiled in: allowing
 * arena/repository config to replace OAuth or backend endpoints would permit
 * a project to redirect machine-local subscription credentials. */
#define CODEX_OAUTH_CLIENT_ID "app_EMoamEEZ73f0CkXaXp7hrann"
#define CODEX_OAUTH_SCOPE_URL "openid%20profile%20email%20offline_access%20api.connectors.read%20api.connectors.invoke"
#define AUTH_PORT 1455
#define AUTH_FALLBACK_PORT 1457
#define AUTH_REFRESH_WINDOW 300

struct auth_http {
	char *data;
	size_t len;
	long status;
};

static struct fy_generic_builder *auth_builder(struct fyai_ctx *ctx)
{
	struct fy_generic_builder_cfg cfg = {
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED,
		.parent = ctx->cfg->gb,
	};

	if (!ctx->auth_gb)
		ctx->auth_gb = fy_generic_builder_create(&cfg);
	return ctx->auth_gb;
}

static int auth_save_file(struct fyai_ctx *ctx, struct fyai_credentials *c);
static int auth_delete_file(struct fyai_ctx *ctx);

static void credentials_clear(struct fyai_credentials *c)
{
	if (!c)
		return;
	memset(c, 0, sizeof(*c));
}

const char *fyai_auth_mode_string(enum fyai_auth_mode mode)
{
	switch (mode) {
	case FYAI_AUTH_AUTO: return "auto";
	case FYAI_AUTH_API_KEY: return "api-key";
	case FYAI_AUTH_CHATGPT: return "chatgpt";
	}
	return "unknown";
}

static const char *auth_state_dir(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg = ctx->cfg;
	const char *p;
	const char *home;
	const char *dir = NULL;

	if (cfg->auth_state_dir)
		return cfg->auth_state_dir;

	dir = NULL;

	p = getenv("XDG_STATE_HOME");
	if (p && *p)
		dir = fy_gb_intern_string(auth_builder(ctx), fy_sprintfa("%s/fyai", p));

	if (!dir) {
		home = getenv("HOME");
		if (home && *home)
			dir = fy_gb_intern_string(auth_builder(ctx),
				fy_sprintfa("%s/.local/state/fyai", home));
	}
	if (!dir)
		dir = "";

	cfg->auth_state_dir = dir;

	return dir;
}

static const char *auth_path(struct fyai_ctx *ctx, const char *name, bool create)
{
	const char *dir;

	dir = auth_state_dir(ctx);
	if (!dir || !*dir)
		return "";
	if (create && mkdir_private(dir))
		return "";
	return fy_gb_intern_string(ctx->transient_gb,
			fy_sprintfa("%s/%s", dir, name));
}

static int auth_lock(struct fyai_ctx *ctx)
{
	const char *path;
	int fd = -1;

	path = auth_path(ctx, "auth.lock", true);
	if (!*path)
		goto err_out;

	fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0)
		goto err_out;

	if (flock(fd, LOCK_EX))
		goto err_out;

	return fd;

err_out:
	if (fd >= 0)
		close(fd);
	return -1;
}

static void auth_unlock(struct fyai_ctx *ctx, int fd)
{
	(void)ctx;

	if (fd < 0)
		return;

	flock(fd, LOCK_UN);
	close(fd);
}

static int token_claims(struct fyai_ctx *ctx, struct fyai_credentials *c)
{
	const char *p, *end;
	char *part;
	char *json;
	size_t len;
	fy_generic doc, auth;
	long long exp;

	if (!c->id_token)
		return -1;

	p = strchr(c->id_token, '.');
	if (!p || !(end = strchr(++p, '.')))
		return -1;

	part = strndup(p, (size_t)(end - p));
	if (!part)
		return -1;
	json = (char *)fyai_base64url_decode(part, &len);
	free(part);
	part = NULL;
	if (!json)
		return -1;

	doc = parse_json_string(ctx->transient_gb, json);
	free(json);
	json = NULL;

	if (fy_generic_is_invalid(doc))
		goto err;

	c->email = fy_gb_intern_string(auth_builder(ctx), fy_get(doc, "email", ""));

	auth = fy_get(doc, "https://api.openai.com/auth");
	if (!fy_generic_is_mapping(auth))
		auth = doc;

	c->account_id = fy_gb_intern_string(auth_builder(ctx), fy_get(auth, "chatgpt_account_id", ""));
	c->plan = fy_gb_intern_string(auth_builder(ctx), fy_get(auth, "chatgpt_plan_type", "unknown"));
	c->fedramp = fy_get(auth, "chatgpt_account_is_fedramp", false);

	exp = fy_get(doc, "exp", 0LL);
	if (exp > 0)
		c->expires_at = (time_t)exp;

	if (!*c->account_id)
		goto err;

	return 0;
err:
	return -1;
}

static int auth_parse_content(struct fyai_ctx *ctx, struct fyai_credentials *c,
			      const char *text, size_t len,
			      const char *storage)
{
	fy_generic doc;
	int rc;

	credentials_clear(c);

	doc = parse_json_string_size(ctx->transient_gb, text, len);
	if (fy_generic_is_invalid(doc))
		goto err_out;

	c->access_token = fy_gb_intern_string(auth_builder(ctx), fy_get(doc, "access_token", ""));
	c->refresh_token = fy_gb_intern_string(auth_builder(ctx), fy_get(doc, "refresh_token", ""));
	c->id_token = fy_gb_intern_string(auth_builder(ctx), fy_get(doc, "id_token", ""));
	c->expires_at = (time_t)fy_get(doc, "expires_at", 0LL);
	c->storage = fy_gb_intern_string(auth_builder(ctx), storage);

	/*
	 * Detail behind a failed load: the caller falls back and the use site
	 * reports the actionable "not logged in", so this only has to explain
	 * why to whoever is debugging it.
	 */
	if (!*c->access_token || !*c->refresh_token || !*c->id_token) {
		fyai_debug(ctx, "%s credentials incomplete", storage);
		goto err_out;
	}

	rc = token_claims(ctx, c);
	if (rc) {
		fyai_debug(ctx, "%s credentials carry no usable claims", storage);
		goto err_out;
	}
	rc = 0;

	return 0;

err_out:
	return -1;
}

static int auth_load_file(struct fyai_ctx *ctx, struct fyai_credentials *c)
{
	const char *path;
	char *text = NULL;
	struct stat st;
	int fd = -1, rc = -1;
	ssize_t n;
	size_t size, total;

	/*
	 * Not being logged in is the ordinary case - the caller falls back to
	 * an API key - so an absent store is only debug. A store that exists
	 * but cannot be used is a different matter: it leaves the user looking
	 * logged in while every request is anonymous, so say so.
	 */
	path = auth_path(ctx, "auth.json", false);
	if (!*path) {
		fyai_debug(ctx, "no credential store path");
		goto err_out;
	}

	fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0) {
		if (errno == ENOENT)
			fyai_debug(ctx, "no stored credentials at %s", path);
		else
			fyai_warning(ctx, "ignoring %s: %s", path,
				     strerror(errno));
		goto err_out;
	}

	if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
		fyai_warning(ctx, "ignoring %s: not a regular file", path);
		goto err_out;
	}

	if (st.st_mode & 077) {
		fyai_warning(ctx, "ignoring %s: group or world accessible; "
			     "run chmod 600 on it", path);
		goto err_out;
	}

	if (st.st_size < 0 || st.st_size > 1024 * 1024) {
		fyai_warning(ctx, "ignoring %s: implausible size", path);
		goto err_out;
	}
	size = (size_t)st.st_size;

	/* read the whole file */
	text = malloc(size + 1);
	if (!text)
		goto err_out;

	for (total = 0; total < size; total += n) {
		do {
			n = read(fd, text + total, size - total);
		} while (n == -1 && (errno == EINTR || errno == EAGAIN));
		if (n <= 0) {
			fyai_warning(ctx, "ignoring %s: short read", path);
			goto err_out;
		}
	}
	close(fd);
	fd = -1;
	text[size] = '\0';

	rc = auth_parse_content(ctx, c, text, size, "file");
	free(text);
	text = NULL;

out:
	if (rc)
		credentials_clear(c);

	free(text);
	text = NULL;

	if (fd >= 0)
		close(fd);

	return rc;

err_out:
	rc = -1;
	goto out;
}

#ifdef HAVE_KEYRING
static const char *auth_json(struct fyai_ctx *ctx, struct fyai_credentials *c)
{
	fy_generic doc;

	doc = fy_mapping(
		"type", "chatgpt",
		"access_token", c->access_token,
		"refresh_token", c->refresh_token,
		"id_token", c->id_token,
		"expires_at", c->expires_at);
	return emit_json_string(ctx->transient_gb, doc);
}
#endif

#if defined(__APPLE__)
/*
 * The legacy SecKeychain* file-based API is deprecated since macOS 10.10.
 * Use the modern keychain-item API (SecItem*) with a generic-password class
 * keyed by service/account, matching the semantics of the previous code.
 */
static CFDictionaryRef auth_keychain_query(void)
{
	const char service[] = "org.fyai.Auth";
	const char account[] = "default";
	const void *keys[3];
	const void *vals[3];
	CFStringRef svc, acct;
	CFDictionaryRef query;

	svc = CFStringCreateWithCString(NULL, service, kCFStringEncodingUTF8);
	acct = CFStringCreateWithCString(NULL, account, kCFStringEncodingUTF8);
	if (!svc || !acct) {
		if (svc)
			CFRelease(svc);
		if (acct)
			CFRelease(acct);
		return NULL;
	}

	keys[0] = kSecClass;
	vals[0] = kSecClassGenericPassword;
	keys[1] = kSecAttrService;
	vals[1] = svc;
	keys[2] = kSecAttrAccount;
	vals[2] = acct;

	query = CFDictionaryCreate(NULL, keys, vals, 3,
		&kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFRelease(svc);
	CFRelease(acct);

	return query;
}

static int auth_load_keyring(struct fyai_ctx *ctx, struct fyai_credentials *c)
{
	CFMutableDictionaryRef query;
	CFDictionaryRef base;
	CFTypeRef result;
	OSStatus status;
	int rc;

	base = auth_keychain_query();
	if (!base)
		return -1;

	query = CFDictionaryCreateMutableCopy(NULL, 0, base);
	CFRelease(base);
	if (!query)
		return -1;
	CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
	CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);

	result = NULL;
	status = SecItemCopyMatching(query, &result);
	CFRelease(query);
	if (status != errSecSuccess || !result)
		return -1;

	rc = auth_parse_content(ctx, c,
		(const char *)CFDataGetBytePtr((CFDataRef)result),
		(size_t)CFDataGetLength((CFDataRef)result), "keychain");
	CFRelease(result);

	return rc;
}

static int auth_save_keyring(struct fyai_ctx *ctx, struct fyai_credentials *c)
{
	CFDictionaryRef query, update, add;
	CFDataRef value;
	const char *json;
	const void *ukey, *uval;
	OSStatus status;

	json = auth_json(ctx, c);
	if (!json || !*json)
		return -1;

	query = auth_keychain_query();
	if (!query)
		return -1;

	value = CFDataCreate(NULL, (const UInt8 *)json, (CFIndex)strlen(json));
	if (!value) {
		CFRelease(query);
		return -1;
	}

	ukey = kSecValueData;
	uval = value;
	update = CFDictionaryCreate(NULL, &ukey, &uval, 1,
		&kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	status = SecItemUpdate(query, update);
	if (update)
		CFRelease(update);

	if (status == errSecItemNotFound) {
		add = CFDictionaryCreateMutableCopy(NULL, 0, query);
		if (add) {
			CFDictionarySetValue((CFMutableDictionaryRef)add,
				kSecValueData, value);
			status = SecItemAdd(add, NULL);
			CFRelease(add);
		} else {
			status = errSecAllocate;
		}
	}

	CFRelease(value);
	CFRelease(query);

	return status == errSecSuccess ? 0 : -1;
}

static int auth_delete_keyring(struct fyai_ctx *ctx)
{
	CFDictionaryRef query;
	OSStatus status;

	(void)ctx;

	query = auth_keychain_query();
	if (!query)
		return -1;

	status = SecItemDelete(query);
	CFRelease(query);

	if (status == errSecItemNotFound)
		return 0;
	return status == errSecSuccess ? 0 : -1;
}
#elif defined(HAVE_LIBSECRET)
static const SecretSchema fyai_secret_schema = {
	"org.fyai.Auth", SECRET_SCHEMA_NONE,
	{
		{ "account", SECRET_SCHEMA_ATTRIBUTE_STRING },
		{ NULL, 0 },
	}
};

static int auth_load_keyring(struct fyai_ctx *ctx, struct fyai_credentials *c)
{
	char *secret;
	size_t len;
	int rc;

	secret = secret_password_lookup_sync(&fyai_secret_schema, NULL, NULL,
					     "account", "default", NULL);
	if (!secret)
		return -1;

	len = strlen(secret);
	rc = auth_parse_content(ctx, c, secret, (size_t)len, "keychain");
	secret_password_free(secret);

	return rc;
}

static int auth_save_keyring(struct fyai_ctx *ctx, struct fyai_credentials *c)
{
	const char *json;
	gboolean ok;

	json = auth_json(ctx, c);
	if (!json || !*json)
		return -1;

	ok = secret_password_store_sync(&fyai_secret_schema,
		SECRET_COLLECTION_DEFAULT, "fyai ChatGPT authentication", json,
		NULL, NULL, "account", "default", NULL);

	return ok ? 0 : -1;
}

static int auth_delete_keyring(struct fyai_ctx *ctx)
{
	GError *error = NULL;

	(void)ctx;

	(void)secret_password_clear_sync(&fyai_secret_schema, NULL, &error,
					 "account", "default", NULL);
	if (error) {
		g_error_free(error);
		return -1;
	}
	return 0;
}
#else
static int auth_load_keyring(struct fyai_ctx *ctx, struct fyai_credentials *c)
{
	(void)ctx;
	(void)c;
	return -1;
}

static int auth_save_keyring(struct fyai_ctx *ctx, struct fyai_credentials *c)
{
	(void)ctx;
	(void)c;
	return -1;
}

static int auth_delete_keyring(struct fyai_ctx *ctx)
{
	(void)ctx;
	return 0;
}
#endif

static int auth_load(struct fyai_ctx *ctx, struct fyai_credentials *c)
{
	int rc;

	rc = auth_load_keyring(ctx, c);
	if (!rc)
		return 0;

	rc = auth_load_file(ctx, c);
	if (!rc)
		return 0;

	return -1;
}

static int auth_save(struct fyai_ctx *ctx, struct fyai_credentials *c)
{
	int rc;

	rc = auth_save_keyring(ctx, c);
	if (!rc) {
		auth_delete_file(ctx);
		c->storage = "keyring";
		return 0;
	}

	rc = auth_save_file(ctx, c);
	if (!rc) {
		/* clear keyring? */
		c->storage = "file";
		return 0;
	}

	return -1;
}

static int auth_save_file(struct fyai_ctx *ctx, struct fyai_credentials *c)
{
	fy_generic doc;
	const char *json;
	const char *tmp;
	const char *path, *dir;
	int fd = -1, rc = -1;
	int dfd = -1;
	ssize_t wrn;
	size_t total, len;

	path = auth_path(ctx, "auth.json", true);
	if (!path)
		goto err_out;

	tmp = fy_sprintfa("%s.tmp.%ld", path, (long)getpid());
	doc = fy_mapping(
		"type", "chatgpt",
		"access_token", c->access_token,
		"refresh_token", c->refresh_token,
		"id_token", c->id_token,
		"expires_at", (long long)c->expires_at);
	json = emit_json_string(ctx->transient_gb, doc);
	if (!json)
		goto err_out;
	len = strlen(json);

	fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0)
		goto err_out;

	for (total = 0; total < len;) {
		do {
			wrn = write(fd, json + total, len - total);
		} while (wrn == -1 && (errno == EINTR || errno == EAGAIN));
		if (wrn <= 0)
			goto err_out;
		total += (size_t)wrn;
	}

	do {
		wrn = write(fd, "\n", 1);
	} while (wrn == -1 && (errno == EINTR || errno == EAGAIN));
	if (wrn != 1)
		goto err_out;

	rc = fsync(fd);
	if (rc)
		goto err_out;

	rc = close(fd);
	fd = -1;
	if (rc)
		goto err_out;

	rc = rename(tmp, path);
	if (rc)
		goto err_out;

	dir = auth_state_dir(ctx);
	if (dir && *dir)
		dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dfd < 0)
		goto err_out;

	rc = fsync(dfd);
	if (rc)
		goto err_out;
	close(dfd);
	dfd = -1;

	rc = 0;
out:
	if (dfd >= 0)
		close(dfd);

	if (fd >= 0)
		close(fd);
	if (rc && tmp)
		unlink(tmp);
	return rc;

err_out:
	rc = -1;
	goto out;
}

static int auth_delete_file(struct fyai_ctx *ctx)
{
	const char *path;
	int rc;

	path = auth_path(ctx, "auth.json", false);
	if (!*path)
		return -1;

	rc = unlink(path);
	if (rc && errno == ENOENT)
		rc = 0;

	return rc;
}

static size_t auth_write(void *ptr, size_t size, size_t nmemb, void *arg)
{
	struct auth_http *r = arg;
	size_t n = size * nmemb;
	char *p = realloc(r->data, r->len + n + 1);

	if (!p)
		return 0;
	r->data = p;
	memcpy(r->data + r->len, ptr, n);
	r->len += n;
	r->data[r->len] = '\0';
	return n;
}

static int auth_http_request(struct fyai_ctx *ctx,
			     const char *url, const char *body,
			     struct curl_slist *headers, struct auth_http *out)
{
	CURL *curl;
	CURLcode code;

	(void)ctx;

	memset(out, 0, sizeof(*out));
	curl = curl_easy_init();
	if (!curl)
		return -1;
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, auth_write);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "fyai/" VERSION);
	if (body)
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
	if (headers)
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	code = curl_easy_perform(curl);
	if (code == CURLE_OK)
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out->status);
	curl_easy_cleanup(curl);
	return code == CURLE_OK ? 0 : -1;
}

static int parse_tokens(struct fyai_ctx *ctx, struct fyai_credentials *c, const char *json)
{
	fy_generic doc;
	long long expires;
	int rc = -1;

	doc = parse_json_string(ctx->transient_gb, json ? json : "");

	credentials_clear(c);

	c->access_token = fy_gb_intern_string(auth_builder(ctx), fy_get(doc, "access_token", ""));
	c->refresh_token = fy_gb_intern_string(auth_builder(ctx), fy_get(doc, "refresh_token", ""));
	c->id_token = fy_gb_intern_string(auth_builder(ctx), fy_get(doc, "id_token", ""));
	expires = fy_get(doc, "expires_in", 0LL);
	c->expires_at = expires > 0 ? time(NULL) + (time_t)expires : 0;
	c->storage = fy_gb_intern_string(auth_builder(ctx), "file");
	if (!*c->access_token || !*c->refresh_token || !*c->id_token)
		goto err_out;

	rc = token_claims(ctx, c);
	if (rc)
		goto err_out;

	rc = 0;
out:
	if (rc)
		credentials_clear(c);
	return rc;

err_out:
	rc = -1;
	goto out;
}

static int exchange_code(struct fyai_ctx *ctx,
			 struct fyai_credentials *c, const char *code,
			 const char *redirect, const char *verifier)
{
	struct auth_http r;
	struct curl_slist *h = NULL;
	char *ec = NULL, *er = NULL, *ev = NULL;
	const char *body;
	int rc = -1;

	memset(&r, 0, sizeof(r));

	ec = curl_easy_escape(ctx->curl, code, 0);
	er = curl_easy_escape(ctx->curl, redirect, 0);
	ev = curl_easy_escape(ctx->curl, verifier, 0);
	if (!ec || !er || !ev)
		goto err_out;

	body = fy_sprintfa("grant_type=authorization_code&code=%s&redirect_uri=%s&client_id=%s&code_verifier=%s",
			ec, er, CODEX_OAUTH_CLIENT_ID, ev);

	h = curl_slist_append(h, "Content-Type: application/x-www-form-urlencoded");
	if (!h)
		goto err_out;

	rc = auth_http_request(ctx, AUTH_ISSUER "/oauth/token", body, h, &r);
	fyai_error_check(ctx, !rc, err_out, "token exchange request failed");

	fyai_error_check(ctx, r.status >= 200 && r.status < 300, err_out,
			 "token exchange failed (HTTP %ld)", r.status);

	rc = parse_tokens(ctx, c, r.data);
	if (rc)
		goto err_out;

out:
	free(r.data);
	curl_slist_free_all(h);
	curl_free(ec);
	curl_free(er);
	curl_free(ev);
	return rc;

err_out:
	rc = -1;
	goto out;
}

static int bind_callback(unsigned short *portp)
{
	const unsigned short ports[] = { AUTH_PORT, AUTH_FALLBACK_PORT };
	struct sockaddr_in sa;
	int fd = -1;
	size_t i;
	int rc;

	for (i = 0; i < ARRAY_SIZE(ports); i++) {

		if (fd >= 0)
			close(fd);
		fd = socket(AF_INET, SOCK_STREAM, 0);
		if (fd < 0)
			break;

		fcntl(fd, F_SETFD, FD_CLOEXEC);

		memset(&sa, 0, sizeof(sa));
		sa.sin_family = AF_INET;
		sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		sa.sin_port = htons(ports[i]);
		rc = bind(fd, (struct sockaddr *)&sa, sizeof(sa));
		if (rc)
			continue;

		rc = listen(fd, 4);
		if (rc)
			continue;

		/* we're good */
		*portp = ports[i];
		return fd;
	}

	/* can't work */

	if (fd >= 0)
		close(fd);
	return -1;
}

static void open_browser(const char *url)
{
	extern char **environ;
	pid_t pid;
#ifdef __APPLE__
	char *const argv[] = { "open", (char *)url, NULL };
	const char *program = "open";
#else
	char *const argv[] = { "xdg-open", (char *)url, NULL };
	const char *program = "xdg-open";
#endif

	if (!posix_spawnp(&pid, program, NULL, NULL, argv, environ))
		(void)pid;
}

/* Read a pasted callback URL (or bare code) from stdin, verify the CSRF
 * state, and exchange the code.  Used when no localhost socket is reachable
 * from the browser (e.g. signing in from another machine over SSH). */
static int manual_login(struct fyai_ctx *ctx, struct fyai_credentials *c,
			const char *redirect, const char *verifier,
			const char *state)
{
	char line[8192];
	char *code = NULL, *got_state = NULL, *request = NULL;
	char *nl;
	int rc = -1;

	printf("\nAfter authorizing, your browser is redirected to a URL that\n"
	       "cannot load (it points at this machine's localhost). Copy that\n"
	       "URL from the address bar and paste it here.\n\n"
	       "Paste redirect URL (or code): ");
	fflush(stdout);

	if (!fgets(line, sizeof(line), stdin))
		return -1;
	line[sizeof(line) - 1] = '\0';
	nl = strpbrk(line, "\r\n");
	if (nl)
		*nl = '\0';
	if (!*line)
		goto err_out;

	/* query_value scans a "GET <target> ..." request line for ?key=val. */
	request = fy_sprintfa("GET %s \r\n", line);

	code = query_value(ctx->curl, request, "code");
	got_state = query_value(ctx->curl, request, "state");
	if (!code) {
		/* No query string: treat the whole paste as the bare code. */
		code = strdup(line);
		if (!code)
			goto err_out;
	} else if (!got_state || strcmp(got_state, state)) {
		fyai_error(ctx, "state mismatch in pasted URL");
		goto err_out;
	}

	rc = exchange_code(ctx, c, code, redirect, verifier);
	if (rc)
		goto err_out;

	rc = 0;
out:
	free(code);
	free(got_state);
	return rc;
err_out:
	rc = -1;
	goto out;
}

static int browser_login(struct fyai_ctx *ctx, struct fyai_credentials *c,
			 bool no_browser, bool manual)
{
	unsigned char verifier_random[48], state_random[32];
	unsigned char digest[SHA256_DIGEST_LENGTH];
	char *verifier = NULL, *challenge = NULL, *state = NULL;
	char *code = NULL, *got_state = NULL;
	char request[8192];
	const char *redirect, *url;
	unsigned short port;
	struct pollfd pfd;
	int server = -1, client = -1, rc = -1;
	ssize_t n;
	static const char ok[] = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nLogin complete. You may close this window.\n";
	static const char bad[] = "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nLogin failed.\n";

	if (RAND_bytes(verifier_random, sizeof(verifier_random)) != 1 ||
	    RAND_bytes(state_random, sizeof(state_random)) != 1)
		return -1;

	verifier = fyai_base64url_encode(verifier_random, sizeof(verifier_random));
	state = fyai_base64url_encode(state_random, sizeof(state_random));
	if (!verifier || !state)
		goto err_out;

	SHA256((unsigned char *)verifier, strlen(verifier), digest);
	challenge = fyai_base64url_encode(digest, sizeof(digest));

	if (!challenge)
		goto err_out;

	if (manual) {
		port = AUTH_PORT;
	} else {
		server = bind_callback(&port);
		if (server < 0)
			goto err_out;
	}

	redirect = fy_sprintfa("http://localhost:%u/auth/callback", port);
	url = fy_sprintfa(
		AUTH_ISSUER "/oauth/authorize?response_type=code&client_id=" CODEX_OAUTH_CLIENT_ID
		"&redirect_uri=http%%3A%%2F%%2Flocalhost%%3A%u%%2Fauth%%2Fcallback"
		"&scope=%s"
		"&code_challenge=%s&code_challenge_method=S256&id_token_add_organizations=true"
		"&codex_cli_simplified_flow=true&state=%s&originator=fyai",
		port, CODEX_OAUTH_SCOPE_URL, challenge, state);

	printf("Open this URL to sign in:\n%s\n", url);
	fflush(stdout);

	if (!no_browser && !manual)
		open_browser(url);

	if (manual) {
		rc = manual_login(ctx, c, redirect, verifier, state);
		goto out;
	}

	pfd.fd = server;
	pfd.events = POLLIN;
	if (poll(&pfd, 1, 600000) <= 0)
		goto err_out;

	client = accept(server, NULL, NULL);
	if (client < 0)
		goto err_out;

	(void)fcntl(client, F_SETFD, FD_CLOEXEC);

	n = read(client, request, sizeof(request) - 1);
	if (n <= 0)
		goto err_out;
	request[n] = '\0';

	if (strncmp(request, "GET /auth/callback?", 19))
		goto respond;

	code = query_value(ctx->curl, request, "code");
	got_state = query_value(ctx->curl, request, "state");
	if (!code || !got_state || strcmp(got_state, state))
		goto respond;

	rc = exchange_code(ctx, c, code, redirect, verifier);
	if (rc)
		goto respond;

	(void)write(client, ok, sizeof(ok) - 1);
	rc = 0;
	goto out;

respond:
	(void)write(client, bad, sizeof(bad) - 1);
out:
	if (client >= 0)
		close(client);
	if (server >= 0)
		close(server);
	free(verifier);
	free(challenge);
	free(state);
	free(code);
	free(got_state);
	return rc;

err_out:
	rc = -1;
	goto out;
}

static int device_login(struct fyai_ctx *ctx, struct fyai_credentials *c)
{
	struct auth_http r = {};
	fy_generic doc;
	const char *device_id, *user_code, *body;
	int interval = 5, rc = -1, elapsed;
	const char *code, *verifier;
	struct curl_slist *json_headers = NULL;
	const char *request_json;
	bool req_ok;

	request_json = emit_json_string(ctx->transient_gb,
			fy_mapping(
				"client_id", CODEX_OAUTH_CLIENT_ID));

	json_headers = curl_slist_append(json_headers, "Content-Type: application/json");
	if (!json_headers)
		goto out;

	rc = auth_http_request(ctx,
		AUTH_ISSUER "/api/accounts/deviceauth/usercode",
		request_json, json_headers, &r);
	fyai_error_check(ctx, !rc, err_out, "device code request failed");
	fyai_error_check(ctx, r.status / 100 == 2, err_out,
			 "device code request failed (HTTP %ld)", r.status);

	doc = parse_json_string(ctx->transient_gb, r.data);

	free(r.data);
	memset(&r, 0, sizeof(r));

	device_id = fy_get(doc, "device_auth_id", "");
	user_code = fy_get(doc, "user_code", "");
	interval = atoi(fy_get(doc, "interval", "5"));
	if (interval < 1)
		interval = 5;
	fyai_error_check(ctx, *device_id && *user_code, err_out,
			 "device code response is missing the code");

	printf("Open https://auth.openai.com/codex/device and enter code %s\n", user_code);
	fflush(stdout);

	body = emit_json_string(ctx->transient_gb,
			fy_mapping(
				"device_auth_id", device_id,
				"user_code", user_code));

	req_ok = false;
	for (elapsed = 0; !req_ok && elapsed < 900; elapsed += interval) {

		sleep((unsigned int)interval);

		rc = auth_http_request(ctx,
				      AUTH_ISSUER "/api/accounts/deviceauth/token",
				      body, json_headers, &r);
		fyai_error_check(ctx, !rc, err_out, "device token request failed");
		if (r.status == 403 || r.status == 404) {
			/* retry */
			free(r.data);
			memset(&r, 0, sizeof(r));
			continue;
		}

		fyai_error_check(ctx, r.status / 100 == 2, err_out,
				 "device token request failed (HTTP %ld)",
				 r.status);

		req_ok = true;
	}

	fyai_error_check(ctx, req_ok, err_out,
			 "timed out waiting for the code to be entered");

	doc = parse_json_string(ctx->transient_gb, r.data);
	free(r.data);
	memset(&r, 0, sizeof(r));

	code = fy_get(doc, "authorization_code", "");
	verifier = fy_get(doc, "code_verifier", "");

	fyai_error_check(ctx, *code && *verifier, err_out,
			 "device token response is missing the code");

	rc = exchange_code(ctx, c, code, AUTH_ISSUER "/deviceauth/callback", verifier);
	fyai_error_check(ctx, !rc, err_out, "could not exchange the device code");

	rc = 0;
out:
	free(r.data);
	curl_slist_free_all(json_headers);

	return rc;

err_out:
	rc = -1;
	goto out;
}

static int refresh_locked(struct fyai_ctx *ctx, struct fyai_credentials *c)
{
	CURL *curl = ctx->curl;
	struct auth_http r;
	struct curl_slist *h = NULL;
	char *ert = NULL;
	const char *body;
	int rc = -1;

	memset(&r, 0, sizeof(r));

	if (!curl || !c->refresh_token)
		goto out;

	ert = curl_easy_escape(curl, c->refresh_token, 0);
	if (!ert)
		goto out;

	body = fy_sprintfa("grant_type=refresh_token&refresh_token=%s&client_id=%s",
		ert, CODEX_OAUTH_CLIENT_ID);

	h = curl_slist_append(h, "Content-Type: application/x-www-form-urlencoded");
	if (!h)
		goto out;

	rc = auth_http_request(ctx, AUTH_ISSUER "/oauth/token", body, h, &r);
	if (rc)
		goto out;

	if (r.status / 100 != 2) {
		fyai_error(ctx, "refresh failed (HTTP %ld); run `fyai auth login`",
			   r.status);
		goto out;
	}

	rc = parse_tokens(ctx, c, r.data);
	fyai_error_check(ctx, !rc, out, "failed to parse tokens");

	rc = 0;
out:
	free(r.data);
	curl_free(ert);
	curl_slist_free_all(h);
	return rc;
}

static void revoke_token(struct fyai_ctx *ctx, struct fyai_credentials *c)
{
	struct curl_slist *headers = NULL;
	struct auth_http r;
	const char *body;
	int rc;

	memset(&r, 0, sizeof(r));

	if (!*c->refresh_token)
		return;

	body = emit_json_string(ctx->transient_gb,
			fy_mapping(
				"token", c->refresh_token,
				"token_type_hint", "refresh_token",
				"client_id", CODEX_OAUTH_CLIENT_ID));

	headers = curl_slist_append(headers, "Content-Type: application/json");
	rc = auth_http_request(ctx, AUTH_ISSUER "/oauth/revoke", body, headers, &r);
	free(r.data);

	/* Revocation is deliberately best-effort; local logout must still win. */
	curl_slist_free_all(headers);

	if (rc)
		fyai_error(ctx, "revoke request failed");
}

int fyai_auth_refresh(struct fyai_ctx *ctx, bool force)
{
	int lockfd = -1, rc = -1;

	if (!ctx->cfg->chatgpt_auth)
		return 0;

	lockfd = auth_lock(ctx);
	if (lockfd < 0)
		goto out;

	/* Another stateless process may already have rotated the token. */
	if (!auth_load(ctx, &ctx->auth) && !force && ctx->auth.expires_at > time(NULL) + AUTH_REFRESH_WINDOW) {
		rc = 0;
		goto out;
	}
	if (!ctx->auth.refresh_token || !*ctx->auth.refresh_token)
		goto out;

	if (!refresh_locked(ctx, &ctx->auth) && !auth_save(ctx, &ctx->auth))
		rc = 0;
out:
	auth_unlock(ctx, lockfd);

	return rc;
}

int fyai_auth_apply_headers(struct fyai_ctx *ctx, struct curl_slist **headers)
{
	char *bearer = NULL;
	char *account = NULL;
	int rc = -1;

	if (!ctx->cfg->chatgpt_auth || !ctx->auth.access_token ||
	    !ctx->auth.account_id)
		return -1;
	if (asprintf(&bearer, "Authorization: Bearer %s",
		     ctx->auth.access_token) < 0 ||
	    asprintf(&account, "ChatGPT-Account-ID: %s",
		     ctx->auth.account_id) < 0)
		goto out;
	if (append_header(headers, bearer) || append_header(headers, account) ||
	    append_header(headers, "originator: fyai"))
		goto out;
	if (ctx->auth.fedramp &&
	    append_header(headers, "X-OpenAI-Fedramp: true"))
		goto out;
	rc = 0;
out:
	free(bearer);
	free(account);
	return rc;
}

bool fyai_auth_should_retry(struct fyai_ctx *ctx, long status)
{
	return status == 401 && ctx->cfg->chatgpt_auth &&
	       !ctx->auth_retry_done;
}

int fyai_auth_prepare_retry(struct fyai_ctx *ctx)
{
	ctx->auth_retry_done = true;
	if (fyai_auth_refresh(ctx, true))
		return -1;
	return fyai_request_state_apply(ctx);
}

int fyai_auth_resolve(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg = ctx->cfg;
	bool want_chatgpt;

	want_chatgpt = cfg->auth_mode == FYAI_AUTH_CHATGPT ||
		(cfg->auth_mode == FYAI_AUTH_AUTO && (!cfg->api_key || !*cfg->api_key));

	if (!want_chatgpt)
		return 0;

	if (!cfg->provider || fy_not_equal(cfg->provider, "openai")) {
		if (cfg->auth_mode == FYAI_AUTH_CHATGPT)
			fyai_error(ctx, "ChatGPT subscriptions support only the OpenAI provider");
		return cfg->auth_mode == FYAI_AUTH_CHATGPT ? -1 : 0;
	}

	if (cfg->api_mode != FYAI_API_RESPONSES || cfg->response_chain) {
		fyai_error(ctx, "ChatGPT requires Responses API with "
			   "response_chain disabled");
		return -1;
	}

	if (cfg->api_url && fy_not_equal(cfg->api_url, OPENAI_RESPONSES_URL)) {
		fyai_error(ctx, "refusing to send ChatGPT credentials to custom api_url");
		return -1;
	}

	if (auth_load(ctx, &ctx->auth)) {
		if (cfg->auth_mode == FYAI_AUTH_CHATGPT)
			fyai_error(ctx, "not logged in; run `fyai auth login`");
		return cfg->auth_mode == FYAI_AUTH_CHATGPT ? -1 : 0;
	}
	cfg->chatgpt_auth = true;
	if (ctx->auth.expires_at <= time(NULL) + AUTH_REFRESH_WINDOW &&
	    fyai_auth_refresh(ctx, false))
		return -1;
	cfg->api_url = CHATGPT_BASE_URL "/responses";
	return 0;
}

fy_generic fyai_auth_models(struct fyai_ctx *ctx,
			    struct fy_generic_builder *gb, bool full)
{
	struct auth_http r;
	struct curl_slist *headers = NULL;
	const char *url;
	fy_generic doc, models, m, out = fy_seq_empty, providers, item;
	const char *slug;
	bool active;

	if (fyai_auth_resolve(ctx) || !ctx->cfg->chatgpt_auth)
		return fy_invalid;

	url = fy_sprintfa(CHATGPT_BASE_URL "/models?client_version=%s", VERSION);
	if (fyai_auth_apply_headers(ctx, &headers))
		goto err;

	if (auth_http_request(ctx, url, NULL, headers, &r) || r.status / 100 != 2) {
		if (r.status)
			fyai_error(ctx, "model discovery failed (HTTP %ld)",
				   r.status);
		else
			fyai_error(ctx, "model discovery failed");
		free(r.data);
		goto err;
	}

	doc = parse_json_string(gb, r.data);
	free(r.data);
	if (fy_generic_is_invalid(doc))
		goto err;
	models = fy_get(doc, "models");
	if (!fy_generic_is_sequence(models))
		goto err;
	providers = fy_sequence(gb, fy_value(gb, "**chatgpt**"));
	fy_foreach(m, models) {
		slug = fy_get(m, "slug", "");
		if (!*slug)
			slug = fy_get(m, "id", "");
		if (!*slug)
			continue;
		active = ctx->cfg->model && fy_equal(ctx->cfg->model, slug);
		item = fy_null_filtered_mapping(gb,
			"name", fy_value(gb, active ?
					     fy_sprintfa("**%s**", slug) : slug),
			"providers", providers,
			"context_window", fy_get(m, "context_window", 0LL),
			"max_output_tokens", fy_get(m, "max_output_tokens", 0LL),
			"open_source", false,
			"display_name", full ? fy_get(m, "display_name", fy_null) : fy_null,
			"modalities", fy_null,
			"capabilities", full ? fy_get(m, "supported_reasoning_levels", fy_seq_empty) : fy_null);
		out = fy_append(gb, out, item);
	}
	curl_slist_free_all(headers);
	return out;
err:
	curl_slist_free_all(headers);
	return fy_invalid;
}

static const char *auth_effective_method(struct fyai_ctx *ctx, bool logged_in)
{
	struct fyai_cfg *cfg = ctx->cfg;

	if (cfg->auth_mode == FYAI_AUTH_API_KEY)
		return "api-key";
	if (cfg->auth_mode == FYAI_AUTH_CHATGPT)
		return logged_in ? "chatgpt" : "unavailable";
	if (cfg->api_key && *cfg->api_key)
		return "api-key";
	return logged_in ? "chatgpt" : "unavailable";
}

/* Return the status data without choosing an output format. */
fy_generic fyai_auth_status_data(struct fyai_ctx *ctx,
				 struct fy_generic_builder *gb, bool info)
{
	struct fyai_credentials c = {};
	fy_generic doc;
	char when[64] = "unknown";
	struct tm tm;

	if (auth_load(ctx, &c))
		return fy_mapping(gb,
			"provider", "openai", "status", "signed_out",
			"configured_mode", fyai_auth_mode_string(ctx->cfg->auth_mode),
			"effective_method", auth_effective_method(ctx, false));
	if (c.expires_at && gmtime_r(&c.expires_at, &tm))
		strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%SZ", &tm);
	if (info)
		doc = fy_mapping(gb,
			"provider", "openai", "status", "signed_in",
			"configured_mode", fyai_auth_mode_string(ctx->cfg->auth_mode),
			"effective_method", auth_effective_method(ctx, true),
			"auth", "chatgpt", "email", c.email ? c.email : "",
			"account_id", c.account_id, "plan", c.plan ? c.plan : "",
			"expires_at", when, "storage", c.storage ? c.storage : "file",
			"fedramp", c.fedramp);
	else
		doc = fy_mapping(gb,
			"provider", "openai", "status", "signed_in",
			"configured_mode", fyai_auth_mode_string(ctx->cfg->auth_mode),
			"effective_method", auth_effective_method(ctx, true),
			"auth", "chatgpt", "email", c.email ? c.email : "",
			"account_id", c.account_id, "plan", c.plan ? c.plan : "",
			"expires_at", when, "fedramp", c.fedramp);
	credentials_clear(&c);
	return doc;
}

int fyai_auth_status(struct fyai_ctx *ctx, bool json, bool info)
{
	fy_generic doc;
	const char *out;
	doc = fyai_auth_status_data(ctx, ctx->transient_gb, info || json);
	if (fy_generic_is_invalid(doc))
		return -1;
	/*
	 * Column overrides: the keys whose humanized form is not the label we
	 * want, plus the humanized `status` value (signed_in -> "Signed in").
	 * fy_mapping() without a builder is frame-local storage, so the
	 * renderopts must be built here, in the frame that renders them.
	 */
	if (!json)
		return fyai_generic_to_markdown(ctx,
			fy_mapping(
				"title", "Authentication",
				"columns", fy_mapping(
					"status", fy_mapping("format", "humanize"),
					"account_id", fy_mapping("name", "Account"),
					"plan", fy_mapping("name", "Subscription"),
					"expires_at", fy_mapping("name", "Token expires"),
					"fedramp", fy_mapping("name", "FedRAMP account"),
					"storage", fy_mapping("name", "Credential storage"))),
			doc);
	out = emit_request_body(ctx->transient_gb, doc);
	if (out)
		printf("%s\n", out);
	return out ? 0 : -1;
}

static const char *limit_reset_local(fy_generic window, char *buf, size_t size)
{
	long long resets;
	time_t t;
	struct tm tm;

	strncpy(buf, "unknown", size);
	resets = fy_get(window, "reset_at", 0LL);
	t = (time_t)resets;
	/* reset_at is an absolute provider epoch. Present it in the user's local
	 * timezone; %z makes the conversion explicit without assuming a provider
	 * zone or silently labelling local wall time as UTC. */
	if (t && localtime_r(&t, &tm))
		strftime(buf, size, "%Y-%m-%d %H:%M:%S %z", &tm);
	return buf;
}

int fyai_auth_usage(struct fyai_ctx *ctx, bool json)
{
	struct auth_http r = {};
	struct curl_slist *headers = NULL;
	struct fy_generic_builder_cfg cfg = { .flags = FYGBCF_SCOPE_LEADER };
	struct fy_generic_builder *gb = NULL;
	fy_generic doc, rate, credits, reset_credits, display, credit_balance;
	fy_generic primary, secondary;
	char primary_reset[64], secondary_reset[64];
	int rc = -1;

	ctx->cfg->chatgpt_auth = true;
	if (auth_load(ctx, &ctx->auth)) {
		fyai_error(ctx, "openai: not logged in; run `fyai auth openai login`");
		return -1;
	}
	if (ctx->auth.expires_at <= time(NULL) + AUTH_REFRESH_WINDOW &&
	    fyai_auth_refresh(ctx, false)) {
		fyai_error(ctx, "openai: token refresh failed");
		return -1;
	}
	if (fyai_auth_apply_headers(ctx, &headers))
		goto out;
	if (auth_http_request(ctx, CHATGPT_BACKEND_URL "/wham/usage", NULL, headers, &r) ||
	    r.status / 100 != 2) {
		if (r.status)
			fyai_error(ctx, "openai: usage request failed (HTTP %ld)",
				   r.status);
		else
			fyai_error(ctx, "openai: usage request failed");
		goto out;
	}
	gb = fy_generic_builder_create(&cfg);
	if (!gb)
		goto out;
	doc = parse_json_string(gb, r.data);
	if (!fy_generic_is_mapping(doc)) {
		fyai_error(ctx, "openai: malformed usage response");
		goto out;
	}
	if (json) {
		const char *out_json = emit_request_body(gb, doc);
		if (!out_json)
			goto out;
		printf("%s\n", out_json);
	} else {
		rate = fy_get(doc, "rate_limit");
		primary = fy_get(rate, "primary_window");
		secondary = fy_get(rate, "secondary_window");
		reset_credits = fy_get(doc, "rate_limit_reset_credits");
		credits = fy_get(doc, "credits");
		credit_balance = fy_null;
		if (fy_generic_is_mapping(credits))
			credit_balance = fy_get(credits, "unlimited", false) ?
				fy_string("unlimited") :
				fy_get(credits, "balance", fy_string("unknown"));
		display = fy_null_filtered_mapping(gb,
			"effective_method", auth_effective_method(ctx, true),
			"subscription", fy_get(doc, "plan_type",
				ctx->auth.plan ? ctx->auth.plan : "unknown"),
			"requests_allowed", fy_get(rate, "allowed", false),
			"limit_reached", fy_get(rate, "limit_reached", false),
			"reset_credits", fy_generic_is_mapping(reset_credits) ?
				fy_get(reset_credits, "available_count") : fy_null,
			"primary_used", fy_generic_is_mapping(primary) ?
				fy_sprintfa("%lld%%", fy_get(primary, "used_percent", 0LL)) : "unknown",
			"primary_window_hours", fy_generic_is_mapping(primary) ?
				fy_get(primary, "limit_window_seconds", 0LL) / 3600 : 0LL,
			"primary_resets", limit_reset_local(primary, primary_reset,
							 sizeof(primary_reset)),
			"secondary_used", fy_generic_is_mapping(secondary) ?
				fy_sprintfa("%lld%%", fy_get(secondary, "used_percent", 0LL)) : "unknown",
			"secondary_window_hours", fy_generic_is_mapping(secondary) ?
				fy_get(secondary, "limit_window_seconds", 0LL) / 3600 : 0LL,
			"secondary_resets", limit_reset_local(secondary, secondary_reset,
							   sizeof(secondary_reset)),
			"credit_balance", credit_balance);
		if (fyai_generic_to_markdown(ctx,
			fy_mapping("title", "OpenAI subscription usage"), display))
			goto out;
	}
	rc = 0;
out:
	free(r.data);
	curl_slist_free_all(headers);
	if (gb)
		fy_generic_builder_destroy(gb);
	return rc;
}

static int auth_logout(struct fyai_ctx *ctx)
{
	struct fyai_credentials c;
	int lockfd = -1;

	memset(&c, 0, sizeof(c));

	lockfd = auth_lock(ctx);
	if (lockfd < 0) {
		fyai_error(ctx, "cannot lock credential store");
		return -1;
	}
	if (!auth_load(ctx, &c))
		revoke_token(ctx, &c);

	(void)auth_delete_file(ctx);
	(void)auth_delete_keyring(ctx);

	credentials_clear(&c);
	auth_unlock(ctx, lockfd);

	printf("auth: logged out\n");
	return 0;
}

static int auth_login(struct fyai_ctx *ctx, bool device_code, bool no_browser,
		      bool manual)
{
	struct fyai_credentials c;
	int lockfd = -1;
	int rc;

	memset(&c, 0, sizeof(c));

	if (device_code) {
		rc = device_login(ctx, &c);
		fyai_error_check(ctx, !rc, err_out, "device login failed");
	} else {
		rc = browser_login(ctx, &c, no_browser, manual);
		fyai_error_check(ctx, !rc, err_out, "%s login failed",
				 manual ? "manual" :
				 no_browser ? "no-browser" : "browser");
	}

	lockfd = auth_lock(ctx);
	fyai_error_check(ctx, lockfd >= 0, err_out,
			 "cannot lock credential store");

	rc = auth_save(ctx, &c);
	fyai_error_check(ctx, !rc, err_out, "cannot save");

	printf("auth: logged in as %s (%s)\n",
		c.email && *c.email ? c.email : "unknown",
		c.plan ? c.plan : "unknown");

	rc = 0;
out:
	auth_unlock(ctx, lockfd);
	return rc;

err_out:
	rc = -1;
	goto out;
}

int fyai_auth_execute(struct fyai_ctx *ctx)
{
	struct fyai_auth_args *a = &ctx->cfg->cmd.args.auth;
	int rc = -1;

	if (fy_not_equal(a->provider, "openai")) {
		fyai_error(ctx, "provider '%s' is not supported yet", a->provider);
		return -1;
	}

	switch (a->command) {
	case FYAI_AUTH_STATUS:
	case FYAI_AUTH_INFO:
		rc = fyai_auth_status(ctx, a->json, a->command == FYAI_AUTH_INFO);
		if (rc)
			fyai_error(ctx, "status failed");
		break;

	case FYAI_AUTH_USAGE:
		rc = fyai_auth_usage(ctx, a->json);
		if (rc)
			fyai_error(ctx, "usage failed");
		break;

	case FYAI_AUTH_LOGOUT:
		rc = auth_logout(ctx);
		if (rc)
			fyai_error(ctx, "logout failed");
		break;

	case FYAI_AUTH_LOGIN:
		rc = auth_login(ctx, a->device_code, a->no_browser, a->manual);
		if (rc)
			fyai_error(ctx, "login failed");
		break;

	default:
		/* impossible */
		rc = -1;
		break;
	}

	return rc;
}

void fyai_auth_cleanup(struct fyai_ctx *ctx)
{
	credentials_clear(&ctx->auth);
	ctx->cfg->auth_state_dir = NULL;
	if (ctx->auth_gb) {
		fy_generic_builder_destroy(ctx->auth_gb);
		ctx->auth_gb = NULL;
	}
}
