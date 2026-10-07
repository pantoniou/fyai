/*
 * fyai_cmd_branch.c - handlers of the branch, checkout, and model commands
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
#include "fyai_branch.h"
#include "fyai_session.h"
#include "fyai_agents.h"
#include "fyai_tools.h"
#include "fyai_ui.h"

#define FYAI_MODULE FYAIEM_UNKNOWN

int fyai_cmd_branch_list(struct fyai_cmd_call *call, fy_generic *result)
{
	*result = fyai_branch_list_data(call->ctx, call->gb,
					fyai_cmd_arg_str(call, "under"),
					fyai_cmd_arg_bool(call, "all"));
	fyai_error_check(call->ctx, fy_is_valid(*result), err,
			 "%s: cannot build the branch list", call->path);
	return 0;
err:
	return -1;
}

int fyai_cmd_branch_show(struct fyai_cmd_call *call, fy_generic *result)
{
	return fyai_branch_show_data(call->ctx, call->gb,
				     fyai_cmd_arg_str(call, "name"), result);
}

int fyai_cmd_branch_new(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *name = fyai_cmd_arg_str(call, "name");
	bool session = call->surface == FYAI_CMD_SESSION;
	int rc;

	/* A session goes on the branch it creates. */
	rc = fyai_branch_create(call->ctx, name, fyai_cmd_arg_str(call, "ref"),
				NULL, session);
	if (rc)
		return -1;
	*result = session ? fy_mapping(call->gb, "branch", name, "switched",
				       name) :
			    fy_mapping(call->gb, "branch", name);
	return 0;
}

int fyai_cmd_branch_delete(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *name = fyai_cmd_arg_str(call, "name");

	if (fyai_branch_delete(call->ctx, name,
			       fyai_cmd_arg_bool(call, "force")))
		return -1;
	*result = fy_mapping(call->gb, "branch", name);
	return 0;
}

int fyai_cmd_branch_rename(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *from = fyai_cmd_arg_str(call, "name");
	const char *to = fyai_cmd_arg_str(call, "new_name");

	if (fyai_branch_rename(call->ctx, from, to))
		return -1;
	*result = fy_mapping(call->gb, "from", from, "to", to);
	return 0;
}

int fyai_cmd_branch_describe(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *name = fyai_cmd_arg_str(call, "name");
	const char *text = fyai_cmd_arg_str(call, "text");

	if (fyai_branch_describe(call->ctx, name, text ? text : ""))
		return -1;
	*result = fy_mapping(call->gb, "branch", name, "description",
			     text ? text : "");
	return 0;
}

int fyai_cmd_branch_switch(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *name = fyai_cmd_arg_str(call, "name");
	struct fyai_branch b;
	bool found;
	int rc;

	found = fyai_branch_lookup(call->ctx->arena_branches, name, &b);
	rc = fyai_session_branch_switch(call->ctx, name, true, false);
	fyai_error_check(call->ctx, !rc, err, "branch: could not switch to "
			 "'%s'", name);
	*result = found ? fy_mapping(call->gb, "branch", name) :
			  fy_mapping(call->gb, "branch", name, "created", name);
	return 0;
err:
	return -1;
}

int fyai_cmd_branch_attach(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *name = fyai_cmd_arg_str(call, "name");

	(void)result;
	fyai_error_check(call->ctx, fyai_agents_zoom(call->ctx, name, true),
			 err, "branch attach: '%s' is not a reachable live "
			 "agent", name);
	return 0;
err:
	return -1;
}

int fyai_cmd_branch_detach(struct fyai_cmd_call *call, fy_generic *result)
{
	(void)result;
	fyai_agents_detach(call->ctx);
	return 0;
}

/*
 * A session never moves a reference: a historical one starts a new session
 * branch at that state. A named branch is switched to in place.
 */
static int checkout_session(struct fyai_cmd_call *call, const char *target,
			    bool create, const char *start,
			    const char **branchp, const char **createdp)
{
	struct fyai_ctx *ctx = call->ctx;
	char name[FYAI_BRANCH_NAME_MAX + 1], ref_name[FYAI_BRANCH_NAME_MAX + 1];
	long long n;
	int kind, rc;

	fyai_error_check(ctx, !fyai_ui_busy(ctx) && !fyai_tools_active(ctx) &&
			 !fyai_agents_attached(ctx), err,
			 "branch changes require idle model and tool work");
	if (create) {
		fyai_error_check(ctx, start, err,
				 "checkout: -b needs a start point in a session");
		rc = fyai_branch_create(ctx, target, start, NULL, false);
		fyai_error_check(ctx, !rc, err,
				 "checkout: could not create branch '%s'",
				 target);
		*branchp = target;
		*createdp = target;
	} else {
		fyai_error_check(ctx, !start, err,
				 "checkout: a start point needs -b");
		kind = fyai_ref_parse(target, ref_name, sizeof(ref_name), &n);
		fyai_error_check(ctx, kind >= 0, err,
				 "checkout: invalid reference '%s'", target);
		if (!kind) {
			*branchp = !strcmp(target, "HEAD") ?
				   fyai_ctx_head_branch(ctx) : target;
		} else {
			rc = fyai_branch_session_name(ctx->arena_branches, name,
						      sizeof(name));
			fyai_error_check(ctx, !rc, err,
					 "checkout: could not name a new "
					 "session");
			rc = fyai_branch_create(ctx, name, target, NULL, false);
			fyai_error_check(ctx, !rc, err,
					 "checkout: could not create branch "
					 "'%s'", name);
			*branchp = fy_gb_intern_string(call->gb, name);
			*createdp = *branchp;
		}
	}
	/* The switch moves the head; copy the name before it. */
	*branchp = fy_gb_intern_string(call->gb, *branchp);
	rc = fyai_session_branch_switch(ctx, *branchp, false, false);
	fyai_error_check(ctx, !rc, err,
			 "checkout: could not switch to branch '%s'", *branchp);
	return 0;
err:
	return -1;
}

int fyai_cmd_checkout(struct fyai_cmd_call *call, fy_generic *result)
{
	const char *target = fyai_cmd_arg_str(call, "target");
	const char *start = fyai_cmd_arg_str(call, "start");
	bool create = fyai_cmd_arg_bool(call, "create");
	const char *branch = target, *created = NULL;
	struct fyai_branch b;

	if (call->surface == FYAI_CMD_SESSION) {
		if (checkout_session(call, target, create, start, &branch,
				     &created))
			return -1;
	} else {
		if (create && !fyai_branch_lookup(call->ctx->arena_branches,
						  target, &b))
			created = target;
		if (fyai_branch_checkout(call->ctx, target, create, start))
			return -1;
	}
	*result = created ? fy_mapping(call->gb, "branch", branch,
				       "created", created) :
			    fy_mapping(call->gb, "branch", branch);
	return 0;
}

int fyai_cmd_model(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = call->ctx;
	struct fyai_cfg *cfg = ctx->cfg;
	const char *name = fyai_cmd_arg_str(call, "name");
	const char *old = cfg->model ? cfg->model : "";
	long long window;
	fy_generic note = fy_invalid;
	double delta;

	if (name && fyai_session_model(ctx, name,
				       call->surface == FYAI_CMD_SESSION))
		return -1;
	if (name && ctx->switch_pending) {
		delta = ctx->switch_fresh - ctx->switch_stay;
		note = fy_stringf(call->gb, "%lld tokens go to %s with no cache: "
				  "~$%.4f, against ~$%.4f read from the cache "
				  "of %s (%s~$%.4f)", ctx->switch_prefix,
				  cfg->model, ctx->switch_fresh,
				  ctx->switch_stay, old, delta < 0.0 ? "-" : "+",
				  delta < 0.0 ? -delta : delta);
	}
	window = fyai_context_window(ctx);
	*result = fy_mapping(call->gb,
		"model", cfg->model ? cfg->model : "",
		"provider", cfg->provider ? cfg->provider : "?",
		"api", fyai_api_to_string(cfg->api_mode),
		"window", window);
	if (fy_is_valid(*result) && fy_is_valid(note))
		*result = fy_assoc(call->gb, *result, "cache", note);
	fyai_error_check(ctx, fy_is_valid(*result), err,
			 "model: cannot build the result");
	return 0;
err:
	return -1;
}
