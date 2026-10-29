# The canonical conversation format

## Scope

This document specifies the format of a stored conversation. The format is
independent of the provider. Each of the three provider grammars can continue
a conversation in it (`doc/provider-grammars.md`).

The document specifies:

- the stored turn;
- the messages and items of a turn;
- the conversion of a reply of each grammar into the format;
- the conversion of the format into a request of each grammar, and the
  provider stream.

The examples are output of the implementation. The grammar unit tests
(`tests/fyai_grammar_test.c`) verify the same conversions.

## The turn

A conversation is a chain of turns. Each turn refers to the turn before it.
A branch head refers to the last turn. The messages of a conversation are the
`messages` of its turns, in chain order.

One exchange usually has several turns: one for the user line, one for each
model reply, and one for each tool result. A turn reference `~N` counts these
turns.

```yaml
previous: <the turn before, or null>
messages: [ ... ]               # canonical content; the model input
metadata:
  api: responses                # the grammar of the turn
  provider: openai
  model: gpt-5.4-mini
  temperature: 0.2
  reasoning_effort: high        # present when configured
  reasoning_summary: auto       # present when configured
  usage: {input, cached, cache_write, output, reasoning, total, cost, est, model}
  tokens: [ {text, pos, lp}, ... ]   # token extents, when configured
  response_id: resp_1           # Responses, when response chaining is on
  compacted_from: <the head before a compaction>
  compact_instructions: <the hint of that compaction>
  imported: true                # a turn of an imported session
  import_source: codex
provider_stream:
  openai: [ ... ]               # the wire record of the reply
display_outputs:
  - {tag: assistant, markdown: "...", state: finalized, fragments: [ ... ]}
```

| Member | Content | Sent to a model |
| --- | --- | --- |
| `previous` | The link to the turn before. | No |
| `messages` | The canonical messages of the turn. | Yes |
| `metadata` | Provenance and accounting. | No |
| `provider_stream` | The wire record of one reply, by provider name. | No |
| `display_outputs` | The Markdown that was shown, with its fragments. | No |

A continuation takes its settings from the branch configuration, not from
`metadata`. `provider_stream` keeps the reply as the provider sent it, for
fidelity and inspection (`dump providers`). Because it is never sent, the wire
fields of one provider never reach another. `display_outputs` is the source of
history replay (`doc/display-output-semantics.md`).

A published turn does not change. A rewind, a merge, or a rebase makes new
turns (`doc/branching.md`).

## Messages and items

A message is a role message or an item. A role message has `role` and
`content`. An item has `type`.

| Shape | Origin | Notes |
| --- | --- | --- |
| `{role: system, content}` | Setup, `/clear`, compaction | The first message of a conversation. |
| `{role: user, content}` | The user | Text, or parts with attachments. |
| `{role: assistant, content}` | A reply | The words of a reply. |
| `{role: agent, content, origin}` | A sub-agent | `origin` is `{branch, execution, id, reply_to}`. |
| `{type: function_call, call_id, name, arguments}` | A reply | `arguments` is a JSON string. |
| `{type: function_call_output, call_id, output}` | The tool loop | `output` is a string. |
| `{type: shell_call, call_id, action}` | A Responses reply | The native shell. `action` holds `commands`. |
| `{type: shell_call_output, call_id, output, max_output_length}` | The tool loop | `output` is a list of `{stdout, stderr, outcome}`. |
| `{type: message, role, content}` | A Responses reply | The words of a turn that calls tools. |
| `{type: reasoning, ...}` | A Responses reply | Kept as a record. Not sent again. |
| `{role: assistant, content: [blocks]}` | A Messages reply | A turn that `pause_turn` ended. |

### Content

Content is a string when it holds text only. Content with an attachment is a
sequence of parts:

```yaml
role: user
content:
  - {type: text, text: "what is wrong in this screenshot?"}
  - {type: image, media_type: image/png, digest: "ab12...", width: 1024,
     height: 768, bytes: 98304, data: !!binary "iVBORw0KGgo...",
     name: clipboard}
```

The part types are `text` and `image`. A text part can also carry a file.
`doc/media.md` specifies the parts.

### Calls and results

A call and its result have one shape for every grammar:

```yaml
- {type: function_call, call_id: call_1, name: read_file,
   arguments: '{"path":"a.txt"}'}
- {type: function_call_output, call_id: call_1, output: hello}
```

A result refers to its call by `call_id`. A `shell_call` has a
`shell_call_output` as its result. The outcome of a shell command is one of:

- `{type: exit, exit_code}`
- `{type: timeout, timeout_ms}`
- `{type: signal, signal}`

`arguments` is a JSON string. For Responses and Chat Completions, it holds
the bytes that the provider sent. Messages sends `input` as an object, and its
grammar encodes that object again. The encoder spaces the result
(`{"path": "a.txt"}`), but the value does not change.

The key `_fyai_user_owned` gives the user ownership of a call. A call from a
model cannot have this key: the parse of the reply removes it
(`fyai_response_canonical()`).

## Replies into the format

The grammar of a reply converts the reply into a canonical response
(`doc/provider-grammars.md`). The `messages` of that response become the next
turn, and its `wire` becomes the provider stream of that turn
(`fyai_append_assistant_response()`). A streamed reply is first assembled into
the document of a buffered reply, so both paths give the same turn.

Each example below is one exchange: the model writes a few words and reads a
file, then it answers.

### Responses

The reply that calls a tool:

```yaml
id: resp_1
output:
  - {type: reasoning, id: rs_1, summary: []}
  - type: message
    id: msg_1
    role: assistant
    content: [{type: output_text, text: Let me look.}]
  - {type: function_call, id: fc_1, call_id: call_1, name: read_file,
     arguments: '{"path":"a.txt"}'}
usage: {input_tokens: 120, output_tokens: 30,
        input_tokens_details: {cached_tokens: 100}}
```

The turn keeps the output items unchanged, both in `messages` and in the
provider stream. The next request sends them again in the same form, which
keeps the prefix that the provider cached.

```yaml
messages:
  - {type: reasoning, id: rs_1, summary: []}
  - type: message
    id: msg_1
    role: assistant
    content: [{type: output_text, text: Let me look.}]
  - {type: function_call, id: fc_1, call_id: call_1, name: read_file,
     arguments: '{"path":"a.txt"}'}
provider_stream:
  openai: [ <the same three items> ]
```

The final answer is stored as its words. The provider stream keeps its
message item:

```yaml
messages:
  - {role: assistant, content: It says hello.}
provider_stream:
  openai:
    - type: message
      id: msg_2
      role: assistant
      content: [{type: output_text, text: It says hello.}]
```

With response chaining on, the turn also holds `response_id`. The next request
then has `previous_response_id` and only the messages after that turn.

### Chat Completions

The reply that calls a tool:

```yaml
id: cc_1
choices:
  - index: 0
    message:
      role: assistant
      content: Let me look.
      tool_calls:
        - id: call_1
          type: function
          function: {name: read_file, arguments: '{"path":"a.txt"}'}
    finish_reason: tool_calls
usage: {prompt_tokens: 120, completion_tokens: 30,
        prompt_tokens_details: {cached_tokens: 100}}
```

The words of the message become a text message, and each of its calls becomes
a call item. The message itself is the provider stream:

```yaml
messages:
  - {role: assistant, content: Let me look.}
  - {type: function_call, call_id: call_1, name: read_file,
     arguments: '{"path":"a.txt"}'}
provider_stream:
  openai:
    - role: assistant
      content: Let me look.
      tool_calls:
        - id: call_1
          type: function
          function: {name: read_file, arguments: '{"path":"a.txt"}'}
```

A message that has calls and no words gives no text message. A final message
always gives a text message, also an empty one.

### Anthropic Messages

The reply that calls a tool:

```yaml
id: msg_1
type: message
role: assistant
content:
  - {type: thinking, thinking: "..."}
  - {type: text, text: Let me look.}
  - {type: tool_use, id: call_1, name: read_file, input: {path: a.txt}}
stop_reason: tool_use
usage: {input_tokens: 20, output_tokens: 30, cache_read_input_tokens: 100}
```

The text blocks become one text message, and each `tool_use` block becomes a
call item. The canonical messages do not keep the thinking. The content blocks
are the provider stream:

```yaml
messages:
  - {role: assistant, content: Let me look.}
  - {type: function_call, call_id: call_1, name: read_file,
     arguments: '{"path": "a.txt"}'}
provider_stream:
  anthropic:
    - {type: thinking, thinking: "..."}
    - {type: text, text: Let me look.}
    - {type: tool_use, id: call_1, name: read_file, input: {path: a.txt}}
```

A turn that `pause_turn` ends is stored as two parts: its content blocks in one
assistant message, and its calls as call items. A server tool, such as the
web search, continues only from the complete block array.

Messages reports cache reads and cache writes beside the input count. The
canonical usage includes them in the input, as the other grammars do. The
three replies above all have the usage
`{input: 120, cached: 100, output: 30, total: 150}`.

### Imported sessions

A Codex session holds Responses items, and a Claude Code session holds
Messages messages. The `history` operation of the grammar of the session is
the inverse of its request input, and it reads such a session into the format
(`fyai_grammar_history()`). The importer maps the tools of each agent to the
native tools (`doc/foreign-session-import-plan.md`,
`doc/foreign-tool-mapping.md`). Content that the format cannot hold becomes the
text `[content omitted]` and a loss record.

## The format into a request

The request input is made from the canonical messages for each request
(`fyai_grammar_input()`). No wire form of a request is stored. The system
message goes where the grammar carries it.

The history below was stored by Responses, from the exchange above:

```yaml
- {role: system, content: Be brief.}
- {role: user, content: What is in a.txt?}
- {type: reasoning, id: rs_1, summary: []}
- type: message
  id: msg_1
  role: assistant
  content: [{type: output_text, text: Let me look.}]
- {type: function_call, id: fc_1, call_id: call_1, name: read_file,
   arguments: '{"path":"a.txt"}'}
- {type: function_call_output, call_id: call_1, output: hello}
```

### Responses

The system message is the `instructions`. The reasoning item is not sent. The
other items are sent as stored:

```yaml
instructions: Be brief.
input:
  - {role: user, content: What is in a.txt?}
  - type: message
    id: msg_1
    role: assistant
    content: [{type: output_text, text: Let me look.}]
  - {type: function_call, id: fc_1, call_id: call_1, name: read_file,
     arguments: '{"path":"a.txt"}'}
  - {type: function_call_output, call_id: call_1, output: hello}
store: false
```

### Chat Completions

The system message stays a message. The words and the calls of one turn
become one assistant message, and each result becomes a tool message:

```yaml
messages:
  - {role: system, content: Be brief.}
  - {role: user, content: What is in a.txt?}
  - role: assistant
    content: Let me look.
    tool_calls:
      - id: call_1
        type: function
        function: {name: read_file, arguments: '{"path":"a.txt"}'}
  - {role: tool, tool_call_id: call_1, content: hello}
```

### Anthropic Messages

The system message is the `system` field. Each message becomes content
blocks. The roles of Messages alternate, so consecutive blocks of one role
make one message. The system prompt and the last block of the request carry a
`cache_control` breakpoint:

```yaml
system:
  - {type: text, text: Be brief., cache_control: {type: ephemeral}}
messages:
  - role: user
    content: [{type: text, text: What is in a.txt?}]
  - role: assistant
    content:
      - {type: text, text: Let me look.}
      - {type: tool_use, id: call_1, name: read_file, input: {path: a.txt}}
  - role: user
    content:
      - {type: tool_result, tool_use_id: call_1, content: hello,
         cache_control: {type: ephemeral}}
```

### Other conversions

- **Native shell.** A provider that does not accept the native shell gets a
  `shell_call` as a call of the function shell tool (`exec_command`), and its
  output as the text of a function result.
- **Images.** Each grammar sends an image part in its own image shape
  (`doc/media.md`).
- **Paused Messages turns.** Messages sends the content blocks of a paused turn
  again, with each call once. Chat Completions and Responses send the words of
  the blocks and the calls.

## Invariants

- **One stored shape.** For every grammar, a call is a call item and a result
  is an output item. `messages` holds no wire shape of a grammar, with two
  exceptions: the Responses items of a turn that calls tools, and the block
  array of a paused Messages turn. Each of these goes back unchanged to the
  grammar that made it. The other grammars send its words and its calls.
- **Derived requests.** Every wire form of a request comes from `messages`. A
  request never comes from `provider_stream`.
- **Stable prefix.** A stored turn goes back as the provider sent it. Each
  request starts with the request before it. The Messages breakpoint moves to
  the last block. The last message of a request can gain blocks when the next
  request merges a role. `tests/fyai_grammar_test.c` and the
  `grammar_cache_prefix_*` cases verify this for every grammar and across
  grammars.
- **One format version.** A stored shape that the implementation no longer
  writes is not read.
