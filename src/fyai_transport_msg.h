/*
 * fyai_transport_msg.h - transport request and response payloads
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * A request payload is a 32-bit little-endian length, a JSON header of that
 * length, and the body bytes. The header names an egress profile and a small
 * set of request properties. It has no URL, no header lines, and no
 * credential: the parser rejects every key outside the list below.
 */

#ifndef FYAI_TRANSPORT_MSG_H
#define FYAI_TRANSPORT_MSG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libfyaml.h>
#include <libfyaml/libfyaml-generic.h>

#define FYAI_TRANSPORT_MAX_REQ_HEADER	4096
/* Response credit window, in body bytes, granted before the first credit frame. */
#define FYAI_TRANSPORT_INITIAL_CREDIT	(256 * 1024)

struct fyai_transport_request {
	const char *profile;		/* required */
	const char *method;		/* "POST" (default) or "GET" */
	const char *content_type;	/* application/json or form; may be NULL */
	const char *accept;		/* application/json or text/event-stream */
	long timeout_ms;		/* 0 selects the transport default */
	const uint8_t *body;
	size_t body_len;
};

/*
 * Encode @rq into a malloc'd payload. Strings of @rq are copied into it.
 * Return 0, -EINVAL if a field is not allowed, or -ENOMEM.
 */
int fyai_transport_request_encode(struct fy_generic_builder *gb,
				  const struct fyai_transport_request *rq,
				  uint8_t **out, size_t *len);

/*
 * Parse a request. Strings of @rq belong to @gb and @body points into @p.
 * Return 0 or -EPROTO; @why then names the reason in static storage.
 */
int fyai_transport_request_parse(struct fy_generic_builder *gb,
				 const uint8_t *p, size_t len,
				 struct fyai_transport_request *rq,
				 const char **why);

#define FYAI_TRANSPORT_RL_MAX		16
#define FYAI_TRANSPORT_RL_NAME		64
#define FYAI_TRANSPORT_RL_VALUE		128

/*
 * The rate-limit headers of one response, as the provider sent them: names in
 * lower case, values unchanged. The transport records them and reports them.
 * It never delays, throttles, or retries because of them.
 */
struct fyai_transport_ratelimit {
	unsigned int count;
	struct {
		char name[FYAI_TRANSPORT_RL_NAME];
		char value[FYAI_TRANSPORT_RL_VALUE];
	} item[FYAI_TRANSPORT_RL_MAX];
};

/* Return true for the name of a header that states a rate limit. */
bool fyai_transport_ratelimit_header(const char *name, size_t len);

/*
 * Add "name: value" from a header line, if it states a rate limit. Return
 * true when it was added. A full table drops the header.
 */
bool fyai_transport_ratelimit_add(struct fyai_transport_ratelimit *rl,
				  const char *line, size_t len);

/* Response start: the status and the properties the agent needs. */
struct fyai_transport_response {
	long status;
	long retry_after_s;		/* -1 when the header is absent */
	const char *content_type;	/* may be NULL */
	const char *tag;		/* the tag of the profile; may be NULL */
	const char *model;		/* the model of the profile; may be NULL */
	struct fyai_transport_ratelimit rate_limit;
};

/* Return a builder-owned JSON string, or NULL. */
const char *fyai_transport_response_encode(struct fy_generic_builder *gb,
					   const struct fyai_transport_response *rs);
int fyai_transport_response_parse(struct fy_generic_builder *gb,
				  const uint8_t *p, size_t len,
				  struct fyai_transport_response *rs);

/* A terminal error: the curl code, its text, and whether a retry can succeed. */
struct fyai_transport_error {
	long code;			/* CURLcode, or -1 for a transport refusal */
	const char *message;
	bool transient;
};

const char *fyai_transport_error_encode(struct fy_generic_builder *gb,
					const struct fyai_transport_error *er);
int fyai_transport_error_parse(struct fy_generic_builder *gb,
			       const uint8_t *p, size_t len,
			       struct fyai_transport_error *er);

#endif
