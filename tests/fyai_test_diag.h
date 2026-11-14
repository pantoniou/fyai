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
 * is static and the configuration points at it, as the program points the
 * configuration at the sink of struct fyai_app.
 */
static inline struct fyai_diag *fyai_test_diag_storage(void)
{
	/* Static, so a loop that kept the pointer never holds freed memory. */
	static struct fyai_diag diag;

	return &diag;
}

static inline int fyai_test_diag_setup(struct fyai_cfg *cfg)
{
	cfg->diag = fyai_test_diag_storage();
	return fyai_diag_setup(cfg->diag);
}

static inline void fyai_test_diag_cleanup(struct fyai_cfg *cfg)
{
	if (!cfg->diag)
		return;
	fyai_diag_cleanup(cfg->diag);
	cfg->diag = NULL;
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
 * The event loop component of a test context: the one that the test gave the
 * context, else one of its own, made on first use. A loop without a sink
 * reports into the sink of the tests.
 */
static inline struct fyai_loop *fyai_test_loop_of(struct fyai_ctx *ctx)
{
	static struct fyai_loop own;
	static bool own_ready;

	if (!ctx->loop) {
		if (!own_ready) {
			fyai_loop_init(&own, fyai_test_diag_storage());
			own_ready = true;
		}
		ctx->loop = &own;
	}
	fyai_test_link_of(ctx)->loop = ctx->loop;
	/* A loop that a test declared itself starts with no sink. */
	if (!ctx->loop->diag)
		ctx->loop->diag = fyai_test_diag_storage();
	return ctx->loop;
}

/*
 * The arena component of a test context: the one that the test gave the
 * context, else one of its own, made on first use.
 */
static inline struct fyai_arena *fyai_test_arena_of(struct fyai_ctx *ctx)
{
	static struct fyai_arena own;

	if (!ctx->arena) {
		fyai_arena_init(&own);
		ctx->arena = &own;
	}
	return ctx->arena;
}

/*
 * The transient builder component of a test context: the one that the test
 * gave the context, else one of its own, made on first use. It stacks on the
 * working builder of the context.
 */
static inline struct fyai_transient *fyai_test_transient_of(struct fyai_ctx *ctx)
{
	static struct fyai_transient own;

	if (!ctx->transient) {
		fyai_transient_init(&own);
		own.parent_gb = &fyai_test_arena_of(ctx)->gb;
		ctx->transient = &own;
	}
	return ctx->transient;
}

#endif
