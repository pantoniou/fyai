# Tool gap analysis

This review compares the tool calls recorded for Codex, Claude Code, and
OpenCode with the native tool surface that fyai exposes to a model. It is a
planning document: it identifies user-visible capability gaps, separates them
from intentional differences, and assigns an implementation order. It does
not propose changing the stateless architecture or the security boundary.

Updated 2026-09-20. The repository was inspected at the current working tree;
no source, configuration, or test files were changed by the review.

## Executive conclusion

fyai already has a strong local coding loop: file reads and writes, Codex-style
patches, foreground and interactive shell sessions, structured user questions,
delegated agents, wait and time tools, MCP integration, streaming, parallel
calls, and durable branches. The earlier version of this document
under-counted that surface, especially the shell-session and agent-input tools.

The largest practical gaps are:

1. A first-class plan and task-state tool.
2. Native, policy-controlled web search and URL retrieval.
3. Richer user questions: multiple questions, headers, descriptions,
   multi-select, and cancellation semantics.
4. Agent lifecycle controls: resume/continue, background execution while the
   invocation remains alive, output retrieval, and explicit cancellation.
5. Workspace isolation through temporary worktrees.
6. Tool discovery and resource access for MCP-compatible environments.
7. Image inspection/generation and notebook or artifact-specific operations.

Recommended order: planning and interaction primitives, web and resource access,
then agent lifecycle and isolation. Specialized integrations come last because
they need explicit storage and process-lifetime decisions.

## Method and evidence

The primary local source is data/catalog.yaml. Its agents entries record the
tool descriptions and JSON Schemas captured for the comparison harnesses. The
current catalog contains:

| Harness | Recorded tools |
| --- | ---: |
| Claude Code | 32 |
| Codex | 15 |
| OpenCode | 11 |

The native fyai inventory comes from data/tools.yaml, the single source of
truth embedded into the build. It currently contains 12 tools:

read_file, write_file, apply_patch, exec_command, shell_input, shell_output,
shell_close, agent_input, ask_user, agent, time, and wait.

The comparison is semantic, not a name-count exercise. A tool is covered when
fyai can perform the same user-visible operation safely, even if the call name
or argument names differ. A tool is partial when the operation exists but
important behavior, state, or output semantics are missing. A tool is
preserved-only when importing a foreign transcript keeps the record but fyai
cannot execute an equivalent operation.

The external baseline is secondary to the vendored catalog. See the official
[OpenAI model guidance](https://developers.openai.com/api/docs/guides/latest-model),
[Codex CLI documentation](https://developers.openai.com/docs/codex/cli), and
[Claude Code CLI reference](https://docs.anthropic.com/en/docs/claude-code/cli-usage).

## Native fyai coverage

| Native tool | Main capability | Status |
| --- | --- | --- |
| read_file | Bounded UTF-8 reads with line or byte continuation | Covered |
| write_file | Complete UTF-8 file replacement | Covered |
| apply_patch | Unified diff and Codex patch envelope | Covered |
| exec_command | Foreground command, optional PTY, timeout, workdir, output bound | Covered |
| shell_input | Write input to a named live shell session | Covered |
| shell_output | Read a named live shell session with bounded output or screen views | Covered |
| shell_close | Close a named live shell session | Covered |
| agent_input | Send input to a live delegated agent | Covered |
| ask_user | One question with optional single-choice options | Partial |
| agent | Delegate with persona, context, timeout, and durable branch | Partial |
| time | Current time | Covered |
| wait | Interruptible foreground or named background wait | Covered |

The shell-session tools are important: the old analysis treated interactive shell
input as absent, but the current native schema exposes it explicitly. Likewise,
agent continuation input exists for live agents; what is missing is lifecycle
control after a tool call or after the owning invocation ends.

## Detailed comparison

### Codex

#### Covered or substantially covered

| Catalog tool | Fyai equivalent | Assessment |
| --- | --- | --- |
| exec_command | exec_command | Covered with adapted fields; fyai adds named PTY sessions. |
| write_stdin | shell_input, shell_output, shell_close | Covered with a named-session model; numeric IDs are not wire-compatible. |
| apply_patch | apply_patch | Exact or near-exact operation. |
| read_file | read_file | Covered; fyai adds byte continuation and configured limits. |
| write_file | write_file | Covered. |
| request_user_input | ask_user | Partial: no structured multi-question input. |
| list_mcp_resources | MCP subsystem | Partial: no native resource-listing model tools. |
| list_mcp_resource_templates | MCP subsystem | Partial for the same reason. |
| read_mcp_resource | MCP subsystem | Partial for the same reason. |

#### Missing or partial Codex capabilities

| Priority | Catalog tool | Gap | User impact |
| --- | --- | --- | --- |
| P1 | update_plan | No first-class transient plan with status and progress. | Complex work is harder to steer and audit. |
| P1 | request_user_input | No multi-question request, headers, option descriptions, multi-select, or structured answer. | Decisions become serial and harder to resume safely. |
| P1 | web_search | No native web-search tool. | Current facts require MCP or provider-specific behavior. |
| P1 | tool_search | No runtime discovery/activation mechanism. | Optional integrations must be loaded up front. |
| P2 | view_image | No native image inspection tool. | Visual debugging cannot be requested uniformly. |
| P2 | image_generation | No native image generation tool. | The model cannot create bitmap assets through the fyai loop. |
| P2 | get_goal, create_goal, update_goal | No goal object separate from a conversation branch. | Long-running objectives lack a machine-readable contract. |

Codex justification, sandbox_permissions, and prefix_rule are not automatic
requirements for fyai. They expose a different approval runtime. Fyai already
has configuration, Landlock confinement, command policy, diagnostics, and
user-controlled sandbox settings. Model-requested security escape would
contradict the repository rules, so this is an intentional difference.

### Claude Code

#### Covered or substantially covered

| Catalog tool | Fyai equivalent | Assessment |
| --- | --- | --- |
| Bash | exec_command | Covered for foreground commands; live PTY sessions are covered by the shell family. |
| Read | read_file | Covered for UTF-8 text; not equivalent for PDF page-oriented reads. |
| Write | write_file | Covered with path/content adaptation. |
| Edit | apply_patch | Partial; exact old-string/new-string editing is not represented. |
| AskUserQuestion | ask_user | Partial; no structured question array or multi-select. |
| Agent | agent plus agent_input | Partial; personas, fork/fresh context, timeouts, branches, live input, and concurrency exist, but lifecycle/isolation parity does not. |
| MCP-backed tools | MCP subsystem | Covered for configured Streamable HTTP and stdio servers. |

#### Missing or partial Claude Code capabilities

| Priority | Catalog tool or family | Gap | User impact |
| --- | --- | --- | --- |
| P1 | WebFetch, WebSearch | No native web retrieval/search pair. | Documentation and current information require MCP or shell workarounds. |
| P1 | EnterPlanMode, ExitPlanMode | No native plan-mode state. | Model and UI cannot formally switch between planning and execution. |
| P1 | TaskCreate, TaskGet, TaskList, TaskUpdate | No structured task list. | Multi-step work lacks shared progress and explicit completion. |
| P2 | SendMessage | No durable resume-by-agent identity after a completed call. | Follow-up work must be restated or run as a new delegation. |
| P2 | TaskOutput, TaskStop, Monitor | No background-agent output retrieval, stop, or condition monitoring. | Parallel work cannot be supervised with the same granularity. |
| P2 | Agent background/remote execution | No background/remote agent mode and remote workspace. | Expensive or isolated jobs block the main loop or need an external system. |
| P2 | EnterWorktree, ExitWorktree | No temporary filesystem worktree isolation. | Parallel agents share the workspace. |
| P2 | NotebookEdit | No notebook-cell-aware edit primitive. | Notebooks are treated as ordinary files. |
| P2 | Artifact | No artifact-specific creation, preview, or handoff lifecycle. | Generated artifacts have no standard model-facing contract. |
| P3 | Skill | No dynamic model-visible skill loader. | Optional procedures are configured statically or externally. |
| P3 | DesignSync | No design-system or design-file integration. | Design-to-code needs MCP or external tooling. |
| P3 | CronCreate, CronDelete, CronList, ScheduleWakeup | No persistent scheduler. | Later-invocation work conflicts with the no-daemon rule. |
| P3 | PushNotification, RemoteTrigger | No external notification/trigger integration. | Work cannot notify or be started by an external control plane. |
| P3 | EndConversation, ReportFindings | No Claude-specific termination/findings protocol. | Mostly orchestration interoperability; low direct coding value. |

Claude permission modes and allowed/disallowed controls are policy comparisons,
not a reason to add model-controlled security escalation. Fyai's sandbox and
approval model should remain owned by the user and configuration.

### OpenCode as a secondary reference

OpenCode is also cataloged and is useful because it exposes specialized search
tools instead of asking the shell to do all searching.

| Catalog tool | Fyai equivalent | Assessment |
| --- | --- | --- |
| question | ask_user | Partial; same interaction gap. |
| bash | exec_command | Covered. |
| read, write, edit | read_file, write_file, apply_patch | Covered or partial by edit semantics. |
| glob, grep | shell or MCP | Partial; no dedicated bounded search tools. |
| task | agent | Partial; lifecycle and isolation differ. |
| webfetch | none | Missing native web retrieval. |
| todowrite | none | Missing structured task list. |
| skill | none | Missing native dynamic skill loading. |

Dedicated glob and grep are not urgent because Unix search is available and the
repository deliberately keeps tools Unix-shaped. Add them only if measurements
show shell search is a reliability or latency bottleneck.

## Cross-cutting gaps

### Planning and task state

This is the highest-leverage missing abstraction. Codex has update_plan and
goal tools; Claude Code and OpenCode have task-list tools and plan markers.
Fyai has durable branches and rich progress UI, but no model-facing object that
says what is pending, active, blocked, or complete.

Recommended shape:

- Keep an invocation-local plan separate from canonical conversation state.
- Represent ordered tasks with stable IDs, status, priority, and concise text.
- Publish changes through the sink and UI, not direct output.
- Allow a final snapshot to attach to the turn, but do not put it in
  conversation identity unless explicitly requested.
- Import foreign plan/task calls as preserved provenance until equivalent state
  exists.

This respects the stateless architecture while delivering most plan/todo value.
A durable cross-invocation goal object should be a separate design.

### Web retrieval

Web search and URL fetch are common across the comparison set, but differ in
trust and reproducibility from shell and file tools. Define network policy and
allowed origins; timeout, response-size, MIME, redirect, and security behavior;
citation-bearing canonical results; provider-stream versus canonical-content
boundaries; and transcript replay.

Do not silently route web requests through the shell: that loses structured
citations and makes policy enforcement opaque.

### Structured user interaction

ask_user should grow to support a bounded queue with one active question,
stable option IDs, single- or multi-select, free-text fallback, cancellation,
and an answer object retaining the question ID. The existing UI queue and
agent-origin metadata make this a protocol/schema expansion, not a new
event-loop concept.

### Agent lifecycle

Fyai already has personas, fork/fresh context, live input, branch provenance,
concurrency limits, child routing, and cancellation paths. The remaining gap is
a consistent lifecycle API: admit/start, inspect status, stream or retrieve
bounded output, send follow-up input, cancel/stop, join/collect, and optionally
resume from the durable agent branch.

Background execution is safe only while the owning invocation lives. A job a
later process must find requires a new durable-state decision and must not be
smuggled in as a tool feature.

### Workspace isolation

Claude worktree isolation is a safety and concurrency feature. Fyai branches
protect conversation and configuration state, but not filesystem workspaces.
A future implementation needs creation, ownership, cleanup, path reporting,
failure recovery, and interaction with Landlock and --root. Design it after
agent lifecycle control because isolation is an agent launch policy.

## Recommended priority order

Priority reflects user value, breadth across tools, architectural fit, and
prerequisites.

| Order | Work item | Priority | Rationale and guardrails |
| ---: | --- | --- | --- |
| 0 | Keep catalog and native schema mechanically comparable | P0 | The old analysis drifted because the two schemas evolve independently. Add a read-only comparison test/report; catalog data must not become runtime tool definitions. |
| 1 | Structured plan and task tool | P0 | Shared by Codex, Claude Code, and OpenCode. Invocation-local first; render through sink; no daemon or hidden state. |
| 2 | Rich ask_user protocol | P0 | Small implementation with immediate UX and correctness gains. One active question, bounded queue, stable IDs, cancellation. |
| 3 | Native web search and web fetch | P1 | Broadest capability gap. Add explicit network policy, citations, size limits, and replay semantics. |
| 4 | MCP resource listing and reading | P1 | Completes the existing MCP surface and closes three Codex gaps. Keep resources distinct from tool results. |
| 5 | Agent lifecycle controls | P1 | Turns existing machinery into continue/output/stop equivalents. Invocation-local background first; branch resume needs a state contract. |
| 6 | Agent worktree isolation | P1 | Safer parallel edits and closer Claude parity. Define cleanup and Landlock behavior first. |
| 7 | Goal objects and durable task continuation | P2 | Useful for longer work, but crosses from turn-local planning into persistent state. Extend canonical schema deliberately; no sidecars. |
| 8 | Image inspection and generation | P2 | Valuable for UI/assets, not core text coding. Define binary artifact storage and rendering. |
| 9 | Notebook-aware editing and artifact lifecycle | P2 | Specialized workflow. Prefer structured formats and explicit ownership. |
| 10 | Dedicated glob/grep tools | P2 | Add only if measurements show shell search is a bottleneck. |
| 11 | Dynamic skill loading and design integrations | P3 | Better served by plugins or MCP; keep display configuration separate from model content. |
| 12 | Scheduler, wakeup, notification, and remote triggers | P3 / defer | Require external lifetimes and coordination; conflict with no-daemon unless a controller owns state. |

## Explicit non-goals and deliberate differences

Do not implement these merely to match a catalog name:

- Model-controlled sandbox escape or arbitrary approval escalation.
- A resident daemon or hidden process surviving normal invocation exit.
- A sidecar task database outside the content-addressed arena.
- Numeric object references for durable work, because arena objects may move.
- Background jobs discoverable by a later invocation without a durable-state design.
- A second transcript renderer for web, agent, or artifact output.
- Raw API keys or credentials in tool arguments or arena state.

Codex justification, prefix_rule, and sandbox_permissions, Claude's dangerous
permission bypass, and Claude's cron/remote-trigger family expose runtime
policies or external lifetimes not automatically portable to fyai.

## Import and interoperability implications

Keep foreign-tool mapping loss-aware:

1. Add exact or adapted mappings only when semantics are equivalent.
2. Preserve non-equivalent calls with source name, arguments, result, and a
   provenance-loss marker.
3. Never execute imported calls.
4. Add fixtures for successful, partial, and preserved-only calls.
5. Keep call IDs stable so results remain paired with calls.

After implementation, prioritize mappings for update_plan, task-list calls,
request_user_input, MCP resources, web fetch/search, and agent lifecycle.
Image, notebook, and artifact operations should remain preserved-only until
storage and replay contracts exist.

## Validation plan

Each priority item should ship with:

- a schema test for the model-facing tool definition;
- a unit test for state and cancellation;
- a sink-only or transcript-replay test;
- a provider test for every supported grammar that can carry the call;
- a foreign-import fixture where the catalog records the capability;
- a read-only comparison report showing new coverage and remaining gaps.

Regenerate the comparison report from data/catalog.yaml and data/tools.yaml so a
future catalog update cannot silently make this document stale again.

