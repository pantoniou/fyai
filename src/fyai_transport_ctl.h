/*
 * fyai_transport_ctl.h - the control protocol of the credential transport
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * The supervisor drives the transport over one SOCK_SEQPACKET socketpair. A
 * datagram is one JSON mapping with an "op" and a "seq", and can carry one
 * file descriptor with SCM_RIGHTS. The transport answers each request with
 * {"op":"ok","seq":N} or {"op":"error","seq":N,"message":"..."}, and reports
 * events of its own with {"op":"event", ...}.
 *
 * Requests:
 *   init        level, cgroup (level-a), log        first, once
 *   profiles    profiles[]                          replace the profile set
 *   credential  name, value                         source "mem:NAME"
 *   admit       id, parent, pid, uid, grant[], ns   with the agent channel fd
 *   grant       id, grant[]                         replace one grant
 *   retire      id
 *   log         on
 *   shutdown
 *
 * A credential source is "env:NAME", "secret:NAME", or "mem:NAME", or a chain
 * of them separated by "|", tried in order: the first that holds a value wins.
 * A value sent in a credential request is held in the memory of the transport
 * only.
 */

#ifndef FYAI_TRANSPORT_CTL_H
#define FYAI_TRANSPORT_CTL_H

#include <stddef.h>

#include <libfyaml.h>
#include <libfyaml/libfyaml-generic.h>

#include "fyai_transport.h"

#define FYAI_CTL_MAX		(64 * 1024)
#define FYAI_CTL_MAX_GRANT	64

/*
 * Receive one datagram without blocking. The mapping is built in @gb. If the
 * datagram carries a descriptor, @fdp gets it (close-on-exec); with @fdp NULL
 * it is closed. Extra descriptors are closed. Return 0, -EAGAIN, -ECONNRESET
 * at end of file, -EMSGSIZE for truncated data, or -EPROTO when the datagram is
 * not a JSON mapping with a string "op".
 */
int fyai_ctl_recv(int sock, struct fy_generic_builder *gb, fy_generic *doc,
		  int *fdp);

/*
 * Send @doc, with @fd if it is not negative. @flags are sendmsg flags, such as
 * MSG_DONTWAIT for an event that can be dropped. Return 0 or a negative errno.
 */
int fyai_ctl_send(int sock, fy_generic doc, int fd, int flags);

/*
 * The same with up to two descriptors. @fd2p gets the second one, or -1; a
 * datagram with more than two has the others closed. With @fd2p NULL a second
 * descriptor is closed, as in fyai_ctl_recv().
 */
int fyai_ctl_recv2(int sock, struct fy_generic_builder *gb, fy_generic *doc,
		   int *fdp, int *fd2p);

/*
 * Send @doc with up to two descriptors: @fd2 only with a @fd. A descriptor
 * that is negative is not sent. Return 0 or a negative errno.
 */
int fyai_ctl_send2(int sock, fy_generic doc, int fd, int fd2, int flags);

/* Build a request or reply with the given op and sequence number. */
fy_generic fyai_ctl_reply_ok(struct fy_generic_builder *gb, long long seq);
fy_generic fyai_ctl_reply_error(struct fy_generic_builder *gb, long long seq,
				const char *message);

/*
 * Convert the "profiles" sequence to profiles of @out, which must be empty.
 * Return 0 or -EINVAL; @why then names the reason in static storage.
 */
int fyai_ctl_profiles_parse(fy_generic profiles, struct fyai_transport_grant *out,
			    const char **why);
fy_generic fyai_ctl_profiles_encode(struct fy_generic_builder *gb,
				    const struct fyai_transport_grant *grant);

/*
 * Convert a "grant" sequence to @out, up to @max entries. Strings belong to
 * @gb. Return 0 or -EINVAL.
 */
int fyai_ctl_grant_parse(struct fy_generic_builder *gb, fy_generic grant,
			 struct fyai_transport_allow *out, size_t max,
			 size_t *count);
fy_generic fyai_ctl_grant_encode(struct fy_generic_builder *gb,
				 const struct fyai_transport_allow *allow,
				 size_t count);

/* Convert an "ns" mapping such as {"net": 4026531840}. Return 0 or -EINVAL. */
int fyai_ctl_ns_parse(fy_generic ns, struct fyai_transport_ns_req *out);

#endif
