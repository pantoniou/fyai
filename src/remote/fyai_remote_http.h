/* SPDX-License-Identifier: MIT */
/*
 * The HTTP front of `fyai serve`: the routes, the bearer check and the bridge
 * of a WebSocket to a socket of lines, on top of the HTTP server of
 * fyai_httpd.h (section 5.1 of doc/remote-sessions-sdd.md). A client that
 * passes the upgrade is given to the server as a socket of NDJSON lines, as a
 * Unix client is, so the server code does not know the transport.
 */
#ifndef FYAI_REMOTE_HTTP_H
#define FYAI_REMOTE_HTTP_H

#include <stdbool.h>
#include <stddef.h>

#include <openssl/ssl.h>

#include "fyai_httpd.h"

struct fyai_ctx;

/* The path of the WebSocket upgrade. */
#define FYAI_HTTP_SESSION_PATH	"/api/fyai/session"

/* What a verified bearer token says about its client. */
struct fyai_remote_identity {
	char subject[128];	/* who the token names; empty for a shared token */
};

/*
 * The check of a bearer token, which a transport calls before the upgrade.
 * Returns 0 for a valid token, and fills @id. The token is a secret: the
 * verifier compares it in constant time and the caller never logs it.
 */
struct fyai_remote_auth {
	int (*verify)(void *data, const char *bearer,
		      struct fyai_remote_identity *id);
	void *data;
};

struct fyai_remote_http_ops {
	/*
	 * Give the server the client socket @fd of lines; @tls says that the
	 * client came over TLS. 0, or -1 to refuse it.
	 */
	int (*admit)(void *data, int fd, const struct fyai_remote_identity *id,
		     const char *peer, bool tls);
	void (*log)(void *data, const char *line);
	void *data;
};

struct fyai_remote_http;

/* The listeners that share one verifier and one server. */
struct fyai_remote_http *
fyai_remote_http_new(struct fyai_ctx *ctx, const struct fyai_remote_auth *auth,
		     const struct fyai_remote_http_ops *ops);

/*
 * Listen on @address, HOST:PORT or [HOST]:PORT with a numeric host; port 0
 * takes one, and @bound receives the address that is listening. With @tls the
 * listener speaks TLS: it takes a reference to the context, and the caller
 * keeps its own. Returns 0, or -1 with a diagnostic.
 */
int fyai_remote_http_listen(struct fyai_remote_http *http, const char *address,
			    SSL_CTX *tls, char *bound, size_t bound_size);
void fyai_remote_http_close(struct fyai_remote_http *http);

/* The verifier of one shared token of @token_len characters. */
int fyai_remote_token_verify(void *token, const char *bearer,
			     struct fyai_remote_identity *id);

#endif
