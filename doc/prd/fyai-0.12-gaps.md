# fyai 0.12 Gap Register

**Status:** Working release-gap register
**PRD:** `doc/prd/fyai-prd.md`
**Scope ledger:** `doc/prd/fyai-0.12-scope.md`
**SRD:** `doc/srd/fyai-srd.md`

This document records the remaining gaps between the proposed 0.12 product contract and the evidence currently available in the PRD/SRD/design set. It is intentionally narrower than the PRD and scope ledger: it lists the items that still need a decision, implementation check, test evidence, or documentation cleanup before the PRD can be treated as the 0.12 baseline.

## 1. Summary

The broad PRD/SRD structure is in place. The remaining gaps are concentrated in three release gates and one product decision:

| Gap | Type | Related requirements | Release impact |
| --- | --- | --- | --- |
| Delegated-agent live-state isolation | Engineering conformance gate | `PRD-AGENT-005` | Must be demonstrated or fixed before 0.12 baseline. |
| Provider/platform truthfulness | Engineering/documentation conformance gate | `PRD-PROVIDER-004`, `PRD-TOOL-003` | Must be made consistent before 0.12 baseline. |
| PTY/session correctness | Engineering/test conformance gate | `PRD-TERM-003` | Must be verified for supported terminal-backed workflows. |
| Persistent delegation durability point | Product decision | `PRD-AGENT-001` through `PRD-AGENT-004`, `PRD-STATE-*` | Should be decided before fork-to-exec or other delegation redesign. |

A documentation cleanup item also remains: `doc/display-output-semantics.md` has stale historical wording about the absence of persisted display documents. Current code and the updated SRD indicate that tagged `display_outputs` are now durable; the design document should be refreshed so it no longer contradicts the implemented state.

## 2. Gap: delegated-agent live-state isolation

### Product requirement

A persistent delegated agent must not accidentally observe, address, or control live process state that belongs to its parent or sibling work.

This is the product-level rule behind `PRD-AGENT-005`. It does not require a particular process model.

### Current understanding

Persistent delegation already has the desired product shape: a delegated task runs as a separate line of work, publishes an inspectable agent branch, and returns a bounded report to the parent.

The active implementation model begins from `fork()`. A forked child inherits the parent's address space, including live state that is not semantically part of the child's work. The focused fork-model document identifies copied state that must be disowned, including shell sessions, tool jobs, waits, queued events, patch views, UI/session state, MCP connections, and accounting.

### Gap

The isolation boundary needs release-level proof. The code may remain fork-plus-disown, or it may eventually move to exec-plus-explicit-start-state, but 0.12 should not rely only on developer memory to keep inherited live state out of delegated work.

### Acceptance target

Before closing this gap:

1. the SRD must state the live-state isolation requirement independently of `fork()` or `exec()`;
2. the focused design must identify the current mechanism and its maintenance rule;
3. tests or code-review evidence must cover the highest-risk inherited states, especially named shell sessions, active tool jobs, waits, parent UI/display state, MCP connections, and accounting/token state;
4. adding a new live field to `struct fyai_ctx` should have an obvious review point for whether it must be disowned or explicitly passed.

### Likely next actions

- Audit `struct fyai_ctx` live fields against `fyai_ctx_fork_disown()`.
- Add or identify tests where a sub-agent cannot read, resize, close, or otherwise interact with a parent-owned shell/session/job.
- Add a maintenance note in the relevant source or design doc tying new live state to the disown boundary.

## 3. Gap: provider/platform truthfulness

### Product requirement

fyai must report the capability boundary it is actually operating under. Unsupported provider/model behavior and unavailable platform confinement must not silently appear equivalent to supported behavior.

This covers `PRD-PROVIDER-004` and `PRD-TOOL-003`.

### Current understanding

The SRD already describes many specific boundaries: supported provider grammars, deferred reasoning translation, ChatGPT-auth restrictions, Landlock being Linux-specific/best-effort elsewhere, and platform-dependent confinement strength.

### Gap

These boundaries need one consistent user-visible behavior rule. The risk is not just missing documentation; it is an implementation path that quietly degrades behavior while the user still thinks the stronger capability is active.

Examples:

- a provider/model lacks a requested grammar, compaction mechanism, reasoning translation, or tool behavior;
- a platform cannot enforce configured Landlock-style confinement;
- a requested authentication mode is incompatible with the selected endpoint/provider;
- a provider-specific beta or catalogue capability is unavailable.

### Acceptance target

Before closing this gap:

1. unsupported provider/model capabilities must produce explicit refusal, warning, or documented fallback behavior;
2. sandbox/confinement status must be observable enough that a user can tell whether the requested boundary is enforced or degraded;
3. docs must distinguish fallback behavior from equivalent native support;
4. tests or manual verification should cover representative provider and platform capability failures.

### Likely next actions

- Inventory provider capability checks and their diagnostics.
- Inventory sandbox status reporting on Linux and non-Linux platforms.
- Add a small conformance checklist to the SRD or verification notes for capability fallback/refusal paths.

## 4. Gap: PTY/session correctness

### Product requirement

Supported terminal-backed workflows must preserve the terminal behavior fyai claims to support.

This covers `PRD-TERM-003`.

### Current understanding

The PTY/session design has moved beyond a boolean `tty` option. It now describes observable product behavior: bounded scrollback, parity with pipe output limits, interpreted live output, binary summaries, timeout shapes, alternate-screen handling, resize propagation, named sessions, session input/output tools, deterministic shutdown, and a user-facing terminal view.

### Gap

A PTY existing is not sufficient. The release gate is whether the supported terminal-backed workflows behave correctly under normal terminal expectations and whether failures are bounded and visible.

High-risk areas:

- commands that print more rows than the visible screen;
- output-budget enforcement on PTY output;
- alternate-screen programs;
- resize propagation through the parent to the PTY child;
- live output that must not expose raw escape sequences;
- named-session lifecycle, input, output, idle timeout, and close behavior;
- interrupt/shutdown semantics for process groups.

### Acceptance target

Before closing this gap:

1. PTY output should retain bounded scrollback plus the final visible screen;
2. PTY output should obey the same practical output bounds as pipe execution;
3. live PTY output should be interpreted text or display state, not raw terminal-control bytes;
4. supported resize/control interactions should work or be explicitly outside scope;
5. tests should cover scrollback, output bounds, timeout/shutdown, alternate screen, and resize where supported.

### Likely next actions

- Reconcile `doc/pty-terminal-plan.md` against current implementation and mark completed vs. remaining items.
- Confirm the functional/unit tests named in the plan exist and pass, or add them to the release checklist.
- Decide whether any terminal behaviors are explicitly deferred from 0.12.

## 5. Product decision: persistent delegation durability point

### Product question

When does persistent delegated work become independently durable?

This is not merely an implementation detail. It affects recoverability, inspection, crash behavior, and future process architecture.

### Current tension

A fork-context delegated child can begin from parent conversation state that has not necessarily been published yet. If delegation later moves to an executed worker, the worker cannot recover that unpublished parent state from the arena unless the parent publishes a delegation point first or sends explicit start state over the control protocol.

### Candidate contracts

1. **Durable at delegation.** Starting persistent delegation publishes an independently addressable delegation point before the child begins.
2. **Durable on success.** Delegated work becomes independently durable when the child successfully publishes its own branch.
3. **Parent transactional.** Delegation may execute against unpublished parent state, and parent publication defines the durability boundary unless or until the child publishes its own branch.

### Gap

The PRD should not accidentally inherit this answer from fork mechanics. The decision should be explicit before a fork-to-exec redesign or any durable-agent recovery work.

### Acceptance target

Before closing this decision:

1. choose one observable contract or explicitly defer the choice;
2. document what a user can inspect after parent failure, child failure, and successful child completion;
3. document how the chosen rule interacts with `context: fork`, `context: fresh`, transient operation, and parent branch publication;
4. ensure any future process redesign preserves the chosen product behavior.

### Likely next actions

- Add a short decision record once the preferred contract is chosen.
- If no contract is chosen for 0.12, mark fork-to-exec redesign as blocked on this product decision.

## 6. Documentation cleanup: display-output semantics

### Current finding

The previous traceability pass flagged a contradiction between the SRD and `doc/display-output-semantics.md` around durable display output.

The implementation check resolved the contradiction in favor of the SRD: tagged display output is now durable. `src/fyai_output.c` owns progressive tagged transcript documents and finalizes records with `tag`, `markdown`, `state`, and `fragments`; `src/fyai_turn.c` appends those records to a turn's `display_outputs` sequence.

### Gap

`doc/display-output-semantics.md` still contains historical current-state wording that says the persisted model has no display document. That is now stale and can confuse future product/SRD review.

### Acceptance target

Before closing this cleanup:

1. update the document to distinguish historical analysis from the current implemented state;
2. preserve any useful rationale about why live output and durable history need a shared semantic boundary;
3. remove or reword statements that describe absence of persisted display documents as current behavior;
4. ensure the document points to the SRD for the current software contract.

## 7. Not currently gaps

The following were investigated during the PRD/SRD pass and are not currently treated as 0.12 release gaps:

- durable tagged display output itself, because current code persists `display_outputs`;
- local durable state and no-daemon lifecycle;
- branch-local conversation and configuration;
- retained root handles and read-only exact-state inspection;
- provider-independent canonical history with provider observations retained separately;
- bounded file/shell results protecting model context;
- provenance-preserving compaction behavior;
- credential indirection and exclusion of raw secrets from ordinary durable project state.

These areas may still need normal tests or refinements, but they are not the concentrated gap list for the PRD baseline.

## 8. Recommended closure order

1. Refresh `doc/display-output-semantics.md` so the documentation set stops contradicting itself.
2. Close delegated-agent live-state isolation with an audit and targeted tests.
3. Close PTY/session correctness with implementation/test reconciliation against the terminal plan.
4. Add a provider/platform truthfulness checklist and representative tests.
5. Decide or explicitly defer the persistent delegation durability point.

The first item is documentation hygiene. The next three are release gates. The final item is a product decision that should happen before any delegation process-model redesign.
