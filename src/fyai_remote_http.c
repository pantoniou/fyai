/* SPDX-License-Identifier: MIT */
#include "config.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <openssl/crypto.h>

#include "fyai.h"
#include "fyai_event.h"
#include "fyai_httpd.h"
#include "fyai_jsonrpc.h"
#include "fyai_net.h"
#include "fyai_remote_http.h"
#include "fyai_remote_int.h"
#include "fyai_ws.h"
#include "utils.h"

#define FYAI_MODULE FYAIEM_REMOTE

/* The frame limit of the JSON-RPC connection, which the lines end at. */
#define HTTP_FRAME_MAX		(16UL << 20)
/* Connections of all kinds; a client over it is refused. */
#define HTTP_CONN_MAX		256u
/* Output waiting for a client that reads slowly stops the reads of its session. */
#define HTTP_HIGH_WATER		(4u * 1024u * 1024u)

/* ---- the shared token ---------------------------------------------------- */

int fyai_remote_token_verify(void *token, const char *bearer,
			     struct fyai_remote_identity *id)
{
	const char *want = token;
	size_t n = strlen(want);

	if (!bearer || strlen(bearer) != n || CRYPTO_memcmp(bearer, want, n))
		return -1;
	id->subject[0] = '\0';
	return 0;
}

/* ---- the bridge ---------------------------------------------------------- */

struct fyai_remote_http {
	struct fyai_ctx *ctx;
	struct fyai_httpd *httpd;
	struct fyai_remote_auth auth;
	struct fyai_remote_http_ops ops;
};

/* One client: its WebSocket, and the socket of lines that the server has. */
struct remote_client {
	struct fyai_remote_http *http;
	struct fyai_httpd_conn *conn;
	struct fyai_netio sock;		/* our end of the pair */
	struct fyai_event_source *src;
	bool eof;			/* the server ended the client */
	struct fyai_buf to_sock;	/* lines to the server */
	struct fyai_buf lines;	/* a line from the server that is not whole */
};

static void remote_log(void *data, const char *line)
{
	struct fyai_remote_http *http = data;

	if (http->ops.log)
		http->ops.log(http->ops.data, line);
}

static void remote_logf(struct fyai_remote_http *http, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static void remote_logf(struct fyai_remote_http *http, const char *fmt, ...)
{
	va_list ap;
	char *text;
	int len;

	if (!http->ops.log)
		return;
	va_start(ap, fmt);
	len = vasprintf(&text, fmt, ap);
	va_end(ap);
	if (len < 0)
		return;
	http->ops.log(http->ops.data, text);
	free(text);
}

/* Watch what the server has to read, and what it wrote. */
static void client_interest(struct remote_client *cl)
{
	unsigned int events = 0;

	if (!cl->src)
		return;
	if (!cl->eof && fyai_httpd_ws_pending(cl->conn) < HTTP_HIGH_WATER)
		events |= FYAIEV_READ;
	if (fyai_buf_pending(&cl->to_sock))
		events |= FYAIEV_WRITE;
	(void)fyai_event_fd_modify(cl->src, events ? events : FYAIEV_EOF);
}

/* Lines of the server are frames to the client. */
static int client_lines(struct remote_client *cl)
{
	struct fyai_buf *b = &cl->lines;
	char *nl;
	size_t len;

	while (fyai_buf_pending(b) &&
	       (nl = memchr(b->data + b->off, '\n', fyai_buf_pending(b)))) {
		len = (size_t)(nl - (b->data + b->off));
		if (len && fyai_httpd_ws_send(cl->conn, FYAI_WS_TEXT, b->data + b->off, len))
			return -1;
		fyai_buf_consume(b, len + 1);
	}
	return fyai_buf_pending(b) > HTTP_FRAME_MAX ? -1 : 0;
}

static enum fyai_event_action client_sock_cb(const struct fyai_event *ev)
{
	struct remote_client *cl = ev->userdata;
	struct fyai_httpd_conn *conn = cl->conn;
	unsigned char chunk[65536];
	ssize_t n;
	int rounds;

	if (ev->events & FYAIEV_ERROR) {
		fyai_httpd_kill(conn);
		return FYAIEA_CONTINUE;
	}
	if ((ev->events & FYAIEV_WRITE) && fyai_netio_flush(&cl->sock, &cl->to_sock)) {
		fyai_httpd_kill(conn);
		return FYAIEA_CONTINUE;
	}
	if (ev->events & (FYAIEV_READ | FYAIEV_EOF))
		for (rounds = 0; rounds < 16 && !cl->eof &&
		     fyai_httpd_ws_pending(conn) < HTTP_HIGH_WATER; rounds++) {
			n = read(cl->sock.fd, chunk, sizeof(chunk));
			if (n < 0 && errno == EINTR)
				continue;
			if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
				break;
			if (n <= 0) {
				/* The server ended the client: close the WebSocket. */
				cl->eof = true;
				fyai_httpd_ws_close(conn, 1000);
				break;
			}
			if (fyai_buf_append_data(&cl->lines, chunk, (size_t)n) || client_lines(cl)) {
				fyai_httpd_kill(conn);
				return FYAIEA_CONTINUE;
			}
		}
	fyai_httpd_kick(conn);
	client_interest(cl);
	return FYAIEA_CONTINUE;
}

static void client_free(struct remote_client *cl)
{
	fyai_event_source_remove(cl->src);
	cl->src = NULL;
	fyai_netio_close(&cl->sock);
	fyai_buf_free(&cl->to_sock);
	fyai_buf_free(&cl->lines);
	free(cl);
}

static void remote_closed(void *data, struct fyai_httpd_conn *c)
{
	struct remote_client *cl = fyai_httpd_conn_user(c);

	(void)data;
	if (cl)
		client_free(cl);
}

/* One message of the client. */
static void remote_message(void *data, struct fyai_httpd_conn *c,
			   const struct fyai_ws_message *msg)
{
	static const unsigned char newline = '\n';
	struct remote_client *cl = fyai_httpd_conn_user(c);

	(void)data;
	if (msg->opcode != FYAI_WS_TEXT) {
		fyai_httpd_ws_close(c, 1003);
		return;
	}
	/* One frame is one line: a newline in it would make two. */
	if (memchr(msg->data, '\n', msg->size) || memchr(msg->data, '\r', msg->size)) {
		fyai_httpd_ws_close(c, 1007);
		return;
	}
	if (fyai_buf_pending(&cl->to_sock) + msg->size + 1 > REMOTE_OUTPUT_MAX ||
	    fyai_buf_append_data(&cl->to_sock, msg->data, msg->size) ||
	    fyai_buf_append_data(&cl->to_sock, &newline, 1) ||
	    fyai_netio_flush(&cl->sock, &cl->to_sock))
		fyai_httpd_kill(c);
}

static void remote_drain(void *data, struct fyai_httpd_conn *c)
{
	struct remote_client *cl = fyai_httpd_conn_user(c);

	(void)data;
	if (cl)
		client_interest(cl);
}

/* True when @bearer fails the verifier of the listener. */
static bool remote_auth_fail(struct fyai_remote_http *http, const char *bearer,
			     struct fyai_remote_identity *id)
{
	return !*bearer || http->auth.verify(http->auth.data, bearer, id);
}

/* Give the server a client for the WebSocket of @c, and switch to it. */
static void remote_upgrade(struct fyai_remote_http *http, struct fyai_httpd_conn *c,
			   const struct fyai_http_request *req,
			   const struct fyai_remote_identity *id)
{
	struct remote_client *cl;
	int sv[2], rc;

	cl = calloc(1, sizeof(*cl));
	if (!cl) {
		fyai_httpd_respond(c, 503, "no memory", NULL, NULL, 0);
		return;
	}
	if (fyai_remote_socketpair(SOCK_STREAM, sv)) {
		free(cl);
		fyai_httpd_respond(c, 503, "no socket pair", NULL, NULL, 0);
		return;
	}
	if (http->ops.admit(http->ops.data, sv[1], id, fyai_httpd_conn_peer(c),
			    fyai_httpd_conn_tls(c))) {
		close(sv[0]);
		free(cl);
		fyai_httpd_respond(c, 503, "the server refused the client", NULL, NULL, 0);
		return;
	}
	/* sv[1] belongs to the server now. */
	fyai_netio_init(&cl->sock, sv[0]);
	cl->http = http;
	cl->conn = c;
	if (fyai_net_nonblock(sv[0])) {
		client_free(cl);
		fyai_httpd_kill(c);
		return;
	}
	fyai_httpd_conn_set_user(c, cl);
	if (fyai_httpd_ws_accept(c, req))
		return;
	rc = fyai_event_add_fd(fyai_ctx_loop(http->ctx), sv[0], FYAIEV_READ,
			       client_sock_cb, cl, &cl->src);
	if (rc) {
		fyai_httpd_kill(c);
		return;
	}
	remote_logf(http, "%s %s: upgraded%s%s", fyai_httpd_conn_tls(c) ? "https" : "http",
		    fyai_httpd_conn_peer(c), id->subject[0] ? " as " : "", id->subject);
}

/* A request head is whole: answer it. */
static void remote_request(void *data, struct fyai_httpd_conn *c,
			   const struct fyai_http_request *req)
{
	struct fyai_remote_http *http = data;
	struct fyai_remote_identity id = { .subject = "" };
	const char *query;
	size_t n;

	if (!req->bearer || remote_auth_fail(http, req->bearer, &id)) {
		fyai_httpd_respond(c, 401, "no valid token",
				   "WWW-Authenticate: Bearer\r\n", NULL, 0);
		return;
	}
	if (strcmp(req->method, "GET")) {
		fyai_httpd_respond(c, 405, "not GET", "Allow: GET\r\n", NULL, 0);
		return;
	}
	query = strchr(req->target, '?');
	n = query ? (size_t)(query - req->target) : strlen(req->target);
	if (n != strlen(FYAI_HTTP_SESSION_PATH) ||
	    memcmp(req->target, FYAI_HTTP_SESSION_PATH, n)) {
		fyai_httpd_respond(c, 404, "no such path", NULL, NULL, 0);
		return;
	}
	if (fyai_httpd_ws_check(c, req))
		return;
	if (!fyai_http_origin_ok(req, fyai_httpd_conn_tls(c))) {
		fyai_httpd_respond(c, 403, "another origin", NULL, NULL, 0);
		return;
	}
	remote_upgrade(http, c, req, &id);
}

struct fyai_remote_http *
fyai_remote_http_new(struct fyai_ctx *ctx, const struct fyai_remote_auth *auth,
		     const struct fyai_remote_http_ops *ops)
{
	struct fyai_httpd_opts opts = {
		.max_conns = HTTP_CONN_MAX,
		.max_message = HTTP_FRAME_MAX,
		.max_output = REMOTE_OUTPUT_MAX,
		.head_ms = REMOTE_ADMIT_MS,
	};
	struct fyai_httpd_ops hops = {
		.request = remote_request,
		.ws_message = remote_message,
		.ws_drain = remote_drain,
		.closed = remote_closed,
		.log = remote_log,
	};
	struct fyai_remote_http *http = calloc(1, sizeof(*http));

	fyai_error_check(ctx, http, err, "cannot allocate the HTTP front");
	http->ctx = ctx;
	http->auth = *auth;
	http->ops = *ops;
	hops.data = http;
	http->httpd = fyai_httpd_new(ctx, &opts, &hops);
	if (!http->httpd) {
		free(http);
		return NULL;
	}
	return http;
err:
	return NULL;
}

int fyai_remote_http_listen(struct fyai_remote_http *http, const char *address,
			    SSL_CTX *tls, char *bound, size_t bound_size)
{
	return fyai_httpd_listen(http->httpd, address, tls, bound, bound_size);
}

void fyai_remote_http_close(struct fyai_remote_http *http)
{
	if (!http)
		return;
	fyai_httpd_close(http->httpd);
	free(http);
}
