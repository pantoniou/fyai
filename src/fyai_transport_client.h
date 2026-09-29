/*
 * fyai_transport_client.h - an agent's side of the credential transport
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * An agent holds one channel to the transport. It sends requests on it and
 * gets the response as frames: a start, the body in order, and one terminal
 * frame. The client runs on the event loop of a context and lets many calls
 * run at once. It never opens a network connection: if the channel fails, every
 * call fails, and nothing retries directly.
 */

#ifndef FYAI_TRANSPORT_CLIENT_H
#define FYAI_TRANSPORT_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fyai_transport_msg.h"

struct fyai_ctx;
struct fyai_tclient;
struct fyai_tcall;

/* How a call ended. */
enum fyai_tcall_end {
	FYAI_TC_OK,			/* the response ended */
	FYAI_TC_FAILED,			/* the transfer failed; see the result */
	FYAI_TC_REFUSED,		/* the transport refused the request */
	FYAI_TC_CANCELLED,		/* cancelled by the caller */
	FYAI_TC_LOST,			/* the channel to the transport failed */
	FYAI_TC_ABORTED,		/* the body callback refused the data */
};

struct fyai_tcall_result {
	enum fyai_tcall_end end;
	long code;			/* the CURLcode of a FAILED transfer */
	bool transient;			/* a retry can succeed */
	char message[256];		/* the reason, for the user */
};

/* The start of the response; the call can read these until it ends. */
struct fyai_tcall_start {
	long status;
	long retry_after_s;		/* -1 when absent */
	struct fyai_transport_ratelimit rate_limit;
};

/* Take @len body bytes. Return @len to take them, less to abort the call. */
typedef size_t (*fyai_tcall_body_fn)(const void *data, size_t len, void *ud);
typedef void (*fyai_tcall_start_fn)(struct fyai_tcall *call,
				    const struct fyai_tcall_start *start,
				    void *ud);
/* The terminal event. The call is destroyed by the caller afterwards. */
typedef void (*fyai_tcall_end_fn)(struct fyai_tcall *call,
				  const struct fyai_tcall_result *result,
				  void *ud);

struct fyai_tcall_callbacks {
	fyai_tcall_start_fn start;	/* may be NULL */
	fyai_tcall_body_fn body;
	fyai_tcall_end_fn end;
	void *userdata;
};

/*
 * Serve @channel, a SOCK_SEQPACKET socket to the transport, on the event loop
 * of @ctx. @exec_id is the execution that the supervisor registered. The client
 * owns @channel. Return NULL, with a diagnostic, on failure.
 */
struct fyai_tclient *fyai_tclient_open(struct fyai_ctx *ctx, int channel,
				       uint64_t exec_id);

/*
 * Free the client and leave the channel open, and return its descriptor. The
 * owner keeps the channel across an execution of this program, as `/reload`
 * does. Calls still open are ended as lost.
 */
int fyai_tclient_release(struct fyai_tclient *client);

/* Cancel every call, close the channel, and free the client. */
void fyai_tclient_close(struct fyai_tclient *client);

/*
 * Send a request. @rq->profile names the profile. The strings and the body of
 * @rq are copied into the request before it is sent. The end callback runs from
 * the event loop, not from this function. Return NULL, with a diagnostic, if
 * the request cannot be sent.
 */
struct fyai_tcall *fyai_tclient_submit(struct fyai_tclient *client,
				       const struct fyai_transport_request *rq,
				       const struct fyai_tcall_callbacks *cb);

/* Ask the transport to stop the call. Its end callback still runs. */
void fyai_tcall_cancel(struct fyai_tcall *call);

/* Free a call, cancelling it if it has not ended. */
void fyai_tcall_destroy(struct fyai_tcall *call);

bool fyai_tcall_ended(const struct fyai_tcall *call);

/* True while the channel to the transport works. */
bool fyai_tclient_alive(const struct fyai_tclient *client);

/*
 * The latest rate-limit report from the provider, copied to @out. It is a
 * record for the user and the statistics; nothing in the client acts on it.
 * Return false if none arrived.
 */
bool fyai_tclient_ratelimit(const struct fyai_tclient *client,
			    struct fyai_transport_ratelimit *out);

#endif
