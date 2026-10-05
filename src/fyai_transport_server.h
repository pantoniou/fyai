/*
 * fyai_transport_server.h - the request engine of the credential transport
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * The server serves the channels of the registry on the event loop of a
 * context. It turns an authenticated request into a curl transfer to the
 * endpoint of the granted profile, adds the credential, and streams the
 * response back in bounded frames. A slow agent pauses only its own transfer.
 * See doc/agent-transport-isolation-sdd.md.
 */

#ifndef FYAI_TRANSPORT_SERVER_H
#define FYAI_TRANSPORT_SERVER_H

#include <curl/curl.h>

#include "fyai_event.h"
#include "fyai_transport.h"
#include "fyai_transport_msg.h"

struct fyai_ctx;
struct fyai_transport_server;

/*
 * Resolve the credential source of @pr. Return 0 and a malloc'd secret that
 * the server clears and frees, or a negative errno. A source can also state
 * header lines that it derives from the credential, such as an account
 * identifier: it sets @extra to a list that the server frees, or leaves it
 * NULL. The callback runs in the transport and is the only code that reads a
 * credential.
 */
typedef int (*fyai_transport_cred_fn)(void *userdata,
				      const struct fyai_transport_profile *pr,
				      char **secret, struct curl_slist **extra);

/*
 * Make the credential of @pr usable before a request reads it, for a source
 * that refreshes. Return 0 when the credential is ready, a positive value when
 * the source started work and will call fyai_transport_server_prepared() when
 * it ends, or a negative errno. @resumed is true for a request that waited:
 * the source must then answer from its current state and start no new work, so
 * that a failed refresh ends each waiting request one time.
 */
typedef int (*fyai_transport_prepare_fn)(void *userdata,
					 const struct fyai_transport_profile *pr,
					 bool resumed);

/*
 * A request was answered with HTTP 401 and the credential may be renewed.
 * With @probe set, say whether the source of @pr can renew a credential
 * (nonzero) and do nothing else. Otherwise renew it: @tag identifies the
 * credential that the request used (fyai_transport_credential_tag()), and a
 * credential that has changed since needs no renewal. Return 0 when a retry
 * can start now, a positive value when the source started work and will call
 * fyai_transport_server_prepared(), or a negative errno when no renewal is
 * possible and the response goes to the agent.
 */
typedef int (*fyai_transport_reject_fn)(void *userdata,
					const struct fyai_transport_profile *pr,
					uint64_t tag, bool probe);

/* The tag that names a credential value, for fyai_transport_reject_fn. */
uint64_t fyai_transport_credential_tag(const char *secret);

/*
 * Report a rejected message or a retired execution. @event is a static
 * string. The callback is optional and never receives request content.
 */
typedef void (*fyai_transport_log_fn)(void *userdata, uint64_t exec_id,
				      const char *event, const char *detail);

/* @reg must outlive the server, which does not destroy it. */
struct fyai_transport_server *
fyai_transport_server_create(struct fyai_ctx *ctx,
			     struct fyai_transport_registry *reg,
			     fyai_transport_cred_fn cred, void *cred_userdata,
			     fyai_transport_log_fn log, void *log_userdata);

/* Install the optional preparation of credentials. */
void fyai_transport_server_set_prepare(struct fyai_transport_server *srv,
				       fyai_transport_prepare_fn prepare);

/*
 * Install the optional renewal after an HTTP 401. A request is retried one
 * time, and the agent never sees the rejected response.
 */
void fyai_transport_server_set_reject(struct fyai_transport_server *srv,
				      fyai_transport_reject_fn reject);

/* The work that a prepare callback started has ended: run the waiting requests. */
void fyai_transport_server_prepared(struct fyai_transport_server *srv);

/* Cancel every transfer, remove the sources, and free the server. */
void fyai_transport_server_destroy(struct fyai_transport_server *srv);

/*
 * Replace the profiles that requests can name. The server takes the profiles
 * of @grant, which is cleared. A request in flight keeps the profile it
 * started with; later requests see the new set. The supervisor calls this
 * when a configuration or catalogue change moves an endpoint, a provider, or a
 * model. Return 0 or -ENOMEM.
 */
int fyai_transport_server_set_profiles(struct fyai_transport_server *srv,
				       struct fyai_transport_grant *grant);

/*
 * Add profiles to the current set: a profile of the same name is replaced and
 * the others stay. Each agent adds the profiles of its own configuration, so
 * the set grows with the providers in use. @add is cleared. Return 0 or
 * -ENOMEM.
 */
int fyai_transport_server_add_profiles(struct fyai_transport_server *srv,
				       struct fyai_transport_grant *add);

/*
 * Register an execution with its grant and start serving its channel. The
 * arguments are those of fyai_transport_register(); ownership follows it. The
 * server makes the transport end of the channel non-blocking.
 */
int fyai_transport_server_admit(struct fyai_transport_server *srv, uint64_t id,
				uint64_t parent_id, pid_t pid, uid_t uid,
				int channel,
				const struct fyai_transport_allow *allow,
				size_t nallow,
				const struct fyai_transport_ns_req *ns);

/*
 * The same for an execution that the caller names by a pidfd. See
 * fyai_transport_register_pidfd(); the pidfd is owned as the channel is.
 */
int fyai_transport_server_admit_pidfd(struct fyai_transport_server *srv, uint64_t id,
				      uint64_t parent_id, int pidfd, uid_t uid,
				      int channel,
				      const struct fyai_transport_allow *allow,
				      size_t nallow,
				      const struct fyai_transport_ns_req *ns);

/*
 * Replace the grant of an execution. A request in flight is not affected; the
 * next request is checked against the new grant. See fyai_transport_set_grant().
 */
int fyai_transport_server_set_grant(struct fyai_transport_server *srv,
				    uint64_t id,
				    const struct fyai_transport_allow *allow,
				    size_t nallow);

/* Cancel the transfers of an execution, close its channel, and retire it. */
int fyai_transport_server_retire(struct fyai_transport_server *srv, uint64_t id);

/* The profiles of the current set, or NULL before the first is set. */
const struct fyai_transport_grant *
fyai_transport_server_profiles(const struct fyai_transport_server *srv);

/* The profile names of the current set, for a status report. */
size_t fyai_transport_server_profile_count(const struct fyai_transport_server *srv);
const char *fyai_transport_server_profile_name(const struct fyai_transport_server *srv,
					       size_t index);

/* Number of transfers that wait for credit or for the agent to read. */
size_t fyai_transport_server_paused(const struct fyai_transport_server *srv);

/* Number of transfers in flight, for tests and status. */
size_t fyai_transport_server_active(const struct fyai_transport_server *srv);

/* The latest rate-limit state that the endpoint reported. */
struct fyai_transport_ratelimit_record {
	long status;			/* HTTP status of that response */
	long retry_after_s;		/* -1 when absent */
	fyai_event_ms_t when_ms;
	struct fyai_transport_ratelimit rl;
};

/*
 * Return true and copy the latest record for the endpoint @url. Providers
 * limit their rate, and the transport records what they report. It does not
 * act on it: it adds no delay, no retry, and no refusal. A response that states
 * no limit and is not a 429 leaves the record as it was.
 */
bool fyai_transport_server_ratelimit(const struct fyai_transport_server *srv,
				     const char *url,
				     struct fyai_transport_ratelimit_record *out);

/*
 * Build the request header lines: content type, accept, the fixed lines of the
 * profile, and the credential. Return 0 or -EINVAL when @secret has a
 * control character. Exported for tests. Free with curl_slist_free_all().
 */
int fyai_transport_headers_build(const struct fyai_transport_profile *pr,
				 const struct fyai_transport_request *rq,
				 const char *secret, struct curl_slist **out);

#endif
