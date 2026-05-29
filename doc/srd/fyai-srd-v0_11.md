# Software Requirements Document: fyai

**Version:** 0.11 draft
**Status:** Pre-implementation
**Author:** Pantelis Antoniou

*Revision note (0.11): incorporates an architecture review pass. Principal changes: refs-update durability ordering made normative (§5.10); VMA reservation strategy specified concretely (§5.3); arena base derived once at init and never recomputed (§4.8, §5.3); reload/start performance restated as three regimes with measured figures (§8); inference-state reclassified as reconstructible cache addressed by content hash, not as canonical arena data (§5.1, §5.13, Appendix E); branch-point records moved to the metadata layer (§6.1, Appendix D); safe-yolo confidentiality story corrected and `--sandbox` repositioned as defense-in-depth (§4.5, §10, Appendix C); `--dry-run` semantics corrected for the tool-use loop (§4.5); minor canonicalization and wording fixes (§6.5, §6.6, §7.2).*

-----

## 1. Purpose

fyai is a stateless command-line AI coding assistant. It performs AI-assisted tasks (code generation, review, explanation, transformation) as a standard Unix tool: starts, does work, exits. Conversation history, parsed context, and tool state persist in files and content-addressed storage rather than in a resident process.

The name is pronounced “FYI.” The binary is `fyai` to avoid collision with an existing Rust notification tool packaged as `fyi` in Arch and Fedora.

-----

## 2. Design Principles

**Stateless between invocations.** No daemon, no resident process, no PTY management. Each invocation is self-contained. State that needs to persist lives in files.

**Stateful within an invocation.** A single invocation runs the full tool-use loop: many model calls, many tool executions, many turns of reasoning. Statelessness is a property of process lifetime, not of task scope.

**Composability with Unix.** stdin, stdout, stderr, exit codes, pipes. fyai is usable as a building block in shell pipelines, makefiles, git hooks, and CI jobs without special accommodation.

**Files as the source of truth.** Conversation history, tool state, configuration, and trust policy all live in files. Anything fyai needs to remember between invocations is on disk in a form a human can read and a tool can diff.

**Zero-cost context reload.** The libfyaml mmap cache makes parsing prior context O(1). This is the technical foundation that makes statelessness viable: a stateful daemon’s only structural advantage is avoiding context reload, and that advantage is eliminated. Measured warm-start overhead is under 30ms for typical session sizes, well within the 50ms target.

**Local-first compatible.** The architecture must accommodate fully local inference (llama.cpp) as a first-class deployment mode, not as a degraded fallback from cloud APIs.

**Address-stable persistent storage.** Persistent arenas are mapped at fixed, configured base addresses across all processes. This preserves canonical 64-bit pointer identity across process boundaries, which is the foundation of cross-process structural sharing and dedup. Relocation is forbidden for persistent storage and reserved for transient parse caches and bundle import.

-----

## 3. Non-Goals

- TUI or interactive full-screen interface.
- Resident process, daemon, or background service (with one possible exception: a weights-only inference daemon for local models, treated as infrastructure analogous to a database).
- Replicating the exact UX of stateful tools (Claude Code, Aider, Cursor). Where their UX depends on statefulness, fyai will choose differently.
- Lock-in to a single model provider or inference backend.
- Built-in editor, file browser, or any UI beyond stdin/stdout streaming.
- Multi-machine shared arena access. Single-machine concurrency is supported; cross-machine sharing is achieved through bundle export/import, not shared storage.

-----

## 4. Functional Requirements

### 4.1 Invocation Model

fyai accepts a task description as command-line argument, stdin, or both. It executes the task — which may involve many internal model calls and tool uses — and exits with a status code reflecting outcome.

```
fyai 'write a commit message'
make 2>&1 | fyai 'explain this build error'
git diff --name-only | xargs -P8 -I{} fyai 'review {}'
fyai --session=design 'continue the discussion'
```

### 4.2 Session Management

Conversations persist in the fyai arena (see §5). Default session location is discovered by walking upward from the current directory looking for a `.fyai/` directory, analogous to git. If none is found, a new session is created in `.fyai/` in the current directory on first write, or in a global default location if explicitly requested.

A session is a directed acyclic graph of turns. Branches are first-class: free to create (structural sharing makes branching zero-cost), named, switchable, exportable. Convergent branches that arrive at identical conversation states share storage and identity by construction.

Required operations: create session, list branches, switch branch, fork branch, prune branch, export branch as portable bundle, import bundle.

### 4.3 Tool-Use Loop

A single invocation drives a complete tool-use loop until the task is finished or a stop condition is met:

- Model produces output, possibly including tool-call requests.
- fyai executes requested tools subject to approval policy (§4.5).
- Tool results are appended to context.
- Loop continues until model produces a terminal response, hits a configured maximum iteration count, or encounters an unrecoverable error.

Stop conditions are configurable per invocation and per session, and include: maximum iteration count, cost limit, wall-clock timeout, and repetition detection (same tool call repeated N times indicating a stuck model).

### 4.4 Tool Surface

The tool surface is intentionally minimal and Unix-shaped:

- Read file.
- Write file (with optional structured patch semantics).
- Execute shell command, returning stdout, stderr, exit code.

Higher-level operations are achieved by the model invoking shell commands. The shell is the tool taxonomy.

### 4.5 Approval Policy

Tool execution is governed by a per-repository allowlist file (`.fyai/allow` or equivalent), git-committable and reviewable. The allowlist specifies which commands or command patterns may execute without prompting. Commands outside the allowlist either prompt the user (interactive mode) or fail (non-interactive mode, e.g., CI).

A `--yolo` flag bypasses prompts entirely.

**`--dry-run` semantics.** A `--dry-run` flag reports a proposed action without enacting it. Its meaning is necessarily limited in v1: because the model's reasoning at each step depends on the actual output of the previous tool execution, a dry run cannot stub out execution and still produce a faithful trajectory — the moment the first command's real output is withheld, the model's subsequent reasoning diverges from what would happen in a real run, and any further "what would have happened" report is fiction. Therefore in v1 `--dry-run` is defined for single-shot generation only: it surfaces the first proposed tool call (or the terminal response) without executing it, and stops. A faithful dry run *over the full tool-use loop* — running the agent to completion and reporting its complete proposed effect without enacting any of it — is precisely what the phase 2 transaction layer (§Appendix C.6) provides: run the agent for real inside a transaction that is never committed. The full-loop dry run is therefore documented as a phase 2 capability, not a v1 one.

The approval policy and `--yolo` flag are v1 mitigations for an unsolved problem: the absence of a mechanism for separating agent intent from filesystem effect. Phase 2 (§Appendix C.6) replaces the destructive-write defense layer with transactional filesystem state and post-session review, at which point the allowlist's role shifts from security boundary to scope constraint. It does not, by itself, address confidentiality or network egress; see §10 and §C.6 for the layers that do. v1 commands and configuration should not conflict with the phase 2 review-command namespace.

### 4.6 Streaming Output

Model output streams to stdout as it is generated. Tool calls interrupt streaming, execute, and the loop resumes with results appended. The user sees progress in real time; long invocations are not silent.

Reasoning text, tool calls, tool results, and final output are visually distinguished on the terminal but produced as a single stdout stream suitable for piping. A `--quiet` flag suppresses everything except the final response. A `--json` flag produces structured output for programmatic consumption.

**Observability vs. canonicality.** Streamed stdout output is observable but not authoritative. Only canonical turns committed to the arena are authoritative. If a streaming response is interrupted, stdout may contain content that never enters any arena. This is by design — stdout is ephemeral, the arena is the source of truth — but is documented as an explicit contract for tools that consume fyai output.

### 4.7 Provider Abstraction

fyai supports multiple inference backends through a thin provider interface:

- Cloud APIs (Anthropic, OpenAI, others).
- Local inference via llama.cpp, optionally fronted by a weights-only daemon.

Provider selection is per-invocation (`--provider`) or per-session (config file). The canonical content layer of the schema is provider-agnostic; switching providers mid-session is supported. Provider-specific wire-format streams and metadata (model name, sampling parameters) are stored alongside canonical content but do not break canonical identity.

### 4.8 Configuration

Configuration is layered:

- System default.
- User config (`$XDG_CONFIG_HOME/fyai/config.yaml`).
- Repository config (`.fyai/config.yaml`).
- Environment variables.
- Command-line flags.

Later layers override earlier ones. All configuration is YAML, read using libfyaml.

Arena base addresses and reserve sizes are configured under the `arenas` section. The global arena uses a fixed default base. The repository arena base is **derived once, at `fyai init` time**, from a stable hash of repository identity (project path canonicalized, plus filesystem inode, plus a configurable salt), and the resulting value is recorded in the repository config. After init, the recorded value is authoritative for the arena's entire lifetime; the hash is never consulted again at map time. This matters because the inputs to the hash are *not* stable across a repository's life — inodes are reused after delete/recreate, a restored-from-backup or rsync'd tree gets fresh inodes, bind mounts and overlays change inode identity, and moving the project directory changes the canonicalized path. Recomputing the base from these inputs at map time would silently move a pointer-identity-load-bearing base under a live arena, which is data corruption rather than a remap inconvenience. The hash therefore serves only as an init-time slot *suggestion*; the recorded base is the contract. Both bases must remain stable for the arena's lifetime — they are pointer-identity-load-bearing. Relocating an arena to a different base (for instance on a detected slot collision at import) is possible but is a quiesced operation requiring the arena exclusive lock, identical in concurrency posture to garbage collection (§5.12); it is never performed online under concurrent mappers.

### 4.9 Subagent Forking

fyai supports true subagent forking as a first-class operation. A subagent is spawned by forking the current branch at an exact conversation point. The forked subagent:

- Shares all parent context via structural sharing — zero copy, zero memory overhead
- Starts with an identical KV cache prefix marker, so the inference provider’s prompt cache is immediately hot
- Diverges independently from the fork point, accumulating its own turns in a new branch
- Can be compared, merged, or pruned against the parent branch after completion

This is qualitatively different from the subagent model used by all current AI coding tools (Claude Code, Codex, Aider, Cursor, Devin and others), which spawn subagents as fresh processes with summarized or selected context injected via instructions. That model pays an LLM call to compress context, loses information in compression, starts with a cold KV cache, and gives the subagent an epistemically impoverished view of the parent’s state.

fyai’s fork model gives the subagent the full parent context at zero marginal cost. The subagent is not a diminished copy — it is the parent at a point in time, diverging from there with full fidelity. No other current tool provides this capability; it is a direct consequence of modeling the conversation as a first-class immutable content-addressed data structure rather than as process-resident state.

-----

## 5. Storage Architecture

### 5.1 Single Storage Mechanism

fyai has one storage mechanism: the libfyaml content-addressed arena, in the same mmap-friendly cached-generic blob format used by the parse cache. All persistent *canonical* data — conversation content, provider-specific wire-format streams, turn metadata, tool outputs, configuration as parsed, and branch references — is stored as `fy_generic` values in this arena. There is no separate “session file format,” no sidecar store, no parallel filesystem layout. There is one format and one mechanism, applied to multiple populations of canonical data.

**Canonical generics versus opaque blobs.** A second, subordinate population exists: large opaque byte payloads that are *referenced by* canonical generics but are not themselves canonical. The motivating case is phase 4 inference state (KV cache deltas, Appendix E), which is engine- and hardware-specific, has no stable cross-context identity, and cannot satisfy the canonical-generic invariants of §5.13 (deterministic emission to YAML, no mutable views, cross-context dedup). Such payloads are stored as content-addressed blobs — named by their BLAKE3 hash, held in a blob store — and are pointed at by an ordinary canonical *handle* generic. The handle is a first-class generic and obeys every §5.13 invariant; the blob it names is opaque bytes and obeys none of them beyond integrity-by-hash. This preserves "one allocation and mmap mechanism" while being explicit that canonical identity is a property of handles, not of the bytes behind them. Blobs additionally carry a durability class (§5.7): blobs whose contents are reconstructible (KV deltas, regenerable by reprefill) live on the cache side of the cache/data line and are evicted independently of garbage collection; blobs whose contents are not reconstructible (e.g. agent-produced binaries with no other source) live on the data side.

The arena is otherwise immutable; the single category of mutable state is the head pointer of the branch references directory generic (§5.10), updated atomically via the same CAS mechanism that the arena uses for chunk-list publication and allocator bump pointers.

This is a direct consequence of two libfyaml properties: canonical 64-bit identity (any value has exactly one in-arena representation at a stable address) and zero-cost reload (mmap plus optional pointer relocation for transient caches only). Both properties apply uniformly regardless of what kind of data the generics represent.

### 5.2 Arena Stack

The arena is stacked, typically in three layers:

- **Global arena** at `$XDG_DATA_HOME/fyai/arena/`: shared across all sessions on the machine. Holds canonical content, provider streams, and metadata that recur across sessions — system prompts, common configurations, cross-session deduped content.
- **Repository arena** at `.fyai/arena/` within a project root: scoped to one repository, travels with the repository when archived. Holds branch references and arena content scoped to that project.
- **Scratch arena**: in-memory, lives only for the duration of one fyai invocation. Holds in-progress turns during streaming and any work not yet committed to a persistent layer.

Lookup walks the stack from top to bottom; allocation goes to the topmost writable layer. Canonicalization is global within a stack: any logical value has exactly one 64-bit generic representation, making value equality equivalent to pointer equality.

Each persistent arena layer (global, repository) is implemented as a directory containing one or more chunk files (§5.4). The directory is the unit of arena identity; the chunk files within it collectively comprise the arena's address space and contain all persistent state, including branch references.

### 5.3 VMA Layout and Address Stability

Each persistent arena occupies a fixed virtual address range. The range is reserved at process startup before any other allocation has a chance to claim it.

The default address layout reserves regions in the upper half of the user address space, well above typical heap/mmap/stack regions but below the loader's preferred mmap base:

- **Global arena region**: 64 GiB at a configured fixed base (default 0x500000000000 on 64-bit Linux).
- **Repository arena region**: 16 GiB at a base fixed at `fyai init` time. The base is chosen by a stable hash of repository identity (canonicalized project path, plus filesystem inode, plus a configurable salt) selecting a 16 GiB slot within a designated repository-region superblock starting at 0x540000000000, but this selection happens *once* at init; the chosen base is recorded in the repository config and is authoritative thereafter (§4.8). The hash is never re-evaluated at map time.

Both region sizes and base addresses are configurable per arena and recorded in each arena's bootstrap chunk (chunk 0, §5.4). The recorded base, not the derivation, is authoritative once init has run. Multiple repository arenas mapped simultaneously must occupy disjoint regions; the stable-hash slot selection at init makes collision astronomically unlikely but not impossible, and a collision is detected and reported at map time (resolved by a quiesced relocation to a free slot, §5.12).

**VMA reservation protocol.** Each configured arena region is reserved before any ASLR-placed mapping can claim it, with:

```c
mmap(base, region_size,
     PROT_NONE,
     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE,
     -1, 0);
```

`PROT_NONE` with `MAP_NORESERVE` costs no physical memory and no swap reservation — only VMA bookkeeping in the kernel.

Reserving "early in `main()`" is insufficient on its own: by the time `main()` runs, the dynamic loader has already mapped the interpreter and every `DT_NEEDED` library at ASLR-chosen addresses, any one of which could land in the arena window. Two mechanisms together make the reservation robust:

- **Static linkage of dependencies.** fyai links its few dependencies statically (or `dlopen`s them lazily only after the reservation is established). The design already targets a small dependency surface — at most an HTTP/TLS client, and the §10 auditable-binary goal favors a minimal self-contained one — so there are no ASLR-placed shared objects competing for the window at the point of reservation. The reservation itself is performed in an `__attribute__((constructor))` that runs before `main()`.
- **Re-exec on conflict.** If `MAP_FIXED_NOREPLACE` reports the window already occupied (an unlucky ASLR draw, a container or hardened layout, or a configured base that genuinely overlaps a fixed mapping), the process re-execs itself to obtain a fresh address-space layout, carrying an attempt counter in the environment. With a 64 GiB window high in a 47-bit user address space, per-attempt collision probability is negligible and the expected number of re-execs is at most two or three; the attempt counter caps retries so that a genuinely unsatisfiable base fails loudly with a diagnostic (parsed from `/proc/self/maps`, identifying the conflicting mapping and instructing the user to reconfigure the base) rather than looping indefinitely.

**No relocation for persistent arenas.** Persistent arena chunks are subsequently mapped with `MAP_FIXED` over portions of the reserved region. Relocation is not attempted at map time; if the configured base is unavailable after the bounded re-exec attempts, the process exits. This is necessary because relocation would break cross-process pointer identity: process A mapping the arena at one base and process B mapping it at another would produce non-pointer-comparable representations of the same canonical generic, collapsing the entire dedup substrate. Relocating an arena to a *new* recorded base (e.g. on a detected slot collision at import) is a separate, deliberate, quiesced operation under the arena exclusive lock (§5.12), never an online remap under concurrent mappers.

**Platform scope.** The address-stability invariant is a Linux-first guarantee. The default high-half layout, `MAP_FIXED_NOREPLACE` semantics, and the re-exec strategy are specified against Linux on x86-64 and ARM64. macOS (dyld address-space conventions) and Windows (no `mmap`; `MapViewOfFileEx` fails rather than relocating) require per-platform base selection and are explicitly secondary: they are desired but not gated on for the initial Linux-complete implementation, and they are permitted an escape hatch (configurable base, graceful refusal) rather than a guarantee. Address-tagging regimes (arm64 TBI, x86 LAM) are not fought; fyai asserts at startup that the tag bits it relies on for canonical pointers are in their expected state and refuses to start otherwise.

**Cross-process consistency.** All processes touching a given arena must agree on the region base address. The bootstrap chunk (§5.4) records the expected base. On map, each process verifies its configured base matches the recorded base; mismatch is a configuration error and the process exits without mapping.

### 5.4 Multi-Chunk Arena Layout

A persistent arena is a sequence of fixed-size chunk files within the arena directory. Each chunk is a separate file mapped at a deterministic address within the arena region.

**Naming and addressing.** Chunks are named `arena-{N}.bin` where `N` is the chunk's generation id (§5.5). The address at which a chunk maps is computed deterministically:

```
chunk_base(N) = region_base + N * chunk_size
```

`chunk_size` is fixed per arena at `fyai init` time (default 256 MiB). With a 64 GiB region and 256 MiB chunks, an arena can hold up to 256 chunks before exhausting its region; with a 16 GiB repository region, up to 64 chunks. Exhaustion triggers a `fyai gc` requirement.

**Chunk 0 (bootstrap).** The first chunk is special: it always exists, it cannot grow, and it holds the synchronization roots for the rest of the arena. Specifically, chunk 0's header contains:

- Magic number and arena format version.
- Expected region base address (verified by mappers; mismatch is fatal).
- Region size and chunk size.
- The `generation` counter (atomic, monotonic; §5.5).
- The `head` pointer to the singly-linked chunk list (§5.5).
- The refs head pointer (§5.10), pointing at the current branch references directory generic.
- A bounded history of prior refs head pointers, retained for recovery (§5.10).
- Bootstrap allocator state for chunk 0's own allocation region.

Chunk 0 is created at `fyai init` time as a single-writer operation. Its size is fixed at creation; it never grows. The remaining chunks (1, 2, 3, ...) are created on demand via the grow protocol (§5.5).

**Chunk file allocation.** Each chunk file is allocated to its full `chunk_size` at creation. The default is `fallocate(2)` on Linux to reserve disk blocks immediately, eliminating mid-write `SIGBUS` on `ENOSPC`. A `--sparse` configuration option uses `ftruncate(2)` for sparse allocation when the user explicitly accepts the ENOSPC-mid-write risk in exchange for not consuming disk until written.

**Discontiguous chunk ids.** Chunk ids are not necessarily contiguous (see §5.5: race losers consume generation ids without creating chunks). The chunk file directory may show `arena-0.bin`, `arena-1.bin`, ..., `arena-46.bin`, `arena-48.bin`, ... with no `arena-47.bin`. The VMA range for generation 47 remains `PROT_NONE` for the arena's lifetime. The wasted VMA is bounded (one chunk_size per skipped generation), bounded in practice (skipped generations are rare), and free (PROT_NONE has no physical cost). No physical resource is wasted.

### 5.5 Chunk Growth Protocol

When a process exhausts the allocable space in the current set of chunks, it grows the arena by adding a new chunk. The protocol is optimistic, lock-free, and resolves contention by discarding wasted parallel work rather than serializing.

**Protocol.**

1. Suitor reads `generation` and `head` from chunk 0.
2. Suitor atomically increments `generation` via `fetch_add(1)`, claiming a unique generation id `G`. The claimed VMA base is `region_base + G * chunk_size`. Multiple concurrent suitors obtain distinct `G` values; none block.
3. Each suitor independently:
   - Creates `arena-{G}.bin` in the arena directory (using a `.tmp` suffix and atomic rename for crash safety).
   - `fallocate`s the chunk to `chunk_size`.
   - `mmap`s the chunk at `region_base + G * chunk_size` with `MAP_SHARED | MAP_FIXED`.
   - Initializes the chunk's allocator header.
   - Prepares a chunk list node referencing the new chunk.
4. Each suitor attempts to CAS the list `head` from `head_observed` to its new chunk node.
5. **Winner**: CAS succeeds. The new chunk is now visible to all readers via list traversal. Winner proceeds to allocate from the new chunk.
6. **Loser**: CAS fails. Loser unmaps its chunk, unlinks its chunk file, discards its prepared node. Loser then re-reads `head` (now pointing at the winner's chunk), maps the winner's chunk, and retries its allocation against the now-available winner's chunk. The loser's generation id is permanently consumed; its VMA slot remains `PROT_NONE` for the arena's lifetime.

**Properties.**

- **No serialization.** Suitors do `fallocate` and `mmap` in parallel. The only synchronization point is the head CAS, which is a single cache-line atomic.
- **No phantom chunks.** A suitor that has not yet CAS-succeeded has created a file that is not yet in the list. If the suitor crashes before CAS, the file is orphaned and reclaimed by recovery (§5.5.1). If the suitor loses the race, the file is deleted by the loser itself. No process can create a file at `arena-{G}.bin` for an already-published `G` because each `G` is uniquely claimed.
- **Bounded waste.** Losers waste `fallocate` and `mmap` work but never block winners. Under typical low-contention grows, losers are rare. Under high contention, wasted work scales with the contention level but throughput continues to increase with concurrency.
- **Visible side effect: discontinuous generation ids.** This is the only externally-visible consequence of the race protocol. VMA gaps are free; no other resource is consumed.

**Slot consumption is not a practical exhaustion risk.** Because a lost race permanently consumes a generation id, and the generation id is coupled to a VMA slot (`chunk_base(N) = region_base + N * chunk_size`), a pathological loser stream could in principle exhaust the fixed region (256 slots global, 64 repo) before storage is exhausted. Quantitatively this does not occur under any realistic workload. Grows are separated by the time to fill a chunk — on the order of tens of seconds even for an aggressive 64-agent fleet, since canonical content is small and dedups heavily — while the window in which two suitors collide on the same head CAS is microseconds. The resulting amortized loser-burn is a fraction of one slot per day of continuous maximum-contention operation, against which ordinary capacity-driven `fyai gc` intervenes many orders of magnitude sooner. The coupled slot/id design is therefore retained.

The one correlated exception is a **synchronized cold start**: a CI matrix or `make -j64` that fans out N agents simultaneously against a fresh arena, all of which observe "chunk 0 full" within the same instant and race the *first* content grow together, producing up to N−1 losers in a single burst. This is bounded (≤ N slots, once, at startup — comfortably absorbed by the global region's 256 slots) and is not a sustained leak. It is eliminated entirely by creating the first post-bootstrap content chunk as a single-writer-on-demand operation, mirroring chunk 0's single-writer creation (§5.4); subsequent grows use the lock-free raced protocol, by which point grows are temporally uncorrelated and losers are vanishingly rare.

#### 5.5.1 Crash Recovery

If a process crashes during chunk creation, the arena directory may contain:

- An orphan `arena-{G}.bin.tmp` (chunk creation incomplete, never renamed).
- An orphan `arena-{G}.bin` not in the list (renamed but CAS not yet attempted, or CAS lost — but loser cleanup didn't run).

On the next process startup (or on demand at the first grow), fyai performs a recovery scan:

- Any `.tmp` file is unlinked.
- Any `arena-{G}.bin` not in the list is unlinked (after verifying its absence from the list under acquire-ordered load of `head`).

Recovery is safe to perform concurrently with other processes' operations because the recovery scan only acts on files that are definitively orphaned (not in the published list), and the live arena state is determined entirely by the list. Files outside the list are invisible to readers.

### 5.6 Immutability and Concurrency

The arena is lock-free and immutable at the content level. The C API enforces immutability; there are no mutable views into arena memory. Edits are expressed as new generics with structural sharing of unchanged subtrees.

Multiple fyai invocations may write concurrently without coordination. Within a chunk, libfyaml's allocator uses cross-process atomic CAS on the chunk's bump pointer (currently using `memory_order_seq_cst` for safety; later optimization to acq/rel possible). Across chunks, the chunk growth protocol (§5.5) handles concurrent grow attempts.

The only mutable persistent state is the head pointer of the branch references directory generic (§5.10), updated via the same cross-process atomic CAS mechanism that drives the chunk-list and bump-pointer protocols. The refs directory generic itself is immutable; updates produce new directory generics with structural sharing of unchanged entries, and the head pointer is atomically swapped to publish.

**Cross-process atomic semantics.** `MAP_SHARED` mappings of the same file from multiple processes resolve to the same physical pages in the kernel page cache. Atomic operations on `MAP_SHARED` addresses are coherent across processes via the CPU's standard cache coherency protocol (MESI/MOESI on x86, equivalent on ARM64). This requires:

- Natural alignment of atomic operands (8-byte alignment for 64-bit atomics; enforced via `_Static_assert` on relevant struct layouts).
- `MAP_SHARED`, not `MAP_PRIVATE` (asserted at map time).
- Local filesystem backing (NFS, SMB, FUSE do not provide the required atomic semantics; detected at startup and refused for persistent-arena mode).

### 5.7 Cache vs. Data Distinction

The libfyaml parse cache lives in `$XDG_CACHE_HOME/libfyaml/`: reconstructible, may be wiped by the system, used purely as memoization of parse results. The parse cache may use relocation (§5.8) because it is per-process and transient.

The fyai arena lives in `$XDG_DATA_HOME/fyai/`: not reconstructible (losing it loses conversation history and provider stream caches), must not be wiped by aggressive cache cleaners. The arena does not use relocation; its addresses are stable across processes by construction.

Both use the same on-disk format and same loading mechanism. The distinction is policy, location, and relocation eligibility. fyai refuses to start if the parse cache path and the arena path resolve to the same directory, to prevent accidental mixing of relocatable and non-relocatable storage.

**Reconstructible blobs are cache, not data.** The content-addressed blob store (§5.1) is split along exactly this line. A blob whose contents can be regenerated from canonical data — paradigmatically a phase 4 KV cache delta, regenerable by reprefill from canonical turns (Appendix E) — is *cache*: it lives under `$XDG_CACHE_HOME/fyai/blobs/`, losing it is a performance event (a reprefill) rather than a data-loss event, it is evicted under storage pressure by its own cache policy, and it is decoupled from arena garbage collection (§5.12) and its quiescence requirement. A blob whose contents cannot be reconstructed lives on the data side under `$XDG_DATA_HOME`. The blob store mechanism is identical for both; only the retention policy differs, keyed on a per-blob reconstructible flag. This keeps multi-gigabyte reconstructible KV data out of the durable arena, so that `fyai gc`'s survivor copy moves only the small canonical set and never relocates regenerable bytes.

### 5.8 Parse Cache Relocation (Transient Only)

The parse cache (not the arena) loads with `MAP_FIXED_NOREPLACE` at the address recorded in the cache header. On success, no work is required: pointers are already correct, pages fault in on demand. On address conflict (containers, hardened environments, or unlucky ASLR draws), the cache falls through to a relocation path that walks the generics using the in-arena type tags and rebases pointers.

This relocation mechanism is **explicitly forbidden** for persistent arenas. It is available for the parse cache because the parse cache is per-process — each process can relocate independently without breaking pointer identity with any other process. Persistent arenas must use identical address bases across all processes; if the base is unavailable in any process, that process exits rather than relocating.

Bundle import (§7.3) is a related but distinct case: bundles are wire-format, designed to be relocatable, and are relocated exactly once at import time into the receiver's chosen arena base. This is a one-time cost at a well-defined boundary, not a startup cost.

### 5.9 Parallel Access and Memory Sharing

Multiple concurrent fyai invocations — parallel agents, `make -j` pipelines, CI jobs — all mmap the same arena chunk files. The shared pages are physically present once in the page cache regardless of the number of readers. Memory for shared content — system prompts, common file reads, shared conversation history — is O(1) in the number of concurrent agents, not O(N).

This is the same mechanism as shared libraries: one physical copy, N virtual mappings, managed correctly by the OS without explicit coordination from fyai. Idle agents consume zero memory; the kernel evicts arena pages under memory pressure and faults them back in on next access. There is no resident process to consume memory on the off chance that an agent will be invoked.

The contrast with resident-daemon tools is sharp: three active projects in a daemon-based tool means three processes, each holding full context in private heap, consuming memory whether in use or not. Three fyai sessions share arena pages that are present once in the page cache, and consume zero memory when no invocation is running.

### 5.10 Branch References Directory Generic

Branch references are stored as a content-addressed directory generic within the arena. The directory generic is a mapping from branch name to per-branch metadata, where per-branch metadata includes at minimum a reference to the branch's sequence head generic and may include additional fields (creation timestamp, parent branch, description, and similar metadata).

The refs directory generic is itself canonical and immutable, like all generics in the arena. Updates do not modify the existing directory; they allocate a new directory generic representing the updated mapping, with structural sharing of unchanged entries.

**Refs head pointer.** Chunk 0's header contains a pointer to the current refs directory generic, in a dedicated 8-byte aligned field designated for atomic operations. This pointer is the only mutable field in the arena beyond the chunk-list head and the allocator bump pointers. All three follow the same CAS-based concurrency discipline.

**Update protocol.** A process updating one or more branch references proceeds optimistically:

1. Atomic-load the current refs head pointer.
2. Walk the current refs directory generic to read existing entries.
3. Allocate a new directory generic representing the desired updated mapping. Unchanged entries share storage with the prior directory by construction.
4. CAS the refs head pointer from the observed value to the new directory generic.
5. On success, the update is published. Readers observing the head after this point see the new directory.
6. On failure (another process updated refs concurrently), re-load the head pointer, re-read the directory, recompute the update against the new state, and retry.

The CAS is on a single 8-byte aligned pointer, with the same cross-process atomicity guarantees as the chunk-list head CAS (§5.5).

**Concurrency characteristics.** Multiple processes updating disjoint refs (different branch names) succeed serially through the CAS, each retry recomputing against the latest state. Multiple processes updating the same ref serialize naturally: only one CAS succeeds per round, and the losers' computed states are based on stale information about that ref's prior value, so they retry with the winner's update visible. Contention on refs updates is bounded by the refs head pointer, which is one cache line; the underlying directory generic's structural sharing keeps allocation cost low even under contention.

**Durability and ordering.** Refs updates inherit the arena's durability model: a new directory generic and the updated head pointer become durable when their containing chunk's page-cache pages are written back. Crucially, the head pointer and the generic it points at occupy independently-written dirty pages, and the kernel may write them back in either order. A normative ordering invariant therefore governs every durability checkpoint:

> **A refs head pointer must never become durable before the directory generic it points at — and everything that directory newly references — is durable.**

This is enforced by an explicit barrier at each checkpoint: `msync(MS_SYNC)` (and `fsync(2)`) the chunk ranges holding the new directory generic and its newly-referenced content *to completion*, and only then write and durably flush the head pointer in chunk 0. Without this ordering, a crash between the two writes leaves a durable pointer into non-durable bytes; integrity verification of a structure reached through such a pointer is not sound, because there is no write-ordering guarantee that makes a header-magic-plus-checksum check sufficient against arbitrary page-writeback reordering. The recovery history (below) is a backstop for partial writes, not a substitute for the ordering barrier.

**Stacked durability tiers.** The barrier is required at every point where durability is *promised*, but not every CAS is such a point. Three tiers follow from the arena stack (§5.2):

- **Scratch / worker arena:** no durability ordering at all. A worker (subagent, §4.9) accumulates refs in transient storage that evaporates on crash by design; nothing is owed those intermediate refs.
- **Durable arena between checkpoints:** the CAS publishes visibility to concurrent readers via the page cache (all that in-process and cross-process correctness requires), with no `msync`. Updates may be batched.
- **Durability checkpoint:** at commit of worker results into the durable parent, at explicit `fyai sync`, and at process exit in automated contexts (CI), the ordering barrier above is enforced.

Promoting a worker's result into the durable parent is itself a checkpoint with a specific shape, because a worker's result generics live in transient storage that cannot be pointed at by a durable head. The commit therefore (a) materializes the divergent suffix — only the worker's new turns since the fork point, since everything below the fork is already shared durable parent storage — into the durable parent arena, (b) flushes that suffix durable, then (c) CAS-publishes and flushes the parent refs head. This is the same allocate-into-fresh-durable-space-then-swap-a-root discipline as the GC survivor copy (§5.12). The cost of a checkpoint is bounded by the divergent suffix plus one chunk-0 page; refs updates are infrequent relative to the work that produces them (one CAS per committed turn, not per token), so the durable parent performs a full checkpoint per refs update without it being a contention point. The synchronous barrier is offered as an explicit operation rather than implied on every CAS so that within-session batching remains available where a workload prefers it.

**Recovery.** On process startup, the refs head pointer is read from chunk 0's header. If the pointed-to directory generic is found via its in-arena address and passes integrity verification (header magic, structural checksums), the refs are loaded. If verification fails (incomplete write-back at the time of a prior crash), the recovery scan (§5.5.1) examines prior refs directory generics retained in chunk 0's history and selects the most recent one that verifies. Chunk 0 retains a bounded history of prior refs directory references for this purpose; older entries are eligible for collection by `fyai gc`.

**Garbage collection roots.** The refs directory generic and all per-branch metadata it references are the roots of arena garbage collection (§5.12). The mark phase begins from the refs head pointer and walks transitively through branch sequence heads, turns, content, provider streams, and metadata references to identify the live set.

### 5.11 Xmit Cache

The xmit cache stores the assembled emit stream for a given branch and provider, keyed by branch head and provider identity. On a cache hit the assembled wire-format message list is available without re-emitting from generics. On a cache miss the emit path walks the canonical turn sequence and generates the provider-format message list, populating the cache for subsequent calls.

The critical property is incremental append: adding a new turn to a branch produces a new sequence generic (the prior sequence plus the new turn). The xmit cache for the prior sequence head is still valid; only the new turn needs to be emitted and appended. Emit cost per turn is O(new turn size) regardless of total context size. This eliminates the O(N) serialization cost that affects tools which reconstruct the full message array from scratch on each API call.

The xmit cache is reconstructible and lives in `$XDG_CACHE_HOME/fyai/xmit/`. Loss of the xmit cache is a performance event, not a data loss event. The xmit cache is per-process or shared at the user's discretion; sharing it across processes uses the same chunk-based mechanism as the arena but with relocation enabled (since it is reconstructible).

### 5.12 Garbage Collection

Arenas grow monotonically under normal operation. The `fyai gc` command performs explicit collection: walk all live branch references, mark reachable generics (transitively through content, provider streams, and metadata references), copy survivors to a fresh arena, swap. GC is never implicit; it is a deliberate user operation analogous to `git gc`.

**Quiescence requirement (v1).** GC requires arena quiescence: no other fyai processes may have the arena mapped. This is enforced by an exclusive lock on the arena directory acquired at GC start. Concurrent GC with active processes is a v2 problem requiring more sophisticated copying-collector protocols (epoch-based reclamation or hazard pointers across process boundaries).

**Atomicity.** GC writes a new arena directory alongside the old, validates it, then atomically renames the old directory aside and renames the new directory into place. The old directory is retained until the next successful invocation as a recovery option. The rename dance is followed by `fsync` of the containing directory.

**Selective GC.** Selective GC is possible by walking only certain reference types — for instance, "drop all provider streams older than 90 days but keep canonical content" walks content and metadata references but treats stale provider streams as collectable. This gives users policy levers over storage growth without giving up the unified storage mechanism.

### 5.13 Required libfyaml Invariants

The architecture rests on libfyaml properties that must hold end-to-end:

- **Canonical 64-bit identity** within an arena stack, at stable addresses across all processes mapping the stack.
- **Deterministic emission**: any generic renders to a unique canonical YAML form.
- **Canonicalizing parse**: parsing any YAML form of a generic produces the canonical generic.
- **Immutability enforcement**: no API path produces mutable views into arena memory.
- **Lock-free concurrent writes** within a chunk (atomic CAS on bump pointer) and across chunks (chunk growth protocol of §5.5).
- **Multi-arena allocation transparency**: the allocator handles N chunks as a single logical allocation domain, with the growth protocol invisible to callers.
- **Cross-process atomic semantics** on `MAP_SHARED` regions, dependent on natural alignment and local filesystem backing.

All seven are libfyaml-provided. fyai depends on them and does not re-implement or work around any of them.

These invariants govern *canonical generics*. They do not govern the opaque content-addressed blobs of §5.1 (phase 4 inference state and any other engine- or hardware-specific payload): a blob is named by its BLAKE3 hash and verified by that hash, but makes no claim to deterministic YAML emission, cross-context canonical identity, or immutability-as-a-generic. The canonical *handle* that references a blob does obey all seven invariants; the bytes behind the handle obey only integrity-by-hash. This boundary is what allows large non-canonical payloads to share the arena's allocation and mmap machinery without weakening the canonical substrate.

-----

## 6. Schema

### 6.1 Reference Structure

A branch is a named reference to a head generic, where the head is a canonical sequence-of-turns generic. A turn generic bundles three references:

- A **content generic**: role plus content elements (text, tool use, tool result), provider-agnostic and fully canonical. This is the layer that determines semantic identity and is the basis for KV cache prefix matching, branch comparison, and conversation-level dedup.
- A **provider-streams generic**: a map from provider identity to that provider’s decoded wire-format representation of the turn, stored as a proper generic in the arena (not an opaque blob). Populated when a turn is produced by, or sent to, a given provider. May contain entries for multiple providers if the same canonical turn has been realized against more than one. Loaded by mmap on replay; never reconstructed from canonical content unless switching providers.
- A **metadata-events generic**: a sequence of production-event records (timestamp, provider, model name, sampling parameters, token counts, latency, cost, request and response IDs). The same canonical turn produced multiple times accumulates multiple metadata events here.

All three references point at canonical generics in the arena. Each is independently dedupable: identical content shares storage even when provider streams differ, identical provider streams share storage even when metadata differs, identical sampling-parameter records share storage across thousands of turns.

**Forward compatibility note.** The turn reference structure is designed to accommodate additional reference types in future versions without breaking canonical identity of existing turns. Phase 2 (§Appendix C) anticipates a fourth reference — filesystem-state — to be added without modifying the existing three. Phase 3 (§Appendix D) records branch-point information (the token-selection distribution at positions of substantive uncertainty within an assistant turn) in the **metadata-events** layer, *not* in canonical content: a token-selection distribution is a production-time fact, dependent on engine, hardware, and floating-point reduction order, and is therefore not reproducible or dedup-stable across contexts the way canonical content must be. Placing it in metadata is consistent with where per-segment model identity is recorded (§D.5.4) and avoids making canonical identity depend on non-reproducible floating-point values. Phase 4 (§Appendix E) anticipates an additional turn reference — an inference-state handle (pointing at a content-addressed KV-delta blob, §5.1) — that complements phase 3 by making fork operations efficient, both during eager pruned branching and during retrospective exploration. The schema layer is deliberately extensible at the turn level, and the content/metadata boundary is chosen so that only reproducible, semantics-determining information is canonical.

### 6.2 Canonicalization Layers

The schema is deliberately layered to control what canonicalizes with what:

- The **content generic** excludes everything that varies independently of semantics. Provider-assigned tool-call IDs, content-block indices, and finish reasons are not part of canonical content. Tool call/result pairing within a conversation uses local references (turn position plus call index), not provider-assigned IDs.
- The **provider-streams generic** carries the wire-level shapes faithfully decoded into arena generics, including provider-assigned IDs. This canonicalizes when the decoded structure is identical, which happens for identical model outputs but not for outputs that differ only in provider-assigned IDs.
- The **metadata-events generic** carries production-time facts. Timestamps and request IDs vary per event, so metadata events typically don’t dedup at the event level — but the structures they’re built from (sampling-parameter generics, model-name generics, provider-identity generics) do dedup heavily.

The turn generic itself canonicalizes when all three of its references are canonical-equal. This is rare for production turns because metadata events differ, and that’s correct: two production events of identical content remain distinguishable at the turn level while sharing every byte of the underlying content and any matching provider streams.

### 6.3 Canonical Content

A turn is one of: `user`, `assistant`, `tool_result`, `system`. (System content is modeled as a turn for uniformity, even though it is typically positioned at the head of the conversation rather than appearing as a successor.)

A turn has a role and a content payload. The content payload is a sequence of content elements. Each content element is one of:

- `text`: a string of natural-language content.
- `tool_use`: a structured tool invocation (name, arguments). Assistant turns only.
- `tool_result`: the result of executing a tool invocation, referenced by intra-conversation local reference. Tool result turns only.

The canonical form does not include a turn ID, timestamp, or any provider-assigned identifier. The canonical form is exactly the information that determines what the model “saw” or “produced” semantically.

### 6.4 Multi-Step Assistant Turns

A single model inference step may produce a structured output containing multiple content elements: interleaved text, reasoning blocks (where the provider exposes them), and one or more tool calls. Anthropic represents this as a single assistant message with a content array; OpenAI represents this as an assistant message with a `tool_calls` array; other providers vary.

**Canonical decision.** One model inference step produces exactly one canonical assistant turn. The content payload is the sequence of content elements emitted in that step, in emission order. Multiple tool calls in one step are multiple `tool_use` elements within the same canonical assistant turn, not separate turns.

This matches the model's semantic action (one forward pass, one sampled output) and gives the cleanest mapping to KV cache prefix matching (one turn = one prefix-boundary). Provider wire-format differences in how this single step is serialized are handled at the provider-streams layer.

Tool results for multiple tool calls within a single assistant turn appear as one or more subsequent `tool_result` turns, each referencing its tool call by `(turn_index, call_index)`. The aggregation of tool results into a single user-role message (as some providers require on the wire) is handled at the provider-streams layer, not in canonical content.

### 6.5 Tool Call Identity

Tool-call IDs are assigned by providers (Anthropic’s `toolu_<random alphanumeric>`, OpenAI’s `call_<random alphanumeric>`) and required by the protocol for pairing tool uses with their results. For canonicalization purposes, the ID itself is not part of semantic content — what matters is the call/result pairing.

The canonical form represents this with intra-turn-list local references: a `tool_use` has an index within its assistant turn; a `tool_result` references the assistant turn and index it answers. The provider’s wire-level ID is generated at send time from this local reference and recorded in the provider-streams generic for that turn.

This means two assistant turns with identical text and identical tool calls (same name, same arguments) canonicalize to the same content generic, even if one was produced in a session where the provider assigned `toolu_abc123` and the other where the provider assigned `toolu_xyz789`. The canonical content is provider-ID-free.

**ID generation policy.** At reconstruction time (provider switch or xmit cache miss), fresh provider-format IDs are generated deterministically from canonical local references: `toolu_{hash(turn_index, call_index)}` for Anthropic-format, equivalent for others. The branch identity is deliberately *not* an input to the hash. Pairing only needs to be unique and consistent within a single emitted message list, and `(turn_index, call_index)` already satisfies that within any conversation; concurrently-emitted branches go to separate API calls in separate wire contexts and never share a message list, so branch identity is unnecessary for correctness. Excluding it means a tool-bearing turn that is identical across two branches produces identical wire IDs and therefore shares its provider-stream generic across those branches (§6.1), which including branch identity would have defeated. Deterministic generation makes wire output reproducible across runs given the same canonical input, which simplifies testing and debugging. Providers do not, to current knowledge, perform server-side dedup based on tool-call IDs, so deterministic IDs are safe.

### 6.6 System Prompts

System prompts are conversation content semantically but have peculiarities worth documenting:

**They are usually large and stable.** A coding-assistant system prompt is kilobytes and changes rarely. Canonicalization handles this perfectly: the system prompt generic is allocated once, shared across every turn and every session that references it. This is the most reliable deduplication hit in the entire system — a shared system prompt is present once in the global arena regardless of how many sessions use it.

**They sometimes include dynamic content.** Current date, repository name, user identity. If this dynamic content is templated into the system prompt directly, every conversation gets a unique system prompt and canonicalization loses its value — and KV cache prefix matching breaks. The recommended pattern is to keep the system prompt itself canonical and stable, and represent dynamic context as a separate user turn at the start of the conversation. This preserves canonical identity of the system prompt across sessions and lets KV cache prefixes hit reliably. The split is along *stability*, not merely role: stable behavioral instructions the model must obey (tool contracts, safety constraints, persona) stay in the canonical system prompt; only volatile context (date, working directory, repository name) moves to the leading user turn. This is a deliberate tradeoff rather than a free win — models weight system-role and user-role text differently, so behavioral instructions should not be pushed into a user turn merely to chase deduplication. Keep in the system prompt anything whose role placement affects behavior; move out only what is purely contextual.

**Provider differences.** Anthropic uses a dedicated system parameter; OpenAI uses a system role message. The canonical form represents system content as a turn with role `system`; the provider layer maps it to whichever wire shape the target provider expects. No information loss either direction.

### 6.7 Streaming and Partial Turns

Cloud providers stream assistant responses as a sequence of events: text deltas, tool-use start/argument-delta/stop, finish events. fyai consumes these incrementally to provide streaming output to the user, but the canonical turn is constructed only after the stream completes.

A turn is canonical only when complete. Partial turns during streaming are held in scratch arena memory and either committed to the global/repository arena on completion or discarded on cancellation/error. There is no canonical representation of “in-progress turn” — incomplete generations don’t enter the deduplicated store.

**Observability contract.** Streamed content is emitted to stdout as it arrives, before any canonical commitment. If a stream is interrupted, stdout content may not correspond to any canonical turn. This is the same observability/canonicality split called out in §4.6.

**Truncation marker.** Truncated turns committed explicitly carry a truncation marker that is part of canonical content. Two truncated turns are canonical-equal iff their committed content is byte-equal including the truncation marker. The marker is a structural content element, not a flag — it is part of what the model "produced" from the perspective of canonical identity.

If a streaming response is interrupted partway through, the user has options: discard the partial turn (default), commit a truncated version (with explicit marker), or retry. Each is a different operation on the scratch state.

### 6.8 Refusals, Errors, and Non-Standard Outcomes

A model refusal is a normal turn — the assistant said something, that something happens to be a refusal. It canonicalizes like any other text content. The metadata-events generic may record a refusal flag if the provider exposes one (Anthropic's `stop_reason: "refusal"`, for instance).

A provider error (rate limit, transient failure, malformed response) does not produce a canonical turn. The attempt is logged in metadata as a failed attempt against the prior turn; no new content enters the arena. Retries are independent attempts, each potentially producing a turn or another failure record.

A timeout or user-cancellation during streaming is treated as the streaming-incomplete case from §6.7.

### 6.9 Branch Heads and Turn Sequences

A branch’s head generic is a sequence-of-turns generic — itself canonical, itself shared when two branches happen to have identical histories. Pointing branches at sequence generics rather than at the latest turn means branch comparison is one pointer compare, and identical histories share their entire sequence storage.

Adding a turn to a branch produces a new sequence generic (parent sequence + new turn), which canonicalizes against any other sequence with the same content. The previous head sequence is unchanged and remains the head of any other branch that pointed at it. This is the persistent-data-structure pattern, applied at the conversation level, with arena dedup ensuring identity.

Forking a branch for subagent use is allocation of a new entry in the refs directory generic pointing at the same sequence head generic. Cost is one CAS-published update to the refs directory, with structural sharing of all unchanged entries. The forked branch and the parent share all prior turn generics by construction.

-----

## 7. Human-Facing Views

### 7.1 YAML as a Derived View

YAML is not a storage format. libfyaml’s deterministic emitter renders any generic to YAML on demand, and the canonicalizing parser absorbs YAML back into the arena losslessly. This makes YAML the natural format for human-facing operations:

- `fyai show <branch>` emits a branch’s canonical conversation content to stdout as YAML.
- `fyai show --full <branch>` emits content, provider streams, and metadata, expanded.
- `fyai edit <branch>` materializes the branch as YAML in `$EDITOR`, re-parses on save, and the canonical immutable arena absorbs the result. Round-trip is identity-preserving by construction.
- `fyai export --format=yaml <branch>` produces a YAML rendering for git commit or external sharing.

YAML is generated when humans need to read or edit; storage and inter-process exchange use the binary cached-generic format throughout.

### 7.2 Editability

Direct edit is supported through the materialize-edit-reparse workflow described in §7.1. Users do not edit the binary arena directly; they edit a YAML rendering, and the canonical immutable arena absorbs the result. Pruning bad turns, fixing prompts, and reorganizing branches are supported by direct file edit followed by re-parse.

The deterministic emission and canonicalizing parse properties make this workflow lossless in the precise sense that matters: subtrees the edit leaves unchanged re-canonicalize to their original generics with pointer-equal identity, and content the edit changes produces new canonical generics — which is the entire point of editing. There is no semantic drift in either case: unchanged content cannot silently acquire a new identity, and changed content is absorbed canonically rather than appended as an uninterpreted blob. The round trip preserves the identity of everything the user did not touch.

### 7.3 Export and Bundling

A bundle is the export of a branch as a self-contained portable arena. The bundle operation walks reachable generics from the branch head and serializes them in a relocatable variant of the arena format. Reference type filters control what’s included:

- **Default export**: content, provider streams, metadata. Full fidelity for sharing within trusted contexts.
- **Scrubbed export** (`--scrub-provider-streams`): omits provider stream references and metadata fields containing provider-assigned IDs. Useful when sharing publicly or across organizations where wire-level details could leak account identity.
- **Content-only export** (`--content-only`): canonical content alone. Useful for “here is the conversation, ignore how it happened.”
- **YAML export** (`--format=yaml`): renders selected reference types to YAML rather than producing a binary bundle.

Bundle import merges the imported arena’s generics into the receiver’s stack. Because the source and receiver may use different region base addresses, bundles are relocated at import time: the import path walks the bundle's generics and rebases pointers into the receiver's arena region. Canonicalization collapses any content already present in the receiver, so bundle import is naturally idempotent and incremental.

Deterministic emission and stable relocation also make bundles reproducible: two exports of the same branch on two machines produce byte-identical bundles (modulo the embedded source-base address), enabling integrity verification, signing, and content-addressed distribution.

-----

## 8. Performance Requirements

Start and reload comprise three distinct regimes, which should not be conflated:

- **Arena creation (cold-populate)**: the first invocation against a fresh repository builds the arena — allocating chunks, populating canonical content, writing back. This is a one-time cost paid once per repository, and it is the *most* expensive regime: populating a cache is slower than not having one (a representative libfyaml measurement shows a large structure parsing in ~17 s with cache off and ~22 s while populating the cache cold, versus ~36 ms once hot). CI that starts from a clean checkout on every run lives in this regime unless the arena is cached between runs; this is worth surfacing to users explicitly, because the cold-populate cost is real and is not the number the warm path advertises.
- **Reload, warm page cache**: a subsequent invocation maps an already-populated arena whose pages are warm. This is the headline path: O(1) in CPU work — `mmap`, validate chunk 0 and the refs head's immediate integrity, return the root, with no structural walk of the arena. The target is under 30 ms for typical session sizes. The supporting libfyaml figure is 35.9 ms to load a 427 MB structure hot, and that figure *includes* a BLAKE3 hash over the input that the arena hot path does not pay (arena identity is its address, not a recomputed content hash), so under-30 ms is a conservative ceiling rather than an optimistic target. Observed RSS delta on that hot load is ~1 MB, confirming the path maps rather than materializes.
- **Reload, cold page cache**: the arena exists but its pages were evicted (fresh boot, or eviction under memory pressure — which §5.9 relies on happening). Reload remains cheap because it touches only the *spine* of generics on the path to the head, not the body: the ~1 MB working-set figure above is the relevant one, so this is a handful of demand faults, not a read of the whole arena.

The corollary, stated so it is not mistaken for a regression: reload time is O(1) in CPU and bounded by spine faults, but the *first touch* of any individual generic during the actual work is a demand fault. A workload that deliberately touches every turn in a gigabyte session pays O(touched data) — but that is the cost of the work, identical for any architecture (a daemon must equally hold or fault that working set), and it is not a reload cost. Normal operation does not touch the whole arena; the ~1 MB reload working set is the evidence.

Other figures:

- **Memory footprint between invocations**: zero. The process exits.
- **Memory footprint during invocation**: bounded by working set of touched generics plus model client buffers. No requirement to hold the full arena resident.
- **Parallel agent memory**: O(1) in number of concurrent agents for shared content while resident, by virtue of mmap page sharing — N parallel agents on the same repository share arena pages physically present once. Under memory pressure those shared pages evict and re-fault; the re-fault cost is bounded by the touched working set (per the cold-page-cache figure above), not by arena size, so the resident-once and zero-idle properties coexist without contradiction.
- **Emit cost per turn**: O(new turn size), not O(total context size), by virtue of the xmit cache incremental append property.
- **Grow latency under contention**: bounded by `fallocate` time for the new chunk (typically tens of milliseconds for a 256 MiB chunk on a modern filesystem, but higher on slow or networked block devices; losers waste this work but do not block winners). `chunk_size` trades grow latency against grow frequency; deployments on slow storage may prefer a smaller chunk.
- **Phase 4 fork/resume (when present)**: savings over reprefill are backend-dependent, from large (CPU and unified-memory engines consuming KV directly from mmap'd pages) to marginal (discrete-GPU engines, which still pay a host-to-device transfer comparable to recompute, and benefit only for long prefixes). The reprefill fallback is always available and is the performance floor; phase 4 is an optimization above that floor, never a correctness dependency (§Appendix E).

-----

## 9. Platform Requirements

- **Linux is the primary and initially complete target** (x86_64 and ARM64). The address-stability invariant — fixed-base reservation, `MAP_FIXED_NOREPLACE`, re-exec-on-conflict, no online relocation — is specified and guaranteed against Linux. macOS and Windows are desired but secondary: they are not gated on for the Linux-complete implementation, they require per-platform base selection, and they are permitted an escape hatch (configurable base and graceful refusal) rather than the full guarantee, because dyld conventions (macOS) and the absence of `mmap`/`MAP_FIXED_NOREPLACE` semantics (Windows; `MapViewOfFileEx` fails rather than relocating) do not admit the same approach. See §5.3.
- 64-bit only for persistent arenas. 32-bit address space is insufficient for the fixed-base region reservation scheme. fyai refuses to start in persistent-arena mode on 32-bit systems.
- No dependency on systemd, launchd, or any service manager.
- Functional in containers, CI environments, RAM-constrained systems, SSH sessions, and remote development setups. Container configurations must permit `MAP_FIXED_NOREPLACE` at the configured arena base; pathological container configurations (e.g., extremely high `vm.mmap_min_addr`, restrictive SELinux policies forbidding the chosen base range) require either configuration adjustment or arena base reconfiguration.
- Local filesystem required for arena storage. NFS, SMB, FUSE, and other network filesystems are detected at arena init time and refused. Bundle export/import is the supported mechanism for cross-machine sharing.
- Suitable for distribution packaging: small binary, dependencies (libfyaml, libcurl) already present in major distribution repositories, no bundled runtime, no vendored dependencies, reproducible builds.

-----

## 10. Security and Trust

- Tool execution gated by repository allowlist, reviewable in version control.
- Allowlist matches on resolved binary path (after PATH resolution and symlink resolution), not on raw argv[0]. Argument inspection patterns may be specified per-binary.
- Network egress limited to configured inference providers.
- A `--sandbox` mode (Linux only initially) executes tool commands in a restricted user namespace with seccomp filters and bind-mounted read-only views of the repository, limiting the impact of prompt-injection-to-RCE attacks. Phase 2's transactional filesystem layer (§Appendix C.6) *severely reduces* the scope this mode needs to cover — it eliminates destructive-write risk by construction, and, via read-side interposition (redacting recognized secrets before they ever enter agent-visible state), it starves the most common confidentiality-exfiltration path at the source. But it does not make `--sandbox` fully redundant: the transaction layer bounds *filesystem effect*, not *process and network behavior*. It does nothing about network egress of data the agent legitimately holds, processes that outlive the transaction, or an agent that fetches and runs an attacker payload within its own transaction. `--sandbox` (and configured egress limits) therefore persist as a distinct defense-in-depth layer behind the transaction, severely reduced in scope — the target is that ordinary UX does not require it — but available as belt-and-suspenders rather than deprecated. The two layers are orthogonal: the transaction is the sandbox for filesystem effect; `--sandbox`/seccomp is the sandbox for process and network behavior.
- Read-side secret redaction (phase 2) is a *bounded* mechanism, not a guarantee: it protects content the classifier recognizes as sensitive (known credential paths and formats, high-entropy strings), with completeness limited by classifier coverage, and un-redacting a secret for a legitimate credential-adjacent task reintroduces exactly the surface it removed. It is correctly described as reducing the exfiltration surface for recognizable secrets, not as ensuring nothing sensitive can leave.
- Arena files may contain sensitive content; users are responsible for `.gitignore` policy. fyai provides reasonable defaults (large tool outputs ignored or content-addressed sidecar, branch references and small content committable) but does not enforce.
- Bundle export supports scrubbing of provider-specific identifiers (§7.3) for cross-organization sharing.
- No telemetry. No phone-home. fyai contacts only the configured inference provider.
- Small auditable C binary with no bundled runtime; tractable security review for enterprise and distribution inclusion.

-----

## 11. Open Questions

- Long-running invocation cancellation semantics: SIGINT during tool execution, mid-stream model output.
- Schema evolution for the arena format across fyai versions, including format migration paths when canonical-content rules change.
- KV cache integration with local inference: how the prompt-prefix-stable assembly contract is enforced from the fyai side, and how prefix-stable KV reuse interacts with llama.cpp’s slot system.
- Bundle format specification for cross-version portability.
- Tool result content can include images and binary data; the schema needs precise rules for non-text tool results. The likely home is the content-addressed blob store introduced for inference state (§5.1), on its *non-reconstructible* (data) side: a content-element variant carries a canonical handle naming the binary blob by hash. Unlike KV blobs, binary tool results dedup well — the same image or binary read across retries, branches, or parallel agents is one blob — so this is a natural and high-value second population for the blob mechanism. Precise content-element rules remain to be specified.
- Anthropic prompt-caching `cache_control` markers map naturally onto canonical prefix identity, but the placement rules at send time need specification.
- Subagent result merging: after forked subagents complete, the policy for merging divergent branches back into a parent (or selecting among them) is undefined for v1.
- Repository arena base collision: the derivation and authority model is decided (base chosen once at init, recorded value authoritative thereafter, detected collision resolved by quiesced relocation to a free slot — §4.8, §5.3, §5.12). What remains open is only the v1 *default* on a detected collision: auto-relocate to a free slot without prompting, versus refuse-and-prompt for user confirmation. Auto-relocate is the lower-friction default and is recommended unless a deployment wants the collision surfaced.
- Concurrent GC with active processes: v1 requires quiescence; v2 may relax this with epoch-based reclamation.

-----

## Appendix A: Worked Example

A minimal exchange in canonical form:

A `system` turn with content `[text: "You are a helpful coding assistant."]`. The text generic is `T_sys`. The turn generic is `S(T_sys)`.

A `user` turn with content `[text: "What's 2+2?"]`. Text generic `T_q`. Turn generic `U(T_q)`.

An `assistant` turn with content `[text: "4."]`. Text generic `T_a`. Turn generic `A(T_a)`.

The branch head is the sequence generic `Seq(S(T_sys), U(T_q), A(T_a))`.

If a different session, on the same machine, sharing the global arena, conducts the same exchange, every generic — `T_sys`, `T_q`, `T_a`, the turn generics, the sequence generic — is pointer-equal to the originals. The two sessions share storage entirely, and pointer comparison of branch heads tells you they have identical histories.

The metadata-events generic for the assistant turn contains two records: one timestamped from the original session, one timestamped from the second session with whatever model it used. Same canonical content, distinct production events.

-----

## Appendix B: Tool Call Example

A minimal tool-use exchange illustrating canonical representation vs. provider wire format.

The model issues a `read_file` call. The OpenAI wire format returned by the API:

```json
{
  "role": "assistant",
  "content": null,
  "tool_calls": [
    {
      "id": "call_abc123",
      "type": "function",
      "function": {
        "name": "read_file",
        "arguments": "{\"path\": \"foo.c\"}"
      }
    }
  ]
}
```

Followed by the tool result:

```json
{
  "role": "tool",
  "tool_call_id": "call_abc123",
  "content": "int main() { return 0; }"
}
```

The provider-stream generic stores this decoded faithfully into the arena, including `call_abc123`, with `arguments` decoded from JSON string into a proper generic (eliminating whitespace-variant duplicates).

The canonical content generic strips the provider ID and uses positional reference:

```yaml
role: assistant
content:
  - type: tool_use
    index: 0
    name: read_file
    arguments:
      path: foo.c
```

```yaml
role: tool_result
call:
  turn: 3
  index: 0
content:
  - type: text
    text: "int main() { return 0; }"
```

At emit time on xmit cache miss: positional index 0 generates a fresh deterministic `call_{hash(...)}`, wired consistently across the assistant turn and its tool result in a single pass. The original `call_abc123` lives only in the provider-stream generic.

The deduplication benefit is most significant for large tool results. A `read_file` on a substantial source file produces a tool result text generic allocated once in the arena. Subsequent reads of the same file in the same or different sessions — retries, parallel agents, branched subagents — are pointer-equal to the original. No copy, no re-parse, no re-emit until the xmit cache is populated.

-----

## Appendix C: Phase 2 — Filesystem-State Causal Log

This appendix documents the anticipated phase 2 extension to fyai: bringing filesystem effects of tool execution under the same content-addressed immutable-log discipline as conversation content. Phase 2 requires kernel support and is not part of v1 scope, but informs v1 schema decisions.

### C.1 Problem

In v1, the conversation log captures tool calls and their stdout/stderr/exit code, but does not capture the filesystem effects of those calls. A model invocation of `cc -o foo foo.c` produces a canonical `tool_result` turn recording stdout/stderr/exit, but the actual changed bytes — the new `foo` binary, the modified `foo.o`, the updated directory mtime — live in the live filesystem, outside the arena.

This creates an asymmetry. The conversation is content-addressed, immutable, branchable, and rollback-able at the arena level. The filesystem is mutable, non-versioned, and diverges from the conversation log the moment anything else touches it. Rolling back a branch rewinds the conversation but not the filesystem; forking a subagent forks the conversation context but shares the same mutable filesystem with the parent and with other subagents; replaying a conversation log produces unpredictable results because the starting filesystem state is not part of the log.

### C.2 Approach

Phase 2 closes this gap by making filesystem state a first-class reference type in the turn schema. A turn gains a fourth reference (alongside content, provider streams, metadata):

- **Filesystem-state generic**: a content-addressed delta describing the filesystem changes effected by this turn's tool execution(s).

The delta is structured (per-file content hashes, mode/owner changes, deletions, renames), content-addressed, and shares storage with identical deltas from other turns or branches. Forking a branch forks the filesystem-state chain; rolling back applies the inverse delta sequence; replay reconstructs filesystem state deterministically.

### C.3 Required Kernel Support

Userspace cannot implement this honestly. Approximations using filesystem snapshots (btrfs/ZFS/APFS), `fanotify`, FUSE overlays, `LD_PRELOAD`, or `OverlayFS` each fail one or more required properties: composability with conversation branches, content-addressing at the byte level, structural sharing across branches, deterministic replay, transparent operation regardless of how tools access the filesystem.

Phase 2 anticipates a Linux kernel subsystem providing:

- A new namespace or mount-namespace extension implementing a transactional copy-on-write layer.
- Within the namespace, all writes are captured as a content-addressed delta in a delta store.
- Reads see base + delta transparently.
- Transactions can be committed (delta becomes part of parent view), discarded (delta dropped), or persisted as named snapshots.
- Snapshots are first-class identifiers usable as the base for new transactions or namespaces.
- Operations: `fs_tx_begin`, `fs_tx_commit`, `fs_tx_abort`, `fs_tx_snapshot`, `fs_branch_from_snapshot`.

This is a substantial kernel feature, multi-year in scope, requiring upstream collaboration. It is not within v1's delivery.

### C.4 Consequences for v1 Schema

The v1 schema (§6) is designed to accommodate this extension without modification. Specifically:

- The turn generic has three references in v1, with the structure designed to extend to a fourth without breaking canonical identity of existing turns.
- The canonical content boundaries (§6.2) deliberately exclude transient identifiers, ensuring that the same canonical content corresponds to the same semantic action regardless of when or where executed — a precondition for filesystem-state attribution.
- The branch references directory generic (§5.10) and branch head sequence pattern (§6.9) generalize naturally: a phase 2 per-branch metadata entry includes both a content head and a filesystem snapshot id; structural sharing applies to both.
- Bundle export (§7.3) generalizes to include filesystem snapshot deltas as additional reachable generics.

This forward-looking design discipline is part of why §6 deserves careful specification before v1 implementation. Getting canonical content boundaries wrong in v1 means re-canonicalizing the arena in v2 to add filesystem-state references, which is the migration path the design exists to avoid.

### C.5 Capabilities Enabled

With phase 2 in place, the architectural symmetry becomes total:

- **True subagent forking**: parent and child fork both conversation context and filesystem view; subagent filesystem changes do not affect parent until/unless merged.
- **Real rollback**: undoing N turns rolls the filesystem back to that point. Experimentation becomes cheap.
- **Deterministic replay**: a conversation bundle plus a starting filesystem snapshot reproduces the agent's behavior byte-for-byte. This is reproducibility at a level no current tool offers.
- **Proper isolation of parallel agents**: each agent has its own filesystem view branched from a common ancestor; changes don't collide; merge is explicit.
- **OS-level audit**: every byte changed by every tool execution is recorded, attributable, and reversible.

Phase 2 is the long-term arc the v1 architecture is designed to support. v1 stands on its own as the best stateless AI coding tool available; phase 2 is what the architecture allows once the kernel substrate exists.

### C.6 The Safe-Yolo Model

The capabilities enumerated in §C.5 combine into a qualitative shift in how humans interact with AI coding agents. This subsection articulates the shift explicitly because it informs design decisions throughout the SRD.

**The defense problem in v1.** In v1 and in every current AI coding tool, the user faces a tradeoff between convenience and safety. Permitting the agent to execute freely (`--yolo`) is fast and matches how the agent works best, but accepts that any mistake is immediately effected on the real filesystem with only manual recovery available (`git reset`, restoration from backup, hope the agent didn't touch anything outside version control). Requiring per-tool approval makes the agent usable for the cautious but is slow, interrupts the agent's flow, and forces the user to evaluate each action in isolation without seeing its place in the larger task.

This tradeoff exists because v1 has no mechanism for separating *intent* from *effect*. When the agent runs `rm -rf build/`, the bytes are gone. The user's only opportunities to intervene are before execution (approval) or after the damage is done (recovery). There is no middle ground in which the agent's work is recorded but not yet enacted.

**Phase 2 dissolves the tradeoff.** With filesystem state captured as a transactional content-addressed delta against a baseline snapshot, the agent's work is fully visible without being applied. The agent operates in its own filesystem transaction throughout the session; nothing it does propagates to the user's live filesystem view until the user explicitly commits the result.

This means:

- The agent runs at full speed with no in-flight approval prompts. Its tool calls execute, produce results, the agent reasons about them, the loop continues. From the agent's perspective, nothing has changed from `--yolo` mode.
- The user is freed from in-flight supervision. Steering the agent (via prompts, redirection, conversation) is the user's only active job during execution.
- At the end of the session, the user reviews the proposed effect: what files would change, what would be created, what would be deleted, attributed to which turns. The user decides per-session, per-file, or per-hunk what to commit.
- Discarded sessions cost nothing. The agent's work was real, captured, and inspectable, but had zero effect on the live filesystem. Experimentation is free.

**Yolo becomes the default.** The `--yolo` flag and the per-repository allowlist (§4.5) are v1 mechanisms for mitigating an unsolved problem. In phase 2 they are obsolete:

- The allowlist's role as a *destructive-write security boundary* disappears: the agent's filesystem actions cannot harm anything because they cannot escape the transaction. Its role with respect to network and process behavior does not disappear and is covered by the reduced-scope `--sandbox` layer (§10, §C.6 below).
- The allowlist may persist as a *scope constraint*: a way to declare "the agent should not even attempt to modify `/etc`" so that review surface is bounded and unexpected attempts are surfaced as audit signals. This is a different policy with a different purpose.
- The approval-prompt flow ceases to exist as a runtime behavior. There is nothing to approve mid-flight because nothing mid-flight is real.

The `--sandbox` mode introduced in §10 as a v1 mitigation is *severely reduced in scope* by phase 2, but not eliminated. The transaction layer is the sandbox for *filesystem effect* — by virtue of being in the syscall path, it ensures no write escapes the transaction. It is not, however, a sandbox for *confidentiality and network behavior*, and the distinction must be stated plainly rather than elided:

- **Destructive-write risk is dissolved.** Nothing the agent writes propagates until commit; a wrong action costs a discarded transaction, not recovery.
- **Confidentiality risk is reduced at the source, not eliminated.** Because reads also pass through the transaction layer (reads see base+delta transparently), phase 2 can interpose on the read path: on first agent access to a file recognized as sensitive (known credential paths, `.env`, private keys, high-entropy secrets), the bytes the agent sees are redacted — whited out, or for simple container formats decompressed, scrubbed, and recompressed — so the secret never enters agent-visible state and there is nothing to exfiltrate regardless of available network paths. This is content-defined rather than destination-defined, which is why it is stronger than egress filtering for the cases it covers. But it is bounded by classification quality: it protects recognized secrets, not arbitrary sensitive content, and un-redacting for a legitimate credential-adjacent task reintroduces the surface. Each redaction is itself a recorded event in the transaction, so review (§C.7) can show "these files were redacted on read," serving both as a confidentiality artifact and as an explanation when the agent could not reason about a redacted credential.
- **Process and network behavior is not addressed by the transaction layer at all.** An agent that fetches and runs an attacker payload, exfiltrates data it legitimately holds, or abuses an internal service is outside the transaction's scope. `--sandbox` (seccomp, egress limits) remains the layer for this, behind the transaction as defense-in-depth.

So the v1 approval prompt and the v1 blast-radius framing change shape but the security surface does not collapse to nothing: phase 2 makes destructive writes free to undo and starves recognized-secret exfiltration, while egress/process containment stays as a distinct, reduced-scope layer.

### C.7 Workflow Consequences and the Review Interface

The human's role in an AI-assisted coding session changes shape in phase 2.

**v1 workflow.** Two concurrent activities:
- *Steering*: directing the agent toward the desired outcome via prompts, corrections, and conversational redirection.
- *Defense*: evaluating each significant tool call as it happens, deciding whether to permit it.

These activities conflict cognitively. Steering requires holding the larger task in mind; defense requires evaluating local actions in isolation. Fine-grained defense slows steering; loose defense invites mistakes. Most users resolve the conflict by oscillating between high-trust periods (clicking through approvals) and high-suspicion periods (reading every prompt carefully), neither of which is sustained well over a long session.

**Phase 2 workflow.** Two sequential activities:
- *Steering* during the session, unencumbered by defense considerations.
- *Review* at the session's end, with the complete proposed effect visible at once.

The cognitive shapes of these activities are now compatible with their tools. Steering is conversational and forward-looking; review is analytical and retrospective. The user is never asked to do both at once.

**The review interface.** The post-session review is the user's primary act of control in phase 2, and its UX is correspondingly load-bearing. A `fyai review` command (reserved as a v2 entry point) presents the session's proposed filesystem effect with:

- A grouped diff against the live filesystem, organized by file, directory, or change type.
- Per-change attribution to the conversation turn that produced it. The user can navigate from a changed file to the tool call that changed it, to the model's reasoning around that tool call, to the broader task context.
- Net-effect display: a file modified at turn 7 and again at turn 14 is shown as one net diff, with the intermediate state inspectable on demand.
- Scope classification: changes inside the repository (likely intended), changes inside the user's home directory (likely intended but flagged), changes outside both (flagged prominently as unexpected scope).
- Selective application: commit subsets of the delta, leave the rest in a stash-like state for later review or for the next session to incorporate.
- Branch-aware: if the session was conducted on a branched conversation, review can compare branch results against each other or against the parent, supporting an "evaluate the candidates and pick one" workflow.

The closest existing analog is `magit` or `tig` with conversation-aware attribution and a wider filesystem scope than git provides. The review interface is the locus of human control in phase 2; its quality is the project's primary v2 UX deliverable.

**Reframing fyai's value proposition.** v1 fyai sells as the architecturally-correct stateless AI coding tool — better dedup, free subagent forking, address-stable shared storage, no daemon. These are real wins for users who think in architectural terms.

v2 fyai sells as something larger: the first AI coding tool in which the user is not exposed to risk during agent execution. The agent works; the user reviews; mistakes are free. This is a different relationship to AI assistance — the agent as a colleague whose work is evaluated and accepted, rather than as a contractor whose every step must be supervised.

The asymmetry of trust that defines current tools — the user must trust the AI in the moment of execution, with no recourse if the trust was misplaced — is replaced by symmetry for *filesystem effect*: the AI proposes, the user disposes, and the cost of any individual proposal being wrong is zero for everything the review surface shows. Two honest boundaries on this claim: it holds for filesystem effects the review actually surfaces, which excludes network and process behavior (§10, §C.6); and end-of-session review trades the *interruption* failure mode of per-action approval for an *under-scrutiny* failure mode, since a large net diff invites rubber-stamping exactly as large pull requests do. The review interface's grouping, attribution, and scope-classification features (above) are the mitigations for that failure mode, and their quality is why review UX is the load-bearing v2 deliverable rather than an afterthought.

**Implications for v1 deliverables.** This reframing affects how v1 is designed and described, even though v1 does not implement phase 2:

- The `--sandbox` mode (§10) is documented as a v1-only mitigation, not a permanent architectural feature. Its deprecation path is the phase 2 transaction layer.
- The bundle format (§7.3, §11) gains long-term importance: a phase 2 bundle including filesystem deltas is a complete reproducible record of an agent session, suitable for audit, replay, sharing, and forking. The bundle spec deserves the design attention its phase 2 role implies.
- The schema's per-turn reference structure (§6.1) is explicitly required to support a per-turn filesystem-state reference, not a per-session one. Per-turn granularity is what enables per-turn rollback, per-turn attribution in review, and selective hunk-level application.
- The `fyai review` command namespace is reserved for phase 2 even though v1 does not implement it. v1 commands should not collide with the planned v2 review namespace.

The discipline of designing v1 to admit phase 2 without migration is the principal architectural constraint on v1. Most concrete v1 decisions — canonical content boundaries, reference structure, bundle format, unified in-arena storage, address stability — are justified independently for v1, and additionally serve as preparation for the phase 2 substrate.

-----

## Appendix D: Phase 3 — Inference Branching

This appendix specifies the anticipated phase 3 extension: making the token-selection distribution at each generation step a first-class production-time artifact (recorded in the metadata layer, §D.1), supporting fork operations at decision points during generation, and supporting fork operations at decision points retrospectively after a session has completed. Phase 3 adds branch-point records to the turn's metadata-events generic and a class of operations that consume them; it does not add a canonical-content reference type, because token-selection distributions are not reproducible across engines and hardware and therefore must not participate in canonical identity.

Phase 3 is not part of v1 or v2 scope. It is documented here because the architectural pattern that delivers v1 and v2 extends to v3 without modification, and because v1 schema decisions must accommodate the phase 3 reference type without migration.

### D.1 Concept

At each token generation step, an autoregressive language model produces a probability distribution over its vocabulary, from which a single token is sampled. Most distributions are sharply peaked: one token holds near-unit probability and the rest are noise. A minority of distributions are non-trivial: two or more tokens hold substantial probability mass, indicating positions at which the model's continuation is genuinely under-determined.

These non-trivial distributions correspond to decision points within the generated turn. Recording them preserves information that single-token sampling discards. In phase 3 the sampled token sequence is canonical content as before, and the turn additionally accumulates, in its **metadata-events generic** (§6.1), a sequence of branch-point records at positions where the distribution exceeded configured significance thresholds. The records are metadata about the production of the turn, not part of its canonical content.

Branch-point records are deliberately *not* part of canonical content. A token-selection distribution is a production-time fact: the top-K probabilities depend on the engine, the hardware, and floating-point reduction order (batched GPU inference is not bitwise-reproducible across batch sizes or kernel versions), so the exact values — and therefore which positions cross the entropy/top-k/mass thresholds of §D.3 — are not reproducible across environments. Were these records canonical, canonical identity would depend on non-reproducible floating-point values and cross-context dedup of identical turns would fail on low-order bits near a threshold. Placing them in metadata (where production-time facts like per-segment model identity already live, §D.5.4) avoids both problems: metadata need not be reproducible or dedup-stable. A branch-point record contains the position within the turn, the top-K alternative tokens with their probabilities, and any later-computed scoring metadata.

### D.2 Operational Modes

Phase 3 supports three operational modes, selected per session by configuration or by command-line flag.

**Mode 1: Single-path generation.** No branching during the session. Branch-point metadata is recorded passively as a side effect of normal generation. The session produces a single conversation path identical to what a non-phase-3 invocation would produce. Branch points are available for retrospective exploration (§D.7) after the session completes. This is the default mode and the cheapest in terms of inference cost.

**Mode 2: Eager pruned branching.** Branching occurs during the session at detected significant uncertainty points. Multiple branches generate in parallel. Branches whose running confidence drops below a configured threshold are pruned mid-generation to bound active compute. Surviving branches are presented at session end. This mode is more expensive than mode 1 but produces multiple candidate outcomes from a single invocation.

**Mode 3: Hybrid.** Eager branching is performed up to a configurable depth limit; deeper exploration is deferred to retrospective fork after the session completes. This combines mode 2's benefit of producing multiple candidates from one invocation with a bounded eager cost.

The user (or per-session configuration) selects the mode based on workload requirements. Sessions executed in automated contexts (CI, batch processing) typically use mode 1 for cost predictability. Interactive sessions exploring difficult problems typically use mode 2 or mode 3 for candidate diversity.

### D.3 Detection

A branch point is identified when the token-selection distribution meets configurable significance criteria. The default criteria combine three signals:

- **Entropy threshold.** The entropy of the distribution exceeds a configured value. Higher entropy indicates the model considered more alternatives with non-trivial probability.
- **Top-k ratio.** The ratio of the probability of the second-most-likely token to the most-likely token exceeds a configured value. A high ratio indicates a near-tie between the chosen token and at least one alternative.
- **Mass concentration.** The probability mass held by the top token is below a configured threshold. Low concentration indicates the distribution is spread across multiple alternatives rather than dominated by one.

A position is recorded as a branch point when all three criteria are met. The thresholds are configurable per session and per workload, and reasonable defaults are provided.

Recording captures the position, the chosen token, and the top-K alternatives with their probabilities. K is configurable; a default of 8 is suggested. Probabilities are stored with sufficient precision for downstream operations and reproducibility.

Detection adds no inference cost beyond what is required to expose the distribution at each step. Recording adds storage cost proportional to the number of branch points and the configured top-K, which is small compared to the underlying turn content for typical workloads.

### D.4 Eager Pruned Branching (Modes 2 and 3)

When operating in mode 2 or mode 3, fyai performs branching during generation. At each detected branch point, multiple branches are spawned from the top-K alternatives, generate in parallel, and are subject to runtime pruning to bound the active branch population.

#### D.4.1 Branch Spawning

When a branch point is detected during generation and the active branch budget permits additional branches, fyai forks the current branch at the detected position. The number of forks spawned is the configured branching factor (default: 2), drawn from the top-K alternatives at that position. The branching factor may be smaller than the configured K from §D.3; recording uses K alternatives, while spawning uses the branching factor B ≤ K.

Each spawned branch begins inference with the same KV cache state as the parent at the branch position, with the alternative token forced as the next sampled token. On local inference where the parent's slot is still resident, this is an in-memory slot fork at genuinely zero KV reconstruction cost — the parent KV is already in the engine. (Resuming a branch whose parent slot is *not* resident — retrospective fork, or after eviction — instead loads a stored KV blob or reprefills, with the backend-dependent costs of §E.5.) On cloud inference, this is an additional API call per branch with the prefix re-submitted.

Spawned branches are recorded in the refs directory generic (§5.10) as new branches with metadata identifying their parent and the branch point they diverged at. Spawned branches are first-class branches and may themselves spawn further branches at subsequently detected branch points.

#### D.4.2 Active Branch Budget

The active branch budget is a configured upper bound on the number of branches generating simultaneously. When a new branch would exceed the budget, fyai applies the configured budget-enforcement policy:

- **Block new branching.** Existing branches continue; the would-be new branch point is recorded but not spawned. Recording the suppressed branch preserves the option of retrospective exploration.
- **Displace the weakest existing branch.** The active branch with the lowest running confidence is pruned (§D.4.3), and the new branch takes its slot. The displaced branch is recorded as a killed branch.
- **Suspend.** Existing branches pause; the new branch generates; on its completion or pruning, suspended branches resume. This option is available only for inference backends that support slot suspension and resumption efficiently.

The default policy is to block new branching at budget exhaustion. The other policies are available where workload characteristics favor them.

#### D.4.3 Running Confidence and Pruning

Each active branch tracks a running confidence signal computed from its per-token entropy over a sliding window. The window size (default: 32 tokens) and the pruning threshold are configurable. When a branch's running confidence falls below the threshold, the branch is pruned.

Pruning is an internal control operation, not a user-facing output. The confidence signal is not surfaced as a score the user evaluates. The system uses confidence to decide which branches deserve continued compute; the user evaluates the surviving branches at session end based on their actual outputs.

Pruning terminates inference on the branch and discards its KV cache state. The branch's canonical content up to the pruning point is retained in the arena. The branch's entry in the refs directory generic is updated with status "killed" and metadata recording the pruning reason (running confidence threshold violation), the position at which pruning occurred, and the running confidence value at that position.

A grace period (default: 16 tokens) is applied after a branch is spawned, during which pruning is suspended. The grace period prevents premature termination of a branch that has just forked into an alternative trajectory and has not yet stabilized its generation pattern.

#### D.4.4 Confidence Measurement

Running confidence is derived from per-token entropy within the configured sliding window. Several measurement schemes are supported, selected by configuration:

- **Absolute floor.** A fixed threshold on mean per-token entropy. Below the floor, the branch is pruned. Simple and predictable, may be over- or under-tuned for workload-specific difficulty.
- **Relative to parent.** The branch's mean per-token entropy compared to its parent's entropy at the corresponding position. If the branch is meaningfully less confident than its parent was at the same point, the branch is pruned. Corrects for task difficulty.
- **Adaptive.** The threshold is adjusted during the session based on the observed entropy distribution across all active branches. Lenient pruning on uniformly hard sessions, stricter pruning on uniformly easy ones.

The default measurement scheme is relative to parent. The other schemes are available where workload characteristics favor them.

Running confidence is a measurement of the model's selection certainty over recent tokens, not an estimate of the branch's correctness. A branch may generate confidently into incorrect output (low entropy, wrong answer) and a branch may generate uncertainly into correct output (high entropy, eventual right answer). The pruning signal is biased toward continuation of confident generation regardless of correctness, with the rationale that confident generations reach completion and become evaluable, while uncertain generations consume compute without producing evaluable outputs.

#### D.4.5 Killed Branches as Recorded Artifacts

Pruned branches are not deleted; they are recorded with a killed status. The canonical content generated up to the pruning point is retained in the arena, contributes to deduplication with other branches that share prefixes, and is available for inspection.

Recording killed branches serves several functions:

- **Audit.** The session record shows which branches were explored and which were pruned, with the pruning reason in metadata.
- **Resumption.** A user reviewing the session may explicitly resume a killed branch from its pruning point. The KV state at the pruning point was discarded for compute savings, so resumption pays the reprefill cost; canonical content provides the prefix.
- **Training signal.** Killed branches with their running confidence trajectories are training data for tuning pruning thresholds or for training a learned pruning model (§D.4.6).

The arena garbage collector treats killed branches like any other branches: they are reachable through the refs directory generic and their content is retained until the branch entry is removed and a `fyai gc` operation collects the now-unreachable generics.

#### D.4.6 Learned Pruning (Optional Extension)

The default pruning policy is heuristic (running confidence threshold). An optional extension replaces the heuristic with a learned model that consumes a branch's recent generation history and predicts whether the branch is likely to reach a valuable completion.

The training signal is the same as for the divergence scoring model (§D.6.3): user actions in retrospective review label branches as valuable or not. A learned pruning model uses the same labeled corpus to predict "should this branch continue or be pruned" instead of (or in addition to) "is this branch point worth exploring retrospectively."

Learned pruning is available as an extension; the default deployment uses heuristic pruning. Switching to learned pruning is a configuration change and does not affect canonical content or arena format.

### D.5 Asymmetric Inference

Phase 3's detection of significant uncertainty positions enables an inference strategy in which a less-capable model handles routine generation and a more-capable model is engaged selectively at detected branching points. The routine model carries the majority of tokens; the strong model is consulted at positions where the routine model itself indicates substantive choice. This strategy reduces the share of generation that requires strong-model capability without surrendering quality at the positions where strong-model capability is consequential.

Asymmetric inference is an optional operational mechanism, available independently of the operational modes described in §D.2. It composes with all three modes: a single-path session may use asymmetric inference, an eager-branched session may use asymmetric inference within each branch, and retrospective fork continuations may use asymmetric inference for the continuation.

#### D.5.1 Concept

A session is configured with two model assignments: a *routine model* and a *strong model*. The routine model handles generation by default. At each token step, the routine model's distribution is evaluated against the branch-point detection criteria of §D.3. When the criteria fire, the escalation policy (§D.5.2) determines whether the strong model takes over generation for a bounded segment.

The routine and strong models may be:

- Two local models of different sizes (e.g., a 7B routine model with a larger local strong model).
- A local routine model paired with a cloud strong model.
- Two cloud models of different tiers (e.g., a smaller cloud model as routine, a frontier cloud model as strong).

The selection is per-session configuration. Sessions may also configure more than two models in a tiered arrangement, with escalation policy deciding which tier to engage at each detected position based on observed uncertainty and scoring.

#### D.5.2 Escalation Policy

When detection fires, the escalation policy determines whether to actually engage the strong model. Several policies are supported, selected by configuration:

- **Threshold escalation.** Escalation occurs whenever detection criteria fire above a configured escalation threshold (typically stricter than the recording threshold of §D.3). Simple and predictable; does not distinguish significant from synonymous alternatives.
- **Scored escalation.** The divergence scoring mechanism of §D.6 is applied at the detection point to estimate significance. Escalation occurs when the score exceeds a configured threshold. More principled than threshold escalation but adds latency from the scoring step.
- **Hybrid escalation.** Threshold escalation handles obviously significant positions (very high entropy or very low mass concentration) at low latency. Scored escalation handles borderline positions where threshold alone is ambiguous. This is the default policy.

The escalation threshold and scoring threshold are configurable per session. Reasonable defaults are provided; tuning per workload is expected.

Positions where detection fires but escalation does not occur are still recorded as branch points in the turn's metadata (§D.1). The routine model continues generation at the detected position. The recorded branch point remains available for retrospective fork (§D.7) and for the training corpus described in §D.6.3.

#### D.5.3 Prefix Transfer and Escalation Horizon

When escalation is triggered at position N within a turn, the strong model must begin generation from a state equivalent to having processed tokens 0..N-1 of the prefix. The mechanism for establishing this state depends on the deployment:

- **Local routine, local strong, both phase-4-capable.** The strong model independently reprefills the prefix tokens 0..N-1 against its own KV cache. The routine model's KV cache is not transferable across architectures.
- **Local routine, cloud strong.** The strong model is invoked via API with the prefix tokens 0..N-1 in the request, generated by the routine model so far, as an assistant-prefilled continuation.
- **Cloud routine, cloud strong.** Equivalent to the previous case; the request to the strong model includes the prefix.

Prefix transfer pays the cost of reprefilling on the strong model. This cost is bounded by the prefix length up to the escalation point and is incurred only at escalation events, not at every token.

The strong model generates from the escalation point for a bounded segment, the *escalation horizon*. The horizon is determined by one of several policies:

- **Fixed horizon.** A configured token count (default: 32). The strong model generates this many tokens and yields back to the routine model.
- **Distribution-bounded horizon.** The strong model generates until its own distribution becomes sharply peaked (low entropy across a configured window), indicating that it has resolved the uncertain region. Then it yields back to the routine model.
- **Cap.** A configured upper bound on horizon length, regardless of distribution behavior. Prevents the strong model from absorbing a session if it remains uncertain.

After the strong model yields, the routine model resumes generation from the strong model's last output token. The routine model reprefills (its own KV cache discarded for the escalated segment), producing routine-model KV state from the escalation point onward. Subsequent detections in the routine model's continued generation may trigger further escalations.

#### D.5.4 Recording

Per-token model identity is recorded as metadata on the turn, not as part of canonical content. The metadata indicates, for each token range within the turn, which model generated that range. Two turns with identical content but different model identity metadata canonicalize equal at the content level. The metadata is part of the metadata-events generic (§6.1) which already accommodates per-model production records; phase 3 extends this to per-segment-within-a-turn granularity.

Recording model identity per range supports audit, reproducibility, and cost analysis. A session's record shows which segments were generated by which model, allowing the user to verify that escalation occurred at expected positions and to assess the contribution of each model to the outcome.

Branch-point records (§D.9) at escalation positions are augmented with the escalation outcome: which model was engaged, the escalation horizon used, and whether the escalation altered the trajectory relative to what the routine model would have produced (determinable when the routine model's continuation from the detection point is also recorded, as it is in modes 2 and 3 with eager branching).

#### D.5.5 Cost and Quality Properties

The cost profile of asymmetric inference depends on the escalation rate, the cost ratio between routine and strong models, and the escalation horizon. The structural shape is that total cost approaches routine-model cost as the escalation rate decreases, and approaches strong-model cost as the escalation rate increases. For typical workloads with detection criteria tuned for substantive significance, escalation rates of one to a few percent of tokens are expected, producing total cost dominated by the routine model with a bounded strong-model overhead.

Specific cost ratios depend on the model pairing and provider pricing and are not committed here. The architectural property is the asymmetry itself: routine generation dominates the token count, strong generation dominates the per-token cost, and the cost-quality tradeoff is determined by the escalation policy.

Quality at escalation positions is determined by the strong model. Quality at non-escalation positions is determined by the routine model. The detection mechanism's role is to ensure that escalation occurs at positions where strong-model quality would meaningfully differ from routine-model quality. Where the routine model and strong model would produce equivalent outputs (low-entropy positions), escalation is unnecessary and is correctly avoided by the detection criteria.

A routine model that is poorly calibrated — producing low-entropy distributions at positions where its outputs are nonetheless incorrect — will under-trigger escalation and produce sessions that are routine-model-quality at positions where strong-model intervention would have helped. Calibration of the routine model is therefore a prerequisite for effective asymmetric inference. Open-weight models in the 7B-13B range typically exhibit acceptable calibration for coding workloads; calibration quality should be verified per workload before relying on asymmetric inference for production use.

#### D.5.6 Composition With Other Phase 3 Mechanisms

Asymmetric inference composes with eager pruned branching (§D.4) as follows: at a detected branch point in mode 2 or 3, the eager branching mechanism spawns multiple alternative branches; if asymmetric inference is also enabled, each spawned branch may use the strong model for generation up to its escalation horizon before yielding to the routine model. Spawning and escalation are independent operations triggered by the same detection event.

Asymmetric inference composes with retrospective fork (§D.7) as follows: a retrospective fork at a recorded branch point may be configured to use the strong model for the continuation, the routine model, or asymmetric inference. The user (or automatic policy) selects which inference strategy to apply to the retrospective branch.

Asymmetric inference composes with the scoring mechanism (§D.6) as follows: the scoring model evaluates branch points using the divergence between top-K alternatives. When asymmetric inference is enabled, the same divergence scoring can drive the escalation policy (§D.5.2) directly. The scoring model serves both retrospective ranking and live escalation decisions from a shared trained artifact.

### D.6 Scoring (Retrospective Mode)

Branch points detected during generation but not spawned (in mode 1, or in modes 2/3 when budget was exceeded) are candidates for retrospective exploration. Branch points vary widely in significance; detection alone does not distinguish those whose alternatives would lead to substantively different outcomes from those whose alternatives are near-synonyms.

Phase 3 introduces a two-stage scoring mechanism that estimates the significance of each recorded branch point for the purpose of ranking retrospective exploration candidates.

#### D.6.1 Stage 1: Divergence Sampling

For each branch point being scored, the top-K alternative tokens are extended forward by greedy continuation for a configured lookahead horizon (default: 64 tokens). This produces K short completion fragments rooted at the same prefix and diverging from one specific token onward.

Divergence sampling is performed on demand, when scoring is requested. Sessions in which scoring is never consulted incur no divergence sampling cost.

Each fragment is generated against the same model and tokenizer as the original session. The inference cost per fragment is bounded by the lookahead horizon. Total stage 1 cost per scored branch point is approximately K times the cost of generating the lookahead horizon, which is small relative to the full session cost.

For local inference, divergence sampling reuses the KV cache state at the branch position via phase 4 infrastructure (Appendix E). For cloud inference, divergence sampling requires an additional API call per alternative.

#### D.6.2 Stage 2: Divergence Scoring

A scoring model consumes the original chosen continuation and the K alternative fragments and produces a single scalar significance score per branch point. The score estimates the probability that exploring this branch point retrospectively would lead to a meaningfully different downstream trajectory than the original.

The scoring model is small relative to the generation model. The task — comparing short fragments for semantic and behavioral divergence — does not require open-ended generation capability. A model in the hundreds-of-millions to low-billions of parameters range, specialized for this task, is sufficient.

The scoring model considers multiple signals:

- **Tool-call divergence.** Whether the fragments contain tool calls, and whether those tool calls differ in name, arguments, or both.
- **Structural divergence.** For code-generation contexts, whether the fragments produce different abstract syntax tree shapes when parsed in the appropriate language.
- **Embedding distance.** Distance between fragment embeddings in a representation space, computed using the scoring model's encoder.
- **Surface-form similarity.** Token-level overlap and edit distance, used to identify cases where alternatives differ only in synonyms or stylistic variants.

The output score is a value in a configurable range (default 0 to 1) where higher scores indicate higher predicted significance. Scores are recorded as part of the branch-point metadata in the metadata-events generic (§D.1).

#### D.6.3 Scoring Model Distribution and Training Signal

The scoring model is a content-addressed artifact stored in the arena. Multiple scoring models may coexist in a deployment; users may select among them per session or per scoring operation. The scoring model is versioned, and the version used to score a given branch point is recorded as part of the metadata.

A default scoring model is distributed with fyai releases. Deployments may train deployment-specific scoring models using collected session data.

The retrospective fork operation (§D.7) and the eager pruned branching operation (§D.4) both provide training signals for the scoring model. When a user revisits a branch point retrospectively and the alternative path leads to a successful outcome, the branch point is labeled as significant. When the alternative path leads to the same or worse outcome, the branch point is labeled as not significant. When an eager-spawned branch survives pruning and is selected by the user as the preferred outcome, the branch point that spawned it is labeled as significant. Branch points that are not revisited and not spawned remain unlabeled.

This produces a corpus of labeled branch-point examples as a side effect of normal usage. The corpus is local to each deployment; aggregation across deployments is opt-in.

The corpus is used to train successor versions of the scoring model and, where used, the learned pruning model of §D.4.6. Training is performed offline, outside the inference path. Trained models are deployed as updated content-addressed artifacts in the arena.

### D.7 Retrospective Fork Operation

When a session completes with an outcome the user finds unsatisfactory, the user may consult the recorded branch points and initiate a retrospective fork at a selected point. This operation is available in all three operational modes; in mode 1 it is the primary exploration mechanism, in modes 2 and 3 it complements the eager-branching outcomes.

The fork operation creates a new branch that diverges from the original at the specified position, with a specified alternative token forced as the model's selection at that position.

The fork operation proceeds as follows:

1. The user (or automatic policy) selects a branch point and an alternative token from that branch point's recorded top-K.
2. fyai allocates a new entry in the refs directory generic for the new branch, pointing at a sequence head that shares all turns prior to the branch point's containing turn with the original branch, and CAS-publishes the updated refs directory.
3. The branch point's containing turn is reconstructed: tokens up to the branch position are retained; the alternative token is substituted at the branch position; the inference engine continues generation from the alternative token.
4. Inference proceeds from the substituted token through the remainder of the turn and into subsequent turns. The tool-use loop and approval policy apply as in any session. The eager pruned branching mechanism (§D.4) is available within the retrospective continuation if the user has selected mode 2 or mode 3.
5. The new branch accumulates turns independently of the original. Filesystem state (phase 2) and inference state (phase 4) are forked from their respective snapshots at the branch point.
6. The new branch undergoes the same review process at its completion. If the user remains unsatisfied, additional retrospective forks may be initiated from either the original branch or the new branch's recorded branch points.

The cost of the fork operation depends on the inference backend. With phase 4 infrastructure on local inference, the KV cache at the branch position is restored from the arena and inference continues without re-prefilling the prefix. Without phase 4 or on cloud inference, the prefix is re-prefilled at the cost of one additional inference call's prefill phase.

### D.8 Integration With Review Interface

The phase 2 review interface (§C.7) is extended in phase 3 with a branching surface that presents both eager-branching outcomes (modes 2 and 3) and retrospective branching candidates (all modes).

For sessions executed in mode 2 or mode 3, the review interface presents all surviving branches at session end. Each branch is shown with its conversation outcome, its filesystem effects, and its provenance (which branch point spawned it). Killed branches are not presented in the primary review surface but are available for inspection on demand. The user selects one or more surviving branches to commit, discards the rest, and may initiate retrospective forks from any branch's recorded branch points.

For sessions executed in mode 1, or for retrospective exploration in any mode, the review interface presents the recorded branch points ranked by significance score (§D.6). The ranked view shows, for each branch point: its position in the session (turn and offset), a brief excerpt of the surrounding context, the chosen token, the top alternatives with their probabilities and significance scores, and an indication of the predicted divergence shape.

The user selects a branch point and an alternative to initiate a retrospective fork. The new branch is created and the session continues. The user may initiate multiple forks from the same review session if the alternatives are independent and worth exploring in parallel.

The review interface also exposes the branch-point distribution as inspectable data independent of any actual fork operation. The distribution provides insight into where the model exercised consequential choice during the session.

### D.9 Recording

Branch-point information is recorded in the assistant turn's **metadata-events generic** (§6.1, §D.1), not in canonical content. A turn's representation in phase 3 has the form:

- The canonical content payload: the sequence of generated content elements (text, tool_use), unchanged from earlier phases and unaffected by phase 3.
- A metadata sequence of branch-point records, each containing position within the turn, top-K alternatives with probabilities, and optional scoring metadata.

Two assistant turns are canonical-equal when their content elements match, *regardless* of their branch-point records. A session that records branch points and a session that does not collapse to the same canonical turn when their content elements are identical — which is the desired behavior, since the sampled content is the same semantic action whether or not the distribution around it was captured. Branch-point records distinguish the turns only at the production-event level, exactly as differing timestamps or model identity do.

This placement is deliberate and resolves a determinism problem. Branch-point records are *not* reproducible across environments: top-K probabilities depend on engine, hardware, and floating-point reduction order, and even local engines vary unless they document strict floating-point determinism. Were these records canonical, identity would hinge on non-reproducible low-order bits and cross-context dedup of identical turns would fail near a detection threshold. As metadata, they carry no reproducibility or dedup-stability requirement; the recorded distributions are accurate snapshots of one specific inference call, which is all retrospective fork and scoring require.

Scoring metadata is added to branch-point records after scoring is performed: scoring model version, timestamp, resulting score. Multiple scoring entries may accumulate on a single branch-point record if it is rescored with newer or different scoring models. All of this lives in the metadata layer.

Per-token entropy data, used for running confidence in eager pruned branching (§D.4.3), is stored as optional metadata on the turn. Two turns with identical content and different entropy metadata canonicalize equal. Entropy data is recoverable in principle by re-running inference and is treated as a derived artifact retained for efficiency.

Per-segment model identity, used for asymmetric inference (§D.5.4), is stored as metadata on the turn. Two turns with identical content generated by different model assignments canonicalize equal at the content level; the model identity metadata distinguishes them at the production-event level.

Killed branches are recorded with a "killed" status in the refs directory generic. Per-branch metadata for killed branches includes the pruning reason, the position at which pruning occurred, and the running confidence value at that position. Killed branches share canonical content with their unkilled prefix through structural sharing.

### D.10 Provider Asymmetry

Phase 3 requires the inference provider to expose the token-selection distribution at each generation step. Providers vary in their support for this capability, and the practicality of each operational mode varies accordingly.

- **Local inference engines** (llama.cpp, vLLM, SGLang, and others) expose the full distribution natively. All three operational modes operate at native efficiency. Eager pruned branching is particularly well-suited to local inference because slot fork is cheap and pruning frees inference slots immediately.
- **OpenAI-compatible APIs** (OpenAI, Together, Fireworks, OpenRouter, and others) expose the top-K alternatives via the `logprobs` and `top_logprobs` parameters, with K typically capped at 20. Phase 3 detection operates on the exposed top-K. Mode 1 (single-path generation with retrospective fork available) is well-supported. Modes 2 and 3 (eager branching) are feasible but expensive: each branch requires an additional API call, and pruning a branch wastes the tokens generated so far. The cost overhead may be acceptable for high-value sessions but is not the default for routine sessions.
- **Google Gemini** exposes top-K alternatives via the `responseLogprobs` parameter. Operation is analogous to OpenAI-compatible APIs.
- **Anthropic** does not currently expose token-level probability data on the messages API. Phase 3 detection is not available on Anthropic-native inference at this time. fyai's provider abstraction (§4.7) declares the logprobs capability per provider; when the capability is absent, phase 3 is disabled for that provider, and the user is informed.

The provider abstraction surfaces the available capabilities to the user. Sessions conducted against providers without logprobs support proceed normally but do not produce branch-point records, and phase 3 operations are not available for them.

The cost profile of eager pruned branching on local inference compared to cloud inference is one of the more pronounced asymmetries in the architecture. On local inference, the marginal cost of an additional branch is the cost of generating its tokens, and pruning frees inference capacity immediately. On cloud inference, the marginal cost of an additional branch is the cost of a full API call including prefix prefill, and pruning wastes whatever inference has been performed. Eager branching is the default-on capability for local inference deployments and an opt-in for cost-aware cloud deployments.

Asymmetric inference (§D.5) operates across providers in three configurations. With routine and strong models both local, escalation cost is the strong-model prefill plus its horizon, paid in local compute. With routine local and strong cloud, escalation cost is one API call per escalation event, including prefill of the prefix on the cloud strong model. With both routine and strong on cloud, escalation cost is one API call per escalation event against the strong model's tier. The detection mechanism (§D.3) requires logprobs support on the routine model only; the strong model is not required to expose logprobs because it is engaged only during the escalation horizon and not used for branch-point detection within that horizon.

### D.11 Schema Forward Compatibility

The phase 3 branch-point records extend the turn's metadata-events generic following the same pattern by which metadata already accommodates per-model production records (§6.1). Existing canonical turns from v1 and v2 are forward-compatible: they have no branch-point records, and since the records are metadata rather than content, their absence does not affect canonical identity in any case — a pre-phase-3 turn and a phase-3 turn with identical content are canonical-equal.

Bundle export (§7.3) extends to optionally include branch-point records from the metadata layer. A scrubbed export may omit them if the user prefers not to share inference-uncertainty data; a `--scrub-branch-points` flag is added to the bundle export interface. Because the records are metadata, omitting them never alters the canonical content of the exported turns.

The arena allocator and storage substrate (§5.4, §5.5) handle branch-point record storage identically to other metadata generics. Killed-branch metadata in the refs directory generic uses the same CAS-published update mechanism as any other refs directory update (§5.10). No changes to the arena format are required.

-----

## Appendix E: Phase 4 — Content-Addressed Inference State

This appendix specifies the anticipated phase 4 extension: storing KV cache state as content-addressed *cache blobs* referenced by canonical handle generics, enabling efficient fork operations against local inference engines. Phase 4 adds a turn-level inference-state handle and is principally an optimization of the phase 3 fork operations — both eager branch spawning (§D.4) and retrospective fork (§D.7) — for local inference. It is never a correctness dependency: the reprefill path is always available and is the performance floor.

Phase 4 is not part of v1, v2, or v3 scope. It is documented here for the same forward-compatibility reasons as the preceding appendices.

### E.1 Concept

The transformer KV cache at any token position is a function of the model, the tokenizer, the sampling decisions, the prefix tokens — *and* the engine and hardware, since floating-point reduction order varies across backends and is not bitwise-reproducible in general. KV cache state is therefore **reconstructible but not canonical**: it can always be regenerated by reprefilling the prefix against the same engine, but two engines (or two hardware configurations) produce different bytes for the same logical prefix. This is the defining difference from conversation content and filesystem state, which *are* canonical, and it dictates how KV is stored.

Phase 4 stores KV cache deltas as content-addressed **blobs** (named by BLAKE3 hash, §5.1) on the reconstructible-cache side of the cache/data line (§5.7). A turn may carry an inference-state *handle* — a canonical generic recording model identity, tokenizer identity, sampling parameters, engine/determinism class, and the token range, pointing at the blob that holds the KV bytes for that range. The handle is canonical and dedupable; the blob it names is opaque bytes verified by hash, not a canonical generic, and is not required to dedup across engines (it generally will not, since the bytes differ). Because KV is reconstructible, losing a KV blob is a performance event — a reprefill — not a data-loss event, and KV blobs live under `$XDG_CACHE_HOME` and are evicted by cache policy independently of `fyai gc` (§5.7, §E.7).

The principal beneficiaries are the phase 3 fork operations. Eager branch spawning (§D.4.1) forks an inference slot at the branch position with the alternative token forced as the next sampled token. Retrospective fork (§D.7) resumes generation from a previously recorded branch position with an alternative token. Both require the engine to begin from a specific KV cache state. Without phase 4, the engine reprefills the prefix to reconstruct that state. With phase 4 and the blob present, the state is loaded directly and inference resumes from the alternative token without re-prefill — subject to the backend-dependent savings discussed in §E.5.

### E.2 Applicability

Phase 4 applies only to inference backends that expose primitives for direct KV cache state management. Local inference engines that support these primitives include llama.cpp, vLLM, SGLang, and similar high-throughput inference servers, subject to the specific KV management APIs each provides.

Cloud inference providers do not currently expose KV cache state management to clients. Phase 4 has no effect on cloud-provider sessions; fork operations against cloud providers continue to operate via the prefill mechanism described in §D.7.

The phase 4 reference type is recorded only when the inference backend supports it. The absence of a phase 4 reference does not affect canonical identity of a turn; canonical content remains the determinant of turn identity, and phase 4 storage is an optimization that does not alter semantics.

### E.3 Storage

KV cache deltas are stored as content-addressed blobs in the blob store (§5.1) on the reconstructible-cache side (§5.7), under `$XDG_CACHE_HOME/fyai/blobs/`, not in the durable arena. The blob store uses the same allocation and mmap machinery as the arena, but its retention is cache policy (§E.7), decoupled from arena garbage collection and its quiescence requirement: evicting a KV blob is unlinking bytes and clearing a handle, not a structural arena mutation. Chunk sizes for the KV blob store may be tuned independently; KV deltas for long contexts are large, and larger chunks reduce per-blob metadata overhead.

Each inference-state *handle* (a canonical generic) contains:

- The model identity: a canonical reference to the model identity generic (model name, weights hash, quantization, and other parameters affecting inference output).
- The tokenizer identity: a canonical reference identifying the tokenizer version.
- The sampling parameters, including the engine/determinism class (§E.4) — exact versus approximate (§E.8) — so that an approximate-KV resume is never mistaken for an exact one.
- The token range within the source turn that this KV delta covers.
- The BLAKE3 hash naming the KV blob, plus the layout tag identifying which engine layout the blob holds.

The KV tensor data itself — per-layer, per-head keys and values for the covered range — is the blob, opaque bytes named by hash. Layout and quantization are per-engine implementation policy; the SRD does not mandate a single layout. The same logical KV delta may exist as multiple blobs in multiple layouts if multiple engines are in use; each is a distinct hash-named blob referenced by its own handle. BLAKE3 here earns integrity and stable naming; it does not generally earn cross-context dedup for KV, because the bytes differ by engine and hardware (unlike canonical content, which dedups heavily). This is expected and not a regression.

### E.4 Inference Engine Requirements

Phase 4 requires the inference engine to support the following operations:

- **KV slot population from external memory.** Given a buffer or arena chunk reference containing a serialized KV delta, populate an inference slot with that state. The operation must accept KV deltas in the layout fyai produces for that engine.
- **KV delta extraction.** After processing a token range, expose the resulting KV delta in a serializable form. For an *exact* determinism class, the extracted delta must round-trip: extracting after processing tokens, then loading into a fresh slot, must produce a slot state equivalent to having processed those tokens. An *approximate* class (e.g. lossy-compressed or aggressively quantized KV, §E.8) relaxes "equivalent" to "close," with the consequences for reproducibility spelled out in §E.8.
- **Slot fork.** Duplicate an inference slot's KV state to a new slot for divergent continuation. This operation is supported natively by inference engines that handle parallel sampling; phase 4 requires that the fork point be configurable to an arbitrary position within the slot's history, not only at the end.
- **Deterministic inference.** Given identical model weights, identical prefix, identical KV state, and identical sampling parameters, produce identical output tokens. Floating-point determinism guarantees vary by engine and hardware; phase 4 requires that the engine document its determinism properties and that fyai record the determinism class (including exact-versus-approximate KV) as part of the sampling parameters carried on the inference-state handle (§E.3).

Inference engines that meet these requirements may be integrated as phase-4-capable backends. Inference engines that meet only a subset may still be used; the missing primitives degrade specific operations (e.g., absence of arbitrary-position slot fork prevents efficient retrospective forking but does not prevent normal session execution).

### E.5 Fork Operations With Phase 4

Both eager branch spawning (§D.4.1) and retrospective fork (§D.7) are modified in phase 4 as follows:

1. The user selects a branch point and an alternative token.
2. fyai locates the inference-state handle for the KV cache state at the branch position, identified canonically via model identity, sampling parameters, engine/determinism class, and prefix position. The handle names a KV blob by hash.
3. If the named blob is present in the cache, fyai loads it into the engine's slot via the external-memory population primitive. If it is absent (evicted, or never produced on this engine), fyai falls back to reprefill — the always-available floor.
4. The engine resumes generation from the alternative token. On a blob hit, no prefix tokens are reprocessed.
5. Subsequent generation proceeds as in any session; the new turn's KV delta is extracted, stored as a new hash-named blob, and referenced by a new handle on the new branch.

Savings over reprefill are **backend-dependent**, and the SRD does not bank them unconditionally:

- **CPU and unified-memory engines** consuming the KV blob directly from mmap'd cache pages realize the largest savings: loading is a memory copy or zero-copy, and the model is not run.
- **Discrete-GPU engines** must transfer the blob host-to-device; that transfer cost is comparable to materializing the KV by reprefill, so the win shrinks and is realized mainly for long prefixes where transfer bandwidth beats recompute. For short prefixes the two are close and reprefill may be preferable.

In all cases reprefill is the correctness floor; phase 4 is an optimization above it, never a dependency. §F.2's cost contribution for phase 4 should be read with this backend sensitivity, not as a flat elimination of prefill cost.

### E.6 Parallel Agent Memory

KV blobs participate in the same mmap-based memory sharing as other arena-resident data (§5.9). Multiple inference processes operating against the same blob store map the same blob pages, present once in the kernel page cache regardless of the number of consumers.

For local inference where multiple sessions share a common conversation prefix, this sharing extends to the KV cache layer: the prefix KV is present once in arena pages, and each session's inference engine maps it from the arena. The memory cost of running N parallel sessions against a shared prefix is dominated by the divergent suffixes, not by the prefix.

GPU-backed engines that require KV cache to be resident in device memory do not benefit from page cache sharing directly; each engine must independently transfer the shared KV to its device. CPU-backed engines and engines using unified memory architectures benefit directly.

### E.7 Storage Bounds and Garbage Collection

KV blobs are large relative to canonical content; a long session can accumulate gigabytes across its turn sequence. Because KV blobs are reconstructible cache (§5.7), their reclamation is **cache eviction, decoupled from `fyai gc`** and its quiescence requirement: dropping a KV blob unlinks bytes and clears the referencing handle's blob pointer, which is not a structural arena mutation and needs no exclusive lock. The arena GC (§5.12) walks and compacts only the small durable canonical set and never copies regenerable KV bytes. Phase 4 cache policy levers:

- **Time-based eviction.** KV blobs not accessed within a configured window are discarded. The canonical content and the handle remain; only the blob pointer is cleared. Subsequent fork at affected positions falls back to reprefill.
- **Branch-aware retention.** KV blobs on currently active branches are retained preferentially over those on archived branches.
- **Tiered storage.** KV blobs may be moved to secondary storage (slower or networked) as they age, with retrieval cost paid only on access — the mechanism for the "warm resume of a long-dormant session" want, without promoting KV to durable data.

These policies are configurable. A reasonable default evicts on cache pressure and on idle timeout; retaining KV for the full arena lifetime is available but is rarely the right default given KV size.

### E.8 Schema Compatibility and Reproducibility Class

The phase 4 inference-state handle extends the turn schema following the same optional-reference pattern as preceding phases. The handle is optional on any turn; turns without it remain canonically valid. Its presence never alters canonical content identity — two turns with identical content but differing inference-state presence are canonical-equal — because the handle records a reconstructible cache artifact, not semantic content.

Because KV blobs are cache (§5.7), they are **scrubbed from bundles by default**: the bundle export interface includes inference state only on explicit opt-in (`--include-inference-state`), since the blobs are large, engine-specific, and useless to a recipient on a different engine (§E.9). This is the correct polarity for a reconstructible artifact — inclusion is the exception, omission the default — and it differs from canonical content, which exports by default.

**Reproducibility class.** A session may use *exact* KV (round-trip-faithful, §E.4) or *approximate* KV (lossy compression or aggressive quantization, traded for storage). Approximate KV is permitted but is not free of consequence: resuming from an approximate KV state produces a continuation that may diverge from what an exact reprefill would have produced, which weakens the byte-for-byte replay guarantee (§C.5) for any branch that resumed from approximate state. The determinism class on the handle (§E.3, §E.4) records which was used, so a session or branch that relied on approximate KV is honestly marked as belonging to a weaker reproducibility class rather than silently claiming exact replay. Implementations may default to exact KV and offer approximate KV as an explicit storage-versus-reproducibility trade.

### E.9 Out of Scope

The following are explicitly out of scope for phase 4:

- **Cross-engine KV transfer.** A KV delta produced by one inference engine is not generally usable by another, even for the same model, because layouts and numerical conventions differ. Phase 4 does not attempt to define an engine-portable KV format.
- **Cross-model KV transfer.** KV state is specific to the model that produced it. Phase 4 does not support reusing KV state across different model identities.
- **Synthesis of forked KV states.** Combining KV state from multiple forked branches into a single integrated state is research-grade and is not addressed by phase 4. Fork operations produce independent branches; subsequent reasoning over multiple branches' outputs occurs at the canonical content layer, not at the KV layer.

These omissions are deliberate. Phase 4 delivers the optimization that is well-defined and achievable with current inference engine capabilities. Extensions beyond this scope are deferred pending advances in inference engine and model architecture.

-----

## Appendix F: Cost Composition

This appendix describes the cost structure that the architecture services. It is included because the architectural choices specified throughout the document are individually justified on their own terms, but their combined effect on operating cost is not visible from any single section. This appendix makes the integrative cost picture explicit.

### F.1 Framing

The unit of measurement is dollars per successful outcome, not dollars per inference call. An outcome is the completion of a task — code written and tested, refactor performed and verified, bug diagnosed and fixed — to the user's acceptance. The cost of producing an outcome includes inference cost, failure-recovery cost, human supervision cost, and any other cost component the production of that outcome consumed.

This unit differs from the unit by which inference is conventionally priced. Inference providers price per token. Tools built on inference inherit that pricing surface. The user, however, does not consume tokens; the user consumes outcomes. The gap between dollars-per-token and dollars-per-outcome is filled by everything the system does between receiving a task and delivering an accepted result: the number of attempts required, the work wasted on failed attempts, the human time spent reviewing and correcting, and the share of tokens that needed to be expensive versus cheap.

The architecture is constructed to make the dollars-per-outcome quantity favorable across a workload mix typical of agentic coding tasks. Each phase contributes to this quantity through a specific cost-reduction mechanism. The contributions compose: each is independently meaningful, and the combination is what makes the overall cost position defensible.

### F.2 Phase Contributions

Each phase reduces a specific component of the dollars-per-outcome quantity.

**Phase 1 (stateless invocation, content-addressed arena).** Eliminates the cost of maintaining a resident process between invocations. Conventional agentic tools either run as daemons, consuming memory continuously regardless of activity, or restart from cold on each invocation, paying full context reload time on every interaction. Phase 1 supports concurrent invocations sharing arena pages through the kernel page cache, with no resident state between sessions. The cost contribution is the elimination of a baseline running cost that would otherwise be paid per active session per unit of wall-clock time.

**Phase 2 (filesystem-state causal log).** Eliminates the cost of recovering from agent actions that produced unwanted side effects. Without transactional filesystem isolation, a failed or wrong agent action leaves persistent artifacts that require manual cleanup, rollback via version control (when possible), or restoration from backup (when not). The human time consumed by these operations is a real cost component of producing outcomes with current tools, and it scales with the failure rate. Phase 2 reduces the cost of any individual failure to zero: the transaction is discarded, the filesystem returns to its prior state, and the user proceeds without manual recovery work. The cost contribution is the removal of recovery cost as a function of failure rate.

**Phase 3 detection and asymmetric inference.** Routes routine tokens to inference at the lowest per-token cost capable of producing them, and engages higher-cost inference selectively at positions where the higher cost is justified. The per-token cost ratio between routine and frontier inference is typically large; the proportion of tokens that require frontier capability is typically small. The cost contribution is a multiplicative reduction in average per-token inference cost, with magnitude determined by the workload's distribution of routine versus consequential positions.

**Phase 3 multi-sampling and pruned eager branching.** Increases the probability that a single invocation produces an accepted outcome by exploring multiple candidate continuations at significant decision points and selecting the best result. Without multi-sampling, sessions that produce unsatisfactory outcomes are re-run from the beginning, paying full inference cost for each retry. Multi-sampling pays incremental cost per additional branch — bounded by the divergent suffix length, not the full session — and reduces the expected number of full retries per successful outcome. The cost contribution is a reduction in the retry multiplier on the expected cost of an outcome.

**Phase 4 (content-addressed inference-state cache).** Reduces the prefill cost on fork and resume operations, making the multi-sampling and retrospective-fork mechanisms of phase 3 cheaper to use routinely. Without phase 4, each branch or retry pays full prefix re-prefill cost; the cost of exploration scales with prefix length and discourages frequent use. With phase 4 and a cached KV blob present, branches resume from stored KV state and pay only the divergent suffix cost. The magnitude is backend-dependent: large for CPU and unified-memory engines that consume KV directly from mmap'd pages, marginal for discrete-GPU engines that must transfer KV host-to-device at a cost comparable to recompute (§E.5). The cost contribution is therefore "available and cheaper on the backends where it helps," with reprefill as the floor everywhere — not an unconditional elimination of prefill cost.

**Execution retry within the tool-use loop.** Reduces the failure rate at the tool-call layer by catching malformed model output and providing corrective signals within the same invocation. Without this mechanism, format failures and protocol mismatches surface as failed sessions that require manual intervention or full retry. With this mechanism, the small model's known fidelity issues are absorbed by the surrounding system and do not contribute to outcome failure rate. The cost contribution is a reduction in the failure rate component of the per-outcome cost equation.

### F.3 Composition

The contributions in §F.2 are not independent in the cost equation; they multiply.

The cost of producing an outcome can be approximated as:

```
cost_per_outcome ≈
  inference_cost_per_token × tokens_per_attempt × attempts_per_outcome
  + recovery_cost_per_failure × failures_per_outcome
  + supervision_cost_per_session × sessions_per_outcome
```

Each phase reduces one or more factors in this equation. Phase 3 asymmetric inference reduces `inference_cost_per_token` by routing most tokens to cheaper models. Phase 3 multi-sampling and execution retry reduce `attempts_per_outcome` and `sessions_per_outcome` by increasing per-attempt success probability. Phase 2 reduces `recovery_cost_per_failure` to near zero. Phase 4 reduces the marginal cost of additional branches enough that phase 3's mechanisms can be applied routinely rather than sparingly.

The composition is multiplicative across factors. A workload that benefits from each of inference-cost reduction, attempt-count reduction, and recovery-cost reduction realizes the product of the individual reductions, not their sum.

For workloads in which all three factors are reducible — typical of agentic coding tasks where most tokens are routine, decisions are concentrated at specific positions, and failures are common but recoverable — the integrated reduction in cost per outcome is substantially larger than any individual mechanism would suggest.

### F.4 Workload Sensitivity

The cost reduction realized in practice depends on workload characteristics.

For workloads in which most tokens require frontier capability — tasks where the underlying reasoning difficulty is high throughout, where small models cannot produce viable continuations even on routine positions — the asymmetric inference mechanism provides minimal savings because escalation occurs constantly. For these workloads, the architecture still provides phase 2 rollback and phase 3 retrospective exploration, but the per-token cost reduction is small.

For workloads in which most tokens are routine and decisions are clustered — typical of well-scoped coding tasks, refactors, bug fixes, and the broader category of work that involves substantial mechanical generation around a small number of consequential choices — the asymmetric inference mechanism provides the largest savings, and the composition with the other mechanisms delivers the integrated reduction described in §F.3.

The architecture is well-suited to the second workload category and provides bounded benefit on the first. The proportion of real-world agentic work that falls into each category is an empirical question whose answer determines the architecture's aggregate value. Available evidence from production AI coding tool usage patterns suggests that the second category is the larger share, but the precise proportion is workload-specific and deployment-specific.

### F.5 Scope and Honest Boundaries

The architecture does not address tasks where the underlying reasoning capability gap between routine and frontier models is the binding constraint. If a task requires frontier-class reasoning at most positions, no surrounding system can substitute small-model inference for frontier inference without quality loss. The architecture provides mechanisms that reduce cost on tasks where this constraint does not bind; it does not claim to overcome the constraint where it does.

The architecture does not address tasks for which agentic execution is the wrong tool entirely. Some user needs are better served by direct human work, by non-AI tooling, or by simple frontier-model chat without the orchestration overhead. The cost structure described here applies to tasks for which agentic execution is the right approach; for other tasks, the comparison is to a different baseline.

The cost reduction figures depend on inference provider pricing, which varies and changes over time. The structural argument — that routing tokens to inference at the lowest cost capable of producing them, recovering failures cheaply, and exploring alternatives cheaply, reduces total cost per outcome — holds across pricing scenarios. The magnitudes depend on the specific ratios at any given time.

### F.6 Implications for Architectural Discipline

The cost composition described above depends on the architectural properties specified throughout this document. Each cost-reduction mechanism requires specific properties from the substrate:

- Multi-sampling at low marginal cost is *helped* by content-addressed inference-state cache (phase 4) on backends where KV load beats reprefill; where it does not (discrete GPU, short prefixes), reprefill remains the floor and multi-sampling still functions, at higher marginal cost.
- Filesystem rollback at zero cost requires the transactional namespace layer (phase 2) with content-addressed deltas.
- Asymmetric inference requires distribution-level branch detection (phase 3) and provider abstraction (§4.7) supporting heterogeneous model assignments within a session.
- Retrospective exploration requires canonical recording of decision points (phase 3) and persistent address-stable storage (§5.3) for forking from prior session states.
- Execution retry requires the tool-use loop to expose intermediate state for corrective injection without restarting the session.

Each of these requirements is independently justified within its respective section of the document. The cost-composition perspective makes their interdependence visible: each mechanism contributes to the cost equation, and each mechanism depends on substrate properties specified for reasons that may appear unrelated to cost at the point of specification. The disciplined preservation of these substrate properties across implementation is therefore a prerequisite for realizing the cost composition described here.
