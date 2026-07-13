# Software Requirements Document: fyai

**Version:** 0.11 draft
**Status:** Phase 1
**Author:** Pantelis Antoniou

This document describes the implemented and supported Phase 1 contract. It supersedes earlier design material that mentioned sessions, named branches, provider presets, repository YAML sidecars, \`--yolo\`, \`--dry-run\`, or portable session bundles.

-----

## 1. Purpose

\`fyai\` is a stateless command-line AI coding assistant. One invocation opens the local durable arena, runs a complete tool-use loop or management command, publishes canonical state when required, and exits. There is no daemon, resident process, TUI, or hidden process state.

Conversation state, repository configuration, catalogue snapshots, and turn metadata are durable \`fy_generic\` values in a libfyaml content-addressed arena. The process is stateless between invocations; the arena is the source of truth.

## 2. Design Principles

**Unix-shaped operation.** \`fyai\` reads prompts from arguments or stdin, writes canonical response content to stdout, writes diagnostics and optional statistics to stderr, and returns a conventional exit status.

**Provider-independent durable content.** Provider wire streams are preserved for observability, while canonical conversation content remains independent of the API grammar used to produce it.

**Immutable canonical state.** Published arena values are immutable, deterministic, and address-stable. Persistent arena relocation is forbidden.

**Explicit configuration and credentials.** YAML configuration stores intent; the catalogue resolves provider details. API keys are environment references or explicit command-line values, never raw configuration values.

**Small tool surface.** The model may read files, apply/write files, invoke a shell, and ask the user for input. Tool execution is bounded by configured policy and optional sandboxing.

## 3. Invocation Model

The grammar is:

\`\`\`
fyai [global-options] <verb> [verb-args]
fyai [global-options] <prompt...>
\`\`\`

Global parsing stops at the first non-option. If that token is a known verb it is dispatched as a verb; otherwise the remaining arguments form a prompt. A lone \`-\` reads the prompt from stdin. No prompt on a terminal, or \`-i\`, starts the line-oriented interactive REPL.

Supported verbs are \`init\`, \`dump\`, \`history\` (\`display\` is an alias), \`stats\`, \`config\`, \`list\`, \`catalog\`, \`clear\`, \`compact\`, \`context\`, \`api\`, \`log\`, \`sandbox\`, \`gc\`, \`tool\`, and \`help\`. \`fyai help\` is the authoritative command reference for flags and verb arguments.

Management verbs require no API key. A prompt invocation may make multiple model calls and tool calls until it reaches a terminal response, a configured iteration limit, an interrupt, or an unrecoverable error.

## 4. Durable Storage

### 4.1 Arena discovery and root

\`fyai init\` creates \`./.fyai/arena/\`. Subsequent commands discover the nearest \`.fyai\` directory by walking upward from the current working directory, unless \`arena_dir\` explicitly selects an arena.

The arena root is an immutable container mapping:

\`\`\`
{ fyai: 1, config: <mapping|null>, catalog: <mapping|null>,
  head: <turn|null>, prev: <root|null> }
\`\`\`

Each publish CAS-updates the allocator refs pointer to a new root. \`prev\` forms a reflog: a turnless configuration update is a first-class root update and does not discard prior conversation state. This is audit/history machinery, not a user-facing branch API.

### 4.2 Address stability and lifecycle

Persistent libfyaml arenas map at their configured fixed virtual addresses in every process. Pointer identity is consequently usable for canonical sharing across processes. Arena content is immutable after publication; roots are the only mutable publication point.

\`clear\` publishes a null conversation head. \`compact\` makes one summary model call and starts a fresh chain, retaining the previous head as \`compacted_from\` metadata. \`gc\` compacts unreachable data and requires arena quiescence; \`gc --keep-reflogs N\` first bounds the retained root-reflog window.

### 4.3 Transient mode

\`--transient\` stacks in-memory builders over the durable arena. Configuration edits and conversation state behave normally within the invocation but are not published to the arena.

## 5. Configuration and Catalogue

Configuration is YAML parsed through libfyaml. The persisted repository configuration is the \`config\` entry in the arena root; there is no \`.fyai/config.yaml\` sidecar.

The user file at \`$XDG_CONFIG_HOME/fyai/config.yaml\` (or \`~/.config/fyai/config.yaml\`) is bootstrap-only: it supplies the base only when the arena has no configuration. The merged document is, in order:

\`\`\`
arena configuration (or user bootstrap) -> --config file -> --set deltas
\`\`\`

Built-in defaults back keys absent from that document. \`config effective\` emits the merged document; \`config show\` emits the stored arena document. \`config get\`, \`set\`, and \`delete\`, as well as repeatable global \`--get\`, \`--set\`, and \`--delete\`, use slash paths and YAML-flow typed values.

The top-level \`model\` is the provider-selection input. The catalogue maps it to a canonical provider, endpoint URL, API grammar, and wire model ID. A \`provider/model\` form pins a catalogue provider offering that model. Resolved catalogue values are read-only derivations; they are not configuration presets and are not persisted as user intent. The arena's \`catalog\` block is refreshed when configuration commits resolve a known model or when \`catalog import\` re-syncs the current model.

\`api_key\` in YAML must be an indirection. The default \`{ type: auto }\`
resolves, after the provider is known, in this order: explicit \`--api-key\`,
the conventional \`<PROVIDER>_API_KEY\` environment variable, then logical
secret \`api-key/<provider>\`. \`{ type: env, value: NAME }\` and
\`{ type: secret, value: NAME }\` pin an explicit source. Kernel entries use the UID
persistent keyring through direct syscalls on Linux, remain volatile across
reboot, and never enter the arena; that backend is compiled out on macOS.
\`fyai secret set|status|delete\` and the matching \`/secret\` family manage
logical names without exposing values. \`--env\` accepts
literal \`.env\` assignments, rejects variable substitution, and exports only
variables fyai actually uses.

The top-level \`auth\` intent is \`auto | api-key | chatgpt\` (default
\`auto\`). Auto preserves all API-key precedence and uses a saved ChatGPT
login only when no key resolves. ChatGPT authentication is restricted to the
OpenAI Responses provider, forces the ChatGPT Codex backend with \`store:false\`,
and refuses custom endpoints or response chaining so subscription bearer
tokens cannot be redirected.

\`fyai auth login\` implements browser authorization-code login with PKCE and
a loopback callback; \`--device-code\` supports headless machines. \`auth
status\` displays non-secret account/workspace/plan metadata and \`auth
logout\` performs best-effort revocation before deleting local credentials.
Access, refresh, and ID tokens are the explicit exception to arena-only
persistence: they must never enter an immutable arena. They live in the
machine's macOS Keychain or, when explicitly enabled at build time, Linux
Secret Service keyring; otherwise they use an atomic mode-0600
\`$XDG_STATE_HOME/fyai/auth.json\`. Linux Secret Service support is opt-in
because libsecret carries the GLib runtime dependency; static builds use the
file backend. A cross-process lock
protects refresh-token rotation between concurrent stateless invocations.

Subscription requests use the Codex source contract rather than the public
OpenAI API contract: the ChatGPT Codex endpoint, bearer token,
\`ChatGPT-Account-ID\`, optional FedRAMP routing header, live account-scoped
model catalogue, proactive refresh, and one refresh/retry after HTTP 401.
This is an upstream compatibility surface and must be revalidated when the
pinned Codex implementation changes.

Project \`AGENTS.md\` and \`CLAUDE.md\` instructions are folded into the system prompt only for a new conversation. Global instructions and files from the project root through the current directory are concatenated outermost-first; continuations preserve the already-frozen canonical system turn.

## 6. Provider Requests and Canonical Turns

The supported provider grammars are OpenAI Responses, Chat Completions, and Anthropic Messages. Request and response JSON are constructed and parsed as libfyaml generics; compact JSON exists only at the provider boundary.

A persisted turn links to its predecessor and records canonical content, provider-specific stream data, metadata, and normalized usage. Provider request IDs, tool-call IDs, finish reasons, timestamps, and wire details are not semantic canonical content. A logical assistant turn may contain multiple model/tool steps.

Streaming output is observable, not authoritative. Completed turns are published canonically. During an interactive Ctrl-C, completed steps are preserved and the in-flight step diagnostic is reported; batch mode keeps the default signal dispositions.

The \`reasoning.effort\` and \`reasoning.summary\` configuration values are sampling/request parameters. They do not affect canonical equality. Current wire translations support Responses and Chat Completions; expanding this into a provider-specific translation layer remains future work.

## 7. Tools, Sandboxing, and Secrets

The tool surface is \`read_file\`, structured file writing/patching, \`shell\`, and \`ask_user\`. Tool calls are represented distinctly from assistant text so they can be replayed and rendered. \`tool\` runs one named tool without a model call.

The \`sandbox\` configuration enables Landlock confinement for shell tool subprocesses on Linux, on by default. It supports project-relative and external allow/deny grants plus optional TCP-port restrictions. The \`.fyai\` arena is always denied to sandboxed tools. Landlock is best-effort on unsupported platforms; the configured approval/policy behavior remains the portable control plane.

Independently of the \`sandbox\` setting, \`read_file\`, \`write_file\`, and \`apply_patch\` always refuse a path that resolves to the \`.fyai\` arena or anything beneath it. This portable, lexical check runs on every platform and requires no opt-in; Landlock remains the syscall-level boundary for \`shell\` and for symlink-based evasion of the lexical check.

Secrets are never persisted as raw YAML values. Wire logging can redact API keys, and \`whitewash_api_keys\` defaults to enabled.

## 8. Human-Facing Views and Observability

\`history\` renders the canonical conversation as a readable markdown-oriented view. It is deliberately not a faithful serialization; use \`dump state\`, \`dump anchors\`, or \`dump providers\` for YAML inspection of canonical state, turn graph/metadata, or provider streams respectively.

\`list\` reports catalogue, turn, exchange, and reflog summaries. \`stats\` sums persisted normalized token/cost usage over the current turn chain; the \`stats\` configuration option reports the current invocation's usage to stderr. \`context\` reports context-window fill using catalogued capacity, recorded usage, and a tokenizer-free bytes/4 estimate.

## 9. Platform and Performance

fyai is Linux-first and supports normal Unix command-line use. Fixed arena mapping is currently specified for the supported 64-bit process layouts. Stateless startup and durable replay must remain fast enough for repeated interactive and scripting use.

## 10. Deferred Work

- Provider-specific reasoning translation beyond the current OpenAI request shapes, with reasoning metadata events separated from canonical content.
- The three-layer canonical turn schema described by the design notes.
- Any future session/branch/bundle model, if adopted, requires a new explicit storage and command contract; it is not a Phase 1 feature.
