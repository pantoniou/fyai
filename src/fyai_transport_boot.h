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
#include <stdint.h>
#include <sys/types.h>

#include <libfyaml.h>
#include <libfyaml/libfyaml-generic.h>

struct fyai_cfg;
struct fyai_ctx;

/* Private contract between the bootstrap and the supervisor image. */
#define FYAI_TRANSPORT_FD_ENV		"FYAI_TRANSPORT_FD"	/* agent channel */
#define FYAI_TRANSPORT_CTL_ENV		"FYAI_TRANSPORT_CTL"	/* control channel */
#define FYAI_TRANSPORT_EXEC_ENV		"FYAI_TRANSPORT_EXEC"	/* execution id */
#define FYAI_TRANSPORT_PID_ENV		"FYAI_TRANSPORT_PID"	/* the transport */
#define FYAI_TRANSPORT_OWNER_ENV	"FYAI_TRANSPORT_OWNER"	/* who ends it */
#define FYAI_TRANSPORT_KEYREF_ENV	"FYAI_TRANSPORT_KEYREF"	/* the source of a key */
#define FYAI_TRANSPORT_FORCED_ENV	"FYAI_TRANSPORT_FORCED"	/* the user set the level */
#define FYAI_TRANSPORT_ISOLATION_ENV	"FYAI_TRANSPORT_ISOLATION"
#define FYAI_TRANSPORT_REQUESTED_ENV	"FYAI_TRANSPORT_REQUESTED"	/* before auto resolved */

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

/*
 * Make sure the transport has the profiles that the configuration of this
 * agent needs, and that this execution may use them. An agent calls it before
 * a request: its provider or model can differ from its parent's, or change in
 * the session, and a catalogue update can move an endpoint. It costs nothing
 * once the profiles are stated. Return 0 or -1, with a diagnostic.
 */
int fyai_transport_ensure(struct fyai_ctx *ctx);

/*
 * Register a child process with the transport: give the transport its channel
 * @agent_fd and its control connection @ctl_fd, which stay open in this
 * process for the caller to close. The child is named by @pidfd when it is not
 * negative, which also names a child in a PID namespace of its own, else by @pid.
 * A negative @ctl_fd gives no control connection: the child inherits the one of
 * this process, as the session of a view does. Store the execution id in
 * @exec_id. The child starts with the grant of this agent and states its own
 * with fyai_transport_ensure(). Return 0 or -1, with a diagnostic.
 */
int fyai_transport_admit_child(struct fyai_ctx *ctx, pid_t pid, int pidfd,
			       int agent_fd, int ctl_fd, uint64_t *exec_id);

/*
 * The transport part of the spawn state that a parent sends to an executed
 * child: the execution id, and the names of the profiles that the parent
 * granted it, so that the child can grant them to its own children.
 */
fy_generic fyai_transport_spawn_state(struct fyai_ctx *ctx,
				      struct fy_generic_builder *gb,
				      uint64_t exec_id);

/*
 * Open the client of an executed child, on the descriptors that its parent
 * left it, once the parent has sent it the @state from
 * fyai_transport_spawn_state(). Return 0 or -1.
 */
int fyai_transport_child_attach(struct fyai_ctx *ctx, fy_generic state);

/* The descriptors of an executed child, a contract with its parent. */
#define FYAI_TRANSPORT_CHILD_AGENT_FD	5
#define FYAI_TRANSPORT_CHILD_CTL_FD	6

/*
 * The level in effect for this process: "none", or the level that the bootstrap
 * chose, such as "level-b". A setting of `auto` is not an answer; this is.
 */
const char *fyai_transport_effective_level(const struct fyai_ctx *ctx);

/*
 * Describe the isolation for a person. The transport is asked, because it is
 * the one that enforces the level: it reports its level, its process, the
 * executions it admitted, its profiles, and the transfers in flight. The text
 * adds what was asked for when that differs from the level in effect, and which
 * execution this process is. If the transport does not answer, the text says
 * so and gives the level that this process was started with. Write "none" when
 * this process talks to the provider itself.
 */
/*
 * The profiles of the transport and what execution @exec_id may use of them,
 * as the reply mapping `{id, profiles}` built in @gb. @exec_id 0 is the
 * caller. Return 0, or -1 with the cause reported.
 */
int fyai_transport_describe(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
			    uint64_t exec_id, fy_generic *out);
/*
 * Run the command with the CLI words @words at the transport, which holds the
 * credential stores, and return its result in *@out, built in @gb. Only
 * listed commands run there, and only for the primary image.
 */
int fyai_transport_command(struct fyai_ctx *ctx, struct fy_generic_builder *gb,
			   const char *const *words, size_t nwords,
			   int format, fy_generic *out);

/*
 * The configured isolation changed in a live session. Return 1 when the run
 * must restart to follow it, 0 when it already does, or -1 with @why set when
 * the level is fixed for this run. The isolation level is taken at startup:
 * a restart starts the transport, or ends it and clears the channels.
 */
int fyai_transport_config_changed(struct fyai_ctx *ctx, const char **why);

/*
 * Check that this run could start the transport: the host, the credential and
 * the endpoint. Return 0, or -1 with the cause reported. A run that already has
 * the transport passes. It changes nothing.
 */
int fyai_transport_preflight(struct fyai_ctx *ctx);

void fyai_transport_status_text(struct fyai_ctx *ctx, char *buf, size_t size);

/*
 * True if the credential that @cfg names can be resolved: this process holds a
 * key, the configuration takes none, or the transport, which is asked and
 * answers yes or no, has one. The value never comes back. False if the
 * transport does not answer.
 */
bool fyai_transport_have_credential(struct fyai_ctx *ctx, const struct fyai_cfg *cfg);

/*
 * Have the transport send the value of the named credentials into a socket,
 * for one command that the user configured, such as the catalogue scraper.
 * The process reads only the command child: this process never holds a value.
 * Only the root supervisor may ask. The read end is in ctx->transport_envfd
 * until fyai_transport_env_release(). With no transport this does nothing: the
 * environment already has the credentials. Return 0, or -1 with a diagnostic.
 */
int fyai_transport_env_grant(struct fyai_ctx *ctx, const char *const *names);

/*
 * In a forked command child, move the credentials of the grant into its
 * environment and close the socket. Do nothing when there is no grant.
 */
void fyai_transport_env_take(struct fyai_ctx *ctx);

/* Close the read end of the grant. */
void fyai_transport_env_release(struct fyai_ctx *ctx);

/* Tell the transport which logs are on, after `log start` or `log stop`. */
void fyai_transport_sync_logging(struct fyai_ctx *ctx);

/* Close the client. The process that started the transport also ends it. */
void fyai_transport_detach(struct fyai_ctx *ctx);

#endif
