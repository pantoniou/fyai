# Software Design Document: Fleets of sessions

- **Project:** pantoniou/fyai
- **Status:** Steps 1 to 6 of section 14 are implemented, and the panel of step 7, with the parts that sections 6 to 11 defer
- **Date:** 2026-11-19
- **Repository path:** `doc/fyai-fleet-sdd.md`
- **Baseline:** `devel` at `b4355093` (durable by default, every terminal
  session at a worker)

## 1. Purpose

A directory of projects, for example `~/work`, is itself a project: the
**root**. A session of the root, the **root session**, starts, watches and
controls the sessions of the projects below it, and the sessions that a person
started by hand there. A person attaches to the root session, gives it tasks,
and leaves it running. From the root session the person jumps into any session
of the fleet, controls it, and comes back: the root is a switchboard. It is a resident agent of the user, with structure: its
tasks are records, its work is branches of the projects, and every state it
has is canonical state in an arena.

This design adds no new kind of state and no resident process with state of
its own. It uses what exists: the arena and its CAS publish, `fyai serve` and
its server of projects, the worker of each session, watchers, `durable` and
the `pending` table, events, and filesystem views.

## 2. Terms

| Term | Meaning |
| --- | --- |
| Root | A project whose directory holds other projects (`~/work`). |
| Root server | The project server of the root: the server of the root project and the front of the projects below it. |
| Parent | The nearest project above a project, found by the walk up (section 5.1). |
| Root session | A session of the root project. It is usually parked (`keep`). |
| Member | A session of a project under the root. The root session started it, or a person did. |
| Supervisor | A connection to a session that may send intent to it, beside its driver and its watchers. |
| Lineage | The member record of a branch: the session that started it and the task it was given. |
| Task | A record in the root store: what was asked, of which project and branch, and whether it is open. |

## 3. What exists

| Need | Present mechanism |
| --- | --- |
| Many sessions on one arena, even on one branch | Root CAS publish; merge or rebase after a lost race (`branch/on_conflict`) |
| One front for many projects | `fyai serve --projects DIR`; a project server for each project, started on demand |
| The live sessions of a project | The registry of the server; `<socket>.live`; `session list` |
| Watching a session | `remote connect --watch` (`remote-sessions-sdd.md` 6.1.5) |
| One driver of a session | Attach, `resume` joins a live row |
| A session outlives its terminal | `/background`, the terminal that goes away (`fyai-background-sessions-sdd.md` 20) |
| A session survives a crash | `durable`, the `pending` table: waits, monitors, commands, sub-agents, events, views |
| Ending a session | `session kill` |
| Messages to a model between turns | Events: `[KIND 'NAME' TAIL]` with a body |

What does not exist:

1. A session that a person started is a private child of its client until
   `/background`. No server and no other session knows it.
2. A server of projects serves no session itself, so the root project has no
   server of its own beside the front.
3. A connection is a driver or a watcher. Nothing can send a message, an
   interrupt or an answer to a session that it does not drive.
4. No session records which session started it, or for which task.
5. The model has no tools to start or address a session.

## 4. Decisions

1. A project and its sessions are in the fleet whether a process runs or not.
   The stored layer (the projects under the root and their branches) is always
   there; a session that starts adds the live layer at once when the root
   server or its project server runs. A session never starts a server of its
   own to be present; the root server starts the project server for it.
2. A supervisor may send intent to any session under the root, also to one
   that a person drives: a message, an interrupt, an answer, an end.
3. Members of one project share the working tree. A view is an option of a
   start, not the default.
4. The root is a project with its own arena. The root session lives there. A
   person attaches to it and controls it as any session.
5. The root session is a switchboard: from it a person jumps into any session
   under the root and controls it.
6. Supervision has one level. The root session supervises every session of
   the tree, at any depth. A project with projects below it is a member like
   any other: the front of its subtree for the servers, not a supervisor.

## 5. The root project and its server

### 5.1 The tree of projects

The fleet is the tree of projects by containment. The parent of a project is
the nearest project above its directory, found as git finds a repository: walk
up the parent directories from the directory above the project, and take the
first that holds a `.fyai` with an arena. The walk:

- looks only for `.fyai` with an arena, not for `.git`
  (`fyai_discover_project_root_from()` takes both; the walk of the fleet has
  its own test);
- does not examine a ceiling, and stops there: `$HOME`, whose `.fyai` is the
  directory of the user and not a project, a directory of
  `$FYAI_CEILING_DIRECTORIES` (a colon-separated list, as
  `GIT_CEILING_DIRECTORIES`), and a directory on another file system than the
  one below it. With no ceiling above it, the walk ends at `/`;
- is done once at start, and costs one `stat()` a level
  (`src/store/fyai_fleet.c`).

A project with no parent is a root. `~/work` with a `.fyai` is the root of
every project below it; `~/work/libs` with a `.fyai` is a project of the root
and the parent of `~/work/libs/libfyaml`. The parent matters to the servers
(section 5.2) and to the names: a session is named from the root
(`libs/libfyaml:BRANCH`) at any depth. It does not make a supervisor: the root
session supervises the whole tree (decision 6), and only it has the tools of
section 10.

### 5.2 Every project server is the front of its subtree

A project server serves the sessions of its project, and is the front of the
projects below it, as `serve --projects` is now: a client or a worker that
names a directory below gets the project server of the project that holds the
directory, which the server starts when it does not run. `fyai serve` in
`~/work` is therefore the root server; no option makes it one.

- The socket of a project server stays `PROJECT/.fyai/unix/remote.sock`, so
  the walk up finds it. `--remote .` in a project that no server serves takes
  the server of the nearest project above that runs; the directory of the
  client names the project there.
- A project server started for its parent has no socket: a later client walks
  up to the parent again, which gives it the same project server.
- A `serve --once` server refuses a directory of another project: the project
  server that it started would end with it.
- `serve --projects DIR` stays for a directory that is not a project: it
  serves no session itself, and `~/.fyai/unix/serve.sock` stays its socket.
  The refusal "the server serves projects: name one" stays for it.
- A project server that a parent server started belongs to it, as now: it ends
  with its parent. One that a person started does not.
- Each project server writes the record of its own sessions
  (`PROJECT/.fyai/unix/remote.sock.live`), also one that a server above
  started; `fleet list` reads the record of each project.

The user starts and stops the root server (`fyai serve --daemon` in `~/work`,
`fyai serve stop`). It keeps no canonical state, as every server.

## 6. Presence

The fleet has two layers. The stored layer is canonical and needs no process;
the live layer is a hint from the servers.

### 6.1 The stored layer

A project is in the fleet when its directory is under the root and holds a
`.fyai` arena. Nothing registers a project: a scan finds it. The root is the
topmost project above the current directory (`fyai_fleet_root_of()`).

`fyai fleet scan` (`/fleet scan` in a session) lists the projects, and
`fyai fleet list` their sessions. Each runs the scan (`fyai_fleet_scan()`):

- It finds each directory whose `.fyai` holds an arena, in the order of the
  names. A project below a project is a project of its own, as the nearest
  `.fyai` owns a directory.
- It does not go into `.fyai`, `.git`, a symbolic link, a directory on
  another file system, or a directory that `fleet/scan/exclude` names
  (default: `node_modules`, `build*`, `.cache`). It goes `fleet/scan/depth`
  levels down (default 4). The stored configuration of the root sets both.
- It reads the arena of each project with a peek (`fyai_peek_sessions()`):
  the resumable branches, as `resume --all` lists them, with the owner of
  their `pending` table. A process maps one arena at its fixed address, so
  the commands open none of their own (`no-storage`) and peek at each in turn.

A session is in one of these states when no server lists it:

- `stored`: nothing runs it. A jump or a message starts it (section 11).
- `running`: its `pending` table names a process that runs, which no server
  lists.
- `lost`: its `pending` table names a process that is gone. A start restores
  the work (`fyai-background-sessions-sdd.md` 20.7).

An idle session with no server and no pending work shows as `stored`: nothing
on the branch names its process.

Deferred from this step:

- A search of the processes of the user (`/proc`) for the sessions that run
  with no server and no pending work.
- A `projects` member in the store of the root, which keeps what a scan found
  so that a list need not scan, and the event `[project 'PATH' added]`. Each
  command scans: one `stat()` for each directory to the depth of the scan.

### 6.2 The live layer

A session on a terminal registers at once with the server of its project, or
of the nearest project above that runs (the walk of section 5.1). The
connection is a second channel of the worker beside its client; the server
needs the feature `register` of the hello. A server above gives the
connection to the server of the project, which it starts.

- A project server that a server above started has no socket. It keeps the
  record of its sessions where the record of its own server would be
  (`PROJECT/.fyai/unix/remote.sock.live`), so `session list`, `fleet list`
  and the resume picker of the project find them, and a client joins them
  through the server above.
- `session kill`, `/background` and the registration all find the server by
  the walk, and name the directory to it (`fyai_project`) so that a server
  above gives the connection on.
- With no server on the way up, the worker watches the `.fyai/unix`
  directory of each project on the way (or its `.fyai` where that does not
  exist yet), through the directory watch of the host (inotify on Linux).
  A server writes its record once it listens, and the worker registers
  between turns, not in the event. A server that stops leaves the worker
  running; it watches again and registers with the next server.
- A server that refuses (another worker runs the branch: the picker of
  `resume` on a parked session) leaves the session as it is; the trace says
  why.

Registration is `fyai_session_adopt` with `driven: true`. The server lists
the session with `attached: true` and `local: true` in `serve status`, and
refuses a client that asks to drive it. The worker sends each frame to its
client and to the server; an answer and a snapshot go only to the connection
that asked (a request of a watcher has the prefix that the server gave it).
The worker reports `busy` in its `fyai_worker` frame.

- `/background`, and the terminal that goes away, move the session on the
  registration: the worker sends `fyai_worker` with `driven: false` and
  `keep`, and the connection becomes its channel. No second connection is
  made.
- A worker that SIGTERM asks to end ends its session, and its client says so:
  `session kill` of a session that its own client drives reaches it only so.

Deferred from this step:

- A second worker on a branch that a server has is not registered; the
  design to list it by `pid` is not done.
- A run of a prompt with no `-i` does not register.

### 6.3 The list

`fleet list` joins the layers: one row for each session, with `project`
(relative to the root), `branch`, `state`, `attached` and `pid`. A session
that a server lists has the state of the server (`idle`, `running`,
`waiting`); the others have the states of section 6.1. A live session with
no stored exchange yet is listed from the record alone.

## 7. The supervisor

A supervisor sends intent to the live session on a branch, also to one that
a client drives. Each request names the branch, at the server that runs the
sessions of its project (the server needs the feature `supervise` of the
hello):

| Request at the server | Request at the worker | Effect |
| --- | --- | --- |
| `fyai_session_send {branch, text, from}` | `fyai_message` | The event `[message 'FROM']` with the text as its body. An idle session takes it as a turn between turns; a running one gives it to the model at its next round. |
| `fyai_session_interrupt {branch}` | `interrupt` | The interrupt of the turn, as Escape. |
| `fyai_session_answer {branch, answer}` | `fyai_supervise_answer` | Settles the first open question: the number of an option, from 1, or any other text. A set of several questions is refused. |
| `fyai_session_close {branch, force}` | SIGTERM, or SIGKILL | `session kill`. |

The server sends the request to the worker under the prefix of a watcher, so
the answer of the worker comes back to the supervisor alone. The connection
then watches the session quietly: it gets its answers and no frame of the
session. The driver of the session sees each request as the user does: the
message as an event card, the interrupt as an interrupt, and the answer
settles the question that it shows. A supervisor never takes the driver
role and sends no keys.

Deferred to step 5, where the root session needs it: an attachment that
stays (`fyai_session_attach {branch, supervise: true}`) and gets
`fyai_session_state` frames in place of the UI stream: `{branch, state:
idle|running|waiting|ended, head, question, last}`, where `last` is the last
assistant text of a turn that ended.

## 8. Commands

Verbs and slash commands in `data/commands.yaml`:

```sh
fyai fleet list                              # every session under the root
fyai fleet start fyai "Fix the build"        # fyai:fleet/fix-the-build
fyai fleet start --branch fleet/x fyai "Fix the build"
fyai session send fyai:fleet/x Also run the tests.
fyai session send --from root:main fyai:fleet/x Also run the tests.
fyai session interrupt fyai:fleet/x
fyai session answer fyai:fleet/x 2           # the second option, or a text
fyai session kill fyai:fleet/x
fyai -b fleet/x remote connect --watch       # in fyai: watch it
```

A session is named `PROJECT:BRANCH`, where PROJECT is the directory relative
to the root (`.` is the root project) that holds a project. A name whose text
before its first colon is no project is a branch alone, so `main/agent:x`
stays a branch (`fyai_fleet_name_split()`). Inside a project, `BRANCH` alone
names a session of that project.

`fleet start PROJECT TASK` starts a worker at the server that runs the
sessions of the project, on `--branch` or on `fleet/` and the first words of
the task, with the task as its first message. It attaches with `keep`, sends
the task and leaves: the session runs with no client, and the server keeps it
after its turn. A branch that a worker runs already gets the task as a
message of its client.

Deferred: verbs for a session that is `stored` or `lost` (they need a live
session now; `fleet start` and `resume` start one), `--view` for `fleet
start`, and a `session watch` verb.

## 9. Lineage and tasks

The store of a member branch holds `fleet`, its lineage:

```yaml
fleet:
  root: /home/user/work          # the root directory
  parent: {project: ".", branch: main}
  task: 7                        # the task in the store of the parent
```

`fleet start` in a session passes the lineage to the worker that it starts,
as the hidden global options `--fleet-parent PROJECT:BRANCH` and
`--fleet-task ID`. The first publish of the new session records it, which
with `durable` is before its first model request; a later publish keeps it.
A start from the command line, and a session that a person started, have no
`fleet` member.

The store of a root session branch holds `tasks`, an open mapping by id (a
string of digits, from 1):

```yaml
tasks:
  "7": {project: fyai, branch: fleet/x, text: "Fix the build",
        created: 1763550000000000, state: open}
```

`created` is the time of the branch log (microseconds). A session whose
project is the root makes a task when it runs `fleet start`, and publishes it
once the server took the new session (a session with no stored exchange keeps
it for its first publish). `state` is `open`, `done` or `dropped`; only the
root session that keeps a task changes it (`/fleet task ID done`). Whether a
member runs, waits or ended is not stored: it is read from the servers and
from the member branch. A task is a record, not prose in the conversation,
so a compaction of the root conversation loses no task.

`fleet tasks` lists the tasks of every branch of the root (the root sessions),
and `fleet list` shows the task and the parent of each member. Both store
members are decoded and built as the other members of a store, and a CAS
merge merges them as an open mapping.

Deferred: `session adopt PROJECT:BRANCH`, which gives a session that a person
started a lineage and a task. It needs a request to its worker, which stages
the lineage and publishes.

## 10. The root session and its model

The root session is an ordinary session of the root project: a person runs
`fyai` in `~/work`, or joins the live one with `fyai resume`, and parks it
with `/background`. Its model gets one tool, `fleet` (`data/tools.yaml`,
`src/tools/fyai_fleet_tool.c`), made as `settings` is: an `action` selects
what it does, and `search` is where the model starts.

| Action | Does | As the command |
| --- | --- | --- |
| `search` | The sessions and tasks that hold each word of `query`, in any case: in the project, the branch, the description, the text of a task, or the words of the last 20 turns of a session. Each row names the session, where the first word is, and the text around it. | `fleet search` |
| `list` | Each session with its state, task and parent. | `fleet list` |
| `read` | The words of the last `turns` turns of a session, oldest first. | `fleet read` |
| `tasks` | The tasks of the root sessions. | `fleet tasks` |
| `start` | Starts a member with a task; the root keeps the task. | `fleet start` |
| `send`, `interrupt`, `answer`, `stop` | The requests of the supervisor (section 7); a message is from `.:BRANCH` of the root session. | `session send` and the others |
| `task` | Marks a task `open`, `done` or `dropped`. | `/fleet task` |

The commands and the tool share one implementation
(`src/remote/fyai_fleet_ops.c`). In a session, which has its arena open, the
readers run the matching command in a child with no arena and take its rows
as JSON. A `start` and a `task` of the tool stage the tasks for the next
publish, which a turn makes; the commands publish at once.

Only a session of a root project gets the tool: the `describe` of its entry
offers it when the project of the session is the root and a scan finds a
project below it, once for a run. A delegated agent cannot call it. A member has its
sub-agents and no fleet tools: a member that needs more work done says so in
its answer, and the root session decides. A person runs the commands of
section 8 from any directory of the tree.

The tool sends data to other sessions and starts programs. Step 7 adds
`fleet/approve`, which decides which calls ask the person; until then a call
runs as the model asks it.

The root session supervises each member of an open task with a connection
that stays (`fyai_session_attach {branch, supervise: true}`), kept by the
component `struct fyai_fleet_watch` (`ctx->fleet`,
`src/remote/fyai_fleet_watch.c`). The server sends it a frame `{type: fyai,
subtype: session_state, branch, state, last}` when the state changes:
`running` and `waiting` from the `fyai_worker` frame of the worker, `idle`
with `last`, the text of the answer, from each `result` frame, and `ended`
when the worker goes, after which the connection drains and closes. The root
model gets events:

```text
[session 'fyai:fleet/x' idle]        body: the last answer
[session 'fyai:fleet/x' waiting]     body: a question waits for an answer
[session 'fyai:fleet/x' ended]
[session 'fyai:fleet/x' restarted: its process was lost; ...]
```

`running` is no event. The events of a member are its own; the root model
gets no UI stream, and reads more with the `read` action.

### 10.1 Durability

- A member is a process of its own. It does not end with the root session.
- The open tasks are the subscriptions: they are canonical state of the root
  branch, so the root needs no `pending` rows for them. A worker of a root
  session takes them up before its first turn
  (`fyai_fleet_watch_restore()`): it supervises each member that a server
  lists, and tells the model if one waits on a question. A change during the
  loss of the root is not repeated: `read` gives what the member did.
- A member of an open task that is `lost` (its process went with work in
  flight) is started again at the server of its project on its branch, with
  no message; its own `pending` table restores its work, and the root model
  gets `restarted`.
- A start supervises the member at once. A failure to supervise is a
  warning: the member runs, and the root does not hear of it.

Deferred: a member whose session is `stored` (it ended, or its worker went
with no pending work) is not started again, and a root with no server runs
no member of its own.

### 10.2 What the root is not

The root holds no free-form memory store and no hidden plan. What it knows is
its conversation, its tasks, its skills, and the branches of the projects. A
person reads all of it with the commands that exist: `branch log`, `session
list --all`, `session watch`.

## 11. The switchboard

A person in the root session jumps into any session of the fleet, controls
it, and comes back. The root session goes on while the person is away: it is
parked at its server, its supervisors stay, and its events wait for the next
turn.

- `/jump NAME` (PROJECT:BRANCH, or BRANCH of this project) leaves the session
  as `/background` does (`fyai_session_leave()`), with the next session
  `jump:NAME RETURN`, where RETURN is the full name of this session. The
  client of the terminal runs `fyai fleet attach NAME --return RETURN` in its
  place.
- `fleet attach NAME` finds the session in `fleet list`. One that no process
  runs (`stored` or `lost`) starts at the server of its project on its
  branch, with no message, which restores the work of a lost one. The client
  then runs `fyai -b BRANCH remote connect` in the directory of the project,
  with `$FYAI_FLEET_RETURN` set to RETURN.
- `remote connect` finds the server by the walk of section 5.1 and names its
  directory, so a project whose server runs behind the root is reached
  through the root.
- A session with no client takes the terminal as its driver: keys, lines,
  questions and interrupts, as in a session of its own. A session that
  another client drives is watched (`--watch`): this client sends nothing.
- `/root` leaves the session as it was before the jump: a session that a
  server holds keeps it, and this client ends with the next session `root:`.
  The client runs `fleet attach` of `$FYAI_FLEET_RETURN`, the root session,
  which the server holds. In a session that no jump reached, the client says
  so and ends; the session stays as it is.

Deferred: a picker of the fleet (`/sessions`); the lines that a watcher of a
driven session types going to it as messages; the name of the session and
the way back on the status line; and the events `jumped` and `returned` of
the root session.

### 11.1 The fleet panel

The page of a root session shows its members under the todo list, in a panel
headed `Fleet`: one row for each member that it supervises, with its name, its
state, and the first line of its last answer (at most 60 characters):

```text
Fleet
- a:fleet/fix-the-build · idle — Build fixed.
- libs/inner:fleet/docs · running
```

The component of section 10 writes the rows to the member `fleet` of the
`panels` store of the UI model at each change of a member; the client of a
split session mirrors the store as it does the todo list. The page gives the
panel its rows after the todo list, and leaves it out when it does not fit,
as it does the todo list (`data/page.yaml`, page `fleet`; the state `fleet`).

## 12. The shared tree

Members of one project work in its tree. CAS publish protects the arena, not
the files: two members that edit the same file race. This is accepted. A
start with `--view` runs a member in a view of its own, applied with the
commands of views.

## 13. Limits and policy

- `fleet/scan/depth`, `fleet/scan/exclude`: the
  scan of section 6.1.
- `fleet/max_live`: the number of members that run at one time. A start above
  it waits in the task as `state: open` with no member, and starts when one
  ends.
- `fleet/approve`: `never`, `start` (a new member asks the person; the
  default) or `always`.
- A supervisor connection is admitted as any client of the server: the same
  user on the Unix socket.
- Spend: each member counts its own usage; `session_status` shows it.

## 14. Delivery

Each step is one series, with its tests.

1. **The root server.** The walk up to the parent project; every project
   server is the front of the projects below it. Tests: the walk stops at
   `$HOME`, at a ceiling directory, and at a file system boundary; a session
   of the root and a session of a project through the server of the root; a
   project below a project; `serve --projects` of a directory that is not a
   project is as before.
2. **Presence.** `fleet scan`, the `projects` member, the read of each
   arena, the search of unregistered processes (stored layer); registration
   at the project server or the root server at start, the watch for a server
   that starts later, frames to both channels, prompt runs for presence (live
   layer); `fleet list`. Tests: a project with no process is listed
   with its sessions as `stored`; a project below a project and an excluded
   directory; a killed session with work in flight is `lost`; a session
   started by hand under the root with no project server is registered at
   once, its project added, and it can be watched; a session started before
   the root server registers when the server starts; the server that stops
   leaves the session running; `/background` of a registered worker; two
   workers on one branch.
3. **The supervisor and the commands.** `send`, `interrupt`, `answer`,
   `fleet start`, names `PROJECT:BRANCH`. Tests: a message to an idle
   session and to a busy one; an answer to a question that a driver sees; an
   interrupt of a running turn.
4. **Lineage and tasks.** `fleet` and `tasks` in the stores, `fleet tasks`,
   `fleet task`. Test: a root session starts a member, which records its
   parent and task; the root marks the task done.
5. **The tool and the events.** The `fleet` tool, the supervisors that stay,
   the events, the open tasks taken up again, the restart of a lost member. Tests with the mock: the
   root starts a member, gets `idle` with the answer, the root is killed and
   comes back, a member is killed and comes back.
6. **The switchboard.** `/jump`, `/root`, `fleet attach`, the return target
   in the client. Test: a jump into a parked member, a turn there, and back
   to the root session. Not tested yet: a jump into a member that another
   client drives, and into one that no process runs.
7. **The fleet view.** First part done: the fleet panel of the page (section
   11.1). Tests: the panel shows a member and its answer; the page tests fit
   it and keep it under the todo list. Members as tiles of the work pane, with
   their stream, are not done.
8. **Policy.** `fleet/max_live`, `fleet/approve`.

## 15. Open questions

1. Hosts: a member on another machine through an ssh or HTTP endpoint of a
   server. The names and the supervisor protocol do not change; the root
   server needs a list of endpoints.
2. Channels to the root session other than a terminal (mobile, web,
   messages): `fyai-mobile-tailscale-sdd.md` and `fyai-web.md`.
