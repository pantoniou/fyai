/*
 * fyai_retry_test.c - unit tests for the provider retry backoff
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * The schedule is what keeps a refused request from coming straight back.
 * These cases pin its shape: the delay doubles, it stops at the ceiling, it
 * keeps a random part, and a Retry-After from the server replaces it.
 */

/* Diagnostics raised from this file are the test harness's own. */
#define FYAI_MODULE FYAIEM_UNKNOWN

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fyai.h"
#include "fyai_curl.h"
#include "fyai_provider.h"
#include "fyai_stream.h"
#include "fyai_test.h"

#include "fyai_test_registry.h"

FYAI_TEST_ENTRY(retry, backoff_doubles, retry_backoff_doubles)
FYAI_TEST_ENTRY(retry, backoff_caps, retry_backoff_caps)
FYAI_TEST_ENTRY(retry, backoff_jitters, retry_backoff_jitters)
FYAI_TEST_ENTRY(retry, retry_after_wins, retry_retry_after_wins)
FYAI_TEST_ENTRY(retry, transient_selection, retry_transient_selection)
FYAI_TEST_ENTRY(retry, error_object_selection, retry_error_object_selection)

/* Draws needed before a spread is certain enough to assert on. */
#define RETRY_DRAWS 64

static struct fyai_cfg retry_cfg;

static void retry_cfg_open(void)
{
	memset(&retry_cfg, 0, sizeof(retry_cfg));
	retry_cfg.retry_max_attempts = 4;
	retry_cfg.retry_initial_delay_ms = 1000;
	retry_cfg.retry_max_delay_ms = 30000;
}

/*
 * Each attempt waits about twice as long as the one before it. Half of the
 * delay is fixed and half is random, so assert the band, not one value.
 */
int retry_backoff_doubles(void)
{
	fyai_event_ms_t delay, base;
	int attempt;
	int i;

	retry_cfg_open();
	for (attempt = 1; attempt <= 4; attempt++) {
		base = (fyai_event_ms_t)1000 << (attempt - 1);
		for (i = 0; i < RETRY_DRAWS; i++) {
			delay = fyai_retry_delay_ms(&retry_cfg, attempt, 0);
			FYAI_TCHECK(delay >= base / 2);
			FYAI_TCHECK(delay <= base);
		}
	}
	return 0;
}

/* The ceiling bounds the wait however many attempts were made. */
int retry_backoff_caps(void)
{
	fyai_event_ms_t delay;
	int attempt;

	retry_cfg_open();
	for (attempt = 1; attempt <= 40; attempt++) {
		delay = fyai_retry_delay_ms(&retry_cfg, attempt, 0);
		FYAI_TCHECK(delay >= 0);
		FYAI_TCHECK(delay <= retry_cfg.retry_max_delay_ms);
	}
	return 0;
}

/*
 * Two requests a provider refused together must not return together. A fixed
 * delay would make every caller come back at the same moment.
 */
int retry_backoff_jitters(void)
{
	fyai_event_ms_t first, delay;
	bool differs = false;
	int i;

	retry_cfg_open();
	first = fyai_retry_delay_ms(&retry_cfg, 3, 0);
	for (i = 0; i < RETRY_DRAWS; i++) {
		delay = fyai_retry_delay_ms(&retry_cfg, 3, 0);
		if (delay != first) {
			differs = true;
			break;
		}
	}
	FYAI_TCHECK(differs);
	return 0;
}

/* The server knows when it is ready; its answer replaces the calculation, but
 * it cannot hold a turn beyond the ceiling. */
int retry_retry_after_wins(void)
{
	retry_cfg_open();
	FYAI_TCHECK(fyai_retry_delay_ms(&retry_cfg, 1, 5) == 5000);
	/* A long Retry-After is still bounded by max_delay_ms. */
	FYAI_TCHECK(fyai_retry_delay_ms(&retry_cfg, 1, 600) == 30000);
	/* Attempt number does not matter once the server has answered. */
	FYAI_TCHECK(fyai_retry_delay_ms(&retry_cfg, 4, 2) == 2000);
	return 0;
}

/* Only a condition that can pass on its own is worth repeating. */
int retry_transient_selection(void)
{
	FYAI_TCHECK(fyai_http_transient(CURLE_OK, 429));
	FYAI_TCHECK(fyai_http_transient(CURLE_OK, 503));
	FYAI_TCHECK(fyai_http_transient(CURLE_OK, 408));
	FYAI_TCHECK(fyai_http_transient(CURLE_OPERATION_TIMEDOUT, 0));
	FYAI_TCHECK(fyai_http_transient(CURLE_COULDNT_CONNECT, 0));
	/* The provider's own answer: repeating it asks the same question. */
	FYAI_TCHECK(!fyai_http_transient(CURLE_OK, 400));
	FYAI_TCHECK(!fyai_http_transient(CURLE_OK, 401));
	FYAI_TCHECK(!fyai_http_transient(CURLE_OK, 404));
	FYAI_TCHECK(!fyai_http_transient(CURLE_OK, 200));
	return 0;
}

/*
 * A streamed rate limit arrives with HTTP 200, so the decision has to come
 * from the error object. Providers name the same condition differently, and
 * both a `code` and a `type` field carry it.
 */
int retry_error_object_selection(void)
{
	FYAI_TCHECK(fyai_provider_error_transient(
		fy_mapping("code", "rate_limit_exceeded")));
	FYAI_TCHECK(fyai_provider_error_transient(
		fy_mapping("type", "rate_limit_error")));
	FYAI_TCHECK(fyai_provider_error_transient(
		fy_mapping("type", "overloaded_error")));
	FYAI_TCHECK(fyai_provider_error_transient(
		fy_mapping("code", "server_error")));
	/*
	 * A spent quota reads like a rate limit and is not one: it does not
	 * refill while a backoff runs.
	 */
	FYAI_TCHECK(!fyai_provider_error_transient(
		fy_mapping("code", "insufficient_quota")));
	FYAI_TCHECK(!fyai_provider_error_transient(
		fy_mapping("type", "invalid_request_error")));
	FYAI_TCHECK(!fyai_provider_error_transient(fy_map_empty));
	FYAI_TCHECK(!fyai_provider_error_transient(fy_invalid));
	return 0;
}
