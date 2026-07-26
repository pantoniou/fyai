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
- `fyai_run_tool_group()` looping over `fyai_event_loop_step()`;
- synchronous single-tool execution waiting for a job;
- the synchronous OAuth wrapper waiting for its flow; and
- auxiliary MCP and command paths using `poll()`, `waitpid()`, or blocking
  input directly.

`struct stream_response` is also local to
`fyai_perform_streaming_request()`. Curl callbacks can update it only while
that stack frame is waiting for completion.

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

`fyai_curl.c` already uses curl's socket-driven multi interface, but
`fyai_curl_perform()` waits synchronously. It should be split into:

```text
fyai_curl_submit()
fyai_curl_cancel()
fyai_curl_done()
fyai_curl_collect()
fyai_curl_transfer_destroy()
```

The curl multi handle remains context-owned so connection pooling survives
between requests. Transfer-specific state becomes a separate object containing:

- the easy handle;
- result and completion state;
- cancellation state;
- the completion callback; and
- caller userdata.

The current curl state stores a single active easy handle and a single result.
Those fields must move to the transfer object. Socket bookkeeping should route
curl messages to the matching transfer.

`fyai_curl_perform()` can remain temporarily as a compatibility wrapper:

```text
submit transfer
pump the context loop until its completion callback fires
collect result
```

This keeps noninteractive and not-yet-converted callers working while the
interactive path stops using the wrapper.

## Model execution stream

The model request and its streamed response should become one execution
object. The current `struct stream_response` should move out of
`fyai_perform_streaming_request()` and own:

- the curl transfer;
- raw HTTP and SSE assembly buffers;
- provider-specific response state;
- progressive Markdown rendering state;
- reasoning and answer output state;
- accumulated tool-call fragments;
- usage and token extents;
- retry and response-chain state; and
- its terminal result and diagnostic.

Provider events continue to be applied incrementally by curl's write callback.
When the HTTP transfer completes, the stream validates provider completion,
builds the canonical response document, and invokes the model-step completion
callback.

Authentication and response-chain retries must be explicit state transitions.
They must not recursively call `fyai_perform_streaming_request()` or
`fyai_run_model_step()`.

## Tool job groups

Tool job groups already have most of the desired lifecycle:

- create and populate;
- submit;
- service;
- cancel;
- test for completion;
- collect; and
- destroy.

The remaining polling relationship should be removed. A tool job completion
callback should:

1. park the completed job;
2. update its workband immediately;
3. dispatch the next queued job when capacity is available; and
4. notify the group owner when every job is parked.

The application loop should not need to call
`fyai_tool_job_group_service()` after unrelated events. Animation should use a
timer owned by the group or the UI and should redraw only when a frame or tool
output changed.

Parallel-capable calls use a group with the configured concurrency limit.
Exclusive calls use the same implementation with a group of one.

Collection remains ordered according to the provider tool-call list even when
execution finishes out of order.

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

## Interactive application pump

The outer interactive loop should become event callbacks around one top-level
run:

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

Introduce per-transfer state and callback completion while retaining
`fyai_curl_perform()` as a wrapper. Verify connection reuse, cancellation,
socket removal, and completion-callback ordering.

### 3. Heap-own model streams

Convert buffered and streaming requests to model-step operations. Remove
recursive retry calls in favor of state transitions. Preserve byte-for-byte
live and durable rendering behavior.

### 4. Make tool groups self-servicing

Connect child and output callbacks to their group. Dispatch FIFO work and
signal group completion without a manual `event_loop_step()` loop.

### 5. Introduce the turn machine

Move the model/tool iteration state out of `fyai_run_model_loop()`. Keep that
function as a synchronous compatibility wrapper initially.

### 6. Convert the interactive frontend

Start turns from UI events and run one application loop until quit. Remove
`fyai_ui_readline()` and model/tool nested waits from the interactive path.

### 7. Convert auxiliary blocking paths

Incrementally move OAuth wrappers, MCP startup and transport, external
commands, and remaining direct `poll()` or `waitpid()` users onto operations.

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
- curl connection reuse across sequential transfers; and
- parity between live rendering and the durable transcript.

ASAN builds should disable event-source pooling as they do today so stale
callback ownership remains visible to the sanitizer.

## Completion criteria

The migration is complete when the interactive application enters one
top-level event pump and no model, tool, input, or OAuth path beneath it calls
`fyai_event_loop_run_until()` or manually drives `fyai_event_loop_step()`.

Synchronous wrappers may remain for standalone command paths, provided they
are thin adapters over the same asynchronous operations and are never used
from the interactive application state machine.
