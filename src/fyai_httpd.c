/* SPDX-License-Identifier: MIT */
#include "config.h"
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "fyai.h"
#include "fyai_event.h"
#include "fyai_httpd.h"
#include "fyai_buf.h"
#include "fyai_net.h"
#include "fyai_ws.h"

#define FYAI_MODULE FYAIEM_REMOTE

/* ---- the request head ---------------------------------------------------- */

void fyai_http_request_release(struct fyai_http_request *req)
{
	free(req->text);
	free(req->headers);
	memset(req, 0, sizeof(*req));
}

const char *fyai_http_header(const struct fyai_http_request *req, const char *name)
{
	size_t i;

	for (i = 0; i < req->nheaders; i++)
		if (!strcasecmp(req->headers[i].name, name))
			return req->headers[i].value;
	return NULL;
}

/* Keep one header line, in order. */
static int http_add_header(struct fyai_http_request *req, const char *name,
			   const char *value)
{
	struct fyai_http_header *list;

	list = realloc(req->headers, (req->nheaders + 1) * sizeof(*list));
	if (!list)
		return -1;
	req->headers = list;
	list[req->nheaders].name = name;
	list[req->nheaders].value = value;
	req->nheaders++;
	return 0;
}

/* True when the list @value, separated by commas, has the token @word. */
static bool http_list_has(const char *value, const char *word)
{
	size_t n = strlen(word);

	while (*value) {
		while (*value == ' ' || *value == '\t' || *value == ',')
			value++;
		if (!strncasecmp(value, word, n) &&
		    (value[n] == '\0' || value[n] == ',' || value[n] == ' ' ||
		     value[n] == '\t'))
			return true;
		while (*value && *value != ',')
			value++;
	}
	return false;
}

/* Set @slot to @value once; a second value is an error. */
static int http_once(const char **slot, const char *value)
{
	if (*slot)
		return -1;
	*slot = value;
	return 0;
}

int fyai_http_parse(const char *head, size_t len, unsigned int flags,
		    struct fyai_http_request *req)
{
	char *line, *next, *colon, *name, *value, *end, *sp;
	const char *upgrade = NULL, *connection = NULL, *version = NULL;
	size_t i;
	char *vend, *stop;
	int status;

	memset(req, 0, sizeof(*req));
	if (len < 4 || len > FYAI_HTTP_HEAD_MAX || memcmp(head + len - 4, "\r\n\r\n", 4))
		return 400;
	for (i = 0; i < len; i++)
		if (!head[i] || (head[i] == '\n' && (!i || head[i - 1] != '\r')) ||
		    (head[i] == '\r' && (i + 1 >= len || head[i + 1] != '\n')))
			return 400;
	req->text = malloc(len + 1);
	if (!req->text)
		return 400;
	memcpy(req->text, head, len);
	req->text[len] = '\0';

	/* The request line: METHOD SP TARGET SP HTTP/1.1. */
	line = req->text;
	end = strstr(line, "\r\n");
	*end = '\0';
	next = end + 2;
	sp = strchr(line, ' ');
	if (!sp || sp == line)
		goto bad;
	*sp = '\0';
	req->method = line;
	req->target = sp + 1;
	sp = strchr(sp + 1, ' ');
	if (!sp || sp == req->target)
		goto bad;
	*sp = '\0';
	version = sp + 1;
	if (!strcmp(version, "HTTP/1.0") && (flags & FYAI_HTTP_PARSE_HTTP10)) {
		req->http10 = true;
	} else if (strcmp(version, "HTTP/1.1")) {
		status = !strncmp(version, "HTTP/", 5) ? 505 : 400;
		fyai_http_request_release(req);
		return status;
	}
	if (req->target[0] != '/')
		goto bad;

	while (*next && !(next[0] == '\r' && next[1] == '\n')) {
		end = strstr(next, "\r\n");
		*end = '\0';
		name = next;
		next = end + 2;
		colon = strchr(name, ':');
		/* No folded line, no blank before the colon, and a name. */
		if (!colon || colon == name || *name == ' ' || *name == '\t')
			goto bad;
		for (i = 0; name + i < colon; i++)
			if (!isalnum((unsigned char)name[i]) && !strchr("-_", name[i]))
				goto bad;
		*colon = '\0';
		value = colon + 1;
		while (*value == ' ' || *value == '\t')
			value++;
		vend = value + strlen(value);
		while (vend > value && (vend[-1] == ' ' || vend[-1] == '\t'))
			*--vend = '\0';
		if (http_add_header(req, name, value))
			goto bad;
		if (!strcasecmp(name, "Host")) {
			if (http_once(&req->host, value))
				goto bad;
		} else if (!strcasecmp(name, "Origin")) {
			if (http_once(&req->origin, value))
				goto bad;
		} else if (!strcasecmp(name, "Sec-WebSocket-Key")) {
			if (http_once(&req->ws_key, value))
				goto bad;
		} else if (!strcasecmp(name, "Sec-WebSocket-Version")) {
			if (req->ws_version)
				goto bad;
			req->ws_version = strtol(value, &stop, 10);
			if (*stop || req->ws_version <= 0)
				goto bad;
		} else if (!strcasecmp(name, "Upgrade")) {
			if (http_once(&upgrade, value))
				goto bad;
		} else if (!strcasecmp(name, "Connection")) {
			/* The header may be given more than once; the lists join. */
			if (http_list_has(value, "upgrade"))
				connection = value;
		} else if (!strcasecmp(name, "Authorization")) {
			if (req->bearer)
				goto bad;
			if (!strncasecmp(value, "Bearer ", 7)) {
				value += 7;
				while (*value == ' ')
					value++;
				if (!*value)
					goto bad;
				req->bearer = value;
			} else {
				req->bearer = "";
			}
		}
	}
	req->upgrade = upgrade && !strcasecmp(upgrade, "websocket");
	req->connection_upgrade = connection != NULL;
	/* HTTP/1.0 has no need of a Host. */
	if (!req->host && !req->http10)
		goto bad;
	return 0;
bad:
	fyai_http_request_release(req);
	return 400;
}

bool fyai_http_origin_ok(const struct fyai_http_request *req, bool tls)
{
	size_t n, scheme = tls ? 8 : 7;

	if (!req->origin)
		return true;
	if (!req->host)
		return false;
	if (strncasecmp(req->origin, tls ? "https://" : "http://", scheme))
		return false;
	n = strlen(req->host);
	return !strncasecmp(req->origin + scheme, req->host, n) &&
	       !req->origin[scheme + n];
}

/* ---- the connections ----------------------------------------------------- */

struct fyai_httpd_conn {
	struct fyai_httpd_conn *next;
	struct fyai_httpd *h;
	struct fyai_netio io;
	struct fyai_event_source *src, *deadline;
	bool upgraded;		/* the WebSocket carries the frames */
	bool dispatched;	/* the handler has the request */
	bool dead;		/* free it at the next reap */
	bool closing;		/* flush the output, then end */
	bool keep;		/* the request in hand leaves the connection open */
	bool continue_sent;	/* 100 Continue went for the request in hand */
	struct fyai_buf in;	/* the request: its head, then its body */
	struct fyai_buf out;		/* bytes to the client */
	struct fyai_ws ws;
	char peer[64];
	void *user;
};

struct httpd_listener {
	struct httpd_listener *next;
	struct fyai_httpd *h;
	struct fyai_event_source *source;
	SSL_CTX *tls;
	int fd;
};

struct fyai_httpd {
	struct fyai_ctx *ctx;
	struct fyai_event_loop *loop;
	struct httpd_listener *listeners;
	struct fyai_httpd_conn *conns;
	size_t count;
	struct fyai_httpd_opts opts;
	struct fyai_httpd_ops ops;
	bool reap_queued;
};

static void httpd_log(struct fyai_httpd *h, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static void httpd_log(struct fyai_httpd *h, const char *fmt, ...)
{
	va_list ap;
	char *text;
	int len;

	if (!h->ops.log)
		return;
	va_start(ap, fmt);
	len = vasprintf(&text, fmt, ap);
	va_end(ap);
	if (len < 0)
		return;
	h->ops.log(h->ops.data, text);
	free(text);
}

static void httpd_reap_cb(void *data);

void fyai_httpd_kill(struct fyai_httpd_conn *c)
{
	struct fyai_httpd *h = c->h;

	if (c->dead)
		return;
	c->dead = true;
	/* Stop the sources now; the memory goes at the next turn of the loop. */
	fyai_event_source_remove(c->src);
	c->src = NULL;
	fyai_event_source_remove(c->deadline);
	c->deadline = NULL;
	if (h->ops.closed)
		h->ops.closed(h->ops.data, c);
	c->user = NULL;
	if (!h->reap_queued &&
	    !fyai_event_defer(h->loop, httpd_reap_cb, h))
		h->reap_queued = true;
}

static void httpd_free(struct fyai_httpd_conn *c)
{
	fyai_netio_close(&c->io);
	fyai_buf_free(&c->in);
	fyai_buf_free(&c->out);
	fyai_ws_release(&c->ws);
	free(c);
}

static void httpd_reap_cb(void *data)
{
	struct fyai_httpd *h = data;
	struct fyai_httpd_conn **link = &h->conns, *c;

	h->reap_queued = false;
	while ((c = *link)) {
		if (!c->dead) {
			link = &c->next;
			continue;
		}
		*link = c->next;
		h->count--;
		httpd_free(c);
	}
}

/* Watch what @c has to write, and what it can read. */
static void httpd_interest(struct fyai_httpd_conn *c)
{
	unsigned int events = 0;

	if (c->dead || !c->src)
		return;
	if (!c->closing || c->io.want_read)
		events |= FYAIEV_READ;
	if (fyai_buf_pending(&c->out) || c->io.want_write)
		events |= FYAIEV_WRITE;
	(void)fyai_event_fd_modify(c->src, events ? events : FYAIEV_EOF);
}

void fyai_httpd_kick(struct fyai_httpd_conn *c)
{
	httpd_interest(c);
}

void fyai_httpd_conn_set_user(struct fyai_httpd_conn *c, void *user)
{
	c->user = user;
}

void *fyai_httpd_conn_user(const struct fyai_httpd_conn *c)
{
	return c->user;
}

const char *fyai_httpd_conn_peer(const struct fyai_httpd_conn *c)
{
	return c->peer;
}

bool fyai_httpd_conn_tls(const struct fyai_httpd_conn *c)
{
	return c->io.ssl != NULL;
}

void fyai_httpd_conn_hold(struct fyai_httpd_conn *c)
{
	fyai_event_source_remove(c->deadline);
	c->deadline = NULL;
}

static enum fyai_event_action httpd_deadline_cb(const struct fyai_event *ev);
static void httpd_request(struct fyai_httpd_conn *c);

/* Start the time limit of the next request, when the server has one. */
static int httpd_arm(struct fyai_httpd_conn *c)
{
	if (!c->h->opts.head_ms || c->deadline)
		return 0;
	return fyai_event_add_timer(c->h->loop, c->h->opts.head_ms, 0,
				    httpd_deadline_cb, c, &c->deadline);
}

int fyai_httpd_conn_write(struct fyai_httpd_conn *c, const void *data, size_t n)
{
	struct fyai_ctx *ctx = c->h->ctx;
	int rc;

	if (c->dead || c->closing)
		return -1;
	/* A client that cannot be served is cut. */
	if (fyai_buf_pending(&c->out) + n > c->h->opts.max_output) {
		httpd_log(c->h, "%s %s: the client holds too much output",
			  c->io.ssl ? "https" : "http", c->peer);
		goto err_kill;
	}
	rc = fyai_buf_append_data(&c->out, data, n);
	fyai_error_check(ctx, !rc, err_kill, "cannot queue %zu bytes for a client", n);
	rc = fyai_netio_flush(&c->io, &c->out);
	if (rc)
		goto err_kill;
	httpd_interest(c);
	return 0;
err_kill:
	fyai_httpd_kill(c);
	return -1;
}

void fyai_httpd_conn_done(struct fyai_httpd_conn *c, bool close)
{
	struct fyai_ctx *ctx = c->h->ctx;
	int rc;

	if (c->dead || c->closing || c->upgraded)
		return;
	if (close || !c->keep) {
		c->closing = true;
		if (!fyai_buf_pending(&c->out))
			goto err_kill;
		httpd_interest(c);
		return;
	}
	c->dispatched = false;
	c->keep = false;
	c->continue_sent = false;
	rc = httpd_arm(c);
	fyai_error_check(ctx, !rc, err_kill, "cannot start the request timer");
	httpd_interest(c);
	/* The next request can have come with the last one. */
	if (fyai_buf_pending(&c->in))
		httpd_request(c);
	return;
err_kill:
	fyai_httpd_kill(c);
}

void fyai_httpd_respond(struct fyai_httpd_conn *c, int status, const char *why,
			const char *extra, const void *body, size_t body_len)
{
	static const struct { int code; const char *text; } names[] = {
		{ 200, "OK" }, { 400, "Bad Request" }, { 401, "Unauthorized" },
		{ 403, "Forbidden" }, { 404, "Not Found" },
		{ 405, "Method Not Allowed" }, { 426, "Upgrade Required" },
		{ 413, "Content Too Large" },
		{ 431, "Request Header Fields Too Large" },
		{ 503, "Service Unavailable" },
		{ 505, "HTTP Version Not Supported" },
	};
	char head[1024];
	size_t i, n;
	const char *name = "Error";

	if (c->dead || c->closing)
		return;
	for (i = 0; i < sizeof(names) / sizeof(*names); i++)
		if (names[i].code == status)
			name = names[i].text;
	n = (size_t)snprintf(head, sizeof(head), "HTTP/1.1 %d %s\r\n%s"
			     "Content-Length: %zu\r\nConnection: close\r\n\r\n",
			     status, name, extra ? extra : "", body_len);
	if (why)
		httpd_log(c->h, "%s %s: refused: %d %s",
			  c->io.ssl ? "https" : "http", c->peer, status, why);
	/* A client that cannot be told is cut. */
	if (n >= sizeof(head) || fyai_buf_append_data(&c->out, head, n) ||
	    (body_len && fyai_buf_append_data(&c->out, body, body_len))) {
		fyai_httpd_kill(c);
		return;
	}
	c->closing = true;
	httpd_interest(c);
}

/* A short answer that is not an upgrade, then the end. */
static void httpd_refuse(struct fyai_httpd_conn *c, int status, const char *why,
			 const char *extra)
{
	fyai_httpd_respond(c, status, why, extra, NULL, 0);
}

/* ---- the WebSocket ------------------------------------------------------- */

int fyai_httpd_ws_check(struct fyai_httpd_conn *c, const struct fyai_http_request *req)
{
	char answer[256];

	if (!req->upgrade || !req->connection_upgrade) {
		httpd_refuse(c, 426, "not an upgrade", "Upgrade: websocket\r\n");
		return -1;
	}
	if (req->ws_version != 13) {
		httpd_refuse(c, 426, "WebSocket version",
			     "Sec-WebSocket-Version: 13\r\n");
		return -1;
	}
	if (!req->ws_key) {
		httpd_refuse(c, 400, "no Sec-WebSocket-Key", NULL);
		return -1;
	}
	if (fyai_ws_server_response(req->ws_key, answer, sizeof(answer)) < 0) {
		httpd_refuse(c, 400, "bad Sec-WebSocket-Key", NULL);
		return -1;
	}
	return 0;
}

int fyai_httpd_ws_accept(struct fyai_httpd_conn *c, const struct fyai_http_request *req)
{
	char answer[256];
	int n;

	n = fyai_ws_server_response(req->ws_key, answer, sizeof(answer));
	if (n < 0 || fyai_buf_append_data(&c->out, answer, (size_t)n)) {
		fyai_httpd_kill(c);
		return -1;
	}
	fyai_httpd_conn_hold(c);
	fyai_ws_init(&c->ws, true, c->h->opts.max_message);
	c->upgraded = true;
	fyai_buf_free(&c->in);
	httpd_interest(c);
	return 0;
}

int fyai_httpd_ws_send(struct fyai_httpd_conn *c, int opcode, const void *data, size_t n)
{
	unsigned char hdr[FYAI_WS_HEADER_MAX];
	size_t h;
	char *at;

	if (fyai_buf_pending(&c->out) + n > c->h->opts.max_output)
		return -1;
	/* A frame of a server has no mask. */
	h = fyai_ws_header(hdr, opcode, true, n, NULL);
	at = fyai_buf_extend(&c->out, h + n);
	if (!at)
		return -1;
	memcpy(at, hdr, h);
	if (n)
		memcpy(at + h, data, n);
	return 0;
}

void fyai_httpd_ws_close(struct fyai_httpd_conn *c, unsigned int code)
{
	unsigned char payload[2] = { (unsigned char)(code >> 8), (unsigned char)code };

	/* A frame that cannot be queued ends the connection with no close. */
	if (!c->closing && fyai_httpd_ws_send(c, FYAI_WS_CLOSE, payload, sizeof(payload))) {
		fyai_httpd_kill(c);
		return;
	}
	c->closing = true;
	httpd_interest(c);
}

size_t fyai_httpd_ws_pending(const struct fyai_httpd_conn *c)
{
	return fyai_buf_pending(&c->out);
}

/* One message of the client. */
static void httpd_message(struct fyai_httpd_conn *c, const struct fyai_ws_message *msg)
{
	struct fyai_httpd *h = c->h;

	switch (msg->opcode) {
	case FYAI_WS_TEXT:
	case FYAI_WS_BINARY:
		h->ops.ws_message(h->ops.data, c, msg);
		return;
	case FYAI_WS_PING:
		if (fyai_httpd_ws_send(c, FYAI_WS_PONG, msg->data, msg->size))
			fyai_httpd_kill(c);
		return;
	case FYAI_WS_PONG:
		return;
	case FYAI_WS_CLOSE:
		/* Echo the status code of the client, else the normal close. */
		fyai_httpd_ws_close(c, msg->size >= 2 ?
				    (unsigned int)(msg->data[0] << 8 | msg->data[1]) : 1000);
		return;
	default:
		fyai_httpd_ws_close(c, 1003);
		return;
	}
}

/* ---- the request body ---------------------------------------------------- */

/*
 * Decode the chunked body at @p of @n bytes into @out. Returns 1 and sets
 * @used when it is complete, 0 when it needs more bytes, -1 when it is
 * malformed, -2 when it is longer than @max and -3 when memory runs out.
 */
static int http_chunked(const char *p, size_t n, size_t max, struct fyai_buf *out,
			size_t *used)
{
	size_t pos = 0, size;
	const char *eol;
	char *endp;

	for (;;) {
		eol = memchr(p + pos, '\n', n - pos);
		if (!eol)
			return 0;
		size = strtoul(p + pos, &endp, 16);
		if (endp == p + pos)
			return -1;
		pos = (size_t)(eol - p) + 1;
		if (!size)
			break;
		if (size > max || out->len + size > max)
			return -2;
		if (n - pos < size + 2)
			return 0;
		if (fyai_buf_append_data(out, p + pos, size))
			return -3;
		pos += size + 2;
	}
	/* The trailers end with an empty line. */
	for (;;) {
		eol = memchr(p + pos, '\n', n - pos);
		if (!eol)
			return 0;
		size = (size_t)(eol - (p + pos));
		pos = (size_t)(eol - p) + 1;
		if (size <= 1)
			break;
	}
	*used = pos;
	return 1;
}

/* True when a Connection header of @req names close. */
static bool http_wants_close(const struct fyai_http_request *req)
{
	size_t i;

	for (i = 0; i < req->nheaders; i++)
		if (!strcasecmp(req->headers[i].name, "Connection") &&
		    http_list_has(req->headers[i].value, "close"))
			return true;
	return false;
}

/*
 * Take the request at the front of the input, when it is whole, and give it
 * to the handler. The handler may answer, and end the connection, before this
 * returns.
 */
static void httpd_request(struct fyai_httpd_conn *c)
{
	struct fyai_httpd *h = c->h;
	struct fyai_ctx *ctx = h->ctx;
	struct fyai_http_request req;
	struct fyai_buf body = { 0 };
	const char *base, *mark, *te, *cl, *expect, *why;
	size_t pending, end, used = 0, need;
	char *stop;
	int status, rc = 1;

	memset(&req, 0, sizeof(req));
	base = c->in.data + c->in.off;
	pending = fyai_buf_pending(&c->in);
	mark = memmem(base, pending, "\r\n\r\n", 4);
	if (!mark) {
		if (pending >= FYAI_HTTP_HEAD_MAX) {
			status = 431;
			why = "the head is too long";
			goto err_respond;
		}
		goto out;
	}
	end = (size_t)(mark - base) + 4;
	status = fyai_http_parse(base, end, h->opts.http10 ? FYAI_HTTP_PARSE_HTTP10 : 0,
				 &req);
	if (status) {
		why = "bad request";
		goto err_respond;
	}
	if (!h->opts.max_body) {
		if (end != pending) {
			/* The client sent more before it was answered. */
			status = 400;
			why = "bytes after the head";
			goto err_respond;
		}
	} else {
		te = fyai_http_header(&req, "Transfer-Encoding");
		cl = fyai_http_header(&req, "Content-Length");
		if (te) {
			rc = strcasestr(te, "chunked") ?
			     http_chunked(base + end, pending - end, h->opts.max_body,
					  &body, &used) : -1;
			fyai_error_check(ctx, rc != -3, err_kill,
					 "cannot hold a chunked body");
		} else if (cl) {
			need = strtoul(cl, &stop, 10);
			if (!*cl || *stop) {
				rc = -1;
			} else if (need > h->opts.max_body) {
				rc = -2;
			} else if (pending - end < need) {
				rc = 0;
			} else {
				rc = fyai_buf_append_data(&body, base + end, need);
				fyai_error_check(ctx, !rc, err_kill,
						 "cannot hold a body of %zu bytes", need);
				rc = 1;
				used = need;
			}
		}
		if (rc < 0) {
			status = rc == -2 ? 413 : 400;
			why = rc == -2 ? "the body is too long" : "bad body";
			goto err_respond;
		}
		if (!rc) {
			/* The body is not whole: ask for it once, and wait. */
			expect = fyai_http_header(&req, "Expect");
			if (!c->continue_sent && expect &&
			    http_list_has(expect, "100-continue")) {
				c->continue_sent = true;
				fyai_httpd_conn_write(c, "HTTP/1.1 100 Continue\r\n\r\n", 25);
			}
			goto out;
		}
		rc = fyai_buf_append_data(&body, "", 0);
		fyai_error_check(ctx, !rc, err_kill, "cannot hold a body");
		req.body = body.data;
		req.body_len = body.len;
	}
	c->dispatched = true;
	c->keep = h->opts.keepalive && !req.http10 && !http_wants_close(&req);
	/* The handler can finish the request, and read the next, in this call. */
	fyai_buf_consume(&c->in, end + used);
	h->ops.request(h->ops.data, c, &req);
out:
	fyai_http_request_release(&req);
	fyai_buf_free(&body);
	return;
err_respond:
	fyai_httpd_respond(c, status, why, NULL, NULL, 0);
	goto out;
err_kill:
	fyai_httpd_kill(c);
	goto out;
}

/* ---- the events ---------------------------------------------------------- */

/* Bytes of the client. */
static void httpd_read(struct fyai_httpd_conn *c)
{
	unsigned char chunk[16384];
	struct fyai_ctx *ctx = c->h->ctx;
	struct fyai_ws_message msg;
	size_t used, got, off;
	ssize_t n;
	int rc, rounds;

	for (rounds = 0; (rounds < 16 || fyai_netio_buffered(&c->io)) &&
	     !c->dead && !c->closing; rounds++) {
		n = fyai_netio_recv(&c->io, chunk, sizeof(chunk));
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return;
		if (n <= 0)
			goto err_kill;
		got = (size_t)n;
		if (!c->upgraded) {
			rc = fyai_buf_append_data(&c->in, chunk, got);
			fyai_error_check(ctx, !rc, err_kill,
					 "cannot hold %zu bytes of a request", got);
			if (c->dispatched) {
				/* A client may send its next request before the answer. */
				if (!c->h->opts.keepalive ||
				    fyai_buf_pending(&c->in) > FYAI_HTTP_HEAD_MAX +
				    2 * c->h->opts.max_body + 4096)
					fyai_httpd_respond(c, c->h->opts.keepalive ? 413 : 400,
							   "bytes after the head", NULL, NULL, 0);
				continue;
			}
			httpd_request(c);
			continue;
		}
		for (off = 0; off < got && !c->closing && !c->dead;) {
			rc = fyai_ws_feed(&c->ws, chunk + off, got - off, &used, &msg);
			off += used;
			if (rc < 0) {
				fyai_httpd_ws_close(c, 1002);
				return;
			}
			if (rc == 0)
				break;
			httpd_message(c, &msg);
		}
	}
	return;
err_kill:
	fyai_httpd_kill(c);
}

static enum fyai_event_action httpd_conn_cb(const struct fyai_event *ev)
{
	struct fyai_httpd_conn *c = ev->userdata;

	if (c->dead)
		return FYAIEA_CONTINUE;
	if (ev->events & FYAIEV_ERROR) {
		fyai_httpd_kill(c);
		return FYAIEA_CONTINUE;
	}
	if ((ev->events & FYAIEV_WRITE) && fyai_netio_flush(&c->io, &c->out)) {
		fyai_httpd_kill(c);
		return FYAIEA_CONTINUE;
	}
	if (c->closing && !fyai_buf_pending(&c->out)) {
		fyai_httpd_kill(c);
		return FYAIEA_CONTINUE;
	}
	/* A handshake that waits for a write goes on when the socket allows it. */
	if ((ev->events & (FYAIEV_READ | FYAIEV_EOF)) ||
	    (c->io.ssl && !c->io.handshaken))
		httpd_read(c);
	if (c->dead)
		return FYAIEA_CONTINUE;
	httpd_interest(c);
	if (c->upgraded && !c->closing && c->h->ops.ws_drain)
		c->h->ops.ws_drain(c->h->ops.data, c);
	return FYAIEA_CONTINUE;
}

static enum fyai_event_action httpd_deadline_cb(const struct fyai_event *ev)
{
	struct fyai_httpd_conn *c = ev->userdata;

	/* The timer is one shot and goes with its connection. */
	c->deadline = NULL;
	httpd_log(c->h, "%s %s: no request in time", c->io.ssl ? "https" : "http",
		  c->peer);
	fyai_httpd_kill(c);
	return FYAIEA_CONTINUE;
}

static enum fyai_event_action httpd_accept_cb(const struct fyai_event *ev)
{
	struct httpd_listener *listener = ev->userdata;
	struct fyai_httpd *h = listener->h;
	struct fyai_httpd_conn *c;
	int fd, one = 1, rc;

	fd = accept(listener->fd, NULL, NULL);
	if (fd < 0)
		return FYAIEA_CONTINUE;
	if (fyai_net_cloexec(fd) || fyai_net_nonblock(fd) ||
	    h->count >= h->opts.max_conns) {
		close(fd);
		return FYAIEA_CONTINUE;
	}
	(void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	fyai_net_nosigpipe(fd);
	c = calloc(1, sizeof(*c));
	if (!c) {
		close(fd);
		return FYAIEA_CONTINUE;
	}
	c->h = h;
	fyai_netio_init(&c->io, fd);
	if (listener->tls && fyai_netio_tls_accept(&c->io, listener->tls)) {
		close(fd);
		free(c);
		return FYAIEA_CONTINUE;
	}
	fyai_net_peer_text(fd, c->peer, sizeof(c->peer));
	c->next = h->conns;
	h->conns = c;
	h->count++;
	rc = fyai_event_add_fd(h->loop, fd, FYAIEV_READ,
			       httpd_conn_cb, c, &c->src);
	if (!rc)
		rc = httpd_arm(c);
	if (rc)
		fyai_httpd_kill(c);
	return FYAIEA_CONTINUE;
}

struct fyai_httpd *fyai_httpd_new(struct fyai_ctx *ctx,
				  const struct fyai_httpd_opts *opts,
				  const struct fyai_httpd_ops *ops)
{
	struct fyai_httpd *h = calloc(1, sizeof(*h));

	fyai_error_check(ctx, h, err, "cannot allocate the HTTP server");
	h->ctx = ctx;
	h->opts = *opts;
	h->loop = opts->loop ? opts->loop : fyai_ctx_loop(ctx);
	h->ops = *ops;
	return h;
err:
	return NULL;
}

int fyai_httpd_adopt(struct fyai_httpd *h, int fd, SSL_CTX *tls)
{
	struct fyai_ctx *ctx = h->ctx;
	struct httpd_listener *listener;
	int rc;

	listener = calloc(1, sizeof(*listener));
	fyai_error_check(ctx, listener, err, "cannot allocate the HTTP listener");
	listener->h = h;
	listener->fd = fd;
	if (tls) {
		fyai_error_check(ctx, SSL_CTX_up_ref(tls) == 1, err,
				 "cannot keep the TLS context");
		listener->tls = tls;
	}
	rc = fyai_event_add_fd(h->loop, fd, FYAIEV_READ, httpd_accept_cb,
			       listener, &listener->source);
	fyai_error_check(ctx, !rc, err, "cannot watch the listener");
	listener->next = h->listeners;
	h->listeners = listener;
	return 0;
err:
	if (listener) {
		SSL_CTX_free(listener->tls);
		free(listener);
	}
	return -1;
}

int fyai_httpd_listen(struct fyai_httpd *h, const char *address, SSL_CTX *tls,
		      char *bound, size_t bound_size)
{
	int fd = fyai_net_listen(h->ctx, address, bound, bound_size);

	if (fd < 0)
		return -1;
	if (fyai_httpd_adopt(h, fd, tls)) {
		close(fd);
		return -1;
	}
	return 0;
}

void fyai_httpd_stop(struct fyai_httpd *h, struct fyai_httpd_conn *keep)
{
	struct httpd_listener *listener;
	struct fyai_httpd_conn *c;

	while ((listener = h->listeners)) {
		h->listeners = listener->next;
		fyai_event_source_remove(listener->source);
		close(listener->fd);
		SSL_CTX_free(listener->tls);
		free(listener);
	}
	for (c = h->conns; c; c = c->next)
		if (c != keep)
			fyai_httpd_kill(c);
}

void fyai_httpd_close(struct fyai_httpd *h)
{
	if (!h)
		return;
	fyai_httpd_stop(h, NULL);
	fyai_event_defer_cancel(h->loop, httpd_reap_cb, h);
	httpd_reap_cb(h);
	free(h);
}
