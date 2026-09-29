/*
 * fyai_transport_boot.h - start the credential transport and become its client
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * With agent/transport_isolation on, an invocation runs in two steps. The
 * bootstrap is the invocation as the user started it: it may hold a key in its
 * environment or its arguments. It starts the transport, gives it the profiles
 * and the credentials, registers itself as an agent, removes every credential
 * from its environment and its arguments, and executes itself again. The new
 * image is the supervisor. It has no credential, and it finds its channel to
 * the transport in the environment.
 *
 * The bootstrap keeps its process ID across the execution, so the registration
 * of the transport, which holds a pidfd of it, stays valid.
 */

#ifndef FYAI_TRANSPORT_BOOT_H
#define FYAI_TRANSPORT_BOOT_H

#include <stdbool.h>

struct fyai_cfg;
struct fyai_ctx;

/* Private contract between the bootstrap and the supervisor image. */
#define FYAI_TRANSPORT_FD_ENV		"FYAI_TRANSPORT_FD"	/* agent channel */
#define FYAI_TRANSPORT_CTL_ENV		"FYAI_TRANSPORT_CTL"	/* control channel */
#define FYAI_TRANSPORT_EXEC_ENV		"FYAI_TRANSPORT_EXEC"	/* execution id */
#define FYAI_TRANSPORT_PID_ENV		"FYAI_TRANSPORT_PID"	/* the transport */
#define FYAI_TRANSPORT_OWNER_ENV	"FYAI_TRANSPORT_OWNER"	/* who ends it */
#define FYAI_TRANSPORT_KEYREF_ENV	"FYAI_TRANSPORT_KEYREF"	/* the source of a key */
#define FYAI_TRANSPORT_ISOLATION_ENV	"FYAI_TRANSPORT_ISOLATION"

/* True if this image was started by a bootstrap and has a channel. */
bool fyai_transport_supervised(void);

/*
 * True for the name of an environment variable that holds a model provider
 * credential: <PROVIDER>_API_KEY, except the key of an MCP server, which the
 * MCP client reads.
 */
bool fyai_env_is_credential(const char *name);

/*
 * If @cfg asks for isolation and runs something that talks to a model, start
 * the transport and execute this program again as the supervisor; that does
 * not return. Otherwise return 0 and change nothing. Return -1, with a
 * diagnostic in @cfg, if isolation is asked for and cannot be set up: there is
 * no fallback to a direct connection.
 */
int fyai_transport_bootstrap(struct fyai_cfg *cfg, int argc, char **argv);

/*
 * Open the client of a supervisor. Return 0 when this image is not one, or
 * when it is and the client is open; -1, with a diagnostic, when the channel
 * is missing.
 */
int fyai_transport_attach(struct fyai_ctx *ctx);

/* Tell the transport which logs are on, after `log start` or `log stop`. */
void fyai_transport_sync_logging(struct fyai_ctx *ctx);

/* Close the client. The process that started the transport also ends it. */
void fyai_transport_detach(struct fyai_ctx *ctx);

#endif
