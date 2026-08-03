# Textual export format

`fyai export` writes a branch as one Markdown document. The document contains
the provider-independent conversation and its configuration history. It omits
provider request IDs, tool-call IDs, timestamps, and stream data.

Export writes to standard output or `-o <file>`. Import reads from standard
input or `-i <file>`:

```sh
fyai export | ssh host fyai import
fyai export -o conversation.md
fyai import -i conversation.md
```

On export, `--branch`/`-b` selects the source. On import, it selects an empty
destination branch. Import without this option replaces the active branch.

## Directives

A directive starts with `<!-- meta:yaml` at column zero and ends with `-->` at
column zero. The content is one YAML mapping.

```md
<!-- meta:yaml
format: 1
kind: conversation
-->
```

Visible headings present the directive that precedes them. Headings do not
contain state.

The document uses these structural directives:

- `conversation` starts the document and specifies the format version.
- `publish` starts one branch reference-log entry.
- `turn` starts one stored turn.
- `message` contains one message role and is followed by its text.
- `tool_call` contains one tool name, its arguments, and its result.
- `compact` records a compaction operation.
- `config` contains the complete initial configuration.
- `config-update` contains a later configuration change.

Publish and turn directives preserve boundaries that cannot be derived from
messages. Import replays these boundaries in order.

## Configuration

The first publish contains a `config` directive. A later publish can contain a
`config-update` mapping. Each update key is a slash path, as used by
`fyai config set`:

```yaml
kind: config-update
config-update:
  temperature: 0.7
  display/tool_detail: full
  agent/timeout_ms: null
```

A null value removes the key. A sequence is one value and is replaced as a
whole.

## Messages and tool calls

A message directive contains the role. The Markdown text after the directive
is the message content.

```md
<!-- meta:yaml
kind: message
role: user
-->
## User

Inspect the source.
```

A tool-call directive contains the arguments and the result:

```md
<!-- meta:yaml
kind: tool_call
tool: shell
arguments:
  command: rg -n TODO src
result: |
  src/main.c:10: TODO
-->
```

Canonical state stores a call and its result as separate messages. The export
joins them. It does not store a call ordinal or provider call ID. A diff or
merge can therefore move the complete call without changing an identifier.

An unanswered call has no `result` key. Parallel calls use separate
directives. Import creates new call IDs and restores each result in the next
stored turn.

## Compaction

The export records a compaction operation and its instructions. It does not
store the provider output, because that output is bound to the provider and
session.

Import re-issues the operation with the current model and endpoint. Use
`--ignore-compact` to skip compaction markers. An import without a compaction
marker makes no provider request.

## Payloads

Structured payloads are inside directives. They are not in fenced Markdown
blocks. YAML indents block scalars, so payload text cannot put the directive
terminator at column zero.

Only message text is outside a directive. The exporter escapes a directive
opener at column zero by adding one `x`:

| Message text | Exported text |
| --- | --- |
| `<!-- meta:yaml` | `<!-- x-meta:yaml` |
| `<!-- x-meta:yaml` | `<!-- xx-meta:yaml` |

The importer removes one `x`. This rule also escapes an already escaped
opener, so the conversion is reversible.

## Determinism

The export uses canonical content and stable ordering. It omits provider wire
data and generated call identifiers. Equivalent canonical state produces the
same document.
