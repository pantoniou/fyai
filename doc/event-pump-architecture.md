# Event Pump Architecture

## Purpose

fyai uses one application-owned event pump. Model requests, tool jobs, terminal
input, signals, OAuth, and MCP operations make progress through callbacks
instead of entering nested blocking loops.

The portable event infrastructure and the interactive conversion are in place.
This document records the architecture, the remaining standalone adapters, and
the invariants that new asynchronous work must preserve.

## Existing foundation

`src/fyai_event.c` provides a context-owned portable event loop with:

- readable and writable file-descriptor sources;
- one-shot and repeating timers;
- synchronous signal delivery;
- child-process completion;
- Linux `epoll`, `signalfd`, `timerfd`, and `pidfd` support; and
- BSD and macOS `kqueue` support.

The loop is owned by `struct fyai_ctx` and obtained with `fyai_ctx_loop()`.
There are no process-global loop objects.

Several subsystems already use this infrastructure:

- `src/fyai_ui.c` registers terminal input and redraw timers.
- `src/fyai_curl.c` maps curl multi sockets and timers to event sources.
- `src/fyai_oauth.c` implements the OAuth receiver as a state machine.
- `src/fyai_tools.c` registers tool output pipes and child completion.
- signal ownership and dispatch belong to fyai.

This means the backend does not need to be replaced. The remaining work is to
move ownership of execution state out of nested call stacks.

## Progress snapshot

The interactive application has crossed the asynchronous boundary:

- curl transfers have per-transfer submit, completion, cancellation,
  collection, and destruction semantics;
- streamed model requests are heap-owned state machines with completion and
  tool-call callbacks;
- tool job groups are self-servicing, enforce a concurrency limit, retain
  provider order, and notify their owner on completion;
- complete parallel-capable calls may enter an open tool group while the model
  response is still streaming; and
- live workbands and durable Markdown share the same tool-call boundary;
- the turn machine and interactive frontend run from the application pump;
- MCP startup, requests, OAuth recovery, and shutdown are callback-driven; and
- editors and interactive config edits are child operations beneath the pump.

## Remaining synchronous adapters

The interactive path has no nested pump. Thin synchronous adapters remain for
standalone commands and setup paths that own the top-level loop themselves:

- batch and one-shot turn execution;
- command-line provider login and token refresh;
- command-line config editing;
- one-shot MCP bring-up; and
- compatibility helpers used by isolated tests.

These adapters submit the same heap-owned operations, pump until their
completion callback fires, and collect the latched result. They must never be
called beneath the interactive application pump.

## Target architecture

There should be one top-level application pump. All long-running work should
be represented by context-owned or heap-owned execution objects.

The interactive application advances through explicit states:

```text
IDLE
  -> TURN_PREPARE
  -> MODEL_SUBMITTED
  -> MODEL_STREAMING
  -> RESPONSE_READY
  -> TOOLS_SUBMITTED
  -> TOOLS_RUNNING
  -> TOOLS_COLLECTING
  -> MODEL_SUBMITTED
  -> TURN_COMMIT
  -> IDLE
```

Failure and interruption may occur in any active state. They transition to
cancellation and finalization instead of unwinding a nested event loop.

The application state belongs to `struct fyai_ctx`, directly or through a
context-owned application object. No global state is required.

## Operation lifecycle

Asynchronous operations should expose a common conceptual lifecycle:

```c
submit(...)
cancel(...)
is_done(...)
collect(...)
destroy(...)
```

Completion is reported through a callback:

```c
typedef void (*fyai_operation_cb)(struct fyai_operation *op,
				  void *userdata);
```

This does not require every operation to share one concrete base structure
immediately. Consistent semantics are more important than premature generic
machinery.

The following rules apply:

- Submission registers all event sources and returns without pumping.
- Event callbacks update only the operation that owns the event.
- Completion is latched and its callback fires exactly once.
- Cancellation is idempotent.
- Collection never blocks.
- Destruction removes every surviving event source.
- Callback state is owned by the operation, never by a temporary stack frame.
- Diagnostics are retained on the operation result.

## Curl transfer boundary

`fyai_curl.c` uses curl's socket-driven multi interface and now exposes:

```text
fyai_curl_submit()
fyai_curl_cancel()
fyai_curl_done()
fyai_curl_collect()
fyai_curl_transfer_destroy()
```

The curl multi handle remains context-owned so connection pooling survives
between requests. Transfer-specific state is a separate object containing:

- the easy handle;
- result and completion state;
- cancellation state;
- the completion callback; and
- caller userdata.

`fyai_curl_perform()` can remain temporarily as a compatibility wrapper:

```text
submit transfer
pump the context loop until its completion callback fires
collect result
```

This currently keeps noninteractive and not-yet-converted callers working.
The turn machine and MCP operations must use submission directly.

## Model execution stream

The streamed model request is now a heap-owned execution object. It owns:

- the curl transfer;
- raw HTTP and SSE assembly buffers;
- provider-specific response state;
- progressive Markdown rendering state;
- reasoning and answer output state;
- accumulated tool-call fragments;
- usage and token extents;
- retry and response-chain state; and
- its terminal result and diagnostic.

Provider events are applied incrementally by curl's write callback. Complete
tool calls can be published before the response ends. When the HTTP transfer
completes, the stream validates provider completion, builds the canonical
response document, and invokes its completion callback.

Authentication and response-chain retries must be explicit state transitions.
They must not recursively call `fyai_perform_streaming_request()` or
`fyai_run_model_step()`.

The model step is now a separate heap-owned operation around that execution
stream. It owns request construction, the active streamed or buffered request,
response-chain and token-extents retries, cancellation, and the final response.
Its states are:

```text
NEW -> BUILDING -> REQUEST_PENDING
                       |
                       +-> RETRYING -> REQUEST_PENDING
                       |
                       +-> COMPLETED | CANCELLED | FAILED
```

Buffered requests now have the same submit, cancel, collect, and completion
callback lifecycle as streamed requests. `fyai_run_model_step()` remains a
compatibility wrapper that submits the operation and pumps the context event
loop until the model step completes. The future turn machine can use the same
operation without entering a nested pump.

## Tool job groups

Tool job groups now have the desired lifecycle:

- create and populate;
- submit;
- service;
- cancel;
- test for completion;
- collect; and
- destroy.

A tool job completion callback:

1. park the completed job;
2. update its workband immediately;
3. dispatch the next queued job when capacity is available; and
4. notify the group owner when every job is parked.

The synchronous compatibility path still manually steps the event loop while
checking group completion. The turn machine should instead install the group
completion callback and collect only after that callback resumes it. Animation
remains timer-driven and redraws only when a frame or tool output changed.

Parallel-capable calls use a group with the configured concurrency limit.
Exclusive calls use the same implementation with a group of one.

Collection remains ordered according to the provider tool-call list even when
execution finishes out of order.

## MCP lifecycle

MCP is context-owned and persistent for one fyai invocation. The interactive
path fires a concurrent startup group in STARTING and folds discovered tools
into the provider catalogue at the first-turn gate; the one-shot and batch
paths keep the synchronous `fyai_mcp_refresh()` wrapper, which submits the same
group and pumps the loop until it settles. Each server advances through its own
startup op:

```text
create server context
  -> start transport
  -> initialize request
  -> initialized notification
  -> tools/list pagination
  -> publish namespaced tools
  -> ready
```

An HTTP server owns one reusable curl easy handle and an optional MCP session
ID. Requests go through the async `jsonrpc` primitive over the shared curl
event backend. A session-bound request receiving HTTP 404 clears the session,
re-runs initialization, and replays the original request as a small sequence of
async requests. Shutdown submits a best-effort HTTP DELETE for an active session
through the awaited shutdown group.

A stdio server owns a persistent child and stdin/stdout pipes. Requests frame
one newline-delimited JSON document and read replies through nonblocking event
sources, matching on response ID and discarding notifications and other IDs
while waiting - one in-flight request per server. Shutdown closes the pipes and
registers the async child-termination ladder (EOF -> SIGTERM -> SIGKILL) in the
shutdown group; there is no blocking `waitpid()` fallback.

MCP tool calls are excluded from parallel tool job groups and run through the
turn's exclusive path. A call routed to a not-yet-settled server is parked on a
poll timer (Gate 2) rather than blocking, so the pump stays live while it waits.

fyai owns SIGPIPE policy through its context. The parent keeps it blocked for
synchronous event delivery, while child setup restores the child disposition
without installing a process-global parent handler.

Existing functional tests cover HTTP retry, discovery pagination, session
recovery, session deletion, multiple configured servers, persistent stdio
environment/cwd handling, polite EOF shutdown, and stubborn-child escalation.
New work should add event-order and cancellation assertions without replacing
that lifecycle coverage.

### MCP server state

Each configured server should become an operation with an explicit lifecycle:

```text
NEW
  -> STARTING
  -> INITIALIZING
  -> NOTIFYING_INITIALIZED
  -> DISCOVERING
  -> READY
  -> RECOVERING
  -> STOPPING
  -> STOPPED
```

`FAILED` and `CANCELLED` are terminal from every active state. Startup is
fire-and-continue rather than a barrier: servers initialize concurrently because
their sessions and transports are independent, and the RUNNING pump starts
immediately. The first-turn gate (`mcp/startup_wait`, default true) holds only
the first model submission until every enabled server is READY or terminally
FAILED; the per-call gate parks a tool call whose server has not yet settled.
Tool definitions are published in configuration order, not completion order.

### MCP request state

Requests need their own operation beneath the server:

```text
QUEUED
  -> WRITING
  -> IN_FLIGHT
  -> READING
  -> RETRY_WAIT
  -> COMPLETED
```

The first implementation should keep a FIFO with one active request per
server. That preserves current session and stdio semantics while allowing
requests to different servers to overlap. Supporting multiple simultaneous
requests to one server can follow later; HTTP then needs one easy handle per
request, and stdio needs response-ID dispatch instead of a single waiter.

For HTTP, the request owns its curl easy handle, transfer, headers, response
buffer, retry timer, status, and parsed result. Retry backoff uses an event
timer. Session recovery queues an initialize handshake and resumes the parked
request afterward without recursion.

For stdio, the server owns nonblocking pipes, a child event source, an input
frame buffer, and an output queue. Readable callbacks assemble complete JSON
lines and route responses by ID. Writable callbacks drain queued request
frames. Server notifications are handled independently rather than discarded.
EOF or child exit fails every queued and active request exactly once.

Cancellation removes a queued request immediately. An active HTTP request
cancels its curl transfer. An active stdio request should send the MCP
cancelled notification where supported, mark the local operation cancelled,
and ignore a late response by ID. If transport integrity cannot be guaranteed,
the server transitions through stop and reinitialization before accepting
another request.

### MCP shutdown

Escalating child termination is already an event-loop operation.
`fyai_event_add_child_terminate()` owns a one-shot child source and escalation
timer with the implicit states:

```text
GRACE_OR_EOF_WAIT
  -> SIGTERM_WAIT
  -> SIGKILL_WAIT
  -> REAPED
```

Zero-duration stages are skipped. Child exit removes the escalation timer
before invoking the completion callback. The final SIGKILL stage has no timer;
the child source remains responsible for reaping and publishing the exit
status. The operation supports several concurrent child ladders, and event
tests cover voluntary exit, SIGTERM, SIGKILL, and concurrent termination.

`fyai_event_child_terminate()` is only the synchronous compatibility wrapper
around that operation. MCP currently uses this wrapper and therefore blocks
its caller despite the callback-driven implementation underneath it.

MCP shutdown should use the existing asynchronous form as part of a group
operation:

- enqueue session DELETE for ready HTTP servers;
- close stdio input after pending writes are cancelled;
- register `fyai_event_add_child_terminate()` for each stdio child;
- let its child source perform reaping and escalation callbacks;
- enforce one bounded application shutdown deadline; and
- treat teardown failures as warnings while still withdrawing every source.

The MCP server must retain the termination source until its callback fires and
must not free the server context from the same stack frame that starts
shutdown. Cancellation of application shutdown does not abandon children: it
shortens their remaining ladder to SIGKILL and still waits for the child event.
Failure to register the operation uses the current direct SIGKILL/reap fallback
only as a last-resort cleanup path.

The termination helper itself should gain named stages, checked `kill()`
results for diagnostics, and an explicit expedite-to-SIGKILL operation. These
are lifecycle refinements, not a reason to duplicate escalation inside MCP.

The application may wait for this shutdown group before destroying the
context, but it must do so through the top-level pump. No MCP callback may
retain the transient builder used by the turn that initiated a request.

### MCP OAuth lifecycle

MCP OAuth is a child operation of the server lifecycle. Resource discovery,
authorization-server discovery, dynamic client registration, browser
authorization, token exchange, and refresh all use the shared curl and event
backends:

```text
RESOURCE
  -> AUTHORIZATION_SERVER
  -> REGISTER        (when no configured or cached client exists)
  -> AUTHORIZE
  -> TOKEN
  -> COMPLETED

REFRESH
  -> COMPLETED

any active state
  -> FAILED | CANCELLED
```

A protected request receiving HTTP 401 parks server recovery behind this
operation. Successful token collection installs the bearer token, persists the
credential record, schedules refresh, and resumes server initialization. No
MCP callback enters the synchronous OAuth wait wrapper.

The credential record is bound to the authorization issuer, protected
resource, client ID, and configured scopes. A mismatch ignores the cached
tokens rather than sending them to a different endpoint. Access and refresh
tokens are stored in the machine-local authentication state, while configured
client secrets remain in the configured secret provider.

Configured and dynamically registered clients have different redirect
lifetimes:

- A configured client uses its configured loopback host, port, and path. Login
  reuses that stable client registration.
- Dynamic registration starts the loopback receiver first and registers its
  exact ephemeral redirect URI. Logout clears both the persisted registration
  and the in-memory dynamic client ID, because the next receiver normally uses
  a different port and therefore requires a fresh registration.

Every terminal path destroys the discovery object and clears
`mcp->oauth_discovery`. Explicit `/mcp login NAME` and
`/mcp logout NAME` supersede an outstanding browser wait, discovery, or
refresh operation. This is necessary when an authorization server rejects the
redirect before visiting fyai's receiver; otherwise the endpoint would remain
busy until the receiver timeout.

Login success is terminal only after the token exchange and durable save have
completed. Logout success is terminal only after the credential record has
been removed and in-memory tokens, timers, and dynamic registration state have
been cleared.

## Turn state machine

The turn machine replaces the loop in `fyai_run_model_loop()`. It owns:

- the input and current turn documents;
- the current model-step operation;
- the current tool group;
- the next tool iteration;
- partial completed work;
- interruption and failure state; and
- the final turn result.

Its callbacks perform small, nonblocking transitions:

```text
user line
  -> prepare and submit model step

model step complete
  -> append assistant response
  -> commit answer when no tools remain
  -> otherwise build and submit tool groups

tool group complete
  -> collect and append tool results
  -> submit the next exclusive group or model step

turn complete
  -> publish durable state
  -> refresh status
  -> return application to IDLE
```

An interrupt cancels the active model transfer or tool group. Completed model
and tool steps remain eligible for publication, matching current partial-turn
semantics.

The model and tool completion callbacks do not transition the machine directly -
a callback runs inside event dispatch and could otherwise free the very step or
group whose completion is on the stack. They instead defer the service through
`fyai_event_defer()`. `fyai_turn_run_service()` runs outside dispatch and
re-derives readiness from the child operation's own latched completion
(`fyai_model_step_done()` / `fyai_tool_job_group_done()`) rather than a cached
flag. The turn therefore keeps its coarse `MODEL`/`TOOLS` states with no
intermediate "notified" state.

## Interactive application pump

This pump is the RUNNING phase of the application state machine below. The outer
interactive loop should become event callbacks around one top-level run:

- a UI line event queues or starts a turn;
- model completion advances the turn machine;
- tool completion advances the turn machine;
- a slash command writes scrolling display output without entering the
  transcript;
- signals and UI interrupt events cancel the active operation;
- resize and animation events redraw only affected UI state; and
- a quit event stops the application loop.

Input entered while a turn is running remains queued by the UI. Escape or
Ctrl-C cancels the active turn and recalls the next pending input according to
the existing UI semantics.

The main event loop may still be pumped while an external editor is open in a
future change. That operation should eventually be represented as another
child-backed asynchronous operation rather than a special nested mode.

## Application state machine

The interactive invocation advances through an explicit lifecycle around the
pump (`fyai_prompt_interactive_async()`):

```text
STARTING -> RUNNING -> STOPPING -> DONE
```

- **STARTING** brings the UI up first, then fires every server's async startup
  op (`fyai_mcp_start()`) and enters RUNNING immediately - input is live while
  servers connect concurrently. This is fire-and-continue, not a barrier.
- **RUNNING** is the interactive application pump. Two gates keep the tool list
  coherent without blocking startup. Gate 1, a one-time first-turn gate, holds
  the first model submission until `fyai_mcp_settled()` (every server READY or
  terminally FAILED) and then folds the discovered tools into the request
  catalogue; the config key `mcp/startup_wait` (default true) turns it off for
  an optimistic first submit. Gate 2 parks a tool call routed to a server that
  has not yet settled: READY admits it, a terminally FAILED server fails it
  fast, and a still-connecting server keeps it parked until it settles or the
  per-call budget runs out.
- **STOPPING** drains MCP and waits for its servers to terminate
  (`fyai_mcp_cleanup()`). It is reached on every exit, including the RUNNING
  error unwind, so teardown belongs to the application rather than to
  `fyai_cleanup()` (whose later MCP teardown is then an idempotent no-op).

`fyai_setup()` defers MCP bring-up for the interactive modes to STARTING so the
UI appears before servers connect; the one-shot and batch modes keep synchronous
bring-up at setup time. The synchronous non-async interactive fallback connects
MCP itself before its first turn.

This is now realised, not a skeleton: STARTING fires the concurrent startup
group (`fyai_mcp_start()`) and continues, the first-turn and per-call gates
above keep the tool list coherent, and STOPPING drains an awaited shutdown group
over the child-termination ladder. The connect/settle machinery is described in
the MCP lifecycle section and migration step 7.

## Compatibility during migration

Synchronous wrappers are useful transition tools. Each wrapper may submit an
asynchronous operation, run the shared loop until that operation completes,
and collect its result.

This permits conversion in dependency order:

- the operation implementation becomes callback-driven first;
- interactive callers switch to callbacks next; and
- synchronous wrappers remain for command verbs, tests, and unconverted
  subsystems.

Nested runs disappear only after their callers no longer need them. The event
backend and operation implementation do not need to change again.

## Migration sequence

### 1. Establish operation conventions

Document and test completion, cancellation, collection, and destruction
semantics. Add helpers only where they eliminate repeated lifecycle mistakes.

### 2. Make curl submission asynchronous

Completed. Per-transfer state and callback completion are in place;
`fyai_curl_perform()` remains as a wrapper.

### 3. Heap-own model streams

Completed. Streaming and buffered requests are heap-owned and callback-driven
(`fyai_stream_request_*` / `fyai_buffered_request_*`), and the async model step
submits through them without a nested pump. Model-step retry ownership lives on
the step (`fyai_model_step_retry` re-enters `fyai_model_step_start`), covering
the response-chain-miss and token-extents retries in both modes. The synchronous
`fyai_perform_*` wrappers remain only for the batch driver (see step 5's
`fyai_run_model_loop`).

### 4. Make tool groups self-servicing

Completed. Child completion services the FIFO and notifies the turn operation
when a sealed group is parked.

### 5. Introduce the turn machine

Completed for every path. The heap-owned turn operation advances model
requests, streamed tool prefetch, parallel groups, exclusive groups, retries,
cancellation, and final publication without a nested event pump. The batch,
one-shot, non-tty interactive and compact paths now drive the same operation
through `fyai_run_turn()`, a bare top-level pump; the synchronous
`fyai_run_model_loop()` and its per-step model/tool nested `run_until` are
gone.

### 6. Convert the interactive frontend

Completed for the terminal UI. UI events
queue input, the application loop starts turns while idle, and model and tool
callbacks wake the turn operation.

### 7. Convert MCP lifecycle

Completed. A shared `jsonrpc` request primitive backs both the call op and the
per-server startup op; servers connect concurrently through a startup group
(`fyai_mcp_start()`, fire-and-continue in STARTING) and drain through an awaited
shutdown group in STOPPING. HTTP requests are off `fyai_curl_perform()` and
stdio `poll()` is replaced by nonblocking event sources. The first-turn gate
(`mcp/startup_wait`) and the per-call readiness gate keep tool calls coherent
while servers are still connecting. OAuth discovery, configured and dynamic
clients, browser login, token exchange, refresh, explicit login/logout, and
401 recovery use the same application pump. Deferred to a follow-up: active
mid-session reconnect of a dropped server.

### 8. Convert auxiliary blocking paths

Completed. An audit of every remaining
`run_until()`/`loop_step()`/`waitpid()` outside the event core places each site
in one of three permitted classes:

- **Async beneath the pump (done):** model steps, tool groups
  (`fyai_tool_job_submit`), MCP tool calls (`fyai_mcp_call_submit`, intercepted
  before any sync path), and in-turn auth refresh (`fyai_auth_refresh_submit`)
  are all callback-driven. The exclusive in-process tool path only ever runs
  `ask_user`, which does not nest a pump.
- **Standalone command/setup adapters (permitted):** the `auth`/`login` verbs
  (`fyai_auth_refresh`, `auth_login`), `fyai_oauth_flow_wait`, the one-shot
  `fyai_mcp_refresh` bringup, `fyai_ui_readline` setup, config editing from the
  CLI, and the auth-only `fyai_curl_perform` are thin sync wrappers over async
  ops, never reached from the interactive state machine.
- **Forked-child isolated loops (by design):** `run_shell_command_capture_cb`
  runs after `fyai_ctx_loop_abandon()`, on the child's own loop; the `waitpid()`
  sites are forked-child reaps and MCP shutdown force-reaps.

The one dead site the first audit turned up -
`fyai_tool_run_forked()`'s parent-side `run_until`, unreachable once tool
dispatch became async - was removed along with the
`fyai_tool_job_spawn()` synchronous-job branches. The tool path now has no
parent-side `run_until`; the fork itself is unchanged and the sandbox is still
applied in every tool child.

External editors now use a heap-owned child operation with
submit/cancel/done/collect/destroy semantics. Ctrl-G starts the line editor
directly from its UI event and resumes the input buffer from a deferred child
completion. `/config edit` retains a context-owned edit operation and applies
the edited document from its deferred continuation. The CLI forms keep a
top-level synchronous adapter over the same operation.

Interactive MCP shutdown is likewise split into start, settled, and finish
operations. The application enters STOPPING, starts every server teardown,
continues its one pump until session DELETE transfers and child termination
ladders settle, then frees server state. `fyai_mcp_cleanup()` remains the
standalone adapter for noninteractive and setup-failure paths.

## Verification

Each conversion should test:

- completion callbacks fire exactly once;
- cancellation before and after submission;
- cancellation while output is readable;
- source removal from inside callbacks;
- destruction with pending sources;
- signal delivery during model and tool execution;
- queued input during model streaming;
- immediate per-tool progress and terminal color updates;
- concurrency-limit FIFO behavior;
- ordered collection after out-of-order completion;
- terminal resize during streaming and tool output;
- provider retry without recursion;
- curl connection reuse across sequential transfers;
- concurrent initialization of multiple MCP servers with ordered discovery;
- MCP stdio partial writes, split frames, notifications, and out-of-order IDs;
- MCP HTTP retry timers and session recovery without recursion;
- cancellation of queued and active MCP requests;
- cancellation or replacement of an active MCP OAuth browser wait;
- configured-client login with an exact stable redirect;
- dynamic-client logout and re-registration for a new ephemeral redirect;
- OAuth token refresh with issuer and resource binding;
- shutdown escalation for a stubborn MCP stdio child;
- best-effort HTTP session deletion during bounded shutdown; and
- parity between live rendering and the durable transcript.

ASAN builds should disable event-source pooling as they do today so stale
callback ownership remains visible to the sanitizer.

## Completion criteria

The migration is complete when the interactive application enters one
top-level event pump and no model, tool, input, MCP, or OAuth path beneath it
calls `fyai_event_loop_run_until()`, manually drives
`fyai_event_loop_step()`, or blocks in `poll()`, retry sleeps, pipe I/O, or
`waitpid()`.

Synchronous wrappers may remain for standalone command paths, provided they
are thin adapters over the same asynchronous operations and are never used
from the interactive application state machine.

Status: the interactive model, turn, tool, editor, config-edit, OAuth-refresh,
and MCP lifecycle paths meet this criterion. None calls `run_until()`, enters a
private pump, or performs a blocking child reap beneath the application pump.
Batch and one-shot drivers are themselves top-level pumps. Standalone command
wrappers remain for OAuth/login, config editing, one-shot MCP bringup, and
other noninteractive adapters. Blocking `waitpid()` remains only in emergency
post-fork cleanup where event-source registration failed and inside the
isolated tool child after `fyai_ctx_loop_abandon()`.
