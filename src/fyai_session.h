/*
 * fyai_session.h - session commands (/clear, /compact, /model, /context)
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FYAI_SESSION_H
#define FYAI_SESSION_H

#include <linenoise.h>

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
 * summary; @hint optionally focuses the summary. Needs an API key. */
int fyai_session_compact(struct fyai_ctx *ctx, const char *hint);

/* Print the current model (@name NULL/empty) or switch to @name
 * mid-session: re-resolve against the catalogue and rebuild request state. */
int fyai_session_model(struct fyai_ctx *ctx, const char *name);

/* Print the current API grammar (@arg NULL/empty) or switch to it
 * mid-session: re-resolve the same provider's endpoint for the new grammar
 * and rebuild request state. */
int fyai_session_api(struct fyai_ctx *ctx, const char *arg);

/* Report context fill: window, last-call tokens, next-request estimate. */
int fyai_session_context(struct fyai_ctx *ctx);

/* Overview: model/provider selection, request shaping, auth, token usage. */
int fyai_session_status(struct fyai_ctx *ctx);

/*
 * Dispatch one REPL line starting with '/'. Returns 1 when the session
 * should end (/exit, /quit), 0 otherwise (handled, even on error - the
 * line never reaches the model).
 */
int fyai_session_slash(struct fyai_ctx *ctx, const char *line);

/*
 * Refresh the REPL footer row (linenoise bottom info) with the session
 * settings: model, provider, api grammar, effort/summary or temperature, and
 * the context fill. No-op outside an interactive markdown tty session.
 */
void fyai_session_banner_update(struct fyai_ctx *ctx);

/* linenoise tab completion for slash commands and their values. */
void fyai_session_completion_init(struct fyai_ctx *ctx);
void fyai_session_completion(const char *buf, linenoiseCompletions *lc);

#endif
