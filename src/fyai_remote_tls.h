/* SPDX-License-Identifier: MIT */
/*
 * TLS for remote sessions: the key and certificate of `fyai serve`, the
 * context of the HTTPS listener, and the checks of a client of such a server
 * (doc/remote-sessions-sdd.md, sections 5.1.2 and 6.3).
 */
#ifndef FYAI_REMOTE_TLS_H
#define FYAI_REMOTE_TLS_H

#include <stdbool.h>
#include <stddef.h>

#include <openssl/ssl.h>

struct fyai_ctx;

/* "sha256:" and 64 hexadecimal digits of a public key, and the end. */
#define FYAI_TLS_FP_SIZE	72

/*
 * The path of the generated key and certificate, ~/.fyai/tls/server.pem: one
 * PEM file, the private key and then the certificate, so one rename publishes
 * both. The caller frees it.
 */
char *fyai_tls_default_path(struct fyai_ctx *ctx);

/*
 * Write a new self-signed ECDSA P-256 key and certificate to @path, mode 0600,
 * by a rename. The certificate is valid for ten years for localhost, the
 * loopback addresses, the host name and each name of the NULL-terminated list
 * @names, a DNS name or an IP address. Returns 0 or -1 with a diagnostic.
 */
int fyai_tls_generate(struct fyai_ctx *ctx, const char *path,
		      const char *const *names);

/*
 * The context of a TLS listener. With @cert and @key it takes those files.
 * Else it takes the generated file, which it makes when it is missing. The
 * file with the key must be readable by its owner alone. @fp gets the
 * fingerprint of the public key. Returns NULL with a diagnostic.
 */
SSL_CTX *fyai_tls_server_ctx(struct fyai_ctx *ctx, const char *cert,
			     const char *key, const char *const *names,
			     char fp[FYAI_TLS_FP_SIZE]);

/* The fingerprint of the public key of the first certificate in @path. */
int fyai_tls_file_fingerprint(struct fyai_ctx *ctx, const char *path,
			      char fp[FYAI_TLS_FP_SIZE]);

/*
 * The client end in three steps, for a connection that does not block: make
 * the SSL object of @host, run the handshake (fyai_netio_tls_connect() and
 * fyai_netio_handshake()), then check the key that the server showed. The first
 * returns NULL with a diagnostic. After a handshake that failed, the second
 * reports why. The third is the check of @pin, and must pass before the client
 * sends anything: a client with no @pin has checked the chain in the
 * handshake. @pin and @ca_file are as for fyai_tls_connect().
 */
SSL *fyai_tls_client_new(struct fyai_ctx *ctx, const char *host, const char *pin,
			 const char *ca_file);
void fyai_tls_client_error(struct fyai_ctx *ctx, SSL *ssl, const char *host,
			   const char *pin);
int fyai_tls_client_verify(struct fyai_ctx *ctx, SSL *ssl, const char *host,
			   const char *pin);

/*
 * Do the TLS handshake of a client on the connected blocking socket @fd of
 * @host. With @pin, a fingerprint as "sha256:HEX", the key of the server must
 * be that one and no other check applies. Else the certificate must chain to
 * the system roots, or to @ca_file, and name @host. On a failure the
 * diagnostic says why, and with the fingerprint that the server showed.
 */
int fyai_tls_connect(struct fyai_ctx *ctx, int fd, const char *host,
		     const char *pin, const char *ca_file, SSL **sslp);

#endif
