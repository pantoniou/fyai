/*
 * fyai_transport_msg.c - transport request and response payloads
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "fyai_transport_msg.h"
#include "utils.h"

static const char *const rl_prefixes[] = {
	"x-ratelimit-", "anthropic-ratelimit-", "ratelimit-", NULL
};

bool fyai_transport_ratelimit_header(const char *name, size_t len)
{
	const char *const *p;
	size_t n;

	for (p = rl_prefixes; *p; p++) {
		n = strlen(*p);

		if (len > n && !strncasecmp(name, *p, n))
			return true;
	}
	return false;
}

bool fyai_transport_ratelimit_add(struct fyai_transport_ratelimit *rl,
				  const char *line, size_t len)
{
	const char *colon = memchr(line, ':', len);
	size_t nlen, vlen, i;
	const char *v;

	if (!colon || !fyai_transport_ratelimit_header(line, colon - line))
		return false;
	nlen = colon - line;
	v = colon + 1;
	vlen = len - nlen - 1;
	while (vlen && (*v == ' ' || *v == '\t')) {
		v++;
		vlen--;
	}
	while (vlen && (v[vlen - 1] == '\r' || v[vlen - 1] == '\n' ||
			v[vlen - 1] == ' ' || v[vlen - 1] == '\t'))
		vlen--;
	if (rl->count >= FYAI_TRANSPORT_RL_MAX ||
	    nlen >= FYAI_TRANSPORT_RL_NAME || vlen >= FYAI_TRANSPORT_RL_VALUE)
		return false;
	for (i = 0; i < nlen; i++)
		rl->item[rl->count].name[i] = tolower((unsigned char)line[i]);
	rl->item[rl->count].name[nlen] = '\0';
	memcpy(rl->item[rl->count].value, v, vlen);
	rl->item[rl->count].value[vlen] = '\0';
	rl->count++;
	return true;
}

static bool one_of(const char *s, const char *const *list)
{
	for (; *list; list++)
		if (!strcmp(s, *list))
			return true;
	return false;
}

static const char *const methods[] = { "POST", "GET", NULL };
static const char *const content_types[] = {
	"application/json", "application/x-www-form-urlencoded", NULL
};
static const char *const accepts[] = {
	"application/json", "text/event-stream", NULL
};

static bool field_allowed(const char *key, const char *val)
{
	if (!strcmp(key, "method"))
		return one_of(val, methods);
	if (!strcmp(key, "content_type"))
		return one_of(val, content_types);
	if (!strcmp(key, "accept"))
		return one_of(val, accepts);
	return false;
}

int fyai_transport_request_encode(struct fy_generic_builder *gb,
				  const struct fyai_transport_request *rq,
				  uint8_t **out, size_t *len)
{
	static const char *const keys[] = { "method", "content_type", "accept" };
	const char *vals[] = { rq->method, rq->content_type, rq->accept };
	fy_generic hdr;
	const char *json;
	uint8_t *buf;
	size_t hlen, i;

	if (!rq->profile || !*rq->profile || rq->timeout_ms < 0)
		return -EINVAL;
	hdr = fy_mapping(gb, "profile", rq->profile);
	for (i = 0; i < 3; i++) {
		if (!vals[i])
			continue;
		if (!field_allowed(keys[i], vals[i]))
			return -EINVAL;
		hdr = fy_assoc(gb, hdr, fy_value(gb, keys[i]),
			       fy_value(gb, vals[i]));
	}
	if (rq->timeout_ms)
		hdr = fy_assoc(gb, hdr, fy_value(gb, "timeout_ms"),
			       fy_value(gb, (long long)rq->timeout_ms));
	json = emit_json_string(gb, hdr);
	if (!json)
		return -ENOMEM;
	hlen = strlen(json);
	if (hlen > FYAI_TRANSPORT_MAX_REQ_HEADER)
		return -EINVAL;

	buf = malloc(4 + hlen + rq->body_len);
	if (!buf)
		return -ENOMEM;
	for (i = 0; i < 4; i++)
		buf[i] = (hlen >> (8 * i)) & 0xff;
	memcpy(buf + 4, json, hlen);
	if (rq->body_len)
		memcpy(buf + 4 + hlen, rq->body, rq->body_len);
	*out = buf;
	*len = 4 + hlen + rq->body_len;
	return 0;
}

int fyai_transport_request_parse(struct fy_generic_builder *gb,
				 const uint8_t *p, size_t len,
				 struct fyai_transport_request *rq,
				 const char **why)
{
	fy_generic hdr, key, val;
	size_t hlen = 0, i;
	bool have_profile = false;
	const char *k, *s;
	long long t;

	memset(rq, 0, sizeof(*rq));
	rq->method = "POST";
	if (len < 4) {
		*why = "request is shorter than its length prefix";
		return -EPROTO;
	}
	for (i = 0; i < 4; i++)
		hlen |= (size_t)p[i] << (8 * i);
	if (hlen > FYAI_TRANSPORT_MAX_REQ_HEADER || hlen > len - 4) {
		*why = "request header length is not valid";
		return -EPROTO;
	}

	hdr = parse_json_string_size(gb, (const char *)p + 4, hlen);
	if (!fy_is_mapping(hdr)) {
		*why = "request header is not a JSON mapping";
		return -EPROTO;
	}
	fy_foreach_key_value(key, val, hdr) {
		k = fy_castp(&key, "");

		if (!strcmp(k, "profile")) {
			if (!fy_is_string(val) || fy_empty(val)) {
				*why = "profile is not a name";
				return -EPROTO;
			}
			rq->profile = fy_gb_intern_string(gb, fy_castp(&val, ""));
			have_profile = rq->profile != NULL;
		} else if (!strcmp(k, "timeout_ms")) {
			t = fy_number(val, -1LL);

			if (!fy_generic_is_int(val) || t < 0 || t > 86400000LL) {
				*why = "timeout_ms is out of range";
				return -EPROTO;
			}
			rq->timeout_ms = t;
		} else if (fy_is_string(val) &&
			   (!strcmp(k, "method") || !strcmp(k, "content_type") ||
			    !strcmp(k, "accept"))) {
			s = fy_gb_intern_string(gb, fy_castp(&val, ""));

			if (!s || !field_allowed(k, s)) {
				*why = "request property has a value that is not allowed";
				return -EPROTO;
			}
			if (!strcmp(k, "method"))
				rq->method = s;
			else if (!strcmp(k, "content_type"))
				rq->content_type = s;
			else
				rq->accept = s;
		} else {
			*why = "request header has a field that is not allowed";
			return -EPROTO;
		}
	}
	if (!have_profile) {
		*why = "request names no profile";
		return -EPROTO;
	}
	rq->body = p + 4 + hlen;
	rq->body_len = len - 4 - hlen;
	return 0;
}

const char *fyai_transport_response_encode(struct fy_generic_builder *gb,
					   const struct fyai_transport_response *rs)
{
	fy_generic m = fy_mapping(gb, "status", (long long)rs->status);
	fy_generic rl;
	unsigned int i;

	if (rs->retry_after_s >= 0)
		m = fy_assoc(gb, m, fy_value(gb, "retry_after"),
			     fy_value(gb, (long long)rs->retry_after_s));
	if (rs->content_type)
		m = fy_assoc(gb, m, fy_value(gb, "content_type"),
			     fy_value(gb, rs->content_type));
	if (rs->tag)
		m = fy_assoc(gb, m, fy_value(gb, "tag"), fy_value(gb, rs->tag));
	if (rs->model)
		m = fy_assoc(gb, m, fy_value(gb, "model"),
			     fy_value(gb, rs->model));
	if (rs->rate_limit.count) {
		rl = fy_mapping(gb);

		for (i = 0; i < rs->rate_limit.count; i++)
			rl = fy_assoc(gb, rl,
				      fy_value(gb, rs->rate_limit.item[i].name),
				      fy_value(gb, rs->rate_limit.item[i].value));
		m = fy_assoc(gb, m, fy_value(gb, "rate_limit"), rl);
	}
	return emit_json_string(gb, m);
}

/* Fill a string field from @doc, or NULL when the key is absent. */
static const char *opt_str(struct fy_generic_builder *gb, fy_generic doc,
			   const char *key)
{
	fy_generic v = fy_get(doc, key, fy_invalid);

	if (!fy_is_string(v))
		return NULL;
	return fy_gb_intern_string(gb, fy_castp(&v, ""));
}

int fyai_transport_response_parse(struct fy_generic_builder *gb,
				  const uint8_t *p, size_t len,
				  struct fyai_transport_response *rs)
{
	fy_generic doc = parse_json_string_size(gb, (const char *)p, len);
	fy_generic rl, key, val;
	const char *k, *v;
	unsigned int n;

	memset(rs, 0, sizeof(*rs));
	if (!fy_is_mapping(doc))
		return -EPROTO;
	rs->status = fy_get(doc, "status", 0LL);
	rs->retry_after_s = fy_get(doc, "retry_after", -1LL);
	rs->content_type = opt_str(gb, doc, "content_type");
	rs->tag = opt_str(gb, doc, "tag");
	rs->model = opt_str(gb, doc, "model");
	rl = fy_get(doc, "rate_limit", fy_invalid);
	if (fy_is_mapping(rl)) {
		fy_foreach_key_value(key, val, rl) {
			k = fy_castp(&key, "");
			v = fy_castp(&val, "");
			n = rs->rate_limit.count;

			if (!fy_is_string(val) || n >= FYAI_TRANSPORT_RL_MAX ||
			    strlen(k) >= FYAI_TRANSPORT_RL_NAME ||
			    strlen(v) >= FYAI_TRANSPORT_RL_VALUE)
				continue;
			strcpy(rs->rate_limit.item[n].name, k);
			strcpy(rs->rate_limit.item[n].value, v);
			rs->rate_limit.count++;
		}
	}
	return rs->status > 0 ? 0 : -EPROTO;
}

const char *fyai_transport_error_encode(struct fy_generic_builder *gb,
					const struct fyai_transport_error *er)
{
	return emit_json_string(gb,
		fy_mapping(gb, "code", (long long)er->code,
			   "message", er->message ? er->message : "",
			   "transient", er->transient));
}

int fyai_transport_error_parse(struct fy_generic_builder *gb,
			       const uint8_t *p, size_t len,
			       struct fyai_transport_error *er)
{
	fy_generic doc = parse_json_string_size(gb, (const char *)p, len);

	memset(er, 0, sizeof(*er));
	if (!fy_is_mapping(doc))
		return -EPROTO;
	er->code = fy_get(doc, "code", -1LL);
	er->message = opt_str(gb, doc, "message");
	er->transient = fy_get(doc, "transient", false);
	return 0;
}
