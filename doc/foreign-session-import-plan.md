# Foreign session import

## Goal

Import local Claude Code and OpenAI Codex sessions into fyai so that their
portable final context can be continued with fyai's current model, system
prompt, tools and permissions.

The feature discovers sessions for the current directory, accepts explicit
JSONL files, and exposes foreign sessions in the resume picker.  Import is
offline: it neither calls a provider nor executes a recorded tool.

## Command line and picker

Extend `fyai import` with these forms:

```text
fyai import -i conversation.md
fyai import --from claude-code -i session.jsonl
fyai import --from codex -i rollout.jsonl
fyai import --from auto -i session.jsonl
fyai import --list [--from SOURCE] [--all] [--json]
fyai import --from SOURCE --session ID [--snapshot]
fyai import --from SOURCE -i FILE --dry-run [--json]
```

`SOURCE` is `fyai`, `claude-code`, `codex`, or `auto`.  Plain import keeps the
current native Markdown/stdin behavior.  `--source-root` overrides the state
directory for one concrete foreign source.  Foreign import rejects
`--ignore-compact`.

The resume picker starts with fyai sessions in the branch hierarchy.  The `f`
key toggles unimported Claude Code and Codex sessions.  They join imported
sessions under the `import/claude-code` and `import/codex` groups.  Its
all-directories view widens all three sources.  Selecting an unimported foreign
session imports it and continues the new branch for that invocation without
changing stored `HEAD`.  Selecting an imported session resumes its fyai branch.

By default import creates `import/<source>/<session-id>`.  `-b` selects a new
or empty destination.  Repeating an import resumes the associated branch;
`--snapshot` creates a digest-suffixed snapshot instead of synchronizing an
existing branch.

## Conversion architecture

Use a shared pipeline with source-specific adapters:

```text
discover/select -> snapshot -> parse -> reconstruct source context
                -> normalize -> validate -> publish one branch-table update
```

Parse JSON directly into libfyaml generics.  Keep the native Markdown parser
unchanged and do not use Markdown as an intermediate foreign format.

Claude Code discovery scans `projects/` beneath `CLAUDE_CONFIG_DIR` or
`~/.claude`.  Exclude subagent JSONL files.  Reconstruct the active path using
UUID/parent links and active-leaf metadata.  Coalesce assistant records that
carry blocks of one response.  Import user and assistant content, tool-use and
tool-result blocks, compaction summaries, preserved segments, and rewinds.

Codex discovery scans `sessions/` and `archived_sessions/` beneath
`CODEX_HOME` or `~/.codex`.  Treat `response_item` records as conversation
content.  Use event records only for lifecycle and display metadata so content
is not duplicated.  Apply `compacted` replacement histories and rollback
records rather than appending them to the context they replace.

Both adapters produce ordered exchanges, a portable active context, archived
context segments, source provenance, attachments, and loss records.  Replace
foreign tool IDs with deterministic import-local IDs after reconstruction.
Historical calls are inert and never enter the tool scheduler.

Use fyai's current system prompt and configuration for continuation.  Foreign
instructions, model choices, permissions, hooks, credentials, opaque reasoning
and response IDs remain provenance only.  Preserve readable reasoning for
display where supported.

## Context, attachments, and losses

Keep active context separate from archived history.  Honor the source's final
compaction and rewind state while retaining recoverable earlier segments for
inspection.  Do not use fyai's `compacted_from` field for foreign boundaries,
because native export interprets it as a compaction operation.

Preserve embedded content supported by fyai.  Resolve spilled tool results
only within the selected session storage directory, rejecting traversal and
symlink escape.  Never fetch remote transcript URLs or read arbitrary workspace
paths.  Unsupported or unavailable content becomes a labeled placeholder and
a persisted loss record.  Import succeeds with precise warnings.

Malformed interior JSONL records, cycles, contradictory graph structure, and
ambiguous automatic detection fail before publication.  A partial final record
is skipped with a warning.  Read a captured initial file length so concurrent
appends cannot change the snapshot being imported.

## Storage and publication

Keep root version 2 and add an optional `import` mapping to a branch entry.  It
records the format version, source, session ID, snapshot digest, source title,
directory and times, adapter version, losses, and archived-context references.
Update branch build, decode, validation, copy, conflict reconciliation and
garbage collection paths to preserve it.  Old entries remain valid.

Build and validate the whole import before publication.  Publish the completed
branch in one branch-table CAS without changing `HEAD`.  On a lost CAS, retain
unrelated branches and recheck that the destination is still absent or empty.
Report destination conflicts separately from parsing, allocation and arena
capacity failures.  `--root` allows listing and dry-run but rejects import;
`--transient` keeps the imported state in memory.

Store normalized canonical messages and durable display documents.  Preserve
selected source records in provider-stream generics tagged by source.  Native
Markdown export writes the portable active conversation; it does not claim to
be a lossless source export or regenerate foreign compactions.

Add `transcript --imported-history` to render archived segments with explicit
compaction, rewind, and abandoned-path labels through the existing transcript
renderer.

## Implementation stages and tests

1. Add synthetic source fixtures and implement detection, bounded JSONL
   parsing, reconstruction, normalization, and dry-run reports.
2. Add branch import metadata, atomic publication, explicit file/session
   import, and loss reporting.  The initial implementation persists structured
   losses and inserts labeled placeholders for unsupported content.
3. Add discovery, `/import`, and mixed-source resume-picker rows.  Discovery
   and progressive picker rows are implemented: native sessions render first,
   then bounded Claude and Codex scans update the picker.  The interactive
   `/import` command remains.
4. Add archived-history rendering, documentation, and compatibility tests.

Test plain and split messages, parallel and custom tools, missing results,
compaction, preserved segments, rewinds, duplicate events, attachments, CRLF,
Unicode, truncated tails, malformed records, cycles, and unknown content.
Verify request builders for all three provider grammars receive valid portable
context without foreign instructions or opaque state.  Verify restart, rename,
reset, configuration commits, GC, idempotent reimport, snapshot import, CAS
conflicts, cancellation, read-only roots, and arena exhaustion.  Add PTY tests
for picker filtering, resize, cancellation, import-and-resume, and preservation
of stored `HEAD`.  Keep existing native import/export and sink-only tests
passing.
