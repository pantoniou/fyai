# Future cleanups

Status: one plan is open, the split of the TUI from the core. Every other
cleanup that this document listed is done or was checked and left as it is.

Nothing keeps compatibility with old arenas or old interfaces.

## Plan: separate the TUI from the core

Status: plan. No step is done. The prerequisites are: the implicit state
machines that both sides write are enums with one owner, the oracles of the UI
model are gone, and the long functions of the UI are split. The counts below
come from a grep and must be checked before work starts.

### Goal

An interactive invocation runs two threads:

- The core thread owns `struct fyai_ctx`, its event loop, the arena, the tool
  jobs, the curl transfers and the engine.
- The TUI thread owns the terminal, its own event loop, the renderer, the
  prompt, focus, zoom, scroll, the theme and the layout.

The threads share no mutable state. They exchange frames over shared memory.
A frame is a generic, as in the Agent SDK protocol that `fyai serve` carries
on Unix sockets. The invocation stays stateless: the thread dies with the
process, and the core commits the canonical state as it does now.

### What exists already

The seam is built, but it is on the other side of a process boundary:

- The UI model (`doc/ui-model.md`) holds data and Markdown source, and holds
  no rows, no width and no theme. View state is already outside it.
- The remote worker is the engine with no display. It sends `fyai`/`store`
  and `fyai`/`text` frames. The remote client (`src/fyai_remote_ui.c`) is a
  terminal UI over a mirror of the model, and sends `fyai_answer`,
  `fyai_command`, `fyai_terminal_input`, `fyai_bang` and `fyai_sync`.
- `scope: view` commands and `display/` settings already run in the client.
- Frames are generics. Image mode (`doc/remote-sessions-sdd.md`, 5.2.1)
  sends a frame as a relocatable image, with no text parse.
- Flow control (section 5.3) already says what to drop when a client is slow:
  a store frame replaces the root, so a lost one is recovered by the next.

Thus the work is not a new protocol. A local session becomes a remote client
that is connected to an in-process worker.

### What blocks it

1. The core calls the UI directly. About 98 different `fyai_ui_*` functions
   are called from files other than the UI files. The most calls are in
   `fyai_tools.c` (90), `fyai.c` (47), `fyai_sink.c` (31), `fyai_browser.c`
   (30), `fyai_agents.c` (21) and `fyai_workpane.c` (20). Each call is one of:
   a state write (becomes a store frame), an input read (`fyai_ui_take_line`,
   `fyai_ui_peek_line`, `readline`; becomes a request frame), a question that needs
   an answer (`fyai_ask_open`; already a frame pair), or a drain
   (`fyai_diag_drain`, `fyai_ui_drain_output`; becomes a log entry).
2. The sink is one per process. The core must run with the worker sink, so it
   makes Markdown source and no rows. The sink flow (`fyai_flow.c`) and the
   width-dependent code then belong to the TUI thread only.
3. Several parts hold terminal state in the ctx: `ctx->ui`, the work pane
   (`fyai_workpane.c`), the chrome, the browser tiles, the terminal views.
   The work pane and the chrome are view state. The terminal view of a shell
   is the screen of a program, and the core owns the program.
4. A shadow band keeps a paint that nothing reads, and the span of a command
   captures output that only a mirror places. Both are work of a producer for
   a renderer that already draws from the model.
5. State machines that both sides write: the turn run gate, the sink document
   mode and band kind, the UI tool band, the pane command capture and the
   browser action. Each is an enum now, but the engine and the UI still write
   it, and a thread split makes a data race of each one.
6. Process-wide state with one owner today: signals (SIGINT with the watchdog
   on SIGALRM, SIGWINCH, SIGPIPE), the diagnostic sink (lock-free raise, and
   a drain that needs all raisers inactive), and the terminal modes.
7. A tool child forks without `exec` and runs fyai code. A fork in a program
   with two threads leaves only the calling thread in the child. A lock that
   the TUI thread held at that time stays locked for ever.

### Ownership: no shared context

The two threads do not share a `struct fyai_ctx`. Each thread has a context
of its own, and the only link between them is the channel. A pointer to a
context, to a builder or to a loop never crosses it.

- The core context keeps the name `struct fyai_ctx`: the branch and publish
  record, turn control, usage, transfer, transport client, tool jobs, shell
  sessions, agents, the asks registry, the arena and the model of the UI (as
  writer).
- The TUI context is a new `struct fyai_tui_ctx`: the terminal and the probe
  result, the renderer and the palette, the mirror of the model, the work pane
  geometry, the chrome, focus, zoom, scroll, the popup, the log view, the
  prompt text and history, completion state and the tile views.
- Display configuration, the theme variant and the size of the window go by
  value, as a frame at start and at each change.

The two contexts need two things that are inside `struct fyai_ctx` and
`struct fyai_config` today: the event loop and the diagnostics. They go out
first, as structures that a context holds by pointer and does not own.

- Event loop. `ctx->el` and the pool (`ctx->event_loop_pool`) become a
  `struct fyai_loop_ctx`: the loop, its pool, the interrupt state, the
  watchdog and the signal sources. Each thread makes one. The borrowers (curl,
  tool jobs, readline, OAuth, MCP) take the pointer of the loop context, not
  the ctx. About 9 uses of `ctx->el` name it directly, and the borrowers name
  it through `fyai_event_*` calls that take the ctx.
- Diagnostics. `cfg->diag` (`struct fyai_diag`) becomes a `struct
  fyai_diag_ctx` with its own builder and output descriptor, which the rules
  already give it. About 41 uses of `cfg->diag` change. Each thread has a
  diagnostic context. The TUI one reports TUI faults to the terminal log of
  the TUI. The core one drains at the core boundaries, and the drain goes to
  the TUI as log entries.

A function takes the context it needs: `fyai_loop_ctx`, `fyai_diag_ctx`, or
both, and not the whole ctx. A function that needs the whole core ctx is core
code. A function that needs the TUI ctx is TUI code. A file may not include
both. A check in `tests/` fails the build when a TUI file names `struct
fyai_ctx`, or the reverse.

A field that both sides write is a defect of the split. List each one in
step 0. Most will be the flags of the implicit state machines.

### Display configuration

Status: proposal. Check the key lists against the code and the schema.

Today the `display` group (68 keys in `data/config.schema.yaml`) is in the
merged configuration document, which is the branch configuration plus
`--config` plus `--set`. The group is `x-fyai-scope: session`, so a slash
command does not store it, but `config set` and the stored layer can. The
model can write it (`x-fyai-model: write` on the group).

Decide the seam by consumer, not by key prefix. A key has one of three kinds:

1. View. Only the renderer reads it: theme, layout, work pane, keys, page,
   completion, focus, glyphs, scroll, popup. It describes a terminal and a
   person, not a conversation. It must not travel with a branch through a
   merge, a rebase, a checkout on another machine or a remote session. This
   is the large majority.
2. Output. The core reads it and the result is stored or sent to a program,
   so it is intent of the conversation. Examples to check: the preview and
   history limits of a tool exchange (the stored exchange keeps the preview),
   `command_output` (it decides if a command turn is recorded), and whether
   the output is Markdown. These keys leave `display/` and go to a group of
   their own, such as `output/`, which stays in the branch.
3. Capability. What the TUI knows and the core needs: the size of the window
   for the size of a pseudo-terminal, image input, the colour depth. They go
   from the TUI to the core as a frame, at start and at each change. They are
   never configuration.

Place the view keys in a regular user configuration file, with a session
layer in the TUI context, and not in an arena:

- An arena is the history of a conversation: content-addressed, immutable,
  with a reflog. A theme change would be an entry in it, and `config undo`
  and `config reset` would mix the two.
- A file is what a person edits and copies between machines. It is the
  existing user configuration file, or a file next to it, checked against the
  same schema.
- The TUI reads it. The core in an interactive run never does. A headless run
  (a verb, `--print`) still renders with the sink, so it reads the same file
  for its colour and Markdown settings. The renderer library takes one small
  structure, `fyai_render_cfg` (planned: it does not exist yet), from whoever
  renders.
- The layers are the file, then `--set display/...` of the command line, then
  the session. A project file (`.fyai/ui.yaml`) can come later. It is not
  needed now.

This reverses one rule of `CLAUDE.md`. Today the user file only initializes an
arena that has no configuration. For view keys the file is the source and the
arena has no copy.

Effects:

- The `settings` tool and the model write access. A change of a view key
  becomes a request frame to the TUI, which applies it. The keys keep a
  `review` access, because a key that hides the work of the model from the user
  must stay visible to the user. A headless run has no such keys to change.
- Remote sessions. The client reads its own file, so the `display` frame that
  the worker sends for a `--remote` command is not needed. A command with
  `scope: view` has a clear owner: the TUI.
- Old arenas hold `display` keys. There is no compatibility rule. The schema
  does not know them, so an old key is an error, and the user removes it. A
  store that holds one is not read until then.
- Schema. `data/config.schema.yaml` splits into the core schema and a UI
  schema. The command definitions bind a setting to a key path, so each
  setting command names the schema it belongs to, and completion reads it.

### The channel

One channel is a pair of single-producer, single-consumer rings in memory
that both threads map. It is the third connection kind of the frame
dispatcher, after stdio and the Unix socket. It has three rules:

- A slot holds one frame as an image (`fy_generic_image_create()`). The
  receiver loads it with the same checks as image mode, and copies it into a
  builder of its own. No pointer into the builder of the sender crosses the
  channel. Builders are not thread-safe, so each thread keeps its own.
- A ring has a limit in bytes. When the ring of live deltas is full, the
  sender drops the queued deltas of that subscription and marks it lost. The
  receiver then gets one `fyai`/`resync` frame, as in section 5.3. A store
  frame and a request frame never drop.
- Each ring has an eventfd, or a pipe where that is not portable, as a source
  of the event loop of its reader. The writer wakes the reader once for each
  empty-to-full transition, not once for each frame.

The order rules of the remote worker stay: a text goes before the store that
names it, and a snapshot goes before a delta.

Terminal bytes of a shell use the credit window of section 5.3. The core keeps
the program and the pseudo-terminal master, and the TUI keeps no copy of the
child state. Keys go to the core as `fyai_terminal_input`. Check the latency
of a typed key with a measurement before a change of this rule.

### Threads and processes

- The core thread is the main thread. It keeps the process identity: the
  signals of the invocation, the children, `fork` and the exit status.
- The TUI thread starts after the terminal probe, which one thread runs. The
  probe result goes to the TUI in its start frame.
- A tool child that forks must not use the UI. It calls
  `fyai_ctx_fork_disown()`, which drops the channel and the thread handle
  without a join. A `pthread_atfork` handler in the TUI gives the child a
  rule: the TUI takes no lock across a call that can block, so the lock state
  at a fork is known. Keep the fork in the core thread only.
- Signals: the core thread blocks SIGINT, SIGWINCH and SIGALRM. The TUI thread
  reads the interrupt (`fyai_event_interrupt_open()`) and sends an
  `interrupt` request. This is better than today: the interrupt reaches a
  core that is stuck, because the TUI still runs. The watchdog moves with the
  interrupt, and it reports a core that does not answer.
- Diagnostics: the core drains at its boundaries and sends the drain as log
  entries. The TUI never drains.
- Shutdown: the TUI sends `quit`, the core finishes the turn as an
  interrupted one, commits, sends `closed` and the TUI restores the terminal
  and returns. A crash of either thread ends the process, so a terminal
  restore runs from `atexit` and from the fatal-signal handler of the TUI.

### The clusters of the context

`struct fyai_ctx` holds these clusters, each with one lifecycle:

1. The model transfer: `xfer_body`, `xfer_url`, `xfer_profile`, `xfer_status`,
   `xfer_retry_after`, `xfer_error`. A context has one request in flight
   (`ctx->curl`). A sub-agent and a `/btw` run are forked children, so each has
   its own copy of these fields and none is shared between concurrent runs.
   The earlier claim that these runs collide on them is not supported by the
   code.
2. Turn control: `turn_end_requested`, `turn_stop`, `turn_cost_limit`,
   `compacting`, `interrupt_*`, `terminate_pending`, `tool_diff_tracking`
   (written only by `fyai_turn_run_diff_begin()` and `fyai_turn_run_diff_end()`),
   `tool_gate*`, `relay_gate`, `relay_steps`, `output_schema`,
   `structured_output`. Each is per turn and a person clears it.
3. Usage: `usage_*`, `last_call_*`, `switch_*`, `usage_agents*`. Two phases,
   the run and the last call, are mixed.
4. The record of the next publish: `branch_op`, `branch_op_from`,
   `branch_store`, `branch_prev`, `branch_desc`, `branch_agent`,
   `project_state`, `session_unstored` (`fyai_branch_adopt()` clears it).

Clusters 1 to 3 have a reset function that `fyai_ctx_fork_disown()` calls.
Cluster 1 keeps `xfer_body`, `xfer_url` and `xfer_profile`: the child sends to
the endpoint of its parent. Cluster 4 has none: a sub-agent sets it from the
branch table when it selects its branch, and `fyai_ctx_branch_prev_set()` also
updates the todo model and the session head, which a fork must not do.

### Steps

Each step builds, passes the suites and lands as its own patch series, in
this order: implementation, tests, documentation.

0. Prepare. Delete the paint of a shadow band and the capture of a span that
   nothing reads. Give each state machine that both sides write one
   owner. List every field of the ctx with its owner (see the clusters of the
   context below).
1. Parcel out the loop. Make `struct fyai_loop_ctx` and move the loop, pool,
   interrupt and watchdog state into it. `struct fyai_ctx` holds a pointer.
   Change the borrowers to take the loop context. One context still uses one
   loop, so no behaviour changes.
2. Parcel out the diagnostics. Make `struct fyai_diag_ctx`, take it out of
   `struct fyai_config`, and change the raise and drain functions to take it.
   A forked child gets a new one (`fyai_diag_trace_reopen()` stays).
3. Split the configuration. Move the output keys out of `display/`, make
   the UI schema and the file layer, and give the renderer
   `fyai_render_cfg`. The TUI context loads it. Capabilities become a frame.
4. Split the context. Make `struct fyai_tui_ctx` and move the fields of the
   TUI table into it. The TUI code takes it, and it holds a loop context and a
   diagnostic context of its own. The UI files stop including the core
   structures. Replace each `fyai_ui_*` call from the core with a call of a
   new interface, `fyai_display_*`, of three kinds only: write a store, send a
   request with no answer, ask for an answer with a callback. In this step
   both contexts are still in one thread and the interface calls the TUI
   directly. Add the check that no file names both contexts.
5. Make the channel an abstraction (`fyai_chan`: send a frame, a receive
   callback, a limit, a lost mark) and put the Unix socket and stdio behind
   it. The dispatcher `sdk_frame` and the remote client use it. Add the
   ring as an implementation, with unit tests for order, limit, loss, wake
   and the image load of a damaged frame. Then replace the direct calls of
   `fyai_display_*` with frames on the channel.
6. One thread, two contexts, two loops, joined by the ring. This proves that
   no state is shared, and a failure is a plain test failure with no race. The
   PTY suite runs on this mode. Measure the frame rate of a long answer and
   the latency of a key.
7. Move the TUI to its own thread. Apply the rules of signals, fork and
   shutdown. Add a ThreadSanitizer build and run the whole suite on it. Add
   tests that kill the core, that stall the core, and that send a burst of
   output into a full ring.
8. Remove the old path: the in-process sink presentation of an interactive
   run, and the second interactive renderer if one remains. The remote client
   and the local session are one program. Update the layout and the event-loop
   rules of `CLAUDE.md`: each context has its own loop context.

A run with no terminal (a verb, `--print`, a sub-agent child) has no TUI
thread and keeps the plain sink. This plan does not change it.

### Risks and open questions

| Risk | Note |
| ---- | ---- |
| A fork with two threads | The worst one. Test a fork while the TUI paints. The child must touch no lock that the TUI can hold: a malloc arena, stdio and the libraries of the terminal are the suspects. Check each. |
| Latency of a key to a shell tile | One more hop. The measurement of step 3 decides whether the TUI keeps the pseudo-terminal master of a user-owned shell. |
| Completion and pickers need data of the core | Branches, the catalogue, skills. Use a request and answer pair, or let the TUI read the arena. A read of the arena while the core publishes is safe only if the arena rules hold for a second reader in the same process. Check this. |
| The arena and `fyai_arena_reopen()` | A reopen in the core must not move memory that the TUI maps. The TUI should not map the arena. |
| Slash commands | A command runs in the core, with `scope: view` as the exception. A command that writes to the sink must reach the log of the TUI as a store frame. |
| Memory | A mirror of the model doubles the live document. The two-builder rule of a store keeps it bounded. Check with a long session. |
| Event-loop pooling | The pool moves into the loop context (step 1), so two loops in one process have two pools. |
| Sanitizers | ASAN stays valid. Add TSAN, which the suite has not run before. |

Do not start step 4 before steps 0 to 3 end. A thread split over the state
machines that both sides write makes each of them a race, and no test finds
it.
