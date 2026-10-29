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

/*
 * The state of one streamed reply, which a grammar parses into. The core
 * owns it and presents what the grammar hands it through the
 * fyai_stream_*() functions.
 */
struct fyai_stream_parse {
	struct fyai_ctx *ctx;
	const struct fyai_grammar *grammar;	/* the grammar of the request */
	struct fy_generic_builder *gb;	/* lives as long as the request */
	void *state;			/* stream_state_size bytes of the grammar */
	fy_generic content_chunks;	/* the answer text, as it arrived */
	/*
	 * Tool slots by index: {id, type, name, function: {name_chunks,
	 * argument_chunks}}.
	 */
	fy_generic tool_calls;
	fy_generic metadata;		/* id, model and the like of the reply */
	fy_generic finish_reason;
	fy_generic usage;		/* as the provider reports it */
	/*
	 * Token extents: {text, pos, lp} per logprob token. extents_pos is
	 * the byte offset into the joined text; extents_lp says that logprobs
	 * delimit them.
	 */
	fy_generic token_extents;
	size_t extents_pos;
	bool extents_lp;
	bool done;			/* the reply ended */
	bool failed;
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
	/* The size of the stream state of the grammar, and its setup. */
	size_t stream_state_size;
	void (*stream_init)(struct fyai_stream_parse *p);
	/* One SSE event of a streamed reply. */
	int (*stream_event)(struct fyai_stream_parse *p, fy_generic event);
	/* The streamed reply as the document of a buffered reply, in p->gb. */
	fy_generic (*stream_finish)(struct fyai_stream_parse *p);
	/*
	 * The canonical response of a reply document, in the transient
	 * builder:
	 *
	 *   id:       the response id, or ""
	 *   text:     the answer text
	 *   calls:    the function_call and shell_call items
	 *   messages: the canonical messages that the turn stores
	 *   final:    true when the turn ends here
	 *   usage:    {input, cached, cache_write, output, reasoning, total,
	 *              cost}, or null
	 *   wire:     the provider stream of the turn, or null
	 *   doc:      the reply document
	 */
	fy_generic (*response)(struct fyai_ctx *ctx, fy_generic doc);
	/* True when a failed reply says that the previous response is gone. */
	bool (*chain_miss)(long status, const char *body);
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

/* The grammars in order of preference, ended by NULL. */
extern const struct fyai_grammar *const fyai_grammars[];

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

/*
 * Give @p the grammar of the configuration and its stream state; free the
 * state with fyai_grammar_stream_close(). The stream operations use that
 * grammar, so a reply is read in the grammar of its request.
 */
int fyai_grammar_stream_open(struct fyai_ctx *ctx, struct fyai_stream_parse *p);
void fyai_grammar_stream_close(struct fyai_stream_parse *p);

/* One SSE event of a streamed reply. */
int fyai_grammar_stream_event(struct fyai_stream_parse *p, fy_generic event);

/* The streamed reply as the document of a buffered reply, in p->gb. */
fy_generic fyai_grammar_stream_finish(struct fyai_stream_parse *p);

/* The canonical response of the reply document @doc. */
fy_generic fyai_grammar_response(struct fyai_ctx *ctx, fy_generic doc);

/*
 * True when a request that continued a response failed because the
 * provider no longer has that response.
 */
bool fyai_grammar_chain_miss(struct fyai_ctx *ctx, long status,
			     const char *body);

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

/* True when reasoning options are configured. */
bool fyai_grammar_reasoning_set(const struct fyai_cfg *cfg);

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

/* Text of the answer: end the reasoning, record and present @text. */
int fyai_stream_content(struct fyai_stream_parse *p, const char *text);

/* Text of reasoning, presented when the configuration shows it. */
void fyai_stream_reasoning(struct fyai_stream_parse *p, const char *text);

/* The reply ended. */
void fyai_stream_done(struct fyai_stream_parse *p);

/* Make tool slots up to @index. */
int fyai_stream_tool_slot(struct fyai_stream_parse *p, size_t index);

/* Start the call item @call of slot @index, one time. */
int fyai_stream_tool_emit(struct fyai_stream_parse *p, size_t index,
			  fy_generic call);

/* Present a tool that ran at the provider. */
int fyai_stream_hosted(struct fyai_stream_parse *p, const char *name,
		       fy_generic args, bool ok, const char *cause);

/* Report a failure event of the provider and fail the reply. */
void fyai_stream_fail(struct fyai_stream_parse *p, fy_generic type,
		      fy_generic event);

/*
 * The canonical messages as Anthropic Messages blocks, without the cache
 * marker. The Claude SDK layer also writes this shape.
 */
fy_generic fyai_messages_blocks(struct fyai_ctx *ctx, fy_generic messages);

/* "responses|chat-completions|messages", for a message. */
#define FYAI_GRAMMAR_NAMES "responses|chat-completions|messages"

#endif
