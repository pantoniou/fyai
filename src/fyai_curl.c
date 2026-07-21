/*
 * fyai_curl.c - run a curl transfer on the event loop
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * curl's multi interface in its socket-driven form: curl tells us which
 * descriptors it cares about (CURLMOPT_SOCKETFUNCTION) and when it next wants
 * to be poked (CURLMOPT_TIMERFUNCTION), we turn both into event sources, and
 * every wakeup is handed back through curl_multi_socket_action().
 *
 * The multi handle and the loop are both per-invocation, not per-transfer.
 * That is not an optimisation, it is a correctness requirement:
 *
 *  - the multi handle owns curl's connection pool, so a fresh one per request
 *    drops every keep-alive connection;
 *  - curl keeps watching the socket of a pooled connection between transfers,
 *    so the sources describing those sockets have to outlive the transfer that
 *    registered them. A per-transfer loop recycles them underneath curl and
 *    hands the next transfer a source belonging to a destroyed loop.
 *
 * The transfer is still *awaited* synchronously, which keeps this a drop-in for
 * curl_easy_perform() at every call site. Making the wait asynchronous is a
 * later change to the callers, not to this file.
 */

#define FYAI_MODULE FYAIEM_STREAM

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "fyai.h"
#include "fyai_curl.h"
#include "fyai_diag.h"
#include "fyai_event.h"

struct fyai_curl_sock;

/* Per-invocation curl plumbing. */
struct fyai_curl_state {
	struct fyai_ctx *ctx;
	struct fyai_event_loop *el;
	struct fyai_event_source *timer;
	struct fyai_curl_sock *socks;
	CURLM *multi;

	CURL *easy;
	CURLcode result;
	int running;
	bool done;
	bool failed;
};

/* One watched socket. curl_multi_assign() keeps this on the socket itself. */
struct fyai_curl_sock {
	struct fyai_curl_sock *next;
	struct fyai_curl_state *state;
	struct fyai_event_source *src;
	curl_socket_t fd;
};

/* Collect finished transfers. */
static void fyai_curl_reap(struct fyai_curl_state *state)
{
	struct CURLMsg *msg;
	int left;

	for (;;) {
		msg = curl_multi_info_read(state->multi, &left);
		if (!msg)
			break;
		if (msg->msg != CURLMSG_DONE)
			continue;
		if (msg->easy_handle == state->easy) {
			state->result = msg->data.result;
			state->done = true;
		}
	}
}

/* Hand a readiness (or timeout) back to curl and harvest what it finished. */
static void fyai_curl_action(struct fyai_curl_state *state, curl_socket_t fd,
			     int mask)
{
	CURLMcode mc;

	mc = curl_multi_socket_action(state->multi, fd, mask, &state->running);
	if (mc != CURLM_OK) {
		state->failed = true;
		state->done = true;
		fyai_error(state->ctx, "curl multi failed: %s",
			   curl_multi_strerror(mc));
		return;
	}
	fyai_curl_reap(state);
}

static enum fyai_event_action fyai_curl_on_socket(const struct fyai_event *ev)
{
	struct fyai_curl_sock *sock = ev->userdata;
	int mask = 0;

	if (ev->events & FYAIEV_READ)
		mask |= CURL_CSELECT_IN;
	if (ev->events & FYAIEV_WRITE)
		mask |= CURL_CSELECT_OUT;
	/* Report an error as readiness in both directions rather than as
	 * CURL_CSELECT_ERR alone. */
	if (ev->events & (FYAIEV_ERROR | FYAIEV_EOF))
		mask |= CURL_CSELECT_IN | CURL_CSELECT_OUT;

	fyai_curl_action(sock->state, sock->fd, mask);
	return FYAIEA_CONTINUE;
}

static enum fyai_event_action fyai_curl_on_timeout(const struct fyai_event *ev)
{
	struct fyai_curl_state *state = ev->userdata;

	fyai_curl_action(state, CURL_SOCKET_TIMEOUT, 0);
	return FYAIEA_CONTINUE;
}

/* Stop watching a socket and drop curl's pointer to its bookkeeping. */
static void fyai_curl_sock_release(struct fyai_curl_sock *sock)
{
	struct fyai_curl_state *state = sock->state;
	struct fyai_curl_sock **pp;

	for (pp = &state->socks; *pp; pp = &(*pp)->next) {
		if (*pp == sock) {
			*pp = sock->next;
			break;
		}
	}
	/* Unregister before curl closes it: the loop holds a kernel-side reference to
	 * the descriptor. */
	fyai_event_source_remove(sock->src);
	curl_multi_assign(state->multi, sock->fd, NULL);
	free(sock);
}

/* curl's socket callback: start, change or stop watching @fd. */
static int fyai_curl_socket_cb(CURL *easy, curl_socket_t fd, int what,
			       void *userp, void *socketp)
{
	struct fyai_curl_state *state = userp;
	struct fyai_curl_sock *sock = socketp;
	unsigned int events = 0;
	int rc;

	(void)easy;

	if (what == CURL_POLL_REMOVE) {
		if (sock)
			fyai_curl_sock_release(sock);
		return 0;
	}

	if (what == CURL_POLL_IN || what == CURL_POLL_INOUT)
		events |= FYAIEV_READ;
	if (what == CURL_POLL_OUT || what == CURL_POLL_INOUT)
		events |= FYAIEV_WRITE;
	if (!events)
		return 0;

	if (sock)
		return fyai_event_fd_modify(sock->src, events);

	sock = calloc(1, sizeof(*sock));
	fyai_error_check(state->ctx, sock, err_out, "out of memory");

	sock->state = state;
	sock->fd = fd;
	rc = fyai_event_add_fd(state->el, fd, events, fyai_curl_on_socket, sock,
			       &sock->src);
	if (rc) {
		free(sock);
		goto err_out;
	}
	sock->next = state->socks;
	state->socks = sock;
	curl_multi_assign(state->multi, fd, sock);
	return 0;

err_out:
	return -1;
}

/* curl's timer callback: it wants curl_multi_socket_action(TIMEOUT) in
 * @timeout_ms, or never again when negative. */
static int fyai_curl_timer_cb(CURLM *multi, long timeout_ms, void *userp)
{
	struct fyai_curl_state *state = userp;

	(void)multi;

	if (timeout_ms < 0)
		return fyai_event_timer_disarm(state->timer);

	/* Zero means "immediately". */
	return fyai_event_timer_rearm(state->timer,
				      timeout_ms ? timeout_ms : 1, 0);
}

/* Build the per-invocation plumbing on first use. */
static struct fyai_curl_state *fyai_curl_state_get(struct fyai_ctx *ctx)
{
	struct fyai_curl_state *state = ctx->curl_state;
	int rc;

	if (state)
		return state;

	state = calloc(1, sizeof(*state));
	fyai_error_check(ctx, state, err_out, "out of memory");
	state->ctx = ctx;

	state->el = fyai_ctx_loop(ctx);
	fyai_error_check(ctx, state->el, err_free,
			 "could not create an event loop");

	state->multi = curl_multi_init();
	fyai_error_check(ctx, state->multi, err_loop,
			 "could not create a curl multi");

	/* The timer stays registered for the whole invocation, armed only when curl
	 * asks. */
	rc = fyai_event_add_timer(state->el, 0, 0, fyai_curl_on_timeout, state,
				  &state->timer);
	fyai_error_check(ctx, !rc, err_multi, "could not create the curl timer");
	fyai_event_timer_disarm(state->timer);

	curl_multi_setopt(state->multi, CURLMOPT_SOCKETFUNCTION,
			  fyai_curl_socket_cb);
	curl_multi_setopt(state->multi, CURLMOPT_SOCKETDATA, state);
	curl_multi_setopt(state->multi, CURLMOPT_TIMERFUNCTION,
			  fyai_curl_timer_cb);
	curl_multi_setopt(state->multi, CURLMOPT_TIMERDATA, state);

	ctx->curl_state = state;
	return state;

err_multi:
	curl_multi_cleanup(state->multi);
err_loop:
err_free:
	free(state);
err_out:
	return NULL;
}

void fyai_curl_cleanup(struct fyai_ctx *ctx)
{
	struct fyai_curl_state *state;

	if (!ctx || !ctx->curl_state)
		return;
	state = ctx->curl_state;
	ctx->curl_state = NULL;

	/* Tear the multi down first: it releases the sockets it still holds for
	 * pooled connections. */
	if (state->multi)
		curl_multi_cleanup(state->multi);
	while (state->socks)
		fyai_curl_sock_release(state->socks);
	free(state);
}

CURLcode fyai_curl_perform(struct fyai_ctx *ctx, CURL *easy)
{
	struct fyai_curl_state *state;
	CURLcode result = CURLE_FAILED_INIT;
	CURLMcode mc;
	int rc;

	state = fyai_curl_state_get(ctx);
	fyai_error_check(ctx, state, out, "curl is not usable");

	state->easy = easy;
	state->result = CURLE_FAILED_INIT;
	state->done = false;
	state->failed = false;

	mc = curl_multi_add_handle(state->multi, easy);
	fyai_error_check(ctx, mc == CURLM_OK, out, "curl multi failed: %s",
			 curl_multi_strerror(mc));

	/* Kick it off; curl arms its timer and asks for sockets from here. */
	fyai_curl_action(state, CURL_SOCKET_TIMEOUT, 0);

	if (!state->done) {
		rc = fyai_event_loop_run_until(state->el, &state->done, -1);
		if (rc)
			state->failed = true;
	}

	if (state->done && !state->failed)
		result = state->result;

	/* Removing the handle returns its connection to the multi's pool rather than
	 * closing it, which is what keeps keep-alive working across transfers. */
	curl_multi_remove_handle(state->multi, easy);
	state->easy = NULL;
out:
	return result;
}
