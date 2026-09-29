/*
 * fyai_transport_client.c - an agent's side of the credential transport
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * Ownership: a call belongs to its caller, which destroys it after the end
 * callback or earlier. The client keeps the calls in a list so that it can end
 * them when the channel fails, and unlinks a call before it calls back.
 */

#define FYAI_MODULE FYAIEM_STREAM

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "fyai.h"
#include "fyai_event.h"
#include "fyai_transport.h"
#include "fyai_transport_client.h"

/* Return this much credit when this much body has been consumed. */
#define CREDIT_STEP	(FYAI_TRANSPORT_INITIAL_CREDIT / 4)
#define FRAME_BUILDER_SIZE (2 * FYAI_TRANSPORT_MAX_FRAME + \
			    FY_GENERIC_BUILDER_LINEAR_IN_PLACE_MIN_SIZE)

struct fyai_tcall {
	struct fyai_tcall *next;
	struct fyai_tclient *client;
	uint64_t id;
	uint64_t next_seq;
	struct fyai_tcall_callbacks cb;
	bool started;
	bool ended;
	bool cancel_sent;
	size_t unreported;		/* consumed body bytes not yet credited */
};

struct fyai_tclient {
	struct fyai_ctx *ctx;
	struct fyai_event_loop *el;
	struct fyai_event_source *src;
	int channel;
	uint64_t exec_id;
	uint64_t next_id;
	struct fyai_tcall *calls;
	struct fyai_transport_ratelimit rate_limit;
	bool have_rate_limit;
	bool lost;
};

static void tcall_unlink(struct fyai_tcall *call)
{
	struct fyai_tcall **pp;

	for (pp = &call->client->calls; *pp; pp = &(*pp)->next) {
		if (*pp == call) {
			*pp = call->next;
			return;
		}
	}
}

/* End a call: unlink it, then tell its owner, who may destroy it. */
static void tcall_end(struct fyai_tcall *call, enum fyai_tcall_end end,
		      long code, bool transient, const char *message)
{
	struct fyai_tcall_result res = {
		.end = end, .code = code, .transient = transient,
	};

	if (call->ended)
		return;
	call->ended = true;
	tcall_unlink(call);
	if (message)
		snprintf(res.message, sizeof(res.message), "%s", message);
	call->cb.end(call, &res, call->cb.userdata);
}

static void client_send_simple(struct fyai_tclient *c, uint16_t kind,
			       uint64_t request, const void *payload, size_t len)
{
	struct fyai_transport_hdr hdr = {
		.kind = kind, .exec_id = c->exec_id, .request_id = request,
		.len = len,
	};

	if (fyai_transport_send_frame(c->channel, &hdr, payload))
		c->lost = true;
}

static void call_credit(struct fyai_tcall *call, size_t consumed)
{
	uint8_t b[4];
	uint32_t n;

	call->unreported += consumed;
	if (call->unreported < CREDIT_STEP)
		return;
	n = call->unreported;
	call->unreported = 0;
	b[0] = n;
	b[1] = n >> 8;
	b[2] = n >> 16;
	b[3] = n >> 24;
	client_send_simple(call->client, FYAI_TK_CREDIT, call->id, b, 4);
}

/* Map the terminal error frame to how the call ended. */
static void call_error(struct fyai_tcall *call, const uint8_t *p, size_t len)
{
	char storage[FRAME_BUILDER_SIZE];
	struct fy_generic_builder *gb;
	struct fyai_transport_error er = { .code = -1 };
	char msg[256];

	gb = fy_generic_builder_create_in_place(FYGBCF_SCOPE_LEADER, NULL,
						 storage, sizeof(storage));
	if (gb && !fyai_transport_error_parse(gb, p, len, &er) && er.message)
		snprintf(msg, sizeof(msg), "%s", er.message);
	else
		snprintf(msg, sizeof(msg), "the transport reported an error");
	if (er.code == -2)
		tcall_end(call, FYAI_TC_CANCELLED, 0, false, msg);
	else if (er.code < 0)
		tcall_end(call, FYAI_TC_REFUSED, 0, false, msg);
	else
		tcall_end(call, FYAI_TC_FAILED, er.code, er.transient, msg);
}

static void call_start(struct fyai_tcall *call, const uint8_t *p, size_t len)
{
	char storage[FRAME_BUILDER_SIZE];
	struct fy_generic_builder *gb;
	struct fyai_transport_response rs;
	struct fyai_tcall_start st;
	struct fyai_tclient *c = call->client;

	gb = fy_generic_builder_create_in_place(FYGBCF_SCOPE_LEADER, NULL,
						 storage, sizeof(storage));
	if (!gb || fyai_transport_response_parse(gb, p, len, &rs)) {
		client_send_simple(c, FYAI_TK_CANCEL, call->id, NULL, 0);
		tcall_end(call, FYAI_TC_LOST, 0, false,
			  "the transport sent a bad response start");
		return;
	}
	call->started = true;
	st.status = rs.status;
	st.retry_after_s = rs.retry_after_s;
	st.rate_limit = rs.rate_limit;
	if (rs.rate_limit.count) {
		c->rate_limit = rs.rate_limit;
		c->have_rate_limit = true;
	}
	if (call->cb.start)
		call->cb.start(call, &st, call->cb.userdata);
}

static void call_body(struct fyai_tcall *call, const uint8_t *p, size_t len)
{
	if (!call->started) {
		tcall_end(call, FYAI_TC_LOST, 0, false,
			  "the transport sent a body before a start");
		return;
	}
	if (call->cb.body(p, len, call->cb.userdata) != len) {
		client_send_simple(call->client, FYAI_TK_CANCEL, call->id, NULL, 0);
		tcall_end(call, FYAI_TC_ABORTED, 0, false,
			  "the response was not accepted");
		return;
	}
	call_credit(call, len);
}

static void call_frame(struct fyai_tcall *call,
		       const struct fyai_transport_hdr *hdr, const uint8_t *p)
{
	struct fyai_tclient *c = call->client;

	if (hdr->seq != call->next_seq) {
		client_send_simple(c, FYAI_TK_CANCEL, call->id, NULL, 0);
		tcall_end(call, FYAI_TC_LOST, 0, false,
			  "the transport sent a frame out of order");
		return;
	}
	call->next_seq++;

	switch (hdr->kind) {
	case FYAI_TK_RESP_START:
		call_start(call, p, hdr->len);
		break;
	case FYAI_TK_RESP_BODY:
		call_body(call, p, hdr->len);
		break;
	case FYAI_TK_RESP_END:
		tcall_end(call, FYAI_TC_OK, 0, false, NULL);
		break;
	case FYAI_TK_RESP_ERROR:
		call_error(call, p, hdr->len);
		break;
	default:
		tcall_end(call, FYAI_TC_LOST, 0, false,
			  "the transport sent a frame of an unknown kind");
	}
}

/* End every call: the channel is gone, and so is the transport. */
static void client_lose(struct fyai_tclient *c, const char *why)
{
	struct fyai_tcall *call;

	c->lost = true;
	if (c->src) {
		fyai_event_source_remove(c->src);
		c->src = NULL;
	}
	while ((call = c->calls))
		tcall_end(call, FYAI_TC_LOST, 0, false, why);
}

static void client_readable(struct fyai_tclient *c)
{
	uint8_t buf[FYAI_TRANSPORT_MAX_FRAME];
	struct fyai_transport_hdr hdr;
	struct fyai_tcall *call;
	unsigned int n;
	ssize_t r;

	for (n = 0; n < 64 && !c->lost; n++) {
		r = recv(c->channel, buf, sizeof(buf), MSG_DONTWAIT);
		if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return;
		if (r <= 0) {
			client_lose(c, "the credential transport is not available");
			return;
		}
		if (fyai_transport_hdr_decode(buf, r, &hdr) ||
		    hdr.len != (size_t)r - FYAI_TRANSPORT_HDR_SIZE ||
		    hdr.exec_id != c->exec_id) {
			client_lose(c, "the credential transport sent a bad frame");
			return;
		}
		for (call = c->calls; call; call = call->next)
			if (call->id == hdr.request_id)
				break;
		/* A frame for a call that has ended is the tail of a cancel. */
		if (call)
			call_frame(call, &hdr, buf + FYAI_TRANSPORT_HDR_SIZE);
	}
}

static enum fyai_event_action client_on_event(const struct fyai_event *ev)
{
	struct fyai_tclient *c = ev->userdata;

	if (ev->events & (FYAIEV_READ | FYAIEV_EOF))
		client_readable(c);
	if (ev->events & FYAIEV_ERROR)
		client_lose(c, "the credential transport is not available");
	return FYAIEA_CONTINUE;
}

struct fyai_tclient *fyai_tclient_open(struct fyai_ctx *ctx, int channel,
				       uint64_t exec_id)
{
	struct fyai_tclient *c;
	int rc;

	c = calloc(1, sizeof(*c));
	fyai_error_check(ctx, c, err, "could not allocate the transport client");
	c->ctx = ctx;
	c->channel = channel;
	c->exec_id = exec_id;
	c->next_id = 1;
	c->el = fyai_ctx_loop(ctx);
	fyai_error_check(ctx, c->el, err_free,
			 "could not create the transport event loop");
	fcntl(channel, F_SETFD, FD_CLOEXEC);
	rc = fyai_event_add_fd(c->el, channel, FYAIEV_READ, client_on_event, c,
			       &c->src);
	fyai_error_check(ctx, !rc, err_free,
			 "could not watch the transport channel");
	return c;
err_free:
	free(c);
err:
	return NULL;
}

void fyai_tclient_close(struct fyai_tclient *c)
{
	struct fyai_tcall *call;

	if (!c)
		return;
	if (c->src)
		fyai_event_source_remove(c->src);
	c->src = NULL;
	/* The owners still hold their calls; the client is no longer theirs. */
	while ((call = c->calls)) {
		c->calls = call->next;
		call->client = NULL;
		call->ended = true;
	}
	close(c->channel);
	free(c);
}

struct fyai_tcall *fyai_tclient_submit(struct fyai_tclient *c,
				       const struct fyai_transport_request *rq,
				       const struct fyai_tcall_callbacks *cb)
{
	struct fy_generic_builder_cfg cfg = { .flags = FYGBCF_SCOPE_LEADER };
	struct fy_generic_builder *gb;
	struct fyai_tcall *call;
	uint8_t *payload = NULL;
	size_t len = 0;
	int rc;

	fyai_error_check(c->ctx, !c->lost, err,
			 "the credential transport is not available");
	call = calloc(1, sizeof(*call));
	fyai_error_check(c->ctx, call, err, "out of memory");
	gb = fy_generic_builder_create(&cfg);
	fyai_error_check(c->ctx, gb, err_call, "out of memory");
	rc = fyai_transport_request_encode(gb, rq, &payload, &len);
	fy_generic_builder_destroy(gb);
	fyai_error_check(c->ctx, !rc, err_call,
			 "the request cannot be encoded for the transport");

	call->client = c;
	call->id = c->next_id++;
	call->cb = *cb;
	call->next = c->calls;
	c->calls = call;

	rc = fyai_transport_send_message(c->channel, FYAI_TK_REQUEST, c->exec_id,
					 call->id, payload, len);
	free(payload);
	if (rc)
		tcall_unlink(call);
	if (rc)
		c->lost = true;
	fyai_error_check(c->ctx, !rc, err_call,
			 "the credential transport did not take the request: %s",
			 strerror(-rc));
	return call;
err_call:
	free(call);
err:
	return NULL;
}

void fyai_tcall_cancel(struct fyai_tcall *call)
{
	if (!call || call->ended || call->cancel_sent || !call->client)
		return;
	call->cancel_sent = true;
	client_send_simple(call->client, FYAI_TK_CANCEL, call->id, NULL, 0);
}

void fyai_tcall_destroy(struct fyai_tcall *call)
{
	if (!call)
		return;
	if (!call->ended && call->client) {
		fyai_tcall_cancel(call);
		tcall_unlink(call);
	}
	free(call);
}

bool fyai_tcall_ended(const struct fyai_tcall *call)
{
	return call && call->ended;
}

bool fyai_tclient_ratelimit(const struct fyai_tclient *c,
			    struct fyai_transport_ratelimit *out)
{
	if (!c->have_rate_limit)
		return false;
	*out = c->rate_limit;
	return true;
}
