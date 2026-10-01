/*
 * fyai_cmd_auth.c - handlers of the auth command
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai.h"
#include "fyai_cmd.h"
#include "fyai_cmd_int.h"
#include "fyai_auth.h"
#include "fyai_tools.h"
#include "fyai_transport_boot.h"
#include "fyai_ui.h"
#include "fyai_sink.h"

#define FYAI_MODULE FYAIEM_AUTH

static int auth_provider(struct fyai_cmd_call *call)
{
	const char *provider = fyai_cmd_arg_str(call, "provider");

	fyai_error_check(call->ctx, provider && !strcmp(provider, "openai"),
			 err, "provider '%s' is not supported yet",
			 provider ? provider : "");
	return 0;
err:
	return -1;
}

/*
 * An isolated image never loads the login store: the transport runs the
 * command, and this image presents what it returns.
 */
static int auth_forward(struct fyai_cmd_call *call, const char *sub,
			fy_generic *data)
{
	const char *words[3] = { "auth", fyai_cmd_arg_str(call, "provider"), sub };

	return fyai_transport_command(call->ctx, call->gb, words, 3,
				      (int)call->format, data);
}

/*
 * The login and the health of the credentials. info, and a machine format,
 * add the details of the account.
 */
int fyai_cmd_auth_status(struct fyai_cmd_call *call, fy_generic *result)
{
	bool detail;

	if (auth_provider(call))
		return -1;
	detail = fy_equal(fy_get(call->def, "command", fy_invalid), "info") ||
		 call->format != FYAI_CMD_OUT_MARKDOWN;
	if (call->ctx->tclient) {
		fy_generic data;

		if (auth_forward(call, detail ? "info" : "status", &data))
			return -1;
		*result = fyai_auth_status_overlay(call->ctx, call->gb, data);
		return fy_is_valid(*result) ? 0 : -1;
	}
	*result = fyai_auth_status_data(call->ctx, call->gb, detail);
	fyai_error_check(call->ctx, fy_is_valid(*result), err,
			 "auth: cannot read the status");
	return 0;
err:
	return -1;
}

/* Report recorded usage and the available plan information. */
int fyai_cmd_auth_usage(struct fyai_cmd_call *call, fy_generic *result)
{
	if (fy_is_valid(fy_get(call->args, "provider", fy_invalid)) &&
	    auth_provider(call))
		return -1;
	return fyai_auth_usage(call->ctx, call->gb,
			       call->format != FYAI_CMD_OUT_MARKDOWN, result);
}

int fyai_cmd_auth_logout(struct fyai_cmd_call *call, fy_generic *result)
{
	if (auth_provider(call))
		return -1;
	if (call->ctx->tclient) {
		fy_generic data;

		if (auth_forward(call, "logout", &data))
			return -1;
	} else if (fyai_auth_logout(call->ctx))
		return -1;
	*result = fy_mapping(call->gb, "logged_out", true);
	return 0;
}

static void auth_login_complete(struct fyai_auth_login_request *request,
				void *userdata)
{
	struct fyai_cmd_call *call = userdata;
	int rc;

	if (!fyai_auth_login_done(request))
		return;
	rc = fyai_auth_login_collect(request);
	if (rc)
		fyai_error(call->ctx, "login failed");
	fyai_cmd_done(call, rc ? -1 : 0,
		      rc ? fy_invalid : fy_mapping(call->gb, "login", true));
}

static void auth_login_cancel(struct fyai_cmd_call *call)
{
	fyai_auth_login_cancel(call->priv);
}

/* The request outlives its completion callback; release it with the call. */
static void auth_login_cleanup(struct fyai_cmd_call *call)
{
	fyai_auth_login_destroy(call->priv);
	call->priv = NULL;
}

/*
 * Browser authorization runs on the event loop. A manual callback uses the
 * same state machine after it reads the complete redirect URL.
 */
static void auth_login_input(struct fyai_cmd_call *call, const char *line)
{
	if (fyai_auth_login_redirect(call->priv, line))
		fyai_ui_diag_drain(call->ctx, "error");
}

/*
 * The login of an isolated run is a program of its own. It receives the
 * tokens of the sign-in, and the image of a run holds none: the transport
 * reads the store that the program writes. The tile is its terminal, so the
 * URL, the browser and a pasted redirect belong to it.
 */
struct auth_child {
	struct fyai_cmd_call *call;
	struct fyai_shell_session *session;
};

static int auth_quote(struct response_buffer *line, const char *s)
{
	int rc = response_buffer_append(line, "'");

	for (; !rc && *s; s++)
		rc = *s == '\'' ? response_buffer_append(line, "'\\''") :
		     response_buffer_append_data(line, s, 1);
	return rc ? rc : response_buffer_append(line, "'");
}

static void auth_child_exited(void *userdata, int exit_code, int signal)
{
	struct auth_child *child = userdata;
	struct fyai_cmd_call *call = child->call;

	child->session = NULL;
	if (exit_code || signal)
		fyai_error(call->ctx, "login failed; its output is in its tile");
	fyai_cmd_done(call, exit_code || signal ? -1 : 0,
		      exit_code || signal ? fy_invalid :
		      fy_mapping(call->gb, "login", true));
}

static void auth_child_cancel(struct fyai_cmd_call *call)
{
	struct auth_child *child = call->priv;

	if (child && child->session)
		fyai_tools_user_program_close(child->session);
}

static void auth_child_cleanup(struct fyai_cmd_call *call)
{
	struct auth_child *child = call->priv;

	if (!child)
		return;
	if (child->session)
		fyai_tools_user_program_forget(child->session);
	free(child);
	call->priv = NULL;
}

static int auth_login_child(struct fyai_cmd_call *call)
{
	struct response_buffer line = {0};
	struct auth_child *child;
	const char *account = fyai_cmd_arg_str(call, "account");
	char exe[4096];
	ssize_t n;
	int rc;

	n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	fyai_error_check(call->ctx, n > 0, err, "login: cannot find this program: %s",
			 strerror(errno));
	exe[n] = '\0';
	child = calloc(1, sizeof(*child));
	fyai_error_check(call->ctx, child, err, "login: out of memory");
	child->call = call;
	rc = auth_quote(&line, exe) ||
	     response_buffer_append(&line, " auth openai login");
	if (!rc && fyai_cmd_arg_bool(call, "no_browser"))
		rc = response_buffer_append(&line, " --no-browser");
	if (!rc && fyai_cmd_arg_bool(call, "manual"))
		rc = response_buffer_append(&line, " --manual");
	if (!rc && fyai_cmd_arg_bool(call, "device_code"))
		rc = response_buffer_append(&line, " --device-code");
	if (!rc && fyai_cmd_arg_bool(call, "new_account"))
		rc = response_buffer_append(&line, " --new-account");
	if (!rc && account && *account)
		rc = response_buffer_append(&line, " --account ") ||
		     auth_quote(&line, account);
	fyai_error_check(call->ctx, !rc, err_child, "login: out of memory");
	call->priv = child;
	call->cleanup = auth_child_cleanup;
	child->session = fyai_tools_config_program(call->ctx, line.data, "auth",
						   fy_seq_empty,
						   auth_child_exited, child);
	free(line.data);
	if (!child->session) {
		call->priv = NULL;
		free(child);
		return -1;	/* fyai_tools_config_program() says why */
	}
	call->cancel = auth_child_cancel;
	return FYAI_CMD_PENDING;
err_child:
	free(line.data);
	free(child);
err:
	return -1;
}

int fyai_cmd_auth_login(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_auth_login_request *request;

	if (auth_provider(call))
		return -1;
	if (call->ctx->tclient && call->surface == FYAI_CMD_SESSION)
		return auth_login_child(call);
	if (fyai_cmd_arg_bool(call, "manual") && call->surface == FYAI_CMD_CLI) {
		*result = fy_invalid;
		return fyai_auth_login(call->ctx, false, false, true,
			fyai_cmd_arg_str(call, "account"), fyai_cmd_arg_bool(call, "new_account"));
	}
	request = fyai_auth_login_submit(call->ctx,
					 fyai_cmd_arg_bool(call, "device_code"),
					 fyai_cmd_arg_bool(call, "no_browser") ||
					 fyai_cmd_arg_bool(call, "manual"),
					 fyai_cmd_arg_str(call, "account"),
					 fyai_cmd_arg_bool(call, "new_account"),
					 auth_login_complete, call);
	fyai_error_check(call->ctx, request, err,
			 "could not start authentication login");
	call->priv = request;
	call->cleanup = auth_login_cleanup;
	if (fyai_cmd_arg_bool(call, "manual")) {
		call->input = auth_login_input;
		fyai_result(call->ctx, "Paste the complete redirect URL into the input area. ^C or Escape cancels.\n");
	}
	/* A flow that failed at once has completed already. */
	if (call->done) {
		*result = call->result;
		return call->rc;
	}
	call->cancel = auth_login_cancel;
	return FYAI_CMD_PENDING;
err:
	return -1;
}

int fyai_cmd_auth_accounts(struct fyai_cmd_call *call, fy_generic *result)
{
	if (auth_provider(call))
		return -1;
	if (call->ctx->tclient)
		return auth_forward(call, "accounts", result);
	*result = fyai_auth_accounts_data(call->ctx, call->gb);
	return fy_is_valid(*result) ? 0 : -1;
}
