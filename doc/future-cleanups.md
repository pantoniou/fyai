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

### 1. Tool job lifecycle

`struct fyai_tool_job`, `fyai_tools.c`.

- Fields: `out_open`, `have_result`, `reaped`, `failed`, `result_ok`, `done`,
  `terminating`, `timed_out`, `overdue`, `wants_input`; the kind booleans
  `agent`, `exec`, `btw`, `native_shell`; the pointers `csrc`, `deadline`,
  `hang_deadline`, `waiter`, `session`.
- States: SPAWNED, RUNNING, RESULT_IN (the child is alive), CANCELLING, REAPED,
  and DONE with the outcome ok, failed or timed out.
- Transitions: `fyai_tool_job_attach`, `_run_done`, `_child`, `_cancel`,
  `_deadline`, `_hang_deadline`. `fyai_tool_job_update_done()` is the only
  writer of `done`.
- Illegal combinations: `failed` with `result_ok`; `terminating` with
  `reaped`; `timed_out` after `done`; `agent` with `native_shell`.
- Defects: `done` means `reaped && !out_open` for a job with no session and
  `!out_open` for a job with one, and at least seven sites test `session` for
  this. `result_ok` has four or more writers, and the last one wins.
- Change: a state enum and an outcome enum (ok, failed, timed out, cancelled),
  and a job-kind enum.

### 2. Shell session lifecycle

`struct fyai_shell_session`, `fyai_tools.c`.

- Fields: `exited`, `closing`, `timed_out`, `pinned`, `recorded`, `keep_tile`,
  `close_on_exit`, `takes_pane`, `user_owned`, `pipes`, `wants_input`,
  `feeding`, `drew`, `inline_block`; the pointers `job`, `remote`, `waiter`,
  `idle`, `surface`.
- States: STARTING, RUNNING, WAITING_INPUT, CLOSING, EXITED with the tile kept,
  EXITED with the tile dismissed. Each has a local and a remote form.
- Illegal combinations: `closing` with `exited`; `keep_tile` with
  `close_on_exit`; `exited` with `job`, `waiter`, `pinned` or `wants_input`
  set; `remote` with `job`.
- Defects: `fyai_shell_session_exited()` has one guard, `sess->exited`, against
  a second call. The clearing of the fields is a sequence by hand, so a missed
  field leaves a scope pin or a waiter. The restore path rebuilds `closing` and
  `timed_out` from a mapping, so the booleans are also a storage format.
- Change: change items 1 and 2 together. They share the `wants_input`, `done`
  and `exited` handshake.

### 3. Retry and authentication retry

`struct stream_response` and `struct fyai_buffered_request`
(`fyai_stream.c`), and `ctx->auth_retry_done` (`fyai.h`).

- Fields: `attempt`, `retry_hint`, `retry_cause`, `retry_band`, `retry_timer`,
  `auth_refresh`, `printed_content`, `printed_reasoning`, `cancel_requested`.
- `stream_retry_arm()` retries when `!cancel_requested`, the attempt count is
  below the maximum, the failure is transient or has a hint, and nothing was
  printed. The decision is three booleans, a counter and a timer pointer.
- `retry_band` is a three-valued "band is open" that `stream_request_resubmit()`
  carries by hand across a `memset()`.
- The latch `auth_retry_done` was on the context. A model step reset it, so
  a second model request refreshed. A buffered request did not reset it, so
  one after a refresh could not. It is now `auth_refreshed` on each request.
- Illegal combinations: `retry_hint` without `retry_cause`; `retry_timer` while
  the state is not a retry; `auth_refresh` with `retry_timer`.
- Change: split the retry state into RETRY_WAIT and AUTH_REFRESH, and share
  one retry structure between the stream and the buffered request. The latch
  is done.

### 4. Turn run beside its enum

`struct fyai_turn_run` (`fyai.c`) and the turn fields of `struct fyai_ctx`.

- `enum fyai_turn_run_state` exists. Beside it: `cancel_requested`,
  `accept_pending_user`, `group_parallel`, `exclusive_index`, `gate_id`,
  `gate_items`, `gate_count`, `gate_pending`, `gate_next`, `gate_after`,
  `diff_before`; and on the context `turn_end_requested`, `tool_diff_tracking`,
  `tool_gate_run`.
- The gate has its own machine inside the GATE state: `gate_pending > 0` is
  waiting for answers, and `gate_after` is a post-round that resumes at
  `gate_next`.
- `tool_diff_tracking` is set at one site and cleared at five.
- Illegal combinations: `gate_pending > 0` outside GATE; `gate_after` without
  `gate_id`; `tool_diff_tracking` in DONE; `turn_end_requested` and `turn_stop`
  that disagree.
- Change: a nested gate enum (IDLE, ASKING, AFTER) and one owner of
  `tool_diff_tracking`.

### 5. Transport bootstrap and role

`struct fyai_ctx`: `tclient`, `transport_ctl`, `transport_pid`,
`transport_owner`, `transport_primary`, `transport_src`, `transport_exec`,
`transport_seq`, `transport_names`, `transport_nnames`, `transport_gen`,
`transport_stated`, `transport_envfd`, `xfer_*`.

- Roles, set in `fyai_transport_boot.c`: OWNER, SUPERVISOR-PRIMARY, SECONDARY
  and CHILD. Nothing names them.
- "Configuration was stated" is `transport_stated && transport_gen ==
  cfg->config_generation`.
- `transport_envfd` is a descriptor with sentinels: `<= 2` is none and `0` is
  consumed.
- Illegal combinations: `owner` without `primary` (impossible by construction,
  not enforced); `primary` without `tclient`; `transport_ctl >= 0` with no
  `transport_src`; `transport_nnames` that differs from the granted names.
- Change: a role enum and a transport sub-structure.

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
