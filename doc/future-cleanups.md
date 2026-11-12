# Future cleanups

Status: survey and plan. No code was changed. Part 1 lists implicit state
machines. Part 2 plans the split of the TUI from the core. Nothing keeps
compatibility with old arenas or old interfaces.

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
  `fyai_tool_job_update_done()`, `agent_state_active()`,
  `mcp_teardown_settled()`.
- A kind is a set of booleans. Examples: `agent`, `exec`, `btw` and
  `native_shell` on a job; `poll`, `sleep` and `on_event` on a waiter.
- A hand-written reset list stands in for a reset point. The block of the
  forked tool child (`fyai_tools.c`, about 50 assignments) does not reset
  `terminate_pending`, `interrupt_*`, `compacting`, `auth_retry_done` and
  `xfer_*`.
- A state enum exists, but side flags change its meaning. Examples:
  `stream_response`, `fyai_turn_run`, `fyai_model_step`, `mcp_call_request`
  and `auth_login_request`.

## High priority

### 1. Tool job lifecycle (done)

`struct fyai_tool_job`, `fyai_tool_job.c`, `fyai_tools_int.h`.

- Done: `enum fyai_tool_job_phase` (SPAWNED, RUNNING, ANSWERED, DONE) replaces
  `out_open` and `done`. It only moves forward. `fyai_tool_job_settle()` is
  the one place that completes a job: a session job completes at its answer,
  every other job also waits for its child. `enum fyai_tool_job_verdict`
  (UNDECIDED, OK, NOT_OK, CHANNEL_FAILED) replaces `result_ok` and `failed`.
  A failed control channel is final, as `failed` was; the old last writer
  wins rule for `result_ok` stays inside `fyai_tool_job_verdict_set()`.
- `is_agent` copied `agent` and is gone.
- Stay booleans: `reaped` is a process fact that is independent of the
  request (a session job is done before its child is reaped), so the phase
  joins with it and does not hold it. `have_result` says that an answer
  came; the exit status can decide the verdict without one. `terminating`,
  `timed_out` and `overdue` are latches that are set before the outcome is
  known. `timed_out` forces a failure only at collection, and a result that
  arrives before the stop is still genuine. `exec`, `btw` and `native_shell`
  qualify a job and do not exclude each other. Only the test for an agent
  is made anywhere, so a kind enum would carry unused values.
- Survey corrections: `failed` with `result_ok` was reachable (a child that
  exits before its channel fails). `done` never went back to false, and
  `done` has no writer but the settle function. `agent` with `native_shell`
  never meets, since only a shell call is native.
- Left: `inline_block` and the surface pointer record the display mode
  together. That is a display mode, not a lifecycle state.

### 2. Shell session lifecycle (partly done)

`struct fyai_shell_session`, `fyai_shell_session.c`.

- Done: `enum fyai_session_stop` (NONE, CLOSE, IDLE) replaces `closing` and
  `timed_out`. An idle stop is a close, so the pair could not hold
  `timed_out` alone. The reason stays after the program ends: the cause of
  the end reads it. The wire members `closing` and `timed_out` of a mirror
  stay and are derived. A test covers the round trip.
- Left as booleans: `exited` is awaited by `fyai_event_sleep_until()`, which
  takes a `bool *`, so the end is a latch with its own flag and a state enum
  cannot replace it. `wants_input` toggles while the program runs and
  neither orders nor excludes anything else. `keep_tile` and `close_on_exit`
  are independent requests: the check `keep_tile && close_on_exit` is a
  live combination (a bang tile the user closed), and a close can come
  before a mirror learns `keep`. `pinned` is the ownership of a scope pin.
  `recorded` is the guard that stores the exchange one time. `feeding` is a
  guard for the span of a call. `drew`, `pipes`, `user_owned` and
  `takes_pane` are not states.
- Survey corrections: `closing` with `exited` is the normal end of a closed
  session, not an illegal combination. A dismissed session no longer exists,
  so it needs no state. After the end `job` can stay set and `wants_input`
  can stay stale; `waiter` and `pinned` are cleared by `exited()`.
- Still open: the clearing sequence of `fyai_shell_session_exited()`, and
  the display mode (`inline_block` or `surface`).

### 3. Retry and authentication retry (partly done)

`struct stream_response` and `struct fyai_buffered_request`
(`fyai_stream.c`), and the former `ctx->auth_retry_done`.

- Done: `struct fyai_retry` holds the state that the stream and the buffered
  request share: `phase`, `timer`, `auth_refresh`, `attempt`, `band` and
  `refreshed`. `enum fyai_retry_phase` (IDLE, BACKOFF, AUTH) names what the
  request waits on between two transfers. The cancel and the destroy paths
  read the phase and not the pointers, and `stream_request_resubmit()` keeps
  the whole structure across its `memset()` and not four fields.
- Stay booleans: `band` is open from the first retry report to the settle of
  the request, across the phases. `refreshed` is a one-way latch of one
  refresh for each request. `cancel_requested` is a request from outside.
- Survey corrections: `retry_band` is a plain bool and not three-valued.
  The stream state already has RETRYING; it covers both BACKOFF and AUTH,
  which is why the phase is a separate field of the shared structure and
  not new values of `enum fyai_stream_state`. `auth_refresh` with
  `retry_timer` was never possible: each arm site runs after the transfer
  ended, and a phase holds only one of them.
- Left: `retry_hint`, `retry_cause`, `printed_content` and `printed_reasoning`
  are stream only. The buffered request has no counterpart, so they stay in
  `struct stream_response`. `retry_hint` without `retry_cause` is reachable
  when the text of the error is out of memory.

### 4. Turn run beside its enum (partly done)

`struct fyai_turn_run` (`fyai.c`) and the turn fields of `struct fyai_ctx`.

- Done: `enum fyai_gate_phase` (IDLE, ASKING, AFTER) replaces `gate_after`
  and says whether a round is open. `fyai_turn_run_gate_clear()` returns it to
  IDLE. `fyai_turn_run_diff_begin()` and `fyai_turn_run_diff_end()` are the
  only writers of `ctx->tool_diff_tracking`, and every end of the run reaches
  the second.
- Stay: `gate_pending` is a count of missing answers and not a phase; the
  turn waits when it is above 0 and resolves the round when it is 0.
  `turn_end_requested` is a request that a tool makes and the turn consumes,
  and `turn_stop` is the reason that a stopped turn records. They describe
  different events and do not disagree. `cancel_requested`,
  `accept_pending_user` and `group_parallel` are independent flags.
  `tool_gate_run` is a pointer to the run that holds the open round.
- Left: `gate_id` is not cleared with the round. It is checked only after
  `tool_gate_run` matches, and that is NULL outside a round.

### 5. Transport bootstrap and role (partly done)

`struct fyai_ctx` and `fyai_transport_boot.c`.

- Done: `struct fyai_transport_state` (`ctx->transport`) holds `client`, `ctl`,
  `pid`, `src`, `exec`, `seq`, `why`, `names`, `nnames`, `gen`, `stated` and
  `envfd`. `enum fyai_transport_role` (NONE, OWNER, SUPERVISOR, SECONDARY,
  CHILD) replaces `transport_owner` and `transport_primary`: the primary
  connection is OWNER or SUPERVISOR, and only OWNER ends the transport.
  `fyai_transport_detach()` returns the role to NONE.
- Survey correction: SECONDARY and CHILD had the same two flags. No code
  tests them apart, so the roles are named for the doc and for the child
  attach only.
- Stay: `stated` with `gen` is a latch with its value (generation 0 is a valid
  stated value), so it stays a pair. `envfd` keeps its sentinels: it is 0
  when the descriptor was taken or never set, and a descriptor above 2
  otherwise.
- Left: the `xfer_*` fields are the model transfer and not the transport.
  They belong to the model transfer structure of the context list below.

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

### 7. curl transfer

Done for `struct fyai_curl_transfer`: `enum fyai_curl_phase` and one bit for a
cancel that comes while a callback runs. `struct fyai_xfer` still has a
`finished` flag that repeats the phase.

### 8. Sink document mode

Done for `struct sink_term`: `enum sink_doc_mode` (closed, skip, passthrough,
oneshot, live, paused). `write_failed` and `wrote` stay separate facts.
`struct sink_capture.doc_open` is an open and closed bit, and stays.

### 9. Sink band kind

Done for `struct fyai_sink_band`: `enum sink_band_kind` and
`enum sink_band_paint`.

### 10. Tool band of the UI

Done: `enum ui_tool_mode` (idle, band, inline) and `ui_tool_text_free()`. A band
that does not open frees the text of the call, and a closing UI frees the
error text too.

### 11. Pane command capture

Done for the end of a span: `enum ui_pane_span` (none, ending, placed) replaces
`pane_ending` and `pane_shown`, and the phase is reset when the span ends.
`capture` stays a bool because the renderer of the model nests its own capture
inside the end. `pane_status` is set and cleared by its caller in `fyai.c`.

### 12. Browser action protocol

Done for the input: `enum browser_input` (none, filter, action). The details
overlay stays a layer under it, and `pending` is a command for the loop. The
Escape of `browser_key()` for an input is not reached while an input is open:
`fyai_browser_cancel_input()` takes it. `closing`, `picked` and `navigated`
are unchanged.

### 13. Remote peer, run and session

Done for the peer: `enum remote_peer_role` (pending, client, watcher, handed).
`failed`, `native` and `lines_only` stay. `struct fyai_remote_run` joins
`exited`, `err_closed`, `input_closed` and `reported` in `remote_run_settle()`
from four callbacks, and `struct fyai_remote_session` still tests its reap
condition over `exited`, `anonymous`, `snapshot`, `peer` and `watchers`. The
flags of the run are a join, not a sequence: the exit of the child, the end of
its error stream and the close of its output must all happen, and `reported`
latches the one report. They stay flags.

### 14. Tool job group

Done. `struct fyai_tool_job_group` keeps `enum fyai_tool_group_phase`.

### 15. Agent record state

Done. `struct agent_record` holds `enum agent_state`, and an unknown wire state
is rejected with a warning.

## Medium priority

- Interrupt and terminate (`fyai.h`): checked, no change. `terminate_pending` is
  a one-way request that ends the invocation (SIGTERM, or a cancel of an
  agent), so it is never cleared. `interrupt_pending` is acknowledged and
  cleared. Several sites still rebuild `interrupt_pending` from
  `seq != seen`.
- Scope lifetime (`scope_open`, `scope_pins`, `sandbox_scope`): the scope ends
  when `!scope_open && scope_pins == 0` and the scope is not `"session"`. A
  `pinned` bit repeats in `fyai_shell_session`, `fyai_monitor_run` and
  `fyai_agent_bg_run`. `unpin` clamps at zero, which hides an unbalanced pair.
  Change: a pin handle that owns its bit, and the scope kind as an enum.
- Response chain (`response_chain_linked`, `response_chain_miss`,
  `model_step->response_linked`): UNLINKED, LINKED, MISSED. A stale miss can
  pass to the next request. `token_extents_off` is a one-way latch that is
  never re-armed. Change: a `chain_state` enum.
- httpd connection (`fyai_httpd.c`): `upgraded`, `dispatched`, `dead`,
  `closing`, and `reap_queued` on the server. States: HEAD, DISPATCHED,
  UPGRADED, CLOSING, DEAD. `struct fyai_ws` holds the parser state in
  `opcode`, `fin`, `masked`, `message`.
- Terminal child (`struct tty_run`, `struct fyai_terminal_relay`): checked, no
  change for `tty_run`. The result states its precedence: `timed_out` wins,
  and `cancelled` is `cancelling && !timed_out`, whichever came first. A
  state enum would record the first cause and change that. `relay_finish()`
  can still set `done` without `reaped`.
- Terminal view screen mode (`screen_mode`, `alt_screen`, `alt_screen_seen`,
  `alt_screen_used`): the choice of surface is one predicate over three sticky
  flags. States: LINE, SCREEN, ALT_ACTIVE, ALT_EXITED.
- Tool child handshake (`pending`, `done`, `session_started`, `spawn_failed`):
  `done` is set before the work runs, so it means "taken".
- JSON-RPC connection (`closed`, `write_failed`, `protocol_failed`,
  `peer_closed`, `expect_close`): independent bits that lose the reason for
  the close. The request has `done`, `ok`, `cancel_requested`.
- Agent message acknowledgement (`pending`, `cancelled`, `ask_parent`,
  `request`, `branch`): the lookup loops test four combinations of these to
  mean queued, outstanding or settled.
- MCP teardown (`del_done`, `term_done`, `pid`, `session_id`): a structure that
  never began teardown and one that is tearing down look the same.
- UI input owner (`busy`, `marker_busy`, `questions`, `head`,
  `editor_request`, `external`, `editor_external`, `inline_focus`): the marker
  derives from three sources, and the key routing is a priority list.
- Agent registry UI mode (`surface`, `attached`, `question_presented`,
  `draft`): NONE, ZOOMED, ATTACHED, QUESTION. `fyai_agents_detach()` is also
  the error path of zoom.
- Remote client (`struct remote_client`): `waiting`, `answered`, `refusal`,
  `running`, `ended`. A frame that arrives while `waiting` and `running` are
  both set is resolved by dispatch order.
- Session and switch state: `switch_pending` guards three payload numbers.
  `session_unstored` is a branch name that must match `ctx->branch` and can go
  stale on a rename or a switch. `compacting` is set and cleared on each side of
  `fyai_run_turn()` with nothing between, so it is not left set.
- Frame scheduling (`frame_pending`, `reflow_pending`, `repaint_pending`): a
  dirty level NONE < FRAME < REPAINT < REFLOW kept as independent booleans.
  `next_frame_ms` is reset at five sites.
- Layout and popup: `popup_state` is stored as an int next to the `popup`
  pointer, and one site writes it and skips `ui_popup_set()`. Also `tile_full`,
  `view`, `blocks`.
- Browser foreign sessions (`foreign_visible`, `foreign_scheduled`,
  `foreign_phase`): three fields, one lifecycle, re-encoded at three sites.
- Live document pause and end: `fyai_uitranscript`, `fyai_uidraw` and
  `fyai_display_output` each rebuild the same paused or ended state with their
  own guards.
- Progressive markdown stream (`live`, `active`, `render_pending`,
  `full_render`, `flush_src`): `full_render` is set at three sites and appears
  never to be cleared.
- Claude SDK server (`initialized`, `mcp_starting`, `init_id`, `busy`,
  `failed`): `failed` is set at six sites and is not cleared in the code that
  the survey read.

## Low priority

- `mcp_call_request` and `auth_login_request` have enums. Add a CANCELLING
  state to remove `cancel_requested`, and a RETRYING_AFTER_AUTH state in place
  of `auth_retry_done`.
- Event source and term ladder (`removed`, `oneshot`, `armed`, `reaped`;
  `active`, `stage`): the code is core and well tested. The gain is
  documentation.
- `fyai_agent_bg_run.finished` repeats `group == NULL`. In
  `fyai_monitor_run`, `stopped` repeats `events >= FYAI_MONITOR_MAX_EVENTS`.
- Question phases ANSWERING and REVIEW are derived in `fyai_ui` and in the
  page.
- `fyai_flow` (`at_line_start` against `blank_rows`), `fyai_workpane_manager`
  (`layout_pending`, `reconciling`), `sink_frames` (`greeted`, `failed`) and
  the PTY driver (`reaped`, `eof`, `exit_clear`).

## The context structure

`struct fyai_ctx` holds these clusters, each with one lifecycle and no common
reset:

1. The transport client, about 14 fields (item 5).
2. The model transfer: `xfer_body`, `xfer_url`, `xfer_profile`, `xfer_status`,
   `xfer_retry_after`, `xfer_error`. Only one request can be in flight, so
   concurrent requests of `fyai_btw_run` or of agents can collide.
3. Turn control: `turn_end_requested`, `turn_stop`, `turn_cost_limit`,
   `compacting`, `interrupt_*`, `terminate_pending`, `tool_diff_tracking`,
   `tool_gate*`, `relay_gate`, `relay_steps`, `output_schema`,
   `structured_output`. Each is per turn and a person clears it.
4. Usage: `usage_*`, `last_call_*`, `switch_*`, `usage_agents*`. Two phases,
   the run and the last call, are mixed.
5. The record of the next publish: `branch_op`, `branch_op_from`,
   `branch_store`, `branch_prev`, `branch_desc`, `branch_agent`,
   `project_state`, `session_unstored`.

A reset function for each sub-structure, called from `fyai_ctx_fork_disown()`
and from the forked-child block, would remove the risk of a missed field.

## Suggested order

1. Items 1 and 2 together.
2. The retry cluster and `auth_retry_done` (item 3), with the curl transfer
   (item 7).
3. The small self-contained ones: the waiter kind, the agent record state, the
   `uiwork_band` state and the job-group pair.
4. The sub-structures of the context, with one reset function each.
5. The event core, the PTY driver and the markdown stream, last.

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
   a state write (becomes a store frame), an input read (`take_line`,
   `peek_line`, `readline`; becomes a request frame), a question that needs
   an answer (`fyai_ui_ask`; already a frame pair), or a drain
   (`diag_drain`, `drain_output`; becomes a log entry).
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
   sides: items 4 (turn run gate), 8 and 9 (sink document mode and band kind),
   10 (UI tool band), 11 (pane command capture) and 12 (browser action). While
   a flag is written by the engine and by the UI, a thread split makes a data
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
  structure, `fyai_render_cfg`, from whoever renders.
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
   the oracle. Do items 1, 2, 4, 8, 9, 10, 11 and 12 of this document, so that
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

1. Nested loop in a callback (M). `fyai_ask_user_page`
   (`src/fyai_tools.c:1668`) runs `fyai_event_loop_step()` in a loop while it
   serves the `user/ask` request of a child. Another step loop is at
   `src/fyai_tools.c:1613` (unverified: callback or not). Answer the request
   from the callback of `fyai_ask_open()` instead.
2. Done. No error-check condition holds an operation that does work.
3. Done. `tests/sink-only.sh` also matches `write` on descriptors 1 and 2.
4. Done. A failed write of machine output stops the command with a cause.
5. Done. `acp_session_add()` and the request source of a view session report.

### Second owners

6. Done. The transcript copy, the WebSocket codec, the PTY driver and the
   transport assembly use `fyai_buf`.
7. Done. `fyai_cas_temp_open()` and `fyai_cas_temp_seal()` serve the blob store
   and the manifest writer.
8. A private loop (S). `fyai cast` play runs its own `poll` and `fytim_pump`
   (`fyai_cast_player.c:790`). It is a stand-alone command, so the risk is low.
9. The parallel-construction oracles (L). They still run in a production
    repaint: `fyai_chrome.c:410`, `fyai_session.c:1957` and `:2002`,
    `fyai_ask.c:434`, `fyai_uidraw.c:44`, `:100` and `:487`, `fyai_output.c`,
    and four calls in `fyai_ui.c`. This is step 0 of the TUI split. Remove
    each one when its part has converged.

### Fork and cleanup gaps

10. Done. `fyai_ctx_fork_disown()` also clears the tile manager, the active
    command, the catalogue update, the ACP connection, the control source and
    the side questions. `home` stays: the tools of the child use the home of
    the parent. `patch_views` and `waiter_tick` were cleared already.
11. Not a defect. `session_quit` runs on the loop that its ctx owns, and
    `fyai_ui_quit_request()` accepts a ctx with no UI.

### Dead code and compatibility

13. Done. `fyai_curl_perform` is removed. Its callers were `revoke_token`
    and `fyai_auth_models` through `auth_http_request` (`fyai_auth.c`), not
    the login flow, which was on the loop already. `auth logout` and `list
    models` are now async commands that run a request on the loop
    (`fyai_auth_logout_submit()`, `fyai_auth_models_submit()`). Open:
    `fyai_auth_models_submit()` still calls `fyai_auth_resolve()`, which can
    refresh the token with `fyai_auth_refresh()` and its stepped loop.

### Large files and functions

15. Done. `src/fyai_tools.c` was 9585 lines. It is now 2619 lines, the
    dispatch of tool calls, the tool implementations and the glue. The seams
    moved to `src/fyai_tool_job.c` (2340 lines, the job lifecycle and the
    job groups), `src/fyai_shell_session.c` (2655, the sessions, their tiles,
    zoom, focus and bang commands), `src/fyai_agent_view.c` (848, the screen
    of a sub-agent and the inline terminal), `src/fyai_tool_child.c` (821, the
    child serve, spawn and exec path) and `src/fyai_shell_sandbox.c` (436).
    `src/fyai_tools_int.h` (355 lines) holds the structures and the
    prototypes that cross the files. Open: `fyai_shell_session.c` still holds
    the focus and zoom code (about 1000 lines) that could be a file of its
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

18. Done for `CLAUDE.md`, `src/fyai_settings.h`, `doc/exchange-scope.md`,
    `doc/declarative-state.md` and `doc/remote-sessions-sdd.md`. Other plans in
    `doc/` name functions that the survey did not find
    (`markdown-ui-plan.md`, `transport-isolation-review.md`,
    `state-sync-sdd.md`, `command-schema-plan.md`,
    `macos-transport-isolation-sdd.md`, `web-fetch-plan.md`). They may be
    plans, not errors.
19. Timing in tests (S). The sleep of `fyai_event_test.c` is removed. The
    watchdog sleeps in that file test a timer and stay. These sites poll with
    a sleep and need a wake source: `tests/fyai_shell_test.c` (a process that
    exits, a process that reads), `shell_tty.sh`, `transport_bootstrap.sh`
    and `retry_interrupt.sh`.

The agent found no hand-built JSON outside `fyai_mock.c`, no second transcript
renderer, and no event source registered with a discarded handle. Whether each
of the 475 cases has a positive control is unverified.

### Suggested order

First the private loop of `fyai cast` (item 8), then item 13. The remaining timing
sites of item 19 need a wake source and not a poll interval. Item 9 and
items 1, 10 and 15 go with the state-machine and TUI work, because they touch
the same code.

Done: the mock provider now serves on `fyai_httpd`, which takes request
bodies, keeps a connection and lets a handler write its own answer.
