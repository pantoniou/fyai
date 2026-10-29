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
#include <string.h>

#include "fyai_grammar.h"

/* Indexed by enum fyai_api_mode; the order is the order of preference. */
static const struct fyai_grammar *const grammars[] = {
	[FYAI_API_RESPONSES]		= &fyai_grammar_responses,
	[FYAI_API_CHAT_COMPLETIONS]	= &fyai_grammar_chat,
	[FYAI_API_MESSAGES]		= &fyai_grammar_messages,
	[FYAI_API_COUNT]		= NULL,
};

const struct fyai_grammar *fyai_grammar_get(enum fyai_api_mode mode)
{
	if ((unsigned int)mode >= FYAI_API_COUNT)
		return grammars[FYAI_API_RESPONSES];
	return grammars[mode];
}

const struct fyai_grammar *fyai_grammar_next(const struct fyai_grammar *grammar)
{
	return grammar ? grammars[grammar->mode + 1] : grammars[0];
}

const struct fyai_grammar *fyai_grammar_find(const char *name)
{
	const struct fyai_grammar *grammar;

	if (!name)
		return NULL;
	fyai_grammar_foreach(grammar)
		if (!strcmp(grammar->name, name) ||
		    (grammar->alias && !strcmp(grammar->alias, name)))
			return grammar;
	return NULL;
}

bool fyai_grammar_default_url(const char *url)
{
	const struct fyai_grammar *grammar;

	if (!url)
		return false;
	fyai_grammar_foreach(grammar)
		if (!strcmp(url, grammar->default_url))
			return true;
	return false;
}

bool fyai_grammar_compacts(const struct fyai_cfg *cfg)
{
	return fyai_grammar(cfg)->compaction;
}

bool fyai_grammar_reasoning_set(const struct fyai_cfg *cfg)
{
	return !fy_str_empty(cfg->reasoning_effort) ||
	       !fy_str_empty(cfg->reasoning_summary);
}

const char *fyai_api_to_string(enum fyai_api_mode api)
{
	if ((unsigned int)api >= FYAI_API_COUNT)
		return NULL;
	return grammars[api]->name;
}
