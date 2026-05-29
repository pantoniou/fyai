# fyai SRD — Phase 2: Filesystem-State Causal Log

**Version:** 0.11 draft
**Status:** Pre-implementation (future phase — not in v1/v2/v3 scope)
**Author:** Pantelis Antoniou
**Extends:** fyai-srd-core v0.11

*Scope: Phase 2 makes agent filesystem effects transactional via a causal log in the syscall path, enabling commit/rollback, safe-yolo operation, and end-of-session review. It adds the fourth canonical turn reference (filesystem-state) to the core schema. Section references of the form "§N" or "core §N" point into fyai-srd-core; references to other phases name the phase explicitly.*

-----


This appendix documents the anticipated phase 2 extension to fyai: bringing filesystem effects of tool execution under the same content-addressed immutable-log discipline as conversation content. Phase 2 requires kernel support and is not part of v1 scope, but informs v1 schema decisions.

## C.1 Problem

In v1, the conversation log captures tool calls and their stdout/stderr/exit code, but does not capture the filesystem effects of those calls. A model invocation of `cc -o foo foo.c` produces a canonical `tool_result` turn recording stdout/stderr/exit, but the actual changed bytes — the new `foo` binary, the modified `foo.o`, the updated directory mtime — live in the live filesystem, outside the arena.

This creates an asymmetry. The conversation is content-addressed, immutable, branchable, and rollback-able at the arena level. The filesystem is mutable, non-versioned, and diverges from the conversation log the moment anything else touches it. Rolling back a branch rewinds the conversation but not the filesystem; forking a subagent forks the conversation context but shares the same mutable filesystem with the parent and with other subagents; replaying a conversation log produces unpredictable results because the starting filesystem state is not part of the log.

## C.2 Approach

Phase 2 closes this gap by making filesystem state a first-class reference type in the turn schema. A turn gains a fourth reference (alongside content, provider streams, metadata):

- **Filesystem-state generic**: a content-addressed delta describing the filesystem changes effected by this turn's tool execution(s).

The delta is structured (per-file content hashes, mode/owner changes, deletions, renames), content-addressed, and shares storage with identical deltas from other turns or branches. Forking a branch forks the filesystem-state chain; rolling back applies the inverse delta sequence; replay reconstructs filesystem state deterministically.

## C.3 Required Kernel Support

Userspace cannot implement this honestly. Approximations using filesystem snapshots (btrfs/ZFS/APFS), `fanotify`, FUSE overlays, `LD_PRELOAD`, or `OverlayFS` each fail one or more required properties: composability with conversation branches, content-addressing at the byte level, structural sharing across branches, deterministic replay, transparent operation regardless of how tools access the filesystem.

Phase 2 anticipates a Linux kernel subsystem providing:

- A new namespace or mount-namespace extension implementing a transactional copy-on-write layer.
- Within the namespace, all writes are captured as a content-addressed delta in a delta store.
- Reads see base + delta transparently.
- Transactions can be committed (delta becomes part of parent view), discarded (delta dropped), or persisted as named snapshots.
- Snapshots are first-class identifiers usable as the base for new transactions or namespaces.
- Operations: `fs_tx_begin`, `fs_tx_commit`, `fs_tx_abort`, `fs_tx_snapshot`, `fs_branch_from_snapshot`.

This is a substantial kernel feature, multi-year in scope, requiring upstream collaboration. It is not within v1's delivery.

## C.4 Consequences for v1 Schema

The v1 schema (§6) is designed to accommodate this extension without modification. Specifically:

- The turn generic has three references in v1, with the structure designed to extend to a fourth without breaking canonical identity of existing turns.
- The canonical content boundaries (§6.2) deliberately exclude transient identifiers, ensuring that the same canonical content corresponds to the same semantic action regardless of when or where executed — a precondition for filesystem-state attribution.
- The branch references directory generic (§5.10) and branch head sequence pattern (§6.9) generalize naturally: a phase 2 per-branch metadata entry includes both a content head and a filesystem snapshot id; structural sharing applies to both.
- Bundle export (§7.3) generalizes to include filesystem snapshot deltas as additional reachable generics.

This forward-looking design discipline is part of why §6 deserves careful specification before v1 implementation. Getting canonical content boundaries wrong in v1 means re-canonicalizing the arena in v2 to add filesystem-state references, which is the migration path the design exists to avoid.

## C.5 Capabilities Enabled

With phase 2 in place, the architectural symmetry becomes total:

- **True subagent forking**: parent and child fork both conversation context and filesystem view; subagent filesystem changes do not affect parent until/unless merged.
- **Real rollback**: undoing N turns rolls the filesystem back to that point. Experimentation becomes cheap.
- **Deterministic replay**: a conversation bundle plus a starting filesystem snapshot reproduces the agent's behavior byte-for-byte. This is reproducibility at a level no current tool offers.
- **Proper isolation of parallel agents**: each agent has its own filesystem view branched from a common ancestor; changes don't collide; merge is explicit.
- **OS-level audit**: every byte changed by every tool execution is recorded, attributable, and reversible.

Phase 2 is the long-term arc the v1 architecture is designed to support. v1 stands on its own as the best stateless AI coding tool available; phase 2 is what the architecture allows once the kernel substrate exists.

## C.6 The Safe-Yolo Model

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

## C.7 Workflow Consequences and the Review Interface

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

