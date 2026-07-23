# Display Output Semantics

## Purpose

fyai currently produces conversational output through several independent
Markdown renderers, direct `stdio` writes, UI work-bands, and terminal-control
paths. The durable history view does not replay a stored display document; it
reconstructs one from canonical messages and provider-specific data.

This report records the current behavior and defines the boundary needed to
make live output and history structurally identical.

The intended invariant is:

> Each displayed turn output is one progressive Markdown document with an
> explicit `system`, `user`, or `assistant` tag. The tag selects presentation
> policy. The finalized document is the source of truth for history.

For now, only `user` has a bubble presentation. Reasoning, when enabled, is a
distinctively styled block within assistant output.

## Current persisted model

`fyai_turn_append()` creates a new linked turn node for every appended group of
messages. In practice, a user input, an assistant tool request, a tool result,
and a final assistant answer may each become separate linked nodes even though
the display treats them as one conversational exchange.

Persisted turn data currently contains:

- `messages`: canonical provider-independent content used for model replay;
- `provider_stream`: provider-specific wire output, including reasoning;
- `metadata`: provider/model settings, usage, token extents, and related
  provenance;
- `previous`: the link to the preceding stored node.

There is no persisted display document and no explicit display-output tag.

Canonical messages must remain the source for model replay. Provider streams
must remain available for protocol fidelity and debugging. Neither is a
suitable substitute for the exact Markdown document shown to the user.

## How history is produced

`fyai history` traverses stored nodes and reconstructs Markdown from message
roles, native item types, tool adjacency, and provider-stream reasoning.

Important reconstruction behavior includes:

- user messages become blockquotes;
- system messages become italic text with a heading;
- assistant messages become plain Markdown;
- tool calls and results are reformatted from structured data;
- reasoning is recovered separately from `provider_stream`;
- `prev_tool` heuristics suppress separators between adjacent tool records and
  the assistant text that concludes a tool exchange;
- user bubbles and tool results force the accumulated Markdown buffer to be
  flushed and rendered through specialized paths.

Relevant implementation:

- `src/fyai_turn.c`: turn construction and provider-stream attachment;
- `src/fyai_display.c:337`: user blockquote construction;
- `src/fyai_display.c:842`: native reasoning rendering;
- `src/fyai_display.c:957`: turn-level provider-stream reasoning;
- `src/fyai_display.c:1365`: history grouping and `prev_tool`;
- `src/fyai_display.c:1403`: user bubble flush boundary;
- `src/fyai_display.c:1429`: tool-result flush boundary;
- `src/fyai_display.c:1460`: heuristic turn separators.

This means history represents a new rendering of semantic conversation data,
not the finalized live display output.

## Current live-output pipelines

### Assistant content

Streamed assistant content has its own progressive Markdown renderer.

- `src/fyai_stream.c:145` applies terminal line deltas directly.
- `src/fyai_stream.c:164` accumulates and progressively renders assistant
  content.
- `src/fyai_stream.c:224` writes plain-mode content directly to `stdout`.
- `src/fyai_stream.c:370` performs final healing and output.
- `src/fyai.c:72` handles non-streaming final responses through a separate
  one-shot Markdown path with a `printf` fallback.

The streamed and non-streamed paths therefore do not share one output object,
even when they eventually display the same assistant text.

### Reasoning

Reasoning is currently a second progressive document rather than part of the
assistant document.

- `src/fyai_stream.c:231` manually constructs reasoning Markdown headed by
  `**💭 reasoning**`.
- `src/fyai_stream.c:258` invokes a separate one-shot Markdown renderer for
  each update.
- `src/fyai_stream.c:277` sends rendered reasoning to a dedicated UI work-band
  or directly to `stderr`.
- `src/fyai_stream.c:303` has a non-Markdown fallback that prints
  `reasoning ▸` and raw text to `stderr`.
- `src/fyai_stream.c:337` manually prints the reasoning separator.
- `src/fyai_ui.c:429` owns a distinct reasoning work-band.

History separately implements the same concepts in
`fyai_emit_native_item()` and `fyai_emit_turn_reasoning()`. Encrypted reasoning
also differs: history can display an encrypted-reasoning marker while the live
stream has no corresponding display event.

Reasoning formatting is consequently duplicated and can diverge in style,
spacing, visibility, and ordering.

### Tool calls and results

Tool output is split between still more render paths.

For shell tools:

- `src/fyai_tools.c:75` creates a temporary Markdown document for the shell
  header and renders it directly to `stderr`;
- `src/fyai_tools.c:81` has a direct `fprintf` fallback;
- `src/fyai_tools.c:128` starts a separate progressive fenced-block renderer;
- `src/fyai_markdown.c:547` implements that fenced stream independently of the
  assistant stream;
- `src/fyai_tools.c:150` prints plain-mode shell headers directly;
- `src/fyai_tools.c:194` writes plain shell chunks directly to `stderr`;
- `src/fyai_tools.c:355` and `src/fyai_tools.c:529` contain direct binary-output
  summary fallbacks.

For non-shell tools:

- `src/fyai_display.c:1041` builds and renders a temporary Markdown document
  for the tool-call header;
- the tool result is rendered in a separate fenced-output call;
- live rendering and history share some emitters, but do not consume the same
  progressive document.

Tool calls and results are semantically assistant output. They should be
appended to the active assistant Markdown document rather than printed by the
execution layer.

### User output

User input is canonically stored with `role: user`, but its display path
bypasses the normal progressive Markdown renderer.

- `src/fyai_display.c:337` converts user text to blockquote Markdown;
- `src/fyai_display.c:2018` manually emits reverse bubble fence rows using SGR
  state and erase-line control;
- `src/fyai_display.c:2038` owns a separate reverse-render and commit path;
- `src/fyai_display.c:2077` has a direct plain fallback;
- `src/fyai.c:1320` displays the user input before persisting its canonical
  message.

The reverse bubble is therefore encoded in a special printing routine rather
than selected by a `user` output tag.

### Separators and blank lines

Spacing ownership is distributed across producers:

- reasoning prints its own separator and trailing blank lines;
- tool emitters include their own `\n\n`;
- fenced renderers append final newlines;
- the user bubble appends its own separation;
- history inserts separators based on adjacency heuristics;
- `src/fyai.c:1367` adds another blank line after an interactive response.

No component owns the grammar of a complete displayed turn, so small changes
regularly cause live/history spacing differences.

## Direct output that crosses the transcript boundary

Some direct output is transient UI chrome or diagnostics rather than
conversation content:

- the wait spinner writes terminal control directly to `stderr`
  (`src/fyai.c:398`);
- usage statistics print directly to `stderr` (`src/fyai.c:140`);
- `ask_user` prints its question, choices, and echoed answer directly to
  `stderr` (`src/fyai_tools.c:406`);
- diagnostics are drained after the turn;
- slash commands and administrative commands print directly to
  `stdout`/`stderr`;
- the pending-input indicator is manually assembled as ANSI text in
  `src/fyai_ui.c:43`.

The interactive UI redirects both `stdout` and `stderr` to spool files and
commits every captured byte into the transcript:

- `src/fyai_ui.c:112`: spool draining;
- `src/fyai_ui.c:208`: stream redirection setup.

The spool cannot distinguish among:

- semantic conversational Markdown;
- already-rendered ANSI output;
- diagnostics;
- command results;
- transient terminal chrome;
- cursor-control sequences.

This is a fundamental semantic leak. Capturing arbitrary process output cannot
be the primary transcript API.

## Target display model

### Output document

Introduce one output object representing a single progressive Markdown
document:

```text
display_output
    tag: system | user | assistant
    markdown: finalized source Markdown
    state: open | finalized | aborted
    optional metadata:
        reasoning visibility/presence
        interruption/error status
        provenance references
```

The object should support:

- `begin(tag)`;
- `append(markdown_fragment)`;
- progressive renderer updates;
- `finalize()`;
- persistence of the exact finalized Markdown;
- replay through the same tag-selected presentation policy.

Only the output object should drive transcript rendering.

### Tag presentation

Tags select presentation around a document; they do not rewrite its durable
Markdown.

- `system`: system presentation, initially a quiet/distinct style;
- `user`: reverse bubble presentation, with the Markdown content quoted inside
  the bubble;
- `assistant`: normal progressive Markdown presentation.

The user bubble should become a renderer/presentation policy for a tagged
document. Its fence fill is terminal rendering detail and must not be embedded
in durable Markdown.

### Reasoning

Reasoning should be represented as an assistant-document block, for example by
an internal fragment kind or a stable Markdown convention:

```text
assistant output
    reasoning block (optional and enabled)
    assistant prose
    tool call
    tool result
    assistant prose
```

Requirements:

- one implementation generates reasoning Markdown;
- live and history use that same Markdown;
- visibility is decided when constructing/replaying the output;
- encrypted reasoning has an explicit display representation when enabled;
- reasoning does not own a separate stderr document, separator, or work-band;
- styling is applied by the Markdown theme/renderer rather than hard-coded ANSI.

### Tool output

The tool execution layer should emit semantic events or Markdown fragments, not
write to terminal streams.

The assistant output builder should append:

1. the tool-call fragment;
2. progressive tool-output fragments or replacements;
3. the final tool-result fragment;
4. subsequent assistant text.

Progressive shell output may still use bounded/head-tail presentation, but it
must update a region belonging to the active assistant document. Its final
Markdown must be the exact fragment persisted for history.

### History

History should render persisted display outputs directly:

```text
for output in selected exchange:
    renderer.begin(output.tag)
    renderer.render(output.markdown)
    renderer.end()
```

Canonical messages remain available for model replay, and provider streams
remain available for raw/provider inspection. History should not reconstruct
the primary transcript from either.

For older stored conversations without display outputs, the existing
reconstruction code can remain as a compatibility migration/fallback.

## Output-channel separation

The implementation needs explicit channels:

- **transcript**: tagged progressive Markdown documents;
- **status/chrome**: spinner, busy state, pending input, footer;
- **diagnostic**: warnings and errors;
- **command**: slash-command and administrative output;
- **interaction**: prompts such as `ask_user`;
- **raw delivery**: plain/non-Markdown stdout mode where required by CLI
  contracts.

The UI may decide how each channel is presented, but only the transcript
channel belongs in durable display history.

Direct `printf` is acceptable inside isolated CLI verbs with defined
machine/human output contracts. It is not acceptable as an implicit way to add
content to an active conversational turn.

## Suggested migration sequence

1. Define the tagged progressive output API and context-owned active output.
2. Route assistant streaming through that API without changing its rendered
   appearance.
3. Route reasoning into the active assistant document and delete the separate
   reasoning renderer/work-band.
4. Route tool calls and tool results into the active assistant document.
5. Route user display through a `user` tag presentation and remove the special
   top-level print path.
6. Persist finalized output Markdown alongside canonical messages and provider
   streams.
7. Teach history to prefer persisted outputs, retaining reconstruction only
   for legacy records.
8. Replace stdout/stderr transcript spooling with explicit channel calls.
9. Move spinner, diagnostics, slash commands, and `ask_user` interaction onto
   their non-transcript channels.
10. Add libvterm oracles asserting that progressive live rendering and replay
    of the finalized tagged document produce identical terminal cells.

## Required invariants

The completed design should make the following testable:

1. Every transcript block has exactly one tag.
2. Every assistant exchange has exactly one progressive Markdown source.
3. Reasoning, tool calls, tool results, and assistant prose are ordered
   fragments of that source.
4. Finalized live Markdown is byte-identical to persisted display Markdown.
5. History renders persisted display Markdown without semantic reconstruction.
6. Tag presentation does not mutate durable Markdown.
7. User reverse-card fill is identical live and in history.
8. Tool previews and fenced blocks are identical live and in history.
9. Diagnostics and transient chrome never enter display history.
10. Interrupted outputs have an explicit final state and preserve the exact
    content that was visible when interrupted.

## Conclusion

The central problem is not an isolated collection of poorly placed
`fprintf()` calls. It is the absence of a first-class display-output object.
Without one, each producer owns rendering, spacing, terminal destination, and
partial history semantics.

A context-owned, tagged progressive Markdown document provides the missing
boundary. It lets live rendering, persistence, and history share one source
while keeping canonical model messages, provider fidelity, diagnostics, and UI
chrome as separate concerns.
