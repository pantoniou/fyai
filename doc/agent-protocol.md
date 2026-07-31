# Sub-agent control protocol

## Scope

The public control protocol is served by `fyai agent --rpc`. It connects a
controller to one transient standalone sub-agent process for one run. The
worker shares the workspace, but it does not restore or publish arena
conversation state.

The protocol uses JSON-RPC 2.0. Each message is one JSON document followed by a
newline. The controller writes to the child standard input. The child writes
to its standard output. Both endpoints can send requests and notifications.

Standard error is not part of the protocol.

## Sequence

The controller uses this sequence:

1. Send `initialize` and read its response.
2. Send `agent/run` and read the final report response.
3. Send `shutdown` and read its response before the connection closes.

The child continues to service the event loop until it writes all queued
responses.

## Methods

### `initialize`

`initialize` confirms that the peer can service JSON-RPC requests. Its optional
`name` parameter defaults to `agent`. The result is:

```json
{"agentId":"agent","protocol":1}
```

### `agent/run`

`agent/run` starts the one sub-agent turn serviced by this connection. Its
parameters contain:

- `task`: the required, nonempty task text;
- `name`: an optional short name, default `agent`; and
- `description`: an optional short description, default empty.

The child defers the run until the current JSON-RPC dispatch is complete. It
returns `{ "report": <final-assistant-text> }`. A connection services one
`agent/run` request; another returns error `-32003`. This standalone interface
does not accept the delegated tool's `persona`, `context`, or `timeout`
parameters.

### `shutdown`

`shutdown` requests an orderly stop. The child returns JSON `null`. It closes
the connection only after it writes the response.

## Tool child methods

Model tools, including persistent delegated agents, use a separate internal
parent/child connection. It is not the `fyai agent --rpc` connection.

`tool/run` takes a `call` containing the canonical tool call. A delegated agent
also receives the already-reserved `branch` name. The result contains:

- `result`: the tool result generic; and
- `ok`: the tool outcome.

`tool/progress` is a notification with a `text` chunk emitted while the tool
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
