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
- `fyai_grammars[]` lists the grammars in order of preference. Catalogue
  endpoint selection walks it.

The table holds data and operations:

- **Identity:** the name, the alias, the catalogue protocol, the title for
  messages, and the default URL, model and provider.
- **Transport:** the header of the API key (NULL is `Authorization: Bearer`)
  and the headers that every request carries.
- **Capabilities:** `reasoning`, `logprobs`, `builtin_shell`, `chaining` and
  `chatgpt`. `fyai_config_grammar_gate()` disables or rejects the options
  that a grammar cannot carry.
- **Operations:** the input, the tool declarations, the image part, the
  request body, the stream parse, the canonical response, the history, the
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
