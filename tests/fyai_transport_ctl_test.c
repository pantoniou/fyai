/*
 * fyai_transport_ctl_test.c - tests for the transport control protocol
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "fyai_test.h"
#include "fyai_transport_ctl.h"
#include "utils.h"

#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(transport_ctl, profiles_roundtrip, transport_ctl_profiles_roundtrip)
FYAI_TEST_ENTRY(transport_ctl, profiles_rejected, transport_ctl_profiles_rejected)
FYAI_TEST_ENTRY(transport_ctl, grant_and_ns, transport_ctl_grant_and_ns)
FYAI_TEST_ENTRY(transport_ctl, passes_descriptor, transport_ctl_passes_descriptor)
FYAI_TEST_ENTRY(transport_ctl, refuses_bad_datagrams, transport_ctl_refuses_bad_datagrams)

static struct fy_generic_builder *new_gb(void)
{
	struct fy_generic_builder_cfg cfg = { .flags = FYGBCF_SCOPE_LEADER };
	struct fy_generic_builder *gb = fy_generic_builder_create(&cfg);

	FYAI_TCHECK(gb);
	return gb;
}

/* What the supervisor encodes is what the transport builds. */
int transport_ctl_profiles_roundtrip(void)
{
	struct fyai_transport_grant in = { 0 }, out = { 0 };
	struct fyai_transport_profile_spec lan = {
		.name = "local", .url = "http://ollama.lan:11434/v1/chat",
		.auth = FYAI_TA_NONE, .plain_http = true, .tag = "chat",
	};
	struct fy_generic_builder *gb = new_gb();
	const struct fyai_transport_profile *p;
	const char *why = "";

	FYAI_TCHECK(!fyai_transport_grant_add(&in, "main", "https://a.example/v1",
					      "responses", "m1", FYAI_TA_BEARER,
					      NULL, "env:KEY"));
	FYAI_TCHECK(!fyai_transport_grant_add(&in, "anthropic", "https://b.example/v1",
					      NULL, NULL, FYAI_TA_HEADER,
					      "x-api-key", "secret:anth"));
	FYAI_TCHECK(!fyai_transport_grant_add_header(&in, "anthropic",
						     "anthropic-version", "2023-06-01"));
	FYAI_TCHECK(!fyai_transport_grant_add_spec(&in, &lan));

	FYAI_TCHECK(!fyai_ctl_profiles_parse(fyai_ctl_profiles_encode(gb, &in), &out, &why));
	FYAI_TCHECK(out.count == 3);
	p = fyai_transport_grant_find(&out, "main");
	FYAI_TCHECK(p && p->auth == FYAI_TA_BEARER && !strcmp(p->model, "m1"));
	FYAI_TCHECK(!strcmp(p->credential, "env:KEY") && !strcmp(p->tag, "responses"));
	p = fyai_transport_grant_find(&out, "anthropic");
	FYAI_TCHECK(p && p->auth == FYAI_TA_HEADER && !strcmp(p->header, "x-api-key"));
	FYAI_TCHECK(p->nheaders == 1 && !strcmp(p->headers[0], "anthropic-version: 2023-06-01"));
	p = fyai_transport_grant_find(&out, "local");
	FYAI_TCHECK(p && p->plain_http && p->auth == FYAI_TA_NONE);

	fyai_transport_grant_clear(&in);
	fyai_transport_grant_clear(&out);
	fy_generic_builder_destroy(gb);
	return 0;
}

static int parse_json(const char *json, const char **why)
{
	struct fyai_transport_grant g = { 0 };
	struct fy_generic_builder *gb = new_gb();
	int rc = fyai_ctl_profiles_parse(parse_json_string(gb, json), &g, why);

	FYAI_TCHECK(!g.count || !rc);
	fyai_transport_grant_clear(&g);
	fy_generic_builder_destroy(gb);
	return rc;
}

/* A bad profile refuses the whole set, and builds nothing. */
int transport_ctl_profiles_rejected(void)
{
	const char *why = "";

	FYAI_TCHECK(!parse_json("[]", &why));
	FYAI_TCHECK(parse_json("{}", &why) == -EINVAL);
	FYAI_TCHECK(parse_json("[1]", &why) == -EINVAL);
	FYAI_TCHECK(parse_json("[{\"name\":\"a\"}]", &why) == -EINVAL);
	FYAI_TCHECK(parse_json("[{\"name\":\"a\",\"url\":\"https://x.example/\",\"auth\":\"magic\"}]", &why) == -EINVAL);
	FYAI_TCHECK(parse_json("[{\"name\":\"a\",\"url\":5,\"auth\":\"none\"}]", &why) == -EINVAL);
	FYAI_TCHECK(parse_json("[{\"name\":\"a\",\"url\":\"http://evil.example/\",\"auth\":\"none\"}]", &why) == -EINVAL);
	FYAI_TCHECK(parse_json("[{\"name\":\"a\",\"url\":\"https://x.example/\",\"auth\":\"bearer\"}]", &why) == -EINVAL);

	/* The second profile is bad: nothing of the first stays. */
	FYAI_TCHECK(parse_json("[{\"name\":\"a\",\"url\":\"https://x.example/\",\"auth\":\"none\"},"
			       "{\"name\":\"a\",\"url\":\"https://y.example/\",\"auth\":\"none\"}]", &why) == -EINVAL);
	FYAI_TCHECK(strstr(why, "repeated"));

	/* Fixed headers must be valid, strings, and in a mapping. */
	FYAI_TCHECK(parse_json("[{\"name\":\"a\",\"url\":\"https://x.example/\",\"auth\":\"none\",\"headers\":{\"Host\":\"x\"}}]", &why) == -EINVAL);
	FYAI_TCHECK(parse_json("[{\"name\":\"a\",\"url\":\"https://x.example/\",\"auth\":\"none\",\"headers\":{\"X-A\":1}}]", &why) == -EINVAL);
	FYAI_TCHECK(parse_json("[{\"name\":\"a\",\"url\":\"https://x.example/\",\"auth\":\"none\",\"headers\":[1]}]", &why) == -EINVAL);
	FYAI_TCHECK(!parse_json("[{\"name\":\"a\",\"url\":\"https://x.example/\",\"auth\":\"none\",\"headers\":{\"X-A\":\"1\"}}]", &why));
	return 0;
}

int transport_ctl_grant_and_ns(void)
{
	struct fyai_transport_allow allow[4], in[] = { { "main", NULL }, { "side", "m1" } };
	struct fyai_transport_ns_req ns;
	struct fy_generic_builder *gb = new_gb();
	size_t n;

	FYAI_TCHECK(!fyai_ctl_grant_parse(gb, fyai_ctl_grant_encode(gb, in, 2), allow, 4, &n));
	FYAI_TCHECK(n == 2 && !strcmp(allow[0].profile, "main") && !allow[0].model);
	FYAI_TCHECK(!strcmp(allow[1].profile, "side") && !strcmp(allow[1].model, "m1"));

	/* An empty grant is valid and reaches nothing. */
	FYAI_TCHECK(!fyai_ctl_grant_parse(gb, parse_json_string(gb, "[]"), allow, 4, &n) && !n);
	/* Too many entries, and entries that are not mappings with a name. */
	FYAI_TCHECK(fyai_ctl_grant_parse(gb, fyai_ctl_grant_encode(gb, in, 2), allow, 1, &n) == -EINVAL);
	FYAI_TCHECK(fyai_ctl_grant_parse(gb, parse_json_string(gb, "[\"main\"]"), allow, 4, &n) == -EINVAL);
	FYAI_TCHECK(fyai_ctl_grant_parse(gb, parse_json_string(gb, "[{\"model\":\"m\"}]"), allow, 4, &n) == -EINVAL);
	FYAI_TCHECK(fyai_ctl_grant_parse(gb, parse_json_string(gb, "{}"), allow, 4, &n) == -EINVAL);

	FYAI_TCHECK(!fyai_ctl_ns_parse(parse_json_string(gb, "{\"net\":4026531840,\"mnt\":7}"), &ns));
	FYAI_TCHECK(ns.mask == ((1u << FYAI_NS_NET) | (1u << FYAI_NS_MNT)));
	FYAI_TCHECK(ns.ino[FYAI_NS_NET] == 4026531840ull && ns.ino[FYAI_NS_MNT] == 7);
	FYAI_TCHECK(!fyai_ctl_ns_parse(fy_invalid, &ns) && !ns.mask);
	FYAI_TCHECK(fyai_ctl_ns_parse(parse_json_string(gb, "{\"user\":1}"), &ns) == -EINVAL);
	FYAI_TCHECK(fyai_ctl_ns_parse(parse_json_string(gb, "{\"net\":0}"), &ns) == -EINVAL);
	FYAI_TCHECK(fyai_ctl_ns_parse(parse_json_string(gb, "{\"net\":\"x\"}"), &ns) == -EINVAL);
	FYAI_TCHECK(fyai_ctl_ns_parse(parse_json_string(gb, "[1]"), &ns) == -EINVAL);
	fy_generic_builder_destroy(gb);
	return 0;
}

/* A datagram carries one descriptor, and it works on the other side. */
int transport_ctl_passes_descriptor(void)
{
	struct fy_generic_builder *gb = new_gb(), *gb2 = new_gb();
	int sv[2], pipefd[2], fd = -1;
	fy_generic m;
	char c = 0;

	FYAI_TCHECK(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv));
	FYAI_TCHECK(!pipe(pipefd));
	FYAI_TCHECK(!fyai_ctl_send(sv[0], fy_mapping(gb, "op", "admit", "seq", 5LL), pipefd[1], 0));
	FYAI_TCHECK(!fyai_ctl_recv(sv[1], gb2, &m, &fd));
	FYAI_TCHECK(fd >= 0 && fd != pipefd[1]);
	FYAI_TCHECK(fcntl(fd, F_GETFD) & FD_CLOEXEC);
	FYAI_TCHECK(fy_get(m, "seq", 0LL) == 5);
	FYAI_TCHECK(write(fd, "x", 1) == 1 && read(pipefd[0], &c, 1) == 1 && c == 'x');
	close(fd);

	/* Nothing queued; then a message without a descriptor. */
	FYAI_TCHECK(fyai_ctl_recv(sv[1], gb2, &m, &fd) == -EAGAIN);
	FYAI_TCHECK(!fyai_ctl_send(sv[0], fy_mapping(gb, "op", "log", "on", true), -1, 0));
	fd = -1;
	FYAI_TCHECK(!fyai_ctl_recv(sv[1], gb2, &m, &fd) && fd == -1);

	/* A descriptor that the caller did not ask for is closed, not leaked. */
	FYAI_TCHECK(!fyai_ctl_send(sv[0], fy_mapping(gb, "op", "log", "on", true), pipefd[1], 0));
	FYAI_TCHECK(!fyai_ctl_recv(sv[1], gb2, &m, NULL));

	/* The end of the channel is reported. */
	close(sv[0]);
	FYAI_TCHECK(fyai_ctl_recv(sv[1], gb2, &m, &fd) == -ECONNRESET);
	close(sv[1]);
	close(pipefd[0]);
	close(pipefd[1]);
	fy_generic_builder_destroy(gb);
	fy_generic_builder_destroy(gb2);
	return 0;
}

int transport_ctl_refuses_bad_datagrams(void)
{
	static const char *const bad[] = {
		"not json", "[1,2]", "{}", "{\"op\":5}", "{\"op\":\"\"}", "\"op\"",
	};
	struct fy_generic_builder *gb = new_gb();
	static char big[FYAI_CTL_MAX + 100];
	int sv[2], fd = -1;
	fy_generic m;
	size_t i;

	FYAI_TCHECK(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv));
	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		FYAI_TCHECK(send(sv[0], bad[i], strlen(bad[i]), 0) > 0);
		FYAI_TCHECK(fyai_ctl_recv(sv[1], gb, &m, &fd) == -EPROTO);
	}

	/* A datagram that does not fit the limit is refused whole. */
	memset(big, 'a', sizeof(big) - 1);
	FYAI_TCHECK(fyai_ctl_send(sv[0], fy_mapping(gb, "op", "x", "pad", big), -1, 0) == -EMSGSIZE);
	close(sv[0]);
	close(sv[1]);
	fy_generic_builder_destroy(gb);
	return 0;
}
