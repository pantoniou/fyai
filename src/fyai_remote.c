/* SPDX-License-Identifier: MIT */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <openssl/crypto.h>

#include "fyai.h"
#include "fyai_branch.h"
#include "fyai_cmd.h"
#include "fyai_cmd_int.h"
#include "fyai_config.h"
#include "fyai_event.h"
#include "fyai_host.h"
#include "fyai_jsonrpc.h"
#include "fyai_remote.h"
#include "fyai_remote_int.h"
#include "fyai_sink.h"
#include "fyai_storage.h"
#include "fyai_terminal.h"
#include "fyai_transport_boot.h"
#include "utils.h"

#define FYAI_MODULE FYAIEM_REMOTE


/* Mark @fd close-on-exec; macOS has no flag that makes it so at creation. */
int fyai_remote_cloexec(int fd)
{
	return fd >= 0 && fcntl(fd, F_SETFD, FD_CLOEXEC) ? -1 : 0;
}

/* A socket of @type with both ends close-on-exec, or -1. */
int fyai_remote_socketpair(int type, int sv[2])
{
	if (socketpair(AF_UNIX, type, 0, sv))
		return -1;
	return fyai_remote_cloexec(sv[0]) || fyai_remote_cloexec(sv[1]) ? -1 : 0;
}

int fyai_remote_send(struct jsonrpc_conn *conn, fy_generic frame)
{
	if (jsonrpc_conn_output_size(conn) >= REMOTE_OUTPUT_MAX)
		return -1;
	return jsonrpc_conn_send_frame(conn, frame);
}

/*
 * Make each directory of @path after its first @top bytes, mode 0700. The
 * directory that the first @top bytes name, such as the home, is not changed.
 */
int fyai_remote_mkdirs(struct fyai_cfg *cfg, size_t top, const char *path)
{
	char *copy, *slash;
	int rc = 0;

	copy = strdup(path);
	fyai_cfg_error_check(cfg, copy, err, "cannot copy the path '%s'", path);
	for (slash = strchr(copy + top + 1, '/'); slash;
	     slash = strchr(slash + 1, '/')) {
		*slash = '\0';
		rc = mkdir_private(copy);
		if (rc)
			break;
		*slash = '/';
	}
	if (!rc)
		rc = mkdir_private(copy);
	if (rc)
		fyai_cfg_error(cfg, "create '%s': %s", copy, strerror(errno));
	free(copy);
	return rc ? -1 : 0;
err:
	return -1;
}

/*
 * The directory of the endpoints: .fyai/unix of the project, or of the home
 * for a server of projects, @user. Only a serving invocation, @create, makes
 * it. The caller frees the path.
 */
static char *remote_endpoint_dir(struct fyai_ctx *ctx, bool user, bool create)
{
	const char *home;
	char *root = NULL, *path = NULL;
	int rc;

	if (user) {
		home = getenv("HOME");
		fyai_error_check(ctx, home && *home, out, "HOME is not set: no endpoint of the "
				 "server of projects");
		root = strdup(home);
	} else {
		root = fyai_discover_project_root();
		if (!root)
			root = getcwd(NULL, 0);
	}
	fyai_error_check(ctx, root, out, "cannot find the directory of the endpoint");
	rc = asprintf(&path, "%s/.fyai/unix", root);
	fyai_error_check(ctx, rc >= 0, out, "cannot allocate the endpoint directory");
	if (create && fyai_remote_mkdirs(ctx->cfg, strlen(root), path)) {
		free(path);
		path = NULL;
	}
out:
	free(root);
	return path;
}

/*
 * The path of the endpoint: @override, else remote.sock in .fyai/unix of the
 * project, or serve.sock in ~/.fyai/unix for a server of projects, @user.
 * Only a serving invocation, @create, makes the directories. The caller frees
 * the path.
 */
char *fyai_remote_socket_path(struct fyai_ctx *ctx, const char *override,
				bool user, bool create)
{
	char *directory, *path = NULL;
	int rc;

	if (override) {
		path = strdup(override);
		fyai_error_check(ctx, path, out, "cannot copy the socket path");
		return path;
	}
	directory = remote_endpoint_dir(ctx, user, create);
	if (!directory)
		return NULL;
	rc = asprintf(&path, "%s/%s", directory, user ? "serve.sock" : "remote.sock");
	free(directory);
	fyai_error_check(ctx, rc >= 0, out, "cannot allocate the socket path");
	return path;
out:
	return NULL;
}


/* ---- the client of the SDK framing ------------------------------------- */

struct remote_client {
	struct fyai_ctx *ctx;
	struct fy_generic_builder *gb;	/* the answers are copied here */
	struct jsonrpc_conn *peer, *stdio;
	struct fyai_remote_ui *view;	/* the full-screen client, or NULL */
	int fd;
	bool failed;
	/* The control request in flight, and its answer when it came. */
	const char *waiting;
	bool answered;
	fy_generic answer;
	const char *refusal;
	/* A client that ends its input waits for the result of each message. */
	unsigned long long messages, results;
	/* A command that runs at the server, and its exit status when it ended. */
	bool running, ended;
	int status;
	struct fyai_event_source *input;
};

/* A frame of the command that runs at the server: output, or its end. */
static void remote_run_output(struct remote_client *client, fy_generic frame)
{
	fy_generic data = fy_get(frame, "data", fy_invalid);
	const char *subtype = fy_get(frame, "subtype", "");

	if (!fy_equal(fy_get(frame, "type", ""), "fyai")) {
		client->failed = true;
		return;
	}
	if (!strcmp(subtype, "exit")) {
		client->status = fy_get(frame, "status", 1);
		client->ended = true;
		return;
	}
	if (!strcmp(subtype, "stderr")) {
		if (fy_is_string(data))
			(void)fyai_sink_write(client->ctx->sink, FYAI_SINK_DIAG,
					      fy_castp(&data, ""), fy_len(data));
		return;
	}
	if (fyai_sink_frames_present(client->ctx, frame))
		client->failed = true;
}

static void remote_output(struct jsonrpc_conn *conn, fy_generic frame,
			  void *data)
{
	struct remote_client *client = data;
	fy_generic response;

	(void)conn;
	/* While a control request waits, only its answer can come. */
	if (client->waiting) {
		response = fy_get(frame, "response", fy_invalid);
		if (!fy_equal(fy_get(frame, "type", ""), "control_response") ||
		    !fy_equal(fy_get(response, "request_id", ""), client->waiting)) {
			client->failed = true;
			return;
		}
		client->waiting = NULL;
		client->answered = true;
		if (fy_equal(fy_get(response, "subtype", ""), "success"))
			client->answer = fy_gb_internalize(client->gb,
				fy_get(response, "response", fy_map_empty));
		else
			client->refusal = fy_gb_intern_string(client->gb,
				fy_get(response, "error", "the endpoint refused the request"));
		return;
	}
	if (client->view) {
		fyai_remote_ui_receive(client->view, frame);
		return;
	}
	if (client->running) {
		remote_run_output(client, frame);
		return;
	}
	if (!client->stdio) {
		client->failed = true;
		return;
	}
	if (fy_equal(fy_get(frame, "type", ""), "result"))
		client->results++;
	if (fyai_remote_send(client->stdio, frame))
		client->failed = true;
}

static void remote_input(struct jsonrpc_conn *conn, fy_generic frame,
			 void *data)
{
	struct remote_client *client = data;

	(void)conn;
	if (fy_equal(fy_get(frame, "type", ""), "user"))
		client->messages++;
	if (fyai_remote_send(client->peer, frame))
		client->failed = true;
}

/* Read the launch token: 64 characters and a newline on @fd, which closes. */
static int remote_read_token(struct fyai_ctx *ctx, int fd, char token[65])
{
	ssize_t n = 0;
	int i;

	for (i = 0; i < 65; i++) {
		do {
			n = read(fd, token + i, 1);
		} while (n < 0 && errno == EINTR);
		if (n != 1 || token[i] == '\n')
			break;
	}
	close(fd);
	fyai_error_check(ctx, i == 64 && n == 1 && token[64] == '\n', err,
			 "expected a 64-character launch token and a newline");
	token[64] = '\0';
	return 0;
err:
	return -1;
}

/* Serve the loop until @done is set, the peer goes, or the time is over. */
static int remote_wait(struct remote_client *client, const bool *done)
{
	struct fyai_ctx *ctx = client->ctx;
	fyai_event_ms_t deadline = fyai_event_now_ms() + REMOTE_ADMIT_MS;
	int rc;

	while (!*done && !client->failed && !jsonrpc_conn_closed(client->peer) &&
	       fyai_event_now_ms() < deadline && !ctx->terminate_pending &&
	       !fyai_interrupt_check(ctx)) {
		fyai_event_interrupt_ack(ctx);
		rc = fyai_event_loop_step(fyai_ctx_loop(ctx), 100);
		if (rc < 0)
			return -1;
	}
	return *done && !client->failed ? 0 : -1;
}

/*
 * Send the control request @id with @request and wait for its answer, which
 * is then client->answer. A refusal is reported with the reason it gives.
 */
static int remote_request(struct remote_client *client, const char *id,
			  fy_generic request)
{
	struct fyai_ctx *ctx = client->ctx;
	int rc;

	client->answered = false;
	client->answer = fy_invalid;
	client->refusal = NULL;
	client->waiting = id;
	rc = fyai_remote_send(client->peer, fy_mapping(client->gb,
		"type", "control_request", "request_id", id, "request", request));
	fyai_error_check(ctx, !rc, err, "cannot send the request '%s'", id);
	rc = remote_wait(client, &client->answered);
	fyai_error_check(ctx, !rc, err, "the endpoint did not answer the request '%s'",
			 id);
	fyai_error_check(ctx, !client->refusal, err, "%s", client->refusal);
	return 0;
err:
	client->waiting = NULL;
	return -1;
}

/*
 * Connect @client to the endpoint at @path: a server of the current user,
 * and with @token_fd, the launch token it asks for.
 */
static int remote_client_open(struct remote_client *client, const char *path,
			      int token_fd)
{
	struct fyai_ctx *ctx = client->ctx;
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	char token[65] = { 0 };
	uid_t uid;
	int rc;

	client->fd = -1;
	if (token_fd >= 0 && remote_read_token(ctx, token_fd, token))
		return -1;
	fyai_error_check(ctx, strlen(path) < sizeof(address.sun_path), err,
			 "the socket path '%s' is too long", path);
	memcpy(address.sun_path, path, strlen(path) + 1);
	client->fd = socket(AF_UNIX, SOCK_STREAM, 0);
	rc = fyai_remote_cloexec(client->fd);
	fyai_error_check(ctx, client->fd >= 0 && !rc, err, "socket: %s",
			 strerror(errno));
	rc = connect(client->fd, (struct sockaddr *)&address, sizeof(address));
	fyai_error_check(ctx, !rc, err, "connect '%s': %s", path, strerror(errno));
	rc = fyai_host_peer_uid(client->fd, &uid);
	fyai_error_check(ctx, !rc && uid == geteuid(), err,
			 "the endpoint '%s' does not run as the current user", path);
	client->peer = jsonrpc_conn_socket(ctx, client->fd, 0, "remote", NULL);
	fyai_error_check(ctx, client->peer, err,
			 "cannot open the connection to '%s'", path);
	jsonrpc_conn_expect_close(client->peer);
	rc = jsonrpc_conn_frames(client->peer, remote_output, client);
	fyai_error_check(ctx, !rc, err, "cannot receive frames from '%s'", path);
	if (token_fd >= 0) {
		rc = remote_request(client, "auth", fy_mapping(client->gb,
			"subtype", "fyai_authenticate", "token", token));
		memset(token, 0, sizeof(token));
		fyai_error_check(ctx, !rc, err,
				 "the endpoint refused the launch token, or did not answer");
	}
	return 0;
err:
	memset(token, 0, sizeof(token));
	return -1;
}

/*
 * Make the session a fyai session: tell the server the version of the fyai
 * protocol, and what this program can do. A server of another version refuses,
 * and says which it speaks.
 */
static int remote_hello(struct remote_client *client)
{
	fy_generic features = fy_seq_empty;
	size_t i;

	for (i = 0; i < ARRAY_SIZE(remote_features); i++)
		features = fy_append(client->gb, features,
				     fy_value(client->gb, remote_features[i]));
	return remote_request(client, "hello", fy_mapping(client->gb,
		"subtype", "fyai_hello", "version", VERSION,
		"protocol", FYAI_REMOTE_PROTOCOL, "features", features));
}

static void remote_client_close(struct remote_client *client)
{
	fyai_event_interrupt_ack(client->ctx);
	jsonrpc_conn_destroy(client->stdio);
	client->stdio = NULL;
	jsonrpc_conn_destroy(client->peer);
	client->peer = NULL;
	if (client->fd >= 0)
		close(client->fd);
	client->fd = -1;
}

int fyai_cmd_remote_connect_prepare(struct fyai_cfg *cfg, fy_generic args)
{
	/* The client of a session, unless SDK frames go on standard I/O. Its
	 * display takes the configuration that the worker sends. */
	cfg->interactive = fy_is_invalid(fy_get(args, "protocol", fy_invalid));
	return 0;
}

/* Run the full-screen client until the user detaches or the worker goes. */
static int remote_view_run(struct remote_client *client, bool watch)
{
	struct fyai_ctx *ctx = client->ctx;
	int rc = 0;

	client->view = fyai_remote_ui_open(ctx, client->peer);
	if (!client->view)
		return -1;
	fyai_remote_ui_watch(client->view, watch);
	while (!client->failed && !jsonrpc_conn_closed(client->peer) &&
	       !ctx->terminate_pending) {
		rc = fyai_remote_ui_step(client->view);
		if (rc)
			break;
		rc = fyai_event_loop_step(fyai_ctx_loop(ctx), -1);
		if (rc < 0)
			break;
	}
	fyai_remote_ui_close(client->view);
	client->view = NULL;
	if (rc < 0 || client->failed)
		return -1;
	fyai_error_check(ctx, rc == 1 || ctx->terminate_pending, err,
			 "the endpoint closed the connection");
	return 0;
err:
	return -1;
}

/* Carry SDK frames between standard input and output and the endpoint. */
static int remote_sdk_run(struct remote_client *client, const char *path)
{
	struct fyai_ctx *ctx = client->ctx;
	struct fyai_event_loop *loop = fyai_ctx_loop(ctx);
	bool input_ended = false;
	int rc;

	client->stdio = jsonrpc_conn_stdio(ctx, STDOUT_FILENO, STDIN_FILENO, 0,
					   "remote SDK", NULL);
	fyai_error_check(ctx, client->stdio, err,
			 "cannot open standard input and output");
	jsonrpc_conn_expect_close(client->stdio);
	rc = jsonrpc_conn_frames(client->stdio, remote_input, client);
	fyai_error_check(ctx, !rc, err, "cannot read standard input");
	/*
	 * The end of the input is the end of the requests, as it is for the
	 * CLI: the answers to them still come. The write side closes when
	 * each message has its result, and the server then closes.
	 */
	while (!client->failed && !jsonrpc_conn_closed(client->peer) &&
	       !ctx->terminate_pending && !fyai_interrupt_check(ctx)) {
		fyai_event_interrupt_ack(ctx);
		if (!input_ended && jsonrpc_conn_closed(client->stdio) &&
		    !jsonrpc_conn_has_output(client->peer) &&
		    client->results >= client->messages) {
			shutdown(client->fd, SHUT_WR);
			input_ended = true;
		}
		rc = fyai_event_loop_step(loop, -1);
		fyai_error_check(ctx, rc >= 0, err,
				 "the event loop of the connection stopped");
	}
	while (!client->failed && jsonrpc_conn_has_output(client->stdio)) {
		rc = fyai_event_loop_step(loop, -1);
		fyai_error_check(ctx, rc >= 0, err,
				 "the event loop of the connection stopped");
	}
	fyai_error_check(ctx, !client->failed, err,
			 "the connection to '%s' failed", path);
	return 0;
err:
	return -1;
}

int fyai_cmd_remote_connect(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = call->ctx;
	struct remote_client client = { .ctx = ctx, .gb = call->gb, .fd = -1 };
	/* The branch is the global option: -b, else $FYAI_BRANCH. */
	const char *branch = ctx->cfg->branch_explicit ? ctx->cfg->branch : NULL;
	fy_generic request;
	char *path = NULL;
	bool watch, last;
	int rc = -1;

	(void)result;
	watch = fyai_cmd_arg_bool(call, "watch");
	last = fyai_cmd_arg_bool(call, "last");
	fyai_error_check(ctx, !(last && branch), out,
			 "--last and -b name the session twice");
	fyai_error_check(ctx, !watch || ((branch || last) &&
			 !fyai_cmd_arg_str(call, "protocol")), out,
			 "--watch needs -b or --last and the full-screen client");
	path = fyai_remote_socket_path(ctx, fyai_cmd_arg_str(call, "socket"), false,
				  false);
	if (!path)
		goto out;
	rc = remote_client_open(&client, path, fy_get(call->args, "token_fd", -1));
	/* SDK frames make an SDK session; the full-screen client is fyai. */
	if (!rc && !fyai_cmd_arg_str(call, "protocol"))
		rc = remote_hello(&client);
	if (rc)
		goto out;
	if (branch || last) {
		request = fy_mapping(call->gb, "subtype", "fyai_session_attach",
				     "watch", watch);
		request = branch ? fy_assoc(call->gb, request, "branch", branch) :
			fy_assoc(call->gb, request, "resume",
				 fy_mapping(call->gb, "last", true));
		rc = remote_request(&client, "attach", request);
		if (rc) {
			fyai_error(ctx, "cannot %s the session of %s",
				   watch ? "watch" : "attach",
				   branch ? branch : "--last");
			goto out;
		}
	}
	if (!fyai_cmd_arg_str(call, "protocol"))
		rc = remote_view_run(&client, watch);
	else
		rc = remote_sdk_run(&client, path);
out:
	remote_client_close(&client);
	free(path);
	return rc ? -1 : 0;
}

/* Ask the server at the endpoint of @call for @subtype, and give its answer. */
static int remote_server_ask(struct fyai_cmd_call *call, const char *subtype,
			     fy_generic *result)
{
	struct fyai_ctx *ctx = call->ctx;
	struct remote_client client = { .ctx = ctx, .gb = call->gb, .fd = -1 };
	char *path;
	int rc = -1;

	path = fyai_remote_socket_path(ctx, fyai_cmd_arg_str(call, "socket"),
				  fyai_cmd_arg_bool(call, "projects"), false);
	if (!path)
		return -1;
	if (!remote_client_open(&client, path, -1) && !remote_hello(&client))
		rc = remote_request(&client, "server", fy_mapping(call->gb,
				    "subtype", subtype));
	if (!rc)
		*result = client.answer;
	remote_client_close(&client);
	free(path);
	return rc;
}

int fyai_cmd_remote_serve_stop(struct fyai_cmd_call *call, fy_generic *result)
{
	return remote_server_ask(call, "fyai_server_stop", result);
}

int fyai_cmd_remote_serve_status(struct fyai_cmd_call *call, fy_generic *result)
{
	return remote_server_ask(call, "fyai_server_status", result);
}

/* Send what standard input gives to the command at the server. */
static enum fyai_event_action remote_run_stdin(const struct fyai_event *ev)
{
	struct remote_client *client = ev->userdata;
	char buf[16384];
	ssize_t n;

	n = read(STDIN_FILENO, buf, sizeof(buf));
	if (n < 0 && (errno == EAGAIN || errno == EINTR))
		return FYAIEA_CONTINUE;
	if (n <= 0) {
		client->input = NULL;
		fyai_event_source_remove(ev->src);
		if (fyai_remote_send(client->peer, fy_mapping(client->gb, "type", "fyai",
				"subtype", "stdin", "closed", true)))
			client->failed = true;
		return FYAIEA_CONTINUE;
	}
	if (fyai_remote_send(client->peer, fy_mapping(client->gb, "type", "fyai",
			"subtype", "stdin",
			"data", fy_gb_string_size_create(client->gb, buf, (size_t)n))))
		client->failed = true;
	return FYAIEA_CONTINUE;
}

/* Send the file on standard input to the command at the server, and its end. */
static int remote_run_file(struct remote_client *client)
{
	char buf[16384];
	ssize_t n;

	for (;;) {
		n = read(STDIN_FILENO, buf, sizeof(buf));
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0)
			return -1;
		if (fyai_remote_send(client->peer, n ?
				fy_mapping(client->gb, "type", "fyai", "subtype", "stdin",
					   "data", fy_gb_string_size_create(client->gb,
									    buf, (size_t)n)) :
				fy_mapping(client->gb, "type", "fyai", "subtype", "stdin",
					   "closed", true)))
			return -1;
		if (!n)
			return 0;
	}
}

/*
 * Run the command line of --remote at the server and present its output.
 * Its exit status is the exit status of this process. ^C goes to the command.
 */
static int remote_command_run(struct remote_client *client)
{
	struct fyai_ctx *ctx = client->ctx;
	struct fyai_cfg *cfg = ctx->cfg;
	bool input = false, stream = false;
	struct stat st;
	int rc;

	/*
	 * A pipe or a socket is read as it comes, and a file whole; a terminal
	 * or a device gives the command no input.
	 */
	if (!terminal_is_tty(STDIN_FILENO) && !fstat(STDIN_FILENO, &st)) {
		stream = S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode);
		input = stream || S_ISREG(st.st_mode);
	}

	rc = remote_request(client, "run", fy_mapping(client->gb,
		"subtype", "fyai_run", "args", cfg->remote_args, "stdin", input,
		"cols", (long long)(ctx->stdout_tty ? ctx->tty_cols : 0)));
	if (rc)
		return -1;
	client->running = true;
	if (stream) {
		rc = fyai_event_add_fd(fyai_ctx_loop(ctx), STDIN_FILENO, FYAIEV_READ,
				       remote_run_stdin, client, &client->input);
		fyai_error_check(ctx, !rc, err, "cannot read standard input");
	} else if (input) {
		rc = remote_run_file(client);
		fyai_error_check(ctx, !rc, err, "cannot send standard input");
	}
	while (!client->ended && !client->failed &&
	       !jsonrpc_conn_closed(client->peer) && !ctx->terminate_pending) {
		if (fyai_interrupt_check(ctx)) {
			fyai_event_interrupt_ack(ctx);
			if (fyai_remote_send(client->peer, fy_mapping(client->gb,
					"type", "fyai", "subtype", "signal",
					"signal", "INT")))
				client->failed = true;
		}
		rc = fyai_event_loop_step(fyai_ctx_loop(ctx), -1);
		fyai_error_check(ctx, rc >= 0, err,
				 "the event loop of the connection stopped");
	}
	fyai_event_source_remove(client->input);
	client->input = NULL;
	fyai_sink_flush(ctx->sink);
	fyai_error_check(ctx, client->ended, err, "the server ended the command "
			 "without its exit status");
	cfg->exit_status = client->status;
	return 0;
err:
	fyai_event_source_remove(client->input);
	client->input = NULL;
	return -1;
}

/*
 * The arguments of `resume` when the words of --remote are that command, else
 * fy_invalid. It is interactive: the server selects its branch, and this
 * process shows the session.
 */
static fy_generic remote_run_resume(struct fyai_cfg *cfg)
{
	struct fyai_cmd_parsed parsed;
	const char **words;
	size_t i, n = fy_len(cfg->remote_args) - cfg->remote_globals;
	fy_generic first;
	int rc;

	first = fy_get_at(cfg->remote_args, cfg->remote_globals, fy_invalid);
	if (!n || !fyai_cmd_is_verb(fy_castp(&first, "")))
		return fy_invalid;
	words = fy_gb_alloc(cfg->gb, n * sizeof(*words), _Alignof(const char *));
	if (!words)
		return fy_invalid;
	for (i = 0; i < n; i++)
		words[i] = fy_get_at(cfg->remote_args, cfg->remote_globals + i, "");
	memset(&parsed, 0, sizeof(parsed));
	rc = fyai_cmd_parse(cfg, cfg->gb, FYAI_CMD_CLI, n, words, NULL, NULL, &parsed);
	if (rc || parsed.help || !parsed.path || strcmp(parsed.path, "resume"))
		return fy_invalid;
	return parsed.args;
}

int fyai_cmd_remote_run_prepare(struct fyai_cfg *cfg, fy_generic args)
{
	(void)args;
	fyai_cfg_error_check(cfg, cfg->remote, err, "remote run: give the command "
			     "line to the global option --remote");
	cfg->remote_resume = remote_run_resume(cfg);
	/* With no command line, the session is interactive, as it is here. */
	if (fy_is_valid(cfg->remote_resume) ||
	    (!cfg->remote_words && (cfg->interactive || terminal_is_tty(STDIN_FILENO))))
		cfg->interactive = true;
	return 0;
err:
	return -1;
}

/*
 * The endpoint of --remote TARGET: a socket; else, for ".", the server of the project
 * when one serves it and --project does not name another; else the server of
 * projects.
 */
static char *remote_run_endpoint(struct fyai_ctx *ctx)
{
	struct fyai_cfg *cfg = ctx->cfg;
	struct stat st;
	char *path;

	if (cfg->remote_socket)
		return fyai_remote_socket_path(ctx, cfg->remote_socket, false, false);
	if (!cfg->project) {
		path = fyai_remote_socket_path(ctx, NULL, false, false);
		if (!path || (!stat(path, &st) && S_ISSOCK(st.st_mode)))
			return path;
		free(path);
	}
	return fyai_remote_socket_path(ctx, NULL, true, false);
}

int fyai_cmd_remote_run(struct fyai_cmd_call *call, fy_generic *result)
{
	struct fyai_ctx *ctx = call->ctx;
	struct fyai_cfg *cfg = ctx->cfg;
	struct remote_client client = { .ctx = ctx, .gb = call->gb, .fd = -1 };
	fy_generic request, globals = fy_seq_empty;
	char *path = NULL, *cwd = NULL;
	size_t i;
	int rc = -1;

	(void)result;
	path = remote_run_endpoint(ctx);
	if (!path)
		goto out;
	cwd = cfg->project ? NULL : getcwd(NULL, 0);
	fyai_error_check(ctx, cfg->project || cwd, out,
			 "cannot read the current directory: %s", strerror(errno));
	if (remote_client_open(&client, path, -1) || remote_hello(&client))
		goto out;
	/* The directory to work in, which also names the project. */
	rc = remote_request(&client, "project", fy_mapping(call->gb,
		"subtype", "fyai_project", "path", cfg->project ? cfg->project : cwd));
	if (rc)
		goto out;
	if (!cfg->interactive) {
		rc = remote_command_run(&client);
		goto out;
	}
	/* An interactive session: the global options start its worker. */
	for (i = 0; i < cfg->remote_globals; i++)
		globals = fy_append(call->gb, globals,
				    fy_get_at(cfg->remote_args, i, fy_invalid));
	request = fy_mapping(call->gb, "subtype", "fyai_session_attach",
			     "args", globals);
	if (fy_is_valid(cfg->remote_resume))
		request = fy_assoc(call->gb, request, "resume", cfg->remote_resume);
	else if (cfg->branch)
		request = fy_assoc(call->gb, request, "branch", cfg->branch);
	rc = remote_request(&client, "attach", request);
	if (!rc)
		rc = remote_view_run(&client, false);
out:
	remote_client_close(&client);
	free(cwd);
	free(path);
	return rc ? -1 : 0;
}
