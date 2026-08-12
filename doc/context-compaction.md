# Context compaction

Long-running conversations eventually need context reduction. The three API
grammars supported by fyai do not expose the same mechanism, so compaction is
implemented per grammar rather than as one provider-neutral model prompt.

## API support

- **OpenAI Responses:** supports `POST /v1/responses/compact` and automatic
  compaction through `context_management`. fyai uses the standalone endpoint
  for an explicit `/compact`.
- **OpenAI Chat Completions:** has no native compaction operation. fyai asks
  the model for a summary and restarts from it.
- **Anthropic Messages:** supports the beta `compact_20260112`
  context-management edit. Native support in fyai is planned; the model-summary
  fallback remains in use until then.

Anthropic Messages also supports context-editing strategies that clear old tool
results or thinking blocks. Those are useful context controls, but they are not
equivalent to full conversation compaction.

## Responses implementation

For an OpenAI Responses session, `/compact`:

1. Reconstructs the full Responses input-item window.
2. Sends it to the provider's `/responses/compact` endpoint with the model,
   instructions, and tools used by the conversation.
3. Treats the returned `output` array as opaque. It may contain retained
   messages as well as an encrypted `compaction` item.
4. Starts a new local turn chain containing the system turn followed by that
   output array.
5. Keeps the former head reachable as `compacted_from` provenance metadata.

The returned array must not be summarized, parsed into prose, or selectively
pruned. It is the canonical input window for the next Responses request.

## Fallback implementation

Chat Completions and currently Anthropic Messages use the portable fallback:
append a request asking the model to summarize the conversation, then replace
the old chain with a system turn and a user message containing that summary.

The summary request uses a bounded copy of the history. Thus, compaction can
operate when the context is full. The following reductions apply in order:

1. The content of any message that alone takes a large share of the budget is
   elided. One oversized tool result is the usual cause.
2. The oldest messages are dropped until the rest fits.

Each reduction adds a marker. The model can then identify a partial history.
Only `content` is replaced. The role, `tool_calls`, and `tool_call_id` remain,
so call and result messages remain paired. The stored head does not change.
Thus, `compacted_from` records the complete conversation.

A tokens-per-minute limit can still fail the request.

## Keeping a request inside the window

Compaction reduces a conversation that has grown too large. Two rules keep it
from growing past the window in the first place.

**A bounded tool result.** Both tools that return file or command text bound
what they return, because one call could otherwise put more tokens into the
conversation than the window holds.

- `read_file` reads at most `read/max_bytes`, bounding a model-supplied
  `max_bytes` by `read/hard_max_bytes`. It also takes `offset` and `limit` to
  read a window of lines. Use the reported `offset` to continue at a line
  boundary. Use `offset_bytes` to continue after a byte-limited result.
- `shell` returns at most `shell/max_output_tokens`, bounding a
  model-supplied `max_output_tokens` by `shell/hard_max_output_tokens`. The
  end of the output is kept, because a command reports its conclusion last,
  and standard error is served first so the reason a command failed survives
  a noisy standard output.

The shell limit is configured in tokens because that is the unit of the
budget it protects; bytes follow at four per token, the estimator's rule.

**A checked request.** The catalogue `context_window` is enforced, not only
displayed. Before each request:

- The prompt is measured against the window. A prompt that does not fit is
  refused, with the recoveries named. This keeps the completed turns intact:
  a conversation is append-only, so a prompt the provider rejects for size is
  rebuilt by every retry.
- The output allowance is reduced to the room the prompt leaves. Asking for
  more output than the window has left is too generous, not fatal, so it is
  corrected rather than refused.

A model whose catalogue entry declares no `context_window` is not checked. A
compaction request is not checked, because it is the recovery.

One rule computes the output allowance, and both the request builder and the
fill gauge use it, so what `fyai context` and the interactive footer report is
what the next request asks for. A reduced allowance is reported as such rather
than shown as an over-full window.

### Prompt size and its two sources

A prompt size comes from one of two places, and they are kept apart in
`struct fyai_context_prompt` rather than reduced to one number:

- **Measured.** The input tokens the provider counted, normalized across the
  three grammars. It is exact, but it describes the request that was already
  sent. It comes from the last call in this process, or from the usage stored
  on the turn chain; `fyai context` names which.
- **Estimated.** Canonical bytes divided by 4, plus a small per-message
  overhead, including the tool schema. It describes the request about to be
  sent, so it alone accounts for a pending user message and for tool results
  appended since the last call.

The two cover different spans, so the projection takes the larger and records
which one it used. The estimate normally wins directly after a tool result
lands; the measurement wins at the start of a turn.

`bytes / 4` suits English prose. Source code and escaped payloads are denser,
so the estimate reads low for a code-heavy conversation, and a projection
driven by it errs toward letting a request through rather than refusing a
valid one.

## Planned Messages implementation

Native Anthropic compaction requires the `compact-2026-01-12` beta header and a
`context_management.edits` entry of type `compact_20260112`. The returned
compaction content block must be preserved and round-tripped in later Messages
requests. Model capability and beta-header configuration need to be represented
in the provider catalogue before this replaces the fallback.

## References

- OpenAI: <https://developers.openai.com/api/docs/guides/compaction>
- OpenAI endpoint: <https://developers.openai.com/api/reference/resources/responses/methods/compact>
- Anthropic: <https://platform.claude.com/docs/en/build-with-claude/compaction>
- Anthropic context editing: <https://platform.claude.com/docs/en/build-with-claude/context-editing>
