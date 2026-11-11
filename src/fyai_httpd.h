/* SPDX-License-Identifier: MIT */
/*
 * An HTTP/1.1 server on the event loop of the invocation. It listens on TCP,
 * with TLS when the caller gives a context, reads the head of each request,
 * and hands it to a handler. The handler answers it, or upgrades the
 * connection to a WebSocket that the server then carries: the handler gets
 * each text or binary message and queues messages of its own. The server
 * knows no route, no credential and no protocol over the WebSocket.
 */
#ifndef FYAI_HTTPD_H
#define FYAI_HTTPD_H

#include <stdbool.h>
#include <stddef.h>

#include <openssl/ssl.h>

#include "fyai_ws.h"

struct fyai_ctx;
struct fyai_event_loop;

/* The head of a request may be this long, with its blank line. */
#define FYAI_HTTP_HEAD_MAX	8192u

/*
 * A request head, parsed. The strings point into the copy of the head in
 * @text, which fyai_http_request_release() frees. A header that holds a value
 * that is not wanted twice is an error, and so is a header line without CRLF.
 * @headers lists every header line in order.
 *
 * @body is the body that the server read, NUL terminated. It is valid during
 * the call of the handler only, and the handler copies what it keeps. A
 * server that takes no body (max_body is 0) leaves it NULL.
 */
struct fyai_http_header {
	const char *name, *value;
};

struct fyai_http_request {
	char *text;
	const char *method, *target, *host, *origin, *bearer, *ws_key;
	bool upgrade;		/* Upgrade: websocket */
	bool connection_upgrade;	/* Connection names upgrade */
	bool http10;		/* HTTP/1.0, with FYAI_HTTP_PARSE_HTTP10 */
	long ws_version;
	struct fyai_http_header *headers;
	size_t nheaders;
	const char *body;
	size_t body_len;
};

/* Take HTTP/1.0 as well as HTTP/1.1. */
#define FYAI_HTTP_PARSE_HTTP10	1u

/*
 * Parse the head of a request that ends at @len bytes, with its blank line.
 * Returns 0, or the HTTP status of the answer: 400 for a head that is not
 * well formed, 505 for another version than HTTP/1.1 (or HTTP/1.0 when
 * @flags allow it).
 */
int fyai_http_parse(const char *head, size_t len, unsigned int flags,
		    struct fyai_http_request *req);
void fyai_http_request_release(struct fyai_http_request *req);

/* The value of the first header @name, compared without case, or NULL. */
const char *fyai_http_header(const struct fyai_http_request *req, const char *name);

/* The Origin of a browser must be the origin of the server it asks. */
bool fyai_http_origin_ok(const struct fyai_http_request *req, bool tls);

struct fyai_httpd;
struct fyai_httpd_conn;

struct fyai_httpd_ops {
	/*
	 * A request head is whole. The handler answers it with
	 * fyai_httpd_respond(), upgrades it with fyai_httpd_ws_accept(), or
	 * keeps the connection and answers later.
	 */
	void (*request)(void *data, struct fyai_httpd_conn *c,
			const struct fyai_http_request *req);
	/* A text or binary message of the client of an upgraded connection. */
	void (*ws_message)(void *data, struct fyai_httpd_conn *c,
			   const struct fyai_ws_message *msg);
	/* The queue of an upgraded connection went down: it may take more. */
	void (*ws_drain)(void *data, struct fyai_httpd_conn *c);
	/*
	 * The connection ended, once, from whatever cause. The handler stops
	 * what it keeps for it. The connection is not valid after the call.
	 */
	void (*closed)(void *data, struct fyai_httpd_conn *c);
	void (*log)(void *data, const char *line);
	void *data;
};

struct fyai_httpd_opts {
	struct fyai_event_loop *loop;	/* NULL for the loop of the invocation */
	size_t max_conns;	/* more are refused */
	size_t max_message;	/* the longest message of a WebSocket */
	size_t max_output;	/* a client that holds more is dropped */
	/*
	 * The time a request has to come whole; 0 sets no limit. A connection
	 * that waits for its next request after an answer has the same limit.
	 */
	unsigned int head_ms;
	/*
	 * The longest body of a request, with Content-Length or chunked. 0 takes
	 * no body: a byte after the head is an error. A request with a longer
	 * body gets 413. The server answers Expect: 100-continue.
	 */
	size_t max_body;
	/*
	 * Keep a connection for the next request after an answer that
	 * fyai_httpd_conn_done() ended, unless the client or the answer says
	 * close. fyai_httpd_respond() always ends the connection.
	 */
	bool keepalive;
	bool http10;		/* take HTTP/1.0 requests */
};

struct fyai_httpd *fyai_httpd_new(struct fyai_ctx *ctx,
				  const struct fyai_httpd_opts *opts,
				  const struct fyai_httpd_ops *ops);

/*
 * Listen on @address, HOST:PORT or [HOST]:PORT with a numeric host; port 0
 * takes one, and @bound receives the address that is listening. With @tls the
 * listener speaks TLS: it takes a reference to the context, and the caller
 * keeps its own. Returns 0, or -1 with a diagnostic.
 */
int fyai_httpd_listen(struct fyai_httpd *h, const char *address, SSL_CTX *tls,
		      char *bound, size_t bound_size);

/*
 * Serve the listening socket @fd, which the caller bound and made
 * non-blocking and close-on-exec, for a caller that has its own rule for the
 * address. @h owns @fd after a success; after a failure the caller does. The
 * rest is as for fyai_httpd_listen().
 */
int fyai_httpd_adopt(struct fyai_httpd *h, int fd, SSL_CTX *tls);

/* Stop listening and end every connection but @keep, which may be NULL. */
void fyai_httpd_stop(struct fyai_httpd *h, struct fyai_httpd_conn *keep);

/* Stop listening, end every connection and free @h. */
void fyai_httpd_close(struct fyai_httpd *h);

/* ---- a connection --------------------------------------------------------- */

void fyai_httpd_conn_set_user(struct fyai_httpd_conn *c, void *user);
void *fyai_httpd_conn_user(const struct fyai_httpd_conn *c);
const char *fyai_httpd_conn_peer(const struct fyai_httpd_conn *c);
bool fyai_httpd_conn_tls(const struct fyai_httpd_conn *c);

/*
 * Cancel the time limit of the request, for a handler that answers later. It
 * must then answer, or end the connection.
 */
void fyai_httpd_conn_hold(struct fyai_httpd_conn *c);

/*
 * Answer with @status, the lines of @extra (each ends in CRLF, or NULL), and
 * @body of @body_len bytes, then end the connection once it is written.
 * @why, when it is not NULL, goes to the log.
 */
void fyai_httpd_respond(struct fyai_httpd_conn *c, int status, const char *why,
			const char *extra, const void *body, size_t body_len);

/*
 * The handler writes its own answer: queue @n bytes as they are, and try to
 * send them at once, so each call is a segment of its own when the socket
 * allows. Returns -1, and ends the connection, when the client holds more than
 * max_output or on no memory. The head of the answer is the handler's.
 */
int fyai_httpd_conn_write(struct fyai_httpd_conn *c, const void *data, size_t n);

/*
 * The answer is whole. With @close, or when the request or the server says
 * close, end the connection once the output is written; else wait for the next
 * request, which can be in the input already. It may call the handler.
 */
void fyai_httpd_conn_done(struct fyai_httpd_conn *c, bool close);

/* End the connection now. */
void fyai_httpd_kill(struct fyai_httpd_conn *c);

/* Wake the connection to write what was queued for it. */
void fyai_httpd_kick(struct fyai_httpd_conn *c);

/* ---- the WebSocket -------------------------------------------------------- */

/*
 * Check that @req asks for WebSocket version 13 with a valid key. When it does
 * not, the connection is answered, with 426 or 400, and the call returns -1.
 */
int fyai_httpd_ws_check(struct fyai_httpd_conn *c, const struct fyai_http_request *req);

/*
 * Switch to the WebSocket: send the 101 answer to a request that
 * fyai_httpd_ws_check() accepted. Returns 0, or -1 when the connection ended.
 */
int fyai_httpd_ws_accept(struct fyai_httpd_conn *c, const struct fyai_http_request *req);

/* Queue one frame. Returns -1 when the client holds too much, or on no memory. */
int fyai_httpd_ws_send(struct fyai_httpd_conn *c, int opcode, const void *data, size_t n);

/* Close the WebSocket with @code, and end once the close frame has left. */
void fyai_httpd_ws_close(struct fyai_httpd_conn *c, unsigned int code);

/* The bytes that wait for the client. */
size_t fyai_httpd_ws_pending(const struct fyai_httpd_conn *c);

#endif
