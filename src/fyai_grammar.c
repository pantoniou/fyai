/*
 * fyai_grammar.c - the lookup of the provider API grammars
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "fyai_grammar.h"
#include "fyai_tools.h"

/* Indexed by enum fyai_api_mode; the order is the order of preference. */
const struct fyai_grammar *const fyai_grammars[] = {
	[FYAI_API_RESPONSES]		= &fyai_grammar_responses,
	[FYAI_API_CHAT_COMPLETIONS]	= &fyai_grammar_chat,
	[FYAI_API_MESSAGES]		= &fyai_grammar_messages,
	[FYAI_API_COUNT]		= NULL,
};

const struct fyai_grammar *fyai_grammar_get(enum fyai_api_mode mode)
{
	if ((unsigned int)mode >= FYAI_API_COUNT)
		return fyai_grammars[FYAI_API_RESPONSES];
	return fyai_grammars[mode];
}

const struct fyai_grammar *fyai_grammar_find(const char *name)
{
	const struct fyai_grammar *const *gp;

	if (!name)
		return NULL;
	for (gp = fyai_grammars; *gp; gp++)
		if (!strcmp((*gp)->name, name) ||
		    ((*gp)->alias && !strcmp((*gp)->alias, name)))
			return *gp;
	return NULL;
}

fy_generic fyai_grammar_input(struct fyai_ctx *ctx, fy_generic messages)
{
	return fyai_grammar(ctx->cfg)->input(ctx, messages);
}

fy_generic fyai_grammar_make_tools(struct fyai_ctx *ctx)
{
	return fyai_grammar(ctx->cfg)->make_tools(ctx);
}

fy_generic fyai_grammar_tool_def(struct fyai_ctx *ctx, fy_generic spec)
{
	return fyai_grammar(ctx->cfg)->tool_def(ctx, spec);
}

fy_generic fyai_grammar_image_part(struct fyai_ctx *ctx, fy_generic part)
{
	return fyai_grammar(ctx->cfg)->image_part(ctx, part);
}

fy_generic fyai_grammar_build_request(struct fyai_ctx *ctx,
				      const struct fyai_grammar_request *req)
{
	return fyai_grammar(ctx->cfg)->build_request(ctx, req);
}

int fyai_grammar_stream_open(struct fyai_ctx *ctx, struct fyai_stream_parse *p)
{
	p->grammar = fyai_grammar(ctx->cfg);
	p->state = calloc(1, p->grammar->stream_state_size ?
			     p->grammar->stream_state_size : 1);
	if (!p->state)
		return -1;
	if (p->grammar->stream_init)
		p->grammar->stream_init(p);
	return 0;
}

void fyai_grammar_stream_close(struct fyai_stream_parse *p)
{
	free(p->state);
	p->state = NULL;
}

int fyai_grammar_stream_event(struct fyai_stream_parse *p, fy_generic event)
{
	return p->grammar->stream_event(p, event);
}

fy_generic fyai_grammar_stream_finish(struct fyai_stream_parse *p)
{
	return p->grammar->stream_finish(p);
}

fy_generic fyai_grammar_response(struct fyai_ctx *ctx, fy_generic doc)
{
	return fyai_grammar(ctx->cfg)->response(ctx, doc);
}

bool fyai_grammar_chain_miss(struct fyai_ctx *ctx, long status,
			     const char *body)
{
	const struct fyai_grammar *grammar = fyai_grammar(ctx->cfg);

	return ctx->response_chain_linked && grammar->chain_miss &&
	       grammar->chain_miss(status, body);
}

bool fyai_grammar_compacts(const struct fyai_cfg *cfg)
{
	return fyai_grammar(cfg)->compact_url != NULL;
}

bool fyai_grammar_compact_url(const struct fyai_cfg *cfg, char *out,
			      size_t size)
{
	const struct fyai_grammar *grammar = fyai_grammar(cfg);

	return grammar->compact_url && grammar->compact_url(cfg, out, size);
}

bool fyai_grammar_tools_wanted(struct fyai_ctx *ctx)
{
	return ctx->cfg->enable_tools || ctx->cfg->enable_builtin_shell ||
	       fyai_mcp_enabled(ctx);
}

bool fyai_grammar_reasoning_set(const struct fyai_cfg *cfg)
{
	return !fy_str_empty(cfg->reasoning_effort) ||
	       !fy_str_empty(cfg->reasoning_summary);
}

fy_generic fyai_grammar_add_stream(struct fyai_ctx *ctx, fy_generic request,
				   fy_generic options)
{
	struct fy_generic_builder *gb = ctx->transient_gb;

	if (!ctx->cfg->stream)
		return request;
	request = fy_assoc(gb, request, "stream", true);
	if (fy_len(options))
		request = fy_assoc(gb, request, "stream_options", options);
	return request;
}

const char *fyai_api_to_string(enum fyai_api_mode api)
{
	if ((unsigned int)api >= FYAI_API_COUNT)
		return NULL;
	return fyai_grammars[api]->name;
}
