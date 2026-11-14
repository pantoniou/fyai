/* SPDX-License-Identifier: MIT */
#ifndef FYAI_TEST_DIAG_H
#define FYAI_TEST_DIAG_H

#include <stdlib.h>

#include "fyai.h"
#include "fyai_diag.h"
#include "fyai_loop.h"

/*
 * A configuration that stands alone owns its diagnostics in a test: the sink
 * is made on the heap and the configuration points at it, as the program
 * points the configuration at the sink of struct fyai_app.
 */
static inline int fyai_test_diag_setup(struct fyai_cfg *cfg)
{
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
	free(cfg->diag);
	cfg->diag = NULL;
}

/*
 * The event loop component of a test context: the one that the test gave the
 * context, else one of its own, made on first use. It reports into the sink
 * of the configuration of the context.
 */
static inline struct fyai_loop *fyai_test_loop_of(struct fyai_ctx *ctx)
{
	static struct fyai_loop own;

	if (!ctx->loop) {
		fyai_loop_init(&own, NULL);
		ctx->loop = &own;
	}
	ctx->loop->diag = ctx->cfg ? ctx->cfg->diag : NULL;
	return ctx->loop;
}

#endif
