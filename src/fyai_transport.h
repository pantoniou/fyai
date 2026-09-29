/*
 * fyai_transport.h - credential transport: wire frames and agent admission
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * The transport process holds the credentials. An agent reaches it only
 * through a SOCK_SEQPACKET channel that the supervisor created and registered.
 * The transport is an allow-listed egress proxy: it does not know the content
 * of a payload. This module holds the parts that decide who may speak and to
 * which destination: the frame format, the egress grant, and the registry that
 * authenticates each received message.
 * See doc/agent-transport-isolation-sdd.md.
 */

#ifndef FYAI_TRANSPORT_H
#define FYAI_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define FYAI_TRANSPORT_MAGIC		0x31545946u	/* "FYT1" */
#define FYAI_TRANSPORT_VERSION		1
#define FYAI_TRANSPORT_HDR_SIZE		40
/* Largest datagram, header included. Larger payloads use fragments. */
#define FYAI_TRANSPORT_MAX_FRAME	(64 * 1024)
#define FYAI_TRANSPORT_MAX_PAYLOAD	(FYAI_TRANSPORT_MAX_FRAME - FYAI_TRANSPORT_HDR_SIZE)
/* Largest reassembled message from an agent. */
#define FYAI_TRANSPORT_MAX_MESSAGE	(64u * 1024 * 1024)

/*
 * Containment level, the value of agent/transport_isolation. The level says how
 * the transport knows that a sender belongs to the admitted set.
 *
 * NONE   no transport; agents hold the credentials. Never called isolated.
 * AUTO   the supervisor selects the highest level the host supports.
 * A      cgroup v2 subtree, verified for every message.
 * B      pidfd, PID and UID only. Weaker: nothing bounds descendants.
 *
 * Only A and B name a registry. The supervisor resolves AUTO before it
 * creates one and reports the result.
 */
enum fyai_transport_level {
	FYAI_TL_NONE,
	FYAI_TL_AUTO,
	FYAI_TL_A,
	FYAI_TL_B,
};

const char *fyai_transport_level_name(enum fyai_transport_level level);
/* Return 0 or -EINVAL. Names: none, auto, level-a, level-b. */
int fyai_transport_level_parse(const char *name, enum fyai_transport_level *level);

enum fyai_transport_kind {
	FYAI_TK_REQUEST = 1,		/* agent: start a request */
	FYAI_TK_CANCEL,			/* agent: cancel a request */
	FYAI_TK_CREDIT,			/* agent: grant response credit */
	FYAI_TK_RESP_START,		/* transport: status and metadata */
	FYAI_TK_RESP_BODY,		/* transport: exact response bytes */
	FYAI_TK_RESP_END,		/* transport: terminal frame */
	FYAI_TK_RESP_ERROR,		/* transport: terminal error */
	FYAI_TK_COUNT,
};

/* The payload continues in the next frame of the same request. */
#define FYAI_TF_MORE			(1u << 0)
#define FYAI_TF_KNOWN			(FYAI_TF_MORE)

struct fyai_transport_hdr {
	uint16_t kind;			/* enum fyai_transport_kind */
	uint32_t flags;			/* FYAI_TF_* */
	uint64_t exec_id;
	uint64_t request_id;
	uint64_t seq;			/* 0, 1, ... within one message or stream */
	uint32_t len;			/* payload bytes that follow */
};

/* Encode @hdr into @out, which holds FYAI_TRANSPORT_HDR_SIZE bytes. */
void fyai_transport_hdr_encode(uint8_t *out, const struct fyai_transport_hdr *hdr);

/* Return 0, or -EPROTO when the header is not valid. Check bounds only. */
int fyai_transport_hdr_decode(const uint8_t *in, size_t size,
			      struct fyai_transport_hdr *hdr);

/* Return the kind name for a trace record. */
const char *fyai_transport_kind_name(unsigned int kind);

enum fyai_transport_auth {
	FYAI_TA_NONE,			/* no credential is sent */
	FYAI_TA_BEARER,			/* Authorization: Bearer <credential> */
	FYAI_TA_HEADER,			/* <header>: <credential> */
};

/*
 * An egress profile is one destination that a request may reach. The
 * supervisor makes it from trusted configuration, and replaces the set when the
 * configuration or the catalogue changes. A request names a profile;
 * it never names a URL, a header, or a credential. @credential names a source
 * that only the transport resolves, never the value. @tag and @model are
 * opaque to the transport and go back to the agent so that it can parse the
 * response. Strings belong to the grant.
 */
struct fyai_transport_profile {
	char *name;
	char *url;			/* exact endpoint */
	char *tag;			/* may be NULL */
	char *model;			/* may be NULL */
	enum fyai_transport_auth auth;
	char *header;			/* FYAI_TA_HEADER only */
	char *credential;		/* not FYAI_TA_NONE */
	char **headers;			/* fixed "Name: value" lines, trusted */
	size_t nheaders;
	bool plain_http;		/* http to a host that is not loopback */
};

struct fyai_transport_grant {
	struct fyai_transport_profile *profiles;
	size_t count;
};

/* The input of one profile. Strings are copied. */
struct fyai_transport_profile_spec {
	const char *name;
	const char *url;
	const char *tag;		/* may be NULL */
	const char *model;		/* may be NULL */
	enum fyai_transport_auth auth;
	const char *header;		/* FYAI_TA_HEADER only */
	const char *credential;		/* not FYAI_TA_NONE */
	bool plain_http;
};

/*
 * Add a profile. The endpoint must be https with a host and no userinfo, with
 * two exceptions. A loopback endpoint can use http, with or without
 * authentication: the credential does not leave the host. Any other http
 * endpoint needs @spec->plain_http, which the trusted configuration sets when
 * the user chose an http URL: local model servers on a network or in a
 * container are usually reached that way. An authenticated profile needs a
 * credential source, and a profile with no authentication has none. Names are
 * unique. Return 0, -EINVAL, -EEXIST or -ENOMEM; the grant is unchanged on
 * failure.
 */
int fyai_transport_grant_add_spec(struct fyai_transport_grant *grant,
				  const struct fyai_transport_profile_spec *spec);

/* The same for an endpoint that needs no plain_http. */
int fyai_transport_grant_add(struct fyai_transport_grant *grant,
			     const char *name, const char *url,
			     const char *tag, const char *model,
			     enum fyai_transport_auth auth, const char *header,
			     const char *credential);
/*
 * Add a fixed, non-secret header to profile @name, such as a protocol version.
 * It comes from trusted configuration, never from a request. The name must be a
 * valid field name that does not frame the request or carry authentication;
 * the value has no control character. Return 0, -EINVAL, -ENOENT or -ENOMEM.
 */
int fyai_transport_grant_add_header(struct fyai_transport_grant *grant,
				    const char *name, const char *field,
				    const char *value);

/*
 * Copy the profiles of @src into @dst. A profile of the same name in @dst is
 * replaced, and the others are kept. Return 0 or -ENOMEM; on failure @dst may
 * hold part of @src.
 */
int fyai_transport_grant_merge(struct fyai_transport_grant *dst,
			       const struct fyai_transport_grant *src);

const struct fyai_transport_profile *
fyai_transport_grant_find(const struct fyai_transport_grant *grant,
			  const char *name);
void fyai_transport_grant_clear(struct fyai_transport_grant *grant);

/*
 * Namespaces whose identity the transport checks. The supervisor creates them
 * for an agent and states each inode; the sender must be in exactly that one,
 * and it must differ from the namespace of the transport, or the isolation
 * means nothing.
 */
enum fyai_transport_ns {
	FYAI_NS_NET,
	FYAI_NS_MNT,
	FYAI_NS_PID,
	FYAI_NS_COUNT,
};

struct fyai_transport_ns_req {
	unsigned int mask;		/* bit (1 << FYAI_NS_x) */
	uint64_t ino[FYAI_NS_COUNT];	/* st_ino of /proc/PID/ns/x */
};

enum fyai_transport_verdict {
	FYAI_TV_OK = 0,			/* complete message in @msg */
	FYAI_TV_MORE,			/* fragment stored; no message yet */
	FYAI_TV_CLOSED,			/* the peer closed the channel */
	FYAI_TV_AGAIN,			/* nothing to read */
	FYAI_TV_UNKNOWN_CHANNEL,	/* no registration for the channel */
	FYAI_TV_TRUNCATED,		/* data or control message truncated */
	FYAI_TV_NO_CREDENTIALS,		/* the kernel gave no sender identity */
	FYAI_TV_WRONG_SENDER,		/* PID or UID differs from the record */
	FYAI_TV_DEAD,			/* the registered process has exited */
	FYAI_TV_CONTAINMENT,		/* the sender is outside its containment */
	FYAI_TV_BAD_FRAME,		/* malformed header or sequence */
	FYAI_TV_WRONG_EXEC,		/* frame names another execution */
	FYAI_TV_BAD_KIND,		/* kind an agent may not send */
	FYAI_TV_TOO_LARGE,		/* reassembly bound exceeded */
	FYAI_TV_NOSYS,			/* platform has no verification */
	FYAI_TV_ERROR,			/* system error; see errno */
};

const char *fyai_transport_verdict_str(enum fyai_transport_verdict v);

struct fyai_transport_exec;
struct fyai_transport_registry;

/*
 * A received message. @payload belongs to the execution and stays valid until
 * the next receive on its channel.
 */
struct fyai_transport_msg {
	struct fyai_transport_exec *exec;
	struct fyai_transport_hdr hdr;
	const uint8_t *payload;
	size_t len;
};

/*
 * Create a registry for @level, which is FYAI_TL_A or FYAI_TL_B. Level A needs
 * @cgroup_root: the cgroup v2 path, relative to the cgroup mount, of the
 * subtree that holds the admitted agents; the transport is outside it. Level B
 * takes NULL. Return NULL for any other combination.
 */
struct fyai_transport_registry *
fyai_transport_registry_create(enum fyai_transport_level level,
			       const char *cgroup_root);
void fyai_transport_registry_destroy(struct fyai_transport_registry *reg);

enum fyai_transport_level
fyai_transport_registry_level(const struct fyai_transport_registry *reg);

/*
 * Return the containment check that rejected the last message or
 * registration ("cgroup", "ns-net", ...), else NULL. Static storage.
 */
const char *
fyai_transport_registry_failed(const struct fyai_transport_registry *reg);

/*
 * The grant of one execution: the profiles it may name, each with an optional
 * narrower model. It holds names, not definitions. The definitions belong to
 * the server and can change while it runs, so a reload updates every grant at
 * once. An execution with an empty grant can reach nothing.
 */
struct fyai_transport_allow {
	const char *profile;
	const char *model;		/* NULL: the model of the profile */
};

/*
 * Admit one execution. Only the supervisor control channel calls this. The
 * process must already meet the containment of the level and @ns, which can be
 * NULL. @allow is copied. On success the registry owns @channel. On failure
 * the caller keeps it. Return 0 or a negative errno; -EPERM means the process
 * is not contained, and -EINVAL a grant with a bad or repeated name.
 */
int fyai_transport_register(struct fyai_transport_registry *reg, uint64_t id,
			    uint64_t parent_id, pid_t pid, uid_t uid,
			    int channel, const struct fyai_transport_allow *allow,
			    size_t nallow, const struct fyai_transport_ns_req *ns);

/*
 * Replace the grant of an execution, for example when its agent changes the
 * provider or the model. Return 0, -ENOENT or -EINVAL; the old grant stands on
 * failure.
 */
int fyai_transport_set_grant(struct fyai_transport_registry *reg, uint64_t id,
			     const struct fyai_transport_allow *allow,
			     size_t nallow);

/*
 * Return true if @exec may name @profile. Store the narrower model of the
 * grant, or NULL, in @model.
 */
bool fyai_transport_exec_allows(const struct fyai_transport_exec *exec,
				const char *profile, const char **model);

/* Retire an execution: close the channel and the pidfd. Return 0 or -ENOENT. */
int fyai_transport_retire(struct fyai_transport_registry *reg, uint64_t id);

struct fyai_transport_exec *
fyai_transport_find(struct fyai_transport_registry *reg, uint64_t id);
struct fyai_transport_exec *
fyai_transport_find_channel(struct fyai_transport_registry *reg, int channel);

/* The admitted executions, for a status report. Return false past the end. */
size_t fyai_transport_registry_count(const struct fyai_transport_registry *reg);
bool fyai_transport_registry_exec_info(const struct fyai_transport_registry *reg,
				       size_t index, uint64_t *id,
				       uint64_t *parent, pid_t *pid);

uint64_t fyai_transport_exec_id(const struct fyai_transport_exec *exec);
int fyai_transport_exec_channel(const struct fyai_transport_exec *exec);

/*
 * Read one datagram from a registered channel and authenticate it. A message
 * passes only when the kernel-supplied credentials name the registered PID and
 * UID, the pidfd still names a live process, the sender meets the containment
 * of the level and of the execution, and the frame is well formed. Any other
 * result leaves @msg unset. The caller retires the execution on FYAI_TV_CLOSED
 * and on a verdict that shows a copied descriptor.
 */
enum fyai_transport_verdict
fyai_transport_recv(struct fyai_transport_registry *reg, int channel,
		    struct fyai_transport_msg *msg);

/* Enable per-message credentials on an agent endpoint held by the transport. */
int fyai_transport_channel_prepare(int channel);

/* Send one frame. Return 0, -EMSGSIZE if @hdr->len exceeds one datagram, or
 * a negative errno. */
int fyai_transport_send_frame(int channel, const struct fyai_transport_hdr *hdr,
			      const void *payload);

/*
 * Send one message as fragments of up to FYAI_TRANSPORT_MAX_PAYLOAD bytes.
 * Sequence numbers start at 0. Return 0 or a negative errno.
 */
int fyai_transport_send_message(int channel, uint16_t kind, uint64_t exec_id,
				uint64_t request_id, const void *payload,
				size_t len);

#endif
