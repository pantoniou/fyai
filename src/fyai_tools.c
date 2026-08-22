/*
 * fyai_tools.c - tool decoding and execution
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_TOOLS

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>

#include <fcntl.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
/* openpty(3) lives in <util.h> on the BSDs, <pty.h> on glibc */
#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif
#include <sys/ioctl.h>
#include <sys/wait.h>

#include "fyai_agent.h"
#include "fyai_branch.h"
#include "fyai_jsonrpc.h"
#include "fyai_config.h"
#include "fyai_display.h"
#include "fyai_event.h"
#include "fyai_markdown.h"
#include "fyai_patch.h"
#include "fyai_sandbox.h"
#include "fyai_session.h"
#include "fyai_storage.h"
#include "fyai_terminal.h"
#include "fyai_terminal_session.h"
#include "fyai_tools.h"
#include "fyai_wait.h"
#include "fyai_prof.h"
#include "fyai_sink.h"
#include "fyai_ui.h"

static const char *fyai_tool_call_name(struct fyai_ctx *ctx, fy_generic tool_call)
{
	struct fyai_cfg *cfg = ctx->cfg;
	fy_generic v;

	if (fy_equal(fy_get(tool_call, "type"), "shell_call"))
		return "shell";

	switch (cfg->api_mode) {

	case FYAI_API_RESPONSES:
		v = fy_get_at_path(tool_call, "name");
		break;

	case FYAI_API_CHAT_COMPLETIONS:
		v = fy_get_at_path(tool_call, "function", "name");
		break;

	case FYAI_API_MESSAGES:
		/* normalized to Responses-style function_call items */
		v = fy_get_at_path(tool_call, "name");
		break;

	default:
		assert(0);
		__builtin_unreachable();
		break;
	}

	return fy_gb_intern_string(ctx->transient_gb, fy_cast(v, ""));
}

static fy_generic
fyai_tool_call_args(struct fyai_ctx *ctx, fy_generic tool_call)
{
	const char *args_text;

	switch (ctx->cfg->api_mode) {
	case FYAI_API_RESPONSES:
	case FYAI_API_MESSAGES:
		args_text = fy_get(tool_call, "arguments", "");
		break;
	case FYAI_API_CHAT_COMPLETIONS:
		args_text = fy_get(fy_get(tool_call, "function"),
				   "arguments", "");
		break;
	default:
		assert(0);
		__builtin_unreachable();
	}
	return parse_json_string(ctx->transient_gb, args_text);
}

static bool fyai_shell_tty_requested(struct fyai_ctx *ctx, fy_generic call);

/* True when a shell call names a shell that must stay open. */
static bool fyai_shell_named_call(struct fyai_ctx *ctx, fy_generic tool_call)
{
	fy_generic args;
	const char *name;

	if (!fy_equal(fyai_tool_call_name(ctx, tool_call), "shell"))
		return false;
	args = fyai_tool_call_args(ctx, tool_call);
	if (!fy_is_mapping(args))
		return false;
	name = fy_get(args, "name", "");
	return !fy_str_empty(name);
}

/* A named shell is a session only when it requested a terminal. */
static bool fyai_shell_session_call(struct fyai_ctx *ctx, fy_generic tool_call)
{
	if (!fyai_shell_named_call(ctx, tool_call))
		return false;
	return fyai_shell_tty_requested(ctx,
					fyai_tool_call_args(ctx, tool_call));
}

bool fyai_shell_session_display(struct fyai_ctx *ctx, fy_generic tool_call)
{
	const char *name;

	if (!ctx)
		return false;
	name = fyai_tool_call_name(ctx, tool_call);
	if (name && (!strcmp(name, "shell_input") ||
		     !strcmp(name, "shell_output") ||
		     !strcmp(name, "shell_close")))
		return true;
	return fyai_shell_session_call(ctx, tool_call);
}

/* Format the invocation Markdown through the same emitter used by history. */
static char *fyai_format_tool_header(struct fyai_ctx *ctx, const char *tool,
				     fy_generic args, int preview_lines)
{
	char *md = NULL;
	size_t mdlen = 0;
	FILE *mf;

	mf = open_memstream(&md, &mdlen);
	if (!mf)
		return NULL;
	fyai_emit_tool_call(ctx, mf, ctx->transient_gb, tool, args, preview_lines,
			    NULL);
	fclose(mf);
	return md;
}

/* Build the shared live and stored shell view. */
static int fyai_shell_view(struct fyai_ctx *ctx, const char *command,
			   fy_generic args, char **title,
			   struct response_buffer *body)
{
	struct fy_generic_builder *gb = ctx->transient_gb;
	fy_generic desc, workdir, timeout;
	fy_generic disp;

	desc = fy_get(args, "description", fy_null);
	workdir = fy_get(args, "workdir", fy_null);
	timeout = fy_get(args, "timeout", fy_null);
	disp = fy_null_filtered_mapping(
		gb,
		"command", command,
		"description", desc,
		"workdir", workdir,
		"timeout", timeout);
	return fyai_tool_call_view(ctx, "shell", disp, 0, title, body);
}

/* Print a tool title row and its rendered body to @fp. */
/* The tool title and its already-rendered body, on the status stream. */
static void fyai_print_tool_view(struct fyai_ctx *ctx, const char *title,
				 struct response_buffer *body)
{
	if (title && *title)
		(void)fyai_sink_markdown(ctx->sink, FYAI_SINK_STATUS, title);
	if (body->len)
		(void)fyai_sink_write(ctx->sink, FYAI_SINK_STATUS,
				      body->data, body->len);
}

static char *fyai_format_shell_label(fy_generic args)
{
	fy_generic desc;
	const char *text;
	char *label;

	desc = fy_get(args, "description");
	text = fy_castp(&desc, "");
	if (asprintf(&label, "**shell**%s%s%s\n",
		     *text ? " [" : "", text, *text ? "]" : "") < 0)
		return NULL;
	return label;
}

/*
 * Return a short failure cause for the label beside the failure mark.
 *
 * Keep the complete failure information in the tool result for the model. The
 * label contains only a short cause. Return NULL for a successful call or when
 * no specific cause is available. The caller owns the returned string.
 */
char *fyai_tool_error_cause(fy_generic result)
{
	fy_generic outcome, entry;
	const char *p, *nl, *last;
	long long n;
	char *s;
	int rc;

	last = NULL;
	/*
	 * A native shell result is a sequence of {stdout, stderr, outcome}.
	 * Report the first entry that did not complete successfully.
	 */
	if (fy_is_sequence(result)) {
		fy_foreach(entry, result) {
			outcome = fy_get(entry, "outcome");
			if (fy_equal(fy_get(outcome, "type"), "timeout")) {
				n = fy_get(outcome, "timeout_ms", 0LL);
				rc = asprintf(&s, "timed out after %lld ms", n);
				return rc < 0 ? NULL : s;
			}
			if (fy_equal(fy_get(outcome, "type"), "signal")) {
				n = fy_get(outcome, "signal", 0LL);
				rc = asprintf(&s, "killed by signal %lld", n);
				return rc < 0 ? NULL : s;
			}
			n = fy_get(outcome, "exit_code", 0LL);
			if (n) {
				rc = asprintf(&s, "exit %lld", n);
				return rc < 0 ? NULL : s;
			}
		}
		return NULL;
	}
	if (!fy_is_string(result))
		return NULL;
	/*
	 * All other tools report a failure as text. A "tool error: " line
	 * can occur at the end of the captured output.
	 */
	p = fy_castp(&result, "");
	for (nl = strstr(p, "tool error: "); nl; nl = strstr(nl + 1, "tool error: "))
		last = nl;
	if (!last)
		return NULL;
	p = last + strlen("tool error: ");
	nl = strchr(p, '\n');
	return nl ? strndup(p, (size_t)(nl - p)) : strdup(p);
}

static void fyai_tool_progress_flush(struct fyai_ctx *ctx);

/* Resolved display data for one patch call. */
struct fyai_patch_display {
	char *id;
	char *unified;
	struct fyai_patch_display *next;
};

/* Return the provider tool-call ID. */
static fy_generic patch_call_id(fy_generic tool_call)
{
	fy_generic id;

	id = fy_get(tool_call, "call_id");
	return fy_is_string(id) ? id : fy_get(tool_call, "id");
}

void fyai_patch_display_record(struct fyai_ctx *ctx, fy_generic tool_call,
			       const char *unified)
{
	struct fyai_patch_display *entry = NULL;
	fy_generic gid;
	const char *id;

	if (fy_str_empty(unified))
		return;
	entry = calloc(1, sizeof(*entry));
	fyai_error_check(ctx, entry, out, "could not allocate patch display data");
	gid = patch_call_id(tool_call);
	id = fy_castp(&gid, (const char *)NULL);
	entry->id = id ? strdup(id) : NULL;
	fyai_error_check(ctx, !id || entry->id, out,
			 "could not copy the patch call ID");
	entry->unified = strdup(unified);
	fyai_error_check(ctx, entry->unified, out,
			 "could not copy patch display data");
	entry->next = ctx->patch_views;
	ctx->patch_views = entry;
	return;
out:
	if (entry) {
		free(entry->id);
		free(entry->unified);
		free(entry);
	}
}

const char *fyai_patch_display_text(struct fyai_ctx *ctx, fy_generic tool_call)
{
	struct fyai_patch_display *entry;
	fy_generic gid;
	const char *id;

	gid = patch_call_id(tool_call);
	id = fy_castp(&gid, (const char *)NULL);
	for (entry = ctx->patch_views; entry; entry = entry->next) {
		if (!entry->id || !id) {
			if (!entry->id && !id)
				return entry->unified;
			continue;
		}
		if (!strcmp(entry->id, id))
			return entry->unified;
	}
	return NULL;
}

void fyai_patch_display_clear(struct fyai_ctx *ctx)
{
	struct fyai_patch_display *entry;
	struct fyai_patch_display *next;

	for (entry = ctx->patch_views; entry; entry = next) {
		next = entry->next;
		free(entry->id);
		free(entry->unified);
		free(entry);
	}
	ctx->patch_views = NULL;
}

/* Build an apply_patch work band. */
static int patch_band_view(struct fyai_ctx *ctx, fy_generic tool_call,
			   char **title, struct response_buffer *body)
{
	const char *resolved;
	fy_generic args;

	args = fyai_tool_call_args(ctx, tool_call);
	resolved = fyai_patch_display_text(ctx, tool_call);
	if (resolved)
		args = fy_mapping("patch", resolved);
	return fyai_tool_call_view(ctx, "apply_patch", args,
			fyai_tool_preview_lines(ctx->cfg, "apply_patch"),
			title, body);
}

void fyai_patch_band_refresh(struct fyai_ctx *ctx, fy_generic tool_call)
{
	struct response_buffer body = {0};
	char *title = NULL;

	if (!fyai_sink_bands_available(ctx->sink) || !ctx->cfg->markdown)
		return;
	if (!fyai_patch_display_text(ctx, tool_call))
		return;
	if (!patch_band_view(ctx, tool_call, &title, &body) && body.len)
		fyai_sink_band_paint(fyai_sink_band_shared(ctx->sink), NULL,
				     NULL, body.data, body.len, NULL);
	free(body.data);
	free(title);
}

void fyai_print_tool_call(struct fyai_ctx *ctx, fy_generic tool_call)
{
	struct fyai_cfg *cfg = ctx->cfg;
	const char *name;
	const char *args_text;
	const char *command;
	struct response_buffer body = {0};
	char *header;
	fy_generic args;
	fy_generic cmdv;

	/* A delegated agent prints tool calls only on its own terminal. */
	if (fyai_agent_delegated(ctx) && !cfg->agent_pty)
		return;
	args = fy_invalid;
	name = fyai_tool_call_name(ctx, tool_call);
	if (fy_equal(fy_get(tool_call, "type"), "shell_call")) {
		cmdv = fy_get_at_path(tool_call, "action", "commands", 0);
		command = fy_castp(&cmdv, "");
	} else if (fy_equal(name, "shell")) {

		switch (cfg->api_mode) {
		case FYAI_API_RESPONSES:
			args_text = fy_get(tool_call, "arguments", "");
			break;
		case FYAI_API_CHAT_COMPLETIONS:
			args_text = fy_get(fy_get(tool_call, "function"), "arguments", "");
			break;
		case FYAI_API_MESSAGES:
			args_text = fy_get(tool_call, "arguments", "");
			break;
		default:
			assert(0);
			__builtin_unreachable();
			break;
		}

		args = parse_json_string(ctx->transient_gb, args_text);
		command = fy_get(args, "command", "");
	} else {
		command = "";
	}

	if (cfg->markdown && fy_equal(name, "agent")) {
		args = fyai_tool_call_args(ctx, tool_call);
		command = fy_get(args, "description", "");
		header = fyai_format_tool_header(ctx, "agent",
			fy_mapping("name", fy_get(args, "name", ""),
				   "description", *command ? command : name), 0);
		if (fyai_sink_bands_available(ctx->sink)) {
			fyai_sink_band_open(ctx->sink, true,
					    header ? header : "agent", NULL);
		} else if (header) {
			(void)fyai_sink_markdown(ctx->sink, FYAI_SINK_STATUS,
						 header);
		} else {
			fyai_report(ctx, "  agent %s\n",
				*command ? command : name);
		}
		free(header);
		ctx->tool_output_displayed = true;
		return;
	}
	if (cfg->markdown && fy_equal(name, "shell")) {
		/*
		 * Live shell output streams progressively into a bounded,
		 * indented, in-place region - the same libfymd4c fenced render
		 * (row limit + indent) as the history view, only updated live as
		 * the command's output arrives, so live and history match.
		 */
		if (fyai_sink_bands_available(ctx->sink)) {
			header = fyai_format_shell_label(args);
			fyai_sink_band_open(ctx->sink, true,
					    header ? header : "shell",
					    *command ? command : name);
		} else if (!fyai_shell_view(ctx, *command ? command : name,
					    args, &header, &body)) {
			fyai_print_tool_view(ctx, header, &body);
		} else {
			fyai_report(ctx, "  shell %s\n",
				*command ? command : name);
		}
		free(header);
		free(body.data);
		memset(&body, 0, sizeof(body));
		ctx->shell_stream = calloc(1, sizeof(*ctx->shell_stream));
		if (ctx->shell_stream != NULL &&
		    fyai_fenced_stream_start(ctx->shell_stream, ctx, cfg, NULL,
					     cfg->tool_preview_lines > 0 ?
					     (size_t) cfg->tool_preview_lines : 0,
					     FYAI_TOOL_OUTPUT_INDENT, stderr,
					     fyai_ui_active(ctx) ||
					     terminal_is_tty(STDERR_FILENO)) != 0) {
			free(ctx->shell_stream);
			ctx->shell_stream = NULL;
		}
		ctx->tool_output_displayed = true;
	} else if (cfg->markdown && fyai_sink_bands_available(ctx->sink) &&
		   fy_equal(name, "apply_patch")) {
		/* Show the patch in a marked work band. */
		if (patch_band_view(ctx, tool_call, &header, &body))
			header = NULL;
		fyai_sink_band_open(ctx->sink, true,
				    header ? header : "**patch**", NULL);
		if (body.len)
			fyai_sink_band_paint(fyai_sink_band_shared(ctx->sink),
					     NULL, NULL, body.data, body.len,
					     NULL);
		free(body.data);
		free(header);
		ctx->tool_output_displayed = true;
	} else if (cfg->markdown && fyai_sink_bands_available(ctx->sink) &&
		   fy_any_equal(name, "read_file", "write_file")) {
		args = fyai_tool_call_args(ctx, tool_call);
		if (fyai_tool_call_view(ctx, name, args,
					fyai_tool_preview_lines(cfg, name),
					&header, &body))
			header = NULL;
		fyai_sink_band_open(ctx->sink, true, header ? header : name,
				    NULL);
		if (body.len)
			fyai_sink_band_paint(fyai_sink_band_shared(ctx->sink),
					     NULL, NULL, body.data, body.len,
					     NULL);
		free(body.data);
		memset(&body, 0, sizeof(body));
		free(header);
		ctx->tool_output_displayed = true;
	} else if (*command) {
		fyai_report(ctx, "fyai $ %s\n", command);
	} else {
		fyai_report(ctx, "fyai $ %s\n", name);
	}
	if (fy_equal(name, "shell"))
		ctx->tool_output_displayed = true;
}

static void fyai_shell_live_close(struct fyai_ctx *ctx)
{
	if (!ctx || !ctx->shell_stream)
		return;
	fyai_fenced_stream_finish(ctx->shell_stream);
	free(ctx->shell_stream);
	ctx->shell_stream = NULL;
}

/* Send tool progress to the parent through JSON-RPC. */
void fyai_tool_progress_emit(struct fyai_ctx *ctx, const char *data, size_t len)
{
	struct fy_generic_builder *gb;
	const char *text;
	char *copy;

	if (!ctx || !ctx->tool_rpc || !len)
		return;
	/* Do not resend progress already visible on the agent terminal. */
	if (ctx->cfg->agent_pty)
		return;
	gb = fyai_ctx_transient_gb(ctx);
	if (!gb)
		return;
	/* The chunk is not NUL-terminated and may hold partial output. */
	copy = malloc(len + 1);
	if (!copy)
		return;
	memcpy(copy, data, len);
	copy[len] = '\0';
	text = fy_gb_intern_string(gb, copy);
	free(copy);
	(void)jsonrpc_notify(ctx->tool_rpc, "tool/progress",
			     fy_gb_mapping(gb, "text", text));
}

static void fyai_tool_progress_flush(struct fyai_ctx *ctx)
{
	struct fyai_event_loop *el;

	if (!ctx || !ctx->tool_rpc)
		return;
	el = fyai_ctx_loop(ctx);
	if (!el)
		return;
	while (jsonrpc_conn_has_output(ctx->tool_rpc))
		if (fyai_event_loop_step(el, 1000) <= 0)
			break;
}

static void fyai_shell_output(void *userdata,
			      enum shell_output_stream stream,
			      const char *data, size_t len)
{
	struct fyai_ctx *ctx = userdata;

	if (!len)
		return;
	(void)stream;
	/* Forward output unless the current agent terminal already shows it. */
	fyai_tool_progress_emit(ctx, data, len);
	/* A delegated sub-agent with no terminal has nowhere to render. */
	if (fyai_agent_delegated(ctx) && !ctx->cfg->agent_pty)
		return;

	/*
	 * Don't feed binary chunks to the terminal or the fenced renderer; the
	 * post-capture site prints a "binary output: N bytes" summary with the
	 * true total length instead.
	 */
	if (data_is_binary(data, len))
		return;

	/* Markdown streams progressively into the bounded live region; plain
	 * mode dumps raw output for scripting visibility. */
	if (ctx && ctx->cfg->markdown) {
		if (ctx->shell_stream)
			fyai_fenced_stream_push(ctx->shell_stream, data, len);
		return;
	}

	(void)fyai_sink_write(ctx->sink, FYAI_SINK_STATUS, data, len);
	if (data[len - 1] != '\n')
		(void)fyai_report(ctx, "\n");
}

/*
 * Per-call sandbox spec plus the owned backing storage its pointers reference.
 * The spec must outlive the fork inside run_shell_command_capture_cb, so this
 * lives on the caller's stack and fyai_shell_sandbox_end() frees the arrays.
 */
struct fyai_shell_sandbox {
	struct fyai_sandbox_spec spec;
	char *root;
	struct fyai_sandbox_path *allow;	/* each .path owned */
	const char **deny;			/* each entry owned */
	uint16_t *ports;
};

static void fyai_shell_sandbox_end(struct fyai_shell_sandbox *sb);

/*
 * Resolve a config path against @base: absolute as-is, "~"-prefixed against
 * $HOME, otherwise relative to @base (the project root). Returns a malloc'd
 * absolute path or NULL.
 */
static char *sandbox_resolve(const char *base, const char *p)
{
	const char *home;
	char *out;

	if (!p || !*p)
		return NULL;
	if (p[0] == '/')
		return strdup(p);
	if (p[0] == '~') {
		home = getenv("HOME");
		if (!home || asprintf(&out, "%s%s", home, p + 1) < 0)
			return NULL;
		return out;
	}
	if (!base)
		return strdup(p);
	return asprintf(&out, "%s/%s", base, p) < 0 ? NULL : out;
}

/*
 * Build the tool sandbox spec from cfg->sandbox, or return NULL when disabled.
 * Confinement is scoped to the project root (or cwd when none is found), with
 * the arena .fyai plus every config deny entry carved out, the config allow
 * entries added, and egress restricted to config network.ports when a network
 * policy is present.
 */
static int fyai_shell_sandbox_begin(struct fyai_ctx *ctx,
				    struct fyai_shell_sandbox *sb,
				    const struct fyai_sandbox_spec **specp)
{
	struct fyai_sandbox_spec *sp = &sb->spec;
	fy_generic cs = ctx->cfg->sandbox;
	fy_generic allow, deny, net, ports, e;
	fy_generic port, pv;
	enum fyai_sandbox_mode mode;
	char cwd[4096];
	char *resolved;
	const char *ps;
	size_t n;
	int rc;

	memset(sb, 0, sizeof(*sb));
	*specp = NULL;
	if (!ctx->cfg->enable_sandbox || ctx->sandbox_applied)
		return 0;

	sb->root = fyai_discover_project_root();
	if (!sb->root && getcwd(cwd, sizeof(cwd)))
		sb->root = strdup(cwd);
	fyai_error_check(ctx, sb->root, err_out,
			 "sandbox: could not resolve the project root");
	sp->project_root = sb->root;
	sp->strict = false;			/* floor is the config policy */

	/* deny: always the arena, then each config sandbox.deny entry. */
	deny = fy_get(cs, "deny");
	n = fy_is_sequence(deny) ? fy_len(deny) : 0;
	sb->deny = calloc(n + 1, sizeof(*sb->deny));
	fyai_error_check(ctx, sb->deny, err_out,
			 "sandbox: could not allocate the deny list");
	/* Configured denies apply to every grant. Ignore paths that do not exist. */
	fy_foreach(e, deny) {
		ps = fy_castp(&e, "");
		resolved = sandbox_resolve(sb->root, ps);
		fyai_error_check(ctx, resolved, err_out,
				 "sandbox: could not resolve deny path '%s'", ps);
		if (access(resolved, F_OK)) {
			free(resolved);
			continue;
		}
		sb->deny[sp->deny_n++] = resolved;
	}
	sp->deny_global_n = sp->deny_n;
	resolved = sandbox_resolve(sb->root, ".fyai");
	fyai_error_check(ctx, resolved, err_out,
			 "sandbox: could not resolve the arena deny path");
	if (access(resolved, F_OK))
		free(resolved);
	else
		sb->deny[sp->deny_n++] = resolved;
	sp->deny = sb->deny;

	/* allow: extra grants; a string is rw, a mapping {path, mode: ro}. */
	allow = fy_get(cs, "allow");
	n = fy_is_sequence(allow) ? fy_len(allow) : 0;
	if (n) {
		sb->allow = calloc(n, sizeof(*sb->allow));
		fyai_error_check(ctx, sb->allow, err_out,
				 "sandbox: could not allocate the allow list");
		fy_foreach(e, allow) {
			mode = FYAI_SB_RW;
			if (fy_is_mapping(e)) {
				pv = fy_get(e, "path");
				ps = fy_castp(&pv, "");
				rc = fyai_sandbox_mode_parse(
						fy_get(e, "mode", "rw"), &mode);
				fyai_error_check(ctx, !rc,
					err_out, "sandbox: invalid mode '%s' for path '%s'",
					fy_get(e, "mode", "rw"), ps);
			} else {
				ps = fy_castp(&e, "");
			}
			sb->allow[sp->allow_n].path = sandbox_resolve(sb->root, ps);
			fyai_error_check(ctx, sb->allow[sp->allow_n].path, err_out,
					 "sandbox: could not resolve allow path '%s'", ps);
			sb->allow[sp->allow_n].mode = mode;
			sp->allow_n++;
		}
	}
	sp->allow = sb->allow;

	/* network: present => restrict egress to network.ports (empty = deny
	 * all); absent => leave egress unrestricted. */
	net = fy_get(cs, "network");
	if (fy_is_valid(net)) {
		/* Check here because the child cannot report why it failed. */
		fyai_error_check(ctx, fyai_sandbox_net_restrictable(-1), err_out,
				 "sandbox: network egress cannot be restricted by this build or kernel");
		sp->restrict_net = true;
		ports = fy_get(net, "ports");
		n = fy_is_sequence(ports) ? fy_len(ports) : 0;
		if (n) {
			sb->ports = calloc(n, sizeof(*sb->ports));
			fyai_error_check(ctx, sb->ports, err_out,
					 "sandbox: could not allocate the port list");
			fy_foreach(port, ports)
				sb->ports[sp->ports_n++] = (uint16_t)
					fy_cast(port, 0LL);
		}
		sp->ports = sb->ports;
	}

	*specp = sp;
	return 0;
err_out:
	fyai_shell_sandbox_end(sb);
	return -1;
}

static void fyai_shell_sandbox_end(struct fyai_shell_sandbox *sb)
{
	size_t i;

	for (i = 0; i < sb->spec.deny_n; i++)
		free((char *)sb->deny[i]);
	for (i = 0; i < sb->spec.allow_n; i++)
		free((char *)sb->allow[i].path);
	free(sb->deny);
	free(sb->allow);
	free(sb->ports);
	free(sb->root);
	memset(sb, 0, sizeof(*sb));
}

/*
 * Return the model-requested time limit in milliseconds, or 0 if it is absent.
 * The `shell` function tool uses `timeout` in its arguments. The native
 * Responses `shell_call` uses `timeout_ms` in its action. The caller limits
 * each untrusted value to `shell/max_timeout_ms`.
 */
static long long fyai_shell_timeout_requested(fy_generic call, bool native)
{
	if (native)
		return fy_get(fy_get(call, "action"), "timeout_ms", 0LL);
	return fy_get(call, "timeout", 0LL);
}

/* Return the bounded time limit for a shell call. */
static unsigned int fyai_shell_timeout_ms(struct fyai_ctx *ctx, fy_generic call,
					  bool native)
{
	struct fyai_cfg *cfg = ctx->cfg;
	long long ms;

	if (ctx->cfg->tool_child)
		return 0;
	ms = fyai_shell_timeout_requested(call, native);
	if (ms <= 0)
		ms = cfg->shell_timeout_ms;
	if (cfg->shell_max_timeout_ms > 0 && ms > cfg->shell_max_timeout_ms)
		ms = cfg->shell_max_timeout_ms;
	return ms > 0 ? (unsigned int)ms : 0;
}

/* Use the configured limit unless the model supplies a bounded limit. */
static size_t fyai_read_max_bytes(struct fyai_ctx *ctx, fy_generic args)
{
	struct fyai_cfg *cfg = ctx->cfg;
	long long n;

	n = fy_get(args, "max_bytes", 0LL);
	if (n <= 0)
		n = cfg->read_max_bytes;
	else if (cfg->read_hard_max_bytes > 0 && n > cfg->read_hard_max_bytes)
		n = cfg->read_hard_max_bytes;
	return n > 0 ? (size_t)n : 0;
}

/* Return a bounded file result and report truncation. */
static char *fyai_read_file_tool(struct fyai_ctx *ctx, fy_generic args)
{
	struct read_text_info info;
	long long offset, offset_bytes, limit;
	const char *path;
	size_t max_bytes;
	char *text, *out;
	int rc;

	path = fy_get(args, "path", "");
	offset = fy_get(args, "offset", 0LL);
	offset_bytes = fy_get(args, "offset_bytes", -1LL);
	limit = fy_get(args, "limit", 0LL);
	max_bytes = fyai_read_max_bytes(ctx, args);

	text = read_text_file_window(path, offset, offset_bytes, limit, max_bytes,
				     &info);
	if (!text)
		return NULL;

	/* A binary file returns only a size report. */
	if (info.binary)
		return text;

	/* Report an offset past the end as an empty window. */
	if (!info.first_line) {
		free(text);
		rc = asprintf(&out,
			      "[fyai: no lines returned - the file has %lld "
			      "lines and the read started at line %lld]\n",
			      info.total_lines, offset < 1 ? 1 : offset);
		return rc < 0 ? NULL : out;
	}

	/* Return a complete final window without a continuation note. */
	if (!info.byte_capped && info.last_line >= info.total_lines)
		return text;

	/* Report the returned window and the next offset. */
	if (info.byte_capped)
		rc = asprintf(&out,
			      "%s\n[fyai: lines %lld-%lld of %lld shown "
			      "(byte limit reached); continue with "
			      "offset_bytes=%zu]\n",
			      text, info.first_line, info.last_line,
			      info.total_lines, info.next_byte);
	else
		rc = asprintf(&out,
			      "%s\n[fyai: lines %lld-%lld of %lld shown; "
			      "continue with offset=%lld]\n",
			      text, info.first_line, info.last_line,
			      info.total_lines, info.last_line + 1);
	free(text);
	return rc < 0 ? NULL : out;
}

/* Return the configured or model-supplied shell output budget in bytes. */
static size_t fyai_shell_output_bytes(struct fyai_ctx *ctx, fy_generic call)
{
	struct fyai_cfg *cfg = ctx->cfg;
	long long n;

	n = fy_get(call, "max_output_tokens", 0LL);
	if (n <= 0)
		n = cfg->shell_max_output_tokens;
	else if (cfg->shell_hard_max_output_tokens > 0 &&
		 n > cfg->shell_hard_max_output_tokens)
		n = cfg->shell_hard_max_output_tokens;
	if (n <= 0)
		return 0;
	return (size_t)n * FYAI_BYTES_PER_TOKEN;
}

/*
 * The PTY size for one call: the model's request, then the configuration, then
 * the real terminal the parent recorded, then the fixed default. A size the
 * model asks for is bounded, because a huge screen costs output budget for no
 * gain.
 */
#define FYAI_TTY_MAX_ROWS	1000
#define FYAI_TTY_MAX_COLS	1000

static int fyai_shell_tty_dim(long long asked, int configured, int actual,
			      int dflt, int max)
{
	int n;

	n = asked > 0 ? (int)(asked < max ? asked : max) :
	    configured > 0 ? configured :
	    actual > 0 ? actual : dflt;
	return n > max ? max : n;
}

static void fyai_shell_tty_size(struct fyai_ctx *ctx, fy_generic call,
				int *rowsp, int *colsp)
{
	struct fyai_cfg *cfg = ctx->cfg;

	*rowsp = fyai_shell_tty_dim(fy_get(call, "rows", 0LL),
				    cfg->shell_tty_rows, ctx->tty_rows,
				    FYAI_TTY_ROWS_DEFAULT, FYAI_TTY_MAX_ROWS);
	*colsp = fyai_shell_tty_dim(fy_get(call, "cols", 0LL),
				    cfg->shell_tty_cols, ctx->tty_cols,
				    FYAI_TTY_COLS_DEFAULT, FYAI_TTY_MAX_COLS);
}

/* Read the call's tty choice, falling back to shell/tty. */
static bool fyai_shell_tty_requested(struct fyai_ctx *ctx, fy_generic call)
{
	fy_generic tty;

	tty = fy_get(call, "tty", fy_invalid);
	if (fy_generic_is_bool(tty))
		return fy_castp(&tty, (_Bool)false);
	return ctx->cfg->shell_tty;
}

/* Read the call's shell, falling back to shell/shell. */
static const char *fyai_shell_shell_requested(struct fyai_ctx *ctx,
					      fy_generic call)
{
	const char *shell;

	/*
	 * The typed accessor addresses the stored item, so the pointer is
	 * that of @call and not that of a copy. It lives as long as the call.
	 */
	shell = fy_get(call, "shell", "");
	return !fy_str_empty(shell) ? shell : ctx->cfg->shell_shell;
}

static bool fyai_shell_login_requested(struct fyai_ctx *ctx, fy_generic call)
{
	fy_generic login;

	login = fy_get(call, "login", fy_invalid);
	if (fy_generic_is_bool(login))
		return fy_castp(&login, (_Bool)false);
	return ctx->cfg->shell_login;
}

/* Keep the end of a stream and report the number of omitted bytes. */
static char *fyai_shell_bound_alloc(const char *text, size_t max_bytes)
{
	const char *cut, *nl;
	size_t len, drop;
	char *out;
	int rc;

	if (!text)
		return NULL;
	len = strlen(text);
	if (!max_bytes || len <= max_bytes)
		return NULL;

	drop = len - max_bytes;
	cut = text + drop;
	nl = memchr(cut, '\n', len - drop);
	if (nl && (size_t)(nl + 1 - text) < len)
		cut = nl + 1;
	rc = asprintf(&out,
		"[fyai: %zu of %zu bytes elided; the end of the output follows]\n%s",
		(size_t)(cut - text), len, cut);
	return rc < 0 ? NULL : out;
}

/* Give standard error up to half of the shared output budget. */
static void fyai_shell_split_budget(size_t budget, size_t err_len,
				    size_t *out_bytes, size_t *err_bytes)
{
	size_t err_keep;

	if (!budget) {
		*out_bytes = 0;
		*err_bytes = 0;
		return;
	}
	err_keep = err_len < budget / 2 ? err_len : budget / 2;
	*err_bytes = err_keep ? err_keep : 1;
	*out_bytes = budget - err_keep;
}

/* Run on a PTY and return owned, bounded screen text and status. */
static char *fyai_shell_tty_run(struct fyai_ctx *ctx, fy_generic call,
				const char *command, const char *workdir,
				const struct fyai_sandbox_spec *sandbox,
				unsigned int timeout_ms,
				struct fyai_terminal_result *result)
{
	struct fyai_terminal_opts opts = {};
	size_t budget;
	char *text;
	int rc;

	budget = fyai_shell_output_bytes(ctx, call);
	opts.workdir = workdir;
	opts.timeout_ms = timeout_ms;
	opts.max_bytes = budget;
	opts.output_fn = fyai_shell_output;
	opts.output_data = ctx;
	opts.term = ctx->cfg->shell_tty_term;
	opts.shell = fyai_shell_shell_requested(ctx, call);
	opts.login = fyai_shell_login_requested(ctx, call);
	fyai_shell_tty_size(ctx, call, &opts.rows, &opts.cols);

	rc = fyai_terminal_session_run(ctx, command, sandbox, &opts, result);
	fyai_shell_live_close(ctx);
	fyai_error_check(ctx, !rc, err,
			 "shell: could not run the command on a terminal");

	if (result->binary) {
		rc = asprintf(&text, "binary output: %zu bytes",
			      result->raw_bytes);
		fyai_error_check(ctx, rc >= 0, err,
				 "shell: could not format binary terminal output");
		if (!ctx->cfg->markdown)
			fyai_report(ctx, "%s\n", text);
		return text;
	}
	text = fyai_shell_bound_alloc(result->output, budget);
	if (!text)
		text = strdup(result->output ? result->output : "");
	fyai_error_check(ctx, text, err,
			 "shell: could not retain terminal output");
	return text;

err:
	return NULL;
}

static char *fyai_run_shell_command(struct fyai_ctx *ctx, fy_generic args,
				    bool *okp)
{
	struct fyai_cfg *cfg = ctx->cfg;
	struct shell_command_result result = {};
	struct shell_command_opts opts = {};
	struct response_buffer buf = {};
	struct fyai_shell_sandbox sb;
	const struct fyai_sandbox_spec *sandbox;
	fy_generic command, workdir;
	size_t budget, out_bytes, err_bytes;
	const char *msg;
	char *bounded;
	char *ret = NULL;
	int rc;
	struct fyai_terminal_result tty_result = {};
	char *tty_text;

	*okp = false;
	if (fyai_shell_sandbox_begin(ctx, &sb, &sandbox))
		return NULL;

	command = fy_get(args, "command", fy_invalid);
	workdir = fy_get(args, "workdir", fy_invalid);
	opts.workdir = fy_castp(&workdir, (const char *)NULL);
	opts.timeout_ms = fyai_shell_timeout_ms(ctx, args, false);
	opts.shell = fyai_shell_shell_requested(ctx, args);
	opts.login = fyai_shell_login_requested(ctx, args);
	if (fyai_shell_tty_requested(ctx, args)) {
		tty_text = fyai_shell_tty_run(ctx, args, fy_castp(&command, ""),
					      opts.workdir, sandbox,
					      opts.timeout_ms, &tty_result);
		fyai_error_check(ctx, tty_text, out,
				 "shell: command produced no terminal result");

		rc = response_buffer_append(&buf, tty_text);
		free(tty_text);
		fyai_error_check(ctx, !rc, out,
				 "shell: could not retain terminal output");
		if (tty_result.binary) {
			rc = response_buffer_append(&buf, "\n");
			fyai_error_check(ctx, !rc, out,
					 "shell: could not finish binary output");
		}

		if (tty_result.timed_out) {
			msg = fy_sprintfa(
				"\ntool error: command timed out after %u ms\n",
				opts.timeout_ms);
		} else if (tty_result.signaled) {
			if (fyai_interrupt_pending(ctx) || tty_result.cancelled)
				msg = "\ntool error: interrupted\n";
			else
				msg = fy_sprintfa(
					"\ntool error: command killed by signal %d\n",
					tty_result.signal);
		} else if (tty_result.exit_code == FYAI_SHELL_EXIT_WORKDIR &&
			   opts.workdir) {
			msg = fy_sprintfa(
				"\ntool error: cannot enter workdir %s\n",
				opts.workdir);
		} else if (tty_result.exit_code) {
			msg = fy_sprintfa(
				"\ntool error: command exited with status %d\n",
				tty_result.exit_code);
		} else {
			msg = NULL;
			*okp = true;
		}
		if (msg && response_buffer_append(&buf, msg))
			goto out;

		ret = buf.data;
		buf.data = NULL;
		goto out;
	}

	if (run_shell_command_capture_cb(ctx, fy_castp(&command, ""), &result,
					 fyai_shell_output, ctx, sandbox,
					 &opts))
		goto out;
	fyai_shell_live_close(ctx);

	budget = fyai_shell_output_bytes(ctx, args);
	fyai_shell_split_budget(budget, result.stderr_len, &out_bytes,
				&err_bytes);

	if (data_is_binary(result.stdout_data, result.stdout_len)) {
		msg = fy_sprintfa("binary output: %zu bytes\n",
				  result.stdout_len);
		if (!cfg->markdown)
			fyai_report(ctx, "%s", msg);
		if (response_buffer_append(&buf, msg))
			goto out;
	} else {
		bounded = fyai_shell_bound_alloc(result.stdout_data, out_bytes);
		rc = response_buffer_append(&buf, bounded ? bounded :
						  result.stdout_data);
		free(bounded);
		if (rc)
			goto out;
	}

	if (data_is_binary(result.stderr_data, result.stderr_len)) {
		msg = fy_sprintfa("binary stderr: %zu bytes\n",
				  result.stderr_len);
		if (!cfg->markdown)
			fyai_report(ctx, "%s", msg);
		if (response_buffer_append(&buf, msg))
			goto out;
	} else {
		bounded = fyai_shell_bound_alloc(result.stderr_data, err_bytes);
		rc = response_buffer_append(&buf, bounded ? bounded :
						  result.stderr_data);
		free(bounded);
		if (rc)
			goto out;
	}

	if (result.signaled) {
		if (result.timed_out)
			msg = fy_sprintfa(
				"\ntool error: command timed out after %u ms\n",
				opts.timeout_ms);
		else if (fyai_interrupt_pending(ctx))
			msg = "\ntool error: interrupted\n";
		else
			msg = fy_sprintfa(
				"\ntool error: command killed by signal %d\n",
				result.signal);
		if (response_buffer_append(&buf, msg))
			goto out;
	} else if (result.exit_code) {
		if (result.exit_code == FYAI_SHELL_EXIT_WORKDIR && opts.workdir)
			msg = fy_sprintfa(
				"\ntool error: cannot enter workdir %s\n",
				opts.workdir);
		else
			msg = fy_sprintfa(
				"\ntool error: command exited with status %d\n",
				result.exit_code);
		if (response_buffer_append(&buf, msg))
			goto out;
	} else {
		*okp = true;
	}

	ret = buf.data;
	buf.data = NULL;
out:
	fyai_shell_live_close(ctx);
	fyai_shell_sandbox_end(&sb);
	free(buf.data);
	fyai_terminal_result_cleanup(&tty_result);
	shell_command_result_cleanup(&result);
	return ret;
}

/*
 * Execute the `ask_user` tool: put the model's question (and any suggested
 * options, as a numbered menu) to the user and return their answer as the tool
 * result. A bare number selects the matching option; anything else is returned
 * verbatim as a free-form answer. When no answer can be read (non-interactive
 * stdin, EOF), the model is told the user did not answer so it can proceed.
 */
/* Ask the parent to present a delegated sub-agent's question. */
static fy_generic fyai_ask_user_upward(struct fyai_ctx *ctx, fy_generic args)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	struct jsonrpc_request *req;
	fy_generic result = fy_invalid;
	fy_generic answer;

	req = jsonrpc_request_submit(ctx->tool_rpc, "user/ask", args,
				     jsonrpc_conn_next_id(ctx->tool_rpc),
				     false, NULL, NULL);
	if (!req) {
		fyai_error(ctx, "ask_user: could not put the question to the "
			   "parent");
		return fy_value(gb, "tool error: the question could not be "
				"asked");
	}
	/* The loop of this process serves the channel while it waits. */
	while (!jsonrpc_request_done(req)) {
		if (fyai_event_loop_step(fyai_ctx_loop(ctx), -1) < 0)
			break;
	}
	if (jsonrpc_request_ok(req)) {
		answer = fy_get(jsonrpc_request_result(req), "answer",
				fy_invalid);
		if (fy_is_string(answer))
			result = fy_value(gb, fy_castp(&answer, ""));
	}
	jsonrpc_request_destroy(req);
	if (!fy_is_valid(result)) {
		fyai_error(ctx, "ask_user: the parent did not answer");
		return fy_value(gb, "tool note: the user did not provide an "
				"answer");
	}
	return result;
}

static fy_generic fyai_ask_user(struct fyai_ctx *ctx, fy_generic args)
{
	struct fyai_cfg *cfg = ctx->cfg;
	const char *question = fy_get(args, "question", "");
	fy_generic options = fy_get(args, "options");
	size_t n = fy_is_sequence(options) ? fy_len(options) : 0;
	fy_generic result;
	char *line, *end;
	const char *a;
	size_t i;
	long sel;

	if (fyai_agent_delegated(ctx) && ctx->tool_rpc)
		return fyai_ask_user_upward(ctx, args);

	if (ansi_color_on(cfg->color, STDERR_FILENO))
		fyai_report(ctx, "\n" FYAI_ANSI_BOLD "? %s" FYAI_ANSI_RESET
			"\n", question);
	else
		fyai_report(ctx, "\n? %s\n", question);
	i = 0;
	fy_foreach(result, options) {
		fyai_report(ctx, "  %zu) %s\n", i + 1,
			fy_castp(&result, ""));
		i++;
	}

	/*
	 * Batch use: --answer values are consumed in order, one per ask_user
	 * call, instead of prompting. Echo the consumed answer so the
	 * transcript still reads sensibly.
	 */
	if (ctx->answer_next < cfg->answer_count) {
		a = cfg->answers[ctx->answer_next++];

		fyai_report(ctx, "%s%s\n",
			n ? "choose a number or type an answer> " : "> ", a);
		line = strdup(a ? a : "");
		if (!line)
			return fy_value(ctx->transient_gb, "tool error: out of memory");
		goto have_line;
	}

	/*
	 * Batch use with no answer left: if stdin is not a terminal there is no
	 * one to prompt, so an expected answer cannot be obtained. Flag the run
	 * to abort rather than letting the model proceed on a guess.
	 */
	if (!terminal_is_tty(STDIN_FILENO)) {
		fyai_error(ctx, "ask_user: an answer is expected but none is "
			   "available (non-interactive; supply --answer)");
		ctx->ask_abort = true;
		return fy_value(ctx->transient_gb, "tool error: no answer available (non-interactive)");
	}

	/* Editable input via linenoise (only reached on an interactive tty). */
	line = fyai_readline(ctx, n ? "choose a number or type an answer> " : "> ");
have_line:
	if (!line || !*line) {
		free(line);
		return fy_value(ctx->transient_gb, "tool note: the user did not provide an answer");
	}

	/* A bare number (optionally surrounded by space) selects an option. */
	if (n) {
		sel = strtol(line, &end, 10);
		while (*end == ' ' || *end == '\t' || *end == '\n')
			end++;
		if (end != line && !*end && sel >= 1 && (size_t)sel <= n) {
			result = fy_get_at(options, sel - 1);
			if (fy_is_invalid(result))
				result = fy_value("");
			free(line);
			if (fy_is_invalid(result))
				fyai_error(ctx, "ask_user: could not retain the answer");
			return result;
		}
	}

	result = fy_value(ctx->transient_gb, line);
	free(line);
	if (fy_is_invalid(result))
		fyai_error(ctx, "ask_user: could not retain the answer");
	return result;
}


/*
 * The tools of a named session. They run in the parent, where the terminal
 * state is; their bodies sit with the session machinery further down.
 */
static char *fyai_shell_output_tool(struct fyai_ctx *ctx, fy_generic args,
				    bool *okp);
static char *fyai_shell_input_tool(struct fyai_ctx *ctx, fy_generic args,
				   bool *okp);
static char *fyai_agent_input_tool(struct fyai_ctx *ctx, fy_generic args,
				   bool *okp);
static struct fyai_tool_job *fyai_agent_job_named(struct fyai_ctx *ctx,
						  const char *name);
static const char *fyai_agent_job_name(const struct fyai_tool_job *job);
static char *fyai_shell_close_tool(struct fyai_ctx *ctx, fy_generic args,
				   bool *okp);

fy_generic fyai_execute_tool_call(struct fyai_ctx *ctx,
				  fy_generic tool_call, bool *okp)
{
	struct fyai_cfg *cfg = ctx->cfg;
	struct shell_command_result shell_result = {};
	struct shell_command_opts shell_opts = {};
	struct fyai_shell_sandbox sb;
	const struct fyai_sandbox_spec *sandbox;
	const char *name;
	const char *args_text;
	fy_generic type;
	fy_generic args;
	fy_generic result_generic;
	fy_generic action;
	fy_generic commands;
	fy_generic outputs;
	fy_generic command;
	fy_generic output;
	const char *out_text;
	const char *err_text;
	fy_generic out_val, err_val;
	fy_generic outcome;
	size_t out_bytes, err_bytes;
	char *bounded;
	struct fyai_terminal_result tty_result = {};
	char *tty_text;
	bool tty_call;

	*okp = false;
	type = fy_get(tool_call, "type");
	if (fy_equal(type, "shell_call")) {

		action = fy_get(tool_call, "action");
		commands = fy_get(action, "commands");
		shell_opts.timeout_ms = fyai_shell_timeout_ms(ctx, tool_call,
							      true);
		tty_call = fyai_shell_tty_requested(ctx, action);
		shell_opts.shell = fyai_shell_shell_requested(ctx, action);
		shell_opts.login = fyai_shell_login_requested(ctx, action);
		outputs = fy_seq_empty;
		if (fyai_shell_sandbox_begin(ctx, &sb, &sandbox))
			return fy_value(ctx->transient_gb,
					"tool error: invalid sandbox configuration");
		*okp = true;

		fy_foreach(command, commands) {
			if (tty_call) {
				/* Put the PTY screen in stdout and leave stderr empty. */
				tty_text = fyai_shell_tty_run(ctx, action,
						fy_castp(&command, ""), NULL,
						sandbox, shell_opts.timeout_ms,
						&tty_result);
				if (!tty_text) {
					fyai_shell_sandbox_end(&sb);
					return fy_value(ctx->transient_gb,
						"tool error: failed to run shell command on a terminal");
				}
				out_val = fy_gb_internalize(ctx->transient_gb,
							    fy_value(tty_text));
				free(tty_text);
				outcome = tty_result.timed_out ?
					fy_mapping(
						"type", "timeout",
						"timeout_ms",
						(long long)shell_opts.timeout_ms) :
					tty_result.signaled ?
					fy_mapping(
						"type", "signal",
						"signal", tty_result.signal) :
					fy_mapping(
						"type", "exit",
						"exit_code",
						tty_result.exit_code);
				output = fy_mapping("stdout", out_val,
						    "stderr", "",
						    "outcome", outcome);
				outputs = fy_append(outputs, output);
				if (tty_result.signaled || tty_result.exit_code)
					*okp = false;
				fyai_terminal_result_cleanup(&tty_result);
				continue;
			}
			if (run_shell_command_capture_cb(ctx,
							 fy_castp(&command, ""),
							 &shell_result,
							 fyai_shell_output,
							 ctx, sandbox,
							 &shell_opts)) {
				fyai_shell_live_close(ctx);
				fyai_shell_sandbox_end(&sb);
				return fy_value(ctx->transient_gb, "tool error: failed to run shell command");
			}
			fyai_shell_live_close(ctx);

			fyai_shell_split_budget(
				fyai_shell_output_bytes(ctx, action),
				shell_result.stderr_len, &out_bytes,
				&err_bytes);

			/* Internalize bounded streams before their buffers are freed. */
			out_text = shell_result.stdout_data;
			bounded = fyai_shell_bound_alloc(out_text, out_bytes);
			out_val = fy_gb_internalize(ctx->transient_gb,
					fy_value(bounded ? bounded : out_text));
			free(bounded);
			if (data_is_binary(shell_result.stdout_data,
					   shell_result.stdout_len)) {
				out_text = fy_sprintfa("binary output: %zu bytes",
						       shell_result.stdout_len);
				if (!cfg->markdown)
					fyai_report(ctx, "%s\n", out_text);
				out_val = fy_value(out_text);
			}
			err_text = shell_result.stderr_data;
			bounded = fyai_shell_bound_alloc(err_text, err_bytes);
			err_val = fy_gb_internalize(ctx->transient_gb,
					fy_value(bounded ? bounded : err_text));
			free(bounded);
			if (data_is_binary(shell_result.stderr_data,
					   shell_result.stderr_len)) {
				err_text = fy_sprintfa("binary stderr: %zu bytes",
						       shell_result.stderr_len);
				err_val = fy_value(err_text);
			}

			/*
			 * Report a timeout when the command reaches its time
			 * limit. Do not report the termination signal.
			 */
			output = fy_mapping(
				"stdout", out_val,
				"stderr", err_val,
				"outcome", shell_result.timed_out ?
					fy_mapping(
						"type", "timeout",
						"timeout_ms",
						(long long)shell_opts.timeout_ms) :
					shell_result.signaled ?
					fy_mapping(
						"type", "signal",
						"signal", shell_result.signal) :
					fy_mapping(
						"type", "exit",
						"exit_code",
						shell_result.exit_code));
			outputs = fy_append(outputs, output);
			if (shell_result.signaled || shell_result.exit_code)
				*okp = false;
			shell_command_result_cleanup(&shell_result);
		}
		fyai_shell_sandbox_end(&sb);
		result_generic = outputs;
		goto out;
	}

	switch (cfg->api_mode) {
	case FYAI_API_RESPONSES:
		name = fy_get(tool_call, "name", "");
		args_text = fy_get(tool_call, "arguments", "");
		break;
	case FYAI_API_CHAT_COMPLETIONS:
		name = fy_get(fy_get(tool_call, "function"), "name", "");
		args_text = fy_get(fy_get(tool_call, "function"), "arguments", "");
		break;
	case FYAI_API_MESSAGES:
		/* normalized to Responses-style function_call items */
		name = fy_get(tool_call, "name", "");
		args_text = fy_get(tool_call, "arguments", "");
		break;
	default:
		assert(0);
		__builtin_unreachable();
		break;
	}
	args = parse_json_string(ctx->transient_gb, args_text);
	if (fy_is_invalid(args))
		return fy_value(ctx->transient_gb, "tool error: invalid JSON arguments");
	if (fyai_mcp_tool_name(name)) {
		result_generic = fyai_mcp_call(ctx, name, args);
		goto out;
	}

	result_generic = fyai_tool_run_one(ctx, name, args, okp);
	/* Retain the resolved patch for display. */
	if (ctx->patch_display)
		fyai_patch_display_record(ctx, tool_call, ctx->patch_display);
out:
	result_generic = fy_gb_internalize(ctx->transient_gb, result_generic);
	if (fy_is_invalid(result_generic))
		fyai_error(ctx, "could not retain the tool result");
	return result_generic;
}

fy_generic fyai_tool_run_one(struct fyai_ctx *ctx, const char *name,
			     fy_generic args, bool *okp)
{
	fy_generic result_generic;
	const char *path, *content;
	char *result;
	int rc;

	*okp = false;
	if (fy_equal(name, "read_file")) {
		result = fyai_read_file_tool(ctx, args);
		*okp = result != NULL;
	} else if (fy_equal(name, "write_file")) {
		path = fy_get(args, "path", "");
		content = fy_get(args, "content", "");
		rc = write_text_file(path, content);
		result = strdup(!rc ? "ok" : "error");
		*okp = !rc;
	} else if (fy_equal(name, "apply_patch")) {
		content = fy_get(args, "patch", "");
		/* Resolve the patch before it changes the pre-image. */
		free(ctx->patch_display);
		ctx->patch_display = fyai_patch_to_unified_ctx(ctx, content);
		result = fyai_apply_patch_text_ctx(ctx, content);
		*okp = result && strncmp(result, "tool error:", 11);
	} else if (fy_equal(name, "shell")) {
		result = fyai_run_shell_command(ctx, args, okp);
	} else if (fy_equal(name, "shell_output")) {
		result = fyai_shell_output_tool(ctx, args, okp);
	} else if (fy_equal(name, "shell_input")) {
		result = fyai_shell_input_tool(ctx, args, okp);
	} else if (fy_equal(name, "agent_input")) {
		result = fyai_agent_input_tool(ctx, args, okp);
	} else if (fy_equal(name, "shell_close")) {
		result = fyai_shell_close_tool(ctx, args, okp);
	} else if (fy_equal(name, "time")) {
		result = fyai_time_tool(ctx, okp);
	} else if (fy_equal(name, "wait")) {
		result = fyai_wait_tool(ctx, args, okp);
	} else if (fy_equal(name, "ask_user")) {
		result_generic = fyai_ask_user(ctx, args);
		*okp = strncmp(fy_castp(&result_generic, ""),
			      "tool error:", 11) != 0;
		return result_generic;
	} else if (fy_equal(name, "agent")) {
		fy_generic agent_name;
		char *who;
		char *diag;

		/* Copy the name before fyai_agent_run() reopens the arena. */
		agent_name = fy_get(args, "name");
		who = fy_is_string(agent_name) ?
			strdup(fy_castp(&agent_name, "")) : NULL;
		result_generic = fyai_agent_run(ctx, args, okp);
		if (fy_is_invalid(result_generic)) {
			/* Quote the cause without consuming the user's diagnostic. */
			diag = fyai_diag_string(&ctx->cfg->diag);
			result_generic = fy_gb_internalize(ctx->transient_gb,
				fy_stringf("tool error: sub-agent%s%s%s "
					   "failed: %s",
					   who && *who ? " '" : "",
					   who ? who : "",
					   who && *who ? "'" : "",
					   diag && *diag ? diag :
					   "no reason was recorded"));
			free(diag);
			free(who);
			return result_generic;
		}
		free(who);
		return result_generic;
	} else {
		return fy_gb_internalize(ctx->transient_gb,
				fy_stringf("tool error: unknown tool %s", name));
	}

	if (result) {
		/*
		 * Internalize before freeing: fy_value() on a char * only
		 * references the buffer (long strings are not copied into the
		 * scratch generic), so freeing first leaves the deferred
		 * internalize reading freed memory - survivable for small
		 * results, silently empty for large ones.
		 */
		result_generic = fy_gb_internalize(ctx->transient_gb,
						   fy_value(result));
		free(result);
	} else {
		result_generic = fy_gb_internalize(ctx->transient_gb,
				fy_stringf("tool error: %s", strerror(errno)));
	}
	return result_generic;
}

/* Build the shell/tool sandbox spec from cfg and apply it to this process
 * irreversibly. Best-effort per spec->strict; a no-op when disabled. Marks
 * ctx->sandbox_applied so inner steps do not re-derive/re-apply it. */
static int fyai_tool_apply_sandbox(struct fyai_ctx *ctx)
{
	struct fyai_shell_sandbox sb;
	const struct fyai_sandbox_spec *spec;

	int rc = 0;

	if (fyai_shell_sandbox_begin(ctx, &sb, &spec))
		return -1;
	/* Fail closed: an unreported failure runs the tool unconfined. */
	if (spec)
		rc = fyai_sandbox_apply(spec);
	fyai_shell_sandbox_end(&sb);
	if (rc) {
		fyai_error(ctx, "sandbox: could not confine this process");
		return -1;
	}
	ctx->sandbox_applied = true;
	return 0;
}

/* These descriptors contain the private JSON-RPC channel. */
#define FYAI_TOOL_CHILD_REQ_FD 3	/* parent -> child, read by the child */
#define FYAI_TOOL_CHILD_RSP_FD 4	/* child -> parent, written by the child */

/* Install the control descriptors and close all other inherited descriptors. */
/* Make @slave the session leader's controlling terminal. */
static int fyai_tool_child_tty(int slave)
{
	if (ioctl(slave, TIOCSCTTY, 0) < 0)
		return -1;
	if (dup2(slave, STDIN_FILENO) < 0 ||
	    dup2(slave, STDOUT_FILENO) < 0 ||
	    dup2(slave, STDERR_FILENO) < 0)
		return -1;
	if (slave > STDERR_FILENO)
		close(slave);
	return 0;
}

static int fyai_tool_child_fds(int req_fd, int rsp_fd)
{
	int req_dup = -1, rsp_dup = -1;
	int devnull;
	int rc;

	/* Move both clear of the target numbers before dup2 can clobber one. */
	req_dup = fcntl(req_fd, F_DUPFD_CLOEXEC, 5);
	if (req_dup < 0)
		goto err;
	rsp_dup = fcntl(rsp_fd, F_DUPFD_CLOEXEC, 5);
	if (rsp_dup < 0)
		goto err;

	rc = dup2(req_dup, FYAI_TOOL_CHILD_REQ_FD);
	if (rc < 0)
		goto err;
	rc = dup2(rsp_dup, FYAI_TOOL_CHILD_RSP_FD);
	if (rc < 0)
		goto err;
	/* Do not pass the control channel to a shell command. */
	rc = fcntl(FYAI_TOOL_CHILD_REQ_FD, F_SETFD, FD_CLOEXEC);
	if (rc < 0)
		goto err;
	rc = fcntl(FYAI_TOOL_CHILD_RSP_FD, F_SETFD, FD_CLOEXEC);
	if (rc < 0)
		goto err;

	/* Detach unused input unless the child owns this terminal. */
	if (isatty(STDIN_FILENO) && ttyname(STDIN_FILENO) &&
	    getsid(0) == tcgetsid(STDIN_FILENO)) {
		fyai_close_fds_from(FYAI_TOOL_CHILD_RSP_FD + 1);
		return 0;
	}
	devnull = open("/dev/null", O_RDONLY);
	if (devnull < 0)
		goto err;
	rc = dup2(devnull, STDIN_FILENO);
	if (devnull != STDIN_FILENO)
		close(devnull);
	if (rc < 0)
		goto err;

	fyai_close_fds_from(FYAI_TOOL_CHILD_RSP_FD + 1);
	return 0;

err:
	if (req_dup >= 0)
		close(req_dup);
	if (rsp_dup >= 0)
		close(rsp_dup);
	return -1;
}


struct fyai_tool_child {
	struct fyai_ctx *ctx;
	struct jsonrpc_conn *conn;
	struct fyai_terminal_relay *relay;	/* a session this child drives */
	fy_generic id;
	fy_generic args;
	fy_generic branch;	/* sub-agent branch named by the parent */
	bool pending;
	bool done;
	bool session_started;	/* the session this child was asked for opened */
};

static fy_generic fyai_tool_child_serve(struct jsonrpc_conn *conn,
					const char *method, fy_generic params,
					fy_generic id, void *userdata,
					fy_generic *errorp)
{
	struct fyai_tool_child *tc = userdata;
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(tc->ctx);
	char *bytes;
	size_t len;

	if (!strcmp(method, "tool/run")) {
		if (!fy_is_valid(id) || tc->pending || tc->done) {
			*errorp = fy_gb_mapping(gb, "code", -32600LL,
						"message", "unexpected tool/run");
			return fy_invalid;
		}
		tc->args = fy_get(params, "call", fy_invalid);
		tc->branch = fy_get(params, "branch", fy_invalid);
		tc->id = id;
		tc->pending = true;
		jsonrpc_conn_defer(conn);
		return fy_invalid;
	}
	if (!strcmp(method, "tty/resize")) {
		tc->ctx->tty_rows = (int)fy_get(params, "rows", 0LL);
		tc->ctx->tty_cols = (int)fy_get(params, "cols", 0LL);
		fyai_terminal_session_resize(tc->ctx, tc->ctx->tty_rows,
					     tc->ctx->tty_cols);
		if (tc->relay)
			fyai_terminal_relay_resize(tc->relay, tc->ctx->tty_rows,
						   tc->ctx->tty_cols);
		return fy_invalid;
	}
	if (!strcmp(method, "shell/write")) {
		bytes = fyai_bytes_from_generic(params, &len);
		if (bytes && tc->relay)
			(void)fyai_terminal_relay_write(tc->relay, bytes, len);
		free(bytes);
		return fy_invalid;
	}
	if (!strcmp(method, "shell/close")) {
		if (tc->relay)
			fyai_terminal_relay_close(tc->relay,
					fy_get(params, "force", false));
		return fy_invalid;
	}
	*errorp = fy_gb_mapping(gb, "code", -32601LL,
				"message", "method not found");
	return fy_invalid;
}

/* Open a session, answer its start, then serve it until it ends. */
static void fyai_tool_child_session(struct fyai_ctx *ctx,
				    struct fyai_tool_child *tc,
				    struct jsonrpc_conn *conn)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	const struct fyai_sandbox_spec *sandbox;
	struct fyai_terminal_opts opts = {};
	struct fyai_shell_sandbox sb;
	struct fyai_event_loop *el;
	fy_generic args, command, workdir;
	fy_generic diag;
	bool started;
	int rc;

	args = fyai_tool_call_args(ctx, tc->args);
	command = fy_get(args, "command", fy_invalid);
	workdir = fy_get(args, "workdir", fy_invalid);

	rc = fyai_shell_sandbox_begin(ctx, &sb, &sandbox);
	if (!rc) {
		opts.workdir = fy_castp(&workdir, (const char *)NULL);
		opts.term = ctx->cfg->shell_tty_term;
		fyai_shell_tty_size(ctx, args, &opts.rows, &opts.cols);
		/* Use pipes unless the call explicitly requests a terminal. */
		opts.pipes = !fyai_shell_tty_requested(ctx, args);
		opts.shell = fyai_shell_shell_requested(ctx, args);
		opts.login = fyai_shell_login_requested(ctx, args);
		tc->relay = fyai_terminal_relay_start(ctx, conn,
						fy_castp(&command, ""),
						sandbox, &opts);
		/* The spec is only needed by the fork inside the start. */
		fyai_shell_sandbox_end(&sb);
	}
	if (rc)
		fyai_error(ctx,
			   "shell: the session was refused before it started");
	else if (!tc->relay)
		fyai_error(ctx, "shell: the session terminal did not open");

	/* A C comparison is an int; the flag must reach the wire as a bool. */
	started = tc->relay != NULL;
	tc->session_started = started;
	diag = fyai_diag_take_generic(&ctx->cfg->diag, gb);
	jsonrpc_conn_respond(conn, tc->id,
		fy_mapping("result",
			   fy_mapping("session", started,
				      "rows", (long long)opts.rows,
				      "cols", (long long)opts.cols),
			   "ok", started, "display", fy_null, "diag", diag),
		fy_invalid);
	if (!tc->relay)
		return;

	/* Stop the program when this serving child is terminated. */
	el = fyai_ctx_loop(ctx);
	assert(el);
	while (!fyai_terminal_relay_done(tc->relay)) {
		if (ctx->terminate_pending)
			fyai_terminal_relay_close(tc->relay, true);
		if (fyai_event_loop_step(el, ctx->terminate_pending ? 200 : -1) < 0)
			break;
		if (ctx->terminate_pending && fyai_terminal_relay_reaped(tc->relay))
			break;
	}

	fyai_terminal_relay_destroy(tc->relay);
	tc->relay = NULL;
}

static void fyai_tool_child_serve_loop(struct fyai_ctx *ctx)
{
	struct fyai_tool_child tc;
	struct fyai_event_loop *el;
	struct jsonrpc_conn *conn;
	fy_generic result, diag;
	bool ok = false;

	memset(&tc, 0, sizeof(tc));
	tc.ctx = ctx;

	el = fyai_ctx_loop(ctx);
	if (!el)
		_exit(1);
	/* Write responses to fd 4 and read requests from fd 3. */
	conn = jsonrpc_conn_stdio(ctx, FYAI_TOOL_CHILD_RSP_FD,
				  FYAI_TOOL_CHILD_REQ_FD, 0, "tool", NULL);
	if (!conn || jsonrpc_conn_serve(conn, fyai_tool_child_serve, &tc))
		_exit(1);
	ctx->tool_rpc = conn;

	while (!tc.done) {
		if (tc.pending) {
			tc.pending = false;
			tc.done = true;
			if (fy_is_invalid(tc.args)) {
				jsonrpc_conn_respond(conn, tc.id, fy_invalid,
					fy_gb_mapping(fyai_ctx_transient_gb(ctx),
						      "code", -32602LL,
						      "message", "call is required"));
				break;
			}
			/* Keep the durable branch on the child context. */
			if (fy_is_string(tc.branch))
				ctx->agent_branch =
					strdup(fy_castp(&tc.branch, ""));
			/*
			 * A sub-agent needs provider credentials after a persona
			 * changes its model. Its own tool children sanitize again.
			 */
			if (!fy_equal(fyai_tool_call_name(ctx, tc.args), "agent") &&
			    fyai_env_sanitize())
				fyai_error(ctx,
					   "could not remove every credential from the tool environment");
			if (fyai_shell_session_call(ctx, tc.args)) {
				fyai_tool_child_session(ctx, &tc, conn);
				/* Exit according to whether the session opened. */
				ok = tc.session_started;
				break;
			}
			result = fyai_execute_tool_call(ctx, tc.args, &ok);
			/* Return child diagnostics with the tool result. */
			diag = fyai_diag_take_generic(&ctx->cfg->diag,
						      fyai_ctx_transient_gb(ctx));
			jsonrpc_conn_respond(conn, tc.id,
				fy_gb_mapping(fyai_ctx_transient_gb(ctx),
					      "result", result, "ok", ok,
					      "display", ctx->patch_display ?
						fy_value(ctx->patch_display) :
						fy_null,
					      "diag", diag),
				fy_invalid);
			break;
		}
		if (fyai_event_loop_step(el, -1) < 0)
			_exit(1);
	}

	while (jsonrpc_conn_has_output(conn))
		if (fyai_event_loop_step(el, 1000) <= 0)
			break;
	/* This child leaves through _exit and never returns to main(). */
	fyai_prof_report();
	_exit(ok ? 0 : 1);
}

/*
 * The job owns its source pointers so withdrawing is idempotent: the callbacks
 * retire a source when it is spent and the collect path retires whatever is
 * left, and neither has to know what the other did. The loop is shared and
 * outlives the job, so a source left behind would point at freed storage.
 */
struct fyai_tool_job {
	struct fyai_ctx *ctx;		/* the loop the job's sources live on */
	fy_generic call;		/* the tool call, sent as tool/run */
	struct jsonrpc_conn *conn;	/* control channel to the child */
	struct jsonrpc_request *run;	/* the outstanding tool/run */
	fy_generic result;
	fy_generic display;		/* tool-resolved presentation, if any */
	fy_generic diag;		/* diagnostics collected by the child */
	char *origin;			/* who the child was, for a diagnostic */
	pid_t pid;
	int rfd;
	int pfd;
	struct fyai_fenced_stream stream;
	struct fyai_sink_band *band;
	char *title;
	char *command;
	struct fyai_event_source *csrc;
	bool out_open;		/* tool/run still outstanding */
	bool have_result;
	bool reaped;
	bool failed;
	bool result_ok;
	bool done;
	bool native_shell;
	bool agent;
	bool band_progress;
	int pty;			/* terminal of a sub-agent, -1 if none */
	int pty_rows, pty_cols;
	struct fyai_terminal_view *view;	/* what it drew there */
	struct fytim_surface *surface;		/* and where that is shown */
	struct fyai_event_source *ptysrc;
	struct fyai_event_source *waiter;	/* asks if it stopped for input */
	bool wants_input;
	bool terminating;
	bool timed_out;
	unsigned int timeout_ms;	/* 0 = no limit */
	struct response_buffer progress;	/* tail, for a timeout report */
	char *branch;			/* sub-agent branch, allocated by us */
	struct fyai_event_source *deadline;
	int term_signal;
	struct fyai_tool_job_group *group;
	struct fyai_tool_job *next;	/* ctx->tool_jobs, for a resize */
	struct fyai_shell_session *session;	/* the session this job drives */
};

/* A named job whose terminal view remains readable after exit. */
struct fyai_shell_session {
	struct fyai_shell_session *next;
	struct fyai_ctx *ctx;
	char *name;
	char *command;
	char *branch;
	struct fyai_tool_job *job;	/* NULL once the process has gone */
	struct fyai_terminal_view *view;
	struct fyai_event_source *idle;
	/* The session owns one live surface until its program stops. */
	struct fytim_surface *surface;
	char *title;
	pid_t pid;			/* the program, watched for a read */
	bool pipes;			/* it was given no terminal */
	struct fyai_event_source *waiter;
	bool wants_input;		/* it stopped for input and was said so */
	int rows;			/* the size the session was opened with */
	int exit_code;
	int signal;
	bool exited;
	bool closing;
	bool timed_out;
};

/* Size an agent terminal to its display surface. */
#define FYAI_AGENT_TTY_ROWS	12
#define FYAI_AGENT_TTY_MARGIN	2

static void fyai_agent_tty_size(struct fyai_ctx *ctx, int *rowsp, int *colsp)
{
	int rows = 0, cols = 0;

	if (fyai_ui_size(ctx, &cols, &rows) || cols < 1) {
		cols = markdown_render_width();
		rows = markdown_render_height();
	}
	cols -= FYAI_AGENT_TTY_MARGIN;
	if (cols < FYAI_TTY_COLS_DEFAULT / 4)
		cols = FYAI_TTY_COLS_DEFAULT / 4;
	if (rows > FYAI_AGENT_TTY_ROWS || rows < 1)
		rows = FYAI_AGENT_TTY_ROWS;
	*rowsp = rows;
	*colsp = cols;
}

/* Keep the live jobs reachable, so that a resize finds every child. */
static void fyai_tool_job_link(struct fyai_ctx *ctx, struct fyai_tool_job *job)
{
	job->next = ctx->tool_jobs;
	ctx->tool_jobs = job;
}

static void fyai_tool_job_unlink(struct fyai_ctx *ctx,
				 struct fyai_tool_job *job)
{
	struct fyai_tool_job **pp;

	for (pp = &ctx->tool_jobs; *pp; pp = &(*pp)->next) {
		if (*pp != job)
			continue;
		*pp = job->next;
		job->next = NULL;
		return;
	}
}

/*
 * The window of the user changed. A tool child called setsid(), so the kernel
 * does not signal it. The parent sends the new size on the control channel,
 * and the child applies it to its pseudo-terminal.
 */
void fyai_tool_jobs_resize(struct fyai_ctx *ctx, int rows, int cols)
{
	struct fy_generic_builder *gb;
	struct fyai_tool_job *job;

	if (!ctx || rows <= 0 || cols <= 0)
		return;
	gb = fyai_ctx_transient_gb(ctx);
	if (!gb)
		return;

	for (job = ctx->tool_jobs; job; job = job->next) {
		if (!job->conn || job->done)
			continue;
		(void)jsonrpc_notify(job->conn, "tty/resize",
				fy_gb_mapping(gb, "rows", (long long)rows,
					      "cols", (long long)cols));
	}
}


/* A session name is short and says what the shell is for. */
#define FYAI_SHELL_SESSION_NAME_MAX	32
/* Time a program gets to answer input before the reading is taken. */
#define FYAI_SHELL_INPUT_WAIT_MS	250
#define FYAI_SHELL_INPUT_WAIT_MAX_MS	30000
/* Time a program gets to leave after it is asked to. */
#define FYAI_TTY_CLOSE_WAIT_MS		500

static void fyai_tool_job_discard(struct fyai_tool_job *job);
static void fyai_shell_session_close(struct fyai_shell_session *sess,
				     bool force);

/* The live display of a session shows its lines, as a normal tool call does. */
static void fyai_shell_session_line(void *userdata,
				    enum shell_output_stream stream,
				    const char *data, size_t len)
{
	struct fyai_shell_session *sess = userdata;

	(void)stream;
	if (!sess->job || !sess->job->stream.active)
		return;
	(void)fyai_fenced_stream_push(&sess->job->stream, data, len);
}

struct fyai_shell_session *fyai_shell_session_find(struct fyai_ctx *ctx,
						   const char *name)
{
	struct fyai_shell_session *sess;

	if (!ctx || !name || !*name)
		return NULL;
	for (sess = ctx->shell_sessions; sess; sess = sess->next)
		if (!strcmp(sess->name, name))
			return sess;
	return NULL;
}

/*
 * A session name says what the shell is for. Write it as a sub-agent name is
 * written: letters, digits, a dash or an underscore.
 */
static bool fyai_shell_session_name_valid(const char *name)
{
	size_t i;

	if (!name || !*name || strlen(name) > FYAI_SHELL_SESSION_NAME_MAX)
		return false;
	for (i = 0; name[i]; i++) {
		if (isalnum((unsigned char)name[i]) || name[i] == '-' ||
		    name[i] == '_' || name[i] == '/')
			continue;
		return false;
	}
	return true;
}

static void fyai_shell_session_idle_arm(struct fyai_shell_session *sess);

/* Nothing was read from or written to the session: end it. */
static enum fyai_event_action
fyai_shell_session_idle(const struct fyai_event *ev)
{
	struct fyai_shell_session *sess = ev->userdata;

	sess->idle = NULL;
	if (sess->exited)
		return FYAIEA_CONTINUE;
	sess->timed_out = true;
	fyai_shell_session_close(sess, false);
	return FYAIEA_CONTINUE;
}

static void fyai_shell_session_idle_arm(struct fyai_shell_session *sess)
{
	struct fyai_event_loop *el;
	int ms;

	ms = sess->ctx->cfg->shell_session_timeout_ms;
	if (sess->idle) {
		if (ms > 0)
			(void)fyai_event_timer_rearm(sess->idle, ms, 0);
		return;
	}
	if (ms <= 0 || sess->exited)
		return;
	el = fyai_ctx_loop(sess->ctx);
	if (!el)
		return;
	(void)fyai_event_add_timer(el, ms, 0, fyai_shell_session_idle, sess,
				   &sess->idle);
}

/* Create the session and reserve its name, before the process is spawned. */
static struct fyai_shell_session *
fyai_shell_session_create(struct fyai_ctx *ctx, const char *name,
			  const char *command, const char *title, int rows,
			  int cols, size_t max_bytes, bool pipes)
{
	struct fyai_shell_session *sess;

	sess = calloc(1, sizeof(*sess));
	if (!sess)
		return NULL;
	sess->ctx = ctx;
	sess->name = strdup(name);
	sess->command = strdup(command ? command : "");
	sess->branch = strdup(fyai_ctx_branch(ctx));
	sess->view = fyai_terminal_view_create(ctx, rows, cols, max_bytes);
	fyai_error_check(ctx,
			 sess->name && sess->command && sess->branch && sess->view,
			 fail, "shell: could not allocate the terminal session");
	/* Pipe output uses bare line feeds. */
	fyai_terminal_view_cooked(sess->view, pipes);
	fyai_terminal_view_line_cb(sess->view, fyai_shell_session_line, sess);
	sess->title = title ? strdup(title) : NULL;
	sess->rows = rows;
	sess->pipes = pipes;
	sess->surface = fyai_ui_surface_open(ctx, rows, cols);
	if (sess->surface) {
		(void)fyai_ui_surface_set_head(ctx, sess->surface,
					       sess->title ? sess->title :
					       "**shell**", NULL,
					       FYAI_UI_MARK_RUNNING);
		(void)fyai_ui_surface_set_margin(sess->surface,
						 ctx->cfg->session_margin);
		/* Publish the initial blank screen. */
		fyai_terminal_view_damage_all(sess->view);
		if (fyai_ui_surface_publish(sess->surface, sess->view) > 0)
			fyai_ui_wake(ctx);
	}
	sess->next = ctx->shell_sessions;
	ctx->shell_sessions = sess;
	fyai_shell_session_idle_arm(sess);
	return sess;

fail:
		fyai_terminal_view_destroy(sess->view);
		free(sess->name);
		free(sess->command);
		free(sess->branch);
		free(sess);
	return NULL;
}

/* Resize the program to the surface area it receives. */
static void fyai_shell_session_follow(struct fyai_shell_session *sess)
{
	struct fy_generic_builder *gb;
	int cols, rows, have_rows = 0, have_cols = 0;

	cols = fyai_ui_surface_granted_cols(sess->surface);
	if (cols < 1)
		return;
	fyai_terminal_view_size(sess->view, &have_rows, &have_cols);
	/* Do not grow beyond the session's initial height. */
	rows = fyai_ui_surface_granted_rows(sess->surface);
	if (rows < 1 || rows > sess->rows)
		rows = sess->rows;
	if (cols == have_cols && rows == have_rows)
		return;

	fyai_terminal_view_resize(sess->view, rows, cols);
	(void)fyai_ui_surface_resize(sess->surface, rows, cols);
	/*
	 * The process that drives the terminal is the child: it owns the
	 * pseudo-terminal, so only it can tell the program.
	 */
	if (!sess->job || !sess->job->conn || sess->job->done)
		return;
	gb = fyai_ctx_transient_gb(sess->ctx);
	if (!gb)
		return;
	(void)jsonrpc_notify(sess->job->conn, "tty/resize",
			     fy_gb_mapping(gb, "rows", (long long)rows,
					   "cols", (long long)cols));
}

/* Show what the program has drawn since the last look. */
static void fyai_shell_session_refresh(struct fyai_shell_session *sess)
{
	if (!sess || !sess->surface)
		return;
	fyai_shell_session_follow(sess);
	if (fyai_ui_surface_publish(sess->surface, sess->view) > 0)
		fyai_ui_wake(sess->ctx);
}

/* Queue the session's input request and visible prompt for the model. */
static void fyai_shell_session_input_wanted(struct fyai_shell_session *sess)
{
	char *prompt;
	char *text;

	prompt = fyai_terminal_view_last_line(sess->view);
	text = prompt && *prompt ?
		strdup(fy_sprintfa("[shell '%s' is waiting for input: %s]",
				   sess->name, prompt)) :
		strdup(fy_sprintfa("[shell '%s' is waiting for input]",
				   sess->name));
	free(prompt);
	if (text)
		(void)fyai_event_inject(sess->ctx, text);
}

/* Poll for transitions into and out of an input wait. */
static enum fyai_event_action fyai_shell_session_wait_poll(
					const struct fyai_event *ev)
{
	struct fyai_shell_session *sess = ev->userdata;
	bool wants;

	if (sess->exited || sess->closing)
		return FYAIEA_CONTINUE;
	wants = fyai_process_reads_stdin(sess->pid);
	if (wants == sess->wants_input)
		return FYAIEA_CONTINUE;
	sess->wants_input = wants;
	/* Said one time for each wait; reading on is not a new question. */
	if (wants)
		fyai_shell_session_input_wanted(sess);
	return FYAIEA_CONTINUE;
}

/* Watch @pid, the program this session runs, for a stop on its input. */
static void fyai_shell_session_watch(struct fyai_shell_session *sess, pid_t pid)
{
	struct fyai_event_loop *el;
	int ms;

	if (!sess || pid <= 0 || sess->waiter)
		return;
	sess->pid = pid;
	ms = sess->ctx->cfg->shell_input_poll_ms;
	el = fyai_ctx_loop(sess->ctx);
	if (ms <= 0 || !el)
		return;
	(void)fyai_event_add_timer(el, ms, ms, fyai_shell_session_wait_poll,
				   sess, &sess->waiter);
}

/* How the session ended, for the mark and the cause beside its title. */
static char *fyai_shell_session_cause(const struct fyai_shell_session *sess,
				      bool *okp)
{
	char buf[64];

	*okp = true;
	if (sess->timed_out) {
		*okp = false;
		return strdup("timed out");
	}
	/* An expected close signal is not a failure. */
	if (sess->closing)
		return NULL;
	if (sess->signal) {
		*okp = false;
		snprintf(buf, sizeof(buf), "killed by signal %d", sess->signal);
		return strdup(buf);
	}
	if (sess->exit_code) {
		*okp = false;
		snprintf(buf, sizeof(buf), "exit %d", sess->exit_code);
		return strdup(buf);
	}
	return NULL;
}

/* Commit the completed session screen to the transcript. */
static void fyai_shell_session_display_finish(struct fyai_shell_session *sess)
{
	char *cause;
	bool ok;

	if (!sess || !sess->surface)
		return;

	fyai_shell_session_refresh(sess);
	cause = fyai_shell_session_cause(sess, &ok);
	(void)fyai_ui_surface_set_head(sess->ctx, sess->surface,
				       sess->title ? sess->title : "**shell**",
				       cause,
				       ok ? FYAI_UI_MARK_OK :
					    FYAI_UI_MARK_FAILED);
	free(cause);
	fyai_ui_surface_commit(sess->ctx, sess->surface);
	sess->surface = NULL;
	fyai_ui_wake(sess->ctx);
}

/* The program ended. The view stays; only the process is gone. */
static void fyai_shell_session_exited(struct fyai_shell_session *sess,
				      int exit_code, int signal)
{
	if (!sess || sess->exited)
		return;
	sess->exited = true;
	sess->exit_code = exit_code;
	sess->signal = signal;
	fyai_shell_session_display_finish(sess);
	if (sess->idle) {
		fyai_event_source_remove(sess->idle);
		sess->idle = NULL;
	}
	/* Nothing waits for input once the program has gone. */
	if (sess->waiter) {
		fyai_event_source_remove(sess->waiter);
		sess->waiter = NULL;
	}
}

static void fyai_shell_session_close(struct fyai_shell_session *sess,
				     bool force)
{
	struct fy_generic_builder *gb;

	if (!sess || sess->exited || !sess->job || !sess->job->conn)
		return;
	gb = fyai_ctx_transient_gb(sess->ctx);
	if (!gb)
		return;
	sess->closing = true;
	(void)jsonrpc_notify(sess->job->conn, "shell/close",
			     fy_mapping("force", force));
	if (force && sess->job->pid > 0)
		(void)kill(-sess->job->pid, SIGKILL);
}

static void fyai_shell_session_destroy(struct fyai_shell_session *sess)
{
	if (!sess)
		return;
	/* A session torn down while its program was still there commits what
	 * it drew all the same: it was shown, so it is kept. */
	fyai_shell_session_display_finish(sess);
	if (sess->idle)
		fyai_event_source_remove(sess->idle);
	if (sess->waiter)
		fyai_event_source_remove(sess->waiter);
	fyai_terminal_view_destroy(sess->view);
	free(sess->name);
	free(sess->command);
	free(sess->branch);
	free(sess->title);
	free(sess);
}

/* End and release every session owned by this invocation. */
void fyai_shell_sessions_release(struct fyai_ctx *ctx, bool force)
{
	struct fyai_shell_session *sess, *next;
	struct fyai_tool_job *job;

	if (!ctx)
		return;
	for (sess = ctx->shell_sessions; sess; sess = next) {
		next = sess->next;
		job = sess->job;
		if (job) {
			job->session = NULL;
			sess->job = NULL;
			if (!job->reaped && job->pid > 0)
				(void)kill(-job->pid,
					   force ? SIGKILL : SIGTERM);
			fyai_tool_job_discard(job);
		}
		fyai_shell_session_destroy(sess);
	}
	ctx->shell_sessions = NULL;
}

/* The reading a session offers: what appeared, the screen, or part of it. */
static enum fyai_terminal_read fyai_shell_view_kind(fy_generic args,
						    struct fyai_terminal_region *region)
{
	fy_generic view, r;

	view = fy_get(args, "view", fy_invalid);
	if (fy_equal(view, "screen"))
		return FYAITR_SCREEN;
	if (fy_equal(view, "all"))
		return FYAITR_ALL;
	if (fy_equal(view, "region")) {
		r = fy_get(args, "region", fy_invalid);
		region->row = fy_get(r, "row", 0);
		region->col = fy_get(r, "col", 0);
		region->rows = fy_get(r, "rows", 0);
		region->cols = fy_get(r, "cols", 0);
		return FYAITR_REGION;
	}
	return FYAITR_NEW;
}

/* Say what the session is and what the reading is, then give the text. */
static char *fyai_shell_session_result(struct fyai_ctx *ctx,
				       struct fyai_shell_session *sess,
				       fy_generic args,
				       enum fyai_terminal_read what,
				       const struct fyai_terminal_region *region)
{
	struct response_buffer buf = {};
	char *text = NULL;
	char *bounded = NULL;
	size_t len = 0;
	int rows = 0, cols = 0;
	int rc;
	bool screen;

	screen = fyai_terminal_view_screen_mode(sess->view);
	fyai_terminal_view_size(sess->view, &rows, &cols);
	text = fyai_terminal_view_read(sess->view, what, region, &len);
	fyai_error_check(ctx, text, err,
			 "shell: could not read session '%s'", sess->name);

	if (screen)
		rc = response_buffer_append(&buf,
			fy_sprintfa("[shell '%s': the screen it draws, %dx%d]\n",
				    sess->name, rows, cols));
	else
		rc = response_buffer_append(&buf,
			fy_sprintfa("[shell '%s': %s]\n", sess->name,
				what == FYAITR_NEW ? "the lines since the last read" :
				"its output"));
	fyai_error_check(ctx, !rc, err,
			 "shell: could not build session '%s' result", sess->name);

	bounded = fyai_shell_bound_alloc(text,
					 fyai_shell_output_bytes(ctx, args));
	rc = response_buffer_append(&buf, bounded ? bounded : text);
	fyai_error_check(ctx, !rc, err,
			 "shell: could not retain session '%s' output", sess->name);
	free(bounded);
	bounded = NULL;
	free(text);
	text = NULL;

	if (sess->exited) {
		if (sess->signal)
			rc = response_buffer_append(&buf,
				fy_sprintfa("\n[shell '%s' ended: signal %d%s]\n",
					    sess->name, sess->signal,
					    sess->timed_out ? ", idle limit" : ""));
		else
			rc = response_buffer_append(&buf,
				fy_sprintfa("\n[shell '%s' ended: status %d]\n",
					    sess->name, sess->exit_code));
		fyai_error_check(ctx, !rc, err,
				 "shell: could not append session '%s' outcome",
				 sess->name);
	}
	return buf.data;

err:
	free(bounded);
	free(text);
	free(buf.data);
	return NULL;
}

/* Find the session a call names, or say why it cannot be used. */
static struct fyai_shell_session *
fyai_shell_session_of(struct fyai_ctx *ctx, fy_generic args, char **errp)
{
	struct fyai_shell_session *sess;
	fy_generic name;

	*errp = NULL;
	name = fy_get(args, "name", fy_invalid);
	sess = fyai_shell_session_find(ctx, fy_castp(&name, ""));
	if (sess)
		return sess;
	/*
	 * A session lives for one invocation. A name from an earlier one names
	 * nothing, so report that rather than let the model wait for output.
	 */
	*errp = strdup(fy_sprintfa(
		"tool error: no shell named '%s' is open on branch %s; "
		"open one with the shell tool and a name",
		fy_castp(&name, ""), fyai_ctx_branch(ctx)));
	return NULL;
}

static char *fyai_shell_output_tool(struct fyai_ctx *ctx, fy_generic args,
				    bool *okp)
{
	struct fyai_terminal_region region = {};
	struct fyai_shell_session *sess;
	enum fyai_terminal_read what;
	char *err;

	*okp = false;
	sess = fyai_shell_session_of(ctx, args, &err);
	if (!sess) {
		fyai_error_check(ctx, err, out,
				 "shell: could not report the missing session");
		return err;
	}

	fyai_shell_session_idle_arm(sess);
	what = fyai_shell_view_kind(args, &region);
	*okp = true;
	return fyai_shell_session_result(ctx, sess, args, what, &region);

out:
	return NULL;
}

static char *fyai_shell_input_tool(struct fyai_ctx *ctx, fy_generic args,
				   bool *okp)
{
	struct fyai_terminal_region region = {};
	struct fyai_shell_session *sess;
	struct fy_generic_builder *gb;
	struct fyai_event_loop *el;
	struct response_buffer in = {};
	fy_generic input;
	const char *text;
	long long wait;
	char *err, *result;
	int rc;

	*okp = false;
	sess = fyai_shell_session_of(ctx, args, &err);
	if (!sess) {
		fyai_error_check(ctx, err, out,
				 "shell: could not report the missing session");
		return err;
	}
	if (sess->exited)
		return strdup(fy_sprintfa(
			"tool error: the shell '%s' has ended; its output can "
			"still be read with shell_output", sess->name));

	gb = fyai_ctx_transient_gb(ctx);
	if (!gb || !sess->job || !sess->job->conn)
		return strdup("tool error: the shell session is not reachable");

	input = fy_get(args, "input", fy_invalid);
	text = fy_castp(&input, "");
	rc = response_buffer_append(&in, text);
	fyai_error_check(ctx, !rc, out,
			 "shell: could not retain session input");
	/* End the line as the session's terminal or pipe expects. */
	if (fy_get(args, "enter", true)) {
		rc = response_buffer_append(&in, sess->pipes ? "\n" : "\r");
		fyai_error_check(ctx, !rc, out,
				 "shell: could not append return to session input");
	}
	(void)jsonrpc_notify(sess->job->conn, "shell/write",
			     fyai_bytes_to_generic(gb, in.data, in.len));
	free(in.data);
	in.data = NULL;

	/* Let the program answer before the reading is taken. */
	wait = fy_get(args, "wait_ms", (long long)FYAI_SHELL_INPUT_WAIT_MS);
	if (wait < 0)
		wait = 0;
	if (wait > FYAI_SHELL_INPUT_WAIT_MAX_MS)
		wait = FYAI_SHELL_INPUT_WAIT_MAX_MS;
	el = fyai_ctx_loop(ctx);
	assert(el);
	if (wait)
		(void)fyai_event_sleep(el, wait);

	fyai_shell_session_idle_arm(sess);
	*okp = true;
	result = fyai_shell_session_result(ctx, sess, args,
					   fyai_shell_view_kind(args, &region),
					   &region);
	fyai_error_check(ctx, result, out,
			 "shell: could not read session '%s'", sess->name);
	return result;

out:
	free(in.data);
	return NULL;
}

static char *fyai_shell_close_tool(struct fyai_ctx *ctx, fy_generic args,
				   bool *okp)
{
	struct fyai_shell_session *sess;
	struct fyai_event_loop *el;
	bool force;
	char *err, *result;

	*okp = false;
	sess = fyai_shell_session_of(ctx, args, &err);
	if (!sess) {
		fyai_error_check(ctx, err, out,
				 "shell: could not report the missing session");
		return err;
	}

	force = fy_get(args, "force", false);
	if (!sess->exited) {
		fyai_shell_session_close(sess, force);
		/* Give the program the time to leave before reporting. */
		el = fyai_ctx_loop(ctx);
		assert(el);
		(void)fyai_event_sleep(el, force ? 100 :
					       FYAI_TTY_CLOSE_WAIT_MS);
	}
	*okp = true;
	if (sess->exited)
		result = strdup(fy_sprintfa("[shell '%s' ended: status %d]",
					  sess->name, sess->signal ?
					  128 + sess->signal : sess->exit_code));
	else
		result = strdup(fy_sprintfa("[shell '%s' was asked to end]",
					    sess->name));
	fyai_error_check(ctx, result, out,
			 "shell: could not build session close result");
	return result;

out:
	return NULL;
}

bool fyai_tool_call_parallel_eligible(struct fyai_ctx *ctx,
				      fy_generic tool_call)
{
	const char *name;

	name = fyai_tool_call_name(ctx, tool_call);
	/* Keep tools that use parent-owned sessions or timers in the parent. */
	return !fy_equal(name, "ask_user") &&
	       !fy_any_equal(name, "shell_output", "shell_input",
			     "shell_close") &&
	       !fy_any_equal(name, "time", "wait") &&
	       !fyai_mcp_tool_name(name);
}

/* Keep the sub-agent PTY aligned with its display surface. */
static void fyai_agent_view_follow(struct fyai_tool_job *job)
{
	struct winsize ws = {};
	int rows, cols;

	cols = fyai_ui_surface_granted_cols(job->surface);
	rows = fyai_ui_surface_granted_rows(job->surface);
	if (cols < 1)
		return;
	if (rows < 1)
		rows = job->pty_rows;
	if (cols == job->pty_cols && rows == job->pty_rows)
		return;

	job->pty_cols = cols;
	job->pty_rows = rows;
	ws.ws_row = (unsigned short)rows;
	ws.ws_col = (unsigned short)cols;
	(void)ioctl(job->pty, TIOCSWINSZ, &ws);
	fyai_terminal_view_resize(job->view, rows, cols);
	(void)fyai_ui_surface_resize(job->surface, rows, cols);
}

/* Show what the sub-agent has drawn on its terminal since the last look. */
static void fyai_agent_view_refresh(struct fyai_tool_job *job)
{
	if (!job->surface || !job->view)
		return;
	fyai_agent_view_follow(job);
	if (fyai_ui_surface_publish(job->surface, job->view) > 0)
		fyai_ui_wake(job->ctx);
}

static enum fyai_event_action fyai_agent_pty_read(const struct fyai_event *ev)
{
	struct fyai_tool_job *job = ev->userdata;
	char buf[4096];
	ssize_t n;

	for (;;) {
		n = read(job->pty, buf, sizeof(buf));
		if (n > 0) {
			/* It wrote: whatever it waited for, it has it. */
			job->wants_input = false;
			(void)fyai_terminal_view_feed(job->view, buf,
						      (size_t)n);
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
			fyai_agent_view_refresh(job);
			return FYAIEA_CONTINUE;
		}
		/* EIO is the last slave closing: the sub-agent has gone. */
		fyai_agent_view_refresh(job);
		if (job->ptysrc) {
			fyai_event_source_remove(job->ptysrc);
			job->ptysrc = NULL;
		}
		return FYAIEA_CONTINUE;
	}
}

/* Queue a sub-agent's new input request for the model. */
static enum fyai_event_action fyai_agent_wait_poll(const struct fyai_event *ev)
{
	struct fyai_tool_job *job = ev->userdata;
	const char *who;
	char *prompt = NULL;
	char *text;
	bool wants;

	if (job->done)
		return FYAIEA_CONTINUE;
	wants = fyai_process_reads_stdin(job->pid);
	if (wants == job->wants_input)
		return FYAIEA_CONTINUE;
	job->wants_input = wants;
	if (!wants)
		return FYAIEA_CONTINUE;
	if (job->view)
		prompt = fyai_terminal_view_last_line(job->view);
	who = fyai_agent_job_name(job);
	text = prompt && *prompt ?
		strdup(fy_sprintfa("[agent '%s' is waiting for input: %s]",
				   who, prompt)) :
		strdup(fy_sprintfa("[agent '%s' is waiting for input]", who));
	free(prompt);
	if (text)
		(void)fyai_event_inject(job->ctx, text);
	return FYAIEA_CONTINUE;
}

/* Write the model's answer to a running sub-agent terminal. */
static char *fyai_agent_input_tool(struct fyai_ctx *ctx, fy_generic args,
				   bool *okp)
{
	struct fyai_tool_job *job;
	struct response_buffer in = {};
	fy_generic name_v, input_v;
	const char *name;
	const char *text;
	ssize_t n;
	size_t off;

	*okp = false;
	name_v = fy_get(args, "name", fy_invalid);
	name = fy_castp(&name_v, "");
	job = fyai_agent_job_named(ctx, name);
	if (!job)
		return strdup(fy_sprintfa(
			"tool error: no sub-agent named '%s' is running; one "
			"that has finished is reached with the agent tool",
			name));
	if (job->pty < 0)
		return strdup(fy_sprintfa(
			"tool error: the sub-agent '%s' has no terminal to "
			"write to", name));

	input_v = fy_get(args, "input", fy_invalid);
	text = fy_castp(&input_v, "");
	fyai_error_check(ctx, !response_buffer_append(&in, text), err,
			 "agent: could not retain input for '%s'", name);
	/* Its terminal turns the return into the end of a line for it. */
	if (fy_get(args, "enter", true))
		fyai_error_check(ctx, !response_buffer_append(&in, "\r"), err,
				 "agent: could not append return for '%s'", name);
	for (off = 0; off < in.len; off += (size_t)n) {
		n = write(job->pty, in.data + off, in.len - off);
		if (n > 0)
			continue;
		if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
			n = 0;
			continue;
		}
		free(in.data);
		return strdup(fy_sprintfa(
			"tool error: could not write to the sub-agent '%s': %s",
			name, strerror(errno)));
	}
	free(in.data);
	/* It read what it was given: the next stop is a new question. */
	job->wants_input = false;
	*okp = true;
	return strdup(fy_sprintfa("[agent '%s': the input was typed]", name));

err:
	free(in.data);
	return NULL;
}

/* Open a display surface for a sub-agent terminal. */
static int fyai_agent_view_open(struct fyai_ctx *ctx,
				struct fyai_tool_job *job)
{
	struct fyai_event_loop *el;
	int flags, rc;

	if (job->pty < 0)
		return 0;

	flags = fcntl(job->pty, F_GETFL, 0);
	if (flags >= 0)
		(void)fcntl(job->pty, F_SETFL, flags | O_NONBLOCK);

	job->view = fyai_terminal_view_create(ctx, job->pty_rows,
					      job->pty_cols, 0);
	if (!job->view)
		return -1;
	el = fyai_ctx_loop(ctx);
	if (!el || fyai_event_add_fd(el, job->pty, FYAIEV_READ,
				     fyai_agent_pty_read, job, &job->ptysrc))
		return -1;
	/* The same watch a session has, on the terminal of a sub-agent. */
	if (ctx->cfg->shell_input_poll_ms > 0) {
		rc = fyai_event_add_timer(el, ctx->cfg->shell_input_poll_ms,
					   ctx->cfg->shell_input_poll_ms,
					   fyai_agent_wait_poll, job,
					   &job->waiter);
		fyai_error_check(ctx, !rc, err,
				 "agent: could not watch for input requests");
	}

	job->surface = fyai_ui_surface_open(ctx, job->pty_rows, job->pty_cols);
	if (job->surface) {
		(void)fyai_ui_surface_set_head(ctx, job->surface,
					       job->title ? job->title :
					       "**agent**", NULL,
					       FYAI_UI_MARK_RUNNING);
		(void)fyai_ui_surface_set_margin(job->surface,
						 ctx->cfg->session_margin);
		/* Publish the initial blank screen. */
		fyai_terminal_view_damage_all(job->view);
		fyai_agent_view_refresh(job);
	}
	return 0;

err:
	return -1;
}

/* The sub-agent is done: its last screen is committed, with its outcome. */
static void fyai_agent_view_close(struct fyai_tool_job *job, bool ok,
				  const char *cause)
{
	if (job->waiter) {
		fyai_event_source_remove(job->waiter);
		job->waiter = NULL;
	}
	if (job->ptysrc) {
		fyai_event_source_remove(job->ptysrc);
		job->ptysrc = NULL;
	}
	if (job->surface) {
		fyai_agent_view_refresh(job);
		(void)fyai_ui_surface_set_head(job->ctx, job->surface,
					       job->title ? job->title :
					       "**agent**", cause,
					       ok ? FYAI_UI_MARK_OK :
						    FYAI_UI_MARK_FAILED);
		fyai_ui_surface_commit(job->ctx, job->surface);
		job->surface = NULL;
		fyai_ui_wake(job->ctx);
	}
	if (job->pty >= 0) {
		close(job->pty);
		job->pty = -1;
	}
	fyai_terminal_view_destroy(job->view);
	job->view = NULL;
}

static void fyai_tool_job_drop(struct fyai_event_source **srcp)
{
	fyai_event_source_remove(*srcp);
	*srcp = NULL;
}

static void fyai_tool_job_update_done(struct fyai_tool_job *job)
{
	bool was_done = job->done;

	/* A session job completes when its start answer arrives. */
	job->done = job->session ? !job->out_open :
				   (job->reaped && !job->out_open);
	if (!was_done && job->done) {
		/* Remove the deadline before the job waits for its group. */
		fyai_tool_job_drop(&job->deadline);
	}
	if (!was_done && job->done && job->stream.active) {
		/*
		 * Test `timed_out` here and during collection. The parent owns
		 * the deadline. A stopped job can report success immediately
		 * before termination. This state selects the mark before
		 * collection corrects the result.
		 */
		(void)fyai_fenced_stream_set_indicator(&job->stream,
			job->result_ok && !job->failed && !job->timed_out ?
			FYMD_INDICATOR_SUCCESS : FYMD_INDICATOR_FAILURE, 0);
		(void)fyai_fenced_stream_push(&job->stream, NULL, 0);
	}
	if (!was_done && job->done && job->group)
		fyai_tool_job_group_service(job->group);
}

/* Handle termination in the child event loop. Ignore SIGPIPE. */
static enum fyai_event_action fyai_tool_child_signal(const struct fyai_event *ev)
{
	struct fyai_ctx *ctx = ev->userdata;

	if (ev->signo == SIGPIPE)
		return FYAIEA_CONTINUE;
	ctx->interrupt_pending = true;
	ctx->terminate_pending = true;
	return FYAIEA_CONTINUE;
}

static void fyai_tool_child_signals(struct fyai_ctx *ctx)
{
	static const int signals[] = { SIGTERM, SIGHUP, SIGPIPE };
	struct fyai_event_loop *el;
	struct fyai_event_source *src;
	size_t i;
	int rc;

	el = fyai_ctx_loop(ctx);
	if (!el)
		return;
	for (i = 0; i < ARRAY_SIZE(signals); i++) {
		src = NULL;
		rc = fyai_event_add_signal(el, signals[i],
					   fyai_tool_child_signal, ctx, &src);
		(void)rc;
	}
}

static enum fyai_event_action fyai_tool_job_child(const struct fyai_event *ev)
{
	struct fyai_tool_job *job = ev->userdata;

	job->csrc = NULL;
	job->reaped = true;
	/* Preserve a session start result after its serving child exits. */
	if (!job->have_result)
		job->result_ok = WIFEXITED(ev->status) &&
				 WEXITSTATUS(ev->status) == 0;
	job->term_signal = WIFSIGNALED(ev->status) ?
				WTERMSIG(ev->status) : 0;
	fyai_tool_job_update_done(job);
	return FYAIEA_CONTINUE;
}

/* A sub-agent is named by the branch it owns: main/agent:<name>. */
static const char *fyai_agent_job_name(const struct fyai_tool_job *job)
{
	const char *who;

	who = job->branch ? strstr(job->branch, FYAI_BRANCH_AGENT_PREFIX) : NULL;
	return who ? who + strlen(FYAI_BRANCH_AGENT_PREFIX) : "agent";
}

/* The running sub-agent called @name, or NULL. */
static struct fyai_tool_job *fyai_agent_job_named(struct fyai_ctx *ctx,
						  const char *name)
{
	struct fyai_tool_job *job;
	const char *who;

	if (fy_str_empty(name))
		return NULL;
	for (job = ctx->tool_jobs; job; job = job->next) {
		if (!job->agent || job->done || !job->branch)
			continue;
		who = strstr(job->branch, FYAI_BRANCH_AGENT_PREFIX);
		if (who && !strcmp(who + strlen(FYAI_BRANCH_AGENT_PREFIX),
				   name))
			return job;
	}
	return NULL;
}

/* Present a named sub-agent's question and return the user's answer. */
static fy_generic fyai_ask_user_for_child(struct fyai_tool_job *job,
					  struct jsonrpc_conn *conn,
					  fy_generic id, fy_generic params)
{
	struct fy_generic_builder *gb;
	fy_generic question, asked;
	fy_generic answer;
	const char *who;
	int rc;

	gb = fyai_ctx_transient_gb(job->ctx);
	who = fyai_agent_job_name(job);
	question = fy_get(params, "question", fy_invalid);
	asked = fy_value(gb, fy_sprintfa("the sub-agent '%s' asks: %s", who,
					 fy_castp(&question, "")));
	answer = fyai_ask_user(job->ctx, fy_gb_mapping(gb, "question", asked,
					"options", fy_get(params, "options",
							  fy_invalid)));
	rc = jsonrpc_conn_respond(conn, id,
				  fy_gb_mapping(gb, "answer", answer),
				  fy_invalid);
	fyai_error_check(job->ctx, !rc, err,
			 "agent: could not return the user's answer to '%s'", who);

err:
	return fy_invalid;
}

/* True while a job of this process owns @branch. */
static bool fyai_tool_job_branch_live(struct fyai_ctx *ctx, const char *branch)
{
	const struct fyai_tool_job *job;

	for (job = ctx->tool_jobs; job; job = job->next) {
		if (job->branch && !strcmp(job->branch, branch) && !job->done)
			return true;
	}
	return false;
}

/* Spawn a tool child, optionally with a PTY on its standard descriptors. */
static int fyai_tool_job_spawn(struct fyai_ctx *ctx,
			       struct fyai_tool_job *job, bool pty)
{
	int req[2] = { -1, -1 };	/* parent -> child */
	int rsp[2] = { -1, -1 };	/* child -> parent */
	int master = -1, slave = -1;
	struct winsize ws = {};
	int rows = 0, cols = 0;
	pid_t pid;
	int rc;

	memset(job, 0, sizeof(*job));
	job->ctx = ctx;
	job->rfd = -1;
	job->pfd = -1;
	job->pty = -1;
	rc = pipe(req);
	fyai_error_check(ctx, !rc, err,
			 "could not create tool request pipe: %s",
			 strerror(errno));
	rc = pipe(rsp);
	fyai_error_check(ctx, !rc, err,
			 "could not create tool response pipe: %s",
			 strerror(errno));
	rc = fcntl(rsp[0], F_SETFL, O_NONBLOCK);
	fyai_error_check(ctx, !rc, err,
			 "could not make the tool channel non-blocking: %s",
			 strerror(errno));
	if (pty) {
		rc = openpty(&master, &slave, NULL, NULL, NULL);
		fyai_error_check(ctx, !rc, err,
				 "could not open a terminal for the tool: %s",
				 strerror(errno));
		fyai_agent_tty_size(ctx, &rows, &cols);
		ws.ws_row = (unsigned short)rows;
		ws.ws_col = (unsigned short)cols;
		(void)ioctl(slave, TIOCSWINSZ, &ws);
	}
	pid = fork();
	fyai_error_check(ctx, pid >= 0, err,
			 "could not fork tool process: %s", strerror(errno));

	if (!pid) {			/* child */
		close(req[1]);
		close(rsp[0]);
		if (setsid() < 0)
			(void)setpgid(0, 0);	/* already a leader: still isolate */
		fyai_ctx_loop_abandon(ctx);
		if (master >= 0)
			close(master);
		/* Install the PTY before arranging standard and control descriptors. */
		if (slave >= 0 && fyai_tool_child_tty(slave))
			_exit(126);
		/* Record whether this child presents through its own terminal. */
		ctx->cfg->agent_pty = slave >= 0;
		if (fyai_tool_child_fds(req[0], rsp[1]))
			_exit(126);

		ctx->ui = NULL;
		ctx->shell_stream = NULL;
		ctx->cfg->tool_child = true;
		fyai_diag_trace_reopen();
		fyai_tool_child_signals(ctx);
		if (fyai_setup_transient_builder(ctx))
			_exit(1);
		if (fyai_tool_apply_sandbox(ctx))
			_exit(126);
		fyai_tool_child_serve_loop(ctx);	/* never returns */
		_exit(1);
	}

	close(req[0]);
	req[0] = -1;
	close(rsp[1]);
	rsp[1] = -1;
	if (slave >= 0) {
		close(slave);
		slave = -1;
	}
	job->pid = pid;
	job->rfd = rsp[0];
	job->pfd = req[1];
	job->pty = master;
	job->pty_rows = rows;
	job->pty_cols = cols;
	rsp[0] = -1;
	req[1] = -1;
	master = -1;
	return 0;

err:
	if (req[0] >= 0)
		close(req[0]);
	if (req[1] >= 0)
		close(req[1]);
	if (rsp[0] >= 0)
		close(rsp[0]);
	if (rsp[1] >= 0)
		close(rsp[1]);
	if (master >= 0)
		close(master);
	if (slave >= 0)
		close(slave);
	return -1;
}


static void fyai_tool_job_live_close(struct fyai_tool_job *job)
{
	/* A sub-agent's screen is committed with its outcome, not discarded. */
	if (job->agent)
		fyai_agent_view_close(job, job->result_ok && !job->failed,
				      NULL);
	if (job->stream.active)
		fyai_fenced_stream_finish(&job->stream);
	fyai_sink_band_destroy(job->band);
	job->band = NULL;
	free(job->title);
	job->title = NULL;
	free(job->command);
	job->command = NULL;
}

static int fyai_tool_job_attach(struct fyai_ctx *ctx,
				struct fyai_tool_job *job);

static void fyai_tool_job_close_channel(struct fyai_tool_job *job)
{
	if (job->run) {
		jsonrpc_request_destroy(job->run);
		job->run = NULL;
	}
	if (job->conn) {
		jsonrpc_conn_destroy(job->conn);
		job->conn = NULL;
	}
	if (job->rfd >= 0)
		close(job->rfd);
	job->rfd = -1;
	if (job->pfd >= 0)
		close(job->pfd);
	job->pfd = -1;
}

static void fyai_tool_job_discard(struct fyai_tool_job *job)
{
	if (!job)
		return;
	fyai_tool_job_cancel(job);
	fyai_tool_job_close_channel(job);
	fyai_tool_job_drop(&job->deadline);
	fyai_tool_job_drop(&job->csrc);
	if (!job->reaped && job->pid > 0)
		while (waitpid(job->pid, NULL, 0) < 0 && errno == EINTR)
			;
	fyai_tool_job_live_close(job);
	fyai_tool_job_unlink(job->ctx, job);
	free(job->progress.data);
	free(job->branch);
	free(job->origin);
	free(job);
}

static enum fyai_event_action
fyai_tool_job_deadline(const struct fyai_event *ev);
static unsigned int fyai_tool_job_timeout_ms(struct fyai_ctx *ctx,
					     const char *name, bool native_call,
					     fy_generic args);

static const char *fyai_tool_submit_error(struct fyai_ctx *ctx)
{
	if (!ctx->tool_submit_error)
		return "tool error: could not start the tool";
	return ctx->tool_submit_error;
}

static void fyai_tool_submit_error_set(struct fyai_ctx *ctx,
				       const char *fmt, ...)
{
	va_list ap;
	char *msg = NULL;

	va_start(ap, fmt);
	if (vasprintf(&msg, fmt, ap) < 0)
		msg = NULL;
	va_end(ap);
	free(ctx->tool_submit_error);
	ctx->tool_submit_error = msg;
}

struct fyai_tool_job *fyai_tool_job_submit(struct fyai_ctx *ctx,
					    fy_generic tool_call)
{
	struct fyai_tool_job *job = NULL;
	struct response_buffer view = {0};
	const char *name, *args_text, *command;
	const char *asked, *cmdtext;
	fy_generic args, progress_args, agent_name;
	bool agent_stored = false;
	fy_generic session_call, session_command;
	struct fyai_event_loop *el;
	char child_branch[FYAI_BRANCH_NAME_MAX + 1];
	char *session_name = NULL;
	char *session_title = NULL;
	char *label = NULL;
	bool have_session = false;
	bool have_branch = false;
	bool native_call;
	bool eligible;
	int srows = 0, scols = 0;
	int rc;

	free(ctx->tool_submit_error);
	ctx->tool_submit_error = NULL;
	eligible = fyai_tool_call_parallel_eligible(ctx, tool_call);
	fyai_error_check(ctx, eligible, err,
		"tool call is not eligible for asynchronous submission");
	name = fyai_tool_call_name(ctx, tool_call);
	native_call = fy_equal(fy_get(tool_call, "type"), "shell_call");
	if (native_call) {
		args_text = NULL;
	} else if (ctx->cfg->api_mode == FYAI_API_CHAT_COMPLETIONS) {
		args_text = fy_get(fy_get(tool_call, "function"),
				   "arguments", "");
	} else if (ctx->cfg->api_mode == FYAI_API_RESPONSES ||
		   ctx->cfg->api_mode == FYAI_API_MESSAGES) {
		args_text = fy_get(tool_call, "arguments", "");
	} else {
		fyai_error_check(ctx, false, err,
				 "unsupported API mode for tool submission");
	}
	args = native_call ? tool_call :
		parse_json_string(ctx->transient_gb, args_text);
	fyai_error_check(ctx, fy_is_valid(args), err,
			 "invalid tool call arguments");
	if (fyai_agent_delegated(ctx)) {
		char *header;

		progress_args = native_call ?
			fy_mapping("command",
				fy_cast(fy_get_at_path(tool_call, "action",
						      "commands", 0), "")) :
			args;
		header = fyai_format_tool_header(ctx, name, progress_args,
					fyai_tool_preview_lines(ctx->cfg, name));
		if (header) {
			fyai_tool_progress_emit(ctx, header, strlen(header));
			fyai_tool_progress_flush(ctx);
			free(header);
		}
	}
	/* Reserve the sub-agent branch before the job is spawned. */
	if (fy_equal(name, "agent")) {
		rc = fyai_branches_refresh(ctx);
		fyai_error_check(ctx, !rc, err,
				 "could not refresh the branch table");
		agent_name = fy_get(args, "name", fy_invalid);
		/* Reuse stored agents, but reject a name owned by a live job. */
		rc = fyai_branch_alloc_child(ctx, fyai_ctx_branch(ctx),
				fy_castp(&agent_name, "agent"),
				(unsigned int)ctx->cfg->agent_max_branch_depth,
				child_branch, sizeof(child_branch),
				&agent_stored);
		if (!rc && fyai_tool_job_branch_live(ctx, child_branch)) {
			fyai_tool_submit_error_set(ctx,
				"tool error: the sub-agent named '%s' is still "
				"running; wait for its report or choose a "
				"different name",
				fy_castp(&agent_name, "agent"));
			rc = -1;
		}
		fyai_error_check(ctx, !rc, err,
				 "could not name the sub-agent branch");
		have_branch = true;
	}

	/* A named session must explicitly request a terminal. */
	if (fyai_shell_named_call(ctx, tool_call) &&
	    !fyai_shell_tty_requested(ctx, args)) {
		session_call = fy_get(args, "name", fy_invalid);
		fyai_tool_submit_error_set(ctx,
			"tool error: a shell that stays open needs a terminal; "
			"add \"tty\": true to keep '%s' open, or drop \"name\" "
			"to run the command to completion and read what it "
			"wrote", fy_castp(&session_call, ""));
		fyai_error_check(ctx, false, err,
				 "a session was asked for with no terminal");
	}

	/* Reserve and validate the session name before spawning its job. */
	if (fyai_shell_session_call(ctx, tool_call)) {
		session_call = fy_get(args, "name", fy_invalid);
		asked = fy_castp(&session_call, "");
		if (!fyai_shell_session_name_valid(asked))
			fyai_tool_submit_error_set(ctx,
				"tool error: '%s' is not a usable session name; "
				"use letters, digits, '-' or '_'", asked);
		else if (fyai_shell_session_find(ctx, asked))
			fyai_tool_submit_error_set(ctx,
				"tool error: a shell named '%s' is already open "
				"on branch %s; write to it or choose another name",
				asked, fyai_ctx_branch(ctx));
		else
			/* Own the name: the generic it came from is transient. */
			session_name = strdup(asked);
		fyai_error_check(ctx, session_name && !ctx->tool_submit_error,
				 err, "could not name the terminal session");
		have_session = true;
	}

	job = calloc(1, sizeof(*job));
	fyai_error_check(ctx, job, err,
			 "could not allocate tool job");
	/* A sub-agent renders to a terminal of its own; the parent shows it. */
	rc = fyai_tool_job_spawn(ctx, job, fy_equal(name, "agent") &&
				 fyai_ui_active(ctx));
	fyai_error_check(ctx, !rc, err,
		"could not spawn tool job");
	fyai_tool_job_link(ctx, job);
	if (have_session) {
		fyai_shell_tty_size(ctx, args, &srows, &scols);
		session_command = fy_get(args, "command", fy_invalid);
		session_title = fyai_format_shell_label(args);
		job->session = fyai_shell_session_create(ctx, session_name,
					fy_castp(&session_command, ""),
					session_title, srows, scols,
					fyai_shell_output_bytes(ctx, args),
					!fyai_shell_tty_requested(ctx, args));
		free(session_title);
		session_title = NULL;
		fyai_error_check(ctx, job->session, err,
				 "could not open the terminal session");
		job->session->job = job;
		/* The session copied the name it was reserved under. */
		free(session_name);
		session_name = NULL;
	}
	/* A child that asked for a terminal must follow the window of the
	 * user, and only the parent can see it change. */
	if (fyai_shell_tty_requested(ctx, native_call ?
				     fy_get(tool_call, "action") : args))
		(void)fyai_terminal_winch_open(ctx);
	/* The spawn clears the job, so the name is carried in a local. */
	if (have_branch) {
		job->branch = strdup(child_branch);
		fyai_error_check(ctx, job->branch, err,
				 "out of memory naming the sub-agent branch");
	}
	job->call = tool_call;
	job->native_shell = native_call;
	/* A zeroed generic decodes as an empty sequence, not as invalid. */
	job->diag = fy_invalid;
	/* Save the identity that the parent adds to child diagnostics. */
	if (job->branch) {
		job->origin = strdup(job->branch);
		rc = job->origin ? 0 : -1;
	} else {
		rc = asprintf(&job->origin, "%s %s", fyai_ctx_branch(ctx), name);
	}
	fyai_error_check(ctx, rc >= 0 && job->origin, err,
			 "out of memory naming the tool job");
	/* The trace pairs with the reap record below: a child that dies leaves
	 * these two lines and nothing else. */
	fyai_diag_tracef("spawn", "%s, pid %ld", job->origin, (long)job->pid);
	/* Stream slow shell work on the sub-agent terminal. */
	if (fy_equal(name, "shell") && !fyai_ui_active(ctx) &&
	    ctx->cfg->agent_pty) {
		/* Native shell commands live in the action object. */
		cmdtext = native_call ?
			fy_cast(fy_get_at_path(tool_call, "action", "commands",
					       0), "") :
			fy_get(args, "command", "");
		if (!fyai_shell_view(ctx, *cmdtext ? cmdtext : name, args,
				     &label, &view))
			fyai_print_tool_view(ctx, label, &view);
		free(label);
		free(view.data);
		if (!fyai_fenced_stream_start(&job->stream, ctx, ctx->cfg,
				NULL, ctx->cfg->tool_preview_lines > 0 ?
				(size_t)ctx->cfg->tool_preview_lines : 0,
				FYAI_TOOL_OUTPUT_INDENT, stderr, true)) {
			/*
			 * The surface that the parent shows already carries the state of this call.
			 * A second mark here has no animation.
			 */
			fyai_fenced_stream_clear_indicator(&job->stream);
			job->band_progress = true;
		}
	}
	if (fy_any_equal(name, "shell", "agent") &&
	    fyai_ui_active(ctx)) {
		if (fy_equal(name, "agent")) {
			job->agent = true;
			command = fy_get(args, "description", "");
			job->title = fyai_format_tool_header(ctx, "agent",
				fy_mapping("name", fy_get(args, "name", ""),
					   "description",
					   *command ? command : name), 0);
		} else {
			command = native_call ?
				fy_cast(fy_get_at_path(tool_call, "action",
						       "commands", 0), "") :
				fy_get(args, "command", "");
			job->title = fyai_format_shell_label(
					native_call ? fy_invalid : args);
			job->command = strdup(*command ? command : name);
			fyai_error_check(ctx, job->command, err,
					 "out of memory formatting shell progress");
			job->band_progress = true;
		}
		/* Agents use their surface; shells continue to use a work band. */
		if (job->agent) {
			if (fyai_agent_view_open(ctx, job))
				fyai_error(ctx,
					   "agent: could not show the sub-agent terminal");
			goto live_open_done;
		}
		job->band = fyai_sink_band_open(ctx->sink, false, NULL, NULL);
		if (job->band &&
		    !fyai_fenced_stream_start(&job->stream, ctx, ctx->cfg,
				NULL, ctx->cfg->tool_preview_lines > 0 ?
				(size_t)ctx->cfg->tool_preview_lines : 0,
				FYAI_TOOL_OUTPUT_INDENT, stderr, true)) {
			job->stream.band = job->band;
			job->stream.title = job->title;
			job->stream.command = job->command;
			fyai_sink_band_paint(job->band, job->title,
					     job->command, NULL, 0, NULL);
		} else {
			fyai_tool_job_live_close(job);
		}
	}
live_open_done:
	rc = fyai_tool_job_attach(ctx, job);
	fyai_error_check(ctx, !rc, err,
			 "could not attach tool job to event loop");

	job->timeout_ms = fyai_tool_job_timeout_ms(ctx, name, native_call, args);
	if (job->timeout_ms) {
		el = fyai_ctx_loop(ctx);
		assert(el);
		rc = fyai_event_add_timer(el, job->timeout_ms, 0,
					  fyai_tool_job_deadline, job,
					  &job->deadline);
		fyai_error_check(ctx, !rc, err,
				 "could not arm the tool job time limit");
	}
	return job;

err:
	free(session_name);
	fyai_tool_job_discard(job);
	return NULL;
}

/*
 * Keep the end of the live output from a time-limited job. The deadline can
 * stop a job before it reports a result. In this case, progress notifications
 * are the only command output. The end usually contains the failure cause.
 */
#define FYAI_TOOL_PROGRESS_TAIL	8192

static void fyai_tool_job_progress_retain(struct fyai_tool_job *job,
					  const char *p, size_t len)
{
	struct response_buffer *buf = &job->progress;
	int rc;

	rc = response_buffer_reserve(buf, buf->len + len + 1);
	if (rc)
		return;
	memcpy(buf->data + buf->len, p, len);
	buf->len += len;
	buf->data[buf->len] = '\0';
	if (buf->len <= FYAI_TOOL_PROGRESS_TAIL)
		return;
	memmove(buf->data, buf->data + buf->len - FYAI_TOOL_PROGRESS_TAIL,
		FYAI_TOOL_PROGRESS_TAIL);
	buf->len = FYAI_TOOL_PROGRESS_TAIL;
	buf->data[buf->len] = '\0';
}

/* The child's tool/progress notifications: its live output. */
static fy_generic fyai_tool_job_serve(struct jsonrpc_conn *conn,
				      const char *method, fy_generic params,
				      fy_generic id, void *userdata,
				      fy_generic *errorp)
{
	struct fyai_tool_job *job = userdata;
	fy_generic text;
	const char *p;
	char *bytes;
	size_t len, n;

	(void)errorp;
	/* Handle the question request a delegated child sends to its parent. */
	if (fy_is_valid(id)) {
		if (strcmp(method, "user/ask"))
			return fy_invalid;
		return fyai_ask_user_for_child(job, conn, id, params);
	}

	/* A session sends the bytes of its terminal; the parent renders. */
	if (!strcmp(method, "shell/output")) {
		if (!job->session)
			return fy_invalid;
		bytes = fyai_bytes_from_generic(params, &n);
		if (bytes) {
			/* It wrote: whatever it waited for, it has it. */
			job->session->wants_input = false;
			(void)fyai_terminal_view_feed(job->session->view,
						      bytes, n);
			fyai_shell_session_refresh(job->session);
		}
		free(bytes);
		return fy_invalid;
	}
	/* Watch the child-reported process for input waits. */
	if (!strcmp(method, "shell/started")) {
		if (job->session)
			fyai_shell_session_watch(job->session,
					(pid_t)fy_get(params, "pid", 0LL));
		return fy_invalid;
	}
	if (!strcmp(method, "shell/exit")) {
		if (job->session)
			fyai_shell_session_exited(job->session,
				(int)fy_get(params, "exit_code", 0LL),
				(int)fy_get(params, "signal", 0LL));
		return fy_invalid;
	}
	if (strcmp(method, "tool/progress"))
		return fy_invalid;
	text = fy_get(params, "text", fy_invalid);
	if (!fy_is_string(text))
		return fy_invalid;
	p = fy_castp(&text, "");
	len = strlen(p);
	if (data_is_binary(p, len))
		return fy_invalid;
	if (!job->agent && job->timeout_ms)
		fyai_tool_job_progress_retain(job, p, len);
	if (job->band_progress && job->stream.active)
		(void)fyai_fenced_stream_push(&job->stream, p, len);
	else if (!job->agent && job->ctx->shell_stream &&
		 job->ctx->shell_stream->active)
		/* Stream command output into the sub-agent's live shell region. */
		(void)fyai_fenced_stream_push(job->ctx->shell_stream, p, len);
	return fy_invalid;
}

static void fyai_tool_job_run_done(struct jsonrpc_request *req, void *userdata)
{
	struct fyai_tool_job *job = userdata;

	job->out_open = false;
	if (jsonrpc_request_ok(req)) {
		fy_generic r = jsonrpc_request_result(req);

		job->result = fy_get(r, "result", fy_invalid);
		job->result_ok = fy_get(r, "ok", false);
		job->display = fy_get(r, "display", fy_invalid);
		job->diag = fy_get(r, "diag", fy_invalid);
		if (fy_is_string(job->display))
			fyai_patch_display_record(job->ctx, job->call,
					fy_castp(&job->display, ""));
		job->have_result = true;
	} else {
		job->failed = true;
	}
	fyai_tool_job_update_done(job);
}

static int fyai_tool_job_attach(struct fyai_ctx *ctx,
				struct fyai_tool_job *job)
{
	struct fyai_event_loop *el;
	struct fy_generic_builder *gb;
	int rc;

	fyai_error_check(ctx, job, err,
			 "cannot attach an empty tool job");
	el = fyai_ctx_loop(ctx);
	fyai_error_check(ctx, el, err,
			 "tool job requires an event loop");
	gb = fyai_ctx_transient_gb(ctx);
	fyai_error_check(ctx, gb, err,
			 "tool job requires transient storage");

	job->conn = jsonrpc_conn_stdio(ctx, job->pfd, job->rfd, 0,
				       "tool", NULL);
	fyai_error_check(ctx, job->conn, err,
			 "could not open the tool control channel");
	rc = jsonrpc_conn_serve(job->conn, fyai_tool_job_serve, job);
	fyai_error_check(ctx, !rc, err,
			 "could not serve the tool control channel");

	job->out_open = true;
	job->run = jsonrpc_request_submit(job->conn, "tool/run",
					  job->branch ?
					  fy_gb_mapping(gb, "call", job->call,
							"branch", job->branch) :
					  fy_gb_mapping(gb, "call", job->call),
					  jsonrpc_conn_next_id(job->conn),
					  false, fyai_tool_job_run_done, job);
	fyai_error_check(ctx, job->run, err,
			 "could not dispatch the tool call");

	rc = fyai_event_add_child(el, job->pid, fyai_tool_job_child,
				  job, &job->csrc);
	fyai_error_check(ctx, !rc, err,
			 "could not attach tool process");
	return 0;

err:
	if (job)
		job->failed = true;
	return -1;
}

bool fyai_tool_job_done(const struct fyai_tool_job *job)
{
	return job && job->done;
}

#define FYAI_TOOL_TERM_MS 2000

void fyai_tool_job_cancel(struct fyai_tool_job *job)
{
	struct fyai_event_loop *el;
	int rc;

	if (!job || job->reaped || job->pid <= 0 || job->terminating)
		return;
	job->terminating = true;
	jsonrpc_conn_expect_close(job->conn);

	el = fyai_ctx_loop(job->ctx);
	if (el) {
		fyai_tool_job_drop(&job->csrc);
		rc = fyai_event_add_child_terminate_group(el, job->pid, 0,
							  FYAI_TOOL_TERM_MS,
							  fyai_tool_job_child,
							  job, &job->csrc);
		if (!rc)
			return;
		/* Watch the child without staged termination. */
		(void)fyai_event_add_child(el, job->pid, fyai_tool_job_child,
					   job, &job->csrc);
	}
	rc = kill(-job->pid, SIGTERM);
	if (rc && errno == ESRCH)
		(void)kill(job->pid, SIGTERM);
}

/* Stop the process group when the job limit expires. */
static enum fyai_event_action
fyai_tool_job_deadline(const struct fyai_event *ev)
{
	struct fyai_tool_job *job = ev->userdata;

	job->deadline = NULL;
	job->timed_out = true;
	fyai_tool_job_cancel(job);
	return FYAIEA_CONTINUE;
}

/* Return the time limit for a shell or agent job. */
static unsigned int fyai_tool_job_timeout_ms(struct fyai_ctx *ctx,
					     const char *name, bool native_call,
					     fy_generic args)
{
	fy_generic persona_name, personas, persona;
	long long ms;

	if (native_call || fy_equal(name, "shell"))
		return fyai_shell_timeout_ms(ctx, args, native_call);
	if (fy_equal(name, "agent")) {
		/*
		 * Use the first available limit in this order: the call, the
		 * persona, and the global sub-agent setting. Only the call
		 * contains an untrusted value. Apply the maximum only to this
		 * value.
		 */
		ms = fy_get(args, "timeout", 0LL);
		if (ms > 0 && ctx->cfg->agent_max_timeout_ms > 0 &&
		    ms > ctx->cfg->agent_max_timeout_ms)
			ms = ctx->cfg->agent_max_timeout_ms;
		persona_name = fy_get(args, "persona", fy_invalid);
		personas = fy_get(fy_get(ctx->cfg->config_doc, "agent"),
				  "personas", fy_invalid);
		persona = fy_get(personas, persona_name, fy_invalid);
		if (ms <= 0)
			ms = fy_get(persona, "timeout_ms", 0LL);
		if (ms <= 0)
			ms = ctx->cfg->agent_timeout_ms;
		return ms > 0 ? (unsigned int)ms : 0;
	}
	return 0;
}

/*
 * Return the length of @s without a final "tool error: interrupted" report.
 *
 * A job sees the group termination from a parent deadline as an interrupt.
 * The parent knows that a timeout occurred and removes this incorrect report.
 * Remove only a final report. The same text at an earlier position is command
 * output.
 */
static size_t fyai_tool_drop_interrupt(const char *s)
{
	static const char intr[] = "tool error: interrupted";
	const char *p, *last;
	size_t len;

	len = strlen(s);
	last = NULL;
	for (p = strstr(s, intr); p; p = strstr(p + 1, intr))
		last = p;
	if (!last)
		return len;
	for (p = last + sizeof(intr) - 1; *p; p++)
		if (!isspace((unsigned char)*p))
			return len;
	len = (size_t)(last - s);
	while (len && isspace((unsigned char)s[len - 1]))
		len--;
	return len;
}

/*
 * Report an expired native-shell limit in stderr for the next model request.
 */
#define FYAI_SHELL_TIMEOUT_NOTE \
	"tool error: command timed out after %u ms; request a larger " \
	"timeout_ms (up to %u ms) if the command needs longer"
#define FYAI_SHELL_TIMEOUT_NOTE_NOMAX \
	"tool error: command timed out after %u ms; request a larger " \
	"timeout_ms if the command needs longer"

/*
 * Change the outcome of a native shell result after a parent deadline.
 * The child sees group termination only as a signal and cannot identify the
 * timeout. The parent identifies the timeout and keeps all captured output.
 */
static fy_generic fyai_shell_result_retime(struct fyai_ctx *ctx,
					   fy_generic result,
					   unsigned int timeout_ms,
					   const char *note)
{
	fy_generic outputs = fy_seq_empty;
	fy_generic entry, outcome;
	fy_generic gerr;
	const char *err;

	if (!fy_is_sequence(result))
		return result;
	fy_foreach(entry, result) {
		outcome = fy_get(entry, "outcome");
		if (fy_equal(fy_get(outcome, "type"), "signal")) {
			gerr = fy_get(entry, "stderr");
			err = fy_castp(&gerr, "");
			entry = fy_mapping(
				"stdout", fy_get(entry, "stdout", ""),
				"stderr", fy_stringf(ctx->transient_gb,
						     "%s%s%s", err,
						     *err ? "\n" : "", note),
				"outcome", fy_mapping(
					"type", "timeout",
					"timeout_ms",
					(long long)timeout_ms));
		}
		outputs = fy_append(outputs, entry);
	}
	return fy_gb_internalize(ctx->transient_gb, outputs);
}

fy_generic fyai_tool_job_collect(struct fyai_ctx *ctx,
				 struct fyai_tool_job *job, bool *okp)
{
	const char *reason;
	const char *note;
	const char *captured;
	fy_generic result;
	fy_generic out;
	char *cause;
	size_t keep;
	bool genuine;
	int status;
	pid_t rc;

	if (!job)
		return fy_invalid;
	if (!job->done) {
		*okp = false;
		return fy_invalid;
	}
	/* A started session keeps its process after this call completes. */
	if (job->session) {
		fyai_tool_job_live_close(job);
		fyai_tool_job_drop(&job->deadline);
		job->group = NULL;
		if (fy_is_valid(job->diag))
			fyai_diag_adopt(fyai_ctx_diag(ctx), job->diag,
					job->origin);
		*okp = job->result_ok && !job->failed;
		if (!*okp) {
			/* Quote the cause without consuming the diagnostic. */
			cause = fyai_diag_string(fyai_ctx_diag(ctx));
			out = fy_stringf(ctx->transient_gb,
				"tool error: the terminal session could not be "
				"opened: %s", cause && *cause ? cause :
				"no reason was recorded");

			free(cause);
			return out;
		}
		return fy_stringf(ctx->transient_gb,
			"[terminal session '%s' started: %s]\n"
			"Read it with shell_output, drive it with shell_input, "
			"end it with shell_close.", job->session->name,
			job->session->command);
	}
	fyai_tool_job_live_close(job);
	fyai_tool_job_close_channel(job);
	fyai_tool_job_drop(&job->deadline);
	fyai_tool_job_drop(&job->csrc);
	if (!job->reaped && job->pid > 0) {
		do {
			rc = waitpid(job->pid, &status, 0);
		} while (rc < 0 && errno == EINTR);
		if (rc == job->pid) {
			job->reaped = true;
			/* The answer stands; see fyai_tool_job_child(). */
			if (!job->have_result)
				job->result_ok = WIFEXITED(status) &&
						 WEXITSTATUS(status) == 0;
			job->term_signal = WIFSIGNALED(status) ?
						WTERMSIG(status) : 0;
		}
	}
	/* Adopt diagnostics before reporting a missing child result. */
	fyai_diag_tracef("reap", "%s, pid %ld: %s%d%s",
			 job->origin ? job->origin : "tool", (long)job->pid,
			 job->term_signal ? "signal " : "exit ",
			 job->term_signal ? job->term_signal :
					    (job->result_ok ? 0 : 1),
			 job->timed_out ? ", timed out" :
			 (job->have_result ? "" : ", no result"));
	if (fy_is_valid(job->diag))
		fyai_diag_adopt(fyai_ctx_diag(ctx), job->diag, job->origin);
	/* Supply a cause when a failed child returned no diagnostic. */
	if (!job->timed_out && job->term_signal) {
		/* A job we stopped ended as we asked it to; that is not a
		 * failure to report, only detail for whoever is debugging. */
		fyai_diag_type(fyai_ctx_diag(ctx),
			       job->terminating ? FYAIET_DEBUG : FYAIET_ERROR,
			       "[%s] terminated by signal %d",
			       job->origin ? job->origin : "tool",
			       job->term_signal);
	} else if (!job->timed_out && !job->have_result) {
		fyai_error(ctx, "[%s] ended without a result%s",
			   job->origin ? job->origin : "tool",
			   job->failed ? " (the control channel failed)" : "");
	}
	if (job->term_signal || !job->have_result)
		result = fy_invalid;
	else
		result = job->result;
	if (fy_is_invalid(result) && job->term_signal) {
		if (job->native_shell)
			result = fy_gb_internalize(ctx->transient_gb,
				fy_sequence(fy_mapping(
					"stdout", "",
					"stderr", "",
					"outcome", fy_mapping(
						"type", "signal",
						"signal", job->term_signal))));
		else
			result = fy_value(ctx->transient_gb,
					  "tool error: interrupted");
	}
	/* The parent knows whether its time limit expired. */
	if (job->timed_out) {
		/*
		 * A job that reports before termination has a complete result.
		 * Otherwise, use the retained end of the progress output.
		 */
		genuine = job->have_result && !job->term_signal;
		captured = job->progress.len ? job->progress.data : "";
		reason = fy_sprintfa("tool error: timed out after %u ms",
				     job->timeout_ms);
		if (ctx->cfg->shell_max_timeout_ms > 0)
			note = fy_sprintfa(FYAI_SHELL_TIMEOUT_NOTE,
					      job->timeout_ms,
					      (unsigned int)
					      ctx->cfg->shell_max_timeout_ms);
		else
			note = fy_sprintfa(FYAI_SHELL_TIMEOUT_NOTE_NOMAX,
					      job->timeout_ms);
		if (!job->native_shell) {
			assert(fy_is_string(result));
			if (genuine)
				captured = fy_castp(&result, "");
			keep = fyai_tool_drop_interrupt(captured);
			result = fy_gb_internalize(ctx->transient_gb,
				fy_stringf("%.*s%s%s", (int)keep, captured,
					   keep ? "\n" : "", reason));
		} else if (genuine) {
			result = fyai_shell_result_retime(ctx, result,
							  job->timeout_ms,
							  note);
		} else {
			/*
			 * Keep the native result shape after a timeout. The model
			 * always receives a sequence of {stdout, stderr, outcome}.
			 */
			result = fy_gb_internalize(ctx->transient_gb,
				fy_sequence(fy_mapping(
					"stdout", captured,
					"stderr", note,
					"outcome", fy_mapping(
						"type", "timeout",
						"timeout_ms",
						(long long)job->timeout_ms))));
		}
		job->result_ok = false;
	}
	*okp = job->result_ok && !job->failed;
	fyai_tool_job_unlink(job->ctx, job);
	free(job->progress.data);
	free(job->branch);
	free(job->origin);
	free(job);
	return result;
}

enum fyai_tool_group_state {
	FYAITGS_QUEUED,
	FYAITGS_RUNNING,
	FYAITGS_PARKED,
	FYAITGS_COLLECTED,
	FYAITGS_SUBMIT_FAILED,
};

struct fyai_tool_group_entry {
	char *call_text;
	char *result_text;
	struct fyai_tool_job *job;
	struct fyai_mcp_call_request *mcp_request;
	enum fyai_tool_group_state state;
	bool parallel;
	bool result_ok;
};

static void
fyai_tool_job_group_mcp_complete(struct fyai_mcp_call_request *request,
				 void *userdata)
{
	struct fyai_tool_job_group *group;

	if (!fyai_mcp_call_done(request))
		return;
	group = userdata;
	fyai_tool_job_group_service(group);
}

struct fyai_tool_job_group {
	struct fyai_ctx *ctx;
	struct fyai_tool_group_entry *entries;
	struct fyai_event_source *animation_timer;
	size_t count;
	size_t capacity;
	size_t next;
	size_t active;
	size_t parked;
	size_t max_parallel;
	bool submitted;
	bool sealed;
	bool cancelled;
	bool exclusive;
	bool notified;
	fyai_tool_group_complete_fn complete;
	void *userdata;
};

static unsigned int
fyai_tool_job_group_animation_interval(struct fyai_tool_job_group *group)
{
	struct fyai_tool_group_entry *entry;
	unsigned int interval;
	size_t i;

	interval = 0;
	for (i = 0; i < group->next; i++) {
		entry = &group->entries[i];
		if (entry->state != FYAITGS_RUNNING || !entry->job ||
		    !entry->job->stream.active ||
		    entry->job->stream.indicator_state !=
			    FYMD_INDICATOR_PENDING)
			continue;
		if (!entry->job->stream.indicator_interval_ms)
			continue;
		if (!interval ||
		    entry->job->stream.indicator_interval_ms < interval)
			interval = entry->job->stream.indicator_interval_ms;
	}
	return interval;
}

static enum fyai_event_action
fyai_tool_job_group_animation_cb(const struct fyai_event *ev)
{
	struct fyai_tool_job_group *group;

	group = ev->userdata;
	fyai_tool_job_group_service(group);
	return FYAIEA_CONTINUE;
}

static void
fyai_tool_job_group_animation_sync(struct fyai_tool_job_group *group)
{
	unsigned int interval;
	int rc;

	interval = fyai_tool_job_group_animation_interval(group);
	if (!interval) {
		fyai_event_source_remove(group->animation_timer);
		group->animation_timer = NULL;
		return;
	}
	if (group->animation_timer)
		return;
	rc = fyai_event_add_timer(fyai_ctx_loop(group->ctx), interval,
				  interval,
				  fyai_tool_job_group_animation_cb, group,
				  &group->animation_timer);
	if (rc)
		fyai_warning(group->ctx,
			     "tool indicator animation timer could not start");
}

static int fyai_tool_job_group_reserve(struct fyai_tool_job_group *group)
{
	struct fyai_tool_group_entry *entries;
	size_t capacity;

	if (group->count < group->capacity)
		return 0;
	capacity = group->capacity ? group->capacity * 2 : 8;
	entries = realloc(group->entries, capacity * sizeof(*entries));
	if (!entries)
		return -1;
	memset(entries + group->capacity, 0,
	       (capacity - group->capacity) * sizeof(*entries));
	group->entries = entries;
	group->capacity = capacity;
	return 0;
}

struct fyai_tool_job_group *fyai_tool_job_group_create(struct fyai_ctx *ctx)
{
	return fyai_tool_job_group_create_notify(ctx, NULL, NULL);
}

struct fyai_tool_job_group *
fyai_tool_job_group_create_notify(struct fyai_ctx *ctx,
				  fyai_tool_group_complete_fn complete,
				  void *userdata)
{
	struct fyai_tool_job_group *group;

	if (!ctx)
		return NULL;
	group = calloc(1, sizeof(*group));
	if (!group)
		return NULL;
	group->ctx = ctx;
	group->complete = complete;
	group->userdata = userdata;
	group->max_parallel = ctx->cfg->parallel_tool_calls ? 16 : 1;
	return group;
}

struct fyai_tool_job_group *
fyai_tool_job_group_create_open(struct fyai_ctx *ctx,
				fyai_tool_group_complete_fn complete,
				void *userdata)
{
	struct fyai_tool_job_group *group;

	group = fyai_tool_job_group_create_notify(ctx, complete, userdata);
	if (!group)
		return NULL;
	group->submitted = true;
	return group;
}

int fyai_tool_job_group_add(struct fyai_tool_job_group *group,
			    fy_generic tool_call)
{
	const char *text;
	char *copy;
	bool parallel;
	int rc;
	size_t i;

	if (!group || group->sealed)
		return -1;
	rc = fyai_tool_job_group_reserve(group);
	if (rc)
		return -1;
	parallel = fyai_tool_call_parallel_eligible(group->ctx, tool_call);
	if (group->submitted && !parallel)
		return 1;
	if (group->count && (group->exclusive || !parallel))
		return -1;
	text = emit_json_string(group->ctx->transient_gb, tool_call);
	copy = text ? strdup(text) : NULL;
	if (!copy)
		return -1;
	for (i = 0; i < group->count; i++) {
		if (!strcmp(group->entries[i].call_text, copy)) {
			free(copy);
			return 0;
		}
	}
	group->entries[group->count].call_text = copy;
	group->entries[group->count].parallel = parallel;
	group->entries[group->count].state = FYAITGS_QUEUED;
	if (!parallel) {
		group->exclusive = true;
		group->max_parallel = 1;
	}
	group->count++;
	if (group->submitted)
		fyai_tool_job_group_service(group);
	return 0;
}

static void fyai_tool_job_group_dispatch(struct fyai_tool_job_group *group)
{
	struct fyai_tool_group_entry *entry;
	fy_generic call;
	fy_generic args;
	fy_generic result;
	const char *name;
	const char *text;

	while (!group->cancelled && group->next < group->count &&
	       group->active < group->max_parallel) {
		entry = &group->entries[group->next++];
		call = parse_json_string(group->ctx->transient_gb,
					 entry->call_text);
		name = fy_is_valid(call) ?
			fyai_tool_call_name(group->ctx, call) : NULL;
		if (name && fyai_mcp_tool_name(name)) {
			args = fyai_tool_call_args(group->ctx, call);
			entry->mcp_request =
				fy_is_valid(args) ?
				fyai_mcp_call_submit(group->ctx, name, args,
					fyai_tool_job_group_mcp_complete,
					group) : NULL;
			if (entry->mcp_request) {
				entry->state = FYAITGS_RUNNING;
				group->active++;
				continue;
			}
			result = fy_value(group->ctx->transient_gb,
					  "tool error: MCP call failed");
			text = emit_json_string(group->ctx->transient_gb,
						result);
			entry->result_text = text ? strdup(text) : NULL;
			entry->state = entry->result_text ?
				FYAITGS_PARKED : FYAITGS_SUBMIT_FAILED;
			group->parked++;
			continue;
		}
		if (!entry->parallel) {
			result = fyai_execute_tool_call(group->ctx, call,
							&entry->result_ok);
			text = fy_is_valid(result) ?
				emit_json_string(group->ctx->transient_gb,
						 result) : NULL;
			entry->result_text = text ? strdup(text) : NULL;
			entry->state = entry->result_text ?
				FYAITGS_PARKED : FYAITGS_SUBMIT_FAILED;
			group->parked++;
			continue;
		}
		entry->job = fy_is_valid(call) ?
			fyai_tool_job_submit(group->ctx, call) : NULL;
		if (!entry->job) {
			/* Return a submission error as the tool result. */
			result = fy_value(group->ctx->transient_gb,
					  fyai_tool_submit_error(group->ctx));
			text = emit_json_string(group->ctx->transient_gb,
						result);
			entry->result_text = text ? strdup(text) : NULL;
			entry->result_ok = false;
			entry->state = entry->result_text ?
				FYAITGS_PARKED : FYAITGS_SUBMIT_FAILED;
			group->parked++;
			continue;
		}
		entry->state = FYAITGS_RUNNING;
		entry->job->group = group;
		group->active++;
	}
}

static void fyai_tool_job_group_notify(struct fyai_tool_job_group *group)
{
	fyai_tool_group_complete_fn complete;
	void *userdata;

	if (!group->sealed || group->parked != group->count ||
	    group->notified)
		return;
	group->notified = true;
	complete = group->complete;
	userdata = group->userdata;
	if (complete)
		complete(group, userdata);
}

void fyai_tool_job_group_service(struct fyai_tool_job_group *group)
{
	struct fyai_tool_group_entry *entry;
	fyai_event_ms_t now;
	size_t i;

	if (!group || !group->submitted)
		return;
	now = fyai_event_now_ms();
	for (i = 0; i < group->next; i++) {
		entry = &group->entries[i];
		if (entry->state != FYAITGS_RUNNING)
			continue;
		if (entry->mcp_request) {
			if (!fyai_mcp_call_done(entry->mcp_request))
				continue;
			entry->state = FYAITGS_PARKED;
			group->active--;
			group->parked++;
			continue;
		}
		(void)fyai_fenced_stream_animate(&entry->job->stream, now);
		if (!fyai_tool_job_done(entry->job))
			continue;
		entry->job->group = NULL;
		entry->state = FYAITGS_PARKED;
		group->active--;
		group->parked++;
	}
	fyai_tool_job_group_dispatch(group);
	fyai_tool_job_group_animation_sync(group);
	fyai_tool_job_group_notify(group);
}

int fyai_tool_job_group_submit(struct fyai_tool_job_group *group)
{
	if (!group || group->submitted || !group->count)
		return -1;
	group->submitted = true;
	group->sealed = true;
	fyai_tool_job_group_dispatch(group);
	fyai_tool_job_group_animation_sync(group);
	fyai_tool_job_group_notify(group);
	return 0;
}

int fyai_tool_job_group_seal(struct fyai_tool_job_group *group)
{
	if (!group || !group->submitted || group->sealed)
		return -1;
	group->sealed = true;
	fyai_tool_job_group_service(group);
	return 0;
}

bool fyai_tool_job_group_done(const struct fyai_tool_job_group *group)
{
	return group && group->submitted && group->sealed &&
	       group->parked == group->count;
}

size_t fyai_tool_job_group_count(const struct fyai_tool_job_group *group)
{
	return group ? group->count : 0;
}

void fyai_tool_job_group_cancel(struct fyai_tool_job_group *group)
{
	struct fyai_tool_group_entry *entry;
	size_t i;

	if (!group || group->cancelled)
		return;
	group->cancelled = true;
	group->sealed = true;
	for (i = 0; i < group->count; i++) {
		entry = &group->entries[i];
		if (entry->state == FYAITGS_RUNNING) {
			if (entry->mcp_request)
				fyai_mcp_call_cancel(entry->mcp_request);
			else
				fyai_tool_job_cancel(entry->job);
		}
		else if (entry->state == FYAITGS_QUEUED) {
			entry->state = FYAITGS_PARKED;
			group->parked++;
		}
	}
	group->next = group->count;
	fyai_tool_job_group_notify(group);
}

static fy_generic
fyai_tool_job_group_cancelled_result(struct fyai_tool_job_group *group,
				     struct fyai_tool_group_entry *entry)
{
	fy_generic call;

	call = parse_json_string(group->ctx->transient_gb, entry->call_text);
	if (fy_equal(fy_get(call, "type"), "shell_call"))
		return fy_gb_internalize(group->ctx->transient_gb,
			fy_sequence(fy_mapping(
				"stdout", "",
				"stderr", "",
				"outcome", fy_mapping(
					"type", "signal",
					"signal", SIGTERM))));
	return fy_value(group->ctx->transient_gb, "tool error: interrupted");
}

int fyai_tool_job_group_collect(struct fyai_tool_job_group *group,
				size_t index, fy_generic *result, bool *okp)
{
	struct fyai_tool_group_entry *entry;

	if (!group || !result || !okp || index >= group->count)
		return -1;
	entry = &group->entries[index];
	if (entry->state != FYAITGS_PARKED &&
	    entry->state != FYAITGS_SUBMIT_FAILED)
		return -1;
	if (entry->job) {
		*result = fyai_tool_job_collect(group->ctx, entry->job, okp);
		entry->job = NULL;
	} else if (entry->mcp_request) {
		*result = fyai_mcp_call_collect(entry->mcp_request, okp);
		fyai_mcp_call_destroy(entry->mcp_request);
		entry->mcp_request = NULL;
	} else if (entry->result_text) {
		*result = parse_json_string(group->ctx->transient_gb,
					    entry->result_text);
		*okp = entry->result_ok;
	} else if (group->cancelled) {
		*result = fyai_tool_job_group_cancelled_result(group, entry);
		*okp = false;
	} else {
		return -1;
	}
	entry->state = FYAITGS_COLLECTED;
	return 0;
}

void fyai_tool_job_group_destroy(struct fyai_tool_job_group *group)
{
	struct fyai_tool_group_entry *entry;
	size_t i;

	if (!group)
		return;
	fyai_tool_job_group_cancel(group);
	fyai_event_source_remove(group->animation_timer);
	group->animation_timer = NULL;
	for (i = 0; i < group->count; i++) {
		entry = &group->entries[i];
		if (entry->job)
			fyai_tool_job_discard(entry->job);
		fyai_mcp_call_destroy(entry->mcp_request);
		free(entry->call_text);
		free(entry->result_text);
	}
	free(group->entries);
	free(group);
}

int fyai_run_tool_verb(struct fyai_ctx *ctx)
{
	struct fyai_tool_args *a = &ctx->cfg->cmd.args.tool;
	bool ok;
	fy_generic args, result;
	char *stdin_buf = NULL;
	const char *args_text;
	int rc, ret = -1;

	/* Before the transient builder exists, so there is no out: to jump to. */
	if (!a->name || !*a->name) {
		fyai_error(ctx, "missing tool name");
		return -1;
	}

	if (fyai_setup_transient_builder(ctx))
		return -1;

	/* Arguments: from argv, else stdin, else an empty object. */
	args_text = a->args_json;
	if (!args_text) {
		stdin_buf = read_all_stdin();
		args_text = stdin_buf;
	}
	if (!args_text || !*args_text)
		args_text = "{}";

	args = parse_json_string(ctx->transient_gb, args_text);
	fyai_error_check(ctx, fy_is_valid(args), out,
			 "invalid JSON arguments");

	/*
	 * Sanitize the environment and confine this process before running the
	 * tool: the one-shot process *is* the sandboxed context, so no fork is
	 * needed. The shell tool still forks internally to capture output and
	 * inherits this confinement.
	 */
	rc = fyai_env_sanitize();
	fyai_error_check(ctx, !rc, out,
			 "tool: could not remove every credential from the environment");
	rc = fyai_tool_apply_sandbox(ctx);
	fyai_error_check(ctx, !rc, out,
			 "tool: could not apply sandbox policy");

	result = fyai_tool_run_one(ctx, a->name, args, &ok);
	result = fy_gb_internalize(ctx->transient_gb, result);

	if (fy_is_string(result))
		/* Machine output: the verb result as it stands. */
		(void)fyai_sink_printf(ctx->sink, FYAI_SINK_MACHINE, "%s\n",
				       fy_castp(&result, ""));
	else
		emit_generic_to_stdout(ctx, NULL, result, ctx->cfg->pretty);
	ret = 0;
out:
	free(stdin_buf);
	fyai_cleanup_transient_builder(ctx);
	return ret;
}
