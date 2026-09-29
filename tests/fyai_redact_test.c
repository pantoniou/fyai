/*
 * fyai_redact_test.c - tests for credential white-out
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis@konsulko.com>
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai_redact.h"
#include "fyai_test.h"

#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(redact, builtin_headers, redact_builtin_headers)
FYAI_TEST_ENTRY(redact, custom_header, redact_custom_header)
FYAI_TEST_ENTRY(redact, bearer_anywhere, redact_bearer_anywhere)
FYAI_TEST_ENTRY(redact, secret_anywhere, redact_secret_anywhere)
FYAI_TEST_ENTRY(redact, keeps_length_and_other_text, redact_keeps_length_and_other_text)
FYAI_TEST_ENTRY(redact, secret_limits, redact_secret_limits)

#define SECRET "sk-live-0123456789abcdef"

/* Redact a copy of @in and return it; the caller frees. */
static char *run(const struct fyai_redactor *r, const char *in)
{
	size_t n = strlen(in);
	char *copy = malloc(n + 1);

	FYAI_TCHECK(copy);
	memcpy(copy, in, n + 1);
	fyai_redact(r, copy, n);
	FYAI_TCHECK(strlen(copy) == n);
	return copy;
}

int redact_builtin_headers(void)
{
	static const char *const names[] = {
		"Authorization", "authorization", "AUTHORIZATION",
		"Proxy-Authorization", "X-Api-Key", "api-key", "X-Goog-Api-Key",
		"Cookie", "Set-Cookie",
	};
	unsigned int i;
	char in[256], *out;

	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		snprintf(in, sizeof(in), "POST / HTTP/1.1\r\n%s: %s\r\nHost: x\r\n\r\n",
			 names[i], SECRET);
		out = run(NULL, in);
		FYAI_TCHECK(!strstr(out, SECRET));
		FYAI_TCHECK(strstr(out, FYAI_REDACTED));
		/* The name, the other headers, and the line ends stay. */
		FYAI_TCHECK(strstr(out, "POST / HTTP/1.1\r\n"));
		FYAI_TCHECK(strstr(out, "Host: x\r\n\r\n"));
		FYAI_TCHECK(!strncasecmp(strstr(out, "\r\n") + 2, names[i],
					 strlen(names[i])));
		free(out);
	}
	return 0;
}

int redact_custom_header(void)
{
	struct fyai_redactor r;
	char *out;

	fyai_redactor_init(&r);
	out = run(&r, "X-Custom-Key: abcdef123456\r\nX-Other: keep\r\n");
	FYAI_TCHECK(strstr(out, "abcdef123456"));	/* not known yet */
	free(out);

	FYAI_TCHECK(!fyai_redactor_add_header(&r, "X-Custom-Key"));
	FYAI_TCHECK(fyai_redactor_add_header(&r, "bad:name") == -EINVAL);
	out = run(&r, "X-Custom-Key: abcdef123456\r\nX-Other: keep\r\n");
	FYAI_TCHECK(!strstr(out, "abcdef123456"));
	FYAI_TCHECK(strstr(out, "X-Other: keep"));
	free(out);
	fyai_redactor_clear(&r);
	return 0;
}

int redact_bearer_anywhere(void)
{
	char *out = run(NULL, "{\"error\":\"bad Bearer abc.DEF-123_x/y+z= given\"}");

	FYAI_TCHECK(!strstr(out, "abc.DEF"));
	FYAI_TCHECK(strstr(out, "bad Bearer "));
	FYAI_TCHECK(strstr(out, " given\"}"));
	free(out);

	/* A word that only ends in "bearer" is not a scheme. */
	out = run(NULL, "pallbearer stuff");
	FYAI_TCHECK(!strcmp(out, "pallbearer stuff"));
	free(out);
	return 0;
}

int redact_secret_anywhere(void)
{
	struct fyai_redactor r;
	char *out;

	fyai_redactor_init(&r);
	FYAI_TCHECK(!fyai_redactor_add_secret(&r, SECRET));
	FYAI_TCHECK(!fyai_redactor_add_secret(&r, SECRET));	/* kept once */
	FYAI_TCHECK(r.nsecrets == 1);

	/* A provider can echo the key in a body, twice, and in an error. */
	out = run(&r, "{\"echo\":\"" SECRET "\",\"again\":\"" SECRET "\",\"n\":1}");
	FYAI_TCHECK(!strstr(out, SECRET));
	FYAI_TCHECK(strstr(out, "\"n\":1}"));
	free(out);

	out = run(&r, "key " SECRET SECRET " end");
	FYAI_TCHECK(!strstr(out, "sk-live"));
	FYAI_TCHECK(strstr(out, " end"));
	free(out);
	fyai_redactor_clear(&r);
	return 0;
}

int redact_keeps_length_and_other_text(void)
{
	const char *plain = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nhello\r\n";
	char *out = run(NULL, plain);

	FYAI_TCHECK(!strcmp(out, plain));
	free(out);

	out = fyai_redact_dup(NULL, "Authorization: Bearer " SECRET "\r\n");
	FYAI_TCHECK(out && !strstr(out, SECRET));
	free(out);
	FYAI_TCHECK(!fyai_redact_dup(NULL, NULL));

	/* An empty header value, and a header at the end with no line end. */
	out = run(NULL, "Authorization:\r\nX-Api-Key: " SECRET);
	FYAI_TCHECK(!strstr(out, SECRET));
	free(out);
	return 0;
}

int redact_secret_limits(void)
{
	struct fyai_redactor r;
	char *out;

	fyai_redactor_init(&r);
	FYAI_TCHECK(fyai_redactor_add_secret(&r, NULL) == -EINVAL);
	FYAI_TCHECK(fyai_redactor_add_secret(&r, "abc") == -EINVAL);
	FYAI_TCHECK(!fyai_redactor_add_secret(&r, "abcd"));

	/* A short value still leaves no trace of the secret. */
	out = run(&r, "value abcd here");
	FYAI_TCHECK(!strstr(out, "abcd"));
	free(out);
	fyai_redactor_clear(&r);
	FYAI_TCHECK(!r.nsecrets && !r.secrets);
	return 0;
}
