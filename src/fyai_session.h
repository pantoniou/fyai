/*
 * fyai_session.h - session commands (/clear, /compact, /model, /context)
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_SESSION_H
#define FYAI_SESSION_H

#include "fyai.h"

/*
 * Shared backends for the interactive slash commands and their CLI verb
 * forms (fyai clear|compact|context). Each performs one session operation
 * against the open ctx; persistence goes through the usual publish path
 * (skipped under --transient).
 */

/* Reset the conversation: head -> null (durable), fresh system turn when a
 * request session is live. */
int fyai_session_clear(struct fyai_ctx *ctx);

/* Summarize the history with one model call and restart the chain from the
 * summary; @hint optionally focuses the summary. Needs request credentials. */
int fyai_session_compact(struct fyai_ctx *ctx, const char *hint);

/* Return true when compaction uses a Responses input trigger. */
bool fyai_session_compact_v2(const struct fyai_cfg *cfg);

/* Print the current model (@name NULL/empty) or switch to @name
 * mid-session: re-resolve against the catalogue and rebuild request state. */
int fyai_session_model(struct fyai_ctx *ctx, const char *name);

/* Print the current API grammar (@arg NULL/empty) or switch to it
 * mid-session: re-resolve the same provider's endpoint for the new grammar
 * and rebuild request state. */
int fyai_session_api(struct fyai_ctx *ctx, const char *arg);

/* Report projected next-request context fill and its prompt/output parts. */
int fyai_session_context(struct fyai_ctx *ctx);

/* Return the active context window, or zero if it is not specified. */
long long fyai_context_window(struct fyai_ctx *ctx);

/* Return the projected prompt and output size of the next request. */
long long fyai_context_projected(struct fyai_ctx *ctx);

/* As above, measuring the turn chain that ends at @head. */
long long fyai_context_projected_at(struct fyai_ctx *ctx, fy_generic head);

/* Where a measured prompt size came from. */
enum fyai_context_source {
	FYAICS_NONE,		/* nothing measured yet */
	FYAICS_LAST_CALL,	/* provider usage from a call in this process */
	FYAICS_STORED,		/* provider usage stored on the turn chain */
};

/* Prompt sizes from provider usage and the current byte estimate. */
struct fyai_context_prompt {
	long long measured;
	long long estimated;
	long long prompt;
	enum fyai_context_source source;
	bool from_estimate;
};

/* Fill @out for the turn chain ending at @head. */
void fyai_context_prompt_at(struct fyai_ctx *ctx, fy_generic head,
			    struct fyai_context_prompt *out);

/* Human-readable name for @source ("last call", "stored", "none"). */
const char *fyai_context_source_name(enum fyai_context_source source);

/* Limit the configured output allowance to the available context space. */
long long fyai_context_output_tokens(struct fyai_ctx *ctx, long long prompt,
				     long long window);

/* Overview: model/provider selection, request shaping, auth, token usage. */
int fyai_session_status(struct fyai_ctx *ctx);

/*
 * Dispatch one REPL line starting with '/'. Returns 1 when the session
 * should end (/exit, /quit), 0 otherwise (handled, even on error - the
 * line never reaches the model).
 */
int fyai_session_slash(struct fyai_ctx *ctx, const char *line);

/*
 * Refresh the interactive footer with the session
 * settings: model, provider, api grammar, effort/summary or temperature, and
 * the context fill. No-op outside an interactive markdown tty session.
 */
void fyai_session_banner_update(struct fyai_ctx *ctx);

/* Read one edited line through the active frontend. */
char *fyai_readline(struct fyai_ctx *ctx, const char *prompt);

struct fytim_completions;
void fyai_session_completion(struct fyai_ctx *ctx, const char *buf,
			     struct fytim_completions *comps);

#endif
