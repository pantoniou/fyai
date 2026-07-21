/*
 * fyai_oauth.h - provider-agnostic OAuth 2.0 authorization-code mechanics
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_OAUTH_H
#define FYAI_OAUTH_H

#include <stdbool.h>
#include <stddef.h>

#include <curl/curl.h>

#include "fyai_event.h"

/* The parts of an authorization-code + PKCE login that do not depend on who
 * the provider is. */

struct fyai_ctx;

/* PKCE (RFC 7636) parameters plus the CSRF state, all base64url without
 * padding. */
struct fyai_oauth_pkce {
	char *verifier;		/* sent at token exchange */
	char *challenge;	/* S256(verifier), sent at authorize */
	char *state;		/* echoed back on the redirect */
};

int fyai_oauth_pkce_generate(struct fyai_oauth_pkce *p);
void fyai_oauth_pkce_cleanup(struct fyai_oauth_pkce *p);

/* Where a login has got to. */
enum fyai_oauth_state {
	FYAI_OAUTH_LISTENING = 0,	/* waiting for the browser redirect */
	FYAI_OAUTH_GOT_CODE,		/* code in hand; exchange it */
	FYAI_OAUTH_TIMED_OUT,		/* nobody arrived in time */
	FYAI_OAUTH_BAD_STATE,		/* a redirect with the wrong state */
	FYAI_OAUTH_FAILED,		/* the receiver itself failed */
};

bool fyai_oauth_state_is_terminal(enum fyai_oauth_state st);
const char *fyai_oauth_state_string(enum fyai_oauth_state st);

struct fyai_oauth_flow;

/* Called once, when the flow reaches a terminal state. */
typedef void (*fyai_oauth_done_cb)(struct fyai_oauth_flow *f, void *user);

struct fyai_oauth_params {
	const char *path;		/* redirect path, e.g. "/auth/callback" */
	const unsigned short *ports;	/* tried in order; first free wins */
	size_t nports;
	const char *state;		/* expected CSRF state; must outlive the flow */
	fyai_event_ms_t timeout_ms;	/* <= 0 for no timeout */
};

/* Arm the loopback receiver on @el and return immediately. */
int fyai_oauth_flow_start(struct fyai_ctx *ctx, struct fyai_event_loop *el,
			  const struct fyai_oauth_params *params,
			  fyai_oauth_done_cb cb, void *user,
			  struct fyai_oauth_flow **flowp);

enum fyai_oauth_state fyai_oauth_flow_state(const struct fyai_oauth_flow *f);

/* The port actually bound, for building the redirect URI. */
unsigned short fyai_oauth_flow_port(const struct fyai_oauth_flow *f);

/* The authorization code, valid once the state is GOT_CODE and until the flow
 * is destroyed. */
const char *fyai_oauth_flow_code(const struct fyai_oauth_flow *f);

/* Tell the browser how the login ended. */
void fyai_oauth_flow_finish(struct fyai_oauth_flow *f, bool ok);

void fyai_oauth_flow_destroy(struct fyai_oauth_flow *f);

/* The synchronous convenience form, for a caller with nothing else on this
 * loop. */
int fyai_oauth_flow_wait(struct fyai_ctx *ctx, struct fyai_oauth_flow *f);

/* Hand @url to the desktop browser; best effort, failure is not fatal. */
void fyai_oauth_open_browser(const char *url);

/* Percent-decoded value of @key in the query string of an HTTP request line. */
char *fyai_oauth_query_value(CURL *curl, const char *request, const char *key);

#endif
