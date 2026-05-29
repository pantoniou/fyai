# fyai SRD — Architecture Review Decision Log

Records the disposition of each item raised in the v0.10 architecture review and applied in v0.11. Format: **item → resolution → rationale**. Severity tiers are from the original review (Tier 1 = load-bearing claims, Tier 2 = internal tensions / security, Tier 3 = precision/gaps).

---

## Tier 1 — load-bearing claims

### 1.1 Fixed-base VMA reservation fragility
**Resolution (applied §5.3, §4.8, §9).** Reservation moved to an `__attribute__((constructor))`; dependencies static-linked or lazily `dlopen`'d so no ASLR-placed object can claim the window before reservation; conflict handled by bounded re-exec until a clean layout is drawn, with an attempt counter and a loud diagnostic on exhaustion. Repository base derived once at `fyai init` and recorded as authoritative — never recomputed from path/inode at map time. Address stability declared a Linux-first guarantee; macOS/Windows secondary with a configurable-base escape hatch. Relocation-on-conflict is a quiesced operation under the arena exclusive lock (GC posture), never online.
**Rationale.** "Early in main()" is too late on glibc — the loader has already mapped libraries. The constructor + static linkage makes the reservation *preventive* rather than merely *detective*; re-exec handles the residual ASLR draw with negligible expected retries given a 64 GiB window in a 47-bit space. Inodes/paths are unstable (backup restore, rsync, bind mounts, dir moves); recomputing a pointer-identity-load-bearing base from them would corrupt a live arena, so the recorded value must be authoritative. Relocation is mechanically cheap but breaks cross-process pointer identity if done under concurrent mappers — hence quiesced, reusing the GC lock rather than inventing a new path.
**Status:** resolved. The arbitrary-import VMA-carving problem (no central authority for base allocation across unrelated projects) is acknowledged as a "when fyai is big enough to matter" problem, addressable per-user/per-org, not a v1 blocker.

### 1.2 Refs head CAS durability ordering
**Resolution (applied §5.10).** A normative invariant added: *a refs head pointer must never become durable before the directory generic it points at — and everything that directory newly references — is durable.* Enforced by `msync(MS_SYNC)`/`fsync` of the pointee to completion, then the head write, then flush of chunk 0. Three durability tiers defined: scratch/worker arena (no ordering, evaporates on crash by design), durable arena between checkpoints (CAS for visibility, no msync, batchable), durability checkpoint (commit / `fyai sync` / CI-exit — barrier enforced). Worker→parent promotion materializes the divergent suffix into the durable parent *before* swapping the parent head, mirroring the GC survivor-copy discipline.
**Rationale.** Visibility (page-cache coherency) and durability (write-back) are independent; head pointer and pointee are separate dirty pages the kernel may write back in either order, so a crash can leave a durable pointer into non-durable bytes, and integrity-checking a structure reached through such a pointer is unsound. The barrier is the only correct fix; the recovery-history scan is a backstop for partial writes, not a substitute. The stacked-arena split makes the common case (worker churn) pay nothing, since workers never checkpoint.
**Status:** resolved. Per the author, the durable parent performs a full checkpoint per refs update unconditionally (refs updates are infrequent relative to work, so this is not a contention point) — the batching tier is available but not required.
**Reflog refinement (post-review).** The recovery history is carried as an inline predecessor back-reference *inside* each refs directory generic, forming an immutable back-chain, rather than as a separate reflog or a chunk-0 history field. The single head-pointer CAS that publishes a new directory thereby publishes the entire history; there is no second mutable word and no second CAS, preserving the "refs head pointer is the sole refs-related mutable field" invariant. GC trims the chain tail beyond the retention bound (§5.12). Recovery walks the back-chain to the first verifying directory, which the durability ordering invariant guarantees is durable.
**This was the one latent data-corruption bug in v0.10. It is now closed.**

### 1.3 "Zero-cost reload" needs its fault-cost asterisk
**Resolution (applied §8).** Start/reload restated as **three regimes**: arena creation (cold-populate, one-time, most expensive); reload with warm page cache (~30 ms target, O(1) CPU, spine validation only); reload with cold page cache (spine faults only, cheap because the reload working set is ~1 MB). A corollary states reload is O(1) in CPU and bounded by spine faults, while first-touch of any generic during work is a demand fault whose cost is the work's, not reload's — and is unavoidable for any architecture.
**Rationale.** Backed by measured libfyaml figures (hot load of a 427 MB structure: 35.9 ms, +1.0 MB RSS — and that figure *includes* a BLAKE3 over the input that the arena hot path does not pay, so <30 ms is a conservative ceiling). The +1 MB RSS proves reload maps rather than materializes, which retires the "cold cache = synchronous full read" worry: reload touches the reference spine, not the body. The cold-populate regime is called out explicitly because `cache=cold` measured *slower* than `cache=off` (populate cost), and CI from clean checkout lives there unless the arena is cached between runs — a reputational trap if unstated.
**Status:** resolved; downgraded from "needs hardening" to "phrase the claim to match the mechanism," which v0.11 does, with measurements.

---

## Tier 2 — internal tensions and security

### 2.1 Safe-yolo security framing
**Resolution (applied §4.5, §10, §C.6, §C.7).** `--sandbox` repositioned from "made redundant by phase 2" to "severely reduced in scope, retained as orthogonal defense-in-depth." Phase 2 dissolves the destructive-write tradeoff (rollback) **and** starves recognized-secret exfiltration at the source via read-side interposition (redact known secrets before they enter agent-visible state), but explicitly does **not** address process/network behavior. Read-side redaction documented as a *bounded* mechanism (covers recognized secrets; completeness limited by classifier coverage; un-redaction for legitimate credential-adjacent tasks reintroduces surface; each redaction is a recorded review event). Review-fatigue acknowledged as the trade against approval-fatigue (large net diff invites rubber-stamping), with the review interface's grouping/attribution/scope-classification as the mitigation.
**Rationale.** A filesystem transaction bounds filesystem effect, not confidentiality or egress; "no blast radius" was false for an agent with read access to secrets and a network path. The author's read-side interposition insight materially strengthened the confidentiality story (content-defined beats destination-defined) and is now credited — but it relocates the hard part to byte-classification, which has no clean guarantee, so the claim is stated as bounded. Decompress-redact-recompress scoped to simple formats (general container interposition is parser-attack-surface and breaks content-addressed hashes via non-byte-identical recompression).
**Status:** confidentiality prong substantially resolved for recognized secrets; `--sandbox`/egress retained as distinct reduced-scope layer; review-fatigue noted as a real trade, not a strict win.

### 2.2 Branch-point records: canonical vs metadata
**Resolution (applied §6.1, §D.1, §D.9, §D.11, phase-3 intro).** Branch-point records moved from canonical content to the **metadata-events** layer. Two turns with identical content are canonical-equal regardless of branch-point records. Phase 3 no longer adds a canonical-content reference type.
**Rationale.** Token-selection distributions depend on engine/hardware/FP-reduction-order and are not bitwise-reproducible (notably batched GPU inference across batch sizes/kernels). Canonicalizing them would make identity hinge on non-reproducible low-order bits and break cross-context dedup of identical turns near a detection threshold. Metadata carries no reproducibility/dedup-stability requirement — the correct home, consistent with where per-segment model identity already lives (§D.5.4). Conceded by the author.
**Status:** resolved. This was a v1 *schema* decision (the §6 boundary the whole document is designed to fix before v1), so it mattered to settle now.

### 2.3 KV deltas as arena generics
**Resolution (applied §5.1, §5.7, §5.13, Appendix E, §F.2/§F.6).** KV cache state reclassified as **reconstructible cache**, stored as **content-addressed blobs** (BLAKE3-named) referenced by **canonical handle generics**. Handles obey all §5.13 invariants; blobs obey only integrity-by-hash. Blobs live on the cache side (`$XDG_CACHE_HOME`), evicted by cache policy **decoupled from `fyai gc`** and its quiescence requirement; scrubbed from bundles by default. Phase-4 savings stated as **backend-dependent** (large for CPU/unified-memory; marginal for discrete GPU, which pays an H2D transfer ≈ recompute), with reprefill as the always-available floor. A reproducibility class (exact vs approximate KV) is recorded on the handle so lossy KV is honestly marked as weaker replay.
**Rationale.** KV fails all three properties justifying §5.1's "one mechanism" (no canonical cross-context identity, no cross-context dedup, no deterministic YAML emission), so §5.1 and §5.13 contradicted each other in v0.10. By §5.7's own taxonomy KV is cache (reconstructible by reprefill, disposable), not data — so it must not sit in the durable arena, must not be copied by GC survivor-copy, and must not gate eviction on GC quiescence. BLAKE3 earns identity/integrity but mostly *misses* on dedup for KV (bytes differ by engine/hardware) — expected, not a regression; it *will* dedup well for other future blob populations (binary tool results). Lossy KV breaks §E.4's round-trip "equivalent," so it forecloses byte-for-byte replay and must be flagged in the determinism class — a one-line schema-adjacent commitment kept now to avoid painful retrofit.
**Status:** resolved. Author's BLAKE3-CAS proposal adopted for the framing; the cache-vs-data durability classification (the core of the item) held and applied. "Smart" KV delta/reconstruction/compression layers punted as cache-internal optimizations behind the handle — safe because they don't touch the schema; only the exact-vs-approximate reproducibility bit is committed now.

---

## Tier 3 — precision and gaps

### 3.1 Generation-id / VMA-slot exhaustion under contention (§5.5)
**Resolution (applied §5.5).** Coupled slot/id design **retained**; no decouple. Added a note that loser-burn is a fraction of a slot per day of continuous max-contention operation (collision window in microseconds vs. inter-grow interval in tens of seconds), against which capacity-driven `fyai gc` intervenes far sooner. The one correlated case — synchronized cold start (N agents racing the first grow) — bounded to ≤ N slots once at startup and eliminated by creating the first post-bootstrap content chunk single-writer, mirroring chunk 0.
**Rationale.** Probability modeling (confirmed by calculation) showed the degenerate exhaustion is reachable but vanishingly unlikely under any human-run workload: amortized burn ~0.3 slots/day at unrealistic 64-agent max load, ~0.7 years to exhaust the repo region via losers alone — long after legitimate `gc`. The only independence-breaking case is the synchronized first grow, which is a bounded one-shot, not a sustained leak, and the single-writer-first-chunk mitigation removes even that. Decouple would reintroduce complexity for a non-problem.
**Status:** resolved; downgraded from "fix the architecture" to a one-paragraph note plus a trivial first-chunk mitigation.

### 3.2 `--dry-run` over the tool-use loop (§4.5)
**Resolution (applied §4.5).** v1 `--dry-run` defined for single-shot generation only (surface first proposed action, don't execute, stop). Faithful full-loop dry run documented as a phase-2 capability (run for real inside an uncommitted transaction).
**Rationale.** A dry run that stubs execution diverges from reality at the first command whose output the model reasons about, making the "what would have happened" report fiction past step one. The honest v1 scope is single-shot; the full-loop version is exactly what the phase-2 transaction provides.
**Status:** resolved.

### 3.3 `branch_id` in tool-call ID hash (§6.5)
**Resolution (applied §6.5).** `branch_id` dropped; deterministic ID = `toolu_{hash(turn_index, call_index)}`.
**Rationale.** Pairing only needs intra-message-list uniqueness, which `(turn_index, call_index)` already provides; concurrently-emitted branches go to separate wire contexts and never share a message list. Including `branch_id` defeated cross-branch provider-stream dedup for every tool-bearing turn (undercutting §6.1) for no correctness benefit.
**Status:** resolved (author confirmed no replay-disambiguation reason to keep it).

### 3.4 Dynamic content as leading user turns (§6.6)
**Resolution (applied §6.6).** Split clarified along *stability*, not role: stable behavioral instructions (tool contracts, safety, persona) stay in the canonical system prompt; only volatile context (date, cwd, repo name) moves to a leading user turn. Flagged as a deliberate tradeoff (models weight system vs. user role differently), not a free win.
**Rationale.** Moving behavioral instructions into user turns to chase dedup changes model behavior; only purely contextual volatile data should move.
**Status:** resolved (one-sentence tightening).

### 3.5 `fyai edit` "identity-preserving by construction" (§7.2)
**Resolution (applied §7.2).** Restated to the accurate narrower claim: unchanged subtrees re-canonicalize pointer-equal; changed content produces new canonical generics; no semantic drift in either direction.
**Rationale.** The original wording read as if editing couldn't change anything, contradicting the feature's purpose. Mechanism is sound; only the claim was overstated.
**Status:** resolved (wording).

### 3.6 Zero-idle vs O(1)-shared memory (§5.9, §8)
**Resolution (applied §8).** One sentence: shared pages are resident-once when warm and evict under pressure; re-fault cost is bounded by touched working set (per the cold-page-cache figure), not arena size — so both properties coexist.
**Rationale.** Defused by the ~1 MB reload working-set measurement; re-faulting the spine after eviction is cheap.
**Status:** resolved (clarification, no design change).

### 3.7 `fallocate` grow latency (§5.5, §8)
**Resolution (applied §8).** Documented that `chunk_size` trades grow-latency against grow-frequency and that slow/networked storage may want smaller chunks; every race loser pays the `fallocate` before discarding.
**Rationale.** "Tens of ms" is optimistic on slow/networked block devices; config note rather than a fix.
**Status:** resolved (config note).

---

## Items left explicitly open (narrowed, in §11)

- **Repo base collision default:** derivation/authority decided (init-once, recorded, quiesced relocation); only the v1 *default* on detected collision remains a choice — auto-relocate (recommended) vs. refuse-and-prompt.
- **Binary tool results:** likely home is the new blob store on its non-reconstructible (data) side, behind a canonical handle; these dedup well (unlike KV). Content-element rules still to specify.
- Untouched by this review: cancellation semantics, arena format schema-evolution/migration, KV/llama.cpp slot-system integration, bundle cross-version format, `cache_control` placement, subagent result merging, concurrent GC (v2).

---

## What did not change, and why

The thesis (statelessness via zero-cost reload via address-stable content-addressed arena) was never in question. The three-layer canonical/provider-stream/metadata schema split, subagent forking via shared sequence heads, xmit incremental append, and the persistent-data-structure framing are endorsed as the strong core. Almost every item above reduced to one of two shapes: **tighten the claim to what the mechanism delivers** (1.3, 2.1, 3.5, 3.6) or **this is cache, not data — classify it accordingly** (2.3). The only latent correctness bug (1.2 durability ordering) and the only headline-workload scaling trap (3.1 slot exhaustion) were both concrete and bounded, and are resolved.
