# fyai SRD — Cost Composition

**Version:** 0.11 draft
**Status:** Pre-implementation (future phase — not in v1/v2/v3 scope)
**Author:** Pantelis Antoniou
**Extends:** fyai-srd-core v0.11

*Scope: A cross-phase analysis of how v1 and the phase mechanisms compose into the overall cost model. This document spans all phases and depends on substrate properties specified across core and the phase documents. Section references of the form "§N" or "core §N" point into fyai-srd-core; references to other phases name the phase explicitly.*

-----


This appendix describes the cost structure that the architecture services. It is included because the architectural choices specified throughout the document are individually justified on their own terms, but their combined effect on operating cost is not visible from any single section. This appendix makes the integrative cost picture explicit.

## F.1 Framing

The unit of measurement is dollars per successful outcome, not dollars per inference call. An outcome is the completion of a task — code written and tested, refactor performed and verified, bug diagnosed and fixed — to the user's acceptance. The cost of producing an outcome includes inference cost, failure-recovery cost, human supervision cost, and any other cost component the production of that outcome consumed.

This unit differs from the unit by which inference is conventionally priced. Inference providers price per token. Tools built on inference inherit that pricing surface. The user, however, does not consume tokens; the user consumes outcomes. The gap between dollars-per-token and dollars-per-outcome is filled by everything the system does between receiving a task and delivering an accepted result: the number of attempts required, the work wasted on failed attempts, the human time spent reviewing and correcting, and the share of tokens that needed to be expensive versus cheap.

The architecture is constructed to make the dollars-per-outcome quantity favorable across a workload mix typical of agentic coding tasks. Each phase contributes to this quantity through a specific cost-reduction mechanism. The contributions compose: each is independently meaningful, and the combination is what makes the overall cost position defensible.

## F.2 Phase Contributions

Each phase reduces a specific component of the dollars-per-outcome quantity.

**Phase 1 (stateless invocation, content-addressed arena).** Eliminates the cost of maintaining a resident process between invocations. Conventional agentic tools either run as daemons, consuming memory continuously regardless of activity, or restart from cold on each invocation, paying full context reload time on every interaction. Phase 1 supports concurrent invocations sharing arena pages through the kernel page cache, with no resident state between sessions. The cost contribution is the elimination of a baseline running cost that would otherwise be paid per active session per unit of wall-clock time.

**Phase 2 (filesystem-state causal log).** Eliminates the cost of recovering from agent actions that produced unwanted side effects. Without transactional filesystem isolation, a failed or wrong agent action leaves persistent artifacts that require manual cleanup, rollback via version control (when possible), or restoration from backup (when not). The human time consumed by these operations is a real cost component of producing outcomes with current tools, and it scales with the failure rate. Phase 2 reduces the cost of any individual failure to zero: the transaction is discarded, the filesystem returns to its prior state, and the user proceeds without manual recovery work. The cost contribution is the removal of recovery cost as a function of failure rate.

**Phase 3 detection and asymmetric inference.** Routes routine tokens to inference at the lowest per-token cost capable of producing them, and engages higher-cost inference selectively at positions where the higher cost is justified. The per-token cost ratio between routine and frontier inference is typically large; the proportion of tokens that require frontier capability is typically small. The cost contribution is a multiplicative reduction in average per-token inference cost, with magnitude determined by the workload's distribution of routine versus consequential positions.

**Phase 3 multi-sampling and pruned eager branching.** Increases the probability that a single invocation produces an accepted outcome by exploring multiple candidate continuations at significant decision points and selecting the best result. Without multi-sampling, sessions that produce unsatisfactory outcomes are re-run from the beginning, paying full inference cost for each retry. Multi-sampling pays incremental cost per additional branch — bounded by the divergent suffix length, not the full session — and reduces the expected number of full retries per successful outcome. The cost contribution is a reduction in the retry multiplier on the expected cost of an outcome.

**Phase 4 (content-addressed inference-state cache).** Reduces the prefill cost on fork and resume operations, making the multi-sampling and retrospective-fork mechanisms of phase 3 cheaper to use routinely. Without phase 4, each branch or retry pays full prefix re-prefill cost; the cost of exploration scales with prefix length and discourages frequent use. With phase 4 and a cached KV blob present, branches resume from stored KV state and pay only the divergent suffix cost. The magnitude is backend-dependent: large for CPU and unified-memory engines that consume KV directly from mmap'd pages, marginal for discrete-GPU engines that must transfer KV host-to-device at a cost comparable to recompute (§E.5). The cost contribution is therefore "available and cheaper on the backends where it helps," with reprefill as the floor everywhere — not an unconditional elimination of prefill cost.

**Execution retry within the tool-use loop.** Reduces the failure rate at the tool-call layer by catching malformed model output and providing corrective signals within the same invocation. Without this mechanism, format failures and protocol mismatches surface as failed sessions that require manual intervention or full retry. With this mechanism, the small model's known fidelity issues are absorbed by the surrounding system and do not contribute to outcome failure rate. The cost contribution is a reduction in the failure rate component of the per-outcome cost equation.

## F.3 Composition

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

## F.4 Workload Sensitivity

The cost reduction realized in practice depends on workload characteristics.

For workloads in which most tokens require frontier capability — tasks where the underlying reasoning difficulty is high throughout, where small models cannot produce viable continuations even on routine positions — the asymmetric inference mechanism provides minimal savings because escalation occurs constantly. For these workloads, the architecture still provides phase 2 rollback and phase 3 retrospective exploration, but the per-token cost reduction is small.

For workloads in which most tokens are routine and decisions are clustered — typical of well-scoped coding tasks, refactors, bug fixes, and the broader category of work that involves substantial mechanical generation around a small number of consequential choices — the asymmetric inference mechanism provides the largest savings, and the composition with the other mechanisms delivers the integrated reduction described in §F.3.

The architecture is well-suited to the second workload category and provides bounded benefit on the first. The proportion of real-world agentic work that falls into each category is an empirical question whose answer determines the architecture's aggregate value. Available evidence from production AI coding tool usage patterns suggests that the second category is the larger share, but the precise proportion is workload-specific and deployment-specific.

## F.5 Scope and Honest Boundaries

The architecture does not address tasks where the underlying reasoning capability gap between routine and frontier models is the binding constraint. If a task requires frontier-class reasoning at most positions, no surrounding system can substitute small-model inference for frontier inference without quality loss. The architecture provides mechanisms that reduce cost on tasks where this constraint does not bind; it does not claim to overcome the constraint where it does.

The architecture does not address tasks for which agentic execution is the wrong tool entirely. Some user needs are better served by direct human work, by non-AI tooling, or by simple frontier-model chat without the orchestration overhead. The cost structure described here applies to tasks for which agentic execution is the right approach; for other tasks, the comparison is to a different baseline.

The cost reduction figures depend on inference provider pricing, which varies and changes over time. The structural argument — that routing tokens to inference at the lowest cost capable of producing them, recovering failures cheaply, and exploring alternatives cheaply, reduces total cost per outcome — holds across pricing scenarios. The magnitudes depend on the specific ratios at any given time.

## F.6 Implications for Architectural Discipline

The cost composition described above depends on the architectural properties specified throughout this document. Each cost-reduction mechanism requires specific properties from the substrate:

- Multi-sampling at low marginal cost is *helped* by content-addressed inference-state cache (phase 4) on backends where KV load beats reprefill; where it does not (discrete GPU, short prefixes), reprefill remains the floor and multi-sampling still functions, at higher marginal cost.
- Filesystem rollback at zero cost requires the transactional namespace layer (phase 2) with content-addressed deltas.
- Asymmetric inference requires distribution-level branch detection (phase 3) and provider abstraction (§4.7) supporting heterogeneous model assignments within a session.
- Retrospective exploration requires canonical recording of decision points (phase 3) and persistent address-stable storage (§5.3) for forking from prior session states.
- Execution retry requires the tool-use loop to expose intermediate state for corrective injection without restarting the session.

Each of these requirements is independently justified within its respective section of the document. The cost-composition perspective makes their interdependence visible: each mechanism contributes to the cost equation, and each mechanism depends on substrate properties specified for reasons that may appear unrelated to cost at the point of specification. The disciplined preservation of these substrate properties across implementation is therefore a prerequisite for realizing the cost composition described here.
