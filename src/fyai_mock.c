/*
 * fyai_mock.c - scenario-driven mock provider for the test suite
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * One thread, one event loop. A reply is a list of operations (write bytes,
 * sleep, wait for a marker file, shut the connection down) that a connection
 * runs in order, so a held reply never blocks another connection. The
 * scenario format is described in doc/mock-provider.md.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "fyai.h"
#include "fyai_config.h"
#include "fyai_event.h"
#include "fyai_mock.h"
#include "fyai_sink.h"
#include "utils.h"

#define FYAI_MODULE FYAIEM_UNKNOWN

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define MOCK_BACKLOG		256
#define MOCK_MAX_HEAD		(64 * 1024)
#define MOCK_POLL_MS		10
#define MOCK_HOLD_MS		30000	/* before a reply; scaled */
#define MOCK_STREAM_HOLD_MS	5000	/* inside a stream; not scaled */
#define MOCK_TRUNCATE_EXTRA	64	/* bytes a truncated reply promises */
#define MOCK_EXHAUSTED \
	"{\"error\":{\"message\":\"mock: scenario exhausted\"}}"

enum mock_op_kind {
	MOCK_WRITE,
	MOCK_SLEEP,
	MOCK_MARKER,
	MOCK_SHUTDOWN,
};

/* Names the marker for MOCK_MARKER; @ms is the sleep or the deadline. */
struct mock_op {
	enum mock_op_kind kind;
	char *data;
	size_t len;
	fyai_event_ms_t ms;
};

struct mock {
	struct fyai_ctx *ctx;
	struct fy_generic_builder *gb;
	struct fyai_event_loop *el;
	struct fyai_event_source *listen_src;
	const char *rundir;
	fy_generic steps;
	size_t nsteps;
	bool *consumed;
	bool matched;		/* some step has `match` */
	bool log;		/* report each request */
	size_t served;
	double scale;		/* FYAI_TIMEOUT_SCALE */
	unsigned short port;
	int listen_fd;
};

struct mock_conn {
	struct mock *m;
	int fd;
	struct fyai_event_source *src;
	struct fyai_event_source *timer;
	unsigned short peer_port;

	char *in;
	size_t in_len;
	size_t in_cap;
	bool continue_sent;

	/* A reply in progress: the connection reads nothing until it ends. */
	bool busy;
	bool close_after;
	struct mock_op *ops;
	size_t nops;
	size_t cap_ops;
	size_t pos;
	size_t woff;		/* bytes of ops[pos] already sent */
	fyai_event_ms_t deadline;
};

struct mock_req {
	char method[16];
	char target[4096];
	const char *hdrs;
	size_t hlen;
	char *body;		/* NUL terminated, owned */
	size_t body_len;
	size_t consumed;
	bool http10;
};

/* Request fields a `match` can name; unnamed keys compare as "". */
struct mock_fields {
	const char *http;
	const char *path;
	const char *method;
	const char *auth;
	const char *session;
	const char *body;
};

static void conn_pump(struct mock_conn *c);
static void conn_feed(struct mock_conn *c);

/* ---- text buffer ----------------------------------------------------------- */

static int buf_addf(struct fyai_buf *buf, const char *fmt, ...)
{
	va_list ap, ap2;
	char *tmp;
	int n, rc;

	va_start(ap, fmt);
	va_copy(ap2, ap);
	n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	tmp = n < 0 ? NULL : malloc((size_t)n + 1);
	if (tmp)
		vsnprintf(tmp, (size_t)n + 1, fmt, ap2);
	va_end(ap2);
	if (!tmp)
		return -1;
	rc = fyai_buf_append_data(buf, tmp, (size_t)n);
	free(tmp);
	return rc;
}

/* ---- run directory files --------------------------------------------------- */

static void rundir_path(const struct mock *m, const char *name, char *buf,
			size_t size)
{
	snprintf(buf, size, "%s/%s", m->rundir, name);
}

static int file_write(const char *path, int flags, const void *data, size_t n)
{
	ssize_t w;
	int fd;

	fd = open(path, flags | O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
	if (fd < 0)
		return -1;
	/* One write keeps a record whole when several processes append. */
	w = n ? write(fd, data, n) : 0;
	close(fd);
	return w == (ssize_t)n ? 0 : -1;
}

static void marker_failed(const struct mock *m)
{
	char path[4096];

	rundir_path(m, "stream-wait-failed", path, sizeof(path));
	(void)file_write(path, O_TRUNC, NULL, 0);
}

static bool marker_exists(const struct mock *m, const char *name)
{
	char path[4096];

	rundir_path(m, name, path, sizeof(path));
	return !access(path, F_OK);
}

/* ---- operations of a reply ------------------------------------------------- */

static int op_add(struct mock_conn *c, enum mock_op_kind kind, const void *data,
		  size_t len, fyai_event_ms_t ms)
{
	struct mock_op *ops, *op;
	char *copy = NULL;

	if (c->nops == c->cap_ops) {
		ops = realloc(c->ops, (c->cap_ops ? c->cap_ops * 2 : 8) *
			      sizeof(*ops));
		if (!ops)
			return -1;
		c->ops = ops;
		c->cap_ops = c->cap_ops ? c->cap_ops * 2 : 8;
	}
	if (data) {
		copy = malloc(len + 1);
		if (!copy)
			return -1;
		memcpy(copy, data, len);
		copy[len] = '\0';
	}
	op = &c->ops[c->nops++];
	op->kind = kind;
	op->data = copy;
	op->len = len;
	op->ms = ms;
	return 0;
}

static void ops_free(struct mock_conn *c)
{
	size_t i;

	for (i = 0; i < c->nops; i++)
		free(c->ops[i].data);
	free(c->ops);
	c->ops = NULL;
	c->nops = c->cap_ops = c->pos = c->woff = 0;
}

static const char *status_text(int status)
{
	switch (status) {
	case 200: return "OK";
	case 201: return "Created";
	case 202: return "Accepted";
	case 204: return "No Content";
	case 400: return "Bad Request";
	case 401: return "Unauthorized";
	case 403: return "Forbidden";
	case 404: return "Not Found";
	case 408: return "Request Timeout";
	case 429: return "Too Many Requests";
	case 500: return "Internal Server Error";
	case 501: return "Not Implemented";
	case 502: return "Bad Gateway";
	case 503: return "Service Unavailable";
	case 504: return "Gateway Timeout";
	default: return "Status";
	}
}

/* @extra holds complete header lines. */
static int op_head(struct mock_conn *c, int status, const char *ctype,
		   const char *extra, size_t clen)
{
	struct fyai_buf sb = { 0 };
	int rc;

	rc = buf_addf(&sb, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\n", status,
		     status_text(status), ctype);
	if (!rc && extra)
		rc = fyai_buf_append(&sb, extra);
	if (!rc && c->close_after)
		rc = fyai_buf_append(&sb, "Connection: close\r\n");
	if (!rc)
		rc = buf_addf(&sb, "Content-Length: %zu\r\n\r\n", clen);
	if (!rc)
		rc = op_add(c, MOCK_WRITE, sb.data, sb.len, 0);
	free(sb.data);
	return rc;
}

static int op_reply(struct mock_conn *c, int status, const char *ctype,
		    const char *extra, const char *body, size_t blen)
{
	struct fyai_buf sb = { 0 };
	int rc;

	rc = buf_addf(&sb, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\n", status,
		     status_text(status), ctype);
	if (!rc && extra)
		rc = fyai_buf_append(&sb, extra);
	if (!rc && c->close_after)
		rc = fyai_buf_append(&sb, "Connection: close\r\n");
	if (!rc)
		rc = buf_addf(&sb, "Content-Length: %zu\r\n\r\n", blen);
	if (!rc)
		rc = fyai_buf_append_data(&sb, body, blen);
	if (!rc)
		rc = op_add(c, MOCK_WRITE, sb.data, sb.len, 0);
	free(sb.data);
	return rc;
}

/* ---- connections ----------------------------------------------------------- */

static void conn_close(struct mock_conn *c)
{
	fyai_event_source_remove(c->timer);
	fyai_event_source_remove(c->src);
	if (c->fd >= 0)
		close(c->fd);
	ops_free(c);
	free(c->in);
	free(c);
}

/* Send what @c owes of the current write; 1 when the socket is full. */
static int conn_send(struct mock_conn *c, struct mock_op *op)
{
	ssize_t n;

	while (c->woff < op->len) {
		n = send(c->fd, op->data + c->woff, op->len - c->woff,
			 MSG_NOSIGNAL);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return 1;
			return -1;
		}
		c->woff += (size_t)n;
	}
	return 0;
}

static void conn_idle(struct mock_conn *c)
{
	ops_free(c);
	c->busy = false;
	c->continue_sent = false;
	fyai_event_timer_disarm(c->timer);
	fyai_event_fd_modify(c->src, FYAIEV_READ);
	/* The next request can have arrived with the last one. */
	if (c->in_len)
		conn_feed(c);
}

static enum fyai_event_action conn_on_timer(const struct fyai_event *ev)
{
	struct mock_conn *c = ev->userdata;
	struct mock_op *op;

	if (!c->busy || c->pos >= c->nops)
		return FYAIEA_CONTINUE;
	op = &c->ops[c->pos];
	if (op->kind == MOCK_MARKER) {
		if (marker_exists(c->m, op->data))
			goto done;
		if (fyai_event_now_ms() < c->deadline)
			return FYAIEA_CONTINUE;
		marker_failed(c->m);
	}
done:
	fyai_event_timer_disarm(c->timer);
	c->pos++;
	conn_pump(c);
	return FYAIEA_CONTINUE;
}

static void conn_arm(struct mock_conn *c, fyai_event_ms_t first,
		     fyai_event_ms_t interval)
{
	int rc;

	if (c->timer) {
		rc = fyai_event_timer_rearm(c->timer, first, interval);
		if (rc)
			conn_close(c);
		return;
	}
	rc = fyai_event_add_timer(c->m->el, first, interval, conn_on_timer, c,
				  &c->timer);
	if (rc)
		conn_close(c);
}

/*
 * Run the operations of the reply until one has to wait. The connection may
 * be gone when this returns.
 */
static void conn_pump(struct mock_conn *c)
{
	struct mock_op *op;
	int rc;

	while (c->pos < c->nops) {
		op = &c->ops[c->pos];
		switch (op->kind) {
		case MOCK_WRITE:
			rc = conn_send(c, op);
			if (rc < 0) {
				conn_close(c);
				return;
			}
			if (rc) {
				fyai_event_fd_modify(c->src, FYAIEV_READ | FYAIEV_WRITE);
				return;
			}
			c->woff = 0;
			c->pos++;
			break;
		case MOCK_SLEEP:
			conn_arm(c, op->ms, 0);
			return;
		case MOCK_MARKER:
			if (marker_exists(c->m, op->data)) {
				c->pos++;
				break;
			}
			c->deadline = fyai_event_now_ms() + op->ms;
			conn_arm(c, MOCK_POLL_MS, MOCK_POLL_MS);
			return;
		case MOCK_SHUTDOWN:
			shutdown(c->fd, SHUT_WR);
			c->close_after = true;
			c->pos++;
			break;
		}
	}
	if (c->close_after) {
		conn_close(c);
		return;
	}
	conn_idle(c);
}

/* Read what the peer sent into the input; -1 when the connection is over. */
static int conn_fill(struct mock_conn *c)
{
	size_t cap;
	ssize_t n;
	char *p;

	if (c->in_len + 4096 > c->in_cap) {
		cap = c->in_cap ? c->in_cap * 2 : 8192;
		p = realloc(c->in, cap);
		if (!p)
			return -1;
		c->in = p;
		c->in_cap = cap;
	}
	n = recv(c->fd, c->in + c->in_len, c->in_cap - c->in_len - 1, 0);
	if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
		return 0;
	if (n <= 0)
		return -1;
	c->in_len += (size_t)n;
	return 0;
}

static enum fyai_event_action conn_on_event(const struct fyai_event *ev)
{
	struct mock_conn *c = ev->userdata;

	/* Input that arrives during a reply is kept for the next request. */
	if (ev->events & FYAIEV_READ) {
		if (conn_fill(c)) {
			conn_close(c);
			return FYAIEA_CONTINUE;
		}
		if (!c->busy)
			conn_feed(c);
		return FYAIEA_CONTINUE;
	}
	if (ev->events & (FYAIEV_EOF | FYAIEV_ERROR)) {
		conn_close(c);
		return FYAIEA_CONTINUE;
	}
	if (c->busy && (ev->events & FYAIEV_WRITE)) {
		fyai_event_fd_modify(c->src, FYAIEV_READ);
		conn_pump(c);
	}
	return FYAIEA_CONTINUE;
}

/* ---- request parsing ------------------------------------------------------- */

static const char *find_blank(const char *p, size_t n)
{
	size_t i;

	for (i = 0; i + 4 <= n; i++)
		if (p[i] == '\r' && p[i + 1] == '\n' && p[i + 2] == '\r' &&
		    p[i + 3] == '\n')
			return p + i;
	return NULL;
}

/* Copy the value of header @name of @hdrs into @out; "" when absent. */
static void hdr_get(const char *hdrs, size_t hlen, const char *name, char *out,
		    size_t size)
{
	size_t nlen = strlen(name), vlen;
	const char *line = hdrs, *end = hdrs + hlen, *eol, *colon, *v;

	out[0] = '\0';
	while (line < end) {
		eol = memchr(line, '\n', (size_t)(end - line));
		if (!eol)
			eol = end;
		colon = memchr(line, ':', (size_t)(eol - line));
		if (colon && (size_t)(colon - line) == nlen &&
		    !strncasecmp(line, name, nlen)) {
			v = colon + 1;
			while (v < eol && (*v == ' ' || *v == '\t'))
				v++;
			vlen = (size_t)(eol - v);
			while (vlen && (v[vlen - 1] == '\r' ||
					v[vlen - 1] == ' ' ||
					v[vlen - 1] == '\t'))
				vlen--;
			if (vlen >= size)
				vlen = size - 1;
			memcpy(out, v, vlen);
			out[vlen] = '\0';
			return;
		}
		line = eol + 1;
	}
}

/*
 * Decode a chunked body at @p. Return 1 and set @bodyp, @lenp and @usedp when
 * complete, 0 when more bytes are needed, -1 when malformed.
 */
static int chunked_decode(const char *p, size_t n, char **bodyp, size_t *lenp,
			  size_t *usedp)
{
	struct fyai_buf sb = { 0 };
	size_t pos = 0, size;
	const char *eol;
	char *endp;
	int rc = 0;

	for (;;) {
		eol = memchr(p + pos, '\n', n - pos);
		if (!eol)
			goto more;
		size = strtoul(p + pos, &endp, 16);
		if (endp == p + pos)
			goto bad;
		pos = (size_t)(eol - p) + 1;
		if (!size)
			break;
		if (n - pos < size + 2)
			goto more;
		if (fyai_buf_append_data(&sb, p + pos, size))
			goto bad;
		pos += size + 2;
	}
	/* Trailers end with an empty line. */
	for (;;) {
		eol = memchr(p + pos, '\n', n - pos);
		if (!eol)
			goto more;
		size = (size_t)(eol - (p + pos));
		pos = (size_t)(eol - p) + 1;
		if (size <= 1)
			break;
	}
	if (!sb.data && fyai_buf_append_data(&sb, "", 0))
		goto bad;
	*bodyp = sb.data;
	*lenp = sb.len;
	*usedp = pos;
	return 1;
more:
	free(sb.data);
	return rc;
bad:
	free(sb.data);
	return -1;
}

/*
 * Parse the request at the start of the input of @c. Return 1 when complete,
 * 0 when more bytes are needed, -1 when malformed.
 */
static int req_parse(struct mock_conn *c, struct mock_req *r)
{
	const char *blank, *eol, *sp1, *sp2, *base = c->in;
	char te[64], cl[32];
	size_t head, tlen, mlen, need, used;
	int rc;

	memset(r, 0, sizeof(*r));
	c->in[c->in_len] = '\0';
	blank = find_blank(base, c->in_len);
	if (!blank)
		return c->in_len > MOCK_MAX_HEAD ? -1 : 0;
	head = (size_t)(blank - base) + 4;

	eol = memchr(base, '\n', head);
	sp1 = memchr(base, ' ', (size_t)(eol - base));
	if (!sp1)
		return -1;
	sp2 = memchr(sp1 + 1, ' ', (size_t)(eol - sp1 - 1));
	if (!sp2)
		return -1;
	mlen = (size_t)(sp1 - base);
	tlen = (size_t)(sp2 - sp1 - 1);
	if (mlen >= sizeof(r->method) || tlen >= sizeof(r->target))
		return -1;
	memcpy(r->method, base, mlen);
	memcpy(r->target, sp1 + 1, tlen);
	r->http10 = !strncmp(sp2 + 1, "HTTP/1.0", 8);
	r->hdrs = eol + 1;
	r->hlen = head - (size_t)(eol + 1 - base);

	hdr_get(r->hdrs, r->hlen, "Transfer-Encoding", te, sizeof(te));
	if (strcasestr(te, "chunked")) {
		rc = chunked_decode(base + head, c->in_len - head, &r->body,
				    &r->body_len, &used);
		if (rc < 0)
			return -1;
		if (!rc)
			goto incomplete;
		r->consumed = head + used;
		return 1;
	}
	hdr_get(r->hdrs, r->hlen, "Content-Length", cl, sizeof(cl));
	need = cl[0] ? strtoul(cl, NULL, 10) : 0;
	if (c->in_len - head < need)
		goto incomplete;
	r->body = malloc(need + 1);
	if (!r->body)
		return -1;
	memcpy(r->body, base + head, need);
	r->body[need] = '\0';
	r->body_len = need;
	r->consumed = head + need;
	return 1;

incomplete:
	hdr_get(r->hdrs, r->hlen, "Expect", te, sizeof(te));
	if (!c->continue_sent && strcasestr(te, "100-continue")) {
		c->continue_sent = true;
		(void)send(c->fd, "HTTP/1.1 100 Continue\r\n\r\n", 25,
			   MSG_NOSIGNAL);
	}
	return 0;
}

/* ---- scenario -------------------------------------------------------------- */

static bool is_set(fy_generic v)
{
	return fy_is_valid(v) && !fy_is_null(v);
}

static const char *field_value(const struct mock_fields *f, const char *key)
{
	if (!strcmp(key, "http"))
		return f->http;
	if (!strcmp(key, "path"))
		return f->path;
	if (!strcmp(key, "method"))
		return f->method;
	if (!strcmp(key, "auth"))
		return f->auth;
	if (!strcmp(key, "session"))
		return f->session;
	return "";
}

static bool step_matches(fy_generic step, const struct mock_fields *f)
{
	fy_generic match, key, value;
	const char *k, *v;
	bool ok = true;

	match = fy_get(step, "match", fy_map_empty);
	fy_foreach_key_value(key, value, match) {
		k = fy_castp(&key, "");
		if (!fy_is_string(value)) {
			ok = false;
			continue;
		}
		v = fy_castp(&value, "");
		/* A substring test on the raw body separates requests that
		 * differ only in content. */
		if (!strcmp(k, "body_contains"))
			ok = ok && strstr(f->body, v);
		else
			ok = ok && !strcmp(field_value(f, k), v);
	}
	return ok;
}

/* The step for a request, or fy_invalid when the scenario ran out. */
static fy_generic mock_pick(struct mock *m, const struct mock_fields *f)
{
	fy_generic step;
	size_t i, j, used;

	if (!m->matched) {
		i = m->served++;
		return i < m->nsteps ? fy_get_at(m->steps, i) : fy_invalid;
	}
	for (i = 0; i < m->nsteps; i++) {
		if (m->consumed[i])
			continue;
		step = fy_get_at(m->steps, i);
		if (!step_matches(step, f))
			continue;
		m->consumed[i] = true;
		for (used = 0, j = 0; j < m->nsteps; j++)
			used += m->consumed[j];
		m->served = used;
		return step;
	}
	return fy_invalid;
}

static void mock_note(struct mock *m, const char *line, size_t n)
{
	char path[4096], num[32];
	int len;

	rundir_path(m, "requests.jsonl", path, sizeof(path));
	(void)file_write(path, O_APPEND, line, n);
	rundir_path(m, "served", path, sizeof(path));
	len = snprintf(num, sizeof(num), "%zu\n", m->served);
	(void)file_write(path, O_TRUNC, num, (size_t)len);
}

/* ---- replies --------------------------------------------------------------- */

static fyai_event_ms_t secs_to_ms(double s)
{
	return (fyai_event_ms_t)(s * 1000.0);
}

static char *expand_base(const struct mock *m, const char *value)
{
	static const char tag[] = "{{BASE_URL}}";
	struct fyai_buf sb = { 0 };
	const char *p = value, *hit;
	int rc = 0;

	while (!rc && (hit = strstr(p, tag))) {
		rc = fyai_buf_append_data(&sb, p, (size_t)(hit - p));
		if (!rc)
			rc = buf_addf(&sb, "http://127.0.0.1:%u", m->port);
		p = hit + sizeof(tag) - 1;
	}
	if (!rc)
		rc = fyai_buf_append(&sb, p);
	if (rc) {
		free(sb.data);
		return NULL;
	}
	return sb.data;
}

/* The JSON text of the `response` of @step, or `raw` verbatim. */
static const char *step_body(struct mock *m, fy_generic step)
{
	fy_generic raw, resp;
	const char *s;

	raw = fy_get(step, "raw", fy_invalid);
	if (fy_is_string(raw))
		return fy_gb_intern_string(m->gb, fy_castp(&raw, ""));
	resp = fy_get(step, "response", fy_map_empty);
	s = emit_json_string(m->gb, resp);
	return s ? s : "{}";
}

static int event_text(struct mock *m, fy_generic ev, struct fyai_buf *sb)
{
	fy_generic type;
	const char *json;
	int rc = 0;

	if (fy_is_mapping(ev)) {
		type = fy_get(ev, "type", fy_invalid);
		if (fy_is_string(type))
			rc = buf_addf(sb, "event: %s\n", fy_castp(&type, ""));
	}
	json = emit_json_string(m->gb, ev);
	if (!json)
		return -1;
	if (!rc)
		rc = buf_addf(sb, "data: %s\n\n", json);
	return rc;
}

/* Build the SSE parts; @ends gets the end offset of each in the payload. */
static int sse_build(struct mock *m, fy_generic step, bool truncated,
		     struct fyai_buf *payload, size_t **endsp, size_t *nendsp)
{
	fy_generic events, raw, ev, line;
	size_t *ends = NULL, *grown, n = 0, cap = 0, limit;
	long long close_after;
	const char *s;
	int rc = 0;

	events = fy_get(step, "sse", fy_seq_empty);
	raw = fy_get(step, "raw_sse", fy_seq_empty);
	close_after = fy_get(step, "close_after", 0LL);
	limit = (size_t)-1;
	if (truncated)
		limit = close_after < 0 ? 0 : (size_t)close_after;

	fy_foreach(ev, events) {
		if (n >= limit)
			break;
		rc = event_text(m, ev, payload);
		if (rc)
			goto out;
		if (n == cap) {
			cap = cap ? cap * 2 : 16;
			grown = realloc(ends, cap * sizeof(*ends));
			if (!grown)
				goto nomem;
			ends = grown;
		}
		ends[n++] = payload->len;
	}
	fy_foreach(line, raw) {
		s = fy_castp(&line, "");
		rc = buf_addf(payload, "%s\n", s);
		if (rc)
			goto out;
		if (n == cap) {
			cap = cap ? cap * 2 : 16;
			grown = realloc(ends, cap * sizeof(*ends));
			if (!grown)
				goto nomem;
			ends = grown;
		}
		ends[n++] = payload->len;
	}
	if (fy_get(step, "done", false) && !truncated) {
		rc = fyai_buf_append(payload, "data: [DONE]\n\n");
		if (rc)
			goto out;
		if (n == cap) {
			cap = cap ? cap * 2 : 16;
			grown = realloc(ends, cap * sizeof(*ends));
			if (!grown)
				goto nomem;
			ends = grown;
		}
		ends[n++] = payload->len;
	}
	*endsp = ends;
	*nendsp = n;
	return 0;
nomem:
	rc = -1;
out:
	free(ends);
	return rc;
}

static int reply_sse(struct mock_conn *c, fy_generic step, int status)
{
	struct mock *m = c->m;
	struct fyai_buf payload = { 0 };
	size_t *ends = NULL, n = 0, i, pos, split;
	fy_generic wait_after, wait_for, splits, v;
	fyai_event_ms_t delay_ms;
	bool truncated;
	long long wa;
	char marker[256];
	int rc;

	truncated = is_set(fy_get(step, "close_after", fy_invalid));
	rc = sse_build(m, step, truncated, &payload, &ends, &n);
	if (rc)
		goto out;
	rc = fyai_buf_append_data(&payload, "", 0);
	if (rc)
		goto out;
	rc = op_head(c, status, "text/event-stream", NULL,
		     payload.len + (truncated ? MOCK_TRUNCATE_EXTRA : 0));
	if (rc)
		goto out;

	wait_after = fy_get(step, "wait_after_event", fy_invalid);
	wait_for = fy_get(step, "wait_for", fy_invalid);
	if (is_set(wait_after) && fy_is_string(wait_for)) {
		snprintf(marker, sizeof(marker), "%s", fy_castp(&wait_for, ""));
		wa = fy_number(wait_after, 0LL);
		for (i = 0, pos = 0; i < n && !rc; pos = ends[i++]) {
			rc = op_add(c, MOCK_WRITE, payload.data + pos,
				    ends[i] - pos, 0);
			if (!rc && (long long)i == wa)
				rc = op_add(c, MOCK_MARKER, marker,
					    strlen(marker), MOCK_STREAM_HOLD_MS);
		}
		goto out;
	}

	splits = fy_get(step, "chunk_split", fy_seq_empty);
	delay_ms = secs_to_ms(fy_number(fy_get(step, "chunk_delay", fy_invalid),
					0.0));
	pos = 0;
	fy_foreach(v, splits) {
		split = (size_t)fy_number(v, 0LL);
		if (split <= pos || split >= payload.len)
			continue;
		rc = op_add(c, MOCK_WRITE, payload.data + pos, split - pos, 0);
		if (rc)
			goto out;
		pos = split;
		if (delay_ms) {
			rc = op_add(c, MOCK_SLEEP, NULL, 0, delay_ms);
			if (rc)
				goto out;
		}
	}
	if (payload.len > pos)
		rc = op_add(c, MOCK_WRITE, payload.data + pos, payload.len - pos, 0);
	if (!rc && truncated)
		rc = op_add(c, MOCK_SHUTDOWN, NULL, 0, 0);
out:
	free(ends);
	free(payload.data);
	return rc;
}

/* The lines `Key: value` of the `headers` of @step, with the base URL set. */
static int step_headers(struct mock *m, fy_generic step, struct fyai_buf *sb)
{
	fy_generic headers, key, value;
	char *text;
	int rc = 0;

	headers = fy_get(step, "headers", fy_map_empty);
	fy_foreach_key_value(key, value, headers) {
		if (rc || !fy_is_string(key) || !fy_is_string(value))
			continue;
		text = expand_base(m, fy_castp(&value, ""));
		if (!text) {
			rc = -1;
			continue;
		}
		rc = buf_addf(sb, "%s: %s\r\n", fy_castp(&key, ""), text);
		free(text);
	}
	return rc;
}

static int reply_exhausted(struct mock_conn *c)
{
	return op_reply(c, 500, "application/json", NULL, MOCK_EXHAUSTED,
			strlen(MOCK_EXHAUSTED));
}

static int reply_post(struct mock_conn *c, fy_generic step)
{
	struct mock *m = c->m;
	struct fyai_buf extra = { 0 };
	fy_generic wait_for, wait_after;
	const char *body;
	char marker[256];
	double delay;
	int status, rc;

	if (!fy_is_mapping(step))
		return reply_exhausted(c);

	/* A marker without an event index holds the whole reply until the case
	 * creates it: the case decides when the reply may arrive. */
	wait_for = fy_get(step, "wait_for", fy_invalid);
	wait_after = fy_get(step, "wait_after_event", fy_invalid);
	if (fy_is_string(wait_for) && !is_set(wait_after)) {
		snprintf(marker, sizeof(marker), "%s", fy_castp(&wait_for, ""));
		rc = op_add(c, MOCK_MARKER, marker, strlen(marker),
			    secs_to_ms(MOCK_HOLD_MS / 1000.0 * m->scale));
		if (rc)
			return rc;
	}
	/* The client waits with no byte of the reply yet. */
	delay = fy_number(fy_get(step, "delay", fy_invalid), 0.0);
	if (delay > 0) {
		rc = op_add(c, MOCK_SLEEP, NULL, 0, secs_to_ms(delay));
		if (rc)
			return rc;
	}

	status = (int)fy_number(fy_get(step, "status", fy_invalid), 200LL);
	if (fy_is_valid(fy_get(step, "sse", fy_invalid)) ||
	    fy_is_valid(fy_get(step, "raw_sse", fy_invalid)))
		return reply_sse(c, step, status);

	rc = step_headers(m, step, &extra);
	if (!rc) {
		body = step_body(m, step);
		rc = op_reply(c, status, "application/json", extra.data, body,
			      strlen(body));
	}
	free(extra.data);
	return rc;
}

static int reply_get(struct mock_conn *c, fy_generic step)
{
	const char *body;
	char *text;
	int status, rc;

	if (!fy_is_mapping(step))
		return reply_exhausted(c);
	status = (int)fy_number(fy_get(step, "status", fy_invalid), 200LL);
	body = step_body(c->m, step);
	text = expand_base(c->m, body);
	if (!text)
		return -1;
	rc = op_reply(c, status, "application/json", NULL, text, strlen(text));
	free(text);
	return rc;
}

static int reply_delete(struct mock_conn *c, fy_generic step)
{
	fy_generic raw;
	const char *body = "";
	int status;

	if (!fy_is_mapping(step))
		return reply_exhausted(c);
	status = (int)fy_number(fy_get(step, "status", fy_invalid), 200LL);
	raw = fy_get(step, "raw", fy_invalid);
	if (fy_is_string(raw))
		body = fy_gb_intern_string(c->m->gb, fy_castp(&raw, ""));
	return op_reply(c, status, "application/json", NULL, body,
			strlen(body));
}

/* ---- requests -------------------------------------------------------------- */

/* Record the request, pick its step and build the reply. */
static int mock_handle(struct mock_conn *c, struct mock_req *r)
{
	struct mock *m = c->m;
	struct mock_fields f;
	char auth[1024], session[256], xkey[1024], aver[128], proto[128];
	char ctype[256], conn[64];
	fy_generic parsed, v, rec, step;
	const char *rpc = "", *line;
	bool post, get, del;

	hdr_get(r->hdrs, r->hlen, "Authorization", auth, sizeof(auth));
	hdr_get(r->hdrs, r->hlen, "Mcp-Session-Id", session, sizeof(session));
	hdr_get(r->hdrs, r->hlen, "x-api-key", xkey, sizeof(xkey));
	hdr_get(r->hdrs, r->hlen, "anthropic-version", aver, sizeof(aver));
	hdr_get(r->hdrs, r->hlen, "MCP-Protocol-Version", proto, sizeof(proto));
	hdr_get(r->hdrs, r->hlen, "Content-Type", ctype, sizeof(ctype));
	hdr_get(r->hdrs, r->hlen, "Connection", conn, sizeof(conn));
	c->close_after = r->http10 || strcasestr(conn, "close");

	post = !strcmp(r->method, "POST");
	get = !strcmp(r->method, "GET");
	del = !strcmp(r->method, "DELETE");
	if (!post && !get && !del) {
		static const char msg[] = "{\"error\":{\"message\":\"mock: "
					  "unsupported method\"}}";

		return op_reply(c, 501, "application/json", NULL, msg,
				sizeof(msg) - 1);
	}

	parsed = fy_invalid;
	if (post) {
		parsed = parse_json_string_size(m->gb, r->body, r->body_len);
		if (fy_is_mapping(parsed)) {
			v = fy_get(parsed, "method", fy_invalid);
			if (fy_is_string(v))
				rpc = fy_gb_intern_string(m->gb,
							  fy_castp(&v, ""));
		}
	}

	f.http = r->method;
	f.path = r->target;
	f.method = rpc;
	f.auth = auth;
	f.session = post || del ? session : "";
	f.body = post ? r->body : "";
	step = mock_pick(m, &f);
	if (m->log)
		fyai_report(m->ctx, "mock: %s %s -> %s\n", r->method, r->target,
			    fy_is_valid(step) ? "step" : "exhausted");

	if (post)
		rec = fy_mapping(m->gb,
			"path", r->target,
			"auth", auth,
			"x_api_key", xkey,
			"anthropic_version", aver,
			"mcp_session_id", session,
			"mcp_protocol_version", proto,
			"client_port", (long long)c->peer_port,
			"content_type", ctype,
			"body", fy_is_valid(parsed) && !fy_is_null(parsed) ?
				parsed : fy_value(m->gb, r->body));
	else if (get)
		rec = fy_mapping(m->gb,
			"path", r->target,
			"method", "GET",
			"auth", auth,
			"client_port", (long long)c->peer_port,
			"body", "");
	else
		rec = fy_mapping(m->gb,
			"path", r->target,
			"method", "DELETE",
			"auth", auth,
			"mcp_session_id", session,
			"mcp_protocol_version", proto,
			"client_port", (long long)c->peer_port,
			"body", "");
	line = emit_json_string(m->gb, rec);
	if (line) {
		char *out = NULL;
		size_t n = strlen(line);

		out = malloc(n + 2);
		if (out) {
			memcpy(out, line, n);
			out[n] = '\n';
			mock_note(m, out, n + 1);
			free(out);
		}
	}

	if (!fy_is_valid(step))
		return reply_exhausted(c);
	if (post)
		return reply_post(c, step);
	if (get)
		return reply_get(c, step);
	return reply_delete(c, step);
}

/* Handle every complete request in the input of @c. */
static void conn_feed(struct mock_conn *c)
{
	struct mock_req r;
	int rc;

	rc = req_parse(c, &r);
	if (rc <= 0) {
		free(r.body);
		if (rc < 0)
			conn_close(c);
		return;
	}
	c->busy = true;
	rc = mock_handle(c, &r);
	if (rc) {
		free(r.body);
		conn_close(c);
		return;
	}
	free(r.body);
	memmove(c->in, c->in + r.consumed, c->in_len - r.consumed);
	c->in_len -= r.consumed;
	conn_pump(c);
}

static enum fyai_event_action mock_on_accept(const struct fyai_event *ev)
{
	struct mock *m = ev->userdata;
	struct mock_conn *c;
	struct sockaddr_in sa;
	socklen_t slen = sizeof(sa);
	int fd, one = 1, rc;

	fd = accept(ev->fd, (struct sockaddr *)&sa, &slen);
	if (fd < 0)
		return FYAIEA_CONTINUE;
	c = calloc(1, sizeof(*c));
	if (!c) {
		close(fd);
		return FYAIEA_CONTINUE;
	}
	(void)fcntl(fd, F_SETFD, FD_CLOEXEC);
	(void)fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
	/* Keep each write a segment of its own, so a client sees the splits. */
	(void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
	(void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
	c->m = m;
	c->fd = fd;
	c->peer_port = ntohs(sa.sin_port);
	rc = fyai_event_add_fd(m->el, fd, FYAIEV_READ, conn_on_event, c,
			       &c->src);
	if (rc) {
		close(fd);
		free(c);
	}
	return FYAIEA_CONTINUE;
}

static enum fyai_event_action mock_on_term(const struct fyai_event *ev)
{
	(void)ev;
	return FYAIEA_STOP;
}

/* ---- start ----------------------------------------------------------------- */

static int mock_listen(struct mock *m)
{
	struct sockaddr_in sa;
	socklen_t slen = sizeof(sa);
	int fd, one = 1, rc;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	fyai_error_check(m->ctx, fd >= 0, err, "mock: no socket: %s",
			 strerror(errno));
	(void)fcntl(fd, F_SETFD, FD_CLOEXEC);
	(void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	rc = bind(fd, (struct sockaddr *)&sa, sizeof(sa));
	fyai_error_check(m->ctx, !rc, err_close, "mock: cannot bind: %s",
			 strerror(errno));
	rc = listen(fd, MOCK_BACKLOG);
	fyai_error_check(m->ctx, !rc, err_close, "mock: cannot listen: %s",
			 strerror(errno));
	(void)fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
	rc = getsockname(fd, (struct sockaddr *)&sa, &slen);
	fyai_error_check(m->ctx, !rc, err_close, "mock: no port: %s",
			 strerror(errno));
	m->port = ntohs(sa.sin_port);
	return fd;
err_close:
	close(fd);
err:
	return -1;
}

/* The port file appears whole or not at all. */
static int mock_publish_port(struct mock *m)
{
	char path[4096], tmp[4100], num[32];
	int len, rc;

	rundir_path(m, "port", path, sizeof(path));
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	len = snprintf(num, sizeof(num), "%u\n", m->port);
	rc = file_write(tmp, O_TRUNC, num, (size_t)len);
	fyai_error_check(m->ctx, !rc, err, "mock: cannot write %s: %s", tmp,
			 strerror(errno));
	rc = rename(tmp, path);
	fyai_error_check(m->ctx, !rc, err, "mock: cannot publish %s: %s", path,
			 strerror(errno));
	return 0;
err:
	return -1;
}

int fyai_mock_serve(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
		    const char *scenario, const char *rundir, bool log)
{
	struct mock m = { .ctx = ctx, .gb = gb, .rundir = rundir,
			  .log = log, .listen_fd = -1 };
	struct fyai_event_source *term = NULL;
	fy_generic doc, step;
	const char *scale;
	size_t i;
	int rc;

	doc = fy_parse_file(gb, FYAI_YAML_PARSE_FLAGS, scenario);
	fyai_error_check(ctx, fy_is_mapping(doc), err,
			 "mock: cannot parse the scenario %s", scenario);
	m.steps = fy_get(doc, "steps", fy_seq_empty);
	m.nsteps = fy_len(m.steps);
	m.consumed = calloc(m.nsteps + 1, sizeof(*m.consumed));
	fyai_error_check(ctx, m.consumed, err, "mock: out of memory");
	for (i = 0; i < m.nsteps; i++) {
		step = fy_get_at(m.steps, i);
		if (fy_is_mapping(step) &&
		    fy_is_valid(fy_get(step, "match", fy_invalid)))
			m.matched = true;
	}
	scale = getenv("FYAI_TIMEOUT_SCALE");
	m.scale = scale && *scale ? atof(scale) : 1.0;
	if (m.scale <= 0)
		m.scale = 1.0;

	m.el = fyai_ctx_loop(ctx);
	fyai_error_check(ctx, m.el, err, "mock: no event loop");
	m.listen_fd = mock_listen(&m);
	if (m.listen_fd < 0)
		goto err;
	rc = fyai_event_add_fd(m.el, m.listen_fd, FYAIEV_READ, mock_on_accept,
			       &m, &m.listen_src);
	fyai_error_check(ctx, !rc, err, "mock: cannot watch the listener");
	rc = fyai_event_add_signal(m.el, SIGTERM, mock_on_term, &m, &term);
	fyai_error_check(ctx, !rc, err, "mock: cannot watch SIGTERM");
	rc = mock_publish_port(&m);
	if (rc)
		goto err;

	rc = fyai_event_loop_run_until(m.el, NULL, -1);
	fyai_event_source_remove(term);
	fyai_event_source_remove(m.listen_src);
	close(m.listen_fd);
	free(m.consumed);
	return rc < 0 ? -1 : 0;
err:
	if (m.listen_fd >= 0)
		close(m.listen_fd);
	free(m.consumed);
	return -1;
}
