# fyai SRD — Phase 3: Inference Branching

**Version:** 0.11 draft
**Status:** Pre-implementation (future phase — not in v1/v2/v3 scope)
**Author:** Pantelis Antoniou
**Extends:** fyai-srd-core v0.11

*Scope: Phase 3 captures the token-selection distribution at decision points (in the metadata layer, not canonical content) and supports eager and retrospective fork, scoring, and asymmetric inference. It adds branch-point records to the turn metadata-events generic. Section references of the form "§N" or "core §N" point into fyai-srd-core; references to other phases name the phase explicitly.*

-----


This appendix specifies the anticipated phase 3 extension: making the token-selection distribution at each generation step a first-class production-time artifact (recorded in the metadata layer, §D.1), supporting fork operations at decision points during generation, and supporting fork operations at decision points retrospectively after a session has completed. Phase 3 adds branch-point records to the turn's metadata-events generic and a class of operations that consume them; it does not add a canonical-content reference type, because token-selection distributions are not reproducible across engines and hardware and therefore must not participate in canonical identity.

Phase 3 is not part of v1 or v2 scope. It is documented here because the architectural pattern that delivers v1 and v2 extends to v3 without modification, and because v1 schema decisions must accommodate the phase 3 reference type without migration.

## D.1 Concept

At each token generation step, an autoregressive language model produces a probability distribution over its vocabulary, from which a single token is sampled. Most distributions are sharply peaked: one token holds near-unit probability and the rest are noise. A minority of distributions are non-trivial: two or more tokens hold substantial probability mass, indicating positions at which the model's continuation is genuinely under-determined.

These non-trivial distributions correspond to decision points within the generated turn. Recording them preserves information that single-token sampling discards. In phase 3 the sampled token sequence is canonical content as before, and the turn additionally accumulates, in its **metadata-events generic** (§6.1), a sequence of branch-point records at positions where the distribution exceeded configured significance thresholds. The records are metadata about the production of the turn, not part of its canonical content.

Branch-point records are deliberately *not* part of canonical content. A token-selection distribution is a production-time fact: the top-K probabilities depend on the engine, the hardware, and floating-point reduction order (batched GPU inference is not bitwise-reproducible across batch sizes or kernel versions), so the exact values — and therefore which positions cross the entropy/top-k/mass thresholds of §D.3 — are not reproducible across environments. Were these records canonical, canonical identity would depend on non-reproducible floating-point values and cross-context dedup of identical turns would fail on low-order bits near a threshold. Placing them in metadata (where production-time facts like per-segment model identity already live, §D.5.4) avoids both problems: metadata need not be reproducible or dedup-stable. A branch-point record contains the position within the turn, the top-K alternative tokens with their probabilities, and any later-computed scoring metadata.

## D.2 Operational Modes

Phase 3 supports three operational modes, selected per session by configuration or by command-line flag.

**Mode 1: Single-path generation.** No branching during the session. Branch-point metadata is recorded passively as a side effect of normal generation. The session produces a single conversation path identical to what a non-phase-3 invocation would produce. Branch points are available for retrospective exploration (§D.7) after the session completes. This is the default mode and the cheapest in terms of inference cost.

**Mode 2: Eager pruned branching.** Branching occurs during the session at detected significant uncertainty points. Multiple branches generate in parallel. Branches whose running confidence drops below a configured threshold are pruned mid-generation to bound active compute. Surviving branches are presented at session end. This mode is more expensive than mode 1 but produces multiple candidate outcomes from a single invocation.

**Mode 3: Hybrid.** Eager branching is performed up to a configurable depth limit; deeper exploration is deferred to retrospective fork after the session completes. This combines mode 2's benefit of producing multiple candidates from one invocation with a bounded eager cost.

The user (or per-session configuration) selects the mode based on workload requirements. Sessions executed in automated contexts (CI, batch processing) typically use mode 1 for cost predictability. Interactive sessions exploring difficult problems typically use mode 2 or mode 3 for candidate diversity.

## D.3 Detection

A branch point is identified when the token-selection distribution meets configurable significance criteria. The default criteria combine three signals:

- **Entropy threshold.** The entropy of the distribution exceeds a configured value. Higher entropy indicates the model considered more alternatives with non-trivial probability.
- **Top-k ratio.** The ratio of the probability of the second-most-likely token to the most-likely token exceeds a configured value. A high ratio indicates a near-tie between the chosen token and at least one alternative.
- **Mass concentration.** The probability mass held by the top token is below a configured threshold. Low concentration indicates the distribution is spread across multiple alternatives rather than dominated by one.

A position is recorded as a branch point when all three criteria are met. The thresholds are configurable per session and per workload, and reasonable defaults are provided.

Recording captures the position, the chosen token, and the top-K alternatives with their probabilities. K is configurable; a default of 8 is suggested. Probabilities are stored with sufficient precision for downstream operations and reproducibility.

Detection adds no inference cost beyond what is required to expose the distribution at each step. Recording adds storage cost proportional to the number of branch points and the configured top-K, which is small compared to the underlying turn content for typical workloads.

## D.4 Eager Pruned Branching (Modes 2 and 3)

When operating in mode 2 or mode 3, fyai performs branching during generation. At each detected branch point, multiple branches are spawned from the top-K alternatives, generate in parallel, and are subject to runtime pruning to bound the active branch population.

### D.4.1 Branch Spawning

When a branch point is detected during generation and the active branch budget permits additional branches, fyai forks the current branch at the detected position. The number of forks spawned is the configured branching factor (default: 2), drawn from the top-K alternatives at that position. The branching factor may be smaller than the configured K from §D.3; recording uses K alternatives, while spawning uses the branching factor B ≤ K.

Each spawned branch begins inference with the same KV cache state as the parent at the branch position, with the alternative token forced as the next sampled token. On local inference where the parent's slot is still resident, this is an in-memory slot fork at genuinely zero KV reconstruction cost — the parent KV is already in the engine. (Resuming a branch whose parent slot is *not* resident — retrospective fork, or after eviction — instead loads a stored KV blob or reprefills, with the backend-dependent costs of §E.5.) On cloud inference, this is an additional API call per branch with the prefix re-submitted.

Spawned branches are recorded in the refs directory generic (§5.10) as new branches with metadata identifying their parent and the branch point they diverged at. Spawned branches are first-class branches and may themselves spawn further branches at subsequently detected branch points.

### D.4.2 Active Branch Budget

The active branch budget is a configured upper bound on the number of branches generating simultaneously. When a new branch would exceed the budget, fyai applies the configured budget-enforcement policy:

- **Block new branching.** Existing branches continue; the would-be new branch point is recorded but not spawned. Recording the suppressed branch preserves the option of retrospective exploration.
- **Displace the weakest existing branch.** The active branch with the lowest running confidence is pruned (§D.4.3), and the new branch takes its slot. The displaced branch is recorded as a killed branch.
- **Suspend.** Existing branches pause; the new branch generates; on its completion or pruning, suspended branches resume. This option is available only for inference backends that support slot suspension and resumption efficiently.

The default policy is to block new branching at budget exhaustion. The other policies are available where workload characteristics favor them.

### D.4.3 Running Confidence and Pruning

Each active branch tracks a running confidence signal computed from its per-token entropy over a sliding window. The window size (default: 32 tokens) and the pruning threshold are configurable. When a branch's running confidence falls below the threshold, the branch is pruned.

Pruning is an internal control operation, not a user-facing output. The confidence signal is not surfaced as a score the user evaluates. The system uses confidence to decide which branches deserve continued compute; the user evaluates the surviving branches at session end based on their actual outputs.

Pruning terminates inference on the branch and discards its KV cache state. The branch's canonical content up to the pruning point is retained in the arena. The branch's entry in the refs directory generic is updated with status "killed" and metadata recording the pruning reason (running confidence threshold violation), the position at which pruning occurred, and the running confidence value at that position.

A grace period (default: 16 tokens) is applied after a branch is spawned, during which pruning is suspended. The grace period prevents premature termination of a branch that has just forked into an alternative trajectory and has not yet stabilized its generation pattern.

### D.4.4 Confidence Measurement

Running confidence is derived from per-token entropy within the configured sliding window. Several measurement schemes are supported, selected by configuration:

- **Absolute floor.** A fixed threshold on mean per-token entropy. Below the floor, the branch is pruned. Simple and predictable, may be over- or under-tuned for workload-specific difficulty.
- **Relative to parent.** The branch's mean per-token entropy compared to its parent's entropy at the corresponding position. If the branch is meaningfully less confident than its parent was at the same point, the branch is pruned. Corrects for task difficulty.
- **Adaptive.** The threshold is adjusted during the session based on the observed entropy distribution across all active branches. Lenient pruning on uniformly hard sessions, stricter pruning on uniformly easy ones.

The default measurement scheme is relative to parent. The other schemes are available where workload characteristics favor them.

Running confidence is a measurement of the model's selection certainty over recent tokens, not an estimate of the branch's correctness. A branch may generate confidently into incorrect output (low entropy, wrong answer) and a branch may generate uncertainly into correct output (high entropy, eventual right answer). The pruning signal is biased toward continuation of confident generation regardless of correctness, with the rationale that confident generations reach completion and become evaluable, while uncertain generations consume compute without producing evaluable outputs.

### D.4.5 Killed Branches as Recorded Artifacts

Pruned branches are not deleted; they are recorded with a killed status. The canonical content generated up to the pruning point is retained in the arena, contributes to deduplication with other branches that share prefixes, and is available for inspection.

Recording killed branches serves several functions:

- **Audit.** The session record shows which branches were explored and which were pruned, with the pruning reason in metadata.
- **Resumption.** A user reviewing the session may explicitly resume a killed branch from its pruning point. The KV state at the pruning point was discarded for compute savings, so resumption pays the reprefill cost; canonical content provides the prefix.
- **Training signal.** Killed branches with their running confidence trajectories are training data for tuning pruning thresholds or for training a learned pruning model (§D.4.6).

The arena garbage collector treats killed branches like any other branches: they are reachable through the refs directory generic and their content is retained until the branch entry is removed and a `fyai gc` operation collects the now-unreachable generics.

### D.4.6 Learned Pruning (Optional Extension)

The default pruning policy is heuristic (running confidence threshold). An optional extension replaces the heuristic with a learned model that consumes a branch's recent generation history and predicts whether the branch is likely to reach a valuable completion.

The training signal is the same as for the divergence scoring model (§D.6.3): user actions in retrospective review label branches as valuable or not. A learned pruning model uses the same labeled corpus to predict "should this branch continue or be pruned" instead of (or in addition to) "is this branch point worth exploring retrospectively."

Learned pruning is available as an extension; the default deployment uses heuristic pruning. Switching to learned pruning is a configuration change and does not affect canonical content or arena format.

## D.5 Asymmetric Inference

Phase 3's detection of significant uncertainty positions enables an inference strategy in which a less-capable model handles routine generation and a more-capable model is engaged selectively at detected branching points. The routine model carries the majority of tokens; the strong model is consulted at positions where the routine model itself indicates substantive choice. This strategy reduces the share of generation that requires strong-model capability without surrendering quality at the positions where strong-model capability is consequential.

Asymmetric inference is an optional operational mechanism, available independently of the operational modes described in §D.2. It composes with all three modes: a single-path session may use asymmetric inference, an eager-branched session may use asymmetric inference within each branch, and retrospective fork continuations may use asymmetric inference for the continuation.

### D.5.1 Concept

A session is configured with two model assignments: a *routine model* and a *strong model*. The routine model handles generation by default. At each token step, the routine model's distribution is evaluated against the branch-point detection criteria of §D.3. When the criteria fire, the escalation policy (§D.5.2) determines whether the strong model takes over generation for a bounded segment.

The routine and strong models may be:

- Two local models of different sizes (e.g., a 7B routine model with a larger local strong model).
- A local routine model paired with a cloud strong model.
- Two cloud models of different tiers (e.g., a smaller cloud model as routine, a frontier cloud model as strong).

The selection is per-session configuration. Sessions may also configure more than two models in a tiered arrangement, with escalation policy deciding which tier to engage at each detected position based on observed uncertainty and scoring.

### D.5.2 Escalation Policy

When detection fires, the escalation policy determines whether to actually engage the strong model. Several policies are supported, selected by configuration:

- **Threshold escalation.** Escalation occurs whenever detection criteria fire above a configured escalation threshold (typically stricter than the recording threshold of §D.3). Simple and predictable; does not distinguish significant from synonymous alternatives.
- **Scored escalation.** The divergence scoring mechanism of §D.6 is applied at the detection point to estimate significance. Escalation occurs when the score exceeds a configured threshold. More principled than threshold escalation but adds latency from the scoring step.
- **Hybrid escalation.** Threshold escalation handles obviously significant positions (very high entropy or very low mass concentration) at low latency. Scored escalation handles borderline positions where threshold alone is ambiguous. This is the default policy.

The escalation threshold and scoring threshold are configurable per session. Reasonable defaults are provided; tuning per workload is expected.

Positions where detection fires but escalation does not occur are still recorded as branch points in the turn's metadata (§D.1). The routine model continues generation at the detected position. The recorded branch point remains available for retrospective fork (§D.7) and for the training corpus described in §D.6.3.

### D.5.3 Prefix Transfer and Escalation Horizon

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

### D.5.4 Recording

Per-token model identity is recorded as metadata on the turn, not as part of canonical content. The metadata indicates, for each token range within the turn, which model generated that range. Two turns with identical content but different model identity metadata canonicalize equal at the content level. The metadata is part of the metadata-events generic (§6.1) which already accommodates per-model production records; phase 3 extends this to per-segment-within-a-turn granularity.

Recording model identity per range supports audit, reproducibility, and cost analysis. A session's record shows which segments were generated by which model, allowing the user to verify that escalation occurred at expected positions and to assess the contribution of each model to the outcome.

Branch-point records (§D.9) at escalation positions are augmented with the escalation outcome: which model was engaged, the escalation horizon used, and whether the escalation altered the trajectory relative to what the routine model would have produced (determinable when the routine model's continuation from the detection point is also recorded, as it is in modes 2 and 3 with eager branching).

### D.5.5 Cost and Quality Properties

The cost profile of asymmetric inference depends on the escalation rate, the cost ratio between routine and strong models, and the escalation horizon. The structural shape is that total cost approaches routine-model cost as the escalation rate decreases, and approaches strong-model cost as the escalation rate increases. For typical workloads with detection criteria tuned for substantive significance, escalation rates of one to a few percent of tokens are expected, producing total cost dominated by the routine model with a bounded strong-model overhead.

Specific cost ratios depend on the model pairing and provider pricing and are not committed here. The architectural property is the asymmetry itself: routine generation dominates the token count, strong generation dominates the per-token cost, and the cost-quality tradeoff is determined by the escalation policy.

Quality at escalation positions is determined by the strong model. Quality at non-escalation positions is determined by the routine model. The detection mechanism's role is to ensure that escalation occurs at positions where strong-model quality would meaningfully differ from routine-model quality. Where the routine model and strong model would produce equivalent outputs (low-entropy positions), escalation is unnecessary and is correctly avoided by the detection criteria.

A routine model that is poorly calibrated — producing low-entropy distributions at positions where its outputs are nonetheless incorrect — will under-trigger escalation and produce sessions that are routine-model-quality at positions where strong-model intervention would have helped. Calibration of the routine model is therefore a prerequisite for effective asymmetric inference. Open-weight models in the 7B-13B range typically exhibit acceptable calibration for coding workloads; calibration quality should be verified per workload before relying on asymmetric inference for production use.

### D.5.6 Composition With Other Phase 3 Mechanisms

Asymmetric inference composes with eager pruned branching (§D.4) as follows: at a detected branch point in mode 2 or 3, the eager branching mechanism spawns multiple alternative branches; if asymmetric inference is also enabled, each spawned branch may use the strong model for generation up to its escalation horizon before yielding to the routine model. Spawning and escalation are independent operations triggered by the same detection event.

Asymmetric inference composes with retrospective fork (§D.7) as follows: a retrospective fork at a recorded branch point may be configured to use the strong model for the continuation, the routine model, or asymmetric inference. The user (or automatic policy) selects which inference strategy to apply to the retrospective branch.

Asymmetric inference composes with the scoring mechanism (§D.6) as follows: the scoring model evaluates branch points using the divergence between top-K alternatives. When asymmetric inference is enabled, the same divergence scoring can drive the escalation policy (§D.5.2) directly. The scoring model serves both retrospective ranking and live escalation decisions from a shared trained artifact.

## D.6 Scoring (Retrospective Mode)

Branch points detected during generation but not spawned (in mode 1, or in modes 2/3 when budget was exceeded) are candidates for retrospective exploration. Branch points vary widely in significance; detection alone does not distinguish those whose alternatives would lead to substantively different outcomes from those whose alternatives are near-synonyms.

Phase 3 introduces a two-stage scoring mechanism that estimates the significance of each recorded branch point for the purpose of ranking retrospective exploration candidates.

### D.6.1 Stage 1: Divergence Sampling

For each branch point being scored, the top-K alternative tokens are extended forward by greedy continuation for a configured lookahead horizon (default: 64 tokens). This produces K short completion fragments rooted at the same prefix and diverging from one specific token onward.

Divergence sampling is performed on demand, when scoring is requested. Sessions in which scoring is never consulted incur no divergence sampling cost.

Each fragment is generated against the same model and tokenizer as the original session. The inference cost per fragment is bounded by the lookahead horizon. Total stage 1 cost per scored branch point is approximately K times the cost of generating the lookahead horizon, which is small relative to the full session cost.

For local inference, divergence sampling reuses the KV cache state at the branch position via phase 4 infrastructure (Appendix E). For cloud inference, divergence sampling requires an additional API call per alternative.

### D.6.2 Stage 2: Divergence Scoring

A scoring model consumes the original chosen continuation and the K alternative fragments and produces a single scalar significance score per branch point. The score estimates the probability that exploring this branch point retrospectively would lead to a meaningfully different downstream trajectory than the original.

The scoring model is small relative to the generation model. The task — comparing short fragments for semantic and behavioral divergence — does not require open-ended generation capability. A model in the hundreds-of-millions to low-billions of parameters range, specialized for this task, is sufficient.

The scoring model considers multiple signals:

- **Tool-call divergence.** Whether the fragments contain tool calls, and whether those tool calls differ in name, arguments, or both.
- **Structural divergence.** For code-generation contexts, whether the fragments produce different abstract syntax tree shapes when parsed in the appropriate language.
- **Embedding distance.** Distance between fragment embeddings in a representation space, computed using the scoring model's encoder.
- **Surface-form similarity.** Token-level overlap and edit distance, used to identify cases where alternatives differ only in synonyms or stylistic variants.

The output score is a value in a configurable range (default 0 to 1) where higher scores indicate higher predicted significance. Scores are recorded as part of the branch-point metadata in the metadata-events generic (§D.1).

### D.6.3 Scoring Model Distribution and Training Signal

The scoring model is a content-addressed artifact stored in the arena. Multiple scoring models may coexist in a deployment; users may select among them per session or per scoring operation. The scoring model is versioned, and the version used to score a given branch point is recorded as part of the metadata.

A default scoring model is distributed with fyai releases. Deployments may train deployment-specific scoring models using collected session data.

The retrospective fork operation (§D.7) and the eager pruned branching operation (§D.4) both provide training signals for the scoring model. When a user revisits a branch point retrospectively and the alternative path leads to a successful outcome, the branch point is labeled as significant. When the alternative path leads to the same or worse outcome, the branch point is labeled as not significant. When an eager-spawned branch survives pruning and is selected by the user as the preferred outcome, the branch point that spawned it is labeled as significant. Branch points that are not revisited and not spawned remain unlabeled.

This produces a corpus of labeled branch-point examples as a side effect of normal usage. The corpus is local to each deployment; aggregation across deployments is opt-in.

The corpus is used to train successor versions of the scoring model and, where used, the learned pruning model of §D.4.6. Training is performed offline, outside the inference path. Trained models are deployed as updated content-addressed artifacts in the arena.

## D.7 Retrospective Fork Operation

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

## D.8 Integration With Review Interface

The phase 2 review interface (§C.7) is extended in phase 3 with a branching surface that presents both eager-branching outcomes (modes 2 and 3) and retrospective branching candidates (all modes).

For sessions executed in mode 2 or mode 3, the review interface presents all surviving branches at session end. Each branch is shown with its conversation outcome, its filesystem effects, and its provenance (which branch point spawned it). Killed branches are not presented in the primary review surface but are available for inspection on demand. The user selects one or more surviving branches to commit, discards the rest, and may initiate retrospective forks from any branch's recorded branch points.

For sessions executed in mode 1, or for retrospective exploration in any mode, the review interface presents the recorded branch points ranked by significance score (§D.6). The ranked view shows, for each branch point: its position in the session (turn and offset), a brief excerpt of the surrounding context, the chosen token, the top alternatives with their probabilities and significance scores, and an indication of the predicted divergence shape.

The user selects a branch point and an alternative to initiate a retrospective fork. The new branch is created and the session continues. The user may initiate multiple forks from the same review session if the alternatives are independent and worth exploring in parallel.

The review interface also exposes the branch-point distribution as inspectable data independent of any actual fork operation. The distribution provides insight into where the model exercised consequential choice during the session.

## D.9 Recording

Branch-point information is recorded in the assistant turn's **metadata-events generic** (§6.1, §D.1), not in canonical content. A turn's representation in phase 3 has the form:

- The canonical content payload: the sequence of generated content elements (text, tool_use), unchanged from earlier phases and unaffected by phase 3.
- A metadata sequence of branch-point records, each containing position within the turn, top-K alternatives with probabilities, and optional scoring metadata.

Two assistant turns are canonical-equal when their content elements match, *regardless* of their branch-point records. A session that records branch points and a session that does not collapse to the same canonical turn when their content elements are identical — which is the desired behavior, since the sampled content is the same semantic action whether or not the distribution around it was captured. Branch-point records distinguish the turns only at the production-event level, exactly as differing timestamps or model identity do.

This placement is deliberate and resolves a determinism problem. Branch-point records are *not* reproducible across environments: top-K probabilities depend on engine, hardware, and floating-point reduction order, and even local engines vary unless they document strict floating-point determinism. Were these records canonical, identity would hinge on non-reproducible low-order bits and cross-context dedup of identical turns would fail near a detection threshold. As metadata, they carry no reproducibility or dedup-stability requirement; the recorded distributions are accurate snapshots of one specific inference call, which is all retrospective fork and scoring require.

Scoring metadata is added to branch-point records after scoring is performed: scoring model version, timestamp, resulting score. Multiple scoring entries may accumulate on a single branch-point record if it is rescored with newer or different scoring models. All of this lives in the metadata layer.

Per-token entropy data, used for running confidence in eager pruned branching (§D.4.3), is stored as optional metadata on the turn. Two turns with identical content and different entropy metadata canonicalize equal. Entropy data is recoverable in principle by re-running inference and is treated as a derived artifact retained for efficiency.

Per-segment model identity, used for asymmetric inference (§D.5.4), is stored as metadata on the turn. Two turns with identical content generated by different model assignments canonicalize equal at the content level; the model identity metadata distinguishes them at the production-event level.

Killed branches are recorded with a "killed" status in the refs directory generic. Per-branch metadata for killed branches includes the pruning reason, the position at which pruning occurred, and the running confidence value at that position. Killed branches share canonical content with their unkilled prefix through structural sharing.

## D.10 Provider Asymmetry

Phase 3 requires the inference provider to expose the token-selection distribution at each generation step. Providers vary in their support for this capability, and the practicality of each operational mode varies accordingly.

- **Local inference engines** (llama.cpp, vLLM, SGLang, and others) expose the full distribution natively. All three operational modes operate at native efficiency. Eager pruned branching is particularly well-suited to local inference because slot fork is cheap and pruning frees inference slots immediately.
- **OpenAI-compatible APIs** (OpenAI, Together, Fireworks, OpenRouter, and others) expose the top-K alternatives via the `logprobs` and `top_logprobs` parameters, with K typically capped at 20. Phase 3 detection operates on the exposed top-K. Mode 1 (single-path generation with retrospective fork available) is well-supported. Modes 2 and 3 (eager branching) are feasible but expensive: each branch requires an additional API call, and pruning a branch wastes the tokens generated so far. The cost overhead may be acceptable for high-value sessions but is not the default for routine sessions.
- **Google Gemini** exposes top-K alternatives via the `responseLogprobs` parameter. Operation is analogous to OpenAI-compatible APIs.
- **Anthropic** does not currently expose token-level probability data on the messages API. Phase 3 detection is not available on Anthropic-native inference at this time. fyai's provider abstraction (§4.7) declares the logprobs capability per provider; when the capability is absent, phase 3 is disabled for that provider, and the user is informed.

The provider abstraction surfaces the available capabilities to the user. Sessions conducted against providers without logprobs support proceed normally but do not produce branch-point records, and phase 3 operations are not available for them.

The cost profile of eager pruned branching on local inference compared to cloud inference is one of the more pronounced asymmetries in the architecture. On local inference, the marginal cost of an additional branch is the cost of generating its tokens, and pruning frees inference capacity immediately. On cloud inference, the marginal cost of an additional branch is the cost of a full API call including prefix prefill, and pruning wastes whatever inference has been performed. Eager branching is the default-on capability for local inference deployments and an opt-in for cost-aware cloud deployments.

Asymmetric inference (§D.5) operates across providers in three configurations. With routine and strong models both local, escalation cost is the strong-model prefill plus its horizon, paid in local compute. With routine local and strong cloud, escalation cost is one API call per escalation event, including prefill of the prefix on the cloud strong model. With both routine and strong on cloud, escalation cost is one API call per escalation event against the strong model's tier. The detection mechanism (§D.3) requires logprobs support on the routine model only; the strong model is not required to expose logprobs because it is engaged only during the escalation horizon and not used for branch-point detection within that horizon.

## D.11 Schema Forward Compatibility

The phase 3 branch-point records extend the turn's metadata-events generic following the same pattern by which metadata already accommodates per-model production records (§6.1). Existing canonical turns from v1 and v2 are forward-compatible: they have no branch-point records, and since the records are metadata rather than content, their absence does not affect canonical identity in any case — a pre-phase-3 turn and a phase-3 turn with identical content are canonical-equal.

Bundle export (§7.3) extends to optionally include branch-point records from the metadata layer. A scrubbed export may omit them if the user prefers not to share inference-uncertainty data; a `--scrub-branch-points` flag is added to the bundle export interface. Because the records are metadata, omitting them never alters the canonical content of the exported turns.

The arena allocator and storage substrate (§5.4, §5.5) handle branch-point record storage identically to other metadata generics. Killed-branch metadata in the refs directory generic uses the same CAS-published update mechanism as any other refs directory update (§5.10). No changes to the arena format are required.

