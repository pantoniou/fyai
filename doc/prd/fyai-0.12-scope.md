# fyai 0.12 Product Requirement Scope

**Status:** Working scope ledger
**PRD:** `doc/prd/fyai-prd.md`
**Target:** 0.12 product baseline

This document classifies the requirements in the fyai PRD by product maturity. It is deliberately separate from implementation status in the SRD: a product requirement can be part of the 0.12 contract even when one implementation detail is still being completed, and an implemented mechanism does not automatically become a product promise.

## 1. Status vocabulary

- **Current** - observable product behavior already represented by the current `devel` contract and documentation. The 0.12 release should preserve it unless the PRD is deliberately changed.
- **0.12 required** - part of the intended 0.12 product contract, but current behavior or documentation still has a known gap that must be closed or explicitly descoped before release.
- **Planned** - desirable continuation of the product model, but not required to call 0.12 complete.
- **Future** - intentionally outside the 0.12 product contract.
- **Decision required** - the product behavior is not yet sufficiently defined to make an implementation choice safely.

The PRD remains the normative statement of *what* fyai promises. This ledger records *when* each promise is intended to become release-binding.

## 2. 0.12 baseline

The 0.12 baseline is not a feature-reset. It establishes the product contract around behavior that fyai already treats as fundamental:

- one invocation owns one process lifetime; no daemon or hidden resident state;
- durable local agent state survives process exit;
- canonical conversation history is fyai-owned rather than provider-owned;
- branches are first-class lines of agent work and own their configuration;
- provider/model selection is replaceable around a canonical conversation model;
- tool execution and credential handling have explicit trust boundaries;
- persistent delegated agents leave separately inspectable work;
- context limits are visible, checked, and recoverable;
- interactive terminal use is a first-class surface of the same product used non-interactively.

The main 0.12 convergence work is not inventing a new architecture. It is turning those existing architectural invariants into a stable product contract and closing user-visible semantic gaps where multiple internal paths currently disagree.

## 3. Requirement classification

| Requirement | Status | 0.12 interpretation |
| --- | --- | --- |
| `PRD-LIFE-001` | Current | No daemon or resident fyai process is a product invariant. |
| `PRD-LIFE-002` | Current | Published state is durable across invocations. |
| `PRD-LIFE-003` | Current | One-shot complete model/tool loops are core behavior. |
| `PRD-LIFE-004` | Current | Interactive and non-interactive modes share the same durable model. |
| `PRD-LIFE-005` | Current | Explicit transient operation exists and must remain non-publishing. |
| `PRD-CLI-001` | Current | Arguments and standard streams remain first-class interfaces. |
| `PRD-CLI-002` | Current | Conventional exit status remains part of non-interactive operation. |
| `PRD-CLI-003` | Current | Local inspection/management must not require model inference. |
| `PRD-STATE-001` | Current | The local arena is the source of truth for durable project agent state. |
| `PRD-STATE-002` | Current | Canonical history is provider-independent. |
| `PRD-STATE-003` | Current | Published canonical state is immutable. |
| `PRD-STATE-004` | Current | Durable history is independently inspectable. |
| `PRD-STATE-005` | Current | Reflogs provide a retained recovery path. |
| `PRD-STATE-006` | Current | Pinned roots provide exact retained-state reads for automation. |
| `PRD-BRANCH-001` | Current | Multiple named lines of work are fundamental product state. |
| `PRD-BRANCH-002` | Current | A branch owns its conversation. |
| `PRD-BRANCH-003` | Current | A branch owns the configuration needed to continue it. |
| `PRD-BRANCH-004` | Current | Branching does not duplicate the working directory or full arena history. |
| `PRD-BRANCH-005` | Current | Historical symbolic start points are supported. |
| `PRD-BRANCH-006` | Current | Merge/rebase join conversation work explicitly rather than editing history in place. |
| `PRD-PROVIDER-001` | Current | Provider-side session identity does not define a branch. |
| `PRD-PROVIDER-002` | Current | Supported model/provider changes operate over canonical history, subject to representable semantics. |
| `PRD-PROVIDER-003` | Current | Provider observations are retained separately from canonical content. |
| `PRD-PROVIDER-004` | 0.12 required | Capability differences must be surfaced consistently rather than failing as provider-specific surprises. |
| `PRD-AGENT-001` | Current | Persistent delegated work uses a separately addressable agent branch. |
| `PRD-AGENT-002` | Current | Persistent delegated conversation remains inspectable after the parent receives its report. |
| `PRD-AGENT-003` | Current | Forked and fresh delegation are distinct user-visible context choices. |
| `PRD-AGENT-004` | Current | The parent receives a bounded delegated result rather than a flattened full transcript. |
| `PRD-AGENT-005` | 0.12 required | Parent/sibling live-state isolation is a product requirement; current fork inheritance makes this an active correctness boundary. |
| `PRD-TOOL-001` | Current | Local effects occur through an explicit tool surface. |
| `PRD-TOOL-002` | Current | Tool execution is bounded by configurable policy. |
| `PRD-TOOL-003` | 0.12 required | UI/diagnostics must accurately describe effective confinement on the running platform. |
| `PRD-TOOL-004` | Current | Human questions are a defined tool interaction. |
| `PRD-TOOL-005` | Current | File and shell results are bounded before admission to model context. |
| `PRD-SECRET-001` | Current | Raw credentials are excluded from ordinary durable project state. |
| `PRD-SECRET-002` | Current | Durable configuration stores credential resolution intent, not secret values. |
| `PRD-SECRET-003` | Current | Persistent auth material is machine-local. |
| `PRD-SECRET-004` | Current | Child environments are sanitized before arbitrary program execution. |
| `PRD-CONTEXT-001` | Current | Context pressure is visible through context reporting and the interactive surface. |
| `PRD-CONTEXT-002` | Current | Known context windows are checked before normal requests. |
| `PRD-CONTEXT-003` | Current | Compaction retains provenance to the complete prior conversation. |
| `PRD-CONTEXT-004` | Current | Native compaction and portable fallback may coexist; native Anthropic support is not itself a 0.12 product requirement. |
| `PRD-TERM-001` | Current | Interactive terminal use is a native product surface, not a separate hosted-chat mode. |
| `PRD-TERM-002` | Current | Streaming model/tool/agent progress is visible while work runs. |
| `PRD-TERM-003` | 0.12 required | PTY-backed programs must behave correctly enough for supported interactive use, including resize/control behavior. |
| `PRD-TERM-004` | 0.12 required | Live output and durable history need one explicit semantic boundary; current reconstruction paths can diverge. |
| `PRD-PLATFORM-001` | Current | Linux remains the primary platform. |
| `PRD-PLATFORM-002` | Current | Core conversation/branch/lifecycle semantics remain portable across supported Unix-like systems. |
| `PRD-PLATFORM-003` | Current | fyai remains native command-line software without a required language runtime or resident framework. |

## 4. 0.12 product gaps

### 4.1 Durable display semantics

`PRD-TERM-004` is the clearest product gap in the current tree.

Today, canonical messages are suitable for model replay and provider streams are suitable for protocol fidelity, but neither is the exact document shown to the user. Live assistant content, reasoning, tools, user bubbles, diagnostics, and history reconstruction travel through partially independent rendering paths. That can make the live transcript and later history differ in spacing, grouping, visibility, or presentation.

For 0.12, the product-level requirement is:

> A conversational output shown as durable history must have one durable semantic representation whose finalized content does not depend on reconstructing presentation from unrelated provider or execution data.

This does **not** require the PRD to specify a particular `display_output` structure, Markdown renderer, sink implementation, spool design, or terminal library. Those remain SRD/design concerns.

The release decision is binary: either close the semantic gap and keep `PRD-TERM-004` in the 0.12 contract, or explicitly move it to Planned. The current state should not be described as fully satisfying the requirement merely because history is reconstructable.

### 4.2 Delegated-process isolation

`PRD-AGENT-005` is also release-significant.

Persistent delegation already has the desired product shape: separate work, separate branch, inspectable history, and a bounded parent result. The current process model, however, begins from `fork()`, so live parent process state is inherited and must be explicitly disowned before the child proceeds.

The product requirement is not "use `exec()`". It is that a delegated agent must not observe or control parent/sibling live state merely because of process inheritance.

For 0.12, correctness at that boundary is required. Whether the implementation remains fork-plus-disown or moves to exec-plus-explicit-start-state is an engineering decision.

### 4.3 Capability and confinement truthfulness

`PRD-PROVIDER-004` and `PRD-TOOL-003` are both forms of the same product rule: fyai must describe the capabilities it actually has.

A provider/model combination that lacks a capability must not silently masquerade as equivalent. A platform without a confinement feature must not be presented as though the stronger boundary is active. These are primarily consistency/documentation requirements for 0.12, not invitations to emulate every missing provider or OS facility.

### 4.4 PTY correctness

The terminal-session work has moved beyond "shell can allocate a PTY" into a user-visible product surface: bounded scrollback, interpreted live output, resize propagation, named sessions, session input/output, and a terminal view for the user.

For 0.12, `PRD-TERM-003` should mean that supported terminal-backed workflows do not silently lose normal terminal semantics that fyai claims to support. It does not mean implementing every terminal emulator feature or keeping sessions alive across fyai invocations.

## 5. Product decisions still required

### 5.1 When does delegated work become independently durable?

The current persistent-agent experience and the possible future exec worker model expose a product question that should be answered before architecture makes it accidental.

A fork-context delegated agent can currently begin from parent conversation state that has not yet been published. An executed worker cannot recover that state from the arena unless the parent either publishes the delegation point first or sends the not-yet-durable conversation explicitly.

The PRD should eventually choose one of these observable contracts:

- **Durable-at-delegation:** starting persistent delegation publishes an independently addressable delegation point before the child begins.
- **Durable-on-success:** delegated work becomes independently durable only when the delegated run successfully publishes.
- **Parent-transactional:** delegation may execute against unpublished parent state, and parent publication defines the durability boundary unless/until the child publishes its own branch.

This is marked **Decision required** for the product model. 0.12 does not need to replace the current process implementation merely to answer it, but the answer should precede a fork-to-exec redesign.

### 5.2 What exactly is a durable transcript?

The desired invariant is stronger than "history can print something readable." The product needs to distinguish:

- canonical model-replay content;
- provider observations;
- durable human-facing conversational output;
- transient UI chrome and diagnostics.

The PRD should promise that durable conversational output is inspectable and stable enough to represent what the user saw, while leaving rendering mechanics to the SRD/design layer.

0.12 should resolve this through `PRD-TERM-004` rather than expanding provider-native content until it accidentally becomes a UI serialization format.

## 6. Explicitly not required for 0.12

The following may be valuable, but should not block the 0.12 product baseline unless deliberately promoted:

- remote arena synchronization;
- portable session or conversation bundles;
- automatic coupling of fyai branch selection to the current Git branch;
- persistent background shell jobs surviving a fyai invocation;
- identical sandbox strength on every supported platform;
- native provider compaction for every supported grammar when an explicit, provenance-preserving fallback exists;
- perfect translation of every provider-specific reasoning representation;
- a fork-to-exec sub-agent rewrite by itself;
- a hosted fyai account or cloud conversation authority.

## 7. Release gate

Before declaring the PRD a 0.12 product baseline:

1. Each `0.12 required` row must either be demonstrated against the current implementation, have a concrete closing change, or be explicitly moved to Planned with rationale.
2. The SRD must cite the PRD identifiers for software requirements that exist to satisfy these product promises.
3. Focused design documents must not override PRD product intent; unresolved product decisions must be promoted here or into the PRD rather than silently settled in implementation plans.
4. README product language must agree with the PRD on lifecycle, durable ownership, branching, delegation, provider independence, trust boundaries, and terminal behavior.
5. Release notes for the revision bump should describe user-visible product changes in PRD terms instead of allocator, event-loop, or provider-wire implementation vocabulary.

## 8. Immediate follow-up

The next documentation pass should annotate the SRD with traceability to the stable PRD IDs. Start with the requirements whose implementation mechanisms are already well documented:

- lifecycle and Unix invocation;
- arena publication and immutable retained roots;
- branch-local conversation/configuration;
- canonical/provider separation;
- persistent delegated-agent branches;
- bounded tool results and context preflight;
- credential indirection and child environment sanitization;
- PTY/session behavior;
- durable display-output semantics.

That pass should expose any PRD requirement that has no corresponding software requirement, and any SRD section that is actually a design mechanism rather than a requirement.