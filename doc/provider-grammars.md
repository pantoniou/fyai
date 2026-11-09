# Provider grammars

fyai speaks three provider API grammars: OpenAI Responses, Chat Completions
and Anthropic Messages. The `api` configuration key selects one. Each grammar
is a backend, as each platform is for the sandbox and the views. The core does
not name a grammar.

| Grammar | File | `api` | Catalogue protocol |
| --- | --- | --- | --- |
| Responses | `src/fyai_grammar_responses.c` | `responses` | `responses` |
| Chat Completions | `src/fyai_grammar_chat.c` | `chat-completions`, `chat` | `chat_completions` |
| Anthropic Messages | `src/fyai_grammar_messages.c` | `messages` | `messages` |

## The interface

`src/fyai_grammar.h` defines `struct fyai_grammar`. Each backend exports one
table. The grammar is selected at run time, so every build links all three.

- `fyai_grammar(cfg)` gives the grammar of a configuration.
- `fyai_grammar_find(name)` resolves a name or an alias.
- `fyai_grammar_foreach(grammar)` walks the grammars in order of
  preference. Catalogue endpoint selection walks it.
- `fyai_grammar_default_url(url)` tells a built-in default URL from one the
  user chose.

The table holds data and operations:

- **Identity:** the name, the alias, the catalogue protocol, the title for
  messages, and the default URL, model and provider.
- **Transport:** the header of the API key (NULL is `Authorization: Bearer`)
  and the headers that every request carries.
- **Capabilities:** `reasoning`, `logprobs`, `builtin_shell`, `chaining` and
  `chatgpt`. `fyai_config_grammar_gate()` disables or rejects the options
  that a grammar cannot carry.
- **Operations:** the input, the split of the input, the tool declarations,
  the image part, the request body, the stream parse, the canonical response, the history, the
  chain miss and the compact URL.

The core calls an operation through its wrapper, such as
`fyai_grammar_build_request(ctx, req)`. The wrapper takes the grammar from
`ctx->cfg`; the context holds no grammar of its own, so a configuration
change cannot leave a stale one behind. A streamed reply is the exception:
`fyai_grammar_stream_open()` records the grammar in the parse state, and the
stream operations use that one, so a reply is read in the grammar of its
request. Only `src/fyai_grammar.c` reads a function member. An operation that
a grammar does not have does nothing.

## Requests

The core decides what a request needs and gives it to the grammar in
`struct fyai_grammar_request`. That is the canonical messages, the system
prompt, the previous response when the request continues one, the output
limit, and whether the model takes a temperature and logprobs. The grammar
puts it on the wire with `build_request`. The tool declarations come from
`make_tools`, and `tool_def` converts each MCP or discovery tool.

## Replies

A grammar parses a reply into one canonical response:

```yaml
id: resp_1             # "" when the reply has none
text: the answer
calls: [...]           # function_call and shell_call items
messages: [...]        # the canonical messages that the turn stores
final: true            # false when the turn calls tools or pauses
usage: {input, cached, cache_write, output, reasoning, total, cost}
wire: [...]            # the provider stream of the turn; never sent
doc: {...}             # the reply document
```

A buffered reply is a document. A streamed reply arrives as SSE events. The
stream core splits the lines, retries and presents. The grammar parses each
event with `stream_event` into `struct fyai_stream_parse` and into its own
state. It calls back to present content, reasoning and hosted tools, and to
start each call. At the end `stream_finish` builds the document that a
buffered reply would be. `fyai_response_canonical()` then reads both paths
through the `response` operation of the grammar and takes away the ownership
that only the user can give a call.

`fyai_response_output_text()`, `fyai_response_tool_calls()`,
`fyai_response_is_final()`, `fyai_extract_usage()` and the turn store read
the canonical response. None of them tests a grammar.

## History

`history` is the inverse of the input: it reads wire messages of the grammar
into canonical messages. The Chat Completions and Messages replies build
their stored messages with it. The session importer reads a Codex session
(Responses items) and a Claude Code session (Messages messages) with it, so
the grammar is the one place that knows a wire shape
(`doc/foreign-session-import-plan.md`).

A file of another program names its grammar, so `fyai_grammar_history()` takes
the grammar as an argument. With a loss list, content that the canonical form
cannot hold leaves `[content omitted]` in the text and a `{kind,
content_type}` record; a reply passes no list and drops such content, such
as the blocks of a hosted search. The tests read the input of every grammar
back through its history.

## Hosted tools

A hosted tool, such as the web search, runs at the provider. The catalogue
says which endpoint offers one (`hosted_tools`), and the tool type that the
endpoint declares it with when the protocol has no standard type for it
(`hosted_tool_types`). Model resolution derives `cfg->web_search_type` from
it; Chat Completions declares the search as a tool of that type beside the
function tools, and as the `web_search_options` request option when the
endpoint names none. No grammar tests a provider name.

## One stored shape

A tool call is a `function_call` or `shell_call` item with `call_id`, `name`
and `arguments`. A result is a `function_call_output` or `shell_call_output`
item. Every grammar stores these shapes. The input of each grammar converts
them back to its wire. Chat Completions joins an assistant text message and
the calls that follow it into one message. No Chat-shaped message is stored.
`doc/canonical-format.md` gives the whole format, with the replies and requests
of each grammar.

## The prompt cache

A provider caches a prefix of the request. Two rules keep the cached prefix:

- A stored turn goes back on the wire as the provider sent it. Responses
  stores the output items of a turn that calls tools. Chat Completions and
  Responses keep the `arguments` string of the provider. Messages encodes the
  `input` of a `tool_use` block, and its keys keep their order.
- Each request starts with the messages of the one before. Messages merges the
  blocks of one role into one message, so the last message of a request can
  gain blocks. Its `cache_control` breakpoint moves to the last block of each
  request.

`tests/fyai_grammar_test.c` checks both rules for every grammar, and for a
conversation that moves from one grammar to the next. The
`grammar_cache_prefix_*` cases check them on the requests that the mock
records.

## The request cache

A request that does not continue a stored response carries the whole
conversation, and the conversation grows with each turn. Making the text of
all the turns again for each request costs more with each turn: the core
collects the messages, the grammar builds the input from them, and the emitter
writes the input as JSON. The stored turns never change, so `src/fyai_reqcache.c`
keeps the text.

The cache keeps the text of the part of the input that the next request
repeats. The body of the next request is the text before the input, the kept
text, the text of the turns that came after it, and the text after the input.
The text before and after the input holds the tools and the options. The
cache keeps it with the word of the request that made it, and a copy of that
request in a builder of its own. The transient builder dedups, so a request of
the same value has the same word, and the emitter writes the tools one time for
the exchange. In a later exchange the word is of no use, and the cache compares
the request with its copy: an equal request has the same text. A request that
is not the same is written one time, pair by pair, and cut at the pair that
holds the input.

What the grammar can say about its input decides how much is kept. The
operation `input_split` makes the input of some messages and says two things:
how many items at its start do not change when messages follow, and the index
of the first message that makes the other items.

- Chat Completions keeps all the input. A request ends between two turns, and
  an item depends on the items around it only inside one assistant turn.
- Messages keeps all the messages but the last. The last message can gain the
  blocks of the messages that follow it, and its `cache_control` breakpoint
  moves to its last block in each request. The request makes the last message
  again, from the first message that made it.
- Responses has no split. It chains requests with `previous_response_id` and
  sends only the new items, so it needs none. A grammar that does not split
  its input makes the request as before.

The cache serves a request when these are true:

- the configuration and the catalogue are the same. Any change of them, such
  as another model, endpoint or system prompt, clears the cache;
- the request extends the last request of the exchange. The cache keeps the
  turn where the last request ended, named by the word of the turn. A turn
  holds the turn before it, so the word stands for the whole chain up to it.
  The request walks back from its last turn to that turn and reads only the
  turns after it. A request on an earlier turn, or on another branch, is not an
  extension: it makes the text again, and the cache then keeps that request;
- the request needs no value of the request: debug output and conversation
  logging print the request, and a subscription request changes it.

The cache keeps one body: the text before the input, the stable items, and the
open items. The next request copies the stable items and emits only the items
of the new messages and the open items. The cache hashes no text to find the
turn. `$FYAI_TRACE` records the bytes that the emitter made for each request.

The word of a turn names storage of the builder that holds it. A turn that the
transient builder holds has a word that a new object can take when the builder
goes away, so the cache forgets it with the builder. A turn that the arena
holds has a word that stays: the arena does not move. When the transient
builder goes away, the cache keeps the text and the turn if the arena holds the
turn, and forgets the word of the request that the builder made. The head and
the tail stay with their copy of the request. The next exchange finds the turn
in its chain, and its first request emits only the messages that came after
the turn, unless its tools or options differ from the copy.
A line with a tool call stores its last turn with the display of the call, as
another value. The cache then keeps the newest turn of the chain that the arena
held, the number of messages that follow it in the text, and the text of those
messages. The next exchange finds that turn in its chain and uses the kept text
only if the messages after it say the same text.

The `request_cache` key of the configuration turns the cache off.
`$FYAI_TRACE` records each request as a hit or a miss, and the cause of a miss.

`tests/fyai_grammar_test.c` makes the request at each turn of a conversation
with and without the cache and requires the same bytes. For Chat Completions it
also checks that the old body, less its text after the input, is the start of the new
body. It also checks a request that is not an extension and the end of the
builder. The cases `request_cache_chat`, `request_cache_messages` and
`request_cache_responses` run a session of one process, with tool calls, for
each grammar, with the cache and without it, and require the same requests from
the mock. For the two grammars that split their input they also require that
the second request of a line emits only the new items.
