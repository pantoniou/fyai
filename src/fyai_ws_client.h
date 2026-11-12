/* SPDX-License-Identifier: MIT */
/*
 * The client end of a WebSocket, in two forms that share the options, the
 * handshake text and the codec:
 *
 * - struct fyai_ws_client is a blocking connection: connect, TLS, the upgrade
 *   handshake, and masked frames out and whole messages in. The caller polls
 *   the descriptor and decides what a message means, as a standalone command
 *   does.
 * - struct fyai_wsc is a connection on an event loop that never blocks, but
 *   for the resolution of the name. It connects, makes the TLS handshake,
 *   checks the pin, upgrades, and gives each message to a handler, as
 *   fyai_httpd does for the server end.
 */
#ifndef FYAI_WS_CLIENT_H
#define FYAI_WS_CLIENT_H

#include <stdbool.h>
#include <stddef.h>

#include "fyai_net.h"
#include "fyai_ws.h"

struct fyai_ctx;
struct fyai_event_loop;

#define FYAI_WS_CLIENT_CHUNK	65536u

struct fyai_ws_client {
	struct fyai_netio io;
	struct fyai_ws ws;
	/* Bytes of the server that are read and not parsed yet. */
	unsigned char in[FYAI_WS_CLIENT_CHUNK];
	size_t in_pos, in_len;
};

struct fyai_ws_client_opts {
	struct fyai_net_url url;
	const char *path;	/* the target of the upgrade request */
	const char *bearer;	/* Authorization: Bearer, or NULL */
	const char *pin;	/* "sha256:HEX" of the key of an https server, or NULL */
	const char *ca_file;	/* roots to add for an https server, or NULL */
	size_t max_message;
};

/*
 * Connect, make the TLS handshake of an https URL, and upgrade to a WebSocket.
 * Returns 0, or -1 with a diagnostic that says why the server did not accept
 * the connection. @c is then not valid and needs no close.
 */
int fyai_ws_client_open(struct fyai_ctx *ctx, const struct fyai_ws_client_opts *opts,
			struct fyai_ws_client *c);
void fyai_ws_client_close(struct fyai_ws_client *c);

/* The descriptor to poll. */
int fyai_ws_client_fd(const struct fyai_ws_client *c);

/* Queue-free: write one masked frame now. Returns 0, or -1 with errno. */
int fyai_ws_client_send(struct fyai_ws_client *c, int opcode, const void *data, size_t n);

/*
 * Take the next message. Parsed bytes come first. When none is whole and
 * @readable is true, one read is made. Returns 1 with @msg (valid until the
 * next call), 0 when more bytes are needed, -1 for a frame that is not valid,
 * and -2 when the server has ended. After 1, call again with @readable false
 * to take the messages that the same bytes held.
 */
int fyai_ws_client_recv(struct fyai_ws_client *c, struct fyai_ws_message *msg,
			bool readable);

/* True when bytes wait that the descriptor does not show: poll does not wake. */
bool fyai_ws_client_buffered(const struct fyai_ws_client *c);

/* ---- the client on an event loop ------------------------------------------ */

struct fyai_wsc;

struct fyai_wsc_ops {
	/* The server upgraded the connection: the client may send. */
	void (*open)(void *data, struct fyai_wsc *c);
	/* A text or binary message. It is valid during the call. */
	void (*message)(void *data, struct fyai_wsc *c,
			const struct fyai_ws_message *msg);
	/* The queue of the connection went down: it may take more. */
	void (*drain)(void *data, struct fyai_wsc *c);
	/*
	 * The connection ended, once, from whatever cause. A failure has raised
	 * its diagnostic before. @opened says that open() was called. The client
	 * is not valid after the call.
	 */
	void (*closed)(void *data, struct fyai_wsc *c, bool opened);
	void *data;
};

struct fyai_wsc_opts {
	struct fyai_ws_client_opts ws;
	struct fyai_event_loop *loop;	/* NULL for the loop of the invocation */
	size_t max_output;	/* a send that would queue more fails */
	unsigned int open_ms;	/* the time to open; 0 sets no limit */
};

/*
 * Start a connection. Returns at once with the client, or NULL with a
 * diagnostic when the connection cannot start; @ops->closed is not called
 * then. The options are copied, the token among them, and the copy of the
 * token is wiped when the client ends.
 */
struct fyai_wsc *fyai_wsc_open(struct fyai_ctx *ctx, const struct fyai_wsc_opts *opts,
			       const struct fyai_wsc_ops *ops);

/*
 * Queue one frame. Returns -1 when the connection is not open, when it holds
 * more than max_output, and on no memory; the connection then goes on.
 */
int fyai_wsc_send(struct fyai_wsc *c, int opcode, const void *data, size_t n);

/* Close the WebSocket with @code, and end once the close frame has left. */
void fyai_wsc_close(struct fyai_wsc *c, unsigned int code);

#endif
