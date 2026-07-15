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

#if defined(__linux__)
#include <sys/syscall.h>
#include <sys/wait.h>
#include <poll.h>
#endif

#include <linenoise.h>

#include "fyai_config.h"
#include "fyai_display.h"
#include "fyai_markdown.h"
#include "fyai_patch.h"
#include "fyai_sandbox.h"
#include "fyai_terminal.h"
#include "fyai_tools.h"

static bool fyai_should_fork_tool(struct fyai_ctx *ctx, const char *name);
#if defined(__linux__)
static fy_generic fyai_tool_run_forked(struct fyai_ctx *ctx, const char *name,
				       fy_generic args);
#endif

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

/*
 * Render the live shell header to stderr through the one shared emitter the
 * history view uses (fyai_emit_tool_call), so the live and history headers stay
 * in sync - bold "shell" plus the inline-code command, themed identically.
 */
static void fyai_print_shell_header(struct fyai_ctx *ctx, const char *command)
{
	char *md = NULL;
	size_t mdlen = 0;
	FILE *mf = open_memstream(&md, &mdlen);

	if (!mf) {
		fprintf(stderr, "  shell %s\n", command);
		return;
	}
	fyai_emit_tool_call(mf, ctx->transient_gb, "shell",
			    fy_mapping("command", command), 0);
	fclose(mf);
	if (md && fyai_fprint_markdown(stderr, md, ctx->cfg))
		fputs(md, stderr);
	free(md);
}

void fyai_print_tool_call(struct fyai_ctx *ctx, fy_generic tool_call)
{
	struct fyai_cfg *cfg = ctx->cfg;
	const char *name;
	const char *args_text;
	const char *command;
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
		fyai_print_shell_header(ctx, *command ? command : name);
		ctx->shell_stream = calloc(1, sizeof(*ctx->shell_stream));
		if (ctx->shell_stream != NULL &&
		    fyai_fenced_stream_start(ctx->shell_stream, cfg, NULL,
					     cfg->tool_preview_lines > 0 ?
					     (size_t) cfg->tool_preview_lines : 0,
					     FYAI_TOOL_OUTPUT_INDENT, stderr,
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

	if (!len)
		return;
	(void)stream;

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

static char *fyai_run_shell_command(struct fyai_ctx *ctx, const char *command)
{
	struct fyai_cfg *cfg = ctx->cfg;
	struct shell_command_result result = {};
	struct response_buffer buf = {};
	struct fyai_shell_sandbox sb;
	const struct fyai_sandbox_spec *sandbox;
	const char *msg;
	char *ret = NULL;

	sandbox = fyai_shell_sandbox_begin(ctx, &sb);

	if (run_shell_command_capture_cb(command, &result,
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
		msg = fy_sprintfa("\ntool error: command killed by signal %d\n",
				  result.signal);
		if (response_buffer_append(&buf, msg))
			goto out;
	} else if (result.exit_code) {
		msg = fy_sprintfa("\ntool error: command exited with status %d\n",
				  result.exit_code);
		if (response_buffer_append(&buf, msg))
			goto out;
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
	line = linenoise(n ? "choose a number or type an answer> " : "> ");
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
			return assert_valid_generic(result, NULL);
		}
	}

	result = fy_value(ctx->transient_gb, line);
	free(line);
	return assert_valid_generic(result, NULL);
}

fy_generic fyai_execute_tool_call(struct fyai_ctx *ctx,
				  fy_generic tool_call)
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

	type = fy_get(tool_call, "type");
	if (fy_equal(type, "shell_call")) {

		action = fy_get(tool_call, "action");
		commands = fy_get(action, "commands");
		outputs = fy_seq_empty;
		sandbox = fyai_shell_sandbox_begin(ctx, &sb);

		fy_foreach(command, commands) {
			if (run_shell_command_capture_cb(fy_castp(&command, ""),
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

#if defined(__linux__)
	if (fyai_should_fork_tool(ctx, name))
		result_generic = fyai_tool_run_forked(ctx, name, args);
	else
#endif
		result_generic = fyai_tool_run_one(ctx, name, args);
out:
	result_generic = fy_gb_internalize(ctx->transient_gb, result_generic);
	return assert_valid_generic(result_generic, NULL);
}

fy_generic fyai_tool_run_one(struct fyai_ctx *ctx, const char *name,
			     fy_generic args)
{
	fy_generic result_generic;
	const char *path, *content;
	char *result;
	int rc;

	if (fy_equal(name, "read_file")) {
		path = fy_get(args, "path", "");
		result = read_text_file(path);
	} else if (fy_equal(name, "write_file")) {
		path = fy_get(args, "path", "");
		content = fy_get(args, "content", "");
		rc = write_text_file(path, content);
		result = strdup(!rc ? "ok" : "error");
	} else if (fy_equal(name, "apply_patch")) {
		result = fyai_apply_patch_text(fy_get(args, "patch", ""));
	} else if (fy_equal(name, "shell")) {
		result = fyai_run_shell_command(ctx, fy_get(args, "command", ""));
	} else if (fy_equal(name, "ask_user")) {
		return fyai_ask_user(ctx, args);
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

#if defined(__linux__)

/*
 * Fork-per-tool execution. Each tool call runs in its own child that creates a
 * fresh transient builder, internalizes the arguments, sanitizes the
 * environment, applies the sandbox, runs the tool, and writes the result string
 * back over a pipe. The parent tracks the child by a pidfd (a pollable fd), so
 * a later change can spawn several jobs and wait on all their pidfds at once for
 * parallel tool-call execution; today collect() runs right after spawn().
 *
 * macOS keeps the in-process path (fyai_should_fork_tool is false there); the
 * analog when we get to it is CLONE-free fork(2) plus kqueue EVFILT_PROC
 * (NOTE_EXIT) for the pollable child handle that pidfd provides here.
 *
 * Result transport is the raw result string for now (every forked tool returns
 * a string; ask_user, which returns structure, stays in-process). A framed or
 * shared-arena transport can replace it without touching the job lifecycle.
 */
struct fyai_tool_job {
	pid_t pid;
	int pidfd;		/* -1 when pidfd_open is unavailable */
	int rfd;		/* result pipe read end */
	struct response_buffer buf;
};

static int fyai_pidfd_open(pid_t pid)
{
	return (int)syscall(__NR_pidfd_open, pid, 0U);
}

static int fyai_tool_job_spawn(struct fyai_ctx *ctx, const char *name,
			       fy_generic args, struct fyai_tool_job *job)
{
	int p[2];
	pid_t pid;
	fy_generic result;
	const char *s;
	size_t len, off;
	ssize_t w;

	memset(job, 0, sizeof(*job));
	job->pidfd = -1;
	if (pipe(p))
		return -1;

	pid = fork();
	if (pid < 0) {
		close(p[0]);
		close(p[1]);
		return -1;
	}

	if (!pid) {			/* child */
		close(p[0]);
		/* Fresh builder, then internalize the arguments into it. */
		fyai_setup_transient_builder(ctx);
		args = fy_gb_internalize(ctx->transient_gb, args);
		fyai_env_sanitize();
		fyai_tool_apply_sandbox(ctx);

		result = fyai_tool_run_one(ctx, name, args);
		s = fy_castp(&result, "");
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
		_exit(0);
	}

	close(p[1]);
	job->pid = pid;
	job->rfd = p[0];
	job->pidfd = fyai_pidfd_open(pid);
	return 0;
}

static fy_generic fyai_tool_job_collect(struct fyai_ctx *ctx,
					struct fyai_tool_job *job)
{
#ifdef P_PIDFD
	siginfo_t si = {0};
#endif
	char chunk[4096];
	ssize_t r;

	/* Drain the result pipe to EOF (child closes it on exit). */
	while ((r = read(job->rfd, chunk, sizeof(chunk))) != 0) {
		if (r < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (response_buffer_reserve(&job->buf,
					    job->buf.len + (size_t)r + 1))
			break;
		memcpy(job->buf.data + job->buf.len, chunk, (size_t)r);
		job->buf.len += (size_t)r;
		job->buf.data[job->buf.len] = '\0';
	}
	close(job->rfd);

	/* Reap. waitid(P_PIDFD) exercises the pidfd; fall back to waitpid. */
#ifdef P_PIDFD
	if (job->pidfd >= 0) {
		waitid(P_PIDFD, (id_t)job->pidfd, &si, WEXITED);
	} else
#endif
		waitpid(job->pid, NULL, 0);
	if (job->pidfd >= 0)
		close(job->pidfd);

	return fy_gb_internalize(ctx->transient_gb,
				 fy_value(job->buf.data ? job->buf.data : ""));
}

/* Run one named tool in a forked, sandboxed child and return its result. */
static fy_generic fyai_tool_run_forked(struct fyai_ctx *ctx, const char *name,
				       fy_generic args)
{
	struct fyai_tool_job job;
	fy_generic result;

	if (fyai_tool_job_spawn(ctx, name, args, &job))
		return fy_value(ctx->transient_gb,
				"tool error: failed to spawn tool process");
	result = fyai_tool_job_collect(ctx, &job);
	free(job.buf.data);
	return result;
}

/*
 * Whether this tool call should run in a forked, sandboxed child rather than
 * in-process. ask_user is interactive (needs the tty and cannot pipe its
 * result), so it always runs in-process. Only meaningful on Linux and when the
 * sandbox is enabled; the fork path is the isolation/parallelism substrate.
 */
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

#else /* !__linux__ */

static bool fyai_should_fork_tool(struct fyai_ctx *ctx, const char *name)
{
	(void)ctx;
	(void)name;
	return false;			/* macOS: in-process, as before */
}

#endif /* __linux__ */

int fyai_run_tool_verb(struct fyai_ctx *ctx)
{
	struct fyai_tool_args *a = &ctx->cfg->cmd.args.tool;
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

	result = fyai_tool_run_one(ctx, a->name, args);
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
