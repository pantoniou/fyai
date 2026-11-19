# Software Design Document: Fleets of sessions

- **Project:** pantoniou/fyai
- **Status:** Proposed; implementation has not started
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
- stops at `$HOME` (it looks at `$HOME` itself, not above it) when the
  directory is under `$HOME`, else at `/`;
- stops at a directory of `$FYAI_CEILING_DIRECTORIES`, a colon-separated list,
  as `GIT_CEILING_DIRECTORIES` does, and at the boundary of a file system;
- is done once at start, and costs one `stat()` a level.

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
  the walk up finds it.
- `serve --projects DIR` stays for a directory that is not a project: it
  serves no session itself, and `~/.fyai/unix/serve.sock` stays its socket.
  The refusal "the server serves projects: name one" stays for it.
- A project server that a parent server started belongs to it, as now: it ends
  with its parent. One that a person started does not.
- The root server writes the record of its own sessions as a project server
  does (`ROOT/.fyai/unix/remote.sock.live`), and answers `session list --all`
  with the records of the project servers below it.

The user starts and stops the root server (`fyai serve --daemon` in `~/work`,
`fyai serve stop`). It keeps no canonical state, as every server.

## 6. Presence

The fleet has two layers. The stored layer is canonical and needs no process;
the live layer is a hint from the servers.

### 6.1 The stored layer

A project is in the fleet when its directory is under the root and holds a
`.fyai` arena. Nothing registers a project: a scan finds it.

`fyai fleet scan` (`/fleet scan` in a session) walks the root:

- It finds each directory with a `.fyai` that holds an arena. A project below
  a project is a project of its own, as the nearest `.fyai` owns a directory
  (`fyai_discover_project_root_from()`).
- It does not go into `.fyai`, `.git`, a mount point of another file system,
  a symbolic link, or a directory that `fleet/scan/exclude` names
  (default: `node_modules`, `build*`, `.cache`). It stops at
  `fleet/scan/depth` levels (default 4).
- It reads the arena of each project read-only, as `--root` does: the
  branches that are sessions, with head, last update, task and `pending`
  table.
- It finds the running sessions that no server knows (section 6.2): the
  processes of the user whose executable is fyai and whose directory is in a
  project of the root. A process found this way is `running (unregistered)`.
  The search reads `/proc` of the user only, through a host backend
  (`fyai_host_proc_*()`), and is skipped where the host has none.

The result is stored in the store of the root branch as `projects`, an open
mapping by the directory relative to the root:

```yaml
projects:
  fyai: {scanned: 1763550000000, sessions: 14}
  libs/libfyaml: {scanned: 1763550000000, sessions: 3}
```

The scan is a hint that the next scan replaces, not the truth: a list reads
the arena of each project in `projects` again, so the state of a session is
never older than the list. A directory in `projects` that has no arena now
is dropped from the list and from the next scan.

A scan runs:

- when the root server starts, and when the root session starts;
- on `fleet scan`, and on `session list --all --scan`;
- when a name `PROJECT:BRANCH` names a project that `projects` does not hold;
- every `fleet/scan/interval_ms` while the root server runs (default 0: off).

A project that a person makes with `fyai init` under the root, or in which a
session starts, is in `projects` at once: the start registers it (section
6.2). The scan finds the projects in which nothing started since the last
scan.

A session of a project is a branch of its arena: a `session/` branch, a
`fleet/` branch, or a branch with a `fleet` member (section 9). The root reads
the arena of each project read-only, as `--root` does, for the head, the time
of the last update, the task, and the `pending` table. A project with no
process and no server is listed with its sessions in the state `stored`, and
a jump or a message starts its session (section 11).

A `pending` table whose owner does not run marks a session that was lost with
work in flight: the state is `lost`, and a start restores the work
(`fyai-background-sessions-sdd.md` 20.7).

### 6.2 The live layer

A session that starts under the root registers at once with the root server,
when it runs. It does not wait for a scan, and it does not need a project
server: the root server starts the project server, as it does for any client
of that project, and gives it the worker. A session in a project that the
root does not know yet (made now, in an excluded directory, below the depth
of the scan) adds the project to `projects` by this registration, and the
root session gets the event `[project 'PATH' added]`.

The worker finds its server by the walk of section 5.1, from its project up:

1. the server of its own project, `PROJECT/.fyai/unix/remote.sock`, when it
   answers;
2. else the server of the nearest project above it that answers. That server
   starts the project servers between it and the session, as it does for a
   client, and gives the worker to the server of its project;
3. no server on the way up: the worker runs unregistered. It watches the
   `.fyai/unix` directory of each project on the way (an inotify watch on the
   loop, through a host backend) and registers when a server appears in one.
   A session that a person started before the root server is therefore
   registered when the server starts.

A session that starts under the root registers with no project server and no
scan, as soon as some server above it runs.

A run of a prompt with no `-i` registers as well, for presence: the server
lists it and reports its state and its end, and a supervisor can interrupt or
end it. It has no UI model, so it can not be watched, and a message to it is
refused. A verb and a frame mode do not register.

The stored layer still lists a session that runs unregistered (no server, or
a host with no watch): its `pending` table, when it has one, names a live
owner, and the scan finds its process, so the state is `running
(unregistered)`.

Registration is `fyai_session_adopt` with `keep: false` and a new member
`driven: true`: the driver of the session is the client of the worker, not a
client of the server. The server enters the session in its registry, its
record and `session list` with `attached: local`. The worker sends each frame
of the UI model to its client and to the server, so watchers and supervisors
of the server see it. The server sends the worker only the intent of
supervisors and the requests of watchers.

- The server that goes leaves the worker running; the worker forgets the
  connection.
- `/background`, and the terminal that goes away, use the connection that
  exists: the server channel becomes the only channel, and `driven` goes to
  false. No second adopt is sent.
- A second worker on a branch that the server already has registers with
  `pid` as a second key and is listed as `BRANCH (pid)`. Supervisor intent
  names the branch and, when two sessions have it, the pid. Their publishes
  meet at the CAS as they do now.

### 6.3 The list

`session list --all` joins the layers: one row for each session, with
`project`, `branch`, `state` (`stored`, `lost`, `idle`, `running`,
`waiting`), `attached`, `task`, and the time of the last update. A live row
comes from the record of a server; every other row comes from the arena.
`session list --all --live` shows only the live layer.

## 7. The supervisor

A third role of a server connection, beside the driver and the watcher:
`fyai_session_attach {branch, supervise: true}`. Any number of supervisors
can attach to a session. A supervisor is also a watcher: it may read the
model and the stored turns. It may send:

| Request | Effect at the worker |
| --- | --- |
| `fyai_session_send {text, from}` | The event `[message from 'FROM']` with the text as its body. An idle session starts a turn with it; a busy one gets it at its next round. |
| `fyai_session_interrupt` | `fyai_ui_interrupt()`, as Escape. |
| `fyai_session_answer {question, answer}` | Settles an open question with the answer of the supervisor. |
| `fyai_session_end {force}` | As `session kill`. |

The driver of the session sees each of these as the user does in the session:
the message arrives as an event card, the interrupt as an interrupt, and the
answer settles the question that it shows. The supervisor never takes the
driver role and sends no keys.

A supervisor that wants changes, not frames, asks for `state: true` in the
attach. It then gets `fyai_session_state` frames in place of the UI stream:
`{branch, state: idle|running|waiting|ended, head, question, last}`, where
`last` is the last assistant text of a turn that ended. The server makes them
from the frames that it already reads for its record.

## 8. Commands

Verbs and slash commands in `data/commands.yaml`, at a server through
`--remote` and the root server:

```sh
fyai session list --all                      # every session under the root
fyai session new --project fyai --branch fleet/x --task "Fix the build"
fyai session send fyai:fleet/x "Also run the tests."
fyai session interrupt fyai:fleet/x
fyai session answer fyai:fleet/x yes
fyai session watch fyai:fleet/x              # remote connect --watch
fyai session kill fyai:fleet/x
```

A session is named `PROJECT:BRANCH`, where PROJECT is the directory relative
to the root (`.` is the root project). Inside a project, `BRANCH` alone names
a session of that project.

A session in the state `stored` or `lost` is a valid target of every verb
but `interrupt` and `answer`: `send` starts it at the project server with the
message as a turn, and `watch` starts it parked.

`session new --task` starts a parked worker at the project server, on a new
branch (`fleet/` and a name from the task when no branch is given), with the
task as its first message. `--view` runs it in a view of its own.

## 9. Lineage and tasks

The store of a member branch holds `fleet`:

```yaml
fleet:
  root: /home/user/work          # the root directory
  parent: {project: ".", branch: main}
  task: 7                        # the task in the store of the root
```

A session that a person started has no `fleet` member. `session adopt
PROJECT:BRANCH` writes one when the root session takes it over.

The store of the root session branch holds `tasks`, an open mapping by id:

```yaml
tasks:
  7: {project: fyai, branch: fleet/x, text: "Fix the build",
      created: 1763550000000, state: open}
```

`state` is `open`, `done` or `dropped`, and only the root session changes it.
Whether a member runs, waits or ended is not stored: it is read from the
servers and from the member branch. A task is a record, not prose in the
conversation, so a compaction of the root conversation loses no task.

## 10. The root session and its model

The root session is an ordinary session of the root project: a person runs
`fyai` in `~/work`, or joins the live one with `fyai resume`, and parks it
with `/background`. Its model gets these tools (`data/tools.yaml`, defined in
`src/tools/fyai_fleet.c`):

| Tool | Does |
| --- | --- |
| `session_start` | `session new --task`: makes a task and a member. |
| `session_send` | Sends a message to a member. |
| `session_status` | The sessions under the root, with state, task and last answer. |
| `session_read` | The last N exchanges of a member branch, from its arena. |
| `session_stop` | Interrupt or end a member; drop or close its task. |
| `task_update` | Marks a task done or dropped. |

Only a session of a root project gets these tools. A member has its
sub-agents and no fleet tools: a member that needs more work done says so in
its answer, and the root session decides. A person runs the commands of
section 8 from any directory of the tree.

The tools send data to other sessions and start programs, so their
configuration keys are `review` or `read` (`x-fyai-model`), and
`fleet/approve` decides which calls ask the person.

The root session attaches to each member of an open task as a supervisor with
`state: true`. A change arrives as an event:

```text
[session 'fyai:fleet/x' idle]        body: the last answer
[session 'fyai:fleet/x' waiting]     body: the question
[session 'fyai:fleet/x' ended]
[session 'fyai:fleet/x' restarted: the session was lost ...]
```

The events of a member are its own; the root model gets no UI stream.

### 10.1 Durability

- A member is a process of its own. It does not end with the root session.
- The `pending` table of the root session gets a `session` row for each
  supervisor attachment. A restart of the root session attaches again and
  reads the head of each member, so a change during the loss arrives as one
  event.
- A member that ended without a result while its task is open (its worker was
  lost) is started again at its project server on its branch; its own
  `pending` table restores its work, and the root model gets `restarted`.
- A member whose project server is not there: the root server starts it.
- With no root server, the root session runs the member as a worker of its
  own (a child, as `/background` would make one) and supervises it through
  the channel of that child; the member is a peer in every other respect.

### 10.2 What the root is not

The root holds no free-form memory store and no hidden plan. What it knows is
its conversation, its tasks, its skills, and the branches of the projects. A
person reads all of it with the commands that exist: `branch log`, `session
list --all`, `session watch`.

## 11. The switchboard

A person in the root session jumps into any session under the root, controls
it, and comes back. The root session goes on while the person is away: it is
parked at the root server, and its events wait for the next turn.

- `/jump PROJECT:BRANCH`, or the picker of `session list --all` (`/sessions`,
  the live rows of every project), chooses the session. The root session
  goes to the root server as for `/background`. The client then runs
  `remote connect` to the project server of the member in its place, as the
  resume picker does for a live row now (`cfg->attach_branch`), with the
  project and the session to return to.
- A member with no driver gets the client as its driver: keys, lines,
  questions, interrupts, as in a session of its own.
- A member that a person drives in another terminal keeps that driver. The
  client attaches as a watcher and a supervisor: a line that it types goes
  as `fyai_session_send` from `user`, Escape as `fyai_session_interrupt`, and
  it answers the questions that it sees. The display says so.
- `/root`, and `/background` in a session that was jumped into, leave the
  member as it was before the jump (parked or with its own driver) and run
  `remote connect` back to the root session. `/exit` leaves the member as
  `/background` does and ends the client.
- A jump into a session that is not live (`stored` or `lost`) starts it at
  its project server on its branch, as `resume` does, and a project with no
  server gets one from the root server. A `lost` session restores its work
  first.
- The status line of a jumped-into session names it and the way back
  (`fyai:fleet/x · /root`).

The root session gets the event `[session 'PROJECT:BRANCH' jumped: the user
controls it]` and `... returned`, so its model knows that the person worked
there.

## 12. The shared tree

Members of one project work in its tree. CAS publish protects the arena, not
the files: two members that edit the same file race. This is accepted. A
start with `--view` runs a member in a view of its own, applied with the
commands of views.

## 13. Limits and policy

- `fleet/scan/depth`, `fleet/scan/exclude`, `fleet/scan/interval_ms`: the
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
   layer); `session list --all`. Tests: a project with no process is listed
   with its sessions as `stored`; a project below a project and an excluded
   directory; a killed session with work in flight is `lost`; a session
   started by hand under the root with no project server is registered at
   once, its project added, and it can be watched; a session started before
   the root server registers when the server starts; the server that stops
   leaves the session running; `/background` of a registered worker; two
   workers on one branch.
3. **The supervisor and the commands.** `send`, `interrupt`, `answer`,
   `session new --task`, names `PROJECT:BRANCH`. Tests: a message to an idle
   session and to a busy one; an answer to a question that a driver sees; an
   interrupt of a running turn.
4. **Lineage and tasks.** `fleet` and `tasks` in the stores; `session adopt`.
5. **The tools and events.** The tools, `state` attachments, the `session`
   rows of `pending`, the restart of a lost member. Tests with the mock: the
   root starts a member, gets `idle` with the answer, the root is killed and
   comes back, a member is killed and comes back.
6. **The switchboard.** `/jump`, `/sessions`, `/root`, the return target in
   the client. Tests: a jump into a parked member and back; a jump into a
   member that another client drives, a line that arrives there as a message;
   a jump into a member that is not live.
7. **The fleet view.** Members as tiles of the work pane in the root session.
8. **Policy.** `fleet/max_live`, `fleet/approve`.

## 15. Open questions

1. Hosts: a member on another machine through an ssh or HTTP endpoint of a
   server. The names and the supervisor protocol do not change; the root
   server needs a list of endpoints.
2. Channels to the root session other than a terminal (mobile, web,
   messages): `fyai-mobile-tailscale-sdd.md` and `fyai-web.md`.
