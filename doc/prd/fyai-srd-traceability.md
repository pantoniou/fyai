# fyai PRD to SRD Traceability

**PRD:** `doc/prd/fyai-prd.md`
**SRD baseline:** `doc/srd/fyai-srd.md` 0.11 draft
**Status:** Initial traceability pass

This document maps product requirements to the existing SRD and focused design material. It is intentionally diagnostic: a mapping does not prove implementation conformance, and a missing or contradictory mapping is evidence that either the SRD or PRD needs revision.

## 1. Traceability rules

- A PRD requirement defines an externally meaningful product behavior or invariant.
- The SRD should define the software behavior required to realize that product requirement.
- Focused design documents may define mechanisms, constraints, migration plans, and implementation choices.
- Tests verify the SRD behavior; they should not be the only place where a product guarantee is defined.
- If a focused design document describes current behavior that contradicts the SRD, treat that as a documentation/conformance gap rather than choosing whichever document is convenient.

## 2. Requirement map

| PRD requirement | Primary SRD coverage | Focused design / evidence | Traceability note |
| --- | --- | --- | --- |
| `PRD-LIFE-001` | §1 Purpose; §3 Invocation Model | `CLAUDE.md` architecture rules | Strong. No daemon/resident state is repeated as an invariant. |
| `PRD-LIFE-002` | §1; §4 Durable Storage | `doc/branching.md` | Strong. Arena publication and later continuation are core SRD behavior. |
| `PRD-LIFE-003` | §3 Invocation Model | README quick-start behavior | Strong. Prompt invocation may perform multiple model/tool calls to a terminal response. |
| `PRD-LIFE-004` | §1; §3; §8 | `CLAUDE.md` | Strong. Interactive mode is process-local UI over the same durable model. |
| `PRD-LIFE-005` | §4.3 Transient mode | `CLAUDE.md` configuration rules | Strong. Transient builders skip publication. |
| `PRD-CLI-001` | §2 Unix-shaped operation; §3 | README | Strong. Standard streams and arguments are explicit. |
| `PRD-CLI-002` | §2 Unix-shaped operation | command runner/tests | Strong. Conventional exit status is explicitly required. |
| `PRD-CLI-003` | §3 Invocation Model | management verb behavior | Strong. Inspection/config/storage verbs require no provider key. |
| `PRD-STATE-001` | §1; §4 | `CLAUDE.md` durable state rules | Strong. Arena is the local source of truth. |
| `PRD-STATE-002` | §2; §6 Provider Requests and Canonical Turns | provider translation code/design | Strong. Canonical content is independent of provider grammar. |
| `PRD-STATE-003` | §2; §4.2 | `CLAUDE.md` architecture rules | Strong. Published canonical state is immutable. |
| `PRD-STATE-004` | §8 Human-Facing Views | `doc/display-output-semantics.md` | Strong intent; see display contradiction below. |
| `PRD-STATE-005` | §4.1-4.2 | `doc/branching.md` reflogs | Strong. Reset and prior tips remain recoverable within retention. |
| `PRD-STATE-006` | §4.2 | `doc/branching.md` root handles | Strong. Pinned roots provide exact read-only retained state. |
| `PRD-BRANCH-001` | §4.1-4.2 | `doc/branching.md` | Strong. Named branches are first-class arena state. |
| `PRD-BRANCH-002` | §4.1 | `doc/branching.md` | Strong. Conversation head belongs to the branch. |
| `PRD-BRANCH-003` | §4.1; §5 | `doc/branching.md` | Strong. Branch configuration ownership is explicit. |
| `PRD-BRANCH-004` | §4.1 | `doc/branching.md` | Strong. Branches share content-addressed history rather than duplicating project state. |
| `PRD-BRANCH-005` | §4.2 | `doc/branching.md` references | Strong. Historical symbolic references may be branch start points. |
| `PRD-BRANCH-006` | §4.2 | `doc/branching.md` merge/rebase | Strong. Join operations append ordered exchanges rather than editing history. |
| `PRD-PROVIDER-001` | §2; §6 | provider request implementations | Strong. Provider wire identity is observational, not canonical identity. |
| `PRD-PROVIDER-002` | §5-6 | provider grammar support | Moderate/strong. SRD supports canonical replay across grammars, while some reasoning translation remains deferred. |
| `PRD-PROVIDER-003` | §2; §6 | `dump providers` | Strong. Provider stream data is retained separately. |
| `PRD-PROVIDER-004` | §5-6; §10 Deferred Work | catalogue/provider capability handling | Needs tightening. SRD describes supported grammars and deferred translation, but does not consistently state the user-facing behavior for unsupported capabilities. |
| `PRD-AGENT-001` | §7.1 Sub-agents | `doc/agent-fork-model.md` | Strong. Persistent delegated agents reserve and publish agent branches. |
| `PRD-AGENT-002` | §7.1 | branch inspection/transcript | Strong. Complete child conversation is published independently. |
| `PRD-AGENT-003` | §7.1 | `doc/agent-fork-model.md` | Strong. `context: fork` and `context: fresh` are explicit. |
| `PRD-AGENT-004` | §7.1 | agent protocol/work-band design | Strong. Parent receives the final report as the bounded tool result. |
| `PRD-AGENT-005` | §7.1 | `doc/agent-fork-model.md` disown boundary | Product intent is clear but the SRD should state process-state isolation more directly instead of leaving it to fork implementation notes. |
| `PRD-TOOL-001` | §2; §7 | tool schemas | Strong. Tool surface is explicit and finite. |
| `PRD-TOOL-002` | §2; §7 | config schema/sandbox policy | Strong. Policy and sandboxing bound tool execution. |
| `PRD-TOOL-003` | §7 | sandbox reporting | Moderate. SRD says Landlock is best-effort on unsupported platforms; user-visible truthfulness should be made explicit. |
| `PRD-TOOL-004` | §7 | `ask_user` behavior | Strong for parent agent. Note delegated-agent question relay behavior should remain consistent with the active agent model. |
| `PRD-TOOL-005` | §7; context behavior | `doc/context-compaction.md` | Strong. File/shell outputs are bounded to protect context. |
| `PRD-SECRET-001` | §5; §7 | auth/secret docs | Strong. Raw secrets do not enter ordinary arena config. |
| `PRD-SECRET-002` | §5 | config schema | Strong. YAML stores credential indirection. |
| `PRD-SECRET-003` | §5 | OAuth/secret backend docs | Strong. Persistent authentication is explicitly machine-local. |
| `PRD-SECRET-004` | §7 | `CLAUDE.md`; `doc/pty-terminal-plan.md` | Strong. Child execution sanitizes provider credentials. |
| `PRD-CONTEXT-001` | §8 | `doc/context-compaction.md` | Strong. `context` and interactive reporting expose fill/pressure. |
| `PRD-CONTEXT-002` | context behavior implied by §8 | `doc/context-compaction.md` preflight | Needs explicit SRD requirement. Focused design clearly requires refusal/recovery before an oversized normal request. |
| `PRD-CONTEXT-003` | §4.2 compact | `doc/context-compaction.md` | Strong. `compacted_from` preserves provenance. |
| `PRD-CONTEXT-004` | §4.2 compact; §10 | `doc/context-compaction.md`; `doc/openai-compaction.md` | Strong at product level. Grammar-specific mechanisms differ by design. |
| `PRD-TERM-001` | §1; §8-9 | terminal/UI docs | Moderate/strong. SRD establishes interactive terminal ownership but product quality belongs partly in focused terminal docs. |
| `PRD-TERM-002` | §6-8 | UI work-band/tool streaming behavior | Strong. Streaming/progress is observable during invocation. |
| `PRD-TERM-003` | §7; §9 | `doc/pty-terminal-plan.md` | Needs stronger SRD wording. Focused design contains the actual terminal correctness contract: scrollback, output bounds, resize, session semantics, interpreted output. |
| `PRD-TERM-004` | §6; §8 | `doc/display-output-semantics.md` | **Contradiction.** SRD says tagged Markdown `display_outputs` are recorded and replayed exactly; the focused design document says the current persisted model has no display document and history reconstructs output. Resolve before treating this requirement as Current. |
| `PRD-PLATFORM-001` | §9 Platform and Performance | sandbox/platform docs | Strong. Linux-first is explicit. |
| `PRD-PLATFORM-002` | §9 | portable event interface | Strong for core semantics; confinement strength may differ. |
| `PRD-PLATFORM-003` | §9 and project/build docs | README/build system | Strong. Native CLI implementation/distribution is central. |

## 3. Documentation contradictions and missing SRD requirements

### 3.1 Display-output state is inconsistent

The SRD §6 currently states that transcript rendering is recorded as tagged Markdown `display_outputs` documents and replayed exactly, with reconstruction only as a legacy-arena fallback.

`doc/display-output-semantics.md`, however, describes the current persisted model as containing canonical messages, provider stream, metadata, and predecessor links but **no persisted display document or explicit display-output tag**. It says `fyai history` reconstructs Markdown from canonical/provider data and identifies the target state as introducing one progressive tagged output document.

These cannot both describe current `devel` behavior.

Before the 0.12 baseline is accepted, one of the following must happen:

1. implementation has already moved to persisted `display_outputs`, in which case `doc/display-output-semantics.md` needs to be updated to distinguish historical analysis from completed implementation; or
2. implementation still reconstructs history, in which case the SRD overstates the implemented Phase 1 contract and must be corrected until the display-output work lands.

This is the highest-value documentation/conformance check exposed by the PRD pass.

### 3.2 Context preflight belongs in the SRD

`doc/context-compaction.md` defines strong behavior before a normal model request: known context windows are enforced, oversized prompts are refused with named recovery paths, output allowance is reduced to available room, and file/shell tool results are bounded so one result cannot consume the window.

The SRD mentions `context` reporting and compaction, but the preflight behavior should be explicit because it directly satisfies `PRD-CONTEXT-002` and `PRD-TOOL-005`.

### 3.3 Delegated-agent isolation belongs above the fork mechanism

The SRD describes delegated children as independent writers that abandon the inherited event loop and reopen the shared arena. The focused fork-model document contains the stronger correctness story: copied parent shell sessions, jobs, waits, UI state, MCP connections, and accounting must be disowned because they belong to another process.

The software requirement should say that a delegated agent must not address or observe live parent/sibling execution state. `fyai_ctx_fork_disown()` is one implementation of that requirement, not the requirement itself.

### 3.4 Terminal correctness should be promoted from plan to contract

The PTY design defines several behaviors that users can observe directly: bounded scrollback, output-limit parity with pipe execution, interpreted live PTY output, resize propagation, named session addressability, terminal input/output tools, and deterministic session shutdown.

Where those behaviors are implemented and supported, the SRD should describe them as software requirements instead of leaving the product contract dependent on a document titled `Plan`.

### 3.5 Capability truthfulness needs a common rule

The SRD individually describes provider restrictions, platform sandbox differences, ChatGPT authentication restrictions, and deferred provider translations. It would benefit from one general software requirement: when the active provider/model/platform cannot realize a requested capability, fyai reports the limitation and does not silently claim equivalent behavior.

That gives `PRD-PROVIDER-004` and `PRD-TOOL-003` one consistent software-level expression.

## 4. Recommended SRD 0.12 edit order

1. Resolve the `display_outputs` contradiction and make §6 match actual `devel` behavior.
2. Add explicit context preflight and bounded-output requirements from `doc/context-compaction.md`.
3. Promote delegated-agent live-state isolation into §7.1 without mandating fork or exec.
4. Promote supported PTY/session correctness behavior into §7 or a dedicated terminal subsection, leaving implementation mechanics in `doc/pty-terminal-plan.md`.
5. Add the general capability/confinement truthfulness rule.
6. Add `PRD-*` references to SRD sections after the wording is accurate, rather than attaching traceability IDs to statements known to be stale.

## 5. Traceability policy for the revision bump

The 0.12 documentation set should use the following direction of authority:

`PRD product contract -> SRD software contract -> focused design -> implementation -> verification`

Traceability is not intended to duplicate prose. A concise SRD requirement may cite one or more PRD IDs, and a focused design document may cite the SRD behavior it implements. The value is that a future implementation change can be classified correctly:

- if it changes only a mechanism while preserving the SRD behavior, it is an implementation/design change;
- if it changes software-visible behavior while preserving the PRD product promise, it is an SRD change;
- if it changes the user-visible product promise or invariant, it is a PRD change and should be reviewed as such.