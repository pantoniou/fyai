/*
 * fyai_transport_test.c - tests for transport frames and agent admission
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fyai_transport.h"
#include "fyai_test.h"

#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(transport, frame_roundtrip, transport_frame_roundtrip)
FYAI_TEST_ENTRY(transport, frame_rejects_malformed, transport_frame_rejects_malformed)
FYAI_TEST_ENTRY(transport, grant_endpoints, transport_grant_endpoints)
FYAI_TEST_ENTRY(transport, grant_rejects_profiles, transport_grant_rejects_profiles)
FYAI_TEST_ENTRY(transport, registry_rejects_root, transport_registry_rejects_root)
FYAI_TEST_ENTRY(transport, accepts_registered_sender, transport_accepts_registered_sender)
FYAI_TEST_ENTRY(transport, reassembles_fragments, transport_reassembles_fragments)
FYAI_TEST_ENTRY(transport, rejects_other_sender, transport_rejects_other_sender)
FYAI_TEST_ENTRY(transport, rejects_dead_sender, transport_rejects_dead_sender)
FYAI_TEST_ENTRY(transport, rejects_bad_frames, transport_rejects_bad_frames)
FYAI_TEST_ENTRY(transport, rejects_outside_cgroup, transport_rejects_outside_cgroup)
FYAI_TEST_ENTRY(transport, grant_plain_http, transport_grant_plain_http)
FYAI_TEST_ENTRY(transport, grant_names, transport_grant_names)
FYAI_TEST_ENTRY(transport, level_names, transport_level_names)
FYAI_TEST_ENTRY(transport, level_b_needs_no_cgroup, transport_level_b_needs_no_cgroup)
FYAI_TEST_ENTRY(transport, rejects_shared_namespace, transport_rejects_shared_namespace)
FYAI_TEST_ENTRY(transport, accepts_isolated_namespace, transport_accepts_isolated_namespace)

/* Read the cgroup v2 path of this process; return false without cgroup v2. */
static bool own_cgroup(char *out, size_t size)
{
	char line[4096];
	FILE *fp = fopen("/proc/self/cgroup", "re");
	bool ok = false;
	size_t n;

	if (!fp)
		return false;
	while (fgets(line, sizeof(line), fp)) {
		n = strlen(line);

		if (n && line[n - 1] == '\n')
			line[n - 1] = '\0';
		if (!strncmp(line, "0::/", 4) && strlen(line + 3) < size &&
		    strcmp(line + 3, "/")) {
			strcpy(out, line + 3);
			ok = true;
			break;
		}
	}
	fclose(fp);
	return ok;
}

struct fixture {
	struct fyai_transport_registry *reg;
	int agent;		/* agent endpoint */
	int chan;		/* transport endpoint, owned by the registry */
};

static int fixture_open(struct fixture *fx, pid_t pid, uint64_t id)
{
	char cg[4096];
	int sv[2], rc;

	if (!own_cgroup(cg, sizeof(cg)))
		return 1;	/* no cgroup v2: skip */
	rc = socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv);
	FYAI_TCHECK(!rc);
	fx->agent = sv[0];
	fx->chan = sv[1];
	fx->reg = fyai_transport_registry_create(FYAI_TL_A, cg);
	FYAI_TCHECK(fx->reg);
	rc = fyai_transport_register(fx->reg, id, 0, pid, getuid(), fx->chan,
				     NULL, 0, NULL);
	FYAI_TCHECK(!rc);
	return 0;
}

static void fixture_close(struct fixture *fx)
{
	fyai_transport_registry_destroy(fx->reg);
	close(fx->agent);
}

static int send_req(int fd, uint64_t id, uint16_t kind, const char *body)
{
	return fyai_transport_send_message(fd, kind, id, 1, body, strlen(body));
}

int transport_frame_roundtrip(void)
{
	struct fyai_transport_hdr in = {
		.kind = FYAI_TK_REQUEST, .flags = FYAI_TF_MORE,
		.exec_id = 0x1122334455667788ull, .request_id = 9,
		.seq = 3, .len = 100,
	}, out;
	uint8_t buf[FYAI_TRANSPORT_HDR_SIZE];
	int rc;

	fyai_transport_hdr_encode(buf, &in);
	rc = fyai_transport_hdr_decode(buf, sizeof(buf), &out);
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(out.kind == in.kind && out.flags == in.flags);
	FYAI_TCHECK(out.exec_id == in.exec_id && out.request_id == 9);
	FYAI_TCHECK(out.seq == 3 && out.len == 100);
	return 0;
}

int transport_frame_rejects_malformed(void)
{
	struct fyai_transport_hdr h = { .kind = FYAI_TK_REQUEST }, out;
	uint8_t buf[FYAI_TRANSPORT_HDR_SIZE];

	fyai_transport_hdr_encode(buf, &h);
	FYAI_TCHECK(fyai_transport_hdr_decode(buf, sizeof(buf) - 1, &out));

	buf[0] ^= 1;			/* magic */
	FYAI_TCHECK(fyai_transport_hdr_decode(buf, sizeof(buf), &out));
	buf[0] ^= 1;

	h.kind = 0;
	fyai_transport_hdr_encode(buf, &h);
	FYAI_TCHECK(fyai_transport_hdr_decode(buf, sizeof(buf), &out));

	h.kind = FYAI_TK_COUNT;
	fyai_transport_hdr_encode(buf, &h);
	FYAI_TCHECK(fyai_transport_hdr_decode(buf, sizeof(buf), &out));

	h.kind = FYAI_TK_REQUEST;
	h.flags = 0x80;
	fyai_transport_hdr_encode(buf, &h);
	FYAI_TCHECK(fyai_transport_hdr_decode(buf, sizeof(buf), &out));

	h.flags = 0;
	h.len = FYAI_TRANSPORT_MAX_PAYLOAD + 1;
	fyai_transport_hdr_encode(buf, &h);
	FYAI_TCHECK(fyai_transport_hdr_decode(buf, sizeof(buf), &out));
	return 0;
}

int transport_grant_endpoints(void)
{
	struct fyai_transport_grant g = { 0 };
	const struct fyai_transport_profile *pr;

	FYAI_TCHECK(!fyai_transport_grant_add(&g, "main", "https://a.example/v1/x",
					      "chat", "m", FYAI_TA_BEARER, NULL, "env:K"));
	pr = fyai_transport_grant_find(&g, "main");
	FYAI_TCHECK(pr && !strcmp(pr->url, "https://a.example/v1/x"));
	FYAI_TCHECK(!strcmp(pr->credential, "env:K") && !strcmp(pr->tag, "chat"));
	FYAI_TCHECK(!fyai_transport_grant_find(&g, "other"));
	FYAI_TCHECK(!fyai_transport_grant_find(&g, NULL));

	/* Names are unique. */
	FYAI_TCHECK(fyai_transport_grant_add(&g, "main", "https://b.example/",
					     NULL, NULL, FYAI_TA_BEARER, NULL, "env:K") == -EEXIST);
	FYAI_TCHECK(!fyai_transport_grant_add(&g, "side", "https://b.example/",
					      NULL, NULL, FYAI_TA_HEADER, "x-api-key", "env:K"));
	FYAI_TCHECK(g.count == 2);
	fyai_transport_grant_clear(&g);
	FYAI_TCHECK(!g.count && !g.profiles);
	return 0;
}

#define BAD(url, auth, hdr, cred) \
	FYAI_TCHECK(fyai_transport_grant_add(&g, "p", url, NULL, NULL, auth, hdr, cred) == -EINVAL)
#define GOOD(url, auth, hdr, cred) \
	do { \
		FYAI_TCHECK(!fyai_transport_grant_add(&g, "p", url, NULL, NULL, auth, hdr, cred)); \
		fyai_transport_grant_clear(&g); \
	} while (0)

int transport_grant_rejects_profiles(void)
{
	struct fyai_transport_grant g = { 0 };

	/* An authenticated endpoint is https only, with no userinfo. */
	BAD("http://a.example/", FYAI_TA_BEARER, NULL, "c");
	BAD("https://u:p@a.example/", FYAI_TA_BEARER, NULL, "c");
	BAD("https://", FYAI_TA_BEARER, NULL, "c");
	BAD("https://a.example/ x", FYAI_TA_BEARER, NULL, "c");

	/* A credential source is required, and forbidden without authentication. */
	BAD("https://a.example/", FYAI_TA_BEARER, NULL, NULL);
	BAD("https://a.example/", FYAI_TA_BEARER, NULL, "");
	BAD("https://a.example/", FYAI_TA_NONE, NULL, "c");

	/* Plain http is allowed only on loopback, with or without a credential. */
	GOOD("http://127.0.0.1:1/", FYAI_TA_BEARER, NULL, "c");
	GOOD("http://localhost:9/", FYAI_TA_HEADER, "X-Api-Key", "c");
	BAD("http://10.0.0.1/", FYAI_TA_BEARER, NULL, "c");
	GOOD("http://127.0.0.1:8080/v1", FYAI_TA_NONE, NULL, NULL);
	GOOD("http://localhost/v1", FYAI_TA_NONE, NULL, NULL);
	GOOD("http://[::1]:1/v1", FYAI_TA_NONE, NULL, NULL);
	GOOD("https://a.example/v1", FYAI_TA_NONE, NULL, NULL);
	BAD("http://evil.example/", FYAI_TA_NONE, NULL, NULL);
	BAD("http://127.0.0.1.evil.example/", FYAI_TA_NONE, NULL, NULL);

	/* A header name cannot frame the request or hide another header. */
	GOOD("https://a.example/", FYAI_TA_HEADER, "X-Api-Key", "c");
	BAD("https://a.example/", FYAI_TA_HEADER, NULL, "c");
	BAD("https://a.example/", FYAI_TA_HEADER, "Host", "c");
	BAD("https://a.example/", FYAI_TA_HEADER, "Content-Length", "c");
	BAD("https://a.example/", FYAI_TA_HEADER, "X: y\r\nZ", "c");
	BAD("https://a.example/", FYAI_TA_BEARER, "X-Api-Key", "c");

	FYAI_TCHECK(fyai_transport_grant_add(&g, "bad name", "https://a.example/",
					     NULL, NULL, FYAI_TA_NONE, NULL, NULL) == -EINVAL);
	FYAI_TCHECK(fyai_transport_grant_add(&g, "", "https://a.example/",
					     NULL, NULL, FYAI_TA_NONE, NULL, NULL) == -EINVAL);
	FYAI_TCHECK(!g.count);
	return 0;
}

int transport_registry_rejects_root(void)
{
	FYAI_TCHECK(!fyai_transport_registry_create(FYAI_TL_A, "/"));
	FYAI_TCHECK(!fyai_transport_registry_create(FYAI_TL_A, "relative"));
	FYAI_TCHECK(!fyai_transport_registry_create(FYAI_TL_A, "/a/"));
	FYAI_TCHECK(!fyai_transport_registry_create(FYAI_TL_A, NULL));
	/* Level B has no cgroup; NONE and AUTO name no registry. */
	FYAI_TCHECK(!fyai_transport_registry_create(FYAI_TL_B, "/a"));
	FYAI_TCHECK(!fyai_transport_registry_create(FYAI_TL_NONE, NULL));
	FYAI_TCHECK(!fyai_transport_registry_create(FYAI_TL_AUTO, NULL));
	return 0;
}

int transport_accepts_registered_sender(void)
{
	struct fixture fx;
	struct fyai_transport_msg msg;

	if (fixture_open(&fx, getpid(), 7))
		return 0;
	FYAI_TCHECK(fyai_transport_recv(fx.reg, fx.chan, &msg) == FYAI_TV_AGAIN);
	FYAI_TCHECK(!send_req(fx.agent, 7, FYAI_TK_REQUEST, "hello"));
	FYAI_TCHECK(fyai_transport_recv(fx.reg, fx.chan, &msg) == FYAI_TV_OK);
	FYAI_TCHECK(msg.len == 5 && !memcmp(msg.payload, "hello", 5));
	FYAI_TCHECK(fyai_transport_exec_id(msg.exec) == 7);
	FYAI_TCHECK(msg.hdr.kind == FYAI_TK_REQUEST && msg.hdr.request_id == 1);
	fixture_close(&fx);
	return 0;
}

int transport_reassembles_fragments(void)
{
	static char big[3 * FYAI_TRANSPORT_MAX_PAYLOAD + 123];
	struct fixture fx;
	struct fyai_transport_msg msg;
	struct fyai_transport_hdr h;

	if (fixture_open(&fx, getpid(), 7))
		return 0;
	memset(big, 'x', sizeof(big));
	big[sizeof(big) - 1] = 'y';
	FYAI_TCHECK(!fyai_transport_send_message(fx.agent, FYAI_TK_REQUEST, 7, 4,
						 big, sizeof(big)));
	FYAI_TCHECK(fyai_transport_recv(fx.reg, fx.chan, &msg) == FYAI_TV_MORE);
	FYAI_TCHECK(fyai_transport_recv(fx.reg, fx.chan, &msg) == FYAI_TV_MORE);
	FYAI_TCHECK(fyai_transport_recv(fx.reg, fx.chan, &msg) == FYAI_TV_MORE);
	FYAI_TCHECK(fyai_transport_recv(fx.reg, fx.chan, &msg) == FYAI_TV_OK);
	FYAI_TCHECK(msg.len == sizeof(big) && !memcmp(msg.payload, big, sizeof(big)));

	/* A fragment out of order is a protocol error and resets the message. */
	h = (struct fyai_transport_hdr) {
		.kind = FYAI_TK_REQUEST, .exec_id = 7, .request_id = 5,
		.seq = 1, .len = 1,
	};
	FYAI_TCHECK(!fyai_transport_send_frame(fx.agent, &h, "z"));
	FYAI_TCHECK(fyai_transport_recv(fx.reg, fx.chan, &msg) == FYAI_TV_BAD_FRAME);
	fixture_close(&fx);
	return 0;
}

/* A process holding a copy of the descriptor is not the registered agent. */
int transport_rejects_other_sender(void)
{
	struct fixture fx;
	struct fyai_transport_msg msg;
	pid_t child;

	child = fork();
	FYAI_TCHECK(child >= 0);
	if (!child) {
		pause();
		_exit(0);
	}
	if (fixture_open(&fx, child, 7)) {
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		return 0;
	}
	FYAI_TCHECK(!send_req(fx.agent, 7, FYAI_TK_REQUEST, "x"));
	FYAI_TCHECK(fyai_transport_recv(fx.reg, fx.chan, &msg) == FYAI_TV_WRONG_SENDER);
	FYAI_TCHECK(!msg.exec);
	fixture_close(&fx);
	kill(child, SIGKILL);
	waitpid(child, NULL, 0);
	return 0;
}

/* The message stays queued after its sender exits; the pidfd shows it dead. */
int transport_rejects_dead_sender(void)
{
	struct fyai_transport_registry *reg;
	struct fyai_transport_msg msg;
	int sv[2], go[2], st, rc;
	char cg[4096], c;
	pid_t child;

	if (!own_cgroup(cg, sizeof(cg)))
		return 0;
	FYAI_TCHECK(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv));
	FYAI_TCHECK(!pipe(go));
	child = fork();
	FYAI_TCHECK(child >= 0);
	if (!child) {
		if (read(go[0], &c, 1) != 1)
			_exit(1);
		_exit(send_req(sv[0], 7, FYAI_TK_REQUEST, "x") ? 2 : 0);
	}
	reg = fyai_transport_registry_create(FYAI_TL_A, cg);
	FYAI_TCHECK(reg);
	rc = fyai_transport_register(reg, 7, 0, child, getuid(), sv[1], NULL, 0, NULL);
	FYAI_TCHECK(!rc);
	/* The child sends from the endpoint it inherited, then exits. */
	FYAI_TCHECK(write(go[1], "g", 1) == 1);
	rc = waitpid(child, &st, 0);
	FYAI_TCHECK(rc == child && WIFEXITED(st) && !WEXITSTATUS(st));
	FYAI_TCHECK(fyai_transport_recv(reg, sv[1], &msg) == FYAI_TV_DEAD);
	fyai_transport_registry_destroy(reg);
	close(sv[0]);
	close(go[0]);
	close(go[1]);
	return 0;
}

int transport_rejects_bad_frames(void)
{
	struct fixture fx;
	struct fyai_transport_msg msg;
	struct fyai_transport_hdr h = { .kind = FYAI_TK_RESP_BODY, .exec_id = 7 };
	uint8_t raw[FYAI_TRANSPORT_HDR_SIZE + 1];

	if (fixture_open(&fx, getpid(), 7))
		return 0;

	/* An agent cannot send a transport-to-agent kind. */
	FYAI_TCHECK(!fyai_transport_send_frame(fx.agent, &h, NULL));
	FYAI_TCHECK(fyai_transport_recv(fx.reg, fx.chan, &msg) == FYAI_TV_BAD_KIND);

	/* An agent cannot speak for another execution. */
	FYAI_TCHECK(!send_req(fx.agent, 8, FYAI_TK_REQUEST, "x"));
	FYAI_TCHECK(fyai_transport_recv(fx.reg, fx.chan, &msg) == FYAI_TV_WRONG_EXEC);

	/* The payload length must match the datagram. */
	h.kind = FYAI_TK_REQUEST;
	h.len = 5;
	fyai_transport_hdr_encode(raw, &h);
	FYAI_TCHECK(send(fx.agent, raw, sizeof(raw), 0) == sizeof(raw));
	FYAI_TCHECK(fyai_transport_recv(fx.reg, fx.chan, &msg) == FYAI_TV_BAD_FRAME);

	/* Garbage is not a frame. */
	FYAI_TCHECK(send(fx.agent, "junk", 4, 0) == 4);
	FYAI_TCHECK(fyai_transport_recv(fx.reg, fx.chan, &msg) == FYAI_TV_BAD_FRAME);

	/* A closed peer is reported, not read as data. */
	close(fx.agent);
	FYAI_TCHECK(fyai_transport_recv(fx.reg, fx.chan, &msg) == FYAI_TV_CLOSED);
	fyai_transport_registry_destroy(fx.reg);
	return 0;
}

int transport_rejects_outside_cgroup(void)
{
	struct fyai_transport_registry *reg;
	char cg[4096];
	int sv[2], rc;

	if (!own_cgroup(cg, sizeof(cg)))
		return 0;
	FYAI_TCHECK(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv));
	/* A sibling path with a shared prefix is not in the subtree. */
	strcat(cg, "-other");
	reg = fyai_transport_registry_create(FYAI_TL_A, cg);
	FYAI_TCHECK(reg);
	rc = fyai_transport_register(reg, 7, 0, getpid(), getuid(), sv[1], NULL, 0, NULL);
	FYAI_TCHECK(rc == -EPERM);
	FYAI_TCHECK(!strcmp(fyai_transport_registry_failed(reg), "cgroup"));
	fyai_transport_registry_destroy(reg);
	close(sv[0]);
	close(sv[1]);
	return 0;
}

int transport_level_names(void)
{
	enum fyai_transport_level l;

	FYAI_TCHECK(!fyai_transport_level_parse("none", &l) && l == FYAI_TL_NONE);
	FYAI_TCHECK(!fyai_transport_level_parse("auto", &l) && l == FYAI_TL_AUTO);
	FYAI_TCHECK(!fyai_transport_level_parse("level-a", &l) && l == FYAI_TL_A);
	FYAI_TCHECK(!fyai_transport_level_parse("level-b", &l) && l == FYAI_TL_B);
	FYAI_TCHECK(fyai_transport_level_parse("level-c", &l) == -EINVAL);
	FYAI_TCHECK(fyai_transport_level_parse("true", &l) == -EINVAL);
	FYAI_TCHECK(fyai_transport_level_parse(NULL, &l) == -EINVAL);
	FYAI_TCHECK(!strcmp(fyai_transport_level_name(FYAI_TL_A), "level-a"));
	return 0;
}

/* Level B admits by PID and pidfd alone, so a foreign subtree is not checked. */
int transport_level_b_needs_no_cgroup(void)
{
	struct fyai_transport_registry *reg = fyai_transport_registry_create(FYAI_TL_B, NULL);
	struct fyai_transport_msg msg;
	int sv[2], rc;

	FYAI_TCHECK(reg);
	FYAI_TCHECK(fyai_transport_registry_level(reg) == FYAI_TL_B);
	FYAI_TCHECK(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv));
	rc = fyai_transport_register(reg, 7, 0, getpid(), getuid(), sv[1], NULL, 0, NULL);
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(!send_req(sv[0], 7, FYAI_TK_REQUEST, "x"));
	FYAI_TCHECK(fyai_transport_recv(reg, sv[1], &msg) == FYAI_TV_OK);
	fyai_transport_registry_destroy(reg);
	close(sv[0]);
	return 0;
}

static uint64_t ns_ino(pid_t pid, const char *kind)
{
	char path[64];
	struct stat st;

	snprintf(path, sizeof(path), "/proc/%d/ns/%s", (int)pid, kind);
	FYAI_TCHECK(!stat(path, &st));
	return st.st_ino;
}

/* A sender in the namespace of the transport is not isolated. */
int transport_rejects_shared_namespace(void)
{
	struct fyai_transport_registry *reg = fyai_transport_registry_create(FYAI_TL_B, NULL);
	struct fyai_transport_ns_req ns = { .mask = 1u << FYAI_NS_NET };
	int sv[2], rc;

	FYAI_TCHECK(reg);
	FYAI_TCHECK(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv));
	ns.ino[FYAI_NS_NET] = ns_ino(getpid(), "net");
	rc = fyai_transport_register(reg, 7, 0, getpid(), getuid(), sv[1], NULL, 0, &ns);
	FYAI_TCHECK(rc == -EPERM);
	FYAI_TCHECK(!strcmp(fyai_transport_registry_failed(reg), "ns-net"));

	/* A stated inode that the sender does not hold is refused too. */
	ns.ino[FYAI_NS_NET] ^= 1;
	rc = fyai_transport_register(reg, 7, 0, getpid(), getuid(), sv[1], NULL, 0, &ns);
	FYAI_TCHECK(rc == -EPERM);
	fyai_transport_registry_destroy(reg);
	close(sv[0]);
	close(sv[1]);
	return 0;
}

/* A sender in a network namespace of its own passes; skip where the host
 * forbids an unprivileged user namespace. */
int transport_accepts_isolated_namespace(void)
{
	struct fyai_transport_registry *reg = fyai_transport_registry_create(FYAI_TL_B, NULL);
	struct fyai_transport_ns_req ns = { .mask = 1u << FYAI_NS_NET };
	struct fyai_transport_msg msg;
	int sv[2], ready[2], go[2], rc, st;
	uint64_t ino;
	pid_t child;
	char c = 0;

	FYAI_TCHECK(reg);
	FYAI_TCHECK(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv));
	FYAI_TCHECK(!pipe(ready) && !pipe(go));
	child = fork();
	FYAI_TCHECK(child >= 0);
	if (!child) {
		c = unshare(CLONE_NEWUSER | CLONE_NEWNET) ? 'n' : 'y';
		if (write(ready[1], &c, 1) != 1 || c == 'n')
			_exit(0);
		if (read(go[0], &c, 1) != 1 ||
		    send_req(sv[0], 7, FYAI_TK_REQUEST, "x"))
			_exit(1);
		if (write(ready[1], "s", 1) != 1 || read(go[0], &c, 1) != 1)
			_exit(2);
		_exit(0);
	}
	FYAI_TCHECK(read(ready[0], &c, 1) == 1);
	if (c == 'n') {
		waitpid(child, NULL, 0);
		fyai_transport_registry_destroy(reg);
		return 0;
	}
	ino = ns_ino(child, "net");
	FYAI_TCHECK(ino != ns_ino(getpid(), "net"));
	ns.ino[FYAI_NS_NET] = ino;
	rc = fyai_transport_register(reg, 7, 0, child, getuid(), sv[1], NULL, 0, &ns);
	FYAI_TCHECK(!rc);
	FYAI_TCHECK(write(go[1], "g", 1) == 1);
	/* The child sends, then waits until the parent has read it. */
	FYAI_TCHECK(read(ready[0], &c, 1) == 1 && c == 's');
	FYAI_TCHECK(fyai_transport_recv(reg, sv[1], &msg) == FYAI_TV_OK);
	FYAI_TCHECK(msg.len == 1 && msg.payload[0] == 'x');
	FYAI_TCHECK(write(go[1], "d", 1) == 1);
	rc = waitpid(child, &st, 0);
	FYAI_TCHECK(rc == child && WIFEXITED(st) && !WEXITSTATUS(st));
	fyai_transport_registry_destroy(reg);
	close(sv[0]);
	return 0;
}

static void sock_pair(int sv[2])
{
	FYAI_TCHECK(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv));
}

/* A grant is a list of names with an optional narrower model. */
int transport_grant_names(void)
{
	struct fyai_transport_registry *reg = fyai_transport_registry_create(FYAI_TL_B, NULL);
	struct fyai_transport_allow two[] = { { "main", NULL }, { "side", "m1" } };
	struct fyai_transport_allow dup[] = { { "main", NULL }, { "main", "m1" } };
	struct fyai_transport_allow bad[] = { { "bad name", NULL } };
	struct fyai_transport_allow empty_model[] = { { "main", "" } };
	struct fyai_transport_allow one[] = { { "other", "m2" } };
	struct fyai_transport_exec *ex;
	const char *model;
	int sv[2], sv2[2];

	FYAI_TCHECK(reg);
	sock_pair(sv);
	FYAI_TCHECK(!fyai_transport_register(reg, 7, 0, getpid(), getuid(), sv[1], two, 2, NULL));
	ex = fyai_transport_find(reg, 7);
	FYAI_TCHECK(ex);
	FYAI_TCHECK(fyai_transport_exec_allows(ex, "main", &model) && !model);
	FYAI_TCHECK(fyai_transport_exec_allows(ex, "side", &model) && !strcmp(model, "m1"));
	FYAI_TCHECK(!fyai_transport_exec_allows(ex, "other", &model) && !model);
	FYAI_TCHECK(!fyai_transport_exec_allows(ex, NULL, &model));

	/* A replacement takes effect at once; a bad one leaves the grant. */
	FYAI_TCHECK(fyai_transport_set_grant(reg, 7, dup, 2) == -EINVAL);
	FYAI_TCHECK(fyai_transport_set_grant(reg, 7, bad, 1) == -EINVAL);
	FYAI_TCHECK(fyai_transport_set_grant(reg, 7, empty_model, 1) == -EINVAL);
	FYAI_TCHECK(fyai_transport_exec_allows(ex, "main", &model));
	FYAI_TCHECK(fyai_transport_set_grant(reg, 9, one, 1) == -ENOENT);
	FYAI_TCHECK(!fyai_transport_set_grant(reg, 7, one, 1));
	FYAI_TCHECK(!fyai_transport_exec_allows(ex, "main", &model));
	FYAI_TCHECK(fyai_transport_exec_allows(ex, "other", &model) && !strcmp(model, "m2"));

	/* An empty grant reaches nothing; it does not mean everything. */
	FYAI_TCHECK(!fyai_transport_set_grant(reg, 7, NULL, 0));
	FYAI_TCHECK(!fyai_transport_exec_allows(ex, "other", &model));

	/* A bad grant refuses the admission and leaves no execution. */
	sock_pair(sv2);
	FYAI_TCHECK(fyai_transport_register(reg, 8, 0, getppid(), getuid(), sv2[1], bad, 1, NULL) == -EINVAL);
	FYAI_TCHECK(!fyai_transport_find(reg, 8));
	fyai_transport_registry_destroy(reg);
	close(sv[0]);
	close(sv2[0]);
	close(sv2[1]);
	return 0;
}

/* A local model server on a network is reached over http; configuration says so. */
int transport_grant_plain_http(void)
{
	struct fyai_transport_grant g = { 0 };
	struct fyai_transport_profile_spec spec = {
		.name = "p", .url = "http://192.168.1.5:8080/v1",
		.auth = FYAI_TA_NONE,
	};
	const struct fyai_transport_profile *pr;

	/* Without the flag, only loopback may use http. */
	FYAI_TCHECK(fyai_transport_grant_add_spec(&g, &spec) == -EINVAL);
	spec.url = "http://ollama.lan:11434/v1";
	FYAI_TCHECK(fyai_transport_grant_add_spec(&g, &spec) == -EINVAL);
	spec.url = "http://127.0.0.1:1/";
	FYAI_TCHECK(!fyai_transport_grant_add_spec(&g, &spec));
	fyai_transport_grant_clear(&g);

	/* With the flag, any host, with or without a credential. */
	spec.plain_http = true;
	spec.url = "http://192.168.1.5:8080/v1";
	FYAI_TCHECK(!fyai_transport_grant_add_spec(&g, &spec));
	pr = fyai_transport_grant_find(&g, "p");
	FYAI_TCHECK(pr && pr->plain_http);
	fyai_transport_grant_clear(&g);

	spec.url = "http://ollama.lan:11434/v1";
	spec.auth = FYAI_TA_BEARER;
	spec.credential = "env:K";
	FYAI_TCHECK(!fyai_transport_grant_add_spec(&g, &spec));
	fyai_transport_grant_clear(&g);

	/* The flag is for http; on an https URL it is a mistake. */
	spec.url = "https://a.example/";
	FYAI_TCHECK(fyai_transport_grant_add_spec(&g, &spec) == -EINVAL);

	/* The flag does not relax the rest of the check. */
	spec.url = "http://u:p@ollama.lan/";
	FYAI_TCHECK(fyai_transport_grant_add_spec(&g, &spec) == -EINVAL);
	spec.url = "ftp://ollama.lan/";
	FYAI_TCHECK(fyai_transport_grant_add_spec(&g, &spec) == -EINVAL);
	spec.url = "http://ollama.lan/ x";
	FYAI_TCHECK(fyai_transport_grant_add_spec(&g, &spec) == -EINVAL);
	return 0;
}
