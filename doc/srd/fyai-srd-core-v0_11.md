# Software Requirements Document: fyai — Core (v1)

**Version:** 0.11 draft
**Status:** Pre-implementation
**Author:** Pantelis Antoniou

*Revision note (0.11): incorporates an architecture review pass, and splits the specification into a core document plus companion phase documents (see Companion Documents, end of file). Principal changes in core: refs-update durability ordering made normative, with the recovery history carried as an inline predecessor back-chain inside the directory generic rather than a separate reflog or chunk-0 field (§5.10, §5.12); VMA reservation strategy specified concretely (§5.3); arena base derived once at init and never recomputed (§4.8, §5.3); reload/start performance restated as three regimes with measured figures (§8); inference-state reclassified as reconstructible cache addressed by content hash, not as canonical arena data (§5.1, §5.13); branch-point records moved to the metadata layer (§6.1); safe-yolo confidentiality story corrected and `--sandbox` repositioned as defense-in-depth (§4.5, §10); `--dry-run` semantics corrected for the tool-use loop (§4.5); minor canonicalization and wording fixes (§6.5, §6.6, §7.2). Corresponding changes in the phase documents: Phase 2 (safe-yolo, read-side redaction), Phase 3 (branch-point records in metadata), Phase 4 (KV as content-addressed cache blobs behind canonical handles; backend-dependent savings; reproducibility class).*

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
- The refs head pointer (§5.10), pointing at the current branch references directory generic. The recovery history is carried inline as a predecessor back-chain within the directory generics themselves (§5.10), not as a separate chunk-0 field.
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

**Inline recovery chain.** Each refs directory generic additionally carries a reference to its immediate predecessor directory generic. Because directories are immutable and content-addressed, this back-reference forms an immutable singly-linked history at no extra cost: publishing a new directory implicitly publishes the entire chain reachable from it. There is no separate reflog, no second mutable field, and no second CAS — the recovery history is a property *of* the head generic, not an independent structure updated alongside it. This preserves the invariant that the refs head pointer is the sole refs-related mutable word in the arena. The chain has a bounded retained depth: directories older than the retention bound are no longer referenced by any live directory's prefix-of-interest and become eligible for `fyai gc`, which trims the tail of the chain.

**Refs head pointer.** Chunk 0's header contains a pointer to the current refs directory generic, in a dedicated 8-byte aligned field designated for atomic operations. This pointer is the only mutable field in the arena beyond the chunk-list head and the allocator bump pointers. All three follow the same CAS-based concurrency discipline.

**Update protocol.** A process updating one or more branch references proceeds optimistically:

1. Atomic-load the current refs head pointer.
2. Walk the current refs directory generic to read existing entries.
3. Allocate a new directory generic representing the desired updated mapping, recording a back-reference to the observed current directory as its predecessor. Unchanged entries share storage with the prior directory by construction.
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

**Recovery.** On process startup, the refs head pointer is read from chunk 0's header. If the pointed-to directory generic is found via its in-arena address and passes integrity verification (header magic, structural checksums), the refs are loaded. If verification fails (incomplete write-back at the time of a prior crash), recovery follows the directory's inline predecessor back-reference (above) to the prior directory generic, and continues down the chain until it reaches one that verifies — which, given the durability ordering invariant, is guaranteed durable because it was a predecessor of a successfully published head. Because the history is carried inside the directory generics rather than in a separate chunk-0 structure, no distinct reflog is consulted; the back-chain *is* the history. The retained depth of the chain bounds how far recovery can walk; in normal operation the head itself verifies and the chain is never traversed.

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

**Refs history trimming.** The live roots are the current refs directory generic and everything it references. The predecessor back-chain (§5.10) beyond the configured retention depth is not a live root: GC's mark phase follows the chain only to the retention bound, so older directory generics in the tail (and any storage uniquely theirs) are collected. This is how the bounded recovery history is enforced — by GC trimming the tail of the chain, not by mutating any chunk-0 field.

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

## Companion Documents

This is the core v1 specification. The anticipated future phases and the cross-phase cost analysis are maintained as separate companion documents, each of which extends this core and refers back to it by section number (e.g. "core §6.2"):

- **fyai-srd-phase2-filesystem** — Phase 2: filesystem-state causal log (transactional agent effects, safe-yolo, review interface).
- **fyai-srd-phase3-branching** — Phase 3: inference branching (distribution capture, eager and retrospective fork, asymmetric inference).
- **fyai-srd-phase4-inference-state** — Phase 4: content-addressed inference-state cache (KV-delta blobs behind canonical handles).
- **fyai-srd-cost-composition** — Cross-phase cost composition: how v1 and the phases compose into the overall cost model.

The phases are not part of v1, v2, or v3 scope; they are specified to ensure the v1 schema and storage substrate admit them without migration.
