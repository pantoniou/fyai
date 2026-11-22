# Software Design Document: Fleets of sessions

- **Project:** pantoniou/fyai
- **Status:** Steps 1 to 9 of section 14 are implemented (step 7 as the fleet
  panel in section 11.1), with deferred items noted in sections 6 to 13
- **Date:** 2026-11-19
- **Repository path:** `doc/fyai-fleet-sdd.md`
- **Baseline:** `devel` at `b4355093` (durable by default, every terminal
  session running in a split worker)

## 1. Purpose

A directory containing multiple projects (for example, `~/work`) can itself be
initialized as a project: the **fleet root**. A session running in the root
project (the **root session**) starts, monitors, and controls sessions in
nested projects beneath it, including user-driven sessions started manually.
Users attach to the root session, assign tasks, and leave it running. From the
root session, users can jump into any fleet session, interact with it, and
return—making the root session a switchboard. All state (tasks, member
branches, and conversation history) is stored as canonical state in project
arenas rather than in an ad-hoc daemon.

Typical workflow: a user runs a root session in the root project (via CLI or
web UI), asks it to create a subproject directory and arena, and either
delegates work to sessions in that subproject or jumps directly into them. The
root creates subprojects through the `fleet` tool's `create` action; users can
also run `fleet create` from the CLI (section 8). Section 5.3 describes how to
initialize a root project.

This design introduces no new storage engine and no stateful daemon. It builds
directly on existing primitives: arenas and CAS publishing, `fyai serve` and
project-server multiplexing, per-session workers, watchers, `durable`
checkpoints and the `pending` table, model events, and filesystem views.

## 2. Terms

| Term | Meaning |
| --- | --- |
| Root | A top-level project whose directory tree contains subprojects (for example, `~/work`). |
| Root server | The project server for the root project; it serves root sessions and routes requests for nested subprojects. |
| Parent | The nearest ancestor project above a given project, found by upward directory traversal (section 5.1). |
| Root session | A session running in the root project, typically parked (`keep`) on the root server. |
| Member | A session in a project under the root, started either by the root session or manually by a user. |
| Supervisor | A non-driving connection to a session authorized to send control requests (messages, interrupts, answers, termination) alongside the driver and watchers. |
| Lineage | Branch-store metadata recording which session started a member and for which task ID. |
| Task | A record in the root branch store tracking the requested work, target project and branch, and lifecycle state. |

## 3. Existing building blocks

| Requirement | Existing mechanism |
| --- | --- |
| Concurrent sessions on one arena or branch | Root CAS publish; conflict merge, rebase, or fork (`branch/on_conflict`) |
| Single front end for multiple projects | `fyai serve --projects DIR`; per-project servers spawned on demand |
| Live session inventory per project | Server registry; `<socket>.live`; `session list` |
| Observing a session | `remote connect --watch` (`remote-sessions-sdd.md` 6.1.5) |
| Single interactive driver per session | Attach; `resume` joins an existing live session |
| Sessions outliving their terminal | `/background` and client-disconnect handoff (`fyai-background-sessions-sdd.md` 20) |
| Crash recovery | `durable` checkpoints and `pending` table (waits, monitors, commands, sub-agents, events, views) |
| Terminating a session | `session kill` |
| Inter-turn notifications to the model | Events (`[KIND 'NAME' TAIL]` with optional body) |

Gaps addressed by this design:

1. Manually started terminal sessions were private children of their client
   until `/background`, invisible to servers and other sessions.
2. Multi-project servers (`serve --projects`) did not serve sessions of their
   own, so a root project could not both host a root session and front its
   subprojects.
3. Connections were limited to driver or read-only watcher; nothing could send
   messages, interrupts, or question answers to a session driven by another
   client.
4. Sessions did not record parent lineage or associated tasks.
5. The model had no tool to discover, start, or control other sessions.

## 4. Design decisions

1. **Always-present stored layer.** Every project and branch under the root is
   part of the fleet whether or not a process is running. Starting a session
   adds it to the live layer immediately if the project or ancestor server is
   running. Sessions never spawn a server solely for registration; the root
   server spawns project servers on demand.
2. **Supervisor authority.** A supervisor can send messages, interrupts,
   question answers, and termination requests to any session under the root,
   including user-driven terminal sessions.
3. **Shared working tree by default.** Members in the same project share its
   working tree. Running in an isolated filesystem view is opt-in per start.
4. **Root as a standard project.** The root has its own `.fyai` arena, and root
   sessions are standard sessions that users attach to and control normally.
5. **Root as a switchboard.** Users can jump from the root session into any
   fleet session and return in the same terminal.
6. **Flat, single-level supervision.** Only the topmost root session supervises
   the fleet, across all depths. Intermediate projects act as server routing
   boundaries for their subtrees, not as nested supervisors.

## 5. Root project and server hierarchy

### 5.1 Project tree discovery

The fleet is defined by directory containment. A project's parent is the
nearest ancestor directory containing a `.fyai` arena, discovered by walking up
from the parent directory (similar to Git repository discovery):

- Only `.fyai` directories containing an arena qualify (unlike
  `fyai_discover_project_root_from()`, which also checks `.git`).
- Traversal stops before examining any ceiling boundary: `$HOME` (whose
  `~/.fyai` is the user configuration directory, not a project), any directory
  listed in `$FYAI_CEILING_DIRECTORIES` (colon-separated, like
  `GIT_CEILING_DIRECTORIES`), and filesystem mount boundaries. Without a
  ceiling, traversal terminates at `/`.
- Discovery runs once at startup at a cost of one `stat()` per directory level
  (`src/store/fyai_fleet.c`).

A project with no parent project is a root. For example, `~/work/.fyai` is the
root for everything beneath `~/work`, and `~/work/libs/.fyai` is both a member
project under `~/work` and the parent of `~/work/libs/libfyaml`. Hierarchy
determines server routing (section 5.2) and canonical names: sessions are
always addressed relative to the root (`libs/libfyaml:BRANCH`) regardless of
depth. Intermediate projects do not supervise subprojects (decision 6); only
the root session receives the `fleet` tool (section 10).

### 5.2 Unified project server and subtree front end

Every project server serves sessions for its own project and acts as a front
end for all nested projects beneath it (matching `serve --projects`): when a
client or worker targets a nested directory, the server spawns the child
project server on demand and forwards the connection. Running `fyai serve` in
`~/work` makes it the root server automatically, with no special flag required.

- Each project server listens on `PROJECT/.fyai/unix/remote.sock`, allowing
  upward traversal to locate it. `--remote .` in a project without a local
  server walks upward to the nearest running ancestor server and sends its
  working directory to select the target project.
- Child project servers spawned by an ancestor server do not create their own
  listening socket; subsequent clients walk up to the ancestor server, which
  routes them to the existing child server.
- A `serve --once` server rejects requests targeting other projects, because
  child servers spawned under it would terminate when it exits.
- `serve --projects DIR` remains available for directories that are not
  themselves projects: it serves no sessions of its own, keeps
  `~/.fyai/unix/serve.sock`, and retains the `"the server serves projects: name
  one"` error when no project is specified.
- Child project servers belong to the ancestor server that spawned them and
  exit when it stops; manually started servers remain independent.
- Every project server—including child servers spawned by an ancestor—writes
  its live session registry to `PROJECT/.fyai/unix/remote.sock.live` so `fleet
  list` can inspect each project directly.

Users start and stop the root server with `fyai serve --daemon` and `fyai serve
stop`. Like all fyai servers, it holds no canonical state.

### 5.3 Initializing a fleet root

A fleet root is a top-level project with `fleet/root: true`:

```sh
mkdir ~/work && cd ~/work
fyai init --root          # creates the project with fleet/root: true
fyai serve --daemon       # optional: starts the root server
```

`fyai init --root` initializes a project and sets `fleet/root` in its config.
To enable or disable root mode on an existing top-level project, run `fyai
config set fleet/root true` (or `false`). Configure the default model and
provider settings in the root before creating subprojects, since `fleet create`
copies the root's configuration (section 8).

Any session started in the root project (`fyai -i` or via `remote connect`)
receives the `fleet` tool. A project that has an ancestor project is never
treated as a root, regardless of its `fleet/root` setting. Create member
projects with `fyai fleet create PROJECT` or the `fleet` tool's `create`
action.

## 6. Presence

Fleet visibility consists of two layers: a canonical **stored layer** read from
project arenas without requiring running processes, and a **live layer**
reported by active servers.

### 6.1 Stored layer

Any directory under the root containing a `.fyai` arena is automatically part
of the fleet; no manual registration is required. The root is the topmost
ancestor project above the current working directory (`fyai_fleet_root_of()`).

`fyai fleet scan` (and `/fleet scan`) lists discovered projects; `fyai fleet
list` lists their sessions. Both invoke `fyai_fleet_scan()`:

- Traverses subdirectories in sorted name order to find every directory
  containing a `.fyai` arena. Nested projects are independent entries.
- Skips `.fyai`, `.git`, symlinks, other filesystems, and names matching
  `fleet/scan/exclude` (default: `node_modules`, `build*`, `.cache`), up to
  `fleet/scan/depth` levels deep (default `4`). Both keys are read from the
  root project's stored configuration.
- Inspects each project's arena using `fyai_peek_sessions()` to list resumable
  branches (matching `resume --all`) and any `pending` table owner PIDs.
  Because a process maps arenas at a fixed virtual address, CLI fleet commands
  run with `no-storage` and peek each arena sequentially.

When no server reports a session, its state is one of:

- `stored`: no active process; attaching or messaging starts a worker (section
  11).
- `running`: its `pending` table references a live process that is not
  registered with any server.
- `lost`: its `pending` table references a dead process; starting the session
  restores pending work (`fyai-background-sessions-sdd.md` 20.7).

An idle, unregistered session with no pending work appears as `stored`, because
nothing in the branch store records its PID.

Scanning `/proc` for unregistered idle processes is intentionally out of scope:
it would be Linux-specific, fragile to map back to branches, and unhelpful
since an unregistered process has no server socket for supervision.

Deferred:

- Caching discovered projects in a `projects` field in the root store and
  emitting `[project 'PATH' added]` events. Currently each command scans on
  demand at a cost of one `stat()` per directory within `fleet/scan/depth`.

### 6.2 Live layer

Every interactive terminal worker registers on startup with its project server
or the nearest running ancestor server (section 5.1). Registration uses a
second worker channel alongside the terminal client and requires the server's
`register` hello capability. Ancestor servers forward the registration to the
target project's child server, spawning it if needed.

- Child project servers without a listening socket still maintain
  `PROJECT/.fyai/unix/remote.sock.live`, enabling `session list`, `fleet list`,
  and the resume picker to discover live sessions and route clients through the
  ancestor server.
- `session kill`, `/background`, and startup registration all locate the server
  via upward traversal and send `fyai_project` so ancestor servers route the
  request to the right project server.
- If no server is running along the ancestor chain, the worker watches
  `.fyai/unix` (or `.fyai` if `.fyai/unix` does not yet exist) for each
  ancestor project using the host directory watcher (`inotify` on Linux). When
  a server starts and writes its `.live` file, the worker registers between
  turns. If a server stops, the worker continues running and resumes watching
  for the next server.
- If a server rejects registration (for example, because another worker already
  owns the branch), the worker continues running locally and logs the reason to
  the trace.

Registration sends `fyai_session_adopt` with `driven: true`. The server lists
the session as `attached: true, local: true` in `serve status` and rejects
competing driver connections. The worker mirrors UI frames to both its client
and the server, while replies and snapshots go only to the requesting channel
(watcher requests carry server-assigned prefixes). The worker reports `busy`
and `waiting` states via `fyai_worker` frames.

- On `/background` or client disconnect, the worker reuses the registration
  connection by sending `fyai_worker` with `driven: false` and `keep`,
  promoting that connection to its primary channel without opening a second
  socket.
- On `SIGTERM` (sent by `session kill`), the worker shuts down cleanly and
  notifies its terminal client.

Deferred:

- Registering multiple concurrent workers on the same branch (keyed by PID).
- Registering non-interactive one-shot prompt runs (without `-i`).

### 6.2.1 Idle timeout for parked workers

Parked workers (`keep` with no attached client) exit automatically after
`serve/idle_ms` milliseconds (default `30000`, read from `fyai serve --config`;
`0` disables idle exit). The session remains saved in the arena and restarts on
demand via `resume`, `/jump`, or `fleet start`. Project servers remain running;
stopping the root server is left to the user.

The server tracks eligibility: a session is eligible for idle timeout when it
has no attached client, no active watcher (persistent supervisors do not
prevent timeout), no active turn, and no open question. After `idle_ms` of
eligibility, the server queries the worker with `fyai_idle`. The worker reports
`settled` only if it has no active turn, queued input, running tool job, side
question, sub-agent, monitor, terminal session, supervised member, or open
fleet task. A root session with open tasks never times out because it maintains
member subscriptions.

Race handling during shutdown:

- **Activity before `fyai_idle` replies:** the server counts client frames
  forwarded to the worker. If any frame arrived after `fyai_idle` was sent, a
  `settled` reply is ignored, the timer resets, and the worker processes the
  frame.
- **Activity after deciding to exit:** the server marks the session `closing`
  before sending `SIGTERM`. New clients do not attach to a `closing` session
  and instead start a fresh worker. Because the exiting worker has no active
  turn and branch publishing uses CAS, no state is lost or overwritten.

### 6.3 Merged session list

`fleet list` merges the stored and live layers into one row per session with
`project` (relative to the root), `branch`, `state`, `attached`, and `pid`.
Sessions reported by a server use the server's live state (`idle`, `running`,
`waiting`); all others use the stored-layer states from section 6.1. Live
sessions with no published exchange yet are included directly from the server
registry.

## 7. Supervisor protocol

A supervisor sends control requests to any live session on a branch, including
sessions driven by a terminal client. Requests require the server's `supervise`
hello capability and target the project's server:

| Server request | Forwarded worker request | Behavior |
| --- | --- | --- |
| `fyai_session_send {branch, text, from}` | `fyai_message` | Delivers `[message 'FROM']` with `text` as the body. Starts a new turn if idle, or queues for the next model round if busy. |
| `fyai_session_interrupt {branch}` | `interrupt` | Interrupts the active turn (equivalent to pressing Escape). |
| `fyai_session_answer {branch, answer}` | `fyai_supervise_answer` | Resolves the first open question using a 1-based option index or free-form text. Multi-question batches are rejected. |
| `fyai_session_close {branch, force}` | `SIGTERM` or `SIGKILL` | Terminates the worker (`session kill`). |

The server forwards supervisor requests using a watcher prefix so responses
return only to the supervisor; the supervisor connection does not receive the
session's UI frame stream. Any attached driver sees the effects directly in its
UI (an event card for messages, turn interruption, or resolved question).
Supervisors never acquire the driver role and never inject raw keystrokes.

Persistent supervisor attachments (`fyai_session_attach {branch, supervise:
true}`) are described in section 10; instead of UI frames, they receive
`fyai_session_state` updates (`{branch, state: idle|running|waiting|ended,
head, question, last}`), where `last` is the final assistant text of a
completed turn.

## 8. Commands

CLI subcommands and slash commands are defined in `data/commands.yaml`:

```sh
fyai fleet list                              # list all sessions under the root
fyai fleet kill [PROJECT]                    # stop live sessions (branches remain stored)
fyai fleet list libs                         # list sessions in libs and its subprojects
fyai fleet start fyai "Fix the build"        # starts fyai:fleet/fix-the-build
fyai fleet start --branch fleet/x fyai "Fix the build"
fyai fleet create apps/web                   # create directory and project arena
fyai fleet create apps/web "Write the readme"  # create project and start a session
fyai session send fyai:fleet/x Also run the tests.
fyai session send --from root:main fyai:fleet/x Also run the tests.
fyai session interrupt fyai:fleet/x
fyai session answer fyai:fleet/x 2           # option 2, or free-form text
fyai session kill fyai:fleet/x
fyai -b fleet/x remote connect --watch       # watch the session from inside fyai
```

Sessions are addressed as `PROJECT:BRANCH`, where `PROJECT` is the project path
relative to the root (`.` denotes the root project). If the prefix before the
first colon does not match an existing project, the entire string is treated as
a branch name, preserving branch names containing colons such as `main/agent:x`
(`fyai_fleet_name_split()`). Inside a project, `BRANCH` alone refers to that
project.

`fleet start PROJECT TASK` starts a worker on the target project's server using
`--branch` (or a generated `fleet/<slug>` branch derived from the first words
of `TASK`), attaches with `keep`, sends `TASK` as the first message, and
detaches so the session runs headless and remains parked after finishing. If a
worker is already running on that branch, `TASK` is sent to it as a message.

`fleet create PROJECT [TASK] [--default-config]` creates a new project at
`PROJECT`, a relative path of plain directory components under the root (to
create a root itself, see section 5.3). Missing parent directories are created,
and the arena is initialized in-process via `fyai_arena_create()` even when the
caller already has another arena mapped. Paths containing `.`, `..`, empty
segments, `.fyai`, leading `/`, or symlinks resolving outside the root are
rejected before creating any directories. If `PROJECT` already contains an
arena, it is preserved (`kept`). By default, the new project inherits the
caller's effective configuration (including `--set` and `--config` overrides,
with `fleet/root` stripped) so member sessions share the root's model setup;
`--default-config` (or `default_config: true` in the tool) writes the default
`fyai init` sample config instead. Because a process maps each arena at the
base address stored in its boot header, a session creating a new arena assigns
it a non-overlapping base address shifted by one region size from its open
arena; subsequent processes opening that arena map it at that recorded base.
When `TASK` is provided, `fleet create` immediately starts a session following
the same rules as `fleet start` and returns both results.

Deferred: commands targeting `stored` or `lost` sessions directly (currently
they require a live session; use `fleet start` or `resume` first), `--view` on
`fleet start`, and a `session watch` alias.

## 9. Lineage and tasks

A member branch store records its lineage under `fleet`:

```yaml
fleet:
  root: /home/user/work          # root project directory
  parent: {project: ".", branch: main}
  task: 7                        # task ID in the parent's branch store
```

When invoked from inside a session, `fleet start` passes lineage via hidden
global options `--fleet-parent PROJECT:BRANCH` and `--fleet-task ID`. The
worker persists this metadata on its first publish (before its first model
request when `durable` is enabled) and preserves it on subsequent publishes.
Sessions started directly from the CLI or by a user have no `fleet` field.

A root session branch store records its tasks under `tasks`, an open mapping
keyed by decimal ID strings starting at `"1"`:

```yaml
tasks:
  "7": {project: fyai, branch: fleet/x, text: "Fix the build",
        created: 1763550000000000, state: open}
```

`created` is a branch-log timestamp in microseconds. When a root session runs
`fleet start`, it creates a task and publishes it once the server accepts the
new worker (or stages it for the first publish if the root branch has no
exchanges yet). `state` is `open`, `done`, or `dropped` (and `queued` when
throttled by `fleet/max_live`; see section 13); only the owning root session
mutates its tasks (`/fleet task ID done`). Live runtime status (`running`,
`waiting`, `ended`) is never duplicated into the task record—it is read from
the servers and member branches. Because tasks live in structured store fields
rather than conversation prose, conversation compaction never drops them.

`fleet tasks` lists tasks across all root-project branches, and `fleet list`
displays the task ID and parent for each member. Both `fleet` and `tasks`
participate in standard store serialization and open-mapping CAS merges.

`session adopt PROJECT:BRANCH [TEXT]` (and the `fleet` tool's `adopt` action)
attaches lineage and a task to an existing live user-started session. The root
session sends `fyai_session_lineage {parent, task}` to the target worker as a
supervisor request; the worker stages the lineage and publishes it immediately
if idle or on the next publish if busy. The root then records the task and
begins supervising the member. The target session must be registered with a
server, and the root must have capacity under `fleet/max_live`. Adopting a
session that already has a parent replaces its parent with the adopting root.

## 10. Root session and the `fleet` tool

The root session is a normal session in the root project: run `fyai` in
`~/work` (or attach with `fyai resume`) and park it with `/background`. Its
model receives the `fleet` tool (`data/tools.yaml`,
`src/tools/fyai_fleet_tool.c`), structured like `settings` with an `action`
discriminator:

| Action | Behavior | Equivalent command |
| --- | --- | --- |
| `search` | Case-insensitive match of all words in `query` across project name, branch name, description, task text, and the last 20 turns of each session. Returns session name, match location, and surrounding snippet. | `fleet search` |
| `list` | Lists all fleet sessions with state, task ID, and parent. | `fleet list` |
| `read` | Returns the last `turns` turns of a session, oldest first. | `fleet read` |
| `tasks` | Lists tasks across root sessions. | `fleet tasks` |
| `start` | Starts a member session with a task and records the task in the root. | `fleet start` |
| `create` | Creates a subproject at `project` and optionally starts a member session if `task` is provided. | `fleet create` |
| `send`, `interrupt`, `answer`, `stop` | Sends supervisor control requests (section 7); messages use `.:BRANCH` of the root session as `from`. | `session send` / `interrupt` / `answer` / `kill` |
| `task` | Updates a task state to `open`, `done`, or `dropped`. | `/fleet task` |

CLI commands and the `fleet` tool share a single implementation
(`src/remote/fyai_fleet_ops.c`). Because an active session already has its own
arena mapped, read-only tool actions spawn a short-lived child process without
an open arena and parse its JSON output. Tool `start` and `task` actions stage
store updates for the turn's next publish, whereas slash/CLI commands publish
immediately.

The `fleet` tool is registered only when the session runs in the topmost
project of its tree and `fleet/root` is enabled in configuration (evaluated
once per run). Requiring explicit opt-in avoids adding tool schema tokens to
ordinary projects while allowing an empty root to create its first subprojects.
`fleet/root` is read-only to the model, is stripped by `fleet create`, and is
not exposed to delegated sub-agents. Member sessions use their own sub-agents
and have no `fleet` tool; if cross-project work is needed, a member reports it
in its reply and the root session decides. Users can invoke the CLI commands
from section 8 in any directory under the root.

Because `fleet` can start processes and send input to other sessions,
`fleet/approve` controls user confirmation and `fleet/max_live` caps concurrent
supervised members (section 13).

For every open task, the root session maintains a persistent supervisor
connection (`fyai_session_attach {branch, supervise: true}`) managed by
`struct fyai_fleet_watch` (`ctx->fleet`, `src/remote/fyai_fleet_watch.c`). The
server emits `{type: fyai, subtype: session_state, branch, state, last}` frames
on state transitions: `running` and `waiting` from worker `fyai_worker` frames,
`idle` (with `last` containing the assistant's reply text) from each `result`
frame, and `ended` when the worker exits (after which the connection drains and
closes). These translate into root model events:

```text
[session 'fyai:fleet/x' idle]        body: the last answer
[session 'fyai:fleet/x' waiting]     body: a question waits for an answer
[session 'fyai:fleet/x' ended]
[session 'fyai:fleet/x' restarted: its process was lost; ...]
[session 'fyai:fleet/x' restarted: its process crashed; ...]
```

Transitions to `running` do not emit events. Member UI streams and internal
events stay local to the member; the root model uses `read` when it needs full
transcript detail.

### 10.1 Durability and crash recovery

- Member workers run in independent processes and continue running if the root
  session exits or crashes.
- Open tasks in the root branch store serve as canonical subscriptions, so no
  separate `pending` rows are needed. On startup, `fyai_fleet_watch_restore()`
  reattaches supervisor connections for every open task whose member is live
  and notifies the model if any member is `waiting` on a question. Transitions
  missed while the root was down are not replayed; the model can inspect recent
  output with `read`.
- If an open task's member is `lost` (its worker died with pending work), the
  root restarts it on its project server without sending a new message; the
  member's own `pending` table recovers its work and the root model receives a
  `restarted` event.
- If a supervised member crashes while the root is running, the root restarts
  it the same way and notifies the model with `restarted: its process crashed`.
  The server marks `ended` frames as `crashed` when a worker dies from
  `SIGSEGV`, `SIGABRT`, `SIGBUS`, `SIGFPE`, `SIGILL`, or an unexpected non-zero
  exit status (excluding clean shutdowns, explicit kills, and closed local
  sessions). Automatic restarts are capped at `FLEET_RESTARTS_MAX` (2); further
  exits deliver `ended` so the model can decide how to proceed.
- Newly started members are supervised immediately. If attaching the supervisor
  fails, a warning is logged; the member continues running without emitting
  events to the root.

Deferred: automatically restarting `stored` members (those that exited cleanly
with no pending work), and running members when no server is available.

### 10.2 Scope boundaries

The root session has no hidden memory store or private plan database. Its
entire state consists of its conversation history, structured `tasks`, skills,
and the branches of member projects—all inspectable with existing commands
(`branch log`, `session list --all`, `remote connect --watch`).

## 11. Switchboard and navigation

Users can jump from the root session (or any session) into another fleet
session and return in the same terminal. While away, the root session stays
parked on its server with its supervisor connections active, queuing incoming
member events for its next turn.

- `/jump NAME` (`PROJECT:BRANCH`, or `BRANCH` in the current project) detaches
  from the current session via `fyai_session_leave()` (as `/background` does)
  and passes `next = jump:NAME RETURN` (where `RETURN` is the current session's
  full `PROJECT:BRANCH` name). The terminal client then executes
  `fyai fleet attach NAME --return RETURN` in its place.
- `/jump --split NAME` keeps the current session active and opens a watcher for
  `NAME` in a work-pane tile by running `fyai fleet attach NAME` as a user
  command (like `!`). Watchers do not display commands typed by other clients.
- `/split` opens a parallel session on the same branch inside a work-pane tile
  using `fyai --set branch/on_conflict=fork -b BRANCH` and applies the same
  conflict policy to the current session. Both sessions share the branch until
  both commit concurrently, at which point the second commit forks onto a new
  `session/` branch from their common base. Because the tile is a worker-emulated
  terminal, the nested client skips terminal capability queries and receives
  its color scheme via `FYAI_TERMINAL_VARIANT`.
- `fleet attach NAME` resolves `NAME` via `fleet list`. If the session is
  `stored` or `lost`, it starts a worker on the project's server without
  sending a message (restoring pending work for `lost` sessions). It then runs
  `fyai -b BRANCH remote connect` in the project directory with
  `$FYAI_FLEET_RETURN` set to `RETURN`.
- `remote connect` locates the project or ancestor server via upward traversal
  (section 5.1) and passes its working directory so nested projects behind a
  root server are reached transparently.
- If the target session has no driver, the attaching terminal becomes its
  driver with full interactive control. If another client already drives it,
  the attaching client connects in watch mode (`--watch`): slash commands are
  disabled, and typed lines are sent as `fyai_session_send` messages from
  `a watcher` (delivered to the model as `[message 'a watcher']`). Replies
  appear for both the driver and the watcher.
- The client status bar displays the jump origin: the `{fleet}` template key
  expands to `jumped from SESSION · /root returns · ` while `$FYAI_FLEET_RETURN`
  is set, and is empty otherwise. Default `prompt_top` places `{fleet}` first
  so long paths cannot truncate it.
- `/root` leaves the target session running on its server, exits the current
  client with `next = root:`, and runs `fleet attach` on `$FYAI_FLEET_RETURN`.
  If invoked in a session not reached via `/jump`, the client prints a notice
  and exits without stopping the session.

Tab completion (`fleet-session` kind, showing `PROJECT:BRANCH`, state, and
title) is supported for `/jump`, `fleet attach`, `fleet read`, and `session
send`/`interrupt`/`answer`. Each completion runs `fleet list` in a short-lived
child process.

Deferred: a cross-project fleet picker UI (`/sessions`; the question panel
holds four options and the existing session browser is scoped to one project),
and `jumped`/`returned` events for the root session.

### 11.1 Fleet panel

Root sessions display supervised members in a `Fleet` panel below the todo
list, with one row per member showing its name, state, and the first line of
its latest reply (truncated to 60 characters):

```text
Fleet
· a:fleet/fix-the-build · idle — Build fixed.
● libs/inner:fleet/docs · running
```

If a member branch has a title (`/title` branch description), the panel shows
it before the session name: `Fix the build (a:fleet/fix-the-build)`. The watch
component refreshes titles from `fleet list` when attaching to a member and
after each member turn completes (since auto-titling runs after a turn).

Each row begins with a state glyph: a dot for `idle`, a diamond for `waiting`,
a dash for other states, and the theme's animated tool-running spinner for
`running` (driven by the UI activity clock only while at least one member is
running).

The panel is visible from startup (`no members` when empty). Toggle it per
session with `fleet panel off` and `fleet panel on` (`fleet/panel`, enabled by
default). When disabled, the root worker omits panel rows so the client needs
no separate setting.

`ctx->fleet` writes rows into the `fleet` entry of the UI model's `panels`
store on every member state change, and the split client mirrors that store
just like the todo list. Layout (`data/page.yaml`, page `fleet`, state `fleet`)
places the panel immediately below the todo list and hides it if vertical space
is insufficient.

## 12. Shared working tree

Members within the same project share its working directory. CAS publishing
protects arena consistency, not working-tree files, so concurrent edits to the
same file by two members can race. This is accepted as the default; starting a
member with `--view` (deferred) will isolate its working tree using filesystem
views.

## 13. Limits and approval policy

- `fleet/scan/depth` and `fleet/scan/exclude`: control project discovery
  (section 6.1).
- `fleet/max_live` (default `8`, `0` for unlimited): caps the number of
  concurrently supervised members per root session. A `start` request above the
  limit still prompts for approval if required, then enters the `queued` task
  state without creating a branch or spawning a worker (the tool returns
  `queued`). When a supervised member exits and frees a slot,
  `fyai_fleet_dequeue()` starts the oldest queued task, attaches supervision,
  marks the task `open`, and notifies the model with `[session 'NAME' started:
  a place became free for its queued task]`. If a queued task fails to start,
  it is marked `dropped` with an event. Queued tasks are not treated as open
  subscriptions on root restart and can be canceled with `task ID dropped`.
- `fleet/approve`: controls which `fleet` tool calls require user confirmation
  via the question panel (defaulting the cursor to Reject):
  - `start` (default): prompts before `start` and `create`.
  - `always`: prompts before every mutating action (`start`, `create`, `send`,
    `interrupt`, `answer`, `stop`).
  - `never`: disables prompts.
  If the user rejects a call, their optional reason is returned as the tool
  result. Calls requiring approval in non-interactive runs (where no user can
  answer) are rejected. Both `fleet/approve` and `fleet/max_live` are read-only
  to the model.
- **Authorization:** supervisor connections use the same Unix-socket
  same-user authentication as all other server clients.
- **Usage tracking:** each member tracks its own token usage and spend;
  fleet-wide aggregation is deferred.

## 14. Delivery milestones

Each milestone ships with automated tests.

1. **Root server.** Upward project traversal and unified project-server
   subtree routing. Tests: traversal stops at `$HOME`, ceiling directories, and
   filesystem boundaries; root and nested sessions served through the root
   server; multi-level nested projects; backward compatibility of `serve
   --projects` on non-project directories.
2. **Presence.** Stored layer (`fleet scan`, arena peeking, `stored`/`running`/
   `lost` states) and live layer (startup registration, directory watching for
   servers starting later, dual-channel frame mirroring, server restart
   resilience, `/background` handoff over the registration channel, `fleet
   list`).
3. **Supervisor protocol and commands.** `session send`/`interrupt`/`answer`/
   `kill`, `fleet start`, and `PROJECT:BRANCH` addressing. Tests: messaging
   idle and busy sessions, answering questions visible to a driver, and
   interrupting active turns.
4. **Lineage and tasks.** `fleet` and `tasks` store fields, `fleet tasks`, and
   `/fleet task`. Tests: parent/task recording on member creation and task
   state transitions.
5. **Root `fleet` tool and events.** `fleet` tool implementation, persistent
   supervisor connections, state-change events, subscription recovery on root
   restart, and automatic recovery of lost/crashed members.
6. **Switchboard.** `/jump`, `/root`, `fleet attach`, and return-target status
   display. Tests: jumping to a parked member, running a turn, and returning to
   the root session. Remaining test coverage: jumping into a user-driven
   session and into a `stored` session.
7. **Fleet panel.** Implemented as the `Fleet` panel in section 11.1 (member
   status rows, answer previews, animated running indicator, layout tests).
   Full streaming member tiles in the work pane are deferred.
8. **Policy and limits.** `fleet/max_live` queuing and `fleet/approve`
   confirmation prompts. Tests: approval, rejection, queueing over the limit,
   dequeue on member exit, and rejection in non-interactive runs.
9. **Subproject creation.** `fleet create` and the `create` tool action
   (directory creation, arena initialization with inherited root config,
   optional initial session). Tests (`fleet_create`): nested paths, existing
   projects, path-escape/symlink rejection, visibility in `fleet list`, and
   model-driven creation plus task launch. Web UI parity (list, task, create,
   jump) is deferred; the CLI serves as the reference implementation.

## 15. Open questions

1. **Remote hosts:** supervising members on other machines over SSH or HTTP
   endpoints. Session naming and supervisor semantics remain unchanged, but the
   root server will need a configured list of remote endpoints.
2. **Alternative front ends:** accessing the root session from mobile, web, or
   messaging clients (`fyai-mobile-tailscale-sdd.md` and `fyai-web.md`).
