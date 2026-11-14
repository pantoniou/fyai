/*
 * fyai_mcp_oauth.h - the token refresh of an MCP server that uses OAuth
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis@konsulko.com>
 * SPDX-License-Identifier: MIT
 *
 * One refresh takes the store lock, reads the record of the server again, and
 * decides: another process that refreshed meanwhile has replaced the token, so
 * the request adopts that token and sends nothing; else it sends the refresh
 * token that the store holds. The answer is saved under the lock before the
 * lock is released. The request owns copies of everything it needs, so it can
 * finish after its owner has gone. The discovery, the registration and the
 * browser authorization stay in fyai_mcp.c.
 */

#ifndef FYAI_MCP_OAUTH_H
#define FYAI_MCP_OAUTH_H

#include <stdbool.h>
#include <time.h>

struct fyai_ctx;
struct fyai_mcp_oauth_refresh;

enum fyai_mcp_oauth_client_auth {
	FYAI_MCP_CLIENT_NONE,
	FYAI_MCP_CLIENT_POST,	/* client_secret in the body */
	FYAI_MCP_CLIENT_BASIC,	/* HTTP basic */
};

/* What a refresh needs. The request copies every string. */
struct fyai_mcp_oauth_spec {
	const char *server;		/* names the record of the store */
	const char *issuer;
	const char *resource;
	const char *token_endpoint;
	const char *client_id;
	const char *client_secret;	/* sent to the endpoint; NULL for none */
	const char *store_secret;	/* kept in the record; "" when configured */
	const char *scopes;
	const char *access_token;	/* in memory: the token that was refused */
	const char *refresh_token;	/* in memory, replaced by the stored one */
	enum fyai_mcp_oauth_client_auth auth;
	long timeout;			/* seconds */
};

enum fyai_mcp_oauth_outcome {
	FYAI_MCP_OAUTH_REFRESHED,	/* the endpoint gave new tokens */
	FYAI_MCP_OAUTH_ADOPTED,		/* another process had renewed them */
	FYAI_MCP_OAUTH_FAILED,
	FYAI_MCP_OAUTH_CANCELLED,
};

/*
 * The tokens are valid during the call only; the request clears them after it.
 * @message, for FAILED, names the cause.
 */
struct fyai_mcp_oauth_result {
	enum fyai_mcp_oauth_outcome outcome;
	const char *access_token;
	const char *refresh_token;
	time_t expires_at;
	const char *message;
};

typedef void (*fyai_mcp_oauth_done_fn)(struct fyai_mcp_oauth_refresh *request,
				       const struct fyai_mcp_oauth_result *result,
				       void *userdata);

/*
 * Start a refresh. @done runs once, from the event loop, never from this
 * call. Returns NULL, with a diagnostic, when it cannot start.
 */
struct fyai_mcp_oauth_refresh *
fyai_mcp_oauth_refresh_submit(struct fyai_ctx *ctx,
			      const struct fyai_mcp_oauth_spec *spec,
			      fyai_mcp_oauth_done_fn done, void *userdata);

/*
 * Stop waiting. A refresh that waits for the lock ends. A refresh whose token
 * request is out goes on and saves its answer, because the endpoint can have
 * rotated the refresh token; the owner gets CANCELLED at once.
 */
void fyai_mcp_oauth_refresh_cancel(struct fyai_mcp_oauth_refresh *request);

/*
 * Free the request. A request with a token request out and no owner left ends
 * by itself. Use fyai_mcp_oauth_refresh_abort() when the record must stay as
 * it is, as a logout does.
 */
void fyai_mcp_oauth_refresh_destroy(struct fyai_mcp_oauth_refresh *request);

/* Free the request and drop the token request, whatever it holds. */
void fyai_mcp_oauth_refresh_abort(struct fyai_mcp_oauth_refresh *request);

#endif
