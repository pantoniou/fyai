# The context as a set of components

Status: design. No code is changed. The counts come from a script over
`src/*.c` that counts `ctx->FIELD` per file; check a row against the code
before a change. The first step is at the end.

## Problem

`struct fyai_ctx` holds 176 fields for about twenty components. Each component
reaches the fields of the others through `ctx`, and a function that takes `ctx`
can use all of them. Three effects follow:

- A component has no stated dependencies. `fyai_cred.c` needs the event loop,
  the diagnostics and the credential store, but it takes `ctx`, so nothing
  stops it from reading the sink or the branch.
- A reset is a hand-written list (`fyai_ctx_fork_disown()`), because no
  component owns the state that it must forget.
- The split of the TUI from the core (`doc/future-cleanups.md`) needs two
  contexts. A context that is one struct of everything cannot be cut.

## Rules

1. A component is a struct that owns its state, a constructor, a destructor
   and an `abandon()` for a forked child. The component holds the resources
   that it makes: its sources on the loop, its builders, its descriptors.
2. A component names its dependencies in a struct of pointers that the
   constructor takes, `struct fyai_X_deps`. A dependency is a pointer to
   another component, never a copy of its fields and never `ctx`. The pointers
   stay valid for the life of the component, so the owner makes dependencies
   first and destroys them last.
3. A function of a component takes the component (`struct fyai_X *`). It does
   not take `ctx`, and it reaches another component only through `X->deps`.
4. `struct fyai_ctx` is the composition root. It holds each component, makes
   them in dependency order, wires the pointers, and destroys them in the
   reverse order. It holds no state of its own besides what no component owns.
5. The graph has no cycle. When two components need each other, one of them
   gets a callback (a function pointer and its data) instead of a pointer, as
   `turn_observe` is today.
6. `fyai_ctx_fork_disown()` calls `abandon()` of each component in reverse
   dependency order. A new component adds one line there, and the component,
   not the list, says what it forgets.
7. A component keeps the lifetime rules of the data model: a builder that it
   owns for its values, an interned `const char *` for a long-lived name
   (`CLAUDE.md`, "Copies of arena data").

## Components

Fields are the fields of `struct fyai_ctx` today. "Users" are the components
whose files read them, from the count; a user that is not a dependency of the
owner is a defect of today's layout.

### Services without dependencies

| Component | Owns | Today |
|---|---|---|
| `diag` | the diagnostics: the collected list, its builder, the trace and the dump descriptor | `cfg->diag`, `dump_fd`, `dump_dir`, `dump_file` |
| `scratch` | the transient builder and its allocator; releases itself on an idle iteration | `transient_allocator`, `transient_gb`, `transient_autorelease` |
| `arena` | the durable allocator and builder, the overlay, the working builder `gb`, the refs head | `durable_allocator`, `durable_gb`, `overlay_allocator`, `gb`, `refs_head` |

`scratch` needs the loop for its idle release: that edge is a callback source
that the loop component accepts, so `scratch` depends on `loop`, and `loop`
does not depend on `scratch`.

### The loop

| Component | Owns | Depends on |
|---|---|---|
| `loop` | the event loop and its pools, the signal sources and mask, the interrupt state (`interrupt_pending`, `interrupt_seq`, `interrupt_seen`), the terminate request, the curl multi state | `diag` |

The loop is the one component that every other service borrows. The field `el`
is named in 14 places only; the rest reach the loop through
`fyai_ctx_loop(ctx)`, so the dependency is hidden in the call, not in a field.

### Network and credentials

| Component | Owns | Depends on |
|---|---|---|
| `cred` | the shared refresh of the login, its waiters, the detached refresh count, the credentials, the builder of the credential strings (`auth`, `auth_gb`, `auth_detached`, `cred`) | `loop`, `diag`, the secret store |
| `transport` | the transport client state (`ctx->transport`, `struct fyai_transport_state`) | `loop`, `diag`, `cred` |
| `xfer` | the model transfer: the curl handle, the headers, the auth header, the user agent, the outcome of the last transfer, the request cache | `loop`, `diag`, `cred`, `transport`, the configuration |
| `mcp` | the connections to the servers, the tool catalogue, the active names | `loop`, `diag`, `cred`, `scratch` |

Counts that say where the dependencies are loose today: `auth` reads
`transient_gb` 17 times, which a builder of its own (`auth_gb` exists)
replaces; `xfer` reads the UI fields 15 times, which is a callback for a retry
report, not a dependency; `mcp` reads `transient_gb` 18 times and the
configuration 33 times.

### Tools and processes

| Component | Owns | Depends on |
|---|---|---|
| `scope` | the cover, the scope pins, the orphans and their kill timer, the home of the tools, the sandbox flags | `loop`, `diag`, the configuration |
| `terminal` | the size of the real terminal, the pseudo-terminal session, the resize source, the `stdout_tty` fact | `loop`, `diag` |
| `jobs` | the tool jobs, the control channel to a tool child, the background runs of sub-agents, side questions and monitors, the shell sessions, the live shell stream, the patch display | `loop`, `diag`, `scratch`, `scope`, `terminal`, `ui` (a callback set), `turn` (a callback set) |
| `toolspec` | the tool specification and its generation, the allow list | the registry, `skills`, the configuration |
| `skills` | the directory that the skills are written to, the skill trees, the settings export directory | `scratch`, `arena` (reads the branch skills) |

`jobs` has the widest list of users (118 reads of the configuration, 79 of
`scratch`, 67 of the UI). Most UI reads are presentation: they become the
display callbacks of the job (open a band, paint a head, commit), the same
callbacks that the remote worker must provide in the TUI split.

### Waiting and asking

| Component | Owns | Depends on |
|---|---|---|
| `waits` | the named waits, the waiters that hold a call, the earliest-deadline timer, the kick flag, the queue of events for the model | `loop`, `diag` |
| `asks` | the background questions, the sets of the questions store, the serial numbers, the `--answer` cursor, the abort flag | `diag`, a sink of questions (callback) |

`waits` and `asks` are the cleanest cuts after the services: each is read by
one file (`fyai_wait.c`, `fyai_asks.c` and `fyai_ask.c`), and the only
outside user of `waits` is the loop (9 reads of `events`, which is a queue the
loop wakes on).

### The conversation

| Component | Owns | Depends on |
|---|---|---|
| `branch` | the active branch, the head branch, the unstored session, the branch table, the arena config, catalogue and skills, the record of the next publish (`branch_prev`, `branch_desc`, `branch_op`, `branch_op_from`, `branch_agent`, `branch_store`), the project states and the tool changes | `arena`, `diag`, the configuration |
| `usage` | the usage of the run, the usage of the sub-agents, the cost of a model switch, the usage of the last call | `diag` |
| `turn` | the last message, the observers, the gate, the stop reason, the output schema, the cost limit, the response chain, the token extents, the pending attachments | `branch`, `usage`, `xfer`, `toolspec`, `jobs`, `mcp`, `waits`, `scratch`, `loop`, `diag`, `ui` (callbacks) |
| `agents` | the registry, the execution and parent ids, the agent branch and the spawn state, the gate relays | `branch`, `jobs`, `turn` (callback), `loop` |

`turn` is the hub: its files read the fields of 18 components. That is the
engine, so the width is expected, but each read becomes a named pointer in `struct fyai_turn_deps`, and
a read that a callback can replace is replaced. `usage` is read by `turn` 31
times and by `jobs` 13; nothing else reads it.

### Display

| Component | Owns | Depends on |
|---|---|---|
| `sink` | the one rendering component (`ctx->sink`) | `diag`, `loop`, the display configuration |
| `uimodel` | the stores that renderers read, the work store, the transcript store, the log span, the renderer of the model | `scratch`, `diag` |
| `ui` | the terminal UI, the page, the work pane, the browser, the command in flight, the editor and catalogue update requests | `sink`, `uimodel`, `loop`, `terminal`, `diag` |

This is the part that the TUI split moves to the other thread. Its reads of the
engine are few: 10 of `turn`, 7 of `arena`, 6 of `branch` and 11 of
`terminal`. The other direction is wide: `jobs` reads the UI fields 67 times
and `turn` 31 times. Each of those is an item of the channel of the split: a
request, a store write or a log entry, and until then a display callback.

### Configuration

`cfg` (`struct fyai_cfg`) is read by 82 files, 988 times. It is not a component
of this graph: it is the value that components read for settings. A component
takes `const struct fyai_cfg *` in its deps when it reads settings, and takes
only the sub-structure that it needs once the configuration is split into core
and view parts (`doc/future-cleanups.md`, "Display configuration"). `cfg->diag`
leaves `cfg` with the `diag` component.

## The graph

Arrows go from a component to what it depends on. This is a sketch of the
edges that the counts show, not a complete list; the graph has no cycle once
the callbacks of rule 5 stand in for the edges that point up (`jobs` to `ui`
and to `turn`).

```
                                  turn
                  agents       /  |   \    \
                     |        /   |    \    \
                   jobs   usage  xfer  mcp  toolspec - skills
                  / | \ \          |     \     |
            scope term ui  \       |      \    |
               \    |  |    \      |       \   |
                \   |  |  branch   |        \  |
                 \  |  |     \     |         \ |
                  \ |  |    arena  cred -- transport
                   \|  |      |      |        |
                   scratch   |       |        |
                      \      |       |        |
                       +-----+-------+--------+
                                |
                              loop
                                |
                              diag
```

`waits` and `asks` hang off `loop` and `diag` alone. `sink`, `uimodel` and
`ui` sit beside `jobs` and depend on `loop` and `diag`.

## Order of work

Each step builds, passes both suites, and lands as its own patch series
(implementation, tests, documentation), as `CLAUDE.md` asks.

1. `diag`. Take the diagnostics out of `struct fyai_cfg` into a component
   that owns its builder and its descriptor. `fyai_error()` and its family
   take the component. It has no dependencies, so it is the proof of the
   pattern. This is step 2 of the TUI plan.
2. `loop`, with `diag` as its dependency. Move the loop, the pools, the signal
   state and the interrupt state into it. This is step 1 of the TUI plan.
   `fyai_ctx_loop()` stays as a thin accessor until the last user is gone.
3. `scratch` and `arena`: the builders, with `loop` for the idle release.
4. `cred`, then `transport`, `xfer`, `mcp`: the example of the first
   consumers. `cred` takes `loop` and `diag`; its 17 reads of `transient_gb`
   become its own builder. Each of the four ends with a unit test that builds
   the component with a stub loop and diagnostics, which is the test that a
   struct of pointers makes possible and `ctx` did not.
5. `waits` and `asks`: each has one file and one owner, so each is a short
   change.
6. `scope`, `terminal`, `skills`, `toolspec`.
7. `branch`, `usage`.
8. `jobs`, `agents`, `turn`: the wide components, with the callbacks of the
   display and of the observers. These change the most call sites.
9. `sink`, `uimodel`, `ui`: this part ends in the second context of the TUI
   split.

After step 8 `struct fyai_ctx` is the composition root of rule 4, the list of
`fyai_ctx_fork_disown()` is a loop over `abandon()`, and the reset functions
of the clusters (`fyai_ctx_turn_reset()`, `fyai_ctx_usage_reset()`,
`fyai_ctx_xfer_reset()`) are the `abandon()` of `turn`, `usage` and `xfer`.

## Questions that the work must settle

- Where the configuration is read by a component that is also the owner of the
  state that depends on it, as `tools_spec_generation` depends on
  `config_generation`: the component takes the generation number as part of
  its deps, or the owner pushes the change.
- Whether a component may hold a pointer to the `ctx` of a forked child. A
  child calls `abandon()` and then rebuilds the components that it needs, so no
  component keeps a pointer from a parent.
- The names: `struct fyai_X` is free for most components, but `struct
  fyai_cred`, `struct fyai_transport_state`, `struct fyai_agents` and
  `struct fyai_workpane_manager` exist and are the first of the components.
  They get a `deps` struct, not a new name.
