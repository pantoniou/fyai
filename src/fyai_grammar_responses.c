/*
 * fyai_grammar_responses.c - the OpenAI Responses grammar
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "fyai_grammar.h"

static const struct fyai_grammar_header responses_headers[] = {
	{ NULL, NULL },
};

const struct fyai_grammar fyai_grammar_responses = {
	.mode			= FYAI_API_RESPONSES,
	.name			= "responses",
	.protocol		= "responses",
	.title			= "Responses API",
	.default_url		= OPENAI_RESPONSES_URL,
	.default_model		= DEFAULT_OPENAI_MODEL,
	.default_provider	= "openai",
	.headers		= responses_headers,
	.reasoning		= true,
	.logprobs		= true,
	.builtin_shell		= true,
	.compaction		= true,
	.chatgpt		= true,
};
