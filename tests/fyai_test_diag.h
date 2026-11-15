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
static inline void fyai_test_attach(struct fyai_ctx *ctx);

static inline struct fyai_loop *fyai_test_loop_of(struct fyai_ctx *ctx)
{
	if (!ctx->loop) {
		ctx->loop = calloc(1, sizeof(*ctx->loop));
		if (!ctx->loop)
			return NULL;
		fyai_loop_init(ctx->loop, fyai_cfg_diag(ctx->cfg));
	}
	fyai_test_attach(ctx);
	ctx->link->loop = ctx->loop;
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

/*
 * The MCP client component of a test context: the one that the test gave the
 * context, else one that the context holds.
 */
static inline struct fyai_mcp *fyai_test_mcp_of(struct fyai_ctx *ctx)
{
	if (!ctx->mcp) {
		ctx->mcp = calloc(1, sizeof(*ctx->mcp));
		if (!ctx->mcp)
			return NULL;
		fyai_mcp_state_init(ctx->mcp, ctx->loop, ctx->cred, ctx->transient);
	}
	return ctx->mcp;
}

/*
 * The waits component of a test context: the one that the test gave the
 * context, else one that the context holds.
 */
static inline struct fyai_waitq *fyai_test_waitq_of(struct fyai_ctx *ctx)
{
	if (!ctx->waitq) {
		ctx->waitq = calloc(1, sizeof(*ctx->waitq));
		if (!ctx->waitq)
			return NULL;
		fyai_waitq_init(ctx->waitq, ctx->loop, fyai_cfg_diag(ctx->cfg));
	}
	return ctx->waitq;
}

/*
 * The questions component of a test context: the one that the test gave the
 * context, else one that the context holds.
 */
static inline struct fyai_askq *fyai_test_askq_of(struct fyai_ctx *ctx)
{
	if (!ctx->askq) {
		ctx->askq = calloc(1, sizeof(*ctx->askq));
		if (!ctx->askq)
			return NULL;
		fyai_askq_init(ctx->askq, fyai_cfg_diag(ctx->cfg));
	}
	return ctx->askq;
}

/* The scope, terminal, skill files and tool specification components. */
static inline struct fyai_scope *fyai_test_scope_of(struct fyai_ctx *ctx)
{
	if (!ctx->scope) {
		ctx->scope = calloc(1, sizeof(*ctx->scope));
		if (!ctx->scope)
			return NULL;
		fyai_scope_state_init(ctx->scope, ctx->loop, fyai_cfg_diag(ctx->cfg), ctx->cfg);
	}
	return ctx->scope;
}

static inline struct fyai_tty *fyai_test_tty_of(struct fyai_ctx *ctx)
{
	if (!ctx->tty) {
		ctx->tty = calloc(1, sizeof(*ctx->tty));
		if (!ctx->tty)
			return NULL;
		fyai_tty_init(ctx->tty, ctx->loop, fyai_cfg_diag(ctx->cfg));
	}
	return ctx->tty;
}

static inline struct fyai_skills *fyai_test_skills_of(struct fyai_ctx *ctx)
{
	if (!ctx->skills) {
		ctx->skills = calloc(1, sizeof(*ctx->skills));
		if (!ctx->skills)
			return NULL;
		fyai_skills_init(ctx->skills, ctx->arena, ctx->transient);
	}
	return ctx->skills;
}

static inline struct fyai_toolspec *fyai_test_toolspec_of(struct fyai_ctx *ctx)
{
	if (!ctx->toolspec) {
		ctx->toolspec = calloc(1, sizeof(*ctx->toolspec));
		if (!ctx->toolspec)
			return NULL;
		fyai_toolspec_init(ctx->toolspec, fyai_test_skills_of(ctx));
	}
	return ctx->toolspec;
}

static inline struct fyai_branch_state *fyai_test_bstate_of(struct fyai_ctx *ctx)
{
	if (!ctx->bstate) {
		ctx->bstate = calloc(1, sizeof(*ctx->bstate));
		if (!ctx->bstate)
			return NULL;
		fyai_branch_state_init(ctx->bstate, ctx->arena, fyai_cfg_diag(ctx->cfg));
	}
	return ctx->bstate;
}

static inline struct fyai_usage *fyai_test_usage_of(struct fyai_ctx *ctx)
{
	if (!ctx->usage) {
		ctx->usage = calloc(1, sizeof(*ctx->usage));
		if (!ctx->usage)
			return NULL;
		fyai_usage_init(ctx->usage, fyai_cfg_diag(ctx->cfg));
	}
	return ctx->usage;
}

static inline struct fyai_turnstate *fyai_test_turn_of(struct fyai_ctx *ctx)
{
	if (!ctx->turn) {
		ctx->turn = calloc(1, sizeof(*ctx->turn));
		if (!ctx->turn)
			return NULL;
		fyai_turnstate_init(ctx->turn, fyai_cfg_diag(ctx->cfg));
	}
	return ctx->turn;
}

static inline struct fyai_jobs *fyai_test_jobs_of(struct fyai_ctx *ctx)
{
	if (!ctx->jobs) {
		ctx->jobs = calloc(1, sizeof(*ctx->jobs));
		if (!ctx->jobs)
			return NULL;
		fyai_jobs_init(ctx->jobs, ctx->loop, fyai_cfg_diag(ctx->cfg));
	}
	return ctx->jobs;
}

static inline struct fyai_agent_state *fyai_test_astate_of(struct fyai_ctx *ctx)
{
	if (!ctx->astate) {
		ctx->astate = calloc(1, sizeof(*ctx->astate));
		if (!ctx->astate)
			return NULL;
		fyai_agent_state_init(ctx->astate, ctx->bstate, fyai_cfg_diag(ctx->cfg));
	}
	return ctx->astate;
}

static inline struct fyai_front *fyai_test_front_of(struct fyai_ctx *ctx)
{
	if (!ctx->front) {
		ctx->front = calloc(1, sizeof(*ctx->front));
		if (!ctx->front)
			return NULL;
		fyai_front_init(ctx->front, ctx->loop, fyai_cfg_diag(ctx->cfg));
	}
	return ctx->front;
}

/*
 * Give a test context every component that it lacks. The ones that the test
 * gave it stay.
 */
static inline void fyai_test_attach(struct fyai_ctx *ctx)
{
	fyai_test_link_of(ctx);
	fyai_test_transport_of(ctx);
	fyai_test_mcp_of(ctx);
	fyai_test_waitq_of(ctx);
	fyai_test_askq_of(ctx);
	fyai_test_scope_of(ctx);
	fyai_test_tty_of(ctx);
	fyai_test_toolspec_of(ctx);
	fyai_test_bstate_of(ctx);
	fyai_test_usage_of(ctx);
	fyai_test_turn_of(ctx);
	fyai_test_jobs_of(ctx);
	fyai_test_astate_of(ctx);
	fyai_test_front_of(ctx);
}

#endif
