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

#include "fyai_transport_boot.h"
#include "fyai_transport_sock.h"
#include "fyai_agent.h"
#include "fyai_tool_registry.h"
#include "fyai_branch.h"
#include "fyai_browser.h"
#include "fyai_agents.h"
#include "fyai_chrome.h"
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
#include "fyai_output.h"
#include "fyai_tools.h"
#include "fyai_view.h"
#include "fyai_fsview.h"
#include "fyai_wait.h"
#include "fyai_prof.h"
#include "fyai_render.h"
#include "fyai_sink.h"
#include "fyai_ui.h"
#include "fyai_workpane.h"

const char *fyai_tool_name_canonical(const char *name)
{
	return name && !strcmp(name, FYAI_TOOL_EXEC_WIRE_NAME) ? "shell" : name;
}

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

	return fyai_tool_name_canonical(
		fy_gb_intern_string(ctx->transient_gb, fy_cast(v, "")));
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
	if (fyai_tool_has(name, FYAI_TOOL_SILENT))
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
	fy_generic desc, name;
	const char *description, *session;
	char *label;

	desc = fy_get(args, "description");
	name = fy_get(args, "name");
	description = fy_castp(&desc, "");
	session = fy_castp(&name, "");
	if (*session) {
		if (asprintf(&label, "**shell** [%s]%s%s\n", session,
			     *description ? " " : "", description) < 0)
			return NULL;
	} else if (asprintf(&label, "**shell**%s%s%s\n",
			    *description ? " [" : "", description,
			    *description ? "]" : "") < 0) {
		return NULL;
	}
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

void fyai_print_tool_call(struct fyai_ctx *ctx, fy_generic tool_call,
			  bool execute)
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
		/* Allocate a stream only for an executing call. */
		ctx->shell_stream = execute ?
			calloc(1, sizeof(*ctx->shell_stream)) : NULL;
		if (ctx->shell_stream != NULL &&
		    fyai_fenced_stream_start(ctx->shell_stream, ctx, cfg, NULL,
					     cfg->tool_preview_lines > 0 ?
					     (size_t) cfg->tool_preview_lines : 0,
					     markdown_tool_output_indent(cfg),
					     stderr,
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
	uint16_t *tcp_ports;
	uint16_t *tcp_bind_ports;
	uint16_t *udp_ports;
	uint16_t *udp_bind_ports;
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
 * entries added, and the configured TCP and UDP network policies applied.
 */
static int fyai_shell_sandbox_begin(struct fyai_ctx *ctx,
				    struct fyai_shell_sandbox *sb,
				    const struct fyai_sandbox_spec **specp)
{
	struct fyai_sandbox_spec *sp = &sb->spec;
	fy_generic cs = ctx->cfg->sandbox;
	fy_generic allow, deny, net, tcp, udp, ports, e;
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

	sb->root = ctx->cfg->view_project ? strdup(ctx->cfg->view_project) :
		   fyai_discover_project_root();
	if (!sb->root && getcwd(cwd, sizeof(cwd)))
		sb->root = strdup(cwd);
	fyai_error_check(ctx, sb->root, err_out,
			 "sandbox: could not resolve the project root");
	sp->project_root = sb->root;
	sp->scratch_root = ctx->cfg->view_scratch;
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
	if (ctx->cfg->view_project || access(resolved, F_OK))
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

	/* Each present protocol mapping restricts its destination ports. */
	net = fy_get(cs, "network");
	tcp = fy_get(net, "tcp");
	udp = fy_get(net, "udp");
	if (fy_is_valid(tcp) && !fyai_sandbox_net_restrictable(-1) &&
	    ctx->cfg->sandbox_lockdown) {
		fyai_warning(ctx, "sandbox: TCP egress remains open; Landlock ABI 4 is unavailable");
		tcp = fy_invalid;
	}
	if (fy_is_valid(tcp)) {
		/* Check here because the child cannot report why it failed. */
		fyai_error_check(ctx, fyai_sandbox_net_restrictable(-1), err_out,
				 "sandbox: TCP egress cannot be restricted by this build or kernel");
		sp->restrict_tcp = true;
		ports = fy_get(tcp, "ports");
		n = fy_is_sequence(ports) ? fy_len(ports) : 0;
		if (n) {
			sb->tcp_ports = calloc(n, sizeof(*sb->tcp_ports));
			fyai_error_check(ctx, sb->tcp_ports, err_out,
					 "sandbox: could not allocate the TCP port list");
			fy_foreach(port, ports)
				sb->tcp_ports[sp->tcp_ports_n++] = (uint16_t)
					fy_cast(port, 0LL);
		}
		sp->tcp_ports = sb->tcp_ports;
		ports = fy_get(tcp, "bind_ports");
		if (fy_is_valid(ports)) {
			sp->restrict_tcp_bind = true;
			n = fy_is_sequence(ports) ? fy_len(ports) : 0;
			if (n) {
				sb->tcp_bind_ports = calloc(n, sizeof(*sb->tcp_bind_ports));
				fyai_error_check(ctx, sb->tcp_bind_ports, err_out,
						 "sandbox: could not allocate the TCP bind port list");
				fy_foreach(port, ports)
					sb->tcp_bind_ports[sp->tcp_bind_ports_n++] = (uint16_t)
						fy_cast(port, 0LL);
			}
			sp->tcp_bind_ports = sb->tcp_bind_ports;
		}
	}
	if (fy_is_valid(udp)) {
		if (!fyai_sandbox_udp_restrictable(-1)) {
			fyai_warning(ctx, "sandbox: UDP egress remains open; Landlock ABI 10 is unavailable");
		} else {
			sp->restrict_udp = true;
			ports = fy_get(udp, "ports");
			n = fy_is_sequence(ports) ? fy_len(ports) : 0;
			if (n) {
				sb->udp_ports = calloc(n, sizeof(*sb->udp_ports));
				fyai_error_check(ctx, sb->udp_ports, err_out,
						 "sandbox: could not allocate the UDP port list");
				fy_foreach(port, ports)
					sb->udp_ports[sp->udp_ports_n++] = (uint16_t)
						fy_cast(port, 0LL);
			}
			sp->udp_ports = sb->udp_ports;
			ports = fy_get(udp, "bind_ports");
			n = fy_is_sequence(ports) ? fy_len(ports) : 0;
			if (n) {
				sb->udp_bind_ports = calloc(n, sizeof(*sb->udp_bind_ports));
				fyai_error_check(ctx, sb->udp_bind_ports, err_out,
						 "sandbox: could not allocate the UDP bind port list");
				fy_foreach(port, ports)
					sb->udp_bind_ports[sp->udp_bind_ports_n++] = (uint16_t)
						fy_cast(port, 0LL);
			}
			sp->udp_bind_ports = sb->udp_bind_ports;
		}
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
	free(sb->tcp_ports);
	free(sb->tcp_bind_ports);
	free(sb->udp_ports);
	free(sb->udp_bind_ports);
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

	if (ctx->cfg->tool_child || fy_get(call, "_fyai_user_owned", false))
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
	if (rc && result->start.stage != FYAI_CHILD_STAGE_NONE)
		return strdup("");
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
	char why_buf[FYAI_CHILD_START_TEXT_MAX];
	const char *start_why;
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

		start_why = fyai_child_start_text(&tty_result.start,
						  opts.shell, opts.workdir,
						  why_buf, sizeof(why_buf));
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
		} else if (start_why) {
			/*
			 * The child did not run the
			 * program, so the status is not
			 * the answer of a program: say what stopped it.
			 */
			msg = fy_sprintfa("\ntool error: %s\n", start_why);
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
		start_why = fyai_child_start_text(&result.start, opts.shell,
						  opts.workdir, why_buf,
						  sizeof(why_buf));
		if (start_why)
			/*
			 * The child did not run the
			 * program, so the status is not
			 * the answer of a program: say what stopped it.
			 */
			msg = fy_sprintfa("\ntool error: %s\n", start_why);
		else if (result.exit_code == FYAI_SHELL_EXIT_WORKDIR &&
			 opts.workdir)
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

	fyai_agents_activity(ctx, "waiting for input");
	args = fy_assoc(gb, args, "branch", fyai_ctx_branch(ctx));
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
	fyai_agents_activity(ctx, "running");
	if (!fy_is_valid(result)) {
		fyai_error(ctx, "ask_user: the parent did not answer");
		return fy_value(gb, "tool note: the user did not provide an "
				"answer");
	}
	/* An empty answer is a question the user left: no answer, no error. */
	if (!*fy_castp(&result, ""))
		return fy_value(gb, "tool note: the user did not provide an "
				"answer");
	return result;
}

/* A question put through the input area of the page, and its answer. */
struct fyai_ask_wait {
	bool done;
	bool failed;
	char *answer;
};

static void fyai_ask_user_done(void *user, const char *answer)
{
	struct fyai_ask_wait *w = user;

	w->done = true;
	if (!answer)
		return;
	w->answer = strdup(answer);
	w->failed = !w->answer;
}

/* The options a question of the page offers at most. */
#define FYAI_ASK_OPTIONS_MAX 32

/* Ask through the input area of the page and wait for the answer. */
static fy_generic fyai_ask_user_page(struct fyai_ctx *ctx, const char *question,
				     const char *from, fy_generic options)
{
	const char *opts[FYAI_ASK_OPTIONS_MAX];
	struct fyai_ask_wait w = { 0 };
	const char *option;
	fy_generic result;
	size_t n = 0;

	fy_foreach(option, options) {
		if (n >= FYAI_ASK_OPTIONS_MAX)
			break;
		opts[n++] = option;
	}
	if (fyai_ui_ask(ctx, question, from, opts, n, fyai_ask_user_done, &w))
		return fy_value(ctx->transient_gb,
				"tool error: the question could not be asked");
	while (!w.done) {
		if (fyai_event_loop_step(fyai_ctx_loop(ctx), -1) < 0)
			break;
	}
	if (!w.done) {
		fyai_ui_ask_withdraw(ctx, &w);
		fyai_error(ctx, "ask_user: the event loop stopped while waiting "
			   "for an answer");
		return fy_value(ctx->transient_gb,
				"tool note: the user did not provide an answer");
	}
	if (w.failed)
		fyai_error(ctx, "ask_user: could not keep the answer");
	fyai_report(ctx, "\n? %s\n> %s\n", question,
		    w.answer ? w.answer : "(no answer)");
	if (!w.answer)
		return fy_value(ctx->transient_gb,
				"tool note: the user did not provide an answer");
	result = fy_value(ctx->transient_gb, w.answer);
	free(w.answer);
	if (fy_is_invalid(result))
		fyai_error(ctx, "ask_user: could not retain the answer");
	return result;
}

static fy_generic fyai_ask_user(struct fyai_ctx *ctx, fy_generic args)
{
	struct fyai_cfg *cfg = ctx->cfg;
	const char *question = fy_get(args, "question", "");
	const char *from = fy_get(args, "from", "");
	fy_generic options = fy_get(args, "options");
	size_t n = fy_is_sequence(options) ? fy_len(options) : 0;
	fy_generic result;
	char *line, *end;
	const char *a;
	size_t i;
	long sel;

	if (fyai_agent_delegated(ctx) && ctx->tool_rpc)
		return fyai_ask_user_upward(ctx, args);

	/* The page renderer puts the question in its input area. */
	if (ctx->answer_next >= cfg->answer_count && fyai_ui_ask_available(ctx))
		return fyai_ask_user_page(ctx, question, from, options);

	/* A sub-agent that asks is named in the question. */
	if (*from)
		question = fy_sprintfa("the sub-agent '%s' asks: %s", from,
				       question);
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
static fy_generic fyai_view_tool(struct fyai_ctx *ctx, fy_generic args, bool *okp);
static char *fyai_agent_input_tool(struct fyai_ctx *ctx, fy_generic args,
				   bool *okp);
static struct fyai_tool_job *fyai_agent_job_named(struct fyai_ctx *ctx,
						  const char *name);
static const char *fyai_agent_job_name(const struct fyai_tool_job *job);
static fy_generic fyai_list_tool(struct fyai_ctx *ctx, fy_generic args, bool *okp);
static char *fyai_shell_close_tool(struct fyai_ctx *ctx, fy_generic args,
				   bool *okp);
static char *fyai_cancel_tool(struct fyai_ctx *ctx, fy_generic args, bool *okp);

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

/*
 * The result of a function tool is text: the APIs take a string as the output of
 * a function call and refuse an object. A tool that builds a structure gives it
 * to the model as its compact JSON.
 */
static fy_generic fyai_tool_result_text(struct fyai_ctx *ctx, fy_generic result)
{
	const char *text;

	if (!fy_is_mapping(result) && !fy_is_sequence(result))
		return result;
	text = emit_json_string(ctx->transient_gb, result);
	if (!text) {
		fyai_error(ctx, "could not format the tool result");
		return fy_value(ctx->transient_gb, "tool error: could not format the result");
	}
	return fy_value(ctx->transient_gb, text);
}

/* The tools whose behaviour is a short function of the arguments. */
static char *tool_read_file(struct fyai_ctx *ctx, fy_generic args, bool *okp)
{
	char *result = fyai_read_file_tool(ctx, args);

	*okp = result != NULL;
	return result;
}

static char *tool_write_file(struct fyai_ctx *ctx, fy_generic args, bool *okp)
{
	const char *path, *content;
	int rc;

	(void)ctx;
	path = fy_get(args, "path", "");
	content = fy_get(args, "content", "");
	rc = write_text_file(path, content);
	*okp = !rc;
	return strdup(!rc ? "ok" : "error");
}

static char *tool_apply_patch(struct fyai_ctx *ctx, fy_generic args, bool *okp)
{
	const char *content = fy_get(args, "patch", "");
	char *result;

	/* Resolve the patch before it changes the pre-image. */
	free(ctx->patch_display);
	ctx->patch_display = fyai_patch_to_unified_ctx(ctx, content);
	result = fyai_apply_patch_text_ctx(ctx, content);
	*okp = result && strncmp(result, "tool error:", 11);
	return result;
}

static fy_generic tool_project_view(struct fyai_ctx *ctx, fy_generic args,
				    bool *okp)
{
	return fyai_tool_result_text(ctx, fyai_view_tool(ctx, args, okp));
}

static fy_generic tool_list(struct fyai_ctx *ctx, fy_generic args, bool *okp)
{
	return fyai_tool_result_text(ctx, fyai_list_tool(ctx, args, okp));
}

static fy_generic tool_ask_user(struct fyai_ctx *ctx, fy_generic args,
				bool *okp)
{
	fy_generic result = fyai_ask_user(ctx, args);

	*okp = strncmp(fy_castp(&result, ""), "tool error:", 11) != 0;
	return result;
}

static void tool_head_list(struct fyai_ctx *ctx, FILE *mf,
			   struct fy_generic_builder *gb, fy_generic args,
			   int preview_lines, struct fyai_md_blocks *blocks)
{
	fy_generic gc = fy_get(args, "kind");
	const char *c = fy_castp(&gc, "");

	(void)ctx;
	(void)gb;
	(void)preview_lines;
	(void)blocks;
	fprintf(mf, "**list** %s\n\n", *c ? c : "all");
}

static void tool_head_project_view(struct fyai_ctx *ctx, FILE *mf,
				   struct fy_generic_builder *gb,
				   fy_generic args, int preview_lines,
				   struct fyai_md_blocks *blocks)
{
	fy_generic gc = fy_get(args, "action"), gpath = fy_get(args, "name");
	fy_generic paths = fy_get(args, "paths", fy_invalid), item;
	const char *c = fy_castp(&gc, ""), *path = fy_castp(&gpath, "");
	bool first = true;

	(void)ctx;
	(void)gb;
	(void)preview_lines;
	(void)blocks;
	fprintf(mf, "**view %s**", *c ? c : "?");
	if (*path)
		fprintf(mf, " `%s`", path);
	fy_foreach(item, paths) {
		gc = item;
		fprintf(mf, "%s`%s`", first ? " " : ", ", fy_castp(&gc, ""));
		first = false;
	}
	if (fy_get(args, "dry_run", false))
		fprintf(mf, " (dry run)");
	fprintf(mf, "\n\n");
}

static void tool_head_ask_user(struct fyai_ctx *ctx, FILE *mf,
			       struct fy_generic_builder *gb, fy_generic args,
			       int preview_lines, struct fyai_md_blocks *blocks)
{
	fy_generic gq = fy_get(args, "question");

	(void)ctx;
	(void)gb;
	(void)preview_lines;
	(void)blocks;
	fprintf(mf, "**❓ %s**\n\n", fy_castp(&gq, ""));
}

static void tool_head_cancel(struct fyai_ctx *ctx, FILE *mf,
			     struct fy_generic_builder *gb, fy_generic args,
			     int preview_lines, struct fyai_md_blocks *blocks)
{
	fy_generic gname = fy_get(args, "name"), gkind = fy_get(args, "kind");
	const char *name = fy_castp(&gname, ""), *kind = fy_castp(&gkind, "");

	(void)ctx;
	(void)gb;
	(void)preview_lines;
	(void)blocks;
	fprintf(mf, "**cancel**%s%s%s%s%s\n\n", *kind ? " " : "", kind,
		*name ? " `" : "", name, *name ? "`" : "");
}

static void tool_head_agent_input(struct fyai_ctx *ctx, FILE *mf,
				  struct fy_generic_builder *gb, fy_generic args,
				  int preview_lines,
				  struct fyai_md_blocks *blocks)
{
	fy_generic gname = fy_get(args, "name");
	const char *name = fy_castp(&gname, "");

	(void)ctx;
	(void)gb;
	(void)preview_lines;
	(void)blocks;
	fprintf(mf, "**agent input**%s%s%s\n\n", *name ? " [" : "", name,
		*name ? "]" : "");
}

/*
 * A session or a tile shows the effect of the terminal tools, so they draw no
 * head of their own. The tools of the parent read its job and session tables,
 * which a forked job does not have.
 */
const struct fyai_tool_def fyai_tools_defs[] = {
	{ .name = "read_file", .run_text = tool_read_file,
	  .head = fyai_tool_head_read_file, .flags = FYAI_TOOL_MARKED },
	{ .name = "write_file", .run_text = tool_write_file,
	  .head = fyai_tool_head_write_file, .flags = FYAI_TOOL_MARKED,
	  .effect = FYAI_TOOL_EFFECT_FILES },
	{ .name = "apply_patch", .run_text = tool_apply_patch,
	  .head = fyai_tool_head_apply_patch, .flags = FYAI_TOOL_MARKED,
	  .effect = FYAI_TOOL_EFFECT_FILES },
	{ .name = "shell", .run_text = fyai_run_shell_command,
	  .head = fyai_tool_head_shell, .flags = FYAI_TOOL_MARKED,
	  .effect = FYAI_TOOL_EFFECT_PROCESS },
	{ .name = "shell_input", .run_text = fyai_shell_input_tool,
	  .flags = FYAI_TOOL_PARENT | FYAI_TOOL_SILENT,
	  .effect = FYAI_TOOL_EFFECT_PROCESS },
	{ .name = "shell_output", .run_text = fyai_shell_output_tool,
	  .flags = FYAI_TOOL_PARENT | FYAI_TOOL_SILENT },
	{ .name = "shell_close", .run_text = fyai_shell_close_tool,
	  .flags = FYAI_TOOL_PARENT | FYAI_TOOL_SILENT,
	  .effect = FYAI_TOOL_EFFECT_PROCESS },
	{ .name = "cancel", .run_text = fyai_cancel_tool,
	  .head = tool_head_cancel, .flags = FYAI_TOOL_PARENT,
	  .effect = FYAI_TOOL_EFFECT_PROCESS },
	{ .name = "agent_input", .run_text = fyai_agent_input_tool,
	  .head = tool_head_agent_input,
	  .flags = FYAI_TOOL_PARENT | FYAI_TOOL_NOT_FOR_CHILD,
	  .effect = FYAI_TOOL_EFFECT_PROCESS },
	{ .name = "ask_user", .run = tool_ask_user, .head = tool_head_ask_user,
	  .flags = FYAI_TOOL_PARENT },
	{ .name = "project_view", .run = tool_project_view,
	  .head = tool_head_project_view,
	  .flags = FYAI_TOOL_PARENT | FYAI_TOOL_NOT_FOR_CHILD,
	  .effect = FYAI_TOOL_EFFECT_PROCESS },
	{ .name = "list", .run = tool_list, .head = tool_head_list,
	  .flags = FYAI_TOOL_PARENT },
};
const size_t fyai_tools_defs_count = ARRAY_SIZE(fyai_tools_defs);

fy_generic fyai_tool_run_one(struct fyai_ctx *ctx, const char *name,
			     fy_generic args, bool *okp)
{
	const struct fyai_tool_def *def = fyai_tool_find(name);
	fy_generic result_generic;
	char *result;

	*okp = false;
	if (!def || (!def->run && !def->run_text))
		return fy_gb_internalize(ctx->transient_gb,
				fy_stringf("tool error: unknown tool %s", name));
	if (def->run)
		return def->run(ctx, args, okp);

	result = def->run_text(ctx, args, okp);
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

int fyai_tools_confine(struct fyai_ctx *ctx)
{
	return fyai_tool_apply_sandbox(ctx);
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

/* An executed sub-agent also inherits its channels to the credential transport. */
#define FYAI_TOOL_CHILD_TP_AGENT_FD 5
#define FYAI_TOOL_CHILD_TP_CTL_FD 6
/* A sub-agent in a view announces its process to the supervisor on this one. */
#define FYAI_TOOL_CHILD_VIEW_FD 7

/*
 * Arrange the descriptors of a tool child: the control channel on 3 and 4, and,
 * for a sub-agent under credential isolation, its transport channels on 5 and
 * 6. @tp_agent_fd and @tp_ctl_fd are -1 for a child with none. A
 * non-negative @view_fd moves to FYAI_TOOL_CHILD_VIEW_FD, which only the
 * entry of the view reads. Every other descriptor is closed.
 */
static int fyai_tool_child_fds(struct fyai_ctx *ctx, int req_fd, int rsp_fd, int tp_agent_fd,
			       int tp_ctl_fd, int view_fd, bool keep_arena)
{
	int req_dup = -1, rsp_dup = -1, agent_dup = -1, ctl_dup = -1, view_dup = -1;
	bool tp = tp_agent_fd >= 0 && tp_ctl_fd >= 0;
	int first_free = view_fd >= 0 ? FYAI_TOOL_CHILD_VIEW_FD + 1 :
			 tp ? FYAI_TOOL_CHILD_TP_CTL_FD + 1 : FYAI_TOOL_CHILD_RSP_FD + 1;
	int devnull, arena_dup;
	int rc;

	/* Only a sub-agent runtime keeps the arena of the view, above the fixed targets. */
	if (keep_arena && ctx->cfg->view_arena_fd >= 0) {
		arena_dup = fcntl(ctx->cfg->view_arena_fd, F_DUPFD_CLOEXEC, first_free);
		if (arena_dup < 0)
			goto err;
		close(ctx->cfg->view_arena_fd);
		if (fyai_fsview_arena_adopt(ctx->cfg, arena_dup))
			goto err;
	} else {
		fyai_fsview_arena_drop(ctx->cfg);
	}
	/* Move all clear of the target numbers before dup2 can clobber one. */
	req_dup = fcntl(req_fd, F_DUPFD_CLOEXEC, 7);
	if (req_dup < 0)
		goto err;
	rsp_dup = fcntl(rsp_fd, F_DUPFD_CLOEXEC, 7);
	if (rsp_dup < 0)
		goto err;
	if (tp) {
		agent_dup = fcntl(tp_agent_fd, F_DUPFD_CLOEXEC, 7);
		if (agent_dup < 0)
			goto err;
		ctl_dup = fcntl(tp_ctl_fd, F_DUPFD_CLOEXEC, 7);
		if (ctl_dup < 0)
			goto err;
	}
	if (view_fd >= 0) {
		view_dup = fcntl(view_fd, F_DUPFD_CLOEXEC, FYAI_TOOL_CHILD_VIEW_FD + 1);
		if (view_dup < 0)
			goto err;
	}

	rc = dup2(req_dup, FYAI_TOOL_CHILD_REQ_FD);
	if (rc < 0)
		goto err;
	rc = dup2(rsp_dup, FYAI_TOOL_CHILD_RSP_FD);
	if (rc < 0)
		goto err;
	if (tp) {
		rc = dup2(agent_dup, FYAI_TOOL_CHILD_TP_AGENT_FD);
		if (rc < 0)
			goto err;
		rc = dup2(ctl_dup, FYAI_TOOL_CHILD_TP_CTL_FD);
		if (rc < 0)
			goto err;
	}
	if (view_fd >= 0) {
		rc = dup2(view_dup, FYAI_TOOL_CHILD_VIEW_FD);
		if (rc < 0)
			goto err;
		rc = fcntl(FYAI_TOOL_CHILD_VIEW_FD, F_SETFD, FD_CLOEXEC);
		if (rc < 0)
			goto err;
	}
	/* Do not pass the channels to a shell command. */
	rc = fcntl(FYAI_TOOL_CHILD_REQ_FD, F_SETFD, FD_CLOEXEC);
	if (rc < 0)
		goto err;
	rc = fcntl(FYAI_TOOL_CHILD_RSP_FD, F_SETFD, FD_CLOEXEC);
	if (rc < 0)
		goto err;
	if (tp && (fcntl(FYAI_TOOL_CHILD_TP_AGENT_FD, F_SETFD, FD_CLOEXEC) < 0 ||
		   fcntl(FYAI_TOOL_CHILD_TP_CTL_FD, F_SETFD, FD_CLOEXEC) < 0))
		goto err;

	/* Detach unused input unless the child owns this terminal. */
	if (isatty(STDIN_FILENO) && ttyname(STDIN_FILENO) &&
	    getsid(0) == tcgetsid(STDIN_FILENO)) {
		goto close_fds;
	}
	devnull = open("/dev/null", O_RDONLY);
	if (devnull < 0)
		goto err;
	rc = dup2(devnull, STDIN_FILENO);
	if (devnull != STDIN_FILENO)
		close(devnull);
	if (rc < 0)
		goto err;

close_fds:
	fyai_close_fds_except(first_free, ctx->cfg->view_arena_fd);
	return 0;

err:
	if (req_dup >= 0)
		close(req_dup);
	if (rsp_dup >= 0)
		close(rsp_dup);
	if (agent_dup >= 0)
		close(agent_dup);
	if (ctl_dup >= 0)
		close(ctl_dup);
	if (view_dup >= 0)
		close(view_dup);
	return -1;
}

/*
 * Keep the spawn state of an executed child. fyai_agent_run() re-opens the
 * arena and replaces the builders, so the state is kept as JSON until it is
 * adopted there. The identity of the parent, a command-line key and the
 * configuration are applied now.
 */
static int fyai_tool_child_spawn_take(struct fyai_ctx *ctx, fy_generic spawn)
{
	struct fyai_cfg *cfg = ctx->cfg;
	const char *json, *key;

	if (!fy_is_mapping(spawn))
		return 0;
	json = emit_json_string(fyai_ctx_transient_gb(ctx), spawn);
	fyai_error_check(ctx, json, err,
			 "could not keep the sub-agent spawn state");
	free(ctx->agent_spawn_json);
	ctx->agent_spawn_json = strdup(json);
	fyai_error_check(ctx, ctx->agent_spawn_json, err,
			 "could not keep the sub-agent spawn state");
	ctx->agent_parent = fy_get(spawn, "parent", 0LL);
	/* Credential isolation: the parent has registered this child. */
	if (fyai_transport_supervised()) {
		fyai_error_check(ctx, fy_get(fy_get(spawn, "transport", fy_invalid),
					     "exec", 0LL), err,
				 "the parent of this sub-agent did not register it with "
				 "the credential transport");
		if (fyai_transport_child_attach(ctx,
				fy_get(spawn, "transport", fy_invalid)))
			goto err;
	}
	key = fy_get(spawn, "api_key", "");
	if (!fy_str_empty(key)) {
		cfg->api_key = fy_gb_intern_string(cfg->gb, key);
		fyai_error_check(ctx, cfg->api_key, err,
				 "could not keep the parent API key");
		cfg->api_key_explicit = true;
		cfg->api_key_auto = false;
	}
	return fyai_agent_spawn_config(ctx, spawn);

err:
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
	bool spawn_failed;	/* the parent state could not be adopted */
	/* The size of the tile the session starts at, or 0. */
	int size_rows, size_cols;
};

static fy_generic fyai_tool_child_serve(struct jsonrpc_conn *conn,
					const char *method, fy_generic params,
					fy_generic id, void *userdata,
					fy_generic *errorp)
{
	struct fyai_tool_child *tc = userdata;
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(tc->ctx);
	fy_generic result = fy_invalid;
	char *bytes;
	size_t len;

	if (fyai_agents_serve(tc->ctx, conn, method, params, id, &result, errorp))
		return result;
	if (!strcmp(method, "tool/run")) {
		if (!fy_is_valid(id) || tc->pending || tc->done) {
			*errorp = fy_gb_mapping(gb, "code", -32600LL,
						"message", "unexpected tool/run");
			return fy_invalid;
		}
		tc->args = fy_get(params, "call", fy_invalid);
		tc->branch = fy_get(params, "branch", fy_invalid);
		tc->size_rows = (int)fy_get(fy_get(params, "size", fy_invalid),
					   "rows", 0LL);
		tc->size_cols = (int)fy_get(fy_get(params, "size", fy_invalid),
					   "cols", 0LL);
		/* The result carries the diagnostics of a refused state. */
		if (tc->ctx->cfg->tool_exec)
			tc->spawn_failed = fyai_tool_child_spawn_take(tc->ctx,
					fy_get(params, "spawn", fy_invalid));
		tc->id = id;
		tc->pending = true;
		jsonrpc_conn_defer(conn);
		return fy_invalid;
	}
	if (!strcmp(method, "tty/resize")) {
		tc->ctx->tty_rows = (int)fy_get(params, "rows", 0LL);
		tc->ctx->tty_cols = (int)fy_get(params, "cols", 0LL);
		/*
		 * A resize before the session starts replaces the size of
		 * the run: the parent sends each grant one time.
		 */
		if (!tc->relay && tc->ctx->tty_rows > 0 &&
		    tc->ctx->tty_cols > 0) {
			tc->size_rows = tc->ctx->tty_rows;
			tc->size_cols = tc->ctx->tty_cols;
		}
		fyai_terminal_session_resize(tc->ctx, tc->ctx->tty_rows,
					     tc->ctx->tty_cols);
		if (tc->relay)
			fyai_terminal_relay_resize(tc->relay, tc->ctx->tty_rows,
						   tc->ctx->tty_cols);
		(void)jsonrpc_notify(conn, "tty/resized",
			fy_gb_mapping(gb, "rows", (long long)tc->ctx->tty_rows,
				      "cols", (long long)tc->ctx->tty_cols));
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

/* Most credential names a program of the configuration keeps. */
#define FYAI_TOOL_ENV_KEEP_MAX	32

/*
 * Fill @names with the credentials that a user-owned call keeps, NULL
 * terminated. Only fyai_tools_config_program() sets them, and a provider
 * call cannot be user-owned, so a model cannot name one. The names point into
 * @args, which must outlive their use.
 */
static void fyai_tool_env_keep(fy_generic args, const char **names)
{
	fy_generic keep;
	const char *name;
	size_t n;

	n = 0;
	if (fy_get(args, "_fyai_user_owned", false)) {
		keep = fy_get(args, "_fyai_env_keep", fy_invalid);
		fy_foreach(name, keep) {
			if (n == FYAI_TOOL_ENV_KEEP_MAX)
				break;
			if (!fy_str_empty(name))
				names[n++] = name;
		}
	}
	names[n] = NULL;
}

/* True when a user-owned call runs without the shell sandbox. */
static bool fyai_tool_unconfined(fy_generic args)
{
	return fy_get(args, "_fyai_user_owned", false) &&
	       fy_get(args, "_fyai_unconfined", false);
}

/* Open a session, answer its start, then serve it until it ends. */
static void fyai_tool_child_session(struct fyai_ctx *ctx,
				    struct fyai_tool_child *tc,
				    struct jsonrpc_conn *conn)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	const struct fyai_sandbox_spec *sandbox;
	const char *env_keep[FYAI_TOOL_ENV_KEEP_MAX + 1];
	struct fyai_terminal_opts opts = {};
	struct fyai_shell_sandbox sb;
	struct fyai_event_loop *el;
	fy_generic args, command, workdir;
	fy_generic diag;
	bool started, stop;
	int rc;

	args = fyai_tool_call_args(ctx, tc->args);
	command = fy_get(args, "command", fy_invalid);
	workdir = fy_get(args, "workdir", fy_invalid);

	fyai_tool_env_keep(args, env_keep);
	if (fyai_tool_unconfined(args)) {
		memset(&sb, 0, sizeof(sb));
		sandbox = NULL;
		rc = 0;
	} else {
		rc = fyai_shell_sandbox_begin(ctx, &sb, &sandbox);
	}
	if (!rc) {
		opts.env_keep = env_keep;
		opts.workdir = fy_castp(&workdir, (const char *)NULL);
		opts.term = ctx->cfg->shell_tty_term;
		fyai_shell_tty_size(ctx, args, &opts.rows, &opts.cols);
		/* The parent laid out the tile before the start. */
		if (tc->size_rows > 0 && tc->size_cols > 0) {
			opts.rows = tc->size_rows;
			opts.cols = tc->size_cols;
		}
		/* Use pipes unless the call explicitly requests a terminal. */
		opts.pipes = !fyai_shell_tty_requested(ctx, args);
		opts.shell = fyai_shell_shell_requested(ctx, args);
		opts.login = fyai_shell_login_requested(ctx, args);
		tc->relay = fyai_terminal_relay_start(ctx, conn,
						fy_castp(&command, ""),
						sandbox, &opts);
		if (tc->relay && !opts.pipes)
			fyai_diag_tracef("shell", "terminal %dx%d", opts.rows,
					 opts.cols);
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

	/*
	 * Stop the program when this serving child is terminated, and when the
	 * control channel closes: the parent that owns the session is gone, and
	 * the child is in a session of its own, which no signal of the parent
	 * reaches.
	 */
	el = fyai_ctx_loop(ctx);
	assert(el);
	while (!fyai_terminal_relay_done(tc->relay)) {
		stop = ctx->terminate_pending || jsonrpc_conn_closed(conn);
		if (stop)
			fyai_terminal_relay_close(tc->relay, true);
		if (fyai_event_loop_step(el, stop ? 200 : -1) < 0)
			break;
		if (stop && fyai_terminal_relay_reaped(tc->relay))
			break;
	}

	fyai_terminal_relay_destroy(tc->relay);
	tc->relay = NULL;
}

static void fyai_tool_child_serve_loop(struct fyai_ctx *ctx)
{
	const char *env_keep[FYAI_TOOL_ENV_KEEP_MAX + 1];
	struct fyai_tool_child tc;
	struct fyai_event_loop *el;
	struct jsonrpc_conn *conn;
	fy_generic result, diag;
	bool ok = false, unconfined;

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
			fyai_tool_env_keep(fyai_tool_call_args(ctx, tc.args),
					   env_keep);
			if (!fy_equal(fyai_tool_call_name(ctx, tc.args), "agent") &&
			    fyai_env_sanitize(env_keep))
				fyai_error(ctx,
					   "could not remove every credential from the tool environment");
			/*
			 * A sub-agent runtime publishes to the arena that the sandbox
			 * denies. Each tool that it runs is confined in its own child.
			 */
			unconfined = !fy_equal(fyai_tool_call_name(ctx, tc.args), "agent") &&
				     fyai_tool_apply_sandbox(ctx);
			if (!unconfined && fyai_shell_session_call(ctx, tc.args)) {
				fyai_tool_child_session(ctx, &tc, conn);
				/* Exit according to whether the session opened. */
				ok = tc.session_started;
				break;
			}
			if (unconfined) {
				result = fy_value(fyai_ctx_transient_gb(ctx),
					"tool error: the sandbox could not confine the tool");
			} else if (tc.spawn_failed) {
				result = fy_value(fyai_ctx_transient_gb(ctx),
					"tool error: the sub-agent could not "
					"adopt the state of its parent");
			} else if (fy_equal(fyai_tool_call_name(ctx, tc.args),
					    "agent") &&
				   fyai_agents_enter(ctx)) {
				result = fy_value(fyai_ctx_transient_gb(ctx),
					"tool error: agent admission refused");
			} else {
				result = fyai_execute_tool_call(ctx, tc.args, &ok);
			}
			fyai_agents_leave(ctx, ok);
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
	struct fy_generic_builder *call_gb; /* owns the call across turn cleanup */
	struct jsonrpc_conn *conn;	/* control channel to the child */
	struct jsonrpc_request *run;	/* the outstanding tool/run */
	fy_generic result;
	fy_generic display;		/* tool-resolved presentation, if any */
	fy_generic diag;		/* diagnostics collected by the child */
	char *origin;			/* who the child was, for a diagnostic */
	pid_t pid;
	uint64_t transport_exec;	/* the transport's id for this sub-agent, or 0 */
	int rfd;
	int pfd;
	struct fyai_fenced_stream stream;
	struct fyai_sink_band *band;
	struct fyai_event_source *band_delay;
	struct response_buffer pending_output;
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
	bool btw;			/* the user opened this side question */
	bool btw_panel;		/* its finished screen remains in the pane */
	bool exec;			/* the child executes fyai again */
	bool band_progress;
	int pty;			/* terminal of a sub-agent, -1 if none */
	int pty_rows, pty_cols;
	/* The size of the tile of a terminal session, which its program
	 * starts at, or 0. */
	int start_rows, start_cols;
	/* What the tile draws: the work pane decides it from the grant. */
	enum fyai_workpane_present present;
	struct fyai_terminal_view *view;	/* what it drew there */
	struct fytim_surface *surface;		/* and where that is shown */
	/* The screen is a block of the transcript at the call, not a tile. */
	bool inline_block;
	struct fyai_event_source *ptysrc;
	struct fyai_event_source *animation;
	size_t animation_frame;
	struct fyai_event_source *waiter;	/* asks if it stopped for input */
	bool wants_input;
	bool terminating;
	bool timed_out;
	bool overdue;
	unsigned int timeout_ms;	/* 0 = no limit */
	unsigned int hang_timeout_ms;
	fyai_event_ms_t started_ms;
	fyai_event_ms_t elapsed_ms;
	struct response_buffer progress;	/* tail, for a timeout report */
	char *branch;			/* sub-agent branch, allocated by us */
	struct fyai_event_source *deadline;
	struct fyai_event_source *hang_deadline;
	int term_signal;
	struct fyai_tool_job_group *group;
	struct fyai_tool_job *next;	/* ctx->tool_jobs, for a resize */
	struct fyai_shell_session *session;	/* the session this job drives */
	struct fyai_view_run *view_run;	/* the view of an isolated sub-agent */
	bool view_entry_failed;		/* the child could not enter that view */
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
	struct fyai_event_source *animation;
	size_t animation_frame;
	struct fytim_surface *surface;
	char *title;
	char *description;		/* model-provided call description */
	bool recorded;			/* exchange stored in the document */
	/* A bang shell: its tile stays after the program until the user
	 * dismisses it. */
	bool keep_tile;
	/* The user closed the tile: it goes when the program ends. */
	bool close_on_exit;
	pid_t pid;			/* the program, watched for a read */
	bool pipes;			/* it was given no terminal */
	struct fyai_event_source *waiter;
	bool wants_input;		/* it stopped for input and was said so */
	fyai_event_ms_t quiet_since;	/* when its terminal last had output */
	bool feeding;			/* keystrokes are on their way to it */
	bool drew;			/* its terminal had output one time */
	int rows;			/* the size the session was opened with */
	int exit_code;
	int signal;
	bool exited;
	bool closing;
	bool timed_out;
	bool user_owned;
	/* Told when the program ends, for a program the user runs through fyai. */
	fyai_tools_exit_fn on_exit;
	void *on_exit_data;
	/* What the tile draws: the work pane decides it from the grant. */
	enum fyai_workpane_present present;		/* opened by a bang command */
	int resize_rows;		/* pending child resize */
	int resize_cols;
	/* The screen is a block of the transcript at the call, not a tile. */
	bool inline_block;
};

/* Size an agent terminal to its display surface. */
#define FYAI_AGENT_TTY_ROWS	12

static int fyai_shell_inline_cols(struct fyai_ctx *ctx);

static void fyai_agent_tty_size(struct fyai_ctx *ctx, int *rowsp, int *colsp)
{
	int rows = 0, cols = 0;

	/* A screen drawn in the transcript has the rows of the setting and
	 * the width of the transcript. */
	if (fyai_ui_tools_inline(ctx)) {
		*rowsp = ctx->cfg->inline_terminal_rows;
		*colsp = fyai_shell_inline_cols(ctx);
		return;
	}

	if (fyai_ui_size(ctx, &cols, &rows) || cols < 1) {
		cols = markdown_render_width();
		rows = markdown_render_height();
	}
	cols -= markdown_gutter_cols(ctx->cfg);
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

/* Close inherited job descriptors without affecting parent-owned jobs. */
static void fyai_tool_jobs_abandon(struct fyai_ctx *ctx)
{
	struct fyai_tool_job *job, *next;

	if (!ctx)
		return;
	for (job = ctx->tool_jobs; job; job = next) {
		next = job->next;
		/* Never a standard descriptor: this child keeps its own. */
		if (job->pfd > STDERR_FILENO)
			close(job->pfd);
		if (job->rfd > STDERR_FILENO)
			close(job->rfd);
		if (job->pty > STDERR_FILENO)
			close(job->pty);
		job->pfd = job->rfd = job->pty = -1;
		job->conn = NULL;
		job->session = NULL;
		job->animation = NULL;
		job->next = NULL;
	}
	ctx->tool_jobs = NULL;
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
		/* Displayed terminals use their post-layout tile size. */
		if ((job->session && job->session->surface) || job->surface)
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
/*
 * Time a session must be quiet before it is typed at. Input written to a
 * program that is still starting is lost: the line discipline holds the bytes
 * while the terminal is still cooked, and a program that starts by asking the
 * terminal about itself reads them as the answer it waits for. A program
 * stopped for input writes nothing, thus it is quiet and is answered at once.
 */
#define FYAI_SHELL_IDLE_TRIGGER_MS	100
/*
 * The gate opens at this time whatever the program does. A program that never
 * stops writing is one the quiet time cannot describe, and its input is owed
 * to it all the same.
 */
#define FYAI_SHELL_IDLE_MAX_WAIT_MS	5000
#define FYAI_SHELL_IDLE_WAIT_MAX_MS	30000
/*
 * The interval at which a session that drew nothing is examined again. Such a
 * session is quiet because it did not start, and not because it settled, thus
 * the quiet time does not apply to it yet. The wait stops at the first byte
 * that the session draws, when the program reads its terminal, or at the time
 * limit of the call. A constant time limit is correct for one machine only.
 */
#define FYAI_SHELL_FIRST_DRAW_POLL_MS	50
/* Time a program gets to leave after it is asked to. */
#define FYAI_TTY_CLOSE_WAIT_MS		500

static void fyai_tool_job_discard(struct fyai_tool_job *job);
static void fyai_shell_session_close(struct fyai_shell_session *sess,
				     bool force);

/* Draw the head of a live terminal session at its current frame. */
static void fyai_shell_session_head_paint(void *owner)
{
	struct fyai_shell_session *sess = owner;

	if (!sess->surface || sess->exited)
		return;
	(void)fyai_chrome_update(sess->ctx, sess->surface,
		&(struct fyai_chrome_spec){
			.title = sess->title ? sess->title : "**shell**",
			.command = sess->command,
			.mark = FYAI_UI_MARK_RUNNING,
			.frame = sess->animation_frame,
		}, NULL);
	fyai_ui_wake(sess->ctx);
}

static void fyai_shell_session_inline_paint(struct fyai_shell_session *sess,
					    bool done);
static void fyai_agent_inline_paint(struct fyai_tool_job *job, bool done,
				    bool ok);

/*
 * Arm the timer that advances the state mark of a block of the transcript, at
 * the interval of the indicator of the theme. A mark that does not move asks
 * for none.
 */
static void fyai_inline_animation_arm(struct fyai_ctx *ctx,
		enum fyai_event_action (*fn)(const struct fyai_event *ev),
		void *owner, struct fyai_event_source **srcp)
{
	struct fyai_event_loop *el = fyai_ctx_loop(ctx);
	unsigned int ms = 0;
	char *mark;

	mark = fyai_ui_indicator(ctx, FYAI_UI_MARK_RUNNING, 0, &ms);
	free(mark);
	if (!el || !ms)
		return;
	if (fyai_event_add_timer(el, ms, ms, fn, owner, srcp))
		fyai_warning(ctx, "the indicator of a call in the transcript "
			     "could not start");
}

/* Advance the state mark on the title of a live terminal session. */
static enum fyai_event_action
fyai_shell_session_animate(const struct fyai_event *ev)
{
	struct fyai_shell_session *sess = ev->userdata;

	if (sess->inline_block && !sess->exited) {
		sess->animation_frame++;
		fyai_shell_session_inline_paint(sess, false);
		return FYAIEA_CONTINUE;
	}
	if (!sess->surface || sess->exited)
		return FYAIEA_CONTINUE;
	sess->animation_frame++;
	fyai_shell_session_head_paint(sess);
	return FYAIEA_CONTINUE;
}

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

/*
 * The tool child holds the terminal of a session, thus a reply uses the same
 * path as typed input. A program that sends a query waits for the reply.
 * Without it the program waits for its own time limit, and it then reads the
 * next typed input as the reply.
 */
static void fyai_shell_session_reply(const char *data, size_t len, void *user)
{
	struct fyai_shell_session *sess = user;
	struct fy_generic_builder *gb;

	if (!sess->job || !sess->job->conn || sess->exited)
		return;
	gb = fyai_ctx_transient_gb(sess->ctx);
	if (!gb)
		return;
	(void)jsonrpc_notify(sess->job->conn, "shell/write",
			     fyai_bytes_to_generic(gb, data, len));
}

static const struct fyai_workpane_tile_ops fyai_shell_session_tile_ops;
static const struct fyai_workpane_tile_ops fyai_agent_tile_ops;
static void fyai_agent_head_repaint(void *owner);

/* Sizes below which a live screen is not worth drawing; the head still
 * shows whose call this is and whether it is running. */
#define FYAI_TILE_FULL_ROWS	4
#define FYAI_TILE_FULL_COLS	30
#define FYAI_TILE_OUTPUT_ROWS	2

static const struct fyai_workpane_ladder fyai_tile_ladder = {
	.full_rows = FYAI_TILE_FULL_ROWS,
	.full_cols = FYAI_TILE_FULL_COLS,
	.output_rows = FYAI_TILE_OUTPUT_ROWS,
};

/* True while the tile is large enough to show the screen of its program. */
static bool fyai_tile_draws_program(enum fyai_workpane_present p)
{
	return p == FYAI_WORKPANE_PRESENT_FULL ||
	       p == FYAI_WORKPANE_PRESENT_OUTPUT;
}

/* Create the session and reserve its name, before the process is spawned. */
static struct fyai_shell_session *
fyai_shell_session_create(struct fyai_ctx *ctx, const char *name,
			  const char *command, const char *title,
			  const char *description, int rows,
			  int cols, size_t max_bytes, bool pipes,
			  bool user_owned)
{
	struct fyai_event_loop *el;
	struct fyai_shell_session *sess;
	unsigned int animation_ms;
	int rc;

	animation_ms = 0;
	sess = calloc(1, sizeof(*sess));
	if (!sess)
		return NULL;
	sess->ctx = ctx;
	/* A session that has drawn nothing has been quiet since it opened. */
	sess->quiet_since = fyai_event_now_ms();
	sess->name = strdup(name);
	sess->command = strdup(command ? command : "");
	sess->branch = strdup(fyai_ctx_branch(ctx));
	sess->view = fyai_terminal_view_create(ctx, rows, cols, max_bytes);
	fyai_error_check(ctx,
			 sess->name && sess->command && sess->branch && sess->view,
			 fail, "shell: could not allocate the terminal session");
	rc = fyai_terminal_view_set_history(sess->view,
					    ctx->cfg->work_history_rows);
	fyai_error_check(ctx, !rc, fail,
			 "shell: cannot allocate terminal scrollback");
	/* Pipe output uses bare line feeds. */
	fyai_terminal_view_cooked(sess->view, pipes);
	fyai_terminal_view_line_cb(sess->view, fyai_shell_session_line, sess);
	fyai_terminal_view_reply_cb(sess->view, fyai_shell_session_reply, sess);
	sess->title = title ? strdup(title) : NULL;
	sess->description = description ? strdup(description) : NULL;
	sess->rows = rows;
	sess->pipes = pipes;
	sess->user_owned = user_owned;
	/* A call of the model draws its screen at the call in the
	 * transcript; a program of the user keeps its tile. */
	sess->inline_block = !user_owned && fyai_ui_tools_inline(ctx);
	sess->surface = sess->inline_block ? NULL :
			fyai_ui_surface_open(ctx, rows, cols);
	if (sess->inline_block) {
		fyai_inline_animation_arm(ctx, fyai_shell_session_animate, sess,
					  &sess->animation);
		fyai_terminal_view_damage_all(sess->view);
	}
	if (sess->surface) {
		/* A user program accepts what the pane grants; a model
		 * session keeps the height it asked for. */
		(void)fyai_workpane_register(ctx->workpane, sess->surface,
					     FYAI_WORKPANE_TILE_SHELL, sess,
					     &fyai_shell_session_tile_ops,
					     user_owned ? FYAI_WORKPANE_FILL :
							  rows,
					     user_owned ? 0 : rows);
		fyai_workpane_tile_set_ladder(ctx->workpane, sess->surface,
					      &fyai_tile_ladder);
		(void)fyai_chrome_update(ctx, sess->surface,
			&(struct fyai_chrome_spec){
				.title = sess->title ? sess->title : "**shell**",
				.command = sess->command,
				.mark = FYAI_UI_MARK_RUNNING,
			}, &animation_ms);
		fyai_chrome_body(ctx, sess->surface);
		el = fyai_ctx_loop(ctx);
		if (el && animation_ms) {
			rc = fyai_event_add_timer(el, animation_ms, animation_ms,
					  fyai_shell_session_animate, sess,
					  &sess->animation);
			if (rc)
				fyai_warning(ctx, "shell session indicator animation "
					     "timer could not start");
		}
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

/* Apply a layout grant: size the pseudo-terminal and the grid. */
static void fyai_shell_session_apply_grant(void *owner, int rows, int cols)
{
	struct fyai_shell_session *sess = owner;
	struct fy_generic_builder *gb;
	int have_rows = 0, have_cols = 0;

	if ((!sess->surface && !sess->inline_block) || cols < 1)
		return;
	fyai_terminal_view_size(sess->view, &have_rows, &have_cols);
	if (rows < 1)
		rows = sess->rows;
	if (cols == have_cols && rows == have_rows)
		return;

	/* The child owns the PTY and applies session resize requests. */
	if (!sess->job || !sess->job->conn || sess->exited)
		goto local;
	if (rows == sess->resize_rows && cols == sess->resize_cols)
		return;
	gb = fyai_ctx_transient_gb(sess->ctx);
	if (!gb)
		return;
	if (!jsonrpc_notify(sess->job->conn, "tty/resize",
			     fy_gb_mapping(gb, "rows", (long long)rows,
					   "cols", (long long)cols))) {
		sess->resize_rows = rows;
		sess->resize_cols = cols;
	}
	return;
local:
	fyai_terminal_view_resize(sess->view, rows, cols);
	(void)fyai_ui_surface_resize(sess->surface, rows, cols);
	fyai_workpane_grid_resized(sess->ctx->workpane, sess->surface, rows,
				   cols);
}

/*
 * A program that prints once and ends, such as ls, reads the size of its
 * terminal when it starts. The tile of the session is registered already, so
 * the layout solved now gives it the size it will have, and the program starts
 * at that size. A size the call asks for wins.
 */
static void fyai_shell_session_start_size(struct fyai_tool_job *job,
					  fy_generic args)
{
	struct fyai_shell_session *sess = job->session;
	int rows, cols;

	/* A block of the transcript has the size it opened with. */
	if (sess && sess->inline_block) {
		fyai_terminal_view_size(sess->view, &rows, &cols);
		job->start_rows = rows;
		job->start_cols = cols;
		return;
	}
	if (!sess || !sess->surface || fy_get(args, "rows", 0LL) > 0 ||
	    fy_get(args, "cols", 0LL) > 0 || !fyai_ui_layout_now(sess->ctx))
		return;
	rows = fyai_ui_surface_granted_rows(sess->ctx, sess->surface);
	cols = fyai_ui_surface_granted_cols(sess->ctx, sess->surface);
	if (rows < 1 || cols < 1)
		return;
	job->start_rows = rows;
	job->start_cols = cols;
	/* The grid and the view take the size before the program writes. */
	fyai_shell_session_apply_grant(sess, rows, cols);
}

/* Chrome only: focus changes nothing about the size of this session. */
static void fyai_shell_session_focus_changed(void *owner, bool focused)
{
	struct fyai_shell_session *sess = owner;

	(void)sess;
	(void)focused;
}

/* Draw the program, or blank the grid and leave the head standing. */
static void fyai_shell_session_present(void *owner,
				       enum fyai_workpane_present p)
{
	struct fyai_shell_session *sess = owner;

	if (!sess->surface)
		return;
	sess->present = p;
	if (p == FYAI_WORKPANE_PRESENT_HEAD ||
	    p == FYAI_WORKPANE_PRESENT_HIDDEN) {
		(void)fyai_ui_surface_clear(sess->surface);
	} else {
		/* Draw the whole screen again at the size it now has. */
		fyai_terminal_view_damage_all(sess->view);
		(void)fyai_ui_surface_publish(sess->surface, sess->view);
	}
	fyai_ui_wake(sess->ctx);
}

static const struct fyai_workpane_tile_ops fyai_shell_session_tile_ops = {
	.apply_grant = fyai_shell_session_apply_grant,
	.focus_changed = fyai_shell_session_focus_changed,
	.set_presentation = fyai_shell_session_present,
	.repaint_head = fyai_shell_session_head_paint,
};

static void fyai_shell_session_resized(struct fyai_shell_session *sess,
				       int rows, int cols)
{
	if (!sess || (!sess->surface && !sess->inline_block) || rows <= 0 ||
	    cols <= 0)
		return;
	fyai_terminal_view_resize(sess->view, rows, cols);
	if (sess->inline_block) {
		fyai_terminal_view_damage_all(sess->view);
		if (rows == sess->resize_rows && cols == sess->resize_cols) {
			sess->resize_rows = 0;
			sess->resize_cols = 0;
		}
		fyai_ui_wake(sess->ctx);
		return;
	}
	(void)fyai_ui_surface_resize(sess->surface, rows, cols);
	/* An acknowledgement records the grid; it schedules no new request. */
	fyai_workpane_grid_resized(sess->ctx->workpane, sess->surface, rows,
				   cols);
	/* Publish the complete resized grid before the next frame. */
	(void)fyai_ui_surface_publish(sess->surface, sess->view);
	if (rows == sess->resize_rows && cols == sess->resize_cols) {
		sess->resize_rows = 0;
		sess->resize_cols = 0;
	}
	fyai_ui_wake(sess->ctx);
}

/* Show what the program has drawn since the last look. */
static void fyai_shell_session_refresh(struct fyai_shell_session *sess)
{
	if (!sess || !sess->surface)
		return;
	if (fyai_ui_surface_publish(sess->surface, sess->view) > 0)
		fyai_ui_wake(sess->ctx);
}

char *fyai_shell_session_ended_text(struct fyai_ctx *ctx, const char *name,
				    bool *knownp)
{
	struct fyai_shell_session *sess = fyai_shell_session_find(ctx, name);

	*knownp = sess != NULL;
	if (!sess || !sess->exited)
		return NULL;
	return strdup(fy_sprintfa("[shell '%s' ended: status %d]", name,
				  sess->signal ? 128 + sess->signal :
						 sess->exit_code));
}

/* Whether the queued wait report of session @name still has a live owner. */
bool fyai_event_session_live(struct fyai_ctx *ctx, const char *name)
{
	struct fyai_shell_session *sess;

	sess = fyai_shell_session_find(ctx, name);
	return sess && !sess->exited && !sess->closing && sess->job;
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
	if (!text)
		return;
	/*
	 * Name the session: the report reaches the model after this poll,
	 * and the session may have ended by then. A stale report names a
	 * session with no live owner and is dropped instead of submitted.
	 */
	if (fyai_event_inject_owned(sess->ctx, text, FYAI_EVENT_OWNER_SESSION,
				    strdup(sess->name))) {
		fyai_warning(sess->ctx,
			     "the shell '%s' asked for input, which was lost",
			     sess->name);
	}
}

/* Poll for transitions into and out of an input wait. */
static enum fyai_event_action fyai_shell_session_wait_poll(
					const struct fyai_event *ev)
{
	struct fyai_shell_session *sess = ev->userdata;
	bool wants;

	if (sess->exited || sess->closing)
		return FYAIEA_CONTINUE;
	/*
	 * A program held at a gate is one that is being typed at: its answer
	 * is on its way. Asking the model to answer it would ask for what it
	 * has already sent.
	 */
	if (sess->feeding)
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
	/*
	 * A user-owned session is read by the user on its screen. The model
	 * must not get its output, and a turn started for it would take the
	 * screen from the user.
	 */
	if (sess->user_owned)
		return;
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

/* The columns of a terminal drawn in the transcript: the render width less
 * the indent of tool output. */
static int fyai_shell_inline_cols(struct fyai_ctx *ctx)
{
	int width = ctx->cfg->render_width, rows = 0;
	int indent = (int)strlen(markdown_tool_output_indent(ctx->cfg));

	/* Without a render width the transcript is as wide as the terminal. */
	if (width < 1 && (fyai_ui_size(ctx, &width, &rows) || width < 1))
		width = FYAI_TTY_COLS_DEFAULT;

	return width - indent > 10 ? width - indent : 10;
}

/*
 * The length of the @len bytes of @screen without the rows at its end that
 * hold only the indent of tool output: what the transcript keeps ends at the
 * last row the program drew.
 */
static size_t fyai_inline_screen_trim(struct fyai_ctx *ctx, const char *screen,
				      size_t len)
{
	const char *indent = markdown_tool_output_indent(ctx->cfg);
	size_t ilen = strlen(indent);

	while (len >= ilen + 1 && screen[len - 1] == '\n' &&
	       !memcmp(screen + len - 1 - ilen, indent, ilen) &&
	       (len - 1 - ilen == 0 || screen[len - 2 - ilen] == '\n'))
		len -= ilen + 1;
	return len;
}

/* Draw the screen of an inline session in its block of the transcript, or
 * with @done present it there with its outcome. */
static void fyai_shell_session_inline_paint(struct fyai_shell_session *sess,
					    bool done)
{
	struct response_buffer screen = {0};
	const char *indent = markdown_tool_output_indent(sess->ctx->cfg);
	char *cause = NULL;
	char edge[256];
	int first, last, rc;
	bool ok = true;

	/* The paint draws every row: the damage is taken. */
	(void)fyai_terminal_view_take_damage(sess->view, &first, &last);
	if (!done && fyai_ui_inline_focused(sess->ctx) == (uintptr_t)sess)
		indent = fyai_ui_inline_margin(sess->ctx, indent, edge,
					       sizeof(edge));
	rc = fyai_terminal_view_rows_sgr(sess->view, indent, !done, &screen);
	fyai_error_check(sess->ctx, !rc, out,
			 "shell: cannot draw the screen of session '%s'",
			 sess->name ? sess->name : "");
	if (done) {
		cause = fyai_shell_session_cause(sess, &ok);
		screen.len = fyai_inline_screen_trim(sess->ctx, screen.data,
						     screen.len);
	}
	if (fyai_ui_inline_terminal(sess->ctx, (uintptr_t)sess,
				    sess->title ? sess->title : "**shell**",
				    sess->command, screen.data, screen.len,
				    sess->rows, sess->animation_frame, done,
				    ok))
		fyai_warning(sess->ctx,
			     "shell: cannot draw session '%s' in the transcript",
			     sess->name ? sess->name : "");
out:
	free(cause);
	free(screen.data);
}

/* A terminal drawn in the transcript follows the width of the transcript. */
static void fyai_shell_session_inline_follow(struct fyai_shell_session *sess)
{
	int rows = 0, cols = 0, want;

	if (sess->exited)
		return;
	fyai_terminal_view_size(sess->view, &rows, &cols);
	want = fyai_shell_inline_cols(sess->ctx);
	if (want != cols)
		fyai_shell_session_apply_grant(sess, rows, want);
}

/* A program that ended may no longer be focused or zoomed. */
static void fyai_surface_retire_zoom(struct fyai_ctx *ctx,
				     struct fytim_surface *surface)
{
	if (!ctx || !surface)
		return;
	fyai_workpane_tile_set_selectable(ctx->workpane, surface, false);
	if (fyai_workpane_focused(ctx->workpane) == surface)
		fyai_workpane_clear_focus(ctx->workpane);
	if (fyai_workpane_zoomed(ctx->workpane) == surface)
		fyai_workpane_clear_zoom(ctx->workpane);
}

/*
 * Commit the completed session to the transcript and terminal scrollback. A
 * bang command is not a part of the conversation: it records nothing and
 * commits nothing, and its tile stays with the outcome until the user
 * dismisses it, and keeps the keys if it held them. A full-screen program leaves
 * nothing to read, and its tile goes at once.
 */
static void fyai_shell_session_display_finish(struct fyai_shell_session *sess)
{
	char *cause;
	char *text;
	size_t len;
	bool ok;

	if (!sess || sess->recorded)
		return;
	sess->recorded = true;
	if (!sess->keep_tile) {
		cause = fyai_shell_session_cause(sess, &ok);
		text = fyai_terminal_view_read(sess->view, FYAITR_ALL, NULL,
					       &len);
		if (fyai_record_shell_screen(sess->ctx, sess->description,
					     sess->command, text, ok, cause))
			fyai_warning(sess->ctx,
				     "shell: could not record the session '%s'",
				     sess->name ? sess->name : "");
		free(text);
		free(cause);
	}
	if (sess->inline_block) {
		if (sess->animation) {
			fyai_event_source_remove(sess->animation);
			sess->animation = NULL;
		}
		fyai_shell_session_inline_paint(sess, true);
		sess->inline_block = false;
		fyai_ui_wake(sess->ctx);
		return;
	}

	if (!sess->surface)
		return;

	if (sess->animation) {
		fyai_event_source_remove(sess->animation);
		sess->animation = NULL;
	}
	fyai_shell_session_refresh(sess);
	cause = fyai_shell_session_cause(sess, &ok);
	(void)fyai_chrome_update(sess->ctx, sess->surface,
		&(struct fyai_chrome_spec){
			.title = sess->title ? sess->title : "**shell**",
			.command = sess->command,
			.cause = cause,
			.mark = ok ? FYAI_UI_MARK_OK : FYAI_UI_MARK_FAILED,
		}, NULL);
	free(cause);
	/* A full-screen program leaves nothing to read, and a tile the user
	 * closed is not wanted: its tile goes. */
	if (sess->keep_tile &&
	    (sess->close_on_exit ||
	     fyai_terminal_view_used_alt_screen(sess->view))) {
		fyai_surface_retire_zoom(sess->ctx, sess->surface);
		fyai_ui_surface_close(sess->ctx, sess->surface);
		sess->surface = NULL;
		fyai_ui_wake(sess->ctx);
		return;
	}
	/* The tile keeps the focus it had: Escape closes it there, which the
	 * hint of the status row says. */
	if (sess->keep_tile) {
		if (fyai_workpane_focused(sess->ctx->workpane) == sess->surface)
			fyai_chrome_focus(sess->ctx, sess->surface, true);
		fyai_ui_wake(sess->ctx);
		return;
	}
	fyai_surface_retire_zoom(sess->ctx, sess->surface);
	fyai_ui_surface_commit(sess->ctx, sess->surface);
	sess->surface = NULL;
	fyai_ui_wake(sess->ctx);
}

/* Tell the owner of the program that it ended, one time. */
static void fyai_shell_session_notify_exit(struct fyai_shell_session *sess,
					   int exit_code, int signal)
{
	fyai_tools_exit_fn fn;
	void *data;

	fn = sess->on_exit;
	data = sess->on_exit_data;
	sess->on_exit = NULL;
	sess->on_exit_data = NULL;
	if (fn)
		fn(data, exit_code, signal);
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
	/* Remove the obsolete input waiter. */
	if (sess->waiter) {
		fyai_event_source_remove(sess->waiter);
		sess->waiter = NULL;
	}
	/*
	 * The exited program answers no question: drop the wait reports it
	 * queued, or a later turn asks the model about a program that ended.
	 */
	fyai_events_drop_session(sess->ctx, sess->name);
	fyai_shell_session_notify_exit(sess, exit_code, signal);
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
	/* A session that goes before its program ends still reports an end. */
	if (!sess->exited)
		fyai_shell_session_notify_exit(sess, -1, SIGKILL);
	/* Commit output displayed before teardown. */
	fyai_shell_session_display_finish(sess);
	if (sess->inline_block)
		fyai_ui_inline_drop(sess->ctx, (uintptr_t)sess);
	/* A finished bang session keeps its tile until now. */
	if (sess->surface) {
		fyai_surface_retire_zoom(sess->ctx, sess->surface);
		fyai_ui_surface_close(sess->ctx, sess->surface);
		sess->surface = NULL;
	}
	if (sess->animation)
		fyai_event_source_remove(sess->animation);
	if (sess->idle)
		fyai_event_source_remove(sess->idle);
	if (sess->waiter)
		fyai_event_source_remove(sess->waiter);
	fyai_terminal_view_destroy(sess->view);
	free(sess->name);
	free(sess->command);
	free(sess->branch);
	free(sess->title);
	free(sess->description);
	free(sess);
}

/* Drop inherited session records without touching parent-owned sessions. */
static void fyai_shell_sessions_abandon(struct fyai_ctx *ctx)
{
	struct fyai_shell_session *sess, *next;

	if (!ctx)
		return;
	for (sess = ctx->shell_sessions; sess; sess = next) {
		next = sess->next;
		if (sess->job)
			sess->job->session = NULL;
		sess->job = NULL;
		sess->surface = NULL;
		sess->idle = NULL;
		sess->waiter = NULL;
		sess->animation = NULL;
		/* Exit callbacks belong to the parent invocation. */
		sess->on_exit = NULL;
		sess->on_exit_data = NULL;
		/* Only the parent records its session. */
		sess->recorded = true;
		fyai_shell_session_destroy(sess);
	}
	ctx->shell_sessions = NULL;
}

static void fyai_shell_session_release_one(struct fyai_shell_session *sess,
						   bool force)
{
	struct fyai_tool_job *job = sess->job;

	/*
	 * The session answers no question after this: drop the wait reports
	 * it queued with it.
	 */
	fyai_events_drop_session(sess->ctx, sess->name);
	if (job) {
		job->session = NULL;
		sess->job = NULL;
		if (!job->reaped && job->pid > 0)
			(void)kill(-job->pid, force ? SIGKILL : SIGTERM);
		fyai_tool_job_discard(job);
	}
	fyai_shell_session_destroy(sess);
}

/* A finished bang session leaves the pane: its tile goes, and the session
 * with it. */
static void fyai_shell_session_dismiss(struct fyai_shell_session *sess)
{
	struct fyai_ctx *ctx = sess->ctx;
	struct fyai_shell_session **link;

	for (link = &ctx->shell_sessions; *link && *link != sess;
	     link = &(*link)->next)
		;
	if (*link)
		*link = sess->next;
	fyai_shell_session_release_one(sess, false);
	fyai_ui_wake(ctx);
}

void fyai_tools_display_closed(struct fyai_ctx *ctx)
{
	struct fyai_shell_session *sess;
	struct fyai_tool_job *job;

	if (!ctx)
		return;
	for (sess = ctx->shell_sessions; sess; sess = sess->next) {
		if (sess->animation) {
			fyai_event_source_remove(sess->animation);
			sess->animation = NULL;
		}
		sess->surface = NULL;
	}
	for (job = ctx->tool_jobs; job; job = job->next) {
		if (job->animation) {
			fyai_event_source_remove(job->animation);
			job->animation = NULL;
		}
		job->surface = NULL;
	}
}

/* End and release every session owned by this invocation. */
void fyai_shell_sessions_release(struct fyai_ctx *ctx, bool force)
{
	struct fyai_shell_session *sess;

	if (!ctx)
		return;
	/* Each session leaves the list before it goes, so what its release
	 * calls finds only sessions that live. */
	while ((sess = ctx->shell_sessions) != NULL) {
		ctx->shell_sessions = sess->next;
		fyai_shell_session_release_one(sess, force);
	}
}

/*
 * End and release every session owned by the turn. A shell the user opened
 * with a bang line is not the turn work: interrupting the turn must not
 * take it down.
 */
void fyai_shell_sessions_release_turn(struct fyai_ctx *ctx)
{
	struct fyai_shell_session *sess, **link;

	if (!ctx)
		return;
	link = &ctx->shell_sessions;
	while ((sess = *link) != NULL) {
		if (sess->user_owned) {
			link = &sess->next;
			continue;
		}
		*link = sess->next;
		fyai_shell_session_release_one(sess, true);
	}
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

/*
 * Wait for the session to be quiet for @idle_ms before its input is written,
 * and no longer than @max_ms whatever it does. Each byte the program writes
 * restarts the quiet time, so this waits out a program that is still starting
 * and returns at once for one that is stopped for input.
 */
static void fyai_shell_session_idle_gate(struct fyai_shell_session *sess,
					 long long idle_ms, long long max_ms)
{
	struct fyai_event_loop *el;
	fyai_event_ms_t deadline, quiet, owed, now;

	if (idle_ms <= 0 || max_ms <= 0 || sess->exited)
		return;
	el = fyai_ctx_loop(sess->ctx);
	if (!el)
		return;
	sess->feeding = true;
	now = fyai_event_now_ms();
	deadline = now + max_ms;
	/*
	 * A session that drew nothing did not settle, because it did not
	 * start. Wait for its start before you read the quiet time, so that
	 * the result of a call does not depend on the start time of the
	 * program. A program that draws nothing and waits for input did
	 * start, and a read of its terminal reports this.
	 */
	while (!sess->drew && !sess->exited && now < deadline &&
	       !fyai_process_reads_stdin(sess->pid)) {
		owed = deadline - now;
		if (owed > FYAI_SHELL_FIRST_DRAW_POLL_MS)
			owed = FYAI_SHELL_FIRST_DRAW_POLL_MS;
		(void)fyai_event_sleep(el, owed);
		now = fyai_event_now_ms();
	}
	for (;;) {
		quiet = now - sess->quiet_since;
		if (quiet >= idle_ms || sess->exited || now >= deadline)
			break;
		/*
		 * Sleep what the quiet time still owes, or what is left before
		 * the gate opens anyway. Output during the sleep moves the
		 * quiet time, and the next round waits out the rest of it.
		 */
		owed = idle_ms - quiet;
		if (owed > deadline - now)
			owed = deadline - now;
		(void)fyai_event_sleep(el, owed);
		now = fyai_event_now_ms();
	}
	sess->feeding = false;
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
	long long idle, max_wait;
	char *err;

	*okp = false;
	sess = fyai_shell_session_of(ctx, args, &err);
	if (!sess) {
		fyai_error_check(ctx, err, out,
				 "shell: could not report the missing session");
		return err;
	}

	/*
	 * Read only a program that has settled on its terminal. A session that
	 * has just opened has drawn nothing yet, and the reading of it must
	 * not depend on how fast the program started.
	 */
	idle = fy_get(args, "idle_trigger",
		      (long long)FYAI_SHELL_IDLE_TRIGGER_MS);
	if (idle > FYAI_SHELL_IDLE_WAIT_MAX_MS)
		idle = FYAI_SHELL_IDLE_WAIT_MAX_MS;
	max_wait = fy_get(args, "max_wait",
			  (long long)FYAI_SHELL_IDLE_MAX_WAIT_MS);
	if (max_wait > FYAI_SHELL_IDLE_WAIT_MAX_MS)
		max_wait = FYAI_SHELL_IDLE_WAIT_MAX_MS;
	fyai_shell_session_idle_gate(sess, idle, max_wait);

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
	long long wait, idle, max_wait;
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
	/* Type only into a program that has settled on its terminal. */
	idle = fy_get(args, "idle_trigger",
		      (long long)FYAI_SHELL_IDLE_TRIGGER_MS);
	if (idle > FYAI_SHELL_IDLE_WAIT_MAX_MS)
		idle = FYAI_SHELL_IDLE_WAIT_MAX_MS;
	max_wait = fy_get(args, "max_wait",
			  (long long)FYAI_SHELL_IDLE_MAX_WAIT_MS);
	if (max_wait > FYAI_SHELL_IDLE_WAIT_MAX_MS)
		max_wait = FYAI_SHELL_IDLE_WAIT_MAX_MS;
	fyai_shell_session_idle_gate(sess, idle, max_wait);

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
	if (sess->user_owned) {
		return strdup(fy_sprintfa(
			"tool error: the shell '%s' belongs to the user and cannot "
			"be closed by the model", sess->name));
	}

	force = fy_get(args, "force", false);
	if (!sess->exited) {
		fyai_shell_session_close(sess, force);
		/* Allow the program to exit before reporting its result. */
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

/*
 * Cancel background work by name. The name must be unique among the waits,
 * the running sub-agents and the open sessions unless @kind says which.
 */
static char *fyai_cancel_tool(struct fyai_ctx *ctx, fy_generic args, bool *okp)
{
	struct fyai_tool_job *job;
	fy_generic name_v, kind_v;
	const char *name, *kind;
	bool is_wait, is_agent, is_shell;
	char *result;

	*okp = false;
	name_v = fy_get(args, "name", fy_invalid);
	kind_v = fy_get(args, "kind", fy_invalid);
	name = fy_castp(&name_v, "");
	kind = fy_is_string(kind_v) ? fy_castp(&kind_v, "") : "";
	if (!*name)
		return strdup("tool error: give the name to cancel");

	job = fyai_agent_job_named(ctx, name);
	is_agent = job && (!*kind || !strcmp(kind, "agent"));
	is_shell = fyai_shell_session_find(ctx, name) &&
		   (!*kind || !strcmp(kind, "shell"));
	is_wait = (!*kind || !strcmp(kind, "wait")) &&
		  fyai_wait_exists(ctx, name);
	if (is_wait + is_agent + is_shell > 1)
		return strdup(fy_sprintfa("tool error: '%s' names more than "
					  "one thing; say its kind", name));
	if (is_agent) {
		fyai_tool_job_cancel(job);
		*okp = true;
		return strdup(fy_sprintfa("[agent '%s' was asked to stop]",
					  name));
	}
	if (is_shell) {
		result = fyai_shell_close_tool(ctx,
			fy_mapping(ctx->transient_gb, "name", name,
				   "force", true), okp);
		return result;
	}
	if (is_wait && fyai_wait_cancel(ctx, name)) {
		*okp = true;
		return strdup(fy_sprintfa("[wait '%s' cancelled]", name));
	}
	return strdup(fy_sprintfa("tool error: nothing named '%s' is running"
				  "%s%s; use list to see what is open", name,
				  *kind ? " as a " : "", kind));
}

int fyai_tool_call_file_effect(struct fyai_ctx *ctx, fy_generic tool_call)
{
	const char *name = fyai_tool_call_name(ctx, tool_call);
	const struct fyai_tool_def *def = fyai_tool_find(name);

	if (def)
		return def->effect;
	/* An MCP server can do anything. */
	return fyai_mcp_tool_name(name) ? FYAI_TOOL_EFFECT_PROCESS :
					  FYAI_TOOL_EFFECT_NONE;
}

bool fyai_tool_call_parallel_eligible(struct fyai_ctx *ctx,
				      fy_generic tool_call)
{
	const char *name = fyai_tool_call_name(ctx, tool_call);
	const struct fyai_tool_def *def = fyai_tool_find(name);

	if (fyai_mcp_tool_name(name))
		return false;
	if (!def)
		return true;
	if (def->flags & FYAI_TOOL_PARENT)
		return false;
	return !def->in_parent ||
	       !def->in_parent(fyai_tool_call_args(ctx, tool_call));
}

/* Size the sub-agent's terminal to the grant it received. */
static void fyai_agent_apply_grant(void *owner, int rows, int cols)
{
	struct fyai_tool_job *job = owner;
	struct winsize ws = {};

	if ((!job->surface && !job->inline_block) || cols < 1)
		return;
	if (rows < 1)
		rows = job->pty_rows;
	if (cols == job->pty_cols && rows == job->pty_rows)
		return;

	job->pty_cols = cols;
	job->pty_rows = rows;
	ws.ws_row = (unsigned short)rows;
	ws.ws_col = (unsigned short)cols;
	if (job->pty >= 0)
		(void)ioctl(job->pty, TIOCSWINSZ, &ws);
	fyai_terminal_view_resize(job->view, rows, cols);
	if (job->inline_block) {
		fyai_terminal_view_damage_all(job->view);
		return;
	}
	(void)fyai_ui_surface_resize(job->surface, rows, cols);
	fyai_workpane_grid_resized(job->ctx->workpane, job->surface, rows, cols);
}

static void fyai_agent_focus_changed(void *owner, bool focused)
{
	(void)owner;
	(void)focused;
}

static void fyai_agent_present(void *owner, enum fyai_workpane_present p)
{
	struct fyai_tool_job *job = owner;

	if (!job->surface)
		return;
	job->present = p;
	if (p == FYAI_WORKPANE_PRESENT_HEAD ||
	    p == FYAI_WORKPANE_PRESENT_HIDDEN) {
		(void)fyai_ui_surface_clear(job->surface);
	} else {
		fyai_terminal_view_damage_all(job->view);
		(void)fyai_ui_surface_publish(job->surface, job->view);
	}
	fyai_ui_wake(job->ctx);
}

static const struct fyai_workpane_tile_ops fyai_agent_tile_ops = {
	.apply_grant = fyai_agent_apply_grant,
	.focus_changed = fyai_agent_focus_changed,
	.set_presentation = fyai_agent_present,
	.repaint_head = fyai_agent_head_repaint,
};

/* Draw the screen of an inline sub-agent in its block of the transcript, or
 * with @done present it there with the outcome @ok. */
static void fyai_agent_inline_paint(struct fyai_tool_job *job, bool done,
				    bool ok)
{
	struct response_buffer screen = {0};
	const char *indent;
	char edge[256];
	int first, last, rc;

	if (!job->view)
		return;
	(void)fyai_terminal_view_take_damage(job->view, &first, &last);
	indent = markdown_tool_output_indent(job->ctx->cfg);
	if (!done && fyai_ui_inline_focused(job->ctx) == (uintptr_t)job)
		indent = fyai_ui_inline_margin(job->ctx, indent, edge,
					       sizeof(edge));
	rc = fyai_terminal_view_rows_sgr(job->view, indent, false, &screen);
	fyai_error_check(job->ctx, !rc, out,
			 "agent: cannot draw the screen of a sub-agent");
	if (done)
		screen.len = fyai_inline_screen_trim(job->ctx, screen.data,
						     screen.len);
	if (fyai_ui_inline_terminal(job->ctx, (uintptr_t)job,
				    job->title ? job->title : "**agent**",
				    NULL, screen.data, screen.len,
				    job->pty_rows, job->animation_frame, done,
				    ok))
		fyai_warning(job->ctx,
			     "agent: cannot draw a sub-agent in the transcript");
out:
	free(screen.data);
}

/* A sub-agent drawn in the transcript follows the width of the transcript. */
static void fyai_agent_inline_follow(struct fyai_tool_job *job)
{
	int want = fyai_shell_inline_cols(job->ctx);

	if (want != job->pty_cols)
		fyai_agent_apply_grant(job, job->pty_rows, want);
}

/* Publish what every live tile has drawn since the last frame. */
void fyai_tool_surfaces_publish(struct fyai_ctx *ctx)
{
	struct fyai_shell_session *sess;
	struct fyai_tool_job *job;

	if (!ctx)
		return;
	for (sess = ctx->shell_sessions; sess; sess = sess->next) {
		if (sess->inline_block) {
			fyai_shell_session_inline_follow(sess);
			if (fyai_terminal_view_dirty(sess->view))
				fyai_shell_session_inline_paint(sess, false);
			continue;
		}
		if (sess->surface && fyai_tile_draws_program(sess->present) &&
		    fyai_ui_surface_publish(sess->surface, sess->view) > 0)
			fyai_ui_wake(ctx);
	}
	for (job = ctx->tool_jobs; job; job = job->next) {
		if (job->inline_block) {
			fyai_agent_inline_follow(job);
			if (fyai_terminal_view_dirty(job->view))
				fyai_agent_inline_paint(job, false, true);
			continue;
		}
		if (job->surface && fyai_tile_draws_program(job->present) &&
		    fyai_ui_surface_publish(job->surface, job->view) > 0)
			fyai_ui_wake(ctx);
	}
}

bool fyai_tool_surfaces_active(const struct fyai_ctx *ctx)
{
	const struct fyai_shell_session *sess;
	const struct fyai_tool_job *job;

	if (!ctx)
		return false;
	for (sess = ctx->shell_sessions; sess; sess = sess->next)
		if (sess->surface)
			return true;
	for (job = ctx->tool_jobs; job; job = job->next)
		if (job->surface)
			return true;
	return false;
}

/* Show what the sub-agent has drawn on its terminal since the last look. */
static void fyai_agent_view_refresh(struct fyai_tool_job *job)
{
	if (!job->surface || !job->view)
		return;
	if (fyai_ui_surface_publish(job->surface, job->view) > 0)
		fyai_ui_wake(job->ctx);
}

/*
 * Build the title of a sub-agent tile: the call title, then the branch leaf,
 * the model, and the execution id from the agent registry. Leave out a field
 * that the registry does not have. The caller frees the result.
 */
static char *fyai_agent_head_title(struct fyai_tool_job *job)
{
	const char *base, *end, *leaf, *model, *id;
	const char *leaf_part, *model_part, *text;
	long long execution, started;
	fy_generic call_args, call_name;
	char *title;

	base = job->title ? job->title : "**agent**";
	while (*base == '\n' || *base == '\r' ||
	       *base == ' ' || *base == '\t')
		base++;
	end = base + strlen(base);
	while (end > base && (end[-1] == '\n' || end[-1] == '\r'))
		end--;
	(void)fyai_agents_branch_identity(job->ctx, job->branch, &model,
					  &execution, &started);
	leaf = job->branch ? strstr(job->branch, FYAI_BRANCH_AGENT_PREFIX) : NULL;
	leaf = leaf ? leaf + sizeof(FYAI_BRANCH_AGENT_PREFIX) - 1 : job->branch;
	/* The call title already names the agent. */
	if (!fy_str_empty(leaf) && job->agent && fy_is_valid(job->call)) {
		call_args = fyai_tool_call_args(job->ctx, job->call);
		call_name = fy_get(call_args, "name");
		if (fy_equal(call_name, leaf))
			leaf = NULL;
	}
	id = execution > 0 ? fy_sprintfa(" #%lld", execution) : "";
	leaf_part = fy_str_empty(leaf) ? "" : fy_sprintfa(" `%s`", leaf);
	model_part = fy_str_empty(model) ? "" : fy_sprintfa(" `%s`", model);
	text = fy_sprintfa("%.*s%s%s%s\n", (int)(end - base), base,
			   leaf_part, model_part, id);
	title = strdup(text);
	fyai_error_check(job->ctx, title, err_out,
			 "cannot format the tile title of agent %s",
			 job->branch ? job->branch : "");
	return title;

err_out:
	return NULL;
}

/* Paint the title of a live sub-agent tile with the running mark. */
static void fyai_agent_head_paint(struct fyai_tool_job *job)
{
	const char *right, *model;
	char elapsed[24];
	long long execution, started;
	char *title;

	/* The elapsed time of a running agent stands at the right edge. */
	(void)fyai_agents_branch_identity(job->ctx, job->branch, &model,
					  &execution, &started);
	fyai_event_elapsed_format(elapsed, sizeof(elapsed), started);
	right = fy_sprintfa("%s%s", job->overdue ? "overdue " : "",
			    elapsed[0] == ' ' ? elapsed + 1 : elapsed);
	title = fyai_agent_head_title(job);
	(void)fyai_chrome_update(job->ctx, job->surface,
		&(struct fyai_chrome_spec){
			.title = title ? title :
				 job->title ? job->title : "**agent**",
			.right = *right ? right : NULL,
			.mark = FYAI_UI_MARK_RUNNING,
			.frame = job->animation_frame,
		}, NULL);
	free(title);
	fyai_ui_wake(job->ctx);
}

/* Repaint an active agent header after its width changes. */
static void fyai_agent_head_repaint(void *owner)
{
	struct fyai_tool_job *job = owner;
	char *title;

	if (job->surface && !job->done)
		fyai_agent_head_paint(job);
	else if (job->surface && job->btw_panel) {
		title = fyai_agent_head_title(job);
		(void)fyai_chrome_update(job->ctx, job->surface,
			&(struct fyai_chrome_spec){
				.title = title ? title : "**btw**",
				.mark = job->result_ok && !job->failed ?
					FYAI_UI_MARK_OK : FYAI_UI_MARK_FAILED,
			}, NULL);
		free(title);
	}
}

/* Advance the state mark on the title of a live sub-agent terminal. */
static enum fyai_event_action
fyai_agent_view_animate(const struct fyai_event *ev)
{
	struct fyai_tool_job *job = ev->userdata;

	if (job->inline_block && !job->done) {
		job->animation_frame++;
		fyai_agent_inline_paint(job, false, true);
		return FYAIEA_CONTINUE;
	}
	if (!job->surface || job->done)
		return FYAIEA_CONTINUE;
	job->animation_frame++;
	fyai_agent_head_paint(job);
	return FYAIEA_CONTINUE;
}

/* Write the reply of the view to the terminal of the sub-agent. */
static void fyai_agent_view_reply(const char *data, size_t len, void *user)
{
	struct fyai_tool_job *job = user;

	fyai_terminal_reply_write(job->pty, data, len);
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
		/* EIO means that the last slave closed and the sub-agent exited. */
		fyai_agent_view_refresh(job);
		if (job->ptysrc) {
			fyai_event_source_remove(job->ptysrc);
			job->ptysrc = NULL;
		}
		return FYAIEA_CONTINUE;
	}
}

/* Whether the queued wait report of the sub-agent on @branch is still live. */
bool fyai_event_agent_live(struct fyai_ctx *ctx, const char *branch)
{
	struct fyai_tool_job *job;

	if (!ctx || fy_str_empty(branch))
		return false;
	for (job = ctx->tool_jobs; job; job = job->next) {
		if (!job->agent || job->done || !job->branch)
			continue;
		if (!strcmp(job->branch, branch))
			return true;
	}
	return false;
}

/* Queue a sub-agent's new input request for the model. */
static enum fyai_event_action fyai_agent_wait_poll(const struct fyai_event *ev)
{
	struct fyai_tool_job *job = ev->userdata;
	const char *who;
	char *prompt = NULL;
	char *text;
	char *owner = NULL;
	bool wants;

	if (job->done || !job->branch)
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
	if (!text)
		return FYAIEA_CONTINUE;
	/*
	 * Name the branch: the report reaches the model after this poll,
	 * and the sub-agent may have ended by then. A stale report names a
	 * branch with no live owner and is dropped instead of submitted.
	 */
	owner = strdup(job->branch);
	if (!owner) {
		free(text);
		fyai_warning(job->ctx,
			     "the sub-agent '%s' asked for input, which was lost",
			     who);
		return FYAIEA_CONTINUE;
	}
	if (fyai_event_inject_owned(job->ctx, text,
					  FYAI_EVENT_OWNER_AGENT, owner)) {
		fyai_warning(job->ctx,
			     "the sub-agent '%s' asked for input, which was lost",
			     who);
	}
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
	unsigned int animation_ms;
	int flags, rc;

	animation_ms = 0;
	if (job->pty < 0)
		return 0;

	flags = fcntl(job->pty, F_GETFL, 0);
	if (flags >= 0)
		(void)fcntl(job->pty, F_SETFL, flags | O_NONBLOCK);

	job->view = fyai_terminal_view_create(ctx, job->pty_rows,
					      job->pty_cols, 0);
	if (!job->view)
		return -1;
	if (fyai_terminal_view_set_history(job->view,
					   ctx->cfg->work_history_rows))
		return -1;
	fyai_terminal_view_reply_cb(job->view, fyai_agent_view_reply, job);
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

	/* A delegation of the model draws its screen at the call in the
	 * transcript; a side question keeps its tile. */
	if (!job->btw && fyai_ui_tools_inline(ctx)) {
		job->inline_block = true;
		fyai_inline_animation_arm(ctx, fyai_agent_view_animate, job,
					  &job->animation);
		fyai_terminal_view_damage_all(job->view);
		fyai_ui_wake(ctx);
		return 0;
	}
	job->surface = fyai_ui_surface_open(ctx, job->pty_rows, job->pty_cols);
	if (job->surface) {
		/* A sub-agent keeps the height it opened with, whatever it
		 * has drawn: two lines of output is not a two-row tile. */
		(void)fyai_workpane_register(ctx->workpane, job->surface,
					     FYAI_WORKPANE_TILE_AGENT, job,
					     &fyai_agent_tile_ops,
					     job->pty_rows, 0);
		fyai_workpane_tile_set_ladder(ctx->workpane, job->surface,
					      &fyai_tile_ladder);
		(void)fyai_chrome_update(ctx, job->surface,
			&(struct fyai_chrome_spec){
				.title = job->title ? job->title : "**agent**",
				.mark = FYAI_UI_MARK_RUNNING,
			}, &animation_ms);
		fyai_chrome_body(ctx, job->surface);
		if (animation_ms) {
			rc = fyai_event_add_timer(el, animation_ms, animation_ms,
					  fyai_agent_view_animate, job,
					  &job->animation);
			if (rc)
				fyai_warning(ctx, "agent indicator animation timer "
					     "could not start");
		}
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
	char *title;

	if (job->animation) {
		fyai_event_source_remove(job->animation);
		job->animation = NULL;
	}
	if (job->waiter) {
		fyai_event_source_remove(job->waiter);
		job->waiter = NULL;
	}
	if (job->ptysrc) {
		fyai_event_source_remove(job->ptysrc);
		job->ptysrc = NULL;
	}
	if (job->inline_block) {
		fyai_agent_inline_paint(job, true, ok);
		job->inline_block = false;
		fyai_ui_wake(job->ctx);
	}
	if (job->btw && !job->btw_panel) {
		if (job->surface) {
			fyai_ui_surface_close(job->ctx, job->surface);
			job->surface = NULL;
		}
		if (job->pty >= 0) {
			close(job->pty);
			job->pty = -1;
		}
		fyai_terminal_view_destroy(job->view);
		job->view = NULL;
		return;
	}
	if (job->surface) {
		fyai_agent_view_refresh(job);
		title = fyai_agent_head_title(job);
		(void)fyai_chrome_update(job->ctx, job->surface,
			&(struct fyai_chrome_spec){
				.title = title ? title :
					 job->title ? job->title : "**agent**",
				.cause = cause,
				.mark = ok ? FYAI_UI_MARK_OK :
					     FYAI_UI_MARK_FAILED,
			}, NULL);
		free(title);
		if (!job->btw_panel) {
			fyai_surface_retire_zoom(job->ctx, job->surface);
			fyai_ui_surface_commit(job->ctx, job->surface);
			job->surface = NULL;
		}
		fyai_ui_wake(job->ctx);
	}
	if (job->pty >= 0) {
		close(job->pty);
		job->pty = -1;
	}
	if (!job->btw_panel) {
		fyai_terminal_view_destroy(job->view);
		job->view = NULL;
	}
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
		fyai_tool_job_drop(&job->band_delay);
		job->elapsed_ms = fyai_event_now_ms() - job->started_ms;
		if (job->agent && job->timeout_ms &&
		    job->elapsed_ms >= job->timeout_ms)
			job->overdue = true;
		/* Remove the deadline before the job waits for its group. */
		fyai_tool_job_drop(&job->deadline);
		fyai_tool_job_drop(&job->hang_deadline);
	}
	if (!was_done && job->done && job->agent && job->branch) {
		/*
		 * The sub-agent answers no question now: drop the wait
		 * reports it queued, or a later turn asks the model about
		 * an agent that ended.
		 */
		fyai_events_drop_agent(job->ctx, job->branch);
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

static const int fyai_tool_child_signal_set[] = { SIGTERM, SIGHUP, SIGPIPE };

/* Block the signals fyai_tool_child_signals() arms, before any other
 * setup. A blocked signal queues; it does not take its default action. */
static void fyai_tool_child_signals_block(void)
{
	sigset_t mask;
	size_t i;

	sigemptyset(&mask);
	for (i = 0; i < ARRAY_SIZE(fyai_tool_child_signal_set); i++)
		sigaddset(&mask, fyai_tool_child_signal_set[i]);
	(void)sigprocmask(SIG_BLOCK, &mask, NULL);
}

static void fyai_tool_child_signals(struct fyai_ctx *ctx)
{
	struct fyai_event_loop *el;
	struct fyai_event_source *src;
	size_t i;
	int rc;

	el = fyai_ctx_loop(ctx);
	if (!el)
		return;
	for (i = 0; i < ARRAY_SIZE(fyai_tool_child_signal_set); i++) {
		src = NULL;
		rc = fyai_event_add_signal(el, fyai_tool_child_signal_set[i],
					   fyai_tool_child_signal, ctx, &src);
		(void)rc;
	}
}

int fyai_tool_child_exec_serve(struct fyai_ctx *ctx)
{
	/*
	 * A parent gives a child a terminal only from an interactive display,
	 * and the child opens a display of its own on it.
	 */
	ctx->cfg->interactive = ctx->cfg->agent_pty;
	fyai_diag_tracef("exec", "tool child of pid %ld%s", (long)getppid(),
			 ctx->cfg->agent_pty ? ", on a terminal" : "");
	fyai_tool_child_signals_block();
	fyai_tool_child_signals(ctx);
	if (!ctx->transient_gb && fyai_setup_transient_builder(ctx))
		return -1;
	fyai_tool_child_serve_loop(ctx);	/* never returns */
	return -1;
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

	who = job->branch ? strrchr(job->branch, '/') : NULL;
	return who && !strncmp(who + 1, FYAI_BRANCH_AGENT_PREFIX,
		strlen(FYAI_BRANCH_AGENT_PREFIX)) ?
		who + 1 + strlen(FYAI_BRANCH_AGENT_PREFIX) : "agent";
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
		who = fyai_agent_job_name(job);
		if (!strcmp(job->branch, name) || !strcmp(who, name))
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
	fy_generic question;
	fy_generic answer;
	const char *who, *origin;
	int rc;

	gb = fyai_ctx_transient_gb(job->ctx);
	who = fyai_agent_job_name(job);
	origin = fy_get(params, "branch", "");
	if (*origin && (!job->branch || strcmp(origin, job->branch)))
		who = origin;
	question = fy_get(params, "question", fy_invalid);
	answer = fyai_ask_user(job->ctx, fy_gb_mapping(gb, "question", question,
					"from", fy_value(gb, who),
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

/* The live job of this process that owns @branch, or NULL. */
static struct fyai_tool_job *fyai_tool_job_by_branch(struct fyai_ctx *ctx,
						      const char *branch)
{
	struct fyai_tool_job *job;

	for (job = ctx->tool_jobs; job; job = job->next) {
		if (job->branch && !strcmp(job->branch, branch) && !job->done)
			return job;
	}
	return NULL;
}

uint64_t fyai_tool_agent_transport_exec(struct fyai_ctx *ctx, const char *name)
{
	struct fyai_tool_job *job;
	size_t bl, nl;

	nl = strlen(name);
	for (job = ctx->tool_jobs; job; job = job->next) {
		if (!job->agent || job->done || !job->branch)
			continue;
		bl = strlen(job->branch);
		if (!strcmp(job->branch, name))
			return job->transport_exec;
		/* The name that the delegation gave, without the branch path. */
		if (bl > nl + 6 && !strcmp(job->branch + bl - nl, name) &&
		    !strncmp(job->branch + bl - nl - 6, "agent:", 6))
			return job->transport_exec;
	}
	return 0;
}

/* True while a job of this process owns @branch. */
static bool fyai_tool_job_branch_live(struct fyai_ctx *ctx, const char *branch)
{
	return fyai_tool_job_by_branch(ctx, branch) != NULL;
}

void fyai_tool_agent_title_refresh(struct fyai_ctx *ctx, const char *branch)
{
	struct fyai_tool_job *job;

	job = fyai_tool_job_by_branch(ctx, branch);
	if (job && job->surface)
		fyai_agent_head_paint(job);
}

/* Clear live parent-owned state after fork. */
static void fyai_ctx_fork_disown(struct fyai_ctx *ctx)
{
	if (!ctx)
		return;
	fyai_shell_sessions_abandon(ctx);	/* named shells of the parent */
	fyai_tool_jobs_abandon(ctx);		/* its running tool children */
	fyai_waits_abandon(ctx);		/* its named waits */
	fyai_agent_background_abandon(ctx);	/* its background sub-agents */
	fyai_events_release(ctx);		/* and what they queued for it */
	fyai_patch_display_clear(ctx);		/* patches it resolved */
	free(ctx->patch_display);
	ctx->patch_display = NULL;
	fyai_output_cleanup(ctx);		/* the document it has open */
	ctx->ui = NULL;				/* its display */
	ctx->browser = NULL;
	ctx->agents = NULL;
	ctx->agent_parent = ctx->agent_execution;
	ctx->agent_execution = 0;
	ctx->shell_stream = NULL;
	ctx->tty_session = NULL;		/* the terminal it runs in */
	ctx->winch_src = NULL;			/* a source on the loop it owns */
	ctx->config_edit = NULL;
	ctx->mcp = NULL;			/* its connections to the servers */
	ctx->mcp_tools = fy_invalid;
	/*
	 * What the parent spent is the accounting of its run, and the extents
	 * of its last call point into storage this child does not use.
	 */
	ctx->usage_input = ctx->usage_cached = ctx->usage_cache_write = 0;
	ctx->usage_output = ctx->usage_reasoning = ctx->usage_total = 0;
	ctx->usage_cost = ctx->usage_cost_est = 0;
	ctx->usage_calls = 0;
	ctx->switch_pending = false;
	ctx->last_call_input = ctx->last_call_output = ctx->last_call_total = 0;
	ctx->last_token_extents = fy_invalid;
	ctx->response_chain_linked = false;
	ctx->response_chain_miss = false;
	ctx->dump_fd = -1;	/* the parent closes its diagnostic copy */
}

/*
 * True when a sub-agent child executes fyai again rather than continuing the
 * forked image. The executed child opens the arena itself, so its fork point
 * must be in the arena: a transient run keeps its state in memory, and a
 * pinned root is read-only. `agent/spawn: fork` selects the forked child.
 */
static bool fyai_agent_spawn_exec(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg = ctx->cfg;
	/*
	 * With credential isolation there is one way to start a sub-agent: a
	 * forked child would keep the address space of its parent and could not
	 * be told from it. `agent/spawn: fork` then means exec.
	 */
	bool want_exec = ctx->tclient ||
		(!fy_str_empty(cfg->agent_spawn) && !strcmp(cfg->agent_spawn, "exec"));

	return fyai_exec_self_available() && want_exec &&
	       !cfg->transient && !cfg->root_pinned && cfg->arena_dir;
}

/*
 * The state an executed sub-agent child cannot read from the arena: the
 * configuration of this run, the branch configuration, the conversation head
 * it forks from, and the identity of this execution. The catalogue of the
 * branch is sent as its arena value: the child shares the arena and checks
 * that the value is in it. A command-line key is
 * sent on the private channel; it is never stored.
 */
static fy_generic fyai_agent_spawn_state(struct fyai_ctx *ctx,
					 struct fy_generic_builder *gb,
					 uint64_t transport_exec)
{
	struct fyai_cfg *cfg = ctx->cfg;
	fy_generic fork;

	fork = fy_null;
	if (fy_is_valid(ctx->last_message) && !ctx->session_unstored)
		fork = fy_gb_mapping(gb,
			"branch", fy_value(gb, fyai_ctx_branch(ctx)),
			"head", (long long)ctx->last_message.v);
	return fy_gb_mapping(gb,
		"config", fyai_generic_or_null(cfg->config_doc),
		"theme_variant", cfg->theme_variant ?
			fy_value(gb, cfg->theme_variant) : fy_null,
		"branch_config", fyai_generic_or_null(ctx->arena_config),
		"branch_catalog", fy_is_mapping(ctx->arena_catalog) ?
			fy_value(gb, (long long)ctx->arena_catalog.v) : fy_null,
		"fork", fork,
		"parent", ctx->agent_execution,
		"transport", transport_exec ?
			fyai_transport_spawn_state(ctx, gb, transport_exec) :
			fy_null,
		"api_key", cfg->api_key_explicit && cfg->api_key ?
			fy_value(gb, cfg->api_key) : fy_null);
}

/*
 * Replace the forked image with a new fyai that serves the control channel on
 * fds 3 and 4. Runs in the child between fork and exec: no allocation.
 * fyai_exec_self() owns the platform-specific executable path.
 */
static void fyai_tool_child_exec(struct fyai_ctx *ctx, bool pty, bool tp)
{
	const char *argv[24];
	char arena_text[32];
	int argc, i;

	argc = 0;
	argv[argc++] = "fyai";
	for (i = 0; i < ctx->cfg->debug && i < 4; i++)
		argv[argc++] = "-d";
	argv[argc++] = "-b";
	argv[argc++] = fyai_ctx_branch(ctx);
	if (ctx->cfg->view_project) {
		argv[argc++] = "--view-project";
		argv[argc++] = ctx->cfg->view_project;
		argv[argc++] = "--view-scratch";
		argv[argc++] = ctx->cfg->view_scratch;
	}
	if (ctx->cfg->view_arena_fd >= 0) {
		snprintf(arena_text, sizeof(arena_text), "%d", ctx->cfg->view_arena_fd);
		argv[argc++] = "--view-arena-fd";
		argv[argc++] = arena_text;
		if (fyai_fsview_arena_pass(ctx->cfg))
			_exit(126);
	}
	argv[argc++] = "agent";
	argv[argc++] = "--tool-child";
	if (pty)
		argv[argc++] = "--pty";
	argv[argc++] = "--arena";
	argv[argc++] = ctx->cfg->arena_dir;
	argv[argc] = NULL;

	/* The control channel is the one inherited descriptor pair. */
	if (fcntl(FYAI_TOOL_CHILD_REQ_FD, F_SETFD, 0) < 0 ||
	    fcntl(FYAI_TOOL_CHILD_RSP_FD, F_SETFD, 0) < 0)
		_exit(126);
	/* and, under credential isolation, its two channels to the transport */
	if (tp && (fcntl(FYAI_TOOL_CHILD_TP_AGENT_FD, F_SETFD, 0) < 0 ||
		   fcntl(FYAI_TOOL_CHILD_TP_CTL_FD, F_SETFD, 0) < 0))
		_exit(126);
	if (ctx->signal_mask_valid)
		(void)sigprocmask(SIG_SETMASK, &ctx->signal_mask, NULL);
	fyai_exec_self(argv);
	_exit(127);
}

/* Spawn a tool child, optionally with a PTY on its standard descriptors. */
static int fyai_tool_job_spawn(struct fyai_ctx *ctx,
			       struct fyai_tool_job *job, bool pty, bool exec,
			       const struct fyai_fsview *view, bool agent_runtime)
{
	int req[2] = { -1, -1 };	/* parent -> child */
	int rsp[2] = { -1, -1 };	/* child -> parent */
	int tpa[2] = { -1, -1 };	/* the transport channel of a sub-agent */
	int tpc[2] = { -1, -1 };	/* its control connection */
	int vsync[2] = { -1, -1 };	/* the view namespace init announces itself */
	int master = -1, slave = -1;
	struct winsize ws = {};
	int rows = 0, cols = 0;
	bool tp = exec && ctx->tclient;
	pid_t pid;
	int pidfd, rc;

	memset(job, 0, sizeof(*job));
	job->ctx = ctx;
	job->exec = exec;
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
	if (tp) {
		rc = fyai_transport_socketpair(tpa);
		fyai_error_check(ctx, !rc, err,
				 "could not create the sub-agent transport channel: %s",
				 strerror(errno));
		rc = fyai_transport_socketpair(tpc);
		fyai_error_check(ctx, !rc, err,
				 "could not create the sub-agent control channel: %s",
				 strerror(errno));
	}
	if (tp && view) {
		rc = fyai_transport_socketpair(vsync);
		fyai_error_check(ctx, !rc, err,
				 "could not create the view announcement channel: %s",
				 strerror(errno));
	}
	pid = fork();
	fyai_error_check(ctx, pid >= 0, err,
			 "could not fork tool process: %s", strerror(errno));

	if (!pid) {			/* child */
		close(req[1]);
		close(rsp[0]);
		if (vsync[0] >= 0)
			close(vsync[0]);
		if (tp) {
			close(tpa[1]);
			close(tpc[1]);
		}
		/* A credential grant is for this child; it then holds no other. */
		fyai_transport_env_take(ctx);
		if (setsid() < 0)
			(void)setpgid(0, 0);	/* already a leader: still isolate */
		fyai_ctx_loop_abandon(ctx);
		/* Abandon unblocks SIGTERM. Re-block it before the handler
		 * below is armed, or a cancellation here kills the child. */
		fyai_tool_child_signals_block();
		if (master >= 0)
			close(master);
		/* Install the PTY before arranging standard and control descriptors. */
		if (slave >= 0 && fyai_tool_child_tty(slave))
			_exit(126);
		/* Record whether this child presents through its own terminal. */
		ctx->cfg->agent_pty = slave >= 0;
		if (fyai_tool_child_fds(ctx, req[0], rsp[1], tp ? tpa[0] : -1,
					tp ? tpc[0] : -1, vsync[1], agent_runtime))
			_exit(126);
		/*
		 * The view is entered after the descriptors are arranged:
		 * entering closes none of them. The process that enters
		 * waits for the namespace init and then ends with its status.
		 */
		if (view && fyai_fsview_enter(ctx->cfg, view, -1, vsync[1] >= 0 ?
						       FYAI_TOOL_CHILD_VIEW_FD : -1))
			_exit(FYAI_SHELL_EXIT_SANDBOX);
		if (view && agent_runtime)
			ctx->cfg->arena_dir = ctx->cfg->view_arena;
		if (exec)
			fyai_tool_child_exec(ctx, slave >= 0, tp);

		fyai_ctx_fork_disown(ctx);
		ctx->cfg->tool_child = true;
		fyai_diag_trace_reopen();
		fyai_tool_child_signals(ctx);
		if (fyai_setup_transient_builder(ctx))
			_exit(1);
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
	if (tp) {
		/*
		 * Register the child with the transport before it can send a
		 * request: it sends none until tool/run, which follows this.
		 * The transport keeps the other ends; ours are closed.
		 */
		close(tpa[0]);
		tpa[0] = -1;
		close(tpc[0]);
		tpc[0] = -1;
		/*
		 * A child in a view runs in a PID namespace of its own, which the
		 * transport cannot name by a PID: it takes the pidfd that the child
		 * sent.
		 */
		pidfd = -1;
		if (vsync[0] >= 0) {
			close(vsync[1]);
			vsync[1] = -1;
			rc = fyai_fsview_init_pidfd(vsync[0], &pidfd);
			fyai_error_check(ctx, !rc, err_kill,
					 "could not learn the process of the sub-agent in its view: %s",
					 strerror(errno));
		}
		/* A pidfd names the child also from inside a PID namespace of its own. */
#ifdef SYS_pidfd_open
		if (pidfd < 0)
			pidfd = syscall(SYS_pidfd_open, pid, 0);
#endif
		rc = fyai_transport_admit_child(ctx, pid, pidfd, tpa[1], tpc[1],
						&job->transport_exec);
		if (!rc && vsync[0] >= 0) {
			/* The agent runs only after the transport knows it. */
			rc = fyai_fsview_init_release(vsync[0], NULL, NULL);
			fyai_error_check(ctx, !rc, err_kill,
					 "could not release the sub-agent in its view: %s",
					 strerror(errno));
		}
		if (pidfd >= 0)
			close(pidfd);
		close(tpa[1]);
		tpa[1] = -1;
		close(tpc[1]);
		tpc[1] = -1;
		if (vsync[0] >= 0) {
			close(vsync[0]);
			vsync[0] = -1;
		}
		if (rc)
			goto err_kill;
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

err_kill:
	kill(pid, SIGKILL);
	waitpid(pid, NULL, 0);
err:
	if (req[0] >= 0)
		close(req[0]);
	if (req[1] >= 0)
		close(req[1]);
	if (rsp[0] >= 0)
		close(rsp[0]);
	if (rsp[1] >= 0)
		close(rsp[1]);
	if (tpa[0] >= 0)
		close(tpa[0]);
	if (tpa[1] >= 0)
		close(tpa[1]);
	if (tpc[0] >= 0)
		close(tpc[0]);
	if (tpc[1] >= 0)
		close(tpc[1]);
	if (vsync[0] >= 0)
		close(vsync[0]);
	if (vsync[1] >= 0)
		close(vsync[1]);
	if (master >= 0)
		close(master);
	if (slave >= 0)
		close(slave);
	return -1;
}


static void fyai_tool_job_live_close(struct fyai_tool_job *job,
				     bool commit_band)
{
	fyai_event_source_remove(job->band_delay);
	job->band_delay = NULL;
	free(job->pending_output.data);
	memset(&job->pending_output, 0, sizeof(job->pending_output));
	/* A sub-agent's screen is committed with its outcome, not discarded. */
	if (job->agent)
		fyai_agent_view_close(job, job->result_ok && !job->failed,
				      NULL);
	if (job->stream.active)
		fyai_fenced_stream_finish(&job->stream);
	if (commit_band)
		fyai_sink_band_commit(job->band);
	else
		fyai_sink_band_destroy(job->band);
	job->band = NULL;
	if (!job->btw_panel) {
		free(job->title);
		job->title = NULL;
	}
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
		fyai_agents_conn_closed(job->ctx, job->conn);
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

/*
 * Whether a sub-agent runs in a view of its own: the call decides, else the
 * agent/isolation setting does.
 */
/* The sub-agents that this agent started: the live ones, then the ones that ended. */
static fy_generic fyai_list_agents(struct fyai_ctx *ctx, struct fy_generic_builder *gb)
{
	struct fyai_tool_job *job;
	fy_generic rows, branch, entry, views;
	const char *nm, *rest;
	char prefix[FYAI_BRANCH_NAME_MAX + sizeof(FYAI_BRANCH_AGENT_PREFIX) + 1];
	size_t len;

	rows = fy_sequence(gb);
	len = snprintf(prefix, sizeof(prefix), "%s/" FYAI_BRANCH_AGENT_PREFIX, fyai_ctx_branch(ctx));
	views = fyai_view_list(ctx, gb);
	for (job = ctx->tool_jobs; job; job = job->next) {
		if (!job->agent || job->btw || job->done || !job->branch ||
		    strncmp(job->branch, prefix, len) || strchr(job->branch + len, '/'))
			continue;
		rest = job->branch + len;
		rows = fy_append(gb, rows, fy_mapping(gb, "name", rest,
			"state", job->terminating ? "stopping" : job->wants_input ? "waiting" : "running",
			"view", fy_is_mapping(fy_get(views, fy_sprintfa(FYAI_VIEW_AGENT_PREFIX "%s", rest),
						     fy_invalid))));
	}
	/* The branch table is what the ended ones left. */
	if (fyai_branches_refresh(ctx) || !fy_is_mapping(ctx->arena_branches))
		return rows;
	fy_foreach_key_value(branch, entry, ctx->arena_branches) {
		nm = fy_castp(&branch, "");
		if (strncmp(nm, prefix, len) || !nm[len] || strchr(nm + len, '/') ||
		    !strncmp(nm + len, "btw-", 4) || fyai_agent_job_named(ctx, nm))
			continue;
		rest = nm + len;
		rows = fy_append(gb, rows, fy_mapping(gb, "name", rest, "state", "finished",
			"view", fy_is_mapping(fy_get(views, fy_sprintfa(FYAI_VIEW_AGENT_PREFIX "%s", rest),
						     fy_invalid))));
	}
	return rows;
}

/* The agent views that this agent has: a sub-agent leaves one when it ran isolated. */
static fy_generic fyai_list_views(struct fyai_ctx *ctx, struct fy_generic_builder *gb)
{
	fy_generic rows, view, views;
	const char *key;
	size_t prefix = sizeof(FYAI_VIEW_AGENT_PREFIX) - 1;

	rows = fy_sequence(gb);
	views = fyai_view_list(ctx, gb);
	fy_foreach_key_value(key, view, views) {
		if (strncmp(key, FYAI_VIEW_AGENT_PREFIX, prefix))
			continue;
		rows = fy_append(gb, rows, fy_mapping(gb, "name", fy_value(gb, key + prefix),
						      "state", fy_get(view, "state", "ready")));
	}
	return rows;
}

/* The named terminal sessions of this run that still have a program. */
static fy_generic fyai_list_shells(struct fyai_ctx *ctx, struct fy_generic_builder *gb)
{
	struct fyai_shell_session *sess;
	fy_generic rows;

	rows = fy_sequence(gb);
	for (sess = ctx->shell_sessions; sess; sess = sess->next) {
		if (sess->exited)
			continue;
		rows = fy_append(gb, rows, fy_mapping(gb, "name", sess->name,
			"command", sess->command ? sess->command : "",
			"state", sess->closing ? "stopping" : sess->wants_input ? "waiting" : "running"));
	}
	return rows;
}

/*
 * The list tool: what this agent owns and can name in another call. It is the one
 * way to enumerate; each row is scoped to the caller. A name that a row gives is
 * relative to the caller, and a branch, a user view or the object of another
 * agent is not a row.
 */
static fy_generic fyai_list_tool(struct fyai_ctx *ctx, fy_generic args, bool *okp)
{
	struct fy_generic_builder *gb = ctx->transient_gb;
	const char *kind = fy_get(args, "kind", "");
	fy_generic result = fy_map_empty;
	bool all = !*kind;

	*okp = false;
	if (!all && !fy_any_equal(kind, "agents", "views", "shells", "waits"))
		return fy_value(gb, "tool error: kind is agents, views, shells or waits");
	if (all || !strcmp(kind, "agents"))
		result = fy_assoc(gb, result, "agents", fyai_list_agents(ctx, gb));
	if (all || !strcmp(kind, "views"))
		result = fy_assoc(gb, result, "views", fyai_list_views(ctx, gb));
	if (all || !strcmp(kind, "shells"))
		result = fy_assoc(gb, result, "shells", fyai_list_shells(ctx, gb));
	if (all || !strcmp(kind, "waits"))
		result = fy_assoc(gb, result, "waits", fyai_waits_rows(ctx, gb));
	*okp = true;
	return fy_gb_internalize(gb, result);
}

/*
 * The project_view tool: list a change of the view of a sub-agent that this agent
 * started, or apply it. The name is the one the agent gave the sub-agent. A
 * user view, a reference and the views of other agents are not reachable: each
 * agent has its own store, and the namespace of the sub-agents is the only one
 * the tool names.
 */
static fy_generic fyai_view_tool(struct fyai_ctx *ctx, fy_generic args, bool *okp)
{
	enum fyai_view_pull_mode mode;
	fy_generic paths = fy_get(args, "paths", fy_seq_empty), result;
	const char *action = fy_get(args, "action", ""), *name = fy_get(args, "name", "");
	const char **selected = NULL, *path;
	size_t count = fy_len(paths), index = 0;
	char *diag;
	int rc;

	*okp = false;
	if (ctx->cfg->view_project)
		return fy_value(ctx->transient_gb, "tool error: project_view is not available inside a view");
	/* The tool is not given to such an agent: refuse a call that names it anyway. */
	if (ctx->cfg->agent_child && !ctx->agent_execution)
		return fy_value(ctx->transient_gb, "tool error: project_view is not available to this agent");
	if (strcmp(action, "changes") && strcmp(action, "apply"))
		return fy_value(ctx->transient_gb, "tool error: action is changes or apply");
	if (!*name)
		return fy_value(ctx->transient_gb, "tool error: name the sub-agent");
	selected = calloc(count + 1, sizeof(*selected));
	fyai_error_check(ctx, selected, err, "could not allocate the path list");
	fy_foreach(path, paths)
		selected[index++] = path;
	mode = !strcmp(action, "changes") ? FYAI_VIEW_PULL_CHANGES :
	       fy_get(args, "dry_run", false) ? FYAI_VIEW_PULL_DRY_RUN : FYAI_VIEW_PULL_APPLY;
	rc = fyai_view_pull_agent(ctx, ctx->transient_gb, name, selected, count, mode, &result);
	free(selected);
	if (rc) {
		diag = fyai_diag_string(&ctx->cfg->diag);
		result = fy_gb_internalize(ctx->transient_gb,
			fy_stringf("tool error: %s", diag && *diag ? diag : "the view could not be read"));
		free(diag);
		return result;
	}
	/* A conflict is an outcome to read, not a failure of the call. */
	ctx->cfg->exit_status = 0;
	*okp = true;
	return fy_gb_internalize(ctx->transient_gb, result);
err:
	return fy_value(ctx->transient_gb, "tool error: out of memory");
}

static bool fyai_agent_isolated(struct fyai_ctx *ctx, fy_generic args)
{
	fy_generic asked = fy_get(args, "isolated", fy_invalid);
	fy_generic section = fy_get(ctx->cfg->config_doc, "agent", fy_invalid);

	/* The view that an agent or a session runs in is shared by what it starts. */
	if (ctx->cfg->view_project)
		return false;
	if (fy_is_bool(asked))
		return fy_cast(asked, false);
	return fy_equal(fy_get(section, "isolation", "none"), "view");
}

/*
 * The view of a sub-agent is named for it in the namespace of the sub-agents. The
 * name is one component of characters that a command takes without quoting, so
 * the name of an agent cannot leave the namespace.
 */
static int fyai_agent_view_begin(struct fyai_ctx *ctx, const char *agent,
				 struct fyai_view_run **run)
{
	char name[FYAI_BRANCH_NAME_MAX + 8];
	size_t i, n;
	int rc;

	if (ctx->cfg->view_project) {
		fyai_error(ctx, "a sub-agent that runs in a view cannot isolate "
			   "its own sub-agents");
		return -1;
	}
	n = snprintf(name, sizeof(name), FYAI_VIEW_AGENT_PREFIX);
	for (i = 0; agent[i] && n + 1 < sizeof(name); i++)
		name[n++] = isalnum((unsigned char)agent[i]) || agent[i] == '_' ||
			    agent[i] == '-' ? agent[i] : '-';
	name[n] = '\0';
	rc = fyai_view_run_begin(ctx, name, run);
	return rc;
}

/* Capture what the isolated sub-agent changed and tell its parent where it is. */
static void fyai_agent_view_finish(struct fyai_ctx *ctx, struct fyai_tool_job *job,
				   fy_generic *result)
{
	char *summary = NULL;
	const char *name = fyai_view_run_name(job->view_run);
	int rc;

	rc = fyai_view_run_finish(ctx, job->view_run, &summary);
	if (!fy_is_string(*result)) {
		fyai_view_run_free(job->view_run);
		job->view_run = NULL;
		free(summary);
		return;
	}
	if (rc)
		*result = fy_stringf(ctx->transient_gb, "%s\n\n[The sub-agent ran in the view "
				     "'%s', but its changes could not be captured; see the "
				     "diagnostics.]", fy_castp(result, ""), name);
	else
		*result = fy_stringf(ctx->transient_gb, "%s\n\n[The sub-agent ran in the view "
				     "'%s'. Its changes are not in the project; %s\nReview "
				     "them with the project_view tool, as '%s', or `view diff %s`.]",
				     fy_castp(result, ""), name,
				     summary ? summary : "the change could not be listed",
				     name + sizeof(FYAI_VIEW_AGENT_PREFIX) - 1, name);
	free(summary);
	fyai_view_run_free(job->view_run);
	job->view_run = NULL;
}

static void fyai_tool_job_discard(struct fyai_tool_job *job)
{
	if (!job)
		return;
	job->btw_panel = false;
	if (job->inline_block) {
		fyai_ui_inline_drop(job->ctx, (uintptr_t)job);
		job->inline_block = false;
	}
	fyai_tool_job_cancel(job);
	/*
	 * A discarded job settles nothing through update_done, so drop its
	 * queued wait reports here: the branch answers no question after this.
	 */
	if (job->agent && job->branch)
		fyai_events_drop_agent(job->ctx, job->branch);
	fyai_tool_job_close_channel(job);
	fyai_tool_job_drop(&job->deadline);
	fyai_tool_job_drop(&job->hang_deadline);
	fyai_tool_job_drop(&job->csrc);
	if (!job->reaped && job->pid > 0)
		while (waitpid(job->pid, NULL, 0) < 0 && errno == EINTR)
			;
	fyai_tool_job_live_close(job, false);
	fyai_tool_job_unlink(job->ctx, job);
	/* A cancelled isolated agent still leaves what it wrote in its view. */
	if (job->view_run) {
		char *summary = NULL;

		(void)fyai_view_run_finish(job->ctx, job->view_run, &summary);
		free(summary);
	}
	fyai_view_run_free(job->view_run);
	free(job->progress.data);
	free(job->branch);
	free(job->origin);
	fy_generic_builder_destroy(job->call_gb);
	free(job);
}

void fyai_tools_btw_panels_close(struct fyai_ctx *ctx)
{
	struct fyai_tool_job *job, *next;

	if (!ctx)
		return;
	for (job = ctx->tool_jobs; job; job = next) {
		next = job->next;
		if (job->btw_panel)
			fyai_tool_job_discard(job);
	}
}

bool fyai_tools_btw_dismiss_focused(struct fyai_ctx *ctx)
{
	struct fyai_tool_job *job;
	struct fytim_surface *focused;

	if (!ctx || !ctx->workpane)
		return false;
	focused = fyai_workpane_focused(ctx->workpane);
	for (job = ctx->tool_jobs; job; job = job->next)
		if (job->btw_panel && job->surface == focused) {
			fyai_tool_job_discard(job);
			return true;
		}
	return false;
}

bool fyai_tools_kept_surface(struct fyai_ctx *ctx,
			     const struct fytim_surface *sf)
{
	struct fyai_shell_session *sess;

	if (!ctx || !sf)
		return false;
	for (sess = ctx->shell_sessions; sess; sess = sess->next)
		if (sess->surface == sf)
			return sess->exited && sess->keep_tile;
	return false;
}

bool fyai_tools_btw_surface(struct fyai_ctx *ctx, struct fytim_surface *sf)
{
	struct fyai_tool_job *job;

	if (!ctx || !sf)
		return false;
	for (job = ctx->tool_jobs; job; job = job->next)
		if (job->btw_panel && job->surface == sf)
			return true;
	return false;
}

static enum fyai_event_action
fyai_tool_job_deadline(const struct fyai_event *ev);
static enum fyai_event_action
fyai_tool_job_hang_deadline(const struct fyai_event *ev);
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

/*
 * Report a submission failure and jump to a cleanup label. A failure the model
 * is told about is handled: the caller answers the call with the message that
 * fyai_tool_submit_error_set() left. Such a report stays below error severity,
 * so it neither latches as the cause of the run - hiding a later real failure
 * behind it - nor fails an otherwise complete sub-agent. A failure with no
 * message for the model has nothing to answer it, and keeps error severity.
 */
#define fyai_tool_submit_check(_ctx, _cond, _label, _fmt, ...)		\
	do {								\
		if (!(_cond)) {						\
			if ((_ctx)->tool_submit_error)			\
				fyai_warning((_ctx), (_fmt),		\
					     ##__VA_ARGS__);		\
			else						\
				fyai_error((_ctx), (_fmt),		\
					   ##__VA_ARGS__);		\
			goto _label;					\
		}							\
	} while (0)

/*
 * The rows that a live tool band keeps. The band shows the preview rows and
 * the user scrolls back through the rest, so the stream renders more rows than
 * the band draws. The committed exchange keeps the preview. A call drawn in
 * the transcript keeps the preview alone.
 */
static size_t fyai_tool_history_lines(const struct fyai_ctx *ctx)
{
	const struct fyai_cfg *cfg = ctx->cfg;

	if (cfg->tool_preview_lines <= 0)
		return 0;
	/* The transcript scrolls a call drawn in it: it keeps no history. */
	if (fyai_ui_tools_inline(ctx))
		return (size_t)cfg->tool_preview_lines;
	return cfg->tool_history_lines > cfg->tool_preview_lines ?
	       (size_t)cfg->tool_history_lines :
	       (size_t)cfg->tool_preview_lines;
}

static void fyai_tool_job_band_open(struct fyai_tool_job *job)
{
	struct fyai_ctx *ctx = job->ctx;

	job->band = fyai_sink_band_open(ctx->sink, false, NULL, NULL);
	if (!job->band ||
	    fyai_fenced_stream_start(&job->stream, ctx, ctx->cfg, NULL,
		fyai_tool_history_lines(ctx),
		markdown_tool_output_indent(ctx->cfg), NULL, true)) {
		fyai_tool_job_live_close(job, false);
		return;
	}
	fyai_fenced_stream_bind_band(&job->stream, job->band);
	job->stream.title = job->title;
	job->stream.command = job->command;
	fyai_sink_band_paint(job->band, job->title, job->command, NULL, 0,
			     NULL);
	if (job->pending_output.len)
		(void)fyai_fenced_stream_push(&job->stream,
			job->pending_output.data, job->pending_output.len);
	free(job->pending_output.data);
	memset(&job->pending_output, 0, sizeof(job->pending_output));
}

static enum fyai_event_action
fyai_tool_job_band_delay(const struct fyai_event *ev)
{
	struct fyai_tool_job *job = ev->userdata;

	job->band_delay = NULL;
	if (!job->done)
		fyai_tool_job_band_open(job);
	return FYAIEA_CONTINUE;
}

struct fyai_tool_job *fyai_tool_job_submit(struct fyai_ctx *ctx,
					    fy_generic tool_call)
{
	struct fyai_tool_job *job = NULL;
	struct fyai_view_run *view_run = NULL;
	struct response_buffer view = {0};
	const char *name, *args_text, *command;
	const char *asked, *cmdtext;
	fy_generic args, progress_args, agent_name;
	bool agent_stored = false;
	fy_generic session_call, session_command, session_desc;
	struct fyai_event_loop *el;
	char child_branch[FYAI_BRANCH_NAME_MAX + 1];
	char *session_name = NULL;
	char *session_title = NULL;
	char *label = NULL;
	bool have_session = false;
	bool have_branch = false;
	bool native_call;
	bool eligible;
	bool user_owned;
	struct fy_generic_builder_cfg call_cfg = {};
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
		fyai_tool_submit_check(ctx, !rc, err,
				       "could not name the sub-agent branch");
		have_branch = true;
		if (fyai_agent_isolated(ctx, args)) {
			rc = fyai_agent_view_begin(ctx, fy_castp(&agent_name, "agent"),
						   &view_run);
			fyai_tool_submit_check(ctx, !rc, err,
					       "could not isolate the sub-agent");
		}
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
		fyai_tool_submit_check(ctx, false, err,
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
		fyai_tool_submit_check(ctx,
				       session_name && !ctx->tool_submit_error,
				       err,
				       "could not name the terminal session");
		have_session = true;
	}

	/*
	 * A forked sub-agent would keep the address space of its parent, and it
	 * could not be told from the parent by the transport. With credential
	 * isolation it must be an executed child.
	 */
	if (ctx->tclient && fy_equal(name, "agent") && !fyai_agent_spawn_exec(ctx))
		fyai_tool_submit_error_set(ctx,
			"tool error: with credential isolation a sub-agent must "
			"execute fyai again: use agent/spawn exec with a saved "
			"arena and a root that is not pinned");
	fyai_tool_submit_check(ctx, !ctx->tool_submit_error, err,
			       "a sub-agent cannot be forked with credential isolation");

	job = calloc(1, sizeof(*job));
	fyai_error_check(ctx, job, err,
			 "could not allocate tool job");
	/* A sub-agent renders to a terminal of its own; the parent shows it. */
	rc = fyai_tool_job_spawn(ctx, job, fy_equal(name, "agent") &&
				 fyai_agents_detail(ctx->agent_execution ? 2 : 1) == 2 &&
				 fyai_ui_active(ctx),
				 fy_equal(name, "agent") &&
				 fyai_agent_spawn_exec(ctx),
				 view_run ? fyai_view_run_spec(view_run) : NULL, fy_equal(name, "agent"));
	fyai_error_check(ctx, !rc, err,
		"could not spawn tool job");
	job->view_run = view_run;
	view_run = NULL;
	fyai_tool_job_link(ctx, job);
	if (have_session) {
		user_owned = fy_get(args, "_fyai_user_owned", false);
		fyai_shell_tty_size(ctx, args, &srows, &scols);
		/* A screen drawn in the transcript has the rows of the setting
		 * and the width of the transcript, unless the call asks. */
		if (!user_owned && fyai_ui_tools_inline(ctx)) {
			if (fy_get(args, "rows", 0LL) <= 0)
				srows = ctx->cfg->inline_terminal_rows;
			if (fy_get(args, "cols", 0LL) <= 0)
				scols = fyai_shell_inline_cols(ctx);
		}
		session_command = fy_get(args, "command", fy_invalid);
		session_desc = fy_get(args, "description", fy_invalid);
		session_title = fyai_format_shell_label(args);
		job->session = fyai_shell_session_create(ctx, session_name,
					fy_castp(&session_command, ""),
					session_title,
					fy_castp(&session_desc, ""),
					srows, scols,
					user_owned ? 0 : fyai_shell_output_bytes(ctx, args),
					!fyai_shell_tty_requested(ctx, args),
					user_owned);
		free(session_title);
		session_title = NULL;
		fyai_error_check(ctx, job->session, err,
				 "could not open the terminal session");
		job->session->job = job;
		fyai_shell_session_start_size(job, args);
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
	/* The call outlives the turn that made it. */
	if (fy_get(args, "_fyai_btw", false) ||
	    fy_get(args, "_fyai_background", false)) {
		job->call_gb = fy_generic_builder_create(&call_cfg);
		fyai_error_check(ctx, job->call_gb, err,
				 "could not retain side question call");
		job->call = fy_gb_internalize(job->call_gb, tool_call);
		fyai_error_check(ctx, fy_is_valid(job->call), err,
				 "could not retain side question call");
	} else {
		job->call = tool_call;
	}
	job->agent = fy_equal(name, "agent");
	job->btw = fy_get(args, "_fyai_btw", false);
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
				markdown_tool_output_indent(ctx->cfg),
				stderr, true)) {
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
			if (job->btw) {
				job->title = strdup("**btw**\n");
			} else {
				command = fy_get(args, "description", "");
				job->title = fyai_format_tool_header(ctx, "agent",
					fy_mapping("name", fy_get(args, "name", ""),
						   "description",
						   *command ? command : name), 0);
			}
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
		/* A terminal session displays output on its own surface. */
		if (have_session)
			goto live_open_done;
		el = fyai_ctx_loop(ctx);
		/* A call drawn in the transcript grows in place: it has no
		 * jump of the page to hide. */
		if (ctx->cfg->work_open_delay_ms > 0 && el &&
		    !fyai_ui_tools_inline(ctx) &&
		    !fyai_event_add_timer(el, ctx->cfg->work_open_delay_ms, 0,
					 fyai_tool_job_band_delay, job,
					 &job->band_delay))
			goto live_open_done;
		fyai_tool_job_band_open(job);
	}
live_open_done:
	rc = fyai_tool_job_attach(ctx, job);
	fyai_error_check(ctx, !rc, err,
			 "could not attach tool job to event loop");

	job->timeout_ms = fyai_tool_job_timeout_ms(ctx, name, native_call, args);
	job->started_ms = fyai_event_now_ms();
	if (job->timeout_ms) {
		el = fyai_ctx_loop(ctx);
		assert(el);
		rc = fyai_event_add_timer(el, job->timeout_ms, 0,
					  fyai_tool_job_deadline, job,
					  &job->deadline);
		fyai_error_check(ctx, !rc, err,
				 "could not arm the tool job time limit");
	}
	if (fy_equal(name, "agent") && ctx->cfg->agent_timeout_kill &&
	    ctx->cfg->agent_hang_timeout_ms > 0) {
		job->hang_timeout_ms = ctx->cfg->agent_hang_timeout_ms;
		el = fyai_ctx_loop(ctx);
		assert(el);
		/* With no advisory limit, the hang limit starts at submission. */
		rc = fyai_event_add_timer(el,
			(fyai_event_ms_t)job->timeout_ms + job->hang_timeout_ms,
			0, fyai_tool_job_hang_deadline, job,
			&job->hang_deadline);
		fyai_error_check(ctx, !rc, err,
				 "could not arm the sub-agent hang limit");
	}
	return job;

err:
	free(session_name);
	fyai_view_run_free(view_run);
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

	if (fyai_agents_serve(job->ctx, conn, method, params, id, &text, errorp))
		return text;
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
			/* It is drawing, thus it is not quiet. */
			job->session->quiet_since = fyai_event_now_ms();
			job->session->drew = true;
			(void)fyai_terminal_view_feed(job->session->view,
						      bytes, n);
			fyai_shell_session_refresh(job->session);
		}
		free(bytes);
		return fy_invalid;
	}
	if (!strcmp(method, "tty/resized")) {
		if (job->session)
			fyai_shell_session_resized(job->session,
				(int)fy_get(params, "rows", 0LL),
				(int)fy_get(params, "cols", 0LL));
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
	else if (job->band_delay &&
		 !response_buffer_reserve(&job->pending_output,
					  job->pending_output.len + len + 1)) {
		memcpy(job->pending_output.data + job->pending_output.len, p, len);
		job->pending_output.len += len;
		job->pending_output.data[job->pending_output.len] = '\0';
	}
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
		/*
		 * The reply is in the transient builder, which the turn
		 * releases when it ends. A retained call is collected later,
		 * so its reply moves to the builder that the job owns.
		 */
		if (job->call_gb) {
			job->result = fy_gb_internalize(job->call_gb,
							job->result);
			job->display = fy_gb_internalize(job->call_gb,
							 job->display);
			job->diag = fy_gb_internalize(job->call_gb, job->diag);
		}
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
	fy_generic params;
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
	params = !fy_str_empty(job->branch) ?
		fy_gb_mapping(gb, "call", job->call, "branch", job->branch) :
		fy_gb_mapping(gb, "call", job->call);
	if (job->exec)
		params = fy_assoc(gb, params, fy_value(gb, "spawn"),
				     fyai_agent_spawn_state(job->ctx, gb, job->transport_exec));
	if (job->start_cols > 0)
		params = fy_assoc(gb, params, fy_value(gb, "size"),
				  fy_gb_mapping(gb,
					"rows", (long long)job->start_rows,
					"cols", (long long)job->start_cols));
	fyai_error_check(ctx, fy_is_mapping(params), err,
			 "could not build the tool call request");
	job->run = jsonrpc_request_submit(job->conn, "tool/run", params,
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

bool fyai_tools_active(const struct fyai_ctx *ctx)
{
	const struct fyai_tool_job *job;
	const struct fyai_shell_session *session;

	for (job = ctx->tool_jobs; job; job = job->next)
		if (!job->done)
			return true;
	for (session = ctx->shell_sessions; session; session = session->next)
		if (!session->exited)
			return true;
	return false;
}

#define FYAI_TOOL_TERM_MS 2000

/* Find the live session or sub-agent that owns @sf. */
static void fyai_tile_owner(struct fyai_ctx *ctx, struct fytim_surface *sf,
			    struct fyai_shell_session **sessp,
			    struct fyai_tool_job **jobp)
{
	enum fyai_workpane_tile_kind kind = FYAI_WORKPANE_TILE_TEXT;
	void *owner;

	*sessp = NULL;
	*jobp = NULL;
	owner = fyai_workpane_tile_owner(ctx ? ctx->workpane : NULL, sf, &kind);
	if (!owner)
		return;
	if (kind == FYAI_WORKPANE_TILE_SHELL)
		*sessp = owner;
	else if (kind == FYAI_WORKPANE_TILE_AGENT)
		*jobp = owner;
}

static void fyai_tools_zoom_write(struct fyai_shell_session *sess,
				  struct fyai_tool_job *job, const char *data,
				  size_t len);

/*
 * Return the view scroll in rows for a chunk of arrow and page keys. A chunk
 * can hold several keys; a chunk with any other byte scrolls nothing.
 */
static int fyai_btw_scroll_delta(struct fyai_tool_job *job,
				 const char *data, size_t len)
{
	int page = job->pty_rows > 2 ? job->pty_rows - 2 : 1;
	int delta = 0;

	while (len) {
		if (len >= 4 && !memcmp(data, "\x1b[5~", 4)) {
			delta += page;
			data += 4;
			len -= 4;
		} else if (len >= 4 && !memcmp(data, "\x1b[6~", 4)) {
			delta -= page;
			data += 4;
			len -= 4;
		} else if (len >= 3 && !memcmp(data, "\x1b[A", 3)) {
			delta += 1;
			data += 3;
			len -= 3;
		} else if (len >= 3 && !memcmp(data, "\x1b[B", 3)) {
			delta -= 1;
			data += 3;
			len -= 3;
		} else {
			return 0;
		}
	}
	return delta;
}

/* Route keyboard input from the focused tile. */
static void fyai_tools_zoom_keys(void *user, const char *data, size_t len)
{
	struct fyai_ctx *ctx = user;
	struct fyai_shell_session *sess;
	struct fyai_tool_job *job;
	size_t i;
	int delta;

	if (fyai_agents_keys(ctx, data, len))
		return;
	if (fyai_browser_surface(ctx, fyai_workpane_focused(ctx->workpane))) {
		for (i = 0; i < len; i++) {
			if (data[i] != FYAI_FOCUS_NEXT_KEY && data[i] != FYAI_FOCUS_PROMPT_KEY)
				continue;
			if (i)
				(void)fyai_browser_keys(ctx, data, i);
			if (data[i] == FYAI_FOCUS_NEXT_KEY)
				fyai_tools_focus_next(ctx);
			else
				fyai_tools_unzoom(ctx);
			if (i + 1 < len)
				(void)fyai_ui_keys_return(ctx, data + i + 1, len - i - 1);
			return;
		}
		(void)fyai_browser_keys(ctx, data, len);
		return;
	}
	fyai_tile_owner(ctx, fyai_workpane_focused(ctx->workpane), &sess,
			&job);
	if (!sess && !job) {
		/* Return focus after the program exits. */
		fyai_tools_unzoom(ctx);
		return;
	}
	if (job && job->btw_panel && len == 1 && data[0] == FYAI_KEY_ESC) {
		fyai_tool_job_discard(job);
		return;
	}
	/* A finished bang tile takes Escape or q to leave; its program reads
	 * nothing more. */
	if (sess && sess->exited && sess->keep_tile && len == 1 &&
	    (data[0] == FYAI_KEY_ESC || data[0] == 'q')) {
		fyai_shell_session_dismiss(sess);
		fyai_tools_unzoom(ctx);
		return;
	}
	if (job && job->btw_panel && job->view) {
		delta = fyai_btw_scroll_delta(job, data, len);
		if (delta) {
			if (fyai_terminal_view_scroll(job->view, delta))
				fyai_agent_view_refresh(job);
			return;
		}
	}
	for (i = 0; i < len; i++) {
		if (data[i] != FYAI_FOCUS_NEXT_KEY &&
		    data[i] != FYAI_FOCUS_PROMPT_KEY)
			continue;
		/* Send bytes that precede the intercepted focus key. */
		if (i)
			fyai_tools_zoom_write(sess, job, data, i);
		if (data[i] == FYAI_FOCUS_NEXT_KEY)
			fyai_tools_focus_next(ctx);
		else
			fyai_tools_unzoom(ctx);
		/* Return the unconsumed frame tail to the new input owner. */
		if (i + 1 < len)
			(void)fyai_ui_keys_return(ctx, data + i + 1,
						  len - i - 1);
		return;
	}
	fyai_tools_zoom_write(sess, job, data, len);
}

/* Route a surface control request to its owner. */
void fyai_tools_surface_request(struct fyai_ctx *ctx, struct fytim_surface *sf,
				int delta)
{
	struct fyai_shell_session *sess;
	struct fyai_tool_job *job;
	struct fytim_surface *surface;
	struct fyai_terminal_view *view;

	if (!ctx || !sf)
		return;
	if (delta) {
		/* Scroll the terminal view without sending input to the program. */
		fyai_tile_owner(ctx, sf, &sess, &job);
		view = sess ? sess->view : job ? job->view : NULL;
		surface = sess ? sess->surface : job ? job->surface : NULL;
		if (view && surface && fyai_terminal_view_scroll(view, delta) &&
		    fyai_ui_surface_publish(surface, view) > 0)
			fyai_ui_wake(ctx);
		return;
	}
	if (fyai_browser_surface(ctx, sf)) {
		fyai_browser_close(ctx);
		return;
	}
	if (fyai_agents_surface(ctx, sf)) {
		fyai_agents_detach(ctx);
		return;
	}

	fyai_tile_owner(ctx, sf, &sess, &job);
	/* Request graceful termination so the result remains available. */
	if (sess && sess->exited && sess->keep_tile)
		fyai_shell_session_dismiss(sess);
	else if (sess) {
		/* A closed tile goes when its program ends. */
		sess->close_on_exit = true;
		fyai_shell_session_close(sess, false);
	}
	else if (job) {
		if (job->btw_panel)
			fyai_tool_job_discard(job);
		else
			fyai_tool_job_cancel(job);
	}
}

/* Send @len bytes to the terminal owner. */
static void fyai_tools_zoom_write(struct fyai_shell_session *sess,
				  struct fyai_tool_job *job, const char *data,
				  size_t len)
{
	struct fyai_terminal_view *view = sess ? sess->view : job ? job->view :
					  NULL;

	/* Input returns the view to the live screen. */
	if (view && !(job && job->btw_panel) &&
	    fyai_terminal_view_scroll_live(view) &&
	    (sess ? sess->surface : job->surface))
		(void)fyai_ui_surface_publish(sess ? sess->surface :
					      job->surface, view);
	if (sess)
		fyai_shell_session_reply(data, len, sess);
	else if (job && job->pty >= 0)
		fyai_agent_view_reply(data, len, job);
}

/* Give @sf terminal keyboard focus. Focus changes no geometry. */
static bool fyai_tools_focus(struct fyai_ctx *ctx, struct fytim_surface *sf)
{
	if (!ctx || !sf)
		return false;
	fyai_workpane_set_keys_router(ctx->workpane, fyai_tools_zoom_keys, ctx);
	fyai_workpane_set_focus(ctx->workpane, sf);
	return fyai_workpane_focused(ctx->workpane) == sf;
}

/* Give the named live tile keyboard focus and the full work pane. */
const char *fyai_tools_zoom(struct fyai_ctx *ctx, const char *name)
{
	struct fyai_shell_session *sess;
	struct fyai_shell_session *zoom_sess = NULL;
	struct fyai_tool_job *job;
	struct fyai_tool_job *zoom_job = NULL;
	struct fytim_surface *sf = NULL;
	const char *what = NULL;

	if (!ctx)
		return NULL;
	if (fyai_agents_ambiguous(ctx, name)) {
		fyai_error(ctx, "zoom: agent name '%s' is ambiguous; use its full branch name", name);
		return NULL;
	}
	for (sess = ctx->shell_sessions; sess && !sf; sess = sess->next) {
		if (!sess->surface || sess->exited)
			continue;
		if (!name || !*name || (sess->name && !strcmp(sess->name, name))) {
			sf = sess->surface;
			what = sess->name;
			zoom_sess = sess;
		}
	}
	for (job = ctx->tool_jobs; job && !sf; job = job->next) {
		const char *agent;

		if (!job->surface)
			continue;
		/* Derive a sub-agent name from its branch. */
		agent = job->agent ? fyai_agent_job_name(job) : job->title;
		if (!name || !*name || (job->branch && !strcmp(job->branch, name)) ||
		    (agent && !strcmp(agent, name))) {
			sf = job->surface;
			what = agent;
			zoom_job = job;
		}
	}
	if (!sf)
		return fyai_agents_zoom(ctx, name, false);

	(void)zoom_sess;
	fyai_tools_unzoom(ctx);
	if (fyai_ui_surface_zoom(ctx, sf))
		return NULL;
	if (!fyai_tools_focus(ctx, sf)) {
		(void)fyai_ui_surface_zoom(ctx, NULL);
		return NULL;
	}
	if (zoom_job && zoom_job->btw_panel) {
		fyai_terminal_view_damage_all(zoom_job->view);
		fyai_agent_view_refresh(zoom_job);
	}
	return what ? what : "";
}

/* Return the work pane and keyboard focus to the prompt. */
void fyai_tools_unzoom(struct fyai_ctx *ctx)
{
	if (!ctx)
		return;
	fyai_agents_detach(ctx);
	fyai_workpane_clear_focus(ctx->workpane);
	fyai_workpane_clear_zoom(ctx->workpane);
}

void fyai_tools_counts(struct fyai_ctx *ctx, int *userp, int *shellp,
		       int *agentp)
{
	struct fyai_shell_session *sess;
	struct fyai_tool_job *job;

	*userp = *shellp = *agentp = 0;
	if (!ctx)
		return;
	for (sess = ctx->shell_sessions; sess; sess = sess->next) {
		if (sess->exited)
			continue;
		if (sess->user_owned)
			(*userp)++;
		else
			(*shellp)++;
	}
	for (job = ctx->tool_jobs; job; job = job->next)
		if (job->agent && !job->done)
			(*agentp)++;
}

void fyai_tools_config_changed(struct fyai_ctx *ctx)
{
	struct fyai_shell_session *sess;
	struct fyai_tool_job *job;
	int rows;

	if (!ctx)
		return;
	/* Apply the new scrollback limit to each active terminal view. */
	rows = ctx->cfg->work_history_rows;
	for (sess = ctx->shell_sessions; sess; sess = sess->next)
		if (sess->view &&
		    !fyai_terminal_view_set_history(sess->view, rows) &&
		    sess->surface)
			(void)fyai_ui_surface_publish(sess->surface, sess->view);
	for (job = ctx->tool_jobs; job; job = job->next)
		if (job->view &&
		    !fyai_terminal_view_set_history(job->view, rows) &&
		    job->surface)
			(void)fyai_ui_surface_publish(job->surface, job->view);
}

bool fyai_tools_focus_tile(struct fyai_ctx *ctx, struct fytim_surface *sf)
{
	struct fytim_surface *tiles[FYAI_WORKPANE_TILES_MAX];
	int n, i;

	if (!ctx || !sf)
		return false;
	/* Only visible tiles may receive keyboard focus. */
	n = fyai_workpane_screen_order(ctx->workpane, tiles,
				       FYAI_WORKPANE_TILES_MAX);
	for (i = 0; i < n && tiles[i] != sf; i++)
		;
	if (i == n)
		return false;
	if (fyai_workpane_focused(ctx->workpane) == sf)
		return true;
	return fyai_tools_focus(ctx, sf);
}

void fyai_tools_focus_prompt(struct fyai_ctx *ctx)
{
	/* Preserve zoom and attachment state while returning focus. */
	if (ctx)
		fyai_workpane_clear_focus(ctx->workpane);
}

/* What Ctrl-T can give the keys to: a block of the transcript or a tile,
 * where the last frame drew it. */
struct fyai_focus_target {
	int row, col, seq;
	uintptr_t key;			/* a block of the transcript, or 0 */
	struct fytim_surface *sf;	/* a tile, or NULL */
};

static int fyai_focus_target_cmp(const void *a, const void *b)
{
	const struct fyai_focus_target *x = a, *y = b;

	if (x->row != y->row)
		return x->row < y->row ? -1 : 1;
	if (x->col != y->col)
		return x->col < y->col ? -1 : 1;
	return x->seq - y->seq;
}

/*
 * The places that take the keys, in the order the screen shows them: from
 * the top row down, and from left to right in a row. A block or a tile that
 * the page did not place goes after those it did, in the order it opened.
 */
static int fyai_focus_targets(struct fyai_ctx *ctx,
			      struct fyai_focus_target *out, int max)
{
	struct fytim_surface *tiles[FYAI_WORKPANE_TILES_MAX];
	struct fyai_shell_session *sess;
	struct fyai_tool_job *job;
	uintptr_t key;
	int n = 0, ntiles, i, row, col, at;

	for (sess = ctx->shell_sessions; sess; sess = sess->next) {
		if (!sess->inline_block || sess->exited || n >= max)
			continue;
		out[n].key = (uintptr_t)sess;
		out[n].sf = NULL;
		n++;
	}
	for (job = ctx->tool_jobs; job; job = job->next) {
		if (!job->inline_block || job->done || n >= max)
			continue;
		out[n].key = (uintptr_t)job;
		out[n].sf = NULL;
		n++;
	}
	for (i = 0; i < n; i++) {
		key = out[i].key;
		at = fyai_ui_inline_index(ctx, key);
		out[i].seq = at;
		if (fyai_ui_inline_pos(ctx, key, &row, &col)) {
			out[i].row = row;
			out[i].col = col;
		} else {
			out[i].row = INT_MAX / 2 + at;
			out[i].col = 0;
		}
	}
	ntiles = fyai_workpane_screen_order(ctx->workpane, tiles,
					    FYAI_WORKPANE_TILES_MAX);
	for (i = 0; i < ntiles && n < max; i++, n++) {
		out[n].key = 0;
		out[n].sf = tiles[i];
		out[n].seq = FYAI_WORKPANE_TILES_MAX + i;
		if (!fyai_ui_tile_pos(ctx, tiles[i], &out[n].row,
				      &out[n].col)) {
			out[n].row = INT_MAX / 2 + FYAI_WORKPANE_TILES_MAX + i;
			out[n].col = 0;
		}
	}
	qsort(out, (size_t)n, sizeof(*out), fyai_focus_target_cmp);
	return n;
}

/* The owner of the live block of @key: a session or a sub-agent. */
static void fyai_inline_owner(struct fyai_ctx *ctx, uintptr_t key,
			      struct fyai_shell_session **sessp,
			      struct fyai_tool_job **jobp)
{
	struct fyai_shell_session *sess;
	struct fyai_tool_job *job;

	*sessp = NULL;
	*jobp = NULL;
	if (!key)
		return;
	for (sess = ctx->shell_sessions; sess; sess = sess->next)
		if ((uintptr_t)sess == key && sess->inline_block) {
			*sessp = sess;
			return;
		}
	for (job = ctx->tool_jobs; job; job = job->next)
		if ((uintptr_t)job == key && job->inline_block) {
			*jobp = job;
			return;
		}
}

/* Draw the block of @key again, as the focus edge moved to it or from it. */
static void fyai_inline_repaint(struct fyai_ctx *ctx, uintptr_t key)
{
	struct fyai_shell_session *sess;
	struct fyai_tool_job *job;

	fyai_inline_owner(ctx, key, &sess, &job);
	if (sess)
		fyai_shell_session_inline_paint(sess, false);
	else if (job)
		fyai_agent_inline_paint(job, false, true);
}

static void fyai_inline_keys(void *user, const char *data, size_t len);

/* Give the keys to the block of @key, or with 0 back to the prompt. */
static bool fyai_inline_focus(struct fyai_ctx *ctx, uintptr_t key)
{
	uintptr_t was = fyai_ui_inline_focused(ctx);
	int rc;

	rc = fyai_ui_inline_keys(ctx, key, fyai_inline_keys, ctx);
	if (was && was != key)
		fyai_inline_repaint(ctx, was);
	if (key && !rc)
		fyai_inline_repaint(ctx, key);
	return !rc;
}

/*
 * What is typed while a block of the transcript holds the keys goes to its
 * program. Ctrl-T moves the keys on, and Ctrl-] gives them to the prompt; what
 * follows that key goes to the new owner.
 */
static void fyai_inline_keys(void *user, const char *data, size_t len)
{
	struct fyai_ctx *ctx = user;
	struct fyai_shell_session *sess;
	struct fyai_tool_job *job;
	size_t i;

	fyai_inline_owner(ctx, fyai_ui_inline_focused(ctx), &sess, &job);
	if (!sess && !job) {
		(void)fyai_inline_focus(ctx, 0);
		(void)fyai_ui_keys_return(ctx, data, len);
		return;
	}
	for (i = 0; i < len; i++) {
		if (data[i] != FYAI_FOCUS_NEXT_KEY &&
		    data[i] != FYAI_FOCUS_PROMPT_KEY)
			continue;
		if (i)
			fyai_tools_zoom_write(sess, job, data, i);
		if (data[i] == FYAI_FOCUS_NEXT_KEY)
			(void)fyai_tools_focus_next(ctx);
		else
			(void)fyai_inline_focus(ctx, 0);
		if (i + 1 < len)
			(void)fyai_ui_keys_return(ctx, data + i + 1,
						  len - i - 1);
		return;
	}
	fyai_tools_zoom_write(sess, job, data, len);
}

/*
 * Move the keys on to the next place that takes them in the order the screen
 * shows them, blocks of the transcript and tiles alike, and from the last
 * back to the prompt. A zoomed tile is the whole cycle.
 */
bool fyai_tools_focus_next(struct fyai_ctx *ctx)
{
	struct fyai_focus_target targets[2 * FYAI_WORKPANE_TILES_MAX];
	struct fytim_surface *focused;
	uintptr_t cur;
	int n, i;

	if (!ctx)
		return false;
	fyai_workpane_set_keys_router(ctx->workpane, fyai_tools_zoom_keys, ctx);
	if (fyai_workpane_zoomed(ctx->workpane))
		return fyai_workpane_focus_next(ctx->workpane);
	cur = fyai_ui_inline_focused(ctx);
	focused = fyai_workpane_focused(ctx->workpane);
	n = fyai_focus_targets(ctx, targets, 2 * FYAI_WORKPANE_TILES_MAX);
	for (i = 0; i < n; i++)
		if ((cur && targets[i].key == cur) ||
		    (!cur && focused && targets[i].sf == focused))
			break;
	/* From the prompt the cycle starts at the first place. */
	i = (cur || focused) ? i + 1 : 0;
	if (i >= n) {
		if (!cur && !focused)
			return false;
		(void)fyai_inline_focus(ctx, 0);
		fyai_workpane_clear_focus(ctx->workpane);
		return true;
	}
	if (targets[i].key)
		return fyai_inline_focus(ctx, targets[i].key);
	(void)fyai_inline_focus(ctx, 0);
	return fyai_tools_focus(ctx, targets[i].sf);
}

fy_generic fyai_tools_sessions_data(struct fyai_ctx *ctx,
				    struct fy_generic_builder *gb)
{
	struct fyai_shell_session *sess;
	struct fyai_tool_job *job;
	fy_generic rows, agents, agent;

	rows = fy_gb_sequence(gb);
	agents = fyai_agents_rows(ctx, gb);
	fy_foreach(agent, agents) {
		if (!fy_get(agent, "active", false) ||
		    fy_get(agent, "parent", 0LL) == ctx->agent_execution)
			continue;
		rows = fy_append(gb, rows, fy_mapping(gb,
			"name", fy_get(agent, "branch", ""), "kind", "agent",
			"state", fy_get(agent, "state", "running")));
	}
	for (sess = ctx->shell_sessions; sess; sess = sess->next) {
		if (sess->exited || !sess->surface)
			continue;
		rows = fy_append(gb, rows, fy_mapping(gb,
			"name", sess->name,
			"kind", "shell",
			"state", sess->closing ? "stopping" :
				fyai_workpane_focused(ctx->workpane) ==
					sess->surface ? "focused" :
				"running"));
	}
	for (job = ctx->tool_jobs; job; job = job->next) {
		if (!job->agent || (job->done && !job->btw_panel))
			continue;
		rows = fy_append(gb, rows, fy_mapping(gb,
			"name", fyai_agent_job_name(job),
			"kind", job->btw ? "btw" : "agent",
			"state", job->btw_panel ? "finished" :
				job->terminating ? "stopping" :
				fyai_workpane_focused(ctx->workpane) ==
					job->surface ? "focused" :
				job->wants_input ? "waiting" : "running"));
	}
	return rows;
}

int fyai_tools_kill(struct fyai_ctx *ctx, const char *name,
		    const char **actionp)
{
	struct fyai_shell_session *sess = NULL, *candidate;
	struct fyai_tool_job *agent;

	if (!ctx || fy_str_empty(name)) {
		fyai_error(ctx, "kill: a session name is required");
		return -1;
	}
	if (fyai_agents_ambiguous(ctx, name)) {
		fyai_error(ctx, "kill: agent name '%s' is ambiguous; use its full branch name", name);
		return -1;
	}
	for (candidate = ctx->shell_sessions; candidate;
	     candidate = candidate->next)
		if ((!candidate->exited ||
		     (candidate->keep_tile && candidate->surface)) &&
		    !strcmp(candidate->name, name)) {
			sess = candidate;
			break;
		}
	agent = fyai_agent_job_named(ctx, name);
	if (!agent)
		for (agent = ctx->tool_jobs; agent; agent = agent->next)
			if (agent->btw_panel &&
			    (!strcmp(agent->branch, name) ||
			     !strcmp(fyai_agent_job_name(agent), name)))
				break;
	if (sess && agent) {
		fyai_error(ctx, "kill: '%s' names both a shell and a sub-agent",
			   name);
		return -1;
	}
	if (!sess && !agent) {
		if (!fyai_agents_kill(ctx, name)) {
			*actionp = "stopping agent";
			return 0;
		}
		fyai_error(ctx, "kill: no active shell session or sub-agent is "
			   "called '%s'", name);
		return -1;
	}
	if (sess && sess->exited) {
		fyai_shell_session_dismiss(sess);
		*actionp = "closed shell";
	} else if (sess) {
		fyai_shell_session_close(sess, false);
		*actionp = "stopping shell";
	} else {
		if (agent->btw_panel) {
			fyai_tool_job_discard(agent);
			*actionp = "closed btw panel";
		} else {
			fyai_tool_job_cancel(agent);
			*actionp = "stopping agent";
		}
	}
	return 0;
}

/*
 * Start @command as a user-owned terminal session in the work pane, named
 * @prefix-N. @what names the program in a diagnostic. *@sessp is the session
 * of the job, or NULL when the job has none.
 */
static int fyai_tools_user_start(struct fyai_ctx *ctx, const char *command,
				 const char *prefix, const char *what,
				 fy_generic env_keep,
				 struct fyai_shell_session **sessp)
{
	struct fyai_tool_job *job = NULL;
	struct fy_generic_builder *gb;
	fy_generic call;
	fy_generic args;
	const char *args_text;
	char name[32];
	unsigned int n;

	*sessp = NULL;
	if (!ctx)
		return -1;
	fyai_error_check(ctx, fyai_ui_active(ctx), err,
			 "a %s needs an interactive terminal UI", what);
	/* A key event starts a program between turns, with no turn storage. */
	gb = fyai_ctx_transient_gb(ctx);
	fyai_error_check(ctx, gb, err,
			 "could not make scratch storage for the %s", what);
	/* Allocate a unique user-visible session name. */
	for (n = 1; n < 1000000; n++) {
		snprintf(name, sizeof(name), "%s-%u", prefix, n);
		if (!fyai_shell_session_find(ctx, name))
			break;
	}
	fyai_error_check(ctx, n != 1000000, err,
			 "could not allocate a name for the %s", what);
	args = fy_mapping(gb,
			  "command", command ? command : "",
			  "tty", true,
			  "name", name,
			  "_fyai_user_owned", true);
	/* A program of the configuration runs as the catalogue verb does. */
	if (fy_is_valid(args) && fy_is_sequence(env_keep))
		args = fy_assoc(gb, args, "_fyai_env_keep", env_keep,
				"_fyai_unconfined", true);
	args_text = emit_json_string(gb, args);
	if (ctx->cfg->api_mode == FYAI_API_CHAT_COMPLETIONS)
		call = fy_mapping(gb,
				  "type", "function",
				  "function", fy_mapping(gb,
					  "name", "shell",
					  "arguments", args_text ? args_text : ""));
	else
		call = fy_mapping(gb,
				  "type", "function_call",
				  "name", "shell",
				  "arguments", args_text ? args_text : "");
	fyai_error_check(ctx, fy_is_valid(call), err,
			 "could not build the %s request", what);
	/* Retain the call beyond transient interactive storage. */
	call = fy_gb_internalize(ctx->gb, call);
	fyai_error_check(ctx, fy_is_valid(call), err,
			 "could not retain the %s request", what);
	job = fyai_tool_job_submit(ctx, call);
	fyai_error_check(ctx, job, err, "could not start the %s", what);
	*sessp = job->session;
	return 0;

err:
	return -1;
}

int fyai_tools_bang(struct fyai_ctx *ctx, const char *command)
{
	struct fyai_shell_session *sess;

	if (fyai_tools_user_start(ctx, command, "bang", "bang shell",
				  fy_invalid, &sess))
		return -1;
	if (sess)
		sess->keep_tile = true;
	if (sess && sess->surface)
		(void)fyai_tools_focus(ctx, sess->surface);
	return 0;
}

static struct fyai_shell_session *
fyai_tools_program_start(struct fyai_ctx *ctx, const char *command,
			 const char *prefix, const char *what,
			 fy_generic env_keep, fyai_tools_exit_fn done,
			 void *userdata)
{
	struct fyai_shell_session *sess;

	if (fyai_tools_user_start(ctx, command, prefix, what, env_keep, &sess))
		return NULL;
	fyai_error_check(ctx, sess, err,
			 "the %s was given no terminal session", what);
	/* The end is reported from the loop, so it cannot come before this. */
	sess->on_exit = done;
	sess->on_exit_data = userdata;
	if (sess->surface) {
		(void)fyai_tools_focus(ctx, sess->surface);
		(void)fyai_ui_surface_zoom(ctx, sess->surface);
	}
	return sess;

err:
	return NULL;
}

struct fyai_shell_session *
fyai_tools_user_program(struct fyai_ctx *ctx, const char *command,
			fyai_tools_exit_fn done, void *userdata)
{
	return fyai_tools_program_start(ctx, command, "edit", "editor",
					fy_invalid, done, userdata);
}

struct fyai_shell_session *
fyai_tools_config_program(struct fyai_ctx *ctx, const char *command,
			  const char *prefix, fy_generic env_keep,
			  fyai_tools_exit_fn done, void *userdata)
{
	return fyai_tools_program_start(ctx, command, prefix, prefix,
					fy_is_sequence(env_keep) ? env_keep :
					fy_seq_empty, done, userdata);
}

void fyai_tools_user_program_close(struct fyai_shell_session *sess)
{
	fyai_shell_session_close(sess, false);
}

void fyai_tools_user_program_forget(struct fyai_shell_session *sess)
{
	if (!sess)
		return;
	sess->on_exit = NULL;
	sess->on_exit_data = NULL;
}

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

/* Mark an overdue agent; shell limits still terminate immediately. */
static enum fyai_event_action
fyai_tool_job_deadline(const struct fyai_event *ev)
{
	struct fyai_tool_job *job = ev->userdata;

	job->deadline = NULL;
	if (job->agent) {
		job->overdue = true;
		if (job->surface)
			fyai_agent_head_paint(job);
		return FYAIEA_CONTINUE;
	}
	job->timed_out = true;
	fyai_tool_job_cancel(job);
	return FYAIEA_CONTINUE;
}

static enum fyai_event_action
fyai_tool_job_hang_deadline(const struct fyai_event *ev)
{
	struct fyai_tool_job *job = ev->userdata;

	job->hang_deadline = NULL;
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
		fyai_tool_job_live_close(job, false);
		fyai_tool_job_drop(&job->deadline);
		fyai_tool_job_drop(&job->hang_deadline);
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
	if (!job->btw_panel)
		fyai_tool_job_live_close(job, false);
	fyai_tool_job_close_channel(job);
	fyai_tool_job_drop(&job->deadline);
	fyai_tool_job_drop(&job->hang_deadline);
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
			job->view_entry_failed = job->view_run && !job->have_result &&
				WIFEXITED(status) &&
				WEXITSTATUS(status) == FYAI_SHELL_EXIT_SANDBOX;
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
	} else if (job->view_entry_failed) {
		fyai_error(ctx, "[%s] could not enter its view: the mount setup "
			   "failed; run it without isolation",
			   job->origin ? job->origin : "agent");
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
		if (job->agent && job->timeout_ms)
			reason = fy_sprintfa("tool error: agent hang timeout "
				"after %u ms (%u ms advisory limit and %u ms grace)",
				job->timeout_ms + job->hang_timeout_ms,
				job->timeout_ms, job->hang_timeout_ms);
		else if (job->agent)
			reason = fy_sprintfa("tool error: agent hang timeout "
				"after %u ms", job->hang_timeout_ms);
		else
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
	if (job->agent && job->overdue && !job->timed_out) {
		fyai_notice(ctx, "[%s] exceeded the %u ms advisory limit; "
			    "completed after %lld ms",
			    job->origin ? job->origin : "agent", job->timeout_ms,
			    (long long)job->elapsed_ms);
		if (fy_is_string(result))
			result = fy_stringf(ctx->transient_gb, "%s\n\n"
				"[Agent exceeded its %u ms advisory limit; "
				"completed after %lld ms.]",
				fy_castp(&result, ""), job->timeout_ms,
				(long long)job->elapsed_ms);
	}
	if (job->view_run)
		fyai_agent_view_finish(ctx, job, &result);
	*okp = job->result_ok && !job->failed;
	if (job->btw_panel) {
		job->group = NULL;
		return result;
	}
	/* The result lives in the builder of the job, which goes with it. */
	if (job->call_gb && fy_is_valid(result))
		result = fy_gb_internalize(fyai_ctx_transient_gb(ctx), result);
	fyai_tool_job_unlink(job->ctx, job);
	free(job->progress.data);
	free(job->branch);
	free(job->origin);
	fy_generic_builder_destroy(job->call_gb);
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
		/* Keep a side answer in its tile until the user dismisses it. */
		entry->job->btw_panel = entry->job->btw && entry->job->surface;
		fyai_tool_job_live_close(entry->job, false);
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
	rc = fyai_env_sanitize(NULL);
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
