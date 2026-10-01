/*
 * fyai_auth.c - machine-local ChatGPT subscription authentication
 *
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_AUTH

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <openssl/rand.h>

#ifdef HAVE_LIBSECRET
#include <libsecret/secret.h>
#endif
#ifdef __APPLE__
#include <Security/Security.h>
#endif

#include "fyai_sink.h"
#include "fyai.h"
#include "fyai_event.h"
#include "fyai_oauth.h"
#include "fyai_auth.h"
#include "fyai_curl.h"
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

/* Subscription credentials are sent only to these trusted OpenAI endpoints. */
#define AUTH_ISSUER FYAI_AUTH_ISSUER
#define AUTH_AUTHORIZE_URL AUTH_ISSUER "/api/accounts/authorize"
#define AUTH_TOKEN_URL AUTH_ISSUER "/api/accounts/oauth/token"
#define AUTH_JWKS_URL AUTH_ISSUER "/.well-known/jwks.json"
#define AUTH_RESOURCE "https://api.openai.com/v1"
#define AUTH_DYNAMIC_CLIENT "dynamic_agent_client"
#define AUTH_PLAN_SCOPE "chatgpt.tokens.use.direct"
#define AUTH_SCOPES "openid profile email offline_access resource.invoke " AUTH_PLAN_SCOPE
#define AUTH_CALLBACK_PATH "/auth/callback"
#define AUTH_CALLBACK_TIMEOUT_MS 600000
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
	c->registrations = fy_invalid;
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
	if (create && (fyai_mkdir_p(dir) || mkdir_private(dir)))
		return "";
	return fy_gb_intern_string(ctx->transient_gb,
			fy_sprintfa("%s/%s", dir, name));
}

static int auth_lock_mode(struct fyai_ctx *ctx, bool nonblock)
{
	const char *path;
	int operation;
	int saved_errno;
	int fd = -1;

	path = auth_path(ctx, "auth.lock", true);
	if (!*path)
		goto err_out;

	fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0)
		goto err_out;

	operation = LOCK_EX | (nonblock ? LOCK_NB : 0);
	if (flock(fd, operation)) {
		saved_errno = errno;
		close(fd);
		if (nonblock &&
		    (saved_errno == EWOULDBLOCK || saved_errno == EAGAIN))
			return -2;
		errno = saved_errno;
		fd = -1;
		goto err_out;
	}

	return fd;

err_out:
	if (fd >= 0)
		close(fd);
	return -1;
}

static int auth_lock_try(struct fyai_ctx *ctx)
{
	return auth_lock_mode(ctx, true);
}

static void auth_unlock(struct fyai_ctx *ctx, int fd)
{
	(void)ctx;

	if (fd < 0)
		return;

	flock(fd, LOCK_UN);
	close(fd);
}

static int auth_parse_content(struct fyai_ctx *ctx, struct fyai_credentials *c,
			      const char *text, size_t len, const char *storage)
{
	struct fy_generic_builder *gb;
	fy_generic doc;

	gb = auth_builder(ctx);
	credentials_clear(c);
	doc = parse_json_string_size(ctx->transient_gb, text, len);
	if (!fy_is_mapping(doc))
		return -1;
	c->registrations = fy_gb_internalize(gb, fy_get(doc, "registrations", fy_map_empty));
	c->client_id = fy_gb_intern_string(gb, fy_get(doc, "client_id", ""));
	c->host_id = fy_gb_intern_string(gb, fy_get(doc, "ext_agent_host_id", ""));
	c->subject = fy_gb_intern_string(gb, fy_get(doc, "subject", ""));
	c->scope = fy_gb_intern_string(gb, fy_get(doc, "scope", ""));
	c->email = fy_gb_intern_string(gb, fy_get(doc, "email", ""));
	c->access_token = fy_gb_intern_string(gb, fy_get(doc, "access_token", ""));
	c->refresh_token = fy_gb_intern_string(gb, fy_get(doc, "refresh_token", ""));
	c->id_token = fy_gb_intern_string(gb, fy_get(doc, "id_token", ""));
	c->expires_at = (time_t)fy_get(doc, "expires_at", 0LL);
	c->storage = fy_gb_intern_string(gb, storage);
	c->account_id = c->subject;
	c->plan = "ChatGPT plan";
	return 0;
}

bool fyai_auth_credentials_ready(const struct fyai_credentials *c)
{
	return !fy_str_empty(c->client_id) && strcmp(c->client_id, AUTH_DYNAMIC_CLIENT) &&
	       !fy_str_empty(c->host_id) && !fy_str_empty(c->subject) &&
	       !fy_str_empty(c->access_token) && !fy_str_empty(c->refresh_token) &&
	       !fy_str_empty(c->id_token) &&
	       fyai_auth_scope_has(c->scope, AUTH_PLAN_SCOPE) &&
	       fyai_auth_scope_has(c->scope, "resource.invoke");
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

static const char *auth_json(struct fyai_ctx *ctx, struct fyai_credentials *c)
{
	fy_generic doc;

	doc = fy_mapping(ctx->transient_gb,
		"type", "chatgpt", "issuer", AUTH_ISSUER,
		"client_id", c->client_id ? c->client_id : "",
		"ext_agent_host_id", c->host_id ? c->host_id : "",
		"subject", c->subject ? c->subject : "",
		"email", c->email ? c->email : "",
		"scope", c->scope ? c->scope : "",
		"access_token", c->access_token ? c->access_token : "",
		"refresh_token", c->refresh_token ? c->refresh_token : "",
		"id_token", c->id_token ? c->id_token : "",
		"expires_at", (long long)c->expires_at);
	if (!fy_str_empty(c->client_id)) {
		c->registrations = fy_assoc(auth_builder(ctx),
			fy_is_mapping(c->registrations) ? c->registrations : fy_map_empty,
			c->client_id, fy_gb_internalize(auth_builder(ctx), doc));
	}
	doc = fy_assoc(ctx->transient_gb, doc, "registrations",
		fy_is_mapping(c->registrations) ? c->registrations : fy_map_empty);
	return emit_json_string(ctx->transient_gb, doc);
}

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


#endif

static int auth_load(struct fyai_ctx *ctx, struct fyai_credentials *c)
{
	int rc;

	/* A fallback file takes precedence over an unavailable keyring's old entry. */
	rc = auth_load_file(ctx, c);
	if (!rc)
		return 0;
	return auth_load_keyring(ctx, c);
}

static int auth_save(struct fyai_ctx *ctx, struct fyai_credentials *c)
{
	int rc;

	rc = auth_save_keyring(ctx, c);
	if (!rc) {
		rc = auth_delete_file(ctx);
		if (rc)
			return rc;
		c->storage = "keyring";
		return 0;
	}

	rc = auth_save_file(ctx, c);
	if (!rc) {
		c->storage = "file";
		return 0;
	}

	return -1;
}

static int auth_save_file(struct fyai_ctx *ctx, struct fyai_credentials *c)
{
	const char *json;
	const char *tmp = NULL;
	const char *path, *dir;
	int fd = -1, rc = -1;
	int dfd = -1;
	ssize_t wrn;
	size_t total, len;

	path = auth_path(ctx, "auth.json", true);
	if (!path)
		goto err_out;

	tmp = fy_sprintfa("%s.tmp.%ld", path, (long)getpid());
	json = auth_json(ctx, c);
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
	size_t n;
	char *p;

	if (size && nmemb > 1024 * 1024 / size)
		return 0;
	n = size * nmemb;
	if (r->len > 1024 * 1024 - n)
		return 0;
	p = realloc(r->data, r->len + n + 1);
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

	memset(out, 0, sizeof(*out));
	curl = curl_easy_init();
	/* Reserve SIGALRM for the watchdog. */
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

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
	code = fyai_curl_perform(ctx, curl);
	if (code == CURLE_OK)
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out->status);
	curl_easy_cleanup(curl);
	return code == CURLE_OK ? 0 : -1;
}

int fyai_auth_parse_tokens(struct fyai_ctx *ctx, struct fyai_credentials *c, fy_generic doc, bool refresh)
{
	const char *scope;
	long long expires;
	if (!fy_is_mapping(doc) || fy_not_equal(fy_get(doc, "token_type", ""), "Bearer"))
		return -1;
	scope = fy_get(doc, "scope", refresh && c->scope ? c->scope : "");
	if (!fyai_auth_scope_has(scope, AUTH_PLAN_SCOPE) ||
	    !fyai_auth_scope_has(scope, "resource.invoke")) {
		fyai_error(ctx, "ChatGPT plan permission was not granted; sign in again and authorize plan usage");
		return -1;
	}
	expires = fy_get(doc, "expires_in", 0LL);
	if (expires <= 0 || expires > 86400 ||
	    fy_str_empty(fy_get(doc, "access_token", "")) ||
	    fy_str_empty(fy_get(doc, "refresh_token", "")) ||
	    fy_str_empty(fy_get(doc, "id_token", "")))
		return -1;
	c->access_token = fy_gb_intern_string(auth_builder(ctx), fy_get(doc, "access_token", ""));
	c->refresh_token = fy_gb_intern_string(auth_builder(ctx), fy_get(doc, "refresh_token", ""));
	c->id_token = fy_gb_intern_string(auth_builder(ctx), fy_get(doc, "id_token", ""));
	c->scope = fy_gb_intern_string(auth_builder(ctx), scope);
	c->expires_at = time(NULL) + (time_t)expires;
	return 0;
}

enum fyai_auth_login_state {
	FYAILS_BROWSER_WAIT,
	FYAILS_TOKEN_EXCHANGE,
	FYAILS_JWKS,
	FYAILS_CREDENTIAL_WAIT,
	FYAILS_COMPLETED,
	FYAILS_CANCELLED,
	FYAILS_FAILED,
};

struct fyai_auth_login_request {
	struct fyai_ctx *ctx;
	struct fyai_oauth_flow *flow;
	struct fyai_oauth_pkce pkce;
	struct fyai_curl_transfer *transfer;
	struct fyai_event_source *timer_src;
	fyai_auth_login_complete_fn complete;
	void *userdata;
	CURL *curl;
	struct curl_slist *headers;
	struct auth_http response;
	struct fyai_credentials credentials;
	char *body;
	char *redirect;
	int result;
	enum fyai_auth_login_state state;
	bool cancel_requested;
};

static bool fyai_auth_login_state_final(enum fyai_auth_login_state state)
{
	return state == FYAILS_COMPLETED || state == FYAILS_CANCELLED ||
	       state == FYAILS_FAILED;
}

static void fyai_auth_login_http_cleanup(struct fyai_auth_login_request *request)
{
	curl_easy_cleanup(request->curl);
	request->curl = NULL;
	curl_slist_free_all(request->headers);
	request->headers = NULL;
	free(request->response.data);
	memset(&request->response, 0, sizeof(request->response));
	free(request->body);
	request->body = NULL;
}

static void fyai_auth_login_finish(struct fyai_auth_login_request *request,
				  enum fyai_auth_login_state state, int result)
{
	if (fyai_auth_login_state_final(request->state))
		return;
	if (request->flow)
		fyai_oauth_flow_finish(request->flow, !result);
	fyai_oauth_flow_destroy(request->flow);
	request->flow = NULL;
	fyai_event_source_remove(request->timer_src);
	request->timer_src = NULL;
	request->state = state;
	request->result = result;
	if (request->complete)
		request->complete(request, request->userdata);
}

static void fyai_auth_login_http_complete(struct fyai_curl_transfer *transfer,
					 void *userdata);

static int fyai_auth_login_http_submit(struct fyai_auth_login_request *request,
				       const char *url, const char *body)
{
	char *copy;

	copy = body ? strdup(body) : NULL;
	if (body && !copy)
		return -1;
	fyai_auth_login_http_cleanup(request);
	request->body = copy;
	request->curl = curl_easy_init();
	if (!request->curl)
		return -1;
	curl_easy_setopt(request->curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(request->curl, CURLOPT_URL, url);
	curl_easy_setopt(request->curl, CURLOPT_WRITEFUNCTION, auth_write);
	curl_easy_setopt(request->curl, CURLOPT_WRITEDATA, &request->response);
	curl_easy_setopt(request->curl, CURLOPT_TIMEOUT, 120L);
	curl_easy_setopt(request->curl, CURLOPT_USERAGENT, "fyai/" VERSION);
	if (body) {
		request->headers = curl_slist_append(NULL,
			"Content-Type: application/x-www-form-urlencoded");
		if (!request->headers)
			return -1;
		curl_easy_setopt(request->curl, CURLOPT_POSTFIELDS, request->body);
		curl_easy_setopt(request->curl, CURLOPT_HTTPHEADER, request->headers);
	}
	request->transfer = fyai_curl_submit(request->ctx, request->curl,
					    fyai_auth_login_http_complete, request);
	return request->transfer ? 0 : -1;
}

static enum fyai_event_action
fyai_auth_login_credential_timer(const struct fyai_event *ev);

static int fyai_auth_login_save(struct fyai_auth_login_request *request)
{
	struct fyai_credentials active = { .registrations = fy_invalid };
	int lockfd;
	int rc;

	lockfd = auth_lock_try(request->ctx);
	if (lockfd == -2) {
		request->state = FYAILS_CREDENTIAL_WAIT;
		rc = fyai_event_add_timer(fyai_ctx_loop(request->ctx), 50, 0,
			fyai_auth_login_credential_timer, request, &request->timer_src);
		return rc ? -1 : 1;
	}
	if (lockfd < 0)
		return -1;
	rc = auth_load(request->ctx, &active);
	if (!rc)
		request->credentials.registrations = active.registrations;
	rc = auth_save(request->ctx, &request->credentials);
	if (!rc)
		request->ctx->auth = request->credentials;
	auth_unlock(request->ctx, lockfd);
	return rc;
}

static enum fyai_event_action
fyai_auth_login_credential_timer(const struct fyai_event *ev)
{
	struct fyai_auth_login_request *request;
	int rc;

	request = ev->userdata;
	request->timer_src = NULL;
	rc = fyai_auth_login_save(request);
	if (rc <= 0)
		fyai_auth_login_finish(request,
			rc ? FYAILS_FAILED : FYAILS_COMPLETED, rc ? -1 : 0);
	return FYAIEA_CONTINUE;
}

static int auth_validate_tokens(struct fyai_ctx *ctx,
		struct fyai_credentials *c, fy_generic jwks, const char *nonce)
{
	struct fy_generic_builder *gb;
	fy_generic claims;

	gb = auth_builder(ctx);
	claims = fyai_auth_verify_id_token(ctx->transient_gb, c->id_token, jwks,
					 c->client_id, nonce, c->subject, time(NULL));
	if (fy_is_invalid(claims)) {
		fyai_error(ctx, "OpenAI ID token validation failed");
		return -1;
	}
	c->subject = fy_gb_intern_string(gb, fy_get(claims, "sub", ""));
	c->email = fy_gb_intern_string(gb, fy_get(claims, "email", ""));
	c->account_id = c->subject;
	c->plan = "ChatGPT plan";
	return 0;
}

static void fyai_auth_login_http_complete(struct fyai_curl_transfer *transfer,
					 void *userdata)
{
	struct fyai_auth_login_request *request;
	CURLcode code;
	long status = 0;
	fy_generic doc, jwks;
	int rc;

	request = userdata;
	code = fyai_curl_collect(transfer);
	fyai_curl_transfer_destroy(transfer);
	request->transfer = NULL;
	if (request->cancel_requested || code == CURLE_ABORTED_BY_CALLBACK) {
		fyai_auth_login_finish(request, FYAILS_CANCELLED, -1);
		return;
	}
	fyai_error_check(request->ctx, code == CURLE_OK, failed,
		"authentication request failed: %s", curl_easy_strerror(code));
	curl_easy_getinfo(request->curl, CURLINFO_RESPONSE_CODE, &status);
	fyai_error_check(request->ctx, status / 100 == 2, failed,
		"authentication request failed (HTTP %ld)", status);
	if (request->state == FYAILS_TOKEN_EXCHANGE) {
		doc = parse_json_string(request->ctx->transient_gb, request->response.data);
		rc = fyai_auth_parse_tokens(request->ctx, &request->credentials, doc, false);
		fyai_error_check(request->ctx, !rc, failed, "invalid subscription token response");
		request->state = FYAILS_JWKS;
		rc = fyai_auth_login_http_submit(request, AUTH_JWKS_URL, NULL);
		fyai_error_check(request->ctx, !rc, failed, "could not fetch OpenAI signing keys");
		return;
	}
	jwks = parse_json_string(request->ctx->transient_gb, request->response.data);
	rc = auth_validate_tokens(request->ctx, &request->credentials, jwks, request->pkce.nonce);
	if (rc)
		goto failed;
	rc = fyai_auth_login_save(request);
	fyai_error_check(request->ctx, rc >= 0, failed, "cannot save subscription credentials");
	if (!rc)
		fyai_auth_login_finish(request, FYAILS_COMPLETED, 0);
	return;
failed:
	fyai_auth_login_finish(request, FYAILS_FAILED, -1);
}

/* Query values are encoded independently; credentials never enter diagnostics. */
static char *auth_form(struct fyai_ctx *ctx, fy_generic values)
{
	fy_generic key, value;
	char *encoded_key, *encoded_value;
	char *out = NULL;
	char *next;
	int rc;

	out = strdup("");
	if (!out)
		return NULL;
	fy_foreach_key_value(key, value, values) {
		encoded_key = curl_easy_escape(ctx->curl, fy_castp(&key, ""), 0);
		encoded_value = curl_easy_escape(ctx->curl, fy_castp(&value, ""), 0);
		if (!encoded_key || !encoded_value) {
			curl_free(encoded_key);
			curl_free(encoded_value);
			free(out);
			return NULL;
		}
		rc = asprintf(&next, "%s%s%s=%s", out, *out ? "&" : "",
			      encoded_key, encoded_value);
		curl_free(encoded_key);
		curl_free(encoded_value);
		free(out);
		if (rc < 0)
			return NULL;
		out = next;
	}
	return out;
}

static int auth_register_pending(struct fyai_ctx *ctx, struct fyai_credentials *pending)
{
	struct fyai_credentials active = { .registrations = fy_invalid }, registration;
	const char *encoded;
	fy_generic record;
	int lockfd;
	int rc;

	lockfd = auth_lock_try(ctx);
	if (lockfd < 0) {
		fyai_error(ctx, "cannot save issued registration; retry sign-in");
		return -1;
	}
	rc = auth_load(ctx, &active);
	if (rc)
		credentials_clear(&active);
	registration = *pending;
	registration.access_token = registration.refresh_token = registration.id_token = "";
	registration.scope = registration.subject = registration.email = "";
	registration.expires_at = 0;
	registration.registrations = fy_invalid;
	encoded = auth_json(ctx, &registration);
	record = parse_json_string(ctx->transient_gb, encoded);
	/* A registration record has no registry of its own. */
	record = fy_delete_at_path(ctx->transient_gb, record, "registrations");
	active.host_id = pending->host_id;
	active.registrations = fy_assoc(auth_builder(ctx),
		fy_is_mapping(active.registrations) ? active.registrations : fy_map_empty,
		pending->client_id, record);
	if (fy_str_empty(active.client_id))
		active.client_id = pending->client_id;
	rc = auth_save(ctx, &active);
	pending->registrations = active.registrations;
	auth_unlock(ctx, lockfd);
	return rc;
}

static void fyai_auth_login_browser_ready(struct fyai_oauth_flow *flow, void *userdata)
{
	struct fyai_auth_login_request *request;
	const char *client_id;
	char *body;
	int rc;

	request = userdata;
	if (fyai_oauth_flow_state(flow) != FYAI_OAUTH_GOT_CODE) {
		fyai_error(request->ctx, "ChatGPT authorization failed (%s)",
			fyai_oauth_flow_error(flow) ? "permission declined" :
			fyai_oauth_state_string(fyai_oauth_flow_state(flow)));
		goto failed;
	}
	client_id = fyai_oauth_flow_client_id(flow);
	if (fy_str_empty(request->credentials.client_id)) {
		if (fy_str_empty(client_id) || !strcmp(client_id, AUTH_DYNAMIC_CLIENT)) {
			fyai_error(request->ctx, "registration did not return an issued client ID");
			goto failed;
		}
		request->credentials.client_id = fy_gb_intern_string(auth_builder(request->ctx), client_id);
		if (auth_register_pending(request->ctx, &request->credentials))
			goto failed;
	} else if (client_id && strcmp(client_id, request->credentials.client_id)) {
		fyai_error(request->ctx, "callback client ID differs from the selected registration");
		goto failed;
	}
	body = auth_form(request->ctx, fy_mapping(
		"grant_type", "authorization_code",
		"client_id", request->credentials.client_id,
		"code", fyai_oauth_flow_code(flow),
		"code_verifier", request->pkce.verifier,
		"redirect_uri", request->redirect,
		"resource", AUTH_RESOURCE));
	fyai_error_check(request->ctx, body, failed, "could not encode token request");
	request->state = FYAILS_TOKEN_EXCHANGE;
	rc = fyai_auth_login_http_submit(request, AUTH_TOKEN_URL, body);
	free(body);
	if (!rc)
		return;
failed:
	fyai_auth_login_finish(request, FYAILS_FAILED, -1);
}

static int auth_prepare_host(struct fyai_ctx *ctx, struct fyai_credentials *c)
{
	unsigned char bytes[16];
	const char *host;
	int lockfd;
	int rc;

	lockfd = auth_lock_try(ctx);
	if (lockfd < 0) {
		fyai_error(ctx, "cannot lock registration state; retry sign-in");
		return -1;
	}
	rc = auth_load(ctx, c);
	if (rc)
		credentials_clear(c);
	if (fy_str_empty(c->host_id)) {
		rc = RAND_bytes(bytes, sizeof(bytes));
		if (rc != 1) {
			auth_unlock(ctx, lockfd);
			return -1;
		}
		bytes[6] = (bytes[6] & 0x0f) | 0x40;
		bytes[8] = (bytes[8] & 0x3f) | 0x80;
		host = fy_sprintfa(
			"urn:uuid:%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
			bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
			bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
		c->host_id = fy_gb_intern_string(auth_builder(ctx), host);
		rc = auth_save(ctx, c);
	} else {
		rc = 0;
	}
	auth_unlock(ctx, lockfd);
	return rc;
}

struct fyai_auth_login_request *fyai_auth_login_submit(struct fyai_ctx *ctx,
		bool device_code, bool no_browser, const char *account, bool new_account,
		fyai_auth_login_complete_fn complete,
		void *userdata)
{
	static const unsigned short ports[] = { AUTH_PORT, AUTH_FALLBACK_PORT, 0 };
	struct fyai_auth_login_request *request;
	struct fyai_oauth_params params;
	fy_generic query, selected, registrations;
	const char *host, *selected_json;
	char *encoded;
	const char *url;
	int rc;

	if (device_code) {
		fyai_error(ctx, "ChatGPT plan registration requires browser sign-in; use --no-browser or --manual");
		return NULL;
	}
	request = calloc(1, sizeof(*request));
	fyai_error_check(ctx, request, err_out, "could not allocate login request");
	request->ctx = ctx;
	request->complete = complete;
	request->userdata = userdata;
	request->result = -1;
	request->credentials.registrations = fy_invalid;
	request->state = FYAILS_BROWSER_WAIT;
	rc = auth_prepare_host(ctx, &request->credentials);
	if (rc)
		goto failed;
	if (new_account && !fy_str_empty(account)) {
		fyai_error(ctx, "select an account or request a new registration, not both");
		goto failed;
	}
	if (new_account || !fy_str_empty(account)) {
		host = request->credentials.host_id;
		registrations = request->credentials.registrations;
		if (new_account) {
			credentials_clear(&request->credentials);
		} else {
			selected = fy_get(registrations, account, fy_invalid);
			if (!fy_is_mapping(selected)) {
				fyai_error(ctx, "unknown ChatGPT registration; run `fyai auth accounts`");
				goto failed;
			}
			selected_json = emit_json_string(ctx->transient_gb, selected);
			if (!selected_json)
				goto failed;
			rc = auth_parse_content(ctx, &request->credentials,
				selected_json, strlen(selected_json), "selected");
			if (rc)
				goto failed;
		}
		request->credentials.host_id = host;
		request->credentials.registrations = registrations;
	}
	rc = fyai_oauth_pkce_generate(&request->pkce);
	if (rc)
		goto failed;
	memset(&params, 0, sizeof(params));
	params.path = AUTH_CALLBACK_PATH;
	params.ports = ports;
	params.nports = ARRAY_SIZE(ports);
	params.state = request->pkce.state;
	params.timeout_ms = AUTH_CALLBACK_TIMEOUT_MS;
	rc = fyai_oauth_flow_start(ctx, fyai_ctx_loop(ctx), &params,
			fyai_auth_login_browser_ready, request, &request->flow);
	if (rc)
		goto failed;
	rc = asprintf(&request->redirect, "http://127.0.0.1:%u" AUTH_CALLBACK_PATH,
		      fyai_oauth_flow_port(request->flow));
	if (rc < 0)
		goto failed;
	query = fy_mapping(ctx->transient_gb,
		"client_id", fy_str_empty(request->credentials.client_id) ?
			AUTH_DYNAMIC_CLIENT : request->credentials.client_id,
		"ext_agent_host_id", request->credentials.host_id,
		"response_type", "code", "redirect_uri", request->redirect,
		"scope", AUTH_SCOPES, "resource", AUTH_RESOURCE,
		"state", request->pkce.state, "nonce", request->pkce.nonce,
		"code_challenge_method", "S256", "code_challenge", request->pkce.challenge);
	if (fy_str_empty(request->credentials.client_id))
		query = fy_assoc(ctx->transient_gb, query, "agent_name_hint", "fyai");
	/* Retained ID-token hints are omitted from printable authorization URLs. */
	else if (!fy_str_empty(request->credentials.email))
		query = fy_assoc(ctx->transient_gb, query, "login_hint", request->credentials.email);
	encoded = auth_form(ctx, query);
	if (!encoded)
		goto failed;
	url = fy_sprintfa(AUTH_AUTHORIZE_URL "?%s", encoded);
	free(encoded);
	fyai_print_login_url(ctx, "Open this link to sign in:", "Continue with ChatGPT", url);
	if (!no_browser)
		fyai_oauth_open_browser(url);
	return request;
failed:
	fyai_auth_login_destroy(request);
err_out:
	return NULL;
}

void fyai_auth_login_cancel(struct fyai_auth_login_request *request)
{
	if (!request || fyai_auth_login_state_final(request->state))
		return;
	request->cancel_requested = true;
	if (request->transfer) {
		fyai_curl_cancel(request->transfer);
		return;
	}
	fyai_auth_login_finish(request, FYAILS_CANCELLED, -1);
}

bool fyai_auth_login_done(const struct fyai_auth_login_request *request)
{
	return request && fyai_auth_login_state_final(request->state);
}

int fyai_auth_login_collect(const struct fyai_auth_login_request *request)
{
	return fyai_auth_login_done(request) ? request->result : -1;
}

void fyai_auth_login_destroy(struct fyai_auth_login_request *request)
{
	if (!request)
		return;
	if (request->transfer)
		fyai_curl_transfer_destroy(request->transfer);
	fyai_event_source_remove(request->timer_src);
	fyai_oauth_flow_destroy(request->flow);
	fyai_oauth_pkce_cleanup(&request->pkce);
	fyai_auth_login_http_cleanup(request);
	free(request->redirect);
	free(request);
}

/*
 * Refresh state machine
 * =====================
 *
 *                         lock available
 *   NEW ----------------------------------------------+
 *    |                                                |
 *    | lock busy                                      v
 *    +----> WAIT_LOCK -- timer --> WAIT_LOCK --> REQUEST_PENDING
 *                                                   |
 *                                   curl completion |
 *                                                   v
 *                                  COMPLETED / FAILED / CANCELLED
 *
 * The credential lock is always attempted with LOCK_NB. A contending fyai
 * process therefore becomes a timer-backed event source instead of blocking
 * the application loop in flock(2).
 */
enum fyai_auth_refresh_state {
	FYAIARS_NEW,
	FYAIARS_WAIT_LOCK,
	FYAIARS_REQUEST_PENDING,
	FYAIARS_COMPLETED,
	FYAIARS_CANCELLED,
	FYAIARS_FAILED,
};

struct fyai_auth_refresh_request {
	struct fyai_ctx *ctx;
	struct fyai_curl_transfer *transfer;
	struct fyai_event_source *timer_src;
	fyai_auth_refresh_complete_fn complete;
	void *userdata;
	CURL *curl;
	struct curl_slist *headers;
	struct auth_http response;
	char *body;
	int lockfd;
	int result;
	enum fyai_auth_refresh_state state;
	bool force;
	bool refreshing;
	fy_generic jwks;
	struct fyai_credentials credentials;
};

static bool
fyai_auth_refresh_state_final(enum fyai_auth_refresh_state state)
{
	return state == FYAIARS_COMPLETED ||
	       state == FYAIARS_CANCELLED ||
	       state == FYAIARS_FAILED;
}

static const char *
fyai_auth_refresh_state_name(enum fyai_auth_refresh_state state)
{
	switch (state) {
	case FYAIARS_NEW:
		return "new";
	case FYAIARS_WAIT_LOCK:
		return "wait-lock";
	case FYAIARS_REQUEST_PENDING:
		return "request-pending";
	case FYAIARS_COMPLETED:
		return "completed";
	case FYAIARS_CANCELLED:
		return "cancelled";
	case FYAIARS_FAILED:
		return "failed";
	}
	return "unknown";
}

static bool
fyai_auth_refresh_transition_valid(enum fyai_auth_refresh_state from,
				   enum fyai_auth_refresh_state to)
{
	switch (from) {
	case FYAIARS_NEW:
		return to == FYAIARS_WAIT_LOCK ||
		       to == FYAIARS_REQUEST_PENDING ||
		       to == FYAIARS_COMPLETED ||
		       to == FYAIARS_FAILED;
	case FYAIARS_WAIT_LOCK:
		return to == FYAIARS_REQUEST_PENDING ||
		       to == FYAIARS_COMPLETED ||
		       to == FYAIARS_CANCELLED ||
		       to == FYAIARS_FAILED;
	case FYAIARS_REQUEST_PENDING:
		return to == FYAIARS_COMPLETED ||
		       to == FYAIARS_CANCELLED ||
		       to == FYAIARS_FAILED;
	case FYAIARS_COMPLETED:
	case FYAIARS_CANCELLED:
	case FYAIARS_FAILED:
		return false;
	}
	return false;
}

static void
fyai_auth_refresh_transition(struct fyai_auth_refresh_request *request,
			     enum fyai_auth_refresh_state state)
{
	if (!fyai_auth_refresh_transition_valid(request->state, state)) {
		fyai_error(request->ctx,
			   "invalid auth refresh transition %s -> %s",
			   fyai_auth_refresh_state_name(request->state),
			   fyai_auth_refresh_state_name(state));
		if (!fyai_auth_refresh_state_final(request->state))
			request->state = FYAIARS_FAILED;
		return;
	}
	if (request->ctx->cfg->debug)
		fyai_debug(request->ctx, "auth refresh state %s -> %s",
			   fyai_auth_refresh_state_name(request->state),
			   fyai_auth_refresh_state_name(state));
	request->state = state;
}

static void fyai_auth_refresh_complete(
		struct fyai_curl_transfer *transfer, void *userdata)
{
	struct fyai_auth_refresh_request *request;
	struct fyai_ctx *ctx;
	CURLcode code;
	long status;
	int rc;

	request = userdata;
	ctx = request->ctx;
	status = 0;
	request->result = -1;
	code = fyai_curl_collect(transfer);
	fyai_curl_transfer_destroy(transfer);
	request->transfer = NULL;
	if (code == CURLE_ABORTED_BY_CALLBACK) {
		fyai_auth_refresh_transition(request, FYAIARS_CANCELLED);
		goto done;
	}
	fyai_error_check(ctx, code == CURLE_OK, done,
			 "token refresh request failed: %s",
			 curl_easy_strerror(code));
	curl_easy_getinfo(request->curl, CURLINFO_RESPONSE_CODE, &status);
	request->response.status = status;
	fyai_error_check(ctx, status / 100 == 2, done,
			 "refresh failed (HTTP %ld); run `fyai auth login`",
			 status);
	if (!request->refreshing) {
		request->jwks = parse_json_string(ctx->transient_gb, request->response.data);
		fyai_error_check(ctx, fy_is_sequence(fy_get(request->jwks, "keys", fy_invalid)),
			done, "invalid OpenAI signing keys");
		free(request->response.data);
		memset(&request->response, 0, sizeof(request->response));
		curl_easy_setopt(request->curl, CURLOPT_URL, AUTH_TOKEN_URL);
		curl_easy_setopt(request->curl, CURLOPT_POSTFIELDS, request->body);
		curl_easy_setopt(request->curl, CURLOPT_HTTPHEADER, request->headers);
		request->refreshing = true;
		request->transfer = fyai_curl_submit(ctx, request->curl,
			fyai_auth_refresh_complete, request);
		fyai_error_check(ctx, request->transfer, done, "could not refresh subscription tokens");
		return;
	}
	request->credentials = ctx->auth;
	rc = fyai_auth_parse_tokens(ctx, &request->credentials,
		parse_json_string(ctx->transient_gb, request->response.data), true);
	fyai_error_check(ctx, !rc, done, "invalid refreshed subscription tokens");
	rc = auth_validate_tokens(ctx, &request->credentials, request->jwks, NULL);
	fyai_error_check(ctx, !rc, done, "refreshed identity could not be verified");
	rc = auth_save(ctx, &request->credentials);
	fyai_error_check(ctx, !rc, done, "failed to save refreshed tokens");
	ctx->auth = request->credentials;
	request->result = 0;
	fyai_auth_refresh_transition(request, FYAIARS_COMPLETED);
done:
	if (!fyai_auth_refresh_state_final(request->state))
		fyai_auth_refresh_transition(request, FYAIARS_FAILED);
	auth_unlock(ctx, request->lockfd);
	request->lockfd = -1;
	if (request->complete)
		request->complete(request, request->userdata);
}

static int
fyai_auth_refresh_start_locked(struct fyai_auth_refresh_request *request)
{
	struct fyai_ctx *ctx;

	int rc;

	ctx = request->ctx;
	rc = auth_load(ctx, &ctx->auth);
	fyai_error_check(ctx, !rc, err_out,
			 "could not load authentication state");
	if (!request->force &&
	    ctx->auth.expires_at > time(NULL) + AUTH_REFRESH_WINDOW) {
		request->result = 0;
		fyai_auth_refresh_transition(request, FYAIARS_COMPLETED);
		auth_unlock(ctx, request->lockfd);
		request->lockfd = -1;
		return 0;
	}
	fyai_error_check(ctx, fyai_auth_credentials_ready(&ctx->auth), err_out,
			 "saved login requires registration; run `fyai auth login` again");
	fyai_error_check(ctx, ctx->auth.refresh_token &&
			 *ctx->auth.refresh_token, err_out,
			 "authentication state has no refresh token");
	request->curl = curl_easy_init();
	/* Reserve SIGALRM for the watchdog. */
	curl_easy_setopt(request->curl, CURLOPT_NOSIGNAL, 1L);

	fyai_error_check(ctx, request->curl, err_out,
			 "could not create token refresh transfer");
	request->body = auth_form(ctx, fy_mapping(
		"grant_type", "refresh_token", "refresh_token", ctx->auth.refresh_token,
		"client_id", ctx->auth.client_id, "resource", AUTH_RESOURCE));
	fyai_error_check(ctx, request->body, err_out,
			 "could not build token refresh request");
	request->headers = curl_slist_append(request->headers,
			"Content-Type: application/x-www-form-urlencoded");
	fyai_error_check(ctx, request->headers, err_out,
			 "could not create token refresh headers");
	curl_easy_setopt(request->curl, CURLOPT_URL,
			 AUTH_JWKS_URL);
	curl_easy_setopt(request->curl, CURLOPT_WRITEFUNCTION, auth_write);
	curl_easy_setopt(request->curl, CURLOPT_WRITEDATA,
			 &request->response);
	curl_easy_setopt(request->curl, CURLOPT_TIMEOUT, 120L);
	curl_easy_setopt(request->curl, CURLOPT_USERAGENT, "fyai/" VERSION);
	curl_easy_setopt(request->curl, CURLOPT_HTTPGET, 1L);

	fyai_auth_refresh_transition(request, FYAIARS_REQUEST_PENDING);
	request->transfer = fyai_curl_submit(ctx, request->curl,
					     fyai_auth_refresh_complete,
					     request);
	fyai_error_check(ctx, request->transfer, err_out,
			 "could not submit token refresh");
	return 0;

err_out:
	return -1;
}

static enum fyai_event_action
fyai_auth_refresh_lock_timer(const struct fyai_event *ev)
{
	struct fyai_auth_refresh_request *request;
	int rc;

	request = ev->userdata;
	request->timer_src = NULL;
	request->lockfd = auth_lock_try(request->ctx);
	if (request->lockfd == -2) {
		rc = fyai_event_add_timer(ev->loop, 50, 0,
				fyai_auth_refresh_lock_timer, request,
				&request->timer_src);
		if (!rc)
			return FYAIEA_CONTINUE;
	}
	if (request->lockfd >= 0)
		rc = fyai_auth_refresh_start_locked(request);
	else
		rc = -1;
	if (rc) {
		fyai_auth_refresh_transition(request, FYAIARS_FAILED);
		if (request->complete)
			request->complete(request, request->userdata);
	} else if (fyai_auth_refresh_state_final(request->state) &&
		   request->complete) {
		request->complete(request, request->userdata);
	}
	return FYAIEA_CONTINUE;
}

struct fyai_auth_refresh_request *
fyai_auth_refresh_submit(struct fyai_ctx *ctx, bool force,
			 fyai_auth_refresh_complete_fn complete,
			 void *userdata)
{
	struct fyai_auth_refresh_request *request;
	struct fyai_event_loop *el;
	int rc;

	request = calloc(1, sizeof(*request));
	fyai_error_check(ctx, request, err_out, "out of memory");
	request->ctx = ctx;
	request->complete = complete;
	request->userdata = userdata;
	request->lockfd = -1;
	request->result = -1;
	request->state = FYAIARS_NEW;
	request->force = force;
	request->credentials.registrations = fy_invalid;
	request->jwks = fy_invalid;
	request->lockfd = auth_lock_try(ctx);
	if (request->lockfd == -2) {
		fyai_auth_refresh_transition(request, FYAIARS_WAIT_LOCK);
		el = fyai_ctx_loop(ctx);
		fyai_error_check(ctx, el, err_free,
				 "could not acquire the application event loop");
		rc = fyai_event_add_timer(el, 50, 0,
				fyai_auth_refresh_lock_timer, request,
				&request->timer_src);
		fyai_error_check(ctx, !rc, err_free,
				 "could not wait for authentication state");
		return request;
	}
	fyai_error_check(ctx, request->lockfd >= 0, err_free,
			 "could not lock authentication state");
	rc = fyai_auth_refresh_start_locked(request);
	fyai_error_check(ctx, !rc, err_free,
			 "could not start token refresh");
	return request;

err_free:
	fyai_auth_refresh_destroy(request);
err_out:
	return NULL;
}

void fyai_auth_refresh_cancel(struct fyai_auth_refresh_request *request)
{
	if (!request || fyai_auth_refresh_state_final(request->state))
		return;
	if (request->transfer) {
		fyai_curl_cancel(request->transfer);
		return;
	}
	fyai_event_source_remove(request->timer_src);
	request->timer_src = NULL;
	fyai_auth_refresh_transition(request, FYAIARS_CANCELLED);
	if (request->complete)
		request->complete(request, request->userdata);
}

bool fyai_auth_refresh_done(
		const struct fyai_auth_refresh_request *request)
{
	return request && fyai_auth_refresh_state_final(request->state);
}

int fyai_auth_refresh_collect(
		const struct fyai_auth_refresh_request *request)
{
	return fyai_auth_refresh_done(request) ? request->result : -1;
}

void fyai_auth_refresh_destroy(struct fyai_auth_refresh_request *request)
{
	if (!request)
		return;
	if (request->transfer)
		fyai_curl_transfer_destroy(request->transfer);
	fyai_event_source_remove(request->timer_src);
	auth_unlock(request->ctx, request->lockfd);
	curl_easy_cleanup(request->curl);
	curl_slist_free_all(request->headers);
	free(request->response.data);
	free(request->body);
	free(request);
}

static void revoke_token(struct fyai_ctx *ctx, struct fyai_credentials *c)
{
	struct curl_slist *headers = NULL;
	struct auth_http r = {};
	fy_generic discovery;
	const char *endpoint;
	char *body;
	int rc;

	if (fy_str_empty(c->refresh_token) || fy_str_empty(c->client_id))
		return;
	rc = auth_http_request(ctx, AUTH_ISSUER "/.well-known/openid-configuration", NULL, NULL, &r);
	if (rc || r.status != 200)
		goto unconfirmed;
	discovery = parse_json_string(ctx->transient_gb, r.data);
	free(r.data);
	r.data = NULL;
	endpoint = fy_get(discovery, "revocation_endpoint", "");
	if (strncmp(endpoint, AUTH_ISSUER "/", strlen(AUTH_ISSUER "/")))
		goto unconfirmed;
	body = auth_form(ctx, fy_mapping("token", c->refresh_token,
		"token_type_hint", "refresh_token", "client_id", c->client_id));
	if (!body)
		goto unconfirmed;
	headers = curl_slist_append(NULL, "Content-Type: application/x-www-form-urlencoded");
	rc = auth_http_request(ctx, endpoint, body, headers, &r);
	free(body);
	curl_slist_free_all(headers);
	if (!rc && r.status == 200) {
		free(r.data);
		return;
	}
unconfirmed:
	free(r.data);
	fyai_notice(ctx, "remote revocation was not confirmed; disconnect fyai in ChatGPT Settings");
}

int fyai_auth_refresh(struct fyai_ctx *ctx, bool force)
{
	struct fyai_auth_refresh_request *request;
	struct fyai_event_loop *el;
	int rc;

	if (!ctx->cfg->chatgpt_auth)
		return 0;
	request = fyai_auth_refresh_submit(ctx, force, NULL, NULL);
	if (!request)
		return -1;
	el = fyai_ctx_loop(ctx);
	if (!el) {
		fyai_auth_refresh_destroy(request);
		return -1;
	}
	while (!fyai_auth_refresh_done(request) &&
	       !ctx->interrupt_pending) {
		rc = fyai_event_loop_step(el, -1);
		if (rc < 0) {
			fyai_auth_refresh_cancel(request);
			break;
		}
	}
	if (ctx->interrupt_pending) {
		fyai_event_interrupt_ack(ctx);
		fyai_auth_refresh_cancel(request);
	}
	while (!fyai_auth_refresh_done(request)) {
		rc = fyai_event_loop_step(el, -1);
		if (rc < 0)
			break;
	}
	rc = fyai_auth_refresh_done(request) ?
		fyai_auth_refresh_collect(request) : -1;
	fyai_auth_refresh_destroy(request);
	return rc;
}

int fyai_auth_apply_headers(struct fyai_ctx *ctx, struct curl_slist **headers)
{
	const char *bearer;
	int rc;

	if (!ctx->cfg->chatgpt_auth || !fyai_auth_credentials_ready(&ctx->auth) ||
	    !ctx->cfg->api_url || strcmp(ctx->cfg->api_url, OPENAI_RESPONSES_URL))
		return -1;
	bearer = fy_sprintfa("Authorization: Bearer %s", ctx->auth.access_token);
	rc = append_header(headers, bearer);
	return rc;
}

bool fyai_auth_should_retry(struct fyai_ctx *ctx, long status)
{
	return status == 401 && ctx->cfg->chatgpt_auth &&
	       !ctx->auth_retry_done;
}

int fyai_auth_resolve(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg = ctx->cfg;
	bool want_chatgpt;

	want_chatgpt = cfg->auth_mode == FYAI_AUTH_CHATGPT ||
		(cfg->auth_mode == FYAI_AUTH_AUTO && (!cfg->api_key || !*cfg->api_key) &&
		 /* The key is at the transport, which this image cannot see. */
		 !ctx->tclient);

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
	if (!fyai_auth_credentials_ready(&ctx->auth)) {
		fyai_error(ctx, "saved login does not authorize ChatGPT plan usage; run `fyai auth login` again");
		return -1;
	}
	cfg->chatgpt_auth = true;
	if (ctx->auth.expires_at <= time(NULL) + AUTH_REFRESH_WINDOW &&
	    fyai_auth_refresh(ctx, false))
		return -1;
	cfg->api_url = OPENAI_RESPONSES_URL;
	cfg->stream = true;
	cfg->token_extents = false;
	cfg->response_compaction_supported = false;
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

	url = AUTH_RESOURCE "/models";
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
	if (fy_is_invalid(doc))
		goto err;
	models = fy_get(doc, "models");
	if (!fy_is_sequence(models))
		goto err;
	providers = fy_sequence(gb, fy_value(gb, "chatgpt"));
	fy_foreach(m, models) {
		slug = fy_get(m, "slug", "");
		if (!*slug)
			slug = fy_get(m, "id", "");
		if (!*slug)
			continue;
		if (fy_not_equal(fy_get(m, "visibility", ""), "list"))
			continue;
		active = ctx->cfg->model && fy_equal(ctx->cfg->model, slug);
		item = fy_null_filtered_mapping(gb,
			"name", fy_value(gb, slug),
			"active", active,
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

fy_generic fyai_auth_accounts_data(struct fyai_ctx *ctx, struct fy_generic_builder *gb)
{
	struct fyai_credentials c = { .registrations = fy_invalid };
	fy_generic key, record;
	fy_generic accounts = fy_seq_empty;

	if (auth_load(ctx, &c))
		return accounts;
	fy_foreach_key_value(key, record, c.registrations) {
		accounts = fy_append(gb, accounts, fy_mapping(gb,
			"client_id", key, "email", fy_get(record, "email", ""),
			"subject", fy_get(record, "subject", ""),
			"active", fy_equal(key, c.client_id ? c.client_id : ""),
			"signed_in", !fy_str_empty(fy_get(record, "access_token", ""))));
	}
	return accounts;
}

static const char *auth_effective_method(struct fyai_ctx *ctx, bool logged_in)
{
	struct fyai_cfg *cfg = ctx->cfg;

	if (cfg->auth_mode == FYAI_AUTH_API_KEY)
		return "api-key";
	if (cfg->auth_mode == FYAI_AUTH_CHATGPT)
		return logged_in ? "chatgpt" : "unavailable";
	if ((cfg->api_key && *cfg->api_key) || ctx->tclient)
		return "api-key";
	return logged_in ? "chatgpt" : "unavailable";
}

/* Return the status data without choosing an output format. */
fy_generic fyai_auth_status_data(struct fyai_ctx *ctx,
				 struct fy_generic_builder *gb, bool info)
{
	struct fyai_credentials c = { .registrations = fy_invalid };
	fy_generic doc;
	char when[64] = "unknown";
	struct tm tm;

	if (auth_load(ctx, &c) || !fyai_auth_credentials_ready(&c))
		return fy_mapping(gb,
			"provider", "openai", "status", "signed_out",
			"configured_mode", fyai_auth_mode_string(ctx->cfg->auth_mode),
			"client_id", c.client_id ? c.client_id : "",
			"plan_usage_authorized", fyai_auth_credentials_ready(&c),
			"effective_method", auth_effective_method(ctx, false));
	if (c.expires_at && gmtime_r(&c.expires_at, &tm))
		strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%SZ", &tm);
	if (info)
		doc = fy_mapping(gb,
			"provider", "openai", "status", "signed_in",
			"configured_mode", fyai_auth_mode_string(ctx->cfg->auth_mode),
			"client_id", c.client_id ? c.client_id : "",
			"plan_usage_authorized", fyai_auth_credentials_ready(&c),
			"effective_method", auth_effective_method(ctx, true),
			"auth", "chatgpt", "email", c.email ? c.email : "",
			"account_id", c.account_id, "plan", c.plan ? c.plan : "",
			"expires_at", when, "storage", c.storage ? c.storage : "file",
			"fedramp", c.fedramp);
	else
		doc = fy_mapping(gb,
			"provider", "openai", "status", "signed_in",
			"configured_mode", fyai_auth_mode_string(ctx->cfg->auth_mode),
			"client_id", c.client_id ? c.client_id : "",
			"plan_usage_authorized", fyai_auth_credentials_ready(&c),
			"effective_method", auth_effective_method(ctx, true),
			"auth", "chatgpt", "email", c.email ? c.email : "",
			"account_id", c.account_id, "plan", c.plan ? c.plan : "",
			"expires_at", when, "fedramp", c.fedramp);
	credentials_clear(&c);
	return doc;
}

int fyai_auth_usage(struct fyai_ctx *ctx, struct fy_generic_builder *out_gb,
			bool raw, fy_generic *datap)
{
	(void)ctx;
	(void)raw;
	*datap = fy_mapping(out_gb, "usage", "https://chatgpt.com/settings/usage",
		"message", "Review app usage and plan or credit permissions in ChatGPT Settings");
	return fy_is_valid(*datap) ? 0 : -1;
}

int fyai_auth_logout(struct fyai_ctx *ctx)
{
	struct fyai_credentials c = { .registrations = fy_invalid };
	int lockfd;
	int rc = 0;

	lockfd = auth_lock_try(ctx);
	if (lockfd < 0) {
		fyai_error(ctx, "cannot lock credential store; retry logout");
		return -1;
	}
	if (!auth_load(ctx, &c)) {
		revoke_token(ctx, &c);
		c.access_token = c.refresh_token = c.id_token = c.scope = "";
		c.expires_at = 0;
		rc = auth_save(ctx, &c);
	}
	auth_unlock(ctx, lockfd);
	credentials_clear(&ctx->auth);
	ctx->cfg->chatgpt_auth = false;
	return rc;
}

struct auth_login_sync {
	volatile bool done;
};

static void
auth_login_sync_complete(struct fyai_auth_login_request *request,
			 void *userdata)
{
	struct auth_login_sync *sync;

	if (!fyai_auth_login_done(request))
		return;
	sync = userdata;
	sync->done = true;
}

int fyai_auth_login(struct fyai_ctx *ctx, bool device_code, bool no_browser,
		    bool manual, const char *account, bool new_account)
{
	struct fyai_auth_login_request *request;
	struct fyai_event_loop *el;
	struct auth_login_sync sync;
	char line[8192];
	char *redirect_request;
	int rc;

	memset(&sync, 0, sizeof(sync));
	request = fyai_auth_login_submit(ctx, device_code, no_browser || manual,
					 account, new_account, auth_login_sync_complete, &sync);
	fyai_error_check(ctx, request, err_out,
			 "could not start authentication login");
	el = fyai_ctx_loop(ctx);
	fyai_error_check(ctx, el, err_destroy,
			 "could not acquire the application event loop");
	if (manual) {
		fyai_result(ctx, "Paste the complete redirect URL: ");
		if (!fgets(line, sizeof(line), stdin)) {
			fyai_auth_login_cancel(request);
		} else {
			line[strcspn(line, "\r\n")] = '\0';
			redirect_request = fy_sprintfa("GET %s HTTP/1.1\r\n", line);
			fyai_oauth_flow_redirect(request->flow, redirect_request);
		}
	}
	rc = 0;
	while (!sync.done && !ctx->interrupt_pending) {
		rc = fyai_event_loop_step(el, -1);
		if (rc < 0)
			break;
	}
	if (ctx->interrupt_pending) {
		fyai_event_interrupt_ack(ctx);
		fyai_auth_login_cancel(request);
	}
	while (!sync.done && rc >= 0) {
		rc = fyai_event_loop_step(el, -1);
		if (rc < 0)
			break;
	}
	fyai_error_check(ctx, rc >= 0 && sync.done, err_destroy,
			 "authentication login did not complete");
	rc = fyai_auth_login_collect(request);
	fyai_auth_login_destroy(request);
	if (!rc)
		fyai_result(ctx, "auth: login succeeded\n");
	return rc;

err_destroy:
	fyai_auth_login_destroy(request);
err_out:
	return -1;
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
