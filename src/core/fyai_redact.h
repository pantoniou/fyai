/*
 * fyai_redact.h - white out credentials in text that leaves the process
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * Every dump of wire traffic or of a diagnostic passes through this module
 * before it is written. It removes three kinds of text and keeps the length of
 * the buffer: the value of a credential header, a bearer token, and each
 * occurrence of a secret that the caller states. The last kind is the
 * defence in depth for a provider that echoes a key in a body or an error.
 */

#ifndef FYAI_REDACT_H
#define FYAI_REDACT_H

#include <stddef.h>

#define FYAI_REDACTED		"[redacted]"
/* A secret shorter than this is not a credential and is not tracked. */
#define FYAI_REDACT_MIN_SECRET	4

struct fyai_redactor {
	char **secrets;
	size_t nsecrets;
	char **headers;
	size_t nheaders;
};

void fyai_redactor_init(struct fyai_redactor *r);
/* Wipe the stored secrets, then free everything. */
void fyai_redactor_clear(struct fyai_redactor *r);

/*
 * Track a secret value; the redactor keeps a copy. Return 0, -EINVAL for a
 * value that is too short, or -ENOMEM. The same value is kept once.
 */
int fyai_redactor_add_secret(struct fyai_redactor *r, const char *value);

/* White out the value of this header too, such as a custom key header. */
int fyai_redactor_add_header(struct fyai_redactor *r, const char *name);

/*
 * Redact @buf in place. The length does not change. @r can be NULL: the
 * built-in header names and bearer tokens are still removed. The built-in
 * headers are Authorization, Proxy-Authorization, X-Api-Key, Api-Key,
 * X-Goog-Api-Key, Cookie, and Set-Cookie.
 */
void fyai_redact(const struct fyai_redactor *r, char *buf, size_t len);

/* Return a redacted copy of the NUL-terminated @s, or NULL. Free it. */
char *fyai_redact_dup(const struct fyai_redactor *r, const char *s);

#endif
