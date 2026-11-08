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

/* A peer that reads no more than this is dropped, not buffered without end. */
#define REMOTE_OUTPUT_MAX	(32u * 1024u * 1024u)

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
	struct jsonrpc_conn *peer;
	int fd;
	bool failed;
	/* The control request in flight, and its answer when it came. */
	const char *waiting;
	bool answered;
	fy_generic answer;
	const char *refusal;
};

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
	client->failed = true;
}

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
	jsonrpc_conn_destroy(client->peer);
	client->peer = NULL;
	if (client->fd >= 0)
		close(client->fd);
	client->fd = -1;
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
