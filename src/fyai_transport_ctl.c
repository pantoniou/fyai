/*
 * fyai_transport_ctl.c - the control protocol of the credential transport
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include "fyai_transport_ctl.h"
#include "fyai_transport_sock.h"
#include "utils.h"

#define CTL_MAX_FDS 4

/*
 * Take the first descriptors of a datagram into @keep, up to @nkeep of them; a
 * descriptor with no place is closed. A NULL @keep closes them all.
 */
static void take_fds(struct msghdr *mh, int *keep, size_t nkeep)
{
	struct cmsghdr *c;
	size_t n, i, taken = 0;
	int *fds;

	for (c = CMSG_FIRSTHDR(mh); c; c = CMSG_NXTHDR(mh, c)) {
		if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS)
			continue;
		n = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
		fds = (int *)CMSG_DATA(c);
		for (i = 0; i < n; i++) {
			if (keep && taken < nkeep) {
				keep[taken++] = fds[i];
				continue;
			}
			close(fds[i]);
		}
	}
}

int fyai_ctl_recv2(int sock, struct fy_generic_builder *gb, fy_generic *doc,
		   int *fdp, int *fd2p)
{
	char buf[FYAI_CTL_MAX + 1];
	union {
		struct cmsghdr align;
		char raw[CMSG_SPACE(CTL_MAX_FDS * sizeof(int))];
	} ctl;
	struct iovec iov = { .iov_base = buf, .iov_len = sizeof(buf) - 1 };
	struct msghdr mh = {
		.msg_iov = &iov, .msg_iovlen = 1,
		.msg_control = ctl.raw, .msg_controllen = sizeof(ctl.raw),
	};
	int fd[2] = { -1, -1 }, i;
	ssize_t n;
	fy_generic op;

	n = recvmsg(sock, &mh, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
	if (n < 0)
		return (errno == EAGAIN || errno == EWOULDBLOCK) ? -EAGAIN : -errno;
	take_fds(&mh, fdp ? fd : NULL, fd2p ? 2 : 1);
	/* Where the receive cannot set it, a received descriptor gets it here. */
	for (i = 0; i < 2; i++)
		if (fd[i] >= 0)
			(void)fcntl(fd[i], F_SETFD, FD_CLOEXEC);
	if (n == 0)
		return -ECONNRESET;
	if (mh.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) {
		for (i = 0; i < 2; i++)
			if (fd[i] >= 0)
				close(fd[i]);
		return -EMSGSIZE;
	}

	buf[n] = '\0';
	*doc = parse_json_string_size(gb, buf, n);
	op = fy_get(*doc, "op", fy_invalid);
	if (!fy_is_mapping(*doc) || !fy_is_string(op) || fy_empty(op)) {
		for (i = 0; i < 2; i++)
			if (fd[i] >= 0)
				close(fd[i]);
		return -EPROTO;
	}
	if (fdp)
		*fdp = fd[0];
	if (fd2p)
		*fd2p = fd[1];
	return 0;
}

int fyai_ctl_recv(int sock, struct fy_generic_builder *gb, fy_generic *doc,
		  int *fdp)
{
	return fyai_ctl_recv2(sock, gb, doc, fdp, NULL);
}

int fyai_ctl_send2(int sock, fy_generic doc, int fd, int fd2, int flags)
{
	char storage[4 * FYAI_CTL_MAX + FY_GENERIC_BUILDER_LINEAR_IN_PLACE_MIN_SIZE];
	struct fy_generic_builder *tmp;
	union {
		struct cmsghdr align;
		char raw[CMSG_SPACE(2 * sizeof(int))];
	} ctl;
	struct iovec iov;
	struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1 };
	const char *json;
	int rc, fds[2], nfds = 0;
	ssize_t n;
	struct cmsghdr *c;

	tmp = fy_generic_builder_create_in_place(FYGBCF_SCOPE_LEADER, NULL,
						 storage, sizeof(storage));
	if (!tmp)
		return -ENOMEM;
	json = emit_json_string(tmp, doc);
	if (!json) {
		rc = -EMSGSIZE;
		goto out;
	}
	iov.iov_base = (void *)json;
	iov.iov_len = strlen(json);
	if (iov.iov_len > FYAI_CTL_MAX) {
		rc = -EMSGSIZE;
		goto out;
	}
	if (fd >= 0)
		fds[nfds++] = fd;
	if (fd2 >= 0) {
		/* The second descriptor has a place only after the first. */
		if (fd < 0) {
			rc = -EINVAL;
			goto out;
		}
		fds[nfds++] = fd2;
	}
	if (nfds) {
		memset(&ctl, 0, sizeof(ctl));
		mh.msg_control = ctl.raw;
		mh.msg_controllen = CMSG_SPACE(nfds * sizeof(int));
		c = CMSG_FIRSTHDR(&mh);
		c->cmsg_level = SOL_SOCKET;
		c->cmsg_type = SCM_RIGHTS;
		c->cmsg_len = CMSG_LEN(nfds * sizeof(int));
		memcpy(CMSG_DATA(c), fds, nfds * sizeof(int));
	}
	do
		n = sendmsg(sock, &mh, MSG_NOSIGNAL | flags);
	while (n < 0 && errno == EINTR);
	rc = n < 0 ? -errno : 0;
out:
	return rc;
}

int fyai_ctl_send(int sock, fy_generic doc, int fd, int flags)
{
	return fyai_ctl_send2(sock, doc, fd, -1, flags);
}

fy_generic fyai_ctl_reply_ok(struct fy_generic_builder *gb, long long seq)
{
	return fy_mapping(gb, "op", "ok", "seq", seq);
}

fy_generic fyai_ctl_reply_error(struct fy_generic_builder *gb, long long seq,
				const char *message)
{
	return fy_mapping(gb, "op", "error", "seq", seq, "message", message);
}

/* Profiles. */

static const char *opt_string(struct fy_generic_builder *gb, fy_generic m,
			      const char *key, bool *bad)
{
	fy_generic v = fy_get(m, key, fy_invalid);

	if (fy_is_invalid(v) || fy_is_null(v))
		return NULL;
	if (!fy_is_string(v)) {
		if (bad)
			*bad = true;
		return NULL;
	}
	return fy_gb_intern_string(gb, fy_castp(&v, ""));
}

static int profile_validate(fy_generic profiles, const char **why)
{
	static const char *const optional[] = {
		"tag", "model", "header", "credential"
	};
	fy_generic item, hdrs, key, val, field;
	const char *auth;
	size_t i;

	if (!fy_is_sequence(profiles)) {
		*why = "profiles is not a sequence";
		return -EINVAL;
	}
	fy_foreach(item, profiles) {
		if (!fy_is_mapping(item)) {
			*why = "a profile is not a mapping";
			return -EINVAL;
		}
		field = fy_get(item, "name", fy_invalid);
		if (!fy_is_string(field) || fy_empty(field)) {
			*why = "a profile needs a name, a url, and an auth";
			return -EINVAL;
		}
		field = fy_get(item, "url", fy_invalid);
		if (!fy_is_string(field) || fy_empty(field)) {
			*why = "a profile needs a name, a url, and an auth";
			return -EINVAL;
		}
		field = fy_get(item, "auth", fy_invalid);
		auth = fy_is_string(field) ? fy_castp(&field, "") : NULL;
		if (!auth || !*auth) {
			*why = "a profile needs a name, a url, and an auth";
			return -EINVAL;
		}
		if (strcmp(auth, "none") && strcmp(auth, "bearer") &&
		    strcmp(auth, "header")) {
			*why = "auth is not none, bearer, or header";
			return -EINVAL;
		}
		for (i = 0; i < sizeof(optional) / sizeof(optional[0]); i++) {
			field = fy_get(item, optional[i], fy_invalid);
			if (!fy_is_invalid(field) && !fy_is_null(field) &&
			    !fy_is_string(field)) {
				*why = "a profile string has the wrong type";
				return -EINVAL;
			}
		}
		field = fy_get(item, "plain_http", fy_invalid);
		if (!fy_is_invalid(field) && !fy_generic_is_bool(field)) {
			*why = "plain_http is not a boolean";
			return -EINVAL;
		}
		hdrs = fy_get(item, "headers", fy_invalid);
		if (!fy_is_invalid(hdrs) && !fy_is_null(hdrs) &&
		    !fy_is_mapping(hdrs)) {
			*why = "headers is not a mapping";
			return -EINVAL;
		}
		if (fy_is_mapping(hdrs)) {
			fy_foreach_key_value(key, val, hdrs) {
				if (!fy_is_string(key) || !fy_is_string(val)) {
					*why = "a fixed header is not a string";
					return -EINVAL;
				}
			}
		}
	}
	return 0;
}

int fyai_ctl_profiles_parse(fy_generic profiles, struct fyai_transport_grant *out,
			    const char **why)
{
	char storage[2 * FYAI_CTL_MAX + FY_GENERIC_BUILDER_LINEAR_IN_PLACE_MIN_SIZE];
	struct fy_generic_builder *gb;
	fy_generic item, hdrs, key, val;
	enum fyai_transport_auth auth;
	const char *name, *url, *tag, *model, *header, *cred, *as, *k, *v;
	struct fyai_transport_profile_spec spec;
	int rc = 0;

	rc = profile_validate(profiles, why);
	if (rc)
		return rc;
	gb = fy_generic_builder_create_in_place(FYGBCF_SCOPE_LEADER, NULL,
						 storage, sizeof(storage));
	if (!gb) {
		*why = "out of memory";
		return -ENOMEM;
	}
	fy_foreach(item, profiles) {
		name = opt_string(gb, item, "name", NULL);
		url = opt_string(gb, item, "url", NULL);
		tag = opt_string(gb, item, "tag", NULL);
		model = opt_string(gb, item, "model", NULL);
		header = opt_string(gb, item, "header", NULL);
		cred = opt_string(gb, item, "credential", NULL);
		as = opt_string(gb, item, "auth", NULL);
		if (!name || !url || !as) {
			*why = "out of memory";
			rc = -ENOMEM;
			goto err_out;
		}
		if (!strcmp(as, "none"))
			auth = FYAI_TA_NONE;
		else if (!strcmp(as, "bearer"))
			auth = FYAI_TA_BEARER;
		else if (!strcmp(as, "header"))
			auth = FYAI_TA_HEADER;
		else {
			*why = "profile validation changed";
			rc = -EINVAL;
			goto err_out;
		}
		spec = (struct fyai_transport_profile_spec) {
			.name = name, .url = url, .tag = tag,
			.model = model, .auth = auth, .header = header,
			.credential = cred,
			.plain_http = fy_get(item, "plain_http", (_Bool)false),
		};
		rc = fyai_transport_grant_add_spec(out, &spec);
		if (rc) {
			*why = rc == -EEXIST ? "a profile name is repeated" :
			       rc == -ENOMEM ? "out of memory" :
			       "a profile is not valid";
			rc = rc == -EEXIST ? -EINVAL : rc;
			goto err_out;
		}
		hdrs = fy_get(item, "headers", fy_invalid);
		if (fy_is_mapping(hdrs)) {
			fy_foreach_key_value(key, val, hdrs) {
				k = fy_castp(&key, "");
				v = fy_castp(&val, "");
				rc = fyai_transport_grant_add_header(out, name, k, v);
				if (rc) {
					*why = "a fixed header is not valid";
					rc = rc == -ENOMEM ? rc : -EINVAL;
					goto err_out;
				}
			}
		}
	}
	return 0;
err_out:
	fyai_transport_grant_clear(out);
	return rc;
}

fy_generic fyai_ctl_profiles_encode(struct fy_generic_builder *gb,
				    const struct fyai_transport_grant *grant)
{
	fy_generic seq = fy_seq_empty;
	fy_generic m, hm, name;
	const struct fyai_transport_profile *p;
	const char *line, *colon;
	size_t i, h;

	for (i = 0; i < grant->count; i++) {
		p = &grant->profiles[i];
		hm = fy_null;
		if (p->nheaders) {
			hm = fy_mapping(gb);
			for (h = 0; h < p->nheaders; h++) {
				line = p->headers[h];
				colon = strchr(line, ':');
				if (!colon)
					continue;
				name = fy_value(gb, fy_sprintfa("%.*s",
							  (int)(colon - line), line));
				colon++;
				while (*colon == ' ')
					colon++;
				hm = fy_assoc(gb, hm, name, fy_value(gb, colon));
			}
		}
		m = fy_null_filtered_mapping(gb, "name", p->name, "url", p->url,
			"auth", p->auth == FYAI_TA_NONE ? "none" :
				p->auth == FYAI_TA_BEARER ? "bearer" : "header",
			"plain_http", p->plain_http ? fy_value(gb, true) : fy_null,
			"tag", p->tag ? fy_value(gb, p->tag) : fy_null,
			"model", p->model ? fy_value(gb, p->model) : fy_null,
			"header", p->header ? fy_value(gb, p->header) : fy_null,
			"credential", p->credential ? fy_value(gb, p->credential) : fy_null,
			"headers", hm);
		seq = fy_append(gb, seq, m);
	}
	return seq;
}

/* Grants and namespaces. */

int fyai_ctl_grant_parse(struct fy_generic_builder *gb, fy_generic grant,
			 struct fyai_transport_allow *out, size_t max,
			 size_t *count)
{
	fy_generic item;
	size_t n = 0;
	bool bad;

	*count = 0;
	if (!fy_is_sequence(grant))
		return -EINVAL;
	fy_foreach(item, grant) {
		bad = false;

		if (n >= max || !fy_is_mapping(item))
			return -EINVAL;
		out[n].profile = opt_string(gb, item, "profile", &bad);
		out[n].model = opt_string(gb, item, "model", &bad);
		if (bad || !out[n].profile)
			return -EINVAL;
		n++;
	}
	*count = n;
	return 0;
}

fy_generic fyai_ctl_grant_encode(struct fy_generic_builder *gb,
				 const struct fyai_transport_allow *allow,
				 size_t count)
{
	fy_generic seq = fy_seq_empty;
	fy_generic m;
	size_t i;

	for (i = 0; i < count; i++) {
		m = fy_null_filtered_mapping(gb, "profile", allow[i].profile,
			"model", allow[i].model ? fy_value(gb, allow[i].model) : fy_null);
		seq = fy_append(gb, seq, m);
	}
	return seq;
}

int fyai_ctl_ns_parse(fy_generic ns, struct fyai_transport_ns_req *out)
{
	static const char *const names[FYAI_NS_COUNT] = {
		[FYAI_NS_NET] = "net", [FYAI_NS_MNT] = "mnt",
		[FYAI_NS_PID] = "pid",
	};
	fy_generic key, val;
	const char *k;
	int i;

	memset(out, 0, sizeof(*out));
	if (fy_is_invalid(ns) || fy_is_null(ns))
		return 0;
	if (!fy_is_mapping(ns))
		return -EINVAL;
	fy_foreach_key_value(key, val, ns) {
		k = fy_castp(&key, "");

		for (i = 0; i < FYAI_NS_COUNT; i++)
			if (!strcmp(k, names[i]))
				break;
		/* An inode does not fit an int; test the type, not the C range. */
		if (i == FYAI_NS_COUNT || !fy_is_int(val) ||
		    fy_number(val, -1LL) <= 0)
			return -EINVAL;
		out->mask |= 1u << i;
		out->ino[i] = fy_number(val, 0LL);
	}
	return 0;
}
