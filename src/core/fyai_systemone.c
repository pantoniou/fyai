/*
 * fyai_systemone.c - the System One API of decision models
 *
 * One request is one POST with the key as a bearer token; the answer is
 * parsed whole. A provider with an `envelope` wraps the result: the answer
 * is {code, message, data: {result}}, and a code other than 0 is a failure.
 *
 * SPDX-License-Identifier: MIT
 */
#define FYAI_MODULE FYAIEM_UNKNOWN
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

#include "fyai.h"
#include "fyai_buf.h"
#include "fyai_config.h"
#include "fyai_curl.h"
#include "fyai_decider.h"
#include "fyai_event.h"
#include "fyai_link.h"
#include "fyai_secret.h"
#include "fyai_stream.h"
#include "fyai_systemone.h"
#include "fyai_transport_boot.h"
#include "fyai_transport_client.h"
#include "fyai_transport_cfg.h"
#include "utils.h"

/* A request that takes longer than this has failed. */
#define SYSTEMONE_TIMEOUT_S	120

struct systemone_xfer {
	CURL *easy;
	struct fyai_curl_transfer *transfer;
	struct fyai_buf response;
	CURLcode code;
	long status;
	bool done;
	struct fyai_tcall *call;
	char message[256];
};

const char *fyai_systemone_key(struct fyai_ctx *ctx,
			       struct fy_generic_builder *gb,
			       fy_generic provider)
{
	fy_generic name = fy_get(provider, "name", fy_invalid);
	fy_generic env = fy_get(provider, "key_env", fy_invalid);
	const char *var, *val, *key = NULL;
	char *secret = NULL;
	size_t len = 0;

	(void)ctx;
	var = fy_is_string(env) ? fy_castp(&env, "") :
	      fyai_config_provider_env_key(gb, fy_castp(&name, ""));
	val = var ? getenv(var) : NULL;
	if (val && *val)
		return fy_gb_intern_string(gb, val);
	/* Under isolation only the transport reads the secret store. */
	if (fyai_transport_supervised())
		return NULL;
	if (fyai_secret_kernel_get(fy_sprintfa("fyai:api-key/%s",
					       fy_castp(&name, "")),
				   &secret, &len) == FYAI_SECRET_OK) {
		key = fy_gb_intern_string(gb, secret);
		fyai_secret_clear_and_free(&secret, &len);
	}
	return key;
}

static void systemone_complete(struct fyai_curl_transfer *transfer,
			       void *userdata)
{
	struct systemone_xfer *x = userdata;

	x->code = fyai_curl_collect(transfer);
	curl_easy_getinfo(x->easy, CURLINFO_RESPONSE_CODE, &x->status);
	fyai_curl_transfer_destroy(transfer);
	x->transfer = NULL;
	x->done = true;
}

/*
 * The credential source of @provider that only the transport resolves:
 * its variable, else the secret fyai:api-key/NAME.
 */
const char *fyai_systemone_credential(struct fy_generic_builder *gb,
					fy_generic provider)
{
	fy_generic name = fy_get(provider, "name", fy_invalid);
	fy_generic env = fy_get(provider, "key_env", fy_invalid);
	const char *var = fy_is_string(env) ? fy_castp(&env, "") :
			  fyai_config_provider_env_key(gb, fy_castp(&name, ""));
	const char *nm = fy_castp(&name, "");

	return fy_gb_intern_string(gb, var ?
		fy_sprintfa("env:%s|secret:api-key/%s", var, nm) :
		fy_sprintfa("secret:api-key/%s", nm));
}

const char *const *fyai_systemone_headers(bool images)
{
	/* TypeSafe takes assets only behind this header. */
	static const char *const image_headers[] = {
		"TypeSafe-Danger-Zone-Images: true", NULL
	};

	return images ? image_headers : NULL;
}

fy_generic fyai_systemone_endpoints(struct fyai_cfg *cfg,
				    struct fy_generic_builder *gb)
{
	fy_generic rows = fyai_deciders_rows(cfg, gb), row, decider, model;
	fy_generic provider, url, nm, list = fy_sequence(gb);
	const char *wire, *why;

	fy_foreach(row, rows) {
		nm = fy_get(row, "name", fy_invalid);
		if (!fy_is_string(nm))
			continue;
		decider = fyai_decider_get(cfg, gb, fy_castp(&nm, ""), &why);
		model = fy_get(decider, "model", fy_invalid);
		if (!fy_is_string(model))
			continue;
		provider = fyai_decider_systemone(cfg, gb, fy_castp(&model, ""), &wire);
		if (!fy_is_mapping(provider))
			continue;
		url = fy_get(decider, "url", fy_invalid);
		if (!fy_is_string(url))
			url = fy_get(provider, "url", fy_invalid);
		if (!fy_is_string(url))
			continue;
		list = fy_append(gb, list, fy_mapping(gb, "url", url,
			"credential", fyai_systemone_credential(gb, provider)));
	}
	return list;
}

static size_t systemone_tp_body(const void *data, size_t len, void *ud)
{
	struct systemone_xfer *x = ud;

	return write_response((void *)data, 1, len, &x->response);
}

static void systemone_tp_start(struct fyai_tcall *call,
			       const struct fyai_tcall_start *st, void *ud)
{
	struct systemone_xfer *x = ud;

	(void)call;
	x->status = st->status;
}

static void systemone_tp_end(struct fyai_tcall *call,
			     const struct fyai_tcall_result *r, void *ud)
{
	struct systemone_xfer *x = ud;

	(void)call;
	switch (r->end) {
	case FYAI_TC_OK:
		x->code = CURLE_OK;
		break;
	case FYAI_TC_FAILED:
		x->code = (CURLcode)r->code;
		break;
	case FYAI_TC_CANCELLED:
		x->code = CURLE_ABORTED_BY_CALLBACK;
		break;
	case FYAI_TC_ABORTED:
		x->code = CURLE_WRITE_ERROR;
		break;
	case FYAI_TC_REFUSED:
	case FYAI_TC_LOST:
	default:
		/* Nothing retries a refused request or a lost transport. */
		x->code = CURLE_FAILED_INIT;
		break;
	}
	snprintf(x->message, sizeof(x->message), "%s", r->message);
	x->done = true;
}

/* One attempt through the credential transport; as systemone_attempt(). */
static int systemone_attempt_tp(struct fyai_ctx *ctx, struct systemone_xfer *x,
				const char *profile, const char *body)
{
	struct fyai_event_loop *el = fyai_ctx_loop(ctx);
	struct fyai_transport_request rq = {
		.profile = profile,
		.method = "POST",
		.content_type = "application/json",
		.accept = "application/json",
		.timeout_ms = SYSTEMONE_TIMEOUT_S * 1000L,
		.body = (const uint8_t *)body,
		.body_len = strlen(body),
	};
	struct fyai_tcall_callbacks cb = {
		.start = systemone_tp_start, .body = systemone_tp_body,
		.end = systemone_tp_end, .userdata = x,
	};

	free(x->response.data);
	memset(&x->response, 0, sizeof(x->response));
	x->done = false;
	x->status = 0;
	x->code = CURLE_OK;
	x->message[0] = '\0';
	x->call = fyai_tclient_submit(ctx->transport->client, &rq, &cb);
	if (!x->call || !el)
		return -1;
	while (!x->done) {
		if (fyai_interrupt_pending(ctx)) {
			fyai_event_interrupt_ack(ctx->loop);
			fyai_tcall_cancel(x->call);
		}
		if (fyai_event_loop_step(el, -1) < 0) {
			fyai_tcall_cancel(x->call);
			while (!x->done)
				if (fyai_event_loop_step(el, -1) < 0)
					break;
			fyai_tcall_destroy(x->call);
			x->call = NULL;
			return -1;
		}
	}
	fyai_tcall_destroy(x->call);
	x->call = NULL;
	return 0;
}

/* One attempt; 0 when it ran to an answer or a failure, -1 when it could not. */
static int systemone_attempt(struct fyai_ctx *ctx, struct systemone_xfer *x,
			     const char *url, struct curl_slist *headers,
			     const char *body)
{
	struct fyai_event_loop *el = fyai_ctx_loop(ctx);

	free(x->response.data);
	memset(&x->response, 0, sizeof(x->response));
	x->done = false;
	x->status = 0;
	x->code = CURLE_OK;
	curl_easy_reset(x->easy);
	/* Keep SIGALRM for the watchdog. */
	curl_easy_setopt(x->easy, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(x->easy, CURLOPT_URL, url);
	curl_easy_setopt(x->easy, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(x->easy, CURLOPT_POSTFIELDS, body);
	curl_easy_setopt(x->easy, CURLOPT_WRITEFUNCTION, write_response);
	curl_easy_setopt(x->easy, CURLOPT_WRITEDATA, &x->response);
	curl_easy_setopt(x->easy, CURLOPT_TIMEOUT, (long)SYSTEMONE_TIMEOUT_S);
	curl_easy_setopt(x->easy, CURLOPT_USERAGENT, ctx->link->user_agent);
	x->transfer = fyai_curl_submit(ctx->link, x->easy, systemone_complete,
				       x);
	if (!x->transfer || !el)
		return -1;
	while (!x->done) {
		if (fyai_interrupt_pending(ctx)) {
			fyai_event_interrupt_ack(ctx->loop);
			fyai_curl_cancel(x->transfer);
		}
		if (fyai_event_loop_step(el, -1) < 0) {
			fyai_curl_cancel(x->transfer);
			while (!x->done)
				if (fyai_event_loop_step(el, -1) < 0)
					break;
			return -1;
		}
	}
	return 0;
}

/* Wait @ms on the loop, so that its other work goes on. */
static void systemone_pause(struct fyai_ctx *ctx, fyai_event_ms_t ms)
{
	struct fyai_event_loop *el = fyai_ctx_loop(ctx);
	fyai_event_ms_t end = fyai_event_now_ms() + ms, now;

	while (el && !fyai_interrupt_pending(ctx) &&
	       (now = fyai_event_now_ms()) < end)
		if (fyai_event_loop_step(el, end - now) < 0)
			break;
}

/* The answers of a parsed reply, or fy_invalid with the reason. */
static fy_generic systemone_answers(struct fy_generic_builder *gb,
				    fy_generic provider, fy_generic doc,
				    const char **whyp)
{
	fy_generic envelope = fy_get(provider, "envelope", fy_invalid);
	fy_generic code, msg, answers;

	if (fy_is_string(envelope)) {
		code = fy_get(doc, "code", fy_invalid);
		if (fy_is_valid(code) && !fy_equal(code, 0)) {
			msg = fy_get(doc, "message", fy_invalid);
			*whyp = fy_gb_intern_string(gb, fy_sprintfa(
				"the provider failed: %s",
				fy_is_string(msg) ? fy_castp(&msg, "") :
				"no reason"));
			return fy_invalid;
		}
		doc = fy_get_at_pathstr(gb, doc, fy_castp(&envelope, ""));
	}
	answers = fy_get(doc, "answers", fy_invalid);
	if (!fy_is_mapping(answers)) {
		*whyp = "the answer has no answers";
		return fy_invalid;
	}
	return answers;
}

fy_generic fyai_systemone_ask(struct fyai_ctx *ctx,
			      struct fy_generic_builder *gb,
			      fy_generic provider, const char *url,
			      const char *key, fy_generic request,
			      const char **whyp)
{
	struct systemone_xfer x = {0};
	struct curl_slist *headers = NULL;
	fy_generic purl, doc, result = fy_invalid;
	const char *body;
	int attempt, attempts = ctx->cfg->retry_max_attempts;
	bool isolated = fyai_transport_supervised();
	bool images = fy_is_mapping(fy_get(request, "assets", fy_invalid));
	const char *const *image_headers = fyai_systemone_headers(true);
	char profile[FYAI_TPC_NAME_MAX];
	const char *cred;

	*whyp = NULL;
	purl = fy_get(provider, "url", fy_invalid);
	if (!url || !*url)
		url = fy_is_string(purl) ?
		      fy_gb_intern_string(gb, fy_castp(&purl, "")) : NULL;
	if (!url) {
		*whyp = "the provider has no URL";
		return fy_invalid;
	}
	body = emit_request_body(gb, request);
	if (!body) {
		*whyp = "cannot encode the request";
		return fy_invalid;
	}
	if (isolated) {
		/* The transport adds the key; no header carries it here. */
		cred = fyai_systemone_credential(gb, provider);
		if (fyai_transport_ensure_url(ctx, url, cred,
				fyai_systemone_headers(images), images,
				profile, sizeof(profile))) {
			*whyp = "cannot state the request to the transport";
			goto out;
		}
	} else if (append_header(&headers, "Content-Type: application/json") ||
	    append_header(&headers, "Accept: application/json") ||
	    (key && append_header(&headers, fy_sprintfa(
			"Authorization: Bearer %s", key))) ||
	    (images && append_header(&headers, image_headers[0]))) {
		*whyp = "out of memory";
		goto out;
	}
	x.easy = isolated ? NULL : curl_easy_init();
	if (!isolated && !x.easy) {
		*whyp = "cannot make a curl handle";
		goto out;
	}
	if (attempts < 1)
		attempts = 1;
	for (attempt = 0; attempt < attempts; attempt++) {
		if (attempt)
			systemone_pause(ctx, fyai_retry_delay_ms(ctx->cfg,
							attempt - 1, 0));
		if (fyai_interrupt_pending(ctx)) {
			*whyp = "interrupted";
			goto out;
		}
		if (isolated ? systemone_attempt_tp(ctx, &x, profile, body) :
		    systemone_attempt(ctx, &x, url, headers, body)) {
			*whyp = "cannot send the request";
			goto out;
		}
		if (x.code == CURLE_OK && x.status >= 200 && x.status < 300)
			break;
		if (x.code == CURLE_ABORTED_BY_CALLBACK) {
			*whyp = "interrupted";
			goto out;
		}
		if (!fyai_http_transient(x.code, x.status))
			break;
	}
	if (x.code != CURLE_OK) {
		*whyp = fy_gb_intern_string(gb, fy_sprintfa(
			"the request failed: %s", *x.message ? x.message :
			curl_easy_strerror(x.code)));
		goto out;
	}
	if (x.status < 200 || x.status >= 300) {
		*whyp = fy_gb_intern_string(gb, fy_sprintfa(
			"the request returned HTTP %ld%s%.400s", x.status,
			x.response.data ? ": " : "",
			x.response.data ? x.response.data : ""));
		goto out;
	}
	doc = x.response.data ? parse_json_string(gb, x.response.data) :
	      fy_invalid;
	if (!fy_is_mapping(doc)) {
		*whyp = "the answer is not JSON";
		goto out;
	}
	result = systemone_answers(gb, provider, doc, whyp);
out:
	if (x.easy)
		curl_easy_cleanup(x.easy);
	curl_slist_free_all(headers);
	free(x.response.data);
	return result;
}
