# Software Requirements Document: fyai

**Version:** 0.11 draft
**Status:** Phase 1
**Author:** Pantelis Antoniou

*Revision note (0.11): phase-1-only cut of the SRD. Deferred phase references and companion appendices have been removed. The remaining text describes the stateless CLI, canonical arena storage, configuration, provider access, tool execution, and human-facing views currently implemented or actively supported in the tree.*

-----

## 1. Purpose

fyai is a stateless command-line AI coding assistant. It performs AI-assisted tasks as a standard Unix tool: starts, does work, exits. Conversation history, parsed context, tool state, and configuration persist in files and content-addressed storage rather than in a resident process.

The name is pronounced “FYI.” The binary is `fyai`.

-----

## 2. Design Principles

**Stateless between invocations.** No daemon, no resident process, no hidden process state. Each invocation is self-contained. State that needs to persist lives in files.

**Stateful within an invocation.** A single invocation runs the full tool-use loop: many model calls, many tool executions, many turns of reasoning.

**Composability with Unix.** stdin, stdout, stderr, exit codes, pipes. fyai is usable as a building block in shell pipelines, makefiles, git hooks, and CI jobs without special accommodation.

**Files as the source of truth.** Conversation history, tool state, configuration, and trust policy all live in files. Anything fyai needs to remember between invocations is on disk in a form a human can read and a tool can diff.

**Address-stable persistent storage.** Persistent arenas are mapped at fixed, configured base addresses across all processes. This preserves canonical 64-bit pointer identity across process boundaries, which is the foundation of cross-process structural sharing and dedup. Relocation is forbidden for persistent storage.

-----

## 3. Non-Goals

- TUI or interactive full-screen interface.
- Resident process, daemon, or background service.
- Lock-in to a single model provider or inference backend.
- Built-in editor, file browser, or any UI beyond stdin/stdout streaming.
- Multi-machine shared arena access.

-----

## 4. Functional Requirements

### 4.1 Invocation Model

fyai accepts a task description as command-line argument, stdin, or both. It executes the task, which may involve many internal model calls and tool uses, and exits with a status code reflecting outcome.

```
fyai 'write a commit message'
make 2>&1 | fyai 'explain this build error'
git diff --name-only | xargs -P8 -I{} fyai 'review {}'
fyai --session=design 'continue the discussion'
```

### 4.2 Session Management

Conversations persist in the project fyai arena. Default session location is discovered by walking upward from the current directory looking for a `.fyai/` directory, analogous to git. If none is found, a new session is created in `.fyai/` in the current directory on first write.

A session is a directed acyclic graph of turns. Branches are first-class: free to create, named, switchable, exportable. Convergent branches that arrive at identical conversation states share storage and identity by construction.

Required operations: create session, list branches, switch branch, fork branch, prune branch, export branch as portable bundle, import bundle.

### 4.3 Tool-Use Loop

A single invocation drives a complete tool-use loop until the task is finished or a stop condition is met:

- Model produces output, possibly including tool-call requests.
- fyai executes requested tools subject to approval policy.
- Tool results are appended to context.
- Loop continues until model produces a terminal response, hits a configured maximum iteration count, or encounters an unrecoverable error.

Stop conditions include maximum iteration count, cost limit, wall-clock timeout, and repetition detection.

### 4.4 Tool Surface

The tool surface is intentionally minimal and Unix-shaped:

- Read file.
- Write file, including structured patch semantics.
- Execute shell command, returning stdout, stderr, exit code.

Higher-level operations are achieved by the model invoking shell commands. The shell is the tool taxonomy.

### 4.5 Approval Policy

Tool execution is governed by config-backed admission policy. The `sandbox` mapping holds path grants, path denies, and optional network limits; the non-interactive default is fail-closed unless the configured policy allows the command. Commands outside the configured policy either prompt the user (interactive mode) or fail (non-interactive mode, e.g. CI).

A `--yolo` flag bypasses prompts entirely.

`--dry-run` is limited to single-shot generation: it surfaces the first proposed tool call or terminal response without executing it, and stops.

### 4.6 Streaming Output

Model output streams to stdout as it is generated. Tool calls interrupt streaming, execute, and the loop resumes with results appended. The user sees progress in real time; long invocations are not silent.

Reasoning text, tool calls, tool results, and final output are visually distinguished on the terminal but produced as a single stdout stream suitable for piping. A `--quiet` flag suppresses everything except the final response. A `--json` flag produces structured output for programmatic consumption.

**Observability vs. canonicality.** Streamed stdout output is observable but not authoritative. Only canonical turns committed to the arena are authoritative.

### 4.7 Provider Abstraction

fyai supports multiple inference backends through a thin provider interface:

- Cloud APIs.
- Local inference.

Provider selection is per-invocation or per-session through configuration. The canonical content layer of the schema is provider-agnostic; switching providers mid-session is supported. Provider-specific wire-format streams and metadata are stored alongside canonical content but do not break canonical identity.

### 4.8 Configuration

Configuration is layered:

- System default.
- User config (`$XDG_CONFIG_HOME/fyai/config.yaml`).
- Repository config (`.fyai/config.yaml`).
- Environment variables.
- Command-line flags.

Later layers override earlier ones. All configuration is YAML, read using libfyaml.

Arena base addresses and reserve sizes are configured under the `arenas` section. The durable project arena base is derived once at `fyai init` time from a stable hash of repository identity, and the resulting value is recorded in the repository config. After init, the recorded value is authoritative for the arena's entire lifetime.

### 4.9 REPL and Slash Commands

The interactive REPL dispatches `/`-prefixed lines as slash commands. Commands share the same backends as the CLI verbs where that makes sense. Slash commands include session management, display controls, model selection, context reporting, stats, and other per-session toggles. A slash line never reaches the model; `//...` escapes to send a literal slash-prefixed prompt.

-----

## 5. Storage Architecture

### 5.1 Single Storage Mechanism

fyai has one storage mechanism: the libfyaml content-addressed arena. All persistent canonical data is stored as `fy_generic` values in this arena. There is no separate session file format, no sidecar store, no parallel filesystem layout.

The arena is otherwise immutable; the single category of mutable state is the head pointer of the branch references directory generic, updated atomically via the same CAS mechanism that the arena uses for chunk-list publication and allocator bump pointers.

### 5.2 Arena Stack

The arena is stacked, typically in three layers:

- **Project arena** at `.fyai/arena/` within a project root: durable across invocations and travels with the repository when archived. Holds canonical content, provider streams, metadata, and branch references scoped to that project.
- **Scratch arena**: in-memory, lives only for the duration of one fyai invocation. Holds in-progress turns during streaming and any work not yet committed to a persistent layer.

Lookup walks the stack from top to bottom; allocation goes to the topmost writable layer. Canonicalization is global within a stack: any logical value has exactly one 64-bit generic representation, making value equality equivalent to pointer equality.

Each persistent arena layer is implemented as a directory containing one or more chunk files. The directory is the unit of arena identity; the chunk files within it collectively comprise the arena's address space and contain all persistent state, including branch references.

### 5.3 VMA Layout and Address Stability

Each persistent arena occupies a fixed virtual address range. The range is reserved at process startup before any other allocation has a chance to claim it.

The default address layout reserves regions in the upper half of the user address space, well above typical heap/mmap/stack regions but below the loader's preferred mmap base. The sizes and base addresses are configurable per arena and recorded in each arena's bootstrap chunk.

Persistent arena chunks are subsequently mapped with `MAP_FIXED` over portions of the reserved region. Relocation is not attempted at map time. This is necessary because relocation would break cross-process pointer identity: process A mapping the arena at one base and process B mapping it at another would produce non-pointer-comparable representations of the same canonical generic.

### 5.4 Multi-Chunk Arena Layout

A persistent arena is a sequence of fixed-size chunk files within the arena directory. Each chunk is a separate file mapped at a deterministic address within the arena region.

Chunk 0 is special: it always exists, it cannot grow, and it holds the synchronization roots for the rest of the arena. Specifically, chunk 0's header contains:

- Magic number and arena format version.
- Expected region base address.
- Region size and chunk size.
- Generation counter.
- Head pointer to the singly-linked chunk list.
- Refs head pointer, pointing at the current branch references directory generic.
- Bootstrap allocator state for chunk 0's own allocation region.

Chunk 0 is created at `fyai init` time as a single-writer operation. Its size is fixed at creation; it never grows. The remaining chunks are created on demand.

### 5.5 Chunk Growth Protocol

When a process exhausts the allocable space in the current set of chunks, it grows the arena by adding a new chunk. The protocol is optimistic, lock-free, and resolves contention by discarding wasted parallel work rather than serializing.

The winner publishes the new chunk by CAS on the chunk list head. Losers discard their work and retry against the published head. No process can publish a chunk at the same generation twice.

### 5.6 Immutability and Concurrency

Canonical arena content is immutable once published. Concurrent readers may traverse the arena without locking. Writers allocate new generics and publish them with compare-and-swap on the relevant root or directory head.

### 5.7 Cache vs. Data Distinction

The arena stores canonical generics only. Reconstructible non-canonical data, such as parse caches, may use separate transient caches, but they are not part of canonical identity.

### 5.8 Parse Cache Relocation (Transient Only)

Transient parse caches may relocate because they are not part of the canonical arena contract and are not address-identity load-bearing.

### 5.9 Parallel Access and Memory Sharing

Multiple processes may map the same arena concurrently. The kernel page cache provides sharing of physical pages for identical mapped chunks.

### 5.10 Branch References Directory Generic

The branch references directory generic is the mutable root for session navigation. It stores the current head of the active branch and any named branch heads.

### 5.11 Xmit Cache

The xmit cache is a transient acceleration structure for serialized provider I/O and does not participate in canonical identity.

### 5.12 Garbage Collection

Garbage collection is a quiesced operation that removes unreachable canonical data and compacts the arena. The arena remains content-addressed and deterministic after compaction.

### 5.13 Required libfyaml Invariants

Canonical generics obey deterministic emission, immutable views, and stable identity within the arena. Values that are part of canonical content must round-trip through libfyaml without changing identity.

-----

## 6. Schema

### 6.1 Reference Structure

A turn consists of canonical content, provider streams, metadata, and references to earlier content as needed for branch navigation and replay.

### 6.2 Canonicalization Layers

Canonical content is the semantic turn record. Provider-specific wire details, transport diagnostics, and other observability data are separate from canonical content unless they are required to reconstruct meaning.

### 6.3 Canonical Content

Canonical content contains the user-visible and model-visible message sequence for the turn.

### 6.4 Multi-Step Assistant Turns

Assistant turns may be composed from multiple internal steps, including tool calls and tool results, while still appearing as one logical response to the user.

### 6.5 Tool Call Identity

Tool calls are represented distinctly from normal assistant text so they can be rendered and replayed correctly.

### 6.6 System Prompts

System prompts are part of the canonical context and are preserved across invocations.

### 6.7 Streaming and Partial Turns

Partial streamed output is allowed during execution, but only completed canonical turns are authoritative.

### 6.8 Refusals, Errors, and Non-Standard Outcomes

Refusals, tool failures, and other non-standard outcomes are represented explicitly rather than being collapsed into generic failure text.

### 6.9 Branch Heads and Turn Sequences

Branch heads identify the current continuation of a session. Turn sequences are append-only, with branch navigation achieved by choosing a different head.

-----

## 7. Human-Facing Views

### 7.1 YAML as a Derived View

YAML is a derived view of the arena state for inspection and debugging. It is not the canonical storage format of user-facing terminal output.

### 7.2 Editability

Human-facing views are editable only through supported commands and structured patch operations, not by manually mutating canonical arena bytes.

### 7.3 Export and Bundling

Sessions can be exported and imported as bundles for portability and review.

-----

## 8. Performance Requirements

Context reload should be fast enough that stateless invocation remains practical for normal interactive use. Warm-start cost should stay low enough that repeated invocations do not feel materially slower than a resident process.

-----

## 9. Platform Requirements

fyai is Linux-first. The address-stability and fixed-mapping model is specified against Linux on x86-64 and ARM64. Other platforms are secondary and may require platform-specific address selection and failure modes.

-----

## 10. Security and Trust

Security boundaries are explicit. The command policy is reviewable in config, `--yolo` is an opt-in bypass, and secrets are not stored as raw values in configuration.

-----

## 11. Open Questions

- Which provider capabilities should be treated as mandatory for the first stable release.
- How much of the REPL slash-command surface should remain session-only versus promoted to persistent configuration.
- How to present history, exchange, and turn views without confusing internal storage structure with user-visible conversation structure.
