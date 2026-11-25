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
#include "fyai_event.h"
#include "fyai_link.h"
#include "fyai_secret.h"
#include "fyai_stream.h"
#include "fyai_systemone.h"
#include "fyai_transport_boot.h"
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
	if (append_header(&headers, "Content-Type: application/json") ||
	    append_header(&headers, "Accept: application/json") ||
	    (key && append_header(&headers, fy_sprintfa(
			"Authorization: Bearer %s", key)))) {
		*whyp = "out of memory";
		goto out;
	}
	x.easy = curl_easy_init();
	if (!x.easy) {
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
		if (systemone_attempt(ctx, &x, url, headers, body)) {
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
			"the request failed: %s", curl_easy_strerror(x.code)));
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
