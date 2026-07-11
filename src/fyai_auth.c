/*
 * fyai_auth.c - machine-local ChatGPT subscription authentication
 *
 * SPDX-License-Identifier: MIT
 */

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
#include "fyai_markdown.h"

extern char **environ;

#define CHATGPT_BASE_URL "https://chatgpt.com/backend-api/codex"
#define CHATGPT_BACKEND_URL "https://chatgpt.com/backend-api"
#define AUTH_ISSUER "https://auth.openai.com"
/* Public native-client identifier used by the upstream Codex login flow.
 * It is not a secret. Keep this compatibility contract compiled in: allowing
 * arena/repository config to replace OAuth or backend endpoints would permit
 * a project to redirect machine-local subscription credentials. */
#define CODEX_OAUTH_CLIENT_ID "app_EMoamEEZ73f0CkXaXp7hrann"
#define CODEX_OAUTH_SCOPE_URL "openid%%20profile%%20email%%20offline_access%%20api.connectors.read%%20api.connectors.invoke"
#define AUTH_PORT 1455
#define AUTH_FALLBACK_PORT 1457
#define AUTH_REFRESH_WINDOW 300

struct auth_http {
	char *data;
	size_t len;
	long status;
};

static int auth_save_file(struct fyai_credentials *c);
static int auth_delete_file(void);

static void credentials_clear(struct fyai_credentials *c)
{
	if (!c)
		return;
	free(c->access_token);
	free(c->refresh_token);
	free(c->id_token);
	free(c->account_id);
	free(c->email);
	free(c->plan);
	free(c->storage);
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

static char *auth_state_dir(void)
{
	const char *p = getenv("XDG_STATE_HOME");
	const char *home;
	char *out;

	if (p && *p) {
		if (asprintf(&out, "%s/fyai", p) >= 0)
			return out;
		return NULL;
	}
	home = getenv("HOME");
	if (!home || !*home)
		return NULL;
	if (asprintf(&out, "%s/.local/state/fyai", home) < 0)
		return NULL;
	return out;
}

static int mkdir_private(const char *path)
{
	struct stat st;

	if (!mkdir(path, 0700))
		return 0;
	if (errno != EEXIST || lstat(path, &st) || !S_ISDIR(st.st_mode))
		return -1;
	if ((st.st_mode & 077) && chmod(path, 0700))
		return -1;
	return 0;
}

static char *auth_path(const char *name, bool create)
{
	char *dir = auth_state_dir();
	char *path = NULL;

	if (!dir)
		return NULL;
	if (create && mkdir_private(dir))
		goto out;
	if (asprintf(&path, "%s/%s", dir, name) < 0)
		path = NULL;
out:
	free(dir);
	return path;
}

static int auth_lock(void)
{
	char *path = auth_path("auth.lock", true);
	int fd;

	if (!path)
		return -1;
	fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	free(path);
	if (fd < 0)
		return -1;
	if (flock(fd, LOCK_EX)) {
		close(fd);
		return -1;
	}
	return fd;
}

static int token_claims(struct fyai_credentials *c)
{
	struct fy_generic_builder_cfg cfg = { .flags = FYGBCF_SCOPE_LEADER };
	struct fy_generic_builder *gb;
	const char *p, *end;
	char *part;
	unsigned char *json;
	size_t len;
	fy_generic doc, auth, v;
	long long exp;

	if (!c->id_token)
		return -1;
	p = strchr(c->id_token, '.');
	if (!p || !(end = strchr(++p, '.')))
		return -1;
	part = strndup(p, (size_t)(end - p));
	if (!part)
		return -1;
	json = fyai_base64url_decode(part, &len);
	free(part);
	if (!json)
		return -1;
	gb = fy_generic_builder_create(&cfg);
	if (!gb) {
		free(json);
		return -1;
	}
	doc = parse_json_string(gb, (char *)json);
	free(json);
	if (fy_generic_is_invalid(doc))
		goto err;
	free(c->email);
	c->email = strdup(fy_get(doc, "email", ""));
	auth = fy_get(doc, "https://api.openai.com/auth");
	if (!fy_generic_is_mapping(auth))
		auth = doc;
	free(c->account_id);
	c->account_id = strdup(fy_get(auth, "chatgpt_account_id", ""));
	free(c->plan);
	v = fy_get(auth, "chatgpt_plan_type");
	c->plan = strdup(fy_generic_is_string(v) ? fy_cast(v, "unknown") : "unknown");
	c->fedramp = fy_get(auth, "chatgpt_account_is_fedramp", false);
	exp = fy_get(doc, "exp", 0LL);
	if (exp > 0)
		c->expires_at = (time_t)exp;
	fy_generic_builder_destroy(gb);
	return c->account_id && *c->account_id ? 0 : -1;
err:
	fy_generic_builder_destroy(gb);
	return -1;
}

static int auth_load_file(struct fyai_credentials *c)
{
	struct fy_generic_builder_cfg cfg = { .flags = FYGBCF_SCOPE_LEADER };
	struct fy_generic_builder *gb;
	char *path = auth_path("auth.json", false), *text = NULL;
	fy_generic doc;
	struct stat st;
	int fd, rc = -1;
	ssize_t n;
	size_t used = 0, cap = 4096;

	if (!path)
		return -1;
	fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	free(path);
	if (fd < 0)
		return -1;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (st.st_mode & 077)) {
		close(fd);
		return -1;
	}
	text = malloc(cap);
	if (!text) {
		close(fd);
		return -1;
	}
	while ((n = read(fd, text + used, cap - used - 1)) > 0) {
		used += (size_t)n;
		if (used + 1 == cap) {
			char *p;
			if (cap >= 1024 * 1024) {
				free(text);
				close(fd);
				return -1;
			}
			cap *= 2;
			p = realloc(text, cap);
			if (!p) {
				free(text);
				close(fd);
				return -1;
			}
			text = p;
		}
	}
	close(fd);
	if (n < 0) {
		free(text);
		return -1;
	}
	text[used] = '\0';
	gb = fy_generic_builder_create(&cfg);
	if (!gb) {
		free(text);
		return -1;
	}
	doc = parse_json_string(gb, text);
	free(text);
	if (fy_generic_is_invalid(doc))
		goto out;
	credentials_clear(c);
	c->access_token = strdup(fy_get(doc, "access_token", ""));
	c->refresh_token = strdup(fy_get(doc, "refresh_token", ""));
	c->id_token = strdup(fy_get(doc, "id_token", ""));
	c->expires_at = (time_t)fy_get(doc, "expires_at", 0LL);
	c->storage = strdup("file");
	if (!c->access_token || !*c->access_token || !c->refresh_token ||
	    !*c->refresh_token || !c->id_token || !*c->id_token)
		goto out;
	if (token_claims(c))
		goto out;
	rc = 0;
out:
	if (rc)
		credentials_clear(c);
	fy_generic_builder_destroy(gb);
	return rc;
}

#if defined(__APPLE__)
static int auth_load_keyring(struct fyai_credentials *c)
{
	const char service[] = "org.fyai.Auth";
	const char account[] = "default";
	UInt32 len = 0;
	void *data = NULL;
	OSStatus status;
	char *secret;
	struct fy_generic_builder_cfg cfg = { .flags = FYGBCF_SCOPE_LEADER };
	struct fy_generic_builder *gb;
	fy_generic doc;
	int rc = -1;

	status = SecKeychainFindGenericPassword(NULL,
		(UInt32)strlen(service), service, (UInt32)strlen(account), account,
		&len, &data, NULL);
	if (status != errSecSuccess || !data)
		return -1;
	secret = strndup(data, len);
	SecKeychainItemFreeContent(NULL, data);
	if (!secret)
		return -1;
	gb = fy_generic_builder_create(&cfg);
	doc = gb ? parse_json_string(gb, secret) : fy_invalid;
	free(secret);
	if (gb && fy_generic_is_valid(doc)) {
		credentials_clear(c);
		c->access_token = strdup(fy_get(doc, "access_token", ""));
		c->refresh_token = strdup(fy_get(doc, "refresh_token", ""));
		c->id_token = strdup(fy_get(doc, "id_token", ""));
		c->expires_at = (time_t)fy_get(doc, "expires_at", 0LL);
		c->storage = strdup("keychain");
		if (c->access_token && *c->access_token && c->refresh_token &&
		    *c->refresh_token && c->id_token && *c->id_token &&
		    !token_claims(c))
			rc = 0;
	}
	if (gb)
		fy_generic_builder_destroy(gb);
	if (rc)
		credentials_clear(c);
	return rc;
}

static char *auth_json(struct fyai_credentials *c)
{
	struct fy_generic_builder_cfg cfg = { .flags = FYGBCF_SCOPE_LEADER };
	struct fy_generic_builder *gb = fy_generic_builder_create(&cfg);
	fy_generic doc;
	const char *json;
	char *copy = NULL;

	if (!gb)
		return NULL;
	doc = fy_mapping(gb, "type", "chatgpt",
		"access_token", c->access_token, "refresh_token", c->refresh_token,
		"id_token", c->id_token, "expires_at", (long long)c->expires_at);
	json = emit_json_string(gb, doc);
	if (json)
		copy = strdup(json);
	fy_generic_builder_destroy(gb);
	return copy;
}

static int auth_save_keyring(struct fyai_credentials *c)
{
	const char service[] = "org.fyai.Auth";
	const char account[] = "default";
	SecKeychainItemRef item = NULL;
	UInt32 old_len = 0;
	void *old_data = NULL;
	char *json = auth_json(c);
	OSStatus status;

	if (!json)
		return -1;
	status = SecKeychainFindGenericPassword(NULL,
		(UInt32)strlen(service), service, (UInt32)strlen(account), account,
		&old_len, &old_data, &item);
	if (old_data)
		SecKeychainItemFreeContent(NULL, old_data);
	if (status == errSecSuccess)
		status = SecKeychainItemModifyAttributesAndData(item, NULL,
			(UInt32)strlen(json), json);
	else if (status == errSecItemNotFound)
		status = SecKeychainAddGenericPassword(NULL,
			(UInt32)strlen(service), service,
			(UInt32)strlen(account), account,
			(UInt32)strlen(json), json, NULL);
	if (item)
		CFRelease(item);
	free(json);
	return status == errSecSuccess ? 0 : -1;
}

static int auth_delete_keyring(void)
{
	const char service[] = "org.fyai.Auth";
	const char account[] = "default";
	SecKeychainItemRef item = NULL;
	OSStatus status;

	status = SecKeychainFindGenericPassword(NULL,
		(UInt32)strlen(service), service, (UInt32)strlen(account), account,
		NULL, NULL, &item);
	if (status == errSecItemNotFound)
		return 0;
	if (status == errSecSuccess)
		status = SecKeychainItemDelete(item);
	if (item)
		CFRelease(item);
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

static int auth_load_keyring(struct fyai_credentials *c)
{
	struct fy_generic_builder_cfg cfg = { .flags = FYGBCF_SCOPE_LEADER };
	struct fy_generic_builder *gb;
	fy_generic doc;
	char *secret;
	int rc;

	secret = secret_password_lookup_sync(&fyai_secret_schema, NULL, NULL,
					     "account", "default", NULL);
	if (!secret)
		return -1;
	/* Reuse the strictly validated file parser through a private temporary
	 * file only when testing is not possible would defeat the keyring. Parse
	 * in-memory by temporarily writing nowhere: use a memory builder here. */
	gb = fy_generic_builder_create(&cfg);
	doc = gb ? parse_json_string(gb, secret) : fy_invalid;
	rc = -1;
	if (gb && fy_generic_is_valid(doc)) {
		credentials_clear(c);
		c->access_token = strdup(fy_get(doc, "access_token", ""));
		c->refresh_token = strdup(fy_get(doc, "refresh_token", ""));
		c->id_token = strdup(fy_get(doc, "id_token", ""));
		c->expires_at = (time_t)fy_get(doc, "expires_at", 0LL);
		c->storage = strdup("keyring");
		if (c->access_token && *c->access_token && c->refresh_token &&
		    *c->refresh_token && c->id_token && *c->id_token &&
		    !token_claims(c))
			rc = 0;
	}
	if (gb)
		fy_generic_builder_destroy(gb);
	secret_password_free(secret);
	if (rc)
		credentials_clear(c);
	return rc;
}

static char *auth_json(struct fyai_credentials *c)
{
	struct fy_generic_builder_cfg cfg = { .flags = FYGBCF_SCOPE_LEADER };
	struct fy_generic_builder *gb = fy_generic_builder_create(&cfg);
	fy_generic doc;
	const char *json;
	char *copy = NULL;

	if (!gb)
		return NULL;
	doc = fy_mapping(gb, "type", "chatgpt",
		"access_token", c->access_token, "refresh_token", c->refresh_token,
		"id_token", c->id_token, "expires_at", (long long)c->expires_at);
	json = emit_json_string(gb, doc);
	if (json)
		copy = strdup(json);
	fy_generic_builder_destroy(gb);
	return copy;
}

static int auth_save_keyring(struct fyai_credentials *c)
{
	char *json = auth_json(c);
	gboolean ok;

	if (!json)
		return -1;
	ok = secret_password_store_sync(&fyai_secret_schema,
		SECRET_COLLECTION_DEFAULT, "fyai ChatGPT authentication", json,
		NULL, NULL, "account", "default", NULL);
	free(json);
	return ok ? 0 : -1;
}

static int auth_delete_keyring(void)
{
	GError *error = NULL;
	(void)secret_password_clear_sync(&fyai_secret_schema, NULL, &error,
					 "account", "default", NULL);
	if (error) {
		g_error_free(error);
		return -1;
	}
	return 0;
}
#else
static int auth_load_keyring(struct fyai_credentials *c) { (void)c; return -1; }
static int auth_save_keyring(struct fyai_credentials *c) { (void)c; return -1; }
static int auth_delete_keyring(void) { return 0; }
#endif

static int auth_load(struct fyai_credentials *c)
{
	return !auth_load_keyring(c) ? 0 : auth_load_file(c);
}

static int auth_save(struct fyai_credentials *c)
{
	if (!auth_save_keyring(c)) {
		auth_delete_file();
		free(c->storage);
		c->storage = strdup("keyring");
		return 0;
	}
	free(c->storage);
	c->storage = strdup("file");
	return auth_save_file(c);
}

static int auth_save_file(struct fyai_credentials *c)
{
	struct fy_generic_builder_cfg cfg = { .flags = FYGBCF_SCOPE_LEADER };
	struct fy_generic_builder *gb;
	fy_generic doc;
	const char *json;
	char *path = NULL, *tmp = NULL;
	char *dir = NULL;
	int fd = -1, rc = -1;
	int dfd;
	ssize_t len;

	path = auth_path("auth.json", true);
	if (!path || asprintf(&tmp, "%s.tmp.%ld", path, (long)getpid()) < 0)
		goto out;
	gb = fy_generic_builder_create(&cfg);
	if (!gb)
		goto out;
	doc = fy_mapping(gb,
		"type", "chatgpt",
		"access_token", c->access_token,
		"refresh_token", c->refresh_token,
		"id_token", c->id_token,
		"expires_at", (long long)c->expires_at);
	json = emit_json_string(gb, doc);
	if (!json)
		goto out_gb;
	fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0)
		goto out_gb;
	len = (ssize_t)strlen(json);
	if (write(fd, json, (size_t)len) != len || write(fd, "\n", 1) != 1 ||
	    fsync(fd) || close(fd)) {
		fd = -1;
		goto out_gb;
	}
	fd = -1;
	if (rename(tmp, path))
		goto out_gb;
	dir = auth_state_dir();
	dfd = dir ? open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC) : -1;
	if (dfd >= 0) {
		(void)fsync(dfd);
		close(dfd);
	}
	free(dir);
	dir = NULL;
	rc = 0;
out_gb:
	fy_generic_builder_destroy(gb);
out:
	if (fd >= 0)
		close(fd);
	if (rc && tmp)
		unlink(tmp);
	free(tmp);
	free(path);
	return rc;
}

static int auth_delete_file(void)
{
	char *path = auth_path("auth.json", false);
	int rc;

	if (!path)
		return -1;
	rc = unlink(path);
	if (rc && errno == ENOENT)
		rc = 0;
	free(path);
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

static int auth_http_request(const char *url, const char *body,
			     struct curl_slist *headers, struct auth_http *out)
{
	CURL *curl = curl_easy_init();
	CURLcode code;

	memset(out, 0, sizeof(*out));
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

static int parse_tokens(struct fyai_credentials *c, const char *json)
{
	struct fy_generic_builder_cfg cfg = { .flags = FYGBCF_SCOPE_LEADER };
	struct fy_generic_builder *gb = fy_generic_builder_create(&cfg);
	fy_generic doc;
	long long expires;
	int rc = -1;

	if (!gb)
		return -1;
	doc = parse_json_string(gb, json ? json : "");
	if (fy_generic_is_invalid(doc))
		goto out;
	credentials_clear(c);
	c->access_token = strdup(fy_get(doc, "access_token", ""));
	c->refresh_token = strdup(fy_get(doc, "refresh_token", ""));
	c->id_token = strdup(fy_get(doc, "id_token", ""));
	expires = fy_get(doc, "expires_in", 0LL);
	c->expires_at = expires > 0 ? time(NULL) + (time_t)expires : 0;
	c->storage = strdup("file");
	if (!c->access_token || !*c->access_token || !c->refresh_token ||
	    !*c->refresh_token || !c->id_token || !*c->id_token || token_claims(c))
		goto out;
	rc = 0;
out:
	if (rc)
		credentials_clear(c);
	fy_generic_builder_destroy(gb);
	return rc;
}

static int exchange_code(struct fyai_credentials *c, const char *code,
			 const char *redirect, const char *verifier)
{
	CURL *curl = curl_easy_init();
	struct auth_http r;
	struct curl_slist *h = NULL;
	char *ec = NULL, *er = NULL, *ev = NULL, *body = NULL;
	int rc = -1;

	if (!curl)
		return -1;
	ec = curl_easy_escape(curl, code, 0);
	er = curl_easy_escape(curl, redirect, 0);
	ev = curl_easy_escape(curl, verifier, 0);
	if (!ec || !er || !ev || asprintf(&body,
		"grant_type=authorization_code&code=%s&redirect_uri=%s&client_id=%s&code_verifier=%s",
			ec, er, CODEX_OAUTH_CLIENT_ID, ev) < 0)
		goto out;
	h = curl_slist_append(h, "Content-Type: application/x-www-form-urlencoded");
	if (auth_http_request(AUTH_ISSUER "/oauth/token", body, h, &r))
		goto out;
	if (r.status >= 200 && r.status < 300)
		rc = parse_tokens(c, r.data);
	else
		fprintf(stderr, "auth: token exchange failed (HTTP %ld)\n", r.status);
	free(r.data);
out:
	curl_slist_free_all(h);
	curl_free(ec); curl_free(er); curl_free(ev);
	curl_easy_cleanup(curl);
	free(body);
	return rc;
}

static int bind_callback(unsigned short *portp)
{
	const unsigned short ports[] = { AUTH_PORT, AUTH_FALLBACK_PORT };
	int fd = -1;
	struct sockaddr_in sa = { .sin_family = AF_INET,
				  .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };

	for (size_t i = 0; i < sizeof(ports) / sizeof(ports[0]); i++) {
		fd = socket(AF_INET, SOCK_STREAM, 0);
		if (fd < 0)
			return -1;
		(void)fcntl(fd, F_SETFD, FD_CLOEXEC);
		sa.sin_port = htons(ports[i]);
		if (!bind(fd, (struct sockaddr *)&sa, sizeof(sa)) && !listen(fd, 4)) {
			*portp = ports[i];
			return fd;
		}
		close(fd);
	}
	return -1;
}

static char *query_value(const char *request, const char *key)
{
	const char *q = strchr(request, '?'), *end, *p;
	size_t klen = strlen(key);

	if (!q)
		return NULL;
	q++;
	end = strchr(q, ' ');
	if (!end)
		return NULL;
	for (p = q; p < end;) {
		if ((size_t)(end - p) > klen && !strncmp(p, key, klen) && p[klen] == '=') {
			const char *v = p + klen + 1, *amp = memchr(v, '&', (size_t)(end - v));
			char *raw = strndup(v, (size_t)((amp ? amp : end) - v));
			char *decoded;
			CURL *curl;
			int n;

			if (!raw)
				return NULL;
			curl = curl_easy_init();
			decoded = curl ? curl_easy_unescape(curl, raw, 0, &n) : NULL;
			free(raw);
			if (curl)
				curl_easy_cleanup(curl);
			if (!decoded)
				return NULL;
			raw = strndup(decoded, (size_t)n);
			curl_free(decoded);
			return raw;
		}
		p = memchr(p, '&', (size_t)(end - p));
		if (!p) break;
		p++;
	}
	return NULL;
}

static void open_browser(const char *url)
{
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

static int browser_login(struct fyai_credentials *c, bool no_browser)
{
	unsigned char verifier_random[48], state_random[32];
	unsigned char digest[SHA256_DIGEST_LENGTH];
	char *verifier = NULL, *challenge = NULL, *state = NULL, *url = NULL;
	char *code = NULL, *got_state = NULL;
	char request[8192];
	char redirect[128];
	unsigned short port;
	struct pollfd pfd;
	int server = -1, client = -1, rc = -1;
	ssize_t n;
	const char ok[] = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nLogin complete. You may close this window.\n";
	const char bad[] = "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nLogin failed.\n";

	if (RAND_bytes(verifier_random, sizeof(verifier_random)) != 1 ||
	    RAND_bytes(state_random, sizeof(state_random)) != 1)
		return -1;
	verifier = fyai_base64url_encode(verifier_random, sizeof(verifier_random));
	state = fyai_base64url_encode(state_random, sizeof(state_random));
	if (!verifier || !state)
		goto out;
	SHA256((unsigned char *)verifier, strlen(verifier), digest);
	challenge = fyai_base64url_encode(digest, sizeof(digest));
	server = bind_callback(&port);
	if (!challenge || server < 0)
		goto out;
	snprintf(redirect, sizeof(redirect), "http://localhost:%u/auth/callback", port);
	if (asprintf(&url,
		AUTH_ISSUER "/oauth/authorize?response_type=code&client_id=" CODEX_OAUTH_CLIENT_ID
		"&redirect_uri=http%%3A%%2F%%2Flocalhost%%3A%u%%2Fauth%%2Fcallback"
		"&scope=%s"
		"&code_challenge=%s&code_challenge_method=S256&id_token_add_organizations=true"
		"&codex_cli_simplified_flow=true&state=%s&originator=fyai",
		port, CODEX_OAUTH_SCOPE_URL, challenge, state) < 0)
		goto out;
	printf("Open this URL to sign in:\n%s\n", url);
	fflush(stdout);
	if (!no_browser)
		open_browser(url);
	pfd.fd = server;
	pfd.events = POLLIN;
	if (poll(&pfd, 1, 600000) <= 0)
		goto out;
	client = accept(server, NULL, NULL);
	if (client < 0)
		goto out;
	(void)fcntl(client, F_SETFD, FD_CLOEXEC);
	n = read(client, request, sizeof(request) - 1);
	if (n <= 0)
		goto out;
	request[n] = '\0';
	if (strncmp(request, "GET /auth/callback?", 19))
		goto respond;
	code = query_value(request, "code");
	got_state = query_value(request, "state");
	if (!code || !got_state || strcmp(got_state, state))
		goto respond;
	if (exchange_code(c, code, redirect, verifier))
		goto respond;
	(void)write(client, ok, sizeof(ok) - 1);
	rc = 0;
	goto out;
respond:
	(void)write(client, bad, sizeof(bad) - 1);
out:
	if (client >= 0) close(client);
	if (server >= 0) close(server);
	free(verifier); free(challenge); free(state); free(url);
	free(code); free(got_state);
	return rc;
}

static int device_login(struct fyai_credentials *c)
{
	struct auth_http r;
	struct fy_generic_builder_cfg gcfg = { .flags = FYGBCF_SCOPE_LEADER };
	struct fy_generic_builder_cfg jcfg = { .flags = FYGBCF_SCOPE_LEADER };
	struct fy_generic_builder *gb = NULL;
	struct fy_generic_builder *jgb = NULL;
	fy_generic doc;
	const char *emitted;
	char *device_id = NULL, *user_code = NULL, *body = NULL;
	int interval = 5, rc = -1;
	char *code = NULL, *verifier = NULL;

	struct curl_slist *json_headers = NULL;
	char *request_json = NULL;

	jgb = fy_generic_builder_create(&jcfg);
	if (!jgb)
		return -1;
	emitted = emit_json_string(jgb,
		fy_mapping(jgb, "client_id", CODEX_OAUTH_CLIENT_ID));
	if (emitted)
		request_json = strdup(emitted);
	fy_generic_builder_destroy(jgb);
	jgb = NULL;
	json_headers = curl_slist_append(json_headers, "Content-Type: application/json");
	if (!request_json || auth_http_request(
		AUTH_ISSUER "/api/accounts/deviceauth/usercode",
		request_json, json_headers, &r) || r.status / 100 != 2) {
		free(request_json);
		curl_slist_free_all(json_headers);
		return -1;
	}
	free(request_json);
	gb = fy_generic_builder_create(&gcfg);
	if (!gb) goto out;
	doc = parse_json_string(gb, r.data);
	device_id = strdup(fy_get(doc, "device_auth_id", ""));
	user_code = strdup(fy_get(doc, "user_code", ""));
	interval = atoi(fy_get(doc, "interval", "5"));
	if (interval < 1)
		interval = 5;
	if (!device_id || !*device_id || !user_code || !*user_code)
		goto out;
	printf("Open https://auth.openai.com/codex/device and enter code %s\n", user_code);
	fflush(stdout);
	for (int elapsed = 0; elapsed < 900; elapsed += interval) {
		sleep((unsigned int)interval);
		free(body); body = NULL;
		jgb = fy_generic_builder_create(&jcfg);
		if (!jgb)
			goto out;
		emitted = emit_json_string(jgb, fy_mapping(jgb,
			"device_auth_id", device_id, "user_code", user_code));
		if (emitted)
			body = strdup(emitted);
		fy_generic_builder_destroy(jgb);
		jgb = NULL;
		if (!body)
			goto out;
		free(r.data); memset(&r, 0, sizeof(r));
		if (auth_http_request(AUTH_ISSUER "/api/accounts/deviceauth/token",
				      body, json_headers, &r))
			goto out;
		if (r.status == 403 || r.status == 404)
			continue;
		if (r.status / 100 != 2)
			goto out;
		fy_generic_builder_destroy(gb);
		gb = fy_generic_builder_create(&gcfg);
		doc = parse_json_string(gb, r.data);
		code = strdup(fy_get(doc, "authorization_code", ""));
		verifier = strdup(fy_get(doc, "code_verifier", ""));
		if (!code || !*code || !verifier || !*verifier)
			goto out;
		rc = exchange_code(c, code, AUTH_ISSUER "/deviceauth/callback", verifier);
		break;
	}
out:
	if (jgb)
		fy_generic_builder_destroy(jgb);
	if (gb) fy_generic_builder_destroy(gb);
	free(r.data); free(device_id); free(user_code); free(body);
	free(code); free(verifier);
	curl_slist_free_all(json_headers);
	return rc;
}

static int refresh_locked(struct fyai_credentials *c)
{
	CURL *curl = curl_easy_init();
	struct auth_http r;
	struct curl_slist *h = NULL;
	char *ert = NULL, *body = NULL;
	int rc = -1;

	if (!curl || !c->refresh_token)
		goto out;
	ert = curl_easy_escape(curl, c->refresh_token, 0);
	if (!ert || asprintf(&body,
		"grant_type=refresh_token&refresh_token=%s&client_id=%s",
		ert, CODEX_OAUTH_CLIENT_ID) < 0)
		goto out;
	h = curl_slist_append(h, "Content-Type: application/x-www-form-urlencoded");
	if (auth_http_request(AUTH_ISSUER "/oauth/token", body, h, &r))
		goto out;
	if (r.status / 100 == 2)
		rc = parse_tokens(c, r.data);
	else
		fprintf(stderr, "auth: refresh failed (HTTP %ld); run `fyai auth login`\n",
			r.status);
	free(r.data);
out:
	if (curl) curl_easy_cleanup(curl);
	curl_free(ert); curl_slist_free_all(h); free(body);
	return rc;
}

static void revoke_token(struct fyai_credentials *c)
{
	struct fy_generic_builder_cfg cfg = { .flags = FYGBCF_SCOPE_LEADER };
	struct fy_generic_builder *gb;
	struct curl_slist *headers = NULL;
	struct auth_http r;
	const char *json;
	char *body = NULL;

	if (!c->refresh_token || !*c->refresh_token)
		return;
	gb = fy_generic_builder_create(&cfg);
	if (!gb)
		return;
	json = emit_json_string(gb, fy_mapping(gb,
		"token", c->refresh_token,
		"token_type_hint", "refresh_token",
		"client_id", CODEX_OAUTH_CLIENT_ID));
	if (json)
		body = strdup(json);
	fy_generic_builder_destroy(gb);
	if (!body)
		return;
	headers = curl_slist_append(headers, "Content-Type: application/json");
	if (!auth_http_request(AUTH_ISSUER "/oauth/revoke", body, headers, &r))
		free(r.data);
	/* Revocation is deliberately best-effort; local logout must still win. */
	curl_slist_free_all(headers);
	free(body);
}

int fyai_auth_refresh(struct fyai_ctx *ctx, bool force)
{
	int lockfd, rc = -1;

	if (!ctx->cfg->chatgpt_auth)
		return 0;
	lockfd = auth_lock();
	if (lockfd < 0)
		return -1;
	/* Another stateless process may already have rotated the token. */
	if (!auth_load(&ctx->auth) && !force && ctx->auth.expires_at > time(NULL) + AUTH_REFRESH_WINDOW) {
		rc = 0;
		goto out;
	}
	if (!ctx->auth.refresh_token || !*ctx->auth.refresh_token)
		goto out;
	if (!refresh_locked(&ctx->auth) && !auth_save(&ctx->auth))
		rc = 0;
out:
	flock(lockfd, LOCK_UN);
	close(lockfd);
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
			fprintf(stderr, "auth: ChatGPT subscriptions support only the OpenAI provider\n");
		return cfg->auth_mode == FYAI_AUTH_CHATGPT ? -1 : 0;
	}
	if (cfg->api_mode != FYAI_API_RESPONSES || cfg->response_chain) {
		fprintf(stderr, "auth: ChatGPT requires Responses API with response_chain disabled\n");
		return -1;
	}
	if (cfg->api_url && fy_not_equal(cfg->api_url, OPENAI_RESPONSES_URL)) {
		fprintf(stderr, "auth: refusing to send ChatGPT credentials to custom api_url\n");
		return -1;
	}
	if (auth_load(&ctx->auth)) {
		if (cfg->auth_mode == FYAI_AUTH_CHATGPT)
			fprintf(stderr, "auth: not logged in; run `fyai auth login`\n");
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
	if (auth_http_request(url, NULL, headers, &r) || r.status / 100 != 2) {
		if (r.status)
			fprintf(stderr, "auth: model discovery failed (HTTP %ld)\n", r.status);
		else
			fprintf(stderr, "auth: model discovery failed\n");
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

static void auth_switching_report(FILE *fp)
{
	fprintf(fp, "\n## Switching authentication\n\n"
	       "```sh\n"
	       "fyai config set auth chatgpt  # require the subscription\n"
	       "fyai config set auth api-key  # require an API key\n"
	       "fyai config set auth auto     # prefer an available API key\n"
	       "```\n");
}

static int auth_status(struct fyai_ctx *ctx, bool json, bool info)
{
	struct fyai_credentials c = {};
	struct fy_generic_builder_cfg cfg = { .flags = FYGBCF_SCOPE_LEADER };
	struct fy_generic_builder *gb;
	fy_generic doc;
	const char *out;
	char *markdown = NULL;
	size_t markdown_len = 0;
	FILE *fp;
	char when[64] = "unknown";
	struct tm tm;

	if (auth_load(&c)) {
		if (json)
			printf("{\"provider\":\"openai\",\"status\":\"signed_out\","
			       "\"configured_mode\":\"%s\",\"effective_method\":\"%s\"}\n",
			       fyai_auth_mode_string(ctx->cfg->auth_mode),
			       auth_effective_method(ctx, false));
		else {
			fp = open_memstream(&markdown, &markdown_len);
			if (!fp)
				return -1;
			fprintf(fp, "# Authentication\n\n"
			       "| Field | Value |\n"
			       "|---|---|\n"
			       "| Provider | `openai` |\n"
			       "| Status | Signed out |\n"
			       "| Configured mode | `%s` |\n"
			       "| Effective method | `%s` |\n",
			       fyai_auth_mode_string(ctx->cfg->auth_mode),
			       auth_effective_method(ctx, false));
			auth_switching_report(fp);
			fclose(fp);
			fyai_print_markdown(markdown, ctx->cfg);
			free(markdown);
		}
		return 0;
	}
	if (c.expires_at && gmtime_r(&c.expires_at, &tm))
		strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%SZ", &tm);
	if (json) {
		gb = fy_generic_builder_create(&cfg);
		if (!gb) {
			credentials_clear(&c);
			return -1;
		}
		doc = fy_mapping(gb,
			"provider", "openai", "status", "signed_in",
			"configured_mode", fyai_auth_mode_string(ctx->cfg->auth_mode),
			"effective_method", auth_effective_method(ctx, true),
			"auth", "chatgpt", "email", c.email ? c.email : "",
			"account_id", c.account_id, "plan", c.plan ? c.plan : "",
			"expires_at", when, "storage", c.storage ? c.storage : "file",
			"fedramp", c.fedramp);
		out = emit_request_body(gb, doc);
		if (out)
			printf("%s\n", out);
		fy_generic_builder_destroy(gb);
		credentials_clear(&c);
		return out ? 0 : -1;
	}
	fp = open_memstream(&markdown, &markdown_len);
	if (!fp) {
		credentials_clear(&c);
		return -1;
	}
	fprintf(fp, "# Authentication\n\n"
	       "| Field | Value |\n"
	       "|---|---|\n"
	       "| Provider | `openai` |\n"
	       "| Status | Signed in |\n"
	       "| Configured mode | `%s` |\n"
	       "| Effective method | `%s` |\n"
	       "| Subscription | `%s` |\n"
	       "| Email | `%s` |\n"
	       "| Account | `%s` |\n"
	       "| Token expires | `%s` |\n",
		fyai_auth_mode_string(ctx->cfg->auth_mode),
		auth_effective_method(ctx, true),
		c.plan ? c.plan : "unknown",
		c.email && *c.email ? c.email : "unknown",
		c.account_id, when);
	if (info)
		fprintf(fp, "| Credential storage | `%s` |\n| FedRAMP account | `%s` |\n",
			c.storage ? c.storage : "file", c.fedramp ? "yes" : "no");
	auth_switching_report(fp);
	fclose(fp);
	fyai_print_markdown(markdown, ctx->cfg);
	free(markdown);
	credentials_clear(&c);
	return 0;
}

static void print_limit_window(FILE *fp, const char *name, fy_generic window)
{
	long long used;
	long long seconds, resets;
	char when[64] = "unknown";
	time_t t;
	struct tm tm;

	if (!fy_generic_is_mapping(window))
		return;
	used = fy_get(window, "used_percent", 0LL);
	seconds = fy_get(window, "limit_window_seconds", 0LL);
	resets = fy_get(window, "reset_at", 0LL);
	t = (time_t)resets;
	if (t && gmtime_r(&t, &tm))
		strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%SZ", &tm);
	fprintf(fp, "| %s | %lld%% | ", name, used);
	if (seconds)
		fprintf(fp, "%lldh", seconds / 3600);
	else
		fprintf(fp, "unknown");
	fprintf(fp, " | `%s` |\n", when);
}

static int auth_usage(struct fyai_ctx *ctx, bool json)
{
	struct auth_http r = {};
	struct curl_slist *headers = NULL;
	struct fy_generic_builder_cfg cfg = { .flags = FYGBCF_SCOPE_LEADER };
	struct fy_generic_builder *gb = NULL;
	fy_generic doc, rate, credits, reset_credits;
	char *markdown = NULL;
	size_t markdown_len = 0;
	FILE *fp = NULL;
	int rc = -1;

	ctx->cfg->chatgpt_auth = true;
	if (auth_load(&ctx->auth)) {
		fprintf(stderr, "auth: openai: not logged in; run `fyai auth openai login`\n");
		return -1;
	}
	if (ctx->auth.expires_at <= time(NULL) + AUTH_REFRESH_WINDOW &&
	    fyai_auth_refresh(ctx, false)) {
		fprintf(stderr, "auth: openai: token refresh failed\n");
		return -1;
	}
	if (fyai_auth_apply_headers(ctx, &headers))
		goto out;
	if (auth_http_request(CHATGPT_BACKEND_URL "/wham/usage", NULL, headers, &r) ||
	    r.status / 100 != 2) {
		if (r.status)
			fprintf(stderr, "auth: openai: usage request failed (HTTP %ld)\n",
				r.status);
		else
			fprintf(stderr, "auth: openai: usage request failed\n");
		goto out;
	}
	gb = fy_generic_builder_create(&cfg);
	if (!gb)
		goto out;
	doc = parse_json_string(gb, r.data);
	if (!fy_generic_is_mapping(doc)) {
		fprintf(stderr, "auth: openai: malformed usage response\n");
		goto out;
	}
	if (json) {
		const char *out_json = emit_request_body(gb, doc);
		if (!out_json)
			goto out;
		printf("%s\n", out_json);
	} else {
		fp = open_memstream(&markdown, &markdown_len);
		if (!fp)
			goto out;
		fprintf(fp, "# OpenAI subscription usage\n\n"
		       "| Field | Value |\n"
		       "|---|---|\n"
		       "| Effective method | `%s` |\n"
		       "| Subscription | `%s` |\n",
			auth_effective_method(ctx, true),
			fy_get(doc, "plan_type", ctx->auth.plan ? ctx->auth.plan : "unknown"));
		rate = fy_get(doc, "rate_limit");
		fprintf(fp, "| Requests allowed | `%s` |\n"
		       "| Limit reached | `%s` |\n",
			fy_get(rate, "allowed", false) ? "yes" : "no",
			fy_get(rate, "limit_reached", false) ? "yes" : "no");
		reset_credits = fy_get(doc, "rate_limit_reset_credits");
		if (fy_generic_is_mapping(reset_credits))
			fprintf(fp, "| Reset credits | `%lld` |\n",
				fy_get(reset_credits, "available_count", 0LL));
		fprintf(fp, "\n## Limits\n\n");
		fprintf(fp, "| Limit | Used | Window | Resets at |\n"
		       "|---|---:|---:|---|\n");
		print_limit_window(fp, "primary", fy_get(rate, "primary_window"));
		print_limit_window(fp, "secondary", fy_get(rate, "secondary_window"));
		credits = fy_get(doc, "credits");
		if (fy_generic_is_mapping(credits))
			fprintf(fp, "\n## Credits\n\n| Field | Value |\n|---|---|\n"
			       "| Balance | `%s`%s |\n",
				fy_get(credits, "unlimited", false) ? "unlimited" :
				fy_get(credits, "balance", "unknown"),
				fy_get(credits, "has_credits", false) ? " available" : "");
		fclose(fp);
		fp = NULL;
		if (fyai_print_markdown(markdown, ctx->cfg))
			goto out;
	}
	rc = 0;
out:
	if (fp)
		fclose(fp);
	free(markdown);
	free(r.data);
	curl_slist_free_all(headers);
	if (gb)
		fy_generic_builder_destroy(gb);
	return rc;
}

int fyai_auth_execute(struct fyai_ctx *ctx)
{
	struct fyai_auth_args *a = &ctx->cfg->cmd.args.auth;
	struct fyai_credentials c = {};
	int lockfd = -1, rc = -1;

	if (fy_not_equal(a->provider, "openai")) {
		fprintf(stderr, "auth: provider '%s' is not supported yet\n", a->provider);
		return -1;
	}
	if (a->command == FYAI_AUTH_STATUS || a->command == FYAI_AUTH_INFO)
		return auth_status(ctx, a->json, a->command == FYAI_AUTH_INFO);
	if (a->command == FYAI_AUTH_USAGE)
		return auth_usage(ctx, a->json);
	if (a->command == FYAI_AUTH_LOGOUT) {
		lockfd = auth_lock();
		if (lockfd < 0) {
			fprintf(stderr, "auth: cannot lock credential store\n");
			return -1;
		}
		if (!auth_load(&c))
			revoke_token(&c);
		rc = auth_delete_file();
		if (auth_delete_keyring())
			rc = -1;
		if (!rc)
			printf("auth: logged out\n");
		goto out;
	}
	rc = a->device_code ? device_login(&c) : browser_login(&c, a->no_browser);
	if (!rc) {
		lockfd = auth_lock();
		if (lockfd < 0) {
			fprintf(stderr, "auth: cannot lock credential store\n");
			rc = -1;
		} else {
			rc = auth_save(&c);
		}
	}
	if (!rc)
		printf("auth: logged in as %s (%s)\n",
			c.email && *c.email ? c.email : "unknown",
			c.plan ? c.plan : "unknown");
	else
		fprintf(stderr, "auth: login failed\n");
out:
	credentials_clear(&c);
	if (lockfd >= 0) {
		flock(lockfd, LOCK_UN);
		close(lockfd);
	}
	return rc;
}

void fyai_auth_cleanup(struct fyai_ctx *ctx)
{
	credentials_clear(&ctx->auth);
}
