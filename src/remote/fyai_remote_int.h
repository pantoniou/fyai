/* SPDX-License-Identifier: MIT */
/* What the client and the server of a remote session share. */
#ifndef FYAI_REMOTE_INT_H
#define FYAI_REMOTE_INT_H

#include <stdbool.h>
#include <stddef.h>

#include "fyai.h"
#include "fyai_jsonrpc.h"

/* A peer that reads no more than this is dropped, not buffered without end. */
#define REMOTE_OUTPUT_MAX	(32u * 1024u * 1024u)
/* The time a peer has to authenticate or to have its branch attached. */
#define REMOTE_ADMIT_MS		10000

/* The features of the fyai protocol that this program has. */
static const char *const remote_features[] = {
	"project", "run", "frames", "session", "resume", "server", "model",
};

int fyai_remote_cloexec(int fd);
int fyai_remote_socketpair(int type, int sv[2]);
/*
 * Run @args on one end of a socket pair. The other end goes to @fdp and the
 * pid of the child to @pidp. The child has the pair as its standard input and
 * output, the standard error of this process, and of the environment only the
 * names in the NULL-terminated list @keep and those that every child of fyai
 * keeps.
 */
int fyai_remote_spawn(struct fyai_ctx *ctx, char *const args[],
		      const char *const keep[], int *fdp, pid_t *pidp);
int fyai_remote_send(struct jsonrpc_conn *conn, fy_generic frame);
int fyai_remote_mkdirs(struct fyai_cfg *cfg, size_t top, const char *path);
char *fyai_remote_socket_path(struct fyai_ctx *ctx, const char *override,
			      bool user, bool create);

#endif
