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

#ifdef __FILC__
#include <stdfil.h>
#endif
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>

#include <fcntl.h>
#include <termios.h>
#include "fyai_reqcache.h"
#include "fyai_host.h"
#include <sys/ioctl.h>
#include <sys/wait.h>

#include "fyai_transport_boot.h"
#include "fyai_transport_sock.h"
#include "fyai_agent.h"
#include "fyai_monitor.h"
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
#include "fyai_sandbox_profile.h"
#include "fyai_session.h"
#include "fyai_storage.h"
#include "fyai_terminal.h"
#include "fyai_terminal_session.h"
#include "fyai_output.h"
#include "fyai_ask.h"
#include "fyai_skill.h"
#include "fyai_settings.h"
#include "fyai_todo.h"
#include "fyai_home.h"
#include "fyai_tools.h"
#include "fyai_tools_int.h"
#include "fyai_orphan.h"
#include "fyai_view.h"
#include "fyai_cover.h"
#include "fyai_fsview.h"
#include "fyai_asks.h"
#include "fyai_wait.h"
#include "fyai_prof.h"
#include "fyai_render.h"
#include "fyai_sink.h"
#include "fyai_ui.h"
#include "fyai_uimodel.h"
#include "fyai_uilog.h"
#include "fyai_uiwork.h"
#include "fyai_uitranscript.h"
#include "fyai_workpane.h"

const char *fyai_tool_name_canonical(const char *name)
{
	return name && !strcmp(name, FYAI_TOOL_EXEC_WIRE_NAME) ? "shell" : name;
}

const char *fyai_tool_call_name(struct fyai_ctx *ctx, fy_generic tool_call)
{
	fy_generic v;

	if (fy_equal(fy_get(tool_call, "type"), "shell_call"))
		return "shell";

	v = fy_get(tool_call, "name");
	return fyai_tool_name_canonical(
		fy_gb_intern_string(ctx->transient_gb, fy_cast(v, "")));
}

/*
 * The shell tool takes its command as "command". Codex calls it "cmd", and
 * models that learned that name send it. Treat it as the same argument.
 */
static fy_generic fyai_tool_args_normalize(struct fy_generic_builder *gb,
					   const char *name, fy_generic args)
{
	fy_generic cmd;

	if (!name || strcmp(fyai_tool_name_canonical(name), "shell") ||
	    !fy_is_mapping(args) || fy_is_valid(fy_get(args, "command",
						       fy_invalid)))
		return args;
	cmd = fy_get(args, "cmd", fy_invalid);
	if (!fy_is_string(cmd))
		return args;
	args = fy_assoc(gb, args, "command", cmd);
	return fy_disassoc(gb, args, "cmd");
}

/* True when a call of the shell tool names no command to run. */
static bool fyai_shell_args_lack_command(const char *name, fy_generic args)
{
	return name && !strcmp(fyai_tool_name_canonical(name), "shell") &&
	       fy_is_mapping(args) &&
	       !fy_is_string(fy_get(args, "command", fy_invalid));
}

#define FYAI_SHELL_NO_COMMAND \
	"tool error: the shell tool needs a string argument \"command\""

fy_generic fyai_tool_call_args(struct fyai_ctx *ctx, fy_generic tool_call)
{
	const char *args_text;

	args_text = fy_get(tool_call, "arguments", "");
	return fyai_tool_args_normalize(fyai_ctx_transient_gb(ctx),
		fyai_tool_call_name(ctx, tool_call),
		parse_json_string(fyai_ctx_transient_gb(ctx), args_text));
}

const char *fyai_tool_call_id(struct fyai_ctx *ctx, fy_generic tool_call)
{
	fy_generic id;

	id = fy_get(tool_call, "call_id", fy_invalid);
	return fy_gb_intern_string(fyai_ctx_transient_gb(ctx),
				   fy_castp(&id, ""));
}

fy_generic fyai_tool_call_set_args(struct fyai_ctx *ctx, fy_generic tool_call,
				   fy_generic args)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	const char *json;

	/* A native shell call has no arguments of a function to replace. */
	if (fy_equal(fy_get(tool_call, "type"), "shell_call"))
		return tool_call;
	json = emit_json_string(gb, args);
	if (!json)
		return fy_invalid;
	return fy_assoc(gb, tool_call, "arguments", fy_value(gb, json));
}

/*
 * The call that @tool_call runs as: the call itself, or the call that the
 * expand function of its tool makes of it, with the same id. The scheduler
 * decides on this call and the job runs it; the turn keeps @tool_call, so
 * the result answers the call that the model made.
 */
static fy_generic fyai_tool_call_effective(struct fyai_ctx *ctx,
					   fy_generic tool_call)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	const struct fyai_tool_def *def;
	const char *name, *call_id, *json;
	fy_generic args, id;

	if (fy_equal(fy_get(tool_call, "type"), "shell_call"))
		return tool_call;
	def = fyai_tool_find(fyai_tool_call_name(ctx, tool_call));
	if (!def || !def->expand)
		return tool_call;
	args = fyai_tool_call_args(ctx, tool_call);
	if (!fy_is_mapping(args))
		return tool_call;
	id = fy_get(tool_call, "call_id", fy_invalid);
	call_id = fy_castp(&id, "");
	if (!def->expand(ctx, args, call_id, &name, &args))
		return tool_call;
	json = emit_json_string(gb, args);
	if (!json)
		return tool_call;
	return fy_assoc(gb, tool_call, "name", fy_value(gb, name),
			"arguments", fy_value(gb, json));
}

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
	/* The name of a command in the background names a monitor, not a session. */
	if (fy_get(args, "background", false))
		return false;
	name = fy_get(args, "name", "");
	return !fy_str_empty(name);
}

/* A named shell is a session only when it requested a terminal. */
bool fyai_shell_session_call(struct fyai_ctx *ctx, fy_generic tool_call)
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
			   struct fyai_buf *body)
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
				 struct fyai_buf *body)
{
	if (title && *title)
		(void)fyai_sink_markdown(ctx->sink, FYAI_SINK_STATUS, title);
	if (body->len)
		(void)fyai_sink_write(ctx->sink, FYAI_SINK_STATUS,
				      body->data, body->len);
}

char *fyai_format_shell_label(fy_generic args)
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
			   char **title, struct fyai_buf *body,
			   fy_generic *sourcep)
{
	const char *resolved;
	fy_generic args;

	args = fyai_tool_call_args(ctx, tool_call);
	resolved = fyai_patch_display_text(ctx, tool_call);
	if (resolved)
		args = fy_mapping("patch", resolved);
	return fyai_tool_call_view_source(ctx, "apply_patch", args,
			fyai_tool_preview_lines(ctx->cfg, "apply_patch"),
			title, body, ctx->transient_gb, sourcep);
}

void fyai_patch_band_refresh(struct fyai_ctx *ctx, fy_generic tool_call)
{
	struct fyai_buf body = {0};
	fy_generic source;
	char *title = NULL;

	if (!fyai_sink_bands_available(ctx->sink) || !ctx->cfg->markdown)
		return;
	if (!fyai_patch_display_text(ctx, tool_call))
		return;
	if (!patch_band_view(ctx, tool_call, &title, &body, &source) &&
	    body.len) {
		fyai_sink_band_paint(fyai_sink_band_shared(ctx->sink), NULL,
				     NULL, body.data, body.len, NULL);
		fyai_uiwork_band_view(ctx, fyai_sink_band_shared(ctx->sink),
				      source);
	}
	free(body.data);
	free(title);
}

void fyai_print_tool_call(struct fyai_ctx *ctx, fy_generic tool_call,
			  bool execute)
{
	struct fyai_cfg *cfg = ctx->cfg;
	const char *name;
	const char *command;
	struct fyai_buf body = {0};
	char *header;
	fy_generic args, source;
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
		args = fyai_tool_call_args(ctx, tool_call);
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
					     fyai_sink_bands_available(ctx->sink) ||
					     terminal_is_tty(STDERR_FILENO)) != 0) {
			free(ctx->shell_stream);
			ctx->shell_stream = NULL;
		}
		ctx->tool_output_displayed = true;
	} else if (cfg->markdown && fyai_sink_bands_available(ctx->sink) &&
		   fy_equal(name, "apply_patch")) {
		/* Show the patch in a marked work band. */
		if (patch_band_view(ctx, tool_call, &header, &body, &source))
			header = NULL;
		fyai_sink_band_open(ctx->sink, true,
				    header ? header : "**patch**", NULL);
		if (body.len) {
			fyai_sink_band_paint(fyai_sink_band_shared(ctx->sink),
					     NULL, NULL, body.data, body.len,
					     NULL);
			fyai_uiwork_band_view(ctx,
				fyai_sink_band_shared(ctx->sink), source);
		}
		free(body.data);
		free(header);
		ctx->tool_output_displayed = true;
	} else if (cfg->markdown && fyai_sink_bands_available(ctx->sink) &&
		   fy_any_equal(name, "read_file", "write_file")) {
		args = fyai_tool_call_args(ctx, tool_call);
		if (fyai_tool_call_view_source(ctx, name, args,
					fyai_tool_preview_lines(cfg, name),
					&header, &body, ctx->transient_gb,
					&source))
			header = NULL;
		fyai_sink_band_open(ctx->sink, true, header ? header : name,
				    NULL);
		if (body.len) {
			fyai_sink_band_paint(fyai_sink_band_shared(ctx->sink),
					     NULL, NULL, body.data, body.len,
					     NULL);
			fyai_uiwork_band_view(ctx,
				fyai_sink_band_shared(ctx->sink), source);
		}
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
	/*
	 * A monitor does not hold the turn, and the monitor tool has bounded its limit
	 * already: the ceiling of a foreground call would cut it short, and the
	 * model was told the limit it asked for.
	 */
	if (cfg->shell_max_timeout_ms > 0 && ms > cfg->shell_max_timeout_ms &&
	    !fy_is_valid(fy_get(call, "_fyai_monitor", fy_invalid)))
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
size_t fyai_shell_output_bytes(struct fyai_ctx *ctx, fy_generic call)
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

void fyai_shell_tty_size(struct fyai_ctx *ctx, fy_generic call,
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
bool fyai_shell_tty_requested(struct fyai_ctx *ctx, fy_generic call)
{
	fy_generic tty;

	tty = fy_get(call, "tty", fy_invalid);
	if (fy_generic_is_bool(tty))
		return fy_castp(&tty, (_Bool)false);
	return ctx->cfg->shell_tty;
}

/*
 * The shell of a tool call that names none and for which shell/shell is empty:
 * bash where the system has it, so a command that the model writes meets one
 * dialect on every host, and else the empty string, which runs /bin/sh.
 */
static const char *fyai_shell_default(void)
{
	static const char *const bash[] = {
		"/bin/bash", "/usr/bin/bash", "/usr/local/bin/bash", NULL,
	};
	const char *const *p;

	for (p = bash; *p; p++)
		if (!access(*p, X_OK))
			return *p;
	return "";
}

/* Read the call's shell, falling back to shell/shell, then to the default. */
const char *fyai_shell_shell_requested(struct fyai_ctx *ctx,
				       fy_generic call)
{
	const char *shell;

	/*
	 * The typed accessor addresses the stored item, so the pointer is
	 * that of @call and not that of a copy. It lives as long as the call.
	 */
	shell = fy_get(call, "shell", "");
	if (!fy_str_empty(shell))
		return shell;
	return !fy_str_empty(ctx->cfg->shell_shell) ? ctx->cfg->shell_shell :
							fyai_shell_default();
}

bool fyai_shell_login_requested(struct fyai_ctx *ctx, fy_generic call)
{
	fy_generic login;

	login = fy_get(call, "login", fy_invalid);
	if (fy_generic_is_bool(login))
		return fy_castp(&login, (_Bool)false);
	return ctx->cfg->shell_login;
}

/* Keep the end of a stream and report the number of omitted bytes. */
char *fyai_shell_bound_alloc(const char *text, size_t max_bytes)
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

/* A command in the background is kept in the parent: it returns at once. */
static bool fyai_shell_background_requested(fy_generic args)
{
	return fy_get(args, "background", false);
}

static char *fyai_run_shell_command(struct fyai_ctx *ctx, fy_generic args,
				    bool *okp)
{
	struct fyai_cfg *cfg = ctx->cfg;
	struct shell_command_result result = {};
	struct shell_command_opts opts = {};
	struct fyai_buf buf = {};
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
	/* The invocation keeps a command in the background; no child runs it. */
	if (!ctx->cfg->tool_child && fy_get(args, "background", false))
		return fyai_monitor_background(ctx, args, okp);
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

		rc = fyai_buf_append(&buf, tty_text);
		free(tty_text);
		fyai_error_check(ctx, !rc, out,
				 "shell: could not retain terminal output");
		if (tty_result.binary) {
			rc = fyai_buf_append(&buf, "\n");
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
				"\ntool error: " FYAI_SHELL_EXIT_FMT "\n",
				tty_result.exit_code);
		} else {
			msg = NULL;
			*okp = true;
		}
		if (msg && fyai_buf_append(&buf, msg))
			goto out;

		ret = buf.data;
		buf.data = NULL;
		goto out;
	}

	/* A program that draws on a pipe gets the screen that a terminal run would. */
	fyai_shell_tty_size(ctx, args, &opts.rows, &opts.cols);
	opts.max_bytes = fyai_shell_output_bytes(ctx, args);
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
		if (fyai_buf_append(&buf, msg))
			goto out;
	} else {
		bounded = fyai_shell_bound_alloc(result.stdout_data, out_bytes);
		rc = fyai_buf_append(&buf, bounded ? bounded :
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
		if (fyai_buf_append(&buf, msg))
			goto out;
	} else {
		bounded = fyai_shell_bound_alloc(result.stderr_data, err_bytes);
		rc = fyai_buf_append(&buf, bounded ? bounded :
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
		if (fyai_buf_append(&buf, msg))
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
				"\ntool error: " FYAI_SHELL_EXIT_FMT "\n",
				result.exit_code);
		if (fyai_buf_append(&buf, msg))
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
 * Execute the `ask_user` tool: put the questions of the model to the user and
 * return their answers as the tool result. The input area of the page, a
 * parent invocation, --answer values, or a terminal line can supply them. When
 * no answer can be read, the model is told the user did not answer so it can
 * proceed.
 */
static fy_generic fyai_ask_declined(struct fyai_ctx *ctx)
{
	return fyai_ask_result(ctx->transient_gb, fy_invalid, NULL, 0);
}

/* Ask the parent to present the questions of a delegated sub-agent. */
static fy_generic fyai_ask_user_upward(struct fyai_ctx *ctx,
				       fy_generic questions)
{
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	struct jsonrpc_request *req;
	fy_generic result = fy_invalid;
	fy_generic params;

	fyai_agents_activity(ctx, "waiting for input");
	params = fy_mapping(gb, "questions", questions, "branch",
			    fyai_ctx_branch(ctx));
	req = jsonrpc_request_submit(ctx->tool_rpc, "user/ask", params,
				     jsonrpc_conn_next_id(ctx->tool_rpc),
				     false, NULL, NULL);
	if (!req) {
		fyai_error(ctx, "ask_user: could not put the questions to the "
			   "parent");
		return fy_value(gb, "tool error: the questions could not be "
				"asked");
	}
	/* The loop of this process serves the channel while it waits. */
	while (!jsonrpc_request_done(req)) {
		if (fyai_event_loop_step(fyai_ctx_loop(ctx), -1) < 0)
			break;
	}
	if (jsonrpc_request_ok(req) &&
	    fy_is_mapping(jsonrpc_request_result(req)))
		result = fy_gb_internalize(gb, jsonrpc_request_result(req));
	jsonrpc_request_destroy(req);
	fyai_agents_activity(ctx, "running");
	if (!fy_is_valid(result)) {
		fyai_error(ctx, "ask_user: the parent did not answer");
		return fyai_ask_declined(ctx);
	}
	return result;
}

/* The questions put through the input area of the page, and their replies. */
struct fyai_ask_wait {
	bool done;
	bool failed;
	bool given;
	struct fyai_ask_reply replies[FYAI_ASK_QUESTIONS_MAX];
	size_t n;
};

static void fyai_ask_user_done(void *user, const struct fyai_ask_reply *replies,
			       size_t n)
{
	struct fyai_ask_wait *w = user;
	size_t i;

	w->done = true;
	if (!replies)
		return;
	w->given = true;
	for (i = 0; i < n; i++) {
		w->replies[i].selected = replies[i].selected;
		if (!replies[i].other)
			continue;
		w->replies[i].other = strdup(replies[i].other);
		w->failed |= !w->replies[i].other;
	}
	w->n = n;
}

/* Ask through the input area of the page and wait for the answers. */
static fy_generic fyai_ask_user_page(struct fyai_ctx *ctx, fy_generic questions,
				     const char *from)
{
	struct fyai_ask_wait w = { 0 };
	fy_generic result;
	char *echo;

	if (fyai_ask_open(ctx, questions, from, fyai_ask_user_done, &w))
		return fy_value(ctx->transient_gb,
				"tool error: the questions could not be asked");
	while (!w.done) {
		if (fyai_event_loop_step(fyai_ctx_loop(ctx), -1) < 0)
			break;
	}
	if (!w.done) {
		fyai_ask_close(ctx, &w);
		fyai_error(ctx, "ask_user: the event loop stopped while waiting "
			   "for an answer");
		return fyai_ask_declined(ctx);
	}
	if (w.failed)
		fyai_error(ctx, "ask_user: could not keep the answer");
	echo = fyai_ask_echo(questions, w.given ? w.replies : NULL, w.n);
	if (echo)
		fyai_report(ctx, "\n%s", echo);
	free(echo);
	result = fyai_ask_result(ctx->transient_gb, questions,
				 w.given ? w.replies : NULL, w.n);
	fyai_ask_replies_free(w.replies, w.n);
	return result;
}

/*
 * Read the answers of a terminal that has no page: each question is
 * written, and its answer is the next --answer value or a line from the
 * terminal. A blank line declines every question.
 */
static fy_generic fyai_ask_user_lines(struct fyai_ctx *ctx, fy_generic questions,
				      const char *from)
{
	struct fyai_cfg *cfg = ctx->cfg;
	struct fyai_ask_reply replies[FYAI_ASK_QUESTIONS_MAX] = { 0 };
	fy_generic question, result;
	bool declined = false;
	const char *a;
	char *line, *text;
	size_t n = 0;
	int rc;

	fy_foreach(question, questions) {
		text = fyai_ask_text(question);
		if (!text) {
			fyai_error(ctx, "ask_user: cannot format a question");
			declined = true;
			break;
		}
		/* A sub-agent that asks is named. */
		if (from && *from && !n)
			fyai_report(ctx, "\nthe sub-agent '%s' asks:", from);
		if (ansi_color_on(cfg->color, STDERR_FILENO))
			fyai_report(ctx, "\n" FYAI_ANSI_BOLD "%s" FYAI_ANSI_RESET,
				    text);
		else
			fyai_report(ctx, "\n%s", text);
		free(text);

		/*
		 * Batch use: --answer values are consumed in order, one per
		 * question, instead of prompting. Echo the consumed answer so
		 * the transcript still reads sensibly.
		 */
		if (ctx->answer_next < cfg->answer_count) {
			a = cfg->answers[ctx->answer_next++];
			fyai_report(ctx, "> %s\n", a);
			line = strdup(a ? a : "");
		} else if (!terminal_is_tty(STDIN_FILENO)) {
			/*
			 * Nobody can be prompted, so an expected answer
			 * cannot be obtained. Flag the run to abort rather
			 * than let the model proceed on a guess.
			 */
			fyai_error(ctx, "ask_user: an answer is expected but none is "
				   "available (non-interactive; supply --answer)");
			ctx->ask_abort = true;
			fyai_ask_replies_free(replies, n);
			return fy_value(ctx->transient_gb, "tool error: no answer "
					"available (non-interactive)");
		} else {
			/* Editable input via linenoise. */
			line = fyai_readline(ctx, "> ");
		}
		if (!line) {
			declined = true;
			break;
		}
		rc = fyai_ask_reply_from_line(question, line, &replies[n]);
		free(line);
		if (rc) {
			fyai_error(ctx, "ask_user: could not retain the answer");
			declined = true;
			break;
		}
		if (!fyai_ask_reply_given(&replies[n])) {
			declined = true;
			break;
		}
		n++;
	}
	result = fyai_ask_result(ctx->transient_gb, questions,
				 declined ? NULL : replies, n);
	fyai_ask_replies_free(replies, n);
	return result;
}

/* Ask the user, for the sub-agent @from when it is not NULL. */
static fy_generic fyai_ask_user_from(struct fyai_ctx *ctx, fy_generic args,
				     const char *from)
{
	struct fyai_cfg *cfg = ctx->cfg;
	fy_generic questions, gname, result;
	const char *name;
	char why[160];
	char *text;

	questions = fyai_ask_normalize(ctx->transient_gb, args, why, sizeof(why));
	if (fy_is_invalid(questions))
		return fy_value(ctx->transient_gb,
				fy_sprintfa("tool error: ask_user: %s", why));
	if (!from && fyai_agent_delegated(ctx) && ctx->tool_rpc)
		return fyai_ask_user_upward(ctx, questions);

	/* The page renderer puts the questions in its input area. */
	if (ctx->answer_next >= cfg->answer_count && fyai_ui_ask_available(ctx)) {
		gname = fy_get(args, "name", fy_invalid);
		name = fy_castp(&gname, "");
		/* A background question does not hold the turn. */
		if (!from && fyai_asks_background(args)) {
			text = fyai_asks_start(ctx, name, questions);
			if (!text)
				return fy_value(ctx->transient_gb, "tool error: "
						"the questions could not be asked");
			result = fy_value(ctx->transient_gb, text);
			free(text);
			return result;
		}
		return fyai_ask_user_page(ctx, questions, from);
	}
	return fyai_ask_user_lines(ctx, questions, from);
}

static fy_generic fyai_ask_user(struct fyai_ctx *ctx, fy_generic args)
{
	if (fyai_agent_delegated(ctx))
		return fy_value(ctx->transient_gb,
			"tool error: ask_user is available only in the root session; use ask_parent");
	return fyai_ask_user_from(ctx, args, NULL);
}

static fy_generic fyai_view_tool(struct fyai_ctx *ctx, fy_generic args, bool *okp);

static fy_generic fyai_list_tool(struct fyai_ctx *ctx, fy_generic args, bool *okp);

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
	if (fy_equal(type, "shell_call") &&
	    !fyai_skill_tool_allowed(ctx, "shell"))
		return fy_value(ctx->transient_gb,
				"tool error: shell is not one of the tools of "
				"this skill");
	if (fy_equal(type, "shell_call")) {

		action = fy_get(tool_call, "action");
		commands = fy_get(action, "commands");
		shell_opts.timeout_ms = fyai_shell_timeout_ms(ctx, tool_call,
							      true);
		tty_call = fyai_shell_tty_requested(ctx, action);
		shell_opts.shell = fyai_shell_shell_requested(ctx, action);
		shell_opts.login = fyai_shell_login_requested(ctx, action);
		fyai_shell_tty_size(ctx, action, &shell_opts.rows, &shell_opts.cols);
		shell_opts.max_bytes = fyai_shell_output_bytes(ctx, action);
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

	name = fy_get(tool_call, "name", "");
	args = fyai_tool_call_args(ctx, tool_call);
	if (fy_is_invalid(args))
		return fy_value(ctx->transient_gb, "tool error: invalid JSON arguments");
	if (!fyai_skill_tool_allowed(ctx, fyai_tool_name_canonical(name)))
		return fy_value(ctx->transient_gb,
				fy_sprintfa("tool error: %s is not one of the "
					    "tools of this skill", name));
	/* A tool group submits an MCP call; no call runs one here. */
	if (fyai_mcp_tool_name(name))
		return fy_value(ctx->transient_gb,
				"tool error: MCP calls run in a tool group");

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
	int rc, err;

	(void)ctx;
	path = fy_get(args, "path", "");
	content = fy_get(args, "content", "");
	/* The directories above the file are made, as apply_patch makes them. */
	rc = fyai_patch_ensure_parent_dir(path);
	if (!rc)
		rc = write_text_file(path, content);
	err = errno;
	*okp = !rc;
	if (rc)
		return strdup(fy_sprintfa("tool error: write %s: %s", path,
					  strerror(err)));
	return strdup("ok");
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

static char *tool_list_format(struct fyai_ctx *ctx,
			      struct fy_generic_builder *gb, fy_generic args,
			      const char *result)
{
	static const char *const kinds[] = {
		"agents", "views", "shells", "waits", "monitors",
		"daemons", "questions", "messages",
	};
	fy_generic data, rows, shown, row, opts;
	char *out = NULL, *table;
	const char *kind = fy_get(args, "kind", "");
	size_t len = 0, i, n, j;
	FILE *mf;
	bool any = false;

	(void)ctx;
	data = parse_json_string(gb, result);
	if (!fy_is_mapping(data))
		return NULL;
	mf = open_memstream(&out, &len);
	if (!mf)
		return NULL;
	for (i = 0; i < ARRAY_SIZE(kinds); i++) {
		rows = fy_get(data, kinds[i], fy_invalid);
		if (!fy_is_sequence(rows))
			continue;
		n = fy_len(rows);
		if (!n && (!*kind || strcmp(kind, kinds[i])))
			continue;
		if (any)
			fputc('\n', mf);
		fprintf(mf, "**%c%s**\n\n", toupper((unsigned char)kinds[i][0]),
			kinds[i] + 1);
		shown = fy_sequence(gb);
		j = 0;
		fy_foreach(row, rows) {
			if (j++ == 30)
				break;
			shown = fy_append(gb, shown, row);
		}
		opts = fy_mapping(gb, "empty", "none");
		table = fyai_generic_markdown_source(opts, shown);
		if (table) {
			fputs(table, mf);
			free(table);
		}
		if (n > 30)
			fprintf(mf, "\n_%zu more._\n", n - 30);
		any = true;
	}
	if (!any)
		fputs("_Nothing to list._\n", mf);
	if (fclose(mf)) {
		free(out);
		return NULL;
	}
	return out;
}

static char *tool_project_view_format(struct fyai_ctx *ctx,
				      struct fy_generic_builder *gb,
				      fy_generic args, const char *result)
{
	fy_generic data, rows, shown, row, opts, keys;
	const char *action = fy_get(args, "action", "");
	char *out = NULL, *table;
	size_t len = 0, n = 0, total;
	FILE *mf;

	(void)ctx;
	data = parse_json_string(gb, result);
	rows = fy_get(data, "changes", fy_invalid);
	if (!fy_is_mapping(data) || !fy_is_sequence(rows))
		return NULL;
	total = fy_len(rows);
	mf = open_memstream(&out, &len);
	if (!mf)
		return NULL;
	if (!strcmp(action, "apply"))
		fprintf(mf, "%lld applied · %lld satisfied · %lld conflicts · %lld skipped\n\n",
			fy_get(data, "applied", 0LL), fy_get(data, "satisfied", 0LL),
			fy_get(data, "conflicts", 0LL), fy_get(data, "skipped", 0LL));
	shown = fy_sequence(gb);
	fy_foreach(row, rows) {
		if (n++ == 30)
			break;
		shown = fy_append(gb, shown, row);
	}
	keys = !strcmp(action, "apply") ?
		fy_sequence(gb, "path", "status", "action", "reason") :
		fy_sequence(gb, "path", "status");
	opts = fy_mapping(gb, "keys", keys, "empty", "no changes");
	table = fyai_generic_markdown_source(opts, shown);
	if (table) {
		fputs(table, mf);
		free(table);
	}
	if (total > 30)
		fprintf(mf, "\n_%zu more._\n", total - 30);
	if (fclose(mf)) {
		free(out);
		return NULL;
	}
	return out;
}

static fy_generic tool_ask_user(struct fyai_ctx *ctx, fy_generic args,
				bool *okp)
{
	fy_generic result = fyai_ask_user(ctx, args);

	*okp = !fy_is_string(result) ||
	       strncmp(fy_castp(&result, ""), "tool error:", 11) != 0;
	return fyai_tool_result_text(ctx, result);
}

/*
 * Execute the `todo_write` tool: replace the todo list of the branch with
 * the normalized list and publish it. The list is branch state, as the
 * configuration is, so a sub-agent branch inherits a copy of the list of
 * its parent and then diverges from it.
 */
static fy_generic tool_todo_write(struct fyai_ctx *ctx, fy_generic args,
				  bool *okp)
{
	fy_generic todos;
	char why[1024];
	char *text;

	todos = fyai_todo_normalize(ctx->transient_gb, args, why, sizeof(why));
	if (fy_is_invalid(todos)) {
		*okp = false;
		return fy_value(ctx->transient_gb,
				fy_sprintfa("tool error: todo_write: %s", why));
	}
	text = fyai_todo_commit(ctx, todos);
	if (!text) {
		*okp = false;
		return fy_value(ctx->transient_gb,
				"tool error: the todo list was not stored");
	}
	*okp = true;
	todos = fy_value(ctx->transient_gb, text);
	free(text);
	return todos;
}

static void tool_head_todo_write(struct fyai_ctx *ctx, FILE *mf,
				 struct fy_generic_builder *gb, fy_generic args,
				 int preview_lines, struct fyai_md_blocks *blocks)
{
	fy_generic todos = fy_get(args, "todos", fy_invalid);
	fy_generic first = fy_get_at(todos, 0);
	fy_generic gcontent = fy_get(first, "content", fy_invalid);

	(void)ctx;
	(void)gb;
	(void)preview_lines;
	(void)blocks;
	/* One row, as the head of every call is: the first item, and how
	 * many more there are. */
	fprintf(mf, "**todo** %s", fy_castp(&gcontent, ""));
	if (fy_len(todos) > 1)
		fprintf(mf, " (+%zu more)", fy_len(todos) - 1);
	fprintf(mf, "\n\n");
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
	fy_generic questions = fy_get(args, "questions", fy_invalid);
	fy_generic gq, gn = fy_get(args, "name", fy_invalid);
	const char *name = fy_castp(&gn, "");

	(void)ctx;
	(void)gb;
	(void)preview_lines;
	(void)blocks;
	/* One row, as the head of every call is: the first question, and how
	 * many more there are. */
	gq = fy_get(fy_get_at(questions, 0), "question", fy_invalid);
	fprintf(mf, "**ask** %s", fy_castp(&gq, ""));
	if (fy_len(questions) > 1)
		fprintf(mf, " (+%zu more)", fy_len(questions) - 1);
	if (*name)
		fprintf(mf, " [%s]", name);
	fprintf(mf, "\n\n");
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
	  .head = fyai_tool_head_shell, .in_parent = fyai_shell_background_requested,
	  .flags = FYAI_TOOL_MARKED | FYAI_TOOL_INSTANT,
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
	  .format_result = fyai_ask_format,
	  .in_parent = fyai_asks_background,
	  .flags = FYAI_TOOL_PARENT | FYAI_TOOL_INSTANT |
		   FYAI_TOOL_ROOT_ONLY },
	{ .name = "project_view", .run = tool_project_view,
	  .head = tool_head_project_view,
	  .format_result = tool_project_view_format,
	  .flags = FYAI_TOOL_PARENT | FYAI_TOOL_NOT_FOR_CHILD,
	  .effect = FYAI_TOOL_EFFECT_PROCESS },
	{ .name = "todo_write", .run = tool_todo_write,
	  .head = tool_head_todo_write, .format_result = fyai_todo_format_result,
	  .flags = FYAI_TOOL_PARENT },
	{ .name = "list", .run = tool_list, .head = tool_head_list,
	  .format_result = tool_list_format,
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
	args = fyai_tool_args_normalize(ctx->transient_gb, name, args);
	if (fyai_shell_args_lack_command(name, args))
		return fy_value(ctx->transient_gb, FYAI_SHELL_NO_COMMAND);
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
int fyai_tool_apply_sandbox(struct fyai_ctx *ctx)
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

int fyai_tools_confine_native_view(struct fyai_ctx *ctx, const struct fyai_fsview *view)
{
	struct fyai_shell_sandbox sb;
	const struct fyai_sandbox_spec *configured;
	struct fyai_sandbox_spec policy = { 0 };
	const char *previous_project = ctx->cfg->view_project;
	const char *previous_scratch = ctx->cfg->view_scratch;
	const char **deny = NULL, *scratch;
	const char *slash = strrchr(view->execution, '/');
	size_t i, count;
	int rc, saved;

	if (!slash || ctx->sandbox_applied) {
		errno = ENOTSUP;
		return -1;
	}
	scratch = fy_sprintfa("%.*s/tmp", (int)(slash - view->execution), view->execution);
	ctx->cfg->view_project = view->execution;
	ctx->cfg->view_scratch = scratch;
	rc = fyai_shell_sandbox_build(ctx, &sb, &configured);
	ctx->cfg->view_project = previous_project;
	ctx->cfg->view_scratch = previous_scratch;
	if (rc)
		return -1;
	if (configured)
		policy = *configured;
	count = policy.deny_n;
	deny = calloc(count + 2, sizeof(*deny));
	if (!deny) {
		rc = -1;
		goto out;
	}
	for (i = 0; i < count; i++)
		deny[i] = policy.deny[i];
	deny[count++] = view->project;
	if (view->arena)
		deny[count++] = view->arena;
	policy.project_root = view->execution;
	policy.scratch_root = scratch;
	policy.deny = deny;
	policy.deny_n = policy.deny_global_n = count;
	policy.strict = true;
	policy.filter_path = true;
	rc = fyai_sandbox_apply(&policy);
	if (!rc)
		ctx->sandbox_applied = true;
out:
	saved = errno;
	free(deny);
	fyai_shell_sandbox_end(&sb);
	errno = saved;
	return rc;
}

/*
 * Tell the model that a call left a process running, and when it ends. A
 * scope of one call ends the process with the call, so it needs no note.
 */
fy_generic fyai_tool_result_note(struct fyai_ctx *ctx, bool agent_call,
				 fy_generic result)
{
	const char *scope = ctx->cfg->sandbox_scope;
	struct fy_generic_builder *gb;
	fy_generic note;

	if (!fy_is_string(result) || !strcmp(scope, "call") || agent_call)
		return result;
	gb = fyai_ctx_transient_gb(ctx);
	note = fyai_orphan_note(gb, !strcmp(scope, "session") ?
				"They end with this session." :
				"They end with this exchange.");
	if (!fy_is_string(note))
		return result;
	return fy_stringf(gb, "%s\n%s", fy_castp(&result, ""),
			  fy_castp(&note, ""));
}

/* Keep the live jobs reachable, so that a resize finds every child. */
static void fyai_tool_job_link(struct fyai_ctx *ctx, struct fyai_tool_job *job)
{
	job->next = ctx->tool_jobs;
	ctx->tool_jobs = job;
	fyai_tools_work_update(ctx);
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
		fyai_tools_work_update(ctx);
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

/*
 * Cancel background work by name. The name must be unique among the waits,
 * the running sub-agents and the open sessions unless @kind says which.
 */
static char *fyai_cancel_tool(struct fyai_ctx *ctx, fy_generic args, bool *okp)
{
	struct fyai_tool_job *job;
	fy_generic name_v, kind_v;
	const char *name, *kind;
	bool is_wait, is_agent, is_shell, is_monitor, is_question, is_message;
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
	is_monitor = (!*kind || !strcmp(kind, "monitor")) &&
		     fyai_monitor_running(ctx, name);
	is_question = (!*kind || !strcmp(kind, "question")) &&
		      (fyai_asks_exists(ctx, name) ||
		       fyai_agents_parent_ask_pending(ctx, name));
	is_message = (!*kind || !strcmp(kind, "message")) &&
		     fyai_agents_message_pending(ctx, name);
	if (is_wait + is_agent + is_shell + is_monitor + is_question +
	    is_message > 1)
		return strdup(fy_sprintfa("tool error: '%s' names more than "
					  "one thing; say its kind", name));
	if (is_monitor && fyai_monitor_cancel(ctx, name)) {
		*okp = true;
		return strdup(fy_sprintfa("[monitor '%s' cancelled]", name));
	}
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
	if (is_question && fyai_asks_cancel(ctx, name)) {
		*okp = true;
		return strdup(fy_sprintfa("[question '%s' cancelled]", name));
	}
	if (is_question && fyai_agents_parent_ask_cancel(ctx, name)) {
		*okp = true;
		return strdup(fy_sprintfa("[ask_parent '%s' cancelled]", name));
	}
	if (is_message && fyai_agents_message_cancel(ctx, name)) {
		*okp = true;
		return strdup(fy_sprintfa("[agent_message '%s' receipt cancelled; "
			"the message may still be queued]", name));
	}
	if (is_wait && fyai_wait_cancel(ctx, name)) {
		*okp = true;
		return strdup(fy_sprintfa("[wait '%s' cancelled]", name));
	}
	/* A daemon of the scope, by its label or its pid, when nothing else has the name. */
	if ((!*kind || !strcmp(kind, "daemon")) && fyai_scope_stop(ctx, name)) {
		*okp = true;
		return strdup(fy_sprintfa("[daemon '%s' was asked to stop; it is ended after "
					  "%lld ms if it stays]", name,
					  (long long)ctx->cfg->sandbox_grace_ms));
	}
	return strdup(fy_sprintfa("tool error: nothing named '%s' is running"
				  "%s%s; use list to see what is open", name,
				  *kind ? " as a " : "", kind));
}

int fyai_tool_call_file_effect(struct fyai_ctx *ctx, fy_generic tool_call)
{
	const char *name;
	const struct fyai_tool_def *def;

	tool_call = fyai_tool_call_effective(ctx, tool_call);
	name = fyai_tool_call_name(ctx, tool_call);
	def = fyai_tool_find(name);

	if (def)
		return def->effect;
	/* An MCP server can do anything. */
	return fyai_mcp_tool_name(name) ? FYAI_TOOL_EFFECT_PROCESS :
					  FYAI_TOOL_EFFECT_NONE;
}

/* A call of the parent that returns at once and starts work for later. */
static bool fyai_tool_call_instant(struct fyai_ctx *ctx, fy_generic tool_call)
{
	const struct fyai_tool_def *def;

	tool_call = fyai_tool_call_effective(ctx, tool_call);
	def = fyai_tool_find(fyai_tool_call_name(ctx, tool_call));

	if (!def || !(def->flags & FYAI_TOOL_INSTANT))
		return false;
	if (def->in_parent)
		return def->in_parent(fyai_tool_call_args(ctx, tool_call));
	return def->flags & FYAI_TOOL_PARENT;
}

bool fyai_tool_call_parallel_eligible(struct fyai_ctx *ctx,
				      fy_generic tool_call)
{
	const char *name;
	const struct fyai_tool_def *def;

	tool_call = fyai_tool_call_effective(ctx, tool_call);
	name = fyai_tool_call_name(ctx, tool_call);
	def = fyai_tool_find(name);

	if (fyai_mcp_tool_name(name) ||
	    (name && !strcmp(name, "mcp_resource")))
		return false;
	if (!def)
		return true;
	/* The group runs it in order, so a wait of the response sees it. */
	if (fyai_tool_call_instant(ctx, tool_call))
		return true;
	if (def->flags & FYAI_TOOL_PARENT)
		return false;
	return !def->in_parent ||
	       !def->in_parent(fyai_tool_call_args(ctx, tool_call));
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
		fyai_tools_work_update(job->ctx);
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
const char *fyai_agent_job_name(const struct fyai_tool_job *job)
{
	const char *who;

	who = job->branch ? strrchr(job->branch, '/') : NULL;
	return who && !strncmp(who + 1, FYAI_BRANCH_AGENT_PREFIX,
		strlen(FYAI_BRANCH_AGENT_PREFIX)) ?
		who + 1 + strlen(FYAI_BRANCH_AGENT_PREFIX) : "agent";
}

/* The running sub-agent called @name, or NULL. */
struct fyai_tool_job *fyai_agent_job_named(struct fyai_ctx *ctx,
					   const char *name)
{
	struct fyai_tool_job *job;
	const char *who;
	char slug[FYAI_BRANCH_COMPONENT_MAX + 1];

	if (fy_str_empty(name))
		return NULL;
	fyai_branch_sanitize(name, "agent", slug, sizeof(slug));
	for (job = ctx->tool_jobs; job; job = job->next) {
		if (!job->agent || job->done || !job->branch)
			continue;
		who = fyai_agent_job_name(job);
		if (!strcmp(job->branch, name) || !strcmp(who, name) ||
		    !strcmp(who, slug))
			return job;
	}
	return NULL;
}

const char *fyai_tool_agent_live_branch(struct fyai_ctx *ctx, const char *name)
{
	struct fyai_tool_job *job = fyai_agent_job_named(ctx, name);

	return job ? job->branch : NULL;
}

bool fyai_tool_agent_foreground_wait(struct fyai_ctx *ctx,
				     struct jsonrpc_conn *conn)
{
	struct fyai_tool_job *job;
	fy_generic args;

	for (job = ctx->tool_jobs; job; job = job->next) {
		if (!job->agent || job->done || job->conn != conn)
			continue;
		args = fyai_tool_call_args(ctx, job->call);
		if (!fy_get(args, "_fyai_background", false) && !job->btw)
			return true;
	}
	return false;
}

/* Present a named sub-agent's question and return the user's answer. */
static fy_generic fyai_ask_user_for_child(struct fyai_tool_job *job,
					  struct jsonrpc_conn *conn,
					  fy_generic id, fy_generic params)
{
	fy_generic result;
	const char *who, *origin;
	int rc;

	who = fyai_agent_job_name(job);
	origin = fy_get(params, "branch", "");
	if (*origin && (!job->branch || strcmp(origin, job->branch)))
		who = origin;
	result = fyai_ask_user_from(job->ctx, params, who);
	if (fy_is_string(result)) {
		/* The question was not valid, or nobody can be asked. */
		fyai_error(job->ctx, "agent: the questions of '%s' were not asked: %s",
			   who, fy_castp(&result, ""));
		result = fyai_ask_declined(job->ctx);
	}
	rc = jsonrpc_conn_respond(conn, id, result, fy_invalid);
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

int fyai_tool_agent_respond(struct fyai_ctx *ctx, const char *branch,
			    long long rpc_id, fy_generic result)
{
	struct fyai_tool_job *job;

	job = fyai_tool_job_by_branch(ctx, branch);
	if (!job || !job->conn)
		return -1;
	return jsonrpc_conn_respond(job->conn,
				    fy_value(fyai_ctx_transient_gb(ctx), rpc_id),
				    result, fy_invalid);
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
void fyai_ctx_fork_disown(struct fyai_ctx *ctx)
{
	size_t i;

	if (!ctx)
		return;
	/* First: a release below closes texts of the model, and a consumer of
	 * the parent would send that on its connection. */
	fyai_uimodel_abandon(ctx);		/* the renderers of the parent */
	fyai_shell_sessions_abandon(ctx);	/* named shells of the parent */
	fyai_tool_jobs_abandon(ctx);		/* its running tool children */
	fyai_waits_abandon(ctx);		/* its named waits */
	fyai_asks_abandon(ctx);			/* and its pending questions */
	fyai_ask_abandon(ctx);			/* the questions the parent put */
	fyai_uilog_release(ctx);		/* the span of its command */
	fyai_uiwork_release(ctx);		/* the bands of the parent */
	fyai_uitranscript_release(ctx);		/* the document of the parent */
	fyai_waiters_abandon(ctx);		/* and the waits that hold a call */
	fyai_agent_background_abandon(ctx);	/* its background sub-agents */
	fyai_monitor_abandon(ctx);		/* and its monitors */
	fyai_events_release(ctx);		/* and what they queued for it */
	fyai_request_cache_release(ctx);	/* its conversation text */
	fyai_patch_display_clear(ctx);		/* patches it resolved */
	free(ctx->patch_display);
	ctx->patch_display = NULL;
	fyai_output_cleanup(ctx);		/* the document it has open */
	fyai_diag_reset(&ctx->cfg->diag);	/* what the parent has not drained yet */
	fyai_cover_abandon(&ctx->cover);	/* the holder that it does not own */
	fyai_session_btw_abandon(ctx);		/* its side questions */
	fyai_orphan_abandon(ctx);		/* and the processes that the scope keeps */
	ctx->scope_pins = 0;
	ctx->scope_open = false;
	fyai_skill_dirs_abandon(ctx);		/* the skills it wrote out */
	fyai_settings_abandon(ctx);		/* and the exports */
	fyai_diag_trace_tap(NULL, NULL);	/* the recording of the parent */
	ctx->ui = NULL;				/* its display */
	ctx->workpane = NULL;			/* and the tiles that it drew */
	ctx->browser = NULL;
	ctx->agents = NULL;
	ctx->agent_parent = ctx->agent_execution;
	ctx->agent_execution = 0;
	ctx->shell_stream = NULL;
	ctx->tty_session = NULL;		/* the terminal it runs in */
	ctx->winch_src = NULL;			/* a source on the loop it owns */
	for (i = 0; i < ARRAY_SIZE(ctx->signal_src); i++)
		ctx->signal_src[i] = NULL;
	ctx->transport_src = NULL;
	ctx->cmd_call = NULL;			/* the command that it runs */
	ctx->catalog_update = NULL;		/* and the update that it started */
	ctx->acp_rpc = NULL;			/* its connection to a client */
	ctx->config_edit = NULL;
	ctx->mcp = NULL;			/* its connections to the servers */
	ctx->mcp_tools = fy_invalid;
	ctx->turn_observe = NULL;		/* a stream to the parent's peer */
	ctx->turn_observe_data = NULL;
	ctx->text_observe = NULL;
	ctx->text_observe_data = NULL;
	ctx->tool_gate = NULL;			/* and the policy of its calls */
	ctx->tool_gate_data = NULL;
	ctx->tool_gate_run = NULL;
	ctx->gate_relays = NULL;		/* the calls of its sub-agents */
	ctx->agent_observe = NULL;
	ctx->agent_observe_data = NULL;
	ctx->relay_gate = false;		/* what its own parent asked */
	ctx->relay_steps = false;
	ctx->turn_cost_limit = 0;		/* the budget of the parent's run */
	ctx->output_schema = fy_invalid;	/* and the answer it asks for */
	ctx->structured_output = fy_invalid;
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
	/* The attachments wait for the next line of the parent, not of this
	 * child; the store stays open to read the bytes of the branch. */
	ctx->media_pending = fy_seq_empty;
	ctx->dump_fd = -1;	/* the parent closes its diagnostic copy */
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
		rc = fyai_host_openpty(&master, &slave);
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
		if (agent_runtime)
			ctx->cfg->diag.mask &= ~(1u << FYAIET_NOTICE);
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
		if (pidfd < 0)
			pidfd = fyai_host_pidfd_open(pid);
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
	if (job->agent && job->branch)
		fyai_agents_message_branch_closed(job->ctx, job->branch);
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

/* The processes of the scope: the ones its holder holds, and the ones that it tracks. */
fy_generic fyai_scope_rows(struct fyai_ctx *ctx, struct fy_generic_builder *gb)
{
	pid_t pids[64];
	char name[32], label[64];
	size_t n, i;
	fy_generic rows;

	rows = fyai_orphan_rows(ctx, gb);
	n = fyai_cover_members(&ctx->cover, pids, ARRAY_SIZE(pids));
	for (i = 0; i < n; i++) {
		fyai_orphan_name(pids[i], name, sizeof(name));
		fyai_orphan_label(pids[i], label, sizeof(label));
		rows = fy_append(gb, rows, fy_mapping(gb, "pid", (long long)pids[i],
						      "name", fy_value(gb, name),
						      "label", fy_value(gb, label),
						      "held", "scope"));
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
	if (!all && !fy_any_equal(kind, "agents", "views", "shells", "waits",
				  "monitors", "daemons", "questions", "messages"))
		return fy_value(gb, "tool error: kind is agents, views, shells, "
				"waits, monitors, daemons, questions or messages");
	if (all || !strcmp(kind, "agents"))
		result = fy_assoc(gb, result, "agents", fyai_list_agents(ctx, gb));
	if (all || !strcmp(kind, "views"))
		result = fy_assoc(gb, result, "views", fyai_list_views(ctx, gb));
	if (all || !strcmp(kind, "shells"))
		result = fy_assoc(gb, result, "shells", fyai_list_shells(ctx, gb));
	if (all || !strcmp(kind, "waits"))
		result = fy_assoc(gb, result, "waits", fyai_waits_rows(ctx, gb));
	if (all || !strcmp(kind, "monitors"))
		result = fy_assoc(gb, result, "monitors",
				  fyai_monitors_rows(ctx, gb));
	if (all || !strcmp(kind, "daemons"))
		result = fy_assoc(gb, result, "daemons",
				  fyai_scope_rows(ctx, gb));
	if (all || !strcmp(kind, "questions"))
		result = fy_assoc(gb, result, "questions",
				  fy_concat(gb, fyai_asks_rows(ctx, gb),
					fyai_agents_parent_ask_rows(ctx, gb)));
	if (all || !strcmp(kind, "messages"))
		result = fy_assoc(gb, result, "messages",
				  fyai_agents_message_rows(ctx, gb));
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

void fyai_tool_job_discard(struct fyai_tool_job *job)
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
	free(job->monitor);
	free(job->origin);
	fy_generic_builder_destroy(job->call_gb);
	free(job);
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
	size_t lines = fyai_tool_history_lines(ctx);
	int rc = -1;

	job->band = fyai_sink_band_open(ctx->sink, false, NULL, NULL);
	if (job->band && job->agent) {
		/* The progress of a sub-agent is its prose: a quoted stream. */
		rc = fyai_markdown_quote_stream_start(&job->stream, ctx,
				ctx->cfg, lines, NULL, true);
	} else if (job->band) {
		rc = fyai_fenced_stream_start(&job->stream, ctx, ctx->cfg,
				NULL, lines,
				markdown_tool_output_indent(ctx->cfg), NULL,
				true);
	}
	if (rc) {
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

/* Whether a scope lasts as long as the interactive session. */
static bool fyai_scope_session(const struct fyai_ctx *ctx)
{
	return !strcmp(ctx->cfg->sandbox_scope, "session");
}

/* End the scope when no exchange runs and no object keeps it. */
static void fyai_scope_settle(struct fyai_ctx *ctx)
{
	if (ctx->scope_open || ctx->scope_pins > 0 || fyai_scope_session(ctx))
		return;
	/* A process that did not start the holder does not end it. */
	if (ctx->cover.owner) {
		fyai_diag_tracef("scope", "end, holder %ld", (long)ctx->cover.holder);
		fyai_cover_release(&ctx->cover);
	}
	if (fyai_orphan_count(ctx))
		fyai_diag_tracef("scope", "stop %zu tracked daemons",
				 fyai_orphan_count(ctx));
	fyai_orphan_sweep(ctx);
}

void fyai_scope_exchange(struct fyai_ctx *ctx, bool begin)
{
	ctx->scope_open = begin;
	if (!begin)
		fyai_scope_settle(ctx);
}

void fyai_scope_pin(struct fyai_ctx *ctx)
{
	ctx->scope_pins++;
}

void fyai_scope_unpin(struct fyai_ctx *ctx)
{
	if (ctx->scope_pins > 0)
		ctx->scope_pins--;
	fyai_scope_settle(ctx);
}

/*
 * Start the holder of the scope of the exchange before a tool child forks, so
 * that the child joins it and the holder outlives the call. The tool child
 * and a scope of one call make their own.
 */
static void fyai_scope_ensure(struct fyai_ctx *ctx)
{
	struct fyai_shell_sandbox sb;
	const struct fyai_sandbox_spec *spec;
	pid_t holder;

	if (ctx->cfg->tool_child || !ctx->cfg->enable_sandbox ||
	    ctx->sandbox_applied || !strcmp(ctx->cfg->sandbox_scope, "call"))
		return;
	holder = ctx->cover.holder;
	if (fyai_shell_sandbox_begin(ctx, &sb, &spec))
		return;
	fyai_shell_sandbox_end(&sb);
	if (ctx->cover.holder && ctx->cover.holder != holder)
		fyai_diag_tracef("scope", "begin, holder %ld", (long)ctx->cover.holder);
}

struct fyai_tool_job *fyai_tool_job_submit(struct fyai_ctx *ctx,
					    fy_generic tool_call)
{
	struct fyai_tool_job *job = NULL;
	struct fyai_view_run *view_run = NULL;
	struct fyai_buf view = {0};
	const char *name, *command;
	const char *asked, *cmdtext;
	fy_generic args, progress_args, agent_name, monitor_v;
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
	args = native_call ? tool_call : fyai_tool_call_args(ctx, tool_call);
	fyai_error_check(ctx, fy_is_valid(args), err,
			 "invalid tool call arguments");
	if (!native_call && fyai_shell_args_lack_command(name, args)) {
		fyai_tool_submit_error_set(ctx, "%s", FYAI_SHELL_NO_COMMAND);
		fyai_tool_submit_check(ctx, false, err,
				       "the shell call names no command");
	}
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

	fyai_diag_tracef("tool", "%s start", name);
	/* The first call of an exchange makes the scope that its calls join. */
	if (!fy_equal(name, "agent")) {
		fyai_home_ensure(ctx);
		fyai_scope_ensure(ctx);
	}

	job = calloc(1, sizeof(*job));
	fyai_error_check(ctx, job, err,
			 "could not allocate tool job");
	/* A sub-agent renders to a terminal of its own; the parent shows it. */
	rc = fyai_tool_job_spawn(ctx, job, fy_equal(name, "agent") &&
				 fyai_agents_detail(ctx->agent_execution ? 2 : 1) == 2 &&
				 (fyai_ui_active(ctx) ||
				  fyai_uimodel_consumed(ctx)),
				 fy_equal(name, "agent") &&
				 fyai_agent_spawn_exec(ctx),
				 view_run ? fyai_view_run_spec(view_run) : NULL, fy_equal(name, "agent"));
	fyai_error_check(ctx, !rc, err,
		"could not spawn tool job");
	job->view_run = view_run;
	view_run = NULL;
	job->is_agent = fy_equal(name, "agent");
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
					user_owned, NULL, NULL);
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
	monitor_v = fy_get(args, "_fyai_monitor", fy_invalid);
	if (fy_is_string(monitor_v)) {
		job->monitor = strdup(fy_castp(&monitor_v, ""));
		fyai_error_check(ctx, job->monitor, err,
				 "out of memory naming the monitor");
	}
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
	/* A worker with no display has bands while a client reads it. */
	if (fy_any_equal(name, "shell", "agent") &&
	    fyai_sink_bands_available(ctx->sink)) {
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
		/* Agents use their surface; shells continue to use a work band.
		 * Without a display, an agent reports its progress in a band. */
		/* Without a display a sub-agent with a terminal gives its
		 * screen to a remote client; with none, its progress. */
		if (job->agent && !fyai_ui_active(ctx) && job->pty < 0)
			job->band_progress = true;
		else if (job->agent) {
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
	fyai_diag_tracef("tool", "%s limit %u ms", name, job->timeout_ms);
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
	struct fyai_buf *buf = &job->progress;
	int rc;

	rc = fyai_buf_reserve(buf, buf->len + len + 1);
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

/*
 * Put a tool call of a delegated sub-agent before the gate of this process.
 * With no gate, the call runs as it would here.
 */
static fy_generic fyai_tool_job_gate(struct fyai_tool_job *job,
				     struct jsonrpc_conn *conn, fy_generic id,
				     fy_generic params)
{
	struct fyai_ctx *ctx = job->ctx;
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(ctx);
	bool after;
	int rc;

	after = fy_equal(fy_get(params, "phase", fy_invalid), "after");
	if (!ctx->tool_gate)
		return after ? fy_mapping(gb, "stop", false) :
			       fy_mapping(gb, "allow", true);
	/* The gate can answer during the relay. */
	jsonrpc_conn_defer(conn);
	rc = fyai_tool_gate_relay(ctx, job->branch, fy_cast(id, -1LL), params);
	if (!rc)
		return fy_invalid;
	rc = jsonrpc_conn_respond(conn, id, after ?
		fy_mapping(gb, "stop", false) :
		fy_mapping(gb, "allow", false,
			   "message", "the tool call could not be decided",
			   "stop", true), fy_invalid);
	if (rc)
		fyai_warning(ctx, "could not answer the tool call of the "
			     "sub-agent '%s'", fyai_agent_job_name(job));
	return fy_invalid;
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
	if (job->agent && fy_is_valid(id) && !strcmp(method, "tool/gate"))
		return fyai_tool_job_gate(job, conn, id, params);
	if (job->agent && !strcmp(method, "turn/step")) {
		if (job->ctx->agent_observe)
			job->ctx->agent_observe(job->ctx, job->call, params,
						job->ctx->agent_observe_data);
		return fy_invalid;
	}
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
			fyai_terminal_tap(job->ctx, job->session->view,
					  &job->session->model_text,
					  job->session->name, bytes, n);
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
		/* What the program left is tracked before the session lets go of the scope. */
		fyai_orphan_track(job->ctx, fy_get(params, "detached", fy_invalid));
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
	if (job->monitor)
		fyai_monitor_output(job->ctx, job->monitor, p, len);
	if (!job->agent && job->timeout_ms)
		fyai_tool_job_progress_retain(job, p, len);
	if (job->band_progress && job->stream.active)
		(void)fyai_fenced_stream_push(&job->stream, p, len);
	else if (job->band_delay &&
		 !fyai_buf_reserve(&job->pending_output,
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
		/* The scope keeps what the call left running. */
		fyai_orphan_track(job->ctx, fy_get(r, "detached", fy_invalid));
		if (!strcmp(job->ctx->cfg->sandbox_scope, "call"))
			fyai_orphan_sweep(job->ctx);
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
	/* A sub-agent asks the gate of this process, and reports its steps. */
	if (job->agent && ctx->tool_gate)
		params = fy_assoc(gb, params, "gate", true);
	if (job->agent && ctx->agent_observe)
		params = fy_assoc(gb, params, "steps", true);
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
	if (fy_is_valid(job->diag)) {
		if (job->agent)
			fyai_diag_adopt_agent(fyai_ctx_diag(ctx), job->diag,
					      job->origin);
		else
			fyai_diag_adopt(fyai_ctx_diag(ctx), job->diag,
					    job->origin);
	}
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
	free(job->monitor);
	free(job->origin);
	fy_generic_builder_destroy(job->call_gb);
	free(job);
	return result;
}

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
	group->phase = FYAITGP_OPEN;
	return group;
}

int fyai_tool_job_group_add(struct fyai_tool_job_group *group,
			    fy_generic tool_call)
{
	const char *text;
	char *copy;
	bool parallel, instant;
	int rc;
	size_t i;

	if (!group || group->phase >= FYAITGP_SEALED)
		return -1;
	rc = fyai_tool_job_group_reserve(group);
	if (rc)
		return -1;
	parallel = fyai_tool_call_parallel_eligible(group->ctx, tool_call);
	instant = fyai_tool_call_instant(group->ctx, tool_call);
	if (group->phase == FYAITGP_OPEN && !parallel)
		return 1;
	if (group->count && (group->exclusive || !parallel))
		return -1;
	/* The job runs the call that the tool call expands to. */
	tool_call = fyai_tool_call_effective(group->ctx, tool_call);
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
	group->entries[group->count].parallel = parallel && !instant;
	group->entries[group->count].state = FYAITGS_QUEUED;
	if (!parallel) {
		group->exclusive = true;
		group->max_parallel = 1;
	}
	group->count++;
	if (group->phase == FYAITGP_OPEN)
		fyai_tool_job_group_service(group);
	return 0;
}

/* A waiter of the group ended: look at the group again. */
static void fyai_tool_job_group_waiter_done(void *userdata)
{
	fyai_tool_job_group_service(userdata);
}

static void fyai_tool_job_group_dispatch(struct fyai_tool_job_group *group)
{
	struct fyai_tool_group_entry *entry;
	fy_generic call;
	fy_generic args;
	fy_generic result;
	const char *name;
	const char *text;
	char *why;

	while (group->phase != FYAITGP_CANCELLED && group->next < group->count &&
	       group->active < group->max_parallel) {
		entry = &group->entries[group->next++];
		call = parse_json_string(group->ctx->transient_gb,
					 entry->call_text);
		name = fy_is_valid(call) ?
			fyai_tool_call_name(group->ctx, call) : NULL;
		if (name && (fyai_mcp_tool_name(name) ||
			     !strcmp(name, "mcp_resource"))) {
			args = fyai_tool_call_args(group->ctx, call);
			entry->mcp_request =
				fy_is_valid(args) &&
				fyai_skill_tool_allowed(group->ctx, name) ?
				(!strcmp(name, "mcp_resource") ?
				 fyai_mcp_resource_submit(group->ctx, args,
					fyai_tool_job_group_mcp_complete, group) :
				 fyai_mcp_call_submit(group->ctx, name, args,
					fyai_tool_job_group_mcp_complete, group)) :
				NULL;
			if (entry->mcp_request) {
				entry->state = FYAITGS_RUNNING;
				group->active++;
				continue;
			}
			result = fy_value(group->ctx->transient_gb,
				fyai_skill_tool_allowed(group->ctx, name) ?
				"tool error: MCP call failed" :
				"tool error: tool is not allowed by this skill");
			text = emit_json_string(group->ctx->transient_gb,
						result);
			entry->result_text = text ? strdup(text) : NULL;
			entry->state = entry->result_text ?
				FYAITGS_PARKED : FYAITGS_SUBMIT_FAILED;
			group->parked++;
			continue;
		}
		if (entry->parallel && name && !strcmp(name, "wait") &&
		    fyai_wait_for_requested(fyai_tool_call_args(group->ctx,
								  call))) {
			/* A wait for an event runs beside the other calls. */
			why = NULL;
			entry->waiter = fyai_waiter_start(group->ctx,
				fyai_tool_call_args(group->ctx, call),
				fyai_tool_job_group_waiter_done, group, &why);
			if (entry->waiter) {
				entry->state = FYAITGS_RUNNING;
				group->active++;
				/* It can end at once: nothing else parks it. */
				if (fyai_waiter_done(entry->waiter)) {
					entry->state = FYAITGS_PARKED;
					group->active--;
					group->parked++;
				}
				continue;
			}
			result = fy_value(group->ctx->transient_gb,
					  why ? why : "tool error: the wait "
					  "could not start");
			free(why);
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

	if (group->phase < FYAITGP_SEALED || group->parked != group->count ||
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

	if (!group || group->phase == FYAITGP_BUILDING)
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
		if (entry->waiter) {
			if (!fyai_waiter_done(entry->waiter))
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
	if (!group || group->phase != FYAITGP_BUILDING || !group->count)
		return -1;
	group->phase = FYAITGP_SEALED;
	fyai_tool_job_group_dispatch(group);
	fyai_tool_job_group_animation_sync(group);
	fyai_tool_job_group_notify(group);
	return 0;
}

int fyai_tool_job_group_seal(struct fyai_tool_job_group *group)
{
	if (!group || group->phase != FYAITGP_OPEN)
		return -1;
	group->phase = FYAITGP_SEALED;
	fyai_tool_job_group_service(group);
	return 0;
}

bool fyai_tool_job_group_done(const struct fyai_tool_job_group *group)
{
	return group && group->phase >= FYAITGP_SEALED &&
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

	if (!group || group->phase == FYAITGP_CANCELLED)
		return;
	group->phase = FYAITGP_CANCELLED;
	for (i = 0; i < group->count; i++) {
		entry = &group->entries[i];
		if (entry->state == FYAITGS_RUNNING && entry->waiter) {
			/* Nothing runs for it: it ends here and parks. */
			fyai_waiter_cancel(entry->waiter);
			entry->state = FYAITGS_PARKED;
			group->active--;
			group->parked++;
		} else if (entry->state == FYAITGS_RUNNING) {
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
	char *text;

	if (!group || !result || !okp || index >= group->count)
		return -1;
	entry = &group->entries[index];
	if (entry->state != FYAITGS_PARKED &&
	    entry->state != FYAITGS_SUBMIT_FAILED)
		return -1;
	if (entry->job) {
		*result = fyai_tool_job_collect(group->ctx, entry->job, okp);
		entry->job = NULL;
	} else if (entry->waiter) {
		text = fyai_waiter_result(entry->waiter, okp);
		fyai_waiter_destroy(entry->waiter);
		entry->waiter = NULL;
		*result = fy_gb_internalize(group->ctx->transient_gb,
					    fy_value(text ? text : ""));
		free(text);
	} else if (entry->mcp_request) {
		*result = fyai_mcp_call_collect(entry->mcp_request, okp);
		fyai_mcp_call_destroy(entry->mcp_request);
		entry->mcp_request = NULL;
	} else if (entry->result_text) {
		*result = parse_json_string(group->ctx->transient_gb,
					    entry->result_text);
		*okp = entry->result_ok;
	} else if (group->phase == FYAITGP_CANCELLED) {
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
		fyai_waiter_destroy(entry->waiter);
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
		emit_generic_to_sink(ctx, NULL, result, ctx->cfg->pretty);
	ret = 0;
out:
	free(stdin_buf);
	fyai_cleanup_transient_builder(ctx);
	return ret;
}
