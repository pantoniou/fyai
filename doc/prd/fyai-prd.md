# Product Requirements Document: fyai

**Version:** 0.12 draft
**Status:** Product baseline draft
**Author:** Pantelis Antoniou

This document defines the product contract for `fyai`: what the product is, who it is for, the user-visible model it presents, and the properties that must remain true as its implementation evolves.

The Software Requirements Document (`doc/srd/fyai-srd.md`) defines the software contract that realizes these product requirements. Focused design documents may describe particular mechanisms. When those documents disagree about product intent, this PRD is the upstream authority.

Requirement maturity for the 0.12 baseline is tracked in `doc/prd/fyai-0.12-scope.md`. A requirement may be part of the product contract before every implementation mechanism is settled; conversely, an implemented mechanism does not automatically become a product promise.

---

## 1. Product definition

`fyai` is a local, stateless, daemon-less AI coding assistant designed to behave like a Unix command without giving up the durable history, branching, tool use, delegation, and interactive terminal experience expected from a modern coding agent.

A `fyai` process is temporary. The user's work is durable.

Each invocation opens the repository's durable agent state, performs a requested management operation or model/tool loop, publishes durable changes when appropriate, and exits. Interactive use keeps the invocation alive for the terminal session, but does not introduce a resident service or hidden process state.

The product is intentionally local-first. Its durable history is owned by the user and repository rather than by a model provider, hosted chat product, or long-lived fyai service.

## 2. Problem

AI coding assistants commonly bind useful work to one or more of the following:

- a hosted conversation whose durable identity belongs to a provider;
- a resident application, daemon, IDE extension, or hidden session process;
- a single model or provider transcript format;
- ephemeral delegated agents whose reasoning and work disappear into a parent response;
- opaque mutable state that is difficult to inspect, branch, recover, or automate.

These properties make coding agents convenient in the moment but weaker as durable developer tools. They make it harder to script an agent like a command, preserve and compare alternative lines of reasoning, change providers without abandoning history, inspect delegated work, or understand exactly what state will be used by the next invocation.

`fyai` exists to make agent work behave more like durable developer state and less like a remote chat session.

## 3. Product principles

### 3.1 The process is temporary; the work is durable

The lifetime of a conversation or line of work must not depend on a running `fyai` process. A normal invocation may start, do useful work, publish its state, and terminate without reducing the user's ability to continue later.

### 3.2 History belongs to the user

The durable identity of a conversation must be represented by fyai's own canonical state, not by a provider's server-side session or wire transcript. Provider observations may be retained for diagnostics and fidelity, but they do not define the product's conversation model.

### 3.3 Unix use is first-class

`fyai` must remain useful as a normal command: composable from a shell, understandable through standard input/output/error behavior, automatable without a resident service, and able to perform useful one-shot work.

Interactive use is equally first-class, but must be an interaction mode of the same product rather than a separate stateful application architecture.

### 3.4 Branches are agent work, not UI tabs

A branch represents an independent line of agent work. It owns both conversation state and the configuration needed to continue that work. Branches must make alternatives cheap to create, inspect, revisit, compare, and combine without copying the project directory.

### 3.5 Delegation must leave inspectable work

A delegated agent is not merely an opaque tool result. Persistent delegation must leave behind an addressable line of work that can be inspected and continued. The parent receives a useful report while the delegated conversation remains available as durable history.

### 3.6 Providers and models are replaceable dependencies

Changing a supported provider, model, or API grammar must not require abandoning the semantic identity of an existing fyai conversation. Provider-specific capabilities may differ, but the product must preserve a coherent canonical history across supported transitions.

### 3.7 Explicit state beats hidden state

Durable project state, configuration intent, branch selection, credentials, and transient operation must have explicit boundaries. fyai must not depend on hidden daemon memory to explain what happens next.

### 3.8 Trust boundaries are product behavior

Tool execution, credentials, sandboxing, user questions, and delegated work are part of the product contract rather than implementation afterthoughts. The user must be able to reason about what the agent can access and what durable state it leaves behind.

### 3.9 Human-facing history is its own durable concern

The content needed to continue a model conversation, the provider-specific observations needed for fidelity, and the human-facing conversational output shown to the user serve different purposes and must not be conflated merely because one can sometimes be reconstructed from another.

Transient terminal chrome and diagnostics are not durable conversation history. Durable conversational output must remain inspectable after the interactive rendering process has exited.

## 4. Target users

The primary user is a developer who is comfortable working from a terminal and wants an AI coding agent to participate in an existing Unix-oriented workflow.

Important user profiles include:

- a developer who wants to invoke an agent for one task from a shell or script;
- a developer who wants a rich interactive coding session without starting a resident application;
- a developer exploring multiple implementation approaches and wanting each alternative preserved;
- a developer who wants to delegate research or implementation while retaining the delegated history;
- a developer who changes models or providers according to task, cost, capability, or availability;
- an automation author who needs stable, inspectable state and predictable command behavior.

The product does not require users to understand libfyaml allocation, provider wire formats, event-loop internals, or other implementation mechanisms in order to use these capabilities.

## 5. Core product model

### 5.1 Repository arena

A repository may contain a local fyai arena. The arena is the durable source of truth for fyai's project-local agent state.

The product must make it possible to leave no resident fyai process after an invocation while retaining the state necessary to continue later.

### 5.2 Branch

A branch is a named line of work containing a conversation and the configuration used to continue it. Branches are independent alternatives inside one arena.

Changing branches changes agent context, not the repository working tree. fyai branches are conceptually adjacent to Git branches but are not Git branches and must not pretend to synchronize files as Git does.

### 5.3 Turn history

A branch contains durable conversation history. Users must be able to inspect that history and refer to prior branch states symbolically.

History operations should favor recoverability. Moving a branch backwards must not immediately destroy the state that was previously at its tip.

### 5.4 Configuration

Configuration is part of the state needed to continue a line of work. A branch may therefore use a different model, provider, reasoning policy, or tool policy from another branch.

The product must distinguish durable configuration from invocation-local overrides and transient operation.

### 5.5 Provider

A provider supplies model inference. It does not own fyai's durable conversation identity.

fyai may preserve provider-specific observations where necessary to continue requests faithfully or diagnose behavior, while presenting a provider-independent durable product model.

### 5.6 Display output

A display output is durable human-facing conversational content. It is distinct from canonical model-replay content and from provider-specific observations.

The product may render the same display output differently according to terminal capabilities, theme, or presentation policy, but the durable conversational content must remain inspectable after the live invocation ends.

### 5.7 Agent

A persistent delegated agent works on its own branch. It may begin from inherited context or from a fresh delegated task according to the requested delegation mode.

A standalone agent invocation may be transient when persistence is not part of the requested experience.

## 6. Required experiences

### 6.1 One-shot coding task

A user can invoke `fyai` with a task, allow the model to use permitted tools until the task reaches a terminal response, receive the canonical answer through the command-line interface, and continue from the resulting durable state in a later invocation.

### 6.2 Interactive coding session

A user can enter an interactive terminal session with streaming model output, tool activity, questions, and delegated work while retaining the same durable product model used by one-shot invocations.

Leaving the interactive session must not require a daemon to preserve the work.

### 6.3 Alternative approach

A user can create a branch from a useful point, try a different model, configuration, or implementation approach, and later return to either line of work. Creating an alternative must not require duplicating the repository.

### 6.4 Recovery and historical inspection

A user can inspect prior states and recover from branch movement such as reset while retained history remains available. Automation can pin an exact durable state when a moving branch name is insufficient.

### 6.5 Provider or model change

A user can continue a branch with another supported model or provider without treating the change as a new conversation merely because the provider's API grammar differs.

### 6.6 Delegated work

A user can delegate a bounded task. The parent receives the delegated result, and persistent delegation leaves a complete, inspectable agent branch rather than only a flattened final answer.

### 6.7 Context pressure

When a conversation approaches a model's context limit, fyai must expose the condition and provide a recovery path. Context reduction must preserve provenance to the prior durable history rather than silently making the previous conversation disappear.

### 6.8 Safe tool use

A user can understand and configure the boundaries under which model-requested file, patch, shell, question, and delegation operations execute. Platform capabilities may strengthen confinement, but absence or degradation of a safety boundary must not be silently represented as stronger protection than is actually active.

### 6.9 Durable transcript

A user can inspect durable conversational output after the live invocation ends without requiring provider wire data or transient terminal state to reconstruct what the conversation meant for a human reader.

## 7. Product requirements

The identifiers in this section are stable references for the SRD, focused design documents, tests, and release notes. Wording may be refined, but identifiers should not be casually recycled for unrelated requirements.

### 7.1 Invocation and lifecycle

**PRD-LIFE-001 - No resident service.** Normal fyai operation shall not require a daemon or resident fyai process between invocations.

**PRD-LIFE-002 - Durable continuation.** After a publishing invocation exits, a later invocation shall be able to continue from the published project-local agent state.

**PRD-LIFE-003 - One-shot operation.** A prompt supplied non-interactively shall be capable of running a complete bounded model/tool loop and returning a terminal result without entering an interactive session.

**PRD-LIFE-004 - Interactive parity.** Interactive operation shall use the same durable conversation and configuration model as non-interactive operation.

**PRD-LIFE-005 - Transient operation.** The user shall be able to run work against existing state without publishing resulting conversation or configuration changes when explicitly requesting transient behavior.

### 7.2 Command-line behavior

**PRD-CLI-001 - Unix-shaped interface.** fyai shall support normal command-line composition using arguments and standard streams, with diagnostics distinguishable from canonical response output.

**PRD-CLI-002 - Conventional completion.** Non-interactive operations shall communicate success or failure using conventional process exit status.

**PRD-CLI-003 - Inspection without inference.** State inspection and management operations that do not inherently require model inference shall not require a provider API call merely to inspect or manage local state.

### 7.3 Durable state and history

**PRD-STATE-001 - Local source of truth.** Project-local durable fyai state shall have an explicit local source of truth that does not depend on a provider-hosted conversation.

**PRD-STATE-002 - Canonical conversation.** Durable conversation identity shall be represented independently of any single provider API grammar.

**PRD-STATE-003 - Published immutability.** Published historical conversation state shall not be mutated in place in a way that changes the meaning of an already-addressable historical state.

**PRD-STATE-004 - Inspectable history.** Users shall be able to render and inspect durable conversation history independently of continuing the conversation.

**PRD-STATE-005 - Recoverable movement.** Operations that move a branch away from its current tip shall preserve a recovery path for the prior state for at least the configured retained-history window.

**PRD-STATE-006 - Exact-state inspection.** Automation shall be able to identify and read an exact retained arena state even when branch tips subsequently move.

### 7.4 Branching

**PRD-BRANCH-001 - First-class branches.** fyai shall support multiple named lines of agent work in one repository arena.

**PRD-BRANCH-002 - Conversation ownership.** Each branch shall own its conversation state.

**PRD-BRANCH-003 - Configuration ownership.** Each branch shall own the durable configuration needed to continue that line of work.

**PRD-BRANCH-004 - Cheap alternatives.** Creating an alternative line of work shall not require copying the project directory or duplicating all existing durable history.

**PRD-BRANCH-005 - Historical start points.** A branch shall be creatable from a retained historical state, not only from the current tip.

**PRD-BRANCH-006 - Joinable work.** fyai shall provide explicit operations for incorporating compatible conversation work from another branch while preserving the append-oriented nature of conversation history.

### 7.5 Providers and models

**PRD-PROVIDER-001 - Provider independence.** A branch's durable identity shall not be a provider-side session identifier or provider-native transcript.

**PRD-PROVIDER-002 - Supported transitions.** Users shall be able to continue canonical conversation history across supported model, provider, and API-grammar changes when the target combination can represent the required conversation semantics.

**PRD-PROVIDER-003 - Observable provider behavior.** Provider-specific observations needed for diagnostics, accounting, or faithful continuation may be retained without replacing the canonical product model.

**PRD-PROVIDER-004 - Explicit capability limits.** When a provider or model cannot support a requested product capability, fyai shall surface the limitation rather than silently pretending equivalent behavior.

### 7.6 Delegated agents

**PRD-AGENT-001 - Persistent delegation.** Persistent delegated work shall execute on a separately addressable agent line of work.

**PRD-AGENT-002 - Inspectable delegation.** The complete durable conversation of persistent delegated work shall remain inspectable after the parent receives the delegated report.

**PRD-AGENT-003 - Context choice.** Delegation shall support an explicit distinction between inheriting relevant parent context and beginning from a fresh delegated task where supported.

**PRD-AGENT-004 - Parent result.** A parent invocation shall receive a bounded result representing the delegated agent's outcome without requiring the entire delegated transcript to be flattened into the parent conversation.

**PRD-AGENT-005 - Isolation boundary.** A delegated agent shall not accidentally operate on live process state belonging exclusively to its parent or sibling work.

### 7.7 Tools and trust

**PRD-TOOL-001 - Bounded tool surface.** Model-initiated local actions shall be exposed through an explicit, reviewable tool surface rather than arbitrary hidden host capabilities.

**PRD-TOOL-002 - Configurable execution policy.** Users shall be able to configure meaningful boundaries on tool execution.

**PRD-TOOL-003 - Honest confinement.** fyai shall accurately represent whether platform sandboxing or other confinement is active and shall not imply guarantees that the current platform cannot enforce.

**PRD-TOOL-004 - Human questions.** A model or delegated agent shall have a defined mechanism to request user input when progress requires a human decision.

**PRD-TOOL-005 - Bounded returned content.** Tool output admitted to model context shall be bounded so that one tool result cannot unconditionally consume an entire model context window.

### 7.8 Credentials and secrets

**PRD-SECRET-001 - No raw durable project secrets.** Raw provider credentials shall not be stored as ordinary durable project configuration in the fyai arena.

**PRD-SECRET-002 - Explicit indirection.** Durable configuration may identify how a credential is resolved without embedding the credential value itself.

**PRD-SECRET-003 - Machine-local authentication.** Authentication material that must persist locally shall use an explicitly machine-local credential mechanism rather than becoming part of portable project conversation state.

**PRD-SECRET-004 - Child hygiene.** Credentials available to fyai shall not be unintentionally inherited by arbitrary programs launched as tools.

### 7.9 Context management

**PRD-CONTEXT-001 - Visible pressure.** fyai shall expose enough context information for a user to understand when a branch is approaching the selected model's usable context limit.

**PRD-CONTEXT-002 - Preflight bounds.** When model context limits are known, fyai shall avoid knowingly constructing a normal request that exceeds them without offering an explicit recovery path.

**PRD-CONTEXT-003 - Compaction provenance.** A compacted conversation shall retain provenance to the durable history from which it was compacted.

**PRD-CONTEXT-004 - Provider-appropriate compaction.** fyai may use provider-native compaction where available and a portable fallback elsewhere, provided the resulting behavior remains explicit and inspectable.

### 7.10 Terminal and transcript experience

**PRD-TERM-001 - Native interactive experience.** Interactive use shall provide a terminal-native experience suitable for sustained coding work rather than merely printing a sequence of remote chat responses.

**PRD-TERM-002 - Progressive visibility.** Long-running model, tool, and delegated operations shall expose useful progress without requiring the user to wait for an opaque final response.

**PRD-TERM-003 - Terminal correctness.** Programs run through terminal-backed tools shall receive terminal behavior sufficiently faithful for supported interactive command use, including relevant resize and control interactions.

**PRD-TERM-004 - Durable conversational display.** Human-facing live rendering may be richer than durable storage, but durable conversational output shall have an explicit inspectable representation distinct from canonical model-replay content, provider wire observations, and transient UI chrome.

### 7.11 Portability and native operation

**PRD-PLATFORM-001 - Linux-first native operation.** Linux is the primary platform and may receive the strongest integration and confinement features.

**PRD-PLATFORM-002 - Portable core semantics.** Where fyai supports additional Unix-like platforms, differences in event or confinement mechanisms shall not unnecessarily change the core conversation, branching, and invocation model.

**PRD-PLATFORM-003 - Native distribution.** fyai shall remain usable as native command-line software without requiring a language runtime or resident application framework as part of normal execution.

## 8. Safety and trust model

`fyai` operates in a developer workspace and may be authorized to read files, modify files, run commands, and contact model or MCP services. The product therefore treats authority as something that must be explicit and bounded rather than inferred from the intelligence of the model.

The trust model has four product-level boundaries:

1. **Model output is a request, not ambient authority.** Local effects happen through defined tools and policies.
2. **Credentials are not conversation data.** Provider and service secrets must remain outside ordinary durable project history.
3. **Delegation does not erase accountability.** Persistent delegated work remains inspectable and must obey applicable execution boundaries.
4. **Confinement claims match reality.** Platform sandboxing may provide stronger guarantees on some systems, but the UI and diagnostics must not describe unavailable protection as active.

This PRD does not mandate one sandbox implementation, process model, event system, allocator, renderer, or credential backend. Those are software and design decisions so long as the resulting behavior satisfies the product requirements.

## 9. Success criteria

The product is succeeding when a developer can reasonably treat fyai as both a command and a durable coding collaborator.

For the next product revision, the following are release-level success criteria:

- a repository can be initialized, used for one-shot and interactive work, exited completely, and continued later without a resident service;
- users can create and revisit alternative agent branches with independent configuration;
- users can inspect durable history and recover recently moved branch tips;
- supported provider/model changes do not unnecessarily fragment conversation identity;
- persistent delegated agents leave inspectable work and return useful bounded reports to their parent;
- delegated agents do not accidentally observe or control parent/sibling live execution state;
- context pressure is visible and recoverable rather than appearing only as a provider failure;
- credentials remain outside ordinary durable project state;
- durable human-facing conversation output remains inspectable independently of provider wire data and transient terminal state;
- tool and terminal behavior is reliable enough that normal coding tasks do not require understanding fyai's internal event or process architecture;
- documentation clearly separates product guarantees from software mechanisms.

These criteria are product outcomes. Detailed test matrices, performance thresholds, supported provider feature tables, and platform-specific implementation requirements belong in the SRD or focused verification plans.

## 10. Non-goals

The following are not product goals for this revision:

- providing a hosted fyai conversation service or cloud account as the durable source of truth;
- requiring a background daemon to make normal repository state usable;
- reproducing every provider-specific chat feature in a provider-neutral abstraction;
- making fyai branches synchronize or replace Git branches;
- hiding all provider capability differences behind claims of perfect portability;
- storing raw API keys or OAuth credentials in portable project state;
- making every delegated or standalone agent persistent when transient execution is explicitly desired;
- defining internal allocator layouts, pointer stability mechanisms, event-loop APIs, process inheritance mechanics, renderer internals, or provider wire translations as product requirements;
- guaranteeing identical sandbox strength on platforms that expose different confinement primitives;
- remote arena synchronization or portable conversation bundles in this revision.

## 11. Product and software requirement boundaries

The PRD specifies externally meaningful behavior and invariants. The SRD specifies how the software must realize and verify those requirements.

Examples:

| Product requirement | SRD/design concern |
| --- | --- |
| No resident service | invocation lifecycle and process architecture |
| Durable continuation | arena layout, publication, and recovery mechanics |
| Provider-independent history | canonical turn schema and provider translations |
| First-class branches | root/branch representation, refs, reflogs, CAS behavior |
| Inspectable delegation | agent branches, process/protocol model, child lifecycle |
| Safe tool use | tool schemas, policy evaluation, Landlock and platform behavior |
| Context recovery | token accounting, provider-native compaction, fallback summarization |
| Durable conversational display | display-output records, transcript replay, sink/rendering boundaries |
| Native terminal experience | event pump, PTY/vterm behavior, rendering and display semantics |

Future SRD revisions should cite PRD requirement identifiers where a software requirement exists primarily to satisfy a product requirement.

## 12. Roadmap and maturity

This document begins the transition from the Phase 1 implementation contract described by the 0.11 SRD toward a clearer product contract for 0.12.

The immediate documentation work is:

1. review requirements against current `devel` behavior and classify any requirement that remains aspirational;
2. maintain the 0.12 requirement scope ledger separately from implementation detail;
3. keep the SRD traced to relevant PRD identifiers;
4. move implementation rationale that does not define a software requirement into focused design documents;
5. mark focused design documents as historical where their current-state description has been superseded by implementation;
6. use the PRD and SRD together when deciding whether a behavior change is a bug fix, product change, or implementation refactor.

Likely later product questions include remote synchronization, portable exchange/bundle formats, broader provider reasoning portability, deeper Git-awareness, the durability boundary for delegated work, and the boundary between local persistent agents and remote or distributed execution. These should be decided as product capabilities before their implementation mechanisms become accidental commitments.

---

## Appendix A. Requirement index

- `PRD-LIFE-*`: invocation and lifecycle
- `PRD-CLI-*`: command-line behavior
- `PRD-STATE-*`: durable state and history
- `PRD-BRANCH-*`: branching
- `PRD-PROVIDER-*`: providers and models
- `PRD-AGENT-*`: delegated agents
- `PRD-TOOL-*`: tools and trust
- `PRD-SECRET-*`: credentials and secrets
- `PRD-CONTEXT-*`: context management
- `PRD-TERM-*`: terminal and transcript experience
- `PRD-PLATFORM-*`: portability and native operation
