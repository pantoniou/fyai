/* SPDX-License-Identifier: MIT */
#ifndef FYAI_TEST_DIAG_H
#define FYAI_TEST_DIAG_H

#include <stdbool.h>
#include <stdlib.h>

#include "fyai.h"
#include "fyai_diag.h"
#include "fyai_arena.h"
#include "fyai_link.h"
#include "fyai_loop.h"
#include "fyai_transient.h"

/*
 * A configuration that stands alone owns its diagnostics in a test: the sink
 * is allocated for it and stays allocated, so a loop that kept the pointer
 * never holds freed memory. The configuration keeps it reachable.
 */
static inline int fyai_test_diag_setup(struct fyai_cfg *cfg)
{
	if (!cfg->diag)
		cfg->diag = calloc(1, sizeof(*cfg->diag));
	if (!cfg->diag)
		return -1;
	return fyai_diag_setup(cfg->diag);
}

static inline void fyai_test_diag_cleanup(struct fyai_cfg *cfg)
{
	if (!cfg->diag)
		return;
	fyai_diag_cleanup(cfg->diag);
}

/*
 * The connection component of a test context: the one that the test gave the
 * context, else one that the context holds, so it stays reachable until the
 * process ends.
 */
static inline struct fyai_link *fyai_test_link_of(struct fyai_ctx *ctx)
{
	if (!ctx->link)
		ctx->link = calloc(1, sizeof(*ctx->link));
	return ctx->link;
}

/*
 * The credential transport component of a test context: the one that the test
 * gave the context, else one that the context holds.
 */
static inline struct fyai_transport_state *fyai_test_transport_of(struct fyai_ctx *ctx)
{
	if (!ctx->transport) {
		ctx->transport = calloc(1, sizeof(*ctx->transport));
		if (!ctx->transport)
			return NULL;
		fyai_transport_state_init(ctx->transport, ctx->loop, ctx->cred);
	}
	return ctx->transport;
}

/*
 * The event loop component of a test context: the one that the test gave the
 * context, else one that the context holds, made on first use. A loop that
 * has no sink reports into the sink of the configuration, which is muted
 * when it has none.
 */
static inline struct fyai_loop *fyai_test_loop_of(struct fyai_ctx *ctx)
{
	if (!ctx->loop) {
		ctx->loop = calloc(1, sizeof(*ctx->loop));
		if (!ctx->loop)
			return NULL;
		fyai_loop_init(ctx->loop, fyai_cfg_diag(ctx->cfg));
	}
	fyai_test_link_of(ctx)->loop = ctx->loop;
	fyai_test_transport_of(ctx);
	/* A loop that a test declared itself starts with no sink. */
	if (!ctx->loop->diag)
		ctx->loop->diag = fyai_cfg_diag(ctx->cfg);
	return ctx->loop;
}

/*
 * The arena component of a test context: the one that the test gave the
 * context, else one that the context holds, made on first use.
 */
static inline struct fyai_arena *fyai_test_arena_of(struct fyai_ctx *ctx)
{
	if (!ctx->arena) {
		ctx->arena = calloc(1, sizeof(*ctx->arena));
		if (!ctx->arena)
			return NULL;
		fyai_arena_init(ctx->arena);
	}
	return ctx->arena;
}

/*
 * The transient builder component of a test context: the one that the test
 * gave the context, else one that the context holds. It stacks on the
 * working builder of the context.
 */
static inline struct fyai_transient *fyai_test_transient_of(struct fyai_ctx *ctx)
{
	if (!ctx->transient) {
		ctx->transient = calloc(1, sizeof(*ctx->transient));
		if (!ctx->transient)
			return NULL;
		fyai_transient_init(ctx->transient);
		ctx->transient->parent_gb = &fyai_test_arena_of(ctx)->gb;
	}
	return ctx->transient;
}

#endif
