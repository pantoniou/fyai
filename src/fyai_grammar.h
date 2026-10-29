/* SPDX-License-Identifier: MIT */
#ifndef FYAI_GRAMMAR_H
#define FYAI_GRAMMAR_H

#include <stdbool.h>

#include "fyai.h"

/*
 * The interface between the provider-independent core and the wire grammar
 * of a provider API. A grammar lives in one file and exports one table. The
 * core reaches the grammar of a configuration through fyai_grammar(); nothing
 * else names a grammar.
 *
 *   Responses          fyai_grammar_responses.c
 *   Chat Completions   fyai_grammar_chat.c
 *   Anthropic Messages fyai_grammar_messages.c
 *
 * A grammar is selected at run time by the `api` configuration key, so all
 * three are linked into every build.
 */

/* A header that every request of a grammar carries. */
struct fyai_grammar_header {
	const char *name;
	const char *value;
};

struct fyai_grammar {
	enum fyai_api_mode mode;
	const char *name;		/* the value of the `api` key */
	const char *alias;		/* another accepted spelling, or NULL */
	const char *protocol;		/* the catalogue endpoint protocol */
	const char *title;		/* the name of the API in a message */
	const char *default_url;
	const char *default_model;
	const char *default_provider;
	/* The header of the API key; NULL is "Authorization: Bearer". */
	const char *key_header;
	/* The headers that every request carries, ended by a NULL name. */
	const struct fyai_grammar_header *headers;

	bool reasoning;			/* reasoning options are sent */
	bool logprobs;			/* token log probabilities are returned */
	bool builtin_shell;		/* a provider can accept the native shell */
	bool compaction;		/* a provider can offer /compact */
	bool chatgpt;			/* a ChatGPT subscription can serve it */
};

extern const struct fyai_grammar fyai_grammar_responses;
extern const struct fyai_grammar fyai_grammar_chat;
extern const struct fyai_grammar fyai_grammar_messages;

/* The grammar of @mode. Never NULL for a valid mode. */
const struct fyai_grammar *fyai_grammar_get(enum fyai_api_mode mode);

/* The grammar that @cfg selects. */
static inline const struct fyai_grammar *
fyai_grammar(const struct fyai_cfg *cfg)
{
	return fyai_grammar_get(cfg->api_mode);
}

/* The grammar named @name or its alias, or NULL. */
const struct fyai_grammar *fyai_grammar_find(const char *name);

/* The grammars in order of preference, ended by NULL. */
extern const struct fyai_grammar *const fyai_grammars[];

/* "responses|chat-completions|messages", for a message. */
#define FYAI_GRAMMAR_NAMES "responses|chat-completions|messages"

#endif
