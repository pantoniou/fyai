/*
 * fyai_jsonrpc.c - asynchronous JSON-RPC 2.0 client over stdio or HTTP
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#define FYAI_MODULE FYAIEM_STREAM

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <poll.h>
#include <unistd.h>

#include "fyai_curl.h"
#include "fyai_event.h"
#include "fyai_jsonrpc.h"
#include "fyai_scratch.h"
#include "fyai_log.h"
#include "utils.h"

enum jsonrpc_transport {
	JSONRPC_STDIO,
	JSONRPC_HTTP,
	JSONRPC_TUNNEL,
};

/* The length of an image frame, before its image. */
#define JSONRPC_IMAGE_LEN	sizeof(uint64_t)

/* Maximum newline-terminated standard-I/O frame size. */
#define JSONRPC_FRAME_MAX	(16UL << 20)

/* The waits before the stream of an HTTP server opens again. */
#define JSONRPC_STREAM_RETRY_MS		500
#define JSONRPC_STREAM_RETRY_MAX_MS	30000
/* After a stream that delivered messages and ended: the server closed it. */
#define JSONRPC_STREAM_REOPEN_MS	100

/*
 * A parser of a stream of server-sent events: lines of fields, an empty line
 * ends an event. It keeps the last event id, which a stream that opens again
 * sends back.
 */
struct jsonrpc_sse {
	struct fyai_buf line;		/* the line in hand */
	struct fyai_buf data;		/* the data of the event in hand */
	bool has_data;
	char *last_id;
	long retry_ms;			/* the retry the server asked for */
	bool failed;			/* a line or an event too large */
};

/* A message of the stream, served from the loop and not from curl. */
struct jsonrpc_sse_msg {
	struct jsonrpc_sse_msg *next;
	char *json;
};

struct jsonrpc_conn {
	struct fyai_ctx *ctx;
	enum jsonrpc_transport transport;
	const char *name;
	const char *log_channel;
	int timeout_s;
	long long next_id;

	/* Standard input and output state belongs to the connection. */
	int stdin_fd;
	int stdout_fd;
	int owned_fd;
	struct fyai_event_source *read_src;
	struct fyai_event_source *write_src;
	struct fyai_buf rx;
	struct fyai_buf tx;
	/* Encodes one frame; each frame resets it, as its bytes are copied to
	 * @tx at once. The builder has a tag of its own, so a reset gives its
	 * memory back. */
	struct fy_generic_builder *frame_gb;
	/* Parses each line that a raw frame handler gets. A handler keeps no
	 * borrowed value, so each line resets it. */
	struct fy_generic_builder *rx_gb;
	/* Image mode: each frame is a length and the image of a generic. The
	 * image of the frame in hand stays until the next one arrives. */
	bool image;
	void *image_rx;
	size_t image_hint;		/* the size of the last frame sent */
	size_t tx_off;
	struct jsonrpc_request *pending;
	struct jsonrpc_request *flush_wait;	/* notifications awaiting write */
	/*
	 * Independent facts, not phases. The input and the output of a stdio
	 * peer end apart: an end of input leaves the output usable, and a
	 * failed write can come before or after it, so one close reason would
	 * lose one of them. protocol_failed asks the reader to close the
	 * connection and is set by a local failed send as well.
	 */
	bool protocol_failed;			/* peer sent an invalid frame */
	bool closed;
	bool write_failed;			/* the peer takes no more output */
	unsigned int late_close_tries;		/* deferred drain count */
	jsonrpc_serve_fn serve;
	void *serve_userdata;
	bool serve_deferred;	/* handler will answer later */
	bool expect_close;
	jsonrpc_close_fn on_close;		/* once, when the connection closes */
	void *on_close_data;
	bool accept_batches;
	/* Every frame goes here, and none is read as JSON-RPC. */
	jsonrpc_frame_fn frame;
	void *frame_userdata;
	bool batch_active;
	fy_generic batch_responses;

	/* http */
	const char *endpoint;
	struct jsonrpc_http_hooks hooks;
	bool has_hooks;
	/*
	 * The stream of the messages that the server sends on its own: a GET
	 * that stays open, opened again when it ends.
	 */
	bool listening;
	struct jsonrpc_request *stream;
	struct fyai_event_source *stream_timer;
	struct jsonrpc_sse sse;
	bool stream_delivered;		/* the open stream delivered a message */
	long stream_retry_ms;
	struct jsonrpc_sse_msg *inbox, **inbox_tail;
	jsonrpc_refused_fn refused;
	void *refused_data;

	/* tunnel: the owner carries each frame to the peer */
	jsonrpc_tunnel_send_fn tunnel_send;
	void *tunnel_userdata;
};

/* The context that this object stands on. */
static inline struct fyai_ctx *jsonrpc_conn_ctx(const struct jsonrpc_conn *o)
{
	return o->ctx;
}

static void jsonrpc_tunnel_flush(void *userdata);

/* True for a transport that matches responses to pending requests. */
static bool jsonrpc_conn_framed(const struct jsonrpc_conn *conn)
{
	return conn->transport == JSONRPC_STDIO ||
	       conn->transport == JSONRPC_TUNNEL;
}

/* A request settles one time: PENDING moves to OK or to FAILED. */
enum jsonrpc_request_state {
	JSONRPC_REQ_PENDING,
	JSONRPC_REQ_OK,
	JSONRPC_REQ_FAILED,
};

struct jsonrpc_request {
	struct jsonrpc_conn *conn;
	jsonrpc_complete_fn complete;
	void *userdata;
	const char *method;
	long long id;
	bool notification;
	bool detach;			/* the connection ends with this settle */
	struct timespec started;

	/* http */
	struct fyai_curl_transfer *transfer;
	CURL *curl;
	struct curl_slist *headers;
	struct curl_slist *response_headers;
	struct fyai_buf response;
	char *body;

	/* Standard input and output request state. */
	struct fyai_event_source *timer_src;
	struct jsonrpc_request *next;

	fy_generic result;
	long status;
	CURLcode code;
	enum jsonrpc_request_state state;
	/* The owner asked to cancel while a transfer was in flight. */
	bool cancel_requested;
};

static void jsonrpc_request_free(struct jsonrpc_request *req);
static void jsonrpc_conn_unlink(struct jsonrpc_request *req);

static void jsonrpc_log(struct jsonrpc_request *req, bool success)
{
	struct jsonrpc_conn *conn = req->conn;
	struct timespec end;
	double elapsed_ms;

	if (!conn->log_channel || !jsonrpc_conn_ctx(conn)->cfg->mcp_logging)
		return;
	clock_gettime(CLOCK_MONOTONIC, &end);
	elapsed_ms = (double)(end.tv_sec - req->started.tv_sec) * 1000.0 +
		(double)(end.tv_nsec - req->started.tv_nsec) / 1000000.0;
	(void)fyai_log_generic(jsonrpc_conn_ctx(conn), conn->log_channel,
		fy_null_filtered_mapping(
			"event", "request", "server", conn->name,
			"transport", conn->transport == JSONRPC_STDIO ?
				"stdio" : conn->transport == JSONRPC_TUNNEL ?
				"tunnel" : "http",
			"method", req->method,
			"id", req->notification ? fy_null : fy_value(req->id),
			"notification", req->notification, "success", success,
			"elapsed_ms", elapsed_ms));
}

static void jsonrpc_drop_source(struct fyai_event_source **srcp)
{
	fyai_event_source_remove(*srcp);
	*srcp = NULL;
}

static void jsonrpc_stdio_sources_drop(struct jsonrpc_request *req)
{
	jsonrpc_drop_source(&req->timer_src);
}

/* Latch completion once and hand the result to the owner. */
static void jsonrpc_finish(struct jsonrpc_request *req, bool ok,
			   fy_generic result, long status, CURLcode code)
{
	if (req->state != JSONRPC_REQ_PENDING)
		return;
	/* Remove the request before another frame can use its ID. */
	jsonrpc_conn_unlink(req);
	jsonrpc_stdio_sources_drop(req);
	req->result = result;
	req->status = status;
	req->code = code;
	req->state = ok ? JSONRPC_REQ_OK : JSONRPC_REQ_FAILED;
	jsonrpc_log(req, ok);
	/* The connection is going: no completion may reach it. */
	if (req->detach)
		req->conn = NULL;
	if (req->complete)
		req->complete(req, req->userdata);
}

/*
 * stdio transport
 */

/* Match one framed line against this request. Returns true once the request is
 * finished (matched response, or a fatal parse/transport error). */
/* Unlink a request from one of the connection's lists. */
static struct jsonrpc_request **
jsonrpc_list_find(struct jsonrpc_request **head, struct jsonrpc_request *req)
{
	struct jsonrpc_request **pp;

	for (pp = head; *pp; pp = &(*pp)->next)
		if (*pp == req)
			return pp;
	return NULL;
}

static void jsonrpc_list_remove(struct jsonrpc_request **head,
				struct jsonrpc_request *req)
{
	struct jsonrpc_request **pp;

	pp = jsonrpc_list_find(head, req);
	if (pp)
		*pp = req->next;
	req->next = NULL;
}

/* Remove a request from its connection list. */
static void jsonrpc_conn_unlink(struct jsonrpc_request *req)
{
	if (!req || !req->conn || !jsonrpc_conn_framed(req->conn))
		return;
	jsonrpc_list_remove(&req->conn->pending, req);
	jsonrpc_list_remove(&req->conn->flush_wait, req);
}

static struct jsonrpc_request *jsonrpc_pending_take(struct jsonrpc_conn *conn,
						    long long id)
{
	struct jsonrpc_request **pp, *req;

	for (pp = &conn->pending; *pp; pp = &(*pp)->next) {
		if ((*pp)->id != id)
			continue;
		req = *pp;
		*pp = req->next;
		req->next = NULL;
		return req;
	}
	return NULL;
}

static int jsonrpc_conn_arm_write(struct jsonrpc_conn *conn);
static int jsonrpc_http_post(struct jsonrpc_conn *conn, fy_generic frame);

/* Queue one output frame. */
static int jsonrpc_conn_send(struct jsonrpc_conn *conn, const char *json)
{
	size_t len = strlen(json);
	int rc;

	rc = fyai_buf_reserve(&conn->tx, conn->tx.len + len + 2);
	if (rc)
		return -1;
	memcpy(conn->tx.data + conn->tx.len, json, len);
	conn->tx.len += len;
	conn->tx.data[conn->tx.len++] = '\n';
	conn->tx.data[conn->tx.len] = '\0';
	return jsonrpc_conn_arm_write(conn);
}

/* Answer an inbound request. @error wins when valid, per JSON-RPC. */
static int jsonrpc_conn_reply(struct jsonrpc_conn *conn, fy_generic id,
			      fy_generic result, fy_generic error)
{
	struct fy_generic_builder *gb;
	fy_generic frame;
	const char *body;

	gb = fyai_ctx_transient_gb(jsonrpc_conn_ctx(conn));
	if (!gb)
		goto err;
	if (fy_is_valid(error))
		frame = fy_gb_mapping(gb, "jsonrpc", "2.0", "id", id,
				      "error", error);
	else
		frame = fy_gb_mapping(gb, "jsonrpc", "2.0", "id", id,
				      "result", fy_is_valid(result) ?
						result : fy_null);
	if (conn->batch_active) {
		conn->batch_responses = fy_append(gb, conn->batch_responses, frame);
		if (fy_is_invalid(conn->batch_responses))
			goto err;
		return 0;
	}
	if (conn->transport == JSONRPC_TUNNEL) {
		if (conn->tunnel_send(conn->tunnel_userdata, frame))
			goto err;
		return 0;
	}
	/* An HTTP server takes the answer as a POST of its own. */
	if (conn->transport == JSONRPC_HTTP) {
		if (jsonrpc_http_post(conn, frame))
			goto err;
		return 0;
	}
	body = emit_json_string(gb, frame);
	if (!body || jsonrpc_conn_send(conn, body))
		goto err;
	return 0;
err:
	fyai_error(jsonrpc_conn_ctx(conn), "%s could not send JSON-RPC response", conn->name);
	conn->protocol_failed = true;
	return -1;
}

/* Process one decoded frame. */
static void jsonrpc_conn_frame(struct jsonrpc_conn *conn, fy_generic doc)
{
	struct fyai_ctx *ctx = jsonrpc_conn_ctx(conn);
	struct jsonrpc_request *req;
	fy_generic id, method, error, result;
	const char *name;

	id = fy_get(doc, "id", fy_invalid);
	method = fy_get(doc, "method", fy_invalid);

	if (fy_is_valid(method)) {
		name = fy_castp(&method, "");
		if (!conn->serve) {
			/* Reject a request when no handler is available. */
			if (fy_is_valid(id))
				jsonrpc_conn_reply(conn, id, fy_invalid,
					fy_gb_mapping(
					    fyai_ctx_transient_gb(ctx),
					    "code", -32601LL,
					    "message", "method not found"));
			return;
		}
		error = fy_invalid;
		conn->serve_deferred = false;
		result = conn->serve(conn, name, fy_get(doc, "params", fy_null),
				     id, conn->serve_userdata, &error);
		if (fy_is_valid(id) && !conn->serve_deferred)
			jsonrpc_conn_reply(conn, id, result, error);
		conn->serve_deferred = false;
		return;
	}

	if (!fy_is_valid(id))
		return;			/* neither a call nor a reply */
	req = jsonrpc_pending_take(conn, fy_cast(id, -1LL));
	if (!req)
		return;			/* not ours, or already finished */

	error = fy_get(doc, "error", fy_invalid);
	if (fy_is_valid(error)) {
		fyai_error(ctx, "%s %s: %s", conn->name, req->method,
			   fy_get(error, "message", "server error"));
		jsonrpc_finish(req, false, fy_invalid, 0, CURLE_OK);
		return;
	}
	result = fy_get(doc, "result", fy_invalid);
	if (fy_is_invalid(result)) {
		fyai_error(ctx, "%s %s response has no result",
			   conn->name, req->method);
		jsonrpc_finish(req, false, fy_invalid, 0, CURLE_OK);
		return;
	}
	jsonrpc_finish(req, true, result, 0, CURLE_OK);
}

static void jsonrpc_conn_line(struct jsonrpc_conn *conn, const char *line)
{
	struct fy_generic_builder *gb;
	fy_generic doc, item, method, id, protocol;
	const char *body;

	while (*line == ' ' || *line == '\t' || *line == '\r')
		line++;
	if (!*line)
		return;
	/* Skip and report non-JSON lines without desynchronizing the stream. */
	if (*line != '{' && (!conn->accept_batches || *line != '[')) {
		fyai_debug(jsonrpc_conn_ctx(conn), "%s skipped a non-frame line: %.60s",
			   conn->name, line);
		return;
	}
	if (conn->frame) {
		if (!conn->rx_gb)
			conn->rx_gb = fy_generic_builder_create(NULL);
		else
			fy_generic_builder_reset(conn->rx_gb);
		gb = conn->rx_gb;
	} else {
		gb = fyai_ctx_transient_gb(jsonrpc_conn_ctx(conn));
	}
	if (!gb) {
		fyai_error(jsonrpc_conn_ctx(conn), "%s could not acquire transient storage",
			   conn->name);
		return;
	}
	doc = parse_json_string(gb, line);
	if (conn->accept_batches && fy_is_sequence(doc)) {
		if (fy_empty(doc)) {
			jsonrpc_conn_reply(conn, fy_null, fy_invalid,
				fy_mapping(gb, "code", -32600LL,
					   "message", "invalid request"));
			return;
		}
		conn->batch_active = true;
		conn->batch_responses = fy_seq_empty;
		fy_foreach(item, doc) {
			method = fy_get(item, "method", fy_invalid);
			id = fy_get(item, "id", fy_invalid);
			protocol = fy_get(item, "jsonrpc", fy_invalid);
			if (!fy_is_mapping(item) ||
			    !fy_equal(protocol, "2.0") ||
			    !fy_is_string(method) ||
			    (fy_is_valid(id) && !fy_is_string(id) &&
			     !fy_generic_is_int(id))) {
				if (jsonrpc_conn_reply(conn, fy_null, fy_invalid,
					fy_mapping(gb, "code", -32600LL,
						   "message", "invalid request")))
					break;
				continue;
			}
			jsonrpc_conn_frame(conn, item);
			if (conn->protocol_failed)
				break;
		}
		conn->batch_active = false;
		if (!conn->protocol_failed &&
		    !fy_empty(conn->batch_responses)) {
			body = emit_json_string(gb, conn->batch_responses);
			if (!body || jsonrpc_conn_send(conn, body)) {
				fyai_error(jsonrpc_conn_ctx(conn),
					   "%s could not send JSON-RPC batch",
					   conn->name);
				conn->protocol_failed = true;
			}
		}
		conn->batch_responses = fy_invalid;
		return;
	}
	if (!fy_is_mapping(doc)) {
		parse_diag_report(jsonrpc_conn_ctx(conn), doc,
			fy_is_valid(doc) ? "frame is not an object" :
			"malformed JSON",
			fy_sprintfa("%s received invalid stdio JSON", conn->name));
		/* Stop waiting requests when the peer sends an invalid frame. */
		conn->protocol_failed = true;
		return;
	}
	if (conn->frame) {
		conn->frame(conn, doc, conn->frame_userdata);
		return;
	}
	jsonrpc_conn_frame(conn, doc);
}

void jsonrpc_conn_accept_batches(struct jsonrpc_conn *conn)
{
	if (conn && conn->transport == JSONRPC_STDIO)
		conn->accept_batches = true;
}

bool jsonrpc_conn_in_batch(const struct jsonrpc_conn *conn)
{
	return conn && conn->batch_active;
}

/* Settle pending and flush-wait requests after connection failure. */
static void jsonrpc_conn_settle(struct jsonrpc_conn *conn, const char *why,
				bool detach)
{
	struct jsonrpc_request *req;

	while ((req = conn->pending)) {
		conn->pending = req->next;
		req->next = NULL;
		/* Report only an unexpected close. */
		if (!conn->expect_close)
			fyai_error(jsonrpc_conn_ctx(conn), "%s %s %s", conn->name,
				   req->method, why);
		/* Detach before completion, which may destroy the request. */
		req->detach = detach;
		jsonrpc_finish(req, false, fy_invalid, 0, CURLE_OK);
	}
	while ((req = conn->flush_wait)) {
		conn->flush_wait = req->next;
		req->next = NULL;
		req->detach = detach;
		jsonrpc_finish(req, false, fy_invalid, 0, CURLE_OK);
	}
}

static void jsonrpc_conn_fail_pending(struct jsonrpc_conn *conn,
				      const char *why)
{
	jsonrpc_close_fn fn = conn->on_close;
	void *data = conn->on_close_data;

	conn->closed = true;
	jsonrpc_conn_settle(conn, why, false);
	/* Once, and the owner may destroy the connection in it. */
	conn->on_close = NULL;
	if (fn)
		fn(data);
}

void jsonrpc_conn_on_close(struct jsonrpc_conn *conn, jsonrpc_close_fn fn,
			   void *data)
{
	if (!conn)
		return;
	conn->on_close = fn;
	conn->on_close_data = data;
}

/*
 * Hand the frame that holds @len bytes at @data to the frame handler. The
 * image is copied to memory aligned for it, and the load checks each pointer,
 * so a peer cannot make a bad word of a frame be read.
 */
static void jsonrpc_conn_image_frame(struct jsonrpc_conn *conn,
				     const void *data, size_t len)
{
	fy_generic doc = fy_invalid;
	void *buf;
	int rc;

	if (!conn->frame) {
		fyai_error(jsonrpc_conn_ctx(conn), "%s got an image frame with no handler",
			   conn->name);
		conn->protocol_failed = true;
		return;
	}
	fy_align_free(conn->image_rx);
	conn->image_rx = NULL;
	buf = fy_align_alloc(FY_GENERIC_IMAGE_ALIGN,
			     FY_ALIGN((size_t)FY_GENERIC_IMAGE_ALIGN, len));
	fyai_error_check(jsonrpc_conn_ctx(conn), buf, err, "%s cannot keep a frame",
			 conn->name);
	conn->image_rx = buf;
	memcpy(buf, data, len);
	rc = fy_generic_image_load(buf, len, &doc);
	if (rc || !fy_is_mapping(doc)) {
		fyai_error(jsonrpc_conn_ctx(conn), "%s received an image that is not a frame",
			   conn->name);
		conn->protocol_failed = true;
		return;
	}
	conn->frame(conn, doc, conn->frame_userdata);
	return;
err:
	conn->protocol_failed = true;
}

/* Take each whole frame out of the receive buffer; 0, or -1 on a bad length. */
static int jsonrpc_conn_image_frames(struct jsonrpc_conn *conn)
{
	uint64_t len;
	size_t used = 0;

	while (conn->image && conn->rx.len - used >= JSONRPC_IMAGE_LEN) {
		memcpy(&len, conn->rx.data + used, sizeof(len));
		if (len > JSONRPC_FRAME_MAX || !len) {
			fyai_error(jsonrpc_conn_ctx(conn), "%s sent an image of %llu bytes",
				   conn->name, (unsigned long long)len);
			return -1;
		}
		if (conn->rx.len - used - JSONRPC_IMAGE_LEN < len)
			break;
		jsonrpc_conn_image_frame(conn, conn->rx.data + used +
					 JSONRPC_IMAGE_LEN, (size_t)len);
		used += JSONRPC_IMAGE_LEN + (size_t)len;
		if (conn->protocol_failed)
			break;
	}
	conn->rx.len -= used;
	memmove(conn->rx.data, conn->rx.data + used, conn->rx.len);
	conn->rx.data[conn->rx.len] = '\0';
	return 0;
}

static enum fyai_event_action jsonrpc_conn_readable(const struct fyai_event *ev)
{
	struct jsonrpc_conn *conn = ev->userdata;
	char chunk[65536];
	size_t want;
	char *newline;
	size_t consumed, remaining;
	ssize_t n;
	int rc;

	/*
	 * The size of one read, not of a frame: a frame accumulates in the
	 * receive buffer up to JSONRPC_FRAME_MAX. A line is read in small parts
	 * so that other sources get a turn.
	 */
	want = conn->image ? sizeof(chunk) : 4096;
	do {
		n = read(ev->fd, chunk, want);
	} while (n < 0 && errno == EINTR);
	if (n > 0) {
		/* Reject an oversized frame before growing the receive buffer. */
		if (conn->rx.len + (size_t)n + 1 > JSONRPC_FRAME_MAX) {
			fyai_error(jsonrpc_conn_ctx(conn),
				   "%s frame exceeds %lu bytes", conn->name,
				   (unsigned long)JSONRPC_FRAME_MAX);
			jsonrpc_drop_source(&conn->read_src);
			jsonrpc_conn_fail_pending(conn, "overflowed");
			return FYAIEA_CONTINUE;
		}
		rc = fyai_buf_reserve(&conn->rx,
					     conn->rx.len + (size_t)n + 1);
		if (rc) {
			fyai_error(jsonrpc_conn_ctx(conn), "%s frame is too large", conn->name);
			jsonrpc_conn_fail_pending(conn, "overflowed");
			return FYAIEA_CONTINUE;
		}
		memcpy(conn->rx.data + conn->rx.len, chunk, (size_t)n);
		conn->rx.len += (size_t)n;
		conn->rx.data[conn->rx.len] = '\0';
		/* The frame that switches to images ends the lines. */
		while (!conn->image &&
		       (newline = memchr(conn->rx.data, '\n', conn->rx.len))) {
			*newline = '\0';
			consumed = (size_t)(newline - conn->rx.data) + 1;
			jsonrpc_conn_line(conn, conn->rx.data);
			remaining = conn->rx.len - consumed;
			memmove(conn->rx.data, conn->rx.data + consumed,
				remaining);
			conn->rx.len = remaining;
			conn->rx.data[remaining] = '\0';
		}
		if (conn->image && jsonrpc_conn_image_frames(conn))
			conn->protocol_failed = true;
		if (conn->protocol_failed) {
			jsonrpc_drop_source(&conn->read_src);
			jsonrpc_conn_fail_pending(conn,
						  "sent an invalid frame");
			return FYAIEA_CONTINUE;
		}
		/* Leave further bytes readable so other ready sources get a turn. */
		return FYAIEA_CONTINUE;
	}
	if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
		return FYAIEA_CONTINUE;

	jsonrpc_drop_source(&conn->read_src);
	jsonrpc_conn_fail_pending(conn, "stdio peer closed");
	return FYAIEA_CONTINUE;
}

/*
 * Deferred close handler when write fails with EPIPE before stdin is drained.
 * Re-defers (up to 256 iterations) while POLLIN indicates unread peer frames,
 * then fails pending requests.
 */
static void jsonrpc_conn_late_close(void *userdata)
{
	struct jsonrpc_conn *conn = userdata;
	struct pollfd pfd = { .fd = conn->stdin_fd, .events = POLLIN };

	if (conn->closed)
		return;
	if (conn->read_src && conn->stdin_fd >= 0 && ++conn->late_close_tries < 256 &&
	    poll(&pfd, 1, 0) > 0 &&
	    fyai_event_defer(fyai_ctx_loop(jsonrpc_conn_ctx(conn)),
			     jsonrpc_conn_late_close, conn) == 0)
		return;
	jsonrpc_conn_fail_pending(conn, "stdio peer closed");
}

static enum fyai_event_action jsonrpc_conn_writable(const struct fyai_event *ev)
{
	struct jsonrpc_conn *conn = ev->userdata;
	struct jsonrpc_request *req;
	bool peer_closed;
	ssize_t n;

	while (conn->tx_off < conn->tx.len) {
		n = write(ev->fd, conn->tx.data + conn->tx_off,
			  conn->tx.len - conn->tx_off);
		if (n > 0) {
			conn->tx_off += (size_t)n;
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return FYAIEA_CONTINUE;
		/*
		 * A peer that closed its end is gone, as the end of file of its
		 * output says. That is no error of this write: raised first, it
		 * would hide the cause that ended the peer.
		 */
		peer_closed = n < 0 && errno == EPIPE;
		conn->write_failed = true;
		if (!peer_closed)
			fyai_error(jsonrpc_conn_ctx(conn), "%s stdio write failed", conn->name);
		jsonrpc_drop_source(&conn->write_src);
		if (peer_closed && conn->read_src && !conn->closed &&
		    !fyai_event_defer(fyai_ctx_loop(jsonrpc_conn_ctx(conn)),
				      jsonrpc_conn_late_close, conn))
			return FYAIEA_CONTINUE;
		jsonrpc_conn_fail_pending(conn, peer_closed ? "stdio peer closed" :
					  "could not be written");
		return FYAIEA_CONTINUE;
	}
	conn->tx.len = 0;
	conn->tx_off = 0;
	jsonrpc_drop_source(&conn->write_src);

	/* Complete notifications after the write queue is empty. */
	while ((req = conn->flush_wait)) {
		conn->flush_wait = req->next;
		req->next = NULL;
		jsonrpc_finish(req, true, fy_null, 0, CURLE_OK);
	}
	return FYAIEA_CONTINUE;
}

static int jsonrpc_conn_arm_write(struct jsonrpc_conn *conn)
{
	struct fyai_event_loop *el;

	if (conn->write_src || conn->tx_off >= conn->tx.len)
		return 0;
	el = fyai_ctx_loop(jsonrpc_conn_ctx(conn));
	if (!el)
		return -1;
	return fyai_event_add_fd(el, conn->stdin_fd, FYAIEV_WRITE,
				 jsonrpc_conn_writable, conn,
				 &conn->write_src);
}

static int jsonrpc_conn_arm_read(struct jsonrpc_conn *conn)
{
	struct fyai_event_loop *el;
	int flags;

	if (conn->read_src)
		return 0;
	el = fyai_ctx_loop(jsonrpc_conn_ctx(conn));
	if (!el)
		return -1;
	flags = fcntl(conn->stdout_fd, F_GETFL, 0);
	if (flags >= 0)
		(void)fcntl(conn->stdout_fd, F_SETFL, flags | O_NONBLOCK);
	return fyai_event_add_fd(el, conn->stdout_fd, FYAIEV_READ,
				 jsonrpc_conn_readable, conn, &conn->read_src);
}

bool jsonrpc_conn_closed(const struct jsonrpc_conn *conn)
{
	return !conn || conn->closed;
}

bool jsonrpc_conn_has_output(const struct jsonrpc_conn *conn)
{
	return conn && conn->transport == JSONRPC_STDIO &&
	       !conn->write_failed && conn->tx_off < conn->tx.len;
}

size_t jsonrpc_conn_output_size(const struct jsonrpc_conn *conn)
{
	return jsonrpc_conn_has_output(conn) ? conn->tx.len - conn->tx_off : 0;
}

void jsonrpc_conn_defer(struct jsonrpc_conn *conn)
{
	if (conn)
		conn->serve_deferred = true;
}

int jsonrpc_conn_respond(struct jsonrpc_conn *conn, fy_generic id,
			 fy_generic result, fy_generic error)
{
	if (!conn || (conn->transport != JSONRPC_STDIO &&
		      conn->transport != JSONRPC_HTTP))
		return -1;
	return jsonrpc_conn_reply(conn, id, result, error);
}

int jsonrpc_conn_serve(struct jsonrpc_conn *conn, jsonrpc_serve_fn fn,
		       void *userdata)
{
	if (!conn || (conn->transport != JSONRPC_STDIO &&
		      conn->transport != JSONRPC_HTTP))
		return -1;
	conn->serve = fn;
	conn->serve_userdata = userdata;
	/* An HTTP server speaks in the answers, and on a stream when asked. */
	if (conn->transport == JSONRPC_HTTP)
		return 0;
	/* A server must listen before anyone calls it. */
	return fn ? jsonrpc_conn_arm_read(conn) : 0;
}

int jsonrpc_conn_frames(struct jsonrpc_conn *conn, jsonrpc_frame_fn fn,
			void *userdata)
{
	if (!conn || conn->transport != JSONRPC_STDIO || !fn)
		return -1;
	conn->frame = fn;
	conn->frame_userdata = userdata;
	return jsonrpc_conn_arm_read(conn);
}

bool jsonrpc_image_available(void)
{
	return !getenv("FYAI_REMOTE_NO_IMAGE");
}

const char *jsonrpc_image_feature(void)
{
	if (!jsonrpc_image_available())
		return NULL;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	return sizeof(void *) == 8 ? "image-be64" : "image-be32";
#else
	return sizeof(void *) == 8 ? "image-le64" : "image-le32";
#endif
}

int jsonrpc_conn_set_image(struct jsonrpc_conn *conn)
{
	if (!conn || conn->transport != JSONRPC_STDIO ||
	    !jsonrpc_image_available())
		return -1;
	conn->image = true;
	return 0;
}

bool jsonrpc_conn_image(const struct jsonrpc_conn *conn)
{
	return conn && conn->image;
}

void *jsonrpc_image_encode(struct jsonrpc_conn *conn, fy_generic frame,
			   size_t *sizep)
{
	size_t hint = conn && conn->image_hint ? conn->image_hint : 16384;
	void *image;

	image = fy_generic_image_create_with_hint(frame, hint, sizep);
	if (image && conn)
		conn->image_hint = *sizep + 4096;
	return image;
}

void jsonrpc_image_free(void *image)
{
	fy_generic_image_destroy(image);
}

int jsonrpc_conn_send_image(struct jsonrpc_conn *conn, const void *image,
			    size_t size)
{
	uint64_t len = size;
	int rc;

	if (!conn || !conn->image || conn->write_failed)
		return -1;
	rc = fyai_buf_reserve(&conn->tx, conn->tx.len + sizeof(len) +
				     size + 1);
	fyai_error_check(jsonrpc_conn_ctx(conn), !rc, err, "%s could not queue a frame",
			 conn->name);
	memcpy(conn->tx.data + conn->tx.len, &len, sizeof(len));
	memcpy(conn->tx.data + conn->tx.len + sizeof(len), image, size);
	conn->tx.len += sizeof(len) + size;
	rc = jsonrpc_conn_arm_write(conn);
	fyai_error_check(jsonrpc_conn_ctx(conn), !rc, err, "%s could not queue a frame",
			 conn->name);
	return 0;
err:
	return -1;
}

int jsonrpc_conn_send_frame(struct jsonrpc_conn *conn, fy_generic frame)
{
	struct fy_generic_builder *gb;
	const char *body;
	void *image;
	size_t size;
	int rc;

	if (conn && conn->image) {
		image = jsonrpc_image_encode(conn, frame, &size);
		fyai_error_check(jsonrpc_conn_ctx(conn), image, err,
				 "%s could not encode a frame", conn->name);
		rc = jsonrpc_conn_send_image(conn, image, size);
		jsonrpc_image_free(image);
		return rc;
	}
	/* A peer that closed its output can still read: only a failed write
	 * ends the frames that go to it. */
	if (!conn || conn->transport != JSONRPC_STDIO || conn->write_failed)
		return -1;
	if (!conn->frame_gb)
		conn->frame_gb = fy_generic_builder_create(NULL);
	else
		fy_generic_builder_reset(conn->frame_gb);
	gb = conn->frame_gb;
	body = gb ? emit_json_string(gb, frame) : NULL;
	fyai_error_check(jsonrpc_conn_ctx(conn), body, err,
			 "%s could not encode a frame", conn->name);
	rc = jsonrpc_conn_send(conn, body);
	fyai_error_check(jsonrpc_conn_ctx(conn), !rc, err,
			 "%s could not queue a frame", conn->name);
	return 0;
err:
	return -1;
}

const char *jsonrpc_frame_encode(struct fy_generic_builder *gb, fy_generic frame)
{
	return emit_json_string(gb, frame);
}

int jsonrpc_conn_send_encoded(struct jsonrpc_conn *conn, const char *line)
{
	int rc;

	if (!conn || conn->transport != JSONRPC_STDIO || conn->write_failed)
		return -1;
	rc = jsonrpc_conn_send(conn, line);
	fyai_error_check(jsonrpc_conn_ctx(conn), !rc, err,
			 "%s could not queue a frame", conn->name);
	return 0;
err:
	return -1;
}

static enum fyai_event_action jsonrpc_stdio_timeout(const struct fyai_event *ev)
{
	struct jsonrpc_request *req = ev->userdata;

	req->timer_src = NULL;
	fyai_error(jsonrpc_conn_ctx(req->conn), "%s %s stdio response timed out",
		   req->conn->name, req->method);
	jsonrpc_finish(req, false, fy_invalid, 0, CURLE_OK);
	return FYAIEA_CONTINUE;
}

static int jsonrpc_stdio_submit(struct jsonrpc_request *req, fy_generic params)
{
	struct jsonrpc_conn *conn = req->conn;
	struct fyai_ctx *ctx = jsonrpc_conn_ctx(conn);
	struct fy_generic_builder *gb;
	struct fyai_event_loop *el;
	fy_generic rpc;
	const char *body;
	int flags, rc;

	el = fyai_ctx_loop(ctx);
	fyai_error_check(ctx, el, err_out,
			 "could not acquire the application event loop");
	gb = fyai_ctx_transient_gb(ctx);
	fyai_error_check(ctx, gb, err_out,
			 "could not acquire transient storage");
	rpc = fy_mapping(gb, "jsonrpc", "2.0",
			 "method", req->method, "params", params);
	if (!req->notification)
		rpc = fy_assoc(gb, rpc, "id", req->id);
	body = emit_request_body(gb, rpc);
	fyai_error_check(ctx, body, err_out,
			 "could not encode JSON-RPC stdio request");

	flags = fcntl(conn->stdin_fd, F_GETFL);
	fyai_error_check(ctx, flags >= 0, err_out,
			 "could not inspect stdin: %s", strerror(errno));
	rc = fcntl(conn->stdin_fd, F_SETFL, flags | O_NONBLOCK);
	fyai_error_check(ctx, !rc, err_out,
			 "could not make stdin non-blocking: %s",
			 strerror(errno));

	/* Register the request before the peer can reply. */
	if (req->notification) {
		req->next = conn->flush_wait;
		conn->flush_wait = req;
	} else {
		req->next = conn->pending;
		conn->pending = req;
	}

	rc = jsonrpc_conn_send(conn, body);
	fyai_error_check(ctx, !rc, err_dequeue, "could not queue the request");
	if (req->notification)
		return 0;

	rc = jsonrpc_conn_arm_read(conn);
	fyai_error_check(ctx, !rc, err_dequeue, "could not watch stdout");
	/* A nonpositive timeout disables the timer. */
	if (conn->timeout_s > 0) {
		rc = fyai_event_add_timer(el, conn->timeout_s * 1000, 0,
					  jsonrpc_stdio_timeout, req,
					  &req->timer_src);
		fyai_error_check(ctx, !rc, err_dequeue,
				 "could not arm response timeout");
	}
	return 0;

err_dequeue:
	jsonrpc_list_remove(&conn->pending, req);
	jsonrpc_list_remove(&conn->flush_wait, req);
err_out:
	jsonrpc_stdio_sources_drop(req);
	return -1;
}

/*
 * HTTP transport
 */

static size_t jsonrpc_http_header(void *ptr, size_t size, size_t nmemb,
				  void *userdata)
{
	struct jsonrpc_request *req = userdata;
	struct curl_slist *headers;
	const char *data = ptr;
	char *line;
	size_t raw_len = size * nmemb;
	size_t len = raw_len;

	while (len && (data[len - 1] == '\r' || data[len - 1] == '\n'))
		len--;
	line = len ? strndup(data, len) : NULL;
	if (line) {
		if (!strncasecmp(line, "HTTP/", 5)) {
			curl_slist_free_all(req->response_headers);
			req->response_headers = NULL;
		}
		headers = curl_slist_append(req->response_headers, line);
		free(line);
		if (headers)
			req->response_headers = headers;
	}
	if (req->conn->has_hooks && req->conn->hooks.response_header)
		req->conn->hooks.response_header(req->conn->hooks.userdata,
						 ptr, raw_len);
	return raw_len;
}

typedef void (*jsonrpc_sse_fn)(void *userdata, const char *data);

/* One line of the stream: a field, or the empty line that ends an event. */
static void jsonrpc_sse_line(struct jsonrpc_sse *s, char *line,
			     jsonrpc_sse_fn fn, void *userdata)
{
	char *value, *end;
	long ms;

	if (!*line) {
		if (s->has_data && !s->failed) {
			/* The data ends with the newline of its last line. */
			if (s->data.len)
				s->data.data[--s->data.len] = '\0';
			fn(userdata, s->data.data ? s->data.data : "");
		}
		s->data.len = 0;
		s->has_data = false;
		return;
	}
	if (*line == ':')
		return;				/* a comment, which keeps it open */
	value = strchr(line, ':');
	if (value) {
		*value++ = '\0';
		if (*value == ' ')
			value++;
	} else {
		value = line + strlen(line);
	}
	if (!strcmp(line, "data")) {
		if (s->data.len + strlen(value) + 1 > JSONRPC_FRAME_MAX ||
		    fyai_buf_append(&s->data, value) ||
		    fyai_buf_append(&s->data, "\n")) {
			s->failed = true;
			return;
		}
		s->has_data = true;
	} else if (!strcmp(line, "id")) {
		free(s->last_id);
		s->last_id = strdup(value);
	} else if (!strcmp(line, "retry")) {
		ms = strtol(value, &end, 10);
		if (*value && !*end && ms >= 0)
			s->retry_ms = ms;
	}
}

/* Take @len bytes of the stream; each event that ends goes to @fn. */
static void jsonrpc_sse_feed(struct jsonrpc_sse *s, const char *p, size_t len,
			     jsonrpc_sse_fn fn, void *userdata)
{
	const char *nl;
	size_t n;

	while (len && !s->failed) {
		nl = memchr(p, '\n', len);
		n = nl ? (size_t)(nl - p) : len;
		if (s->line.len + n + 1 > JSONRPC_FRAME_MAX ||
		    fyai_buf_append_data(&s->line, p, n)) {
			s->failed = true;
			return;
		}
		if (!nl)
			return;
		if (s->line.len && s->line.data[s->line.len - 1] == '\r')
			s->line.len--;
		s->line.data[s->line.len] = '\0';
		jsonrpc_sse_line(s, s->line.data, fn, userdata);
		s->line.len = 0;
		p = nl + 1;
		len -= n + 1;
	}
}

/* Discard any incomplete event when the listening stream closes. */
static void jsonrpc_sse_reset(struct jsonrpc_sse *s)
{
	s->line.len = 0;
	s->data.len = 0;
	s->has_data = false;
	s->failed = false;
}

static void jsonrpc_sse_free(struct jsonrpc_sse *s)
{
	fyai_buf_free(&s->line);
	fyai_buf_free(&s->data);
	free(s->last_id);
	s->last_id = NULL;
}

/* The messages of an answer, gathered in order. */
struct jsonrpc_sse_collect {
	struct fy_generic_builder *gb;
	fy_generic docs;
};

static void jsonrpc_sse_collect_one(void *userdata, const char *data)
{
	struct jsonrpc_sse_collect *c = userdata;
	fy_generic doc;

	doc = parse_json_string(c->gb, data);
	c->docs = fy_append(c->gb, c->docs, doc);
}

/* True when the body of an answer is a stream of events, not JSON. */
static bool jsonrpc_http_is_sse(struct jsonrpc_request *req, const char *body)
{
	const char *type;

	type = jsonrpc_request_response_header(req, "Content-Type");
	if (type)
		return !strncasecmp(type, "text/event-stream", 17);
	return !strncmp(body, "event:", 6) || !strncmp(body, "data:", 5) ||
	       !strncmp(body, "id:", 3) || *body == ':';
}

/*
 * The answer of a request that came as a stream of events: the messages that
 * the server sent before it are served in order, as on stdio, and the
 * message with the id of the request is the answer.
 */
static fy_generic jsonrpc_http_sse_answer(struct jsonrpc_request *req,
					  struct fy_generic_builder *gb,
					  const char *body)
{
	struct jsonrpc_conn *conn = req->conn;
	struct jsonrpc_sse s = {0};
	struct jsonrpc_sse_collect c = { .gb = gb, .docs = fy_seq_empty };
	fy_generic doc, answer = fy_invalid;

	jsonrpc_sse_feed(&s, body, strlen(body), jsonrpc_sse_collect_one, &c);
	/* An answer that ends without its empty line still ends its event. */
	jsonrpc_sse_feed(&s, "\n\n", 2, jsonrpc_sse_collect_one, &c);
	jsonrpc_sse_free(&s);
	fy_foreach(doc, c.docs) {
		if (!fy_is_mapping(doc)) {
			answer = doc;
			break;
		}
		if (fy_is_valid(fy_get(doc, "method", fy_invalid))) {
			if (!conn->closed)
				jsonrpc_conn_frame(conn, doc);
			continue;
		}
		if (fy_get(doc, "id", -1LL) == req->id) {
			answer = doc;
			break;
		}
	}
	return answer;
}

/* Parse the JSON-RPC result out of an HTTP response body (JSON or SSE). */
static fy_generic jsonrpc_http_result(struct jsonrpc_request *req)
{
	struct fyai_ctx *ctx = jsonrpc_conn_ctx(req->conn);
	struct fy_generic_builder *gb;
	fy_generic doc, error, out;
	const char *json;

	if (req->code != CURLE_OK || req->status < 200 || req->status >= 300)
		return fy_invalid;
	if (req->notification)
		return fy_null;
	gb = fyai_ctx_transient_gb(ctx);
	fyai_error_check(ctx, gb, err_out, "%s %s could not acquire "
			 "transient storage", req->conn->name, req->method);
	json = req->response.data ? req->response.data : "";
	doc = jsonrpc_http_is_sse(req, json) ?
		jsonrpc_http_sse_answer(req, gb, json) :
		parse_json_string(gb, json);
	if (!fy_is_valid(doc)) {
		parse_diag_report(ctx, doc, "malformed JSON",
			fy_sprintfa("%s %s returned invalid JSON",
				    req->conn->name, req->method));
		goto err_out;
	}
	error = fy_get(doc, "error");
	fyai_error_check(ctx, fy_is_invalid(error), err_out,
			 "%s %s: %s", req->conn->name, req->method,
			 fy_get(error, "message", "server error"));
	out = fy_get(doc, "result", fy_invalid);
	fyai_error_check(ctx, fy_is_valid(out), err_out,
			 "%s %s response has no result", req->conn->name,
			 req->method);
	return out;

err_out:
	return fy_invalid;
}

static void jsonrpc_http_complete(struct fyai_curl_transfer *transfer,
				  void *userdata)
{
	struct jsonrpc_request *req = userdata;
	fy_generic result;

	req->code = fyai_curl_collect(transfer);
	curl_easy_getinfo(req->curl, CURLINFO_RESPONSE_CODE, &req->status);
	fyai_curl_transfer_destroy(transfer);
	req->transfer = NULL;
	if (req->cancel_requested) {
		jsonrpc_finish(req, false, fy_invalid, req->status, req->code);
		return;
	}
	result = jsonrpc_http_result(req);
	jsonrpc_finish(req, fy_is_valid(result), result, req->status,
		       req->code);
}

/* POST the frame @rpc for @req. */
static int jsonrpc_http_send(struct jsonrpc_request *req, fy_generic rpc)
{
	struct jsonrpc_conn *conn = req->conn;
	struct fyai_ctx *ctx = jsonrpc_conn_ctx(conn);
	struct fy_generic_builder *gb;
	const char *body;
	int rc;

	gb = fyai_ctx_transient_gb(ctx);
	fyai_error_check(ctx, gb, err_out, "%s %s could not acquire "
			 "transient storage", conn->name, req->method);
	body = emit_request_body(gb, rpc);
	fyai_error_check(ctx, body, err_out, "%s %s could not encode request",
			 conn->name, req->method);
	req->body = strdup(body);
	fyai_error_check(ctx, req->body, err_out, "out of memory");
	rc = append_header(&req->headers, "Content-Type: application/json");
	if (!rc)
		rc = append_header(&req->headers,
				   "Accept: application/json, text/event-stream");
	fyai_error_check(ctx, !rc, err_out,
			 "%s %s could not create request headers", conn->name,
			 req->method);
	if (conn->has_hooks && conn->hooks.add_headers) {
		rc = conn->hooks.add_headers(conn->hooks.userdata, ctx,
					     &req->headers);
		fyai_error_check(ctx, !rc, err_out,
				 "%s %s could not add request headers",
				 conn->name, req->method);
	}
	req->curl = curl_easy_init();
	/* Reserve SIGALRM for the watchdog. */
	curl_easy_setopt(req->curl, CURLOPT_NOSIGNAL, 1L);

	fyai_error_check(ctx, req->curl, err_out,
			 "%s %s could not create curl handle", conn->name,
			 req->method);
	curl_easy_setopt(req->curl, CURLOPT_URL, conn->endpoint);
	curl_easy_setopt(req->curl, CURLOPT_HTTPHEADER, req->headers);
	curl_easy_setopt(req->curl, CURLOPT_POSTFIELDS, req->body);
	curl_easy_setopt(req->curl, CURLOPT_WRITEFUNCTION, write_response);
	curl_easy_setopt(req->curl, CURLOPT_WRITEDATA, &req->response);
	curl_easy_setopt(req->curl, CURLOPT_HEADERFUNCTION, jsonrpc_http_header);
	curl_easy_setopt(req->curl, CURLOPT_HEADERDATA, req);
	curl_easy_setopt(req->curl, CURLOPT_TIMEOUT, (long)conn->timeout_s);
	curl_easy_setopt(req->curl, CURLOPT_USERAGENT, ctx->link->user_agent);
	req->transfer = fyai_curl_submit(ctx->link, req->curl, jsonrpc_http_complete,
					 req);
	fyai_error_check(ctx, req->transfer, err_out,
			 "%s %s could not submit request", conn->name,
			 req->method);
	return 0;

err_out:
	return -1;
}

static int jsonrpc_http_submit(struct jsonrpc_request *req, fy_generic params)
{
	struct jsonrpc_conn *conn = req->conn;
	struct fyai_ctx *ctx = jsonrpc_conn_ctx(conn);
	struct fy_generic_builder *gb;
	fy_generic rpc;

	gb = fyai_ctx_transient_gb(ctx);
	fyai_error_check(ctx, gb, err_out, "%s %s could not acquire "
			 "transient storage", conn->name, req->method);
	rpc = fy_mapping(gb, "jsonrpc", "2.0",
			 "method", req->method, "params", params);
	if (!req->notification)
		rpc = fy_assoc(gb, rpc, "id", req->id);
	return jsonrpc_http_send(req, rpc);

err_out:
	return -1;
}

static void jsonrpc_notify_done(struct jsonrpc_request *req, void *userdata);

/* POST @frame, an answer to the server, as a notification is posted. */
static int jsonrpc_http_post(struct jsonrpc_conn *conn, fy_generic frame)
{
	struct jsonrpc_request *req;

	req = calloc(1, sizeof(*req));
	fyai_error_check(jsonrpc_conn_ctx(conn), req, err, "out of memory");
	req->conn = conn;
	req->method = "response";
	req->notification = true;
	req->complete = jsonrpc_notify_done;
	req->result = fy_invalid;
	req->code = CURLE_OK;
	clock_gettime(CLOCK_MONOTONIC, &req->started);
	if (jsonrpc_http_send(req, frame)) {
		jsonrpc_request_free(req);
		goto err;
	}
	return 0;
err:
	return -1;
}

/*
 * The stream of an HTTP server: a GET that the server keeps open and sends
 * its messages on, as server-sent events. Each message is served from the
 * loop, not from the callback of curl, which takes no new transfer. A stream
 * that ends opens again, with the id of the last event it delivered.
 */
static int jsonrpc_stream_open(struct jsonrpc_conn *conn);

static void jsonrpc_stream_drain(void *userdata)
{
	struct jsonrpc_conn *conn = userdata;
	struct jsonrpc_sse_msg *m;
	struct fy_generic_builder *gb;
	fy_generic doc;

	while ((m = conn->inbox) && !conn->closed) {
		conn->inbox = m->next;
		if (!conn->inbox)
			conn->inbox_tail = &conn->inbox;
		gb = fyai_ctx_transient_gb(jsonrpc_conn_ctx(conn));
		doc = gb ? parse_json_string(gb, m->json) : fy_invalid;
		if (fy_is_mapping(doc))
			jsonrpc_conn_frame(conn, doc);
		else
			fyai_warning(jsonrpc_conn_ctx(conn),
				     "%s sent a message that is not JSON on "
				     "its stream: %.60s", conn->name, m->json);
		free(m->json);
		free(m);
	}
}

static void jsonrpc_inbox_free(struct jsonrpc_conn *conn)
{
	struct jsonrpc_sse_msg *m;
	struct fyai_event_loop *el;

	el = fyai_ctx_loop(jsonrpc_conn_ctx(conn));
	if (el)
		fyai_event_defer_cancel(el, jsonrpc_stream_drain, conn);
	while ((m = conn->inbox)) {
		conn->inbox = m->next;
		free(m->json);
		free(m);
	}
	conn->inbox_tail = &conn->inbox;
}

static void jsonrpc_stream_event(void *userdata, const char *data)
{
	struct jsonrpc_conn *conn = userdata;
	struct jsonrpc_sse_msg *m;
	bool armed = !!conn->inbox;

	if (!*data)
		return;
	m = calloc(1, sizeof(*m));
	if (m)
		m->json = strdup(data);
	if (!m || !m->json) {
		free(m);
		conn->sse.failed = true;
		return;
	}
	if (!conn->inbox_tail)
		conn->inbox_tail = &conn->inbox;
	*conn->inbox_tail = m;
	conn->inbox_tail = &m->next;
	conn->stream_delivered = true;
	if (!armed)
		(void)fyai_event_defer(fyai_ctx_loop(jsonrpc_conn_ctx(conn)),
				       jsonrpc_stream_drain, conn);
}

static size_t jsonrpc_stream_write(char *ptr, size_t size, size_t nmemb,
				   void *userdata)
{
	struct jsonrpc_request *req = userdata;
	struct jsonrpc_conn *conn = req->conn;
	size_t len = size * nmemb;
	long status = 0;

	curl_easy_getinfo(req->curl, CURLINFO_RESPONSE_CODE, &status);
	/* An error keeps a little of its body, for the report. */
	if (status < 200 || status >= 300) {
		if (req->response.len < 1024)
			(void)fyai_buf_append_data(&req->response, ptr,
				len < 1024 - req->response.len ?
				len : 1024 - req->response.len);
		return len;
	}
	jsonrpc_sse_feed(&conn->sse, ptr, len, jsonrpc_stream_event, conn);
	/* A stream that cannot be read ends, and opens again. */
	return conn->sse.failed ? 0 : len;
}

static enum fyai_event_action jsonrpc_stream_retry(const struct fyai_event *ev)
{
	struct jsonrpc_conn *conn = ev->userdata;

	conn->stream_timer = NULL;
	if (conn->listening && !conn->closed && !conn->stream &&
	    jsonrpc_stream_open(conn))
		fyai_warning(jsonrpc_conn_ctx(conn),
			     "%s could not open its stream again", conn->name);
	return FYAIEA_CONTINUE;
}

/* Open the stream again after @ms. */
static void jsonrpc_stream_schedule(struct jsonrpc_conn *conn, long ms)
{
	struct fyai_event_loop *el = fyai_ctx_loop(jsonrpc_conn_ctx(conn));

	if (conn->stream_timer || !el)
		return;
	if (fyai_event_add_timer(el, ms > 0 ? ms : 1, 0, jsonrpc_stream_retry,
				 conn, &conn->stream_timer))
		fyai_warning(jsonrpc_conn_ctx(conn),
			     "%s cannot keep its stream open", conn->name);
}

static void jsonrpc_stream_complete(struct fyai_curl_transfer *transfer,
				    void *userdata)
{
	struct jsonrpc_request *req = userdata;
	struct jsonrpc_conn *conn = req->conn;
	struct fyai_ctx *ctx = jsonrpc_conn_ctx(conn);
	jsonrpc_refused_fn refused;
	CURLcode code;
	long status = 0, ms;
	bool delivered;

	code = fyai_curl_collect(transfer);
	curl_easy_getinfo(req->curl, CURLINFO_RESPONSE_CODE, &status);
	fyai_curl_transfer_destroy(transfer);
	req->transfer = NULL;
	conn->stream = NULL;
	delivered = conn->stream_delivered;
	conn->stream_delivered = false;
	jsonrpc_sse_reset(&conn->sse);
	if (!conn->listening || conn->closed) {
		jsonrpc_request_free(req);
		return;
	}
	/*
	 * A server that has no stream, a session that it ended, or a refused
	 * credential: opening it again would not help.
	 */
	if (status == 405 || status == 404 || status == 401 ||
	    status == 403) {
		fyai_debug(ctx, "%s refused its stream: HTTP %ld %.200s",
			   conn->name, status,
			   req->response.data ? req->response.data : "");
		jsonrpc_request_free(req);
		conn->listening = false;
		refused = conn->refused;
		conn->refused = NULL;
		if (refused)
			refused(conn->refused_data, status);
		return;
	}
	fyai_debug(ctx, "%s stream ended (curl %d, HTTP %ld)", conn->name,
		   (int)code, status);
	jsonrpc_request_free(req);
	if (delivered)
		conn->stream_retry_ms = 0;
	if (conn->sse.retry_ms > 0)
		ms = conn->sse.retry_ms;
	else if (delivered)
		ms = JSONRPC_STREAM_REOPEN_MS;
	else {
		ms = conn->stream_retry_ms ? conn->stream_retry_ms * 2 :
		     JSONRPC_STREAM_RETRY_MS;
		if (ms > JSONRPC_STREAM_RETRY_MAX_MS)
			ms = JSONRPC_STREAM_RETRY_MAX_MS;
		conn->stream_retry_ms = ms;
	}
	jsonrpc_stream_schedule(conn, ms);
}

static int jsonrpc_stream_open(struct jsonrpc_conn *conn)
{
	struct fyai_ctx *ctx = jsonrpc_conn_ctx(conn);
	struct jsonrpc_request *req;
	char *last = NULL;
	int rc;

	req = calloc(1, sizeof(*req));
	fyai_error_check(ctx, req, err, "out of memory");
	req->conn = conn;
	req->method = "stream";
	req->result = fy_invalid;
	req->code = CURLE_OK;
	clock_gettime(CLOCK_MONOTONIC, &req->started);
	rc = append_header(&req->headers, "Accept: text/event-stream");
	if (!rc && conn->sse.last_id && *conn->sse.last_id) {
		last = fy_sprintfa("Last-Event-ID: %s", conn->sse.last_id);
		rc = append_header(&req->headers, last);
	}
	if (!rc && conn->has_hooks && conn->hooks.add_headers)
		rc = conn->hooks.add_headers(conn->hooks.userdata, ctx,
					     &req->headers);
	fyai_error_check(ctx, !rc, err_free,
			 "%s could not make the headers of its stream",
			 conn->name);
	req->curl = curl_easy_init();
	fyai_error_check(ctx, req->curl, err_free,
			 "%s could not create curl handle", conn->name);
	curl_easy_setopt(req->curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(req->curl, CURLOPT_URL, conn->endpoint);
	curl_easy_setopt(req->curl, CURLOPT_HTTPGET, 1L);
	curl_easy_setopt(req->curl, CURLOPT_HTTPHEADER, req->headers);
	curl_easy_setopt(req->curl, CURLOPT_WRITEFUNCTION, jsonrpc_stream_write);
	curl_easy_setopt(req->curl, CURLOPT_WRITEDATA, req);
	curl_easy_setopt(req->curl, CURLOPT_HEADERFUNCTION, jsonrpc_http_header);
	curl_easy_setopt(req->curl, CURLOPT_HEADERDATA, req);
	/* The stream has no end to wait for; a dead peer is found by TCP. */
	if (conn->timeout_s > 0)
		curl_easy_setopt(req->curl, CURLOPT_CONNECTTIMEOUT,
				 (long)conn->timeout_s);
	curl_easy_setopt(req->curl, CURLOPT_TCP_KEEPALIVE, 1L);
	curl_easy_setopt(req->curl, CURLOPT_USERAGENT, ctx->link->user_agent);
	req->transfer = fyai_curl_submit(ctx->link, req->curl,
					 jsonrpc_stream_complete, req);
	fyai_error_check(ctx, req->transfer, err_free,
			 "%s could not open its stream", conn->name);
	conn->stream = req;
	return 0;

err_free:
	jsonrpc_request_free(req);
err:
	return -1;
}

int jsonrpc_conn_listen(struct jsonrpc_conn *conn, jsonrpc_refused_fn refused,
			void *data)
{
	if (!conn || conn->transport != JSONRPC_HTTP || conn->closed)
		return -1;
	if (conn->listening)
		return 0;
	conn->listening = true;
	conn->refused = refused;
	conn->refused_data = data;
	conn->stream_retry_ms = 0;
	if (!conn->inbox_tail)
		conn->inbox_tail = &conn->inbox;
	if (jsonrpc_stream_open(conn)) {
		conn->listening = false;
		return -1;
	}
	return 0;
}

bool jsonrpc_conn_listening(const struct jsonrpc_conn *conn)
{
	return conn && conn->listening;
}

/* Close the stream, and drop what it delivered and nobody served. */
static void jsonrpc_stream_close(struct jsonrpc_conn *conn)
{
	conn->listening = false;
	jsonrpc_drop_source(&conn->stream_timer);
	if (conn->stream) {
		jsonrpc_request_free(conn->stream);
		conn->stream = NULL;
	}
	jsonrpc_inbox_free(conn);
	jsonrpc_sse_free(&conn->sse);
}

/*
 * Public API
 */

struct jsonrpc_conn *
jsonrpc_conn_stdio(struct fyai_ctx *ctx, int stdin_fd, int stdout_fd,
		   int timeout_s, const char *name, const char *log_channel)
{
	struct jsonrpc_conn *conn;

	conn = calloc(1, sizeof(*conn));
	fyai_error_check(ctx, conn, err_out, "out of memory");
	conn->ctx = ctx;
	conn->transport = JSONRPC_STDIO;
	conn->name = name;
	conn->log_channel = log_channel;
	conn->timeout_s = timeout_s;
	conn->owned_fd = -1;
	conn->stdin_fd = stdin_fd;
	conn->stdout_fd = stdout_fd;
	conn->closed = false;
	return conn;

err_out:
	return NULL;
}

struct jsonrpc_conn *
jsonrpc_conn_socket(struct fyai_ctx *ctx, int fd, int timeout_s,
                    const char *name, const char *log_channel)
{
	struct jsonrpc_conn *conn;
	int write_fd;

	write_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
	if (write_fd < 0) {
		fyai_error(ctx, "%s: cannot duplicate socket: %s", name, strerror(errno));
		return NULL;
	}
	conn = jsonrpc_conn_stdio(ctx, write_fd, fd, timeout_s, name, log_channel);
	if (!conn) {
		close(write_fd);
		return NULL;
	}
	conn->owned_fd = write_fd;
	return conn;
}

struct jsonrpc_conn *
jsonrpc_conn_http(struct fyai_ctx *ctx, const char *endpoint, int timeout_s,
		  const char *name, const char *log_channel,
		  const struct jsonrpc_http_hooks *hooks)
{
	struct jsonrpc_conn *conn;

	conn = calloc(1, sizeof(*conn));
	fyai_error_check(ctx, conn, err_out, "out of memory");
	conn->ctx = ctx;
	conn->transport = JSONRPC_HTTP;
	conn->name = name;
	conn->log_channel = log_channel;
	conn->timeout_s = timeout_s;
	conn->endpoint = endpoint;
	conn->inbox_tail = &conn->inbox;
	if (hooks) {
		conn->hooks = *hooks;
		conn->has_hooks = true;
	}
	return conn;

err_out:
	return NULL;
}

void jsonrpc_conn_shutdown(struct jsonrpc_conn *conn, const char *why)
{
	struct fyai_event_loop *el;

	if (!conn || conn->closed)
		return;
	if (conn->transport == JSONRPC_STDIO) {
		el = fyai_ctx_loop(jsonrpc_conn_ctx(conn));
		if (el)
			fyai_event_defer_cancel(el, jsonrpc_conn_late_close, conn);
		jsonrpc_drop_source(&conn->read_src);
		jsonrpc_drop_source(&conn->write_src);
	}
	if (conn->transport == JSONRPC_HTTP)
		jsonrpc_stream_close(conn);
	jsonrpc_conn_fail_pending(conn, why);
}

void jsonrpc_conn_destroy(struct jsonrpc_conn *conn)
{
	struct fyai_event_loop *el;

	if (conn && conn->transport == JSONRPC_TUNNEL) {
		el = fyai_ctx_loop(jsonrpc_conn_ctx(conn));
		if (el)
			fyai_event_defer_cancel(el, jsonrpc_tunnel_flush, conn);
		jsonrpc_conn_settle(conn, "was destroyed", true);
	}
	if (conn && conn->transport == JSONRPC_STDIO) {
		el = fyai_ctx_loop(jsonrpc_conn_ctx(conn));
		if (el)
			fyai_event_defer_cancel(el, jsonrpc_conn_late_close, conn);
		jsonrpc_drop_source(&conn->read_src);
		jsonrpc_drop_source(&conn->write_src);
		/* Settle and detach requests before destroying the connection. */
		jsonrpc_conn_settle(conn, "was destroyed", true);
		if (conn->owned_fd >= 0)
			close(conn->owned_fd);
		fy_align_free(conn->image_rx);
		free(conn->rx.data);
		free(conn->tx.data);
		conn->rx.data = conn->tx.data = NULL;
		fy_generic_builder_destroy(conn->frame_gb);
		conn->frame_gb = NULL;
		fy_generic_builder_destroy(conn->rx_gb);
		conn->rx_gb = NULL;
	}
	if (conn && conn->transport == JSONRPC_HTTP)
		jsonrpc_stream_close(conn);
	free(conn);
}

void jsonrpc_conn_expect_close(struct jsonrpc_conn *conn)
{
	if (conn)
		conn->expect_close = true;
}

/*
 * tunnel transport
 */

/* Complete the notifications that the owner has carried. */
static void jsonrpc_tunnel_flush(void *userdata)
{
	struct jsonrpc_conn *conn = userdata;
	struct jsonrpc_request *req;

	while ((req = conn->flush_wait)) {
		conn->flush_wait = req->next;
		req->next = NULL;
		jsonrpc_finish(req, true, fy_null, 0, CURLE_OK);
	}
}

static int jsonrpc_tunnel_submit(struct jsonrpc_request *req, fy_generic params)
{
	struct jsonrpc_conn *conn = req->conn;
	struct fyai_ctx *ctx = jsonrpc_conn_ctx(conn);
	struct fy_generic_builder *gb;
	struct fyai_event_loop *el;
	fy_generic rpc;
	int rc;

	fyai_error_check(ctx, !conn->closed, err_out,
			 "%s is closed", conn->name);
	el = fyai_ctx_loop(ctx);
	fyai_error_check(ctx, el, err_out,
			 "could not acquire the application event loop");
	gb = fyai_ctx_transient_gb(ctx);
	fyai_error_check(ctx, gb, err_out,
			 "could not acquire transient storage");
	rpc = fy_mapping(gb, "jsonrpc", "2.0",
			 "method", req->method, "params", params);
	if (!req->notification)
		rpc = fy_assoc(gb, rpc, "id", req->id);
	fyai_error_check(ctx, fy_is_valid(rpc), err_out,
			 "could not build the JSON-RPC tunnel request");

	/* Register the request before the peer can reply. */
	if (req->notification) {
		req->next = conn->flush_wait;
		conn->flush_wait = req;
	} else {
		req->next = conn->pending;
		conn->pending = req;
	}
	rc = conn->tunnel_send(conn->tunnel_userdata, rpc);
	fyai_error_check(ctx, !rc, err_dequeue,
			 "%s could not carry the request", conn->name);
	/* A notification completes after the submit returns, as on stdio. */
	if (req->notification)
		return fyai_event_defer(el, jsonrpc_tunnel_flush, conn);
	if (conn->timeout_s > 0) {
		rc = fyai_event_add_timer(el, conn->timeout_s * 1000, 0,
					  jsonrpc_stdio_timeout, req,
					  &req->timer_src);
		fyai_error_check(ctx, !rc, err_dequeue,
				 "could not arm response timeout");
	}
	return 0;

err_dequeue:
	jsonrpc_conn_unlink(req);
err_out:
	return -1;
}

struct jsonrpc_conn *
jsonrpc_conn_tunnel(struct fyai_ctx *ctx, int timeout_s, const char *name,
		    const char *log_channel, jsonrpc_tunnel_send_fn send,
		    void *userdata)
{
	struct jsonrpc_conn *conn;

	conn = calloc(1, sizeof(*conn));
	fyai_error_check(ctx, conn, err_out, "out of memory");
	conn->ctx = ctx;
	conn->transport = JSONRPC_TUNNEL;
	conn->name = name;
	conn->log_channel = log_channel;
	conn->timeout_s = timeout_s;
	conn->stdin_fd = -1;
	conn->stdout_fd = -1;
	conn->tunnel_send = send;
	conn->tunnel_userdata = userdata;
	return conn;

err_out:
	return NULL;
}

void jsonrpc_conn_tunnel_receive(struct jsonrpc_conn *conn, fy_generic frame)
{
	if (!conn || conn->transport != JSONRPC_TUNNEL || conn->closed ||
	    !fy_is_mapping(frame))
		return;
	jsonrpc_conn_frame(conn, frame);
}

void jsonrpc_conn_tunnel_close(struct jsonrpc_conn *conn, const char *why)
{
	if (!conn || conn->transport != JSONRPC_TUNNEL || conn->closed)
		return;
	conn->closed = true;
	jsonrpc_conn_settle(conn, why, false);
}

long long jsonrpc_conn_next_id(struct jsonrpc_conn *conn)
{
	return ++conn->next_id;
}

struct jsonrpc_request *
jsonrpc_request_submit(struct jsonrpc_conn *conn, const char *method,
		       fy_generic params, long long id, bool notification,
		       jsonrpc_complete_fn complete, void *userdata)
{
	struct jsonrpc_request *req;
	int rc;

	req = calloc(1, sizeof(*req));
	fyai_error_check(jsonrpc_conn_ctx(conn), req, err_out, "out of memory");
	req->conn = conn;
	req->method = method;
	req->id = id;
	req->notification = notification;
	req->complete = complete;
	req->userdata = userdata;
	req->result = fy_invalid;
	req->code = CURLE_OK;
	clock_gettime(CLOCK_MONOTONIC, &req->started);

	rc = conn->transport == JSONRPC_STDIO ?
		jsonrpc_stdio_submit(req, params) :
	     conn->transport == JSONRPC_TUNNEL ?
		jsonrpc_tunnel_submit(req, params) :
		jsonrpc_http_submit(req, params);
	fyai_error_check(jsonrpc_conn_ctx(conn), !rc, err_free, "%s could not submit %s",
			 conn->name, method);
	return req;

err_free:
	jsonrpc_request_free(req);
err_out:
	return NULL;
}

void jsonrpc_request_cancel(struct jsonrpc_request *req)
{
	if (!req || req->state != JSONRPC_REQ_PENDING)
		return;
	req->cancel_requested = true;
	if (jsonrpc_conn_framed(req->conn))
		jsonrpc_finish(req, false, fy_invalid, 0, CURLE_OK);
	else if (req->transfer)
		fyai_curl_cancel(req->transfer);
}

bool jsonrpc_request_done(const struct jsonrpc_request *req)
{
	return req && req->state != JSONRPC_REQ_PENDING;
}

bool jsonrpc_request_ok(const struct jsonrpc_request *req)
{
	return req && req->state == JSONRPC_REQ_OK;
}

fy_generic jsonrpc_request_result(const struct jsonrpc_request *req)
{
	return req ? req->result : fy_invalid;
}

long jsonrpc_request_http_status(const struct jsonrpc_request *req)
{
	return req ? req->status : 0;
}

CURLcode jsonrpc_request_curl_code(const struct jsonrpc_request *req)
{
	return req ? req->code : CURLE_OK;
}

const char *jsonrpc_request_response_body(
		const struct jsonrpc_request *req)
{
	if (!req)
		return NULL;
	/* Only HTTP requests retain a response body. */
	if (req->conn->transport != JSONRPC_HTTP)
		return NULL;
	return req->response.data;
}

const char *
jsonrpc_request_response_header(const struct jsonrpc_request *req,
				const char *name)
{
	const struct curl_slist *item;
	const char *value;
	size_t len;

	if (!req || !name || !*name)
		return NULL;
	len = strlen(name);
	for (item = req->response_headers; item; item = item->next) {
		if (strncasecmp(item->data, name, len) ||
		    item->data[len] != ':')
			continue;
		value = item->data + len + 1;
		while (*value == ' ' || *value == '\t')
			value++;
		return value;
	}
	return NULL;
}

static void jsonrpc_request_free(struct jsonrpc_request *req)
{
	if (!req)
		return;
	jsonrpc_conn_unlink(req);
	jsonrpc_stdio_sources_drop(req);
	if (req->transfer)
		fyai_curl_transfer_destroy(req->transfer);
	curl_easy_cleanup(req->curl);
	curl_slist_free_all(req->headers);
	curl_slist_free_all(req->response_headers);
	free(req->response.data);
	free(req->body);
	free(req);
}

static void jsonrpc_notify_done(struct jsonrpc_request *req, void *userdata)
{
	(void)userdata;
	jsonrpc_request_destroy(req);
}

/* Submit a self-releasing notification. */
int jsonrpc_notify(struct jsonrpc_conn *conn, const char *method,
		   fy_generic params)
{
	struct jsonrpc_request *req;

	req = jsonrpc_request_submit(conn, method, params, 0, true,
				     jsonrpc_notify_done, NULL);
	return req ? 0 : -1;
}

void jsonrpc_request_destroy(struct jsonrpc_request *req)
{
	jsonrpc_request_free(req);
}
