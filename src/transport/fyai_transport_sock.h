/*
 * fyai_transport_sock.h - portable socket flags for the transport channels
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis@konsulko.com>
 * SPDX-License-Identifier: MIT
 */
#ifndef FYAI_TRANSPORT_SOCK_H
#define FYAI_TRANSPORT_SOCK_H

#include <sys/socket.h>

/*
 * A platform that has no MSG_NOSIGNAL runs with SIGPIPE blocked in every
 * process that writes a channel. A platform that has no MSG_CMSG_CLOEXEC gets
 * the close-on-exec flag from the caller after the receive.
 */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifndef MSG_CMSG_CLOEXEC
#define MSG_CMSG_CLOEXEC 0
#endif

/*
 * Make a connected pair of message-oriented Unix sockets with close-on-exec
 * set on both. Only Linux has such a socket, so elsewhere this fails with the
 * errno of socketpair(). Return 0, or -1 with errno.
 */
int fyai_transport_socketpair(int sv[2]);

#endif
