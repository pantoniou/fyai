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
This requires the original conversation to fit in a normal model request and
can therefore fail on context-window or tokens-per-minute limits.

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
