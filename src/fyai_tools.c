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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <fcntl.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#include <sys/wait.h>

#include "fyai_config.h"
#include "fyai_display.h"
#include "fyai_event.h"
#include "fyai_markdown.h"
#include "fyai_patch.h"
#include "fyai_sandbox.h"
#include "fyai_session.h"
#include "fyai_terminal.h"
#include "fyai_tools.h"
#include "fyai_ui.h"

static bool fyai_should_fork_tool(struct fyai_ctx *ctx, const char *name);
static fy_generic fyai_tool_run_forked(struct fyai_ctx *ctx, const char *name,
				       fy_generic args, bool *okp);

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
static char *fyai_format_shell_header(struct fyai_ctx *ctx, const char *command)
{
	char *md = NULL;
	size_t mdlen = 0;
	FILE *mf;

	mf = open_memstream(&md, &mdlen);
	if (!mf)
		return NULL;
	fyai_emit_tool_call(mf, ctx->transient_gb, "shell",
			    fy_mapping("command", command), 0);
	fclose(mf);
	return md;
}

void fyai_print_tool_call(struct fyai_ctx *ctx, fy_generic tool_call)
{
	struct fyai_cfg *cfg = ctx->cfg;
	const char *name;
	const char *args_text;
	const char *command;
	char *header;
	fy_generic args;

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

	if (cfg->markdown && fy_equal(name, "shell")) {
		/*
		 * Live shell output streams progressively into a bounded,
		 * indented, in-place region - the same libfymd4c fenced render
		 * (row limit + indent) as the history view, only updated live as
		 * the command's output arrives, so live and history match.
		 */
		header = fyai_format_shell_header(ctx, *command ? command : name);
		if (fyai_ui_active(ctx)) {
			fyai_ui_tool_begin(ctx, header ? header : "shell");
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

static void fyai_shell_output(void *userdata,
			      enum shell_output_stream stream,
			      const char *data, size_t len)
{
	struct fyai_ctx *ctx = userdata;
	size_t off;
	ssize_t w;

	if (!len)
		return;
	(void)stream;
	if (ctx && ctx->tool_progress_fd > STDERR_FILENO) {
		for (off = 0; off < len; off += (size_t)w) {
			w = write(ctx->tool_progress_fd, data + off, len - off);
			if (w < 0) {
				if (errno == EINTR)
					w = 0;
				else
					break;
			}
		}
	}

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
static const struct fyai_sandbox_spec *
fyai_shell_sandbox_begin(struct fyai_ctx *ctx, struct fyai_shell_sandbox *sb)
{
	struct fyai_sandbox_spec *sp = &sb->spec;
	fy_generic cs = ctx->cfg->sandbox;
	fy_generic allow, deny, net, ports, e;
	fy_generic port, pv;
	enum fyai_sandbox_mode mode;
	char cwd[4096];
	const char *ps;
	size_t n;

	memset(sb, 0, sizeof(*sb));
	if (!ctx->cfg->enable_sandbox || ctx->sandbox_applied)
		return NULL;

	sb->root = fyai_discover_project_root();
	if (!sb->root && getcwd(cwd, sizeof(cwd)))
		sb->root = strdup(cwd);
	sp->project_root = sb->root;
	sp->strict = false;			/* floor is the config policy */

	/* deny: always the arena, then each config sandbox.deny entry. */
	deny = fy_get(cs, "deny");
	n = fy_generic_is_sequence(deny) ? fy_len(deny) : 0;
	sb->deny = calloc(n + 1, sizeof(*sb->deny));
	if (sb->deny && sb->root) {
		sb->deny[sp->deny_n++] = sandbox_resolve(sb->root, ".fyai");
		fy_foreach(e, deny) {
			sb->deny[sp->deny_n] = sandbox_resolve(sb->root,
							fy_castp(&e, ""));
			if (sb->deny[sp->deny_n])
				sp->deny_n++;
		}
	}
	sp->deny = sb->deny;

	/* allow: extra grants; a string is rw, a mapping {path, mode: ro}. */
	allow = fy_get(cs, "allow");
	n = fy_generic_is_sequence(allow) ? fy_len(allow) : 0;
	sb->allow = calloc(n + 1, sizeof(*sb->allow));
	if (sb->allow) {
		fy_foreach(e, allow) {
			mode = FYAI_SB_RW;
			if (fy_generic_is_mapping(e)) {
				pv = fy_get(e, "path");
				ps = fy_castp(&pv, "");
				mode = fyai_sandbox_mode_parse(
						fy_get(e, "mode", "rw"));
			} else {
				ps = fy_castp(&e, "");
			}
			sb->allow[sp->allow_n].path = sandbox_resolve(sb->root, ps);
			if (sb->allow[sp->allow_n].path) {
				sb->allow[sp->allow_n].mode = mode;
				sp->allow_n++;
			}
		}
	}
	sp->allow = sb->allow;

	/* network: present => restrict egress to network.ports (empty = deny
	 * all); absent => leave egress unrestricted. */
	net = fy_get(cs, "network");
	if (fy_generic_is_valid(net)) {
		sp->restrict_net = true;
		ports = fy_get(net, "ports");
		n = fy_generic_is_sequence(ports) ? fy_len(ports) : 0;
		sb->ports = calloc(n + 1, sizeof(*sb->ports));
		if (sb->ports)
			fy_foreach(port, ports)
				sb->ports[sp->ports_n++] = (uint16_t)
					fy_cast(port, 0LL);
		sp->ports = sb->ports;
	}

	return sp;
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

static char *fyai_run_shell_command(struct fyai_ctx *ctx, const char *command,
				    bool *okp)
{
	struct fyai_cfg *cfg = ctx->cfg;
	struct shell_command_result result = {};
	struct response_buffer buf = {};
	struct fyai_shell_sandbox sb;
	const struct fyai_sandbox_spec *sandbox;
	const char *msg;
	char *ret = NULL;

	sandbox = fyai_shell_sandbox_begin(ctx, &sb);
	*okp = false;

	if (run_shell_command_capture_cb(ctx, command, &result,
					 fyai_shell_output, ctx, sandbox))
		goto out;
	fyai_shell_live_close(ctx);

	if (data_is_binary(result.stdout_data, result.stdout_len)) {
		msg = fy_sprintfa("binary output: %zu bytes\n",
				  result.stdout_len);
		if (!cfg->markdown)
			fprintf(stderr, "%s", msg);
		if (response_buffer_append(&buf, msg))
			goto out;
	} else if (response_buffer_append(&buf, result.stdout_data)) {
		goto out;
	}

	if (data_is_binary(result.stderr_data, result.stderr_len)) {
		msg = fy_sprintfa("binary stderr: %zu bytes\n",
				  result.stderr_len);
		if (!cfg->markdown)
			fprintf(stderr, "%s", msg);
		if (response_buffer_append(&buf, msg))
			goto out;
	} else if (response_buffer_append(&buf, result.stderr_data)) {
		goto out;
	}

	if (result.signaled) {
		if (fyai_interrupt_pending(ctx))
			msg = "\ntool error: interrupted\n";
		else
			msg = fy_sprintfa(
				"\ntool error: command killed by signal %d\n",
				result.signal);
		if (response_buffer_append(&buf, msg))
			goto out;
	} else if (result.exit_code) {
		msg = fy_sprintfa("\ntool error: command exited with status %d\n",
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
	size_t n = fy_generic_is_sequence(options) ? fy_len(options) : 0;
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
			if (fy_generic_is_invalid(result))
				result = fy_value("");
			free(line);
			if (fy_generic_is_invalid(result))
				fyai_error(ctx, "ask_user: could not retain the answer");
			return result;
		}
	}

	result = fy_value(ctx->transient_gb, line);
	free(line);
	if (fy_generic_is_invalid(result))
		fyai_error(ctx, "ask_user: could not retain the answer");
	return result;
}

fy_generic fyai_execute_tool_call(struct fyai_ctx *ctx,
				  fy_generic tool_call, bool *okp)
{
	struct fyai_cfg *cfg = ctx->cfg;
	struct shell_command_result shell_result = {};
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

	*okp = false;
	type = fy_get(tool_call, "type");
	if (fy_equal(type, "shell_call")) {

		action = fy_get(tool_call, "action");
		commands = fy_get(action, "commands");
		outputs = fy_seq_empty;
		sandbox = fyai_shell_sandbox_begin(ctx, &sb);
		*okp = true;

		fy_foreach(command, commands) {
			if (run_shell_command_capture_cb(ctx,
							 fy_castp(&command, ""),
							 &shell_result,
							 fyai_shell_output,
							 ctx, sandbox)) {
				fyai_shell_live_close(ctx);
				fyai_shell_sandbox_end(&sb);
				return fy_value(ctx->transient_gb, "tool error: failed to run shell command");
			}
			fyai_shell_live_close(ctx);

			out_text = shell_result.stdout_data;
			if (data_is_binary(shell_result.stdout_data,
					   shell_result.stdout_len)) {
				out_text = fy_sprintfa("binary output: %zu bytes",
						       shell_result.stdout_len);
				if (!cfg->markdown)
					fprintf(stderr, "%s\n", out_text);
			}
			err_text = shell_result.stderr_data;
			if (data_is_binary(shell_result.stderr_data,
					   shell_result.stderr_len)) {
				err_text = fy_sprintfa("binary stderr: %zu bytes",
						       shell_result.stderr_len);
			}

			output = fy_mapping(
				"stdout", out_text,
				"stderr", err_text,
				"outcome", shell_result.signaled ?
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
	if (fy_generic_is_invalid(args))
		return fy_value(ctx->transient_gb, "tool error: invalid JSON arguments");
	if (fyai_mcp_tool_name(name)) {
		result_generic = fyai_mcp_call(ctx, name, args);
		goto out;
	}

	if (fyai_should_fork_tool(ctx, name))
		result_generic = fyai_tool_run_forked(ctx, name, args, okp);
	else
		result_generic = fyai_tool_run_one(ctx, name, args, okp);
out:
	result_generic = fy_gb_internalize(ctx->transient_gb, result_generic);
	if (fy_generic_is_invalid(result_generic))
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
		path = fy_get(args, "path", "");
		result = read_text_file(path);
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
		result = fyai_run_shell_command(ctx, fy_get(args, "command", ""),
						okp);
	} else if (fy_equal(name, "ask_user")) {
		result_generic = fyai_ask_user(ctx, args);
		*okp = strncmp(fy_castp(&result_generic, ""),
			      "tool error:", 11) != 0;
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
static void fyai_tool_apply_sandbox(struct fyai_ctx *ctx)
{
	struct fyai_shell_sandbox sb;
	const struct fyai_sandbox_spec *spec;

	spec = fyai_shell_sandbox_begin(ctx, &sb);
	if (spec)
		(void)fyai_sandbox_apply(spec);
	fyai_shell_sandbox_end(&sb);
	ctx->sandbox_applied = true;
}

/*
 * A tool child must not carry the application's event descriptors, or result
 * pipes belonging to sibling jobs, across exec. Move the result and optional
 * progress descriptors to fd 3/4 and close everything above them.
 */
static int fyai_tool_child_fds(int result_fd, int progress_fd)
{
	int result_dup = -1;
	int progress_dup = -1;
	int rc;
	long max_fd, fd;
	bool result_moved = false;
	bool progress_moved = false;

	result_dup = fcntl(result_fd, F_DUPFD_CLOEXEC, 5);
	if (result_dup < 0)
		goto err;
	if (progress_fd >= 0) {
		progress_dup = fcntl(progress_fd, F_DUPFD_CLOEXEC, 5);
		if (progress_dup < 0)
			goto err;
	}
	rc = dup2(result_dup, 3);
	if (rc < 0)
		goto err;
	result_moved = true;
	if (progress_dup >= 0) {
		rc = dup2(progress_dup, 4);
		if (rc < 0)
			goto err;
		progress_moved = true;
	}
#if defined(__linux__) && defined(SYS_close_range)
	rc = syscall(SYS_close_range, 5U, ~0U, 0U);
	if (!rc)
		return 0;
#endif
	max_fd = sysconf(_SC_OPEN_MAX);
	if (max_fd < 0)
		max_fd = 1024;
	for (fd = 5; fd < max_fd; fd++)
		close((int)fd);
	return 0;

err:
	if (result_dup >= 0) {
		close(result_dup);
		result_dup = -1;
	}
	if (progress_dup >= 0) {
		close(progress_dup);
		progress_dup = -1;
	}
	if (result_moved)
		close(3);
	if (progress_moved)
		close(4);
	return -1;
}

/*
 * Fork-per-tool execution. Each tool call runs in its own child that creates a
 * fresh transient builder, internalizes the arguments, sanitizes the
 * environment, applies the sandbox, runs the tool, and writes the result string
 * back over a pipe. The parent waits on the result pipe and the child together
 * through fyai_event, so spawning several jobs and collecting them from one
 * loop is a change to the caller, not to the job lifecycle; today collect()
 * still runs right after spawn().
 *
 * The pollable-child handle (pidfd on Linux, EVFILT_PROC/NOTE_EXIT on macOS)
 * now lives in the event backend, so nothing here is Linux-specific any more -
 * only fyai_should_fork_tool() still gates the path per platform.
 *
 * Asynchronous jobs transport the canonical JSON result separately from raw
 * progress. Legacy single-tool forks retain their string result transport.
 * A framed or shared-arena transport can replace either pipe without changing
 * the submission and collection lifecycle.
 */
/*
 * The job owns its source pointers so withdrawing is idempotent: the callbacks
 * retire a source when it is spent and the collect path retires whatever is
 * left, and neither has to know what the other did. The loop is shared and
 * outlives the job, so a source left behind would point at freed storage.
 */
struct fyai_tool_job {
	pid_t pid;
	int rfd;
	int pfd;
	struct response_buffer buf;
	struct fyai_fenced_stream stream;
	struct fytim_workband *band;
	char *title;
	struct fyai_event_source *rsrc;
	struct fyai_event_source *psrc;
	struct fyai_event_source *csrc;
	bool out_open;
	bool progress_open;
	bool reaped;
	bool failed;
	bool result_ok;
	bool done;
	bool json_result;
	bool native_shell;
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

	job->done = job->reaped && !job->out_open && !job->progress_open;
	if (!was_done && job->done && job->stream.active) {
		(void)fyai_fenced_stream_set_indicator(&job->stream,
			job->result_ok && !job->failed ?
			FYMD_INDICATOR_SUCCESS : FYMD_INDICATOR_FAILURE, 0);
		(void)fyai_fenced_stream_push(&job->stream, NULL, 0);
	}
	if (!was_done && job->done && job->group)
		fyai_tool_job_group_service(job->group);
}

static enum fyai_event_action
fyai_tool_job_readable(const struct fyai_event *ev)
{
	struct fyai_tool_job *job = ev->userdata;
	char chunk[4096];
	ssize_t r;

	for (;;) {
		r = read(ev->fd, chunk, sizeof(chunk));
		if (r > 0) {
			if (response_buffer_reserve(&job->buf,
						    job->buf.len + (size_t)r + 1)) {
				job->failed = true;
				break;
			}
			memcpy(job->buf.data + job->buf.len, chunk, (size_t)r);
			job->buf.len += (size_t)r;
			job->buf.data[job->buf.len] = '\0';
			continue;
		}
		if (r < 0 && errno == EINTR)
			continue;
		break;
	}
	if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
	    !job->failed)
		return FYAIEA_CONTINUE;

	job->out_open = false;
	fyai_tool_job_drop(&job->rsrc);
	fyai_tool_job_update_done(job);
	return FYAIEA_CONTINUE;
}

static enum fyai_event_action
fyai_tool_job_progress(const struct fyai_event *ev)
{
	struct fyai_tool_job *job = ev->userdata;
	char chunk[4096];
	ssize_t r;

	for (;;) {
		r = read(ev->fd, chunk, sizeof(chunk));
		if (r > 0) {
			if (job->stream.active &&
			    !data_is_binary(chunk, (size_t)r))
				(void)fyai_fenced_stream_push(&job->stream,
							      chunk,
							      (size_t)r);
			continue;
		}
		if (r < 0 && errno == EINTR)
			continue;
		break;
	}
	if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
		return FYAIEA_CONTINUE;
	job->progress_open = false;
	fyai_tool_job_drop(&job->psrc);
	fyai_tool_job_update_done(job);
	return FYAIEA_CONTINUE;
}

static enum fyai_event_action fyai_tool_job_child(const struct fyai_event *ev)
{
	struct fyai_tool_job *job = ev->userdata;

	/* One-shot: the loop retires this source as soon as we return, so drop
	 * our reference rather than leaving it dangling for the collect path. */
	job->csrc = NULL;
	job->reaped = true;
	job->result_ok = WIFEXITED(ev->status) && WEXITSTATUS(ev->status) == 0;
	job->term_signal = WIFSIGNALED(ev->status) ?
				WTERMSIG(ev->status) : 0;
	fyai_tool_job_update_done(job);
	return FYAIEA_CONTINUE;
}

static int fyai_tool_job_spawn(struct fyai_ctx *ctx, const char *name,
			       const char *args_text,
			       struct fyai_tool_job *job, bool async_job)
{
	int p[2] = { -1, -1 };
	int progress[2] = { -1, -1 };
	pid_t pid;
	fy_generic args, result;
	bool ok;
	const char *s;
	size_t len, off;
	ssize_t w;
	int rc;

	memset(job, 0, sizeof(*job));
	job->rfd = -1;
	job->pfd = -1;
	job->json_result = async_job;
	rc = pipe(p);
	fyai_error_check(ctx, !rc, err,
			 "could not create tool result pipe: %s",
			 strerror(errno));
	rc = async_job ? pipe(progress) : 0;
	fyai_error_check(ctx, !rc, err,
			 "could not create tool progress pipe: %s",
			 strerror(errno));
	rc = fcntl(p[0], F_SETFD, FD_CLOEXEC);
	fyai_error_check(ctx, !rc, err,
		"could not protect tool pipe descriptors: %s",
		strerror(errno));
	rc = fcntl(p[1], F_SETFD, FD_CLOEXEC);
	fyai_error_check(ctx, !rc, err,
		"could not protect tool pipe descriptors: %s",
		strerror(errno));
	if (async_job) {
		rc = fcntl(progress[0], F_SETFD, FD_CLOEXEC);
		fyai_error_check(ctx, !rc, err,
			"could not protect tool pipe descriptors: %s",
			strerror(errno));
		rc = fcntl(progress[1], F_SETFD, FD_CLOEXEC);
		fyai_error_check(ctx, !rc, err,
			"could not protect tool pipe descriptors: %s",
			strerror(errno));
	}
	rc = fcntl(p[0], F_SETFL, O_NONBLOCK);
	fyai_error_check(ctx, !rc, err,
			 "could not make tool result pipe non-blocking: %s",
			 strerror(errno));
	if (async_job) {
		rc = fcntl(progress[0], F_SETFL, O_NONBLOCK);
		fyai_error_check(ctx, !rc, err,
			"could not make tool progress pipe non-blocking: %s",
			strerror(errno));
	}
	pid = fork();
	fyai_error_check(ctx, pid >= 0, err,
			 "could not fork tool process: %s", strerror(errno));

	if (!pid) {			/* child */
		close(p[0]);
		p[0] = -1;
		if (progress[0] >= 0)
			close(progress[0]);
		progress[0] = -1;
		if (async_job)
			(void)setpgid(0, 0);
		fyai_ctx_loop_abandon(ctx);
		if (async_job) {
			if (fyai_tool_child_fds(p[1], progress[1]))
				_exit(126);
			p[1] = 3;
			progress[1] = 4;
			ctx->tool_progress_fd = progress[1];
			ctx->ui = NULL;
			ctx->shell_stream = NULL;
		}
		/* Fresh builder, then internalize the arguments into it. */
		rc = fyai_setup_transient_builder(ctx);
		if (rc)
			_exit(1);
		args = parse_json_string(ctx->transient_gb, args_text);
		if (fy_generic_is_invalid(args))
			_exit(1);
		fyai_env_sanitize();
		fyai_tool_apply_sandbox(ctx);
		result = async_job ?
			fyai_execute_tool_call(ctx, args, &ok) :
			fyai_tool_run_one(ctx, name, args, &ok);
		if (async_job)
			s = emit_json_string(ctx->transient_gb, result);
		else
			s = fy_generic_is_valid(result) ?
				fy_castp(&result, "") :
				"tool error: invalid tool result";
		if (!s)
			_exit(1);
		len = strlen(s);
		for (off = 0; off < len; off += (size_t)w) {
			w = write(p[1], s + off, len - off);
			if (w < 0) {
				if (errno == EINTR)
					w = 0;
				else
					_exit(1);
			}
		}
		_exit(ok ? 0 : 1);
	}

	close(p[1]);
	p[1] = -1;
	if (progress[1] >= 0)
		close(progress[1]);
	progress[1] = -1;
	if (async_job)
		(void)setpgid(pid, pid);
	job->pid = pid;
	job->rfd = p[0];
	p[0] = -1;
	job->pfd = progress[0];
	progress[0] = -1;
	job->out_open = true;
	job->progress_open = async_job;
	return 0;

err:
	if (p[0] >= 0) {
		close(p[0]);
		p[0] = -1;
	}
	if (p[1] >= 0) {
		close(p[1]);
		p[1] = -1;
	}
	if (progress[0] >= 0) {
		close(progress[0]);
		progress[0] = -1;
	}
	if (progress[1] >= 0) {
		close(progress[1]);
		progress[1] = -1;
	}
	return -1;
}

static fy_generic fyai_tool_job_wait_result(struct fyai_ctx *ctx,
					     struct fyai_tool_job *job)
{
	struct fyai_event_loop *el;
	int rc;

	el = fyai_ctx_loop(ctx);
	fyai_error_check(ctx, el, err,
			 "forked tool requires an event loop");
	rc = fyai_event_add_fd(el, job->rfd, FYAIEV_READ,
			       fyai_tool_job_readable, job, &job->rsrc);
	fyai_error_check(ctx, !rc, err,
			 "could not attach forked tool output");
	rc = fyai_event_add_child(el, job->pid, fyai_tool_job_child,
				  job, &job->csrc);
	fyai_error_check(ctx, !rc, err,
			 "could not attach forked tool process");
	rc = fyai_event_loop_run_until(el, &job->done, -1);
	fyai_error_check(ctx, !rc, err,
			 "forked tool event loop failed");

err:
	job->failed = !job->done;
	/* Drop sources before their job state and fd. */
	fyai_tool_job_drop(&job->rsrc);
	fyai_tool_job_drop(&job->csrc);
	if (job->rfd >= 0)
		close(job->rfd);
	job->rfd = -1;

	/* Reap a child the loop never acquired. */
	if (!job->reaped && job->pid > 0)
		while (waitpid(job->pid, NULL, 0) < 0 && errno == EINTR)
			;

	return fy_gb_internalize(ctx->transient_gb,
				 fy_value(job->buf.data ? job->buf.data : ""));
}

static void fyai_tool_job_live_close(struct fyai_tool_job *job)
{
	if (job->stream.active)
		fyai_fenced_stream_finish(&job->stream);
	fyai_ui_workband_destroy(job->band);
	job->band = NULL;
	free(job->title);
	job->title = NULL;
}

static int fyai_tool_job_attach(struct fyai_ctx *ctx,
				struct fyai_tool_job *job);

static void fyai_tool_job_discard(struct fyai_tool_job *job)
{
	if (!job)
		return;
	fyai_tool_job_cancel(job);
	fyai_tool_job_drop(&job->rsrc);
	fyai_tool_job_drop(&job->psrc);
	fyai_tool_job_drop(&job->csrc);
	if (job->rfd >= 0)
		close(job->rfd);
	job->rfd = -1;
	if (job->pfd >= 0)
		close(job->pfd);
	job->pfd = -1;
	if (!job->reaped && job->pid > 0)
		while (waitpid(job->pid, NULL, 0) < 0 && errno == EINTR)
			;
	fyai_tool_job_live_close(job);
	free(job->buf.data);
	free(job);
}

struct fyai_tool_job *fyai_tool_job_submit(struct fyai_ctx *ctx,
					    fy_generic tool_call)
{
	struct fyai_tool_job *job = NULL;
	const char *name, *args_text, *call_text, *command;
	fy_generic args;
	bool native_call;
	bool eligible;
	int rc;

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
	fyai_error_check(ctx, fy_generic_is_valid(args), err,
			 "invalid tool call arguments");
	call_text = emit_json_string(ctx->transient_gb, tool_call);
	fyai_error_check(ctx, call_text, err,
			 "could not serialize tool call");
	job = calloc(1, sizeof(*job));
	fyai_error_check(ctx, job, err,
			 "could not allocate tool job");
	rc = fyai_tool_job_spawn(ctx, name, call_text, job, true);
	fyai_error_check(ctx, !rc, err,
		"could not spawn tool job");
	job->native_shell = native_call;
	if (fy_equal(name, "shell") && fyai_ui_active(ctx)) {
		command = native_call ?
			fy_cast(fy_get_at_path(tool_call, "action",
					       "commands", 0), "") :
			fy_get(args, "command", "");
		job->title = fyai_format_shell_header(ctx,
					     *command ? command : name);
		job->band = fyai_ui_workband_create(ctx);
		if (job->band &&
		    !fyai_fenced_stream_start(&job->stream, ctx, ctx->cfg,
				NULL, ctx->cfg->tool_preview_lines > 0 ?
				(size_t)ctx->cfg->tool_preview_lines : 0,
				FYAI_TOOL_OUTPUT_INDENT, stderr, true)) {
			job->stream.band = job->band;
			job->stream.title = job->title;
			fyai_ui_workband_update(ctx, job->band,
						job->title, NULL, 0, NULL);
		} else {
			fyai_tool_job_live_close(job);
		}
	}
	rc = fyai_tool_job_attach(ctx, job);
	fyai_error_check(ctx, !rc, err,
			 "could not attach tool job to event loop");
	return job;

err:
	fyai_tool_job_discard(job);
	return NULL;
}

static int fyai_tool_job_attach(struct fyai_ctx *ctx,
				struct fyai_tool_job *job)
{
	struct fyai_event_loop *el;
	int rc;

	fyai_error_check(ctx, job, err,
			 "cannot attach an empty tool job");
	el = fyai_ctx_loop(ctx);
	fyai_error_check(ctx, el, err,
			 "tool job requires an event loop");
	rc = fyai_event_add_fd(el, job->rfd, FYAIEV_READ,
			       fyai_tool_job_readable, job, &job->rsrc);
	fyai_error_check(ctx, !rc, err,
			 "could not attach tool result pipe");
	if (job->pfd >= 0) {
		rc = fyai_event_add_fd(el, job->pfd, FYAIEV_READ,
				       fyai_tool_job_progress, job,
				       &job->psrc);
		fyai_error_check(ctx, !rc, err,
				 "could not attach tool progress pipe");
	}
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

void fyai_tool_job_cancel(struct fyai_tool_job *job)
{
	int rc;

	if (!job || job->reaped || job->pid <= 0)
		return;
	rc = kill(-job->pid, SIGTERM);
	if (rc && errno == ESRCH)
		(void)kill(job->pid, SIGTERM);
}

fy_generic fyai_tool_job_collect(struct fyai_ctx *ctx,
				 struct fyai_tool_job *job, bool *okp)
{
	fy_generic result;
	int status;
	pid_t rc;

	if (!job)
		return fy_invalid;
	/*
	 * Collection is deliberately non-blocking. The application event loop
	 * owns progress until done becomes true; callers may collect afterward
	 * without re-entering or privately driving that loop.
	 */
	if (!job->done) {
		*okp = false;
		return fy_invalid;
	}
	fyai_tool_job_live_close(job);
	fyai_tool_job_drop(&job->rsrc);
	fyai_tool_job_drop(&job->psrc);
	fyai_tool_job_drop(&job->csrc);
	if (job->rfd >= 0)
		close(job->rfd);
	job->rfd = -1;
	if (job->pfd >= 0)
		close(job->pfd);
	job->pfd = -1;
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
	if (job->term_signal)
		result = fy_invalid;
	else if (job->json_result)
		result = parse_json_string(ctx->transient_gb,
				job->buf.data ? job->buf.data : "");
	else
		result = fy_gb_internalize(ctx->transient_gb,
				fy_value(job->buf.data ? job->buf.data : ""));
	if (fy_generic_is_invalid(result) && job->term_signal) {
		if (job->native_shell)
			result = fy_sequence(fy_mapping(
				"stdout", "",
				"stderr", "",
				"outcome", fy_mapping(
					"type", "signal",
					"signal", job->term_signal)));
		else
			result = fy_value(ctx->transient_gb,
					  "tool error: interrupted");
	}
	*okp = job->result_ok && !job->failed;
	free(job->buf.data);
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
		name = fy_generic_is_valid(call) ?
			fyai_tool_call_name(group->ctx, call) : NULL;
		if (name && fyai_mcp_tool_name(name)) {
			args = fyai_tool_call_args(group->ctx, call);
			entry->mcp_request =
				fy_generic_is_valid(args) ?
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
			text = fy_generic_is_valid(result) ?
				emit_json_string(group->ctx->transient_gb,
						 result) : NULL;
			entry->result_text = text ? strdup(text) : NULL;
			entry->state = entry->result_text ?
				FYAITGS_PARKED : FYAITGS_SUBMIT_FAILED;
			group->parked++;
			continue;
		}
		entry->job = fy_generic_is_valid(call) ?
			fyai_tool_job_submit(group->ctx, call) : NULL;
		if (!entry->job) {
			entry->state = FYAITGS_SUBMIT_FAILED;
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
	fyai_tool_job_group_notify(group);
}

int fyai_tool_job_group_submit(struct fyai_tool_job_group *group)
{
	if (!group || group->submitted || !group->count)
		return -1;
	group->submitted = true;
	group->sealed = true;
	fyai_tool_job_group_dispatch(group);
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
		return fy_sequence(fy_mapping(
			"stdout", "",
			"stderr", "",
			"outcome", fy_mapping(
				"type", "signal",
				"signal", SIGTERM)));
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

/* Run one named tool in a forked, sandboxed child and return its result. */
static fy_generic fyai_tool_run_forked(struct fyai_ctx *ctx, const char *name,
				       fy_generic args, bool *okp)
{
	struct fyai_tool_job job;
	fy_generic result;
	const char *args_text;

	args_text = emit_json_string(ctx->transient_gb, args);
	if (!args_text ||
	    fyai_tool_job_spawn(ctx, name, args_text, &job, false))
		return fy_value(ctx->transient_gb,
				"tool error: failed to spawn tool process");
	result = fyai_tool_job_wait_result(ctx, &job);
	*okp = job.result_ok && !job.failed;
	free(job.buf.data);
	return result;
}

/* Whether this tool call should run in a forked child rather than in-process. */
static bool fyai_should_fork_tool(struct fyai_ctx *ctx, const char *name)
{
	if (ctx->sandbox_applied)	/* already inside a tool child */
		return false;
	if (!ctx->cfg->enable_sandbox)
		return false;
	if (fy_equal(name, "ask_user"))
		return false;
	return true;
}

int fyai_run_tool_verb(struct fyai_ctx *ctx)
{
	struct fyai_tool_args *a = &ctx->cfg->cmd.args.tool;
	bool ok;
	fy_generic args, result;
	char *stdin_buf = NULL;
	const char *args_text;
	int ret = -1;

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
	fyai_error_check(ctx, fy_generic_is_valid(args), out,
			 "invalid JSON arguments");

	/*
	 * Sanitize the environment and confine this process before running the
	 * tool: the one-shot process *is* the sandboxed context, so no fork is
	 * needed. The shell tool still forks internally to capture output and
	 * inherits this confinement.
	 */
	fyai_env_sanitize();
	fyai_tool_apply_sandbox(ctx);

	result = fyai_tool_run_one(ctx, a->name, args, &ok);
	result = fy_gb_internalize(ctx->transient_gb, result);

	if (fy_generic_is_string(result))
		printf("%s\n", fy_castp(&result, ""));
	else
		emit_generic_to_stdout(NULL, result, ctx->cfg->pretty);
	ret = 0;
out:
	free(stdin_buf);
	fyai_cleanup_transient_builder(ctx);
	return ret;
}
