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

#include <assert.h>
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
#include "fyai_app.h"
#include "fyai_mcp_oauth.h"
#include "fyai_oauth.h"
#include "fyai_render.h"
#include "fyai_sandbox.h"
#include "fyai_schema.h"
#include "fyai_secret.h"
#include "fyai_skill.h"
#include "fyai_tools.h"
#include "fyai_tool_registry.h"
#include "fyai_uimodel.h"
#include "fyai_waitq.h"
#include "utils.h"
#include "fyai_shell_capture.h"

#define MCP_PREFIX "mcp__"
#define MCP_DIRECT_TOOL_LIMIT 8
#define MCP_SEARCH_RESULT_LIMIT 8
/* The fyai/events extension: see doc/event-sources-sdd.md. */
#define MCP_EVENTS_CAP		"fyai/events"
#define MCP_EVENTS_VERSION	1LL
/* Events a source may send that fyai has not acknowledged yet. */
#define MCP_EVENTS_CREDITS	16
/* Ids of the last events, to drop a resend of one fyai already has. */
#define MCP_EVENTS_SEEN		256
#define MCP_EVENT_ID_MAX	128
#define MCP_EVENT_KIND_MAX	64
/* The last events of a source that a bound action can answer. */
#define MCP_EVENT_RECORDS	64

enum mcp_transport {
	MCP_TRANSPORT_HTTP,
	MCP_TRANSPORT_STDIO,
	MCP_TRANSPORT_TUNNEL,
};

static const char *mcp_transport_name(enum mcp_transport transport)
{
	return transport == MCP_TRANSPORT_STDIO ? "stdio" :
	       transport == MCP_TRANSPORT_TUNNEL ? "tunnel" : "http";
}

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
	struct fyai_mcp *owner;
	struct jsonrpc_conn *conn;
	struct mcp_startup *startup;
	struct mcp_oauth_discovery *oauth_discovery;
	/* The token refresh: its request, and the startup that waits for it. */
	struct fyai_mcp_oauth_refresh *oauth_refresh;
	struct mcp_startup *oauth_refresh_su;
	struct fyai_event_source *oauth_refresh_timer;
	fy_generic tools;
	bool resources_supported;
	/*
	 * The event source. The configuration asks for events, the server
	 * declared them, and fyai listed the kinds (name -> kind, in the arena
	 * builder). The cursor is the last that fyai acknowledged; the credits
	 * are what the server may still send. A relist after list_changed
	 * collects the kinds of its pages until the last one.
	 */
	bool events_wanted;
	bool events_supported;
	bool events_listed;
	fy_generic event_kinds;
	char *event_cursor;
	unsigned int event_credits;
	char *event_seen[MCP_EVENTS_SEEN];
	unsigned int event_seen_next;
	struct jsonrpc_request *event_relist;
	fy_generic event_relist_kinds;
	/* What the live view counts, and the last event, one line. */
	unsigned long event_received;	/* checked and queued */
	unsigned long event_delivered;	/* published and acknowledged */
	unsigned long event_rejected;
	unsigned long event_resent;	/* dropped as a resend */
	char *event_last;
	/*
	 * The tools as the server listed them, before fyai bound the actions
	 * of the kinds into them, and the last events that a bound action can
	 * answer, a ring.
	 */
	fy_generic tools_raw;
	struct mcp_event_record {
		char *id;
		char *kind;
		char *data;		/* JSON */
		bool answered;
	} event_records[MCP_EVENT_RECORDS];
	unsigned int event_record_next;
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
	const char *oauth_resource;
	const char *oauth_issuer;
	const char *oauth_authorization_endpoint;
	const char *oauth_token_endpoint;
	const char *oauth_registration_endpoint;
	const char *oauth_client_id;
	char *oauth_client_secret;
	char *oauth_access_token;
	char *oauth_refresh_token;
	/* The copy of the access token that auth_token names; see bearer_set. */
	char *oauth_bearer;
	const char *oauth_scopes;
	const char *oauth_redirect_uri;
	const char *oauth_redirect_host;
	const char *oauth_redirect_path;
	const char *oauth_access_type;
	const char *oauth_prompt;
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
	struct fyai_buf del_resp;
	struct fyai_event_source *term_src;
	bool del_done;
	bool term_done;
};

/* Keep @s for the life of the process, in the builder of the configuration. */
static const char *mcp_keep(struct fyai_mcp *mc, const char *s)
{
	const char *kept;

	if (!s)
		return NULL;
	kept = fy_gb_intern_string(mc->cfg->gb, s);
	fyai_diag_check(mc->diag, kept, err_out, "cannot keep '%s'", s);
	return kept;
err_out:
	return NULL;
}

/* The component that a connection stands on; many connections stand on one. */
static inline struct fyai_mcp *mcp_owner(const struct fyai_mcp_ctx *mcp)
{
	return mcp->owner;
}

static void mcp_oauth_secret_free(char **secretp)
{
	size_t len;

	if (!secretp || !*secretp)
		return;
	len = strlen(*secretp);
	fyai_secret_clear_and_free(secretp, &len);
}

struct fyai_mcp_oauth_refresh;

/*
 * The server goes away while a refresh runs: the refresh is stopped, and a
 * token request that is out finishes by itself and saves its answer.
 */
static void mcp_oauth_refresh_release(struct fyai_mcp_ctx *mcp)
{
	if (!mcp->oauth_refresh)
		return;
	fyai_mcp_oauth_refresh_cancel(mcp->oauth_refresh);
	/* The callback of a cancel does not clear what the owner holds. */
	if (mcp->oauth_refresh) {
		fyai_mcp_oauth_refresh_destroy(mcp->oauth_refresh);
		mcp->oauth_refresh = NULL;
		mcp->oauth_refresh_su = NULL;
	}
}

static void mcp_set_error(struct fyai_mcp_ctx *mcp, const char *fmt, ...)
{
	va_list ap;
	char *message;

	va_start(ap, fmt);
	message = fy_vsprintfa(fmt, ap);
	va_end(ap);
	mcp->last_error = fy_gb_intern_string(mcp_owner(mcp)->cfg->gb, message);
}

static void mcp_stdio_stop(struct fyai_mcp *mc, struct fyai_mcp_ctx *mcp);

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
	mcp->session_id = fy_gb_intern_string(mcp_owner(mcp)->cfg->gb,
			fy_sprintfa("%.*s", (int)(end - p), p));
}

bool fyai_mcp_tool_name(const char *name)
{
	return name && !strncmp(name, MCP_PREFIX, sizeof(MCP_PREFIX) - 1) &&
		strstr(name + sizeof(MCP_PREFIX) - 1, "__");
}

fy_generic fyai_mcp_tools(struct fyai_mcp *mc)
{
	return fy_is_valid(mc->tools) ? mc->tools : fy_seq_empty;
}

bool fyai_mcp_enabled(const struct fyai_mcp *mc)
{
	return mc->cfg->mcp_enabled || mc->invocation;
}

bool fyai_mcp_deferred(struct fyai_mcp *mc)
{
	return fyai_mcp_enabled(mc) &&
		fy_len(fyai_mcp_tools(mc)) > MCP_DIRECT_TOOL_LIMIT;
}

bool fyai_mcp_tool_active(struct fyai_mcp *mc, const char *name)
{
	fy_generic item;

	if (!fy_is_valid(mc->active_tools))
		return false;
	fy_foreach(item, mc->active_tools)
		if (fy_equal(item, name))
			return true;
	return false;
}

static bool mcp_contains_case(const char *haystack, const char *needle)
{
	size_t len = strlen(needle);

	for (; *haystack; haystack++)
		if (!strncasecmp(haystack, needle, len))
			return true;
	return false;
}

/* A search activates only the returned schemas for later model requests. */
static fy_generic fyai_mcp_tool_search(struct fyai_ctx *ctx, fy_generic args,
				       bool *okp)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	fy_generic query_v = fy_get(args, "query", fy_invalid);
	fy_generic tools = fyai_mcp_tools(ctx->mcp), tool, fn, out = fy_seq_empty;
	const char *query = fy_castp(&query_v, "");
	const char *name, *description;
	size_t count = 0;
	bool more = false;

	*okp = false;
	if (!*query)
		return fy_value(gb, "tool error: query is empty");
	if (strlen(query) > 128)
		return fy_value(gb, "tool error: query exceeds 128 bytes");
	fy_foreach(tool, tools) {
		fn = fy_get(tool, "function");
		name = fy_get(fn, "name", "");
		description = fy_get(fn, "description", "");
		if (!fyai_skill_tool_allowed(ctx, name))
			continue;
		if (!mcp_contains_case(name, query) &&
		    !mcp_contains_case(description, query))
			continue;
		if (count == MCP_SEARCH_RESULT_LIMIT) {
			more = true;
			break;
		}
		out = fy_append(gb, out, fy_mapping(gb,
			"name", name, "description",
			fy_stringf(gb, "%.256s", description)));
		if (!fyai_mcp_tool_active(ctx->mcp, name)) {
			if (!fy_is_valid(ctx->mcp->active_tools))
				ctx->mcp->active_tools = fy_seq_empty;
			ctx->mcp->active_tools = fy_append(ctx->arena->gb,
				ctx->mcp->active_tools, fy_value(ctx->arena->gb, name));
		}
		count++;
	}
	if (count && fyai_request_state_apply(ctx))
		return fy_value(gb, "tool error: could not activate MCP tools");
	*okp = true;
	return fy_mapping(gb, "tools", out,
		"more", more,
		"total", (long long)fy_len(tools));
}

static fy_generic mcp_resource_tool(struct fyai_ctx *ctx, fy_generic args,
				    bool *okp);

static fy_generic mcp_search_describe(struct fyai_ctx *ctx,
				     struct fy_generic_builder *gb,
				     fy_generic tool)
{
	(void)gb;
	return fyai_mcp_deferred(ctx->mcp) ? tool : fy_invalid;
}

static fy_generic mcp_resource_describe(struct fyai_ctx *ctx,
				       struct fy_generic_builder *gb,
				       fy_generic tool)
{
	struct fyai_mcp_ctx *mcp;

	(void)gb;
	for (mcp = ctx->mcp->conns; mcp; mcp = mcp->next)
		if (mcp->state == MCP_SRV_READY && mcp->resources_supported)
			return tool;
	return fy_invalid;
}

static void mcp_search_head(struct fyai_ctx *ctx, FILE *mf,
			    struct fy_generic_builder *gb, fy_generic args,
			    int preview_lines, struct fyai_md_blocks *blocks)
{
	(void)ctx;
	(void)gb;
	(void)preview_lines;
	(void)blocks;
	fprintf(mf, "**tool search** %s\n\n", fy_get(args, "query", ""));
}

static void mcp_resource_head(struct fyai_ctx *ctx, FILE *mf,
			      struct fy_generic_builder *gb, fy_generic args,
			      int preview_lines, struct fyai_md_blocks *blocks)
{
	(void)ctx;
	(void)gb;
	(void)preview_lines;
	(void)blocks;
	fprintf(mf, "**MCP resource** %s %s\n\n",
		fy_get(args, "server", ""),
		fy_get(args, "operation", ""));
}

const struct fyai_tool_def fyai_mcp_defs[] = {
	{ .name = "tool_search", .run = fyai_mcp_tool_search,
	  .head = mcp_search_head, .describe = mcp_search_describe,
	  .flags = FYAI_TOOL_PARENT },
	{ .name = "mcp_resource", .run = mcp_resource_tool,
	  .head = mcp_resource_head, .describe = mcp_resource_describe,
	  .flags = FYAI_TOOL_PARENT },
};
const size_t fyai_mcp_defs_count = ARRAY_SIZE(fyai_mcp_defs);

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

static const char *mcp_server_auth_token(struct fyai_mcp *mc,
					 fy_generic config)
{
	fy_generic ref = fy_get(config, "auth_token", fy_invalid);
	const char *type, *value, *token;

	if (fy_is_invalid(ref))
		return mc->cfg->mcp_auth_token;
	fyai_diag_check(mc->diag, fy_is_mapping(ref), err_out,
			 "MCP auth_token must be a secret indirection mapping");
	type = fy_get(ref, "type", "");
	if (!strcmp(type, "auto"))
		return NULL;
	fyai_diag_check(mc->diag, !strcmp(type, "env"), err_out,
			 "MCP auth_token has unsupported secret type '%s'", type);
	value = fy_get(ref, "value", "");
	token = *value ? getenv(value) : NULL;
	fyai_diag_check(mc->diag, token && *token, err_out,
			 "MCP auth_token environment variable '%s' is unset", value);
	return token;
err_out:
	return NULL;
}

static int mcp_server_auth_mode(struct fyai_mcp *mc, fy_generic config,
				enum mcp_auth_mode *modep)
{
	fy_generic auth;
	const char *type;

	auth = fy_get(config, "auth", fy_invalid);
	if (fy_is_invalid(auth)) {
		*modep = MCP_AUTH_BEARER;
		return 0;
	}
	fyai_diag_check(mc->diag, fy_is_mapping(auth), err_out,
			 "MCP auth must be a mapping");
	type = fy_get(auth, "type", "");
	fyai_diag_check(mc->diag, !strcmp(type, "bearer") ||
			 !strcmp(type, "oauth"), err_out,
			 "MCP auth has unsupported type '%s'", type);
	*modep = !strcmp(type, "oauth") ? MCP_AUTH_OAUTH : MCP_AUTH_BEARER;
	return 0;

err_out:
	return -1;
}

static char *mcp_oauth_secret(struct fyai_mcp *mc, fy_generic ref)
{
	const char *type;
	const char *name;
	const char *value;
	char *stored;
	char *secret;
	size_t secret_len;
	int rc;

	fyai_diag_check(mc->diag, fy_is_mapping(ref), err_out,
			 "MCP OAuth client secret must be an indirection");
	type = fy_get(ref, "type", "");
	name = fy_get(ref, "value", "");
	fyai_diag_check(mc->diag, *name, err_out,
			 "MCP OAuth client secret has no name");
	if (!strcmp(type, "env")) {
		value = getenv(name);
		fyai_diag_check(mc->diag, value && *value, err_out,
				 "MCP OAuth client secret environment "
				 "variable '%s' is unset", name);
		return strdup(value);
	}
	fyai_diag_check(mc->diag, !strcmp(type, "secret"), err_out,
			 "MCP OAuth client secret has unsupported type '%s'",
			 type);
	stored = strdup(fy_sprintfa("fyai:%s", name));
	fyai_diag_check(mc->diag, stored, err_out,
			 "could not build MCP OAuth secret name");
	secret = NULL;
	secret_len = 0;
	rc = fyai_secret_kernel_get(stored, &secret, &secret_len);
	free(stored);
	fyai_diag_check(mc->diag, rc == FYAI_SECRET_OK && secret && *secret,
			 err_out,
			 "MCP OAuth client secret '%s' is unavailable", name);
	return secret;

err_out:
	return NULL;
}

static int mcp_oauth_configure_client(struct fyai_mcp *mc,
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
	CURLUcode url_rc;

	client = fy_get(auth, "client", fy_invalid);
	if (fy_is_invalid(client))
		return 0;
	fyai_diag_check(mc->diag, fy_is_mapping(client), err_out,
			 "MCP OAuth client must be a mapping");
	mcp->oauth_client_id = mcp_keep(mc, fy_get(client, "id", ""));
	mcp->oauth_redirect_uri =
		mcp_keep(mc, fy_get(client, "redirect_uri", ""));
	fyai_diag_check(mc->diag, mcp->oauth_client_id &&
			 *mcp->oauth_client_id &&
			 mcp->oauth_redirect_uri &&
			 *mcp->oauth_redirect_uri, err_out,
			 "MCP OAuth client needs id and redirect_uri");
	url = curl_url();
	scheme = NULL;
	host = NULL;
	path = NULL;
	port = NULL;
	fyai_diag_check(mc->diag, url, err_out,
			 "could not parse MCP OAuth redirect");
	url_rc = curl_url_set(url, CURLUPART_URL, mcp->oauth_redirect_uri, 0);
	if (url_rc == CURLUE_OK)
		url_rc = curl_url_get(url, CURLUPART_SCHEME, &scheme, 0);
	if (url_rc == CURLUE_OK)
		url_rc = curl_url_get(url, CURLUPART_HOST, &host, 0);
	fyai_diag_check(mc->diag, url_rc == CURLUE_OK, err_url,
			 "invalid MCP OAuth redirect URI");
	fyai_diag_check(mc->diag, !strcmp(scheme, "http") &&
			 (!strcmp(host, "localhost") ||
			  !strcmp(host, "127.0.0.1")),
			 err_url,
			 "MCP OAuth redirect must use an HTTP loopback host");
	(void)curl_url_get(url, CURLUPART_PATH, &path, 0);
	(void)curl_url_get(url, CURLUPART_PORT, &port, 0);
	mcp->oauth_redirect_host = mcp_keep(mc, host);
	mcp->oauth_redirect_path =
		mcp_keep(mc, path && *path ? path : "/");
	fyai_diag_check(mc->diag, mcp->oauth_redirect_host &&
			 mcp->oauth_redirect_path, err_url,
			 "could not retain MCP OAuth redirect");
	if (port) {
		port_value = strtol(port, &endp, 10);
		fyai_diag_check(mc->diag, !*endp && port_value > 0 &&
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
			mcp_oauth_secret(mc, secret_ref);
		fyai_diag_check(mc->diag, mcp->oauth_client_secret, err_out,
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
		fyai_diag_check(mc->diag, false, err_out,
				 "unsupported MCP OAuth token authentication '%s'",
				 method);
	fyai_diag_check(mc->diag, mcp->oauth_token_auth_method ==
			 MCP_TOKEN_AUTH_NONE || mcp->oauth_client_secret,
			 err_out,
			 "MCP OAuth token authentication requires a secret");
	scopes = fy_get(auth, "scopes", fy_invalid);
	fyai_diag_check(mc->diag, fy_is_sequence(scopes) &&
			 fy_len(scopes), err_out,
			 "configured MCP OAuth clients require scopes");
	emitted = fy_seq_empty;
	fy_foreach(scope, scopes) {
		scope_text = fy_castp(&scope, "");
		fyai_diag_check(mc->diag, *scope_text, err_out,
				 "MCP OAuth scope must not be empty");
		emitted = fy_append(mc->cfg->gb, emitted, scope_text);
	}
	scope_text = emit_json_string(mc->cfg->gb, emitted);
	fyai_diag_check(mc->diag, scope_text, err_out,
			 "could not retain MCP OAuth scopes");
	/*
	 * The JSON sequence is durable and unambiguous for store comparison.
	 * The authorization URL converts it back to a space-separated list.
	 */
	mcp->oauth_scopes = scope_text;
	params = fy_get(auth, "authorization_params", fy_invalid);
	if (fy_is_mapping(params)) {
		mcp->oauth_access_type =
			mcp_keep(mc, fy_get(params, "access_type", ""));
		mcp->oauth_prompt =
			mcp_keep(mc, fy_get(params, "prompt", ""));
		fyai_diag_check(mc->diag, mcp->oauth_access_type &&
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
	MCP_SU_EVENTS,		/* fyai/events/list, when the server has events */
};

/* One server's asynchronous startup handshake: initialize, initialized,
 * paginated tools/list, then paginated fyai/events/list for an event source.
 * It settles the server READY or FAILED and drives the next step from each
 * request completion. */
struct mcp_startup {
	struct fyai_mcp_ctx *mcp;
	struct jsonrpc_request *req;
	struct fyai_event_source *timer;
	char *cursor;
	long long id;			/* current request id, reused across retry */
	enum mcp_startup_phase phase;
	int attempt;
};

/* Give the live view the state of the servers; defined with the rows. */
static void mcp_sources_publish(struct fyai_mcp *mc);
static void mcp_actions_apply(struct fyai_mcp_ctx *mcp);
static void mcp_startup_step(struct mcp_startup *su);
static void mcp_startup_enter(struct mcp_startup *su,
			      enum mcp_startup_phase phase);

static void mcp_startup_settle(struct mcp_startup *su,
			       enum mcp_server_state state)
{
	struct fyai_mcp_ctx *mcp = su->mcp;
	struct fyai_mcp *mc = mcp_owner(mcp);

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
	if (mc->cfg->mcp_logging)
		(void)fyai_log_generic(fyai_mcp_state_ctx(mc), "mcp", fy_mapping(
			"event", state == MCP_SRV_READY ? "discovery" : "failed",
			"server", mcp->name, "tools",
			(long long)(fy_is_valid(mcp->tools) ?
				    fy_len(mcp->tools) : 0)));
	if (state == MCP_SRV_FAILED)
		fyai_diag_type(mc->diag, FYAIET_WARNING, "MCP server '%s' is unavailable", mcp->name);
	/* The live view shows a server that starts or is down. */
	mcp_sources_publish(mc);
}

static const char *mcp_startup_phase_name(enum mcp_startup_phase phase)
{
	switch (phase) {
	case MCP_SU_INIT:
		return "initialize";
	case MCP_SU_NOTIFY:
		return "initialized";
	case MCP_SU_DISCOVER:
		return "tools/list";
	case MCP_SU_EVENTS:
		return "fyai/events/list";
	}
	return "startup";
}

/*
 * The event source: the fyai/events extension of MCP. A server that the
 * configuration lets send events declares the capability, lists its kinds,
 * and sends each event as notifications/fyai/event. fyai checks the event
 * against the schema of its kind and queues it for the model. When the turn
 * that takes it is published, fyai acknowledges it with the cursor that the
 * server gave and returns the credit. See doc/event-sources-sdd.md.
 */

static struct fyai_mcp_ctx *mcp_find_server(struct fyai_mcp *mc,
					    const char *name);

/* What fyai offers in initialize: events, to a server allowed to send them. */
static fy_generic mcp_client_capabilities(struct fyai_mcp_ctx *mcp,
					  struct fy_generic_builder *gb)
{
	fy_generic events;

	if (!mcp->events_wanted)
		return fy_map_empty;
	events = fy_mapping(gb, "version", MCP_EVENTS_VERSION,
			    "credits", (long long)MCP_EVENTS_CREDITS);
	if (mcp->event_cursor)
		events = fy_assoc(gb, events, "cursor",
				  (const char *)mcp->event_cursor);
	return fy_mapping(gb, "experimental",
			  fy_mapping(gb, MCP_EVENTS_CAP, events));
}

/* The answer to initialize: does the server send events, and in our version. */
static void mcp_events_negotiated(struct fyai_mcp_ctx *mcp, fy_generic result)
{
	struct fyai_mcp *mc = mcp_owner(mcp);
	fy_generic caps, events;
	long long version;

	mcp->events_supported = false;
	mcp->events_listed = false;
	mcp->event_kinds = fy_map_empty;
	if (!mcp->events_wanted)
		return;
	caps = fy_get(fy_get(result, "capabilities", fy_map_empty),
		      "experimental", fy_map_empty);
	events = fy_get(caps, MCP_EVENTS_CAP, fy_invalid);
	if (!fy_is_mapping(events)) {
		fyai_diag_type(mc->diag, FYAIET_WARNING,
			       "MCP server '%s' is configured for events, "
			       "but it does not declare %s", mcp->name,
			       MCP_EVENTS_CAP);
		return;
	}
	version = fy_get(events, "version", 0LL);
	if (version != MCP_EVENTS_VERSION) {
		fyai_diag_type(mc->diag, FYAIET_WARNING,
			       "MCP server '%s' speaks %s version %lld; fyai "
			       "speaks %lld, so it sends no events", mcp->name,
			       MCP_EVENTS_CAP, version, MCP_EVENTS_VERSION);
		return;
	}
	mcp->events_supported = true;
	mcp->event_credits = MCP_EVENTS_CREDITS;
}

/* A name of a kind or the id of an event: short, and safe to quote. */
static bool mcp_event_word_valid(const char *s, size_t max, const char *extra)
{
	size_t i;

	if (!s || !*s || strlen(s) > max)
		return false;
	for (i = 0; s[i]; i++)
		if (!isalnum((unsigned char)s[i]) && !strchr(extra, s[i]))
			return false;
	return true;
}

/* Add the kinds of one page of fyai/events/list to @kinds. */
static fy_generic mcp_events_collect(struct fyai_mcp_ctx *mcp,
				     fy_generic kinds, fy_generic result)
{
	struct fyai_mcp *mc = mcp_owner(mcp);
	fy_generic list, kind, schema, gname;
	const char *name;

	list = fy_get(result, "kinds", fy_seq_empty);
	fy_foreach(kind, list) {
		/* A short string lives in its generic: keep it in scope. */
		gname = fy_get(kind, "name", fy_invalid);
		name = fy_is_string(gname) ? fy_castp(&gname, "") : "";
		schema = fy_get(kind, "schema", fy_invalid);
		if (!mcp_event_word_valid(name, MCP_EVENT_KIND_MAX, "-_") ||
		    !(fy_is_mapping(schema) || fy_is_bool(schema))) {
			fyai_diag_type(mc->diag, FYAIET_WARNING,
				       "MCP server '%s' lists an event kind "
				       "'%.64s' without a valid name or schema; "
				       "it is ignored", mcp->name, name);
			continue;
		}
		kinds = fy_assoc(mc->arena->gb, kinds, name, kind);
	}
	return kinds;
}

static void mcp_events_relist_submit(struct fyai_mcp_ctx *mcp,
				     const char *cursor);

static void mcp_events_relist_done(struct jsonrpc_request *req,
				   void *userdata)
{
	struct fyai_mcp_ctx *mcp = userdata;
	struct fyai_mcp *mc = mcp_owner(mcp);
	fy_generic result = jsonrpc_request_result(req);
	char *cursor = NULL;
	bool ok = jsonrpc_request_ok(req);

	if (ok) {
		mcp->event_relist_kinds = mcp_events_collect(mcp,
			mcp->event_relist_kinds, result);
		cursor = strdup(fy_get(result, "nextCursor", ""));
	}
	jsonrpc_request_destroy(req);
	mcp->event_relist = NULL;
	if (ok && cursor && *cursor) {
		mcp_events_relist_submit(mcp, cursor);
		free(cursor);
		return;
	}
	free(cursor);
	if (ok) {
		mcp->event_kinds = mcp->event_relist_kinds;
		/* The actions follow the kinds. */
		mcp_actions_apply(mcp);
		fyai_mcp_publish_tools(fyai_mcp_state_ctx(mc));
	} else
		fyai_diag_type(mc->diag, FYAIET_WARNING,
			       "MCP server '%s' changed its event kinds, but "
			       "they could not be listed; the old ones stay",
			       mcp->name);
	mcp->event_relist_kinds = fy_invalid;
}

static void mcp_events_relist_submit(struct fyai_mcp_ctx *mcp,
				     const char *cursor)
{
	struct fyai_mcp *mc = mcp_owner(mcp);
	struct fy_generic_builder *gb = fyai_transient_get(mc->transient);
	fy_generic params;

	if (!gb)
		return;
	params = cursor ? fy_mapping(gb, "cursor", cursor) : fy_map_empty;
	mcp->event_relist = jsonrpc_request_submit(mcp->conn,
		"fyai/events/list", params, jsonrpc_conn_next_id(mcp->conn),
		false, mcp_events_relist_done, mcp);
}

/* The server changed its kinds: list them again, from the first page. */
static void mcp_events_relist(struct fyai_mcp_ctx *mcp)
{
	/* A relist in flight reads the kinds as they are when it reaches them. */
	if (mcp->event_relist || !mcp->events_listed)
		return;
	mcp->event_relist_kinds = fy_map_empty;
	mcp_events_relist_submit(mcp, NULL);
}

static bool mcp_event_seen(const struct fyai_mcp_ctx *mcp, const char *id)
{
	unsigned int i;

	for (i = 0; i < MCP_EVENTS_SEEN; i++)
		if (mcp->event_seen[i] && !strcmp(mcp->event_seen[i], id))
			return true;
	return false;
}

static void mcp_event_remember(struct fyai_mcp_ctx *mcp, const char *id)
{
	char **slot = &mcp->event_seen[mcp->event_seen_next++ %
				       MCP_EVENTS_SEEN];

	free(*slot);
	*slot = strdup(id);
}

/* Release what the event source holds, before the connection goes. */
static void mcp_events_release(struct fyai_mcp_ctx *mcp)
{
	unsigned int i;

	if (mcp->event_relist) {
		jsonrpc_request_destroy(mcp->event_relist);
		mcp->event_relist = NULL;
	}
	for (i = 0; i < MCP_EVENTS_SEEN; i++) {
		free(mcp->event_seen[i]);
		mcp->event_seen[i] = NULL;
	}
	free(mcp->event_cursor);
	mcp->event_cursor = NULL;
	free(mcp->event_last);
	mcp->event_last = NULL;
	for (i = 0; i < MCP_EVENT_RECORDS; i++) {
		free(mcp->event_records[i].id);
		free(mcp->event_records[i].kind);
		free(mcp->event_records[i].data);
		memset(&mcp->event_records[i], 0,
		       sizeof(mcp->event_records[i]));
	}
}

/* Tell the server that fyai does not take its event, and why. */
static void mcp_event_reject(struct fyai_mcp_ctx *mcp,
			     struct fy_generic_builder *gb, fy_generic params,
			     fy_generic problems)
{
	struct fyai_mcp *mc = mcp_owner(mcp);
	fy_generic first = fy_get_at(problems, 0, fy_invalid);

	(void)jsonrpc_notify(mcp->conn, "notifications/fyai/rejected",
		fy_mapping(gb, "id", fy_get(params, "id", fy_null),
			   "kind", fy_get(params, "kind", fy_null),
			   "problems", problems));
	fyai_diag_type(mc->diag, FYAIET_WARNING,
		       "MCP server '%s' sent an event that was rejected: %s%s",
		       mcp->name, fy_castp(&first, "?"),
		       fy_len(problems) > 1 ? " (and more)" : "");
	mcp->event_rejected++;
	mcp_sources_publish(mc);
}

/* What a source waits for after an event: its acknowledgement. */
struct mcp_event_ack {
	struct fyai_mcp *mc;
	char *server;
	char *id;
	char *cursor;
};

/*
 * The turn that took the event is published, or the event was dropped
 * unpublished. Either way the credit returns; only a published event is
 * acknowledged, so the source keeps a dropped one and sends it again.
 */
static void mcp_event_ack(void *data, bool published)
{
	struct mcp_event_ack *ack = data;
	struct fyai_mcp_ctx *mcp;
	struct fy_generic_builder *gb;
	fy_generic params;

	/* The server is looked up by name: it may be gone, or started again. */
	mcp = mcp_find_server(ack->mc, ack->server);
	if (!mcp || !mcp->events_supported || !mcp->conn ||
	    jsonrpc_conn_closed(mcp->conn))
		goto out;
	gb = fyai_transient_get(ack->mc->transient);
	if (!gb)
		goto out;
	if (mcp->event_credits < MCP_EVENTS_CREDITS)
		mcp->event_credits++;
	params = fy_mapping(gb, "credits", 1LL);
	if (published) {
		mcp->event_delivered++;
		params = fy_assoc(gb, params, "id", (const char *)ack->id);
		if (ack->cursor) {
			params = fy_assoc(gb, params, "cursor",
					  (const char *)ack->cursor);
			free(mcp->event_cursor);
			mcp->event_cursor = strdup(ack->cursor);
		}
	}
	(void)jsonrpc_notify(mcp->conn, "notifications/fyai/ack", params);
	mcp_sources_publish(ack->mc);
out:
	free(ack->server);
	free(ack->id);
	free(ack->cursor);
	free(ack);
}

/*
 * Bound actions. A kind names tools of its server that answer an event of
 * the kind, and binds arguments of each to JSON Pointers into the data of
 * the event: {"reply": {"bind": {"channel": "/channel", "thread_ts": "/ts"}}}.
 * fyai offers such a tool without the bound arguments and with one more,
 * `event`, the id of the event it answers. A call fills the bound arguments
 * from the event, so the model cannot answer into the wrong place.
 */

/* The bind of @tool in @kind, or fy_invalid when the kind does not bind it. */
static fy_generic mcp_action_bind(fy_generic kind, const char *tool)
{
	fy_generic bind = fy_get(fy_get(fy_get(kind, "actions", fy_invalid),
					tool, fy_invalid), "bind", fy_invalid);

	return fy_is_mapping(bind) ? bind : fy_invalid;
}

/* Whether a kind of @mcp binds @tool. */
static bool mcp_action_bound(struct fyai_mcp_ctx *mcp, const char *tool)
{
	fy_generic name, kind;

	if (!mcp->events_listed || !fy_is_mapping(mcp->event_kinds))
		return false;
	fy_foreach_key_value(name, kind, mcp->event_kinds)
		if (fy_is_valid(mcp_action_bind(kind, tool)))
			return true;
	return false;
}

/* The value at the JSON Pointer @ptr in @doc, or fy_invalid (RFC 6901). */
static fy_generic mcp_json_pointer(fy_generic doc, const char *ptr)
{
	char token[256];
	const char *p = ptr;
	size_t n;
	char *end;
	long idx;

	if (!*p)
		return doc;
	while (*p == '/') {
		p++;
		for (n = 0; *p && *p != '/'; p++) {
			if (n + 1 >= sizeof(token))
				return fy_invalid;
			if (p[0] == '~' && (p[1] == '0' || p[1] == '1')) {
				token[n++] = p[1] == '0' ? '~' : '/';
				p++;
			} else {
				token[n++] = *p;
			}
		}
		token[n] = '\0';
		if (fy_is_mapping(doc)) {
			doc = fy_get(doc, (const char *)token, fy_invalid);
		} else if (fy_is_sequence(doc)) {
			idx = strtol(token, &end, 10);
			if (!*token || *end || idx < 0 ||
			    (size_t)idx >= fy_len(doc))
				return fy_invalid;
			doc = fy_get_at(doc, (size_t)idx, fy_invalid);
		} else {
			return fy_invalid;
		}
		if (fy_is_invalid(doc))
			return fy_invalid;
	}
	return *p ? fy_invalid : doc;
}

/*
 * Offer the actions: each tool that a kind binds loses the arguments that
 * any kind binds, and takes `event`. Rebuilt from the tools as listed.
 */
static void mcp_actions_apply(struct fyai_mcp_ctx *mcp)
{
	struct fyai_mcp *mc = mcp_owner(mcp);
	struct fy_generic_builder *gb = mc->arena->gb;
	fy_generic tool, schema, props, required, kname, kind, bind, arg, ptr;
	fy_generic out = fy_seq_empty, kept, item, dflt, fn, entry, actions;
	fy_generic gname, gdesc;
	const char *name, *desc, *kinds;
	bool bound;

	if (!fy_is_sequence(mcp->tools_raw) || !fy_is_mapping(mcp->event_kinds))
		return;
	fy_foreach_key_value(kname, kind, mcp->event_kinds) {
		actions = fy_get(kind, "actions", fy_invalid);
		if (fy_is_invalid(actions))
			continue;
		fy_foreach_key_value(arg, bind, actions) {
			bound = false;
			fy_foreach(tool, mcp->tools_raw)
				if (fy_equal(fy_get(tool, "name", fy_invalid), arg))
					bound = true;
			if (!bound)
				fyai_diag_type(mc->diag, FYAIET_WARNING,
					"MCP server '%s': kind '%s' binds the "
					"tool '%s', which it does not have",
					mcp->name, fy_castp(&kname, "?"),
					fy_castp(&arg, "?"));
			fy_foreach_key_value(arg, ptr,
				fy_get(bind, "bind", fy_map_empty))
				if (!fy_is_string(ptr) ||
				    (*fy_castp(&ptr, "") &&
				     *fy_castp(&ptr, "") != '/'))
					fyai_diag_type(mc->diag, FYAIET_WARNING,
						"MCP server '%s': kind '%s' binds "
						"'%s' to a value that is not a "
						"JSON Pointer", mcp->name,
						fy_castp(&kname, "?"),
						fy_castp(&arg, "?"));
		}
	}
	fy_foreach(tool, mcp->tools_raw) {
		/* A short string lives in its generic: keep both in scope. */
		gname = fy_get(tool, "name", fy_invalid);
		gdesc = fy_get(tool, "description", fy_invalid);
		name = fy_is_string(gname) ? fy_castp(&gname, "") : "";
		desc = fy_is_string(gdesc) ? fy_castp(&gdesc, "") : "";
		dflt = fy_mapping("type", "object");
		schema = fy_get(tool, "inputSchema", dflt);
		kinds = NULL;
		if (*name && mcp_action_bound(mcp, name) && fy_is_mapping(schema)) {
			props = fy_get(schema, "properties", fy_map_empty);
			required = fy_get(schema, "required", fy_seq_empty);
			fy_foreach_key_value(kname, kind, mcp->event_kinds) {
				bind = mcp_action_bind(kind, name);
				if (fy_is_invalid(bind))
					continue;
				kinds = kinds ? fy_sprintfa("%s, %s", kinds,
						fy_castp(&kname, "")) :
					fy_castp(&kname, "");
				fy_foreach_key_value(arg, ptr, bind) {
					kept = fy_map_empty;
					fy_foreach_key_value(item, dflt, props)
						if (!fy_equal(item, arg))
							kept = fy_assoc(gb, kept,
								item, dflt);
					props = kept;
					kept = fy_seq_empty;
					fy_foreach(item, required)
						if (!fy_equal(item, arg))
							kept = fy_append(gb, kept,
									 item);
					required = kept;
				}
			}
			props = fy_assoc(gb, props, "event", fy_mapping(gb,
				"type", "string",
				"description", "The id of the event that this "
				"answers, as its card gives it. Omit it when "
				"one event of the kind is open."));
			schema = fy_assoc(gb, schema, "properties", props);
			schema = fy_len(required) ?
				 fy_assoc(gb, schema, "required", required) :
				 fy_assoc(gb, schema, "required", fy_seq_empty);
		}
		fn = fy_mapping(gb,
			"name", fy_sprintfa(MCP_PREFIX "%s__%s", mcp->name, name),
			"description", kinds ? fy_stringf(gb,
				"%s\n\nIt answers an event of %s: the event fills "
				"the arguments that say where.", desc, kinds) :
				fy_value(gb, desc),
			"parameters", schema);
		entry = fy_mapping(gb, "type", "function", "function", fn);
		out = fy_append(gb, out, entry);
	}
	mcp->tools = out;
}

/* Keep an event for the actions that can answer it. */
static void mcp_event_record_keep(struct fyai_mcp_ctx *mcp,
				  struct fy_generic_builder *gb,
				  const char *id, const char *kind,
				  fy_generic data)
{
	struct mcp_event_record *r;
	const char *json;
	fy_generic k = fy_get(mcp->event_kinds, kind, fy_invalid);

	if (fy_is_invalid(fy_get(k, "actions", fy_invalid)))
		return;
	json = emit_json_string(gb, data);
	if (!json)
		return;
	r = &mcp->event_records[mcp->event_record_next++ % MCP_EVENT_RECORDS];
	free(r->id);
	free(r->kind);
	free(r->data);
	r->id = strdup(id);
	r->kind = strdup(kind);
	r->data = strdup(json);
	r->answered = false;
	if (!r->id || !r->kind || !r->data) {
		free(r->id);
		free(r->kind);
		free(r->data);
		memset(r, 0, sizeof(*r));
	}
}

/* How to answer the event: its actions, the names the model calls. */
static void mcp_event_answer_line(struct fyai_mcp_ctx *mcp, FILE *mf,
				  const char *kind, const char *id)
{
	fy_generic actions = fy_get(fy_get(mcp->event_kinds, kind, fy_invalid),
				    "actions", fy_invalid), tool, v;
	const char *sep = "";

	if (!fy_is_mapping(actions) || !fy_len(actions))
		return;
	fprintf(mf, "\nAnswer with ");
	fy_foreach_key_value(tool, v, actions) {
		fprintf(mf, "%s" MCP_PREFIX "%s__%s", sep, mcp->name,
			fy_castp(&tool, ""));
		sep = ", ";
	}
	fprintf(mf, " and event \"%s\"; the event fills where it goes.", id);
}

/* A text in its own allocation, or NULL. */
static char *mcp_strf(const char *fmt, ...)
	__attribute__((format(printf, 1, 2)));
static char *mcp_strf(const char *fmt, ...)
{
	va_list ap;
	char *s;
	int rc;

	va_start(ap, fmt);
	rc = vasprintf(&s, fmt, ap);
	va_end(ap);
	return rc < 0 ? NULL : s;
}

/*
 * The arguments of a call of the action @tool: the arguments of the model,
 * without `event`, with the bound ones from the event. The event is the one
 * named, else the only open one of a kind that binds the tool. fy_invalid
 * with the reason in @whyp, allocated, when there is none; the id of the
 * event in @idp.
 */
static fy_generic mcp_action_args(struct fyai_mcp_ctx *mcp,
				  struct fy_generic_builder *gb,
				  const char *tool, fy_generic args,
				  const char **idp, char **whyp)
{
	struct mcp_event_record *r, *found = NULL;
	fy_generic named = fy_get(args, "event", fy_invalid), bind, data, v;
	fy_generic key, ptr, out = fy_map_empty;
	const char *open = NULL;
	unsigned int i, count = 0;

	for (i = 0; i < MCP_EVENT_RECORDS; i++) {
		r = &mcp->event_records[i];
		if (!r->id || fy_is_invalid(mcp_action_bind(fy_get(
				mcp->event_kinds, r->kind, fy_invalid), tool)))
			continue;
		if (fy_is_string(named)) {
			if (!strcmp(r->id, fy_castp(&named, "")))
				found = r;
			continue;
		}
		if (r->answered)
			continue;
		found = r;
		count++;
		open = open ? fy_sprintfa("%s, %s", open, r->id) : r->id;
	}
	if (fy_is_string(named) && !found) {
		*whyp = mcp_strf("tool error: no event '%s' of %s that "
				 "%s answers is known", fy_castp(&named, ""),
				 mcp->name, tool);
		return fy_invalid;
	}
	if (!fy_is_string(named) && count != 1) {
		*whyp = count ? mcp_strf("tool error: name the event that "
				"this answers: %s are open", open) :
			mcp_strf("tool error: no event that %s answers is "
				 "open; name one with event", tool);
		return fy_invalid;
	}
	data = parse_json_string(gb, found->data);
	bind = mcp_action_bind(fy_get(mcp->event_kinds, found->kind,
				      fy_invalid), tool);
	fy_foreach_key_value(key, v, args)
		if (!fy_equal(key, "event"))
			out = fy_assoc(gb, out, key, v);
	fy_foreach_key_value(key, ptr, bind) {
		v = fy_is_string(ptr) ?
		    mcp_json_pointer(data, fy_castp(&ptr, "")) : fy_invalid;
		if (fy_is_invalid(v)) {
			*whyp = mcp_strf("tool error: the event '%s' has no "
					 "value for %s", found->id,
					 fy_castp(&key, "?"));
			return fy_invalid;
		}
		out = fy_assoc(gb, out, key, v);
	}
	*idp = found->id;
	return out;
}

/* An action answered its event: it is no longer open. */
static void mcp_event_answered(struct fyai_mcp *mc, const char *server,
			       const char *id)
{
	struct fyai_mcp_ctx *mcp = mcp_find_server(mc, server);
	unsigned int i;

	for (i = 0; mcp && i < MCP_EVENT_RECORDS; i++)
		if (mcp->event_records[i].id &&
		    !strcmp(mcp->event_records[i].id, id))
			mcp->event_records[i].answered = true;
}

/*
 * The text of an event for the model: a header for the card, then the data
 * framed as external. "</" is written "<\/", which is the same JSON, so the
 * data cannot close its frame.
 */
static char *mcp_event_text(struct fyai_mcp_ctx *mcp,
			    struct fy_generic_builder *gb, const char *kind,
			    const char *id, fy_generic data)
{
	const char *json = emit_json_string(gb, data), *p;
	char *out = NULL;
	size_t len = 0;
	FILE *mf;

	if (!json)
		return NULL;
	mf = open_memstream(&out, &len);
	if (!mf)
		return NULL;
	fprintf(mf, "[event '%s/%s'] %s\n"
		"<external-data source=\"%s\" kind=\"%s\" id=\"%s\">\n",
		mcp->name, kind, id, mcp->name, kind, id);
	for (p = json; *p; p++) {
		if (p[0] == '<' && p[1] == '/')
			fputs("<\\", mf);
		else if (*p != '\n' || p[1])
			fputc(*p, mf);
	}
	fputs("\n</external-data>", mf);
	mcp_event_answer_line(mcp, mf, kind, id);
	if (fclose(mf)) {
		free(out);
		return NULL;
	}
	return out;
}

/*
 * The last event of a source, one line for the live view: its kind and the
 * start of its data. A string is shown as it is; any other value as JSON.
 */
static void mcp_event_last_set(struct fyai_mcp_ctx *mcp,
			       struct fy_generic_builder *gb, const char *kind,
			       fy_generic data)
{
	const char *text = fy_is_string(data) ? fy_castp(&data, "") :
			   emit_json_string(gb, data);
	char *line, *p;

	if (asprintf(&line, "%s: %.120s", kind, text ? text : "") < 0)
		return;
	for (p = line; *p; p++)
		if (*p == '\n' || *p == '\r' || *p == '\t')
			*p = ' ';
	free(mcp->event_last);
	mcp->event_last = line;
}

/* One notifications/fyai/event: check it, then queue it for the model. */
static void mcp_event_receive(struct fyai_mcp_ctx *mcp,
			      struct fy_generic_builder *gb, fy_generic params)
{
	struct fyai_mcp *mc = mcp_owner(mcp);
	struct fyai_ctx *ctx = fyai_mcp_state_ctx(mc);
	struct mcp_event_ack *ack;
	fy_generic kind, data, report, gkind, gid, gcursor;
	const char *kind_name, *id, *cursor, *problem = NULL;
	char *text;

	/* A short string lives in its generic: keep each one in scope. */
	gkind = fy_get(params, "kind", fy_invalid);
	gid = fy_get(params, "id", fy_invalid);
	gcursor = fy_get(params, "cursor", fy_invalid);
	kind_name = fy_is_string(gkind) ? fy_castp(&gkind, "") : "";
	id = fy_is_string(gid) ? fy_castp(&gid, "") : "";
	cursor = fy_is_string(gcursor) ? fy_castp(&gcursor, "") : "";
	data = fy_get(params, "data", fy_invalid);
	kind = *kind_name ? fy_get(mcp->event_kinds, kind_name, fy_invalid) :
			    fy_invalid;
	if (!mcp->events_listed)
		problem = "an event before fyai listed the kinds";
	else if (!fy_is_mapping(kind))
		problem = fy_sprintfa("unknown kind '%.64s'", kind_name);
	else if (!mcp_event_word_valid(id, MCP_EVENT_ID_MAX, "-_.:/@+="))
		problem = "the id is missing, too long, or has a character "
			  "outside letters, digits and \"-_.:/@+=\"";
	else if (fy_is_invalid(data))
		problem = "the event has no data";
	if (problem) {
		mcp_event_reject(mcp, gb, params,
				 fy_sequence(gb, fy_value(gb, problem)));
		return;
	}
	/* A resend of an event that fyai already has. */
	if (mcp_event_seen(mcp, id)) {
		fyai_diag_tracef("event", "dropped a resend of '%s' from '%s'",
				 id, mcp->name);
		mcp->event_resent++;
		return;
	}
	if (!mcp->event_credits) {
		mcp_event_reject(mcp, gb, params, fy_sequence(gb,
			"no credit: wait for an acknowledgement"));
		return;
	}
	report = fyai_schema_validate(gb, fy_get(kind, "schema", fy_true),
				      data);
	if (!fyai_schema_valid(report)) {
		mcp_event_reject(mcp, gb, params,
				 fy_get(report, "problems", fy_seq_empty));
		return;
	}
	text = mcp_event_text(mcp, gb, kind_name, id, data);
	ack = calloc(1, sizeof(*ack));
	if (ack) {
		ack->mc = mc;
		ack->server = strdup(mcp->name);
		ack->id = strdup(id);
		ack->cursor = *cursor ? strdup(cursor) : NULL;
	}
	if (!text || !ack || !ack->server || !ack->id ||
	    (*cursor && !ack->cursor)) {
		free(text);
		if (ack)
			mcp_event_ack(ack, false);
		fyai_diag_type(mc->diag, FYAIET_WARNING,
			       "an event of MCP server '%s' was lost; the "
			       "server sends it again", mcp->name);
		return;
	}
	mcp_event_remember(mcp, id);
	mcp_event_record_keep(mcp, gb, id, kind_name, data);
	mcp->event_credits--;
	mcp->event_received++;
	mcp_event_last_set(mcp, gb, kind_name, data);
	mcp_sources_publish(mc);
	if (fyai_event_inject_acked(ctx->waitq, text, mcp_event_ack, ack))
		fyai_diag_type(mc->diag, FYAIET_WARNING,
			       "an event of MCP server '%s' was lost; the "
			       "server sends it again", mcp->name);
}

/* What a stdio server sends fyai unasked: events, their kinds, and ping. */
static fy_generic mcp_serve(struct jsonrpc_conn *conn, const char *method,
			    fy_generic params, fy_generic id, void *userdata,
			    fy_generic *errorp)
{
	struct fyai_mcp_ctx *mcp = userdata;
	struct fyai_mcp *mc = mcp_owner(mcp);
	struct fy_generic_builder *gb = fyai_transient_get(mc->transient);

	(void)conn;
	if (!strcmp(method, "ping"))
		return fy_map_empty;
	if (gb && mcp->events_supported &&
	    !strcmp(method, "notifications/fyai/event")) {
		mcp_event_receive(mcp, gb, params);
		return fy_invalid;
	}
	if (mcp->events_supported &&
	    !strcmp(method, "notifications/fyai/events/list_changed")) {
		mcp_events_relist(mcp);
		return fy_invalid;
	}
	/* Any other notification is ignored; any other request is unknown. */
	if (fy_is_valid(id) && gb)
		*errorp = fy_mapping(gb, "code", -32601LL,
				     "message", "method not found");
	return fy_invalid;
}

enum mcp_oauth_discovery_state {
	MCP_OD_RESOURCE,
	MCP_OD_AUTHORIZATION_SERVER,
	MCP_OD_REGISTER,
	MCP_OD_AUTHORIZE,
	MCP_OD_TOKEN,
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
	struct fyai_buf response;
	struct curl_slist *headers;
	struct fyai_oauth_flow *flow;
	struct fyai_oauth_pkce pkce;
	char *url;
	char *body;
	char *redirect;
	enum mcp_oauth_discovery_state state;
	/* A save that waits for the store lock, and what follows it. */
	struct fyai_event_source *save_timer;
	void (*save_next)(struct mcp_oauth_discovery *od);
	const char *save_fail;
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
	fyai_event_source_remove(od->save_timer);
	od->save_timer = NULL;
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
	text = fyai_auth_store_read(fyai_mcp_state_ctx(mcp_owner(mcp)), name);
	free(name);
	if (!text)
		return 0;
	gb = fyai_transient_get(mcp_owner(mcp)->transient);
	doc = parse_json_string(gb, text);
	free(text);
	if (!fy_is_mapping(doc))
		return -1;
	issuer = fy_get(doc, "issuer", "");
	if (strcmp(issuer, mcp->oauth_issuer)) {
		fyai_diag_type(mcp_owner(mcp)->diag, FYAIET_DEBUG,
			"MCP %s ignored OAuth cache: issuer changed",
			mcp->name);
		return 0;
	}
	client_id = fy_get(doc, "client_id", "");
	scopes = fy_get(doc, "scopes", "");
	resource = fy_get(doc, "resource", "");
	if (strcmp(resource, mcp->oauth_resource)) {
		fyai_diag_type(mcp_owner(mcp)->diag, FYAIET_DEBUG,
			"MCP %s ignored OAuth cache: resource changed",
			mcp->name);
		return 0;
	}
	if (mcp->oauth_client_configured &&
	    (strcmp(client_id, mcp->oauth_client_id) ||
	     strcmp(scopes, mcp->oauth_scopes))) {
		fyai_diag_type(mcp_owner(mcp)->diag, FYAIET_DEBUG,
			"MCP %s ignored OAuth cache: client changed",
			mcp->name);
		return 0;
	}
	if (!mcp->oauth_client_configured) {
		mcp_oauth_secret_free(&mcp->oauth_client_secret);
		mcp->oauth_client_id = mcp_keep(mcp_owner(mcp), client_id);
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

	gb = fyai_transient_get(mcp_owner(mcp)->transient);
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
	lockfd = fyai_auth_store_lock(fyai_mcp_state_ctx(mcp_owner(mcp)), true);
	if (lockfd == -2)
		return -2;
	if (lockfd < 0)
		return -1;
	name = mcp_oauth_store_name(mcp);
	if (!name) {
		fyai_auth_store_unlock(lockfd);
		return -1;
	}
	rc = fyai_auth_store_write(fyai_mcp_state_ctx(mcp_owner(mcp)), name, json);
	free(name);
	fyai_auth_store_unlock(lockfd);
	return rc;
}

static void mcp_oauth_discovery_fail(struct mcp_oauth_discovery *od,
				     const char *message)
{
	struct mcp_startup *su;
	struct fyai_mcp_ctx *mcp;
	struct fyai_mcp *mc;

	su = od->startup;
	mcp = od->mcp;
	mc = mcp_owner(mcp);
	od->state = MCP_OD_FAILED;
	mcp_set_error(mcp, "OAuth discovery: %s", message);
	fyai_diag_type(mc->diag, FYAIET_ERROR, "MCP server '%s' OAuth discovery failed: %s",
		   od->mcp->name, message);
	if (mcp->recovering) {
		mcp->recovering = false;
		mcp->state = MCP_SRV_FAILED;
		mcp_oauth_discovery_destroy(od);
		fyai_diag_type(mc->diag, FYAIET_WARNING, "MCP server '%s' is unavailable", mcp->name);
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

/*
 * auth_token names an owned copy of the access token, not the builder of the
 * process: a secret is not interned. The new copy is named before the old one
 * is cleared; a request copies the token into its header when it is sent.
 */
static int mcp_oauth_bearer_set(struct fyai_mcp_ctx *mcp, const char *token)
{
	char *fresh, *old;

	fresh = strdup(token ? token : "");
	if (!fresh)
		return -1;
	old = mcp->oauth_bearer;
	mcp->oauth_bearer = fresh;
	mcp->auth_token = fresh;
	mcp_oauth_secret_free(&old);
	return 0;
}

static void mcp_oauth_resume(struct mcp_oauth_discovery *od)
{
	struct mcp_startup *su;
	struct fyai_mcp_ctx *mcp;

	su = od->startup;
	mcp = od->mcp;
	if (mcp_oauth_bearer_set(mcp, mcp->oauth_access_token)) {
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
	gb = fyai_transient_get(mcp_owner(mcp)->transient);
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
	el = fyai_loop_get(mcp_owner(od->mcp)->loop);
	if (!el)
		return -1;
	rc = fyai_oauth_flow_start(fyai_mcp_state_ctx(mcp_owner(od->mcp)), el, &params,
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
	fyai_print_login_url(fyai_mcp_state_ctx(mcp_owner(od->mcp)),
			     lead ? lead : "Authorize the MCP server:",
			     "open the authorization page", od->url);
	fyai_oauth_open_browser(od->url);
	free(od->url);
	od->url = NULL;
	od->state = MCP_OD_AUTHORIZE;
	return 0;
}

static void mcp_oauth_refresh_fail(struct fyai_mcp_ctx *mcp,
				   struct mcp_startup *su, const char *message)
{
	mcp_set_error(mcp, "OAuth refresh: %s", message);
	if (mcp->recovering) {
		mcp->recovering = false;
		mcp->state = MCP_SRV_FAILED;
		fyai_diag_type(mcp_owner(mcp)->diag, FYAIET_WARNING, "MCP server '%s' is unavailable", mcp->name);
		return;
	}
	if (!su) {
		mcp->oauth_expires_at = time(NULL) + 60;
		mcp_oauth_refresh_schedule(mcp);
		return;
	}
	mcp_startup_settle(su, MCP_SRV_FAILED);
}

/* The refresh ended; take its tokens and go on as the server was going. */
static void mcp_oauth_refresh_done(struct fyai_mcp_oauth_refresh *request,
				   const struct fyai_mcp_oauth_result *res,
				   void *userdata)
{
	struct fyai_mcp_ctx *mcp = userdata;
	struct mcp_startup *su = mcp->oauth_refresh_su;
	char *access, *refresh;

	/* The owner stopped it; the owner clears what it holds. */
	if (res->outcome == FYAI_MCP_OAUTH_CANCELLED)
		return;
	mcp->oauth_refresh = NULL;
	mcp->oauth_refresh_su = NULL;
	if (res->outcome == FYAI_MCP_OAUTH_FAILED) {
		fyai_mcp_oauth_refresh_destroy(request);
		mcp_oauth_refresh_fail(mcp, su, res->message ? res->message : "failed");
		return;
	}
	access = strdup(res->access_token);
	refresh = res->refresh_token && *res->refresh_token ?
		strdup(res->refresh_token) : NULL;
	fyai_mcp_oauth_refresh_destroy(request);
	if (!access || (res->refresh_token && *res->refresh_token && !refresh)) {
		mcp_oauth_secret_free(&access);
		mcp_oauth_secret_free(&refresh);
		mcp_oauth_refresh_fail(mcp, su, "out of memory");
		return;
	}
	mcp_oauth_secret_free(&mcp->oauth_access_token);
	mcp->oauth_access_token = access;
	if (refresh) {
		mcp_oauth_secret_free(&mcp->oauth_refresh_token);
		mcp->oauth_refresh_token = refresh;
	}
	mcp->oauth_expires_at = res->expires_at;
	mcp->oauth_force_refresh = false;
	mcp->oauth_force_login = false;
	if (mcp_oauth_bearer_set(mcp, mcp->oauth_access_token)) {
		mcp_oauth_refresh_fail(mcp, su,
			"could not retain the OAuth access token");
		return;
	}
	mcp_oauth_refresh_schedule(mcp);
	if (su)
		mcp_startup_enter(su, MCP_SU_INIT);
	else if (mcp->recovering) {
		mcp->recovering = false;
		mcp->state = MCP_SRV_READY;
	}
}

static int mcp_oauth_refresh_start(struct fyai_mcp_ctx *mcp,
				    struct mcp_startup *su)
{
	struct fyai_mcp_oauth_spec spec;

	if (!mcp->oauth_refresh_token || !*mcp->oauth_refresh_token ||
	    !mcp->oauth_token_endpoint || !mcp->oauth_client_id)
		return -1;
	/* One refresh at a time: a request that is still out finishes by itself. */
	mcp_oauth_refresh_release(mcp);
	memset(&spec, 0, sizeof(spec));
	spec.server = mcp->name;
	spec.issuer = mcp->oauth_issuer;
	spec.resource = mcp->oauth_resource;
	spec.token_endpoint = mcp->oauth_token_endpoint;
	spec.client_id = mcp->oauth_client_id;
	spec.client_secret = mcp->oauth_client_secret;
	/* A configured client keeps its secret out of the record. */
	spec.store_secret = mcp->oauth_client_configured ? "" :
		mcp->oauth_client_secret;
	spec.scopes = mcp->oauth_scopes;
	spec.access_token = mcp->oauth_access_token;
	spec.refresh_token = mcp->oauth_refresh_token;
	spec.auth = mcp->oauth_token_auth_method == MCP_TOKEN_AUTH_POST ?
		FYAI_MCP_CLIENT_POST :
		mcp->oauth_token_auth_method == MCP_TOKEN_AUTH_BASIC ?
		FYAI_MCP_CLIENT_BASIC : FYAI_MCP_CLIENT_NONE;
	spec.timeout = mcp->timeout;
	mcp->oauth_refresh_su = su;
	mcp->oauth_refresh = fyai_mcp_oauth_refresh_submit(fyai_mcp_state_ctx(mcp_owner(mcp)), &spec,
			mcp_oauth_refresh_done, mcp);
	if (!mcp->oauth_refresh) {
		mcp->oauth_refresh_su = NULL;
		return -1;
	}
	return 0;
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
	el = fyai_loop_get(mcp_owner(mcp)->loop);
	if (!el)
		return;
	rc = fyai_event_add_timer(el, delay, 0, mcp_oauth_refresh_timer, mcp,
				  &mcp->oauth_refresh_timer);
	if (rc)
		fyai_diag_type(mcp_owner(mcp)->diag, FYAIET_WARNING,
			"MCP %s could not schedule OAuth refresh", mcp->name);
}

/*
 * Save the record under the store lock and then run next. A lock that a
 * sibling holds is tried again by a timer: a login that the user completed is
 * not dropped for a busy store.
 */
static void mcp_oauth_save_then(struct mcp_oauth_discovery *od,
				void (*next)(struct mcp_oauth_discovery *),
				const char *fail);

static enum fyai_event_action mcp_oauth_save_timer(const struct fyai_event *ev)
{
	struct mcp_oauth_discovery *od = ev->userdata;

	od->save_timer = NULL;
	mcp_oauth_save_then(od, od->save_next, od->save_fail);
	return FYAIEA_CONTINUE;
}

static void mcp_oauth_save_then(struct mcp_oauth_discovery *od,
				void (*next)(struct mcp_oauth_discovery *),
				const char *fail)
{
	int rc;

	rc = mcp_oauth_store_save(od->mcp);
	if (rc == -2) {
		od->save_next = next;
		od->save_fail = fail;
		rc = fyai_event_add_timer(fyai_loop_get(mcp_owner(od->mcp)->loop), 50, 0,
					  mcp_oauth_save_timer, od,
					  &od->save_timer);
		if (rc)
			mcp_oauth_discovery_fail(od, fail);
		return;
	}
	if (rc) {
		mcp_oauth_discovery_fail(od, fail);
		return;
	}
	next(od);
}

static void mcp_oauth_token_saved(struct mcp_oauth_discovery *od)
{
	if (od->flow) {
		fyai_oauth_flow_finish(od->flow, true);
		fyai_oauth_flow_destroy(od->flow);
		od->flow = NULL;
	}
	mcp_oauth_resume(od);
}

static bool mcp_oauth_token_complete(struct mcp_oauth_discovery *od,
					     fy_generic doc)
{
	struct fyai_mcp_ctx *mcp = od->mcp;
	const char *access_token;
	const char *refresh_token;

	if (od->state != MCP_OD_TOKEN)
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
	    (*refresh_token && !mcp->oauth_refresh_token)) {
		mcp_oauth_discovery_fail(od,
			"could not save OAuth tokens");
		return true;
	}
	mcp_oauth_save_then(od, mcp_oauth_token_saved,
			    "could not save OAuth tokens");
	return true;
}

static void mcp_oauth_registration_saved(struct mcp_oauth_discovery *od)
{
	int rc;

	rc = mcp_oauth_authorize_start(od);
	if (rc)
		mcp_oauth_discovery_fail(od,
			rc == -2 ?
			"browser login needs auth.allow_browser=true" :
			"could not start OAuth authorization");
}

static bool mcp_oauth_registration_complete(struct mcp_oauth_discovery *od,
						     fy_generic doc)
{
	struct fyai_mcp_ctx *mcp;
	const char *client_id;
	const char *client_secret;

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
	mcp_oauth_secret_free(&mcp->oauth_client_secret);
	free(mcp->oauth_access_token);
	free(mcp->oauth_refresh_token);
	mcp->oauth_client_id = mcp_keep(mcp_owner(mcp), client_id);
	mcp->oauth_client_secret = *client_secret ?
		strdup(client_secret) : NULL;
	if (!mcp->oauth_client_id ||
	    (*client_secret && !mcp->oauth_client_secret)) {
		mcp_oauth_discovery_fail(od,
			"could not retain registered client");
		return true;
	}
	mcp_oauth_save_then(od, mcp_oauth_registration_saved,
			    "could not save registered client");
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
	mcp->oauth_issuer = mcp_keep(mcp_owner(mcp), issuer);
	mcp->oauth_resource = mcp_keep(mcp_owner(mcp), resource);
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
	gb = fyai_transient_get(mcp_owner(mcp)->transient);
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
	mcp->oauth_authorization_endpoint = mcp_keep(mcp_owner(mcp), authorize);
	mcp->oauth_token_endpoint = mcp_keep(mcp_owner(mcp), token);
	mcp->oauth_registration_endpoint = *registration ?
		mcp_keep(mcp_owner(mcp), registration) : NULL;
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
	struct fyai_mcp *mc;

	mc = mcp_owner(od->mcp);
	od->curl = curl_easy_init();
	/* Reserve SIGALRM for the watchdog. */
	curl_easy_setopt(od->curl, CURLOPT_NOSIGNAL, 1L);

	fyai_diag_check(mc->diag, od->curl, err_out,
			 "could not create OAuth discovery transfer");
	curl_easy_setopt(od->curl, CURLOPT_URL, od->url);
	curl_easy_setopt(od->curl, CURLOPT_WRITEFUNCTION, write_response);
	curl_easy_setopt(od->curl, CURLOPT_WRITEDATA, &od->response);
	curl_easy_setopt(od->curl, CURLOPT_TIMEOUT, (long)od->mcp->timeout);
	curl_easy_setopt(od->curl, CURLOPT_USERAGENT, mc->link->user_agent);
	if (od->state == MCP_OD_TOKEN &&
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
	od->transfer = fyai_curl_submit(mc->link, od->curl,
					mcp_oauth_discovery_complete, od);
	fyai_diag_check(mc->diag, od->transfer, err_out,
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
	fyai_diag_check(mcp_owner(mcp)->diag, od, err_out,
			 "could not allocate OAuth discovery");
	od->mcp = mcp;
	od->startup = su;
	od->state = MCP_OD_RESOURCE;
	od->url = strdup(mcp->oauth_resource_metadata);
	fyai_diag_check(mcp_owner(mcp)->diag, od->url, err_free,
			 "could not retain OAuth resource metadata URL");
	mcp->oauth_discovery = od;
	rc = mcp_oauth_discovery_submit(od);
	fyai_diag_check(mcp_owner(mcp)->diag, !rc, err_free,
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
	fyai_diag_check(mcp_owner(mcp)->diag, !rc, err_out,
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
	if (mcp->transport != MCP_TRANSPORT_HTTP)
		return true;
	if (status == 401 || status == 403)
		return true;
	return !fyai_http_transient(code, status);
}

static void mcp_startup_collect(struct fyai_mcp_ctx *mcp, fy_generic result)
{
	struct fyai_mcp *mc = mcp_owner(mcp);
	fy_generic tools = fy_get(result, "tools", fy_seq_empty), tool;
	fy_generic dflt, fn, entry;
	const char *tool_name;

	fy_foreach(tool, tools) {
		tool_name = fy_get(tool, "name", "");
		if (!*tool_name)
			continue;
		/* Each level is built apart: a nested builder call expands to
		 * megabytes. */
		dflt = fy_mapping("type", "object");
		fn = fy_mapping(mc->arena->gb,
			"name", fy_sprintfa(MCP_PREFIX "%s__%s", mcp->name,
					    tool_name),
			"description", fy_get(tool, "description", ""),
			"parameters", fy_get(tool, "inputSchema", dflt));
		entry = fy_mapping(mc->arena->gb, "type", "function", "function", fn);
		mcp->tools = fy_append(mc->arena->gb, mcp->tools, entry);
		mcp->tools_raw = fy_append(mc->arena->gb,
			fy_is_sequence(mcp->tools_raw) ? mcp->tools_raw :
			fy_seq_empty, tool);
	}
}

static void mcp_startup_req_done(struct jsonrpc_request *req, void *userdata)
{
	struct mcp_startup *su = userdata;
	struct fyai_mcp_ctx *mcp = su->mcp;
	struct fyai_mcp *mc = mcp_owner(mcp);
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
				      mcp_startup_phase_name(su->phase),
				      status, response_body);
		else
			mcp_set_error(mcp, "%s failed: %s (HTTP %ld)",
				      mcp_startup_phase_name(su->phase),
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
			fyai_diag_reset(mc->diag);
			mcp->oauth_force_refresh = mcp->auth_token &&
				*mcp->auth_token;
			if (mcp->oauth_refresh_timer) {
				fyai_event_source_remove(
					mcp->oauth_refresh_timer);
				mcp->oauth_refresh_timer = NULL;
			}
			mcp_oauth_refresh_release(mcp);
			if (!mcp_oauth_discovery_start(su))
				return;
			mcp_startup_settle(su, MCP_SRV_FAILED);
			return;
		}
		if (!mcp_startup_terminal(mcp, status, code) &&
		    su->attempt + 1 < MCP_STARTUP_RETRIES) {
			su->attempt++;
			el = fyai_loop_get(mc->loop);
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
		mcp->resources_supported = fy_is_mapping(fy_get(
			fy_get(result, "capabilities", fy_map_empty),
			"resources", fy_invalid));
		mcp_events_negotiated(mcp, result);
		if (*protocol)
			mcp->protocol_version =
				fy_gb_intern_string(mc->cfg->gb, protocol);
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
		else if (mcp->events_supported)
			mcp_startup_enter(su, MCP_SU_EVENTS);
		else
			mcp_startup_settle(su, MCP_SRV_READY);
		break;
	case MCP_SU_EVENTS:
		mcp->event_kinds = mcp_events_collect(mcp, mcp->event_kinds,
						      result);
		cursor = fy_get(result, "nextCursor", "");
		free(su->cursor);
		su->cursor = *cursor ? strdup(cursor) : NULL;
		if (su->cursor) {
			mcp_startup_enter(su, MCP_SU_EVENTS);
			break;
		}
		mcp->events_listed = true;
		mcp_actions_apply(mcp);
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
	struct fyai_mcp *mc = mcp_owner(mcp);
	struct fy_generic_builder *gb;
	const char *method;
	fy_generic params;
	bool notification = false;

	gb = fyai_transient_get(mc->transient);
	if (!gb) {
		fyai_diag_type(mc->diag, FYAIET_ERROR, "%s could not acquire transient storage",
			   mcp->name);
		mcp_startup_settle(su, MCP_SRV_FAILED);
		return;
	}

	switch (su->phase) {
	case MCP_SU_INIT:
		method = "initialize";
		params = fy_mapping(gb,
			"protocolVersion", mcp->protocol_version,
			"capabilities", mcp_client_capabilities(mcp, gb),
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
	case MCP_SU_EVENTS:
		method = "fyai/events/list";
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

static int mcp_stdio_spawn(struct fyai_mcp *mc, struct fyai_mcp_ctx *mcp,
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
	fyai_diag_check(mc->diag, fy_is_sequence(args), err_out,
			 "MCP stdio server '%s' args must be a sequence", mcp->name);
	fyai_diag_check(mc->diag, fy_is_invalid(env) ||
			 fy_is_mapping(env), err_out,
			 "MCP stdio server '%s' env must be a mapping", mcp->name);

	argc = fy_len(args);
	fyai_diag_check(mc->diag, *command, err_out,
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
	fyai_diag_check(mc->diag, !rc, err_out,
			 "MCP stdio server '%s' could not create pipes: %s",
			 mcp->name, strerror(errno));
	rc = fcntl(inpipe[0], F_SETFD, FD_CLOEXEC);
	if (!rc)
		rc = fcntl(inpipe[1], F_SETFD, FD_CLOEXEC);
	if (!rc)
		rc = fcntl(outpipe[0], F_SETFD, FD_CLOEXEC);
	if (!rc)
		rc = fcntl(outpipe[1], F_SETFD, FD_CLOEXEC);
	fyai_diag_check(mc->diag, !rc, err_out,
			 "MCP stdio server '%s' could not protect pipes: %s",
			 mcp->name, strerror(errno));

	pid = fork();
	fyai_diag_check(mc->diag, pid >= 0, err_out,
			 "MCP stdio server '%s' could not fork: %s",
			 mcp->name, strerror(errno));

	if (!pid) {
		struct fyai_child_spec spec = {};

		signal(SIGPIPE, SIG_DFL);
		close(inpipe[1]);
		close(outpipe[0]);
		inpipe[1] = outpipe[0] = -1;

		/* Run the configured server on pipes without model confinement. */
		spec.in_fd = inpipe[0];
		spec.out_fd = outpipe[1];
		spec.err_fd = -1;
		spec.ctty_fd = -1;
		spec.own_session = true;
		spec.workdir = cwd;
		rc = fyai_child_exec_prepare(fyai_mcp_state_ctx(mc), &spec);
		if (rc)
			_exit(rc);

		/* Apply configured variables after removing credentials. */
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

static int mcp_create_server(struct fyai_mcp *mc, const char *server_name,
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

	fyai_diag_check(mc->diag, mcp_server_name_valid(server_name), err_out,
			 "invalid MCP server name '%s'", server_name);
	command = fy_get(config, "command", "");
	transport = fy_get(config, "transport", *command ? "stdio" :
						       "streamable-http");
	fyai_diag_check(mc->diag, !strcmp(transport, "stdio") ||
			 !strcmp(transport, "streamable-http") ||
			 !strcmp(transport, "http"), err_out,
			 "MCP server '%s' has unknown transport '%s'",
			 server_name, transport);
	endpoint = fy_get(config, "endpoint", "");
	fyai_diag_check(mc->diag, !strcmp(transport, "stdio") || *endpoint, err_out,
			 "MCP HTTP server '%s' has no endpoint", server_name);
	rc = mcp_server_auth_mode(mc, config, &auth_mode);
	fyai_diag_check(mc->diag, !rc, err_out,
			 "MCP server '%s' has invalid auth configuration",
			 server_name);
	fyai_diag_check(mc->diag, strcmp(transport, "stdio") ||
			 auth_mode == MCP_AUTH_BEARER, err_out,
			 "MCP stdio server '%s' cannot use OAuth",
			 server_name);
	protocol = fy_get(config, "protocol_version",
			  mc->cfg->mcp_protocol_version);
	auth_ref = fy_get(config, "auth_token", fy_invalid);
	auth = fy_get(config, "auth", fy_invalid);
	token = !strcmp(transport, "stdio") ||
		auth_mode == MCP_AUTH_OAUTH ? NULL :
		mcp_server_auth_token(mc, config);
	fyai_diag_check(mc->diag, !strcmp(transport, "stdio") ||
			 auth_mode == MCP_AUTH_OAUTH ||
			 fy_is_invalid(auth_ref) || token ||
			 fy_equal(fy_get(auth_ref, "type"), "auto"), err_out,
			 "MCP server '%s' could not resolve auth token", server_name);

	mcp = calloc(1, sizeof(*mcp));
	fyai_diag_check(mc->diag, mcp, err_out,
			 "MCP server '%s' could not allocate context", server_name);
	mcp->owner = mc;
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
	mcp->name = fy_gb_intern_string(mc->cfg->gb, server_name);
	if (*endpoint)
		mcp->endpoint = fy_gb_intern_string(mc->cfg->gb, endpoint);
	mcp->protocol_version = fy_gb_intern_string(mc->cfg->gb,
						 protocol);
	rc = token ? mcp_oauth_bearer_set(mcp, token) : 0;
	fyai_diag_check(mc->diag, !rc, err_out,
			 "MCP server '%s' could not keep its token", server_name);
	rc = auth_mode == MCP_AUTH_OAUTH ?
		mcp_oauth_configure_client(mc, mcp, auth) : 0;
	fyai_diag_check(mc->diag, !rc, err_out,
			 "MCP server '%s' has invalid OAuth client",
			 server_name);
	mcp->timeout = fy_get(config, "timeout", mc->cfg->mcp_timeout);
	if (mcp->timeout <= 0)
		mcp->timeout = mc->cfg->mcp_timeout;
	/* Not the all-zero word: that is an empty sequence. */
	mcp->event_kinds = fy_map_empty;
	mcp->event_relist_kinds = fy_invalid;
	mcp->events_wanted = fy_get(config, "events", false);
	fyai_diag_check(mc->diag, !mcp->events_wanted ||
			 mcp->transport == MCP_TRANSPORT_STDIO, err_out,
			 "MCP server '%s' can send events only over stdio",
			 server_name);

	if (mcp->transport == MCP_TRANSPORT_HTTP)
		mcp->curl = curl_easy_init();
	/* Reserve SIGALRM for the watchdog. */
	curl_easy_setopt(mcp->curl, CURLOPT_NOSIGNAL, 1L);


	fyai_diag_check(mc->diag, mcp->name && mcp->protocol_version &&
			 (!token || mcp->oauth_bearer) &&
			 (mcp->transport != MCP_TRANSPORT_HTTP ||
			  (mcp->endpoint && mcp->curl)), err_out,
			 "MCP server '%s' could not initialize context", server_name);

	rc = mcp->transport == MCP_TRANSPORT_STDIO ?
		mcp_stdio_spawn(mc, mcp, config) : 0;
	fyai_diag_check(mc->diag, !rc, err_out,
			 "MCP stdio server '%s' could not start", server_name);

	if (mcp->transport == MCP_TRANSPORT_STDIO) {
		mcp->conn = jsonrpc_conn_stdio(fyai_mcp_state_ctx(mc), mcp->stdin_fd,
				mcp->stdout_fd, mcp->timeout, mcp->name, "mcp");
		/* Events, their kinds and ping arrive unasked. */
		if (mcp->conn)
			(void)jsonrpc_conn_serve(mcp->conn, mcp_serve, mcp);
	} else {
		struct jsonrpc_http_hooks hooks = {
			.userdata = mcp,
			.add_headers = mcp_http_add_headers,
			.response_header = mcp_http_response_header,
		};
		mcp->conn = jsonrpc_conn_http(fyai_mcp_state_ctx(mc), mcp->endpoint, mcp->timeout,
				mcp->name, "mcp", &hooks);
	}
	fyai_diag_check(mc->diag, mcp->conn, err_out,
			 "MCP server '%s' could not create connection",
			 server_name);

	tail = &mc->conns;
	while (*tail)
		tail = &(*tail)->next;
	*tail = mcp;

	if (mc->cfg->mcp_logging)
		(void)fyai_log_generic(fyai_mcp_state_ctx(mc), "mcp", fy_mapping(
			"event", "connect", "server", mcp->name,
			"transport", mcp->transport == MCP_TRANSPORT_HTTP ?
				"streamable-http" :
				mcp_transport_name(mcp->transport)));

	return 0;

err_out:
	if (!mcp)
		return -1;
	if (mc->conns == mcp)
		mc->conns = mcp->next;
	else {
		for (prev = mc->conns; prev && prev->next != mcp;
		     prev = prev->next)
			;
		if (prev)
			prev->next = mcp->next;
	}
	mcp_events_release(mcp);
	jsonrpc_conn_destroy(mcp->conn);
	if (mcp->curl)
		curl_easy_cleanup(mcp->curl);
	mcp_stdio_stop(mc, mcp);
	mcp_oauth_discovery_destroy(mcp->oauth_discovery);
	mcp_oauth_refresh_release(mcp);
	free(mcp->oauth_resource_metadata);
	mcp_oauth_secret_free(&mcp->oauth_client_secret);
	mcp_oauth_secret_free(&mcp->oauth_access_token);
	mcp_oauth_secret_free(&mcp->oauth_refresh_token);
	mcp_oauth_secret_free(&mcp->oauth_bearer);
	free(mcp);
	return -1;
}

/* True once every configured server has settled (READY or FAILED). */
bool fyai_mcp_settled(struct fyai_mcp *mc)
{
	struct fyai_mcp_ctx *mcp;

	for (mcp = mc->conns; mcp; mcp = mcp->next)
		if (mcp->state == MCP_SRV_CONNECTING)
			return false;
	return true;
}

/* Fold every ready server's discovered tools into mc->tools, in
 * configuration order (mc->conns is built in that order). */
void fyai_mcp_publish_tools(struct fyai_ctx *ctx)
{
	struct fyai_mcp_ctx *mcp;
	fy_generic out = fy_seq_empty;

	for (mcp = ctx->mcp->conns; mcp; mcp = mcp->next)
		if (mcp->state == MCP_SRV_READY &&
		    fy_is_valid(mcp->tools))
			out = fy_concat(ctx->arena->gb, out, mcp->tools);
	ctx->mcp->tools = fy_gb_internalize(ctx->arena->gb, out);
	ctx->toolspec->spec = fy_invalid;
}

static struct fyai_mcp_ctx *
mcp_find_server(struct fyai_mcp *mc, const char *name)
{
	struct fyai_mcp_ctx *mcp;

	for (mcp = mc->conns; mcp; mcp = mcp->next)
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
	if (!mcp->oauth_discovery && !mcp->oauth_refresh)
		return;
	/* A login or logout supersedes a refresh: its answer is not wanted. */
	if (mcp->oauth_refresh) {
		fyai_mcp_oauth_refresh_abort(mcp->oauth_refresh);
		mcp->oauth_refresh = NULL;
		mcp->oauth_refresh_su = NULL;
	}
	mcp_oauth_discovery_destroy(mcp->oauth_discovery);
	mcp->recovering = false;
	mcp->oauth_force_login = false;
	mcp->oauth_force_refresh = false;
	mcp->state = MCP_SRV_FAILED;
}

fy_generic fyai_mcp_status_data(struct fyai_mcp *mc,
				struct fy_generic_builder *gb)
{
	struct fyai_mcp_ctx *mcp;
	fy_generic rows;
	const char *state;
	const char *oauth;
	long long expires_in;

	rows = fy_seq_empty;
	for (mcp = mc->conns; mcp; mcp = mcp->next) {
		state = mcp->state == MCP_SRV_READY ? "ready" :
			mcp->state == MCP_SRV_CONNECTING ?
			"connecting" : "failed";
		if (mcp->auth_mode != MCP_AUTH_OAUTH)
			oauth = mcp->auth_token ? "bearer" : "none";
		else if (mcp->oauth_discovery || mcp->oauth_refresh)
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
			"transport", mcp_transport_name(mcp->transport),
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
	return rows;
}

/* The state of an event source, as the live view and `list` name it. */
static const char *mcp_source_state(const struct fyai_mcp_ctx *mcp)
{
	if (mcp->state == MCP_SRV_CONNECTING)
		return "starting";
	if (mcp->state == MCP_SRV_FAILED)
		return "failed";
	if (!mcp->conn || jsonrpc_conn_closed(mcp->conn))
		return "down";
	if (!mcp->events_supported)
		return "no-events";
	return "up";
}

fy_generic fyai_mcp_sources_rows(struct fyai_mcp *mc,
				 struct fy_generic_builder *gb)
{
	struct fyai_mcp_ctx *mcp;
	fy_generic rows = fy_seq_empty;

	for (mcp = mc ? mc->conns : NULL; mcp; mcp = mcp->next) {
		if (!mcp->events_wanted)
			continue;
		rows = fy_append(gb, rows, fy_null_filtered_mapping(gb,
			"name", mcp->name,
			"state", mcp_source_state(mcp),
			"credits", (long long)mcp->event_credits,
			"received", (long long)mcp->event_received,
			"delivered", (long long)mcp->event_delivered,
			"rejected", (long long)mcp->event_rejected,
			"resent", (long long)mcp->event_resent,
			"kinds", (long long)(fy_is_mapping(mcp->event_kinds) ?
					     fy_len(mcp->event_kinds) : 0),
			"last", mcp->event_last ?
				fy_value(gb, (const char *)mcp->event_last) :
				fy_null));
	}
	return rows;
}

bool fyai_mcp_source_known(struct fyai_mcp *mc, const char *name)
{
	struct fyai_mcp_ctx *mcp = mc ? mcp_find_server(mc, name) : NULL;

	return mcp && mcp->events_wanted;
}

bool fyai_mcp_source_up(struct fyai_mcp *mc, const char *name)
{
	struct fyai_mcp_ctx *mcp = mc ? mcp_find_server(mc, name) : NULL;

	if (!mcp || !mcp->events_wanted)
		return false;
	return mcp->state == MCP_SRV_CONNECTING ||
	       !strcmp(mcp_source_state(mcp), "up");
}

fy_generic fyai_mcp_live_rows(struct fyai_mcp *mc,
			      struct fy_generic_builder *gb, bool sources)
{
	struct fyai_mcp_ctx *mcp;
	fy_generic rows = fy_seq_empty;
	const char *state, *mark, *title, *last;

	for (mcp = mc ? mc->conns : NULL; mcp; mcp = mcp->next) {
		state = mcp_source_state(mcp);
		title = fy_sprintfa("**%s** `%s`",
				    mcp->events_wanted ? "source" : "mcp",
				    mcp->name);
		if (mcp->state == MCP_SRV_CONNECTING) {
			mark = "running";
			last = "starting";
		} else if (!strcmp(state, "failed") || !strcmp(state, "down")) {
			mark = "failed";
			last = mcp->last_error ?
			       fy_sprintfa("%s: %s", state, mcp->last_error) :
			       state;
		} else if (mcp->events_wanted && sources) {
			/* A source that is up shows only when asked. */
			mark = "ok";
			last = fy_sprintfa("%lu in, %lu done, %lu rejected%s%s",
				mcp->event_received, mcp->event_delivered,
				mcp->event_rejected,
				mcp->event_last ? " \u00b7 " : "",
				mcp->event_last ? mcp->event_last : "");
		} else {
			continue;
		}
		rows = fy_append(gb, rows, fy_mapping(gb,
			"title", fy_value(gb, title),
			"mark", mark,
			"last", fy_value(gb, last),
			"quiet", !strcmp(mark, "ok") ? fy_true : fy_false));
	}
	return rows;
}

/*
 * Put the live lines of the servers in the panels store of the UI model, as
 * "mcp": the display may be the front of a remote worker, which sees the
 * model and not the connections. A source that is up is there too, marked
 * "quiet", and the display shows it when display/live_sources asks.
 */
static void mcp_sources_publish(struct fyai_mcp *mc)
{
	struct fyai_ctx *ctx = fyai_mcp_state_ctx(mc);
	struct fy_generic_builder *gb = fyai_uimodel_begin(ctx);
	fy_generic rows;

	if (!gb)
		return;
	rows = fyai_mcp_live_rows(mc, gb, true);
	if (!fy_is_valid(rows) ||
	    fyai_uimodel_set_member(ctx, FYAI_UI_PANELS, "mcp", rows) < 0)
		fyai_warning(ctx, "mcp: cannot keep the servers for the display");
	fyai_uimodel_end(ctx);
}

int fyai_mcp_login(struct fyai_mcp *mc, const char *name)
{
	struct fyai_mcp_ctx *mcp;
	char *resource;
	int rc;

	mcp = mcp_find_server(mc, name);
	fyai_diag_check(mc->diag, mcp, err_out,
			 "MCP server '%s' is not active", name);
	fyai_diag_check(mc->diag, mcp->transport == MCP_TRANSPORT_HTTP &&
			 mcp->auth_mode == MCP_AUTH_OAUTH, err_out,
			 "MCP server '%s' does not use HTTP OAuth", name);
	fyai_diag_check(mc->diag, mcp->oauth_allow_browser, err_out,
			 "MCP server '%s' needs auth.allow_browser=true",
			 name);
	fyai_diag_check(mc->diag, !mcp->startup, err_out,
			 "MCP server '%s' is starting", name);
	mcp_oauth_operation_cancel(mcp);
	if (!mcp->oauth_resource_metadata) {
		resource = mcp_oauth_resource_url(mcp->endpoint);
		fyai_diag_check(mc->diag, resource, err_out,
				 "could not derive OAuth metadata for '%s'",
				 name);
		mcp->oauth_resource_metadata = resource;
	}
	mcp->last_error = NULL;
	mcp->oauth_force_login = true;
	rc = mcp_oauth_recovery_begin(mcp);
	fyai_diag_check(mc->diag, !rc, err_out,
			 "could not start OAuth login for '%s'", name);
	return 0;

err_out:
	return -1;
}

int fyai_mcp_logout(struct fyai_mcp *mc, const char *name)
{
	struct fyai_mcp_ctx *mcp;
	const char *store_name;
	int lockfd;
	int rc;

	mcp = mcp_find_server(mc, name);
	fyai_diag_check(mc->diag, mcp, err_out,
			 "MCP server '%s' is not active", name);
	fyai_diag_check(mc->diag, mcp->auth_mode == MCP_AUTH_OAUTH, err_out,
			 "MCP server '%s' does not use OAuth", name);
	fyai_diag_check(mc->diag, !mcp->startup, err_out,
			 "MCP server '%s' is starting", name);
	mcp_oauth_operation_cancel(mcp);
	store_name = fy_sprintfa("mcp-auth-%s.json", mcp->name);
	lockfd = fyai_auth_store_lock(fyai_mcp_state_ctx(mc), true);
	fyai_diag_check(mc->diag, lockfd >= 0, err_out,
			 "could not lock OAuth store");
	rc = fyai_auth_store_delete(fyai_mcp_state_ctx(mc), store_name);
	fyai_auth_store_unlock(lockfd);
	fyai_diag_check(mc->diag, !rc, err_out,
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
		mcp->oauth_client_id = NULL;
		mcp_oauth_secret_free(&mcp->oauth_client_secret);
	}
	mcp->oauth_expires_at = 0;
	mcp->auth_token = NULL;
	mcp_oauth_secret_free(&mcp->oauth_bearer);
	mcp->oauth_force_login = false;
	mcp->oauth_force_refresh = false;
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
int fyai_mcp_start(struct fyai_mcp *mc)
{
	fy_generic servers = mc->cfg->mcp_servers;
	struct fy_generic_builder *gb;
	fy_generic key, config;
	struct fyai_mcp_ctx *mcp;
	const char *name;

	fyai_mcp_cleanup(mc);
	mc->tools = fy_invalid;
	mc->active_tools = fy_invalid;

	if (fy_is_mapping(servers) && fy_len(servers)) {
		fy_foreach(key, servers) {
			name = fy_castp(&key, "");
			config = fy_get(servers, key, fy_invalid);
			if (!fy_is_mapping(config)) {
				fyai_diag_type(mc->diag, FYAIET_WARNING, "MCP server '%s' configuration "
					     "is not a mapping; skipped", name);
				continue;
			}
			(void)mcp_create_server(mc, name, config);
		}
	} else if (mc->cfg->mcp_endpoint && *mc->cfg->mcp_endpoint) {
		gb = fyai_transient_get(mc->transient);
		if (!gb) {
			fyai_diag_type(mc->diag, FYAIET_ERROR, "could not acquire transient storage");
			return -1;
		}
		config = fy_mapping(gb,
			"endpoint", mc->cfg->mcp_endpoint,
			"protocol_version", mc->cfg->mcp_protocol_version,
			"timeout", mc->cfg->mcp_timeout);
		(void)mcp_create_server(mc, "default", config);
	} else {
		fyai_diag_type(mc->diag, FYAIET_ERROR, "MCP is enabled but no servers are configured");
		return -1;
	}

	for (mcp = mc->conns; mcp; mcp = mcp->next)
		mcp_startup_begin(mcp);
	return 0;
}

int fyai_mcp_add(struct fyai_mcp *mc, const char *name, fy_generic config)
{
	struct fyai_mcp_ctx *mcp;
	int rc;

	fyai_diag_check(mc->diag, !mcp_find_server(mc, name), err,
			 "MCP server '%s' exists already", name);
	rc = mcp_create_server(mc, name, config);
	if (rc)		/* mcp_create_server() says why */
		goto err;
	mcp = mcp_find_server(mc, name);
	fyai_diag_check(mc->diag, mcp, err, "MCP server '%s' did not start", name);
	mc->invocation = true;
	mcp_startup_begin(mcp);
	return 0;
err:
	return -1;
}

int fyai_mcp_add_tunnel(struct fyai_mcp *mc, const char *name,
			int (*send)(void *userdata, fy_generic frame),
			void *userdata, struct jsonrpc_conn **connp)
{
	struct fyai_mcp_ctx *mcp, **tail;

	*connp = NULL;
	fyai_diag_check(mc->diag, mcp_server_name_valid(name), err,
			 "invalid MCP server name '%s'", name);
	fyai_diag_check(mc->diag, !mcp_find_server(mc, name), err,
			 "MCP server '%s' exists already", name);
	mcp = calloc(1, sizeof(*mcp));
	fyai_diag_check(mc->diag, mcp, err,
			 "MCP server '%s' could not allocate context", name);
	mcp->owner = mc;
	mcp->tools = fy_seq_empty;
	mcp->state = MCP_SRV_CONNECTING;
	mcp->pid = -1;
	mcp->stdin_fd = -1;
	mcp->stdout_fd = -1;
	mcp->transport = MCP_TRANSPORT_TUNNEL;
	mcp->auth_mode = MCP_AUTH_BEARER;
	mcp->name = fy_gb_intern_string(mc->cfg->gb, name);
	mcp->protocol_version = fy_gb_intern_string(mc->cfg->gb,
					mc->cfg->mcp_protocol_version);
	mcp->timeout = mc->cfg->mcp_timeout;
	mcp->conn = mcp->name ? jsonrpc_conn_tunnel(fyai_mcp_state_ctx(mc), mcp->timeout,
					mcp->name, "mcp", send, userdata) : NULL;
	if (!mcp->conn || !mcp->protocol_version) {
		fyai_diag_type(mc->diag, FYAIET_ERROR, "MCP server '%s' could not create connection",
			   name);
		jsonrpc_conn_destroy(mcp->conn);
		free(mcp);
		goto err;
	}
	tail = &mc->conns;
	while (*tail)
		tail = &(*tail)->next;
	*tail = mcp;
	mc->invocation = true;
	*connp = mcp->conn;
	mcp_startup_begin(mcp);
	return 0;
err:
	return -1;
}

/*
 * Synchronous compatibility wrapper: start every server, pump the shared loop
 * until all have settled, then publish their tools. A live catalogue is reused.
 */
int fyai_mcp_refresh(struct fyai_mcp *mc)
{
	struct fyai_event_loop *el;

	if (mc->conns && fy_is_valid(mc->tools)) {
		if (mc->cfg->mcp_logging)
			(void)fyai_log_generic(fyai_mcp_state_ctx(mc), "mcp", fy_mapping(
				"event", "reuse", "tools",
				(long long)fy_len(mc->tools)));
		return 0;
	}
	if (fyai_mcp_start(mc))
		return -1;
	el = fyai_loop_get(mc->loop);
	while (el && !fyai_mcp_settled(mc)) {
		if ((mc->loop && mc->loop->interrupt_pending) ||
		    fyai_event_loop_step(el, -1) < 0)
			break;
	}
	fyai_mcp_publish_tools(fyai_mcp_state_ctx(mc));
	return 0;
}

enum fyai_mcp_call_state {
	FYAIMCS_WAITING,		/* target server not yet READY; parked */
	FYAIMCS_PRIMARY,
	FYAIMCS_RECOVER_INITIALIZE,
	FYAIMCS_RECOVER_NOTIFY,
	FYAIMCS_CANCELLING,		/* cancel recorded; the phase is dropped */
	FYAIMCS_COMPLETED,
	FYAIMCS_CANCELLED,
	FYAIMCS_FAILED,
};

/* Poll interval while a call is parked waiting for its server to settle. */
#define FYAI_MCP_CALL_WAIT_TICK_MS 50

struct fyai_mcp_call_request {
	struct fyai_mcp *owner;
	struct fyai_mcp_ctx *mcp;
	struct jsonrpc_request *req;
	struct fyai_event_source *wait_timer;	/* armed while FYAIMCS_WAITING */
	int wait_remaining_ms;			/* park budget left */
	fyai_mcp_call_complete_fn complete;
	void *userdata;
	char *tool_name;
	const char *method;
	bool raw_result;
	char *params_text;
	long long id;			/* the tools/call id, reused across retry */
	fy_generic result;
	enum fyai_mcp_call_state state;
	bool result_ok;
	bool auth_retry_done;
	char *bind_error;	/* a bound action that cannot be called */
	char *event_id;		/* the event that a bound action answers */
};

/* The component that this request stands on. */
static inline struct fyai_mcp *fyai_mcp_call_request_owner(const struct fyai_mcp_call_request *o)
{
	return o->owner;
}

static bool
fyai_mcp_call_state_final(enum fyai_mcp_call_state state)
{
	return state == FYAIMCS_COMPLETED ||
	       state == FYAIMCS_CANCELLED ||
	       state == FYAIMCS_FAILED;
}

static struct fyai_mcp_ctx *
mcp_find_tool_server(struct fyai_mcp *mc, const char *name,
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
	for (mcp = mc->conns; mcp; mcp = mcp->next)
		if (strlen(mcp->name) == server_len &&
		    !strncmp(mcp->name, server_name, server_len))
			break;
	if (mcp && tool_namep)
		*tool_namep = sep + 2;
	return mcp;
}

static fy_generic
mcp_tool_output(struct fyai_mcp *mc, fy_generic result)
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
			output = fy_append(mc->transient->gb, output, text);
	}
	if (fy_len(output) == 1)
		return fy_get(output, 0);
	if (fy_len(output))
		return output;
	return fy_gb_internalize(mc->transient->gb, result);
}

static void
fyai_mcp_call_finish(struct fyai_mcp_call_request *request,
		     enum fyai_mcp_call_state state,
		     fy_generic result, bool ok)
{
	request->state = state;
	request->result = result;
	request->result_ok = ok;
	if (state == FYAIMCS_COMPLETED && ok && request->event_id)
		mcp_event_answered(request->owner, request->mcp->name,
				   request->event_id);
	if (request->complete)
		request->complete(request, request->userdata);
}

static void fyai_mcp_call_req_done(struct jsonrpc_request *req, void *userdata);

/* Submit the request for the current call phase. */
static int
fyai_mcp_call_submit_phase(struct fyai_mcp_call_request *request)
{
	struct fyai_mcp *mc = fyai_mcp_call_request_owner(request);
	const char *method;
	fy_generic params;
	long long id;
	bool notification = false;

	switch (request->state) {
	case FYAIMCS_PRIMARY:
		method = request->method;
		id = request->id;
		params = parse_json_string(mc->transient->gb,
					   request->params_text);
		break;
	case FYAIMCS_RECOVER_INITIALIZE:
		method = "initialize";
		id = jsonrpc_conn_next_id(request->mcp->conn);
		params = fy_mapping(mc->transient->gb,
			"protocolVersion", request->mcp->protocol_version,
			"capabilities", mcp_client_capabilities(request->mcp,
						mc->transient->gb),
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
	fyai_diag_check(mc->diag, fy_is_valid(params), err_out,
			 "MCP call parameters are invalid");
	request->req = jsonrpc_request_submit(request->mcp->conn, method,
					      params, id, notification,
					      fyai_mcp_call_req_done, request);
	fyai_diag_check(mc->diag, request->req, err_out,
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
	struct fyai_mcp *mc = fyai_mcp_call_request_owner(request);
	char msg[160];

	if (request->state == FYAIMCS_CANCELLING) {
		fyai_mcp_call_wait_disarm(request);
		fyai_mcp_call_finish(request, FYAIMCS_CANCELLED,
			fy_value(mc->transient->gb, "tool error: interrupted"),
			false);
		return FYAIEA_CONTINUE;
	}
	/* The event loop reports binding errors after submission returns. */
	if (request->bind_error) {
		fyai_mcp_call_wait_disarm(request);
		fyai_mcp_call_finish(request, FYAIMCS_FAILED,
			fy_value(mc->transient->gb, request->bind_error),
			false);
		return FYAIEA_CONTINUE;
	}
	switch (request->mcp->state) {
	case MCP_SRV_READY:
		fyai_mcp_call_wait_disarm(request);
		request->state = FYAIMCS_PRIMARY;
		if (fyai_mcp_call_submit_phase(request))
			fyai_mcp_call_finish(request, FYAIMCS_FAILED,
				fy_value(mc->transient->gb,
					 "tool error: MCP call failed"),
				false);
		break;
	case MCP_SRV_FAILED:
		fyai_mcp_call_wait_disarm(request);
		snprintf(msg, sizeof(msg),
			 "tool error: MCP server '%s' is unavailable",
			 request->mcp->name);
		fyai_mcp_call_finish(request, FYAIMCS_FAILED,
				     fy_value(mc->transient->gb, msg), false);
		break;
	case MCP_SRV_CONNECTING:
		request->wait_remaining_ms -= FYAI_MCP_CALL_WAIT_TICK_MS;
		if (request->wait_remaining_ms <= 0) {
			fyai_mcp_call_wait_disarm(request);
			snprintf(msg, sizeof(msg), "tool error: MCP server '%s' "
				 "did not become ready", request->mcp->name);
			fyai_mcp_call_finish(request, FYAIMCS_FAILED,
					     fy_value(mc->transient->gb, msg),
					     false);
		}
		break;
	}
	return FYAIEA_CONTINUE;
}

/* Start a call when its server is ready. Poll all other server states. */
static int fyai_mcp_call_gate(struct fyai_mcp_call_request *request)
{
	struct fyai_mcp *mc = fyai_mcp_call_request_owner(request);
	struct fyai_event_loop *el;
	int rc;

	if (request->mcp->state == MCP_SRV_READY && !request->bind_error) {
		request->state = FYAIMCS_PRIMARY;
		return fyai_mcp_call_submit_phase(request);
	}
	el = fyai_loop_get(mc->loop);
	fyai_diag_check(mc->diag, el, err_out,
			 "could not acquire the application event loop");
	request->state = FYAIMCS_WAITING;
	request->wait_remaining_ms = request->mcp->timeout > 0 ?
		request->mcp->timeout * 1000 : 30000;
	rc = fyai_event_add_timer(el, 0, FYAI_MCP_CALL_WAIT_TICK_MS,
				  fyai_mcp_call_wait_tick, request,
				  &request->wait_timer);
	fyai_diag_check(mc->diag, !rc, err_out,
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
	struct fyai_mcp *mc = fyai_mcp_call_request_owner(request);
	fy_generic result = jsonrpc_request_result(req);
	bool ok = jsonrpc_request_ok(req);
	long status = jsonrpc_request_http_status(req);
	CURLcode code = jsonrpc_request_curl_code(req);
	const char *protocol;
	const char *challenge;
	const char *response_body;
	const char *tool_error;
	const char *encoded;
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
				      "%s failed (HTTP %ld): %.512s",
				      request->method, status, response_body);
		else if (status || code != CURLE_OK)
			mcp_set_error(request->mcp,
				      "%s failed: %s (HTTP %ld)",
				      request->method, curl_easy_strerror(code),
				      status);
		else
			mcp_set_error(request->mcp,
				      "%s failed: %s transport error",
				      request->method,
				      mcp_transport_name(request->mcp->transport));
	}
	jsonrpc_request_destroy(req);
	request->req = NULL;

	if (request->state == FYAIMCS_CANCELLING) {
		fyai_mcp_call_finish(request, FYAIMCS_CANCELLED,
			fy_value(mc->transient->gb, "tool error: interrupted"),
			false);
		return;
	}
	if (ok) {
		switch (request->state) {
		case FYAIMCS_PRIMARY:
			result_error = !request->raw_result &&
				fy_get(result, "isError", false);
			if (!request->raw_result)
				result = mcp_tool_output(mc, result);
			else {
				encoded = emit_json_string(mc->transient->gb,
							   result);
				if (!encoded || strlen(encoded) > 65536) {
					result_error = true;
					result = fy_value(mc->transient->gb,
						"tool error: MCP resource response "
						"exceeds 64 KiB");
				}
			}
			fyai_mcp_call_finish(request,
				result_error ? FYAIMCS_FAILED :
				FYAIMCS_COMPLETED, result, !result_error);
			return;
		case FYAIMCS_RECOVER_INITIALIZE:
			protocol = fy_get(result, "protocolVersion", "");
			if (*protocol) {
				protocol = fy_gb_intern_string(mc->cfg->gb,
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
		fyai_diag_reset(mc->diag);
		request->mcp->oauth_force_refresh = true;
		if (request->mcp->oauth_refresh_timer) {
			fyai_event_source_remove(
				request->mcp->oauth_refresh_timer);
			request->mcp->oauth_refresh_timer = NULL;
		}
		mcp_oauth_refresh_release(request->mcp);
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
		fyai_diag_type(mc->diag, FYAIET_ERROR, "MCP %s failed: %s",
			   request->mcp->name,
			   request->mcp->last_error ?
			   request->mcp->last_error : "unknown error");
failed:
	tool_error = request->mcp->last_error ?
		fy_sprintfa("tool error: %s", request->mcp->last_error) :
		"tool error: MCP call failed";
	fyai_mcp_call_finish(request, FYAIMCS_FAILED,
		fy_value(mc->transient->gb, tool_error),
		false);
}

struct fyai_mcp_call_request *
fyai_mcp_call_submit(struct fyai_mcp *mc, const char *name,
		     fy_generic args, fyai_mcp_call_complete_fn complete,
		     void *userdata)
{
	struct fyai_mcp_call_request *request;
	struct fyai_mcp_ctx *mcp;
	fy_generic params, bound;
	const char *params_text;
	const char *tool_name;
	const char *event_id = NULL;
	char *bind_error = NULL;
	int rc;

	tool_name = NULL;
	mcp = mcp_find_tool_server(mc, name, &tool_name);
	if (!mcp)
		return NULL;
	if (fyai_mcp_deferred(mc) && !fyai_mcp_tool_active(mc, name))
		return NULL;
	if (mcp_action_bound(mcp, tool_name)) {
		bound = mcp_action_args(mcp, mc->transient->gb, tool_name,
					args, &event_id, &bind_error);
		if (fy_is_valid(bound))
			args = bound;
	}
	params = fy_mapping(mc->transient->gb,
			    "name", tool_name, "arguments", args);
	params_text = emit_json_string(mc->transient->gb, params);
	fyai_diag_check(mc->diag, params_text, err_out,
			 "could not encode MCP call parameters");
	request = calloc(1, sizeof(*request));
	fyai_diag_check(mc->diag, request, err_out, "out of memory");
	request->owner = mc;
	request->mcp = mcp;
	request->complete = complete;
	request->userdata = userdata;
	request->tool_name = strdup(tool_name);
	request->method = "tools/call";
	request->params_text = strdup(params_text);
	request->id = jsonrpc_conn_next_id(mcp->conn);
	request->result = fy_invalid;
	request->state = FYAIMCS_PRIMARY;
	request->event_id = event_id ? strdup(event_id) : NULL;
	request->bind_error = bind_error;
	bind_error = NULL;
	fyai_diag_check(mc->diag, request->tool_name && request->params_text &&
			 (!event_id || request->event_id),
			 err_free, "out of memory");
	if (mc->cfg->mcp_logging)
		(void)fyai_log_generic(fyai_mcp_state_ctx(mc), "mcp", fy_mapping(
			"event", "tool_call", "server", mcp->name,
			"tool", tool_name));
	rc = fyai_mcp_call_gate(request);
	fyai_diag_check(mc->diag, !rc, err_free,
			 "could not submit MCP call");
	return request;

err_free:
	fyai_mcp_call_destroy(request);
err_out:
	free(bind_error);
	return NULL;
}

void fyai_mcp_call_cancel(struct fyai_mcp_call_request *request)
{
	if (!request || fyai_mcp_call_state_final(request->state))
		return;
	request->state = FYAIMCS_CANCELLING;
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
	free(request->bind_error);
	free(request->event_id);
	free(request);
}

struct fyai_mcp_call_request *
fyai_mcp_resource_submit(struct fyai_mcp *mc, fy_generic args,
			 fyai_mcp_call_complete_fn complete, void *userdata)
{
	struct fyai_mcp_call_request *request;
	struct fyai_mcp_ctx *mcp;
	fy_generic params;
	const char *server = fy_get(args, "server", "");
	const char *operation = fy_get(args, "operation", "");
	const char *uri = fy_get(args, "uri", "");
	const char *cursor = fy_get(args, "cursor", "");
	const char *method, *json;

	mcp = mcp_find_server(mc, server);
	if (!mcp || mcp->state == MCP_SRV_FAILED ||
	    !mcp->resources_supported)
		return NULL;
	if (!strcmp(operation, "list"))
		method = "resources/list";
	else if (!strcmp(operation, "templates"))
		method = "resources/templates/list";
	else if (!strcmp(operation, "read") && *uri)
		method = "resources/read";
	else
		return NULL;
	params = !strcmp(operation, "read") ?
		fy_mapping(mc->transient->gb, "uri", uri) :
		*cursor ? fy_mapping(mc->transient->gb, "cursor", cursor) :
		fy_map_empty;
	json = emit_json_string(mc->transient->gb, params);
	if (!json)
		return NULL;
	request = calloc(1, sizeof(*request));
	if (!request)
		return NULL;
	request->owner = mc;
	request->mcp = mcp;
	request->method = method;
	request->raw_result = true;
	request->params_text = strdup(json);
	request->id = jsonrpc_conn_next_id(mcp->conn);
	request->result = fy_invalid;
	request->complete = complete;
	request->userdata = userdata;
	if (!request->params_text || fyai_mcp_call_gate(request))
		goto err_out;
	return request;
err_out:
	fyai_mcp_call_destroy(request);
	return NULL;
}

/*
 * A resource request is submitted by the tool group, which runs it beside the
 * other calls. The registry entry only names the tool.
 */
static fy_generic mcp_resource_tool(struct fyai_ctx *ctx, fy_generic args,
				    bool *okp)
{
	(void)args;
	*okp = false;
	return fy_value(ctx->transient->gb,
			"tool error: mcp_resource runs in a tool group");
}

static void mcp_delete_complete(struct fyai_curl_transfer *xfer, void *userdata)
{
	struct fyai_mcp_ctx *mcp = userdata;
	struct fyai_mcp *mc = mcp_owner(mcp);
	CURLcode rc = fyai_curl_collect(xfer);
	long status = 0;

	curl_easy_getinfo(mcp->curl, CURLINFO_RESPONSE_CODE, &status);
	fyai_curl_transfer_destroy(xfer);
	mcp->del_xfer = NULL;
	/* Teardown failure is nonfatal; the server will time the session out. */
	if (rc != CURLE_OK || status < 200 || status >= 300)
		fyai_diag_type(mc->diag, FYAIET_WARNING, "MCP %s session delete failed: %s (HTTP %ld)",
			     mcp->name, curl_easy_strerror(rc), status);
	if (mc->cfg->mcp_logging)
		(void)fyai_log_generic(fyai_mcp_state_ctx(mc), "mcp", fy_mapping(
			"event", "session_delete", "server", mcp->name,
			"http_status", status, "curl_code", (long long)rc));
	mcp->session_id = NULL;
	mcp->del_done = true;
}

/* Submit the session DELETE. Returns true if a transfer is now in flight. */
static bool mcp_delete_session_begin(struct fyai_mcp *mc,
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
	mcp->del_xfer = fyai_curl_submit(mc->link, mcp->curl, mcp_delete_complete,
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
static void mcp_teardown_begin(struct fyai_mcp *mc, struct fyai_mcp_ctx *mcp)
{
	struct fyai_event_loop *el = fyai_loop_get(mc->loop);
	int status;

	mcp->del_done = !mcp_delete_session_begin(mc, mcp);

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
	assert(el);
	/* The child leads its own group; stop the descendants with it. */
	if (!fyai_event_add_child_terminate_group(el, mcp->pid, 1000, 500,
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
	(void)kill(-mcp->pid, SIGKILL);
	if (!fyai_event_add_child(el, mcp->pid, mcp_term_complete,
					mcp, &mcp->term_src)) {
		mcp->term_done = false;
		return;
	}
	fyai_diag_type(mc->diag, FYAIET_WARNING, "MCP %s child could not be watched for shutdown",
		     mcp->name);
	while (waitpid(mcp->pid, &status, 0) < 0 && errno == EINTR)
		;
	mcp->pid = -1;
}

static bool mcp_teardown_settled(const struct fyai_mcp *mc)
{
	const struct fyai_mcp_ctx *mcp;

	for (mcp = mc->conns; mcp; mcp = mcp->next)
		if (!mcp->del_done || !mcp->term_done)
			return false;
	return true;
}

/* Shut the server down: close its pipes so it sees EOF and leaves on its own,
 * then escalate through SIGTERM to SIGKILL if it does not. */
static void mcp_stdio_stop(struct fyai_mcp *mc, struct fyai_mcp_ctx *mcp)
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

	el = fyai_loop_get(mc->loop);
	assert(el);
	if (fyai_event_child_terminate_group(el, mcp->pid, 1000, 500, NULL)) {
		/* Termination failed; reap the child directly. */
		kill(-mcp->pid, SIGKILL);
		while (waitpid(mcp->pid, &status, 0) < 0 && errno == EINTR)
			;
	}
	mcp->pid = -1;
}

int fyai_mcp_stop(struct fyai_mcp *mc)
{
	struct fyai_mcp_ctx *mcp;

	if (!mc || !mc->conns)
		return 0;
	if (mc->stopping)
		return 0;
	mc->stopping = true;

	/* Cancel any in-flight startup, then fire every server's teardown so
	 * they drain concurrently. */
	for (mcp = mc->conns; mcp; mcp = mcp->next) {
		if (mcp->oauth_refresh_timer) {
			fyai_event_source_remove(mcp->oauth_refresh_timer);
			mcp->oauth_refresh_timer = NULL;
		}
		mcp_oauth_discovery_destroy(mcp->oauth_discovery);
		mcp_oauth_refresh_release(mcp);
		if (mcp->startup) {
			struct mcp_startup *su = mcp->startup;

			if (su->timer)
				fyai_event_source_remove(su->timer);
			jsonrpc_request_destroy(su->req);
			free(su->cursor);
			free(su);
			mcp->startup = NULL;
		}
		mcp_teardown_begin(mc, mcp);
	}
	return 0;
}

bool fyai_mcp_stop_done(const struct fyai_mcp *mc)
{
	return !mc || !mc->conns ||
	       (mc->stopping &&
		mcp_teardown_settled(mc));
}

void fyai_mcp_stop_finish(struct fyai_mcp *mc)
{
	struct fyai_mcp_ctx *mcp, *next;

	if (!mc || !fyai_mcp_stop_done(mc))
		return;
	for (mcp = mc->conns; mcp; mcp = next) {
		next = mcp->next;
		if (mc->cfg->mcp_logging)
			(void)fyai_log_generic(fyai_mcp_state_ctx(mc), "mcp", fy_mapping(
				"event", "disconnect", "server", mcp->name));
		curl_slist_free_all(mcp->del_headers);
		free(mcp->del_resp.data);
		mcp_events_release(mcp);
		jsonrpc_conn_destroy(mcp->conn);
		if (mcp->curl)
			curl_easy_cleanup(mcp->curl);
		free(mcp->oauth_resource_metadata);
		mcp_oauth_secret_free(&mcp->oauth_client_secret);
		mcp_oauth_secret_free(&mcp->oauth_access_token);
		mcp_oauth_secret_free(&mcp->oauth_refresh_token);
		mcp_oauth_secret_free(&mcp->oauth_bearer);
		free(mcp);
	}
	mc->conns = NULL;
	mc->stopping = false;
}

void fyai_mcp_cleanup(struct fyai_mcp *mc)
{
	struct fyai_event_loop *el;
	int rc;

	if (!mc || !mc->conns)
		return;
	if (fyai_mcp_stop(mc))
		return;
	el = fyai_loop_get(mc->loop);
	while (el && !fyai_mcp_stop_done(mc)) {
		rc = fyai_event_loop_step(el, -1);
		if (rc < 0)
			return;
	}
	fyai_mcp_stop_finish(mc);
}

void fyai_mcp_state_init(struct fyai_mcp *mcp, struct fyai_loop *loop,
			 struct fyai_cred *cred, struct fyai_transient *transient,
			 struct fyai_cfg *cfg, struct fyai_arena *arena,
			 struct fyai_link *link, struct fyai_diag *diag)
{
	memset(mcp, 0, sizeof(*mcp));
	mcp->loop = loop;
	mcp->cred = cred;
	mcp->transient = transient;
	mcp->cfg = cfg;
	mcp->arena = arena;
	mcp->link = link;
	mcp->diag = diag;
	/* Not the all-zero word: that is an empty sequence. */
	mcp->tools = fy_invalid;
	mcp->active_tools = fy_invalid;
}
