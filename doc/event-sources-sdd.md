# Event sources

## Status

Design, in part implemented. It builds on `doc/events.md` and closes its
gaps "Structured events" and "Other event sources".

Implemented: the wire of the extension for stdio servers (handshake, kinds,
events, rejections, acknowledgement on publish, credits; `mcp/servers/NAME/
events`), `list` and `wait` on sources, the live panel line,
`fyai source lines`, bound actions, trust levels, route rules that drop
or wake, and the decision primitives: `tool_choice`, `fyai decide`, and
personas with a result schema, canned deciders, route `decide:` steps,
and `fyai source decide`. The rest is design.

## Problem

Take a Slack bridge built on what fyai offers today, the obvious way:

1. A Socket Mode bot, started by hand, appends each accepted message to a
   journal file, `inbox.jsonl`.
2. A `monitor` runs a watcher script, a polling `tail` that keeps its
   position in an offset file and prints a "bridge down" line when `pgrep`
   finds no bot.
3. The model reads the line, does the work, and runs a reply script
   from the shell with the right `channel` and `ts`.

The weak points:

| Weak point | Cause |
|---|---|
| The model must restart the monitor every 30 minutes | A monitor lives for one tool call, with a time limit. |
| 200 events and the monitor stops | The firehose cap protects against a chatty command, not a real feed. |
| Lost or repeated messages | The offset is saved when a line is printed, not when the turn that holds it is stored. |
| Liveness by `pgrep` and a poll every second | fyai does not own the bot process. |
| Untyped events | A line of text. Nothing checks it. The model parses JSON out of `[monitor 'slack'] ...`. |
| Replies through the shell | The model must pass the right message id (`ts`, not `thread_ts`) and load the token file each time; a wrong id replies into the wrong place. |
| Trust lives in the bot | `SLACK_ALLOWED_USERS` is the only gate. fyai does not know who sent an event. |
| Every event costs a turn of the main model | Nothing cheaper looks at an event first. |

## Goal

A program that fyai starts, or a socket it connects to, speaks JSON-RPC 2.0.
In the handshake it declares the kinds of events it sends, each with a JSON
Schema, and the actions that answer them. fyai owns the process, validates
each event, stores it as a typed record, acknowledges it when it is safe, and
routes it: to rules, to a small decision model, or to the agent.

## Choice: an MCP extension, not a new protocol

A source is an MCP server that declares the capability
`experimental["fyai/events"]`.

Reasons:

- The MCP client already spawns, sandboxes, restarts and tears down stdio
  servers, speaks HTTP, and does OAuth (`src/mcp/fyai_mcp.c`).
- `jsonrpc_conn_serve()` (`src/net/fyai_jsonrpc.h`) already serves inbound
  requests and notifications. The MCP client does not use it yet.
- A source needs actions (post a reply, add a reaction). Those are MCP tools.
  One process, one config entry, one handshake.
- The same server keeps working as a plain MCP server in another harness:
  a client that does not offer the capability gets the tools and no events.

The wire is newline-framed JSON on stdio, as MCP stdio is. A program that is
not an MCP server is wrapped by a line source (see "Line sources").

A standalone protocol would duplicate spawn, sandbox, auth and tool plumbing
for no gain in expressiveness. The extension keeps every fyai-specific
method under `fyai/`.

Only the wire is the extension. Routing, trust, decisions, compaction and
archives are policy of the client, held in fyai's config and applied to the
stored records. A source knows nothing of them.

### Why not resource subscriptions

MCP has `resources/subscribe` and `notifications/resources/updated`. The
notification carries no data, only a URI, so each event costs a
`resources/read` round trip. It has no event identity, no cursor and no flow
control, and a resource has no schema. The extension adds exactly those.

## Handshake

The handshake mirrors the tools of MCP: the capability says that the server
has event kinds, and a list method returns them.

fyai offers the capability in `initialize`:

```json
{"method": "initialize", "params": {
  "protocolVersion": "2025-06-18",
  "clientInfo": {"name": "fyai", "version": "0.5.2"},
  "capabilities": {"experimental": {"fyai/events": {
    "version": 1,
    "cursor": "1700000000.000100",
    "credits": 16
  }}}
}}
```

- `cursor`: the last position fyai acknowledged for this source, from the
  arena. Absent on the first start. The source resumes after it.
- `credits`: how many unacknowledged events the source may send.

The source answers that it has events, and whether the list can change:

```json
{"capabilities": {"tools": {}, "experimental": {"fyai/events": {
  "version": 1,
  "listChanged": true,
  "health": true
}}}}
```

After `notifications/initialized`, fyai asks for the kinds with
`fyai/events/list`, paginated as `tools/list` is:

```json
{"kinds": [
    {
      "name": "message",
      "description": "A Slack message addressed to the bot",
      "schema": {
        "type": "object",
        "required": ["user", "channel", "ts", "text"],
        "properties": {
          "user": {"type": "string"},
          "channel": {"type": "string"},
          "channel_type": {"enum": ["im", "channel", "group"]},
          "ts": {"type": "string"},
          "thread_ts": {"type": "string"},
          "text": {"type": "string", "maxLength": 40000}
        }
      },
      "principal": "/user",
      "conversation": ["/channel", "/thread_ts"],
      "summary": "{user} in {channel}: {text}",
      "actions": {
        "reply": {"bind": {"channel": "/channel", "thread_ts": "/ts"}},
        "react": {"bind": {"channel": "/channel", "ts": "/ts"}}
      }
    }
  ],
  "nextCursor": null
}
```

A source with `listChanged` sends `notifications/fyai/events/list_changed`
when it adds or removes a kind, and fyai lists again. A source sends no event
before fyai has listed the kinds; fyai rejects an event of a kind it has not
listed. A source that serves many channels can so add a kind per channel at
run time, and the `initialize` reply stays small.

Per kind:

| Key | Meaning |
|---|---|
| `schema` | JSON Schema of `data`. Checked with `fyai_schema_validate()`. |
| `principal` | JSON Pointer to who caused the event. Config maps it to a trust level. |
| `conversation` | Pointers whose values together name the conversation the event belongs to. Used for routing and for threads. |
| `summary` | A template for the card and for a one-line view of the event. |
| `actions` | Tools of this server that answer this kind, with arguments bound from the event. |

A kind schema is a contract. fyai rejects a server whose schema uses a keyword
the validator does not support, at the handshake, not at the first event.

## Events

The source sends a notification:

```json
{"method": "notifications/fyai/event", "params": {
  "kind": "message",
  "id": "C0123ABCD/1700000000.000100",
  "cursor": "1700000000.000100",
  "time": "2026-01-01T00:00:00Z",
  "data": {"user": "U0OWNER", "channel": "C0123ABCD", "ts": "...", "text": "..."}
}}
```

- `id` is unique within the source. fyai drops an id it already stored, so a
  resend after a crash is harmless.
- `cursor` is opaque to fyai. It is stored and given back.
- An event that fails its schema is not delivered. fyai answers
  `notifications/fyai/rejected` with the id and the problems, and shows a
  diagnostic. The source author sees the mistake at once.

## Acknowledgement and flow control

fyai sends `notifications/fyai/ack {cursor, credits}` when the turn that holds
the event is published to the arena, not when the event arrives. A crash
before the publish resends the event on the next start; the id makes the
resend harmless. Delivery is at least once, and the record is stored once.

`credits` replaces the 200-event cap. A source with no credit holds its
events; it does not drop them. A feed that floods only fills its own buffer.
fyai grants credit as it consumes, and grants none to a source that is
paused (`/source pause NAME`).

## Health

With `health`, fyai sends `ping` (an MCP method) on an interval. The source
answers only when it is healthy: a Slack source answers when its socket to
Slack is up. No answer, or the process ends, puts the source in `down`, shows
it in the status line, and restarts it with backoff. This replaces `pgrep`
and `{"bridge":"DOWN"}`.

## Lifetime

A configured source belongs to the session, as an MCP server does: it starts
in `STARTING`, lives across exchanges, and stops in `STOPPING`. It has no
time limit. `/clear` and `/reset` do not stop it; they drop undelivered
records of the old conversation and keep the cursor where it was published.

A source can also be started by the model with a tool, for an ad-hoc feed:

```text
source_start {name, command, workdir}
```

It lives for the session, as a terminal session does, and `cancel` stops it.

When no session runs, nobody consumes. The source keeps its backlog, and the
next session resumes from the stored cursor. Hosting sources in the fleet or
`serve` process, with a durable branch inbox, is the later step described in
`doc/events.md` "Durable events".

## Records

An event becomes a record, not a line of text:

```yaml
event:
  source: slack
  kind: message
  id: C0123ABCD/1700000000.000100
  time: 2026-01-01T00:00:00Z
  principal: {id: U0OWNER, trust: owner}
  conversation: [C0123ABCD, 1700000000.000100]
  data: {...}
```

The live queue of `fyai_event_inject()` carries the record. The existing
text events (`wait`, `agent`, `monitor`) become records of the built-in
source `fyai` with a string `data`, so a waiter, a card and a stored turn
treat all events the same.

What the model sees: one user message that frames the data as external,

```text
[event slack/message from U0OWNER (owner)]
<external-data source="slack" kind="message" id="...">
{"channel": "C0123ABCD", "ts": "...", "text": "..."}
</external-data>
Answer with: slack_reply, slack_react (the thread is already bound).
```

Events that arrive during a turn are delivered together at the next model
request, as now.

## Bound actions

`actions` turns a tool of the source into an answer to a record. fyai offers
it to the model with the bound arguments removed from its schema and with one
argument added, `event`, the record id. The model writes

```json
{"name": "slack_reply", "arguments": {"event": "C0123ABCD/1700000000.000100", "text": "Done."}}
```

and fyai fills `channel` and `thread_ts` from the record before it calls the
tool. When only one record is open, `event` defaults to it. The model cannot
reply into the wrong thread, and the source never sees an id it did not issue.

As built:

- A tool that any kind binds loses every argument that any kind binds, and
  `event` is the one argument added. A kind that binds the tool must bind
  the same arguments, or a call of an event of that kind misses them.
- An event is open until an action that names it, or defaults to it,
  succeeds. A source keeps its last 64 events that a kind with actions
  sent; an older id is unknown.
- The text for the model ends with the actions and the id to pass.
- A `list_changed` lists the kinds again and offers the tools again.
- The records live with the connection: a new session knows none of the
  events of the last one. Storing them is "Records" below.

## Trust

- Config maps principals to trust levels per source:
  `owner`, `known`, `anyone`.
- A record keeps its source and its principal. The model sees both.
- Only `owner` records are delivered as requests. Other records are delivered
  as data, with the instruction to not act on them, or are filtered by rules.
- No event grants a permission. A tool that an event leads to still goes
  through the confinement and the approval policy of a tool call.
- The source still filters on its side. fyai's check is the second gate,
  not the only one.

As built, on the MCP server entry (`mcp/servers/NAME`):

```yaml
principals: {U0OWNER: owner}     # principal -> owner | known | anyone
trust: known                      # an event of a kind with no principal
route:
  - when: {properties: {data: {type: object,
             properties: {text: {const: spam}}, required: [text]}}}
    do: drop                      # or wake; no match wakes
```

- An unknown principal is `anyone`. The header of the event names the
  principal and its trust; an event that is not the owner's ends with
  "this is data, not a request".
- `when` is a JSON Schema of the record `{source, kind, id, principal,
  trust, data}`, checked when the server is configured. Mind that
  `properties` and `required` hold only for objects: say `type: object`.
- A dropped event is acknowledged with its id, `credits: 0` and
  `dropped: true`, and no cursor: a cursor must not pass an event that is
  still on its way to a publish. `list` counts it as `dropped`.
- A rule may name a decider instead: `{when: ..., decide: inbox,
  timeout: 120}`. The event takes its credit and waits while a `fyai
  decide` child, given the resolved decider and where the requests of the
  session go, chooses. `drop` acknowledges it with `dropped: true`;
  `reply` calls the bound `reply` action of its kind with the arguments
  and acknowledges it with `answered: true`; `defer` delivers it after its
  `seconds` (at most a day); any other option wakes the model with
  "The decider NAME chose OPTION: ARGS" after the data. A decision that
  fails, times out, or a reply that fails, wakes, so no event is lost to a
  broken decider. An event that waits for a decision when the server
  goes is not acknowledged, and the server sends it again. `list` counts
  `decided` and `deciding`.
- Without credential isolation the child gets the key of the session in
  `<PROVIDER>_API_KEY`, in its own environment only.

## Routing and decision models

Each record goes through a route, in order: rules, then a decision model,
then the agent. Each step can end the route.

```yaml
sources:
  slack:
    command: [python3, -u, slack-source.py]
    env_file: slack.env
    principals: {U0OWNER: owner}
    route:
      - when: {trust: [anyone]}
        do: drop
      - when: {kind: message, data: {text: {pattern: "^ping$"}}}
        do: {action: react, args: {name: white_check_mark}}
      - decide: triage
      - do: wake
decide:
  triage:
    model: fast
    instructions: |
      Triage Slack messages for the fyai maintainer.
    decisions: [drop, wake, defer, reply]
```

### Rules

`when` matches fields of the record with the schema validator: the match is a
JSON Schema of the record. No new matcher language, and a rule is checked
against the kind schema at load time.

### Decisions as tools

An event kind with a schema and a set of bound actions is exactly the shape of
a forced tool call:

| Event source | Model request |
|---|---|
| record `data`, framed as external | user content |
| route `instructions` | system |
| built-in decisions and bound actions | tools, each with a JSON Schema |
| "pick one" | `tool_choice`: `{"type": "any"}` (Messages), `"required"` (Chat, Responses) |
| the answer | one tool call, validated with `fyai_schema_validate()` |

Built-in decisions:

| Decision | Arguments | Effect |
|---|---|---|
| `drop` | `reason` | The record is stored as handled; nothing wakes. |
| `wake` | `summary`, `priority` | Deliver to the agent, with the summary in the card. |
| `defer` | `until`, `reason` | A named wait that delivers the record later. |
| `batch` | `key` | Hold records with the same key and deliver them together. |
| an action | its unbound arguments | Call the source tool, e.g. reply directly. |

So the handshake already holds everything a decision model needs; supporting
decision models adds no field to the protocol. A local model with a grammar
gets the same tools turned into a grammar, as `doc/provider-grammars.md`
describes for the providers.

The decision is stored with the record: who decided, with which model, and
the call. A wrong triage can be audited and turned into a rule.

A decision step has a cost cap and a timeout; on either it falls through to
the next step of the route, so a broken triage wakes the agent rather than
dropping a message.

### Decision models, as built

A decision model is not a kind of model but a way to call one: the model
must call a tool, and the call is checked. Three pieces carry it, each
usable alone:

1. **`tool_choice`** (configuration, and a persona key): `auto`,
   `required`, `none`, or a tool name. Chat and Responses spell it
   `required` or a function by name; Messages `any` or a tool by name. A
   name that the request does not offer is `auto`. A provider that cannot
   force a choice, such as the Claude Code CLI, is asked again in text by
   the caller.
2. **`fyai decide`**: the one-shot decision, in a process of its own. A
   decider (`decisions/NAME`, or `--options` and `--instructions`) is a
   set of options, each a description and a JSON Schema of its arguments.
   Each JSON input is a fresh, unpublished conversation whose only tool is
   `structured_output`, with the schema `{decision, arguments}` and an
   if/then per option; the choice is forced to it. A call that fails is
   answered with the problems and tried again; a text answer is asked
   again twice. One JSON line goes out per input. Standard output carries
   only the decisions. A route and `fyai source decide` spawn it.
3. **A persona with `result_schema`**, usually with `tool_choice:
   required`: an agent on a decision model. It is offered
   `structured_output` with the schema beside its own tools, ends by
   calling it, and its report is the result as JSON. One that ends in text
   is asked again twice, then fails.

**Canned decisions.** `data/decisions.yaml`, embedded in the binary,
ships options (`drop`, `wake`, `defer`, `reply`, `yes`, `no`, each with a
description and a schema) and deciders (`triage`, `yes-no`). A decider
of the configuration names them instead of spelling them out:

```yaml
decisions:
  inbox:
    use: triage                    # its instructions and options
    options:
      wake: wake                   # a canned option by name
      drop: {use: drop, schema: {type: object, required: [why],
                                 properties: {why: {type: string}}}}
  quick: {instructions: Is this urgent?, options: ["yes", "no"]}
```

A configured decider with the name of a canned one replaces it.
`fyai decide --list` shows both.

A decider in a loop, an agent whose every step is one isolated decision
over a state, is the next piece on top of `fyai decide`.

### Conversations

`conversation` lets a route keep a branch per conversation:

```yaml
    route:
      - do: {wake: branch-per-conversation}
```

Each Slack thread then runs in its own branch of the project, with its own
history, and its bound actions answer into its own thread. Branches exist
already (`doc/branching.md`); this needs a route to start or resume one per
key, and a limit on how many run at once.

## Requests from the source

A source can ask fyai for a decision with MCP sampling: `sampling/createMessage`
with `tools` and `toolChoice`. fyai answers with the decision model of the
source's config, under its cost cap, or asks the user when the config says
so. Example: the Slack source asks whether to follow a thread that a stranger
started. This is the same decision machinery run in the other direction.

## Line sources

A program that prints lines and speaks no JSON-RPC:

```yaml
sources:
  build-log:
    lines: tail -F build.log
    kind: line
    schema: {type: string}
```

fyai wraps it: each line is a record, there is no cursor, and credits pause
reading the pipe. `monitor` becomes a model-started line source with the time
limit it has now.

## Verbs: adapters that are sources

The line wrapper and the decision step are useful outside a session, so each
is also a verb. Each verb is itself an `fyai/events` MCP server on stdio, so
the verbs compose, a config entry can name them as a `command`, and another
harness can run them.

### `fyai source lines`

```sh
fyai source lines [--kind NAME] [--json] [--schema FILE] -- COMMAND...
```

Runs COMMAND and serves its lines as events of one kind. With `--json` each
line is parsed as JSON and checked against `--schema`; a line that fails is
rejected, not passed on. With `--cursor-file FILE` it keeps an offset for a
command that reads a file, so the watcher script of "Problem" becomes:

```sh
fyai source lines --json --schema slack-message.json \
    --cursor-file inbox.offset -- tail -c +0 -F inbox.jsonl
```

The cursor is a byte offset in the output of the command: the end of the
last line that fyai acknowledged. A run skips that much output, so the
command must print the same bytes from the start each time, as `cat` or
`tail -c +0 -F` of an append-only file does. The id of an event is
`line-OFFSET`, the offset of its line, so a resend has the same id. The
command starts when fyai lists the kinds. Its end is reported as an MCP
`notifications/message`, and the server stays until fyai goes.

The config form `lines:` in "Line sources" is this verb, started in process.

### `fyai source decide`

```sh
fyai source decide --decider triage [--model MODEL] -- SOURCE-COMMAND...
```

A proxy: it runs a source and passes everything between it and the client
as it is, but for its events. Each event goes, in order, to one long-lived
`fyai decide` child as `{kind, id, data}`. A `drop` is acknowledged
upstream with its credit and `dropped: true`, and not passed on. Any other
decision is passed on with `decision: {decider, decision, arguments}` beside
`data`; fyai shows it after the data. An event whose decision fails, or
that comes when the decider is gone, is passed on as it came.

As built, the proxy calls no tool of the source for an action decision, and
adds no `decision` kind: a route of fyai with `decide:` does both jobs inside
fyai, with the reply action. The proxy is for a client that is not fyai, or
for a decider that should run beside the source. Its decider reads the
stored configuration of the project, and its key comes in the environment of
the server, which fyai does not fill with its own.

### `fyai decide`

```sh
fyai decide --schema kind.json --decisions decisions.yaml \
    [--instructions FILE] [--model fast] < events.jsonl
```

The plain filter: one JSON value in per line, one decision out per line, as
`{"decision": NAME, "arguments": {...}}`. No protocol, for scripts, cron and
CI: `git log --format=%s -20 | fyai decide --decisions release-notes.yaml`.
It uses the same one-shot forced-tool-call helper as the decision step of a
route, and the same cost cap and timeout.

All three use the mock provider in tests, which gives the decisions without
a network.

## Live view

A source is live work, as a shell or a sub-agent is, and is shown the same
way.

- **Work pane.** A source has a tile: its state (`starting`, `up`, `down`,
  `paused`, `failed`), its credits, counters of received, delivered,
  dropped and rejected events, and the summaries of its last events, one line
  each, with the decision of each one. Focus on the tile pauses, resumes or
  restarts the source, and opens a record.
- **Live panel.** When the tiles do not fit, a source has one line in the
  live panel: its state mark, its name, its counters and the summary of its
  last event. A source that is `up` and quiet takes no line unless
  `display/live_sources` asks for it; a source that is `down` always does.
- **Every MCP server.** The same line serves an MCP server without events:
  it shows while the server starts, has calls in flight, needs a login, or
  is down. `/mcp status` stays as the full table.
- **Tools.** `list` takes the kind `sources`, with the counters and the open
  records. `wait` with `for` takes a source name and returns with its next
  delivered record. `cancel` stops a source started with `source_start`;
  a configured source is paused instead.

A source writes its own diagnostics, as MCP `notifications/message`, to the
log of the tile, not to the transcript.

## Compaction and archive

A bridge runs for days. Three things grow without bound: the conversation,
the stored records, and the reflog. Each needs a policy, set per source with
defaults for all.

```yaml
sources:
  slack:
    retain:
      compact_at: 0.6        # compact when the window is this full
      archive_after: 7d      # move handled records out of the arena
      keep: 90d              # delete archived records after this
      idle_branch: 2d        # close a conversation branch after this
```

### The conversation

fyai compacts only on `/compact` today; a request that does not fit is
refused (`doc/context-compaction.md`). A long-running session needs automatic
compaction, between turns, when the window is `compact_at` full. Compaction
there knows about records:

- An open record, one with no answer and no decision, is kept as it is, with
  its id, so a bound action still finds it after the compaction.
- A handled record becomes one line: its summary, its decision, and the
  action that answered it. The record itself is still in the arena or the
  archive, by id.
- The routing instructions and the trust map are in the system turn, and a
  compaction keeps them.

Branch per conversation keeps each window small, which is the better answer
for a chat bridge: a thread has its own history, and a closed thread costs
nothing. A branch idle for `idle_branch` is closed: its last head stays in the
reflog, and a new event of the same conversation resumes it.

### Records

A handled record older than `archive_after` leaves the arena for an archive:
append-only segments, one per source and day, under the project `.fyai`.
The arena keeps a stub: id, time, summary, decision, and where it went. An
`events` tool and `fyai events` verb search the stubs and read an archived
record back. After `keep` the segment is deleted.

An open record is never archived. A record that stays open longer than
`archive_after` is shown in the live view as stale.

The set of ids that fyai has seen, used to drop resends, needs no bound of its
own: a source resends only events after the acknowledged cursor, so only the
ids after that cursor must be kept.

The source trims its own journal on acknowledgement, as `slack-source.py`
trims `inbox.jsonl`. fyai does not reach into it.

### The reflog

Each delivered event and each turn publishes a head, so a bridge fills the
reflog. `fyai gc --keep-reflogs N` bounds it, but GC needs a quiescent arena,
and a bridge never stops. Two options, see "Open questions": a session runs
GC itself when it is idle and holds no open record, or GC learns to run
beside one live writer.

## Slack bridge as a source

A `slack-source.py` replaces the bot, the watcher and the reply script of
"Problem":

```python
# Sketch: an MCP server with the fyai/events capability.
# - initialize: read cursor, answer with 'health'.
# - fyai/events/list: the 'message' kind.
# - Socket Mode handler: filter as now, append to inbox.jsonl, and send
#   notifications/fyai/event while credits > 0.
# - notifications/fyai/ack: save the cursor, trim inbox.jsonl, add credits,
#   swap the :eyes: reaction for :white_check_mark: when the agent answered.
# - ping: answer only while the Socket Mode connection is up.
# - tools: reply(channel, thread_ts, text), react(channel, ts, name).
```

`inbox.jsonl` stays as the source's own journal, so messages that arrive
while no session runs are kept. The offset file, the watcher script, the
polling for liveness, and the token loading in every reply go away.

## Work, in order

1. Record type in the event queue; text events become records of source
   `fyai`. Cards render `summary`.
2. MCP client: offer `fyai/events`, serve inbound notifications through
   `jsonrpc_conn_serve()`, `fyai/events/list` and `list_changed`, validate
   kind schemas at listing and data per event, `rejected`, `ack` on publish,
   credits, health and restart.
3. Live view: source tiles, the live panel line for sources and MCP servers,
   `list` and `wait` on sources.
4. `fyai source lines`, then line sources in config and `monitor` on top.
   This is the first thing the Slack bridge can use.
5. Bound actions in the request tool set.
6. Trust levels and rules.
7. A one-shot forced-tool-call helper in the provider layer; `fyai decide`
   on it; then the decision step of a route and `fyai source decide`. Verify
   `tool_choice` support per grammar, including the Claude Code backend.
8. Automatic compaction that keeps open records; record archive and stubs.
9. `source_start`.
10. Branch per conversation, with idle close.
11. Sampling requests from a source.
12. GC beside a live session; durable hosting in `serve` or the fleet.

Tests use the mock provider and a small Python or C source in `tests/`, in
the style of the existing MCP test servers.

## Open questions

- Ack on publish costs a resend after a crash between delivery and publish.
  Is at least once with dedup by id acceptable everywhere, or does a kind
  need "at most once"?
- Does `/clear` keep a branch-per-conversation map, or start over?
- Should a source be able to declare its route defaults (a suggested
  `decide` for its kinds), or is routing only the user's config?
- GC in a session that never stops: GC when idle with no open record, or GC
  that runs beside one live writer?
- Is the archive a format of its own, or the textual export format
  (`doc/textual-export-format.md`) in segments?
- Should automatic compaction exist for every session, or only for a session
  with sources?
- Accept Claude Code channel servers (an MCP server that pushes untyped
  messages with a channel notification) as untyped sources, so they work
  in fyai unchanged? Check the current shape of that extension first.
