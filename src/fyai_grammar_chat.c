/*
 * fyai_grammar_chat.c - the Chat Completions grammar
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "fyai_grammar.h"

static const struct fyai_grammar_header chat_headers[] = {
	{ NULL, NULL },
};

const struct fyai_grammar fyai_grammar_chat = {
	.mode			= FYAI_API_CHAT_COMPLETIONS,
	.name			= "chat-completions",
	.alias			= "chat",
	.protocol		= "chat_completions",
	.title			= "Chat Completions API",
	.default_url		= OPENAI_CHAT_COMPLETIONS_URL,
	.default_model		= DEFAULT_OPENAI_MODEL,
	.default_provider	= "openai",
	.headers		= chat_headers,
	.reasoning		= true,
	.logprobs		= true,
};
