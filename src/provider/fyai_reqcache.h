/* SPDX-License-Identifier: MIT */
/*
 * fyai_reqcache.h - the request body of the last request, kept for the next one
 *
 * A stateless request carries the whole conversation, and the conversation
 * grows with each turn, so making the body costs more with each request. The
 * stored turns never change. The cache keeps the text of the part of the
 * input that the next request repeats, and a
 * request makes only the text of the turns that came after it: the body is the
 * text before the input, the kept text, the new text, and the text after the
 * input.
 *
 * What the grammar can say about its input decides how much is kept
 * (`input_split` of struct fyai_grammar). A request that the cache cannot use
 * is made as it always was.
 */
#ifndef FYAI_REQCACHE_H
#define FYAI_REQCACHE_H

#include <stdbool.h>
#include <stddef.h>

#include <libfyaml.h>
#include <libfyaml/libfyaml-generic.h>

struct fyai_ctx;
struct fyai_grammar_request;

/*
 * True when a request of @ctx can use the cache: the grammar splits its input,
 * the configuration allows it, the request does not continue a response, and
 * nothing needs the request as a value (debug and conversation logging, a
 * subscription request).
 */
bool fyai_request_cache_usable(struct fyai_ctx *ctx, fy_generic previous);

/*
 * Make the body of the request that continues the conversation at @turn.
 * @req holds every field of the request but the messages, the input and the
 * instructions. Returns 1 and the body in *bodyp, 0 when the cache does not
 * apply and the caller makes the body, and -1 with the cause reported. The
 * body belongs to the cache and stays valid until the next call.
 */
int fyai_request_cache_body(struct fyai_ctx *ctx, fy_generic turn,
			    struct fyai_grammar_request *req,
			    const char **bodyp);

/*
 * The transient builder that holds the values of the requests is going away.
 * Forget the text that its words made. Keep the text of the conversation when
 * the arena holds the turn that it ends at: the next exchange continues it.
 */
void fyai_request_cache_forget(struct fyai_ctx *ctx);

/* Forget the cached text, such as when the arena closes or the process forks. */
void fyai_request_cache_release(struct fyai_ctx *ctx);

/* How many requests used the cached text, and how many did not. */
void fyai_request_cache_stats(const struct fyai_ctx *ctx,
			      unsigned long *hits, unsigned long *misses);

#endif
