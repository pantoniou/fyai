# Event Pump Architecture

## Purpose

fyai is moving toward one application-owned event pump. Model requests, tool
jobs, terminal input, signals, OAuth, and eventually MCP operations should make
progress through callbacks instead of entering nested blocking loops.

The portable event infrastructure is already in place. This plan describes how
to move synchronous control flow onto it without rewriting every subsystem at
once.

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

The first operation layers have crossed the asynchronous boundary:

- curl transfers have per-transfer submit, completion, cancellation,
  collection, and destruction semantics;
- streamed model requests are heap-owned state machines with completion and
  tool-call callbacks;
- tool job groups are self-servicing, enforce a concurrency limit, retain
  provider order, and notify their owner on completion;
- complete parallel-capable calls may enter an open tool group while the model
  response is still streaming; and
- live workbands and durable Markdown share the same tool-call boundary.

Synchronous wrappers remain and are still used by the application path. The
next architectural step is the turn machine, not another event backend or
polling primitive.

## Current synchronous boundaries

The interactive control flow is effectively:

```text
read input
  -> submit model request
     -> pump until curl completes
  -> inspect response
  -> submit tool group
     -> pump until all jobs complete
  -> submit another model request
  -> publish the turn
  -> read input
```

The nested pumps keep the UI responsive, but the calling function still owns
the operation synchronously. Important examples are:

- `fyai_ui_readline()` calling `fyai_event_loop_run_until()`;
- `fyai_curl_perform()` calling `fyai_event_loop_run_until()`;
- `fyai_perform_streaming_request_tools()` submitting a stream and then
  calling `fyai_event_loop_run_until()`;
- `fyai_run_tool_group()` looping over `fyai_event_loop_step()`;
- synchronous single-tool execution waiting for a job;
- the synchronous OAuth wrapper waiting for its flow;
- MCP HTTP requests using the synchronous curl wrapper;
- MCP stdio requests using blocking writes and `poll()` reads;
- MCP retry delays using `nanosleep()`; and
- auxiliary shutdown and command paths using `waitpid()` or blocking input.

`struct stream_response` is now heap-owned, so curl callbacks no longer depend
on a waiting stack frame. The model loop still consumes it synchronously, and
recursive retry decisions remain in `fyai_run_model_step()`.

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

`async_model_step: false` is a temporary compatibility switch. It keeps model
request construction unchanged but drives streamed or buffered requests
synchronously within `fyai_run_model_step()`. The default is `true`.

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

The stdio spawn path also changes the process-wide SIGPIPE disposition. That
must disappear during conversion: fyai owns signal policy through its context,
and child setup should restore only the child disposition without mutating a
global parent handler.

Existing functional tests cover HTTP retry, discovery pagination, session
recovery, session deletion, multiple configured servers, persistent stdio
environment/cwd handling, polite EOF shutdown, and stubborn-child escalation.
The conversion should retain those tests and add event-order and cancellation
assertions rather than replacing the lifecycle coverage.

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

MCP OAuth adds another continuation to the server/request state machine. A
401 response may park the request behind discovery, refresh, or an interactive
login operation, but must never call the synchronous OAuth wait wrapper from
inside an MCP callback.

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
group whose completion is on the stack. They instead defer
`fyai_turn_run_service()` (`fyai_event_defer()`), which runs outside dispatch and
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

Completed for the interactive path. The heap-owned turn operation advances
model requests, streamed tool prefetch, parallel groups, exclusive groups,
retries, cancellation, and final publication without a nested event pump.
`fyai_run_model_loop()` remains the synchronous batch compatibility path.

### 6. Convert the interactive frontend

Completed for the terminal UI when `async_model_step` is enabled. UI events
queue input, the application loop starts turns while idle, and model and tool
callbacks wake the turn operation. The compatibility switch retains the old
nested path intentionally.

### 7. Convert MCP lifecycle

Completed. A shared `jsonrpc` request primitive backs both the call op and the
per-server startup op; servers connect concurrently through a startup group
(`fyai_mcp_start()`, fire-and-continue in STARTING) and drain through an awaited
shutdown group in STOPPING. HTTP requests are off `fyai_curl_perform()` and
stdio `poll()` is replaced by nonblocking event sources. The first-turn gate
(`mcp/startup_wait`) and the per-call readiness gate keep tool calls coherent
while servers are still connecting. Deferred to a follow-up: active mid-session
reconnect of a dropped server.

### 8. Convert auxiliary blocking paths

Incrementally move OAuth wrappers, external commands, and remaining direct
`poll()` or `waitpid()` users onto operations.

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
