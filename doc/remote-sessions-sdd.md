# Software Design Document: Remote sessions

**Status:** design. No part is implemented.

**Scope:** remote observation and control of fyai sessions: an endpoint in an
interactive session, a `fyai serve` process that owns several sessions, the
Agent SDK protocol with fyai extensions, pairing, interactive terminals, screen
mirroring, and the desktop and mobile clients.

**Related design:** `doc/fyai-web.md` (the client), `doc/claude-sdk-compat.md`
(the base protocol), `doc/acp.md`, `doc/agent-protocol.md`,
`doc/agent-transport-isolation-sdd.md` (the threat model and credential
isolation), `doc/declarative-state.md` (configuration access),
`doc/exchange-scope.md`, `doc/events.md`,
`doc/display-output-semantics.md`.

| Topic                                   | Section |
| --------------------------------------- | ------- |
| What exists                             | 2       |
| Guarantees and threat model             | 3       |
| Processes: `--remote` and `fyai serve`  | 4       |
| Transports and framing                  | 5       |
| Pairing and authentication              | 6       |
| Protocol and extensions                 | 7       |
| Terminals                               | 8       |
| Screen mirroring                        | 9       |
| Input and commands                      | 10      |
| Configuration access                    | 11      |
| Engine changes                          | 12      |
| Clients                                 | 13      |
| Failure and lifecycle                   | 14      |
| Sequence and acceptance gates           | 15      |
| Open decisions                          | 16      |

## 1. Purpose

A user starts work in a terminal and continues it from a browser, a desktop
window or a phone. Or the user starts a session with no terminal and controls
it only from a client. The client shows the same session as the terminal does:
the transcript, the running tools, the todo list, the sub-agents, the questions
that wait, and the terminals of the work pane. The client can type, answer,
approve, cancel, and work in a terminal.

Goals:

1. Attach to a running interactive session and detach without effect on it.
2. Run sessions with no terminal in one `fyai serve` process.
3. Use one protocol for the web page, the desktop app, the mobile app and the
   Agent SDKs: the Claude Agent SDK stream-json protocol, with fyai extensions
   that a client asks for.
4. Show each state as data, not as terminal cells, so that a client renders it
   for its own screen. Also mirror the terminal screen as it is, for a client
   that asks for that.
5. Keep every rule of `CLAUDE.md`: no daemon, canonical state in the arena,
   credentials never leave the credential boundary.

Non-goals:

- A hosted relay or rendezvous service. The user brings the network: loopback,
  SSH forwarding, Tailscale, WireGuard, or a LAN.
- Push notifications to a device that is not connected.
- Several users. A session belongs to the user that started it, and every
  paired device acts as that user.
- A process that outlives the invocation that started it.

## 2. What exists

| Protocol                     | Transport             | Process model                         | Exposes                                                     | Does not expose                                         |
| ---------------------------- | --------------------- | ------------------------------------- | ----------------------------------------------------------- | ------------------------------------------------------- |
| ACP (`fyai acp`)             | stdio, JSON-RPC       | a front process; a worker per prompt  | turns, text chunks, tool completion, cancel, session list   | approvals, `ask_user`, todos, terminals, diffs           |
| Agent SDK (`fyai claude-sdk`)| stdio, NDJSON frames  | one process, one session              | turns, stream events, `can_use_tool`, hooks, model, interrupt | `ask_user`, todos, terminals, attach to a running session |
| `fyai agent --rpc`           | stdio, JSON-RPC       | one transient run                     | one task and its report                                     | everything live                                          |
| Tool child and `agent/*`     | descriptors 3 and 4   | parent and forked child               | `shell/output`, `shell/write`, `tty/resize`, `tool/gate`, `user/ask`, `agent/event`, `agent/control` | an external endpoint |

The richest live vocabulary is internal: the parent and its tool children and
sub-agents (`doc/agent-protocol.md`). It shows that the engine can already
stream terminal bytes, resize a terminal, forward a question and gate a call.
It has no external endpoint.

fyai has no listener for a client. The two loopback listeners are the mock
provider (`src/fyai_mock.c`) and the OAuth redirect catcher
(`oauth_listen`, `src/fyai_oauth.c:417`). Each `AF_UNIX` socket is an
anonymous pair inside one invocation. A second process cannot learn that a
session runs; it sees only what the session publishes to the arena.
`doc/events.md` lists the same gaps: no WebSocket source and no event feed of a
live session.

The web client (`doc/fyai-web.md`) works around this with a Node bridge that
starts one `fyai claude-sdk` process for each turn. Between turns no process
exists, so the page cannot see a running turn after a reload, and it cannot
show a terminal.

## 3. Guarantees and threat model

### 3.1 Guarantees

1. **The endpoint lives with its invocation.** A listener belongs to the
   process that opened it, and it closes when that process exits. No process
   survives the invocation (`doc/exchange-scope.md`, rule 5). `fyai serve` is an
   invocation that the user starts and stops.
2. **The arena stays canonical.** A remote client changes state only through
   the operations that a local user has: a turn, a command, an answer. These
   publish through the normal CAS path. The runtime directory of section 4.4
   holds no canonical state.
3. **Credentials stay at the boundary.** A remote client never gets a provider
   key, an OAuth token, a device token of another device, or a secret value.
   Under credential isolation, the session has no key to give
   (`doc/agent-transport-isolation-sdd.md`).
4. **An approval comes from a user.** An answer, an approval or a settings
   review is a user decision only when it comes from the local terminal or a
   paired device with the `approve` scope. A model, a sub-agent, `--answer` or
   a tool is never an approver (`doc/declarative-state.md`, part 2).
5. **The first answer wins.** A question shows on every attached client and on
   the terminal. The first answer settles it, and every other copy is
   withdrawn.
6. **A remote client cannot weaken confinement.** Configuration access is
   stated in the schema (section 11). A key that weakens confinement, runs a
   program, sends data out or names a credential is never writable from a
   remote client.
7. **A slow client does not stop a session.** A client that cannot keep up
   loses its deltas and resynchronizes from a snapshot. The session never
   waits for it.

### 3.2 Adversaries

| Adversary                                | Defence                                                                          |
| ---------------------------------------- | -------------------------------------------------------------------------------- |
| A web page in a browser of the same user | no wildcard CORS; an origin check; a token that a page cannot read               |
| A process of the same user               | a Unix socket in a 0700 directory, `SO_PEERCRED`; a token for each connection    |
| A host on the network                    | TLS with a pinned certificate; a device token; no plain TCP outside loopback     |
| A stolen device                          | revocation by device; scopes; tokens kept in the keyring of the device           |
| A model that wants more authority        | the model never sees the remote surface; `x-fyai-remote` and `x-fyai-model` are separate |
| A paired device with fewer scopes        | each request is checked against the scopes of its connection                     |

A process of the same user can read the memory of fyai with `ptrace` where the
kernel allows it. This design does not defend against that. The transport SDD
states the same limit.

## 4. Processes

### 4.1 An interactive session that serves itself

`fyai --remote` or `/remote on` opens a listener in the running interactive
session. `/remote off` closes it and ends each connection. `/remote status`
lists the connections, the devices and their scopes.

The listener is a source of the event loop of the invocation
(`fyai_event_add_fd()`, as the OAuth listener does). Each connection is a
source too. The session that serves itself is a session of `fyai serve` with
one member and a local terminal (section 4.2). The two share the code.

### 4.2 `fyai serve`

`fyai serve` is a process with no terminal UI that owns N sessions. A session is
a branch with its engine context.

```text
fyai serve                                   (front: listener, registry, fan-out)
  ├── session sdk/a   ── turn worker ──► transport (credential isolation)
  ├── session sdk/b   ── turn worker ──► transport
  └── session main    (idle: no worker)
```

- **Front.** The front owns the listener, the connections, the session
  registry and the fan-out of events. It runs no model request and no tool.
- **Workers.** A turn of a session runs in a worker, as ACP runs a prompt
  (`acp_spawn_worker`, `src/fyai_acp.c:394`). Unlike ACP, a worker of serve lives
  for as long as its session is active: it holds the work pane, the terminal
  sessions, the monitors and the waits. A worker is an fyai process that the
  front starts with `fork` and `exec` and admits to the credential transport.
  Thus one session cannot block another, and a crash of one worker ends only
  its session.
- **Channel.** The front and a worker speak the same extended SDK protocol over
  a socket pair. The front adds the `session_id` and routes the frames. A
  worker never sees a client connection, a device token or a peer address.
- **Registry.** The front keeps a list of live sessions with their branch,
  directory, state and worker. `fyai_branches_refresh()` gives it the
  branches; the registry adds only what is live.
- **End.** `fyai serve` stops on SIGINT or SIGTERM, on `fyai serve stop` through
  its socket, or, if configured, after an idle time. It ends each worker as an
  invocation ends its children. A turn in flight is interrupted and keeps its
  completed steps.

The infrastructure of serve - registry, admission, fan-out, workers - is the
base for later work that needs several sessions in one process, such as an
editor integration or a scheduler.

### 4.3 What replaces what

| Today                                         | After                                                         |
| --------------------------------------------- | ------------------------------------------------------------- |
| `web/server/bridge.mjs` spawns a turn process | the page connects to `fyai serve`                              |
| Electron runs the bridge as a child           | Electron runs `fyai serve` as a child and connects to it      |
| `fyai claude-sdk` on stdio                    | unchanged; it is one session on standard input and output     |
| `fyai acp` on stdio                           | unchanged (section 16)                                        |

### 4.4 Rendezvous

A client on the same host finds the live sessions in a runtime directory:

```text
$XDG_RUNTIME_DIR/fyai/            mode 0700, else ~/.fyai/run/ (mode 0700)
  <pid>.sock                       Unix socket of one serving process
  <pid>.yaml                       {pid, started, kind: serve|interactive, cwd,
                                    sessions: [{id, branch}], tcp: ADDR?, fingerprint?}
```

- A process writes its record after it binds its socket, and removes both at
  exit. A record whose process does not exist is stale. A reader removes it.
- The record describes a process, not canonical state. It holds no secret.
- `fyai remote list` reads the directory. Electron reads it to offer the
  running sessions.
- A forked child does not own the listener: `fyai_ctx_fork_disown()`
  (`src/fyai_tools.c:5670`) drops the listener, the connections and the record,
  and closes their descriptors without a shutdown on the wire.

## 5. Transports

### 5.1 Kinds

| Transport           | Use                                | Authentication                                    |
| ------------------- | ---------------------------------- | ------------------------------------------------- |
| stdio               | `fyai claude-sdk`, an SDK          | the owner of the pipes                            |
| Unix socket         | Electron, `fyai remote`, local tools | `SO_PEERCRED` uid, and a token                  |
| TCP on loopback     | a browser on the same host         | a token; an origin check                          |
| TLS over TCP        | a device on the network            | a device token; a pinned certificate              |

Each TCP connection carries HTTP. A request to `/` serves the page files when
`fyai serve` serves them (section 16). An upgrade on `/api/fyai/session` opens
a WebSocket. Each WebSocket text message is one frame.

A plain TCP listener outside loopback is refused. The listener address comes
from `remote/listen`, which a remote client cannot change.

### 5.2 Framing

The frames are the NDJSON frames of the Agent SDK protocol
(`doc/claude-sdk-compat.md`). One frame is one JSON object.

- On stdio and on a Unix socket, the frames are lines, carried by the raw-frame
  mode of `jsonrpc_conn_stdio()` (`src/fyai_jsonrpc.c`), as
  `fyai_claude_sdk.c` uses it today.
- On a WebSocket, a text message is one frame. A binary message carries
  terminal and screen bytes (section 8.3).
- The frame limit stays `JSONRPC_FRAME_MAX` (16 MiB).

The dispatcher of the SDK front (`sdk_frame`, `src/fyai_claude_sdk.c:2147`) does
not depend on a connection kind. It gets frames and a reply function. Thus
stdio, a Unix socket and a WebSocket share it.

### 5.3 Flow control

Each connection has an output queue with a limit in bytes.

- A frame of the snapshot or of a stored transcript waits for room.
- A live delta that does not fit removes the deltas of its subscription that
  wait, and marks the subscription as lost. The client then gets one
  `fyai`/`resync` frame and asks for a new snapshot (section 7.4).
- Terminal bytes use a credit window of 256 KiB for each terminal, as the
  credential transport does (`doc/agent-transport-isolation-sdd.md`, 5.4). A
  terminal without credit drops its bytes for that client and sends a fresh
  screen when credit returns. The program in the terminal is never stopped.

## 6. Pairing and authentication

### 6.1 Tokens

| Token          | Lifetime                    | Where it is kept                                                        |
| -------------- | --------------------------- | ----------------------------------------------------------------------- |
| launch token   | one process                 | memory; given to Electron through a descriptor or its standard output    |
| pairing code   | five minutes, one use       | memory of the serving process                                           |
| device token   | until revoked               | host: hash only, in the auth store; device: keyring or secure storage    |

The host keeps the device list in the auth store (`fyai_auth_state_path()`,
`src/fyai_auth_util.c`): a file of mode 0600, written atomically under its lock.
Each entry has the device name, the hash of its token, its scopes, and the
times of pairing and last use. The arena holds no token and no hash.

### 6.2 Pairing

1. The user runs `/remote pair` or `fyai remote pair [--scopes LIST]`.
2. fyai shows a QR code and a text code. The code holds the address, a one-use
   pairing secret and the fingerprint of the TLS certificate.
3. The device connects, checks the fingerprint, and sends the pairing secret
   and a device name.
4. fyai shows the device name and the scopes on the terminal and asks the user
   to confirm with `fyai_ui_ask()`. A session with no terminal accepts a
   pairing only from a connection that already has the `pair` scope.
5. fyai sends the device token one time. The device stores it.

### 6.3 TLS

`fyai serve` makes a self-signed key and certificate the first time a TLS
listener opens, and keeps them in the auth state directory. The certificate
does not change, so the pinned fingerprint stays valid. `fyai remote rekey`
replaces it and invalidates all pairings.

### 6.4 Scopes

| Scope       | Allows                                                                 |
| ----------- | ---------------------------------------------------------------------- |
| `observe`   | snapshots and deltas of every extension; screen mirror without keys    |
| `prompt`    | user lines, cancel, the commands that a remote client may run         |
| `approve`   | answers to questions; `can_use_tool` decisions; settings reviews       |
| `terminal`  | keys and resize for terminals and for the screen mirror; bang sessions |
| `configure` | `config set` on keys whose remote access is `write`                    |
| `pair`      | pair another device                                                    |

`fyai remote devices` lists the devices. `fyai remote revoke NAME` removes one
and ends its connections.

### 6.5 Browsers

- A page served by fyai gets the launch token or the device token in a cookie
  that is `HttpOnly`, `SameSite=Strict`, and `Secure` on TLS.
- A WebSocket upgrade must have the origin of the server, else fyai refuses it.
- No response has `access-control-allow-origin: *`.

## 7. Protocol

### 7.1 Base

The base protocol is the Claude Agent SDK stream-json protocol that `fyai
claude-sdk` speaks. The stock Python and TypeScript SDKs work over a Unix
socket or a WebSocket with no change, and get the stock frames.

### 7.2 Negotiation

The client asks for extensions in the `initialize` control request:

```json
{"type": "control_request", "request_id": "1",
 "request": {"subtype": "initialize",
             "fyai": {"version": 1, "want": ["sessions", "transcript", "panels"]}}}
```

The response grants a subset:

```json
{"type": "control_response",
 "response": {"subtype": "success", "request_id": "1",
              "response": {"...": "stock fields",
                           "fyai": {"version": 1, "granted": ["sessions", "transcript", "panels"],
                                    "scopes": ["observe", "prompt"]}}}}
```

- A client without the `fyai` member gets the stock protocol only.
- The server grants an extension only when the scopes of the connection allow
  it, and names the scopes in the response.
- `system`/`init` repeats the granted set.
- `version` is the version of the extension set. A server that cannot speak
  the version of the client answers with the highest version it has, and the
  client decides whether to go on.

`sdk_control()` (`src/fyai_claude_sdk.c:1918`) reads the `fyai` member in the
`initialize` branch, beside `hooks` and `agents`.

### 7.3 Frames of the extensions

- An event from the server is `{"type": "fyai", "subtype": "...",
  "session_id": "...", "seq": N, ...}`. A stock SDK ignores a frame type that
  it does not know.
- A request from the client is a `control_request` with a subtype `fyai_*`.
  The answer is a stock `control_response`.
- A request from the server to the client, such as a question, is a
  `control_request` with a subtype `fyai_*`, as `can_use_tool` is.
- Each frame of an extension has a `session_id`. A connection to `fyai serve`
  can follow several sessions at the same time.

### 7.4 Snapshots, deltas and resynchronization

Each extension that has state uses the same model:

1. The client subscribes: `fyai_subscribe {session_id, ext, from?}`.
2. The server sends a snapshot: `{"type": "fyai", "subtype": "EXT.snapshot",
   "seq": N, ...}`.
3. The server sends deltas with `seq` N+1, N+2, and so on.
4. After a reconnect, `from: N` asks for the deltas after N. If the server
   still has them, it sends them. If not, it sends a new snapshot.
5. `fyai`/`resync` tells the client that its deltas were lost (section 5.3).

The sequence is per session and extension. The server keeps a short ring of
recent deltas for each subscription kind, so a reconnect after a short
network loss costs no snapshot.

### 7.5 Extensions

| Extension    | State                                                                                         | Source in the engine                                                                       |
| ------------ | --------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------ |
| `sessions`   | live sessions of the process; create, attach, detach, close                                   | the registry of serve; the branch table                                                    |
| `status`     | model, provider, context, usage, auth, isolation, view, busy state, branch, head              | `fyai_session_status_data()` (`src/fyai_session.h:104`)                                     |
| `transcript` | stored exchanges, paged; the live document                                                    | `display_outputs` of each turn; `fyai_output_append()`, `fyai_output_add_*fragment()`, `fyai_output_finalize()` |
| `tools`      | each call: head, progress, result, outcome                                                    | the `tool_head` fragment; `struct fyai_chrome_spec`; `fyai_tool_progress_emit()`            |
| `panels`     | todos, sub-agents, monitors, waits, named questions, terminal sessions, queued input, log     | `fyai_todo_rows()`, `fyai_agents_rows()`, `fyai_monitors_rows()`, `fyai_waits_rows()`, `fyai_asks_rows()`, `fyai_tools_sessions_data()` |
| `questions`  | each open question; answer; withdraw                                                          | `fyai_ask_normalize()`, `fyai_ui_ask()`, the settings review                               |
| `terminals`  | the terminals of the work pane: list, attach, bytes, keys, size                              | `shell/output`, the agent terminal, `fyai_terminal_view`                                    |
| `screen`     | the terminal of the session as bytes                                                          | the output tap of the UI (section 9)                                                       |
| `commands`   | the commands a remote client may run; completion; structured results                          | `data/commands.yaml`; `fyai_session_completion()`                                           |
| `config`     | the effective configuration with the remote access of each key; change a writable key        | the merged configuration; `x-fyai-remote` (section 11)                                     |

#### Transcript

The transcript is Markdown source with fragments, not rendered rows. The
client renders it with its own Markdown renderer, at its own width.

- `transcript.snapshot` gives the last K exchanges of the branch, each with its
  stored display records: `{tag, markdown, state, fragments}`
  (`src/fyai_output.c`, `fyai_turn_append_display_output()`), and a cursor for
  older ones.
- `fyai_transcript_page {session_id, before, count}` reads older exchanges.
- The live document gives `doc.begin {tag}`, `doc.append {text}`,
  `doc.fragment {kind, start, end, tool?, ok?, cause?}`, and
  `doc.end {state}`. These come from the same points where ACP takes its text
  and its tool updates today (`src/fyai_stream.c:267`, `src/fyai_output.c:314`).
- A command turn of the transcript (`display/command_output: transcript`) is a
  record of kind `command` with its line and output.
- An image of a message is a reference `fyai-media:DIGEST`. The client reads
  the bytes with `fyai_media_get {digest}` (`doc/media.md`).

#### Tools

A call gives `tool.start {id, name, title, command}`, then `tool.progress
{id, text}`, then `tool.end {id, ok, cause, result_preview}`. The title and
command are the fields of `struct fyai_chrome_spec` before rendering. A call
with a terminal also gives `terminal.open` (section 8).

#### Panels

Each panel is a list of rows, the same rows that the verbs give with `--output
json`. A delta replaces the whole list of one panel; the lists are short. The
todo list changes when `ctx->branch_prev` changes, as the page of the
terminal finds it (`src/fyai_ui.c`). The agents list changes with
`fyai_agents_generation()`.

#### Questions

- `question.open {qid, from, questions}` carries the normalized sequence of
  `fyai_ask_normalize()`: the header, the text, `multi_select`, and the options
  with label, description and preview.
- A settings review also carries `proposal` and `diff`.
- The client answers with `fyai_answer {qid, replies: [{selected: [i...],
  other?}]}`, which maps to `struct fyai_ask_reply`.
- `question.closed {qid, by}` tells every client that the question is settled
  or withdrawn.
- A `can_use_tool` request is the stock frame. A client without the
  `questions` extension still gets the stock approvals.

### 7.6 Example

```text
client                                            fyai serve
  │ control_request initialize {fyai: want[...]}    │
  │────────────────────────────────────────────────►│
  │◄────────────────────────────────────────────────│ control_response {fyai: granted[...]}
  │ control_request fyai_session_attach {branch}    │
  │────────────────────────────────────────────────►│ start a worker if none
  │◄────────────────────────────────────────────────│ fyai status.snapshot, transcript.snapshot,
  │                                                 │      panels.snapshot
  │ user {"message": ...}                           │
  │────────────────────────────────────────────────►│ worker: turn
  │◄────────────────────────────────────────────────│ stream_event ... (stock)
  │◄────────────────────────────────────────────────│ fyai doc.append, tool.start, panels.todo
  │◄────────────────────────────────────────────────│ control_request can_use_tool
  │ control_response {behavior: allow}              │
  │────────────────────────────────────────────────►│
  │◄────────────────────────────────────────────────│ assistant, result (stock), fyai doc.end
```

## 8. Interactive terminals

### 8.1 Model

The terminals of the work pane are the sessions of the shell tool, the bang
sessions of the user, the editors, and the screens of direct sub-agents. Each
terminal has one pseudo-terminal, which its tool child owns. The parent gets
its output as `shell/output` (`src/fyai_tools.c:7094`) or, for a sub-agent, from
the agent terminal (`fyai_agent_pty_read`, `src/fyai_tools.c:5099`), and feeds it
to a `fyai_terminal_view`.

A remote client gets the same bytes and runs its own terminal emulator, such as
xterm.js. The client does not get cells.

### 8.2 Attach

1. `fyai_terminal_attach {session_id, terminal}`.
2. The server sends `terminal.snapshot`: the size, the alternate-screen flag,
   the history rows, and the screen rows with their SGR from
   `fyai_terminal_view_rows_sgr()`, followed by the cursor position.
3. The server sends each later output chunk as `terminal.data`.
4. `terminal.exit {code, signal}` ends it. A user-owned terminal stays until the
   user closes it, as in the work pane.

### 8.3 Binary frames

On a WebSocket, terminal bytes are a binary message: a header of 8 bytes (the
terminal number in 32 bits and the sequence in 32 bits) and the data. On a line
transport, `terminal.data` carries the bytes in base64. The client chooses
binary frames in the `terminals` extension request.

### 8.4 Input

`fyai_terminal_input {terminal, data}` needs the `terminal` scope. The server
writes the bytes through the same path as the zoomed tile
(`fyai_tools_zoom_write()`), which sends `shell/write` to the child. What the
program echoes reaches the line log, so it is part of the tool result for a
call of the model, exactly as local input is.

### 8.5 Size

A pseudo-terminal has one size. One party sets it:

- By default, the local pane sets it through its grant
  (`fyai_shell_session_apply_grant()`).
- A client with the `terminal` scope can take the size with
  `fyai_terminal_size {terminal, rows, cols, take: true}`. It then holds the
  size until it releases it, detaches or loses its connection. The local tile
  shows the program in the top left of its grant, and the status row says
  that a remote client holds the size.
- Every other client shows the terminal at the size it has, and scales or
  letterboxes it.

A resize uses the barrier that exists: `tty/resize`, the child drains and
resizes, `tty/resized`, then the parent resizes the view. The server sends
`terminal.size` to each client after `tty/resized`, so a client never applies
a size before the bytes that the program wrote at the old size.

### 8.6 Bang sessions

A client with the `terminal` scope can start a bang session:
`fyai_terminal_open {command}`. The session is user-owned, as
`fyai_tools_bang()` makes it: the model cannot read it, and nothing is
stored. In a session with no terminal UI, the work pane exists without a
display. The tile gets its grant from the client that opened it.

## 9. Screen mirroring

The `screen` extension streams the terminal of an interactive session as
bytes. It is the simplest way to show everything that the terminal shows, and
it needs no knowledge of the state.

- **Tap.** The UI already gives every byte that it writes to an output tap
  (`fytim_set_output_tap()`, used by `ui_cast_open()`,
  `src/fyai_ui.c:3259`). The screen extension adds a fan-out on this tap. The
  `--cast` recorder is one consumer and each mirror is another.
- **Attach.** On attach, the server asks the UI for a full repaint, as Ctrl-L
  does, and sends `screen.size` and then the bytes. The client draws them with
  a terminal emulator.
- **Size.** The local terminal owns the size. A client scales or letterboxes.
  A resize of the local terminal gives `screen.size`.
- **Keys.** With the `terminal` scope and `fyai_screen_input {data}`, the bytes
  go to `fyai_ui_keys_return()` as if the user typed them. This is a full
  remote terminal of the session. Without the scope, the mirror is view-only.
- **Limits.** Only an interactive session with a terminal has a screen.
  `fyai serve` sessions have none, and their clients use the semantic
  extensions. A mirror shows rows at the width of the local terminal, so a
  phone shows them small. The semantic extensions are the primary form for a
  phone.
- **Glyph width.** A mirror client that is not VTE can draw some emoji at
  another width than the local terminal. The local terminal decides the layout.

## 10. Input and commands

### 10.1 User lines

A user frame from a client enters as a line typed at the prompt:

- The routing of `FYTIM_EVENT_LINE` (`src/fyai_ui.c`) gets a public entry point,
  `fyai_ui_input_line(ctx, text, origin)`. It runs the same chain: command
  session input, the browser, agent input, then the queue (`ui_queue()`), and
  then `fyai_ui_wake()`.
- A line queued during a turn enters before the next model request, as a local
  line does.
- In a session with no terminal UI, the same entry point feeds the queue of
  the session directly.
- Each line records its origin (`local` or the device name) in the trace, not
  in the transcript.

### 10.2 Cancel

The stock `interrupt` control request today sets `ctx->interrupt_pending`
(`src/fyai_claude_sdk.c:1965`). A remote cancel needs the same effect as Escape:
a public `fyai_session_cancel(ctx)` that runs the steps of
`fyai_interactive_process_interrupt()` without the dependency on
`interrupt_seq`. A remote cancel does not send `^C` to a focused tile; it
cancels the turn.

### 10.3 Commands

- A command definition gets `remote: true` when a remote client may run it.
  The schema of `data/commands.yaml` gets the key; a unit test checks that a
  command with `interactive` or a terminal-only effect does not have it.
- `fyai_command {session_id, line}` runs the command as the session runs a
  slash command, and returns the structured result of the handler (the
  `fy_generic` result and its `render` hint), not terminal text. The client
  renders a table or a document itself.
- A `mode: stream` command sends `command.item` frames. A `mode: async` command
  can be cancelled.
- `fyai_complete {session_id, line, cursor}` returns the candidates of
  `fyai_session_completion()`.
- A command that is presentation (`history`, `page`, `branches`, pickers) has
  no remote form. The client has its own view of that data.

## 11. Configuration access

### 11.1 `x-fyai-remote`

Each key of `data/config.schema.yaml` gets a remote access, beside its
`x-fyai-model` access:

| Value    | Effect                                                                        |
| -------- | ----------------------------------------------------------------------------- |
| `hidden` | the key is not in the `config` extension                                      |
| `read`   | the key shows with its value; a change from a remote client is refused       |
| `write`  | a client with the `configure` scope can change it                             |

A key with no `x-fyai-remote` is `read`. A group gives its access to the keys
below it, and a key can make it stricter. `fyai_config_remote_access_tree()`
gives the strictest access below a path, as `fyai_config_model_access_tree()`
(`src/fyai_config.h:317`) does for the model.

The rules for a value:

- A key that weakens confinement, runs a program, sends data out, names a
  credential or lifts a limit is `read` or `hidden`: `sandbox_*`, `shell/*`
  programs, `catalog_update/*`, `mcp/*` commands, `api_key`, `remote/*`.
- A display key is `write`, but it changes only the session layer of that
  client (section 11.2).
- A key that shapes requests, such as the model or the reasoning effort, is
  `write`.

`remote_access` names the keys that must stay closed in this configuration,
as `model_access` does for the model. It can only make the schema stricter.

### 11.2 How a change applies

- A remote `config set` on a `read` key is refused. The diagnostic names the
  key and says that its remote access is `read`.
- A change of a `write` key with `x-fyai-scope: session` changes the session
  layer (`fyai_config_session_set()`). All other `write` keys publish through
  the normal configuration commit, with the device name as the operation of the
  ref-log entry.
- A display setting from a client is a setting of that client. It does not
  change the terminal of the session.

### 11.3 Checks

- The schema checker refuses a new key without `x-fyai-remote`.
- A unit test walks the schema and checks that each key with `x-fyai-model:
  read` or `hidden` because it weakens confinement is not `write` for remote.
- `doc/declarative-state.md` gets a section on remote access.

## 12. Engine changes

No code exists for these. Each item names the seam.

| Change                                  | Seam                                                                                                    |
| --------------------------------------- | ------------------------------------------------------------------------------------------------------- |
| Observer beside the sink                | A remote observer gets the document and band events of `struct fyai_sink_ops` in addition to the backend. `fyai_sink_captured()` (`src/fyai_sink.c:1477`) tests the ops by identity and must see the backend, not the observer. |
| Band source                             | `band_paint()` gives rendered rows. The fenced stream (`fyai_fenced_stream`, `src/fyai_markdown.c`) also gives its source to the observer. |
| Questions with no terminal UI           | `fyai_ui_ask()` and `fyai_ui_ask_available()` require `ui->page`. A session with no UI keeps the question queue in a model that does not need a page. |
| Answer from outside the UI              | a public call that settles the head question with replies, beside the static `ui_question_finish()`. |
| Approvals in an interactive session     | `ctx->tool_gate` (`struct fyai_tool_gate_ops`, `src/fyai.h:1063`) for a remote approval mode, as the SDK front uses it. |
| Line input                              | `fyai_ui_input_line()` (section 10.1).                                                                  |
| Cancel                                  | `fyai_session_cancel()` (section 10.2).                                                                 |
| Terminal fan-out                        | the handler of `shell/output` and the agent terminal reader give each chunk to the subscribers of the terminal. |
| Output tap fan-out                      | one tap with several consumers: cast and mirrors.                                                       |
| Session without a terminal UI           | a work pane and a question queue without a display; the grants come from the clients.                   |
| SDK dispatcher                          | `sdk_frame` and its server state take a connection, not standard input and output.                       |
| Fork                                    | `fyai_ctx_fork_disown()` drops the listener, the connections, the subscriptions and the runtime record. |
| Trace                                   | one trace line for each connection, pairing, scope refusal and resync.                                  |

The sink rule stays: a remote observer does not write to the terminal, and
nothing that the terminal shows goes through a path other than the sink.

## 13. Clients

### 13.1 fyai web

The page of `doc/fyai-web.md` speaks the protocol over a WebSocket (work item
W10 of that document). It asks for every extension. It renders the transcript
with its Markdown renderer, the tool calls with its cards, the panels in a side
column, and terminals and the screen mirror with xterm.js.

The page is one build for every server. It has a backend seam
(`web/src/lib/services/backend/`) with the kinds `llama`, `fyai-bridge` and
`fyai-remote`; the last one is this protocol. A page on a host that has no
bridge forces the kind with `?backend=fyai-remote`, the local storage or the
build setting, because it has nothing to probe. The conversations are the
branches of the session already (the bridge lists, reads, renames and deletes
them, under the names of the CLI), so the page keeps none, and a turn from an earlier message of
its tree cuts the branch back first (the anchor of `doc/fyai-web.md`, W5). The
protocol needs an equivalent of that cut, since the page keeps a tree and a
session is one line.

### 13.2 Desktop

The Electron main process starts `fyai serve` as a child, with the launch
token on a pipe, or connects to a running session that it finds in the runtime
directory. It connects over the Unix socket, and gives the renderer a
WebSocket-like channel through a preload script. The renderer keeps context
isolation and has no Node access.

### 13.3 Mobile

The same page, packaged with Capacitor (or a similar wrapper) for iOS and
Android:

- Pairing scans the QR code of section 6.2.
- The device token is kept in the Keychain or the Android Keystore.
- The app connects over the network that the user provides, such as Tailscale.
- With no relay, the app gets no push notification. When it comes to the
  foreground, it reconnects and resynchronizes from its last `seq`.
- The semantic extensions are the primary view. The screen mirror is an
  option.

### 13.4 Agent SDKs

A stock SDK connects to the Unix socket of `fyai serve` with the launch or
device token, and gets the stock protocol. This lets a program drive a long
session without a process for each conversation.

## 14. Failure and lifecycle

| Event                                 | Behaviour                                                                                         |
| ------------------------------------- | ------------------------------------------------------------------------------------------------- |
| Client disconnects                    | the session goes on; a size that the client held goes back to the local pane                      |
| Client reconnects                     | it sends `from: seq` for each subscription; it gets the missing deltas or a new snapshot          |
| Session ends                          | each client gets `session.closed {reason}`; the socket and the record go                          |
| Worker of serve fails                 | the front reports the signal or the missing result, and closes the session                        |
| Lost CAS while a client is attached   | the normal reconciliation; the client gets new snapshots for the branch                           |
| Two clients type                      | the lines enter the queue in arrival order; each line shows its origin in the status of the clients |
| Two clients answer                    | the first answer wins; the others get `question.closed {by}`                                     |
| Slow client                           | its deltas are dropped; it gets `resync` (section 5.3)                                            |
| Pairing secret used twice             | refused; the code is spent after the first use or after five minutes                              |
| Revoked device                        | its connections end at once                                                                       |

A failure on the remote path is a display failure for the session: the
session reports it as a warning and goes on. A failure that stops a request of
a client goes back to that client as an error with its cause, as
`fyai_diag_string()` renders it.

## 15. Sequence and acceptance gates

| Phase | Work                                                                                        | Gate                                                                                                     |
| ----- | ------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------- |
| P0    | secure the web bridge: token, origin and host checks (done, `doc/fyai-web.md` W1) | the bridge refuses a request without the token and from another origin (`web/tests/unit/fyai-bridge-access.test.ts`) |
| P1    | the SDK dispatcher on any connection; negotiation; `fyai serve` with a Unix socket and workers | a stock SDK test over the socket; two sessions in parallel against the mock provider                      |
| P2    | `status`, `transcript`, `tools`, `panels` (observe only); runtime directory                 | a functional case attaches to a session in a turn and gets the same transcript as `history`               |
| P3    | input, cancel, `questions`, `commands`                                                      | a remote line, a remote cancel, a question answered remotely, and a refused command                       |
| P4    | `terminals` and bang sessions                                                               | a PTY case: a remote client types into a shell tile and reads its output; a remote resize                 |
| P5    | `screen` and the `--remote` switch of an interactive session                                | a PTY case: the mirror shows what the local terminal shows                                                |
| P6    | TCP, TLS, WebSocket, pairing, scopes, `x-fyai-remote`                                        | a refused pairing, a revoked device, a scope refusal, a `read` key refused                                |
| P7    | fyai web on the protocol; Electron on `fyai serve`                                           | the e2e tests of `web/` against `fyai serve`                                                              |
| P8    | mobile packaging                                                                            | the page on a device over a paired TLS connection                                                         |

Each phase adds unit tests, functional cases with the mock provider, and PTY
cases where a terminal takes part. The cases follow `CLAUDE.md`: no delay, wait
for state, and hold a reply of the mock with a marker. A protocol test client
is a small mode of `fyai __pty-driver` or a new `fyai __remote-client` that
reads a YAML script of frames and waits on frames.

## 16. Open decisions

1. **Version policy of the extensions.** One version for the whole set, or a
   version for each extension.
2. **Idle exit of `fyai serve`.** Exit after a configured idle time with no
   client and no running turn, or run until stopped.
3. **ACP over the listener.** `fyai serve` could also speak ACP on its socket,
   with the protocol chosen by the first frame.
4. **Input from several controllers.** Lines in arrival order (the design
   above), or a controller lease that one client takes at a time.
5. **Page files.** `fyai serve` serves the built page itself, or a separate
   static server does.
6. **A relay.** Out of scope now. If one is added later, it carries the same
   frames end to end, encrypted between the session and the device, and a
   device pairs with the session, not with the relay.
7. **Edit and regenerate.** The page keeps a tree of messages and a session is
   one line. The bridge today takes the last user message that a turn follows
   (the anchor) and cuts the branch back before it runs the turn. The protocol
   can carry the same anchor in the user message, or have a request such as
   `fyai_rewind {session_id, to}` that a client sends before the turn. The
   request is the better fit for a client that is not a page, but it needs a
   way to name a message, since the branch counts turns and not messages.
8. **Keys requested from the client.** The default is that the remote holds its
   keys, stored securely at the remote, and the client never carries one. A
   second mode would leave the keys off the remote entirely. The credential
   transport of `doc/agent-transport-isolation-sdd.md` already separates the
   process that holds a key from the one that runs the model loop, so it can
   ask for a key when a request needs one. The request goes to the attached
   client as a frame, for example `credential/request {provider, session,
   turn}`. The client answers from its own store, such as the secret store or
   keychain of the device, and the transport process holds the value for that
   request only, never in the arena, the configuration or the runtime
   directory. The mode needs a client that is attached with a scope that may
   answer, a refusal that ends the turn with a cause, and a rule for an
   unattended session (a background sub-agent has no client to ask, so the
   turn waits or fails). It changes guarantee 3 in one direction only: the
   client supplies a key to the session, on demand, and still never receives
   one. Not designed yet.
