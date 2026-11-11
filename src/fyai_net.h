/* SPDX-License-Identifier: MIT */
/*
 * Sockets for the network code of fyai: a byte queue, a connection that is
 * plain TCP or TLS over TCP, and the listen, connect and URL helpers that the
 * HTTP server and the clients share. Nothing here owns an event source, so a
 * connection is driven by the event loop of the caller or by a blocking loop.
 */
#ifndef FYAI_NET_H
#define FYAI_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include <openssl/ssl.h>

#include "fyai_buf.h"

struct fyai_ctx;

/* ---- a connection --------------------------------------------------------- */

/*
 * A connected descriptor, with TLS over it when @ssl is set. A TLS listener
 * leaves the handshake to the first fyai_netio_recv(), so a connection can be
 * driven by the event loop. After a call that returned -1 with EAGAIN,
 * @want_read and @want_write say which readiness the call waits for.
 */
struct fyai_netio {
	int fd;
	SSL *ssl;
	bool handshaken;
	bool want_read, want_write;
};

void fyai_netio_init(struct fyai_netio *io, int fd);

/* Make the connection the accepting end of TLS in @tls. Returns 0, or -1. */
int fyai_netio_tls_accept(struct fyai_netio *io, SSL_CTX *tls);

/* Take @ssl, a connection whose handshake is done. The connection owns it. */
void fyai_netio_tls_adopt(struct fyai_netio *io, SSL *ssl);

/*
 * Make the connection the client end of TLS with @ssl, which is not connected
 * to a descriptor yet. The handshake is left to fyai_netio_handshake(), so
 * that the caller can check the peer before it sends. The connection owns
 * @ssl. Returns 0, or -1 with @ssl freed.
 */
int fyai_netio_tls_connect(struct fyai_netio *io, SSL *ssl);

/*
 * Run the TLS handshake. Returns 1 when it is done, and for a connection
 * without TLS; 0 when it waits, with @want_read or @want_write set; -1 when it
 * failed. A failure leaves the error of OpenSSL queued, for the caller to
 * report. Reading and writing run the handshake when it has not been run.
 */
int fyai_netio_handshake(struct fyai_netio *io);

/*
 * Read or write once. They return the bytes moved, 0 when the peer has ended,
 * or -1 with errno: EAGAIN to wait, another value for a failure that ends the
 * connection. On a blocking descriptor EAGAIN does not occur.
 */
ssize_t fyai_netio_recv(struct fyai_netio *io, void *buf, size_t n);
ssize_t fyai_netio_send(struct fyai_netio *io, const void *buf, size_t n);

/* True when TLS holds bytes that it read already: the descriptor is not readable. */
bool fyai_netio_buffered(const struct fyai_netio *io);

/*
 * Write what waits in @b. Returns 0 when @b is empty or the connection must
 * wait, and -1 when the connection has ended or failed.
 */
int fyai_netio_flush(struct fyai_netio *io, struct fyai_buf *b);

/* Write all @n bytes of a blocking connection. Returns 0, or -1 with errno. */
int fyai_netio_send_all(struct fyai_netio *io, const void *buf, size_t n);

/* Say goodbye to the peer without waiting, free TLS and close the descriptor. */
void fyai_netio_close(struct fyai_netio *io);

/* ---- addresses and descriptors ------------------------------------------- */

int fyai_net_cloexec(int fd);
int fyai_net_nonblock(int fd);
/*
 * Make a write to a peer that has left an error, not SIGPIPE, where the
 * socket can say so (SO_NOSIGPIPE on macOS). Elsewhere it does nothing: the
 * program keeps SIGPIPE blocked.
 */
void fyai_net_nosigpipe(int fd);

/* Write the address of the peer of @fd, "HOST:PORT" or "[HOST]:PORT". */
void fyai_net_peer_text(int fd, char *out, size_t size);

/*
 * A non-blocking, close-on-exec listening socket on @address, HOST:PORT or
 * [HOST]:PORT with a numeric host. Port 0 takes one, and @bound receives the
 * address that listens. Returns the descriptor, or -1 with a diagnostic.
 */
int fyai_net_listen(struct fyai_ctx *ctx, const char *address, char *bound,
		    size_t bound_size);

/* A connected blocking socket to @host and @port, or -1 with a diagnostic. */
int fyai_net_connect(struct fyai_ctx *ctx, const char *host, const char *port);

/*
 * Start a connection without blocking: a non-blocking, close-on-exec socket
 * with its connect under way. The name is resolved here, and that can block.
 * The caller waits until the descriptor is writable, and then calls
 * fyai_net_connect_finish(). Returns the descriptor, or -1 with a diagnostic.
 */
int fyai_net_connect_start(struct fyai_ctx *ctx, const char *host, const char *port);

/* The result of the connect of @fd: 0, or -1 with a diagnostic. */
int fyai_net_connect_finish(struct fyai_ctx *ctx, int fd, const char *host,
			    const char *port);

/* The parts of an http:// or https:// URL that a client needs. */
struct fyai_net_url {
	bool tls;
	char host[256];
	char port[16];
	char authority[300];	/* the Host header: HOST[:PORT] as written */
};

/* Split @url; the path and what follows it are not kept. Returns 0, or -1. */
int fyai_net_url_parse(struct fyai_ctx *ctx, const char *url,
		       struct fyai_net_url *u);

#endif
