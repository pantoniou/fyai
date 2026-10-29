/* SPDX-License-Identifier: MIT */
#ifndef FYAI_GRAMMAR_H
#define FYAI_GRAMMAR_H

#include <stdbool.h>
#include <stddef.h>

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

struct fyai_grammar;

/* A header that every request of a grammar carries. */
struct fyai_grammar_header {
	const char *name;
	const char *value;
};

/*
 * What the core decides for one model request. The grammar puts it on the
 * wire.
 */
struct fyai_grammar_request {
	fy_generic messages;		/* the canonical messages */
	const char *instructions;	/* the system prompt */
	/* The response this request continues, or fy_invalid. */
	fy_generic previous_response_id;
	long long max_tokens;		/* the output limit */
	bool temperature;		/* the model takes a temperature */
	bool extents_lp;		/* ask for logprobs of the text */
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

	/*
	 * The canonical messages in the shape of the request input, in the
	 * transient builder. The system message is not part of it.
	 */
	fy_generic (*input)(struct fyai_ctx *ctx, fy_generic messages);
	/* The tool declarations of the configuration, in ctx->gb. */
	fy_generic (*make_tools)(struct fyai_ctx *ctx);
	/* One function tool, given as {type: function, function: ...}. */
	fy_generic (*tool_def)(struct fyai_ctx *ctx, fy_generic spec);
	/* An image part of the canonical content. */
	fy_generic (*image_part)(struct fyai_ctx *ctx, fy_generic part);
	/* The body of a model request, in the transient builder. */
	fy_generic (*build_request)(struct fyai_ctx *ctx,
				    const struct fyai_grammar_request *req);
	/* The compact URL of the endpoint; NULL when there is none. */
	bool (*compact_url)(const struct fyai_cfg *cfg, char *out, size_t size);

	bool reasoning;			/* reasoning options are sent */
	bool logprobs;			/* token log probabilities are returned */
	bool builtin_shell;		/* a provider can accept the native shell */
	bool chaining;			/* a request can name the previous response */
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

/*
 * The grammar after @grammar in order of preference: the first for NULL, and
 * NULL after the last.
 */
const struct fyai_grammar *fyai_grammar_next(const struct fyai_grammar *grammar);

/* Walk the grammars in order of preference. @grammar is NULL after the walk. */
#define fyai_grammar_foreach(grammar) \
	for ((grammar) = fyai_grammar_next(NULL); (grammar); \
	     (grammar) = fyai_grammar_next(grammar))

/* True when @url is the built-in default URL of a grammar. */
bool fyai_grammar_default_url(const char *url);

/* True when reasoning options are configured. */
bool fyai_grammar_reasoning_set(const struct fyai_cfg *cfg);

/*
 * The operations of the grammar that the configuration selects. The core
 * calls these, never a member of the table; an operation that the grammar
 * does not have does nothing.
 */

/* The canonical @messages as the request input, in the transient builder. */
fy_generic fyai_grammar_input(struct fyai_ctx *ctx, fy_generic messages);

/* The tool declarations of the configuration, in ctx->gb. */
fy_generic fyai_grammar_make_tools(struct fyai_ctx *ctx);

/* One function tool @spec, given as {type: function, function: ...}. */
fy_generic fyai_grammar_tool_def(struct fyai_ctx *ctx, fy_generic spec);

/* An image part of the canonical content. */
fy_generic fyai_grammar_image_part(struct fyai_ctx *ctx, fy_generic part);

/* The body of a model request, in the transient builder. */
fy_generic fyai_grammar_build_request(struct fyai_ctx *ctx,
				      const struct fyai_grammar_request *req);

/* True when an endpoint of the grammar can offer /compact. */
bool fyai_grammar_compacts(const struct fyai_cfg *cfg);

/* The compact URL of the endpoint of @cfg; false when there is none. */
bool fyai_grammar_compact_url(const struct fyai_cfg *cfg, char *out,
			      size_t size);

/*
 * The helpers that the grammars share (fyai_grammar.c, fyai_provider.c).
 */

/* True when the request declares function tools. */
bool fyai_grammar_tools_wanted(struct fyai_ctx *ctx);

/* @request with "stream" and @options, when the configuration streams. */
fy_generic fyai_grammar_add_stream(struct fyai_ctx *ctx, fy_generic request,
				   fy_generic options);

/* Report @what when @v is invalid; return @v. */
fy_generic fyai_provider_result(struct fyai_ctx *ctx, fy_generic v,
				const char *what);

/* An agent message as attributed user text. */
fy_generic fyai_agent_wire_text(struct fyai_ctx *ctx, fy_generic message);

/* A tool result as the text that every grammar takes. */
fy_generic fyai_wire_tool_text(struct fyai_ctx *ctx, fy_generic output);

/* Canonical content with each image part in the shape of the grammar. */
fy_generic fyai_wire_content(struct fyai_ctx *ctx, fy_generic content,
			     const char *text_type);

/*
 * The canonical messages as Anthropic Messages blocks, without the cache
 * marker. The Claude SDK layer also writes this shape.
 */
fy_generic fyai_messages_blocks(struct fyai_ctx *ctx, fy_generic messages);

/* "responses|chat-completions|messages", for a message. */
#define FYAI_GRAMMAR_NAMES "responses|chat-completions|messages"

#endif
