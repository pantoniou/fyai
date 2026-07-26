# Event pump architecture

## Overview

Each `fyai` invocation owns one event loop. The interactive application owns
the only event pump. Model requests, tool jobs, terminal input, signals, OAuth,
editors, and MCP operations make progress through callbacks.

Linux uses `epoll`, `signalfd`, `timerfd`, and `pidfd`. BSD and macOS use
`kqueue`.

## Ownership

`struct fyai_ctx` owns the event loop. Subsystems borrow the loop and remove
their own sources.

A callback can retire its own source. The owner must clear its source pointer
when this occurs. Cleanup must be safe when the source is already absent.

A child that forks without `exec` must call `fyai_ctx_loop_abandon()`. An
`epoll` set is shared after `fork()`. The child must close its descriptor
copies without an `epoll_ctl()` operation.

## Application pump

The interactive application has these states:

```text
STARTING -> RUNNING -> STOPPING -> DONE
```

`STARTING` starts MCP servers. `RUNNING` services input and active operations.
`STOPPING` cancels the active turn and drains MCP shutdown. `DONE` releases the
application state.

The first model step waits for MCP startup when `mcp/startup_wait` is true.
The application rebuilds the request tool set after all servers are ready or
failed.

Callbacks below the application pump must not call `run_until()` or step the
loop. Synchronous wrappers are for batch commands, setup adapters, and tests
that own the top-level loop.

## Turn operation

A turn owns one model step or one tool group. An open parallel tool group can
start complete tool calls while a model stream continues.

Completion callbacks defer turn service. Deferred service performs state
transitions after event dispatch. This prevents a callback from releasing an
operation that is still on the dispatch stack.

Cancellation follows ownership. A model-state cancellation also cancels its
open tool group. A tool-state cancellation cancels the current group. Normal
completion callbacks collect and release cancelled operations.

## MCP operations

MCP servers start concurrently. Each server has independent startup, request,
OAuth recovery, and shutdown state. One failed server does not stop the other
servers.

The JSON-RPC connection owns stdio framing and buffers. Requests own their
completion state and timeouts. Responses are matched by request ID.

## Signals

Most signals use event-loop sources. SIGINT uses a process signal handler
because a stopped event pump cannot dispatch a `signalfd` event.

The SIGINT handler sets the interrupt state, writes to the loop wake source,
and starts a 200 ms watchdog. A pump that observes the interrupt must call
`fyai_event_interrupt_ack()`. If the pump does not acknowledge the interrupt,
the watchdog restores the default SIGINT and SIGTERM actions. A second Ctrl-C
can then stop the process.

No subsystem can register a SIGINT `signalfd` source. SIGALRM belongs to the
watchdog. Curl handles must use `CURLOPT_NOSIGNAL`.

SIGUSR2 writes event-loop state to the original standard error descriptor. The
handler uses async-signal-safe operations only.

## Child processes

A forked tool child starts a new session and uses `/dev/null` as standard
input. It must not acquire the parent controlling terminal.

Cancellation uses a process-group shutdown sequence. It sends SIGTERM and then
SIGKILL after the grace period. It also removes descendants that keep protocol
or output descriptors open.

## Validation

Sanitizer builds disable event-source recycling by default. This exposes stale
source pointers. `FYAI_EVENT_NO_POOL=0` enables the pooled path.
`FYAI_EVENT_NO_POOL=1` disables it.

PTY signal tests require a controlling terminal. The driver must use
`setsid()` and `TIOCSCTTY`.
