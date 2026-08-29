# Software Requirements Document: fyai

**Version:** 0.12 draft
**Status:** Product-aligned software contract draft
**Author:** Pantelis Antoniou
**Product requirements:** `doc/prd/fyai-prd.md`

This document describes the implemented and supported software contract for fyai and the requirements that realize the 0.12 product baseline. The PRD defines product intent and user-visible invariants; this SRD defines the software behavior required to realize them. Focused design documents define implementation mechanisms and may not override PRD product intent.

Stable `PRD-*` identifiers are included where a software requirement directly realizes a product requirement. They are traceability references, not substitutes for the software behavior stated here.

This document supersedes earlier design material that mentioned sessions, provider presets, repository YAML sidecars, `--yolo`, `--dry-run`, or portable session bundles.

-----

## 1. Purpose

`fyai` is a stateless command-line AI coding assistant. One invocation opens the local durable arena, runs a complete tool-use loop or management command, publishes canonical state when required, and exits. An interactive invocation owns a terminal UI for its process lifetime, but there is no daemon, resident process, or hidden process state. (`PRD-LIFE-001`, `PRD-LIFE-003`, `PRD-LIFE-004`)

Conversation state, repository configuration, catalogue snapshots, turn metadata, provider observations, and durable human-facing display output are stored in the local content-addressed state model as applicable. The process is stateless between invocations; the arena is the source of truth for durable project agent state. (`PRD-LIFE-002`, `PRD-STATE-001`)

## 2. Design Principles

**Unix-shaped operation.** `fyai` reads prompts from arguments or stdin, writes canonical response content to stdout, writes diagnostics and optional statistics to stderr, and returns a conventional exit status. (`PRD-CLI-001`, `PRD-CLI-002`)

**Provider-independent durable content.** Provider wire streams are preserved for observability, while canonical conversation content remains independent of the API grammar used to produce it. (`PRD-STATE-002`, `PRD-PROVIDER-001`, `PRD-PROVIDER-003`)

**Immutable canonical state.** Published arena values are immutable, deterministic, and address-stable for retained state. (`PRD-STATE-003`)

**Explicit configuration and credentials.** YAML configuration stores intent; the catalogue resolves provider details. API keys are environment references, machine-local secrets, or explicit invocation values, never raw durable project configuration values. (`PRD-SECRET-001`, `PRD-SECRET-002`, `PRD-SECRET-003`)

**Small tool surface.** The model may read files, apply/write files, invoke a shell, ask the user for input, and delegate one restricted sub-agent. Tool execution is bounded by configured policy and optional sandboxing. (`PRD-TOOL-001`, `PRD-TOOL-002`, `PRD-TOOL-004`)

**Capability truthfulness.** When the selected provider, model, API grammar, platform, or confinement mechanism cannot realize a requested capability, fyai reports the limitation and does not silently claim equivalent behavior. (`PRD-PROVIDER-004`, `PRD-TOOL-003`)

## 3. Invocation Model

The grammar is:

```
fyai [global-options] <verb> [verb-args]
fyai [global-options] <prompt...>
```

Global parsing stops at the first non-option. If that token is a known verb it is dispatched as a verb; otherwise the remaining arguments form a prompt. A lone `-` reads the prompt from stdin. No prompt on a terminal, or `-i`, starts the line-oriented interactive REPL.

Supported verbs are `init`, `dump`, `transcript` (with `history` and `display` aliases), `stats`, `config`, `list`, `catalog`, `branch`, `checkout`, `reset`, `root`, `rebase`, `merge`, `clear`, `compact`, `context`, `api`, `auth`, `secret`, `log`, `sandbox`, `mcp`, `gc`, `tool`, `agent`, and `help`. `fyai help VERB` is the detailed command reference for the portions exposed by per-verb help.

Inspection, configuration, and storage-management verbs require no provider API key. Prompt, compaction, and agent runs may make model calls and therefore require the selected authentication. A prompt invocation may make multiple model calls and tool calls until it reaches a terminal response, a configured iteration limit, an interrupt, or an unrecoverable error. (`PRD-CLI-003`)

## 4. Durable Storage

### 4.1 Arena discovery and root

`fyai init` creates `./.fyai/arena/`. Subsequent commands discover the nearest `.fyai` directory by walking upward from the current working directory, unless `arena_dir` explicitly selects an arena.

The arena root is an immutable container mapping:

```
{ fyai: 2, catalog: <mapping|null>, HEAD: <branch-name>,
  branches: <mapping|null>, prev: <root|null> }
```

Each branches entry is keyed by the branch's full name and holds that branch's own state:

```
{ config: <mapping|null>, head: <turn|null>, created: <integer>,
  description: <string|null>, agent: <mapping|null>,
  op: <string>, from: <string|null>,
  prev: <entry|null> }
```

The conversation head and the repository configuration belong to a branch, not to the root: branches are independent lines of work. Only the catalogue is arena-wide, being an ingested provider snapshot rather than user intent. Version 2 is not compatible with version 1 and no migration is attempted; any other version is rejected. (`PRD-BRANCH-001`, `PRD-BRANCH-002`, `PRD-BRANCH-003`)

Each publish CAS-updates the allocator refs pointer to a new root, rebuilding only the active branch's entry and carrying every other branch by reference. A lost CAS is re-read and retried: concurrent invocations on different branches do not conflict merely because another branch moved. A conflict is when the same branch moved, and `branch/on_conflict` (`abort` by default, else `rebase`/`merge`) decides whether to refuse or replay on top. There are two reflog chains: the root's `prev`, spanning the whole arena, and each branch entry's `prev`, spanning that branch alone. A turnless configuration update is a first-class entry on both. `gc --keep-reflogs N` bounds both.

A branch entry records the operation that produced it (`op`, plus `from` on a rename) rather than leaving it to be inferred. Reference indices (`@{N}`) are positional and shift as entries are appended; to keep a point, create a branch at it. See `doc/branching.md`.

### 4.2 Address stability and lifecycle

Persistent libfyaml arenas map at their configured fixed virtual addresses in every process. Arena content is immutable after publication; roots are the mutable publication point.

`branch` lists, creates, deletes, renames, describes and shows branches; `checkout` moves `HEAD`; `reset` moves the active branch's head, which stays recoverable as `<branch>@{1}` until `gc` expires that entry. `rebase` appends another branch's turns and then the current branch's own; `merge` interleaves both branches' exchanges by the time the ref log records for each head. A conversation is append-only, so a join is an ordering decision rather than a textual merge. (`PRD-BRANCH-004`, `PRD-BRANCH-005`, `PRD-BRANCH-006`, `PRD-STATE-005`)

`root print` emits the current root as a stable handle and `--root <handle>` reads that exact retained state; a pinned invocation is read-only and every write is refused rather than skipped. A handle is admitted by walking the root ref log and comparing values; it is never dereferenced on trust. `gc` invalidates handles outside the retained window. (`PRD-STATE-006`)

The global `--branch`/`-b` and `$FYAI_BRANCH` select a branch for one invocation without moving `HEAD`. Turn state is addressed symbolically - `<branch>`, `<branch>~N`, the `<branch>^^` shorthand, or `<branch>@{N}` - because garbage collection can relocate arena objects and a raw object address is not a stable user reference.

`clear` publishes a null conversation head for the active branch. `compact` starts a fresh chain while retaining the previous head as `compacted_from` provenance metadata. `gc` compacts unreachable data and requires arena quiescence; `gc --keep-reflogs N` first bounds the retained root-reflog window. (`PRD-CONTEXT-003`)

### 4.3 Transient mode

`--transient` stacks in-memory builders over the durable arena. Configuration edits and conversation state behave normally within the invocation but are not published to the arena. (`PRD-LIFE-005`)

## 5. Configuration and Catalogue

Configuration is YAML parsed through libfyaml. The persisted repository configuration is the selected branch entry's `config` value; there is no `.fyai/config.yaml` sidecar.

The user file at `$XDG_CONFIG_HOME/fyai/config.yaml` (or `~/.config/fyai/config.yaml`) is bootstrap-only: it supplies the base only when the arena has no configuration. The merged document is, in order:

```
arena configuration (or user bootstrap) -> --config file -> command-line overlays
```

Built-in defaults back keys absent from that document. `config effective` emits the merged document; `config show` emits the stored arena document. `config get`, `set`, and `delete`, as well as repeatable global `--get`, `--set`, and `--delete`, use slash paths and YAML-flow typed values.

Dedicated selectors such as `-m`, `--sandbox`, `--color`, and `--theme` are invocation-only overlays. Global `--set` and `--delete` operations also affect the live invocation and publish their deltas to the selected branch unless `--transient` is active. `--api-key` is a separate invocation-only credential override.

The top-level `model` is the provider-selection input. The catalogue maps it to a canonical provider, endpoint URL, API grammar, and wire model ID. A `provider/model` form pins a catalogue provider offering that model. Resolved catalogue values are read-only derivations; they are not configuration presets and are not persisted as user intent. The arena's `catalog` block is refreshed when configuration commits resolve a known model or when `catalog import` re-syncs the current model. (`PRD-PROVIDER-002`, `PRD-PROVIDER-004`)

`api_key` in YAML must be an indirection. The default `{ type: auto }` resolves, after the provider is known, in this order: explicit `--api-key`, the conventional `<PROVIDER>_API_KEY` environment variable, then logical secret `api-key/<provider>`. `{ type: env, value: NAME }` and `{ type: secret, value: NAME }` pin an explicit source. Kernel entries use the UID persistent keyring through direct syscalls on Linux, remain volatile across reboot, and never enter the arena; that backend is compiled out on macOS. `fyai secret set|status|delete` and the matching `/secret` family manage logical names without exposing values. `--env` accepts literal `.env` assignments, rejects variable substitution, and exports only variables fyai actually uses. (`PRD-SECRET-001`, `PRD-SECRET-002`)

The top-level `auth` intent is `auto | api-key | chatgpt` (default `auto`). Auto preserves all API-key precedence and uses a saved ChatGPT login only when no key resolves. ChatGPT authentication is restricted to the OpenAI Responses provider, forces the ChatGPT Codex backend with `store:false`, and refuses custom endpoints or response chaining so subscription bearer tokens cannot be redirected.

`fyai auth openai login` implements browser authorization-code login with PKCE and a loopback callback; `--device-code` supports headless machines. `auth status` displays non-secret account/workspace/plan metadata and `auth logout` performs best-effort revocation before deleting local credentials. Access, refresh, and ID tokens are the explicit exception to arena-only persistence: they must never enter an immutable arena. They live in the machine's macOS Keychain or, when explicitly enabled at build time, Linux Secret Service keyring; otherwise they use an atomic mode-0600 `$XDG_STATE_HOME/fyai/auth.json`. Linux Secret Service support is opt-in because libsecret carries the GLib runtime dependency; static builds use the file backend. A cross-process lock protects refresh-token rotation between concurrent stateless invocations. (`PRD-SECRET-003`)

Subscription requests use the Codex source contract rather than the public OpenAI API contract: the ChatGPT Codex endpoint, bearer token, `ChatGPT-Account-ID`, optional FedRAMP routing header, live account-scoped model catalogue, proactive refresh, and one refresh/retry after HTTP 401. This is an upstream compatibility surface and must be revalidated when the pinned Codex implementation changes.

Project `AGENTS.md` and `CLAUDE.md` instructions are folded into the system prompt only for a new conversation. Global instructions and files from the project root through the current directory are concatenated outermost-first; continuations preserve the already-frozen canonical system turn.

## 6. Provider Requests, Canonical Turns, and Durable Display

The supported provider grammars are OpenAI Responses, Chat Completions, and Anthropic Messages. Request and response JSON are constructed and parsed as libfyaml generics; compact JSON exists only at the provider boundary.

A persisted turn links to its predecessor and records canonical content, provider-specific stream data, metadata, normalized usage, and durable display outputs when present. Provider request IDs, tool-call IDs, finish reasons, timestamps, and wire details are not semantic canonical content. A logical assistant turn may contain multiple model/tool steps.

The three representations have distinct responsibilities:

- canonical messages are the source for model replay and provider-independent conversation identity;
- provider streams preserve provider-specific observations needed for fidelity, diagnostics, and accounting;
- tagged Markdown `display_outputs` preserve durable human-facing conversational output independently of model replay and provider wire state. (`PRD-STATE-002`, `PRD-PROVIDER-001`, `PRD-PROVIDER-003`, `PRD-TERM-004`)

Streaming output is observable, not authoritative. Completed turns are published canonically. Display output records carry a `system`, `user`, or `assistant` tag and finalized Markdown source; current turns attach these records to the durable turn. Transcript rendering replays durable display outputs when available, with reconstruction from message/provider data reserved for legacy arena state. (`PRD-STATE-004`, `PRD-TERM-002`, `PRD-TERM-004`)

On interrupt, completed steps are preserved and the in-flight step diagnostic is reported. Active operations observe cancellation through the invocation-owned event loop.

The `reasoning.effort` and `reasoning.summary` configuration values are sampling/request parameters. They do not affect canonical equality. Current wire translations support Responses and Chat Completions; expanding this into a broader provider-specific reasoning translation layer remains future work. Unsupported translations or capabilities must be surfaced rather than represented as semantically equivalent when they are not. (`PRD-PROVIDER-004`)

## 7. Tools, Sandboxing, Secrets, and Terminal Execution

The tool surface is `read_file`, `write_file`, `apply_patch`, `shell`, `ask_user`, and delegated `agent`. Tool calls are represented distinctly from assistant text so they can be replayed and rendered. `tool` directly dispatches one named tool without a parent model call. (`PRD-TOOL-001`, `PRD-TOOL-004`)

File and shell results admitted to model context are bounded by configured hard limits. A tool result must not be able to unconditionally consume the entire model context window. (`PRD-TOOL-005`)

The `sandbox` configuration enables Landlock confinement for shell tool subprocesses on Linux. It supports project-relative and external allow/deny grants plus optional TCP-port restrictions. The `.fyai` arena is always denied to sandboxed tools. Landlock is best-effort on unsupported platforms; configured approval/policy behavior remains the portable control plane. The effective UI and diagnostics must not describe unavailable confinement as active protection. (`PRD-TOOL-002`, `PRD-TOOL-003`)

Child execution sanitizes provider credentials before arbitrary programs are executed. Credentials that fyai itself can resolve must not be unintentionally inherited by shell programs merely because they are present in the parent process environment. (`PRD-SECRET-004`)

### 7.1 Sub-agents

A delegated sub-agent is a child process that runs one restricted tool-use loop with all built-in tools except `ask_user` and `agent`. The parent reserves a unique, sanitized `agent:` branch name before spawning it. A taken name is a clash, not an invitation to append an ordinal, so the calling model can retry with a different durable handle. (`PRD-AGENT-001`)

The delegated tool defaults to `context: fork`: its branch begins at the parent's head and keeps the parent's resolved model. `context: fresh` starts from the delegated task and may use a configured persona model. A persona may select instructions, reasoning, temperature, output limit, thinking display, timeout, and default context. In a fork, persona instructions are appended as a user instruction because the canonical system turn remains the request instruction source. (`PRD-AGENT-003`)

Before doing independent delegated work, the child must not retain addressable live execution state that belongs exclusively to the parent or siblings. Parent shell sessions, tool jobs, waits, UI state, MCP connections, event-loop state, and per-run accounting are process-local ownership boundaries, regardless of whether the worker implementation uses `fork()` or `exec()`. (`PRD-AGENT-005`)

Before publishing, the current forked child abandons the inherited event loop and reopens the shared arena as an independent writer. It publishes the complete conversation and agent metadata on its reserved child branch. The parent keeps its own branch and receives the final report as the tool result. (`PRD-AGENT-002`, `PRD-AGENT-004`)

The standalone `fyai agent <task>` verb uses the same run engine but is intentionally transient: it prints the report and writes no arena history. `fyai agent --rpc` exposes that standalone worker over the newline-framed control protocol in `doc/agent-protocol.md`.

A delegated sub-agent sends its tool calls and live output to its parent work band. The parent renders this progress as one Markdown quote. Tool calls use the configured tool-detail policy. Tool results stay hidden. The final report is the last quote update and is also the tool result. The work band shows the agent name and description.

Sub-agents in one assistant message run concurrently in separate child processes. A sub-agent cannot start another sub-agent. A call or configured policy may bound the complete job, including every descendant process.

Secrets are never persisted as raw YAML values. Wire logging can redact API keys, and `whitewash_api_keys` defaults to enabled.

### 7.2 PTY and terminal-backed shell behavior

For supported PTY-backed shell workflows, terminal behavior is a software contract rather than an opaque byte-pipe detail. fyai must interpret terminal output before exposing it as readable model/user output, retain bounded useful output, and propagate supported terminal resize/control behavior to the running program. (`PRD-TERM-003`)

Named shell sessions are invocation-local live programs. They may be addressed for input, output, and close operations while the invocation owns them, but they do not become hidden resident jobs that survive the fyai process. Session shutdown must terminate owned descendant process groups according to the configured execution policy. (`PRD-LIFE-001`, `PRD-TERM-003`)

The terminal implementation may use platform-specific PTY, vterm, surface, and event mechanisms; those mechanisms are design choices as long as the observable contract above is preserved.

## 8. Human-Facing Views, Context, and Observability

`transcript` (also `history` and `display`) renders the durable conversation as a readable Markdown-oriented view. It is deliberately not a faithful serialization; use `dump state`, `dump anchors`, or `dump providers` for YAML inspection of canonical state, turn graph/metadata, or provider streams respectively. Durable display outputs are a human-facing representation and remain distinct from canonical serialization. (`PRD-STATE-004`, `PRD-TERM-004`)

`list` reports catalogue, turn, exchange, and reflog summaries. `stats` sums persisted normalized token/cost usage over the current turn chain; the `stats` configuration option reports the current invocation's usage to stderr.

`context` reports context-window fill using catalogued capacity, recorded usage, and an estimate for pending canonical content. When the selected model has a known context window, fyai checks a normal request before sending it. A prompt known not to fit is refused with an explicit recovery path rather than knowingly sending an oversized normal request. The requested output allowance is reduced to the room that remains when necessary. (`PRD-CONTEXT-001`, `PRD-CONTEXT-002`)

Compaction is the explicit recovery mechanism for an overgrown conversation. Provider-native compaction may be used where supported; other grammars may use a portable fallback. Either path preserves provenance to the prior complete durable history. (`PRD-CONTEXT-003`, `PRD-CONTEXT-004`)

## 9. Platform and Performance

fyai is Linux-first and supports normal Unix command-line use. Linux uses epoll/signalfd/timerfd/pidfd and BSD/macOS use kqueue behind one portable event interface. One invocation-owned event loop is shared by model I/O, tools, MCP, OAuth, readline, signals, and child shutdown. Fixed arena mapping is currently specified for the supported 64-bit process layouts. Stateless startup and durable replay must remain fast enough for repeated interactive and scripting use. (`PRD-PLATFORM-001`, `PRD-PLATFORM-002`, `PRD-PLATFORM-003`)

Platform-specific mechanisms may differ in strength or availability. Differences in event or confinement implementation must not unnecessarily change the core conversation, branch, and lifecycle semantics, and unavailable stronger protection must not be presented as active. (`PRD-PLATFORM-002`, `PRD-TOOL-003`)

## 10. Deferred Work

The following are outside the required 0.12 software contract unless separately promoted through the PRD/SRD process:

- provider-specific reasoning translation beyond the current supported request shapes;
- automatic tracking of a Git repository's branch;
- any bundle or remote-sync model;
- persistent background shell jobs surviving a fyai invocation;
- a fork-to-exec sub-agent rewrite as an end in itself;
- identical sandbox strength on platforms that expose different confinement primitives.

The earlier three-layer canonical-turn work is no longer a license to collapse durable display content into canonical replay content. The software contract explicitly distinguishes canonical messages, provider observations, and durable display outputs; future schema work must preserve those responsibilities.
