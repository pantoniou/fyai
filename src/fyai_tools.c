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
#include "fyai_tools.h"
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
	fyai_emit_tool_call(mf, ctx->transient_gb, tool, args, preview_lines);
	fclose(mf);
	return md;
}

static char *fyai_format_shell_header(struct fyai_ctx *ctx, const char *command,
				      fy_generic args)
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
	return fyai_format_tool_header(ctx, "shell", disp, 0);
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

void fyai_print_tool_call(struct fyai_ctx *ctx, fy_generic tool_call)
{
	struct fyai_cfg *cfg = ctx->cfg;
	const char *name;
	const char *args_text;
	const char *command;
	char *header;
	fy_generic args;

	if (fyai_agent_delegated(ctx))
		return;
	args = fy_invalid;
	name = fyai_tool_call_name(ctx, tool_call);
	if (fy_equal(fy_get(tool_call, "type"), "shell_call")) {
		command = fy_cast(fy_get_at_path(tool_call, "action", "commands", 0), "");
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
		if (fyai_ui_active(ctx)) {
			fyai_ui_tool_begin(ctx, header ? header : "agent");
		} else if (header) {
			if (fyai_fprint_markdown(stderr, header, ctx->cfg))
				fputs(header, stderr);
		} else {
			fprintf(stderr, "  agent %s\n",
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
		header = fyai_format_shell_header(ctx, *command ? command : name,
						  args);
		if (fyai_ui_active(ctx)) {
			free(header);
			header = fyai_format_shell_label(args);
			fyai_ui_shell_begin(ctx, header ? header : "shell",
					    *command ? command : name);
		} else if (header) {
			if (fyai_fprint_markdown(stderr, header, ctx->cfg))
				fputs(header, stderr);
		} else {
			fprintf(stderr, "  shell %s\n",
				*command ? command : name);
		}
		free(header);
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
	} else if (cfg->markdown && fyai_ui_active(ctx) &&
		   (fy_equal(name, "read_file") || fy_equal(name, "write_file"))) {
		args = fyai_tool_call_args(ctx, tool_call);
		header = fyai_format_tool_header(ctx, name, args,
				fyai_tool_preview_lines(ctx->cfg, name));
		fyai_ui_tool_begin(ctx, header ? header : name);
		free(header);
		ctx->tool_output_displayed = true;
	} else if (*command) {
		fprintf(stderr, "fyai $ %s\n", command);
	} else {
		fprintf(stderr, "fyai $ %s\n", name);
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
	(void)jsonrpc_request_submit(ctx->tool_rpc, "tool/progress",
				     fy_gb_mapping(gb, "text", text),
				     0, true, NULL, NULL);
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
	if (fyai_agent_delegated(ctx))
		return;
	fyai_tool_progress_emit(ctx, data, len);

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

	fwrite(data, 1, len, stderr);
	if (data[len - 1] != '\n')
		fputc('\n', stderr);
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
	sb->deny[sp->deny_n] = sandbox_resolve(sb->root, ".fyai");
	fyai_error_check(ctx, sb->deny[sp->deny_n], err_out,
			 "sandbox: could not resolve the arena deny path");
	sp->deny_n++;
	fy_foreach(e, deny) {
		ps = fy_castp(&e, "");
		sb->deny[sp->deny_n] = sandbox_resolve(sb->root, ps);
		fyai_error_check(ctx, sb->deny[sp->deny_n], err_out,
				 "sandbox: could not resolve deny path '%s'", ps);
		sp->deny_n++;
	}
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

	*okp = false;
	if (fyai_shell_sandbox_begin(ctx, &sb, &sandbox))
		return NULL;

	command = fy_get(args, "command", fy_invalid);
	workdir = fy_get(args, "workdir", fy_invalid);
	opts.workdir = fy_castp(&workdir, (const char *)NULL);
	opts.timeout_ms = fyai_shell_timeout_ms(ctx, args, false);

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
			fprintf(stderr, "%s", msg);
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
			fprintf(stderr, "%s", msg);
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

	if (ansi_color_on(cfg->color, STDERR_FILENO))
		fprintf(stderr, "\n" FYAI_ANSI_BOLD "? %s" FYAI_ANSI_RESET
			"\n", question);
	else
		fprintf(stderr, "\n? %s\n", question);
	i = 0;
	fy_foreach(result, options) {
		fprintf(stderr, "  %zu) %s\n", i + 1,
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

		fprintf(stderr, "%s%s\n",
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
	size_t out_bytes, err_bytes;
	char *bounded;

	*okp = false;
	type = fy_get(tool_call, "type");
	if (fy_equal(type, "shell_call")) {

		action = fy_get(tool_call, "action");
		commands = fy_get(action, "commands");
		shell_opts.timeout_ms = fyai_shell_timeout_ms(ctx, tool_call,
							      true);
		outputs = fy_seq_empty;
		if (fyai_shell_sandbox_begin(ctx, &sb, &sandbox))
			return fy_value(ctx->transient_gb,
					"tool error: invalid sandbox configuration");
		*okp = true;

		fy_foreach(command, commands) {
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
					fprintf(stderr, "%s\n", out_text);
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
		result = fyai_apply_patch_text(fy_get(args, "patch", ""));
		*okp = result && strncmp(result, "tool error:", 11);
	} else if (fy_equal(name, "shell")) {
		result = fyai_run_shell_command(ctx, args, okp);
	} else if (fy_equal(name, "ask_user")) {
		result_generic = fyai_ask_user(ctx, args);
		*okp = strncmp(fy_castp(&result_generic, ""),
			      "tool error:", 11) != 0;
		return result_generic;
	} else if (fy_equal(name, "agent")) {
		char *diag;

		result_generic = fyai_agent_run(ctx, args, okp);
		if (fy_is_invalid(result_generic)) {
			diag = fyai_diag_take_string(&ctx->cfg->diag);
			result_generic = fy_gb_internalize(ctx->transient_gb,
				diag && *diag ?
				fy_stringf("tool error: sub-agent failed: %s",
					   diag) :
				fy_value("tool error: sub-agent failed"));
			free(diag);
			return result_generic;
		}
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

	if (fyai_shell_sandbox_begin(ctx, &sb, &spec))
		return -1;
	if (spec)
		(void)fyai_sandbox_apply(spec);
	fyai_shell_sandbox_end(&sb);
	ctx->sandbox_applied = true;
	return 0;
}

/*
 * A tool child must not carry the application's event descriptors, or result
 * pipes belonging to sibling jobs, across exec. Move the result and optional
 * progress descriptors to fd 3/4 and close everything above them.
 */
/* Put the JSON-RPC control channel on stdin and stdout. Close all other fds. */
static int fyai_tool_child_fds(int req_fd, int rsp_fd)
{
	int req_dup = -1, rsp_dup = -1;
	long max_fd, fd;
	int rc;

	/* Move both clear of 0/1 before dup2 can clobber either. */
	req_dup = fcntl(req_fd, F_DUPFD_CLOEXEC, 5);
	if (req_dup < 0)
		goto err;
	rsp_dup = fcntl(rsp_fd, F_DUPFD_CLOEXEC, 5);
	if (rsp_dup < 0)
		goto err;

	rc = dup2(req_dup, STDIN_FILENO);
	if (rc < 0)
		goto err;
	rc = dup2(rsp_dup, STDOUT_FILENO);
	if (rc < 0)
		goto err;

#if defined(__linux__) && defined(SYS_close_range)
	rc = syscall(SYS_close_range, 3U, ~0U, 0U);
	if (!rc)
		return 0;
#endif
	max_fd = sysconf(_SC_OPEN_MAX);
	if (max_fd < 0)
		max_fd = 1024;
	for (fd = 3; fd < max_fd; fd++)
		close((int)fd);
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
	fy_generic id;
	fy_generic args;
	fy_generic branch;	/* sub-agent branch named by the parent */
	bool pending;
	bool done;
};

static fy_generic fyai_tool_child_serve(struct jsonrpc_conn *conn,
					const char *method, fy_generic params,
					fy_generic id, void *userdata,
					fy_generic *errorp)
{
	struct fyai_tool_child *tc = userdata;
	struct fy_generic_builder *gb = fyai_ctx_transient_gb(tc->ctx);

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
	*errorp = fy_gb_mapping(gb, "code", -32601LL,
				"message", "method not found");
	return fy_invalid;
}

static void fyai_tool_child_serve_loop(struct fyai_ctx *ctx)
{
	struct fyai_tool_child tc;
	struct fyai_event_loop *el;
	struct jsonrpc_conn *conn;
	fy_generic result;
	bool ok = false;

	memset(&tc, 0, sizeof(tc));
	tc.ctx = ctx;

	el = fyai_ctx_loop(ctx);
	if (!el)
		_exit(1);
	conn = jsonrpc_conn_stdio(ctx, STDOUT_FILENO, STDIN_FILENO, 0,
				  "tool", NULL);
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
			if (!fy_equal(fyai_tool_call_name(ctx, tc.args), "agent"))
				fyai_env_sanitize();
			result = fyai_execute_tool_call(ctx, tc.args, &ok);
			jsonrpc_conn_respond(conn, tc.id,
				fy_gb_mapping(fyai_ctx_transient_gb(ctx),
					      "result", result, "ok", ok),
				fy_invalid);
			break;
		}
		if (fyai_event_loop_step(el, -1) < 0)
			_exit(1);
	}

	while (jsonrpc_conn_has_output(conn))
		if (fyai_event_loop_step(el, 1000) <= 0)
			break;
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
	pid_t pid;
	int rfd;
	int pfd;
	struct fyai_fenced_stream stream;
	struct fytim_workband *band;
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
	bool terminating;
	bool timed_out;
	unsigned int timeout_ms;	/* 0 = no limit */
	struct response_buffer progress;	/* tail, for a timeout report */
	char *branch;			/* sub-agent branch, allocated by us */
	struct fyai_event_source *deadline;
	int term_signal;
	struct fyai_tool_job_group *group;
};

bool fyai_tool_call_parallel_eligible(struct fyai_ctx *ctx,
				      fy_generic tool_call)
{
	const char *name;

	name = fyai_tool_call_name(ctx, tool_call);
	return !fy_equal(name, "ask_user") && !fyai_mcp_tool_name(name);
}

static void fyai_tool_job_drop(struct fyai_event_source **srcp)
{
	fyai_event_source_remove(*srcp);
	*srcp = NULL;
}

static void fyai_tool_job_update_done(struct fyai_tool_job *job)
{
	bool was_done = job->done;

	job->done = job->reaped && !job->out_open;
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
	job->result_ok = WIFEXITED(ev->status) && WEXITSTATUS(ev->status) == 0;
	job->term_signal = WIFSIGNALED(ev->status) ?
				WTERMSIG(ev->status) : 0;
	fyai_tool_job_update_done(job);
	return FYAIEA_CONTINUE;
}

static int fyai_tool_job_spawn(struct fyai_ctx *ctx,
			       struct fyai_tool_job *job)
{
	int req[2] = { -1, -1 };	/* parent -> child */
	int rsp[2] = { -1, -1 };	/* child -> parent */
	pid_t pid;
	int rc;

	memset(job, 0, sizeof(*job));
	job->ctx = ctx;
	job->rfd = -1;
	job->pfd = -1;
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
	pid = fork();
	fyai_error_check(ctx, pid >= 0, err,
			 "could not fork tool process: %s", strerror(errno));

	if (!pid) {			/* child */
		close(req[1]);
		close(rsp[0]);
		if (setsid() < 0)
			(void)setpgid(0, 0);	/* already a leader: still isolate */
		fyai_ctx_loop_abandon(ctx);
		if (fyai_tool_child_fds(req[0], rsp[1]))
			_exit(126);

		ctx->ui = NULL;
		ctx->shell_stream = NULL;
		ctx->cfg->tool_child = true;
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
	job->pid = pid;
	job->rfd = rsp[0];
	job->pfd = req[1];
	rsp[0] = -1;
	req[1] = -1;
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
	return -1;
}


static void fyai_tool_job_live_close(struct fyai_tool_job *job)
{
	if (job->stream.active)
		fyai_fenced_stream_finish(&job->stream);
	fyai_ui_workband_destroy(job->band);
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
	free(job->progress.data);
	free(job->branch);
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
	const char *name, *args_text, *command;
	fy_generic args, progress_args, agent_name;
	struct fyai_event_loop *el;
	char child_branch[FYAI_BRANCH_NAME_MAX + 1];
	bool have_branch = false;
	bool native_call;
	bool eligible;
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
		rc = fyai_branch_alloc_child(ctx, fyai_ctx_branch(ctx),
				fy_castp(&agent_name, "agent"),
				(unsigned int)ctx->cfg->agent_max_branch_depth,
				child_branch, sizeof(child_branch));
		if (rc)
			fyai_tool_submit_error_set(ctx,
				"tool error: a sub-agent named '%s' already "
				"exists on this branch; choose a different name",
				fy_castp(&agent_name, "agent"));
		fyai_error_check(ctx, !rc, err,
				 "could not name the sub-agent branch");
		have_branch = true;
	}

	job = calloc(1, sizeof(*job));
	fyai_error_check(ctx, job, err,
			 "could not allocate tool job");
	rc = fyai_tool_job_spawn(ctx, job);
	fyai_error_check(ctx, !rc, err,
		"could not spawn tool job");
	/* The spawn clears the job, thus the name is carried in a local. */
	if (have_branch) {
		job->branch = strdup(child_branch);
		fyai_error_check(ctx, job->branch, err,
				 "out of memory naming the sub-agent branch");
	}
	job->call = tool_call;
	job->native_shell = native_call;
	if ((fy_equal(name, "shell") || fy_equal(name, "agent")) &&
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
		job->band = fyai_ui_workband_create(ctx);
		if (job->agent && job->band) {
			if (fyai_markdown_quote_stream_start(&job->stream, ctx,
					ctx->cfg,
					ctx->cfg->tool_preview_lines > 0 ?
					(size_t)ctx->cfg->tool_preview_lines : 0,
					stderr, true)) {
				fyai_tool_job_live_close(job);
			} else {
				job->stream.band = job->band;
				job->stream.title = job->title;
				(void)fyai_fenced_stream_push(&job->stream,
							      NULL, 0);
			}
		} else if (job->band &&
		    !fyai_fenced_stream_start(&job->stream, ctx, ctx->cfg,
				NULL, ctx->cfg->tool_preview_lines > 0 ?
				(size_t)ctx->cfg->tool_preview_lines : 0,
				FYAI_TOOL_OUTPUT_INDENT, stderr, true)) {
			job->stream.band = job->band;
			job->stream.title = job->title;
			job->stream.command = job->command;
			fyai_ui_shell_workband_update(ctx, job->band,
						      job->title, job->command,
						      NULL, 0, NULL);
		} else {
			fyai_tool_job_live_close(job);
		}
	}
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
	size_t len;

	(void)conn;
	(void)errorp;
	if (strcmp(method, "tool/progress") || fy_is_valid(id))
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
	if ((job->agent || job->band_progress) && job->stream.active)
		(void)fyai_fenced_stream_push(&job->stream, p, len);
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
 * Change the outcome of a native shell result after a parent deadline.
 * The child sees group termination only as a signal and cannot identify the
 * timeout. The parent identifies the timeout and keeps all captured output.
 */
static fy_generic fyai_shell_result_retime(struct fyai_ctx *ctx,
					   fy_generic result,
					   unsigned int timeout_ms)
{
	fy_generic outputs = fy_seq_empty;
	fy_generic entry, outcome;

	if (!fy_is_sequence(result))
		return result;
	fy_foreach(entry, result) {
		outcome = fy_get(entry, "outcome");
		if (fy_equal(fy_get(outcome, "type"), "signal"))
			entry = fy_mapping(
				"stdout", fy_get(entry, "stdout", ""),
				"stderr", fy_get(entry, "stderr", ""),
				"outcome", fy_mapping(
					"type", "timeout",
					"timeout_ms",
					(long long)timeout_ms));
		outputs = fy_append(outputs, entry);
	}
	return fy_gb_internalize(ctx->transient_gb, outputs);
}

fy_generic fyai_tool_job_collect(struct fyai_ctx *ctx,
				 struct fyai_tool_job *job, bool *okp)
{
	const char *reason;
	const char *captured;
	fy_generic result;
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
			job->result_ok = WIFEXITED(status) &&
					 WEXITSTATUS(status) == 0;
			job->term_signal = WIFSIGNALED(status) ?
						WTERMSIG(status) : 0;
		}
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
							  job->timeout_ms);
		} else {
			/*
			 * Keep the native result shape after a timeout. The model
			 * always receives a sequence of {stdout, stderr, outcome}.
			 */
			result = fy_gb_internalize(ctx->transient_gb,
				fy_sequence(fy_mapping(
					"stdout", captured,
					"stderr", "",
					"outcome", fy_mapping(
						"type", "timeout",
						"timeout_ms",
						(long long)job->timeout_ms))));
		}
		job->result_ok = false;
	}
	*okp = job->result_ok && !job->failed;
	free(job->progress.data);
	free(job->branch);
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
	fyai_env_sanitize();
	rc = fyai_tool_apply_sandbox(ctx);
	fyai_error_check(ctx, !rc, out,
			 "tool: could not apply sandbox policy");

	result = fyai_tool_run_one(ctx, a->name, args, &ok);
	result = fy_gb_internalize(ctx->transient_gb, result);

	if (fy_is_string(result))
		printf("%s\n", fy_castp(&result, ""));
	else
		emit_generic_to_stdout(NULL, result, ctx->cfg->pretty);
	ret = 0;
out:
	free(stdin_buf);
	fyai_cleanup_transient_builder(ctx);
	return ret;
}
