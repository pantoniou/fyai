# CLAUDE.md

Use this file when you change this repository.

## Project

`fyai` is a stateless AI coding assistant written in C. It does not use a
daemon. One invocation runs one complete tool-use loop. It commits the
canonical state and then exits. An interactive invocation owns its terminal UI
only while its process runs.

### Architecture rules

- Do not add a daemon, resident process, or hidden process state.
- Store persistent state only in content-addressed libfyaml arenas under
  `~/.fyai`.
- Keep canonical data immutable, deterministic, and address-stable between
  processes.
- Do not relocate an arena during normal operation.
- Keep tools compatible with Unix conventions. Tools read files, apply writes
  or patches, and run approved shell commands.

## Data model

Use libfyaml generics as the native data model. Build values with functions
such as `fy_gb_mapping()` and `fy_gb_sequence()`. Do not construct JSON by
hand. Emit compact JSON only at provider boundaries with `FYOPEF_MODE_JSON`.
Parse provider responses directly into generics.

Keep provider wire data in provider-stream generics. This data includes request
IDs, tool-call IDs, finish reasons, and timestamps. Derive provider-independent
content before you calculate the canonical identity.

Use typed accessor defaults:

```c
fy_get(obj, "content", "")
fy_get(obj, "total_tokens", 0LL)
fy_get(obj, "items", fy_invalid)
```

### Generic string lifetime

Prefer `fy_castp()` to `fy_cast()`. A short string can be in the `fy_generic`
word. Thus, a pointer from `fy_cast(v, "")` can point into the local copy of
`v`. The pointer becomes invalid when the copy leaves scope. Use
`fy_castp(&v, "")` at the use site. Make sure that the generic has a sufficient
lifetime. Do not keep a cast pointer longer than its generic.

A `const char *` loop variable from `fy_foreach()`,
`fy_foreach_key_value()`, or `fy_foreach_idx_item()` is safe. The typed
accessor uses the address of the stored item. Thus, the pointer refers to
collection storage and not to a copy.

### Short forms

Use the short generic API. It says the same thing with less text:

- `fy_is_string()`, `fy_is_mapping()`, `fy_is_valid()` and the other
  `fy_is_*()` predicates, and `fy_empty()` for an empty collection or string.
  Keep `fy_generic_is_int()`, `fy_generic_is_float()` and
  `fy_generic_is_bool()`, which also test the C value range.
- `fy_foreach()`, `fy_foreach_key_value()` and `fy_foreach_idx_item()` in
  place of an index loop with `fy_get_at()` and `fy_get_key_at()`.
- `fy_any_equal(v, "a", "b")` in place of a chain of `fy_equal()` tests. It
  evaluates the value one time.
- `fy_str(v)` for a printable string and `fy_number(v, dflt)` for a number.
  `fy_str()` returns NULL for `fy_invalid`.
- `fy_stringf(gb, ...)` and `fy_join(gb, ...)` to build a string generic in a
  builder arena. To get a stable `const char *`, keep
  `fy_gb_intern_string(gb, fy_sprintfa(...))`; there is no interning format
  function.
- `fy_str_empty(s)` for a NULL or empty C string.

### Empty strings

An empty-string generic is a string. It is not null. If the style is not set,
the YAML emitter must write an empty string as `""`. The core and YAML 1.1
schemas can parse a bare empty scalar, such as `key:`, as null.

Keep empty-string configuration keys as `type: string` in
`data/config.schema.yaml`. Do not allow null to hide an emitter defect.

### Generic initialization

A zeroed `fy_generic` is not `fy_invalid`. Zero can represent an empty
sequence. `fyai_setup()` clears `struct fyai_ctx`. After this operation,
initialize each generic field explicitly.

## Source layout

- `src/main.c`: global option parsing and command dispatch.
- `src/commands.c`: verb definitions, usage output, and the main runner.
- `src/fyai.c`: engine orchestration.
- `src/fyai_sink.c`: the one rendering component and its backends.
- `src/fyai_output.c`: transcript document source and fragments.
- `src/fyai_session.c`: interactive input and slash commands.
- `src/fyai_agent.c`: sub-agent execution and agent RPC.
- `src/fyai_event*.c`: the portable event loop and signal handling.
- `src/fyai_event_dump.c`: SIGUSR2 event-loop diagnostics.
- `src/fyai_render.c`: generic-to-Markdown table rendering.
- `src/fyai_diag.c`: collected diagnostics.
- `src/utils.c`: HTTP buffers, shell capture, and generic serialization.
- `src/fyai_sandbox.c`: Linux Landlock confinement.
- `src/fyai_oauth.c`: provider-independent OAuth browser flow.
- `src/fyai_jsonrpc.c`: JSON-RPC over standard I/O and HTTP.
- `src/fyai_config.c`: layered configuration and config commands.
- `src/fyai_branch.c`: branch storage, references, and root selection.
- `src/fyai_merge.c`: branch merge and rebase.
- `src/fyai_catalog.c`: provider and model catalogue.
- `src/*.h`: shared structures and internal interfaces.
- `data/`: embedded configuration schemas and catalogue data.
- `tests/`: unit tests, functional cases, mock providers, and scenarios.

## Persistent state and branches

The durable root has this versioned shape:

```text
{fyai: 2, catalog, HEAD, branches}
```

Each branch entry owns its conversation head and configuration. The root owns
the catalogue. Do not use a sidecar configuration file. Do not use a
root-level configuration value.

Three operations adopt a published root: publish, reconciliation after a lost
CAS, and `fyai_branches_refresh()`. Each operation must set
`ctx->branch_prev` from the adopted root. Do not keep an entry from an older
root. Such an entry links the next publish to the wrong predecessor. It also
makes the conflict check compare entries from different roots.

If a publish stops, report what it could not write and why. Name the branch.
State whether the operation lost the race or could not build the state. Do not
report only that the publish failed. That report cannot distinguish a lost
race from an arena that has insufficient space.

When you add a root key, update all applicable paths. These paths build,
decode, validate, truncate the reflog, and merge CAS conflicts. A concurrent
publish must preserve changes to other branches. During garbage collection,
limit the root `prev` chain and each branch-entry `prev` chain.

### Branches and references

- A branch name defines the hierarchy. Do not add a separate branch tree.
- Accept only symbolic references: `<branch>`, `<branch>~N`, `<branch>^^`, and
  `<branch>@{N}`.
- Never add numeric object references. Garbage collection can relocate arena
  objects.
- `~N` counts stored turns. One user and assistant exchange normally counts as
  two turns.
- `--branch`, `-b`, and `$FYAI_BRANCH` select a branch for one invocation.
- Only `checkout` changes stored `HEAD`.
- Keep `ctx->branch` separate from `ctx->head_branch`.
- Store the operation that produced each reflog entry. Do not infer it from
  head changes.
- Set the operation with `fyai_branch_op_set()`. A publish consumes and clears
  it.

A branch start point includes the conversation and the configuration at that
point. Use `fyai_resolve_ref_state()` for `branch create` and `checkout -b`.
Do not combine a conversation from one state with a configuration from a
different state.

`--root` selects one published root and makes the invocation read-only. Reject
writes. Do not convert them to transient writes. Treat the supplied handle as
untrusted input. Walk the root reflog and compare raw values to find the
handle. Do a low-cost shape check for each entry. Then do one full validation
of the matching entry. Keep full root validation shallow. Code that walks a
reflog must validate each next link with `fyai_branch_entry_contained()`.

### Merge and rebase

A stored `previous` link is data. Walk a conversation chain with
`fyai_turn_foreach()`, which is bounded and detects a cycle; a corrupt chain
must end a walk, not hang it. Validate a root before the peek path follows any
reference in it.

Conversations are append-only. A join selects an order. It does not reconcile
text edits. Work in complete exchanges. Keep each question with its answer.
`rebase` puts our exchanges after their exchanges. `merge` orders exchanges by
their reflog time.

Read an exchange time from the reflog entry for the head that ends the
exchange. Keep timestamps at microsecond precision. Keep one system prompt.
Use stored generic identity to find the merge base.

After a lost publish CAS, retry if only other branches changed. Apply
`branch/on_conflict` if the current branch changed.

## Configuration and providers

Build one merged configuration document in this order:

1. the active branch configuration;
2. the explicit `--config` file; and
3. command-line `--set` changes.

Use the user configuration file only to initialize an arena that has no stored
configuration. Do not overlay it on an existing arena configuration.

Treat `cfg->config_doc` as the source of configuration intent. Populate the
structure fields in one `apply_config` pass. During model resolution, derive
the endpoint, provider, and catalogue `max_tokens`. Do not persist these
derived values. When the model changes, derive and persist the API grammar and
URL for the new provider.

Keep the informational `catalog` block synchronized on every configuration
commit. Remove it when the selected model is not in the catalogue.

Validate at every ingestion point. `--set` and an explicit `--config` file are
both checked against the schema before the value reaches the merged document.
Put the schema problems in the diagnostic that reports the failure; do not
write them separately.

Configuration paths use slash-separated keys. Parse values as YAML flow
documents. `--transient` places an in-memory builder above the durable arena
and skips reference publication.

Never store a raw API key. The `api_key` setting must have this form:

```yaml
api_key: {type: env, value: ENVIRONMENT_VARIABLE}
```

Reject raw keys at every arena ingestion point.

### Transient failures

Retry a provider request after a transient failure and a backoff.
`fyai_http_transient()` makes the retry decision. Retry a refused or lost
connection, a timeout, and HTTP 408, 429, 500, 502, 503, or 504. Do not retry
another HTTP status. It is the provider response.

An HTTP status does not identify every transient failure. On a streamed
endpoint, the connection can succeed with status 200. A rate-limit error can
then arrive in the stream. For this case, `fyai_provider_error_transient()`
reads the `code` and `type` of the error object. Do not classify a spent quota
as transient. The quota does not refill during a backoff.

Keep the text of a transient stream failure. Raise it only after the last
attempt fails. Do not raise it if the turn recovers. If the turn does not
recover, report the original text.

If an event handler stops a stream, curl reports a write error. Process the
parsed event before the transport error. The transport error identifies only
how the stream stopped.

Double the delay after each attempt. Do not exceed `retry/max_delay_ms`. Add a
random part so that requests do not retry at the same time. A `Retry-After`
header replaces the calculated delay. Apply the same maximum delay. Do not use
`random()` for the jitter. It has no seed, so each process gets the same
sequence.

A delegated sub-agent sends its wait through `fyai_tool_progress_emit()`. The
wait then reaches the parent work band for that delegation. Do not send a
status line from the child. It would appear directly on the parent terminal
and outside the display.

Present a retry as a work band through the sink. Do not use a separate status
line. A wait is a step of the active turn. Repaint the band for each attempt.
Close it one time at the common notification point. A backend without bands
uses the plain status line.

Retry a streamed request only if it presented no content. After a mid-stream
transport error, keep the text that the user already saw. Do not retry because
a retry would show the text again.

A request in a backoff has no transfer to cancel. Cancellation and destruction
must remove the timer and complete the request. Otherwise, an interrupt cannot
act on the request.

### Provider grammars

The supported grammars are Responses, Chat Completions, and Anthropic
Messages. The `api` configuration key selects the grammar. Do not add separate
grammar flags.

Normalize inbound provider items at the parse boundary. Store one canonical
shape. Adapt that shape in each request builder so that a conversation can
continue on another provider.

Read endpoint capabilities from the catalogue. Do not infer them from a
provider name. `fyai_config_resolve_model()` derives
`cfg->shell_tool_supported` for the active endpoint. An endpoint with no
capability declaration does not support native shell calls.

Use `fyai_provider_native_shell()` as the single decision for native shell
support. If native shell is unavailable, declare the function shell tool and
rewrite stored native shell items to function-call items in the request
builder.

## Output and terminal UI

`src/fyai_sink.c` is the only rendering component. Send all user-visible bytes
through it. A producer builds Markdown source and gives it to the sink. The
sink controls the presentation. Do not write to standard output or standard
error from another component. `tests/sink-only.sh` fails the build if it finds
such a write. `tests/sink-only-allow.txt` lists the files that cannot use the
sink and gives the reason for each file.

### Streams

A stream says what the content is, not where it goes:

- `FYAI_SINK_TRANSCRIPT`: conversation content.
- `FYAI_SINK_NOTICE`: verb and slash command results, and tables.
- `FYAI_SINK_STATUS`: banner, spinner, usage, tool echo. Commentary.
- `FYAI_SINK_DIAG`: drained diagnostics.
- `FYAI_SINK_MACHINE`: bytes another program parses. Never rendered or
  decorated.

Use `fyai_result()` for a verb result, `fyai_report()` for a status line,
`fyai_sink_markdown()` to render, and `fyai_sink_write()` for bytes that are
already in their final form. `fyai_result()` and `fyai_report()` write plain
text: a model name or a path inside a status line must not be read as markup.

### Backends

A backend supplies the presentation policy for `struct fyai_sink_ops`. The
terminal backend repaints in place. It owns the only progressive Markdown
renderer in the process. The capture backend keeps the requested content. A
document backend, such as HTML, uses the capture backend. Tests also use it to
read the output of a run. Each entry point can be NULL. If a backend cannot
present content, the sink discards the content. A producer does not examine the
destination.

### Documents

`src/fyai_output.c` owns the durable part of a transcript document. This part
contains the Markdown source and its fragments. Keep one tagged document open
for each system, user, or assistant output. Keep an assistant document open
during the complete model and tool loop. Store the final document as
`display_outputs`. Replay these documents for history. Reconstruct a legacy
arena from message and provider data only as a fallback.

- Add generated text with `fyai_output_printf()`.
- Add provider bytes with `fyai_output_append()`.
- Use `fyai_output_append_recorded()` to add source that another path already
  drew. Use it for a tool exchange. The tool path presents the exchange. Do not
  present it again from the document.
- A tool exchange stores a `tool_head` fragment over its title row, with the
  outcome of the call. Replay draws the state mark and the failure cause from
  that fragment through `markdown_render_tool_head()`, the one renderer the
  live work band also uses. Do not draw a tool title row anywhere else.
- Set the presentation mode when the document opens. The mode is live,
  one-shot, or passthrough. Do not infer the mode later from the current state.
  If you do, a paused document can present all content again when it closes.
- A delegated sub-agent and a forked tool child do not present content. They
  use file descriptor 1 for JSON-RPC frames. `sink_may_present()` enforces this
  rule. The document, direct-write, and Markdown paths use this function. The
  sink drops a stream for descriptor 1 in these processes. Do not move the
  stream to standard error. That change can corrupt the parent display.
- Do not create a second transcript renderer.
- Do not call `fytim_pump()` from a render path. Set `frame_pending` and let
  the UI owner paint.

### Work bands

A work band is a sink object. Ask `fyai_sink_bands_available()` before you
choose a banded presentation, open with `fyai_sink_band_open()`, repaint with
`fyai_sink_band_paint()`, and commit the shared band with
`fyai_sink_band_close()`. Do not call `fyai_ui_*` band functions from a
producer; the terminal backend owns that.

### Tables

Render every Markdown table with `fyai_generic_to_markdown()`. Pass a
`renderopts` generic for titles, selected keys, names, alignment, and formats.
Build `renderopts` in the transient builder or in the same stack frame as the
render call. Do not return a builder-less `fy_mapping()` from a helper because
it uses stack storage. `stats` keeps its own table builder: its cells carry
currency and percent formatting that `renderopts` does not express.

Export is deliberately outside the sink. `fyai_export_view()` serializes
canonical messages to a `FILE *` in the `format: 1` textual grammar that
`src/fyai_import.c` reads back, and it must keep working when a conversation
has no stored `display_outputs`.

### Interactive rules

The interactive reader uses the shared event loop. Test terminal input,
signals, and resize behavior under a PTY. Non-TTY functional tests use the
blocking fallback and do not cover the event-driven path.

In the REPL, `/` starts a slash command and `//` sends a literal slash. Ctrl-C
discards nonempty input, ends idle input, or cancels an active turn. Escape and
SIGINT call `fyai_ui_interrupt()`. Keep `ISIG` enabled.

Request-shaping slash settings persist through the common commit path. Display
settings remain session-only. `--new` has the same state effect as `/clear`.
`/compact` makes one tools-disabled summary request and stores the previous
head as `compacted_from`.

## Event loop and process rules

Each invocation owns one lazy `struct fyai_ctx` event loop. Curl, shell
capture, tool jobs, MCP shutdown, readline, and OAuth borrow that loop.

- Each borrower must remove all sources that it registered.
- Store source pointers in the owner and clear them when callbacks retire a
  source. Make cleanup idempotent.
- A child that forks without `exec` must call `fyai_ctx_loop_abandon()`
  and `fyai_ctx_fork_disown()`.
- Abandonment closes the child's descriptor copies. It must not call
  `epoll_ctl()` on the shared epoll object.
- Do not run a nested event loop from an interactive callback.
- Use synchronous wrappers only in standalone commands, setup adapters, and
  isolated tests that own the top-level loop.

Sanitizer builds disable event-loop object recycling by default. Test both
paths. Set `FYAI_EVENT_NO_POOL=1` to disable pooling in a normal build. Set it
to `0` to enable pooling under ASAN.

### Signals

Do not register SIGINT with `signalfd`. Use
`fyai_event_interrupt_open()`. The handler sets the pending flag, wakes the
loop, and starts the watchdog. A pump that sees the interrupt must call
`fyai_event_interrupt_ack()`. Reserve SIGALRM for the watchdog.
`fyai_event_interrupt_close()` removes the handlers during context cleanup,
because they name the context.

The diagnostic dump is the one handler that does more than record and wake. It
must report a loop that is stuck, which is the state a deferred dump could
never reach, so it formats and writes from signal context. Keep it able to do
that: no allocation, no stdio, no `tcgetattr()` - the terminal mode is probed
when the dump is armed - and no blocking write. It refuses to start a second
dump while one is running. Its walk of the loop lists is unsynchronized by
design and stays bounded.

Keep SIGPIPE blocked in the parent. Restore the disposition in children before
`exec`. Set `CURLOPT_NOSIGNAL` on curl handles. Keep signal handlers
async-signal-safe.

An interrupted turn keeps its completed steps. Wrap the partial turn with the
diagnostic indirect through `fyai_with_diag()` and `fyai_report_diag()`.

## Tool jobs, shell capture, and time limits

A forked tool child uses descriptors 3 and 4 for its JSON-RPC control channel.
It does not use standard input and output for this channel. The child is forked
and does not call `exec`, so these descriptor numbers are a private contract.
A stray write to descriptor 1 goes to the terminal and not into a JSON-RPC
frame. Thus, it cannot make the parent read an invalid frame. The control
channel is close-on-exec. A shell command cannot access it.

Do not change the standard descriptors of an MCP server or the
`fyai agent --rpc` verb. These peers are separate programs that use standard
input and output. The sink keeps descriptor 1 clean for these programs.

Close each descriptor above standard error before `exec`. A command must not
hold the event loop, curl sockets, arena files, a control channel, or pipes from
a sibling job. Close the descriptor range with `fyai_close_fds_from()`. Do not
track individual descriptors. This prevents a new descriptor from becoming a
leak.

Every child that runs another program removes the credentials first with
`fyai_env_sanitize()`. Do this in the child, and fail closed: a partial
sanitize still gives the program a provider key. A configured `env` mapping is
applied after it, because `setenv()` adds and replaces but never removes. An
MCP stdio child follows the same rules and leads its own process group, so
teardown can stop its descendants with it. The child
sends `tool/progress` notifications and returns `{result, ok}`. It uses
`/dev/null` as standard input and calls `setsid()`. Do not call `setpgid()` in
the parent.

A forked child keeps what describes the work: the configuration, the arena and
its builders, the sink, and the credentials. It keeps nothing that names a
process, a descriptor, a timer, or a screen of the parent. A copy of such state
addresses another process by mistake, or answers with what belongs to it: an
inherited session took the name a sub-agent asked for and returned what the
program of the parent wrote, and an inherited wait took its name. Drop every
one of them in `fyai_ctx_fork_disown()`, which ends nothing, and add a new kind
of live context state to that list.

Set job fields after `fyai_tool_job_spawn()` because that function clears the
job. Cancel a complete process tree with
`fyai_event_add_child_terminate_group()`.

Finish shell capture when the direct child is reaped. Do not wait for pipe EOF.
A shell descendant can keep a pipe open. Keep the shell child in the process
group of the tool job.

Arm time limits in the parent with `fyai_tool_job_submit()`. A time limit
applies to the complete job and not to one child command. Remove the deadline
when the job finishes. Do not wait until collection. A group is collected only
after all its jobs finish. Thus, a completed job can wait for a slower sibling.
An active timer could incorrectly mark that job as timed out. Do not arm a
second limit in the child when `cfg->tool_child` is set.

`timeout_ms` is the user default. `max_timeout_ms` limits a value supplied by
the model. Apply the ceiling only to a model-supplied value. Use this order:

- Shell: call `timeout`, then `shell/timeout_ms`.
- Agent: call `timeout`, persona `timeout_ms`, then `agent/timeout_ms`.

The function shell tool uses argument `timeout`. A native Responses
`shell_call` uses `action.timeout_ms`. Pass the correct object to
`fyai_shell_timeout_requested()`.

Only the parent reports expiration. Preserve the normal result shape. A native
shell result is a list of `{stdout, stderr, outcome}`. On expiration, set the
outcome to `{type: timeout, timeout_ms}`. Keep all captured output. Do not
replace the result with an error string or a signal outcome.

A program can send a query to its terminal. It asks for the terminal type or
for the cursor position, and it waits for the reply. The terminal view is that
terminal, thus the view makes the reply. Set a reply callback on each view
with `fyai_terminal_view_reply_cb()`. Write the reply to the program with
`fyai_terminal_reply_write()` when this process holds the terminal descriptor,
or with the `shell/write` notification when the tool child holds it. Without a
reply the program waits for its own time limit, and it then reads the next
typed input as the reply.

Apply shell `workdir` before Landlock confinement. Exit status 125 reports an
unreachable working directory. Landlock is Linux-only. Other platforms use
the same interface with no confinement backend. Keep command admission
separate from filesystem confinement.

A configured deny is carved out of every granted hierarchy, the broad scratch
and system trees included. Landlock has no non-recursive rule, so a directory
above a denied path cannot be granted whole: it is descended, each undenied
child is granted, and the directory itself keeps only the right to list its
entries. That cost is why the implicit arena deny stays scoped to the project
root, and why a deny path that does not exist is left out.

Fail closed on network egress. The kernel ABI is not the only condition: a
build made against headers without the network access definitions restricts
nothing on any kernel. Ask `fyai_sandbox_net_restrictable()`.

## Sub-agents

`fyai_agent_run()` takes the full call arguments: `task`, `name`, and
`description`. A sub-agent owns its persona, tools, conversation, curl handle,
and output routing.

- Store each sub-agent conversation on a child branch such as
  `main/agent:greeter`.
- Keep `fyai_branch_name_valid()` strict for user-created names.
- Let `fyai_branch_name_ref_valid()` accept the reserved `agent:` marker.
- Reject a duplicate agent name. Do not add an ordinal automatically.
- Refresh the branch table before you test a name.
- Reserve the name in the parent before `fyai_tool_job_spawn()`.
- Copy the name into the job after spawn.
- Set child persona fields after `fyai_arena_reopen()`.
- Reopen the arena in a forked child before it publishes.

`context: fork` starts at the parent head. `context: fresh` sends only the
task. In fork mode, add the persona as a user instruction message. Do not add a
second system turn.

Expose only configured persona names and descriptions in the tool schema.
Resolve a persona model through the catalogue in a scratch configuration.
Preserve user-set `api_url` and `max_tokens`; replace only derived values.

A delegated sub-agent has a terminal of its own. It renders to that terminal
as this program renders to any terminal. The parent interprets that terminal
and shows it on a surface, behind the session margin. There is no second
rendering for a sub-agent, and nothing of a sub-agent is hidden. What a
sub-agent does on its screen is what the user sees, tool results included.
`cfg->agent_pty` says that a child has a terminal. It lets `sink_may_present()`
present, and it stops the child from sending progress that the parent showed
already.

## OAuth and JSON-RPC

Implement OAuth as a state machine on the borrowed event loop. The start
function must arm the flow and return. Keep the synchronous wait function as a
wrapper over the same state machine.

Bind loopback redirects to `127.0.0.1`. Allow concurrent browser connections.
Return 404 for unrelated paths and continue to wait. Keep issuer, client ID,
scopes, authorization query, token exchange, and credential storage in the
provider-specific caller.

Configured MCP clients survive logout. Remove dynamic clients on logout
because they are tied to one redirect URI. Release discovery state on every
terminal path. Explicit login or logout cancels active browser, discovery, and
refresh work.

A JSON-RPC standard-I/O connection owns its reader, writer, and buffers. Match
responses by request ID. Remove a completed request from both connection lists
before you release its storage.

Every path that ends a connection settles each outstanding request, in
`pending` and in `flush_wait`. A request with no timeout has no other way to
complete. An invalid frame is such a path: the peer can hold the connection
open indefinitely. Destruction also clears the connection pointer on each
request it settles. One frame has a hard size limit, so a peer that sends no newline
cannot grow the receive buffer without end.

## Diagnostics

Collect diagnostics in `cfg->diag`. Drain them only at turn, verb, and slash
command boundaries. Do not print diagnostics during streaming output.

Define `FYAI_MODULE` once in each source file. Let the module add the message
prefix. Do not type the prefix into each message.

The first error is the cause. Demote subsequent errors to debug until the sink
is drained or reset. A path that stops must report the cause at that location.
Do not report only that an operation failed. That report gives no cause.
`fyai_run_turn()` reports a cause on each path that returns no result. It also
supplies a fallback cause.

The component that holds an identity owns it. A sub-agent raises the cause.
Only the caller knows which parallel sub-agent raised it, so the caller adds
the name. Copy a name from generic storage before you run the sub-agent. The
operation reopens the arena, and a cast pointer does not remain valid. Use
`fyai_diag_reset()` after a caller recovers from an error.

A forked tool child exits through `_exit()`. No component drains its sink. The
child must give its collected diagnostics to the parent.
`fyai_diag_take_generic()` puts them in the `tool/run` response. The parent
uses `fyai_diag_adopt()` when it collects the job. Keep the severity and module
that the child recorded. The parent adds a marker because only the parent knows
which child raised the diagnostic. For example:
`[main/agent:greeter] the reason`. Keep an existing marker. During nested
delegation, the innermost path identifies the source.

Do not remove diagnostics when you quote them. The model reads a reason in a
tool result. The user must also receive the same reason. Use
`fyai_diag_string()`. This function renders the diagnostics but does not remove
them.

A child that terminates can have no diagnostic to send. The parent must state
how the job ended. Report the signal or the absence of a result. Put all
details for one failure in one diagnostic. Use a lower severity for a nonfatal
report.

The diagnostic sink owns its own builder and its own output descriptor. Do not
use the durable builder or `transient_gb`, and do not route it through the
rendering sink: diagnostics must report before that sink is created and after
it is destroyed. It is the one output owner beside the sink.

Parse provider JSON with `FYOPPF_COLLECT_DIAG`. The parser then attaches its
report to the failed value instead of writing it to standard error, where it
names the anonymous input buffer by address, sits outside the sink, and cannot
be attributed to the request that produced it. Report it with
`parse_diag_text()` from a caller that has a context, in the one diagnostic
that says what was being read.

### The trace log

The diagnostic sink presents required user actions at a turn boundary. It does
this in one process. The trace log records each raise when it occurs. It records
raises from all processes and keeps the original severity. It also records
raises that the mask removes. The trace remains available if a forked child
terminates. Use it to examine a stress run.

`$FYAI_TRACE` turns it on: `1` or `on` writes `~/.fyai/trace.log`, any other
value is the path to write. `$FYAI_TRACE_LEVEL` sets the lowest severity
recorded and defaults to debug.

Write each record as one line with one `write(2)` operation. Use an append-only,
close-on-exec descriptor. The parent and its forked children share the file.
The single write prevents incomplete concurrent lines. Do not use a low
descriptor number. A tool child uses descriptors 3 and 4 for its control
channel. A child that closes inherited descriptors must call
`fyai_diag_trace_reopen()`.

`fyai_diag_trace_tag()` adds the sub-agent branch to process records.
`fyai_diag_tracef()` records events that are not diagnostics. These events
include process start and child termination. Tracing must fail silently. A
trace-write failure must not raise a diagnostic.

Expand formatted text before you intern it. Diagnostic raises are lock-free.
A drain resets storage. Drain only when all raisers are inactive. A null sink
prints immediately.

Keep normal output out of the diagnostic sink. The banner, spinner, shell echo,
approval prompt, stats, usage, and successful command results are presented
content: send them to the rendering sink on the status or notice stream.

## Build and test

Configure and build:

```sh
cmake -S . -B build -G Ninja
ninja -C build
./build/fyai -m gpt-4o-mini "hello"
./build/fyai dump state
```

Run the full test suite through the parallel test target. Use `ctest` only for
targeted test runs:

```sh
ninja -C build parallel-test
./build/fyai_test --list
./build/fyai_test oauth/plain_redirect
ctest --test-dir build -R fyai/unit/oauth
```

The unit-test binary links production sources except `src/main.c`. Do not use
test stubs. Declare tests with `FYAI_TEST_ENTRY(suite, name, entry)` after the
includes in a `tests/fyai_*_test.c` file.

Functional cases run the real binary with the scenario-driven mock provider.
Keep them hermetic: localhost only, with private `HOME` and `XDG_*` paths. A
case that looks for a process must scope the search to its own run - its
process tree, or its scratch directory in the command line. A search of the
whole machine finds another run's children. Never `pkill` a global pattern.

### Sanitizers

Use ASAN for parser, storage, tool, event, and YAML changes:

```sh
cmake -S . -B build-asan -G Ninja -DENABLE_ASAN=ON -DCMAKE_BUILD_TYPE=Debug
ninja -C build-asan
ninja -C build-asan parallel-test
```

The complete functional suite must also pass under ASAN.

Every deadline in the suite is multiplied by `FYAI_TIMEOUT_SCALE`, which CMake
sets to 3 for a sanitized tree and to 1 otherwise. A run raises it without
reconfiguring:

```sh
FYAI_TIMEOUT_SCALE=4 ninja -C build parallel-test
```

Scale a deadline only. A value that paces a case, such as how long the PTY
driver waits before it types, is a step of the case: scaling it changes what
the case does.

### Static builds

- `FYAI_MOSTLY_STATIC`: statically link dependencies, but keep glibc, libm,
  and the loader dynamic. This mode includes the glibc C23 compatibility
  wrappers.
- `FYAI_MUSL_STATIC`: build a fully static musl binary with the Docker static
  builder.
- `FYAI_STATIC`: use the legacy fully static host-libc mode only for controlled
  same-host tests.

Static dependency modes require `libfyaml.a`. Glibc modes also require static
OpenSSL and zlib libraries. A mostly-static binary must list only libc, libm,
and the loader in `ldd`. A musl-static binary must report that it is not a
dynamic executable.

### Terminal rendering tests

Use libfyvterm when correctness depends on terminal cells, wrapping, cursor
position, background fill, or SGR state. A PTY byte capture cannot prove these
properties.

Use two `struct fyvt_screen` screens. Feed direct libfymd4c output to one
screen. Feed the same content through the libfytimui path to the other.
Compare the complete stable row and adjacent blank rows. Inspect cells after
the final glyph. Normalize colors with `fyvt_screen_convert_color_to_rgb()`
and compare them with `fyvt_color_is_equal()`. Do not compare
`union fyvt_color` with `memcmp()`.

Make the new test fail before the fix. Register it as an individual CTest test.
After the fix, run both the libfytimui and fyai suites. If renderer bytes are
correct but cells differ, trace transport writes and later compositor cleanup.

## C style

Use Linux kernel C style:

- Use hard tabs with 8-column stops.
- Use kernel braces and spacing.
- Declare local variables at the start of the function.
- Do not declare variables inside branches or loops.
- Use `lower_snake_case` for C names.
- Use uppercase names for CMake options.
- Use four spaces in CMake files.
- Compile as GNU C2x with `-Wall -Wextra` and
  `-Wdeclaration-after-statement`.
- Add SPDX headers to new source files.
- Avoid whitespace-only alignment changes.
- State ownership and non-obvious arena, mmap, atomic, durability, and
  filesystem assumptions near the code.

Do not put an operation inside an error-check predicate. Run the operation,
store its result, and then test the result:

```c
	rc = epoll_ctl(el->backend_fd, op, fd, &ee);
	fyai_event_error_check(el, !rc, err_out,
			       "epoll_ctl: %s", strerror(errno));
```

Do not write `fyai_event_error_check(el, !epoll_ctl(...), ...)`. Apply this
rule to all error-check macros. If a subsystem reports through another handle,
define one wrapper macro instead of repeating `->ctx`.

Use ASD-STE100 Simplified Technical English for changed documentation,
retained comments, and commit messages.

## Commits and patch series

Use an imperative commit subject with a subsystem prefix, for example:

```text
cli: add interactive prompt mode
```

Use two or three short body lines. State what changed and why. Wrap at 80
columns. End with exactly this trailer:

```text
Signed-off-by: Pantelis Antoniou <pantelis.antoniou@konsulko.com>
```

Do not add another attribution trailer.

Make each patch one logical change. Build each intermediate patch. Keep these
changes in separate patches and in this order:

1. implementation;
2. tests; and
3. documentation.

Fold a fix into the patch that introduced the defect. Do not add a later fixup
patch. Remove temporary notes and revision history from the final series. Run
the applicable tests after each test patch. Run normal and ASAN suites on the
final patch. Run `git diff --check` on each patch.

## Patch review workflow

Review `master..devel` as a mail series. Before the first review, tag the tip as
`start-of-review` and generate the series:

```sh
git format-patch -o x master..devel
```

The reviewer writes notes that start with `panto>>` into numbered patch files.
Before you rewrite commits, find every annotated file:

```sh
rg -l '^panto>>' x/[0-9][0-9][0-9][0-9]-*.patch
```

Move every annotated numbered file to `_ANNOTATED-NNNN-...patch`. Preserve all
of them before you regenerate any numbered file. Never edit, replace, or
remove an `_ANNOTATED` file.

Fold each requested change into the commit represented by its patch. Carry
interface and semantic changes through all later commits. Keep every
intermediate commit buildable. Resolve commits again from the current series
order after each history rewrite.

When a patch is accepted, rename its preserved file to
`_REVIEWED-NNNN-...patch`. Never edit or replace a `_REVIEWED` file. Remove
only numbered patch files before you regenerate the series. Confirm that new
numbered files contain no `panto>>` notes and that preserved files still
contain their notes.

Keep branch `reviewed` based on `master`. Apply accepted rewritten commits to
it in order. Confirm that its tree matches `devel` at the last accepted patch.
Then switch back to `devel`. Do not apply annotated mail files.

Before regeneration, run `git diff --check master..devel`, build the normal
tree, and run relevant tests. Run the complete ASAN suite after semantic or
structural changes. Remove temporary checkpoint commits and safety tags when
the rewrite is complete. Leave `devel` checked out. Do not change untracked
review data except for the requested files under `x/`.
