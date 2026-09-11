# Foreign tool-call mapping

This document defines how Claude Code and Codex tool records become fyai
conversation items. The catalogue describes the foreign wire formats;
`data/tools.yaml` describes fyai's native tools. Import must use those two
sources rather than infer equivalence from a displayed label.

## Mapping rules

A foreign call has three distinct identities:

1. the source tool and its source arguments;
2. the canonical tool call stored in the imported conversation; and
3. the display operation produced by fyai's normal tool renderer.

For example, Codex `apply_patch` remains canonical `apply_patch`. A patch that
adds a file may display as `add file.c`; `add` is an operation label, not a
tool name. Codex `exec_command` is stored with that wire name and displays as
fyai's `shell` tool.

Mappings have these classes:

- **Exact**: preserve the tool identity and arguments.
- **Adapted**: rename fields or the tool without changing its operation.
- **Conditional**: map only when the recorded options have native semantics.
- **Preserved**: retain the source call and result, and add a provenance loss;
  do not present it as a native call.

Every mapped call keeps its source call ID. Its result becomes a
`function_call_output` with that ID. Unknown content must not break the call
and result pairing. Import is historical conversion: it never executes a
tool.

## Codex

Codex custom tools are recorded as JavaScript orchestration wrappers. Import
extracts the `tools.<name>(...)` invocation and accepts a name only when it is
present in the Codex catalogue. The wrapper itself is not conversation data.

| Codex tool | Fyai canonical tool | Class | Conversion |
|---|---|---|---|
| `exec_command` | `exec_command` | Adapted | `cmd` becomes `command`. Preserve compatible `workdir`, `login`, `shell`, `tty`, and `max_output_tokens`. Drop orchestration-only `yield_time_ms`, `justification`, `prefix_rule`, and `sandbox_permissions`. |
| `apply_patch` | `apply_patch` | Exact | Extract the free-form patch into `{patch}`. Fyai accepts the same Codex patch envelope. |
| `read_file` | `read_file` | Exact | Preserve canonical arguments. This occurs in older and replacement histories even when it is absent from the current catalogue. |
| `write_file` | `write_file` | Exact | Preserve canonical arguments. This occurs in older histories even when it is absent from the current catalogue. |
| `write_stdin` | source call | Preserved | Codex numeric `session_id` has no deterministic fyai shell-session name without reconstructing session state. |
| `list_mcp_resources` | source call | Preserved | Depends on tools connected to the importing invocation. |
| `list_mcp_resource_templates` | source call | Preserved | Depends on tools connected to the importing invocation. |
| `read_mcp_resource` | source call | Preserved | Depends on tools connected to the importing invocation. |
| `request_user_input` | source call | Preserved | Multi-question choices are not equivalent to fyai `ask_user`. |
| `update_plan` | source call | Preserved | No native persistent plan tool. |
| `get_goal` | source call | Preserved | No native goal-state equivalent. |
| `create_goal` | source call | Preserved | No native goal-state equivalent. |
| `update_goal` | source call | Preserved | No native goal-state equivalent. |
| `tool_search` | source call | Preserved | Runtime tool discovery is environment-dependent. |
| `web_search` | source call | Preserved | Provider/runtime facility, not a built-in fyai tool. |
| `image_generation` | source call | Preserved | Provider/runtime facility, not a built-in fyai tool. |
| `view_image` | source call | Preserved | No native image-view tool. |

An ordinary Codex `function_call` is already canonical provider data. A
`custom_tool_call` must be unwrapped first. Structured Codex result parts are
joined in order. Import removes orchestration status (`Script completed`, wall
time, and a standalone `exit=0`) while retaining command output. The empty
`{}` success result of `apply_patch` is stored as an empty result.

## Claude Code

| Claude Code tool | Fyai canonical tool | Class | Conversion |
|---|---|---|---|
| `Bash` | `exec_command` | Conditional | For foreground sandboxed calls, preserve `command`, `timeout`, and `description`. Calls with `run_in_background` or `dangerouslyDisableSandbox` are preserved. |
| `Read` | `read_file` | Conditional | `file_path` becomes `path`; preserve `offset` and `limit`. A `pages` request is preserved because `read_file` reads UTF-8 text, not PDF page ranges. |
| `Write` | `write_file` | Adapted | `file_path` becomes `path`; preserve `content`. |
| `Edit` | source call | Preserved | The exact old/new-string operation cannot be converted to a patch without reading mutable workspace state. |
| `Agent` | source call | Preserved | Agent type, isolation, resume, model, and background semantics are not equivalent to fyai `agent`. |
| `AskUserQuestion` | source call | Preserved | Multiple questions and structured choices are not equivalent to `ask_user`. |
| `WebFetch` | source call | Preserved | Runtime/network facility, not a built-in fyai tool. |
| `WebSearch` | source call | Preserved | Runtime/network facility, not a built-in fyai tool. |
| `NotebookEdit` | source call | Preserved | Fyai has no notebook-cell editing primitive. |
| `Skill` | source call | Preserved | Runtime skill loading has no native historical equivalent. |
| `Artifact` | source call | Preserved | No native artifact operation. |
| `DesignSync` | source call | Preserved | External integration. |
| `EnterPlanMode`, `ExitPlanMode` | source call | Preserved | UI/runtime state, not conversation state in fyai. |
| `EnterWorktree`, `ExitWorktree` | source call | Preserved | Workspace lifecycle has no native tool equivalent. |
| `CronCreate`, `CronDelete`, `CronList` | source call | Preserved | Fyai has no resident scheduler. |
| `TaskCreate`, `TaskGet`, `TaskList`, `TaskOutput`, `TaskStop`, `TaskUpdate` | source call | Preserved | Claude task state has no native fyai representation. |
| `Monitor`, `ScheduleWakeup` | source call | Preserved | No resident process or wakeup state is allowed. |
| `PushNotification`, `RemoteTrigger` | source call | Preserved | External runtime operations. |
| `SendMessage`, `ReportFindings` | source call | Preserved | Claude orchestration protocol, not a native tool call. |
| `EndConversation` | source call | Preserved | Conversation control marker, not a fyai tool. |

The catalogue is allowed to grow. A Claude or Codex tool absent from this
table follows the preserved rule until an explicit mapping and regression
test are added.

## Durable display

Mapped calls are rendered through `fyai_emit_tool_call()`, the same path used
for native calls. Import stores that Markdown and its `tool_head`, `tool_body`,
and `tool_result` fragments in `display_outputs`. Replay therefore uses native
shell and patch presentation rather than dumping arguments or provider JSON.

The call ID to canonical-name map controls result rendering. It must be reset
with replacement history at a compaction boundary. A user message finishes
the preceding assistant display document; tool results continue the assistant
document even when the source format carries them in a user-role envelope.

## Adherence checklist

- Validate wrapper tool names against the source catalogue.
- Keep call IDs stable and pair every available result.
- Never convert `apply_patch` to `write_file` or an invented `add` tool.
- Do not execute tools or inspect workspace files to improve a mapping.
- Preserve non-equivalent calls and record a `tool_mapping` provenance loss.
- Remove source wrapper noise only when its meaning is represented elsewhere.
- Generate imported display fragments with the native renderer.
- Add a fixture for every new exact, adapted, or conditional mapping.
