/* SPDX-License-Identifier: MIT */
#include "config.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include "fyai.h"
#include "fyai_buf.h"
#include "fyai_event.h"
#include "fyai_host.h"
#include "fyai_httpd.h"
#include "fyai_net.h"
#include "fyai_remote_tls.h"
#include "fyai_ws.h"
#include "fyai_ws_client.h"

#define FYAI_MODULE FYAIEM_REMOTE

/* Say why the answer @head to an upgrade, with @status from the check, is not one. */
static void ws_client_refusal(struct fyai_ctx *ctx, int status, char *head)
{
	char *eol = strchr(head, '\r');

	if (eol)
		*eol = '\0';
	if (status == 401)
		fyai_error(ctx, "the server refused the token (%s)", head);
	else if (status == -2)
		fyai_error(ctx, "the server gave no Sec-WebSocket-Accept");
	else if (status == -3)
		fyai_error(ctx, "the server answered with another Sec-WebSocket-Accept");
	else
		fyai_error(ctx, "the server did not upgrade the connection (%s)", head);
}

/* Read the answer to the upgrade; the bytes after its head stay in @c->in. */
static int ws_client_handshake(struct fyai_ctx *ctx, struct fyai_ws_client *c,
			       const struct fyai_ws_client_opts *o)
{
	unsigned char nonce[16];
	char key[FYAI_WS_KEY_SIZE], request[2048], head[FYAI_HTTP_HEAD_MAX + 1], *end;
	size_t used = 0;
	ssize_t got;
	int n, status;

	fyai_host_random(nonce, sizeof(nonce));
	n = fyai_ws_client_request(o->path, o->url.authority, o->bearer, nonce, key,
				   request, sizeof(request));
	fyai_error_check(ctx, n > 0, err, "cannot make the upgrade request");
	status = fyai_netio_send_all(&c->io, request, (size_t)n);
	memset(request, 0, sizeof(request));
	fyai_error_check(ctx, !status, err, "cannot send the request: %s",
			 strerror(errno));
	/* The answer ends with a blank line; the frames follow it at once. */
	for (;;) {
		got = fyai_netio_recv(&c->io, head + used, sizeof(head) - 1 - used);
		if (got < 0 && errno == EINTR)
			continue;
		fyai_error_check(ctx, got > 0, err, "the server closed the connection "
				 "before it answered");
		used += (size_t)got;
		head[used] = '\0';
		end = strstr(head, "\r\n\r\n");
		if (end)
			break;
		fyai_error_check(ctx, used < FYAI_HTTP_HEAD_MAX, err,
				 "the answer of the server is too long");
	}
	end += 4;
	status = fyai_ws_client_check(head, key);
	if (status) {
		ws_client_refusal(ctx, status, head);
		goto err;
	}
	c->in_len = used - (size_t)(end - head);
	memcpy(c->in, end, c->in_len);
	return 0;
err:
	return -1;
}

int fyai_ws_client_open(struct fyai_ctx *ctx, const struct fyai_ws_client_opts *o,
			struct fyai_ws_client *c)
{
	SSL *ssl = NULL;
	int fd;

	memset(c, 0, sizeof(*c));
	fyai_netio_init(&c->io, -1);
	fd = fyai_net_connect(ctx, o->url.host, o->url.port);
	if (fd < 0)
		return -1;
	c->io.fd = fd;
	if (o->url.tls) {
		if (fyai_tls_connect(ctx, fd, o->url.host, o->pin, o->ca_file, &ssl))
			goto err;
		fyai_netio_tls_adopt(&c->io, ssl);
	}
	if (ws_client_handshake(ctx, c, o))
		goto err;
	fyai_ws_init(&c->ws, false, o->max_message);
	return 0;
err:
	fyai_netio_close(&c->io);
	return -1;
}

void fyai_ws_client_close(struct fyai_ws_client *c)
{
	fyai_netio_close(&c->io);
	fyai_ws_release(&c->ws);
}

int fyai_ws_client_fd(const struct fyai_ws_client *c)
{
	return c->io.fd;
}

int fyai_ws_client_send(struct fyai_ws_client *c, int opcode, const void *data, size_t n)
{
	unsigned char key[4], *frame;
	size_t len;
	int rc;

	frame = malloc(FYAI_WS_HEADER_MAX + n);
	if (!frame) {
		errno = ENOMEM;
		return -1;
	}
	fyai_host_random(key, sizeof(key));
	len = fyai_ws_encode(frame, opcode, data, n, key);
	rc = fyai_netio_send_all(&c->io, frame, len);
	free(frame);
	return rc;
}

bool fyai_ws_client_buffered(const struct fyai_ws_client *c)
{
	return c->in_pos < c->in_len || fyai_netio_buffered(&c->io);
}

int fyai_ws_client_recv(struct fyai_ws_client *c, struct fyai_ws_message *msg,
			bool readable)
{
	size_t used;
	ssize_t got;
	int rc;

	for (;;) {
		if (c->in_pos < c->in_len) {
			rc = fyai_ws_feed(&c->ws, c->in + c->in_pos, c->in_len - c->in_pos,
					  &used, msg);
			c->in_pos += used;
			if (rc)
				return rc;
		}
		if (!readable && !fyai_netio_buffered(&c->io))
			return 0;
		got = fyai_netio_recv(&c->io, c->in, sizeof(c->in));
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			return -2;
		readable = false;
		c->in_pos = 0;
		c->in_len = (size_t)got;
	}
}

/* ---- the client on an event loop ------------------------------------------ */


enum wsc_state {
	WSC_CONNECT,	/* waiting for the connect */
	WSC_TLS,	/* the TLS handshake runs */
	WSC_HEAD,	/* the request is sent, the answer is awaited */
	WSC_OPEN,	/* frames go both ways */
	WSC_CLOSING,	/* the close frame is queued: end when it has left */
	WSC_DEAD,	/* ended; the memory goes at the next turn of the loop */
};

struct fyai_wsc {
	struct fyai_ctx *ctx;
	struct fyai_event_loop *loop;
	struct fyai_wsc_ops ops;
	struct fyai_wsc_opts o;		/* the strings are @path to @ca_file */
	char *path, *bearer, *pin, *ca_file;
	enum wsc_state state;
	bool opened;			/* open() was called */
	struct fyai_netio io;
	struct fyai_event_source *src, *timer;
	struct fyai_buf in;		/* the answer to the upgrade */
	struct fyai_buf out;		/* bytes to the server */
	struct fyai_ws ws;
	char key[FYAI_WS_KEY_SIZE];
};

static void wsc_free_cb(void *data)
{
	struct fyai_wsc *c = data;

	fyai_buf_free(&c->in);
	fyai_buf_free(&c->out);
	fyai_ws_release(&c->ws);
	if (c->bearer)
		memset(c->bearer, 0, strlen(c->bearer));
	free(c->path);
	free(c->bearer);
	free(c->pin);
	free(c->ca_file);
	free(c);
}

/* End the connection: once, whatever the cause. */
static void wsc_end(struct fyai_wsc *c)
{
	if (c->state == WSC_DEAD)
		return;
	c->state = WSC_DEAD;
	fyai_event_source_remove(c->src);
	c->src = NULL;
	fyai_event_source_remove(c->timer);
	c->timer = NULL;
	fyai_netio_close(&c->io);
	if (c->ops.closed)
		c->ops.closed(c->ops.data, c, c->opened);
	/* The memory goes when the loop is free of this call. */
	(void)fyai_event_defer(c->loop, wsc_free_cb, c);
}

void fyai_wsc_kill(struct fyai_wsc *c)
{
	wsc_end(c);
}

/* Watch what the connection waits for. */
static void wsc_interest(struct fyai_wsc *c)
{
	unsigned int events = 0;

	if (c->state == WSC_DEAD || !c->src)
		return;
	switch (c->state) {
	case WSC_CONNECT:
		events = FYAIEV_WRITE;
		break;
	case WSC_TLS:
		events = c->io.want_write ? FYAIEV_WRITE : FYAIEV_READ;
		break;
	default:
		if (c->state != WSC_CLOSING || c->io.want_read)
			events |= FYAIEV_READ;
		if (fyai_buf_pending(&c->out) || c->io.want_write)
			events |= FYAIEV_WRITE;
		break;
	}
	(void)fyai_event_fd_modify(c->src, events ? events : FYAIEV_EOF);
}

/* Queue one masked frame. Returns 0, or -1 on no memory or over the limit. */
static int wsc_queue(struct fyai_wsc *c, int opcode, const void *data, size_t n)
{
	unsigned char hdr[FYAI_WS_HEADER_MAX], key[4];
	size_t h;
	char *at;

	if (fyai_buf_pending(&c->out) + n > c->o.max_output)
		return -1;
	fyai_host_random(key, sizeof(key));
	h = fyai_ws_header(hdr, opcode, true, n, key);
	at = fyai_buf_extend(&c->out, h + n);
	if (!at)
		return -1;
	memcpy(at, hdr, h);
	if (n) {
		memcpy(at + h, data, n);
		fyai_ws_mask((unsigned char *)at + h, n, key, 0);
	}
	return 0;
}

/* Write what is queued. Returns 0, or -1 when the connection ended or failed. */
static int wsc_flush(struct fyai_wsc *c)
{
	return fyai_netio_flush(&c->io, &c->out);
}

int fyai_wsc_send(struct fyai_wsc *c, int opcode, const void *data, size_t n)
{
	int rc;

	if (c->state != WSC_OPEN)
		return -1;
	rc = wsc_queue(c, opcode, data, n);
	if (rc)
		return -1;
	/* Try at once, so that a frame is a segment of its own when it can be. */
	rc = wsc_flush(c);
	if (rc) {
		fyai_error(c->ctx, "the connection to '%s' was lost", c->o.ws.url.host);
		wsc_end(c);
		return -1;
	}
	wsc_interest(c);
	return 0;
}

size_t fyai_wsc_pending(const struct fyai_wsc *c)
{
	return fyai_buf_pending(&c->out);
}

void fyai_wsc_close(struct fyai_wsc *c, unsigned int code)
{
	unsigned char payload[2] = { (unsigned char)(code >> 8), (unsigned char)code };
	int rc;

	if (c->state == WSC_DEAD || c->state == WSC_CLOSING)
		return;
	if (c->state != WSC_OPEN) {
		/* No WebSocket yet: nothing to say. */
		wsc_end(c);
		return;
	}
	rc = wsc_queue(c, FYAI_WS_CLOSE, payload, sizeof(payload));
	c->state = WSC_CLOSING;
	if (rc || wsc_flush(c) || !fyai_buf_pending(&c->out)) {
		wsc_end(c);
		return;
	}
	wsc_interest(c);
}

/* One message of the server. */
static void wsc_message(struct fyai_wsc *c, const struct fyai_ws_message *msg)
{
	switch (msg->opcode) {
	case FYAI_WS_TEXT:
	case FYAI_WS_BINARY:
		if (c->state == WSC_OPEN && c->ops.message)
			c->ops.message(c->ops.data, c, msg);
		return;
	case FYAI_WS_PING:
		if (wsc_queue(c, FYAI_WS_PONG, msg->data, msg->size))
			wsc_end(c);
		return;
	case FYAI_WS_PONG:
		return;
	case FYAI_WS_CLOSE:
		/* Echo the status code of the server, else the normal close. */
		fyai_wsc_close(c, msg->size >= 2 ?
			       (unsigned int)(msg->data[0] << 8 | msg->data[1]) : 1000);
		return;
	default:
		fyai_wsc_close(c, 1003);
		return;
	}
}

/* Give the frames in @p of @n bytes to the handler. */
static void wsc_feed(struct fyai_wsc *c, const unsigned char *p, size_t n)
{
	struct fyai_ws_message msg;
	size_t used, off;
	int rc;

	for (off = 0; off < n && c->state != WSC_DEAD && c->state != WSC_CLOSING;) {
		rc = fyai_ws_feed(&c->ws, p + off, n - off, &used, &msg);
		off += used;
		if (rc < 0) {
			fyai_wsc_close(c, 1002);
			return;
		}
		if (!rc)
			break;
		wsc_message(c, &msg);
	}
}

/* The answer to the upgrade is whole: check it, and open the connection. */
static int wsc_answer(struct fyai_wsc *c, size_t end)
{
	char *head = c->in.data + c->in.off;
	int status;

	status = fyai_ws_client_check(head, c->key);
	if (status) {
		ws_client_refusal(c->ctx, status, head);
		return -1;
	}
	fyai_buf_consume(&c->in, end);
	fyai_event_source_remove(c->timer);
	c->timer = NULL;
	fyai_ws_init(&c->ws, false, c->o.ws.max_message);
	c->state = WSC_OPEN;
	c->opened = true;
	if (c->ops.open)
		c->ops.open(c->ops.data, c);
	/* The frames follow the answer at once, in the same bytes. */
	if (c->state == WSC_OPEN && fyai_buf_pending(&c->in)) {
		wsc_feed(c, (unsigned char *)c->in.data + c->in.off,
			 fyai_buf_pending(&c->in));
		fyai_buf_free(&c->in);
	}
	return 0;
}

/* Bytes of the server. */
static void wsc_read(struct fyai_wsc *c)
{
	unsigned char chunk[FYAI_WS_CLIENT_CHUNK];
	struct fyai_ctx *ctx = c->ctx;
	const char *mark;
	ssize_t n;
	int rc, rounds;

	for (rounds = 0; (rounds < 16 || fyai_netio_buffered(&c->io)) &&
	     c->state != WSC_DEAD; rounds++) {
		n = fyai_netio_recv(&c->io, chunk, sizeof(chunk));
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return;
		if (n <= 0) {
			if (!c->opened)
				fyai_error(ctx, "the server closed the connection "
					   "before it answered");
			goto err_end;
		}
		if (c->state == WSC_OPEN) {
			wsc_feed(c, chunk, (size_t)n);
			continue;
		}
		if (c->state == WSC_CLOSING)
			continue;
		rc = fyai_buf_append_data(&c->in, chunk, (size_t)n);
		fyai_error_check(ctx, !rc, err_end, "cannot keep the answer of the server");
		mark = strstr(c->in.data + c->in.off, "\r\n\r\n");
		if (!mark) {
			fyai_error_check(ctx, fyai_buf_pending(&c->in) < FYAI_HTTP_HEAD_MAX,
					 err_end, "the answer of the server is too long");
			continue;
		}
		if (wsc_answer(c, (size_t)(mark - (c->in.data + c->in.off)) + 4))
			goto err_end;
	}
	return;
err_end:
	wsc_end(c);
}

/* Queue the upgrade request. */
static int wsc_request(struct fyai_wsc *c)
{
	unsigned char nonce[16];
	char request[2048];
	int n, rc;

	fyai_host_random(nonce, sizeof(nonce));
	n = fyai_ws_client_request(c->path, c->o.ws.url.authority, c->bearer, nonce,
				   c->key, request, sizeof(request));
	fyai_error_check(c->ctx, n > 0, err, "cannot make the upgrade request");
	rc = fyai_buf_append_data(&c->out, request, (size_t)n);
	memset(request, 0, sizeof(request));
	fyai_error_check(c->ctx, !rc, err, "cannot queue the upgrade request");
	c->state = WSC_HEAD;
	return 0;
err:
	return -1;
}

/* Do what the connection can do now. */
static void wsc_run(struct fyai_wsc *c, unsigned int events)
{
	struct fyai_ctx *ctx = c->ctx;
	const char *host = c->o.ws.url.host;
	SSL *ssl;
	int rc;

	if (c->state == WSC_CONNECT) {
		rc = fyai_net_connect_finish(ctx, c->io.fd, host, c->o.ws.url.port);
		if (rc)
			goto err_end;
		if (c->o.ws.url.tls) {
			ssl = fyai_tls_client_new(ctx, host, c->pin, c->ca_file);
			if (!ssl)
				goto err_end;
			rc = fyai_netio_tls_connect(&c->io, ssl);
			fyai_error_check(ctx, !rc, err_end, "cannot start TLS");
			c->state = WSC_TLS;
		} else if (wsc_request(c)) {
			goto err_end;
		}
	}
	if (c->state == WSC_TLS) {
		rc = fyai_netio_handshake(&c->io);
		if (rc < 0) {
			fyai_tls_client_error(ctx, c->io.ssl, host, c->pin);
			ERR_clear_error();
			goto err_end;
		}
		if (!rc)
			goto wait;
		/* The pin is checked before the token leaves. */
		if (fyai_tls_client_verify(ctx, c->io.ssl, host, c->pin) ||
		    wsc_request(c))
			goto err_end;
		events |= FYAIEV_WRITE;
	}
	if (fyai_buf_pending(&c->out) && ((events & FYAIEV_WRITE) || c->state == WSC_HEAD)) {
		rc = wsc_flush(c);
		if (rc) {
			if (!c->opened)
				fyai_error(ctx, "the connection to '%s' was lost", host);
			goto err_end;
		}
	}
	if (c->state == WSC_CLOSING && !fyai_buf_pending(&c->out))
		goto err_end;
	if (c->state >= WSC_HEAD && (events & (FYAIEV_READ | FYAIEV_EOF)))
		wsc_read(c);
	if (c->state == WSC_DEAD)
		return;
wait:
	wsc_interest(c);
	if (c->state == WSC_OPEN && !fyai_buf_pending(&c->out) && c->ops.drain)
		c->ops.drain(c->ops.data, c);
	return;
err_end:
	wsc_end(c);
}

static enum fyai_event_action wsc_cb(const struct fyai_event *ev)
{
	struct fyai_wsc *c = ev->userdata;

	if (c->state == WSC_DEAD)
		return FYAIEA_CONTINUE;
	wsc_run(c, ev->events);
	return FYAIEA_CONTINUE;
}

static enum fyai_event_action wsc_timer_cb(const struct fyai_event *ev)
{
	struct fyai_wsc *c = ev->userdata;

	/* The timer is one shot and goes with its client. */
	c->timer = NULL;
	fyai_error(c->ctx, "'%s' did not open the WebSocket in time", c->o.ws.url.host);
	wsc_end(c);
	return FYAIEA_CONTINUE;
}

struct fyai_wsc *fyai_wsc_open(struct fyai_ctx *ctx, const struct fyai_wsc_opts *opts,
			       const struct fyai_wsc_ops *ops)
{
	struct fyai_wsc *c;
	int fd, rc;

	c = calloc(1, sizeof(*c));
	fyai_error_check(ctx, c, err, "cannot allocate the WebSocket client");
	c->ctx = ctx;
	c->o = *opts;
	c->ops = *ops;
	c->loop = opts->loop ? opts->loop : fyai_ctx_loop(ctx);
	fyai_error_check(ctx, c->loop, err_free, "no event loop");
	c->path = strdup(opts->ws.path);
	c->bearer = opts->ws.bearer ? strdup(opts->ws.bearer) : NULL;
	c->pin = opts->ws.pin ? strdup(opts->ws.pin) : NULL;
	c->ca_file = opts->ws.ca_file ? strdup(opts->ws.ca_file) : NULL;
	fyai_error_check(ctx, c->path && (c->bearer || !opts->ws.bearer) &&
			 (c->pin || !opts->ws.pin) && (c->ca_file || !opts->ws.ca_file),
			 err_free, "cannot keep the options of the connection");
	fyai_netio_init(&c->io, -1);
	fd = fyai_net_connect_start(ctx, opts->ws.url.host, opts->ws.url.port);
	if (fd < 0)
		goto err_free;
	c->io.fd = fd;
	c->state = WSC_CONNECT;
	rc = fyai_event_add_fd(c->loop, fd, FYAIEV_WRITE, wsc_cb, c, &c->src);
	fyai_error_check(ctx, !rc, err_close, "cannot watch the connection");
	if (opts->open_ms) {
		rc = fyai_event_add_timer(c->loop, opts->open_ms, 0, wsc_timer_cb, c,
					  &c->timer);
		fyai_error_check(ctx, !rc, err_close, "cannot time the connection");
	}
	return c;
err_close:
	fyai_event_source_remove(c->src);
	fyai_netio_close(&c->io);
err_free:
	wsc_free_cb(c);
err:
	return NULL;
}
