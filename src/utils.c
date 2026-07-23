/*
 * utils.c - common fyai generic helpers
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_UNKNOWN

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libfyaml/libfyaml-align.h>

#include "fyai.h"
#include "fyai_diag.h"
#include "fyai_event.h"
#include "fyai_sandbox.h"
#include "fyai_terminal.h"

int response_buffer_reserve(struct response_buffer *buf, size_t need)
{
	size_t cap;
	char *data;

	if (need <= buf->cap)
		return 0;

	cap = buf->cap ? buf->cap : 4096;
	while (cap < need) {
		if (cap > SIZE_MAX / 2)
			return -1;
		cap *= 2;
	}

	data = realloc(buf->data, cap);
	if (!data)
		return -1;

	buf->data = data;
	buf->cap = cap;
	return 0;
}

size_t write_response(void *ptr, size_t size, size_t nmemb, void *userdata)
{
	struct response_buffer *buf = userdata;
	size_t bytes;

	bytes = size * nmemb;

	if (!bytes)
		return 0;

	if (response_buffer_reserve(buf, buf->len + bytes + 1))
		return 0;

	memcpy(buf->data + buf->len, ptr, bytes);
	buf->len += bytes;
	buf->data[buf->len] = '\0';

	return bytes;
}

/*
 * Detect binary data the way git does: a NUL byte within the inspected
 * prefix marks the buffer as non-text. This is also exactly the condition
 * under which a strlen-based length would be truncated, so callers must use
 * the real byte length rather than strlen for any reported count.
 */
bool data_is_binary(const char *data, size_t len)
{
	size_t scan;
	size_t i;

	scan = len < 8192 ? len : 8192;

	for (i = 0; i < scan; i++) {
		if (!data[i])
			return true;
	}
	return false;
}

int response_buffer_append(struct response_buffer *buf, const char *text)
{
	size_t len;

	len = strlen(text);
	if (response_buffer_reserve(buf, buf->len + len + 1))
		return -1;

	memcpy(buf->data + buf->len, text, len);
	buf->len += len;
	buf->data[buf->len] = '\0';
	return 0;
}

int append_header(struct curl_slist **headers, const char *header)
{
	struct curl_slist *next;

	next = curl_slist_append(*headers, header);
	if (!next)
		return -1;

	*headers = next;
	return 0;
}

char *make_header(const char *prefix, const char *value)
{
	size_t len;
	char *header;

	if (!value)
		return NULL;

	len = strlen(prefix) + strlen(value) + 1;
	header = malloc(len);
	if (!header)
		return NULL;

	snprintf(header, len, "%s%s", prefix, value);
	return header;
}

char *join_args(int argc, char **argv)
{
	size_t arg_len;
	size_t len;
	char *out;
	char *p;
	int i;

	len = 0;
	for (i = 0; i < argc; i++)
		len += strlen(argv[i]) + 1;

	out = malloc(len + 1);
	if (!out)
		return NULL;

	p = out;
	for (i = 0; i < argc; i++) {
		arg_len = strlen(argv[i]);
		if (i)
			*p++ = ' ';
		memcpy(p, argv[i], arg_len);
		p += arg_len;
	}
	*p = '\0';

	return out;
}

char *read_text_file(const char *path)
{
	struct response_buffer buf = {};
	char *msg;
	FILE *fp;
	size_t nread;

	fp = fopen(path, "rb");
	if (!fp)
		goto err;

	do {
		if (response_buffer_reserve(&buf, buf.len + 4096 + 1))
			goto err_close;
		nread = fread(buf.data + buf.len, 1, 4096, fp);
		buf.len += nread;
	} while (nread == 4096);

	if (ferror(fp))
		goto err_close;

	fclose(fp);

	if (data_is_binary(buf.data, buf.len)) {
		msg = malloc(64);
		if (msg)
			snprintf(msg, 64, "binary file: %zu bytes", buf.len);
		free(buf.data);
		return msg;
	}

	if (response_buffer_reserve(&buf, buf.len + 1))
		goto err;
	buf.data[buf.len] = '\0';
	return buf.data;

err_close:
	fclose(fp);
err:
	free(buf.data);
	return NULL;
}

int write_text_file(const char *path, const char *content)
{
	FILE *fp = NULL;
	int rc = -1;

	fp = fopen(path, "wb");
	if (!fp)
		goto err_out;

	if (fputs(content, fp) == EOF)
		goto err_out;

	if (fclose(fp))
		goto err_out;

	fp = NULL;

	rc = 0;

out:
	if (fp)
		fclose(fp);
	return rc;
err_out:
	rc = -1;
	goto out;
}

static int read_shell_pipe(int fd, struct response_buffer *buf,
			   enum shell_output_stream stream,
			   shell_output_fn output_fn, void *userdata)
{
	char tmp[4096];
	ssize_t nread;

	nread = read(fd, tmp, sizeof(tmp));
	if (nread < 0)
		return -1;
	if (!nread)
		return 0;

	if (response_buffer_reserve(buf, buf->len + (size_t)nread + 1))
		return -1;
	memcpy(buf->data + buf->len, tmp, (size_t)nread);
	buf->len += (size_t)nread;
	buf->data[buf->len] = '\0';
	if (output_fn)
		output_fn(userdata, stream, tmp, (size_t)nread);
	return 1;
}

/* Shell-capture state carried through the event callbacks. */
/* Each participant owns its own source pointer so that withdrawing is
 * idempotent. */
struct shell_capture {
	struct response_buffer *buf;
	enum shell_output_stream stream;
	shell_output_fn output_fn;
	void *userdata;
	struct shell_capture_job *job;
	struct fyai_event_source *src;
	bool open;
	bool failed;
};

struct shell_capture_job {
	struct shell_capture out;
	struct shell_capture err;
	struct fyai_event_source *csrc;
	bool reaped;
	int status;
	bool done;
};

static void shell_capture_drop(struct fyai_event_source **srcp)
{
	fyai_event_source_remove(*srcp);
	*srcp = NULL;
}

static void shell_capture_update_done(struct shell_capture_job *job)
{
	job->done = !job->out.open && !job->err.open && job->reaped;
}

static enum fyai_event_action shell_capture_readable(const struct fyai_event *ev)
{
	struct shell_capture *cap = ev->userdata;
	int rc;

	if (ev->events & (FYAIEV_READ | FYAIEV_EOF)) {
		/* Drain greedily rather than one read per wakeup. */
		for (;;) {
			rc = read_shell_pipe(ev->fd, cap->buf, cap->stream,
					     cap->output_fn, cap->userdata);
			if (rc > 0)
				continue;
			if (rc < 0 && errno == EINTR)
				continue;
			break;
		}
		if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return FYAIEA_CONTINUE;
		if (rc < 0)
			cap->failed = true;
	} else if (ev->events & FYAIEV_ERROR) {
		cap->failed = true;
	} else {
		return FYAIEA_CONTINUE;
	}

	cap->open = false;
	shell_capture_drop(&cap->src);
	shell_capture_update_done(cap->job);
	return FYAIEA_CONTINUE;
}

static enum fyai_event_action shell_capture_child(const struct fyai_event *ev)
{
	struct shell_capture_job *job = ev->userdata;

	/* One-shot: the loop retires this source as soon as we return, so drop
	 * our reference rather than leaving it dangling for the cleanup path. */
	job->csrc = NULL;
	job->reaped = true;
	job->status = ev->status;
	shell_capture_update_done(job);
	return FYAIEA_CONTINUE;
}

int run_shell_command_capture_cb(struct fyai_ctx *ctx, const char *command,
				 struct shell_command_result *result,
				 shell_output_fn output_fn,
				 void *userdata,
				 const struct fyai_sandbox_spec *sandbox)
{
	struct response_buffer stdout_buf = {};
	struct response_buffer stderr_buf = {};
	struct shell_capture_job job = {};
	struct fyai_event_loop *el = NULL;
	int stdout_pipe[2] = { -1, -1 };
	int stderr_pipe[2] = { -1, -1 };
	int status = 0;
	pid_t pid = -1;
	int ret = -1;

	memset(result, 0, sizeof(*result));

	if (pipe(stdout_pipe) || pipe(stderr_pipe))
		goto out;

	pid = fork();
	if (pid < 0)
		goto out;

	if (!pid) {
		/* The application loop (including its signal mask/signalfd or
		 * kqueue state) belongs to fyai, never to the executed command. */
		fyai_ctx_loop_abandon(ctx);
		close(stdout_pipe[0]);
		close(stderr_pipe[0]);
		dup2(stdout_pipe[1], STDOUT_FILENO);
		dup2(stderr_pipe[1], STDERR_FILENO);
		close(stdout_pipe[1]);
		close(stderr_pipe[1]);
		/*
		 * Confine the tool before handing control to the shell:
		 * inherited across the exec and every process it spawns.
		 * A strict-mode failure aborts the exec (126); non-strict
		 * failures fall through to run unconfined.
		 */
		if (sandbox && fyai_sandbox_apply(sandbox))
			_exit(126);
		execl("/bin/sh", "sh", "-c", command, NULL);
		_exit(127);
	}

	close(stdout_pipe[1]);
	close(stderr_pipe[1]);
	stdout_pipe[1] = -1;
	stderr_pipe[1] = -1;

	fcntl(stdout_pipe[0], F_SETFL, O_NONBLOCK);
	fcntl(stderr_pipe[0], F_SETFL, O_NONBLOCK);

	/* Watch both pipes and the child together. */
	job.out.buf = &stdout_buf;
	job.out.stream = SHELL_OUTPUT_STDOUT;
	job.err.buf = &stderr_buf;
	job.err.stream = SHELL_OUTPUT_STDERR;
	job.out.output_fn = job.err.output_fn = output_fn;
	job.out.userdata = job.err.userdata = userdata;
	job.out.job = job.err.job = &job;
	job.out.open = job.err.open = true;

	el = fyai_ctx_loop(ctx);
	if (!el)
		goto out_wait;

	if (fyai_event_add_fd(el, stdout_pipe[0], FYAIEV_READ,
			      shell_capture_readable, &job.out, &job.out.src) ||
	    fyai_event_add_fd(el, stderr_pipe[0], FYAIEV_READ,
			      shell_capture_readable, &job.err, &job.err.src) ||
	    fyai_event_add_child(el, pid, shell_capture_child, &job, &job.csrc))
		goto out_wait;

	/* Clear the pid once the loop owns the reap. */
	if (fyai_event_loop_run_until(el, &job.done, -1))
		goto out_wait;

	if (job.reaped)
		pid = -1;
	if (!job.done || job.out.failed || job.err.failed)
		goto out_wait;

	status = job.status;

	result->stdout_data = stdout_buf.data ? stdout_buf.data : strdup("");
	result->stderr_data = stderr_buf.data ? stderr_buf.data : strdup("");
	result->stdout_len = stdout_buf.len;
	result->stderr_len = stderr_buf.len;
	stdout_buf.data = NULL;
	stderr_buf.data = NULL;
	if (!result->stdout_data || !result->stderr_data)
		goto out;

	if (WIFSIGNALED(status)) {
		result->signaled = true;
		result->signal = WTERMSIG(status);
	} else if (WIFEXITED(status)) {
		result->exit_code = WEXITSTATUS(status);
	}

	ret = 0;
	goto out;

out_wait:
	if (pid > 0) {
		while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
			;
		pid = -1;
	}
out:
	/* Withdraw sources before closing their pipes. */
	shell_capture_drop(&job.out.src);
	shell_capture_drop(&job.err.src);
	shell_capture_drop(&job.csrc);

	if (pid > 0)
		while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
			;
	if (stdout_pipe[0] >= 0)
		close(stdout_pipe[0]);
	if (stdout_pipe[1] >= 0)
		close(stdout_pipe[1]);
	if (stderr_pipe[0] >= 0)
		close(stderr_pipe[0]);
	if (stderr_pipe[1] >= 0)
		close(stderr_pipe[1]);
	free(stdout_buf.data);
	free(stderr_buf.data);
	if (ret)
		shell_command_result_cleanup(result);
	return ret;
}

void shell_command_result_cleanup(struct shell_command_result *result)
{
	if (!result)
		return;

	free(result->stdout_data);
	free(result->stderr_data);
	memset(result, 0, sizeof(*result));
}

void emit_generic_to_stdout(const char *label, fy_generic value, bool pretty)
{
	emit_generic_to_stdout_anchored(label, value, pretty, false);
}

void emit_generic_to_stdout_anchored(const char *label, fy_generic value,
				     bool pretty, bool auto_anchor)
{
	enum fy_op_emit_flags flags;

	if (pretty) {
		flags = FYOPEF_DISABLE_DIRECTORY |
			FYOPEF_OUTPUT_TYPE_STDOUT |
			FYOPEF_MODE_YAML_1_2 |
			FYOPEF_STYLE_PRETTY |
			FYOPEF_WIDTH_INF |
			FYOPEF_OUTPUT_COMMENTS;
	} else {
		flags = FYOPEF_DISABLE_DIRECTORY |
			FYOPEF_OUTPUT_TYPE_STDOUT |
			FYOPEF_MODE_JSON |
			FYOPEF_STYLE_COMPACT |
			FYOPEF_WIDTH_INF;
	}
	if (auto_anchor)
		flags |= FYOPEF_AUTO_ANCHOR;

	if (label)
		printf("\n# %s:\n", label);
	(void)fy_emit(value, flags, NULL);
}

static fy_generic make_string_property(struct fy_generic_builder *gb,
				       const char *description)
{
	fy_generic v;

	v = fy_mapping(gb,
		"type", "string",
		"description", description);
	return assert_valid_generic(v, "Unable to make string property");
}

static fy_generic make_string_array_property(struct fy_generic_builder *gb,
					     const char *description)
{
	fy_generic v;

	v = fy_mapping(gb,
		"type", "array",
		"description", description,
		"items", fy_mapping(gb, "type", "string"));
	return assert_valid_generic(v, "Unable to make array property");
}

static fy_generic make_tool_parameters(struct fy_generic_builder *gb,
				       fy_generic properties,
				       fy_generic required)
{
	fy_generic v;

	v = fy_mapping(gb,
		"type", "object",
		"properties", properties,
		"required", required,
		"additionalProperties", false);
	return assert_valid_generic(v, "Unable to make tool parameters");
}

static fy_generic make_function_tool(struct fy_generic_builder *gb,
				     const char *name,
				     const char *description,
				     fy_generic parameters)
{
	fy_generic v;

	v = fy_mapping(gb,
		"type", "function",
		"function", fy_mapping(
			"name", name,
			"description", description,
			"parameters", parameters));
	return assert_valid_generic(v, "Unable to build function tool generic");
}

fy_generic make_tools(struct fy_generic_builder *gb)
{
	fy_generic read_file_tool;
	fy_generic write_file_tool;
	fy_generic apply_patch_tool;
	fy_generic shell_tool;
	fy_generic ask_user_tool;
	fy_generic tools;

	read_file_tool = make_function_tool(gb,
		"read_file",
		"Read a UTF-8 text file from the workspace.",
		make_tool_parameters(gb,
			fy_mapping("path", make_string_property(gb, "Workspace-relative path to read.")),
			fy_sequence("path")));

	write_file_tool = make_function_tool(gb,
		"write_file",
		"Write UTF-8 text to a workspace file.",
		make_tool_parameters(gb,
			fy_mapping(
				"path", make_string_property(gb, "Workspace-relative path to write."),
				"content", make_string_property(gb, "Complete UTF-8 file contents.")),
			fy_sequence("path", "content")));

	/* claude distilled models have a big issue with that */
	apply_patch_tool = make_function_tool(gb,
		"apply_patch",
		"Apply a raw Codex-style patch to workspace files. Use "
		"*** Begin Patch / *** End Patch; hunks are *** Add File, "
		"*** Delete File, or *** Update File with optional "
		"*** Move to and @@ changes, plus optional *** End of File.",
		make_tool_parameters(gb,
			fy_mapping(
				"patch", make_string_property(gb,
					"Complete patch text to apply.")),
			fy_sequence("patch")));

	shell_tool = make_function_tool(gb,
		"shell",
		"Run a shell command in the workspace.",
		make_tool_parameters(gb,
			fy_mapping(
				"command", make_string_property(gb, "Shell command to execute.")),
				fy_sequence("command")));

	ask_user_tool = make_function_tool(gb,
		"ask_user",
		"Ask the user a question and wait for their answer. Use this "
		"when you need a decision or clarification only the user can "
		"give. Provide `options` to offer specific choices; the user "
		"may still answer freely.",
		make_tool_parameters(gb,
			fy_mapping(
				"question", make_string_property(gb, "The question to put to the user."),
				"options", make_string_array_property(gb,
					"Optional list of suggested answers to "
					"present as a numbered menu.")),
			fy_sequence("question")));

	tools = fy_sequence(read_file_tool, write_file_tool, apply_patch_tool,
				shell_tool, ask_user_tool);

	tools = fy_gb_internalize(gb, tools);
	return assert_valid_generic(tools, "Unable to make tools (utils)");
}

const char *emit_request_body(struct fy_generic_builder *gb, fy_generic request)
{
	fy_generic emitted;
	const char *body;

	emitted = fy_emit(gb, request,
		FYOPEF_DISABLE_DIRECTORY |
		FYOPEF_MODE_JSON |
		FYOPEF_STYLE_COMPACT |
		FYOPEF_WIDTH_INF |
		FYOPEF_NO_ENDING_NEWLINE,
		NULL);

	if (fy_generic_is_invalid(emitted))
		return NULL;

	body = fy_cast(emitted, "");
	if (!*body)
		return NULL;

	return fy_gb_intern_string(gb, body);
}

fy_generic parse_response(struct fy_generic_builder *gb, const char *response)
{
	fy_generic doc;

	doc = parse_json_string(gb, response);
	if (fy_generic_is_invalid(doc)) {
		fprintf(stderr, "Failed to parse JSON response\n");
		return fy_invalid;
	}

	return doc;
}

fy_generic response_content(struct fy_generic_builder *gb, fy_generic doc)
{
	return fy_get_at_path(gb, doc, "choices", 0, "message", "content");
}

fy_generic response_message(struct fy_generic_builder *gb, fy_generic doc)
{
	return fy_get_at_path(gb, doc, "choices", 0, "message");
}

fy_generic response_tool_calls(struct fy_generic_builder *gb, fy_generic doc)
{
	return fy_get_at_path(gb, doc, "choices", 0, "message", "tool_calls");
}

fy_generic fyai_join_strings(struct fy_generic_builder *gb, fy_generic chunks)
{
	struct fy_generic_op_args args = {};

	args.common.items = fy_generic_sequence_get_items(chunks,
							 &args.common.count);
	return fy_generic_op_args(gb, FYGBOPF_JOIN | FYGBOPF_MAP_ITEM_COUNT,
				  fy_value(fy_szstr_empty), &args);
}

/* find out if I'm being traced */
bool self_is_traced(void)
{
	char line[256];
	bool result = false;
	int pid;
	FILE *fp;

	fp = fopen("/proc/self/status", "r");
	if (fp) {
		while (fgets(line, sizeof(line), fp)) {
			if (strncmp(line, "TracerPid:", 10) == 0) {
			    sscanf(line + 10, "%d", &pid);
			    result = pid != 0;
			    break;
			}
		}
		fclose(fp);
	}
	return result;
}

bool self_is_valgrinded(void)
{
	FILE *fp;
	char buf[1024];
	bool result = false;

	fp = fopen("/proc/self/maps", "r");
	if (fp) {
		while (fgets(buf, sizeof(buf), fp)) {
			if (strstr(buf, "vgpreload")) {
				result = true;
				break;
			}
		}
		fclose(fp);
	}
	return result;
}

/* 0 limit satisfied, 1 not satisfied after reexec, or hard limit, -1 error */
int raise_stack(size_t bytes, char **argv)
{
	size_t aligned_size;
	struct rlimit rl;
	int rc;

	/* if traced or under valgrind, bail; ulimit must be set manually then */
	if (self_is_traced() || self_is_valgrinded())
		return 0;

	/* align to page size */
	aligned_size = fy_size_t_align(bytes, (size_t)sysconf(_SC_PAGESIZE));

	rc = getrlimit(RLIMIT_STACK, &rl);
	if (rc)
		return -1;

	/*
	fprintf(stderr, "stack_req = 0x%zx\n", aligned_size);
	fprintf(stderr, "stack_cur = 0x%zx\n", (size_t)rl.rlim_cur);
	fprintf(stderr, "stack_max = 0x%zx\n", (size_t)rl.rlim_max);
	*/

	/* current limit OK? good to go? */
	if (rl.rlim_cur >= aligned_size)
		return 0;

	/* if we have already raised the limit and it doesn't work bail */
	if (getenv("FYAI_STACK_LIMIT_RAISED"))
		return 1;

	/* we can never satisfy the limit */
	if (rl.rlim_max < aligned_size && rl.rlim_max != RLIM_INFINITY)
		return 1;

	/* change it */
	rl.rlim_cur = aligned_size;
	rc = setrlimit(RLIMIT_STACK, &rl);
	if (rc)
		return -1;

	/* mark that we tried */
	setenv("STACK_LIMIT_RAISED", "1", 1);

	/* and reexec ourselves */
	execv("/proc/self/exe", argv);

	/* should never ever happen */
	perror("execv");
	exit(127);
}

/*
 * Find an option's argument on the command line before the main option
 * parse. Used for --provider/--config/--env, which config loading needs
 * before the authoritative getopt pass runs. Handles "--opt val",
 * "--opt=val", "-o val", and "-oval". Returns a pointer into argv or NULL.
 */
const char *find_cli_option(int argc, char **argv, const char *long_opt, char short_opt)
{
	size_t long_len = strlen(long_opt);
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--"))
			break;
		if (!strcmp(argv[i], long_opt) ||
		    (short_opt && argv[i][0] == '-' &&
		     argv[i][1] == short_opt &&
		     !argv[i][2])) {
			if (i + 1 < argc)
				return argv[i + 1];
		} else if (!strncmp(argv[i], long_opt, long_len) &&
			   argv[i][long_len] == '=') {
			return argv[i] + long_len + 1;
		} else if (short_opt && argv[i][0] == '-' &&
			   argv[i][1] == short_opt && argv[i][2]) {
			return argv[i] + 2;
		}
	}
	return NULL;
}

/* Whether a long flag (no argument) appears before "--". */
bool has_cli_flag(int argc, char **argv, const char *long_opt)
{
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--"))
			break;
		if (!strcmp(argv[i], long_opt))
			return true;
	}
	return false;
}

/* Slurp all of stdin into a NUL-terminated malloc'd buffer (for "-" prompts,
 * where stdin may be a non-seekable pipe). Returns NULL on error. */
char *read_all_stdin(void)
{
	size_t cap = 4096, len = 0;
	char *buf = malloc(cap);
	char *grown;
	ssize_t r;

	if (!buf)
		return NULL;
	for (;;) {
		if (len + 1 >= cap) {
			grown = realloc(buf, cap *= 2);

			if (!grown) {
				free(buf);
				return NULL;
			}
			buf = grown;
		}
		r = read(STDIN_FILENO, buf + len, cap - len - 1);
		if (r < 0) {
			free(buf);
			return NULL;
		}
		if (r == 0)
			break;
		len += (size_t)r;
	}
	buf[len] = '\0';
	return buf;
}

/* Print @opt, coloring "--long"/"-s" cyan/green and "<arg>" green. */
void usage_print_option(FILE *fp, bool color, const char *opt)
{
	const char *s, *e, *style;
	size_t len;

	for (s = opt; *s; s = e) {
		if (*s == ' ' || *s == ',') {
			fputc(*s, fp);
			e = s + 1;
			continue;
		}
		for (e = s; *e && *e != ' ' && *e != ','; e++)
			;
		len = (size_t)(e - s);

		style = NULL;
		if (len >= 2 && s[0] == '-' && s[1] == '-')
			style = FYAI_ANSI_CYAN;
		else if (s[0] == '-')
			style = FYAI_ANSI_GREEN;
		else if (s[0] == '<' && s[len - 1] == '>')
			style = FYAI_ANSI_GREEN;

		if (color && style)
			fputs(style, fp);
		fwrite(s, 1, len, fp);
		if (color && style)
			fputs(FYAI_ANSI_RESET, fp);
	}
}

void usage_item(FILE *fp, bool color, const char *opt, const char *desc)
{
	size_t len, i;

	fputs("  ", fp);
	usage_print_option(fp, color, opt);
	len = strlen(opt);
	for (i = len; i < 26; i++)
		fputc(' ', fp);
	fprintf(fp, " : %s\n", desc);
}

int str_in_set(const char *v, const char *const *opts)
{
	int i;

	for (i = 0; *opts; opts++, i++)
		if (!strcmp(v, *opts))
			return i;
	return -1;
}

bool executable_in_path(const char *name)
{
	const char *path, *start, *end;
	char candidate[4096];
	size_t dir_len, name_len;

	if (strchr(name, '/'))
		return access(name, X_OK) == 0;

	path = getenv("PATH");
	if (!path)
		return false;

	name_len = strlen(name);
	for (start = path; *start; start = *end ? end + 1 : end) {
		end = strchr(start, ':');
		if (!end)
			end = start + strlen(start);
		dir_len = (size_t)(end - start);
		if (!dir_len)
			continue;
		if (dir_len + 1 + name_len + 1 > sizeof(candidate))
			continue;
		memcpy(candidate, start, dir_len);
		candidate[dir_len] = '/';
		memcpy(candidate + dir_len + 1, name, name_len + 1);
		if (access(candidate, X_OK) == 0)
			return true;
	}

	return false;
}

bool is_mode_file(const char *path, int mode)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISREG(st.st_mode) &&
	       access(path, mode) == 0;
}

bool is_mode_directory(const char *path, int mode)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISDIR(st.st_mode) &&
	       access(path, mode) == 0;
}

bool is_readable_file(const char *path)
{
	return is_mode_file(path, R_OK);
}

bool is_writeable_file(const char *path)
{
	return is_mode_file(path, W_OK);
}

bool is_executable_file(const char *path)
{
	return is_mode_file(path, X_OK);
}

bool is_readable_directory(const char *path)
{
	return is_mode_directory(path, W_OK);
}

bool is_writable_directory(const char *path)
{
	return is_mode_directory(path, W_OK);
}

fy_generic parse_json_string(struct fy_generic_builder *gb, const char *str)
{
	return fy_parse(gb, str,
		FYOPPF_DISABLE_DIRECTORY |
		FYOPPF_INPUT_TYPE_STRING |
		FYOPPF_MODE_JSON,
		NULL);
}

fy_generic parse_json_string_size(struct fy_generic_builder *gb,
				  const char *str, size_t len)
{
	const fy_generic_sized_string szstr = {
		.data = str,
		.size = len,
	};

	return fy_parse(gb, szstr,
		FYOPPF_DISABLE_DIRECTORY |
		FYOPPF_INPUT_TYPE_STRING |
		FYOPPF_MODE_JSON,
		NULL);
}

fy_generic parse_json_generic(struct fy_generic_builder *gb, fy_generic v)
{
	return parse_json_string(gb, fy_cast(v, ""));
}

const char *emit_json_string(struct fy_generic_builder *gb, fy_generic v)
{
	fy_generic emitted;
	const char *body;

	emitted = fy_emit(gb, v,
		FYOPEF_DISABLE_DIRECTORY |
		FYOPEF_MODE_JSON |
		FYOPEF_STYLE_COMPACT |
		FYOPEF_WIDTH_INF |
		FYOPEF_NO_ENDING_NEWLINE,
		NULL);

	if (fy_generic_is_invalid(emitted))
		return NULL;

	/* intern while `emitted` is still in scope: a short result lives
	 * inline in the generic word itself (see CLAUDE.md on fy_cast) */
	body = fy_cast(emitted, "");
	return fy_gb_intern_string(gb, body);
}

/*
 * Stack-lifetime verification for out-of-place generics (Linux only).
 *
 * A builder-less scratch generic lives in the stack frame that created it;
 * returning one up the call chain leaves its backing pointer in an exited
 * frame. On a downward-growing stack an exited frame lies strictly below
 * every live local of the surviving callers, so: dead iff the backing
 * pointer is inside the thread stack region and below @live_floor - the
 * caller's stack pointer, captured at the call site by
 * generic_in_dead_stack_frame().
 *
 * Stack bounds come from the [stack] line of /proc/self/maps, cached after
 * the first read and refreshed only when a candidate falls below the cached
 * low bound (the region grows downward). fyai is a single-threaded,
 * one-invocation process, so the main-thread region is the only one that
 * matters.
 */
#ifdef __linux__
static uintptr_t fyai_stack_lo, fyai_stack_hi;

static bool fyai_stack_bounds_read(void)
{
	char line[256];
	unsigned long lo, hi;
	bool found = false;
	FILE *fp;

	fp = fopen("/proc/self/maps", "r");
	if (!fp)
		return false;
	while (fgets(line, sizeof(line), fp)) {
		if (!strstr(line, "[stack]"))
			continue;
		if (sscanf(line, "%lx-%lx", &lo, &hi) == 2) {
			fyai_stack_lo = lo;
			fyai_stack_hi = hi;
			found = true;
		}
		break;
	}
	fclose(fp);
	return found;
}

bool generic_ptr_in_dead_stack(fy_generic v, const void *live_floor)
{
	uintptr_t ptr;

	if (fy_generic_is_invalid(v) || fy_generic_is_in_place(v))
		return false;

	if (fy_generic_is_collection(v))
		ptr = (uintptr_t)fy_generic_resolve_collection_ptr(v);
	else
		ptr = (uintptr_t)fy_generic_resolve_ptr(v);
	if (!ptr)
		return false;

	if (!fyai_stack_hi && !fyai_stack_bounds_read())
		return false;

	/*
	 * The stack region grows downward after the bounds were cached; a
	 * candidate below the cached low bound may sit in newly mapped
	 * stack. Re-read the bounds only then - a genuine heap/arena/mmap
	 * pointer stays outside [stack] no matter how often we look.
	 */
	if (ptr < fyai_stack_lo && ptr < fyai_stack_hi)
		(void)fyai_stack_bounds_read();

	return ptr >= fyai_stack_lo && ptr < fyai_stack_hi &&
	       ptr < (uintptr_t)live_floor;
}
#else
bool generic_ptr_in_dead_stack(fy_generic v, const void *live_floor)
{
	(void)v;
	(void)live_floor;
	return false;
}
#endif

/*
 * Run $VISUAL/$EDITOR (else vi) on @path, blocking until the editor exits.
 * Returns 0 when the editor exited cleanly, -1 otherwise.
 */
static int fyai_spawn_editor_mode(struct fyai_ctx *ctx, const char *path,
				  bool readonly)
{
	const char *editor;
	char *cmd = NULL;
	pid_t pid, ret;
	int status, rc = -1;

	editor = getenv("VISUAL");
	if (!editor || !*editor)
		editor = getenv("EDITOR");
	if (!editor || !*editor)
		editor = "vi";
	fyai_error_check(ctx,
			 asprintf(&cmd, readonly ? "%s -R '%s'" : "%s '%s'",
				  editor, path) >= 0,
			 err_out, "could not format editor command");
	pid = fork();
	fyai_error_check(ctx, pid >= 0, err_out, "could not start editor: %s",
			 strerror(errno));
	if (!pid) {
		if (ctx && ctx->signal_mask_valid)
			(void)sigprocmask(SIG_SETMASK, &ctx->signal_mask, NULL);
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
	/*
	 * The editor handoff is synchronous today. If external editing becomes
	 * long-lived, register this pid as an event-loop child source and keep
	 * pumping non-UI work while the terminal frontend remains suspended.
	 */
	do {
		ret = waitpid(pid, &status, 0);
	} while (ret < 0 && errno == EINTR);
	fyai_error_check(ctx, ret >= 0, err_out, "could not wait for editor: %s",
			 strerror(errno));
	fyai_error_check(ctx, WIFEXITED(status) && !WEXITSTATUS(status), err_out,
			 "editor exited unsuccessfully");
	rc = 0;
err_out:
	free(cmd);
	return rc;
}

int fyai_spawn_editor(struct fyai_ctx *ctx, const char *path)
{
	return fyai_spawn_editor_mode(ctx, path, false);
}

int fyai_spawn_editor_readonly(struct fyai_ctx *ctx, const char *path)
{
	return fyai_spawn_editor_mode(ctx, path, true);
}

int mkdir_private(const char *path)
{
	struct stat st;

	if (!mkdir(path, 0700))
		return 0;
	if (errno != EEXIST || lstat(path, &st) || !S_ISDIR(st.st_mode))
		return -1;
	if ((st.st_mode & 077) && chmod(path, 0700))
		return -1;
	return 0;
}
