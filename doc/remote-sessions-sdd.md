# Software Design Document: Remote sessions

**Status:** P1 Unix-socket transport is implemented: a server in the
foreground or as a daemon, a server of projects, SDK workers, launch-token
admission, a command-line SDK connection, and the global option `--remote`
for commands and the interactive session. Named
branches run concurrently and a disconnected client can reattach to its live
worker. A watcher reads a session that another client drives
(section 6.1.5). Runtime discovery, semantic subscriptions, several clients
that drive one session, interactive terminal clients and network transports
remain design work.

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
5. Keep every rule of `CLAUDE.md`: no hidden resident process, canonical state in the arena,
   credentials never leave the credential boundary.

Non-goals:

- A hosted relay or rendezvous service. The user brings the network: loopback,
  SSH forwarding, Tailscale, WireGuard, or a LAN.
- Push notifications to a device that is not connected.
- Several users. A session belongs to the user that started it, and every
  paired device acts as that user.
- A process that outlives the invocation that started it. A `fyai serve
  --daemon` is itself that invocation: the user stops it.

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

Before P1, fyai had no listener for a client. The two loopback listeners were the mock
provider (`src/fyai_mock.c`) and the OAuth redirect catcher
(`oauth_listen`, `src/fyai_oauth.c:417`). Each `AF_UNIX` socket was an
anonymous pair inside one invocation. A second process could not learn that a
session ran; it sees only what the session publishes to the arena.
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
   invocation that the user starts and stops. `fyai serve --daemon` runs it in
   the background; `fyai serve stop` ends it, and it keeps no canonical
   state.
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
| A process of the same user               | a Unix socket in a 0700 directory, `SO_PEERCRED`; an optional launch token    |
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

A client on the same host finds the project endpoint in `.fyai/unix`.
The server uses `remote.sock`. Future discovery records and interactive
endpoints share this directory:

```text
.fyai/unix/                      mode 0700
  remote.sock                      Unix socket of the project server
  remote.yaml                      server record (planned)
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
| Unix socket         | Electron, `fyai remote`, local tools | `SO_PEERCRED` uid; optional launch token                  |
| ssh                 | a client of another host           | ssh; `SO_PEERCRED` uid at the host                |
| HTTP and WebSocket  | a client or a browser, by `serve --listen` | a bearer token; an origin check           |
| HTTPS and WebSocket | a device on the network            | a bearer token; a pinned key or a CA     |
| TCP on loopback     | a browser on the same host         | a token; an origin check                          |
| TLS over TCP        | a device on the network            | a device token; a pinned certificate              |

Each TCP connection carries HTTP. A request to `/` serves the page files when
`fyai serve` serves them (section 16). An upgrade on `/api/fyai/session` opens
a WebSocket. Each WebSocket text message is one frame.

#### 5.1.1 ssh

`--remote ssh://[USER@]HOST[:PORT]/DIR` runs `ssh -T [-p PORT] -- HOST fyai
remote bridge` on one end of a socket pair. The program is `$FYAI_SSH`, else
`ssh`. The child keeps `SSH_AUTH_SOCK`, `SSH_ASKPASS`, `SSH_ASKPASS_REQUIRE`
and `DISPLAY` and no other variable that `fyai_env_sanitize()` removes. A host
that starts with `-` or holds a character that a host does not is refused, so
the host never reads as an ssh option.

`fyai remote bridge` relays standard input and output to the socket of the
server of the project in its directory, else of the server of projects. It
reads no frame, so the hello, the images and the frames pass unchanged. The
server sees a local client of the same user, and the uid check holds at the
host. ssh authenticates and encrypts: the bridge opens no listener and takes no
token. `DIR` is the project, relative to the projects of the server, as
`--project` is; an ssh target needs one of them.

The client gives the bridge a random `--nonce`. The bridge writes the line
`FYAI-BRIDGE NONCE` before anything else, and the client reads the channel one
byte at a time up to that line, so a banner or the output of a login script
before it is dropped. The line can follow text that has no end, such as a
prompt. The client reads at most 64 KiB before the line. If ssh ends first, the
error has what ssh wrote, which is usually the cause: `fyai` is not on the
`PATH` of the host.

#### 5.1.2 HTTP and WebSocket

`fyai serve --listen HOST:PORT` (or `[HOST]:PORT`, a numeric address; port 0
takes one) opens a TCP listener beside the Unix socket. The log names the
address, `listening on http://ADDRESS/`. `--listen` repeats, and an entry takes
the scheme `http://` (the default) or `https://` (section 5.1.3). A plain http
listener outside loopback is allowed because the user names its address, and
the log then warns that the token and the frames cross the network in the
clear. Use loopback with a tunnel, a proxy that ends TLS, or `https://`.

A connection has ten seconds to send its request head (`REMOTE_ADMIT_MS`). The
head is at most 8 KiB, HTTP/1.1, with CRLF line ends, no folded line and one
value of `Host`, `Authorization`, `Origin` and `Sec-WebSocket-Key`. The server
answers in this order, with no body:

1. 400, 431 or 505 for a head that is not well formed;
2. 401 with `WWW-Authenticate: Bearer` when the bearer token is missing or not
   valid. This comes before any answer about the route;
3. 405 for a method other than GET, and 404 for a path other than
   `/api/fyai/session`;
4. 426 for a request that is not a WebSocket upgrade, or has another
   `Sec-WebSocket-Version` than 13;
5. 403 when `Origin` is present and is not `http://` and the `Host`;
6. 101 with the accept key of RFC 6455.

The token is checked by `struct fyai_remote_auth` before the upgrade, so a
client that fails it never reaches the server code. The verifier of this step
is one shared token: 64 hexadecimal digits in `--token-file`, default
`~/.fyai/serve.token`. The server makes the file with mode 0600 when it is
missing, and refuses a file that group or others can read. The client takes the
token from `$FYAI_REMOTE_TOKEN`, else from the same file under its own home. A
verifier that reads a signed token (JWT) and its scopes uses the same hook
(`struct fyai_remote_identity`).

After the upgrade each text message is one frame, and the server gives the
client to its own code as a socket of lines, so the hello, the requests and the
session are those of a Unix client. The relay is in the event loop of the
server: it joins the pieces of a message, answers a ping, closes with 1002 for
a frame that breaks the protocol, 1003 for a binary message, 1007 for a text
message with a newline in it, and 1000 when the server ends the client. A
message is at most 16 MiB. A client that reads slowly makes the server stop
reading its session, and one that is 32 MiB behind is dropped. At most 256
connections are open. Image mode (section 5.2.1) is not offered over a
WebSocket: the hello answer holds no `image-` feature, and the frames are text.

`--remote http://HOST:PORT/DIR` is the client. It runs `fyai remote relay
http://HOST:PORT` on a socket pair, as ssh runs for an `ssh://` target, so
the client code reads lines from a socket. `DIR` is the project, as for ssh.

The transport has three layers that other code reuses. `src/fyai_net.c` holds
the sockets, `src/fyai_httpd.c` the HTTP server with the server end of a
WebSocket, and `src/fyai_ws_client.c` the client end. `src/fyai_remote_http.c`
and `src/fyai_remote_relay.c` add the token, the route and the lines. The
loopback receiver of the OAuth redirect uses the same server.

#### 5.1.3 TLS

An `https://` listener speaks TLS 1.2 or 1.3 with ALPN `http/1.1`, no
compression and no renegotiation, and carries what 5.1.2 describes. The
handshake is part of the admit window of ten seconds. The `Origin` of a browser
must be the `https://` origin of the server.

The key of the server is an ECDSA P-256 key. `--tls-cert` and `--tls-key` give a
certificate from a CA, as PEM files; the key file is readable by its owner
alone. Else the server uses `~/.fyai/tls/server.pem`, which it makes when it is
missing: the key and then a self-signed certificate valid for ten years, for
`localhost`, the loopback addresses, the host name and each `--tls-name`. It is
one file, so one rename publishes both, and it does not change from one start to
the next, so a pin stays valid. The log gives the fingerprint of the key,
`sha256:` and 64 digits of the hash of its SubjectPublicKeyInfo, and `fyai remote
fingerprint` shows it. `fyai remote rekey` replaces the file: a running server
keeps the old key until it restarts, and every pin of the old key fails.

A client of `https://HOST:PORT/DIR` checks the server one of three ways:

1. `$FYAI_REMOTE_PIN=sha256:HEX` pins the key, as ssh does. Nothing else is
   checked: the name and the issuer are of no use for a key that the user pinned.
   Capitals and colons in the pin are ignored.
2. Else the certificate must chain to the roots of the system, or to the PEM file
   in `$FYAI_REMOTE_CA`, and name the host (an IP address is matched as an IP).
3. A server that passes neither is refused, and the error gives the fingerprint
   that it showed, so the user can pin it after checking it by another route.

Trust on first use, with a file of known servers, is a later step.

### 5.2 Framing

The frames are the NDJSON frames of the Agent SDK protocol
(`doc/claude-sdk-compat.md`). One frame is one JSON object.

- On stdio and on a Unix socket, the frames are lines, carried by the raw-frame
  mode of `jsonrpc_conn_stdio()` (`src/fyai_jsonrpc.c`), as
  `fyai_claude_sdk.c` uses it today.
- Two ends that are both fyai can send images in place of lines (section
  5.2.1).
- On a WebSocket, a text message is one frame. A binary message carries
  terminal and screen bytes (section 8.3).
- The frame limit stays `JSONRPC_FRAME_MAX` (16 MiB).

The dispatcher of the SDK front (`sdk_frame`, `src/fyai_claude_sdk.c:2147`) does
not depend on a connection kind. It gets frames and a reply function. Thus
stdio, a Unix socket and a WebSocket share it.

#### 5.2.1 Images

The frames are generics, so a fyai end can send a frame as the image of its
generic (`fy_generic_image_create()` of libfyaml): the receiver checks and
relocates the image in place, and does not parse text. For a frame of 64 KiB
this takes 5 microseconds in place of 2.5 milliseconds, and the frame is 12%
smaller.

- The feature is `image-ORDERBITS` in the hello (section 6.1.3), for example
  `image-le64`: the byte order and the word size of the sender. A client sends
  it when `jsonrpc_image_feature()` gives one: `FYAI_REMOTE_NO_IMAGE` is not
  set. The server answers with it when its own feature is the same. An end of
  another byte order or word size gets no image feature, and both ends keep
  lines. The reply to the hello is the last
  line: the next frame of each end is an image. The server gives a client to
  a project server with `image` set.
- The connection of a server and its worker is in image mode at once, with no
  hello. The worker moves its channel to a private descriptor and gives
  standard input and output to `/dev/null`, so a stray write cannot break the
  framing.
- A frame is a 64-bit length in native byte order and then the image. The
  limit is `JSONRPC_FRAME_MAX`. The hello has made sure that the peer has the
  byte order and word size of the sender. The magic of the image says it too,
  and an image that differs fails the load.
- A frame that does not load, is not a mapping, or has a bad length ends the
  connection with a diagnostic. The load checks each pointer, the extent of its
  object, the alignment, the end of a string, an escape, and a value that
  contains itself, so an authenticated client cannot make the server read
  outside the image. A hostile client can still send a frame that is valid
  and large: the limits of section 5.3 apply.
- A client that is not fyai, such as an SDK program, sends and gets lines. The
  server sends each frame to a driver and its watchers in the form each reads,
  and makes each form one time.

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

### 6.1.1 Unix-socket admission for P1

The initial command-line surface is `fyai serve` and `fyai remote connect`.
The server creates its endpoint at `.fyai/unix/remote.sock` under the project
root, in a private `unix` directory. The client finds the same endpoint from
the project root or a subdirectory. The server accepts `--socket PATH` and the
client accepts a positional `PATH` to override that endpoint. Both endpoints check that the kernel-reported peer
UID equals their effective UID. The socket is mode 0600. Plain Unix sockets
need no token and accept SDK frames immediately after the UID check.

Both commands accept an optional `--token-fd FD`. On the server, this option
requires launch-token admission for every connection. On the client, it sends
the launch-token admission request before SDK frames. The server generates a launch
token from 32 bytes of cryptographic entropy and writes its hexadecimal text,
followed by a newline, to the supplied descriptor. The client reads that line
from its descriptor. Both close the descriptor after transfer. The token is
never an argument, configuration value, runtime record member, or trace field.
The descriptor must not name standard output, which carries protocol frames.

When token admission is enabled, the server accepts only this first request:

```json
{"type":"control_request","request_id":"auth","request":{"subtype":"fyai_authenticate","token":"<launch token>"}}
```

Successful admission replies with a stock `control_response`, with the same
request ID and subtype `success`. Failed admission closes the connection. Compare equal-length tokens in constant time. An invalid
request, wrong token, oversized frame, or ten-second admission timeout closes
the connection. No worker is started before admission succeeds. Admission grants
the launch owner's scopes; the worker receives neither the token nor the client
socket. Admission parsing uses the existing raw-frame connection and timer
sources. Disable protocol logging on this connection because its first frame
contains a credential. Token admission is retained for future HTTP, TCP and
SSH transports; omitting it applies only to the local Unix-socket endpoint.

After admission, the client sends the normal SDK initialize request. Clients
that manage admission themselves can use the stock SDK frame vocabulary from
this point. The CLI must complete admission before forwarding any SDK frames.
Socket EOF detaches the client. It does not close the worker of a branch
session, which the client can attach again. A session that no branch names
has nothing to attach to: its worker ends with its client. Server shutdown
closes the listener and ends its workers through the existing child
termination path before removing the socket. A socket path where a live
server answers is an error: the server does not unlink the endpoint of
another invocation. A socket where nothing answers is what a server that was
killed left; the next server removes it and binds. Any other file at the path
stays, and the bind fails.

`fyai remote connect` opens the full-screen client. `--protocol sdk` carries
SDK NDJSON on standard input and output instead; it is then a protocol client. The global option `-b NAME` sends `fyai_session_attach {branch: NAME}`
after admission and before forwarding SDK frames. The front creates a worker
with that branch when none is live, or reattaches its existing worker. It
rejects a second simultaneous attachment to the same worker. A worker runs
`claude-sdk --remote-worker`: a reconnect initializes its SDK connection again,
and the worker answers each initialize. The worker reports its branch to the
front in a `fyai_worker` frame that no client sees, so a checkout on the
worker also changes the name a reconnect attaches by. Output produced while
detached is drained, with no replay in P1. Extensions are not granted until
their full subscription semantics are available.

The end of standard input ends the requests of the CLI, as it does for the
Claude Code CLI, not the session: the CLI forwards the answers that follow,
and closes its side of the socket when each user message has its result.

For example, from the project directory:

```sh
fyai serve
# In separate terminals: the full-screen client, and SDK frames on stdin.
fyai -b work/a remote connect
fyai -b work/b remote connect --protocol sdk
```

To require a token, start the server with `--token-fd 3` and pass descriptor 3
to it. Each client then supplies the token through its own `--token-fd`
descriptor. A launcher can use pipes or a private token file (`umask 077`).
Neither command prints the token.

### 6.1.2 The global option `--remote`

A client runs a command line at a server with the global option
`--remote TARGET`. It keeps `--remote TARGET` and `--project DIR` for itself,
and runs the hidden command `remote run`, which opens no arena. The words that
getopt consumed for those options are not forwarded; every other word is.

A TARGET other than `.` is the socket of a server. For `.`, the endpoint is
that of the project of the current directory when a socket is there and
`--project` is not given; else `~/.fyai/unix/serve.sock`, the server of
projects. After admission, the client says hello (section 6.1.3) and names the
directory it works in:

```json
{"type":"control_request","request_id":"project","request":{"subtype":"fyai_project","path":"<DIR or the current directory>"}}
```

A relative path is relative to the directory that the server serves. A project
server accepts a directory in its project and answers `{root, dir}`. A server
of projects accepts a directory under its directory of projects: it finds the
project root of the directory, starts the project server of that root when
none runs (`fyai serve start --front-fd 3` in the root), and sends it the
client socket with `SCM_RIGHTS`, with `{client, dir, request_id}` as the
message. The project server answers the request, so the client sends nothing
until it has the answer, and no byte of the connection stays in the front. A
project server ends when the channel of its front closes. Without
`fyai_project`, a project server serves the client in its own directory.

A command line, or a prompt on standard input, is a command:

```json
{"type":"control_request","request_id":"run","request":{"subtype":"fyai_run","args":["branch","list"],"stdin":false,"cols":120}}
```

The server starts `fyai --sink-frames ARGS` in the directory of the client,
with `COLUMNS` set to `cols`, in the environment of the server. Its sink
(`src/fyai_sink_frames.c`) presents nothing and sends each operation on its
standard output:

- `{type: fyai, subtype: display, config}` comes first: the `display`
  settings of the command. The client applies them to its own configuration
  for this run, so it renders as a local run of the command does.
- `{type: fyai, subtype: sink, op, stream, data, kind, aborted}` is one
  operation: `markdown` and `write` on a stream, and `doc_begin`,
  `doc_append`, `doc_end`, `doc_discard`, `doc_pause` and `doc_resume`. The
  client gives each to its own sink.

The server sends the standard error of the command as
`{type: fyai, subtype: stderr, data}`, which the client writes on its
diagnostic stream. When the command ended and both of its outputs closed, the
server sends `{type: fyai, subtype: exit, status}`; the status is the exit
status of the client. The client sends standard input as
`{type: fyai, subtype: stdin, data}` and `{..., closed: true}`, and `^C` as
`{type: fyai, subtype: signal, signal: INT}`. A pipe or a socket is sent as it
comes, a file whole; a terminal or a device gives the command no input. A
command of a client that goes is ended.

With no command line and a terminal, the session is interactive.
`fyai_session_attach` then carries the global options of the client as `args`:
the server starts the worker with them after its own. Without `branch`, it
starts a session on a new branch. `resume` is interactive too: the client
sends its arguments as `resume: {session, last, all}` in place of `branch`,
and the server selects the branch as `resume` does here: the session it
names, or with `last` the session updated last that started in the directory
of the client (every directory with `all`). A `resume` without either is
refused, because the picker is not remote.

`{subtype: fyai_server_status}` answers the state of the server, and
`{subtype: fyai_server_stop}` also stops it: `fyai serve status` and
`fyai serve stop`.

The server writes one line on its status stream for each connection,
refusal, directory, project server, worker, command and exit, with the time
and its process. `fyai serve --daemon` forks before the arena and the
credential transport exist. The child starts a session of its own, writes the
log (`--log FILE`, else `.fyai/logs/serve.log` of the project, or of the home
for a server of projects), and writes its process and endpoint on a pipe when
it serves. The parent waits for that line, presents it as its result, and
ends; without the line, it names the log.

### 6.1.3 Sessions: fyai and SDK

A connection is an SDK session or a fyai session. A stock Claude Agent SDK
client sends no fyai request, and its session is an SDK session: it keeps the
protocol and the defaults of the SDK, such as the permission mode `default`
and `can_use_tool`, and it can attach to a branch with
`fyai_session_attach {branch}`. fyai must stay compatible with such a client.

A fyai client says hello after admission, before any other request:

```json
{"type":"control_request","request_id":"hello","request":{"subtype":"fyai_hello","version":"0.4.2","protocol":1,"features":["project","run","frames","session","resume","server","model"]}}
```

`protocol` is `FYAI_REMOTE_PROTOCOL` (`src/fyai_remote.h`). A change of a
request, an answer or a frame of a fyai session increments it. A server of
the same protocol answers with its own version, the features that both have,
and the type of the session:

```json
{"version":"0.4.2","protocol":1,"features":["project","run"],"session":"fyai"}
```

The feature `image-ORDERBITS` is in the answer when both ends have the same
one: the next frame of each end is an image (section 5.2.1).

A server of another protocol refuses the hello and names both protocols and
its version, so the user knows which side to change. A client that gets no
answer, or a refusal, stops.

Only a fyai session can send `fyai_project`, `fyai_run`,
`fyai_server_status`, `fyai_server_stop`, and a `fyai_session_attach` with
`args`, with `resume` or without a branch. An SDK session that sends one is
refused with the reason. The type goes with the client: a server of projects
gives it to the project server with the client socket, and a session worker
gets it as `--session-type fyai|sdk`. A worker keeps the type of the client
that started it, also for a client that attaches later.

`fyai remote connect` makes a fyai session; with `--protocol sdk`, it carries
the frames of an SDK program, which makes an SDK session.

### 6.1.4 The UI model stream

The full-screen client does not render frames of the SDK. It renders the UI
model of the worker (`doc/ui-model.md`), which the worker streams to it:

- The client asks for the model in its initialize, `fyai: {model: 1}`, and the
  answer carries the same member when the worker streams it.
- The worker subscribes to its model and sends each open text whole, then the
  root of each store, then each change:
  `{type: fyai, subtype: store, store, seq, root}` replaces the root of a
  store, and `{type: fyai, subtype: text, id, offset, data}` appends to a text,
  or ends it with `closed: true`. A store names a text by its id and length;
  the texts go first, so a store never names a text the client lacks.
- A writer that a display gates, such as the header and the questions, also
  writes for a consumer of the model, so a worker with no display streams
  them.
- `fyai_sync {have, depth}` returns the stored turns that the client lacks,
  each with its id, its display records, its user message, its calls and
  their outputs (`doc/state-sync-sdd.md`). `have` lists the ids of the newest
  turn the client holds and of the turns 1, 2, 4, 8, ... before it. The
  worker answers with the turns after the newest of them and names it in
  `base`, or, when it finds none, with the newest `depth` turns and a null
  `base`. The client has no arena; it keeps the turns in a replica, drops
  those after `base` and appends the new ones, and makes the screen again
  when it dropped any. It asks when the `head` member of the session store
  changes, and the stored turns replace the live rows, as a published turn
  replaces them in a local session. A replay counts the calls, so the calls go
  with their outputs. The answer to `initialize` carries the `epoch` of the
  worker, which a client uses to drop its replica when a worker restarts.
- `fyai_answer {qid, replies: [{selected, other}]}` answers a set of the
  questions store; the worker settles it.

The worker sends the `state` store first: the stored display configuration of
the branch. The client has no arena; it starts from that configuration, as a
local session starts from it, and the display settings that the client changes
stay over it. The client opens its display when the first stored turns
arrive, as a local session opens on its conversation, and replays them as a
local session replays them when it opens.

The model of the client is a mirror: the frames of the worker write it, and the
writers of the client write nothing. The renderer draws the header, the turn
state, the todo panel, the questions and the counts of the work from it as it
draws a local session. The renderer of the model (`src/fyai_uidraw.c`) draws
the live document, the log and the bands of the mirror as it draws them in a
local session. It gives the sink the bytes of the live document up to the
length that the transcript store names, less the recorded ranges, so a text
frame that arrives before the store frame that records it draws nothing. The
entries of the log in the snapshot were shown before the client attached. Escape and `^C` interrupt the turn of the worker.

A slash command runs where its state is. A command whose definition has
`scope: view`, and every setting of a `display/` key, changes only what the
client shows: it runs in the client, whose settings hold for its invocation
because it stores nothing. Every other command goes to the worker as
`fyai_command {line}`, which queues it behind the inputs before it and runs it
as a typed command; an async command holds the inputs behind it until it is
done. Its output enters the log store through the span of the command, and
the client shows it in the pane. A worker runs with Markdown on, so a table
reaches the client as Markdown, which the client renders at its width. `//`
sends a line that starts with a slash. A line that the user types is echoed in
the client, as a local session echoes it, and the card of its document from
the worker is not drawn again. A command of the user (`!`) goes to the worker
as `fyai_bang {command, rows, cols}`, which starts it as a user-owned terminal
session at once, as a typed bang starts it. The size is the grant of the tile
that the client opens for it: the client registers that tile and solves the
layout, as a local session does before its program starts. When the program
ends, the work store keeps the item, `exited` with its outcome, so the screen
stays to read until Escape or `q` dismisses it. An editor that a command of
the worker starts runs as a terminal session of the worker, which takes the
pane and the keys of the client.

The worker has bands while a client reads its model, which present nothing
there: the work store carries each one at its input. The renderer of the
client opens a band of its own for each, feeds a stream from the source of the
band with the rows and the indent of the stream of the worker, or paints the
rows of a band without source, and ends it as the work store says the band of
the worker ended: committed, destroyed, or closed with its outcome. The rows
of the transcript that a band presented are recorded ranges of the live
document, which the client does not draw again. A shell or agent job of the
worker opens its band as a job of a display does, after the open delay, and
streams the output of the call into it while the call runs. The view of a
tool call, such as a file that the model writes or a patch, carries the
Markdown that the worker rendered it from, with its code blocks; the client
renders it with the same view renderer at its own width.

A terminal session of the worker gives the bytes of its program to a text of
the model, as the cast protocol records output. The text starts with a
snapshot of the screen: the bytes that clear it, draw its rows with their
styles and place the cursor. A new text starts when the text grows past
256 KiB, so a client that attaches replays little, and the text stays
bounded. The item of the session in the work store names the text and the
size of the terminal. The client makes a mirror session of it: a terminal
session of `src/fyai_tools.c` whose program runs in the worker, with the
tile or the block of the transcript, the head, the keys and the end of a local
session. The keys go to the worker as `fyai_terminal_input {name, data}`. The
grant of the tile or the block goes as `fyai_terminal_size {name, rows,
cols}`, which resizes the program through the barrier of a local resize; the
size it took comes back in the item, after the bytes written at the old size.
The close button goes as `fyai_terminal_close {name, force}`, and the
dismissal of a screen kept after its program as `fyai_terminal_dismiss
{name}`. A sub-agent of the
worker has a terminal while a client reads the model, as it has one under a
display, at the width of a terminal of its own. Its item in the work store,
named by its branch, names the text of its screen, and the client shows it in
a tile that takes keys as the tile of a session does.

The worker of a fyai session (section 6.1.3) runs with the permission mode
`bypassPermissions` when the client gives none, so it decides on tool calls as
a local session does: the sandbox and the gates of fyai. The worker of an SDK
session keeps the default of the SDK. A call that needs permission
under another mode, when the SDK client gave no prompt tool, goes to a client
of the model as a question of the questions store: allow the call, with its
input as the preview, or decline it.

### 6.1.5 Watching a session

A watcher shows a session that another client drives. It does not drive it.
`fyai -b NAME remote connect --watch` sends
`fyai_session_attach {branch, watch: true}` after the hello.

- `fyai remote connect --last` names the session without a branch: the server
  takes the last session that started in the directory, as `fyai resume --last`
  does, and the request is `fyai_session_attach {resume: {last: true}}`. It
  works with `--watch` and without it, and is refused with `-b`.
- The branch must have a live session. A watcher starts no session, and it
  takes nothing from the client that drives: the session keeps its one
  driver, and any number of watchers can attach beside it.
- The server sends each frame of the worker to the driver and to each watcher.
  An answer goes to the client that asked. A watcher can send two requests,
  `initialize` and `fyai_sync`, which read the model and the stored turns.
  The server gives each of its request IDs the prefix `fyai-watch-N:` to route
  the answer, and refuses the prefix from a driver. Any other request gets the
  error "a watcher only reads". A frame that is not a request ends the watcher.
- A worker answers an `initialize` with the model of the UI whole. The
  snapshot is `{type: fyai, subtype: snapshot, phase: begin}`, the texts and
  the stores, then `phase: end`, each with the `request_id` of the initialize.
  The server sends the snapshot to the client that asked and to no other,
  because a text that a client holds arrives again at its end and breaks the
  order of its deltas.
- The client opens the display as a driver does, with its model as a mirror.
  It sends no intent: not a line, a key, a bang command, an answer or an
  interrupt. It draws no prompt and says so in a notice, and a line that the
  user types goes nowhere. `^C` leaves, and the commands of the view work.
- A watcher that goes does not affect the session. The end of the worker, or of
  the driver of a session that no branch names, ends the watchers.
- `serve status` shows the number of `watchers` of each session.

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

`fyai serve` makes a self-signed key and certificate the first time an
`https://` listener opens, and keeps them in `~/.fyai/tls/server.pem` (section
5.1.3). The certificate does not change, so the pinned fingerprint stays valid.
`fyai remote rekey` replaces it and so invalidates every pin. The pairing of
section 6.2 carries the fingerprint in its code, and is not built yet.

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
