/*
 * fyai_cmd_source.c - the source verbs: adapters that are event sources
 *
 * `fyai source lines -- COMMAND...` is an MCP server on standard input and
 * output with the fyai/events extension (doc/event-sources-sdd.md). It runs
 * COMMAND and serves each line that the command prints as an event of one
 * kind. With --json a line is parsed as JSON and checked against --schema.
 * With --cursor-file the offset of the output that fyai acknowledged is kept
 * in a file, and a run skips that much of the output of the command, so a
 * command that prints a file from its start resumes where the last run was
 * acknowledged.
 *
 * The verb has a loop of its own, on poll(): it owns two descriptors and a
 * child, and it serves one client.
 *
 * SPDX-License-Identifier: MIT
 */
#define FYAI_MODULE FYAIEM_UNKNOWN
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fyai.h"
#include "fyai_buf.h"
#include "fyai_cmd.h"
#include "fyai_cmd_int.h"
#include "fyai_schema.h"
#include "utils.h"

/* A line longer than this is cut into lines of this size. */
#define SOURCE_LINE_MAX		(1024 * 1024)
/* A frame of the client longer than this ends the session. */
#define SOURCE_FRAME_MAX	(16 * 1024 * 1024)
#define SOURCE_KIND_MAX		64

struct source_lines {
	struct fyai_ctx *ctx;
	const char *kind;
	bool json;
	fy_generic schema;		/* of the kind; in @keep */
	const char *cursor_file;
	char **argv;
	struct fy_generic_builder *keep; /* what lives for the run */
	struct fy_generic_builder *gb;	/* one frame; reset for each */
	pid_t pid;
	int out_fd;			/* the output of the command, or -1 */
	struct fyai_buf in;		/* what the client sent, to its line end */
	struct fyai_buf out;		/* the output of the command, unread */
	long long consumed;		/* offset of the start of @out */
	long long skip;			/* the output before this is served */
	long long saved;		/* the cursor in the cursor file */
	unsigned long long credits;
	bool offered;			/* the client speaks fyai/events */
	bool listed;			/* the client has the kinds */
	bool ended;			/* the command ended and was reported */
	bool failed;			/* the client is gone */
};

/* The context that this object stands on. */
static inline struct fyai_ctx *source_lines_ctx(const struct source_lines *o)
{
	return o->ctx;
}

static void source_write(struct source_lines *s, fy_generic frame)
{
	const char *line;
	size_t len, off = 0;
	ssize_t n;

	if (s->failed)
		return;
	line = emit_json_string(s->gb, fy_assoc(s->gb, frame, "jsonrpc", "2.0"));
	if (!line) {
		fyai_warning(source_lines_ctx(s), "source: could not encode a frame");
		return;
	}
	len = strlen(line);
	while (off <= len) {
		n = off < len ? write(STDOUT_FILENO, line + off, len - off) :
				write(STDOUT_FILENO, "\n", 1);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			s->failed = true;
			return;
		}
		off += (size_t)n;
		if (off == len + 1)
			break;
	}
}

static void source_notify(struct source_lines *s, const char *method,
			  fy_generic params)
{
	source_write(s, fy_mapping(s->gb, "method", method, "params", params));
}

/* A note for the client's log of the server: MCP notifications/message. */
static void source_log(struct source_lines *s, const char *level,
		       const char *text)
{
	source_notify(s, "notifications/message", fy_mapping(s->gb,
		"level", level, "logger", "fyai-source",
		"data", fy_value(s->gb, text)));
}

/* The cursor in the cursor file, or -1. */
static long long source_cursor_load(const char *path)
{
	char buf[64];
	long long v;
	ssize_t n;
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = '\0';
	if (sscanf(buf, "%lld", &v) != 1 || v < 0)
		return -1;
	return v;
}

/* Keep @cursor in the cursor file: a new file in its place, then a rename. */
static void source_cursor_save(struct source_lines *s, long long cursor)
{
	char *tmp = NULL;
	FILE *f;

	if (!s->cursor_file || cursor <= s->saved)
		return;
	if (asprintf(&tmp, "%s.tmp", s->cursor_file) < 0)
		return;
	f = fopen(tmp, "we");
	if (!f || fprintf(f, "%lld\n", cursor) < 0 || fflush(f) ||
	    fsync(fileno(f))) {
		if (f)
			fclose(f);
		fyai_warning(source_lines_ctx(s), "source: could not write %s", tmp);
		free(tmp);
		return;
	}
	if (fclose(f) || rename(tmp, s->cursor_file))
		fyai_warning(source_lines_ctx(s), "source: could not keep the cursor in %s",
			     s->cursor_file);
	else
		s->saved = cursor;
	free(tmp);
}

static int source_spawn(struct source_lines *s)
{
	sigset_t none;
	int pipefd[2], null;

	if (pipe2(pipefd, O_CLOEXEC))
		return -1;
	s->pid = fork();
	if (s->pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return -1;
	}
	if (!s->pid) {
		/* The command leads a group, so the end of the verb ends it. */
		(void)setpgid(0, 0);
		sigemptyset(&none);
		(void)sigprocmask(SIG_SETMASK, &none, NULL);
		(void)signal(SIGPIPE, SIG_DFL);
		null = open("/dev/null", O_RDONLY);
		if (null < 0 || dup2(null, STDIN_FILENO) < 0 ||
		    dup2(pipefd[1], STDOUT_FILENO) < 0)
			_exit(127);
		execvp(s->argv[0], s->argv);
		fprintf(stderr, "source: cannot run %s: %s\n", s->argv[0],
			strerror(errno));
		_exit(127);
	}
	close(pipefd[1]);
	s->out_fd = pipefd[0];
	return 0;
}

/* The command ended: say how, once. */
static void source_reap(struct source_lines *s)
{
	int status = 0;
	char text[64];

	if (s->pid <= 0 || s->ended)
		return;
	while (waitpid(s->pid, &status, 0) < 0 && errno == EINTR)
		;
	s->pid = -1;
	s->ended = true;
	if (WIFEXITED(status))
		snprintf(text, sizeof(text), "the command ended: status %d",
			 WEXITSTATUS(status));
	else
		snprintf(text, sizeof(text), "the command ended: signal %d",
			 WIFSIGNALED(status) ? WTERMSIG(status) : 0);
	source_log(s, WIFEXITED(status) && !WEXITSTATUS(status) ?
		   "info" : "warning", text);
}

/*
 * One line of the command, at offset @start, @len bytes long and followed by
 * @end more bytes (its line end). Send it as an event, or drop it when it
 * fails the kind.
 */
static void source_line(struct source_lines *s, const char *line, size_t len,
			long long start, size_t end)
{
	fy_generic data, report;
	char *problems;
	long long next = start + (long long)len + (long long)end;

	fy_generic_builder_reset(s->gb);
	if (s->json) {
		data = parse_json_string_size(s->gb, line, len);
		if (fy_is_invalid(data)) {
			fyai_warning(source_lines_ctx(s), "source: line at %lld is not JSON; "
				     "it is skipped", start);
			return;
		}
	} else {
		data = fy_stringf(s->gb, "%.*s", (int)len, line);
	}
	report = fyai_schema_validate(s->gb, s->schema, data);
	if (!fyai_schema_valid(report)) {
		problems = fyai_schema_report_string(report);
		fyai_warning(source_lines_ctx(s), "source: line at %lld fails the schema of "
			     "'%s': %s; it is skipped", start, s->kind,
			     problems ? problems : "?");
		free(problems);
		return;
	}
	source_notify(s, "notifications/fyai/event", fy_mapping(s->gb,
		"kind", s->kind,
		"id", fy_stringf(s->gb, "line-%lld", start),
		"cursor", fy_stringf(s->gb, "%lld", next),
		"data", data));
	s->credits--;
}

/*
 * Serve the complete lines of the output while there is credit. A line that
 * the last run acknowledged is skipped. At the end of the output the rest is
 * a line of its own.
 */
static void source_drain(struct source_lines *s, bool eof)
{
	char *nl;
	size_t len, end, used = 0;

	while (s->credits && used < s->out.len) {
		nl = memchr(s->out.data + used, '\n', s->out.len - used);
		if (nl) {
			len = (size_t)(nl - (s->out.data + used));
			end = 1;
		} else if (s->out.len - used >= SOURCE_LINE_MAX) {
			len = SOURCE_LINE_MAX;
			end = 0;
		} else if (eof) {
			len = s->out.len - used;
			end = 0;
		} else {
			break;
		}
		if (s->consumed + (long long)(len + end) > s->skip) {
			if (s->consumed >= s->skip)
				source_line(s, s->out.data + used, len,
					    s->consumed, end);
		}
		s->consumed += (long long)(len + end);
		used += len + end;
	}
	if (used) {
		memmove(s->out.data, s->out.data + used, s->out.len - used);
		s->out.len -= used;
	}
}

/* Whether the buffered output holds a line that waits for credit. */
static bool source_line_waits(const struct source_lines *s)
{
	return s->out.len && (memchr(s->out.data, '\n', s->out.len) ||
			      s->out.len >= SOURCE_LINE_MAX);
}

static fy_generic source_initialize(struct source_lines *s, fy_generic params)
{
	fy_generic events, cursor;
	long long from_client = -1, credits;

	events = fy_get(fy_get(fy_get(params, "capabilities", fy_map_empty),
			       "experimental", fy_map_empty),
			"fyai/events", fy_invalid);
	s->offered = fy_is_mapping(events) &&
		     fy_get(events, "version", 0LL) == 1;
	if (s->offered) {
		credits = fy_get(events, "credits", 0LL);
		s->credits = credits > 0 ? (unsigned long long)credits : 0;
		cursor = fy_get(events, "cursor", fy_invalid);
		if (fy_is_string(cursor))
			(void)sscanf(fy_castp(&cursor, ""), "%lld",
				     &from_client);
	}
	/* The file outlives the client; the client is the fallback. */
	s->skip = s->saved >= 0 ? s->saved : from_client > 0 ? from_client : 0;
	return fy_mapping(s->gb,
		"protocolVersion", fy_get(params, "protocolVersion",
					  "2024-11-05"),
		"capabilities", fy_mapping(s->gb,
			"tools", fy_map_empty,
			"experimental", fy_mapping(s->gb, "fyai/events",
				fy_mapping(s->gb, "version", 1LL,
					   "listChanged", fy_false))),
		"serverInfo", fy_mapping(s->gb, "name", "fyai-source-lines",
					 "version", VERSION));
}

/* The kinds: the one kind of the lines. Listing it starts the command. */
static fy_generic source_kinds(struct source_lines *s)
{
	if (!s->listed && s->offered) {
		s->listed = true;
		if (source_spawn(s))
			fyai_warning(source_lines_ctx(s), "source: cannot start %s: %s",
				     s->argv[0], strerror(errno));
	}
	return fy_mapping(s->gb, "kinds", fy_sequence(s->gb, fy_mapping(s->gb,
		"name", s->kind,
		"description", fy_stringf(s->gb, "A line of %s", s->argv[0]),
		"schema", s->schema)));
}

static void source_ack(struct source_lines *s, fy_generic params)
{
	fy_generic cursor = fy_get(params, "cursor", fy_invalid);
	long long credits = fy_get(params, "credits", 0LL), at;

	if (credits > 0)
		s->credits += (unsigned long long)credits;
	if (fy_is_string(cursor) &&
	    sscanf(fy_castp(&cursor, ""), "%lld", &at) == 1)
		source_cursor_save(s, at);
}

/* One frame of the client. */
static void source_frame(struct source_lines *s, const char *line, size_t len)
{
	fy_generic doc, id, params, result = fy_invalid, error = fy_invalid;
	const char *method;

	fy_generic_builder_reset(s->gb);
	doc = parse_json_string_size(s->gb, line, len);
	if (!fy_is_mapping(doc))
		return;
	method = fy_get(doc, "method", "");
	id = fy_get(doc, "id", fy_invalid);
	params = fy_get(doc, "params", fy_map_empty);
	if (!*method)
		return;		/* a reply: the verb asks nothing */
	if (!strcmp(method, "notifications/fyai/ack")) {
		source_ack(s, params);
		return;
	}
	if (fy_is_invalid(id))
		return;		/* another notification */
	if (!strcmp(method, "initialize"))
		result = source_initialize(s, params);
	else if (!strcmp(method, "ping"))
		result = fy_map_empty;
	else if (!strcmp(method, "tools/list"))
		result = fy_mapping(s->gb, "tools", fy_seq_empty);
	else if (!strcmp(method, "fyai/events/list"))
		result = source_kinds(s);
	else
		error = fy_mapping(s->gb, "code", -32601LL,
				   "message", "method not found");
	source_write(s, fy_is_valid(error) ?
		fy_mapping(s->gb, "id", id, "error", error) :
		fy_mapping(s->gb, "id", id, "result", result));
}

/* Read what the client sent and serve each complete frame. */
static int source_read_client(struct source_lines *s)
{
	char buf[65536], *nl;
	size_t used = 0, len;
	ssize_t n;

	n = read(STDIN_FILENO, buf, sizeof(buf));
	if (n < 0)
		return errno == EINTR || errno == EAGAIN ? 0 : -1;
	if (!n)
		return -1;
	if (fyai_buf_append_data(&s->in, buf, (size_t)n))
		return -1;
	while ((nl = memchr(s->in.data + used, '\n', s->in.len - used))) {
		len = (size_t)(nl - (s->in.data + used));
		source_frame(s, s->in.data + used, len);
		used += len + 1;
	}
	memmove(s->in.data, s->in.data + used, s->in.len - used);
	s->in.len -= used;
	return s->in.len > SOURCE_FRAME_MAX ? -1 : 0;
}

/* Read the output of the command; return true at its end. */
static bool source_read_command(struct source_lines *s)
{
	char buf[65536];
	ssize_t n;

	n = read(s->out_fd, buf, sizeof(buf));
	if (n < 0 && (errno == EINTR || errno == EAGAIN))
		return false;
	if (n > 0 && !fyai_buf_append_data(&s->out, buf, (size_t)n))
		return false;
	close(s->out_fd);
	s->out_fd = -1;
	return true;
}

static int source_lines_run(struct source_lines *s)
{
	struct pollfd pfd[2];
	nfds_t nfds;
	bool eof = false;

	while (!s->failed) {
		/* Serve what credit allows before reading more. */
		source_drain(s, eof);
		if (eof && !s->out.len)
			source_reap(s);
		pfd[0].fd = STDIN_FILENO;
		pfd[0].events = POLLIN;
		nfds = 1;
		/* No credit: the pipe fills and the command waits. */
		if (s->out_fd >= 0 && s->credits && !source_line_waits(s)) {
			pfd[1].fd = s->out_fd;
			pfd[1].events = POLLIN;
			nfds = 2;
		}
		if (poll(pfd, nfds, -1) < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (pfd[0].revents && source_read_client(s))
			break;		/* the client is gone: so is the source */
		if (nfds > 1 && pfd[1].revents && source_read_command(s))
			eof = true;
	}
	return 0;
}

int fyai_cmd_source_lines(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = fyai_cmd_call_ctx(call);
	struct fy_generic_builder_cfg cfg = {
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED,
	};
	struct source_lines s = {
		.ctx = ctx, .pid = -1, .out_fd = -1, .saved = -1,
	};
	fy_generic command = fy_get(call->args, "command", fy_seq_empty);
	fy_generic schema;
	const char *schema_file, *schema_json, *arg;
	char *text = NULL;
	size_t argc = fy_len(command), i = 0;
	int status, rc = -1;
	FILE *f;

	(void)result;
	s.kind = fyai_cmd_arg_str(call, "kind");
	if (!s.kind || !*s.kind)
		s.kind = "line";
	s.json = fy_get(call->args, "json", false);
	s.cursor_file = fyai_cmd_arg_str(call, "cursor_file");
	schema_file = fyai_cmd_arg_str(call, "schema");
	schema_json = fyai_cmd_arg_str(call, "schema_json");
	fyai_error_check(ctx, !schema_file || !schema_json, out,
			 "source lines: give --schema or --schema-json, not both");
	fyai_error_check(ctx, argc, out, "source lines: name a command after --");
	fyai_error_check(ctx, strlen(s.kind) <= SOURCE_KIND_MAX &&
			 strspn(s.kind, "abcdefghijklmnopqrstuvwxyz"
				"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") ==
			 strlen(s.kind), out,
			 "source lines: a kind is letters, digits, '-' or '_'");
	fyai_error_check(ctx, (!schema_file && !schema_json) || s.json, out,
			 "source lines: --schema checks JSON lines; add --json");
	s.keep = fy_generic_builder_create(&cfg);
	s.gb = fy_generic_builder_create(&cfg);
	s.argv = calloc(argc + 1, sizeof(*s.argv));
	fyai_error_check(ctx, s.keep && s.gb && s.argv, out,
			 "source lines: out of memory");
	fy_foreach(arg, command)
		s.argv[i++] = (char *)arg;

	if (schema_file) {
		f = fopen(schema_file, "re");
		fyai_error_check(ctx, f, out, "source lines: cannot open %s: %s",
				 schema_file, strerror(errno));
		{
			size_t len = 0;
			FILE *mf = open_memstream(&text, &len);
			char buf[4096];
			size_t n;

			while (mf && (n = fread(buf, 1, sizeof(buf), f)))
				fwrite(buf, 1, n, mf);
			if (mf)
				fclose(mf);
		}
		fclose(f);
		schema = text ? parse_json_string(s.keep, text) : fy_invalid;
		fyai_error_check(ctx, fy_is_mapping(schema) || fy_is_bool(schema),
				 out, "source lines: %s is not a JSON schema",
				 schema_file);
		/* Check that the validator supports the schema before reading input. */
		fyai_error_check(ctx, fy_is_valid(fyai_schema_validate(s.gb,
				 schema, fy_null)), out,
				 "source lines: %s cannot be checked", schema_file);
		s.schema = schema;
	} else if (schema_json) {
		schema = parse_json_string(s.keep, schema_json);
		fyai_error_check(ctx, fy_is_mapping(schema) || fy_is_bool(schema),
				 out, "source lines: --schema-json is not a JSON "
				 "schema");
		fyai_error_check(ctx, fy_is_valid(fyai_schema_validate(s.gb,
				 schema, fy_null)), out,
				 "source lines: --schema-json cannot be checked");
		s.schema = schema;
	} else {
		s.schema = s.json ? fy_true :
			   fy_mapping(s.keep, "type", "string");
	}
	if (s.cursor_file)
		s.saved = source_cursor_load(s.cursor_file);

	(void)signal(SIGPIPE, SIG_IGN);
	rc = source_lines_run(&s);
	if (s.pid > 0) {
		(void)kill(-s.pid, SIGTERM);
		while (waitpid(s.pid, &status, 0) < 0 && errno == EINTR)
			;
	}
	if (s.out_fd >= 0)
		close(s.out_fd);
out:
	free(text);
	free(s.argv);
	free(s.in.data);
	free(s.out.data);
	if (s.gb)
		fy_generic_builder_destroy(s.gb);
	if (s.keep)
		fy_generic_builder_destroy(s.keep);
	return rc;
}
