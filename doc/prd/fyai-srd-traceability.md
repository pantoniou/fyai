# fyai PRD to SRD Traceability

**PRD:** `doc/prd/fyai-prd.md`
**SRD baseline:** `doc/srd/fyai-srd.md`
**Status:** Active traceability pass

This document maps product requirements to the SRD and focused design material. A mapping does not by itself prove conformance; where necessary, current code is used to resolve whether a design document is historical, aspirational, or current.

## 1. Traceability rules

- A PRD requirement defines an externally meaningful product behavior or invariant.
- The SRD defines the software behavior required to realize that product requirement.
- Focused design documents define mechanisms, constraints, migration plans, and implementation choices.
- Tests verify SRD behavior; tests should not be the only place a product guarantee is defined.
- When documentation disagrees, inspect the current implementation before promoting either description into the product contract.

## 2. Requirement map

| PRD requirement | Primary SRD coverage | Focused design / evidence | Traceability note |
| --- | --- | --- | --- |
| `PRD-LIFE-001` | §1 Purpose; §3 Invocation Model | `CLAUDE.md` architecture rules | Strong. No daemon/resident state is a repeated invariant. |
| `PRD-LIFE-002` | §1; §4 Durable Storage | `doc/branching.md` | Strong. Arena publication and later continuation are core behavior. |
| `PRD-LIFE-003` | §3 Invocation Model | README | Strong. Prompt invocation may perform multiple model/tool calls to a terminal response. |
| `PRD-LIFE-004` | §1; §3; §8 | `CLAUDE.md` | Strong. Interactive mode is process-local UI over the same durable model. |
| `PRD-LIFE-005` | §4.3 Transient mode | `CLAUDE.md` | Strong. Transient builders skip publication. |
| `PRD-CLI-001` | §2; §3 | README | Strong. Standard streams and arguments are explicit. |
| `PRD-CLI-002` | §2 | command runner/tests | Strong. Conventional exit status is explicit. |
| `PRD-CLI-003` | §3 | management verb behavior | Strong. Inspection/config/storage verbs require no provider key. |
| `PRD-STATE-001` | §1; §4 | `CLAUDE.md` | Strong. Arena is the local source of truth. |
| `PRD-STATE-002` | §2; §6 | provider implementations | Strong. Canonical content is independent of provider grammar. |
| `PRD-STATE-003` | §2; §4.2 | `CLAUDE.md` | Strong. Published canonical state is immutable. |
| `PRD-STATE-004` | §6; §8 | `src/fyai_output.c`; `src/fyai_turn.c` | Strong. Durable display output is stored independently of continuation. |
| `PRD-STATE-005` | §4.1-4.2 | `doc/branching.md` | Strong. Reflogs retain recovery paths. |
| `PRD-STATE-006` | §4.2 | `doc/branching.md` | Strong. Pinned roots provide exact read-only retained state. |
| `PRD-BRANCH-001` | §4.1-4.2 | `doc/branching.md` | Strong. Named branches are first-class arena state. |
| `PRD-BRANCH-002` | §4.1 | `doc/branching.md` | Strong. Conversation head belongs to the branch. |
| `PRD-BRANCH-003` | §4.1; §5 | `doc/branching.md` | Strong. Branch configuration ownership is explicit. |
| `PRD-BRANCH-004` | §4.1 | `doc/branching.md` | Strong. Branches share content-addressed history. |
| `PRD-BRANCH-005` | §4.2 | `doc/branching.md` | Strong. Historical symbolic references may be start points. |
| `PRD-BRANCH-006` | §4.2 | `doc/branching.md` | Strong. Join operations append ordered exchanges. |
| `PRD-PROVIDER-001` | §2; §6 | provider request implementations | Strong. Provider-side identity is observational, not canonical identity. |
| `PRD-PROVIDER-002` | §5-6 | provider grammar support | Moderate/strong. Canonical replay is supported subject to representable semantics. |
| `PRD-PROVIDER-003` | §2; §6 | `dump providers` | Strong. Provider stream data is retained separately. |
| `PRD-PROVIDER-004` | §5-6; §10 | catalogue/provider capability handling | Needs explicit common software rule for unsupported capabilities. |
| `PRD-AGENT-001` | §7.1 | `doc/agent-fork-model.md` | Strong. Persistent delegated agents reserve and publish agent branches. |
| `PRD-AGENT-002` | §7.1 | branch inspection/transcript | Strong. Complete child conversation is independently inspectable. |
| `PRD-AGENT-003` | §7.1 | `doc/agent-fork-model.md` | Strong. Forked and fresh context are explicit. |
| `PRD-AGENT-004` | §7.1 | agent protocol/work-band behavior | Strong. Parent receives bounded final report. |
| `PRD-AGENT-005` | §7.1 | `doc/agent-fork-model.md` | Needs explicit SRD wording: child must not observe or control parent/sibling live execution state. |
| `PRD-TOOL-001` | §2; §7 | tool schemas | Strong. Tool surface is explicit and finite. |
| `PRD-TOOL-002` | §2; §7 | config/sandbox policy | Strong. Policy and sandboxing bound execution. |
| `PRD-TOOL-003` | §7 | sandbox behavior | Needs explicit user-visible truthfulness rule when confinement is unavailable or degraded. |
| `PRD-TOOL-004` | §7 | `ask_user` behavior | Strong for parent agent. |
| `PRD-TOOL-005` | §7-8 | `doc/context-compaction.md` | Strong in implementation/design; SRD should state preflight/output bounds directly. |
| `PRD-SECRET-001` | §5; §7 | auth/secret docs | Strong. Raw secrets do not enter ordinary arena config. |
| `PRD-SECRET-002` | §5 | config schema | Strong. YAML stores credential indirection. |
| `PRD-SECRET-003` | §5 | OAuth/secret docs | Strong. Persistent authentication is machine-local. |
| `PRD-SECRET-004` | §7 | `CLAUDE.md`; PTY design | Strong. Child execution sanitizes provider credentials. |
| `PRD-CONTEXT-001` | §8 | `doc/context-compaction.md` | Strong. Context fill/pressure is visible. |
| `PRD-CONTEXT-002` | §8 | `doc/context-compaction.md` | Needs explicit SRD requirement for preflight refusal and recovery. |
| `PRD-CONTEXT-003` | §4.2 | `doc/context-compaction.md` | Strong. `compacted_from` preserves provenance. |
| `PRD-CONTEXT-004` | §4.2; §10 | compaction docs | Strong at product level. Grammar-specific mechanisms may differ. |
| `PRD-TERM-001` | §1; §8-9 | terminal/UI docs | Strong. Interactive terminal is a native invocation surface. |
| `PRD-TERM-002` | §6-8 | UI work bands/tool streaming | Strong. Progress is observable during invocation. |
| `PRD-TERM-003` | §7; §9 | `doc/pty-terminal-plan.md` | Needs stronger SRD wording around supported PTY/session correctness. |
| `PRD-TERM-004` | §6; §8 | `src/fyai_output.c`; `src/fyai_turn.c`; `doc/display-output-semantics.md` | **Resolved as Current.** Current code persists tagged Markdown `display_outputs`; the focused design document's "no persisted display document" description is stale historical analysis. |
| `PRD-PLATFORM-001` | §9 | sandbox/platform docs | Strong. Linux-first is explicit. |
| `PRD-PLATFORM-002` | §9 | portable event interface | Strong for core semantics; confinement strength may differ. |
| `PRD-PLATFORM-003` | §9 and build docs | README/build system | Strong. Native CLI is central. |

## 3. Conformance findings and missing SRD requirements

### 3.1 Display-output contradiction is resolved

The SRD states that transcript rendering is recorded as tagged Markdown `display_outputs` documents and replayed exactly, with reconstruction only for legacy arena state.

Current implementation supports that statement:

- `struct fyai_display_output` holds a tag, Markdown source, fragment metadata, and reasoning state;
- finalization creates a durable mapping containing `tag`, `markdown`, `state`, and `fragments`;
- `fyai_turn_append_display_output()` appends that mapping to the turn's `display_outputs` sequence.

The old current-state description in `doc/display-output-semantics.md` predates this implementation and should be treated as historical analysis until refreshed. The document remains useful for the rationale behind separating canonical replay content, provider observations, durable display content, and transient UI output.

### 3.2 Context preflight belongs explicitly in the SRD

`doc/context-compaction.md` defines software-visible behavior before a normal model request: known context windows are enforced, oversized prompts are refused with recovery paths, output allowance is reduced to available room, and file/shell tool results are bounded so one result cannot consume the context window.

These behaviors directly satisfy `PRD-CONTEXT-002` and `PRD-TOOL-005` and should be explicit in the SRD.

### 3.3 Delegated-agent isolation belongs above the fork mechanism

The SRD describes delegated children as independent writers. The focused fork-model document contains the stronger correctness boundary: copied parent shell sessions, jobs, waits, UI state, MCP connections, and accounting belong to another process and must not be addressable from the delegated agent.

The SRD requirement should state the isolation property without mandating `fork()` or `exec()`.

### 3.4 Terminal correctness should be promoted from plan to contract

The PTY design documents observable behavior including bounded output, interpreted terminal state, resize propagation, named sessions, terminal input/output, and deterministic shutdown.

Where supported, these behaviors should be software requirements rather than depending on a document titled `Plan`.

### 3.5 Capability truthfulness needs one common rule

The SRD individually describes provider restrictions, platform sandbox differences, authentication restrictions, and deferred provider translations. It should also state the general rule: when the active provider/model/platform cannot realize a requested capability, fyai reports the limitation and does not silently claim equivalent behavior.

This supplies a common software expression for `PRD-PROVIDER-004` and `PRD-TOOL-003`.

## 4. Recommended SRD 0.12 edit order

1. Update SRD metadata and establish PRD traceability policy.
2. Preserve the current three-way separation of canonical messages, provider observations, and durable display outputs.
3. Add explicit context preflight and bounded-output requirements.
4. Promote delegated-agent live-state isolation into §7.1 without mandating fork or exec.
5. Promote supported PTY/session correctness behavior into the software contract.
6. Add the general capability/confinement truthfulness rule.
7. Attach `PRD-*` references to the corresponding SRD sections.

## 5. Traceability policy for the revision bump

The 0.12 documentation set uses this direction of authority:

`PRD product contract -> SRD software contract -> focused design -> implementation -> verification`

Traceability should not duplicate prose. A concise SRD requirement may cite one or more PRD IDs, and a focused design document may cite the SRD behavior it implements.

A future change can then be classified cleanly:

- mechanism changes that preserve SRD behavior are implementation/design changes;
- software-visible changes that preserve the PRD promise are SRD changes;
- changes to a user-visible product promise or invariant are PRD changes and require product review.