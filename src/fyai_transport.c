/*
 * fyai_transport.c - credential transport: wire frames and agent admission
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * This file reports no diagnostics. It returns a verdict and the caller, which
 * knows the execution and the branch, reports it. It never formats data that
 * an agent sent.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/syscall.h>
#endif

#include <libfyaml/libfyaml-endian.h>

#include "fyai_transport.h"
#include "fyai_transport_sock.h"

struct fyai_transport_exec {
	struct fyai_transport_exec *next;
	uint64_t id;
	uint64_t parent_id;
	pid_t pid;
	uid_t uid;
	int pidfd;
	int channel;
	struct fyai_transport_ns_req ns;
	struct {
		char *profile;
		char *model;
	} *allow;
	size_t nallow;

	/* Reassembly of a fragmented message. */
	uint8_t *asm_buf;
	size_t asm_len;
	size_t asm_cap;
	bool asm_active;
	uint16_t asm_kind;
	uint64_t asm_request;
	uint64_t asm_next_seq;
};

struct fyai_transport_registry {
	enum fyai_transport_level level;
	char *cgroup_root;			/* level A only */
	const char *failed;			/* last failed check, static */
	struct fyai_transport_exec *execs;
};

static void put_le16(uint8_t *p, uint16_t v)
{
	v = htole16(v);
	memcpy(p, &v, sizeof(v));
}

static void put_le32(uint8_t *p, uint32_t v)
{
	v = htole32(v);
	memcpy(p, &v, sizeof(v));
}

static void put_le64(uint8_t *p, uint64_t v)
{
	v = htole64(v);
	memcpy(p, &v, sizeof(v));
}

static uint16_t get_le16(const uint8_t *p)
{
	uint16_t v;

	memcpy(&v, p, sizeof(v));
	return le16toh(v);
}

static uint32_t get_le32(const uint8_t *p)
{
	uint32_t v;

	memcpy(&v, p, sizeof(v));
	return le32toh(v);
}

static uint64_t get_le64(const uint8_t *p)
{
	uint64_t v;

	memcpy(&v, p, sizeof(v));
	return le64toh(v);
}

/*
 * Header layout, little endian:
 *   0 magic  4 version(2) 6 kind(2)  8 exec_id  16 request_id  24 seq
 *  32 len    36 flags
 */
void fyai_transport_hdr_encode(uint8_t *out, const struct fyai_transport_hdr *hdr)
{
	put_le32(out, FYAI_TRANSPORT_MAGIC);
	put_le16(out + 4, FYAI_TRANSPORT_VERSION);
	put_le16(out + 6, hdr->kind);
	put_le64(out + 8, hdr->exec_id);
	put_le64(out + 16, hdr->request_id);
	put_le64(out + 24, hdr->seq);
	put_le32(out + 32, hdr->len);
	put_le32(out + 36, hdr->flags);
}

int fyai_transport_hdr_decode(const uint8_t *in, size_t size,
			      struct fyai_transport_hdr *hdr)
{
	if (size < FYAI_TRANSPORT_HDR_SIZE)
		return -EPROTO;
	if (get_le32(in) != FYAI_TRANSPORT_MAGIC ||
	    get_le16(in + 4) != FYAI_TRANSPORT_VERSION)
		return -EPROTO;

	memset(hdr, 0, sizeof(*hdr));
	hdr->kind = get_le16(in + 6);
	hdr->exec_id = get_le64(in + 8);
	hdr->request_id = get_le64(in + 16);
	hdr->seq = get_le64(in + 24);
	hdr->len = get_le32(in + 32);
	hdr->flags = get_le32(in + 36);

	if (hdr->kind == 0 || hdr->kind >= FYAI_TK_COUNT)
		return -EPROTO;
	if (hdr->flags & ~FYAI_TF_KNOWN)
		return -EPROTO;
	if (hdr->len > FYAI_TRANSPORT_MAX_PAYLOAD)
		return -EPROTO;
	return 0;
}

const char *fyai_transport_kind_name(unsigned int kind)
{
	static const char *const names[FYAI_TK_COUNT] = {
		[FYAI_TK_REQUEST] = "request",
		[FYAI_TK_CANCEL] = "cancel",
		[FYAI_TK_CREDIT] = "credit",
		[FYAI_TK_RESP_START] = "response-start",
		[FYAI_TK_RESP_BODY] = "response-body",
		[FYAI_TK_RESP_END] = "response-end",
		[FYAI_TK_RESP_ERROR] = "response-error",
	};

	if (kind >= FYAI_TK_COUNT || !names[kind])
		return "unknown";
	return names[kind];
}

const char *fyai_transport_verdict_str(enum fyai_transport_verdict v)
{
	switch (v) {
	case FYAI_TV_OK:
		return "accepted";
	case FYAI_TV_MORE:
		return "fragment stored";
	case FYAI_TV_CLOSED:
		return "channel closed";
	case FYAI_TV_AGAIN:
		return "nothing to read";
	case FYAI_TV_UNKNOWN_CHANNEL:
		return "channel has no registered execution";
	case FYAI_TV_TRUNCATED:
		return "message or control data truncated";
	case FYAI_TV_NO_CREDENTIALS:
		return "message carries no sender credentials";
	case FYAI_TV_WRONG_SENDER:
		return "sender is not the registered process";
	case FYAI_TV_DEAD:
		return "registered process has exited";
	case FYAI_TV_CONTAINMENT:
		return "sender is outside its containment";
	case FYAI_TV_BAD_FRAME:
		return "malformed frame";
	case FYAI_TV_WRONG_EXEC:
		return "frame names another execution";
	case FYAI_TV_BAD_KIND:
		return "frame kind is not allowed from an agent";
	case FYAI_TV_TOO_LARGE:
		return "message exceeds the size bound";
	case FYAI_TV_NOSYS:
		return "platform cannot verify the sender";
	case FYAI_TV_ERROR:
		return "system error";
	}
	return "unknown verdict";
}

/* Levels. */

static const char *const level_names[] = {
	[FYAI_TL_NONE] = "none",
	[FYAI_TL_AUTO] = "auto",
	[FYAI_TL_A] = "level-a",
	[FYAI_TL_B] = "level-b",
};

const char *fyai_transport_level_name(enum fyai_transport_level level)
{
	if ((unsigned int)level >= sizeof(level_names) / sizeof(level_names[0]))
		return "unknown";
	return level_names[level];
}

int fyai_transport_level_parse(const char *name, enum fyai_transport_level *level)
{
	unsigned int i;

	if (!name)
		return -EINVAL;
	for (i = 0; i < sizeof(level_names) / sizeof(level_names[0]); i++) {
		if (!strcmp(name, level_names[i])) {
			*level = i;
			return 0;
		}
	}
	return -EINVAL;
}

/* Grants. */

static bool url_chars_ok(const char *s)
{
	unsigned char c;

	for (; *s; s++) {
		c = *s;

		if (c <= ' ' || c == 0x7f || c == '#' || c == '\\')
			return false;
	}
	return true;
}

/* Split "scheme://authority/rest". Return the authority length or 0. */
static size_t url_authority(const char *url, const char *scheme,
			    const char **authp)
{
	size_t n = strlen(scheme);
	const char *a;
	size_t len;

	if (strncmp(url, scheme, n))
		return 0;
	a = url + n;
	len = strcspn(a, "/?");
	if (!len || memchr(a, '@', len))
		return 0;
	*authp = a;
	return len;
}

static bool authority_is_loopback(const char *a, size_t len)
{
	size_t hlen;
	const char *e;

	if (len && a[0] == '[') {
		e = memchr(a, ']', len);

		if (!e)
			return false;
		hlen = e - a + 1;
		if (hlen != sizeof("[::1]") - 1 || memcmp(a, "[::1]", hlen))
			return false;
		return hlen == len || a[hlen] == ':';
	}
	hlen = 0;
	while (hlen < len && a[hlen] != ':')
		hlen++;
	return (hlen == 9 && !strncmp(a, "localhost", 9)) ||
	       (hlen == 9 && !strncmp(a, "127.0.0.1", 9));
}

/*
 * Check the endpoint. Plain http needs a loopback host, whose credential does
 * not leave the host, or the plain_http flag from trusted configuration.
 */
static bool url_allowed(const char *url, bool plain_http)
{
	const char *a;
	size_t len;

	if (!url_chars_ok(url))
		return false;
	if (url_authority(url, "https://", &a))
		return !plain_http;
	len = url_authority(url, "http://", &a);
	return len && (plain_http || authority_is_loopback(a, len));
}

static bool name_ok(const char *s)
{
	size_t n = s ? strlen(s) : 0;

	if (!n || n > 64)
		return false;
	for (; *s; s++)
		if (!isalnum((unsigned char)*s) && !strchr("._:-", *s))
			return false;
	return true;
}

/* An HTTP field name, minus the fields that frame the request. */
static bool header_ok(const char *s)
{
	static const char *const reserved[] = {
		"host", "content-length", "transfer-encoding", "connection",
		"upgrade", "te", "trailer", "proxy-authorization",
	};
	size_t i;

	if (!s || !*s)
		return false;
	for (i = 0; s[i]; i++)
		if (!isalnum((unsigned char)s[i]) && !strchr("!#$%&'*+-.^_`|~", s[i]))
			return false;
	for (i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++)
		if (!strcasecmp(s, reserved[i]))
			return false;
	return true;
}

static char *dup_opt(const char *s)
{
	return s ? strdup(s) : NULL;
}

static void profile_clear(struct fyai_transport_profile *pr)
{
	size_t i;

	for (i = 0; i < pr->nheaders; i++)
		free(pr->headers[i]);
	free(pr->headers);
	free(pr->name);
	free(pr->url);
	free(pr->tag);
	free(pr->model);
	free(pr->header);
	free(pr->credential);
	memset(pr, 0, sizeof(*pr));
}

int fyai_transport_grant_add_spec(struct fyai_transport_grant *grant,
				  const struct fyai_transport_profile_spec *spec)
{
	const char *name = spec->name, *url = spec->url, *tag = spec->tag;
	const char *model = spec->model, *header = spec->header;
	const char *credential = spec->credential;
	enum fyai_transport_auth auth = spec->auth;
	struct fyai_transport_profile pr = { .auth = auth }, *np;
	size_t i;

	if (!name_ok(name) || !url || !url_allowed(url, spec->plain_http))
		return -EINVAL;
	pr.plain_http = spec->plain_http;
	if (auth == FYAI_TA_NONE) {
		if (header || credential)
			return -EINVAL;
	} else {
		if (!credential || !*credential)
			return -EINVAL;
		if (auth == FYAI_TA_HEADER ? !header_ok(header) : header != NULL)
			return -EINVAL;
	}
	if (auth != FYAI_TA_NONE && auth != FYAI_TA_BEARER &&
	    auth != FYAI_TA_HEADER)
		return -EINVAL;
	for (i = 0; i < grant->count; i++)
		if (!strcmp(grant->profiles[i].name, name))
			return -EEXIST;

	pr.name = strdup(name);
	pr.url = strdup(url);
	pr.tag = dup_opt(tag);
	pr.model = dup_opt(model);
	pr.header = dup_opt(header);
	pr.credential = dup_opt(credential);
	np = realloc(grant->profiles, (grant->count + 1) * sizeof(*np));
	if (!pr.name || !pr.url || (tag && !pr.tag) || (model && !pr.model) ||
	    (header && !pr.header) || (credential && !pr.credential) || !np) {
		profile_clear(&pr);
		if (np)
			grant->profiles = np;
		return -ENOMEM;
	}
	grant->profiles = np;
	grant->profiles[grant->count++] = pr;
	return 0;
}

int fyai_transport_grant_add_header(struct fyai_transport_grant *grant,
				    const char *name, const char *field,
				    const char *value)
{
	struct fyai_transport_profile *pr;
	char **nh, *line;
	const char *p;
	size_t i;

	pr = (struct fyai_transport_profile *)fyai_transport_grant_find(grant, name);
	if (!pr)
		return -ENOENT;
	if (!header_ok(field) || !value || strlen(value) > 1024 ||
	    !strcasecmp(field, "authorization") ||
	    (pr->header && !strcasecmp(field, pr->header)))
		return -EINVAL;
	for (p = value; *p; p++)
		if ((unsigned char)*p < ' ' && *p != '\t')
			return -EINVAL;
	for (i = 0; i < pr->nheaders; i++)
		if (!strncasecmp(pr->headers[i], field, strlen(field)) &&
		    pr->headers[i][strlen(field)] == ':')
			return -EINVAL;

	if (asprintf(&line, "%s: %s", field, value) < 0)
		return -ENOMEM;
	nh = realloc(pr->headers, (pr->nheaders + 1) * sizeof(*nh));
	if (!nh) {
		free(line);
		return -ENOMEM;
	}
	pr->headers = nh;
	pr->headers[pr->nheaders++] = line;
	return 0;
}

int fyai_transport_grant_add(struct fyai_transport_grant *grant,
			     const char *name, const char *url,
			     const char *tag, const char *model,
			     enum fyai_transport_auth auth, const char *header,
			     const char *credential)
{
	struct fyai_transport_profile_spec spec = {
		.name = name, .url = url, .tag = tag, .model = model,
		.auth = auth, .header = header, .credential = credential,
	};

	return fyai_transport_grant_add_spec(grant, &spec);
}

const struct fyai_transport_profile *
fyai_transport_grant_find(const struct fyai_transport_grant *grant,
			  const char *name)
{
	size_t i;

	if (!name)
		return NULL;
	for (i = 0; i < grant->count; i++)
		if (!strcmp(grant->profiles[i].name, name))
			return &grant->profiles[i];
	return NULL;
}

void fyai_transport_grant_clear(struct fyai_transport_grant *grant)
{
	size_t i;

	for (i = 0; i < grant->count; i++)
		profile_clear(&grant->profiles[i]);
	free(grant->profiles);
	memset(grant, 0, sizeof(*grant));
}

/* Platform primitives. Without them protected mode cannot start. */

#ifdef __linux__

static int sender_pidfd_open(pid_t pid)
{
	int fd = syscall(SYS_pidfd_open, pid, 0);

	return fd < 0 ? -errno : fd;
}

/* Return 0 when the process is alive, -ESRCH when it has exited. */
static int sender_pidfd_alive(int pidfd)
{
	int rc = syscall(SYS_pidfd_send_signal, pidfd, 0, NULL, 0);

	return rc < 0 ? -errno : 0;
}

/* Return 1 if @pid is in the subtree @root, 0 if not, or a negative errno. */
static int sender_in_cgroup(const char *root, pid_t pid)
{
	char path[64], line[4096];
	size_t rlen = strlen(root);
	FILE *fp;
	int found = 0;
	size_t len;
	const char *p;

	snprintf(path, sizeof(path), "/proc/%d/cgroup", (int)pid);
	fp = fopen(path, "re");
	if (!fp)
		return -errno;

	while (fgets(line, sizeof(line), fp)) {
		len = strlen(line);

		if (len && line[len - 1] == '\n')
			line[--len] = '\0';
		else
			continue;	/* an over-long line is not a match */
		if (strncmp(line, "0::", 3))
			continue;
		p = line + 3;
		found = !strncmp(p, root, rlen) &&
			(p[rlen] == '\0' || p[rlen] == '/');
		break;
	}
	fclose(fp);
	return found;
}

static const char *const ns_dirs[FYAI_NS_COUNT] = {
	[FYAI_NS_NET] = "net",
	[FYAI_NS_MNT] = "mnt",
	[FYAI_NS_PID] = "pid",
};

/* Store the inode of a namespace of @pid, or of this process for pid 0. */
static int sender_ns_ino(pid_t pid, enum fyai_transport_ns ns, uint64_t *ino)
{
	char path[64];
	struct stat st;

	if (pid)
		snprintf(path, sizeof(path), "/proc/%d/ns/%s", (int)pid,
			 ns_dirs[ns]);
	else
		snprintf(path, sizeof(path), "/proc/self/ns/%s", ns_dirs[ns]);
	if (stat(path, &st) < 0)
		return -errno;
	*ino = st.st_ino;
	return 0;
}

int fyai_transport_channel_prepare(int channel)
{
	int on = 1;

	if (setsockopt(channel, SOL_SOCKET, SO_PASSCRED, &on, sizeof(on)) < 0)
		return -errno;
	return 0;
}

#define HAVE_SENDER_CREDS 1

#else

static int sender_pidfd_open(pid_t pid)
{
	(void)pid;
	return -ENOSYS;
}

static int sender_in_cgroup(const char *root, pid_t pid)
{
	(void)root;
	(void)pid;
	return -ENOSYS;
}

static int sender_ns_ino(pid_t pid, enum fyai_transport_ns ns, uint64_t *ino)
{
	(void)pid;
	(void)ns;
	(void)ino;
	return -ENOSYS;
}

int fyai_transport_channel_prepare(int channel)
{
	(void)channel;
	return -ENOSYS;
}

#endif

/* Registry. */

struct fyai_transport_registry *
fyai_transport_registry_create(enum fyai_transport_level level,
			       const char *cgroup_root)
{
	struct fyai_transport_registry *reg;
	size_t len;

	if (level == FYAI_TL_A) {
		/* The root of the hierarchy would admit every process. */
		if (!cgroup_root || cgroup_root[0] != '/' ||
		    !strcmp(cgroup_root, "/"))
			return NULL;
		len = strlen(cgroup_root);
		if (cgroup_root[len - 1] == '/')
			return NULL;
	} else if (level != FYAI_TL_B || cgroup_root) {
		return NULL;
	}

	reg = calloc(1, sizeof(*reg));
	if (!reg)
		return NULL;
	reg->level = level;
	if (cgroup_root) {
		reg->cgroup_root = strdup(cgroup_root);
		if (!reg->cgroup_root) {
			free(reg);
			return NULL;
		}
	}
	return reg;
}

enum fyai_transport_level
fyai_transport_registry_level(const struct fyai_transport_registry *reg)
{
	return reg->level;
}

const char *
fyai_transport_registry_failed(const struct fyai_transport_registry *reg)
{
	return reg->failed;
}

/*
 * Check the containment of @e. Return 1 when it holds, 0 when the sender is
 * outside it, or a negative errno. Set reg->failed on 0.
 */
static int contained(struct fyai_transport_registry *reg,
		     const struct fyai_transport_exec *e)
{
	static const char *const ns_names[FYAI_NS_COUNT] = {
		[FYAI_NS_NET] = "ns-net",
		[FYAI_NS_MNT] = "ns-mnt",
		[FYAI_NS_PID] = "ns-pid",
	};
	int k, rc;
	uint64_t ino;

	reg->failed = NULL;
	if (reg->level == FYAI_TL_A) {
		rc = sender_in_cgroup(reg->cgroup_root, e->pid);
		if (rc <= 0) {
			if (!rc)
				reg->failed = "cgroup";
			return rc;
		}
	}
	for (k = 0; k < FYAI_NS_COUNT; k++) {
		if (!(e->ns.mask & (1u << k)))
			continue;
		rc = sender_ns_ino(e->pid, k, &ino);
		if (rc)
			return rc;
		if (ino != e->ns.ino[k]) {
			reg->failed = ns_names[k];
			return 0;
		}
	}
	return 1;
}

static void allow_free(struct fyai_transport_exec *e)
{
	size_t i;

	for (i = 0; i < e->nallow; i++) {
		free(e->allow[i].profile);
		free(e->allow[i].model);
	}
	free(e->allow);
	e->allow = NULL;
	e->nallow = 0;
}

/* Copy @allow into @e, which must have no grant. Return 0 or a negative errno. */
static int allow_copy(struct fyai_transport_exec *e,
		      const struct fyai_transport_allow *allow, size_t n)
{
	size_t i, j;

	for (i = 0; i < n; i++) {
		if (!name_ok(allow[i].profile) ||
		    (allow[i].model && !*allow[i].model))
			return -EINVAL;
		for (j = 0; j < i; j++)
			if (!strcmp(allow[i].profile, allow[j].profile))
				return -EINVAL;
	}
	if (!n)
		return 0;
	e->allow = calloc(n, sizeof(*e->allow));
	if (!e->allow)
		return -ENOMEM;
	for (i = 0; i < n; i++) {
		e->allow[i].profile = strdup(allow[i].profile);
		e->allow[i].model = dup_opt(allow[i].model);
		e->nallow = i + 1;
		if (!e->allow[i].profile ||
		    (allow[i].model && !e->allow[i].model)) {
			allow_free(e);
			return -ENOMEM;
		}
	}
	return 0;
}

bool fyai_transport_exec_allows(const struct fyai_transport_exec *exec,
				const char *profile, const char **model)
{
	size_t i;

	*model = NULL;
	if (!profile)
		return false;
	for (i = 0; i < exec->nallow; i++) {
		if (!strcmp(exec->allow[i].profile, profile)) {
			*model = exec->allow[i].model;
			return true;
		}
	}
	return false;
}

int fyai_transport_set_grant(struct fyai_transport_registry *reg, uint64_t id,
			     const struct fyai_transport_allow *allow,
			     size_t nallow)
{
	struct fyai_transport_exec *e = fyai_transport_find(reg, id);
	struct fyai_transport_exec tmp = { 0 };
	int rc;

	if (!e)
		return -ENOENT;
	rc = allow_copy(&tmp, allow, nallow);
	if (rc)
		return rc;
	allow_free(e);
	e->allow = tmp.allow;
	e->nallow = tmp.nallow;
	return 0;
}

static void exec_free(struct fyai_transport_exec *e)
{
	allow_free(e);
	if (e->pidfd >= 0)
		close(e->pidfd);
	if (e->channel >= 0)
		close(e->channel);
	free(e->asm_buf);
	free(e);
}

void fyai_transport_registry_destroy(struct fyai_transport_registry *reg)
{
	struct fyai_transport_exec *e;

	if (!reg)
		return;
	while ((e = reg->execs)) {
		reg->execs = e->next;
		exec_free(e);
	}
	free(reg->cgroup_root);
	free(reg);
}

struct fyai_transport_exec *
fyai_transport_find(struct fyai_transport_registry *reg, uint64_t id)
{
	struct fyai_transport_exec *e;

	for (e = reg->execs; e; e = e->next)
		if (e->id == id)
			return e;
	return NULL;
}

struct fyai_transport_exec *
fyai_transport_find_channel(struct fyai_transport_registry *reg, int channel)
{
	struct fyai_transport_exec *e;

	for (e = reg->execs; e; e = e->next)
		if (e->channel == channel)
			return e;
	return NULL;
}

uint64_t fyai_transport_exec_id(const struct fyai_transport_exec *exec)
{
	return exec->id;
}

int fyai_transport_exec_channel(const struct fyai_transport_exec *exec)
{
	return exec->channel;
}

int fyai_transport_register(struct fyai_transport_registry *reg, uint64_t id,
			    uint64_t parent_id, pid_t pid, uid_t uid,
			    int channel, const struct fyai_transport_allow *allow,
			    size_t nallow, const struct fyai_transport_ns_req *ns)
{
	struct fyai_transport_exec *e, *o;
	int k, rc;
	uint64_t own;

	if (id == 0 || pid <= 0 || channel < 0)
		return -EINVAL;
	if (ns && (ns->mask >> FYAI_NS_COUNT))
		return -EINVAL;
	for (o = reg->execs; o; o = o->next)
		if (o->id == id || o->pid == pid || o->channel == channel)
			return -EEXIST;

	e = calloc(1, sizeof(*e));
	if (!e)
		return -ENOMEM;
	e->id = id;
	e->parent_id = parent_id;
	e->pid = pid;
	e->uid = uid;
	e->channel = channel;
	e->pidfd = -1;
	if (ns)
		e->ns = *ns;
	rc = allow_copy(e, allow, nallow);
	if (rc)
		goto err;

	/*
	 * The supervisor has not reaped the child, so its PID cannot be reused
	 * before the pidfd exists. The pidfd then pins the identity.
	 */
	e->pidfd = sender_pidfd_open(pid);
	if (e->pidfd < 0) {
		rc = e->pidfd;
		goto err;
	}

	/* A namespace the sender shares with the transport isolates nothing. */
	for (k = 0; k < FYAI_NS_COUNT; k++) {
		if (!(e->ns.mask & (1u << k)))
			continue;
		rc = sender_ns_ino(0, k, &own);
		if (rc)
			goto err;
		if (own == e->ns.ino[k]) {
			reg->failed = k == FYAI_NS_NET ? "ns-net" :
				      k == FYAI_NS_MNT ? "ns-mnt" : "ns-pid";
			rc = -EPERM;
			goto err;
		}
	}

	rc = contained(reg, e);
	if (rc <= 0) {
		if (!rc)
			rc = -EPERM;
		goto err;
	}

	rc = fyai_transport_channel_prepare(channel);
	if (rc)
		goto err;

	e->channel = channel;
	e->next = reg->execs;
	reg->execs = e;
	return 0;
err:
	/* The caller keeps the channel on failure. */
	e->channel = -1;
	exec_free(e);
	return rc;
}

int fyai_transport_retire(struct fyai_transport_registry *reg, uint64_t id)
{
	struct fyai_transport_exec **pp, *e;

	for (pp = &reg->execs; (e = *pp); pp = &e->next) {
		if (e->id != id)
			continue;
		*pp = e->next;
		exec_free(e);
		return 0;
	}
	return -ENOENT;
}

/* Receive. */

#ifdef HAVE_SENDER_CREDS

static void asm_reset(struct fyai_transport_exec *e)
{
	e->asm_len = 0;
	e->asm_active = false;
}

static int asm_append(struct fyai_transport_exec *e, const uint8_t *data,
		      size_t len)
{
	size_t need = e->asm_len + len;
	size_t cap;
	uint8_t *nb;

	if (need > FYAI_TRANSPORT_MAX_MESSAGE)
		return -EFBIG;
	if (need > e->asm_cap) {
		cap = e->asm_cap ? e->asm_cap : 4096;

		while (cap < need)
			cap *= 2;
		nb = realloc(e->asm_buf, cap);
		if (!nb)
			return -ENOMEM;
		e->asm_buf = nb;
		e->asm_cap = cap;
	}
	if (len)
		memcpy(e->asm_buf + e->asm_len, data, len);
	e->asm_len = need;
	return 0;
}

/* Descriptors an agent passed are never used. Close them. */
static void drop_rights(struct msghdr *mh)
{
	struct cmsghdr *c;
	size_t n, i;
	int *fds;

	for (c = CMSG_FIRSTHDR(mh); c; c = CMSG_NXTHDR(mh, c)) {
		if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS)
			continue;
		n = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
		fds = (int *)CMSG_DATA(c);
		for (i = 0; i < n; i++)
			close(fds[i]);
	}
}

static const struct ucred *find_creds(struct msghdr *mh)
{
	struct cmsghdr *c;

	for (c = CMSG_FIRSTHDR(mh); c; c = CMSG_NXTHDR(mh, c))
		if (c->cmsg_level == SOL_SOCKET &&
		    c->cmsg_type == SCM_CREDENTIALS &&
		    c->cmsg_len == CMSG_LEN(sizeof(struct ucred)))
			return (const struct ucred *)CMSG_DATA(c);
	return NULL;
}

enum fyai_transport_verdict
fyai_transport_recv(struct fyai_transport_registry *reg, int channel,
		    struct fyai_transport_msg *msg)
{
	struct fyai_transport_exec *e;
	struct fyai_transport_hdr hdr;
	uint8_t buf[FYAI_TRANSPORT_MAX_FRAME + 1];
	union {
		struct cmsghdr align;
		uint8_t raw[CMSG_SPACE(sizeof(struct ucred)) +
			    CMSG_SPACE(16 * sizeof(int))];
	} ctl;
	struct iovec iov = { .iov_base = buf, .iov_len = sizeof(buf) };
	struct msghdr mh = {
		.msg_iov = &iov, .msg_iovlen = 1,
		.msg_control = ctl.raw, .msg_controllen = sizeof(ctl.raw),
	};
	const struct ucred *cred;
	ssize_t n;
	int rc;
	enum fyai_transport_verdict verdict;

	memset(msg, 0, sizeof(*msg));
	e = fyai_transport_find_channel(reg, channel);
	if (!e)
		return FYAI_TV_UNKNOWN_CHANNEL;

	/* The previous message is consumed once the caller receives again. */
	if (!e->asm_active)
		asm_reset(e);

	n = recvmsg(channel, &mh, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
	if (n < 0)
		return (errno == EAGAIN || errno == EWOULDBLOCK) ?
			FYAI_TV_AGAIN : FYAI_TV_ERROR;
	drop_rights(&mh);
	if (n == 0)
		return FYAI_TV_CLOSED;
	if (mh.msg_flags & (MSG_TRUNC | MSG_CTRUNC) ||
	    (size_t)n > FYAI_TRANSPORT_MAX_FRAME) {
		verdict = FYAI_TV_TRUNCATED;
		goto err_out;
	}

	cred = find_creds(&mh);
	if (!cred)
		return FYAI_TV_NO_CREDENTIALS;
	if (cred->pid != e->pid || cred->uid != e->uid)
		return FYAI_TV_WRONG_SENDER;

	rc = sender_pidfd_alive(e->pidfd);
	if (rc)
		return rc == -ESRCH ? FYAI_TV_DEAD : FYAI_TV_ERROR;
	rc = contained(reg, e);
	if (rc < 0)
		return rc == -ENOENT ? FYAI_TV_DEAD : FYAI_TV_ERROR;
	if (!rc)
		return FYAI_TV_CONTAINMENT;

	/* From here the sender is the registered process. */
	if (fyai_transport_hdr_decode(buf, n, &hdr) ||
	    hdr.len != (size_t)n - FYAI_TRANSPORT_HDR_SIZE) {
		verdict = FYAI_TV_BAD_FRAME;
		goto err_out;
	}
	if (hdr.exec_id != e->id) {
		verdict = FYAI_TV_WRONG_EXEC;
		goto err_out;
	}
	if (hdr.kind != FYAI_TK_REQUEST && hdr.kind != FYAI_TK_CANCEL &&
	    hdr.kind != FYAI_TK_CREDIT) {
		verdict = FYAI_TV_BAD_KIND;
		goto err_out;
	}
	if ((hdr.flags & FYAI_TF_MORE) && hdr.kind != FYAI_TK_REQUEST) {
		verdict = FYAI_TV_BAD_FRAME;
		goto err_out;
	}

	if (e->asm_active) {
		if (hdr.kind != e->asm_kind ||
		    hdr.request_id != e->asm_request ||
		    hdr.seq != e->asm_next_seq) {
			verdict = FYAI_TV_BAD_FRAME;
			goto err_out;
		}
	} else if (hdr.seq != 0) {
		verdict = FYAI_TV_BAD_FRAME;
		goto err_out;
	}

	rc = asm_append(e, buf + FYAI_TRANSPORT_HDR_SIZE, hdr.len);
	if (rc) {
		verdict = rc == -EFBIG ? FYAI_TV_TOO_LARGE : FYAI_TV_ERROR;
		goto err_out;
	}

	if (hdr.flags & FYAI_TF_MORE) {
		e->asm_active = true;
		e->asm_kind = hdr.kind;
		e->asm_request = hdr.request_id;
		e->asm_next_seq = hdr.seq + 1;
		return FYAI_TV_MORE;
	}

	e->asm_active = false;
	msg->exec = e;
	msg->hdr = hdr;
	msg->hdr.len = e->asm_len;
	msg->hdr.flags = 0;
	msg->payload = e->asm_buf;
	msg->len = e->asm_len;
	return FYAI_TV_OK;

err_out:
	asm_reset(e);
	return verdict;
}

#else

enum fyai_transport_verdict
fyai_transport_recv(struct fyai_transport_registry *reg, int channel,
		    struct fyai_transport_msg *msg)
{
	(void)reg;
	(void)channel;
	memset(msg, 0, sizeof(*msg));
	return FYAI_TV_NOSYS;
}

#endif

/* Send. */

int fyai_transport_socketpair(int sv[2])
{
#ifdef SOCK_CLOEXEC
	return socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv);
#else
	int i;

	if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv))
		return -1;
	for (i = 0; i < 2; i++)
		(void)fcntl(sv[i], F_SETFD, FD_CLOEXEC);
	return 0;
#endif
}

int fyai_transport_send_frame(int channel, const struct fyai_transport_hdr *hdr,
			      const void *payload)
{
	uint8_t buf[FYAI_TRANSPORT_MAX_FRAME];
	ssize_t n;

	if (hdr->len > FYAI_TRANSPORT_MAX_PAYLOAD)
		return -EMSGSIZE;
	fyai_transport_hdr_encode(buf, hdr);
	if (hdr->len)
		memcpy(buf + FYAI_TRANSPORT_HDR_SIZE, payload, hdr->len);

	do {
		n = send(channel, buf, FYAI_TRANSPORT_HDR_SIZE + hdr->len,
			 MSG_NOSIGNAL);
	} while (n < 0 && errno == EINTR);
	return n < 0 ? -errno : 0;
}

int fyai_transport_send_message(int channel, uint16_t kind, uint64_t exec_id,
				uint64_t request_id, const void *payload,
				size_t len)
{
	const uint8_t *p = payload;
	uint64_t seq = 0;
	struct fyai_transport_hdr hdr;
	size_t chunk;
	int rc;

	do {
		hdr = (struct fyai_transport_hdr) {
			.kind = kind, .exec_id = exec_id,
			.request_id = request_id, .seq = seq++,
		};
		chunk = len > FYAI_TRANSPORT_MAX_PAYLOAD ?
			FYAI_TRANSPORT_MAX_PAYLOAD : len;

		hdr.len = chunk;
		if (chunk < len)
			hdr.flags = FYAI_TF_MORE;
		rc = fyai_transport_send_frame(channel, &hdr, p);
		if (rc)
			return rc;
		p += chunk;
		len -= chunk;
	} while (len);
	return 0;
}
