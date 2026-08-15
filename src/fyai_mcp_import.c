/*
 * fyai_mcp_import.c - import pre-registered OAuth clients for MCP
 *
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_TOOLS

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <curl/curl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <stdlib.h>
#include <string.h>

#include "commands.h"
#include "fyai_sink.h"
#include "fyai.h"
#include "fyai_config.h"
#include "fyai_mcp_import.h"
#include "fyai_secret.h"
#include "utils.h"

static fy_generic mcp_import_redirect(fy_generic client)
{
	fy_generic redirects;
	fy_generic item;
	CURLU *url;
	char *scheme;
	char *host;
	const char *value;

	redirects = fy_get(client, "redirect_uris", fy_invalid);
	if (!fy_is_sequence(redirects))
		return fy_invalid;
	fy_foreach(item, redirects) {
		value = fy_castp(&item, "");
		url = curl_url();
		scheme = NULL;
		host = NULL;
		if (!url)
			return fy_invalid;
		if (curl_url_set(url, CURLUPART_URL, value, 0) ||
		    curl_url_get(url, CURLUPART_SCHEME, &scheme, 0) ||
		    curl_url_get(url, CURLUPART_HOST, &host, 0))
			goto next;
		if (!strcmp(scheme, "http") &&
		    (!strcmp(host, "localhost") ||
		     !strcmp(host, "127.0.0.1"))) {
			curl_free(scheme);
			curl_free(host);
			curl_url_cleanup(url);
			return item;
		}
next:
		curl_free(scheme);
		curl_free(host);
		curl_url_cleanup(url);
	}
	return fy_invalid;
}

static int mcp_import_store_secret(struct fyai_ctx *ctx, const char *name,
				   const char *secret, char **oldp,
				   size_t *old_lenp, bool *had_oldp)
{
	char *stored;
	int rc;

	stored = strdup(fy_sprintfa("fyai:mcp-oauth-client/%s", name));
	fyai_error_check(ctx, stored, err_out,
			 "MCP OAuth import ran out of memory");
	rc = fyai_secret_kernel_get(stored, oldp, old_lenp);
	*had_oldp = rc == FYAI_SECRET_OK;
	fyai_error_check(ctx, rc == FYAI_SECRET_OK ||
			 rc == FYAI_SECRET_NOT_FOUND, err_free,
			 "MCP OAuth secret backend is unavailable; "
			 "use --secret-env");
	rc = fyai_secret_kernel_set(stored, secret, strlen(secret));
	fyai_error_check(ctx, rc == FYAI_SECRET_OK, err_free,
			 "MCP OAuth secret backend is unavailable; "
			 "use --secret-env");
	free(stored);
	return 0;

err_free:
	free(stored);
err_out:
	return -1;
}

static void mcp_import_restore_secret(const char *name, char *old,
				      size_t old_len, bool had_old)
{
	char *stored;

	stored = strdup(fy_sprintfa("fyai:mcp-oauth-client/%s", name));
	if (!stored)
		return;
	if (had_old)
		(void)fyai_secret_kernel_set(stored, old, old_len);
	else
		(void)fyai_secret_kernel_delete(stored);
	free(stored);
}

int fyai_mcp_import_client(struct fyai_ctx *ctx)
{
	struct fyai_mcp_args *args;
	struct stat st;
	struct fy_generic_builder *gb;
	fy_generic doc;
	fy_generic client;
	fy_generic scopes;
	fy_generic secret_ref;
	fy_generic auth_client;
	fy_generic auth;
	fy_generic server;
	fy_generic existing;
	fy_generic redirect_v;
	const char *client_id;
	const char *client_secret;
	const char *redirect;
	const char *secret_name;
	char *text;
	char *old_secret;
	char *key;
	size_t old_len;
	size_t text_len;
	size_t i;
	bool had_old;
	bool stored;
	int rc;

	args = &ctx->cfg->cmd.args.mcp;
	gb = ctx->gb;
	text = NULL;
	old_secret = NULL;
	old_len = 0;
	had_old = false;
	stored = false;
	fyai_error_check(ctx, gb, err_out, "no arena; run fyai init");
	rc = stat(args->file, &st);
	fyai_error_check(ctx, !rc && S_ISREG(st.st_mode), err_out,
			 "MCP OAuth client file is not a regular file");
	fyai_error_check(ctx, st.st_uid == getuid(), err_out,
			 "MCP OAuth client file is not owned by this user");
	if (st.st_mode & (S_IRWXG | S_IRWXO))
		fyai_warning(ctx,
			"MCP OAuth client file is accessible by other users");
	text = read_text_file(args->file);
	fyai_error_check(ctx, text, err_out,
			 "could not read MCP OAuth client file");
	doc = parse_json_string(gb, text);
	fyai_error_check(ctx, fy_is_mapping(doc), err_out,
			 "MCP OAuth client file is not valid JSON");
	client = fy_get(doc, "installed", fy_invalid);
	if (!fy_is_mapping(client))
		client = fy_get(doc, "web", fy_invalid);
	fyai_error_check(ctx, fy_is_mapping(client), err_out,
			 "MCP OAuth client file has no installed or web client");
	client_id = fy_get(client, "client_id", "");
	client_secret = fy_get(client, "client_secret", "");
	redirect_v = mcp_import_redirect(client);
	redirect = fy_castp(&redirect_v, "");
	fyai_error_check(ctx, *client_id && *client_secret && redirect, err_out,
			 "MCP OAuth client file is incomplete or has no "
			 "loopback redirect");
	existing = fy_get_at_pathstr(gb, ctx->arena_config,
		fy_sprintfa("/mcp/servers/%s", args->name));
	fyai_error_check(ctx, args->force ||
			 fy_is_invalid(existing), err_out,
			 "MCP server '%s' already exists; use --force",
			 args->name);

	if (args->secret_env) {
		secret_ref = fy_mapping(gb, "type", "env",
				       "value", args->secret_env);
	} else {
		rc = mcp_import_store_secret(ctx, args->name, client_secret,
					     &old_secret, &old_len,
					     &had_old);
		fyai_error_check(ctx, !rc, err_out,
				 "could not store MCP OAuth client secret");
		stored = true;
		secret_name = fy_sprintfa("mcp-oauth-client/%s", args->name);
		secret_ref = fy_mapping(gb, "type", "secret",
				       "value", secret_name);
	}
	scopes = fy_seq_empty;
	for (i = 0; i < args->scope_count; i++)
		scopes = fy_append(gb, scopes, args->scopes[i]);
	auth_client = fy_mapping(gb,
		"id", client_id,
		"secret", secret_ref,
		"redirect_uri", redirect,
		"token_endpoint_auth_method", "client_secret_post");
	auth = fy_mapping(gb,
		"type", "oauth",
		"allow_browser", false,
		"client", auth_client,
		"scopes", scopes,
		"authorization_params", fy_mapping(gb,
			"access_type", "offline",
			"prompt", "consent"));
	server = fy_mapping(gb, "endpoint", args->endpoint, "auth", auth);
	key = strdup(fy_sprintfa("mcp/servers/%s", args->name));
	fyai_error_check(ctx, key, err_restore,
			 "MCP OAuth import ran out of memory");
	rc = fyai_config_set_generic(ctx, key, server);
	free(key);
	fyai_error_check(ctx, !rc, err_restore,
			 "could not commit MCP OAuth server configuration");
	fyai_result(ctx, "mcp: imported OAuth client for %s\n", args->name);
	fyai_result(ctx, "mcp: enable browser login with:\n");
	fyai_result(ctx, "  fyai config set mcp/servers/%s/auth/allow_browser true\n",
	       args->name);
	fyai_result(ctx, "mcp: enable MCP with:\n");
	fyai_result(ctx, "  fyai config set mcp/enabled true\n");
	rc = 0;
	goto out;

err_restore:
	if (stored)
		mcp_import_restore_secret(args->name, old_secret, old_len,
					  had_old);
err_out:
	rc = -1;
out:
	fyai_secret_clear_and_free(&old_secret, &old_len);
	if (text) {
		text_len = strlen(text);
		fyai_secret_clear_and_free(&text, &text_len);
	}
	return rc;
}
