/*
 * fyai_grammar_messages.c - the Anthropic Messages grammar
 *
 * Copyright (c) 2026 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "fyai_grammar.h"

static const struct fyai_grammar_header messages_headers[] = {
	{ "anthropic-version", ANTHROPIC_VERSION },
	{ NULL, NULL },
};

const struct fyai_grammar fyai_grammar_messages = {
	.mode			= FYAI_API_MESSAGES,
	.name			= "messages",
	.protocol		= "messages",
	.title			= "Messages API",
	.default_url		= ANTHROPIC_MESSAGES_URL,
	.default_model		= DEFAULT_ANTHROPIC_MODEL,
	.default_provider	= "anthropic",
	.key_header		= "x-api-key",
	.headers		= messages_headers,
};
