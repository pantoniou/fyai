/*
 * fyai_transport_server.c - the request engine of the credential transport
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * Ownership: a channel belongs to the server from admission until it is
 * doomed. A request belongs to its channel. A doomed channel is freed from a
 * deferred call, never from inside one of its own callbacks.
 *
 * A write callback of curl is atomic: it sends one whole frame or none. When
 * the agent has no credit, or its socket is full, the callback returns
 * CURL_WRITEFUNC_PAUSE, and curl delivers the same bytes again after the
 * transfer resumes. Nothing is buffered here, so a slow agent bounds the memory
 * of its own transfer only.
 *
 * This file reports through the optional log callback and never formats
 * request content or credentials.
 */

#define FYAI_MODULE FYAIEM_STREAM

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "fyai.h"
#include "fyai_curl.h"
#include "fyai_event.h"
#include "fyai_log.h"
#include "fyai_redact.h"
#include "fyai_secret.h"
#include "fyai_transport_server.h"
#include "utils.h"

#define REQ_DEFAULT_TIMEOUT_MS	600000L
#define REQ_MAX_TIMEOUT_MS	3600000L
#define REQ_PENDING_MAX		2
#define REQ_PENDING_DATA	4096
#define MAX_MESSAGES_PER_WAKE	64

struct chan;

/*
 * An immutable snapshot of the profiles. A request keeps a reference to the
 * snapshot it started with, so a reload changes only later requests.
 */
struct pset {
	unsigned int refs;
	struct fyai_transport_grant grant;
};

static struct pset *pset_get(struct pset *set)
{
	if (set)
		set->refs++;
	return set;
}

static void pset_put(struct pset *set)
{
	if (!set || --set->refs)
		return;
	fyai_transport_grant_clear(&set->grant);
	free(set);
}

struct pframe {
	uint16_t kind;
	uint64_t seq;
	size_t len;
	uint8_t data[REQ_PENDING_DATA];
};

struct req {
	struct req *next;
	struct chan *chan;
	uint64_t id;
	uint64_t seq;
	const struct fyai_transport_profile *profile;	/* in @set */
	struct pset *set;
	CURL *easy;
	struct fyai_curl_transfer *xfer;
	struct curl_slist *headers;
	uint8_t *body;

	uint64_t credit;
	bool started;			/* the start frame went out */
	bool paused;			/* curl is paused on this request */
	bool cancelled;
	bool finished;			/* a terminal frame is queued */
	long retry_after;
	char content_type[128];
	struct fyai_transport_ratelimit rl;
	struct pframe pending[REQ_PENDING_MAX];
	unsigned int npending;
};

struct chan {
	struct chan *next;
	struct fyai_transport_server *srv;
	uint64_t id;
	int fd;
	struct fyai_event_source *src;
	struct req *reqs;
	bool want_write;
	bool doomed;
};

/* The latest rate-limit state seen for one endpoint. Informational only. */
struct rl_record {
	struct rl_record *next;
	char *url;
	struct fyai_transport_ratelimit_record rec;
};

struct fyai_transport_server {
	struct fyai_ctx *ctx;
	struct rl_record *records;
	struct pset *profiles;		/* the current snapshot */
	struct fyai_redactor redactor;	/* every credential this server has read */
	struct fyai_event_loop *el;
	struct fyai_transport_registry *reg;
	fyai_transport_cred_fn cred;
	void *cred_ud;
	fyai_transport_log_fn log;
	void *log_ud;
	struct chan *chans;
	struct chan *doomed;		/* waiting for the deferred reap */
	size_t active;
};

/*
 * Write one event to the transport log, if it is on. Everything that the log
 * takes passes the redactor: the log never holds a credential, and the
 * `whitewash_api_keys` setting does not turn that off.
 */
static void srv_event(struct fyai_transport_server *srv, uint64_t id,
		      uint64_t request, const char *event, const char *detail)
{
	struct fyai_cfg *cfg = srv->ctx->cfg;
	struct fy_generic_builder *gb;
	char *safe;
	fy_generic doc;

	if (!cfg->transport_logging)
		return;
	gb = srv->ctx->transient_gb ? srv->ctx->transient_gb : cfg->gb;
	safe = fyai_redact_dup(&srv->redactor, detail ? detail : "");
	if (!safe)
		return;
	doc = fy_mapping(gb, "kind", "transport", "event", event,
			 "t_ms", (long long)fyai_event_now_ms(),
			 "exec", (long long)id, "request", (long long)request,
			 "detail", safe);
	(void)fyai_log_generic(srv->ctx, "transport", doc);
	free(safe);
}

static void srv_log(struct fyai_transport_server *srv, uint64_t id,
		    const char *event, const char *detail)
{
	srv_event(srv, id, 0, event, detail);
	if (srv->log)
		srv->log(srv->log_ud, id, event, detail);
}

/* The raw traffic of one transfer, as curl reports it. */
static int req_debug_cb(CURL *easy, curl_infotype type, char *data, size_t size,
			void *ud);

/* Headers. */

static int header_add(struct curl_slist **list, const char *line)
{
	struct curl_slist *n = curl_slist_append(*list, line);

	if (!n)
		return -ENOMEM;
	*list = n;
	return 0;
}

int fyai_transport_headers_build(const struct fyai_transport_profile *pr,
				 const struct fyai_transport_request *rq,
				 const char *secret, struct curl_slist **out)
{
	struct curl_slist *list = NULL;
	char *line;
	const char *p;
	size_t i;
	int rc;

	*out = NULL;
	if (pr->auth != FYAI_TA_NONE) {
		if (!secret || !*secret)
			return -EINVAL;
		for (p = secret; *p; p++)
			if ((unsigned char)*p < ' ' || *p == 0x7f)
				return -EINVAL;
	}

	/* No Expect: curl would add it for a large body and wait for a reply. */
	rc = header_add(&list, "Expect:");
	if (rc)
		goto err;
	if (rq->content_type) {
		if (asprintf(&line, "Content-Type: %s", rq->content_type) < 0) {
			rc = -ENOMEM;
			goto err;
		}
		rc = header_add(&list, line);
		free(line);
		if (rc)
			goto err;
	}
	if (rq->accept) {
		if (asprintf(&line, "Accept: %s", rq->accept) < 0) {
			rc = -ENOMEM;
			goto err;
		}
		rc = header_add(&list, line);
		free(line);
		if (rc)
			goto err;
	}
	for (i = 0; i < pr->nheaders; i++) {
		rc = header_add(&list, pr->headers[i]);
		if (rc)
			goto err;
	}
	if (pr->auth != FYAI_TA_NONE) {
		if (asprintf(&line, "%s: %s%s",
			     pr->auth == FYAI_TA_BEARER ?
				"Authorization" : pr->header,
			     pr->auth == FYAI_TA_BEARER ? "Bearer " : "",
			     secret) < 0) {
			rc = -ENOMEM;
			goto err;
		}
		rc = header_add(&list, line);
		fyai_secret_clear(line, strlen(line));
		free(line);
		if (rc)
			goto err;
	}
	*out = list;
	return 0;
err:
	curl_slist_free_all(list);
	return rc;
}

/* Frames. */

static void chan_want_write(struct chan *ch, bool want)
{
	if (ch->want_write == want || !ch->src || ch->doomed)
		return;
	ch->want_write = want;
	fyai_event_fd_modify(ch->src, FYAIEV_READ | (want ? FYAIEV_WRITE : 0));
}

/* Send one frame of @rq. Return 0, -EAGAIN when the socket is full, or an error. */
static int req_send(struct req *rq, uint16_t kind, uint64_t seq,
		    const void *data, size_t len)
{
	struct fyai_transport_hdr hdr = {
		.kind = kind, .exec_id = rq->chan->id,
		.request_id = rq->id, .seq = seq, .len = len,
	};

	return fyai_transport_send_frame(rq->chan->fd, &hdr, data);
}

static void chan_doom(struct chan *ch, const char *why);

static int req_queue(struct req *rq, uint16_t kind, const char *json)
{
	struct pframe *pf;
	size_t len = json ? strlen(json) : 0;

	if (rq->npending >= REQ_PENDING_MAX || len > REQ_PENDING_DATA)
		return -ENOSPC;
	pf = &rq->pending[rq->npending++];
	pf->kind = kind;
	pf->seq = rq->seq++;
	pf->len = len;
	if (len)
		memcpy(pf->data, json, len);
	return 0;
}

static void req_free(struct req *rq)
{
	if (rq->xfer)
		fyai_curl_transfer_destroy(rq->xfer);
	if (rq->easy)
		curl_easy_cleanup(rq->easy);
	curl_slist_free_all(rq->headers);
	free(rq->body);
	pset_put(rq->set);
	free(rq);
}

static void req_unlink(struct req *rq)
{
	struct req **pp;

	for (pp = &rq->chan->reqs; *pp; pp = &(*pp)->next) {
		if (*pp == rq) {
			*pp = rq->next;
			return;
		}
	}
}

/*
 * Send the queued frames in order. Return true when the queue is empty. A
 * request with its terminal frame sent is freed and must not be used again.
 */
static bool req_flush(struct req *rq)
{
	struct chan *ch = rq->chan;
	struct pframe *pf;
	unsigned int i;
	int rc;

	for (i = 0; i < rq->npending; i++) {
		pf = &rq->pending[i];

		rc = req_send(rq, pf->kind, pf->seq, pf->data, pf->len);
		if (rc == -EAGAIN) {
			memmove(rq->pending, rq->pending + i,
				(rq->npending - i) * sizeof(*pf));
			rq->npending -= i;
			chan_want_write(ch, true);
			return false;
		}
		if (rc) {
			rq->npending = 0;
			chan_doom(ch, "send failed");
			return false;
		}
	}
	rq->npending = 0;
	if (rq->finished) {
		req_unlink(rq);
		req_free(rq);
		ch->srv->active--;
	}
	return true;
}

/* Queue a terminal error and send it. */
static void req_fail(struct req *rq, long code, const char *message,
		     bool transient)
{
	struct fy_generic_builder_cfg cfg = { .flags = FYGBCF_SCOPE_LEADER };
	struct fyai_transport_error er = {
		.code = code, .message = message, .transient = transient,
	};
	struct fy_generic_builder *gb;
	const char *json;
	char note[192];

	if (rq->finished)
		return;
	snprintf(note, sizeof(note), "code %ld transient %d: %s", code,
		 transient, message ? message : "");
	srv_event(rq->chan->srv, rq->chan->id, rq->id, "error", note);
	gb = fy_generic_builder_create(&cfg);
	json = gb ? fyai_transport_error_encode(gb, &er) : NULL;
	if (!json || req_queue(rq, FYAI_TK_RESP_ERROR, json)) {
		/* An empty error frame still ends the request. */
		rq->npending = 0;
		req_queue(rq, FYAI_TK_RESP_ERROR, NULL);
	}
	rq->finished = true;
	if (gb)
		fy_generic_builder_destroy(gb);
	req_flush(rq);
}

/* Curl callbacks. */

static const char *debug_type_name(curl_infotype type)
{
	switch (type) {
	case CURLINFO_TEXT:
		return "text";
	case CURLINFO_HEADER_IN:
		return "header_in";
	case CURLINFO_HEADER_OUT:
		return "header_out";
	case CURLINFO_DATA_IN:
		return "data_in";
	case CURLINFO_DATA_OUT:
		return "data_out";
	default:
		return NULL;		/* TLS records are noise */
	}
}

static int req_debug_cb(CURL *easy, curl_infotype type, char *data, size_t size,
			void *ud)
{
	struct req *rq = ud;
	struct fyai_transport_server *srv = rq->chan->srv;
	struct fyai_cfg *cfg = srv->ctx->cfg;
	const char *name = debug_type_name(type);
	struct fy_generic_builder *gb;
	char *copy;
	char *wire;
	fy_generic doc;

	(void)easy;
	if (!name || !(cfg->transport_logging || cfg->wire_logging) || !size)
		return 0;
	copy = malloc(size + 1);
	if (!copy)
		return 0;
	memcpy(copy, data, size);
	copy[size] = '\0';
	gb = srv->ctx->transient_gb ? srv->ctx->transient_gb : cfg->gb;
	/* Wire logging keeps the standard curl record and its redaction setting. */
	if (cfg->wire_logging) {
		wire = cfg->whitewash_api_keys ? strdup(copy) : copy;

		if (wire) {
			if (wire != copy)
				fyai_redact(&srv->redactor, wire, size);
			doc = fy_mapping(gb, "kind", "curl", "type", name,
					 "data", fy_string_size(wire, size));
			(void)fyai_log_generic(srv->ctx, "wire", doc);
			if (wire != copy)
				free(wire);
		}
	}
	fyai_redact(&srv->redactor, copy, size);
	if (cfg->transport_logging) {
		doc = fy_mapping(gb, "kind", "transport-wire", "type", name,
				 "exec", (long long)rq->chan->id,
				 "request", (long long)rq->id,
				 "data", fy_string_size(copy, size));
		(void)fyai_log_generic(srv->ctx, "transport", doc);
	}
	free(copy);
	return 0;
}


static size_t req_header_cb(char *p, size_t size, size_t nitems, void *ud)
{
	struct req *rq = ud;
	size_t len = size * nitems;
	char *end;
	const char *vstr;
	long v;
	size_t n;

	if (len >= 5 && !strncasecmp(p, "HTTP/", 5)) {
		/* A new response block: forget the previous one. */
		rq->retry_after = -1;
		rq->content_type[0] = '\0';
		rq->rl.count = 0;
	} else if (fyai_transport_ratelimit_add(&rq->rl, p, len)) {
		/* Recorded and reported; the transport does not act on it. */
	} else if (len > 12 && !strncasecmp(p, "retry-after:", 12)) {
		v = strtol(p + 12, &end, 10);

		if (end != p + 12 && v >= 0)
			rq->retry_after = v;
	} else if (len > 13 && !strncasecmp(p, "content-type:", 13)) {
		vstr = p + 13;
		n = len - 13;

		while (n && (*vstr == ' ' || *vstr == '\t')) {
			vstr++;
			n--;
		}
		while (n && (vstr[n - 1] == '\r' || vstr[n - 1] == '\n' ||
			     vstr[n - 1] == ' '))
			n--;
		if (n >= sizeof(rq->content_type))
			n = sizeof(rq->content_type) - 1;
		memcpy(rq->content_type, vstr, n);
		rq->content_type[n] = '\0';
	}
	return len;
}

/*
 * Keep the latest rate-limit state of the endpoint, and trace it. Only a
 * response that states a limit, or a 429, is recorded. The record is read by
 * fyai_transport_server_ratelimit(); nothing here changes how a request runs.
 */
static void req_record_ratelimit(struct req *rq, long status)
{
	struct fyai_transport_server *srv = rq->chan->srv;
	struct rl_record *r;
	char line[1024];
	size_t n;
	unsigned int i;

	if (!rq->rl.count && status != 429)
		return;
	for (r = srv->records; r; r = r->next)
		if (!strcmp(r->url, rq->profile->url))
			break;
	if (!r) {
		r = calloc(1, sizeof(*r));
		if (!r)
			return;
		r->url = strdup(rq->profile->url);
		if (!r->url) {
			free(r);
			return;
		}
		r->next = srv->records;
		srv->records = r;
	}
	r->rec.status = status;
	r->rec.retry_after_s = rq->retry_after;
	r->rec.when_ms = fyai_event_now_ms();
	r->rec.rl = rq->rl;

	n = snprintf(line, sizeof(line), "%s status %ld retry-after %ld",
		     rq->profile->name, status, rq->retry_after);
	for (i = 0; i < rq->rl.count && n < sizeof(line); i++)
		n += snprintf(line + n, sizeof(line) - n, " %s=%s",
			      rq->rl.item[i].name, rq->rl.item[i].value);
	fyai_diag_tracef("ratelimit", "%s", line);
}

/* Build the start frame. Return a builder-owned JSON string or NULL. */
static const char *req_start_json(struct req *rq, struct fy_generic_builder *gb,
				  bool with_rate_limit)
{
	struct fyai_transport_response rs = {
		.retry_after_s = rq->retry_after,
		.content_type = rq->content_type[0] ? rq->content_type : NULL,
		.tag = rq->profile->tag,
		.model = rq->profile->model,
	};
	char note[96];

	curl_easy_getinfo(rq->easy, CURLINFO_RESPONSE_CODE, &rs.status);
	if (rs.status <= 0)
		rs.status = 599;
	req_record_ratelimit(rq, rs.status);
	if (with_rate_limit) {
		snprintf(note, sizeof(note), "status %ld retry-after %ld limits %u",
			 rs.status, rq->retry_after, rq->rl.count);
		srv_event(rq->chan->srv, rq->chan->id, rq->id, "response", note);
	}
	if (with_rate_limit)
		rs.rate_limit = rq->rl;
	return fyai_transport_response_encode(gb, &rs);
}

static size_t req_write_cb(char *p, size_t size, size_t nmemb, void *ud)
{
	struct req *rq = ud;
	size_t len = size * nmemb;
	struct fy_generic_builder_cfg cfg = { .flags = FYGBCF_SCOPE_LEADER };
	struct fy_generic_builder *gb;
	const char *json;
	int rc;

	if (!len)
		return 0;
	if (len > FYAI_TRANSPORT_MAX_PAYLOAD || rq->cancelled || rq->finished)
		return 0;
	if (rq->credit < len || rq->npending) {
		rq->paused = true;
		return CURL_WRITEFUNC_PAUSE;
	}

	if (!rq->started) {
		gb = fy_generic_builder_create(&cfg);
		json = gb ? req_start_json(rq, gb, true) : NULL;

		if (!json) {
			if (gb)
				fy_generic_builder_destroy(gb);
			return 0;
		}
		rc = req_send(rq, FYAI_TK_RESP_START, rq->seq, json,
			      strlen(json));
		fy_generic_builder_destroy(gb);
		if (rc == -EAGAIN) {
			rq->paused = true;
			chan_want_write(rq->chan, true);
			return CURL_WRITEFUNC_PAUSE;
		}
		if (rc)
			return 0;
		rq->seq++;
		rq->started = true;
	}

	rc = req_send(rq, FYAI_TK_RESP_BODY, rq->seq, p, len);
	if (rc == -EAGAIN) {
		rq->paused = true;
		chan_want_write(rq->chan, true);
		return CURL_WRITEFUNC_PAUSE;
	}
	if (rc)
		return 0;
	rq->seq++;
	rq->credit -= len;
	return len;
}

static void req_complete_cb(struct fyai_curl_transfer *xfer, void *ud)
{
	struct fy_generic_builder_cfg cfg = { .flags = FYGBCF_SCOPE_LEADER };
	struct req *rq = ud;
	struct fy_generic_builder *gb;
	CURLcode code = fyai_curl_collect(xfer);
	long status = 0;
	const char *json;

	curl_easy_getinfo(rq->easy, CURLINFO_RESPONSE_CODE, &status);
	fyai_curl_transfer_destroy(xfer);
	rq->xfer = NULL;
	rq->paused = false;

	if (rq->cancelled) {
		req_fail(rq, -2, "cancelled", false);
		return;
	}
	if (code != CURLE_OK) {
		req_fail(rq, code, curl_easy_strerror(code),
			 fyai_http_transient(code, status));
		return;
	}

	gb = fy_generic_builder_create(&cfg);
	json = gb && !rq->started ? req_start_json(rq, gb, true) : NULL;
	/* The rate limit is a report; drop it if it does not fit a queued frame. */
	if (json && strlen(json) > REQ_PENDING_DATA)
		json = req_start_json(rq, gb, false);
	if (!rq->started) {
		if (!json || req_queue(rq, FYAI_TK_RESP_START, json)) {
			if (gb)
				fy_generic_builder_destroy(gb);
			req_fail(rq, -1, "response could not be reported", false);
			return;
		}
		rq->started = true;
	}
	if (gb)
		fy_generic_builder_destroy(gb);
	req_queue(rq, FYAI_TK_RESP_END, NULL);
	rq->finished = true;
	srv_event(rq->chan->srv, rq->chan->id, rq->id, "end", "");
	req_flush(rq);
}

/* Requests. */

static struct req *req_find(struct chan *ch, uint64_t id)
{
	struct req *rq;

	for (rq = ch->reqs; rq; rq = rq->next)
		if (rq->id == id)
			return rq;
	return NULL;
}

/*
 * Reject a body whose model differs from the granted one. The grant of the
 * execution can narrow the model of the profile; it cannot widen it.
 */
static const char *model_check(const struct fyai_transport_profile *pr,
			       const char *narrow,
			       const struct fyai_transport_request *rq)
{
	struct fy_generic_builder_cfg cfg = { .flags = FYGBCF_SCOPE_LEADER };
	struct fy_generic_builder *gb;
	fy_generic doc, model;
	const char *want, *why = NULL;

	if (pr->model && narrow && strcmp(pr->model, narrow))
		return "model is not granted";
	want = narrow ? narrow : pr->model;
	if (!want)
		return NULL;
	if (!rq->content_type || strcmp(rq->content_type, "application/json"))
		return "profile needs a JSON body";
	gb = fy_generic_builder_create(&cfg);
	if (!gb)
		return "out of memory";
	doc = parse_json_string_size(gb, (const char *)rq->body, rq->body_len);
	model = fy_get(doc, "model", fy_invalid);
	if (!fy_is_mapping(doc) || !fy_is_string(model) ||
	    strcmp(fy_castp(&model, ""), want))
		why = "model is not granted";
	fy_generic_builder_destroy(gb);
	return why;
}

static void req_setup_curl(struct req *rq, const struct fyai_transport_request *r)
{
	const struct fyai_transport_profile *pr = rq->profile;
	bool plain = !strncmp(pr->url, "http://", 7);
	long timeout = r->timeout_ms ? r->timeout_ms : REQ_DEFAULT_TIMEOUT_MS;

	if (timeout > REQ_MAX_TIMEOUT_MS)
		timeout = REQ_MAX_TIMEOUT_MS;

	curl_easy_setopt(rq->easy, CURLOPT_URL, pr->url);
	curl_easy_setopt(rq->easy, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(rq->easy, CURLOPT_TIMEOUT_MS, timeout);
	curl_easy_setopt(rq->easy, CURLOPT_USERAGENT, "fyai-transport");
	curl_easy_setopt(rq->easy, CURLOPT_HTTPHEADER, rq->headers);
	curl_easy_setopt(rq->easy, CURLOPT_WRITEFUNCTION, req_write_cb);
	curl_easy_setopt(rq->easy, CURLOPT_WRITEDATA, rq);
	curl_easy_setopt(rq->easy, CURLOPT_HEADERFUNCTION, req_header_cb);
	curl_easy_setopt(rq->easy, CURLOPT_HEADERDATA, rq);
	if (rq->chan->srv->ctx->cfg->transport_logging ||
	    rq->chan->srv->ctx->cfg->wire_logging) {
		curl_easy_setopt(rq->easy, CURLOPT_VERBOSE, 1L);
		curl_easy_setopt(rq->easy, CURLOPT_DEBUGFUNCTION, req_debug_cb);
		curl_easy_setopt(rq->easy, CURLOPT_DEBUGDATA, rq);
	}

	/*
	 * The endpoint is the profile and nothing else: no redirect, no proxy,
	 * no scheme but its own, and the peer is always verified.
	 */
	curl_easy_setopt(rq->easy, CURLOPT_FOLLOWLOCATION, 0L);
	curl_easy_setopt(rq->easy, CURLOPT_PROTOCOLS_STR, plain ? "http" : "https");
	curl_easy_setopt(rq->easy, CURLOPT_REDIR_PROTOCOLS_STR, "");
	curl_easy_setopt(rq->easy, CURLOPT_PROXY, "");
	curl_easy_setopt(rq->easy, CURLOPT_NOPROXY, "*");
	curl_easy_setopt(rq->easy, CURLOPT_NETRC, (long)CURL_NETRC_IGNORED);
	curl_easy_setopt(rq->easy, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(rq->easy, CURLOPT_SSL_VERIFYHOST, 2L);

	if (!strcmp(r->method, "GET")) {
		curl_easy_setopt(rq->easy, CURLOPT_HTTPGET, 1L);
	} else {
		curl_easy_setopt(rq->easy, CURLOPT_POST, 1L);
		curl_easy_setopt(rq->easy, CURLOPT_POSTFIELDS, rq->body);
		curl_easy_setopt(rq->easy, CURLOPT_POSTFIELDSIZE_LARGE,
				 (curl_off_t)r->body_len);
	}
}

static void req_start(struct chan *ch, const struct fyai_transport_msg *msg)
{
	struct fy_generic_builder_cfg cfg = { .flags = FYGBCF_SCOPE_LEADER };
	struct fyai_transport_server *srv = ch->srv;
	struct fyai_transport_request r;
	struct fy_generic_builder *gb;
	struct fyai_transport_exec *ex;
	const char *why = NULL, *narrow = NULL;
	char *secret = NULL;
	char note[160];
	struct req *rq;
	int rc;

	rq = calloc(1, sizeof(*rq));
	if (!rq) {
		chan_doom(ch, "out of memory");
		return;
	}
	rq->chan = ch;
	rq->id = msg->hdr.request_id;
	rq->credit = FYAI_TRANSPORT_INITIAL_CREDIT;
	rq->retry_after = -1;
	rq->next = ch->reqs;
	ch->reqs = rq;
	srv->active++;

	gb = fy_generic_builder_create(&cfg);
	if (!gb) {
		req_fail(rq, -1, "out of memory", false);
		return;
	}
	rc = fyai_transport_request_parse(gb, msg->payload, msg->len, &r, &why);
	if (rc) {
		req_fail(rq, -1, why, false);
		goto out;
	}
	ex = fyai_transport_find(srv->reg, ch->id);
	if (!ex || !fyai_transport_exec_allows(ex, r.profile, &narrow)) {
		req_fail(rq, -1, "profile is not granted", false);
		goto out;
	}
	rq->set = pset_get(srv->profiles);
	rq->profile = rq->set ?
		fyai_transport_grant_find(&rq->set->grant, r.profile) : NULL;
	if (!rq->profile) {
		/* Granted, but the current set has no such profile. */
		req_fail(rq, -1, "profile does not exist", false);
		goto out;
	}
	snprintf(note, sizeof(note), "profile %s method %s body %zu%s",
		 rq->profile->name, r.method, r.body_len,
		 rq->profile->plain_http ? " plain-http" : "");
	srv_event(srv, ch->id, rq->id, "request", note);
	if (!strcmp(r.method, "GET") && r.body_len) {
		req_fail(rq, -1, "a GET request has no body", false);
		goto out;
	}
	why = model_check(rq->profile, narrow, &r);
	if (why) {
		req_fail(rq, -1, why, false);
		goto out;
	}

	if (rq->profile->auth != FYAI_TA_NONE) {
		rc = srv->cred ? srv->cred(srv->cred_ud, rq->profile, &secret) : -ENOENT;
		if (rc || !secret) {
			req_fail(rq, -1, "credential could not be resolved", false);
			goto out;
		}
	}
	/* Learn the credential before anything can be logged about it. */
	if (secret)
		(void)fyai_redactor_add_secret(&srv->redactor, secret);
	if (rq->profile->auth == FYAI_TA_HEADER)
		(void)fyai_redactor_add_header(&srv->redactor, rq->profile->header);
	rc = fyai_transport_headers_build(rq->profile, &r, secret, &rq->headers);
	if (secret) {
		fyai_secret_clear(secret, strlen(secret));
		free(secret);
	}
	if (rc) {
		req_fail(rq, -1, "request headers could not be built", false);
		goto out;
	}

	if (r.body_len) {
		rq->body = malloc(r.body_len);
		if (!rq->body) {
			req_fail(rq, -1, "out of memory", false);
			goto out;
		}
		memcpy(rq->body, r.body, r.body_len);
	}
	rq->easy = curl_easy_init();
	if (!rq->easy) {
		req_fail(rq, -1, "curl handle could not be created", false);
		goto out;
	}
	req_setup_curl(rq, &r);
	rq->xfer = fyai_curl_submit(srv->ctx, rq->easy, req_complete_cb, rq);
	if (!rq->xfer)
		req_fail(rq, -1, "transfer could not be started", false);
out:
	fy_generic_builder_destroy(gb);
}

static void req_cancel(struct req *rq)
{
	if (rq->cancelled || rq->finished)
		return;
	srv_event(rq->chan->srv, rq->chan->id, rq->id, "cancel", "");
	rq->cancelled = true;
	if (rq->xfer)
		fyai_curl_cancel(rq->xfer);
}

static void req_resume(struct req *rq)
{
	if (rq->npending) {
		req_flush(rq);
		return;
	}
	if (rq->paused && rq->easy && !rq->finished && rq->credit) {
		rq->paused = false;
		curl_easy_pause(rq->easy, CURLPAUSE_CONT);
	}
}

/* Channels. */

static void chan_reap(void *userdata);

static void chan_unlink(struct chan *ch)
{
	struct chan **pp;

	for (pp = &ch->srv->chans; *pp; pp = &(*pp)->next) {
		if (*pp == ch) {
			*pp = ch->next;
			return;
		}
	}
}

/* Move a channel to the doomed list; a deferred call frees it. */
static void chan_doom(struct chan *ch, const char *why)
{
	struct fyai_transport_server *srv = ch->srv;

	if (ch->doomed)
		return;
	ch->doomed = true;
	srv_log(srv, ch->id, "retired", why);
	chan_unlink(ch);
	ch->next = srv->doomed;
	srv->doomed = ch;
	fyai_event_defer(srv->el, chan_reap, srv);
}

/* Free a channel and cancel its work. The registry closes the descriptor. */
static void chan_free(struct chan *ch)
{
	struct req *rq;

	if (ch->src)
		fyai_event_source_remove(ch->src);
	while ((rq = ch->reqs)) {
		ch->reqs = rq->next;
		req_free(rq);
		ch->srv->active--;
	}
	fyai_transport_retire(ch->srv->reg, ch->id);
	free(ch);
}

static void chan_reap(void *userdata)
{
	struct fyai_transport_server *srv = userdata;
	struct chan *ch;

	while ((ch = srv->doomed)) {
		srv->doomed = ch->next;
		chan_free(ch);
	}
}

static void chan_dispatch(struct chan *ch, const struct fyai_transport_msg *msg)
{
	struct req *rq;
	uint64_t add;

	switch (msg->hdr.kind) {
	case FYAI_TK_REQUEST:
		if (req_find(ch, msg->hdr.request_id)) {
			chan_doom(ch, "duplicate request id");
			return;
		}
		req_start(ch, msg);
		break;
	case FYAI_TK_CANCEL:
		rq = req_find(ch, msg->hdr.request_id);
		if (rq)
			req_cancel(rq);
		break;
	case FYAI_TK_CREDIT:
		rq = req_find(ch, msg->hdr.request_id);
		if (msg->len != 4) {
			chan_doom(ch, "credit frame is malformed");
			return;
		}
		if (rq) {
			add = msg->payload[0] | msg->payload[1] << 8 |
				       msg->payload[2] << 16 |
				       (uint64_t)msg->payload[3] << 24;

			rq->credit += add;
			req_resume(rq);
		}
		break;
	}
}

static void chan_readable(struct chan *ch)
{
	struct fyai_transport_server *srv = ch->srv;
	struct fyai_transport_msg msg;
	enum fyai_transport_verdict v;
	unsigned int n;

	for (n = 0; n < MAX_MESSAGES_PER_WAKE && !ch->doomed; n++) {
		v = fyai_transport_recv(srv->reg, ch->fd, &msg);
		switch (v) {
		case FYAI_TV_OK:
			chan_dispatch(ch, &msg);
			break;
		case FYAI_TV_MORE:
			break;
		case FYAI_TV_AGAIN:
			return;
		case FYAI_TV_WRONG_SENDER:
		case FYAI_TV_NO_CREDENTIALS:
		case FYAI_TV_CONTAINMENT:
			/* A process that holds a copy of the descriptor. */
			srv_log(srv, ch->id, "rejected",
				fyai_transport_verdict_str(v));
			break;
		default:
			chan_doom(ch, fyai_transport_verdict_str(v));
			return;
		}
	}
}

static void chan_writable(struct chan *ch)
{
	struct req *rq, *next;

	chan_want_write(ch, false);
	for (rq = ch->reqs; rq && !ch->doomed; rq = next) {
		next = rq->next;
		req_resume(rq);
	}
}

static enum fyai_event_action chan_on_event(const struct fyai_event *ev)
{
	struct chan *ch = ev->userdata;

	if (ch->doomed)
		return FYAIEA_CONTINUE;
	if (ev->events & FYAIEV_ERROR) {
		chan_doom(ch, "channel error");
		return FYAIEA_CONTINUE;
	}
	if (ev->events & FYAIEV_WRITE)
		chan_writable(ch);
	if (!ch->doomed && (ev->events & (FYAIEV_READ | FYAIEV_EOF)))
		chan_readable(ch);
	return FYAIEA_CONTINUE;
}

/* Server. */

struct fyai_transport_server *
fyai_transport_server_create(struct fyai_ctx *ctx,
			     struct fyai_transport_registry *reg,
			     fyai_transport_cred_fn cred, void *cred_userdata,
			     fyai_transport_log_fn log, void *log_userdata)
{
	struct fyai_transport_server *srv;

	srv = calloc(1, sizeof(*srv));
	fyai_error_check(ctx, srv, err, "could not allocate the transport server");
	srv->el = fyai_ctx_loop(ctx);
	fyai_error_check(ctx, srv->el, err_free,
			 "could not create the transport event loop");
	srv->ctx = ctx;
	fyai_redactor_init(&srv->redactor);
	srv->reg = reg;
	srv->cred = cred;
	srv->cred_ud = cred_userdata;
	srv->log = log;
	srv->log_ud = log_userdata;
	return srv;
err_free:
	free(srv);
err:
	return NULL;
}

void fyai_transport_server_destroy(struct fyai_transport_server *srv)
{
	struct chan *ch;
	struct rl_record *r;

	if (!srv)
		return;
	fyai_event_defer_cancel(srv->el, chan_reap, srv);
	while ((ch = srv->chans)) {
		srv->chans = ch->next;
		chan_free(ch);
	}
	while ((ch = srv->doomed)) {
		srv->doomed = ch->next;
		chan_free(ch);
	}
	while (srv->records) {
		r = srv->records;

		srv->records = r->next;
		free(r->url);
		free(r);
	}
	pset_put(srv->profiles);
	fyai_redactor_clear(&srv->redactor);
	free(srv);
}

int fyai_transport_server_set_profiles(struct fyai_transport_server *srv,
				       struct fyai_transport_grant *grant)
{
	struct pset *set;

	set = calloc(1, sizeof(*set));
	if (!set)
		return -ENOMEM;
	set->refs = 1;
	set->grant = *grant;
	memset(grant, 0, sizeof(*grant));
	pset_put(srv->profiles);
	srv->profiles = set;
	srv_event(srv, 0, 0, "profiles", "");
	return 0;
}

int fyai_transport_server_admit(struct fyai_transport_server *srv, uint64_t id,
				uint64_t parent_id, pid_t pid, uid_t uid,
				int channel,
				const struct fyai_transport_allow *allow,
				size_t nallow,
				const struct fyai_transport_ns_req *ns)
{
	struct chan *ch;
	char note[64];
	int fl, rc;

	ch = calloc(1, sizeof(*ch));
	if (!ch)
		return -ENOMEM;
	rc = fyai_transport_register(srv->reg, id, parent_id, pid, uid, channel,
				     allow, nallow, ns);
	if (rc) {
		free(ch);
		return rc;
	}
	ch->srv = srv;
	ch->id = id;
	ch->fd = channel;

	fl = fcntl(channel, F_GETFL);
	if (fl < 0 || fcntl(channel, F_SETFL, fl | O_NONBLOCK) < 0) {
		rc = -errno;
		goto err_retire;
	}
	rc = fyai_event_add_fd(srv->el, channel, FYAIEV_READ, chan_on_event, ch,
			       &ch->src);
	if (rc) {
		rc = -EIO;
		goto err_retire;
	}
	ch->next = srv->chans;
	srv->chans = ch;
	snprintf(note, sizeof(note), "pid %d", (int)pid);
	srv_event(srv, id, 0, "admitted", note);
	return 0;
err_retire:
	/* The registry closed the channel; nothing else refers to it. */
	fyai_transport_retire(srv->reg, id);
	free(ch);
	return rc;
}

int fyai_transport_server_set_grant(struct fyai_transport_server *srv,
				    uint64_t id,
				    const struct fyai_transport_allow *allow,
				    size_t nallow)
{
	int rc = fyai_transport_set_grant(srv->reg, id, allow, nallow);

	if (!rc)
		srv_event(srv, id, 0, "grant", "");
	return rc;
}

int fyai_transport_server_retire(struct fyai_transport_server *srv, uint64_t id)
{
	struct chan *ch;

	for (ch = srv->chans; ch; ch = ch->next) {
		if (ch->id != id)
			continue;
		chan_unlink(ch);
		chan_free(ch);
		return 0;
	}
	return -ENOENT;
}

size_t fyai_transport_server_active(const struct fyai_transport_server *srv)
{
	return srv->active;
}

bool fyai_transport_server_ratelimit(const struct fyai_transport_server *srv,
				     const char *url,
				     struct fyai_transport_ratelimit_record *out)
{
	const struct rl_record *r;

	for (r = srv->records; r; r = r->next) {
		if (!strcmp(r->url, url)) {
			*out = r->rec;
			return true;
		}
	}
	return false;
}
