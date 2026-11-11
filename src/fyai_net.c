/* SPDX-License-Identifier: MIT */
#include "config.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <curl/curl.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include "fyai.h"
#include "fyai_net.h"

#define FYAI_MODULE FYAIEM_REMOTE

/* ---- a connection --------------------------------------------------------- */

void fyai_netio_init(struct fyai_netio *io, int fd)
{
	memset(io, 0, sizeof(*io));
	io->fd = fd;
}

int fyai_netio_tls_accept(struct fyai_netio *io, SSL_CTX *tls)
{
	io->ssl = SSL_new(tls);
	if (!io->ssl || !SSL_set_fd(io->ssl, io->fd)) {
		ERR_clear_error();
		SSL_free(io->ssl);
		io->ssl = NULL;
		return -1;
	}
	SSL_set_accept_state(io->ssl);
	return 0;
}

void fyai_netio_tls_adopt(struct fyai_netio *io, SSL *ssl)
{
	io->ssl = ssl;
	io->handshaken = true;
}

int fyai_netio_tls_connect(struct fyai_netio *io, SSL *ssl)
{
	if (!SSL_set_fd(ssl, io->fd)) {
		ERR_clear_error();
		SSL_free(ssl);
		return -1;
	}
	SSL_set_connect_state(ssl);
	io->ssl = ssl;
	io->handshaken = false;
	return 0;
}

int fyai_netio_handshake(struct fyai_netio *io)
{
	int rc;

	if (!io->ssl || io->handshaken)
		return 1;
	io->want_read = io->want_write = false;
	ERR_clear_error();
	rc = SSL_do_handshake(io->ssl);
	if (rc == 1) {
		io->handshaken = true;
		return 1;
	}
	switch (SSL_get_error(io->ssl, rc)) {
	case SSL_ERROR_WANT_READ:
		io->want_read = true;
		return 0;
	case SSL_ERROR_WANT_WRITE:
		io->want_write = true;
		return 0;
	default:
		return -1;
	}
}

/* Run the handshake that a read or a write needs; -1 with errno when it does not finish. */
static int netio_ensure_handshake(struct fyai_netio *io)
{
	int rc = fyai_netio_handshake(io);

	if (rc > 0)
		return 0;
	if (rc < 0) {
		ERR_clear_error();
		errno = EIO;
	} else {
		errno = EAGAIN;
	}
	return -1;
}

/* What a failed OpenSSL call means for the caller: wait, end, or fail. */
static ssize_t netio_ssl_fail(struct fyai_netio *io, int err)
{
	switch (err) {
	case SSL_ERROR_WANT_READ:
		io->want_read = true;
		errno = EAGAIN;
		return -1;
	case SSL_ERROR_WANT_WRITE:
		io->want_write = true;
		errno = EAGAIN;
		return -1;
	case SSL_ERROR_ZERO_RETURN:
		return 0;
	case SSL_ERROR_SYSCALL:
		/* The peer closed with no close_notify, or the socket failed. */
		if (!errno || errno == ECONNRESET || errno == EPIPE) {
			ERR_clear_error();
			return 0;
		}
		break;
	default:
		break;
	}
	ERR_clear_error();
	errno = EIO;
	return -1;
}

ssize_t fyai_netio_recv(struct fyai_netio *io, void *buf, size_t n)
{
	int rc;

	if (!io->ssl)
		return read(io->fd, buf, n);
	io->want_read = io->want_write = false;
	ERR_clear_error();
	if (netio_ensure_handshake(io))
		return -1;
	rc = SSL_read(io->ssl, buf, (int)(n > INT32_MAX ? INT32_MAX : n));
	if (rc > 0)
		return rc;
	return netio_ssl_fail(io, SSL_get_error(io->ssl, rc));
}

ssize_t fyai_netio_send(struct fyai_netio *io, const void *buf, size_t n)
{
	int rc;

	if (!io->ssl)
		return write(io->fd, buf, n);
	io->want_read = io->want_write = false;
	ERR_clear_error();
	if (netio_ensure_handshake(io))
		return -1;
	rc = SSL_write(io->ssl, buf, (int)(n > INT32_MAX ? INT32_MAX : n));
	if (rc > 0)
		return rc;
	return netio_ssl_fail(io, SSL_get_error(io->ssl, rc));
}

bool fyai_netio_buffered(const struct fyai_netio *io)
{
	return io->ssl && SSL_pending(io->ssl) > 0;
}

int fyai_netio_flush(struct fyai_netio *io, struct fyai_buf *b)
{
	ssize_t n;

	while (fyai_buf_pending(b)) {
		n = fyai_netio_send(io, b->data + b->off, fyai_buf_pending(b));
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0)
			return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
		if (!n)
			return -1;
		fyai_buf_consume(b, (size_t)n);
	}
	return 0;
}

int fyai_netio_send_all(struct fyai_netio *io, const void *buf, size_t n)
{
	const unsigned char *p = buf;
	ssize_t put;

	while (n) {
		put = fyai_netio_send(io, p, n);
		if (put < 0 && errno == EINTR)
			continue;
		if (put <= 0)
			return -1;
		p += put;
		n -= (size_t)put;
	}
	return 0;
}

void fyai_netio_close(struct fyai_netio *io)
{
	if (io->ssl) {
		/* One try, with no wait: the peer may be gone. */
		if (io->handshaken)
			(void)SSL_shutdown(io->ssl);
		SSL_free(io->ssl);
		ERR_clear_error();
		io->ssl = NULL;
	}
	if (io->fd >= 0)
		close(io->fd);
	io->fd = -1;
}

/* ---- addresses and descriptors ------------------------------------------- */

int fyai_net_cloexec(int fd)
{
	return fd >= 0 && fcntl(fd, F_SETFD, FD_CLOEXEC) ? -1 : 0;
}

void fyai_net_nosigpipe(int fd)
{
#ifdef SO_NOSIGPIPE
	int one = 1;

	(void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
	(void)fd;
#endif
}

int fyai_net_nonblock(int fd)
{
	int flags = fcntl(fd, F_GETFL);

	return flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ? -1 : 0;
}

void fyai_net_peer_text(int fd, char *out, size_t size)
{
	struct sockaddr_storage ss;
	socklen_t len = sizeof(ss);
	char host[INET6_ADDRSTRLEN] = "?", port[8] = "?";

	if (!getpeername(fd, (struct sockaddr *)&ss, &len))
		(void)getnameinfo((struct sockaddr *)&ss, len, host, sizeof(host),
				  port, sizeof(port),
				  NI_NUMERICHOST | NI_NUMERICSERV);
	snprintf(out, size, strchr(host, ':') ? "[%s]:%s" : "%s:%s", host, port);
}

int fyai_net_listen(struct fyai_ctx *ctx, const char *address, char *bound,
		    size_t bound_size)
{
	struct addrinfo hints = { .ai_socktype = SOCK_STREAM,
		.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV | AI_PASSIVE };
	struct addrinfo *res = NULL;
	struct sockaddr_storage ss;
	socklen_t len = sizeof(ss);
	char *host = NULL, *port, *close_br;
	char name[INET6_ADDRSTRLEN], service[8];
	int fd = -1, rc, one = 1;

	host = strdup(address);
	fyai_error_check(ctx, host, err, "cannot keep the address of the listener");
	if (host[0] == '[') {
		close_br = strchr(host, ']');
		fyai_error_check(ctx, close_br && close_br[1] == ':', err,
				 "'%s' is not [HOST]:PORT", address);
		*close_br = '\0';
		port = close_br + 2;
		memmove(host, host + 1, strlen(host + 1) + 1);
		port--;
	} else {
		port = strrchr(host, ':');
		fyai_error_check(ctx, port, err, "'%s' is not HOST:PORT", address);
		*port++ = '\0';
	}
	rc = getaddrinfo(host, port, &hints, &res);
	fyai_error_check(ctx, !rc && res, err, "'%s' is not a numeric address and "
			 "port: %s", address, gai_strerror(rc));
	fd = socket(res->ai_family, res->ai_socktype, 0);
	fyai_error_check(ctx, fd >= 0, err, "socket: %s", strerror(errno));
	rc = fyai_net_cloexec(fd);
	if (!rc)
		rc = fyai_net_nonblock(fd);
	if (!rc)
		rc = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (!rc && res->ai_family == AF_INET6)
		rc = setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one));
	fyai_error_check(ctx, !rc, err, "cannot set up the listener: %s",
			 strerror(errno));
	rc = bind(fd, res->ai_addr, res->ai_addrlen);
	fyai_error_check(ctx, !rc, err, "bind '%s': %s", address, strerror(errno));
	rc = listen(fd, 64);
	fyai_error_check(ctx, !rc, err, "listen '%s': %s", address, strerror(errno));
	rc = getsockname(fd, (struct sockaddr *)&ss, &len);
	if (!rc)
		rc = getnameinfo((struct sockaddr *)&ss, len, name, sizeof(name),
				 service, sizeof(service),
				 NI_NUMERICHOST | NI_NUMERICSERV);
	fyai_error_check(ctx, !rc, err, "cannot read the address of the listener");
	snprintf(bound, bound_size, ss.ss_family == AF_INET6 ? "[%s]:%s" : "%s:%s",
		 name, service);
	freeaddrinfo(res);
	free(host);
	return fd;
err:
	if (res)
		freeaddrinfo(res);
	free(host);
	if (fd >= 0)
		close(fd);
	return -1;
}

int fyai_net_connect(struct fyai_ctx *ctx, const char *host, const char *port)
{
	struct addrinfo hints = { .ai_socktype = SOCK_STREAM }, *res, *ai;
	int fd = -1, rc;

	rc = getaddrinfo(host, port, &hints, &res);
	fyai_error_check(ctx, !rc, err, "cannot resolve '%s': %s", host,
			 gai_strerror(rc));
	for (ai = res; ai; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0)
			continue;
		if (!connect(fd, ai->ai_addr, ai->ai_addrlen))
			break;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	fyai_error_check(ctx, fd >= 0, err, "cannot connect to %s:%s: %s", host,
			 port, strerror(errno));
	fyai_net_nosigpipe(fd);
	return fd;
err:
	return -1;
}

int fyai_net_connect_start(struct fyai_ctx *ctx, const char *host, const char *port)
{
	struct addrinfo hints = { .ai_socktype = SOCK_STREAM }, *res = NULL, *ai;
	int fd = -1, rc;

	rc = getaddrinfo(host, port, &hints, &res);
	fyai_error_check(ctx, !rc, err, "cannot resolve '%s': %s", host,
			 gai_strerror(rc));
	for (ai = res; ai; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0)
			continue;
		if (!fyai_net_cloexec(fd) && !fyai_net_nonblock(fd) &&
		    (!connect(fd, ai->ai_addr, ai->ai_addrlen) || errno == EINPROGRESS))
			break;
		close(fd);
		fd = -1;
	}
	fyai_error_check(ctx, fd >= 0, err, "cannot connect to %s:%s: %s", host,
			 port, strerror(errno));
	fyai_net_nosigpipe(fd);
	freeaddrinfo(res);
	return fd;
err:
	if (res)
		freeaddrinfo(res);
	return -1;
}

int fyai_net_connect_finish(struct fyai_ctx *ctx, int fd, const char *host,
			    const char *port)
{
	socklen_t len = sizeof(int);
	int failure = 0, rc;

	rc = getsockopt(fd, SOL_SOCKET, SO_ERROR, &failure, &len);
	fyai_error_check(ctx, !rc && !failure, err, "cannot connect to %s:%s: %s",
			 host, port, strerror(rc ? errno : failure));
	return 0;
err:
	return -1;
}

int fyai_net_url_parse(struct fyai_ctx *ctx, const char *url,
		       struct fyai_net_url *u)
{
	CURLU *cu = curl_url();
	char *scheme = NULL, *host = NULL, *port = NULL, *user = NULL;
	const char *bare;
	size_t n;
	int rc;

	fyai_error_check(ctx, cu, err, "cannot parse '%s'", url);
	rc = curl_url_set(cu, CURLUPART_URL, url, 0);
	fyai_error_check(ctx, !rc, err, "'%s' is not a valid URL: %s", url,
			 curl_url_strerror(rc));
	rc = curl_url_get(cu, CURLUPART_SCHEME, &scheme, 0);
	fyai_error_check(ctx, !rc && (!strcmp(scheme, "http") || !strcmp(scheme, "https")),
			 err, "'%s' is not an http:// or https:// URL", url);
	u->tls = !strcmp(scheme, "https");
	rc = curl_url_get(cu, CURLUPART_USER, &user, 0);
	fyai_error_check(ctx, rc == CURLUE_NO_USER, err,
			 "'%s' holds a user name; pass the token another way", url);
	rc = curl_url_get(cu, CURLUPART_HOST, &host, 0);
	fyai_error_check(ctx, !rc && *host, err, "'%s' has no host", url);
	/* An IPv6 address comes in brackets; the socket wants it without. */
	bare = host;
	n = strlen(host);
	if (host[0] == '[' && n > 2 && host[n - 1] == ']') {
		bare = host + 1;
		n -= 2;
	}
	fyai_error_check(ctx, n < sizeof(u->host), err, "'%s' has a host that is too long",
			 url);
	snprintf(u->host, sizeof(u->host), "%.*s", (int)n, bare);
	/* The port as written goes into the Host header; the default is for the socket. */
	rc = curl_url_get(cu, CURLUPART_PORT, &port, 0);
	if (rc) {
		fyai_error_check(ctx, rc == CURLUE_NO_PORT, err,
				 "'%s' has no valid port", url);
		snprintf(u->port, sizeof(u->port), u->tls ? "443" : "80");
		n = (size_t)snprintf(u->authority, sizeof(u->authority), "%s", host);
	} else {
		fyai_error_check(ctx, strlen(port) < sizeof(u->port), err,
				 "'%s' has no valid port", url);
		snprintf(u->port, sizeof(u->port), "%s", port);
		n = (size_t)snprintf(u->authority, sizeof(u->authority), "%s:%s", host,
				     port);
	}
	fyai_error_check(ctx, n < sizeof(u->authority), err, "'%s' has a host that is "
			 "too long", url);
	curl_free(scheme);
	curl_free(host);
	curl_free(port);
	curl_free(user);
	curl_url_cleanup(cu);
	return 0;
err:
	curl_free(scheme);
	curl_free(host);
	curl_free(port);
	curl_free(user);
	curl_url_cleanup(cu);
	return -1;
}
