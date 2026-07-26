# Sub-agent control protocol

## Scope

The control protocol connects a parent process to one sub-agent process. Each
sub-agent has one connection for one run. A later run restores state from the
arena and uses a new connection.

The protocol uses JSON-RPC 2.0. Each message is one JSON document followed by a
newline. The parent writes to the child standard input. The child writes to its
standard output. Both endpoints can send requests and notifications.

Standard error is not part of the protocol.

## Sequence

The parent uses this sequence:

1. Send `initialize`.
2. Send `agent/run`.
3. Send `shutdown`.
4. Read the shutdown response before it closes the connection.

The child continues to service the event loop until it writes all queued
responses.

## Methods

### `initialize`

`initialize` confirms that the peer can service JSON-RPC requests. It takes an
empty parameter mapping and returns an empty result mapping.

### `agent/run`

`agent/run` starts one sub-agent turn. Its parameters contain:

- `task`: the task text;
- `name`: the short agent name; and
- `description`: the short work-band description.

The child defers the run until the current JSON-RPC dispatch is complete. It
returns the final assistant report. A connection services one `agent/run`
request.

### `shutdown`

`shutdown` requests an orderly stop. The child returns an empty result mapping.
It closes the connection only after it writes the response.

## Tool child methods

Forked tool children use the same peer connection.

`tool/run` starts one tool call. The result contains:

- `result`: the tool result generic; and
- `ok`: the tool outcome.

`tool/progress` is a notification. It carries progress data while the tool
runs.

The child services `tool/run` after the current JSON-RPC dispatch. This rule
prevents a nested event-loop dispatch.

## Framing and errors

Standard output contains protocol frames only. Other output can corrupt the
connection. A delegated child must suppress normal prose, reasoning, cache
information, and statistics.

The connection matches responses by request ID. It can receive notifications
when no request is pending. It returns JSON-RPC error `-32601` for an unknown
request method. It does not reply to an unknown notification.

A completed or cancelled request must leave all connection lists before the
request storage is released.
