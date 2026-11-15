/*
 * fyai_cmd_mcp.c - handlers of the mcp command
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai.h"
#include "fyai_cmd.h"
#include "fyai_cmd_int.h"
#include "fyai_config.h"
#include "fyai_mcp_import.h"
#include "fyai_tools.h"

#define FYAI_MODULE FYAIEM_UNKNOWN

/*
 * The live connections present themselves. With none, the result is the
 * configured servers.
 */
int fyai_cmd_mcp_status(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_cfg *cfg = fyai_cmd_call_ctx(call)->cfg;
	struct fy_generic_builder *gb = call->gb;
	fy_generic rows, key, server, command;
	const char *name;

	/* The live connections, when the session has started them. */
	if (fyai_cmd_call_ctx(call)->mcp->conns) {
		*result = fyai_mcp_status_data(fyai_cmd_call_ctx(call), gb);
		call->renderopts = fy_mapping(gb,
			"title", "MCP endpoints",
			"keys", fy_sequence(gb, "server", "state", "transport",
					    "auth", "tools", "expires",
					    "endpoint", "error"),
			"columns", fy_mapping(gb,
				"error", fy_mapping(gb, "name", "Last error")));
		fyai_error_check(fyai_cmd_call_ctx(call), fy_is_valid(*result), err,
				 "mcp: cannot build the status");
		return 0;
	}
	rows = fy_seq_empty;
	fy_foreach(key, cfg->mcp_servers) {
		name = fy_castp(&key, "");
		server = fy_get(cfg->mcp_servers, key, fy_invalid);
		command = fy_get(server, "command", fy_invalid);
		rows = fy_append(gb, rows, fy_mapping(gb,
			"server", name,
			"enabled", (bool)fy_get(server, "enabled", true),
			"transport", fy_is_string(command) ? "stdio" : "http",
			"target", fy_is_string(command) ? command :
				  fy_get(server, "endpoint", fy_value(gb,
								      "-"))));
	}
	if (!fy_len(rows))
		rows = fy_sequence(gb, fy_mapping(gb, "server", "default",
			"enabled", cfg->mcp_enabled, "transport", "http",
			"target", cfg->mcp_endpoint ? cfg->mcp_endpoint : "-"));
	call->renderopts = fy_mapping(gb,
		"title", "MCP servers",
		"preamble", fy_sprintfa("MCP is %s; protocol %s, time limit "
					"%d ms.", cfg->mcp_enabled ? "on" :
					"off", cfg->mcp_protocol_version ?
					cfg->mcp_protocol_version : "-",
					cfg->mcp_timeout));
	*result = rows;
	fyai_error_check(fyai_cmd_call_ctx(call), fy_is_valid(rows) &&
			 fy_is_valid(call->renderopts), err,
			 "mcp: cannot build the status");
	return 0;
err:
	return -1;
}

int fyai_cmd_mcp_login(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *name = fyai_cmd_arg_str(call, "name");
	bool login;

	login = fy_equal(fy_get(call->def, "command", fy_invalid), "login");
	if (login ? fyai_mcp_login(fyai_cmd_call_ctx(call), name) :
		    fyai_mcp_logout(fyai_cmd_call_ctx(call), name))
		return -1;
	*result = fy_mapping(call->gb, "server", name, "login", login);
	return 0;
}

/* Turn the MCP servers of the session on or off, and store the setting. */
int fyai_cmd_mcp_enable(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = fyai_cmd_call_ctx(call);
	struct fyai_cfg *cfg = ctx->cfg;
	bool on, was;

	on = fy_equal(fy_get(call->def, "command", fy_invalid), "on");
	was = cfg->mcp_enabled;
	*result = fy_mapping(call->gb, "state", on ? "enabled" : "disabled",
			     "changed", was != on, "unchanged", was == on);
	if (was == on)
		return 0;
	cfg->mcp_enabled = on;
	if (fyai_config_set(ctx, "mcp/enabled", on ? "true" : "false"))
		fyai_warning(ctx, "mcp/enabled: could not persist to config");
	if (fyai_request_state_apply(ctx)) {
		cfg->mcp_enabled = was;
		return -1;
	}
	return 0;
}

int fyai_cmd_mcp_import_client(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_mcp_args *args = &fyai_cmd_call_ctx(call)->cfg->cmd.args.mcp;
	const char *scopes[64];
	fy_generic s;
	size_t n;
	int rc;

	(void)result;
	n = 0;
	fy_foreach(s, fy_get(call->args, "scope", fy_invalid)) {
		fyai_error_check(fyai_cmd_call_ctx(call), n < ARRAY_SIZE(scopes), err,
				 "mcp: at most %zu scopes", ARRAY_SIZE(scopes));
		scopes[n++] = fy_gb_intern_string(call->gb, fy_castp(&s, ""));
	}
	memset(args, 0, sizeof(*args));
	args->name = fyai_cmd_arg_str(call, "name");
	args->file = fyai_cmd_arg_str(call, "file");
	args->endpoint = fyai_cmd_arg_str(call, "endpoint");
	args->secret_env = fyai_cmd_arg_str(call, "secret_env");
	args->scopes = scopes;
	args->scope_count = n;
	args->force = fyai_cmd_arg_bool(call, "force");
	rc = fyai_mcp_import_client(fyai_cmd_call_ctx(call));
	/* The scopes are on this stack. */
	args->scopes = NULL;
	args->scope_count = 0;
	return rc;
err:
	return -1;
}
