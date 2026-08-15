/*
 * fyai_mcp.c - minimal MCP Streamable HTTP client
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_TOOLS

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "fyai_sink.h"
#include "fyai_curl.h"
#include "fyai_auth_util.h"
#include "fyai_display.h"
#include "fyai_event.h"
#include "fyai_jsonrpc.h"
#include "fyai_log.h"
#include "fyai_oauth.h"
#include "fyai_render.h"
#include "fyai_secret.h"
#include "fyai_tools.h"
#include "utils.h"

#define MCP_PREFIX "mcp__"

enum mcp_transport {
	MCP_TRANSPORT_HTTP,
	MCP_TRANSPORT_STDIO,
};

enum mcp_auth_mode {
	MCP_AUTH_BEARER,
	MCP_AUTH_OAUTH,
};

enum mcp_token_auth_method {
	MCP_TOKEN_AUTH_NONE,
	MCP_TOKEN_AUTH_POST,
	MCP_TOKEN_AUTH_BASIC,
};

enum mcp_server_state {
	MCP_SRV_CONNECTING,	/* startup handshake in flight */
	MCP_SRV_READY,		/* initialized and tools discovered */
	MCP_SRV_FAILED,		/* terminal failure; dropped from the catalogue */
};

struct mcp_startup;
struct mcp_oauth_discovery;

struct fyai_mcp_ctx {
	struct fyai_mcp_ctx *next;
	struct fyai_ctx *ctx;
	struct jsonrpc_conn *conn;
	struct mcp_startup *startup;
	struct mcp_oauth_discovery *oauth_discovery;
	struct fyai_event_source *oauth_refresh_timer;
	fy_generic tools;
	enum mcp_server_state state;
	enum mcp_transport transport;
	enum mcp_auth_mode auth_mode;
	CURL *curl;
	pid_t pid;
	int stdin_fd;
	int stdout_fd;
	const char *session_id;
	const char *name;
	const char *endpoint;
	const char *auth_token;
	const char *protocol_version;
	char *oauth_resource_metadata;
	char *oauth_resource;
	char *oauth_issuer;
	char *oauth_authorization_endpoint;
	char *oauth_token_endpoint;
	char *oauth_registration_endpoint;
	char *oauth_client_id;
	char *oauth_client_secret;
	char *oauth_access_token;
	char *oauth_refresh_token;
	char *oauth_scopes;
	char *oauth_redirect_uri;
	char *oauth_redirect_host;
	char *oauth_redirect_path;
	char *oauth_access_type;
	char *oauth_prompt;
	const char *last_error;
	time_t oauth_expires_at;
	enum mcp_token_auth_method oauth_token_auth_method;
	unsigned short oauth_redirect_port;
	bool oauth_client_configured;
	bool oauth_allow_browser;
	bool oauth_force_refresh;
	bool oauth_force_login;
	int timeout;
	bool recovering;

	/* Asynchronous teardown state, driven by fyai_mcp_cleanup(). */
	struct fyai_curl_transfer *del_xfer;
	struct curl_slist *del_headers;
	struct response_buffer del_resp;
	struct fyai_event_source *term_src;
	bool del_done;
	bool term_done;
};

static void mcp_oauth_secret_free(char **secretp)
{
	size_t len;

	if (!secretp || !*secretp)
		return;
	len = strlen(*secretp);
	fyai_secret_clear_and_free(secretp, &len);
}

static void mcp_set_error(struct fyai_mcp_ctx *mcp, const char *fmt, ...)
{
	va_list ap;
	char *message;

	va_start(ap, fmt);
	message = fy_vsprintfa(fmt, ap);
	va_end(ap);
	mcp->last_error = fy_gb_intern_string(mcp->ctx->cfg->gb, message);
}

static void mcp_stdio_stop(struct fyai_ctx *ctx, struct fyai_mcp_ctx *mcp);

static bool mcp_transient_status(CURLcode rc, long status)
{
	return rc == CURLE_COULDNT_CONNECT || rc == CURLE_COULDNT_RESOLVE_HOST ||
		rc == CURLE_RECV_ERROR || rc == CURLE_SEND_ERROR ||
		rc == CURLE_OPERATION_TIMEDOUT || status == 408 || status == 429 ||
		status == 500 || status == 502 || status == 503 || status == 504;
}

/*
 * Extract one auth-param from a WWW-Authenticate challenge. MCP metadata URLs
 * are quoted strings in practice, but accepting a token value makes the parser
 * tolerant of otherwise valid producers. Backslash quoting is decoded in the
 * returned allocation.
 */
static char *mcp_auth_param(const char *challenge, const char *name)
{
	const char *p;
	const char *start;
	char *out;
	char *dst;
	size_t name_len;

	if (!challenge || !name)
		return NULL;
	name_len = strlen(name);
	for (p = challenge; *p;) {
		while (*p && (isspace((unsigned char)*p) || *p == ','))
			p++;
		if (strlen(p) <= name_len ||
		    strncasecmp(p, name, name_len) ||
		    p[name_len] != '=') {
			while (*p && *p != ',' &&
			       !isspace((unsigned char)*p))
				p++;
			continue;
		}
		p += name_len + 1;
		if (*p != '"') {
			start = p;
			while (*p && *p != ',' &&
			       !isspace((unsigned char)*p))
				p++;
			return strndup(start, (size_t)(p - start));
		}
		p++;
		start = p;
		while (*p && *p != '"') {
			if (*p == '\\' && p[1])
				p++;
			p++;
		}
		out = malloc((size_t)(p - start) + 1);
		if (!out)
			return NULL;
		dst = out;
		for (p = start; *p && *p != '"'; p++) {
			if (*p == '\\' && p[1])
				p++;
			*dst++ = *p;
		}
		*dst = '\0';
		return out;
	}
	return NULL;
}

/*
 * HTTP transport policy for the server's JSON-RPC connection: the MCP auth,
 * protocol and session headers, and capturing the session id the server
 * assigns. These are the "extra bits" the generic transport delegates back.
 */
static int mcp_http_add_headers(void *userdata, struct fyai_ctx *ctx,
				struct curl_slist **headers)
{
	struct fyai_mcp_ctx *mcp = userdata;
	char *version = NULL, *auth = NULL, *session = NULL;
	int rc;

	(void)ctx;
	version = make_header("MCP-Protocol-Version: ", mcp->protocol_version);
	rc = version ? append_header(headers, version) : -1;
	if (!rc && mcp->auth_token && *mcp->auth_token) {
		auth = make_header("Authorization: Bearer ", mcp->auth_token);
		rc = auth ? append_header(headers, auth) : -1;
	}
	if (!rc && mcp->session_id) {
		session = make_header("Mcp-Session-Id: ", mcp->session_id);
		rc = session ? append_header(headers, session) : -1;
	}
	free(version);
	free(auth);
	free(session);
	return rc;
}

static void mcp_http_response_header(void *userdata, const char *line,
				     size_t len)
{
	struct fyai_mcp_ctx *mcp = userdata;
	const char key[] = "mcp-session-id:";
	const char *p, *end;

	if (len < sizeof(key) - 1 || strncasecmp(line, key, sizeof(key) - 1))
		return;
	p = line + sizeof(key) - 1;
	end = line + len;
	while (p < end && isspace((unsigned char)*p))
		p++;
	while (end > p && isspace((unsigned char)end[-1]))
		end--;
	mcp->session_id = fy_gb_intern_string(mcp->ctx->cfg->gb,
			fy_sprintfa("%.*s", (int)(end - p), p));
}

bool fyai_mcp_tool_name(const char *name)
{
	return name && !strncmp(name, MCP_PREFIX, sizeof(MCP_PREFIX) - 1) &&
		strstr(name + sizeof(MCP_PREFIX) - 1, "__");
}

fy_generic fyai_mcp_tools(struct fyai_ctx *ctx)
{
	return fy_is_valid(ctx->mcp_tools) ? ctx->mcp_tools : fy_seq_empty;
}

static bool mcp_server_name_valid(const char *name)
{
	const unsigned char *p = (const unsigned char *)name;

	if (!p || !*p)
		return false;
	for (; *p; p++)
		if (!isalnum(*p) && *p != '_' && *p != '-')
			return false;
	return true;
}

static const char *mcp_server_auth_token(struct fyai_ctx *ctx,
					 fy_generic config)
{
	fy_generic ref = fy_get(config, "auth_token", fy_invalid);
	const char *type, *value, *token;

	if (fy_is_invalid(ref))
		return ctx->cfg->mcp_auth_token;
	fyai_error_check(ctx, fy_is_mapping(ref), err_out,
			 "MCP auth_token must be a secret indirection mapping");
	type = fy_get(ref, "type", "");
	if (!strcmp(type, "auto"))
		return NULL;
	fyai_error_check(ctx, !strcmp(type, "env"), err_out,
			 "MCP auth_token has unsupported secret type '%s'", type);
	value = fy_get(ref, "value", "");
	token = *value ? getenv(value) : NULL;
	fyai_error_check(ctx, token && *token, err_out,
			 "MCP auth_token environment variable '%s' is unset", value);
	return token;
err_out:
	return NULL;
}

static int mcp_server_auth_mode(struct fyai_ctx *ctx, fy_generic config,
				enum mcp_auth_mode *modep)
{
	fy_generic auth;
	const char *type;

	auth = fy_get(config, "auth", fy_invalid);
	if (fy_is_invalid(auth)) {
		*modep = MCP_AUTH_BEARER;
		return 0;
	}
	fyai_error_check(ctx, fy_is_mapping(auth), err_out,
			 "MCP auth must be a mapping");
	type = fy_get(auth, "type", "");
	fyai_error_check(ctx, !strcmp(type, "bearer") ||
			 !strcmp(type, "oauth"), err_out,
			 "MCP auth has unsupported type '%s'", type);
	*modep = !strcmp(type, "oauth") ? MCP_AUTH_OAUTH : MCP_AUTH_BEARER;
	return 0;

err_out:
	return -1;
}

static char *mcp_oauth_secret(struct fyai_ctx *ctx, fy_generic ref)
{
	const char *type;
	const char *name;
	const char *value;
	char *stored;
	char *secret;
	size_t secret_len;
	int rc;

	fyai_error_check(ctx, fy_is_mapping(ref), err_out,
			 "MCP OAuth client secret must be an indirection");
	type = fy_get(ref, "type", "");
	name = fy_get(ref, "value", "");
	fyai_error_check(ctx, *name, err_out,
			 "MCP OAuth client secret has no name");
	if (!strcmp(type, "env")) {
		value = getenv(name);
		fyai_error_check(ctx, value && *value, err_out,
				 "MCP OAuth client secret environment "
				 "variable '%s' is unset", name);
		return strdup(value);
	}
	fyai_error_check(ctx, !strcmp(type, "secret"), err_out,
			 "MCP OAuth client secret has unsupported type '%s'",
			 type);
	stored = strdup(fy_sprintfa("fyai:%s", name));
	fyai_error_check(ctx, stored, err_out,
			 "could not build MCP OAuth secret name");
	secret = NULL;
	secret_len = 0;
	rc = fyai_secret_kernel_get(stored, &secret, &secret_len);
	free(stored);
	fyai_error_check(ctx, rc == FYAI_SECRET_OK && secret && *secret,
			 err_out,
			 "MCP OAuth client secret '%s' is unavailable", name);
	return secret;

err_out:
	return NULL;
}

static int mcp_oauth_configure_client(struct fyai_ctx *ctx,
				      struct fyai_mcp_ctx *mcp,
				      fy_generic auth)
{
	fy_generic client;
	fy_generic scopes;
	fy_generic scope;
	fy_generic params;
	fy_generic emitted;
	fy_generic secret_ref;
	const char *method;
	const char *scope_text;
	CURLU *url;
	char *scheme;
	char *host;
	char *path;
	char *port;
	long port_value;
	char *endp;

	client = fy_get(auth, "client", fy_invalid);
	if (fy_is_invalid(client))
		return 0;
	fyai_error_check(ctx, fy_is_mapping(client), err_out,
			 "MCP OAuth client must be a mapping");
	mcp->oauth_client_id = strdup(fy_get(client, "id", ""));
	mcp->oauth_redirect_uri =
		strdup(fy_get(client, "redirect_uri", ""));
	fyai_error_check(ctx, mcp->oauth_client_id &&
			 *mcp->oauth_client_id &&
			 mcp->oauth_redirect_uri &&
			 *mcp->oauth_redirect_uri, err_out,
			 "MCP OAuth client needs id and redirect_uri");
	url = curl_url();
	scheme = NULL;
	host = NULL;
	path = NULL;
	port = NULL;
	fyai_error_check(ctx, url, err_out,
			 "could not parse MCP OAuth redirect");
	fyai_error_check(ctx,
			 !curl_url_set(url, CURLUPART_URL,
				       mcp->oauth_redirect_uri, 0) &&
			 !curl_url_get(url, CURLUPART_SCHEME, &scheme, 0) &&
			 !curl_url_get(url, CURLUPART_HOST, &host, 0),
			 err_url, "invalid MCP OAuth redirect URI");
	fyai_error_check(ctx, !strcmp(scheme, "http") &&
			 (!strcmp(host, "localhost") ||
			  !strcmp(host, "127.0.0.1")),
			 err_url,
			 "MCP OAuth redirect must use an HTTP loopback host");
	(void)curl_url_get(url, CURLUPART_PATH, &path, 0);
	(void)curl_url_get(url, CURLUPART_PORT, &port, 0);
	mcp->oauth_redirect_host = strdup(host);
	mcp->oauth_redirect_path = strdup(path && *path ? path : "/");
	fyai_error_check(ctx, mcp->oauth_redirect_host &&
			 mcp->oauth_redirect_path, err_url,
			 "could not retain MCP OAuth redirect");
	if (port) {
		port_value = strtol(port, &endp, 10);
		fyai_error_check(ctx, !*endp && port_value > 0 &&
				 port_value <= 65535, err_url,
				 "invalid MCP OAuth redirect port");
		mcp->oauth_redirect_port = (unsigned short)port_value;
	}
	curl_free(scheme);
	curl_free(host);
	curl_free(path);
	curl_free(port);
	curl_url_cleanup(url);
	secret_ref = fy_get(client, "secret", fy_invalid);
	if (!fy_is_invalid(secret_ref)) {
		mcp->oauth_client_secret =
			mcp_oauth_secret(ctx, secret_ref);
		fyai_error_check(ctx, mcp->oauth_client_secret, err_out,
				 "could not resolve MCP OAuth client secret");
	}
	method = fy_get(client, "token_endpoint_auth_method",
			mcp->oauth_client_secret ? "client_secret_post" : "none");
	if (!strcmp(method, "none"))
		mcp->oauth_token_auth_method = MCP_TOKEN_AUTH_NONE;
	else if (!strcmp(method, "client_secret_post"))
		mcp->oauth_token_auth_method = MCP_TOKEN_AUTH_POST;
	else if (!strcmp(method, "client_secret_basic"))
		mcp->oauth_token_auth_method = MCP_TOKEN_AUTH_BASIC;
	else
		fyai_error_check(ctx, false, err_out,
				 "unsupported MCP OAuth token authentication '%s'",
				 method);
	fyai_error_check(ctx, mcp->oauth_token_auth_method ==
			 MCP_TOKEN_AUTH_NONE || mcp->oauth_client_secret,
			 err_out,
			 "MCP OAuth token authentication requires a secret");
	scopes = fy_get(auth, "scopes", fy_invalid);
	fyai_error_check(ctx, fy_is_sequence(scopes) &&
			 fy_len(scopes), err_out,
			 "configured MCP OAuth clients require scopes");
	emitted = fy_seq_empty;
	fy_foreach(scope, scopes) {
		scope_text = fy_castp(&scope, "");
		fyai_error_check(ctx, *scope_text, err_out,
				 "MCP OAuth scope must not be empty");
		emitted = fy_append(ctx->cfg->gb, emitted, scope_text);
	}
	scope_text = emit_json_string(ctx->cfg->gb, emitted);
	fyai_error_check(ctx, scope_text, err_out,
			 "could not retain MCP OAuth scopes");
	/*
	 * The JSON sequence is durable and unambiguous for store comparison.
	 * The authorization URL converts it back to a space-separated list.
	 */
	mcp->oauth_scopes = strdup(scope_text);
	fyai_error_check(ctx, mcp->oauth_scopes, err_out,
			 "could not retain MCP OAuth scopes");
	params = fy_get(auth, "authorization_params", fy_invalid);
	if (fy_is_mapping(params)) {
		mcp->oauth_access_type =
			strdup(fy_get(params, "access_type", ""));
		mcp->oauth_prompt = strdup(fy_get(params, "prompt", ""));
		fyai_error_check(ctx, mcp->oauth_access_type &&
				 mcp->oauth_prompt, err_out,
				 "could not retain OAuth authorization params");
	}
	mcp->oauth_client_configured = true;
	return 0;

err_url:
	curl_free(scheme);
	curl_free(host);
	curl_free(path);
	curl_free(port);
	curl_url_cleanup(url);
err_out:
	return -1;
}

#define MCP_STARTUP_RETRIES 3

enum mcp_startup_phase {
	MCP_SU_INIT,
	MCP_SU_NOTIFY,
	MCP_SU_DISCOVER,
};

/* One server's asynchronous startup handshake: initialize, initialized, then
 * paginated tools/list. It settles the server READY or FAILED and drives the
 * next step from each request completion. */
struct mcp_startup {
	struct fyai_mcp_ctx *mcp;
	struct jsonrpc_request *req;
	struct fyai_event_source *timer;
	char *cursor;
	long long id;			/* current request id, reused across retry */
	enum mcp_startup_phase phase;
	int attempt;
};

static void mcp_startup_step(struct mcp_startup *su);
static void mcp_startup_enter(struct mcp_startup *su,
			      enum mcp_startup_phase phase);

static void mcp_startup_settle(struct mcp_startup *su,
			       enum mcp_server_state state)
{
	struct fyai_mcp_ctx *mcp = su->mcp;
	struct fyai_ctx *ctx = mcp->ctx;

	if (su->timer) {
		fyai_event_source_remove(su->timer);
		su->timer = NULL;
	}
	if (su->req) {
		jsonrpc_request_destroy(su->req);
		su->req = NULL;
	}
	mcp->state = state;
	if (state == MCP_SRV_READY)
		mcp->last_error = NULL;
	mcp->startup = NULL;
	free(su->cursor);
	free(su);
	if (ctx->cfg->mcp_logging)
		(void)fyai_log_generic(ctx, "mcp", fy_mapping(
			"event", state == MCP_SRV_READY ? "discovery" : "failed",
			"server", mcp->name, "tools",
			(long long)(fy_is_valid(mcp->tools) ?
				    fy_len(mcp->tools) : 0)));
	if (state == MCP_SRV_FAILED)
		fyai_warning(ctx, "MCP server '%s' is unavailable", mcp->name);
}

enum mcp_oauth_discovery_state {
	MCP_OD_RESOURCE,
	MCP_OD_AUTHORIZATION_SERVER,
	MCP_OD_REGISTER,
	MCP_OD_AUTHORIZE,
	MCP_OD_TOKEN,
	MCP_OD_REFRESH,
	MCP_OD_COMPLETED,
	MCP_OD_FAILED,
	MCP_OD_CANCELLED,
};

/*
 * OAuth discovery is a child state machine of MCP startup:
 *
 *   RESOURCE -> AUTHORIZATION_SERVER -> REGISTER -> COMPLETED
 *      |                 |                 |
 *      +------ error ----+-----------------+-------------------> FAILED
 *      +------ teardown --------------------------------------> CANCELLED
 *
 * Each request is submitted to the context curl multi handle. No transition
 * pumps the loop itself. The startup object remains parked while discovery
 * owns the current transfer and resumes only from the completion callback.
 */
struct mcp_oauth_discovery {
	struct fyai_mcp_ctx *mcp;
	struct mcp_startup *startup;
	struct fyai_curl_transfer *transfer;
	CURL *curl;
	struct response_buffer response;
	struct curl_slist *headers;
	struct fyai_oauth_flow *flow;
	struct fyai_oauth_pkce pkce;
	char *url;
	char *body;
	char *redirect;
	enum mcp_oauth_discovery_state state;
};

static void mcp_oauth_discovery_drop_http(struct mcp_oauth_discovery *od)
{
	if (od->transfer) {
		fyai_curl_transfer_destroy(od->transfer);
		od->transfer = NULL;
	}
	curl_easy_cleanup(od->curl);
	od->curl = NULL;
	free(od->response.data);
	memset(&od->response, 0, sizeof(od->response));
	curl_slist_free_all(od->headers);
	od->headers = NULL;
	free(od->url);
	od->url = NULL;
	free(od->body);
	od->body = NULL;
}

static void mcp_oauth_discovery_destroy(struct mcp_oauth_discovery *od)
{
	if (!od)
		return;
	mcp_oauth_discovery_drop_http(od);
	if (od->flow) {
		fyai_oauth_flow_finish(od->flow, false);
		fyai_oauth_flow_destroy(od->flow);
	}
	fyai_oauth_pkce_cleanup(&od->pkce);
	free(od->redirect);
	if (od->mcp && od->mcp->oauth_discovery == od)
		od->mcp->oauth_discovery = NULL;
	free(od);
}

static char *mcp_oauth_metadata_url(const char *issuer)
{
	CURLU *url;
	char *scheme;
	char *host;
	char *port;
	char *path;
	char *out;
	const char *formatted;
	int rc;

	url = NULL;
	scheme = NULL;
	host = NULL;
	port = NULL;
	path = NULL;
	out = NULL;
	url = curl_url();
	if (!url)
		goto out;
	rc = curl_url_set(url, CURLUPART_URL, issuer, 0);
	if (rc)
		goto out;
	rc = curl_url_get(url, CURLUPART_SCHEME, &scheme, 0);
	if (!rc)
		rc = curl_url_get(url, CURLUPART_HOST, &host, 0);
	if (!rc) {
		rc = curl_url_get(url, CURLUPART_PORT, &port, 0);
		if (rc == CURLUE_NO_PORT)
			rc = 0;
	}
	if (!rc)
		rc = curl_url_get(url, CURLUPART_PATH, &path, 0);
	if (rc)
		goto out;
	if (!path || !strcmp(path, "/"))
		path = NULL;
	formatted = fy_sprintfa("%s://%s%s%s/.well-known/"
			       "oauth-authorization-server%s",
			       scheme, host, port ? ":" : "",
			       port ? port : "", path ? path : "");
	out = strdup(formatted);

out:
	curl_free(scheme);
	curl_free(host);
	curl_free(port);
	curl_free(path);
	curl_url_cleanup(url);
	return out;
}

static char *mcp_oauth_resource_url(const char *endpoint)
{
	CURLU *url;
	char *scheme;
	char *host;
	char *port;
	char *path;
	char *out;

	url = curl_url();
	scheme = NULL;
	host = NULL;
	port = NULL;
	path = NULL;
	out = NULL;
	if (!url || curl_url_set(url, CURLUPART_URL, endpoint, 0) ||
	    curl_url_get(url, CURLUPART_SCHEME, &scheme, 0) ||
	    curl_url_get(url, CURLUPART_HOST, &host, 0))
		goto out;
	(void)curl_url_get(url, CURLUPART_PORT, &port, 0);
	(void)curl_url_get(url, CURLUPART_PATH, &path, 0);
	if (asprintf(&out, "%s://%s%s%s/"
		     ".well-known/oauth-protected-resource%s",
		     scheme, host, port ? ":" : "", port ? port : "",
		     path && strcmp(path, "/") ? path : "") < 0)
		out = NULL;
out:
	curl_free(scheme);
	curl_free(host);
	curl_free(port);
	curl_free(path);
	curl_url_cleanup(url);
	return out;
}

static int mcp_oauth_discovery_submit(struct mcp_oauth_discovery *od);
static int mcp_oauth_authorize_start(struct mcp_oauth_discovery *od);
static void mcp_oauth_refresh_schedule(struct fyai_mcp_ctx *mcp);

static char *mcp_oauth_store_name(struct fyai_mcp_ctx *mcp)
{
	return strdup(fy_sprintfa("mcp-auth-%s.json", mcp->name));
}

static int mcp_oauth_store_load(struct fyai_mcp_ctx *mcp)
{
	struct fy_generic_builder *gb;
	fy_generic doc;
	const char *issuer;
	const char *client_id;
	const char *scopes;
	const char *resource;
	char *name;
	char *text;

	name = mcp_oauth_store_name(mcp);
	if (!name)
		return -1;
	text = fyai_auth_store_read(mcp->ctx, name);
	free(name);
	if (!text)
		return 0;
	gb = fyai_ctx_transient_gb(mcp->ctx);
	doc = parse_json_string(gb, text);
	free(text);
	if (!fy_is_mapping(doc))
		return -1;
	issuer = fy_get(doc, "issuer", "");
	if (strcmp(issuer, mcp->oauth_issuer)) {
		fyai_debug(mcp->ctx,
			"MCP %s ignored OAuth cache: issuer changed",
			mcp->name);
		return 0;
	}
	client_id = fy_get(doc, "client_id", "");
	scopes = fy_get(doc, "scopes", "");
	resource = fy_get(doc, "resource", "");
	if (strcmp(resource, mcp->oauth_resource)) {
		fyai_debug(mcp->ctx,
			"MCP %s ignored OAuth cache: resource changed",
			mcp->name);
		return 0;
	}
	if (mcp->oauth_client_configured &&
	    (strcmp(client_id, mcp->oauth_client_id) ||
	     strcmp(scopes, mcp->oauth_scopes))) {
		fyai_debug(mcp->ctx,
			"MCP %s ignored OAuth cache: client changed",
			mcp->name);
		return 0;
	}
	if (!mcp->oauth_client_configured) {
		free(mcp->oauth_client_id);
		mcp_oauth_secret_free(&mcp->oauth_client_secret);
		mcp->oauth_client_id = strdup(client_id);
		mcp->oauth_client_secret =
			strdup(fy_get(doc, "client_secret", ""));
		if (!mcp->oauth_client_id || !mcp->oauth_client_secret)
			return -1;
	}
	free(mcp->oauth_access_token);
	free(mcp->oauth_refresh_token);
	mcp->oauth_access_token = strdup(fy_get(doc, "access_token", ""));
	mcp->oauth_refresh_token = strdup(fy_get(doc, "refresh_token", ""));
	mcp->oauth_expires_at = (time_t)fy_get(doc, "expires_at", 0LL);
	if (!mcp->oauth_access_token || !mcp->oauth_refresh_token)
		return -1;
	return 0;
}

static int mcp_oauth_store_save(struct fyai_mcp_ctx *mcp)
{
	struct fy_generic_builder *gb;
	fy_generic doc;
	const char *json;
	char *name;
	int lockfd;
	int rc;

	gb = fyai_ctx_transient_gb(mcp->ctx);
	doc = fy_mapping(gb,
		"server", mcp->name,
		"issuer", mcp->oauth_issuer,
		"resource", mcp->oauth_resource,
		"client_id", mcp->oauth_client_id ?
			mcp->oauth_client_id : "",
		"client_secret", mcp->oauth_client_secret ?
			(mcp->oauth_client_configured ? "" :
			 mcp->oauth_client_secret) : "",
		"scopes", mcp->oauth_scopes ? mcp->oauth_scopes : "",
		"access_token", mcp->oauth_access_token ?
			mcp->oauth_access_token : "",
		"refresh_token", mcp->oauth_refresh_token ?
			mcp->oauth_refresh_token : "",
		"expires_at", (long long)mcp->oauth_expires_at);
	json = emit_json_string(gb, doc);
	if (!json)
		return -1;
	lockfd = fyai_auth_store_lock(mcp->ctx, true);
	if (lockfd < 0)
		return -1;
	name = mcp_oauth_store_name(mcp);
	if (!name) {
		fyai_auth_store_unlock(lockfd);
		return -1;
	}
	rc = fyai_auth_store_write(mcp->ctx, name, json);
	free(name);
	fyai_auth_store_unlock(lockfd);
	return rc;
}

static void mcp_oauth_discovery_fail(struct mcp_oauth_discovery *od,
				     const char *message)
{
	struct mcp_startup *su;
	struct fyai_mcp_ctx *mcp;
	struct fyai_ctx *ctx;

	su = od->startup;
	mcp = od->mcp;
	ctx = mcp->ctx;
	od->state = MCP_OD_FAILED;
	mcp_set_error(mcp, "OAuth discovery: %s", message);
	fyai_error(ctx, "MCP server '%s' OAuth discovery failed: %s",
		   od->mcp->name, message);
	if (mcp->recovering) {
		mcp->recovering = false;
		mcp->state = MCP_SRV_FAILED;
		mcp_oauth_discovery_destroy(od);
		fyai_warning(ctx, "MCP server '%s' is unavailable", mcp->name);
		return;
	}
	if (!su) {
		mcp->oauth_expires_at = time(NULL) + 60;
		mcp_oauth_discovery_destroy(od);
		mcp_oauth_refresh_schedule(mcp);
		return;
	}
	mcp_oauth_discovery_destroy(od);
	mcp_startup_settle(su, MCP_SRV_FAILED);
}

static void mcp_oauth_resume(struct mcp_oauth_discovery *od)
{
	struct mcp_startup *su;
	struct fyai_mcp_ctx *mcp;

	su = od->startup;
	mcp = od->mcp;
	mcp->auth_token = fy_gb_intern_string(mcp->ctx->cfg->gb,
					      mcp->oauth_access_token);
	if (!mcp->auth_token) {
		mcp_oauth_discovery_fail(od,
			"could not retain the OAuth access token");
		return;
	}
	mcp_oauth_discovery_destroy(od);
	mcp_oauth_refresh_schedule(mcp);
	if (su) {
		mcp_startup_enter(su, MCP_SU_INIT);
	} else if (mcp->recovering) {
		mcp->recovering = false;
		mcp->state = MCP_SRV_READY;
	}
}

static void mcp_oauth_authorize_done(struct fyai_oauth_flow *flow,
				     void *userdata)
{
	struct mcp_oauth_discovery *od;
	struct fyai_mcp_ctx *mcp;
	const char *code;
	char *encoded_code;
	char *encoded_redirect;
	char *encoded_verifier;
	char *encoded_secret;
	char *encoded_client;
	char *encoded_resource;
	const char *formatted;
	int rc;

	od = userdata;
	mcp = od->mcp;
	if (fyai_oauth_flow_state(flow) != FYAI_OAUTH_GOT_CODE) {
		mcp_oauth_discovery_fail(od, "authorization was not completed");
		return;
	}
	code = fyai_oauth_flow_code(flow);
	encoded_code = curl_easy_escape(NULL, code, 0);
	encoded_redirect = curl_easy_escape(NULL, od->redirect, 0);
	encoded_verifier = curl_easy_escape(NULL, od->pkce.verifier, 0);
	encoded_client = curl_easy_escape(NULL, mcp->oauth_client_id, 0);
	encoded_resource = curl_easy_escape(NULL, mcp->oauth_resource, 0);
	encoded_secret = mcp->oauth_token_auth_method == MCP_TOKEN_AUTH_POST ?
		curl_easy_escape(NULL, mcp->oauth_client_secret, 0) : NULL;
	if (!encoded_code || !encoded_redirect || !encoded_verifier ||
	    !encoded_client || !encoded_resource ||
	    (mcp->oauth_token_auth_method == MCP_TOKEN_AUTH_POST &&
	     !encoded_secret)) {
		curl_free(encoded_code);
		curl_free(encoded_redirect);
		curl_free(encoded_verifier);
		curl_free(encoded_secret);
		curl_free(encoded_client);
		curl_free(encoded_resource);
		mcp_oauth_discovery_fail(od,
			"could not encode the token exchange");
		return;
	}
	od->url = strdup(mcp->oauth_token_endpoint);
	formatted = fy_sprintfa(
		"grant_type=authorization_code&code=%s&redirect_uri=%s&"
		"client_id=%s&code_verifier=%s&resource=%s%s%s",
		encoded_code, encoded_redirect, encoded_client,
		encoded_verifier, encoded_resource,
		encoded_secret ? "&client_secret=" : "",
		encoded_secret ? encoded_secret : "");
	od->body = strdup(formatted);
	curl_free(encoded_code);
	curl_free(encoded_redirect);
	curl_free(encoded_verifier);
	curl_free(encoded_secret);
	curl_free(encoded_client);
	curl_free(encoded_resource);
	if (!od->url || !od->body) {
		mcp_oauth_discovery_fail(od,
			"could not build the token exchange");
		return;
	}
	od->headers = curl_slist_append(od->headers,
		"Content-Type: application/x-www-form-urlencoded");
	if (!od->headers) {
		mcp_oauth_discovery_fail(od,
			"could not build token exchange headers");
		return;
	}
	od->state = MCP_OD_TOKEN;
	rc = mcp_oauth_discovery_submit(od);
	if (rc)
		mcp_oauth_discovery_fail(od,
			"could not submit the token exchange");
}

static char *mcp_oauth_scope_param(struct fyai_mcp_ctx *mcp)
{
	struct fy_generic_builder *gb;
	fy_generic scopes;
	fy_generic scope;
	const char *value;
	char *out;
	char *p;
	size_t len;

	if (!mcp->oauth_scopes)
		return strdup("");
	gb = fyai_ctx_transient_gb(mcp->ctx);
	scopes = parse_json_string(gb, mcp->oauth_scopes);
	if (!fy_is_sequence(scopes))
		return NULL;
	len = 1;
	fy_foreach(scope, scopes)
		len += strlen(fy_castp(&scope, "")) + 1;
	out = malloc(len);
	if (!out)
		return NULL;
	p = out;
	fy_foreach(scope, scopes) {
		value = fy_castp(&scope, "");
		if (p != out)
			*p++ = ' ';
		memcpy(p, value, strlen(value));
		p += strlen(value);
	}
	*p = '\0';
	return out;
}

static int mcp_oauth_receiver_start(struct mcp_oauth_discovery *od)
{
	unsigned short ports[1];
	struct fyai_oauth_params params;
	struct fyai_event_loop *el;
	unsigned short port;
	int rc;

	if (od->flow)
		return 0;
	rc = fyai_oauth_pkce_generate(&od->pkce);
	if (rc)
		return -1;
	memset(&params, 0, sizeof(params));
	ports[0] = od->mcp->oauth_client_configured ?
		od->mcp->oauth_redirect_port : 0;
	params.path = od->mcp->oauth_redirect_path ?
		od->mcp->oauth_redirect_path : "/mcp/oauth/callback";
	params.ports = ports;
	params.nports = ARRAY_SIZE(ports);
	params.state = od->pkce.state;
	params.timeout_ms = 300000;
	el = fyai_ctx_loop(od->mcp->ctx);
	if (!el)
		return -1;
	rc = fyai_oauth_flow_start(od->mcp->ctx, el, &params,
				   mcp_oauth_authorize_done, od, &od->flow);
	if (rc)
		return -1;
	port = fyai_oauth_flow_port(od->flow);
	if (od->mcp->oauth_redirect_host)
		od->redirect = strdup(fy_sprintfa("http://%s:%u%s",
			od->mcp->oauth_redirect_host, port,
			od->mcp->oauth_redirect_path));
	else
		od->redirect = strdup(fy_sprintfa(
			"http://127.0.0.1:%u/mcp/oauth/callback", port));
	return od->redirect ? 0 : -1;
}

static int mcp_oauth_authorize_start(struct mcp_oauth_discovery *od)
{
	const char *lead;
	char *encoded_redirect;
	char *encoded_client;
	char *encoded_scopes;
	char *scope_param;
	char *encoded_access_type;
	char *encoded_prompt;
	char *encoded_resource;
	const char *formatted;
	int rc;

	if (!od->mcp->oauth_allow_browser)
		return -2;
	mcp_oauth_discovery_drop_http(od);
	rc = mcp_oauth_receiver_start(od);
	if (rc)
		return rc;
	encoded_redirect = curl_easy_escape(NULL, od->redirect, 0);
	encoded_client = curl_easy_escape(NULL, od->mcp->oauth_client_id, 0);
	encoded_resource = curl_easy_escape(NULL, od->mcp->oauth_resource, 0);
	scope_param = mcp_oauth_scope_param(od->mcp);
	encoded_scopes = scope_param && *scope_param ?
		curl_easy_escape(NULL, scope_param, 0) : NULL;
	encoded_access_type = od->mcp->oauth_access_type &&
		*od->mcp->oauth_access_type ?
		curl_easy_escape(NULL, od->mcp->oauth_access_type, 0) : NULL;
	encoded_prompt = od->mcp->oauth_prompt && *od->mcp->oauth_prompt ?
		curl_easy_escape(NULL, od->mcp->oauth_prompt, 0) : NULL;
	free(scope_param);
	if (!encoded_redirect || !encoded_client || !encoded_resource ||
	    (od->mcp->oauth_scopes && !encoded_scopes)) {
		curl_free(encoded_redirect);
		curl_free(encoded_client);
		curl_free(encoded_scopes);
		curl_free(encoded_access_type);
		curl_free(encoded_prompt);
		curl_free(encoded_resource);
		return -1;
	}
	formatted = fy_sprintfa(
		"%s?response_type=code&client_id=%s&redirect_uri=%s&"
		"code_challenge=%s&code_challenge_method=S256&state=%s"
		"&resource=%s%s%s%s%s%s%s",
		od->mcp->oauth_authorization_endpoint,
		encoded_client, encoded_redirect, od->pkce.challenge,
		od->pkce.state, encoded_resource,
		encoded_scopes ? "&scope=" : "",
		encoded_scopes ? encoded_scopes : "",
		encoded_access_type ? "&access_type=" : "",
		encoded_access_type ? encoded_access_type : "",
		encoded_prompt ? "&prompt=" : "",
		encoded_prompt ? encoded_prompt : "");
	od->url = strdup(formatted);
	curl_free(encoded_redirect);
	curl_free(encoded_client);
	curl_free(encoded_scopes);
	curl_free(encoded_access_type);
	curl_free(encoded_prompt);
	curl_free(encoded_resource);
	if (!od->url)
		return -1;
	lead = fy_sprintfa("Authorize MCP server '%s':", od->mcp->name);
	fyai_print_login_url(od->mcp->ctx,
			     lead ? lead : "Authorize the MCP server:",
			     "open the authorization page", od->url);
	fyai_oauth_open_browser(od->url);
	free(od->url);
	od->url = NULL;
	od->state = MCP_OD_AUTHORIZE;
	return 0;
}

static int mcp_oauth_refresh_start(struct fyai_mcp_ctx *mcp,
				    struct mcp_startup *su)
{
	struct mcp_oauth_discovery *od;
	char *encoded_refresh;
	char *encoded_secret;
	char *encoded_client;
	char *encoded_resource;
	const char *formatted;
	int rc;

	if (!mcp->oauth_refresh_token || !*mcp->oauth_refresh_token ||
	    !mcp->oauth_token_endpoint || !mcp->oauth_client_id)
		return -1;
	od = calloc(1, sizeof(*od));
	if (!od)
		return -1;
	od->mcp = mcp;
	od->startup = su;
	od->state = MCP_OD_REFRESH;
	encoded_refresh = NULL;
	encoded_secret = NULL;
	encoded_client = NULL;
	encoded_resource = NULL;
	od->url = strdup(mcp->oauth_token_endpoint);
	encoded_refresh = curl_easy_escape(NULL, mcp->oauth_refresh_token, 0);
	encoded_secret = mcp->oauth_token_auth_method == MCP_TOKEN_AUTH_POST ?
		curl_easy_escape(NULL, mcp->oauth_client_secret, 0) : NULL;
	encoded_client = curl_easy_escape(NULL, mcp->oauth_client_id, 0);
	encoded_resource = curl_easy_escape(NULL, mcp->oauth_resource, 0);
	if (!od->url || !encoded_refresh ||
	    !encoded_client || !encoded_resource ||
	    (mcp->oauth_token_auth_method == MCP_TOKEN_AUTH_POST &&
	     !encoded_secret))
		goto err_out;
	formatted = fy_sprintfa(
		"grant_type=refresh_token&refresh_token=%s&client_id=%s&"
		"resource=%s%s%s",
		encoded_refresh, encoded_client, encoded_resource,
		encoded_secret ? "&client_secret=" : "",
		encoded_secret ? encoded_secret : "");
	od->body = strdup(formatted);
	curl_free(encoded_refresh);
	curl_free(encoded_secret);
	curl_free(encoded_client);
	curl_free(encoded_resource);
	encoded_refresh = NULL;
	encoded_secret = NULL;
	encoded_client = NULL;
	encoded_resource = NULL;
	if (!od->body)
		goto err_out;
	od->headers = curl_slist_append(od->headers,
		"Content-Type: application/x-www-form-urlencoded");
	if (!od->headers)
		goto err_out;
	mcp->oauth_discovery = od;
	rc = mcp_oauth_discovery_submit(od);
	if (rc)
		goto err_out;
	return 0;

err_out:
	curl_free(encoded_refresh);
	curl_free(encoded_secret);
	curl_free(encoded_client);
	curl_free(encoded_resource);
	mcp_oauth_discovery_destroy(od);
	return -1;
}

static enum fyai_event_action
mcp_oauth_refresh_timer(const struct fyai_event *ev)
{
	struct fyai_mcp_ctx *mcp;

	mcp = ev->userdata;
	mcp->oauth_refresh_timer = NULL;
	if (mcp_oauth_refresh_start(mcp, NULL)) {
		mcp->oauth_expires_at = time(NULL) + 60;
		mcp_oauth_refresh_schedule(mcp);
	}
	return FYAIEA_CONTINUE;
}

static void mcp_oauth_refresh_schedule(struct fyai_mcp_ctx *mcp)
{
	struct fyai_event_loop *el;
	time_t now;
	time_t due;
	fyai_event_ms_t delay;
	int rc;

	if (!mcp || !mcp->oauth_refresh_token ||
	    !*mcp->oauth_refresh_token || !mcp->oauth_expires_at)
		return;
	if (mcp->oauth_refresh_timer) {
		fyai_event_source_remove(mcp->oauth_refresh_timer);
		mcp->oauth_refresh_timer = NULL;
	}
	now = time(NULL);
	due = mcp->oauth_expires_at - 300;
	delay = due > now ? (fyai_event_ms_t)(due - now) * 1000 : 1;
	el = fyai_ctx_loop(mcp->ctx);
	if (!el)
		return;
	rc = fyai_event_add_timer(el, delay, 0, mcp_oauth_refresh_timer, mcp,
				  &mcp->oauth_refresh_timer);
	if (rc)
		fyai_warning(mcp->ctx,
			"MCP %s could not schedule OAuth refresh", mcp->name);
}

static bool mcp_oauth_token_complete(struct mcp_oauth_discovery *od,
					     fy_generic doc)
{
	struct fyai_mcp_ctx *mcp = od->mcp;
	const char *access_token;
	const char *refresh_token;

	if (od->state != MCP_OD_TOKEN && od->state != MCP_OD_REFRESH)
		return false;
	access_token = fy_get(doc, "access_token", "");
	refresh_token = fy_get(doc, "refresh_token", "");
	if (!*access_token) {
		mcp_oauth_discovery_fail(od,
			"token response has no access_token");
		return true;
	}
	free(mcp->oauth_access_token);
	mcp->oauth_access_token = strdup(access_token);
	if (*refresh_token) {
		free(mcp->oauth_refresh_token);
		mcp->oauth_refresh_token = strdup(refresh_token);
	}
	mcp->oauth_expires_at = time(NULL) +
		(time_t)fy_get(doc, "expires_in", 3600LL);
	mcp->oauth_force_refresh = false;
	mcp->oauth_force_login = false;
	if (!mcp->oauth_access_token ||
	    (*refresh_token && !mcp->oauth_refresh_token) ||
	    mcp_oauth_store_save(mcp)) {
		mcp_oauth_discovery_fail(od,
			"could not save OAuth tokens");
		return true;
	}
	if (od->flow) {
		fyai_oauth_flow_finish(od->flow, true);
		fyai_oauth_flow_destroy(od->flow);
		od->flow = NULL;
	}
	mcp_oauth_resume(od);
	return true;
}

static bool mcp_oauth_registration_complete(struct mcp_oauth_discovery *od,
						     fy_generic doc)
{
	struct fyai_mcp_ctx *mcp;
	const char *client_id;
	const char *client_secret;
	int rc;

	if (od->state != MCP_OD_REGISTER)
		return false;
	mcp = od->mcp;
	client_id = fy_get(doc, "client_id", "");
	client_secret = fy_get(doc, "client_secret", "");
	if (!*client_id) {
		mcp_oauth_discovery_fail(od,
			"registration response has no client_id");
		return true;
	}
	free(mcp->oauth_client_id);
	mcp_oauth_secret_free(&mcp->oauth_client_secret);
	free(mcp->oauth_access_token);
	free(mcp->oauth_refresh_token);
	mcp->oauth_client_id = strdup(client_id);
	mcp->oauth_client_secret = *client_secret ?
		strdup(client_secret) : NULL;
	if (!mcp->oauth_client_id ||
	    (*client_secret && !mcp->oauth_client_secret)) {
		mcp_oauth_discovery_fail(od,
			"could not retain registered client");
		return true;
	}
	rc = mcp_oauth_store_save(mcp);
	if (rc) {
		mcp_oauth_discovery_fail(od,
			"could not save registered client");
		return true;
	}
	rc = mcp_oauth_authorize_start(od);
	if (rc)
		mcp_oauth_discovery_fail(od,
			rc == -2 ?
			"browser login needs auth.allow_browser=true" :
			"could not start OAuth authorization");
	return true;
}

/* Save resource metadata and continue with authorization-server metadata. */
static void mcp_oauth_resource_complete(struct mcp_oauth_discovery *od,
					fy_generic doc)
{
	struct fyai_mcp_ctx *mcp;
	fy_generic issuers;
	fy_generic issuer_value;
	const char *issuer;
	const char *resource;
	int rc;

	mcp = od->mcp;
	issuers = fy_get(doc, "authorization_servers", fy_invalid);
	if (!fy_is_sequence(issuers) || !fy_len(issuers)) {
		mcp_oauth_discovery_fail(od,
			"resource metadata has no authorization server");
		return;
	}
	issuer_value = fy_get_at(issuers, 0);
	issuer = fy_castp(&issuer_value, "");
	resource = fy_get(doc, "resource", mcp->endpoint);
	if (!*resource) {
		mcp_oauth_discovery_fail(od,
			"resource metadata has no resource identifier");
		return;
	}
	free(mcp->oauth_issuer);
	free(mcp->oauth_resource);
	mcp->oauth_issuer = strdup(issuer);
	mcp->oauth_resource = strdup(resource);
	mcp_oauth_discovery_drop_http(od);
	od->url = mcp_oauth_metadata_url(issuer);
	if (!mcp->oauth_issuer || !mcp->oauth_resource || !od->url) {
		mcp_oauth_discovery_fail(od,
			"could not retain authorization server");
		return;
	}
	od->state = MCP_OD_AUTHORIZATION_SERVER;
	rc = mcp_oauth_discovery_submit(od);
	if (rc)
		mcp_oauth_discovery_fail(od,
			"could not submit authorization metadata");
}

static void mcp_oauth_discovery_complete(
		struct fyai_curl_transfer *transfer, void *userdata)
{
	struct mcp_oauth_discovery *od;
	struct fyai_mcp_ctx *mcp;
	struct mcp_startup *su;
	struct fy_generic_builder *gb;
	fy_generic doc;
	const char *authorize;
	const char *token;
	const char *registration;
	const char *body;
	const char *message;
	CURLcode code;
	long status;
	int rc;

	od = userdata;
	mcp = od->mcp;
	code = fyai_curl_collect(transfer);
	status = 0;
	curl_easy_getinfo(od->curl, CURLINFO_RESPONSE_CODE, &status);
	od->transfer = NULL;
	fyai_curl_transfer_destroy(transfer);
	if (code != CURLE_OK || status < 200 || status >= 300) {
		message = fy_sprintfa("metadata request failed: %s (HTTP %ld)",
				     curl_easy_strerror(code), status);
		mcp_oauth_discovery_fail(od, message);
		return;
	}
	gb = fyai_ctx_transient_gb(mcp->ctx);
	doc = parse_json_string(gb, od->response.data ?
				od->response.data : "");
	if (!fy_is_mapping(doc)) {
		mcp_oauth_discovery_fail(od, "metadata is not a JSON object");
		return;
	}
	if (od->state == MCP_OD_RESOURCE) {
		mcp_oauth_resource_complete(od, doc);
		return;
	}
	if (mcp_oauth_token_complete(od, doc))
		return;
	if (mcp_oauth_registration_complete(od, doc))
		return;
	authorize = fy_get(doc, "authorization_endpoint", "");
	token = fy_get(doc, "token_endpoint", "");
	registration = fy_get(doc, "registration_endpoint", "");
	if (!*authorize || !*token) {
		mcp_oauth_discovery_fail(od,
			"authorization metadata lacks required endpoints");
		return;
	}
	free(mcp->oauth_authorization_endpoint);
	free(mcp->oauth_token_endpoint);
	free(mcp->oauth_registration_endpoint);
	mcp->oauth_authorization_endpoint = strdup(authorize);
	mcp->oauth_token_endpoint = strdup(token);
	mcp->oauth_registration_endpoint = *registration ?
		strdup(registration) : NULL;
	if (!mcp->oauth_authorization_endpoint || !mcp->oauth_token_endpoint) {
		mcp_oauth_discovery_fail(od, "could not retain OAuth endpoints");
		return;
	}
	rc = mcp_oauth_store_load(mcp);
	if (rc) {
		mcp_oauth_discovery_fail(od,
			"could not load stored OAuth credentials");
		return;
	}
	if (mcp->oauth_force_login) {
		free(mcp->oauth_access_token);
		free(mcp->oauth_refresh_token);
		mcp->oauth_access_token = NULL;
		mcp->oauth_refresh_token = NULL;
		mcp->oauth_expires_at = 0;
	}
	if (mcp->oauth_client_id && *mcp->oauth_client_id) {
		if (!mcp->oauth_force_refresh &&
		    mcp->oauth_access_token &&
		    *mcp->oauth_access_token &&
		    (!mcp->oauth_expires_at ||
		     mcp->oauth_expires_at > time(NULL) + 30)) {
			mcp_oauth_resume(od);
			return;
		}
		if (mcp->oauth_refresh_token &&
		    *mcp->oauth_refresh_token) {
			su = od->startup;
			mcp_oauth_discovery_destroy(od);
			rc = mcp_oauth_refresh_start(mcp, su);
			if (rc)
				mcp_startup_settle(su, MCP_SRV_FAILED);
			return;
		}
		rc = mcp_oauth_authorize_start(od);
		if (rc)
			mcp_oauth_discovery_fail(od,
				rc == -2 ?
				"browser login needs auth.allow_browser=true" :
				"could not start OAuth authorization");
		return;
	}
	if (!mcp->oauth_registration_endpoint) {
		mcp_oauth_discovery_fail(od,
			"authorization server does not support registration");
		return;
	}
	rc = mcp_oauth_receiver_start(od);
	if (rc) {
		mcp_oauth_discovery_fail(od,
			"could not start OAuth redirect receiver");
		return;
	}
	body = emit_json_string(gb, fy_mapping(gb,
		"client_name", "fyai",
		"redirect_uris", fy_sequence(gb, od->redirect),
		"grant_types", fy_sequence(gb, "authorization_code",
					   "refresh_token"),
		"response_types", fy_sequence(gb, "code"),
		"token_endpoint_auth_method", "none"));
	if (!body) {
		mcp_oauth_discovery_fail(od,
			"could not build client registration");
		return;
	}
	mcp_oauth_discovery_drop_http(od);
	od->url = strdup(mcp->oauth_registration_endpoint);
	od->body = strdup(body);
	if (!od->url || !od->body) {
		mcp_oauth_discovery_fail(od,
			"could not retain client registration");
		return;
	}
	od->headers = curl_slist_append(od->headers,
					"Content-Type: application/json");
	if (!od->headers) {
		mcp_oauth_discovery_fail(od,
			"could not build client registration headers");
		return;
	}
	od->state = MCP_OD_REGISTER;
	rc = mcp_oauth_discovery_submit(od);
	if (rc)
		mcp_oauth_discovery_fail(od,
			"could not submit client registration");
}

static int mcp_oauth_discovery_submit(struct mcp_oauth_discovery *od)
{
	struct fyai_ctx *ctx;

	ctx = od->mcp->ctx;
	od->curl = curl_easy_init();
	/* Reserve SIGALRM for the watchdog. */
	curl_easy_setopt(od->curl, CURLOPT_NOSIGNAL, 1L);

	fyai_error_check(ctx, od->curl, err_out,
			 "could not create OAuth discovery transfer");
	curl_easy_setopt(od->curl, CURLOPT_URL, od->url);
	curl_easy_setopt(od->curl, CURLOPT_WRITEFUNCTION, write_response);
	curl_easy_setopt(od->curl, CURLOPT_WRITEDATA, &od->response);
	curl_easy_setopt(od->curl, CURLOPT_TIMEOUT, (long)od->mcp->timeout);
	curl_easy_setopt(od->curl, CURLOPT_USERAGENT, ctx->user_agent);
	if ((od->state == MCP_OD_TOKEN || od->state == MCP_OD_REFRESH) &&
	    od->mcp->oauth_token_auth_method == MCP_TOKEN_AUTH_BASIC) {
		curl_easy_setopt(od->curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
		curl_easy_setopt(od->curl, CURLOPT_USERNAME,
				 od->mcp->oauth_client_id);
		curl_easy_setopt(od->curl, CURLOPT_PASSWORD,
				 od->mcp->oauth_client_secret);
	}
	if (od->body) {
		curl_easy_setopt(od->curl, CURLOPT_POSTFIELDS, od->body);
		curl_easy_setopt(od->curl, CURLOPT_HTTPHEADER, od->headers);
	}
	od->transfer = fyai_curl_submit(ctx, od->curl,
					mcp_oauth_discovery_complete, od);
	fyai_error_check(ctx, od->transfer, err_out,
			 "could not submit OAuth discovery transfer");
	return 0;

err_out:
	return -1;
}

static int mcp_oauth_discovery_start_for(struct fyai_mcp_ctx *mcp,
					  struct mcp_startup *su)
{
	struct mcp_oauth_discovery *od;
	int rc;

	od = calloc(1, sizeof(*od));
	fyai_error_check(mcp->ctx, od, err_out,
			 "could not allocate OAuth discovery");
	od->mcp = mcp;
	od->startup = su;
	od->state = MCP_OD_RESOURCE;
	od->url = strdup(mcp->oauth_resource_metadata);
	fyai_error_check(mcp->ctx, od->url, err_free,
			 "could not retain OAuth resource metadata URL");
	mcp->oauth_discovery = od;
	rc = mcp_oauth_discovery_submit(od);
	fyai_error_check(mcp->ctx, !rc, err_free,
			 "could not start OAuth discovery");
	return 0;

err_free:
	mcp_oauth_discovery_destroy(od);
err_out:
	return -1;
}

static int mcp_oauth_discovery_start(struct mcp_startup *su)
{
	return mcp_oauth_discovery_start_for(su->mcp, su);
}

/*
 * A protected tools/call may return 401 even though Google allowed the
 * unauthenticated initialize and tools/list requests. Start OAuth recovery
 * directly in that case. Once mcp_oauth_resume() installs the replacement
 * token it advances this parked startup through initialize, initialized, and
 * tools/list; callers waiting on MCP_SRV_CONNECTING then retry their call.
 */
static int mcp_oauth_recovery_begin(struct fyai_mcp_ctx *mcp)
{
	int rc;

	mcp->state = MCP_SRV_CONNECTING;
	mcp->recovering = true;
	rc = mcp_oauth_discovery_start_for(mcp, NULL);
	fyai_error_check(mcp->ctx, !rc, err_out,
			 "could not start MCP OAuth recovery");
	return 0;

err_out:
	mcp->recovering = false;
	mcp->state = MCP_SRV_FAILED;
	return -1;
}

static enum fyai_event_action mcp_startup_retry(const struct fyai_event *ev)
{
	struct mcp_startup *su = ev->userdata;

	su->timer = NULL;
	mcp_startup_step(su);
	return FYAIEA_CONTINUE;
}

/* Bad auth, an unretryable status, or any stdio error is terminal at this stage
 * (mid-session reconnect is a later step); transient statuses retry. */
static bool mcp_startup_terminal(struct fyai_mcp_ctx *mcp, long status,
				 CURLcode code)
{
	if (mcp->transport == MCP_TRANSPORT_STDIO)
		return true;
	if (status == 401 || status == 403)
		return true;
	return !mcp_transient_status(code, status);
}

static void mcp_startup_collect(struct fyai_mcp_ctx *mcp, fy_generic result)
{
	struct fyai_ctx *ctx = mcp->ctx;
	fy_generic tools = fy_get(result, "tools", fy_seq_empty), tool;
	const char *tool_name;

	fy_foreach(tool, tools) {
		tool_name = fy_get(tool, "name", "");
		if (!*tool_name)
			continue;
		mcp->tools = fy_append(ctx->gb, mcp->tools, fy_mapping(ctx->gb,
			"type", "function", "function", fy_mapping(
				"name", fy_sprintfa(MCP_PREFIX "%s__%s",
						    mcp->name, tool_name),
				"description", fy_get(tool, "description", ""),
				"parameters", fy_get(tool, "inputSchema",
						fy_mapping("type", "object")))));
	}
}

static void mcp_startup_req_done(struct jsonrpc_request *req, void *userdata)
{
	struct mcp_startup *su = userdata;
	struct fyai_mcp_ctx *mcp = su->mcp;
	struct fyai_ctx *ctx = mcp->ctx;
	bool ok = jsonrpc_request_ok(req);
	fy_generic result = jsonrpc_request_result(req);
	long status = jsonrpc_request_http_status(req);
	CURLcode code = jsonrpc_request_curl_code(req);
	struct fyai_event_loop *el;
	const char *protocol, *cursor;
	const char *challenge;
	const char *response_body;

	challenge = jsonrpc_request_response_header(req, "WWW-Authenticate");
	response_body = jsonrpc_request_response_body(req);
	if (status == 401 && challenge) {
		free(mcp->oauth_resource_metadata);
		mcp->oauth_resource_metadata = mcp_auth_param(
			challenge, "resource_metadata");
	}
	if (!ok) {
		if (response_body && *response_body)
			mcp_set_error(mcp, "%s failed (HTTP %ld): %.512s",
				      su->phase == MCP_SU_INIT ? "initialize" :
				      su->phase == MCP_SU_NOTIFY ?
				      "initialized" : "tools/list",
				      status, response_body);
		else
			mcp_set_error(mcp, "%s failed: %s (HTTP %ld)",
				      su->phase == MCP_SU_INIT ? "initialize" :
				      su->phase == MCP_SU_NOTIFY ?
				      "initialized" : "tools/list",
				      curl_easy_strerror(code), status);
	}
	jsonrpc_request_destroy(req);
	su->req = NULL;

	if (!ok) {
		if (status == 401 && mcp->auth_mode == MCP_AUTH_OAUTH &&
		    mcp->oauth_resource_metadata) {
			/*
			 * JSON-RPC quite correctly diagnosed the unsuccessful
			 * response. OAuth is now recovering from that 401, so
			 * release its error latch. Any discovery failure below
			 * becomes the visible cause instead of debug unwind.
			 */
			fyai_diag_reset(&ctx->cfg->diag);
			mcp->oauth_force_refresh = mcp->auth_token &&
				*mcp->auth_token;
			if (mcp->oauth_refresh_timer) {
				fyai_event_source_remove(
					mcp->oauth_refresh_timer);
				mcp->oauth_refresh_timer = NULL;
			}
			if (!mcp_oauth_discovery_start(su))
				return;
			mcp_startup_settle(su, MCP_SRV_FAILED);
			return;
		}
		if (!mcp_startup_terminal(mcp, status, code) &&
		    su->attempt + 1 < MCP_STARTUP_RETRIES) {
			su->attempt++;
			el = fyai_ctx_loop(ctx);
			if (el && !fyai_event_add_timer(el, 100 << su->attempt,
					0, mcp_startup_retry, su, &su->timer))
				return;
		}
		mcp_startup_settle(su, MCP_SRV_FAILED);
		return;
	}
	switch (su->phase) {
	case MCP_SU_INIT:
		protocol = fy_get(result, "protocolVersion", "");
		if (*protocol)
			mcp->protocol_version =
				fy_gb_intern_string(ctx->cfg->gb, protocol);
		mcp_startup_enter(su, MCP_SU_NOTIFY);
		break;
	case MCP_SU_NOTIFY:
		mcp_startup_enter(su, MCP_SU_DISCOVER);
		break;
	case MCP_SU_DISCOVER:
		mcp_startup_collect(mcp, result);
		cursor = fy_get(result, "nextCursor", "");
		free(su->cursor);
		su->cursor = *cursor ? strdup(cursor) : NULL;
		if (su->cursor)
			mcp_startup_enter(su, MCP_SU_DISCOVER);
		else
			mcp_startup_settle(su, MCP_SRV_READY);
		break;
	}
}

/* (Re)submit the request for the current phase. The id is fixed on phase entry
 * and reused here so a retry replays with the same id, as the protocol
 * wants.
 */
static void mcp_startup_step(struct mcp_startup *su)
{
	struct fyai_mcp_ctx *mcp = su->mcp;
	struct fyai_ctx *ctx = mcp->ctx;
	struct fy_generic_builder *gb;
	const char *method;
	fy_generic params;
	bool notification = false;

	gb = fyai_ctx_transient_gb(ctx);
	if (!gb) {
		fyai_error(ctx, "%s could not acquire transient storage",
			   mcp->name);
		mcp_startup_settle(su, MCP_SRV_FAILED);
		return;
	}

	switch (su->phase) {
	case MCP_SU_INIT:
		method = "initialize";
		params = fy_mapping(gb,
			"protocolVersion", mcp->protocol_version,
			"capabilities", fy_map_empty,
			"clientInfo", fy_mapping("name", "fyai",
						 "version", VERSION));
		break;
	case MCP_SU_NOTIFY:
		method = "notifications/initialized";
		notification = true;
		params = fy_map_empty;
		break;
	case MCP_SU_DISCOVER:
		method = "tools/list";
		params = su->cursor ? fy_mapping(gb,
				"cursor", su->cursor) : fy_map_empty;
		break;
	default:
		mcp_startup_settle(su, MCP_SRV_FAILED);
		return;
	}
	su->req = jsonrpc_request_submit(mcp->conn, method, params, su->id,
			notification, mcp_startup_req_done, su);
	if (!su->req)
		mcp_startup_settle(su, MCP_SRV_FAILED);
}

/* Advance to @phase with a fresh retry budget and, for a request phase, a fresh
 * id (a notification keeps id 0), then submit it. */
static void mcp_startup_enter(struct mcp_startup *su,
			      enum mcp_startup_phase phase)
{
	su->phase = phase;
	su->attempt = 0;
	su->id = phase == MCP_SU_NOTIFY ? 0 :
		jsonrpc_conn_next_id(su->mcp->conn);
	mcp_startup_step(su);
}

/* Begin a created server's asynchronous startup. */
static void mcp_startup_begin(struct fyai_mcp_ctx *mcp)
{
	struct mcp_startup *su;

	su = calloc(1, sizeof(*su));
	if (!su) {
		mcp->state = MCP_SRV_FAILED;
		return;
	}
	su->mcp = mcp;
	mcp->startup = su;
	mcp_startup_enter(su, MCP_SU_INIT);
}

static int mcp_stdio_spawn(struct fyai_ctx *ctx, struct fyai_mcp_ctx *mcp,
			   fy_generic config)
{
	int inpipe[2] = { -1, -1 }, outpipe[2] = { -1, -1 };
	fy_generic args, env;
	fy_generic item, key;
	const char *command, *cwd, *name, *value;
	char **argv;
	pid_t pid;
	size_t i, argc;
	int rc;

	args = fy_get(config, "args", fy_seq_empty);
	env = fy_get(config, "env", fy_invalid);
	command = fy_get(config, "command", "");
	cwd = fy_get(config, "cwd", "");
	fyai_error_check(ctx, fy_is_sequence(args), err_out,
			 "MCP stdio server '%s' args must be a sequence", mcp->name);
	fyai_error_check(ctx, fy_is_invalid(env) ||
			 fy_is_mapping(env), err_out,
			 "MCP stdio server '%s' env must be a mapping", mcp->name);

	argc = fy_len(args);
	fyai_error_check(ctx, *command, err_out,
			 "MCP stdio server '%s' has no command", mcp->name);

	argv = alloca((argc + 2) * sizeof(*argv));

	i = 0;
	argv[i++] = (char *)command;
	fy_foreach(item, args)
		argv[i++] = fy_castp(&item, "");
	argv[i] = NULL;

	rc = pipe(inpipe);
	if (!rc)
		rc = pipe(outpipe);
	fyai_error_check(ctx, !rc, err_out,
			 "MCP stdio server '%s' could not create pipes: %s",
			 mcp->name, strerror(errno));
	rc = fcntl(inpipe[0], F_SETFD, FD_CLOEXEC);
	if (!rc)
		rc = fcntl(inpipe[1], F_SETFD, FD_CLOEXEC);
	if (!rc)
		rc = fcntl(outpipe[0], F_SETFD, FD_CLOEXEC);
	if (!rc)
		rc = fcntl(outpipe[1], F_SETFD, FD_CLOEXEC);
	fyai_error_check(ctx, !rc, err_out,
			 "MCP stdio server '%s' could not protect pipes: %s",
			 mcp->name, strerror(errno));

	pid = fork();
	fyai_error_check(ctx, pid >= 0, err_out,
			 "MCP stdio server '%s' could not fork: %s",
			 mcp->name, strerror(errno));

	if (!pid) {
		fyai_ctx_loop_abandon(ctx);
		signal(SIGPIPE, SIG_DFL);
		close(inpipe[1]);
		close(outpipe[0]);
		inpipe[1] = outpipe[0] = -1;

		if (dup2(inpipe[0], STDIN_FILENO) < 0 ||
		    dup2(outpipe[1], STDOUT_FILENO) < 0)
			_exit(126);

		close(inpipe[0]);
		close(outpipe[1]);
		inpipe[0] = outpipe[1] = -1;

		if (*cwd && chdir(cwd))
			_exit(126);

		if (fy_is_mapping(env)) {
			fy_foreach(key, env) {
				name = fy_castp(&key, "");
				value = fy_get(env, key, "");

				if (setenv(name, value, 1))
					_exit(126);
			}
		}
		execvp(command, argv);
		_exit(127);
	}
	close(inpipe[0]);
	close(outpipe[1]);
	inpipe[0] = outpipe[1] = -1;

	mcp->pid = pid;
	mcp->stdin_fd = inpipe[1];
	mcp->stdout_fd = outpipe[0];
	return 0;

err_out:
	for (i = 0; i < ARRAY_SIZE(inpipe); i++) {
		if (inpipe[i] >= 0)
			close(inpipe[i]);
	}
	for (i = 0; i < ARRAY_SIZE(outpipe); i++) {
		if (outpipe[i] >= 0)
			close(outpipe[i]);
	}
	return -1;
}

static int mcp_create_server(struct fyai_ctx *ctx, const char *server_name,
			     fy_generic config)
{
	struct fyai_mcp_ctx *mcp = NULL, **tail, *prev;
	enum mcp_auth_mode auth_mode;
	fy_generic auth_ref, auth;
	const char *endpoint, *protocol, *token, *transport, *command;
	bool enabled;
	int rc;

	enabled = fy_get(config, "enabled", true);
	if (!enabled)
		return 0;

	fyai_error_check(ctx, mcp_server_name_valid(server_name), err_out,
			 "invalid MCP server name '%s'", server_name);
	command = fy_get(config, "command", "");
	transport = fy_get(config, "transport", *command ? "stdio" :
						       "streamable-http");
	fyai_error_check(ctx, !strcmp(transport, "stdio") ||
			 !strcmp(transport, "streamable-http") ||
			 !strcmp(transport, "http"), err_out,
			 "MCP server '%s' has unknown transport '%s'",
			 server_name, transport);
	endpoint = fy_get(config, "endpoint", "");
	fyai_error_check(ctx, !strcmp(transport, "stdio") || *endpoint, err_out,
			 "MCP HTTP server '%s' has no endpoint", server_name);
	rc = mcp_server_auth_mode(ctx, config, &auth_mode);
	fyai_error_check(ctx, !rc, err_out,
			 "MCP server '%s' has invalid auth configuration",
			 server_name);
	fyai_error_check(ctx, strcmp(transport, "stdio") ||
			 auth_mode == MCP_AUTH_BEARER, err_out,
			 "MCP stdio server '%s' cannot use OAuth",
			 server_name);
	protocol = fy_get(config, "protocol_version",
			  ctx->cfg->mcp_protocol_version);
	auth_ref = fy_get(config, "auth_token", fy_invalid);
	auth = fy_get(config, "auth", fy_invalid);
	token = !strcmp(transport, "stdio") ||
		auth_mode == MCP_AUTH_OAUTH ? NULL :
		mcp_server_auth_token(ctx, config);
	fyai_error_check(ctx, !strcmp(transport, "stdio") ||
			 auth_mode == MCP_AUTH_OAUTH ||
			 fy_is_invalid(auth_ref) || token ||
			 fy_equal(fy_get(auth_ref, "type"), "auto"), err_out,
			 "MCP server '%s' could not resolve auth token", server_name);

	mcp = calloc(1, sizeof(*mcp));
	fyai_error_check(ctx, mcp, err_out,
			 "MCP server '%s' could not allocate context", server_name);
	mcp->ctx = ctx;
	mcp->tools = fy_seq_empty;
	mcp->state = MCP_SRV_CONNECTING;
	mcp->pid = -1;
	mcp->stdin_fd = -1;
	mcp->stdout_fd = -1;
	mcp->transport = !strcmp(transport, "stdio") ? MCP_TRANSPORT_STDIO :
		MCP_TRANSPORT_HTTP;
	mcp->auth_mode = auth_mode;
	mcp->oauth_allow_browser = auth_mode == MCP_AUTH_OAUTH &&
		fy_get(auth, "allow_browser", false);
	mcp->name = fy_gb_intern_string(ctx->cfg->gb, server_name);
	if (*endpoint)
		mcp->endpoint = fy_gb_intern_string(ctx->cfg->gb, endpoint);
	mcp->protocol_version = fy_gb_intern_string(ctx->cfg->gb,
						 protocol);
	if (token)
		mcp->auth_token = fy_gb_intern_string(ctx->cfg->gb, token);
	rc = auth_mode == MCP_AUTH_OAUTH ?
		mcp_oauth_configure_client(ctx, mcp, auth) : 0;
	fyai_error_check(ctx, !rc, err_out,
			 "MCP server '%s' has invalid OAuth client",
			 server_name);
	mcp->timeout = fy_get(config, "timeout", ctx->cfg->mcp_timeout);
	if (mcp->timeout <= 0)
		mcp->timeout = ctx->cfg->mcp_timeout;

	if (mcp->transport == MCP_TRANSPORT_HTTP)
		mcp->curl = curl_easy_init();
	/* Reserve SIGALRM for the watchdog. */
	curl_easy_setopt(mcp->curl, CURLOPT_NOSIGNAL, 1L);


	fyai_error_check(ctx, mcp->name && mcp->protocol_version &&
			 (!token || mcp->auth_token) &&
			 (mcp->transport != MCP_TRANSPORT_HTTP ||
			  (mcp->endpoint && mcp->curl)), err_out,
			 "MCP server '%s' could not initialize context", server_name);

	rc = mcp->transport == MCP_TRANSPORT_STDIO ?
		mcp_stdio_spawn(ctx, mcp, config) : 0;
	fyai_error_check(ctx, !rc, err_out,
			 "MCP stdio server '%s' could not start", server_name);

	if (mcp->transport == MCP_TRANSPORT_STDIO) {
		mcp->conn = jsonrpc_conn_stdio(ctx, mcp->stdin_fd,
				mcp->stdout_fd, mcp->timeout, mcp->name, "mcp");
	} else {
		struct jsonrpc_http_hooks hooks = {
			.userdata = mcp,
			.add_headers = mcp_http_add_headers,
			.response_header = mcp_http_response_header,
		};
		mcp->conn = jsonrpc_conn_http(ctx, mcp->endpoint, mcp->timeout,
				mcp->name, "mcp", &hooks);
	}
	fyai_error_check(ctx, mcp->conn, err_out,
			 "MCP server '%s' could not create connection",
			 server_name);

	tail = &ctx->mcp;
	while (*tail)
		tail = &(*tail)->next;
	*tail = mcp;

	if (ctx->cfg->mcp_logging)
		(void)fyai_log_generic(ctx, "mcp", fy_mapping(
			"event", "connect", "server", mcp->name,
			"transport", mcp->transport == MCP_TRANSPORT_STDIO ?
				"stdio" : "streamable-http"));

	return 0;

err_out:
	if (!mcp)
		return -1;
	if (ctx->mcp == mcp)
		ctx->mcp = mcp->next;
	else {
		for (prev = ctx->mcp; prev && prev->next != mcp;
		     prev = prev->next)
			;
		if (prev)
			prev->next = mcp->next;
	}
	jsonrpc_conn_destroy(mcp->conn);
	if (mcp->curl)
		curl_easy_cleanup(mcp->curl);
	mcp_stdio_stop(ctx, mcp);
	mcp_oauth_discovery_destroy(mcp->oauth_discovery);
	free(mcp->oauth_resource_metadata);
	free(mcp->oauth_resource);
	free(mcp->oauth_issuer);
	free(mcp->oauth_authorization_endpoint);
	free(mcp->oauth_token_endpoint);
	free(mcp->oauth_registration_endpoint);
	free(mcp->oauth_client_id);
	mcp_oauth_secret_free(&mcp->oauth_client_secret);
	free(mcp->oauth_access_token);
	free(mcp->oauth_refresh_token);
	free(mcp->oauth_scopes);
	free(mcp->oauth_redirect_uri);
	free(mcp->oauth_redirect_host);
	free(mcp->oauth_redirect_path);
	free(mcp->oauth_access_type);
	free(mcp->oauth_prompt);
	free(mcp);
	return -1;
}

/* True once every configured server has settled (READY or FAILED). */
bool fyai_mcp_settled(struct fyai_ctx *ctx)
{
	struct fyai_mcp_ctx *mcp;

	for (mcp = ctx->mcp; mcp; mcp = mcp->next)
		if (mcp->state == MCP_SRV_CONNECTING)
			return false;
	return true;
}

/* Fold every ready server's discovered tools into ctx->mcp_tools, in
 * configuration order (ctx->mcp is built in that order). */
void fyai_mcp_publish_tools(struct fyai_ctx *ctx)
{
	struct fyai_mcp_ctx *mcp;
	fy_generic out = fy_seq_empty;

	for (mcp = ctx->mcp; mcp; mcp = mcp->next)
		if (mcp->state == MCP_SRV_READY &&
		    fy_is_valid(mcp->tools))
			out = fy_concat(ctx->gb, out, mcp->tools);
	ctx->mcp_tools = fy_gb_internalize(ctx->gb, out);
}

static struct fyai_mcp_ctx *
mcp_find_server(struct fyai_ctx *ctx, const char *name)
{
	struct fyai_mcp_ctx *mcp;

	for (mcp = ctx->mcp; mcp; mcp = mcp->next)
		if (!strcmp(mcp->name, name))
			return mcp;
	return NULL;
}

/*
 * Explicit control commands supersede a browser wait, refresh, or discovery
 * started for the endpoint. In particular, an authorization server can reject
 * the redirect before calling our loopback receiver; without this cancellation
 * escape hatch the receiver would keep the endpoint busy until its timeout.
 */
static void mcp_oauth_operation_cancel(struct fyai_mcp_ctx *mcp)
{
	if (!mcp->oauth_discovery)
		return;
	mcp_oauth_discovery_destroy(mcp->oauth_discovery);
	mcp->recovering = false;
	mcp->oauth_force_login = false;
	mcp->oauth_force_refresh = false;
	mcp->state = MCP_SRV_FAILED;
}

int fyai_mcp_status(struct fyai_ctx *ctx)
{
	struct fyai_mcp_ctx *mcp;
	struct fy_generic_builder *gb;
	fy_generic rows;
	fy_generic opts;
	const char *state;
	const char *oauth;
	long long expires_in;

	if (!ctx->mcp)
		return 1;
	gb = fyai_ctx_transient_gb(ctx);
	fyai_error_check(ctx, gb, err_out,
			 "could not build MCP status");
	rows = fy_seq_empty;
	for (mcp = ctx->mcp; mcp; mcp = mcp->next) {
		state = mcp->state == MCP_SRV_READY ? "ready" :
			mcp->state == MCP_SRV_CONNECTING ?
			"connecting" : "failed";
		if (mcp->auth_mode != MCP_AUTH_OAUTH)
			oauth = mcp->auth_token ? "bearer" : "none";
		else if (mcp->oauth_discovery)
			oauth = "authorizing";
		else if (mcp->oauth_access_token)
			oauth = "authenticated";
		else
			oauth = "logged-out";
		expires_in = 0;
		if (mcp->auth_mode == MCP_AUTH_OAUTH &&
		    mcp->oauth_expires_at)
			expires_in = (long long)mcp->oauth_expires_at -
				(long long)time(NULL);
		rows = fy_append(gb, rows, fy_null_filtered_mapping(gb,
			"server", mcp->name,
			"state", state,
			"transport", mcp->transport == MCP_TRANSPORT_STDIO ?
				"stdio" : "http",
			"auth", oauth,
			"tools", (long long)(
				fy_is_valid(mcp->tools) ?
				fy_len(mcp->tools) : 0),
			"expires", expires_in ?
				fy_stringf(gb, "%llds", expires_in) : fy_null,
			"endpoint", mcp->endpoint ?
				fy_value(gb, mcp->endpoint) : fy_null,
			"error", mcp->last_error ?
				fy_value(gb, mcp->last_error) : fy_null));
	}
	opts = fy_mapping(gb,
		"title", "MCP endpoints",
		"keys", fy_sequence(gb, "server", "state", "transport",
				    "auth", "tools", "expires", "endpoint",
				    "error"),
		"columns", fy_mapping(gb,
			"server", fy_mapping(gb, "name", "Server"),
			"state", fy_mapping(gb, "name", "State"),
			"transport", fy_mapping(gb, "name", "Transport"),
			"auth", fy_mapping(gb, "name", "Auth"),
			"tools", fy_mapping(gb, "name", "Tools"),
			"expires", fy_mapping(gb, "name", "Expires"),
			"endpoint", fy_mapping(gb, "name", "Endpoint"),
			"error", fy_mapping(gb, "name", "Last error")));
	return fyai_generic_to_markdown(ctx, opts, rows);

err_out:
	return -1;
}

int fyai_mcp_login(struct fyai_ctx *ctx, const char *name)
{
	struct fyai_mcp_ctx *mcp;
	char *resource;
	int rc;

	mcp = mcp_find_server(ctx, name);
	fyai_error_check(ctx, mcp, err_out,
			 "MCP server '%s' is not active", name);
	fyai_error_check(ctx, mcp->transport == MCP_TRANSPORT_HTTP &&
			 mcp->auth_mode == MCP_AUTH_OAUTH, err_out,
			 "MCP server '%s' does not use HTTP OAuth", name);
	fyai_error_check(ctx, mcp->oauth_allow_browser, err_out,
			 "MCP server '%s' needs auth.allow_browser=true",
			 name);
	fyai_error_check(ctx, !mcp->startup, err_out,
			 "MCP server '%s' is starting", name);
	mcp_oauth_operation_cancel(mcp);
	if (!mcp->oauth_resource_metadata) {
		resource = mcp_oauth_resource_url(mcp->endpoint);
		fyai_error_check(ctx, resource, err_out,
				 "could not derive OAuth metadata for '%s'",
				 name);
		mcp->oauth_resource_metadata = resource;
	}
	mcp->last_error = NULL;
	mcp->oauth_force_login = true;
	rc = mcp_oauth_recovery_begin(mcp);
	fyai_error_check(ctx, !rc, err_out,
			 "could not start OAuth login for '%s'", name);
	fyai_result(ctx, "mcp: login started for %s\n", name);
	return 0;

err_out:
	return -1;
}

int fyai_mcp_logout(struct fyai_ctx *ctx, const char *name)
{
	struct fyai_mcp_ctx *mcp;
	const char *store_name;
	int lockfd;
	int rc;

	mcp = mcp_find_server(ctx, name);
	fyai_error_check(ctx, mcp, err_out,
			 "MCP server '%s' is not active", name);
	fyai_error_check(ctx, mcp->auth_mode == MCP_AUTH_OAUTH, err_out,
			 "MCP server '%s' does not use OAuth", name);
	fyai_error_check(ctx, !mcp->startup, err_out,
			 "MCP server '%s' is starting", name);
	mcp_oauth_operation_cancel(mcp);
	store_name = fy_sprintfa("mcp-auth-%s.json", mcp->name);
	lockfd = fyai_auth_store_lock(ctx, true);
	fyai_error_check(ctx, lockfd >= 0, err_out,
			 "could not lock OAuth store");
	rc = fyai_auth_store_delete(ctx, store_name);
	fyai_auth_store_unlock(lockfd);
	fyai_error_check(ctx, !rc, err_out,
			 "could not remove OAuth tokens for '%s'", name);
	if (mcp->oauth_refresh_timer) {
		fyai_event_source_remove(mcp->oauth_refresh_timer);
		mcp->oauth_refresh_timer = NULL;
	}
	mcp_oauth_secret_free(&mcp->oauth_access_token);
	mcp_oauth_secret_free(&mcp->oauth_refresh_token);
	/*
	 * A dynamically registered client is bound to the exact ephemeral
	 * loopback redirect used during registration. The next login opens a new
	 * receiver, so it must register a new client for that redirect instead
	 * of reusing the now-stale in-memory client ID. Configured clients use a
	 * stable redirect and remain intact.
	 */
	if (!mcp->oauth_client_configured) {
		mcp_oauth_secret_free(&mcp->oauth_client_id);
		mcp_oauth_secret_free(&mcp->oauth_client_secret);
	}
	mcp->oauth_expires_at = 0;
	mcp->auth_token = NULL;
	mcp->oauth_force_login = false;
	mcp->oauth_force_refresh = false;
	fyai_result(ctx, "mcp: logged out %s\n", name);
	return 0;

err_out:
	return -1;
}

/*
 * Create and connect every configured server, then submit its startup
 * handshake. The servers initialize concurrently; a server that cannot even be
 * created is warned and skipped (degrade gracefully). This does not wait -
 * callers observe fyai_mcp_settled() and then fyai_mcp_publish_tools().
 */
int fyai_mcp_start(struct fyai_ctx *ctx)
{
	fy_generic servers = ctx->cfg->mcp_servers;
	struct fy_generic_builder *gb;
	fy_generic key, config;
	struct fyai_mcp_ctx *mcp;
	const char *name;

	fyai_mcp_cleanup(ctx);
	ctx->mcp_tools = fy_invalid;

	if (fy_is_mapping(servers) && fy_len(servers)) {
		fy_foreach(key, servers) {
			name = fy_castp(&key, "");
			config = fy_get(servers, key, fy_invalid);
			if (!fy_is_mapping(config)) {
				fyai_warning(ctx, "MCP server '%s' configuration "
					     "is not a mapping; skipped", name);
				continue;
			}
			(void)mcp_create_server(ctx, name, config);
		}
	} else if (ctx->cfg->mcp_endpoint && *ctx->cfg->mcp_endpoint) {
		gb = fyai_ctx_transient_gb(ctx);
		if (!gb) {
			fyai_error(ctx, "could not acquire transient storage");
			return -1;
		}
		config = fy_mapping(gb,
			"endpoint", ctx->cfg->mcp_endpoint,
			"protocol_version", ctx->cfg->mcp_protocol_version,
			"timeout", ctx->cfg->mcp_timeout);
		(void)mcp_create_server(ctx, "default", config);
	} else {
		fyai_error(ctx, "MCP is enabled but no servers are configured");
		return -1;
	}

	for (mcp = ctx->mcp; mcp; mcp = mcp->next)
		mcp_startup_begin(mcp);
	return 0;
}

/*
 * Synchronous compatibility wrapper: start every server, pump the shared loop
 * until all have settled, then publish their tools. A live catalogue is reused.
 */
int fyai_mcp_refresh(struct fyai_ctx *ctx)
{
	struct fyai_event_loop *el;

	if (ctx->mcp && fy_is_valid(ctx->mcp_tools)) {
		if (ctx->cfg->mcp_logging)
			(void)fyai_log_generic(ctx, "mcp", fy_mapping(
				"event", "reuse", "tools",
				(long long)fy_len(ctx->mcp_tools)));
		return 0;
	}
	if (fyai_mcp_start(ctx))
		return -1;
	el = fyai_ctx_loop(ctx);
	while (el && !fyai_mcp_settled(ctx)) {
		if (fyai_event_loop_step(el, -1) < 0)
			break;
	}
	fyai_mcp_publish_tools(ctx);
	return 0;
}

enum fyai_mcp_call_state {
	FYAIMCS_WAITING,		/* target server not yet READY; parked */
	FYAIMCS_PRIMARY,
	FYAIMCS_RECOVER_INITIALIZE,
	FYAIMCS_RECOVER_NOTIFY,
	FYAIMCS_COMPLETED,
	FYAIMCS_CANCELLED,
	FYAIMCS_FAILED,
};

/* Poll interval while a call is parked waiting for its server to settle. */
#define FYAI_MCP_CALL_WAIT_TICK_MS 50

struct fyai_mcp_call_request {
	struct fyai_ctx *ctx;
	struct fyai_mcp_ctx *mcp;
	struct jsonrpc_request *req;
	struct fyai_event_source *wait_timer;	/* armed while FYAIMCS_WAITING */
	int wait_remaining_ms;			/* park budget left */
	fyai_mcp_call_complete_fn complete;
	void *userdata;
	char *tool_name;
	char *params_text;
	long long id;			/* the tools/call id, reused across retry */
	fy_generic result;
	enum fyai_mcp_call_state state;
	bool result_ok;
	bool cancel_requested;
	bool auth_retry_done;
};

static bool
fyai_mcp_call_state_final(enum fyai_mcp_call_state state)
{
	return state == FYAIMCS_COMPLETED ||
	       state == FYAIMCS_CANCELLED ||
	       state == FYAIMCS_FAILED;
}

static struct fyai_mcp_ctx *
mcp_find_tool_server(struct fyai_ctx *ctx, const char *name,
		     const char **tool_namep)
{
	struct fyai_mcp_ctx *mcp;
	const char *server_name;
	const char *sep;
	size_t server_len;

	server_name = name + sizeof(MCP_PREFIX) - 1;
	sep = strstr(server_name, "__");
	if (!sep)
		return NULL;
	server_len = (size_t)(sep - server_name);
	for (mcp = ctx->mcp; mcp; mcp = mcp->next)
		if (strlen(mcp->name) == server_len &&
		    !strncmp(mcp->name, server_name, server_len))
			break;
	if (mcp && tool_namep)
		*tool_namep = sep + 2;
	return mcp;
}

static fy_generic
mcp_tool_output(struct fyai_ctx *ctx, fy_generic result)
{
	fy_generic content;
	fy_generic item;
	fy_generic output;
	const char *text;

	content = fy_get(result, "content", fy_seq_empty);
	output = fy_seq_empty;
	fy_foreach(item, content) {
		text = fy_get(item, "text", "");
		if (*text)
			output = fy_append(ctx->transient_gb, output, text);
	}
	if (fy_len(output) == 1)
		return fy_get(output, 0);
	if (fy_len(output))
		return output;
	return fy_gb_internalize(ctx->transient_gb, result);
}

static void
fyai_mcp_call_finish(struct fyai_mcp_call_request *request,
		     enum fyai_mcp_call_state state,
		     fy_generic result, bool ok)
{
	request->state = state;
	request->result = result;
	request->result_ok = ok;
	if (request->complete)
		request->complete(request, request->userdata);
}

static void fyai_mcp_call_req_done(struct jsonrpc_request *req, void *userdata);

/* Submit the request for the current call phase. */
static int
fyai_mcp_call_submit_phase(struct fyai_mcp_call_request *request)
{
	struct fyai_ctx *ctx = request->ctx;
	const char *method;
	fy_generic params;
	long long id;
	bool notification = false;

	switch (request->state) {
	case FYAIMCS_PRIMARY:
		method = "tools/call";
		id = request->id;
		params = parse_json_string(ctx->transient_gb,
					   request->params_text);
		break;
	case FYAIMCS_RECOVER_INITIALIZE:
		method = "initialize";
		id = jsonrpc_conn_next_id(request->mcp->conn);
		params = fy_mapping(ctx->transient_gb,
			"protocolVersion", request->mcp->protocol_version,
			"capabilities", fy_map_empty,
			"clientInfo", fy_mapping("name", "fyai",
						 "version", VERSION));
		break;
	case FYAIMCS_RECOVER_NOTIFY:
		method = "notifications/initialized";
		id = 0;
		notification = true;
		params = fy_map_empty;
		break;
	default:
		return -1;
	}
	fyai_error_check(ctx, fy_is_valid(params), err_out,
			 "MCP call parameters are invalid");
	request->req = jsonrpc_request_submit(request->mcp->conn, method,
					      params, id, notification,
					      fyai_mcp_call_req_done, request);
	fyai_error_check(ctx, request->req, err_out,
			 "MCP %s could not submit %s", request->mcp->name,
			 method);
	return 0;

err_out:
	return -1;
}

static void fyai_mcp_call_wait_disarm(struct fyai_mcp_call_request *request)
{
	if (request->wait_timer) {
		fyai_event_source_remove(request->wait_timer);
		request->wait_timer = NULL;
	}
}

/*
 * Poll a parked call's target server. A terminally failed server fails the call
 * fast; a server that becomes READY admits it to the PRIMARY phase; a server
 * still connecting keeps the call parked until it settles or the budget runs
 * out. Runs off the poll timer, so completion never re-enters the submit path.
 */
static enum fyai_event_action
fyai_mcp_call_wait_tick(const struct fyai_event *ev)
{
	struct fyai_mcp_call_request *request = ev->userdata;
	struct fyai_ctx *ctx = request->ctx;
	char msg[160];

	if (request->cancel_requested) {
		fyai_mcp_call_wait_disarm(request);
		fyai_mcp_call_finish(request, FYAIMCS_CANCELLED,
			fy_value(ctx->transient_gb, "tool error: interrupted"),
			false);
		return FYAIEA_CONTINUE;
	}
	switch (request->mcp->state) {
	case MCP_SRV_READY:
		fyai_mcp_call_wait_disarm(request);
		request->state = FYAIMCS_PRIMARY;
		if (fyai_mcp_call_submit_phase(request))
			fyai_mcp_call_finish(request, FYAIMCS_FAILED,
				fy_value(ctx->transient_gb,
					 "tool error: MCP call failed"),
				false);
		break;
	case MCP_SRV_FAILED:
		fyai_mcp_call_wait_disarm(request);
		snprintf(msg, sizeof(msg),
			 "tool error: MCP server '%s' is unavailable",
			 request->mcp->name);
		fyai_mcp_call_finish(request, FYAIMCS_FAILED,
				     fy_value(ctx->transient_gb, msg), false);
		break;
	case MCP_SRV_CONNECTING:
		request->wait_remaining_ms -= FYAI_MCP_CALL_WAIT_TICK_MS;
		if (request->wait_remaining_ms <= 0) {
			fyai_mcp_call_wait_disarm(request);
			snprintf(msg, sizeof(msg), "tool error: MCP server '%s' "
				 "did not become ready", request->mcp->name);
			fyai_mcp_call_finish(request, FYAIMCS_FAILED,
					     fy_value(ctx->transient_gb, msg),
					     false);
		}
		break;
	}
	return FYAIEA_CONTINUE;
}

/* Start a call when its server is ready. Poll all other server states. */
static int fyai_mcp_call_gate(struct fyai_mcp_call_request *request)
{
	struct fyai_ctx *ctx = request->ctx;
	struct fyai_event_loop *el;
	int rc;

	if (request->mcp->state == MCP_SRV_READY) {
		request->state = FYAIMCS_PRIMARY;
		return fyai_mcp_call_submit_phase(request);
	}
	el = fyai_ctx_loop(ctx);
	fyai_error_check(ctx, el, err_out,
			 "could not acquire the application event loop");
	request->state = FYAIMCS_WAITING;
	request->wait_remaining_ms = request->mcp->timeout > 0 ?
		request->mcp->timeout * 1000 : 30000;
	rc = fyai_event_add_timer(el, 0, FYAI_MCP_CALL_WAIT_TICK_MS,
				  fyai_mcp_call_wait_tick, request,
				  &request->wait_timer);
	fyai_error_check(ctx, !rc, err_out,
			 "could not park MCP call for '%s'", request->mcp->name);
	return 0;

err_out:
	return -1;
}

/*
 * One request phase completed. Advance the tool-call state machine: deliver the
 * tool output, walk the session-recovery handshake, or fail. HTTP 404 with a
 * live session triggers reinitialize-then-retry; stdio never 404s, so it only
 * ever runs the PRIMARY phase.
 */
static void fyai_mcp_call_req_done(struct jsonrpc_request *req, void *userdata)
{
	struct fyai_mcp_call_request *request = userdata;
	struct fyai_ctx *ctx = request->ctx;
	fy_generic result = jsonrpc_request_result(req);
	bool ok = jsonrpc_request_ok(req);
	long status = jsonrpc_request_http_status(req);
	CURLcode code = jsonrpc_request_curl_code(req);
	const char *protocol;
	const char *challenge;
	const char *response_body;
	const char *tool_error;
	bool result_error;

	challenge = jsonrpc_request_response_header(req, "WWW-Authenticate");
	response_body = jsonrpc_request_response_body(req);
	if (status == 401 && challenge) {
		free(request->mcp->oauth_resource_metadata);
		request->mcp->oauth_resource_metadata = mcp_auth_param(
			challenge, "resource_metadata");
	}
	if (!ok) {
		if (response_body && *response_body)
			mcp_set_error(request->mcp,
				      "tools/call failed (HTTP %ld): %.512s",
				      status, response_body);
		else if (status || code != CURLE_OK)
			mcp_set_error(request->mcp,
				      "tools/call failed: %s (HTTP %ld)",
				      curl_easy_strerror(code), status);
		else
			mcp_set_error(request->mcp,
				      "tools/call failed: %s transport error",
				      request->mcp->transport ==
				      MCP_TRANSPORT_HTTP ? "http" : "stdio");
	}
	jsonrpc_request_destroy(req);
	request->req = NULL;

	if (request->cancel_requested) {
		fyai_mcp_call_finish(request, FYAIMCS_CANCELLED,
			fy_value(ctx->transient_gb, "tool error: interrupted"),
			false);
		return;
	}
	if (ok) {
		switch (request->state) {
		case FYAIMCS_PRIMARY:
			result_error = fy_get(result, "isError", false);
			result = mcp_tool_output(ctx, result);
			fyai_mcp_call_finish(request,
				result_error ? FYAIMCS_FAILED :
				FYAIMCS_COMPLETED, result, !result_error);
			return;
		case FYAIMCS_RECOVER_INITIALIZE:
			protocol = fy_get(result, "protocolVersion", "");
			if (*protocol) {
				protocol = fy_gb_intern_string(ctx->cfg->gb,
							       protocol);
				if (!protocol)
					goto failed;
				request->mcp->protocol_version = protocol;
			}
			request->state = FYAIMCS_RECOVER_NOTIFY;
			if (!fyai_mcp_call_submit_phase(request))
				return;
			goto failed;
		case FYAIMCS_RECOVER_NOTIFY:
			request->state = FYAIMCS_PRIMARY;
			if (!fyai_mcp_call_submit_phase(request))
				return;
			goto failed;
		default:
			goto failed;
		}
	}
	if (request->state == FYAIMCS_PRIMARY && status == 401 &&
	    request->mcp->auth_mode == MCP_AUTH_OAUTH &&
	    request->mcp->oauth_resource_metadata &&
	    !request->auth_retry_done) {
		request->auth_retry_done = true;
		fyai_diag_reset(&ctx->cfg->diag);
		request->mcp->oauth_force_refresh = true;
		if (request->mcp->oauth_refresh_timer) {
			fyai_event_source_remove(
				request->mcp->oauth_refresh_timer);
			request->mcp->oauth_refresh_timer = NULL;
		}
		if (!request->mcp->startup) {
			code = mcp_oauth_recovery_begin(request->mcp);
			if (code)
				goto failed;
		}
		if (!fyai_mcp_call_gate(request))
			return;
	}
	if (request->state == FYAIMCS_PRIMARY && status == 404 &&
	    request->mcp->session_id) {
		request->mcp->session_id = NULL;
		request->state = FYAIMCS_RECOVER_INITIALIZE;
		if (!fyai_mcp_call_submit_phase(request))
			return;
	}
	if (request->mcp->transport == MCP_TRANSPORT_HTTP)
		fyai_error(ctx, "MCP %s failed: %s",
			   request->mcp->name,
			   request->mcp->last_error ?
			   request->mcp->last_error : "unknown error");
failed:
	tool_error = request->mcp->last_error ?
		fy_sprintfa("tool error: %s", request->mcp->last_error) :
		"tool error: MCP call failed";
	fyai_mcp_call_finish(request, FYAIMCS_FAILED,
		fy_value(ctx->transient_gb, tool_error),
		false);
}

struct fyai_mcp_call_request *
fyai_mcp_call_submit(struct fyai_ctx *ctx, const char *name,
		     fy_generic args, fyai_mcp_call_complete_fn complete,
		     void *userdata)
{
	struct fyai_mcp_call_request *request;
	struct fyai_mcp_ctx *mcp;
	fy_generic params;
	const char *params_text;
	const char *tool_name;
	int rc;

	tool_name = NULL;
	mcp = mcp_find_tool_server(ctx, name, &tool_name);
	if (!mcp)
		return NULL;
	params = fy_mapping(ctx->transient_gb,
			    "name", tool_name, "arguments", args);
	params_text = emit_json_string(ctx->transient_gb, params);
	fyai_error_check(ctx, params_text, err_out,
			 "could not encode MCP call parameters");
	request = calloc(1, sizeof(*request));
	fyai_error_check(ctx, request, err_out, "out of memory");
	request->ctx = ctx;
	request->mcp = mcp;
	request->complete = complete;
	request->userdata = userdata;
	request->tool_name = strdup(tool_name);
	request->params_text = strdup(params_text);
	request->id = jsonrpc_conn_next_id(mcp->conn);
	request->result = fy_invalid;
	request->state = FYAIMCS_PRIMARY;
	fyai_error_check(ctx, request->tool_name && request->params_text,
			 err_free, "out of memory");
	if (ctx->cfg->mcp_logging)
		(void)fyai_log_generic(ctx, "mcp", fy_mapping(
			"event", "tool_call", "server", mcp->name,
			"tool", tool_name));
	rc = fyai_mcp_call_gate(request);
	fyai_error_check(ctx, !rc, err_free,
			 "could not submit MCP call");
	return request;

err_free:
	fyai_mcp_call_destroy(request);
err_out:
	return NULL;
}

void fyai_mcp_call_cancel(struct fyai_mcp_call_request *request)
{
	if (!request || fyai_mcp_call_state_final(request->state))
		return;
	request->cancel_requested = true;
	if (request->req)
		jsonrpc_request_cancel(request->req);
}

bool fyai_mcp_call_done(const struct fyai_mcp_call_request *request)
{
	return request && fyai_mcp_call_state_final(request->state);
}

fy_generic fyai_mcp_call_collect(struct fyai_mcp_call_request *request,
				 bool *okp)
{
	if (!fyai_mcp_call_done(request) || !okp)
		return fy_invalid;
	*okp = request->result_ok;
	return request->result;
}

void fyai_mcp_call_destroy(struct fyai_mcp_call_request *request)
{
	if (!request)
		return;
	fyai_mcp_call_wait_disarm(request);
	jsonrpc_request_destroy(request->req);
	free(request->tool_name);
	free(request->params_text);
	free(request);
}

static void fyai_mcp_call_sync_done(struct fyai_mcp_call_request *request,
				    void *userdata)
{
	(void)request;
	*(volatile bool *)userdata = true;
}

/* Synchronous compatibility wrapper over the asynchronous call operation:
 * submit, pump the shared loop until it completes, collect. */
fy_generic fyai_mcp_call(struct fyai_ctx *ctx, const char *name,
			 fy_generic args)
{
	struct fyai_mcp_call_request *request;
	struct fyai_event_loop *el;
	volatile bool done = false;
	fy_generic result = fy_invalid;
	bool ok = false;
	int rc;

	request = fyai_mcp_call_submit(ctx, name, args,
				       fyai_mcp_call_sync_done, (void *)&done);
	if (!request)
		return fy_value(ctx->transient_gb, "tool error: MCP call failed");
	el = fyai_ctx_loop(ctx);
	while (el && !done) {
		rc = fyai_event_loop_step(el, -1);
		if (rc < 0) {
			fyai_mcp_call_cancel(request);
			break;
		}
	}
	if (fyai_mcp_call_done(request))
		result = fyai_mcp_call_collect(request, &ok);
	fyai_mcp_call_destroy(request);
	if (fy_is_invalid(result))
		return fy_value(ctx->transient_gb, "tool error: MCP call failed");
	return result;
}

static void mcp_delete_complete(struct fyai_curl_transfer *xfer, void *userdata)
{
	struct fyai_mcp_ctx *mcp = userdata;
	struct fyai_ctx *ctx = mcp->ctx;
	CURLcode rc = fyai_curl_collect(xfer);
	long status = 0;

	curl_easy_getinfo(mcp->curl, CURLINFO_RESPONSE_CODE, &status);
	fyai_curl_transfer_destroy(xfer);
	mcp->del_xfer = NULL;
	/* Teardown failure is nonfatal; the server will time the session out. */
	if (rc != CURLE_OK || status < 200 || status >= 300)
		fyai_warning(ctx, "MCP %s session delete failed: %s (HTTP %ld)",
			     mcp->name, curl_easy_strerror(rc), status);
	if (ctx->cfg->mcp_logging)
		(void)fyai_log_generic(ctx, "mcp", fy_mapping(
			"event", "session_delete", "server", mcp->name,
			"http_status", status, "curl_code", (long long)rc));
	mcp->session_id = NULL;
	mcp->del_done = true;
}

/* Submit the session DELETE. Returns true if a transfer is now in flight. */
static bool mcp_delete_session_begin(struct fyai_ctx *ctx,
				     struct fyai_mcp_ctx *mcp)
{
	char *auth = NULL, *session = NULL, *version = NULL;

	if (!mcp->curl || !mcp->session_id)
		return false;

	version = make_header("MCP-Protocol-Version: ", mcp->protocol_version);
	session = make_header("Mcp-Session-Id: ", mcp->session_id);
	if (version)
		append_header(&mcp->del_headers, version);
	if (session)
		append_header(&mcp->del_headers, session);
	if (mcp->auth_token && *mcp->auth_token) {
		auth = make_header("Authorization: Bearer ", mcp->auth_token);
		if (auth)
			append_header(&mcp->del_headers, auth);
	}
	free(version);
	free(session);
	free(auth);
	curl_easy_setopt(mcp->curl, CURLOPT_URL, mcp->endpoint);
	curl_easy_setopt(mcp->curl, CURLOPT_HTTPHEADER, mcp->del_headers);
	curl_easy_setopt(mcp->curl, CURLOPT_CUSTOMREQUEST, "DELETE");
	curl_easy_setopt(mcp->curl, CURLOPT_POSTFIELDS, "");
	curl_easy_setopt(mcp->curl, CURLOPT_POSTFIELDSIZE, 0L);
	curl_easy_setopt(mcp->curl, CURLOPT_WRITEFUNCTION, write_response);
	curl_easy_setopt(mcp->curl, CURLOPT_WRITEDATA, &mcp->del_resp);
	curl_easy_setopt(mcp->curl, CURLOPT_HEADERFUNCTION, NULL);
	curl_easy_setopt(mcp->curl, CURLOPT_HEADERDATA, NULL);
	curl_easy_setopt(mcp->curl, CURLOPT_TIMEOUT,
			 (long)(mcp->timeout < 5 ? mcp->timeout : 5));
	mcp->del_xfer = fyai_curl_submit(ctx, mcp->curl, mcp_delete_complete,
					 mcp);
	return mcp->del_xfer != NULL;
}

static enum fyai_event_action mcp_term_complete(const struct fyai_event *ev)
{
	struct fyai_mcp_ctx *mcp = ev->userdata;

	/* The one-shot child source is withdrawn by the event layer. */
	mcp->term_src = NULL;
	mcp->pid = -1;
	mcp->term_done = true;
	return FYAIEA_CONTINUE;
}

/* Begin a server's asynchronous teardown: session DELETE for HTTP, the
 * SIGTERM->SIGKILL child ladder for stdio. Both are optional; whichever is not
 * needed is marked done immediately. */
static void mcp_teardown_begin(struct fyai_ctx *ctx, struct fyai_mcp_ctx *mcp)
{
	struct fyai_event_loop *el = fyai_ctx_loop(ctx);
	int status;

	mcp->del_done = !mcp_delete_session_begin(ctx, mcp);

	if (mcp->stdin_fd >= 0) {
		close(mcp->stdin_fd);
		mcp->stdin_fd = -1;
	}
	if (mcp->stdout_fd >= 0) {
		close(mcp->stdout_fd);
		mcp->stdout_fd = -1;
	}
	mcp->term_done = true;
	if (mcp->pid <= 0)
		return;
	if (el && !fyai_event_add_child_terminate(el, mcp->pid, 1000, 500,
						  mcp_term_complete, mcp,
						  &mcp->term_src)) {
		mcp->term_done = false;
		return;
	}
	/*
	 * Registration failure is exceptional, but teardown must still remain
	 * nonblocking. Kill now and make one final attempt to register a plain
	 * child source whose only job is reaping.
	 */
	(void)kill(mcp->pid, SIGKILL);
	if (el && !fyai_event_add_child(el, mcp->pid, mcp_term_complete,
					mcp, &mcp->term_src)) {
		mcp->term_done = false;
		return;
	}
	fyai_warning(ctx, "MCP %s child could not be watched for shutdown",
		     mcp->name);
	while (waitpid(mcp->pid, &status, 0) < 0 && errno == EINTR)
		;
	mcp->pid = -1;
}

static bool mcp_teardown_settled(const struct fyai_ctx *ctx)
{
	const struct fyai_mcp_ctx *mcp;

	for (mcp = ctx->mcp; mcp; mcp = mcp->next)
		if (!mcp->del_done || !mcp->term_done)
			return false;
	return true;
}

/* Shut the server down: close its pipes so it sees EOF and leaves on its own,
 * then escalate through SIGTERM to SIGKILL if it does not. */
static void mcp_stdio_stop(struct fyai_ctx *ctx, struct fyai_mcp_ctx *mcp)
{
	struct fyai_event_loop *el;
	int status;

	if (mcp->stdin_fd >= 0) {
		close(mcp->stdin_fd);
		mcp->stdin_fd = -1;
	}
	if (mcp->stdout_fd >= 0) {
		close(mcp->stdout_fd);
		mcp->stdout_fd = -1;
	}
	if (mcp->pid <= 0)
		return;

	el = fyai_ctx_loop(ctx);
	if (!el || fyai_event_child_terminate(el, mcp->pid, 1000, 500, NULL)) {
		/* No loop, or it failed: the reap is still ours to do. */
		kill(mcp->pid, SIGKILL);
		while (waitpid(mcp->pid, &status, 0) < 0 && errno == EINTR)
			;
	}
	mcp->pid = -1;
}

int fyai_mcp_stop(struct fyai_ctx *ctx)
{
	struct fyai_mcp_ctx *mcp;

	if (!ctx || !ctx->mcp)
		return 0;
	if (ctx->mcp_stopping)
		return 0;
	ctx->mcp_stopping = true;

	/* Cancel any in-flight startup, then fire every server's teardown so
	 * they drain concurrently. */
	for (mcp = ctx->mcp; mcp; mcp = mcp->next) {
		if (mcp->oauth_refresh_timer) {
			fyai_event_source_remove(mcp->oauth_refresh_timer);
			mcp->oauth_refresh_timer = NULL;
		}
		mcp_oauth_discovery_destroy(mcp->oauth_discovery);
		if (mcp->startup) {
			struct mcp_startup *su = mcp->startup;

			if (su->timer)
				fyai_event_source_remove(su->timer);
			jsonrpc_request_destroy(su->req);
			free(su->cursor);
			free(su);
			mcp->startup = NULL;
		}
		mcp_teardown_begin(ctx, mcp);
	}
	return 0;
}

bool fyai_mcp_stop_done(const struct fyai_ctx *ctx)
{
	return !ctx || !ctx->mcp ||
	       (ctx->mcp_stopping &&
		mcp_teardown_settled(ctx));
}

void fyai_mcp_stop_finish(struct fyai_ctx *ctx)
{
	struct fyai_mcp_ctx *mcp, *next;

	if (!ctx || !fyai_mcp_stop_done(ctx))
		return;
	for (mcp = ctx->mcp; mcp; mcp = next) {
		next = mcp->next;
		if (ctx->cfg->mcp_logging)
			(void)fyai_log_generic(ctx, "mcp", fy_mapping(
				"event", "disconnect", "server", mcp->name));
		curl_slist_free_all(mcp->del_headers);
		free(mcp->del_resp.data);
		jsonrpc_conn_destroy(mcp->conn);
		if (mcp->curl)
			curl_easy_cleanup(mcp->curl);
		free(mcp->oauth_resource_metadata);
		free(mcp->oauth_resource);
		free(mcp->oauth_issuer);
		free(mcp->oauth_authorization_endpoint);
		free(mcp->oauth_token_endpoint);
		free(mcp->oauth_registration_endpoint);
		free(mcp->oauth_client_id);
		mcp_oauth_secret_free(&mcp->oauth_client_secret);
		free(mcp->oauth_access_token);
		free(mcp->oauth_refresh_token);
		free(mcp->oauth_scopes);
		free(mcp->oauth_redirect_uri);
		free(mcp->oauth_redirect_host);
		free(mcp->oauth_redirect_path);
		free(mcp->oauth_access_type);
		free(mcp->oauth_prompt);
		free(mcp);
	}
	ctx->mcp = NULL;
	ctx->mcp_stopping = false;
}

void fyai_mcp_cleanup(struct fyai_ctx *ctx)
{
	struct fyai_event_loop *el;
	int rc;

	if (!ctx || !ctx->mcp)
		return;
	if (fyai_mcp_stop(ctx))
		return;
	el = fyai_ctx_loop(ctx);
	while (el && !fyai_mcp_stop_done(ctx)) {
		rc = fyai_event_loop_step(el, -1);
		if (rc < 0)
			return;
	}
	fyai_mcp_stop_finish(ctx);
}
