/*
 * fyai_link.h - the model connection component
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_LINK_H
#define FYAI_LINK_H

#include <curl/curl.h>

struct fyai_cred;
struct fyai_diag;
struct fyai_curl_state;
struct fyai_loop;
struct fyai_request_cache;

/*
 * What a model request needs on the wire: the curl handle and its headers,
 * the transfer in progress and the outcome of the last one, the curl multi
 * state, and the body of the last request kept for the next. The component
 * stands on the loop for its transfers and on the credential component for
 * the login. The owner is struct fyai_app.
 */
struct fyai_link {
	/* Dependencies. */
	struct fyai_loop *loop;
	struct fyai_cred *cred;
	struct fyai_diag *diag;

	CURL *curl;
	struct curl_slist *headers;
	char *auth_header;
	char *user_agent;
	/* The curl multi state of this run; it stands on the loop. */
	struct fyai_curl_state *curl_state;
	/* The body of the last request, kept for the next (fyai_reqcache.c). */
	struct fyai_request_cache *request_cache;
	/* The model transfer in progress. */
	const char *body;
	const char *url;		/* NULL: the endpoint of the configuration */
	const char *profile;		/* NULL: "model" */
	long status;			/* of the last transfer that finished */
	long retry_after;
	char error[256];
};

void fyai_link_init(struct fyai_link *link, struct fyai_loop *loop,
		    struct fyai_cred *cred, struct fyai_diag *diag);
/* The child starts with no outcome; the endpoint and profile stay. */
void fyai_link_abandon(struct fyai_link *link);

#endif
