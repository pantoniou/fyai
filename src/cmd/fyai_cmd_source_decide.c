/*
 * fyai_cmd_source_decide.c - fyai source decide: a decider in front of a source
 *
 * `fyai source decide --decider NAME -- SOURCE...` is an event source that
 * runs another one, SOURCE, and passes everything between it and the client
 * as it is, but for its events. Each event goes, in order, to one long-lived
 * `fyai decide NAME` child as {kind, id, data}. A `drop` is acknowledged to
 * SOURCE with its credit and `dropped: true`, and the client never sees it;
 * any other decision is passed on with `decision: {decider, decision,
 * arguments}` beside the data. An event whose decision fails, or that comes
 * when the decider is gone, is passed on as it came.
 *
 * The verb has a loop of its own, on poll(): it serves one client.
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
#include "utils.h"

/* A line of a peer longer than this ends the proxy. */
#define PROXY_LINE_MAX	(16 * 1024 * 1024)

struct proxy_child {
	pid_t pid;
	int to_fd, from_fd;
	struct fyai_buf in;		/* what it wrote, to its line end */
};

struct proxy_event {
	struct proxy_event *next;
	char *line;			/* the frame as the source sent it */
};

struct source_proxy {
	struct fyai_ctx *ctx;
	const char *decider;
	struct fy_generic_builder *gb;
	struct proxy_child source, decide;
	struct fyai_buf client;		/* what the client sent */
	struct proxy_event *queue, **tail;
	bool deciding;			/* the head of the queue is with the decider */
	bool decider_gone;
	unsigned long dropped, passed;
};

static int proxy_write(int fd, const char *data, size_t len)
{
	ssize_t n;

	while (len) {
		n = write(fd, data, len);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		data += n;
		len -= (size_t)n;
	}
	return 0;
}

static int proxy_write_line(int fd, const char *line)
{
	return proxy_write(fd, line, strlen(line)) || proxy_write(fd, "\n", 1);
}

static int proxy_write_frame(struct source_proxy *p, int fd, fy_generic frame)
{
	const char *line = emit_json_string(p->gb,
		fy_assoc(p->gb, frame, "jsonrpc", "2.0"));

	return line ? proxy_write_line(fd, line) : -1;
}

/* A note for the log of the server at the client: MCP notifications/message. */
static void proxy_log(struct source_proxy *p, const char *level,
		      const char *text)
{
	(void)proxy_write_frame(p, STDOUT_FILENO, fy_mapping(p->gb,
		"method", "notifications/message", "params", fy_mapping(p->gb,
			"level", level, "logger", "fyai-source-decide",
			"data", fy_value(p->gb, text))));
}

static int proxy_spawn(struct proxy_child *c, char **argv, bool self)
{
	int in[2] = { -1, -1 }, out[2] = { -1, -1 };
	sigset_t none;

	if (pipe2(in, O_CLOEXEC) || pipe2(out, O_CLOEXEC))
		goto err;
	c->pid = fork();
	if (c->pid < 0)
		goto err;
	if (!c->pid) {
		sigemptyset(&none);
		(void)sigprocmask(SIG_SETMASK, &none, NULL);
		(void)signal(SIGPIPE, SIG_DFL);
		if (dup2(in[0], STDIN_FILENO) < 0 ||
		    dup2(out[1], STDOUT_FILENO) < 0)
			_exit(127);
		if (self)
			(void)fyai_exec_self((const char *const *)argv);
		else
			execvp(argv[0], argv);
		fprintf(stderr, "source decide: cannot run %s: %s\n", argv[0],
			strerror(errno));
		_exit(127);
	}
	close(in[0]);
	close(out[1]);
	c->to_fd = in[1];
	c->from_fd = out[0];
	return 0;
err:
	if (in[0] >= 0) {
		close(in[0]);
		close(in[1]);
	}
	if (out[0] >= 0) {
		close(out[0]);
		close(out[1]);
	}
	return -1;
}

static void proxy_child_end(struct proxy_child *c)
{
	int status;

	if (c->to_fd >= 0)
		close(c->to_fd);
	if (c->from_fd >= 0)
		close(c->from_fd);
	c->to_fd = c->from_fd = -1;
	if (c->pid > 0) {
		(void)kill(c->pid, SIGTERM);
		while (waitpid(c->pid, &status, 0) < 0 && errno == EINTR)
			;
		c->pid = -1;
	}
	free(c->in.data);
	c->in.data = NULL;
	c->in.len = 0;
}

/* Give the next event to the decider, or pass on those it cannot decide. */
static void proxy_pump(struct source_proxy *p)
{
	struct proxy_event *ev;
	fy_generic frame, params, input;
	const char *line;

	while (!p->deciding && (ev = p->queue)) {
		fy_generic_builder_reset(p->gb);
		frame = parse_json_string(p->gb, ev->line);
		params = fy_get(frame, "params", fy_invalid);
		input = fy_mapping(p->gb, "kind", fy_get(params, "kind", fy_null),
				   "id", fy_get(params, "id", fy_null),
				   "data", fy_get(params, "data", fy_null));
		line = p->decider_gone ? NULL : emit_json_string(p->gb, input);
		if (line && !proxy_write_line(p->decide.to_fd, line)) {
			p->deciding = true;
			return;
		}
		/* No decider: the event goes on as it came. */
		p->queue = ev->next;
		if (!p->queue)
			p->tail = &p->queue;
		(void)proxy_write_line(STDOUT_FILENO, ev->line);
		p->passed++;
		free(ev->line);
		free(ev);
	}
}

/* The decision of the head of the queue. */
static void proxy_decided(struct source_proxy *p, const char *line)
{
	struct proxy_event *ev = p->queue;
	fy_generic answer, choice, frame, params, error;

	if (!ev || !p->deciding)
		return;
	p->queue = ev->next;
	if (!p->queue)
		p->tail = &p->queue;
	p->deciding = false;
	fy_generic_builder_reset(p->gb);
	answer = parse_json_string(p->gb, line);
	choice = fy_get(answer, "decision", fy_invalid);
	frame = parse_json_string(p->gb, ev->line);
	params = fy_get(frame, "params", fy_invalid);
	if (fy_equal(choice, "drop")) {
		/* Handled: its credit goes back, and no cursor passes it. */
		(void)proxy_write_frame(p, p->source.to_fd, fy_mapping(p->gb,
			"method", "notifications/fyai/ack",
			"params", fy_mapping(p->gb,
				"id", fy_get(params, "id", fy_null),
				"credits", 1LL, "dropped", fy_true)));
		p->dropped++;
	} else if (fy_is_string(choice) && fy_is_mapping(params)) {
		params = fy_assoc(p->gb, params, "decision", fy_mapping(p->gb,
			"decider", p->decider, "decision", choice,
			"arguments", fy_get(answer, "arguments", fy_map_empty)));
		(void)proxy_write_frame(p, STDOUT_FILENO,
			fy_assoc(p->gb, frame, "params", params));
		p->passed++;
	} else {
		/* A short string lives in its generic: keep it in scope. */
		error = fy_get(answer, "error", fy_invalid);
		proxy_log(p, "warning", fy_sprintfa("the decider %s gave no "
			  "decision for an event: %s", p->decider,
			  fy_is_string(error) ? fy_castp(&error, "") :
			  "no answer"));
		(void)proxy_write_line(STDOUT_FILENO, ev->line);
		p->passed++;
	}
	free(ev->line);
	free(ev);
}

/* A line of the source: an event waits for its decision; the rest goes on. */
static void proxy_source_line(struct source_proxy *p, const char *line)
{
	struct proxy_event *ev;
	fy_generic frame;

	fy_generic_builder_reset(p->gb);
	frame = parse_json_string(p->gb, line);
	if (!fy_equal(fy_get(frame, "method", fy_invalid),
		      "notifications/fyai/event")) {
		(void)proxy_write_line(STDOUT_FILENO, line);
		return;
	}
	ev = calloc(1, sizeof(*ev));
	if (ev)
		ev->line = strdup(line);
	if (!ev || !ev->line) {
		free(ev);
		(void)proxy_write_line(STDOUT_FILENO, line);
		return;
	}
	*p->tail = ev;
	p->tail = &ev->next;
}

/*
 * Read @fd into @buf and hand each complete line to @fn. Return -1 at the
 * end of the input or on an error.
 */
static int proxy_read_lines(struct source_proxy *p, int fd, struct fyai_buf *buf,
			    void (*fn)(struct source_proxy *, const char *))
{
	char chunk[65536], *nl;
	size_t used = 0;
	ssize_t n;

	n = read(fd, chunk, sizeof(chunk));
	if (n < 0)
		return errno == EINTR || errno == EAGAIN ? 0 : -1;
	if (!n || fyai_buf_append_data(buf, chunk, (size_t)n))
		return -1;
	while ((nl = memchr(buf->data + used, '\n', buf->len - used))) {
		*nl = '\0';
		if (nl > buf->data + used)
			fn(p, buf->data + used);
		used = (size_t)(nl - buf->data) + 1;
	}
	memmove(buf->data, buf->data + used, buf->len - used);
	buf->len -= used;
	return buf->len > PROXY_LINE_MAX ? -1 : 0;
}

static void proxy_client_line(struct source_proxy *p, const char *line)
{
	(void)proxy_write_line(p->source.to_fd, line);
}

static int proxy_run(struct source_proxy *p)
{
	struct pollfd pfd[3];
	nfds_t n;

	for (;;) {
		proxy_pump(p);
		pfd[0] = (struct pollfd){ .fd = STDIN_FILENO, .events = POLLIN };
		pfd[1] = (struct pollfd){ .fd = p->source.from_fd,
					  .events = POLLIN };
		n = 2;
		if (!p->decider_gone) {
			pfd[2] = (struct pollfd){ .fd = p->decide.from_fd,
						  .events = POLLIN };
			n = 3;
		}
		if (poll(pfd, n, -1) < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (pfd[0].revents &&
		    proxy_read_lines(p, STDIN_FILENO, &p->client,
				     proxy_client_line))
			return 0;		/* the client is gone */
		if (pfd[1].revents &&
		    proxy_read_lines(p, p->source.from_fd, &p->source.in,
				     proxy_source_line))
			return 0;		/* the source is gone */
		if (n > 2 && pfd[2].revents &&
		    proxy_read_lines(p, p->decide.from_fd, &p->decide.in,
				     proxy_decided)) {
			p->decider_gone = true;
			proxy_log(p, "warning", fy_sprintfa("the decider %s "
				  "ended; events pass undecided", p->decider));
			/* The event it held goes on undecided. */
			p->deciding = false;
		}
	}
}

int fyai_cmd_source_decide(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = fyai_cmd_call_ctx(call);
	struct fy_generic_builder_cfg cfg = {
		.flags = FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED,
	};
	struct source_proxy p = {
		.ctx = ctx,
		.source = { .pid = -1, .to_fd = -1, .from_fd = -1 },
		.decide = { .pid = -1, .to_fd = -1, .from_fd = -1 },
	};
	fy_generic command = fy_get(call->args, "command", fy_seq_empty);
	const char *model = fyai_cmd_arg_str(call, "model"), *arg;
	char *dargv[8], **argv = NULL;
	size_t argc = fy_len(command), i = 0;
	int dargc = 0, rc = -1;
	struct proxy_event *ev;

	(void)result;
	p.tail = &p.queue;
	p.decider = fyai_cmd_arg_str(call, "decider");
	fyai_error_check(ctx, p.decider && *p.decider, out,
			 "source decide: name a decider with --decider");
	fyai_error_check(ctx, argc, out,
			 "source decide: name the source command after --");
	fyai_error_check(ctx, fyai_exec_self_available(), out,
			 "source decide: fyai cannot run itself here");
	p.gb = fy_generic_builder_create(&cfg);
	argv = calloc(argc + 1, sizeof(*argv));
	fyai_error_check(ctx, p.gb && argv, out, "source decide: out of memory");
	fy_foreach(arg, command)
		argv[i++] = (char *)arg;

	dargv[dargc++] = "fyai";
	dargv[dargc++] = "--color";
	dargv[dargc++] = "off";
	if (model && *model) {
		dargv[dargc++] = "-m";
		dargv[dargc++] = (char *)model;
	}
	dargv[dargc++] = "decide";
	dargv[dargc++] = (char *)p.decider;
	dargv[dargc] = NULL;

	(void)signal(SIGPIPE, SIG_IGN);
	fyai_error_check(ctx, !proxy_spawn(&p.source, argv, false), out,
			 "source decide: cannot start %s", argv[0]);
	if (proxy_spawn(&p.decide, dargv, true)) {
		p.decider_gone = true;
		fyai_warning(ctx, "source decide: cannot start the decider %s; "
			     "events pass undecided", p.decider);
	}
	rc = proxy_run(&p);
	/* Events still queued are not acknowledged: the source keeps them. */
	while ((ev = p.queue)) {
		p.queue = ev->next;
		free(ev->line);
		free(ev);
	}
out:
	proxy_child_end(&p.decide);
	proxy_child_end(&p.source);
	free(p.client.data);
	free(argv);
	if (p.gb)
		fy_generic_builder_destroy(p.gb);
	return rc;
}
