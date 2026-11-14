# Future cleanups

Status: the document has four parts.

- Part 1 lists implicit state machines. Open: the shell session end and its
  display mode (item 2) and the reset of the publish record in a forked child.
  The entries that stay were checked and need no change.
- Part 2 plans the split of the TUI from the core. It is a plan, and no step
  is done.
- "Other cleanups" lists what is open: the parallel-construction oracles
  (item 9), the split of `utils.c` (item 16), the long functions (item 17),
  the focus and zoom code of `fyai_shell_session.c` (item 15) and the tests
  that poll (item 19). The wait of a delegated child for an answer to its
  question stays by design (item 1).
- "Copies of arena data" lists copies that a generic or an interned string
  replaces. Open: the rows of class a and class b that the status paragraph
  does not name.

Nothing keeps compatibility with old arenas or old interfaces.

## Purpose

This document lists state machines that the code holds in a combination of
flags, counters and pointers, with no enum that names the state. The
transitions of such a machine are in many functions, and the flags allow
combinations that no transition makes.

Three read-only surveys made the list, one for each area: the UI and display,
the tools, jobs and agents, and the turn engine, the transport and the storage.
The inferred states and the illegal combinations come from the struct
definitions and from the writes that the survey found. Check each item against
the code before a change.

## Patterns

- Several booleans are one enum. A lifecycle is in four or more flags, which
  one `begin` function sets together. Other functions derive the mode again
  with guards such as `if (!a && !b)`.
- A NULL pointer and a boolean record one fact. Examples: `session->job`,
  `job->csrc`, `term_src`, `session_id`, `group`.
- A derived flag is computed again at each event. Examples:
  `fyai_tool_job_settle()`, `agent_active()`,
  `mcp_teardown_settled()`.
- A kind is a set of booleans. Examples: `agent`, `exec`, `btw` and
  `native_shell` on a job; `poll`, `sleep` and `on_event` on a waiter.
- A hand-written reset list stands in for a reset point. The model transfer,
  the turn control and the usage of a forked child each have a reset function
  (`fyai_ctx_fork_disown()`); the publish record has none.
  `auth_retry_done` is not a field of `struct fyai_ctx`: it is a latch of
  `struct fyai_mcp_call_request` and lives and dies with the request.
- A state enum exists, but side flags change its meaning. Examples:
  `stream_response`, `fyai_turn_run` and `fyai_model_step`.
  `fyai_mcp_call_request` and `fyai_auth_login_request` were listed here; their only
  side flag, `cancel_requested`, is now the CANCELLING state.

## High priority

### 2. Shell session end

`struct fyai_shell_session`, `fyai_shell_session.c`.

- Open: the clearing sequence of `fyai_shell_session_exited()`, and the display
  mode (`inline_block` or `surface`).
- After the end `job` can stay set and `wants_input` can stay stale; `waiter`
  and `pinned` are cleared by `exited()`.

### 6. Transport server request

`struct req`, `fyai_transport_server.c`.

- Fields: `rejected`, `retried`, `started`, `paused`, `cancelled`, `finished`;
  the pointers `park`, `retry`, `easy`, `xfer`, `headers`.
- States: PARKED (waits for a credential), RUNNING, STARTED, PAUSED by credit,
  REJECTED (401), RETRIED, FINISHED, CANCELLED. The transitions are in
  `req_rejected()` and four other places.
- Illegal combinations: `finished` with `paused`; `cancelled` without
  `finished`; `retried` without `retry`; `rejected` with `started` (the retry
  is valid only before the start frame); `paused` without `easy`.
- The client end, `struct fyai_tcall`, has `started`, `ended` and `cancel_sent`
  with the same pattern.
- Checked, no change. A request can end with an error before its start frame
  (`finished` without `started`), so `started` is not a step between running
  and finished, and an ordered phase would change what the code after a failed
  request tests. `cancelled` is a one-way request and `paused` describes curl,
  not the request. Only `rejected` and `retried` form a sequence, and each is
  read at one site.

## Medium priority

- Interrupt and terminate (`fyai.h`): checked, no change. `terminate_pending` is
  a one-way request that ends the invocation (SIGTERM, or a cancel of an
  agent), so it is never cleared. `interrupt_pending` is acknowledged and
  cleared. Several sites still rebuild `interrupt_pending` from
  `seq != seen`.
- Terminal child (`struct tty_run`, `struct fyai_terminal_relay`): checked, no
  change for `tty_run`. The result states its precedence: `timed_out` wins,
  and `cancelled` is `cancelling && !timed_out`, whichever came first. A
  state enum would record the first cause and change that. `relay_finish()`
  can still set `done` without `reaped`.
- MCP teardown (`del_done`, `term_done`, `pid`, `session_id`): checked, no
  change. The session delete and the stop of the child are independent events
  that finish in either order, and `mcp_teardown_settled()` is their join. A
  structure that never began teardown cannot be read as one that is tearing
  down: `fyai_mcp_stop_done()` needs `mcp_stopping`, and `fyai_mcp_stop()`
  begins every server in the pass that sets it. A server added after that
  pass would block the join, but no path adds one then.
- UI input owner (`busy`, `marker_busy`, `questions`, `head`,
  `editor_request`, `external`, `editor_external`, `inline_focus`): checked, no
  change. The fields are independent facts, not one phase: `busy` is a turn,
  `questions` and `head` are lists, `editor_request` is a request pointer,
  `external` is a suspended terminal and `inline_focus` is a key. The marker
  and the interrupt are a priority list over them, and `marker_busy` is the
  last state drawn, a cache that avoids a repaint.
- Agent registry UI mode (`surface`, `attached`, `question_presented`,
  `draft`): checked, no change. A question can be presented while a view is
  zoomed or attached, each with its own draft, so QUESTION is not exclusive of
  the view. `attached` is the kind of the one view, and `surface` is the
  tile that it owns; `fyai_agents_detach()` is also the error path of zoom and
  is safe for a view that has no tile.
- Live document pause and end: checked, no change. The three components keep
  different facts. `fyai_display_output` holds what the producer asked for,
  `fyai_uitranscript` holds the published store, and `fyai_uidraw` holds what
  the renderer gave the sink, so a mirror can stay behind the store. A merge
  would remove that difference. `paused` is a toggle inside the live phase.
- Progressive markdown stream (`live`, `active`, `render_pending`,
  `full_render`, `flush_src`): checked, no change. `full_render` is a one-way
  latch of one stream: a repaint, a change of width and the close set it, and
  it is not cleared on purpose, because the window of the tail is wrong after
  the width changed. `live` is set when the stream starts, `active` is its
  life (the finish clears the structure), and `render_pending` is a request.
- Claude SDK server (`initialized`, `mcp_starting`, `init_id`, `busy`,
  `failed`): checked, no change. `failed` is a latch for a broken connection
  and the loop ends on it, so it is not cleared. `initialized` and
  `mcp_starting` are two events that finish in either order, and `init_id` is
  their join. `busy` is the turn.

## The context structure

`struct fyai_ctx` holds these clusters, each with one lifecycle and no common
reset:

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

## Suggested order

The reset of the publish record is the open step of part 1. The event core, the PTY driver and the markdown stream need no
change.

## Plan: separate the TUI from the core

Status: plan. No code was changed. The counts below come from a grep and
must be checked before work starts.

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
4. The parallel construction (the oracle, `fyai_uimodel_check()`) compares the
   model with the legacy path in one process. It cannot run across two
   threads. The legacy path must be removed first.
5. The implicit state machines of this document. Many are written by both
   sides: the turn run gate, the sink document mode and band kind, the UI tool
   band, the pane command capture and the browser action. While a flag is written by the engine and by the UI, a thread split makes a data
   race of each one.
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

### Steps

Each step builds, passes the suites and lands as its own patch series, in
this order: implementation, tests, documentation.

0. Prepare. Finish the parallel construction and delete the legacy paths and
   the oracle. Close item 2 of this document, so that
   each machine has one owner and an enum. List every field of the ctx with
   its owner.
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

Do not start step 4 before steps 0 to 3 end. A thread split over the flags that
this document lists makes each of them a race, and the oracle cannot find it.

## Other cleanups

Status: survey by one read-only agent. It checked each item against the code
unless the item says unverified. Size: S is a day or less, M is a few days, L
is a series. Check an item again before work on it.

### Rule violations

1. By design, a delegated child that asks its parent steps its own loop in the
   dispatch of its tool child (`fyai_ask_user_upward()`, `src/fyai_tools.c`).
   That dispatch is the top level of the child, as the serve loop of a terminal
   session is, and no callback is on its stack. The text path (`--answer`, a
   terminal with no page) reads a line and steps nothing.

### Second owners

9. The parallel-construction oracles (L). They still run in a production
    repaint: `fyai_chrome.c:410`, `fyai_session.c:1957` and `:2002`,
    `fyai_ask.c:434`, `fyai_uidraw.c:44`, `:100` and `:487`, `fyai_output.c`,
    and four calls in `fyai_ui.c`. This is step 0 of the TUI split. Remove
    each one when its part has converged.

### Dead code and compatibility

15. `fyai_shell_session.c` still holds the focus and zoom code (about 1000
    lines) that could be a file of its own.

    own.
16. `src/utils.c` holds shell capture (490-1277, which overlaps
    `fyai_tools.c`), the editor pane, and JSON helpers (M). Move shell capture
    and the editor into files of their own, and correct the one-line entry in
    `CLAUDE.md`.
17. Long functions (M to L each), with the split seam: `fyai_config_apply`
    (474 lines, `fyai_config.c:398`; a table of key and setter),
    `fyai_tool_job_submit` (351; validate, build, spawn), `capture_run`
    (347, `fyai_project_capture.c`), `ui_page_update` (323; one function for
    each region), `fyai_agent_run` (312), `fyai_config_setup` (308; argv,
    layers, validation), `fyai_cmd_claude_sdk` (279), `fyai_view_capture`
    (268), `capture_scan_dir` (252) and `ui_service` (250; key routing).

### Documentation and tests

18. Other plans in `doc/` name functions that the survey did not find
    (`markdown-ui-plan.md`, `transport-isolation-review.md`,
    `state-sync-sdd.md`, `command-schema-plan.md`,
    `macos-transport-isolation-sdd.md`, `web-fetch-plan.md`). They may be
    plans, not errors.

19. Timing in tests (S). Open, with no portable wake source:
    `reads_stdin_is_seen` polls the state of a process that is no child, and
    `transport_bootstrap.sh` and `retry_interrupt.sh` poll a file that a
    running program writes (`tail --pid` is not on macOS).


The agent found no hand-built JSON outside `fyai_mock.c`, no second transcript
renderer, and no event source registered with a discarded handle. Whether each
of the 475 cases has a positive control is unverified.

### Suggested order

The remaining timing sites of item 19 need a wake source and not a poll
interval. Item 9 and items 1 and 15 go with the state-machine and TUI work,
because they touch the same code.

## Copies of arena data

Status: survey by three read-only agents over `src/*.c`. About 440 `strdup`
sites exist. Roughly 300 are correct (see "Keep"). The rest copy a generic into
the C heap, or round-trip it through JSON text, only to pass it to a consumer
that could hold the generic. Each lifetime claim comes from reading the code,
except where the item says unverified. Size: S is a day or less, M is a few
days. Check an item again before work on it.

Checked and kept: the question text of `fyai_ui.c` (`ui_ask_copy()`). The UI
model store has two builders and a write resets one of them, so a string of
a root is valid for one further write at most, and a question lives until the
user answers it. The oracle path of `fyai_ui_ask_oracle()` passes arguments of
the transient builder. The command line of a tool job is new for each call.

Dropped: the event queue. Its text is formatted by the producer and is new for
each event, so no arena string is copied, and it must not go into the
configuration builder. Only the eight `monitor_event(strdup(fy_sprintfa()))`
calls of `fyai_monitor.c` could use `fyai_event_injectf()`. ACP (`worker->text`,
`message_id`, `agent_id`) and `uidraw` titles and commands are new for each
turn or call, so they stay copies. The export call list of `fyai_display.c`
stays: a mapping copies at each add. `peer->dir` takes a `realpath` string.

### Rules for this list

- A `fy_generic` field with `fy_castp(&s->field, "")` at the use site replaces a
  `char *` field with `strdup()` and `free()`. A short string is in the
  generic word, so the cast must read the field and not a copy.
- `ctx->gb` and `ctx->durable_gb` are not stable across `fyai_arena_reopen()`.
  A generic in them must not be borrowed across a reopen. Copy it into
  `ctx->cfg->gb` (process lifetime, deduplicated, never freed). The code does
  this already: `fy_gb_intern_string(cfg->gb, ...)` in `fyai_mcp.c`,
  `fyai_acp.c` and `fyai_cmd_store.c`.
- `ctx->transient_gb` is released on an idle loop iteration. Nothing
  asynchronous can borrow from it.
- An object that lives longer than one call and holds many values owns a
  builder, as `job->call_gb` and the `asks` record (`p->gb`) do
  (`FYGBCF_SCOPE_LEADER | FYGBCF_DEDUP_ENABLED`).
- A string that does not change for each turn or exchange is interned into
  `cfg->gb` and kept as a `const char *`. Examples: a branch name, a model
  name, an agent or session name, an id, a path, a persona, an endpoint, a
  `cwd`. The builder deduplicates, so a repeated value costs nothing, and the
  pointer is stable for the life of the process. No `free()` and no OOM check
  remain. Use it in place of a `fy_generic` field when the consumer needs a
  `const char *` and has no reason to hold a generic.
- Use a `fy_generic` field (cast with `fy_castp(&s->field, "")`) when the value
  is built in an arena the owner keeps, or is passed on to `fy_assoc()` or
  `fy_mapping()` and thus needs no cast.
- Do not intern a value that is new for each turn, exchange or event: the
  agent `source`, rendered text, diffs, typed text, tool-result text, the text
  of an event, a shell command line. `cfg->gb` never frees, so such values
  would grow it for the life of the process.
- Do not intern tokens and secrets. They must be zeroed (`fyai_secret_clear`)
  and a builder never zeroes.

### Recurring shapes

All model tools are `.run` and return generics. `read_file`, `apply_patch` and
`shell` build their output in a malloc'd buffer and move it with
`fyai_tool_take_text()`, `wait` does the same for its waiter result: the buffer
is not a copy of a generic, so it stays.

1. The event queue holds `char *`: dropped (see the status paragraph). The
   text of an event is new for each event and must not go into `cfg->gb`.
   The monitor formats an event once, through `fyai_event_vinjectf()`.

### Class a: the copy is not needed

The source generic outlives the consumer. Replace the copy with a generic or a
borrowed pointer. The sites that the status paragraph above names as done,
checked, kept or dropped are not listed again.

| Site | Field | Note |
|---|---|---|
| `fyai_browser.c:488` | `copy` | Two more copies follow in `browser_add_path()` and `browser_add()`. S |
| `fyai.c:1961` | `cause` | Unverified: needed only if `gate_refuse` clears `a->text` first. S |
| `utils.h:196` | `fyai_bytes_from_generic()` | Two callers. Unverified: `fy_castp()` with a length may do. S |

### Class b: one intern replaces the copy

The source is transient and the consumer lives long. Intern into `cfg->gb`
with `fyai_ctx_keep_str()` and keep a `const char *`. Every field below is a
name, an id, a path or a bounded set, so none changes for each turn.

- Claude SDK (`fyai_claude_sdk.c`), with a builder in `struct sdk_server`:
  `d->claude_name`, `fyai_name`, `tool_use_id` (1560-1564), `p->rpc_id_json`
  (1835), `in->command_id` and `srv->init_id` (1964, 2404, already passed to
  `sdk_respond()` as a generic), hook `event` and `callback_id` (1179-1180),
  `t->name` (2039), `img->media_type` and `name` (2554-2555), `in->origin_json`
  (2724), `sdk_user_text()` (2578), `cfg->fresh_session_from` and
  `fresh_session_name` (674-726). S each. `img->data` stays.
- Agents and sessions: `run->name` (`fyai_agent.c:953`), `run->branch`
  (`fyai_session.c:2240`, four frees), `fyai_agents.c:400-401, 458`
  (`r->branch`, `r->model`), `fyai_agents.c:1530-1585` (the message ack, six
  fields, M), `fyai_agents.c:1604`, `fyai_monitor.c:316`, `fyai_asks.c:167`
  (into `p->gb`), `fyai_acp.c:915-917` (`worker->text`, `message_id`,
  `agent_id`; its sibling fields are interned already), `fyai_acp.c:693-694`.
- Remote: `fyai_remote_server.c:294, 519` (`session->branch`), `:1983`
  (`peer->dir`), `fyai_remote_ui.c:388, 603` (`t->name`, `view->epoch`).
- Display: `fyai_uidraw.c:416, 515, 585-586` (`language`, `title`,
  `command`; bounded sets), `fyai_browser.c:342-421` (the scan builder is
  local and destroyed, which is why the rows are copied; keep one builder for
  the life of the browser and make `cwd`, `title` and `path` generics, M),
  `fyai_tools.c:386` (`entry->id`).
- Storage: `fyai_cmd_fsview.c:3195` (`run->name`).
- `fyai_skill.c:1591` (`d->name`), `fyai_wait.c:345, 805, 816` (names and
  reasons), `fyai_shell_session.c:475-491` (`name`, `branch`, `title`,
  `description`: the API takes `const char *`, so the call sites stay). The
  `command` of a session is new for each call and stays a copy.

### Keep

About 300 sites copy a value that is not arena data, or must have a mutable
or zeroable copy. Do not change them:

- mutable buffers: `strtok_r`, in-place path split (`fyai_catalog.c`
  506-589), `settings_clean`, `fyai_media_take_mentions()`, line buffers
  (`fyai_monitor.c:139`);
- paths and environment: `getenv`, `getcwd`, `realpath`, `mkdtemp`, argv,
  `-k key=val`, and `asprintf` path building;
- credentials that `fyai_secret_clear()` zeroes: tokens and secrets of
  `fyai_mcp.c` (931, 937-938, 1397-1446), `fyai_transport*.c`,
  `fyai_ws_client.c`, auth;
- strings given to a thread, a fork child or curl;
- text typed by the user, rendered text and diffs (`b->viewed` of
  `fyai_uidraw.c:453`, `entry->unified` of `fyai_tools.c:389`), and the agent
  `source` (`fyai_agents.c:468`), which is replaced on every event;
- formatted text (`asprintf`, `fy_sprintfa`) that a `char *` API returns, such
  as `fyai_tool_error_cause()`, `fyai_schema_report_string()` and
  `fyai_ask_text()`;
- the transport and server code, which is C-heap by design;
- `fyai_patch.c` (a text engine, 29 sites).

### Suggested order

1. Class a: the rows that remain, file by file.
2. Class b with `fyai_ctx_keep_str()`, file by file. A field that is new for
   each turn or call stays a copy.

Each change is one patch with its test. An ASAN run shows a borrowed pointer
that outlives its builder.
