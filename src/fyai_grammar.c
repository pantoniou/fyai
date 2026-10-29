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

const char *fyai_api_to_string(enum fyai_api_mode api)
{
	if ((unsigned int)api >= FYAI_API_COUNT)
		return NULL;
	return fyai_grammars[api]->name;
}
