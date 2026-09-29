/*
 * fyai_redact.c - white out credentials in text that leaves the process
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include <ctype.h>
#include <stdbool.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "fyai_redact.h"
#include "fyai_secret.h"

static const char *const builtin_headers[] = {
	"authorization", "proxy-authorization", "x-api-key", "api-key",
	"x-goog-api-key", "cookie", "set-cookie", NULL
};

void fyai_redactor_init(struct fyai_redactor *r)
{
	memset(r, 0, sizeof(*r));
}

void fyai_redactor_clear(struct fyai_redactor *r)
{
	size_t i;

	for (i = 0; i < r->nsecrets; i++) {
		fyai_secret_clear(r->secrets[i], strlen(r->secrets[i]));
		free(r->secrets[i]);
	}
	for (i = 0; i < r->nheaders; i++)
		free(r->headers[i]);
	free(r->secrets);
	free(r->headers);
	memset(r, 0, sizeof(*r));
}

static int list_add(char ***list, size_t *n, const char *value)
{
	char **nl, *copy;
	size_t i;

	for (i = 0; i < *n; i++)
		if (!strcmp((*list)[i], value))
			return 0;
	copy = strdup(value);
	if (!copy)
		return -ENOMEM;
	nl = realloc(*list, (*n + 1) * sizeof(**list));
	if (!nl) {
		fyai_secret_clear(copy, strlen(copy));
		free(copy);
		return -ENOMEM;
	}
	*list = nl;
	nl[(*n)++] = copy;
	return 0;
}

int fyai_redactor_add_secret(struct fyai_redactor *r, const char *value)
{
	if (!value || strlen(value) < FYAI_REDACT_MIN_SECRET)
		return -EINVAL;
	return list_add(&r->secrets, &r->nsecrets, value);
}

int fyai_redactor_add_header(struct fyai_redactor *r, const char *name)
{
	if (!name || !*name || strchr(name, ':'))
		return -EINVAL;
	return list_add(&r->headers, &r->nheaders, name);
}

/* Replace @n bytes with the marker, or with '*' when it does not fit. */
static void white_out(char *p, size_t n)
{
	size_t m = sizeof(FYAI_REDACTED) - 1;

	if (n < m) {
		memset(p, '*', n);
		return;
	}
	memset(p, ' ', n);
	memcpy(p, FYAI_REDACTED, m);
}

static bool line_is_header(const char *line, size_t len, const char *name)
{
	size_t n = strlen(name);

	return len > n && !strncasecmp(line, name, n) && line[n] == ':';
}

/* Return the length of the header name at @line, or 0 if it is not one. */
static size_t credential_header(const struct fyai_redactor *r, const char *line,
				size_t len)
{
	const char *const *b;
	size_t i;

	for (b = builtin_headers; *b; b++)
		if (line_is_header(line, len, *b))
			return strlen(*b);
	if (r)
		for (i = 0; i < r->nheaders; i++)
			if (line_is_header(line, len, r->headers[i]))
				return strlen(r->headers[i]);
	return 0;
}

static void redact_header_lines(const struct fyai_redactor *r, char *buf,
				size_t len)
{
	char *p = buf, *end = buf + len;
	char *eol, *tail, *v, *e;
	size_t nlen;

	while (p < end) {
		eol = memchr(p, '\n', end - p);
		tail = eol ? eol : end;
		nlen = credential_header(r, p, tail - p);

		if (nlen) {
			v = p + nlen + 1;
			e = tail;

			while (v < e && (*v == ' ' || *v == '\t'))
				v++;
			/* Keep the line ending. */
			while (e > v && (e[-1] == '\r' || e[-1] == ' ' ||
					 e[-1] == '\t'))
				e--;
			if (e > v)
				white_out(v, e - v);
		}
		p = eol ? eol + 1 : end;
	}
}

static bool token_char(unsigned char c)
{
	return isalnum(c) || strchr("._~+/=-", c);
}

static void redact_bearer(char *buf, size_t len)
{
	size_t i = 0;
	size_t j, k;

	while (i + 7 < len) {
		if (strncasecmp(buf + i, "bearer ", 7) ||
		    (i && isalnum((unsigned char)buf[i - 1]))) {
			i++;
			continue;
		}
		j = i + 7;
		k = j;
		while (k < len && token_char(buf[k]))
			k++;
		if (k > j)
			white_out(buf + j, k - j);
		i = k > j ? k : i + 7;
	}
}

static void redact_secrets(const struct fyai_redactor *r, char *buf, size_t len)
{
	size_t i;
	size_t n;
	char *p, *end;

	for (i = 0; i < r->nsecrets; i++) {
		n = strlen(r->secrets[i]);
		p = buf;
		end = buf + len;

		while (p < end && (p = memmem(p, end - p, r->secrets[i], n))) {
			white_out(p, n);
			p += n;
		}
	}
}

void fyai_redact(const struct fyai_redactor *r, char *buf, size_t len)
{
	redact_header_lines(r, buf, len);
	redact_bearer(buf, len);
	if (r)
		redact_secrets(r, buf, len);
}

char *fyai_redact_dup(const struct fyai_redactor *r, const char *s)
{
	char *copy = s ? strdup(s) : NULL;

	if (copy)
		fyai_redact(r, copy, strlen(copy));
	return copy;
}
