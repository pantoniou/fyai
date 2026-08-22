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
#include <sys/syscall.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <libfyaml/libfyaml-align.h>

#include "fyai.h"
#include "fyai_auth_util.h"
#include "fyai_diag.h"
#include "fyai_event.h"
#include "fyai_sink.h"
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

/* The number of bytes in the UTF-8 sequence that starts with @c, 0 if none. */
static size_t utf8_seq_len(unsigned char c)
{
	if (c < 0x80)
		return 1;
	if ((c & 0xe0) == 0xc0)
		return 2;
	if ((c & 0xf0) == 0xe0)
		return 3;
	if ((c & 0xf8) == 0xf0)
		return 4;
	return 0;
}

bool data_is_wire_text(const char *data, size_t len)
{
	unsigned char c;
	size_t i, n, k;

	for (i = 0; i < len; i += n) {
		c = (unsigned char)data[i];
		n = utf8_seq_len(c);
		if (!n || i + n > len)
			return false;
		if (n == 1 && c < 0x20 && c != '\t' && c != '\n' && c != '\r')
			return false;
		if (n == 1 && c == 0x7f)
			return false;
		for (k = 1; k < n; k++)
			if (((unsigned char)data[i + k] & 0xc0) != 0x80)
				return false;
	}
	return true;
}

fy_generic fyai_bytes_to_generic(struct fy_generic_builder *gb,
				 const char *data, size_t len)
{
	fy_generic value;
	char *copy;

	if (!gb || !data)
		return fy_invalid;

	if (data_is_wire_text(data, len)) {
		copy = malloc(len + 1);
		if (!copy)
			return fy_invalid;
		memcpy(copy, data, len);
		copy[len] = '\0';
		value = fy_mapping(gb, "text", fy_gb_intern_string(gb, copy));
		free(copy);
		return value;
	}

	copy = fyai_base64url_encode((const unsigned char *)data, len);
	if (!copy)
		return fy_invalid;
	value = fy_mapping(gb, "data", fy_gb_intern_string(gb, copy));
	free(copy);
	return value;
}

char *fyai_bytes_from_generic(fy_generic value, size_t *lenp)
{
	fy_generic held;
	const char *text;
	char *out;
	size_t len;

	*lenp = 0;
	held = fy_get(value, "text", fy_invalid);
	if (fy_is_string(held)) {
		text = fy_castp(&held, "");
		len = strlen(text);
		out = malloc(len + 1);
		if (!out)
			return NULL;
		memcpy(out, text, len + 1);
		*lenp = len;
		return out;
	}

	held = fy_get(value, "data", fy_invalid);
	if (!fy_is_string(held))
		return NULL;
	out = (char *)fyai_base64url_decode(fy_castp(&held, ""), lenp);
	return out;
}

int response_buffer_append(struct response_buffer *buf, const char *text)
{
	return response_buffer_append_data(buf, text, strlen(text));
}

int response_buffer_append_data(struct response_buffer *buf, const void *data,
				size_t len)
{
	if (response_buffer_reserve(buf, buf->len + len + 1))
		return -1;

	memcpy(buf->data + buf->len, data, len);
	buf->len += len;
	buf->data[buf->len] = '\0';
	return 0;
}

int response_buffer_append_line(struct response_buffer *buf, const void *data,
				size_t len)
{
	int rc;

	rc = response_buffer_append_data(buf, data, len);
	if (rc)
		return rc;
	return response_buffer_append_data(buf, "\n", 1);
}

void response_buffer_trim(struct response_buffer *buf)
{
	while (buf->len && (buf->data[buf->len - 1] == '\n' ||
			    buf->data[buf->len - 1] == '\r'))
		buf->len--;
	if (buf->data)
		buf->data[buf->len] = '\0';
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

char *read_text_file_limited(const char *path, size_t max_bytes, size_t *fullp)
{
	struct response_buffer buf = {};
	char drain[4096];
	size_t nread, want, total;
	char *msg;
	FILE *fp;

	if (fullp)
		*fullp = 0;

	fp = fopen(path, "rb");
	if (!fp)
		goto err;

	/* Keep the bounded prefix and count all bytes from the stream. */
	total = 0;
	do {
		want = sizeof(drain);
		if (max_bytes && buf.len + want > max_bytes)
			want = max_bytes - buf.len;
		if (!want)
			break;
		if (response_buffer_reserve(&buf, buf.len + want + 1))
			goto err_close;
		nread = fread(buf.data + buf.len, 1, want, fp);
		buf.len += nread;
		total += nread;
	} while (nread == want);

	while ((nread = fread(drain, 1, sizeof(drain), fp)) > 0)
		total += nread;

	if (ferror(fp))
		goto err_close;

	fclose(fp);

	if (fullp)
		*fullp = total;

	if (data_is_binary(buf.data, buf.len)) {
		msg = malloc(64);
		if (msg)
			snprintf(msg, 64, "binary file: %zu bytes", total);
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

char *read_text_file_window(const char *path, long long offset,
			    long long offset_bytes, long long limit,
			    size_t max_bytes,
			    struct read_text_info *info)
{
	struct read_text_info local = {};
	struct response_buffer buf = {};
	long long lineno, collected;
	char *line = NULL;
	size_t cap = 0;
	size_t take;
	size_t skip;
	size_t line_start;
	ssize_t len;
	char *msg;
	FILE *fp;

	if (offset < 1)
		offset = 1;
	lineno = 0;
	collected = 0;

	fp = fopen(path, "rb");
	if (!fp)
		goto err;

	/* Collect the window and count the complete input in one pass. */
	while ((len = getline(&line, &cap, fp)) > 0) {
		line_start = local.full_bytes;
		lineno++;
		local.full_bytes += (size_t)len;
		if (local.byte_capped)
			continue;
		skip = 0;
		if (offset_bytes >= 0) {
			if ((size_t)offset_bytes >= local.full_bytes)
				continue;
			if ((size_t)offset_bytes > line_start)
				skip = (size_t)offset_bytes - line_start;
		} else if (lineno < offset) {
			continue;
		}
		if (limit > 0 && collected >= limit)
			continue;

		take = (size_t)len - skip;
		if (max_bytes && buf.len + take > max_bytes) {
			take = max_bytes > buf.len ? max_bytes - buf.len : 0;
			local.byte_capped = true;
		}
		if (take) {
			if (response_buffer_reserve(&buf, buf.len + take + 1))
				goto err_close;
			if (!buf.len)
				local.first_byte = line_start + skip;
			memcpy(buf.data + buf.len, line + skip, take);
			buf.len += take;
			local.next_byte = line_start + skip + take;
			if (!local.first_line)
				local.first_line = lineno;
			local.last_line = lineno;
		}
		if (!local.byte_capped)
			collected++;
	}

	if (ferror(fp))
		goto err_close;
	free(line);
	line = NULL;
	fclose(fp);
	local.total_lines = lineno;

	if (buf.len && data_is_binary(buf.data, buf.len)) {
		local.binary = true;
		msg = malloc(64);
		if (msg)
			snprintf(msg, 64, "binary file: %zu bytes",
				 local.full_bytes);
		free(buf.data);
		if (info)
			*info = local;
		return msg;
	}

	if (response_buffer_reserve(&buf, buf.len + 1))
		goto err;
	buf.data[buf.len] = '\0';
	if (info)
		*info = local;
	return buf.data;

err_close:
	fclose(fp);
err:
	free(line);
	free(buf.data);
	return NULL;
}

char *read_text_file(const char *path)
{
	return read_text_file_limited(path, 0, NULL);
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
	struct fyai_event_source *tsrc;
	struct fyai_event_source *killer;
	struct fyai_event_source *deadline;
	struct fyai_ctx *ctx;
	pid_t pid;
	bool reaped;
	bool cancelling;
	bool isolated_pgrp;
	bool timed_out;
	int status;
	bool done;
};

static void shell_capture_signal_group(struct shell_capture_job *job, int sig);

static void shell_capture_drop(struct fyai_event_source **srcp)
{
	fyai_event_source_remove(*srcp);
	*srcp = NULL;
}

/* Capture is complete after the direct child exits. */
static void shell_capture_update_done(struct shell_capture_job *job)
{
	job->done = job->reaped;
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

/* Drain data that is available when the direct child exits. */
static void shell_capture_drain(struct shell_capture *cap, int fd)
{
	int rc;

	for (;;) {
		rc = read_shell_pipe(fd, cap->buf, cap->stream,
				     cap->output_fn, cap->userdata);
		if (rc > 0)
			continue;
		if (rc < 0 && errno == EINTR)
			continue;
		if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return;
		if (rc < 0)
			cap->failed = true;
		cap->open = false;
		return;
	}
}

static enum fyai_event_action shell_capture_child(const struct fyai_event *ev)
{
	struct shell_capture_job *job = ev->userdata;

	job->csrc = NULL;
	job->reaped = true;
	job->status = ev->status;
	/* Stop descendants that inherited capture descriptors. */
	shell_capture_signal_group(job, SIGKILL);
	shell_capture_update_done(job);
	return FYAIEA_CONTINUE;
}

static enum fyai_event_action shell_capture_signal(const struct fyai_event *ev)
{
	struct shell_capture_job *job = ev->userdata;

	/* Treat SIGINT and SIGTERM as shell-capture interrupts. */
	if (job && job->ctx &&
	    (ev->signo == SIGINT || ev->signo == SIGTERM))
		job->ctx->interrupt_pending = true;
	return FYAIEA_CONTINUE;
}

/* Maximum time for a command to stop after SIGTERM. */
#define SHELL_CAPTURE_KILL_MS 2000

static void shell_capture_signal_group(struct shell_capture_job *job, int sig)
{
	if (job->isolated_pgrp) {
		if (!kill(-job->pid, sig))
			return;
		if (errno != ESRCH)
			return;
	}
	if (!job->reaped)
		(void)kill(job->pid, sig);
}

/* Use SIGKILL when descendants keep the capture descriptors open. */
static enum fyai_event_action shell_capture_kill(const struct fyai_event *ev)
{
	struct shell_capture_job *job = ev->userdata;

	job->killer = NULL;
	if (job->pid > 0 && !job->reaped)
		shell_capture_signal_group(job, SIGKILL);
	return FYAIEA_CONTINUE;
}

static void shell_capture_cancel(pid_t pid, struct shell_capture_job *job)
{
	struct fyai_event_loop *el;

	if (pid <= 0 || !job || job->reaped || job->cancelling)
		return;

	job->cancelling = true;
	job->pid = pid;
	shell_capture_signal_group(job, SIGTERM);
	shell_capture_update_done(job);

	el = job->ctx ? fyai_ctx_loop(job->ctx) : NULL;
	if (el && !job->killer)
		(void)fyai_event_add_timer(el, SHELL_CAPTURE_KILL_MS, 0,
					   shell_capture_kill, job,
					   &job->killer);
}

/* Abort without the delayed kill timer. */
static void shell_capture_abort(pid_t pid, struct shell_capture_job *job,
				int *statusp)
{
	pid_t waited;

	if (pid <= 0 || !job)
		return;

	shell_capture_signal_group(job, SIGKILL);
	if (job->reaped)
		return;

	do {
		waited = waitpid(pid, statusp, 0);
	} while (waited < 0 && errno == EINTR);
	job->reaped = true;
}

/* The time limit expired. Stop the command in the same way as an interrupt. */
static enum fyai_event_action
shell_capture_deadline(const struct fyai_event *ev)
{
	struct shell_capture_job *job = ev->userdata;

	job->deadline = NULL;
	job->timed_out = true;
	shell_capture_cancel(job->pid, job);
	return FYAIEA_CONTINUE;
}

void fyai_close_fds_from(int lowfd)
{
	long max_fd, fd;
	int rc;

#if defined(__linux__) && defined(SYS_close_range)
	rc = (int)syscall(SYS_close_range, (unsigned int)lowfd, ~0U, 0U);
	if (!rc)
		return;
#else
	(void)rc;
#endif
	max_fd = sysconf(_SC_OPEN_MAX);
	if (max_fd < 0)
		max_fd = 1024;
	for (fd = lowfd; fd < max_fd; fd++)
		close((int)fd);
}

/* Check whether Linux reports @pid blocked reading descriptor 0. */
static bool proc_reads_stdin(pid_t pid)
{
	char path[64];
	char buf[256];
	long long nr = -1;
	long long fd = -1;
	ssize_t n;
	int f;

	snprintf(path, sizeof(path), "/proc/%ld/syscall", (long)pid);
	f = open(path, O_RDONLY | O_CLOEXEC);
	if (f < 0)
		return false;
	n = read(f, buf, sizeof(buf) - 1);
	close(f);
	if (n <= 0)
		return false;
	buf[n] = 0;
	/* "running" for a process on a processor: it waits for nothing. */
	if (sscanf(buf, "%lld %llx", &nr, &fd) != 2)
		return false;
	return (nr == SYS_read || nr == SYS_readv) && fd == 0;
}

/* The children of @pid, into @out. Returns how many were read. */
static size_t proc_children(pid_t pid, pid_t *out, size_t max)
{
	char path[80];
	char buf[512];
	size_t count = 0;
	const char *p;
	ssize_t n;
	long v;
	int f;

	snprintf(path, sizeof(path), "/proc/%ld/task/%ld/children",
		 (long)pid, (long)pid);
	f = open(path, O_RDONLY | O_CLOEXEC);
	if (f < 0)
		return 0;
	n = read(f, buf, sizeof(buf) - 1);
	close(f);
	if (n <= 0)
		return 0;
	buf[n] = 0;
	for (p = buf; *p && count < max; ) {
		while (*p == ' ')
			p++;
		v = strtol(p, (char **)&p, 10);
		if (v <= 0)
			break;
		out[count++] = (pid_t)v;
	}
	return count;
}

bool fyai_process_reads_stdin(pid_t pid)
{
#ifdef __linux__
	/* Search the bounded child tree for the process reading input. */
	pid_t queue[FYAI_PROC_WALK_MAX];
	size_t head = 0, tail = 0;
	size_t room;

	if (pid <= 0)
		return false;
	queue[tail++] = pid;
	while (head < tail) {
		pid = queue[head++];
		if (proc_reads_stdin(pid))
			return true;
		room = FYAI_PROC_WALK_MAX - tail;
		if (room)
			tail += proc_children(pid, queue + tail, room);
	}
	return false;
#else
	(void)pid;
	return false;
#endif
}

int fyai_child_exec_prepare(struct fyai_ctx *ctx,
			    const struct fyai_child_spec *spec)
{
	char num[16];

	/* The application loop belongs to fyai, never to the command. */
	fyai_ctx_loop_abandon(ctx);
	/* Isolate the child tree, optionally in a new session. */
	if (spec->own_session) {
		/* A leader already cannot start a session; a group still
		 * separates it from the group of this process. */
		if (setsid() < 0 && setpgid(0, 0) < 0)
			return FYAI_SHELL_EXIT_EXEC;
	} else if (setpgid(0, 0) < 0) {
		return FYAI_SHELL_EXIT_EXEC;
	}
	if (spec->ctty_fd >= 0 && ioctl(spec->ctty_fd, TIOCSCTTY, 0) < 0)
		return FYAI_SHELL_EXIT_EXEC;
	if (spec->in_fd >= 0 && dup2(spec->in_fd, STDIN_FILENO) < 0)
		return FYAI_SHELL_EXIT_EXEC;
	if (spec->out_fd >= 0 && dup2(spec->out_fd, STDOUT_FILENO) < 0)
		return FYAI_SHELL_EXIT_EXEC;
	if (spec->err_fd >= 0 && dup2(spec->err_fd, STDERR_FILENO) < 0)
		return FYAI_SHELL_EXIT_EXEC;
	/* Close all inherited descriptors beyond the installed standard ones. */
	fyai_close_fds_from(3);
	/* Fail closed if any provider credential cannot be removed. */
	if (fyai_env_sanitize())
		return FYAI_SHELL_EXIT_EXEC;
	/* Describe the child's terminal and screen dimensions. */
	if (spec->term)
		setenv("TERM", spec->term, 1);
	if (spec->rows > 0 && spec->cols > 0) {
		snprintf(num, sizeof(num), "%d", spec->rows);
		setenv("LINES", num, 1);
		snprintf(num, sizeof(num), "%d", spec->cols);
		setenv("COLUMNS", num, 1);
	} else if (spec->term) {
		unsetenv("LINES");
		unsetenv("COLUMNS");
	}
	/* Enter the directory before applying confinement. */
	if (spec->workdir && *spec->workdir && chdir(spec->workdir) < 0)
		return FYAI_SHELL_EXIT_WORKDIR;
	/* Confine the tool before handing control to the shell. */
	if (spec->sandbox && fyai_sandbox_apply(spec->sandbox))
		return FYAI_SHELL_EXIT_SANDBOX;
	return 0;
}

/* Replace this process with a command or interactive shell. */
void fyai_exec_shell_command(const char *command, const char *shell,
			     bool login)
{
	char argv0[64];
	const char *base;

	if (!shell || !*shell)
		shell = "/bin/sh";
	base = strrchr(shell, '/');
	base = base ? base + 1 : shell;
	snprintf(argv0, sizeof(argv0), "%s%s", login ? "-" : "", base);

	if (command && *command) {
		execl(shell, argv0, "-c", command, (char *)NULL);
		return;
	}
	execl(shell, argv0, (char *)NULL);
}

static void shell_capture_exec(struct fyai_ctx *ctx, const char *command,
			       const struct shell_command_opts *opts,
			       const struct fyai_sandbox_spec *sandbox,
			       int stdout_pipe[2], int stderr_pipe[2])
{
	struct fyai_child_spec spec = {};
	int devnull;
	int rc;

	close(stdout_pipe[0]);
	close(stderr_pipe[0]);
	/* Nothing here reads: a command that asks is answered end of input. */
	devnull = open("/dev/null", O_RDONLY);

	spec.in_fd = devnull;
	spec.out_fd = stdout_pipe[1];
	spec.err_fd = stderr_pipe[1];
	spec.ctty_fd = -1;
	spec.workdir = opts ? opts->workdir : NULL;
	spec.sandbox = sandbox;
	rc = fyai_child_exec_prepare(ctx, &spec);
	if (rc)
		_exit(rc);
	if (devnull < 0)
		close(STDIN_FILENO);
	fyai_exec_shell_command(command, opts ? opts->shell : NULL,
				opts && opts->login);
	_exit(FYAI_SHELL_EXIT_EXEC);
}

int run_shell_command_capture_cb(struct fyai_ctx *ctx, const char *command,
				 struct shell_command_result *result,
				 shell_output_fn output_fn,
				 void *userdata,
				 const struct fyai_sandbox_spec *sandbox,
				 const struct shell_command_opts *opts)
{
	unsigned int timeout_ms = opts ? opts->timeout_ms : 0;
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
		shell_capture_exec(ctx, command, opts, sandbox,
				   stdout_pipe, stderr_pipe);
	}
	/* Close the fork-to-exec race from the parent side. */
	do {
		ret = setpgid(pid, pid);
	} while (ret < 0 && errno == EINTR);
	job.isolated_pgrp = !ret || errno == EACCES;
	ret = -1;
	job.pid = pid;

	close(stdout_pipe[1]);
	close(stderr_pipe[1]);
	stdout_pipe[1] = -1;
	stderr_pipe[1] = -1;

	ret = fcntl(stdout_pipe[0], F_SETFL, O_NONBLOCK);
	fyai_error_check(ctx, ret >= 0, out_wait,
			 "could not make shell stdout nonblocking");
	ret = fcntl(stderr_pipe[0], F_SETFL, O_NONBLOCK);
	fyai_error_check(ctx, ret >= 0, out_wait,
			 "could not make shell stderr nonblocking");

	/* Watch both pipes and the child together. */
	job.out.buf = &stdout_buf;
	job.out.stream = SHELL_OUTPUT_STDOUT;
	job.err.buf = &stderr_buf;
	job.err.stream = SHELL_OUTPUT_STDERR;
	job.out.output_fn = job.err.output_fn = output_fn;
	job.out.userdata = job.err.userdata = userdata;
	job.out.job = job.err.job = &job;
	job.out.open = job.err.open = true;
	job.ctx = ctx;

	el = fyai_ctx_loop(ctx);
	if (!el)
		goto out_wait;

	if (timeout_ms) {
		ret = fyai_event_add_timer(el, timeout_ms, 0,
					   shell_capture_deadline,
					   &job, &job.deadline);
		fyai_error_check(ctx, !ret, out_wait,
				 "could not arm the shell time limit");
	}

	if (fyai_event_add_fd(el, stdout_pipe[0], FYAIEV_READ,
			      shell_capture_readable, &job.out, &job.out.src) ||
	    fyai_event_add_fd(el, stderr_pipe[0], FYAIEV_READ,
			      shell_capture_readable, &job.err, &job.err.src) ||
	    fyai_event_add_child(el, pid, shell_capture_child, &job, &job.csrc) ||
	    /* Do not block SIGINT. The watchdog handler must receive it. */
	    fyai_event_add_signal(el, SIGTERM, shell_capture_signal, &job,
				  &job.tsrc))
		goto out_wait;

	/* Clear the pid once the loop owns the reap. */
	while (!job.done) {
		if (fyai_interrupt_pending(ctx)) {
			fyai_event_interrupt_ack(ctx);
			shell_capture_cancel(pid, &job);
		}
		ret = fyai_event_loop_step(el, -1);
		fyai_error_check(ctx, ret >= 0, out_wait,
				 "shell capture event loop failed");
	}
	shell_capture_drain(&job.out, stdout_pipe[0]);
	shell_capture_drain(&job.err, stderr_pipe[0]);

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

	result->timed_out = job.timed_out;
	if (WIFSIGNALED(status)) {
		result->signaled = true;
		result->signal = WTERMSIG(status);
	} else if (WIFEXITED(status)) {
		result->exit_code = WEXITSTATUS(status);
	}

	ret = 0;
	goto out;

out_wait:
	shell_capture_abort(pid, &job, &status);
	pid = -1;
out:
	/* Withdraw sources before closing their pipes. */
	shell_capture_drop(&job.out.src);
	shell_capture_drop(&job.err.src);
	shell_capture_drop(&job.csrc);
	shell_capture_drop(&job.tsrc);
	shell_capture_drop(&job.killer);
	shell_capture_drop(&job.deadline);

	if (pid > 0)
		shell_capture_abort(pid, &job, NULL);
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

void emit_generic_to_stdout(struct fyai_ctx *ctx, const char *label,
			    fy_generic value, bool pretty)
{
	emit_generic_to_stdout_anchored(ctx, label, value, pretty, false);
}

/* Send emitted machine data through the sink without rendering it. */
void emit_generic_to_stdout_anchored(struct fyai_ctx *ctx, const char *label,
				     fy_generic value,
				     bool pretty, bool auto_anchor)
{
	enum fy_op_emit_flags flags;
	fy_generic emitted;
	const char *text;

	if (pretty) {
		flags = FYOPEF_DISABLE_DIRECTORY |
			FYOPEF_MODE_YAML_1_2 |
			FYOPEF_STYLE_PRETTY |
			FYOPEF_WIDTH_INF |
			FYOPEF_OUTPUT_COMMENTS;
	} else {
		flags = FYOPEF_DISABLE_DIRECTORY |
			FYOPEF_MODE_JSON |
			FYOPEF_STYLE_COMPACT |
			FYOPEF_WIDTH_INF;
	}
	if (auto_anchor)
		flags |= FYOPEF_AUTO_ANCHOR;

	if (label) {
		text = fy_sprintfa("\n# %s:\n", label);
		if (text)
			(void)fyai_sink_write(ctx->sink, FYAI_SINK_MACHINE,
					      text, strlen(text));
	}
	emitted = fy_emit(fyai_ctx_transient_gb(ctx), value, flags, NULL);
	if (fy_is_invalid(emitted))
		return;
	text = fy_castp(&emitted, "");
	(void)fyai_sink_write(ctx->sink, FYAI_SINK_MACHINE, text, strlen(text));
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

	if (fy_is_invalid(emitted))
		return NULL;

	body = fy_castp(&emitted, "");
	if (!*body)
		return NULL;

	return fy_gb_intern_string(gb, body);
}

/* Preserve parser diagnostics on the returned value. */
fy_generic parse_response(struct fy_generic_builder *gb, const char *response)
{
	return parse_json_string(gb, response);
}

/* Format the first collected parser diagnostic without its input address. */
static bool parse_diag_text(fy_generic v, char *buf, size_t size)
{
	fy_generic diag, rec, gmsg, gcontent;
	const char *msg, *content, *excerpt;
	long long line, column;
	size_t len;

	diag = fy_generic_get_diag(v);
	if (!fy_is_valid(diag) || fy_is_null(diag) || !fy_len(diag))
		return false;
	/* The first record is the cause; the rest follow from it. */
	rec = fy_is_sequence(diag) ? fy_get_at(diag, 0) : diag;
	gmsg = fy_get(rec, "message");
	msg = fy_is_string(gmsg) ? fy_castp(&gmsg, "") : "malformed JSON";
	line = fy_get(rec, "line", 0LL);
	column = fy_get(rec, "column", 0LL);
	gcontent = fy_get(rec, "content");
	content = fy_is_string(gcontent) ? fy_castp(&gcontent, "") : "";
	len = strlen(content);
	excerpt = len > 76 ? content + len - 76 : content;
	snprintf(buf, size, "%s at line %lld column %lld%s%s%s", msg, line,
		 column, *excerpt ? ": " : "", len > 76 ? "..." : "",
		 excerpt);
	return true;
}

void parse_diag_report(struct fyai_ctx *ctx, fy_generic v,
		       const char *fallback, const char *what)
{
	char why[512];
	bool have_why;

	have_why = parse_diag_text(v, why, sizeof(why));
	fyai_error(ctx, "%s: %s", what, have_why ? why : fallback);
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

/* Collect parser diagnostics on the failed value instead of standard error. */
fy_generic parse_json_string(struct fy_generic_builder *gb, const char *str)
{
	return fy_parse(gb, str,
		FYOPPF_DISABLE_DIRECTORY |
		FYOPPF_INPUT_TYPE_STRING |
		FYOPPF_MODE_JSON |
		FYOPPF_COLLECT_DIAG,
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
		FYOPPF_MODE_JSON |
		FYOPPF_COLLECT_DIAG,
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

	if (fy_is_invalid(emitted))
		return NULL;

	/* intern while `emitted` is still in scope: a short result lives
	 * inline in the generic word itself (see CLAUDE.md on fy_cast) */
	body = fy_castp(&emitted, "");
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

	if (fy_is_invalid(v) || fy_generic_is_in_place(v))
		return false;

	if (fy_is_collection(v))
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
 * External editor child operation:
 *
 *   NEW -> RUNNING -> COMPLETED
 *             |
 *             +-> CANCEL_REQUESTED -> COMPLETED
 *             |
 *             +-> ABANDONED -> REAPED_AND_FREED
 *
 * Cancellation signals the child but retains its child source. Destruction
 * before completion abandons caller notification; the child callback performs
 * the final reap and frees the request. No editor child becomes a zombie just
 * because its original UI or command owner went away.
 */
struct fyai_editor_request {
	struct fyai_ctx *ctx;
	struct fyai_event_source *child_src;
	fyai_editor_complete_fn complete;
	void *userdata;
	pid_t pid;
	int status;
	bool done;
	bool cancelled;
	bool notified;
	bool abandoned;
};

static void fyai_editor_notify(struct fyai_editor_request *request)
{
	if (request->notified)
		return;
	request->notified = true;
	if (request->complete)
		request->complete(request, request->userdata);
}

static enum fyai_event_action
fyai_editor_child_complete(const struct fyai_event *ev)
{
	struct fyai_editor_request *request;

	request = ev->userdata;
	request->child_src = NULL;
	request->pid = -1;
	request->status = ev->status;
	request->done = true;
	fyai_editor_notify(request);
	if (request->abandoned)
		free(request);
	return FYAIEA_CONTINUE;
}

struct fyai_editor_request *
fyai_editor_submit(struct fyai_ctx *ctx, const char *path, bool readonly,
		   fyai_editor_complete_fn complete, void *userdata)
{
	const char *editor;
	struct fyai_editor_request *request;
	struct fyai_event_loop *el;
	char *cmd = NULL;
	pid_t pid;
	int rc;

	if (!ctx || !path)
		return NULL;
	request = calloc(1, sizeof(*request));
	if (!request)
		return NULL;
	request->ctx = ctx;
	request->pid = -1;
	request->complete = complete;
	request->userdata = userdata;
	editor = getenv("VISUAL");
	if (!editor || !*editor)
		editor = getenv("EDITOR");
	if (!editor || !*editor)
		editor = "vi";
	rc = asprintf(&cmd, readonly ? "%s -R '%s'" : "%s '%s'",
		      editor, path);
	fyai_error_check(ctx, rc >= 0, err_out,
			 "could not format editor command");
	pid = fork();
	fyai_error_check(ctx, pid >= 0, err_out, "could not start editor: %s",
			 strerror(errno));
	if (!pid) {
		if (ctx && ctx->signal_mask_valid)
			(void)sigprocmask(SIG_SETMASK, &ctx->signal_mask, NULL);
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
	request->pid = pid;
	el = fyai_ctx_loop(ctx);
	fyai_error_check(ctx, el, err_child,
			 "could not acquire event loop for editor");
	rc = fyai_event_add_child(el, pid, fyai_editor_child_complete,
				  request, &request->child_src);
	fyai_error_check(ctx, !rc, err_child,
			 "could not watch editor process");
	free(cmd);
	return request;

err_child:
	(void)kill(pid, SIGKILL);
	while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
		;
err_out:
	free(cmd);
	free(request);
	return NULL;
}

void fyai_editor_cancel(struct fyai_editor_request *request)
{
	if (!request || request->done || request->cancelled)
		return;
	request->cancelled = true;
	if (request->pid > 0)
		(void)kill(request->pid, SIGTERM);
}

bool fyai_editor_done(const struct fyai_editor_request *request)
{
	return request && request->done;
}

int fyai_editor_collect(const struct fyai_editor_request *request)
{
	if (!request || !request->done || request->cancelled)
		return -1;
	return WIFEXITED(request->status) && !WEXITSTATUS(request->status) ?
		0 : -1;
}

void fyai_editor_destroy(struct fyai_editor_request *request)
{
	if (!request)
		return;
	if (!request->done) {
		fyai_editor_cancel(request);
		request->complete = NULL;
		request->userdata = NULL;
		request->abandoned = true;
		return;
	}
	free(request);
}

static void fyai_editor_sync_complete(struct fyai_editor_request *request,
				      void *userdata)
{
	volatile bool *done;

	(void)request;
	done = userdata;
	*done = true;
}

static int fyai_spawn_editor_mode(struct fyai_ctx *ctx, const char *path,
				  bool readonly)
{
	struct fyai_editor_request *request;
	struct fyai_event_loop *el;
	volatile bool done;
	int rc;

	done = false;
	request = fyai_editor_submit(ctx, path, readonly,
				     fyai_editor_sync_complete, (void *)&done);
	if (!request)
		return -1;
	el = fyai_ctx_loop(ctx);
	while (!done) {
		rc = fyai_event_loop_step(el, -1);
		if (rc < 0) {
			fyai_editor_cancel(request);
			break;
		}
	}
	rc = fyai_editor_done(request) ? fyai_editor_collect(request) : -1;
	if (rc)
		fyai_error(ctx, "editor exited unsuccessfully");
	fyai_editor_destroy(request);
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
