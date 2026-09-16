# CLAUDE.md

Use this file when you change this repository.

## Project

`fyai` is a stateless AI coding assistant written in C. It does not use a
daemon. One invocation runs one complete tool-use loop. It commits the
canonical state and then exits. An interactive invocation owns its terminal UI
only while its process runs.

### Architecture rules

- Do not add a daemon, resident process, or hidden process state.
- Store persistent state only in content-addressed libfyaml arenas under
  `~/.fyai`.
- Keep canonical data immutable, deterministic, and address-stable between
  processes.
- Do not relocate an arena during normal operation.
- Keep tools compatible with Unix conventions. Tools read files, apply writes
  or patches, and run approved shell commands.

### Comments

Write comments in concise Technical English. State the required invariant,
ownership, or behavior. Do not narrate implementation history, restate the
code, address the reader, or use colloquial explanations. Remove a comment
when the code is self-explanatory.

## Data model

Use libfyaml generics as the native data model. Build values with functions
such as `fy_gb_mapping()` and `fy_gb_sequence()`. Do not construct JSON by
hand. Emit compact JSON only at provider boundaries with `FYOPEF_MODE_JSON`.
Parse provider responses directly into generics.

Keep provider wire data in provider-stream generics. This data includes request
IDs, tool-call IDs, finish reasons, and timestamps. Derive provider-independent
content before you calculate the canonical identity.

Use typed accessor defaults:

```c
fy_get(obj, "content", "")
fy_get(obj, "total_tokens", 0LL)
fy_get(obj, "items", fy_invalid)
```

### Generic string lifetime

Prefer `fy_castp()` to `fy_cast()`. A short string can be in the `fy_generic`
word. Thus, a pointer from `fy_cast(v, "")` can point into the local copy of
`v`. The pointer becomes invalid when the copy leaves scope. Use
`fy_castp(&v, "")` at the use site. Make sure that the generic has a sufficient
lifetime. Do not keep a cast pointer longer than its generic.

A `const char *` loop variable from `fy_foreach()`,
`fy_foreach_key_value()`, or `fy_foreach_idx_item()` is safe. The typed
accessor uses the address of the stored item. Thus, the pointer refers to
collection storage and not to a copy.

### Short forms

Use the short generic API. It says the same thing with less text:

- `fy_is_string()`, `fy_is_mapping()`, `fy_is_valid()` and the other
  `fy_is_*()` predicates, and `fy_empty()` for an empty collection or string.
  Keep `fy_generic_is_int()`, `fy_generic_is_float()` and
  `fy_generic_is_bool()`, which also test the C value range.
- `fy_foreach()`, `fy_foreach_key_value()` and `fy_foreach_idx_item()` in
  place of an index loop with `fy_get_at()` and `fy_get_key_at()`.
- `fy_any_equal(v, "a", "b")` in place of a chain of `fy_equal()` tests. It
  evaluates the value one time.
- `fy_str(v)` for a printable string and `fy_number(v, dflt)` for a number.
  `fy_str()` returns NULL for `fy_invalid`.
- `fy_stringf(gb, ...)` and `fy_join(gb, ...)` to build a string generic in a
  builder arena. To get a stable `const char *`, keep
  `fy_gb_intern_string(gb, fy_sprintfa(...))`; there is no interning format
  function.
- `fy_str_empty(s)` for a NULL or empty C string.
- `fy_mapping(gb, ...)` and `fy_sequence(gb, ...)` in place of
  `fy_gb_mapping()` and `fy_gb_sequence()`. A first argument of type
  `struct fy_generic_builder *` selects the builder path, thus the call is the
  same. Most other `fy_gb_*` functions have such a short form. The exceptions
  are `fy_gb_internalize()` and `fy_gb_intern_string()`, which have none.
  Without a builder, these forms use stack storage: do not return such a value
  from a function.

### Returned generics

A generic that a function returns must name storage that is alive after the
function. Build it in a builder that the caller supplies or that outlives the
call, or return a value that an arena holds.

- Do not return a generic that a stack builder made. `fy_mapping()` and
  `fy_sequence()` with no builder, and `fy_sprintfa()`, use the stack frame of
  the caller. They are correct as an argument to a call in the same frame.
  Do not free their result.
- Copy a C string into the builder with `fy_value(gb, s)` when the string is a
  local buffer.
- State the lifetime in the header when a function returns a borrowed value.

### Empty strings

An empty-string generic is a string. It is not null. If the style is not set,
the YAML emitter must write an empty string as `""`. The core and YAML 1.1
schemas can parse a bare empty scalar, such as `key:`, as null.

Keep empty-string configuration keys as `type: string` in
`data/config.schema.yaml`. Do not allow null to hide an emitter defect.

### Generic initialization

A zeroed `fy_generic` is not `fy_invalid`. Zero can represent an empty
sequence. `fyai_setup()` clears `struct fyai_ctx`. After this operation,
initialize each generic field explicitly.

## Source layout

- `src/main.c`: global option parsing and command dispatch.
- `src/commands.c`: verb definitions, usage output, and the main runner.
- `src/fyai.c`: engine orchestration.
- `src/fyai_sink.c`: the one rendering component and its backends.
- `src/fyai_flow.c`: the output separation manager.
- `src/fyai_output.c`: transcript document source and fragments.
- `src/fyai_session.c`: interactive input and slash commands.
- `src/fyai_agent.c`: sub-agent execution and agent RPC.
- `src/fyai_event*.c`: the portable event loop and signal handling.
- `src/fyai_event_dump.c`: SIGUSR2 event-loop diagnostics.
- `src/fyai_render.c`: generic-to-Markdown table rendering.
- `src/fyai_diag.c`: collected diagnostics.
- `src/utils.c`: HTTP buffers, shell capture, and generic serialization.
- `src/fyai_workpane.c`: the one owner of work-pane geometry, focus, and zoom.
- `src/fyai_sandbox.c`: Linux Landlock confinement.
- `src/fyai_oauth.c`: provider-independent OAuth browser flow.
- `src/fyai_jsonrpc.c`: JSON-RPC over standard I/O and HTTP.
- `src/fyai_config.c`: layered configuration and config commands.
- `src/fyai_branch.c`: branch storage, references, and root selection.
- `src/fyai_merge.c`: branch merge and rebase.
- `src/fyai_catalog.c`: provider and model catalogue.
- `src/*.h`: shared structures and internal interfaces.
- `data/`: embedded configuration schemas and catalogue data.
- `tests/`: unit tests, functional cases, mock providers, and scenarios.

## Persistent state and branches

The durable root has this versioned shape:

```text
{fyai: 2, catalog, HEAD, branches}
```

Each branch entry owns its conversation head and configuration. The root owns
the catalogue. Do not use a sidecar configuration file. Do not use a
root-level configuration value.

Three operations adopt a published root: publish, reconciliation after a lost
CAS, and `fyai_branches_refresh()`. Each operation must set
`ctx->branch_prev` from the adopted root. Do not keep an entry from an older
root. Such an entry links the next publish to the wrong predecessor. It also
makes the conflict check compare entries from different roots.

If a publish stops, report what it could not write and why. Name the branch.
State whether the operation lost the race or could not build the state. Do not
report only that the publish failed. That report cannot distinguish a lost
race from an arena that has insufficient space.

When you add a root key, update all applicable paths. These paths build,
decode, validate, truncate the reflog, and merge CAS conflicts. A concurrent
publish must preserve changes to other branches. During garbage collection,
limit the root `prev` chain and each branch-entry `prev` chain.

### Branches and references

- A branch name defines the hierarchy. Do not add a separate branch tree.
- Accept only symbolic references: `<branch>`, `<branch>~N`, `<branch>^^`, and
  `<branch>@{N}`.
- Never add numeric object references. Garbage collection can relocate arena
  objects.
- `~N` counts stored turns. One user and assistant exchange normally counts as
  two turns.
- `--branch`, `-b`, and `$FYAI_BRANCH` select a branch for one invocation.
- Only `checkout` changes stored `HEAD`.
- An interactive run that names no branch starts a `session/` branch. It takes
  the configuration of the branch last updated in its directory, else of the
  branch `HEAD` names. `ctx->session_unstored` names it until its first
  exchange is published; before that a publish on it writes nothing.
- Keep `ctx->branch` separate from `ctx->head_branch`.
- Store the operation that produced each reflog entry. Do not infer it from
  head changes.
- Set the operation with `fyai_branch_op_set()`. A publish consumes and clears
  it.

A branch start point includes the conversation and the configuration at that
point. Use `fyai_resolve_ref_state()` for `branch create` and `checkout -b`.
Do not combine a conversation from one state with a configuration from a
different state.

`--root` selects one published root and makes the invocation read-only. Reject
writes. Do not convert them to transient writes. Treat the supplied handle as
untrusted input. Walk the root reflog and compare raw values to find the
handle. Do a low-cost shape check for each entry. Then do one full validation
of the matching entry. Keep full root validation shallow. Code that walks a
reflog must validate each next link with `fyai_branch_entry_contained()`.

### Merge and rebase

A stored `previous` link is data. Walk a conversation chain with
`fyai_turn_foreach()`, which is bounded and detects a cycle; a corrupt chain
must end a walk, not hang it. Validate a root before the peek path follows any
reference in it.

Conversations are append-only. A join selects an order. It does not reconcile
text edits. Work in complete exchanges. Keep each question with its answer.
`rebase` puts our exchanges after their exchanges. `merge` orders exchanges by
their reflog time.

Read an exchange time from the reflog entry for the head that ends the
exchange. Keep timestamps at microsecond precision. Keep one system prompt.
Use stored generic identity to find the merge base.

After a lost publish CAS, retry if only other branches changed. Apply
`branch/on_conflict` if the current branch changed.

## Configuration and providers

Build one merged configuration document in this order:

1. the active branch configuration;
2. the explicit `--config` file; and
3. command-line `--set` changes.

Use the user configuration file only to initialize an arena that has no stored
configuration. Do not overlay it on an existing arena configuration.

Treat `cfg->config_doc` as the source of configuration intent. Populate the
structure fields in one `apply_config` pass. During model resolution, derive
the endpoint, provider, and catalogue `max_tokens`. Do not persist these
derived values. When the model changes, derive and persist the API grammar and
URL for the new provider.

Keep the informational `catalog` block synchronized on every configuration
commit. Remove it when the selected model is not in the catalogue.

Validate at every ingestion point. `--set` and an explicit `--config` file are
both checked against the schema before the value reaches the merged document.
Put the schema problems in the diagnostic that reports the failure; do not
write them separately.

Configuration paths use slash-separated keys. Parse values as YAML flow
documents. `--transient` places an in-memory builder above the durable arena
and skips reference publication.

Never store a raw API key. The `api_key` setting must have this form:

```yaml
api_key: {type: env, value: ENVIRONMENT_VARIABLE}
```

Reject raw keys at every arena ingestion point.

### Transient failures

Retry a provider request after a transient failure and a backoff.
`fyai_http_transient()` makes the retry decision. Retry a refused or lost
connection, a timeout, and HTTP 408, 429, 500, 502, 503, or 504. Do not retry
another HTTP status. It is the provider response.

An HTTP status does not identify every transient failure. On a streamed
endpoint, the connection can succeed with status 200. A rate-limit error can
then arrive in the stream. For this case, `fyai_provider_error_transient()`
reads the `code` and `type` of the error object. Do not classify a spent quota
as transient. The quota does not refill during a backoff.

Keep the text of a transient stream failure. Raise it only after the last
attempt fails. Do not raise it if the turn recovers. If the turn does not
recover, report the original text.

If an event handler stops a stream, curl reports a write error. Process the
parsed event before the transport error. The transport error identifies only
how the stream stopped.

Double the delay after each attempt. Do not exceed `retry/max_delay_ms`. Add a
random part so that requests do not retry at the same time. A `Retry-After`
header replaces the calculated delay. Apply the same maximum delay. Do not use
`random()` for the jitter. It has no seed, so each process gets the same
sequence.

A delegated sub-agent sends its wait through `fyai_tool_progress_emit()`. The
wait then reaches the parent work band for that delegation. Do not send a
status line from the child. It would appear directly on the parent terminal
and outside the display.

Present a retry as a work band through the sink. Do not use a separate status
line. A wait is a step of the active turn. Repaint the band for each attempt.
Close it one time at the common notification point. A backend without bands
uses the plain status line.

Retry a streamed request only if it presented no content. After a mid-stream
transport error, keep the text that the user already saw. Do not retry because
a retry would show the text again.

A request in a backoff has no transfer to cancel. Cancellation and destruction
must remove the timer and complete the request. Otherwise, an interrupt cannot
act on the request.

### Provider grammars

The supported grammars are Responses, Chat Completions, and Anthropic
Messages. The `api` configuration key selects the grammar. Do not add separate
grammar flags.

Normalize inbound provider items at the parse boundary. Store one canonical
shape. Adapt that shape in each request builder so that a conversation can
continue on another provider.

Read endpoint capabilities from the catalogue. Do not infer them from a
provider name. `fyai_config_resolve_model()` derives
`cfg->shell_tool_supported` for the active endpoint. An endpoint with no
capability declaration does not support native shell calls.

Use `fyai_provider_native_shell()` as the single decision for native shell
support. If native shell is unavailable, declare the function shell tool and
rewrite stored native shell items to function-call items in the request
builder.

## Output and terminal UI

`src/fyai_sink.c` is the only rendering component. Send all user-visible bytes
through it. A producer builds Markdown source and gives it to the sink. The
sink controls the presentation. Do not write to standard output or standard
error from another component. `tests/sink-only.sh` fails the build if it finds
such a write. `tests/sink-only-allow.txt` lists the files that cannot use the
sink and gives the reason for each file.

### Separation

`src/fyai_flow.c` decides what goes between two adjacent units of output: the
blank rows and the configured separators. It is the one policy. A live run, a
replay, and the measuring pass that sizes the recap window thus agree.

- A unit is a user card, prose, reasoning, a tool head, body, screen or result,
  or a notice. Separation applies to the transition between two units. It is
  not a property of one unit.
- A producer states the unit it is about to present with `fyai_sink_unit()`,
  which draws the separation and records the unit. `fyai_flow_before()` changes
  no state, thus a measuring pass asks the same question.
- The flow is on the sink, which every path that commits presented bytes
  reaches. Do not put a separation flag on the context. A flag on the UI is
  bypassed by the notice band and by an independent tile.
- The manager keeps the state of the render: the blank rows it starts and ends
  with. It supplies the rows the render does not, and reduces a run of them to
  the policy. A producer states its unit and does not count rows.
- Every path that commits presented bytes must record their tail with
  `fyai_flow_observe()`. A stale tail makes the manager remove a row that is
  not there. A newline that closes an open row is not a blank row.
- A title row opens a call and is fenced by `display/tool_group_fence`, from
  another call included. The body, the screen and the result continue that
  call.
- A turn break is one row. `display/turn_separator` is a rule of the transcript
  view, which draws it, and it is empty by default: no rule stands between
  exchanges unless the user sets one. Do not give it to the manager: a live
  session must not draw a rule under every prompt. `display/user_card_fence` goes under the
  card, and `display/section_separator` where reasoning ends.
- Fence a live band when it opens, not when it commits. A band fenced at
  commit has no blank row above it while it runs.
- Spooled bytes continue the unit being presented. They are not a unit and take
  no separation. A fence there draws blank rows into a live band.
- Blank lines in an assistant document are Markdown syntax, not presentation.
  The manager owns the separation between units, not the block structure in
  prose.
- A document declares its prose as a unit on each path: the whole document
  when it presents at one time, and each gap between fragments on replay. A
  gap inside a tool exchange is part of that exchange and takes no
  separation.
- One walk reads the fragments of a stored document. A render pass and a
  measuring pass drive it through `struct fyai_fragment_ops`. Do not add a
  second walk. A measuring pass that differs from the render pass sizes the
  recap window for rows it does not draw.

### Streams

A stream says what the content is, not where it goes:

- `FYAI_SINK_TRANSCRIPT`: conversation content.
- `FYAI_SINK_NOTICE`: verb and slash command results, and tables.
- `FYAI_SINK_STATUS`: banner, spinner, usage, tool echo. Commentary.
- `FYAI_SINK_DIAG`: drained diagnostics.
- `FYAI_SINK_MACHINE`: bytes another program parses. Never rendered or
  decorated.

Use `fyai_result()` for a verb result, `fyai_report()` for a status line,
`fyai_sink_markdown()` to render, and `fyai_sink_write()` for bytes that are
already in their final form. `fyai_result()` and `fyai_report()` write plain
text: a model name or a path inside a status line must not be read as markup.

### Backends

A backend supplies the presentation policy for `struct fyai_sink_ops`. The
terminal backend repaints in place. It owns the only progressive Markdown
renderer in the process. The capture backend keeps the requested content. A
document backend, such as HTML, uses the capture backend. Tests also use it to
read the output of a run. Each entry point can be NULL. If a backend cannot
present content, the sink discards the content. A producer does not examine the
destination.

### Documents

`src/fyai_output.c` owns the durable part of a transcript document. This part
contains the Markdown source and its fragments. Keep one tagged document open
for each system, user, or assistant output. Keep an assistant document open
during the complete model and tool loop. Store the final document as
`display_outputs`. Replay these documents for history. Reconstruct a legacy
arena from message and provider data only as a fallback.

- Add generated text with `fyai_output_printf()`.
- Add provider bytes with `fyai_output_append()`.
- Use `fyai_output_append_recorded()` to add source that another path already
  drew. Use it for a tool exchange. The tool path presents the exchange. Do not
  present it again from the document.
- A terminal session records its own exchange when its program goes. The
  calls that open and drive it store nothing: the session has one screen and
  what those calls did is on it, but a terminal is not the transcript. Its
  screen is stored as a `tool_text` fragment, which replays from what was
  recorded; a `tool_result` fragment replays from the message the model was
  given, and no one message holds a screen. A window is replayed from storage
  only when every stored result that has a display has a fragment, so a rule
  that stores nothing for a call must also say that nothing is expected.
- A tool exchange stores a `tool_head` fragment over its title row, with the
  outcome of the call. Replay draws the state mark and the failure cause from
  that fragment through `markdown_render_tool_head()`, the one renderer the
  live work band also uses. Do not draw a tool title row anywhere else.
- Set the presentation mode when the document opens. The mode is live,
  one-shot, or passthrough. Do not infer the mode later from the current state.
  If you do, a paused document can present all content again when it closes.
- A delegated sub-agent and a forked tool child do not present content. They
  use file descriptor 1 for JSON-RPC frames. `sink_may_present()` enforces this
  rule. The document, direct-write, and Markdown paths use this function. The
  sink drops a stream for descriptor 1 in these processes. Do not move the
  stream to standard error. That change can corrupt the parent display.
- Do not create a second transcript renderer.
- Do not call `fytim_pump()` from a render path. Set `frame_pending` and let
  the UI owner paint.

### A window that changes width

Rows are hard-wrapped when they are made, so a window that changes width
leaves every row already drawn made for the old one. Nothing rewraps them: the
display makes them again.

- The live region is made again from the source of the open document.
  `fyai_sink_reflow()` renders it from the start at the new width and replaces
  what is on the screen. The terminal backend keeps that source for this.
- The rows a turn already committed belong to the scrollback of the terminal.
  They cannot be made again in place, so a settled width change clears the
  screen and paints the newest exchanges from the stored transcript, through
  `fyai_display_repaint()`. It waits for a turn in flight to finish: only what
  is stored can be made again. Ctrl-L asks for the same repaint.
- The repaint takes whole exchanges until the screen is full, so it must
  measure what it draws. A tool fragment is drawn bounded by its preview
  limit: the thousand lines a call printed are the eight rows of them the
  reader is given. Measure an exchange by its source and one turn takes the
  whole screen, and the exchanges that would have fit under it are not
  painted.
- Everything a repaint draws goes through the sink, the card for what the user
  said included. The display commits that card itself only for the echo of a
  line just typed. Commit it during a repaint and it reaches the screen while
  the answer under it is still in the sink, which puts every card of the
  window above every answer.
- Do not act on a width change inside the event callback. A drag sends a burst
  of them, and one repaint at the end of the service is what a reader needs.
  The first size is not a change: it is the display learning the window it
  opened in.
- `markdown_effective_width()` is the one answer to how wide a render is now. A
  live region keeps the width it made its rows at and compares the two. The
  width the display opened in is recorded when it opens, so every later size is
  a change.
- fyai indents rendered content itself - a tool body under its indent, a shell
  command under its marker - so the content is rendered that much narrower
  through `fyai_width_reserve_begin()`. Rows made at the whole width and then
  indented run past the right edge, and the terminal wraps them: that is what
  turns a live band into interleaved fragments. A width nobody knows stays
  unknown; taking columns off it would make the content one column wide.
- A session opens on the screen its conversation left: the replay takes whole
  exchanges from the newest back until the screen is full, the one that
  overruns it included. The rows of that screen come from the display, not from
  the terminal, because standard output is a pipe of its own while it is open.
- A delegated sub-agent does not repaint a transcript. Its screen is the result
  of the call, and the conversation behind it is not something anyone asked to
  see there.
- The size is read in the pump, and a turn waits on the provider rather than on
  the terminal, so an interactive session keeps a SIGWINCH source to wake the
  display for it.

### The page

`display/renderer` selects how the live screen is composed. `stack` is the
band stack of libfytimui: the library draws the header, the prompt between two
rules and the status. `page` states the same screen as one UI Markdown page.
`src/fyai_page.c` owns the page source; the terminal library draws its slots.

- The page is rendered again for each frame, from the state of the session.
  libfytimui decision 0007 measured it: the Markdown is at most 0.16 ms of a
  frame. Do not cache a rendered page.
- `fyai_page_source()` is a function of `struct fyai_page_state` and nothing
  else, so the tests read it without a display. Put a new element of the
  screen in the state and in the source, not in a draw call.
- A slot holds what the library draws: `pane`, `prompt` and `completion`.
  The height of a slot comes from the library through `fytim_tail_rows()`,
  `fytim_workpane_rows()` and `fytim_prompt_rows()`: an inline page is as tall
  as its rows.
- The tail is drawn on the canvas, not by the library. The page reads its rows
  back with `fytim_tail_content()` and draws the last rows that fit its region,
  so the ground of a fullscreen page is under the tail too. A tail that the
  library draws over the canvas has no ground, and the terminal background
  shows through it.
- The page must look as the band stack does. `tests/cases/ui_page_renderer.sh`
  runs one scenario under both renderers and compares the screens: a change
  to the stack chrome is a change to the page source too.
- The chrome is the stack's: a blank row and the header row, the prompt on its
  card (`fytim_prompt_card()`, a slot two rows taller) or between two rules,
  and two status rows - the focus hint or the completion ribbon, then the
  status. The band stack reserves the blank row with `fytim_set_header_rows()`
  and the page document states it, so both draw it. The header and the status
  carry the heading and blockquote SGR pairs of the theme, so the page
  renderer takes SGR input (`FYMD_SGR_SAFE`).
- fyai escapes each value of the header template and gives it the next colour
  of the palette series (`mermaid.series.N`), and renders the header to one
  row. The band stack draws that row, and the canvas of the page draws it in
  the `header` slot, one row tall. Both cut it at the edge: a row of Markdown
  wraps a long header, such as a long working directory, onto a second row
  that the fit does not count. `tests/cases/ui_page_long_header.sh` runs with
  such a directory.
- `fyai_page_fit()` gives the chrome its rows before the pane, and the pane
  before the tail. A pane that asks for the whole terminal otherwise pushes
  the prompt off the screen.
- The chrome goes in a stated order when the terminal is short: the cap row
  (`fy-drop order="0"`), the status, the header, then the rules. The prompt
  has no drop.
- A margin at the start of a row is `&#32;`: Markdown removes a plain blank
  there, and a non-breaking space reaches the terminal as a different
  character.
- The page is one `fy-tight`: a blank row between two parts of the chrome is
  not part of it.
- Text that the configuration or a program wrote goes into the source through
  `page_append_text()`: it is escaped, loses its SGR and its line breaks, and
  loses its leading blanks at the start of a row, where four of them make an
  indented code block.
- The renderer keeps a right margin of `FYAI_PAGE_RIGHT_MARGIN` columns. The
  page renderer is made that much wider, so rows and slots take the terminal
  width.
- The page places the tiles of the work pane itself. Each tile is bound to a
  slot `tile:N` when it is registered, and `fyai_workpane_page_grid()` writes
  the pane as an `fy-grid`: the cells come from `fyai_workpane_place()`, or
  the zoomed tile alone, and the rows from `fyai_page_grid()`, which solves
  them as the terminal library solves a pane. Slot heights are stated in the
  source, so the rows cannot be left to the grid. The page states the cap row
  above the grid. The band stack ignores the bindings and places the tiles
  with its own solver.
- The grid is written twice a frame: at the rows its tiles ask for, which
  `fyai_page_fit()` reads, and at the rows the fit leaves it.
- fyai draws the page itself. `fyai_page_publish()` parses the rendered rows
  into cells with `fytim_cells_draw_text()`, the parser of the terminal
  library, and publishes them to one surface in the `canvas` slot. The
  library is given blank rows and the canvas as the first region, so the
  tile, prompt and completion slots stand on it. Close the canvas with the
  page: it is a band of the library, and the band stack would draw it.
- The page source is a YAML document, `data/page.yaml`, that
  `fyai_page_source()` transcribes with the state of the frame. The document
  has no expressions: `if` names a flag, and `page_state_generic()` decides
  every flag from the state. A node whose flag is not set is left out of the
  Markdown. Text of the state is escaped; `markup` and `sgr` name values that
  fyai wrote itself. `tests/data/page-source-golden.txt` holds the source of a
  matrix of states. A change to the document or to the state that changes the
  source records the file again with `FYAI_PAGE_GOLDEN_WRITE`, and its commit
  says why the source changed.
- A `switch` of the page document writes the case that a mode of the state
  names, and binds the keys of that case. An `each` writes its body for each
  item of a list; a name is looked up in the item first. An `act` names an
  action with its argument. An action is a named function of
  `struct fyai_page_action`. A document that names an unknown action, or binds
  `Ctrl-]`, `Ctrl-T` or `Ctrl-Tab`, does not transcribe.
- The keys of the active cases are bound with `fytim_set_key_bindings()` when
  they change, and are cleared when the page renderer stops. A bound key
  reaches fyai as `FYTIM_EVENT_KEY` and not the editor. A new name of the
  state goes into `data/page-state.schema.yaml`, which its test checks.
- A question to the user is a mode of the input area. `fyai_ui_ask()` puts it
  in the queue of the UI and calls its `done` function with the answer, or
  with NULL for none; the caller waits in the event loop, not in a nested
  loop of its own. `ask_user` and the question of a sub-agent take this path
  when `fyai_ui_ask_available()` says so and no `--answer` remains. The mode
  is `ask` while nothing is typed, where the number keys choose, and
  `ask_text` once text is typed, where they type. Escape and `^C` answer
  nothing. A caller that goes away withdraws its question with
  `fyai_ui_ask_withdraw()`. Agents put one question at a time, so the page
  adds `fyai_agents_questions_waiting()` to the questions it says wait.
- `display/page` names the file of a page document. `fyai_page_create()`
  loads it with `fyai_page_load()`: the file holds a mapping, matches
  `data/page.schema.yaml`, and passes `fyai_page_check()`, which transcribes
  every case of every switch, every node whatever its flag, and the body of
  every each once. A mode that is not showing must not hide an unknown action
  or key. A file that fails is not used: the warning names it and says why,
  and the embedded document draws the page. A change of the setting makes the
  page again. A page document is display configuration; never take one from a
  model.
- `/page` is a view: it commits the document in use, why a file is not used,
  and the state, source and regions of the last frame to the scrollback. The
  state of a frame lives in the builder of that frame until the next one.
- `display/screen: fullscreen` puts the page on the alternate screen when the
  session starts (`FYTIM_SCREEN_ALT`). There is no scrollback there, so the
  transcript is a view of the page, `src/fyai_transcript_view.c`, drawn into
  its `transcript` text region over the tail. The view renders the stored
  conversation through a view context and a render sink, as the branch browser
  renders its preview, so it is not a second transcript renderer. It renders
  one exchange at a time (`fyai_display_turn_range()`) and keeps the rows of
  each with the key of the exchange - the stored value of its last turn,
  which an arena does not move - and the width, so a turn renders only its
  exchange. An exchange the view does not show is measured with the measuring
  pass of a recap (`fyai_display_turn_range_rows()`) and rendered when a
  scroll shows it, so a new width renders only what the view shows. A view scrolled back keeps
  its top row through that: the exchange that holds the row, and the row in
  it. Rows presented
  during a turn go to the view through `ui_present()`, never to
  `fytim_commit()`, and a band that commits gives the view its rows. The wheel
  over the view and PageUp and PageDown scroll it; a view at its end follows
  what arrives, and a view scrolled back keeps its top row. A frame repaints
  only the cells that changed, so a PTY case waits on the screen
  (`wait-screen`), not on a line of bytes.
- The results, notices and diagnostics of a fullscreen session are not
  stored, so they are not the transcript. `fyai_ui_pane_end()` shows a
  result of two rows or less above the status, until the input changes or
  Escape clears it. A longer one opens a popup that covers the page - the
  transcript, the tiles and the chrome - under a heading that names it. The
  popup is the `open` case of the `popup.mode` switch of the page, and its
  keys are the page's: Escape, Enter and `q` close it (`popup.close`), and Up
  and Down scroll it a row (`popup.scroll`). PageUp, PageDown and the wheel
  scroll it too. Its rows are a transcript view model that shows the newest
  result from its heading; a result that arrives while it is open is added
  under the others. A drag over it copies the text of its rows. A closed
  popup drops its rows.
- A popup hides the tiles, so the page gives them no grant while it is open:
  a grant of no rows would size every program to nothing.
- A drag over the transcript view is a selection of its text region. Its
  text is the text of the rows in its cells, without styles, links and the
  blanks that end a row (`fyai_transcript_view_copy()`), and fyai copies it
  with `fytim_copy()`. When a fullscreen session ends it closes the UI,
  which gives the terminal its own screen back, and prints the last
  exchange there through the sink, so the answer stays after the exit.
- The head of a tile is drawn on the canvas too. A cell of the grid is a
  `head:N` slot over a `tile:N` slot: the head slot is as tall as the tallest
  head of the tiles that start on its row, so their screens stand level, and
  a cell keeps one screen row. The workpane manager keeps the rendered head
  (`fyai_workpane_tile_set_head()`), and the page draws it with the margin of
  the tile and, when the tile holds the keys, its ground through
  `fytim_cells_ground()`.
- The screen of a tile that holds a surface is drawn on the canvas too, in the
  slot `screen:N`, which binds no component of the library. The page reads the
  cells, the cursor, the margin and the ground back from the surface, and draws
  them as the library draws a surface: the last rows of a short region, the
  margin at each row, the cells washed by `fytim_cells_wash()`, and the cursor
  reversed. A tile shown as its head draws no screen and keeps its grant.
- A tile of text - a tool exchange, a notice, the queued-input report - is
  drawn on the canvas too, in the slot `text:N`. The page reads the content,
  the top and the bottom chrome and the row cap back from the band with the
  `fytim_workband_*()` getters, and draws them as the library draws a band:
  the last rows of the content that fit, a short region shedding the top
  first, and plain chrome dim under the rule style of the theme.
- The page owns the grant of the screens it draws. The workpane manager keeps
  it with the tile, and `fyai_ui_surface_granted_rows()` and
  `fyai_ui_surface_granted_cols()` take `ctx` to return it; a surface the page
  does not draw keeps the grant of the library. `fyai_ui_work_tile_cols()`
  does the same for a tile of text.
- A head act on the page is named for its tile, `tile:N:act`, because the
  page is one component: `ui_act()` finds the tile by its slot.
- A head the page draws is rendered at the granted columns of the tile, as
  the band stack renders it, so a long row is cut in the same place. Its zoom
  and close marks are not in its source: the page draws them in the last two
  columns of the head in the chrome style, where the band stack draws them,
  and names their acts `tile:N:zoom` and `tile:N:close`. A mark in the source
  would stand the right margin of the renderer short of the edge.
- `tests/cases/ui_page_tiles.sh` compares two tiles side by side under both
  renderers.
- Under the page renderer the head of a shell or agent tile is a tile page
  (`fytim_surface_set_page()`): the rendered head, then a `screen` slot, built
  by `fyai_page_tile()`. Its zoom and close controls are acts when
  `display/work_controls` grabbed the mouse, and `FYTIM_EVENT_ACT` routes
  `tile:focus`, `tile:zoom` and `tile:close`.
- The ladder of a tile selects the view of its page, where the manager
  reports the presentation: the whole page, the screen alone
  (`FYAI_WORKPANE_PRESENT_OUTPUT`) or the head alone. A view changes what is
  drawn, never what a tile asks for or is granted: the presentation is chosen
  from the grant.
- A page that cannot be built, rendered or accepted is a display failure: the
  session reports it, clears the page and the band stack draws the screen. A
  build without page support says so when `page` is asked for.
- `$FYAI_TRACE` records one `page:` line for each render.

### Work bands

A work band is a sink object. Ask `fyai_sink_bands_available()` before you
choose a banded presentation, open with `fyai_sink_band_open()`, repaint with
`fyai_sink_band_paint()`, and commit the shared band with
`fyai_sink_band_close()`. Do not call `fyai_ui_*` band functions from a
producer; the terminal backend owns that.

### The work pane

Every live shell session and sub-agent screen is a tile of the one work pane,
not a band of its own. A stack of full-width screens pushes the oldest off the
top and says nothing about which screen belongs to which call; one region that
tiles them says both.

`src/fyai_workpane.c` owns the pane. It is the one component that writes a
pane cap or a tile request, and it holds which tile has the keys and which
tile is zoomed. A producer states intent through a semantic operation -
register, focus, zoom, resize - and sizes nothing itself.

- Open a screen tile with `fyai_ui_surface_open()` and a text tile with
  `fyai_ui_work_tile_create()` - a parallel run of shells and sub-agents is
  not all of one kind, and both tile in the same region. Either creates the
  pane on the first call and retires it with the last tile. A producer never
  places a tile.
- Every band this program draws is a tile of that pane: the tool exchange,
  the queued-input report, and the result of a verb or a slash command. There
  is no second kind of band for a status or an error, and nothing draws
  beside the pane. A band that is committed leaves the manager with its tile,
  or the pane keeps a reference to a tile that has gone.
- A `FYAI_WORKPANE_TILE_NOTICE` is a report to the user rather than work. It
  takes a full-width row under the screens and is sized to what it holds,
  through `FYAI_WORKPANE_TRACK_FIT`: scaled down with an elastic screen it
  keeps only its last rows, and the heading that says what it is goes first.
- A tile registers with `fyai_workpane_register()`, which takes the height it
  asks layout for. That preference is explicit state. What the program has
  drawn, how many rows are dirty, and where its cursor is do not change it: a
  sub-agent that printed two lines still prefers the height it opened with.
  `FYAI_WORKPANE_FILL` prefers the whole pane, which follows the terminal, and
  is what a user-owned program takes.
- Focus never changes geometry. `fyai_workpane_set_focus()` moves the keys and
  the focus chrome and nothing else; Ctrl-T is not a layout operation. Zoom is
  separate: it selects the only visible tile and leaves focus where it was.
- A grant is an output. `fyai_workpane_layout_complete()` reads what layout
  gave each tile and hands it to the owner through `apply_grant`, which may
  size a pseudo-terminal and publish. It must not request rows or set a cap,
  or the grant becomes the next request and the tile collapses to its content.
  An acknowledgement records the grid with `fyai_workpane_grid_resized()` and
  states no intent.
- Retiring a tile is one transition: `fyai_workpane_unregister()` clears the
  focus and the zoom that named it in the same pass, so no stale pointer is
  left for the next keystroke to follow.
- `fyai_workpane_reconcile()` applies the whole state in one pass, before the
  frame that solves it. It is the only caller of `fytim_workpane_set_max_rows()`,
  `fytim_workpane_set_zoom()`, and the tile request functions. `$FYAI_TRACE`
  records one line per pass and one per tile.
- A shell reads the same whether or not it has a screen. The head of a tile
  is the title row of the call and, under it, the command the call was given.
  A chrome slot draws a row for each of its lines, and a tile too short for
  the whole head sheds its last row first.
- The head is chrome, not content. Held in the content the output of the call
  would scroll it off the top, which is when a reader most needs to know
  whose output this is, and the row cap would have to reserve rows for it.
  What the band commits is the head and the output together, so the
  transcript keeps the whole of it. Being chrome does not dim it: chrome that
  carries its own styling keeps the emphasis it was given.
- A tile's rows are hard-wrapped when they are made, so make them at the width
  the tile was given: `fyai_sink_band_cols()` for a band and
  `fyai_ui_work_tile_cols()` under it. The share changes as work starts and
  finishes beside it, so a progressive render has to be made again at the new
  width. Rows wrapped for the whole terminal are clipped at the tile.
- The pane owns the grid. A tile learns the size it was given from
  `fyai_ui_surface_granted_rows()` and `fyai_ui_surface_granted_cols()`, which
  is what a pseudo-terminal is sized to. Do not size a program to the
  terminal.
- A tile request uses the current terminal size, which
  `workpane_sample_size()` reads from the terminal. The library keeps the size
  from its last pump. This value is one frame old during a resize. A request
  that uses it asks for the rows of the old window, and the grant then gives
  the old height with the new width.
- A display setting takes on the next frame. `fyai_config_rederive()` tells a
  live session with `fyai_ui_config_changed()`, which reads the prompt chrome
  again and has the manager adopt the pane configuration and reconcile. A
  setting a component caches at open needs a line there, or it takes only on
  the next run.
- `display/work_position` says where the pane stands. `above-prompt`, the
  default, makes it the last band above the prompt block, next to the
  transcript the work came from. `below-prompt` gives it the rows under the
  prompt: the user types over the work, and a pane that grows moves nothing
  that is being read. The chrome is solved in what is left, so the prompt
  outranks the pane whatever the setting says.
- The pane holds its tiles oldest first, and an arrangement that names one -
  the whole top row, a column of its own - means the one that was there
  first. A cycle of the keys starts there too.
- No cell of the grid is left empty. A screen is a share of the pane, not a
  cell of a square that happens to have one spare: three tiles are two and a
  wide one under them, never two and a hole. The tiles of a short last row
  share the columns it leaves.
- `display/work_layout` chooses the arrangement: `auto` fits as many columns
  as keep every tile at `display/work_min_tile_cols`; `columns` takes `display/work_columns`;
  `stack` is one screen above another; `main-top` gives the oldest screen the
  whole top row with the rest sharing the row beneath it; and `main-left`
  gives it a column of its own with the rest stacked beside it. A terminal
  too narrow for two tiles stacks whatever the setting says.
- Every arrangement is an explicit grid: the manager solves the placement
  and states it with `fytim_workpane_set_grid()`, the track sizes, and one
  `fytim_surface_set_cell()` per tile. A span is what makes a tile wider or
  taller than its neighbours, which a grid of equal cells cannot express.
  `fyai_workpane_place()` is that decision on its own, and is what the tests
  assert against.
- `fyai_workpane_set_policy()` installs an arrangement of the user's: it is
  given the tiles and the terminal, and fills in a grid and a placement for
  each. A policy that places nothing usable leaves the arrangement to the
  library, so a bad policy degrades rather than blanks the pane.
- What a tile draws follows the size it was granted, not what it holds. A
  tile declares the sizes at which it changes with
  `fyai_workpane_tile_set_ladder()`, and the manager tells its owner through
  `set_presentation`: the screen, the screen without its head, the head and
  an activity mark, or nothing. A screen too small to read is worth less than
  the one row that says whose call it is, so a collapsed tile blanks its grid
  rather than showing a corner of a screen.
- `display/focus_bg` is the ground of what holds the keys - the prompt, a
  picker and a focused tile - and the one place focus is said. `theme`, the
  default, is the focus wash of the palette theme: the `pane.focus` role of
  libfypalette, resolved by `markdown_focus_ground()`. The theme makes it for
  the variant of the terminal, so its own text reads on it on a light
  terminal and a dark one. Do not put that colour in C. Without a palette,
  `theme` is `reverse`. `reverse` is the ground the terminal draws text in,
  named rather than given: it asks for no colour the terminal may not have,
  but reverse video turns text of a colour into a ground of that colour. A
  `#rrggbb` is a ground of your own. The prompt stands on the same ground, because that is where
  the keys are when no tile holds them and a tile that takes them takes the
  prompt's rows with them: one setting says where the keys are, wherever they
  went. An empty string draws no ground and reverses the margin of the tile
  alone. The library draws it under whatever the program
  draws - every cell with no colour of its own, the margin and the head with
  them - so focus needs no marker at the edge of a tile.
  `display/focus_bg_mix` is how much of a colour of your own reaches a cell
  the program coloured itself: mixing needs 24-bit colour and a colour to mix, so a cell
  drawn from the terminal's own palette is left alone, and so is the cursor,
  which reverse video is what makes visible. With no colour configured the
  margin is reversed instead, which is the mark a terminal of sixteen colours
  can carry.
- `display/work_frame`, `display/tile_frame` and `display/tile_sep` are the
  chrome. `display/session_margin` is the left gutter of a tile, sized
  against the tile. The pane draws no frame by default, and the gutter draws
  no rule, because every tile already carries the title row of the call that
  opened it and is already bounded by the pane: the gutter only holds the
  screen under the name of that call. The column rule is not the gutter: one
  divides two programs and the other runs down the inside of one.
- `display/work_cap` draws the cap row of the pane in place of the frame
  above it: the height, the tiles, how many are shown, how many the layout
  hid, and the keys. `fyai_workpane_cap_source()` writes it and the manager
  draws it again after each layout, so a tile the ladder hid is counted, not
  silently gone. It is chrome that fyai writes whole, so it needs no escaping.
- The head of a tile is UI Markdown. `fyai_ui_surface_set_head_frame()`
  escapes the title with `markdown_ui_escape()` - it holds what a model or a
  program wrote - and makes it the `tile:focus` label, so a click on the name
  gives the tile the keys. The manager keeps the regions of the head with the
  tile (`fyai_workpane_tile_set_regions()`) and frees them with it. A click
  reaches fyai only when the mouse is grabbed, which `display/work_controls`
  decides. Never render chrome with UI Markdown without escaping text that
  fyai did not write.
- `display/work_controls` draws mouse affordances on a tile. Anything but
  `none` grabs the mouse for the whole session, which takes selection and copy
  from the terminal, so it is off by default. The library reports a control as
  an event and acts on nothing itself: `fyai_tools_surface_request()` asks the
  component that started the program, because only it can end it.
- `/zoom` gives one tile the pane and the keys, so the user works in the
  program instead of watching it. The prompt keeps its row while a tile holds
  the keys: it is where the user goes back to, and what says the keys are
  elsewhere is that it is no longer lit. Nothing on the screen moves when
  focus does - a tile grows no line and loses none, and the way back is put
  on the status row, which stands whether it says anything or not. A line
  added to a tile when the keys arrive moves every tile beside it, twice, for
  one keystroke. Escape and `^C` are the program's and `^\` and `^Z` are the
  terminal's, so the way back is `Ctrl-]`. What the user types is echoed by
  the program, so it reaches the emulator and the line log with it: it is part
  of the tool result the model is given, and needs no separate path.
- Every key belongs to the tile that holds them, except the three this
  program keeps: `Ctrl-Tab`, `Ctrl-T` and `Ctrl-]`. Escape and `^C` are the
  program's - `^C` is the terminal of this process turning a key into a
  signal before it can be read as one, so `fyai_ui_interrupt()` gives it to
  the tile with `fyai_workpane_keys_deliver()` and the turn is not cancelled:
  the user was typing into a program, not stopping the model. `^\` and `^Z`
  stay this terminal's, because a program that took them could not be left.
- The keys of a tile arrive one frame at a time, not one key at a time. A key
  this program keeps for itself - `Ctrl-Tab`, `Ctrl-T`, and `Ctrl-]` - can
  thus arrive with
  what was typed after it, and that input belongs to whoever holds the keys
  once the key is acted on: another tile, or the prompt. Give the rest of the
  chunk back with `fyai_ui_keys_return()`, which puts it in front of the input
  the terminal has sent since, to be read again and routed then. Dropping it
  loses what the user typed, and a paste or a fast typist makes one chunk of
  the key and the line under it.
- Look a tile's owner up by surface. A program that ended must not leave a
  pointer behind for the next keystroke to follow.

### A live terminal that is resized

A tile that holds a running program is not rewrapped: the program owns its
grid and draws it again itself. A resize is a barrier between what the program
wrote at the old size and what it writes at the new one.

- The child owns the pseudo-terminal, so only the child can change its size.
  The parent sends `tty/resize`, the child drains what the program wrote,
  sets the new size, and sends `tty/resized`. The parent then resizes the
  view and publishes the new grid.
- Do not stop the program for a resize. A stop races with a fork: a process
  that forks while its group is stopped misses the SIGCONT and stays stopped.
  The kernel sends SIGWINCH when the size changes. A program that must not
  see the change during an operation blocks SIGWINCH and gets it when it
  unblocks it.
- Publish the grid before the next frame. A resize damages every cell, and the
  retained surface still holds the mechanically resized old cells. A program
  that repaints only what it changed never covers them.
- Do not rewrap a screen-addressed row and do not clear the transcript over a
  live surface. Claim a clean screen instead and let the next frame paint the
  whole retained pane.
- A full-screen program draws in the alternate buffer, so the view enables
  that buffer. When the program leaves it, the primary screen is empty: the
  whole-session read returns the stripped log, which is the result of the
  call.

### Bang commands

A line that starts with `!` is a command of the user. It is not a model turn.
`fyai_tools_bang()` starts it as a terminal session in the work pane and gives
that tile the keys. The prompt returns when the program ends. A bang line needs
an interactive terminal UI; without one, report that and do not run the
command.

- A bang session is user-owned. The model cannot close it and cannot read its
  output as a tool result. A tool call keeps the output limit of a shell tool.
  A user-owned session has no such limit: the user reads its screen.
- A user-owned tile can take more rows than the shared pane granted it. A tile
  of a tool call keeps the grant.
- `display/work_zoom_rows` is the height of the work pane. `full` uses the
  terminal height, `half` and `quarter` use that fraction of it, and an
  integer is a direct row count. It is the pane's disposition, not a mode that
  zoom turns on: the first bang shell opens at it, and a fraction is recomputed
  from the terminal after every resize. `display/work_max_rows` is a further
  ceiling on it, not an alternative to it. Zooming, unzooming, and moving focus
  leave the disposition alone. Kitty `Ctrl-Shift-T` cycles `full`, `half`, and
  `quarter` for the session.
- A named session shows its handle in the tile head, so the user can tell two
  sessions apart and name one in a later command.
- An editor for `/edit`, `config edit` or a log view runs as a user-owned
  session too, through `fyai_tools_user_program()`: its tile, `edit-N`, takes
  the pane and the keys. The session reports the end of the program to its
  owner through its exit callback, one time, also when the tile goes first.
  A caller suspends the UI only when `fyai_editor_in_pane()` is false.
  `display/editor: terminal` gives the editor the whole terminal, and a command
  without an interactive UI always does.

### Palette themes

`display/theme` can name a libfypalette theme, such as `ember:auto`, in place
of a libfymd4c theme. A palette theme styles the Markdown, the fenced code, the
reverse card and the chrome from one set of roles. The theme holds the policy:
do not put a colour for a role in C.

- Make every Markdown renderer with `markdown_renderer_new()`. It gives the
  renderer the palette of the configuration. A renderer made with
  `fymd_renderer_create()` directly keeps the default theme and breaks the
  one language.
- `fyai_markdown_load_style()` makes the palette for the resolved variant and
  the colour of the output. It checks that libfymd4c takes a palette and
  reports a cause when it does not.
- A renderer borrows the palette. A long-lived renderer can outlive a change of
  theme, so a palette stays alive until `fyai_config_cleanup()`. Reuse the
  current palette when the theme, the variant, the colour and the ground did
  not change.
- `display/theme_ground=terminal` makes the background of the terminal the
  ground of the palette with `fypal_ctx_set_ground()`, so the neutral ramp of
  the theme keeps its steps over that background. The theme names the ground
  and decides what follows it; fyai holds no colour. The terminal is asked
  once, with OSC 11, and the answer is kept in the configuration: a query
  while the UI reads the terminal would take its input. A background of the
  other variant is not applied, and a sub-agent does not ask. The build
  enables it (`FYAI_FYPAL_GROUND`) only when libfypalette has
  `fypal_ctx_set_ground()`; a PTY case answers the query with
  `$FYAI_PTY_BACKGROUND`.
- A fullscreen page with `display/theme_ground=theme` fills cells whose
  background is default with the palette's `ground` colour. Keep explicit
  backgrounds and cell attributes. Resolve the escape through the palette's
  capabilities, so ANSI defaults and disabled colour remain defaults. Give
  the completion status style the same ground. Inline pages and terminal
  ground do not apply this fill.
- The reverse-card probe sets the variant of each background on the palette
  and restores it. The renderer copies the escapes when it takes the palette.
- Take the colour of an element that fyai draws itself, such as a notice
  heading or a failure cause, from a role with `markdown_role_on()` and
  `markdown_role_off()`. Give the escape the element has without a palette as
  the fallback. Do not write a colour escape for an element that a role names.
- A diagram takes the palette through `fymm_render_cfg.palette`. The build
  enables it (`FYAI_FYMM_PALETTE`) only when the libfymermaid header has the
  field.
- A fenced `mermaid` block of a Markdown answer is drawn as a diagram.
  `markdown_renderer_new()` registers the libfymd4c block renderer, which goes
  through `fyai_sink_diagram_render()`, so the diagram takes the width, the
  colour and the palette of the configuration. The build enables it
  (`FYAI_FYMD4C_BLOCKS`) only when libfymd4c has block renderers. A source that
  does not render stays a code block.
- A palette theme also gives the glyphs and the gutter. The text column starts
  one column after the gutter, which is `gutter.cols` of the theme, else two
  columns. Take a gutter width from `markdown_gutter_cols()`, a blank gutter
  from `markdown_gutter_blank()`, and a mark from `markdown_glyph()` with the
  glyph it has without a palette as the fallback. Do not write a gutter of
  literal blanks: it moves the text column of a palette theme. Indent tool
  output with `markdown_tool_output_indent()`, which starts it at the text
  column.
- A stored assistant document opens reasoning with `FYAI_REASONING_HEAD`.
  A palette theme that has a `gutter.reasoning` glyph draws reasoning as the
  quote alone, under the hairline of its quote bar: the renderer removes the
  heading rows, and the stored document keeps them for every other theme. A
  path that makes reasoning Markdown asks `markdown_reasoning_quoted()`.
- The mark of a tool call is its state. `markdown_indicator_margin()` pads
  every frame of the indicator to the gutter width, so a blinking mark does
  not move the title row. The ASCII form of a glyph, which the `ascii` diagram
  charset selects, keeps the gutter width too.
- The build enables the glyphs (`FYAI_PALETTE_GLYPHS`) only when libfypalette
  has `fypal_ctx_glyph()` and libfymd4c has
  `fymd_renderer_set_palette_flags()`.
- `/theme` completes from `markdown_theme_selectors()`, which asks libfymd4c
  and libfypalette for their themes. Do not keep a list of theme names in C.
- libfypalette is optional. Keep the build and the tests correct without it:
  a palette theme is then not a valid `display/theme`.

### Tables

Render every Markdown table with `fyai_generic_to_markdown()`. Pass a
`renderopts` generic for titles, selected keys, names, alignment, and formats.
Build `renderopts` in the transient builder or in the same stack frame as the
render call. Do not return a builder-less `fy_mapping()` from a helper because
it uses stack storage. `stats` keeps its own table builder: its cells carry
currency and percent formatting that `renderopts` does not express.

Export is deliberately outside the sink. `fyai_export_view()` serializes
canonical messages to a `FILE *` in the `format: 1` textual grammar that
`src/fyai_import.c` reads back, and it must keep working when a conversation
has no stored `display_outputs`.

### Interactive rules

The interactive reader uses the shared event loop. Test terminal input,
signals, and resize behavior under a PTY. Non-TTY functional tests use the
blocking fallback and do not cover the event-driven path.

In the REPL, `/` starts a slash command and `//` sends a literal slash. Ctrl-C
discards nonempty input, ends idle input, or cancels an active turn. Escape and
SIGINT call `fyai_ui_interrupt()`. Keep `ISIG` enabled.

Request-shaping slash settings persist through the common commit path. Display
settings remain session-only. `--new` has the same state effect as `/clear`.
`/compact` makes one tools-disabled summary request and stores the previous
head as `compacted_from`.

## Event loop and process rules

Each invocation owns one lazy `struct fyai_ctx` event loop. Curl, shell
capture, tool jobs, MCP shutdown, readline, and OAuth borrow that loop.

- Each borrower must remove all sources that it registered.
- Store source pointers in the owner and clear them when callbacks retire a
  source. Make cleanup idempotent.
- A child that forks without `exec` must call `fyai_ctx_loop_abandon()`
  and `fyai_ctx_fork_disown()`.
- Abandonment closes the child's descriptor copies. It must not call
  `epoll_ctl()` on the shared epoll object.
- Do not run a nested event loop from an interactive callback.
- Use synchronous wrappers only in standalone commands, setup adapters, and
  isolated tests that own the top-level loop.

Sanitizer builds disable event-loop object recycling by default. Test both
paths. Set `FYAI_EVENT_NO_POOL=1` to disable pooling in a normal build. Set it
to `0` to enable pooling under ASAN.

### Signals

Do not register SIGINT with `signalfd`. Use
`fyai_event_interrupt_open()`. The handler sets the pending flag, wakes the
loop, and starts the watchdog. A pump that sees the interrupt must call
`fyai_event_interrupt_ack()`. Reserve SIGALRM for the watchdog.
`fyai_event_interrupt_close()` removes the handlers during context cleanup,
because they name the context.

The diagnostic dump is the one handler that does more than record and wake. It
must report a loop that is stuck, which is the state a deferred dump could
never reach, so it formats and writes from signal context. Keep it able to do
that: no allocation, no stdio, no `tcgetattr()` - the terminal mode is probed
when the dump is armed - and no blocking write. It refuses to start a second
dump while one is running. Its walk of the loop lists is unsynchronized by
design and stays bounded.

Keep SIGPIPE blocked in the parent. Restore the disposition in children before
`exec`. Set `CURLOPT_NOSIGNAL` on curl handles. Keep signal handlers
async-signal-safe.

An interrupted turn keeps its completed steps. Wrap the partial turn with the
diagnostic indirect through `fyai_with_diag()` and `fyai_report_diag()`.

## Tool jobs, shell capture, and time limits

A forked tool child uses descriptors 3 and 4 for its JSON-RPC control channel.
It does not use standard input and output for this channel. The child is forked
and does not call `exec`, so these descriptor numbers are a private contract.
A stray write to descriptor 1 goes to the terminal and not into a JSON-RPC
frame. Thus, it cannot make the parent read an invalid frame. The control
channel is close-on-exec. A shell command cannot access it.

Do not change the standard descriptors of an MCP server or the
`fyai agent --rpc` verb. These peers are separate programs that use standard
input and output. The sink keeps descriptor 1 clean for these programs.

Close each descriptor above standard error before `exec`. A command must not
hold the event loop, curl sockets, arena files, a control channel, or pipes from
a sibling job. Close the descriptor range with `fyai_close_fds_from()`. Do not
track individual descriptors. This prevents a new descriptor from becoming a
leak.

Every child that runs another program removes the credentials first with
`fyai_env_sanitize()`. Do this in the child, and fail closed: a partial
sanitize still gives the program a provider key. A configured `env` mapping is
applied after it, because `setenv()` adds and replaces but never removes. An
MCP stdio child follows the same rules and leads its own process group, so
teardown can stop its descendants with it. The child
sends `tool/progress` notifications and returns `{result, ok}`. It uses
`/dev/null` as standard input and calls `setsid()`. Do not call `setpgid()` in
the parent.

A forked child keeps what describes the work: the configuration, the arena and
its builders, the sink, and the credentials. It keeps nothing that names a
process, a descriptor, a timer, or a screen of the parent. A copy of such state
addresses another process by mistake, or answers with what belongs to it: an
inherited session took the name a sub-agent asked for and returned what the
program of the parent wrote, and an inherited wait took its name. Drop every
one of them in `fyai_ctx_fork_disown()`, which ends nothing, and add a new kind
of live context state to that list.

A child that serves a terminal session ends its program when its control
channel closes. The child is in a session of its own, so no signal of the
parent reaches it: a parent that is killed leaves only the closed channel to
say that nobody owns the session.

Set job fields after `fyai_tool_job_spawn()` because that function clears the
job. Cancel a complete process tree with
`fyai_event_add_child_terminate_group()`.

Finish shell capture when the direct child is reaped. Do not wait for pipe EOF.
A shell descendant can keep a pipe open. Keep the shell child in the process
group of the tool job.

Arm time limits in the parent with `fyai_tool_job_submit()`. A time limit
applies to the complete job and not to one child command. Remove the deadline
when the job finishes. Do not wait until collection. A group is collected only
after all its jobs finish. Thus, a completed job can wait for a slower sibling.
An active timer could incorrectly mark that job as timed out. Do not arm a
second limit in the child when `cfg->tool_child` is set.

`timeout_ms` is the user default. `max_timeout_ms` limits a value supplied by
the model. Apply the ceiling only to a model-supplied value. Use this order:

- Shell: call `timeout`, then `shell/timeout_ms`.
- Agent: call `timeout`, persona `timeout_ms`, then `agent/timeout_ms`.

The function shell tool uses argument `timeout`. A native Responses
`shell_call` uses `action.timeout_ms`. Pass the correct object to
`fyai_shell_timeout_requested()`.

Only the parent reports expiration. Preserve the normal result shape. A native
shell result is a list of `{stdout, stderr, outcome}`. On expiration, set the
outcome to `{type: timeout, timeout_ms}`. Keep all captured output. Do not
replace the result with an error string or a signal outcome.

A program can send a query to its terminal. It asks for the terminal type or
for the cursor position, and it waits for the reply. The terminal view is that
terminal, thus the view makes the reply. Set a reply callback on each view
with `fyai_terminal_view_reply_cb()`. Write the reply to the program with
`fyai_terminal_reply_write()` when this process holds the terminal descriptor,
or with the `shell/write` notification when the tool child holds it. Without a
reply the program waits for its own time limit, and it then reads the next
typed input as the reply.

Apply shell `workdir` before Landlock confinement. Exit status 125 reports an
unreachable working directory. Landlock is Linux-only. Other platforms use
the same interface with no confinement backend. Keep command admission
separate from filesystem confinement.

A configured deny is carved out of every granted hierarchy, the broad scratch
and system trees included. Landlock has no non-recursive rule, so a directory
above a denied path cannot be granted whole: it is descended, each undenied
child is granted, and the directory itself keeps only the right to list its
entries. That cost is why the implicit arena deny stays scoped to the project
root, and why a deny path that does not exist is left out.

Fail closed on network egress. The kernel ABI is not the only condition: a
build made against headers without the network access definitions restricts
nothing on any kernel. Ask `fyai_sandbox_net_restrictable()`.

## Sub-agents

`fyai_agent_run()` takes the full call arguments: `task`, `name`, and
`description`. A sub-agent owns its persona, tools, conversation, curl handle,
and output routing.

- Store each sub-agent conversation on a child branch such as
  `main/agent:greeter`.
- Keep `fyai_branch_name_valid()` strict for user-created names.
- Let `fyai_branch_name_ref_valid()` accept the reserved `agent:` marker.
- Reject a duplicate agent name. Do not add an ordinal automatically.
- Refresh the branch table before you test a name.
- Reserve the name in the parent before `fyai_tool_job_spawn()`.
- Copy the name into the job after spawn.
- Set child persona fields after `fyai_arena_reopen()`.
- Reopen the arena in a forked child before it publishes.

`context: fork` starts at the parent head. `context: fresh` sends only the
task. In fork mode, add the persona as a user instruction message. Do not add a
second system turn.

Expose only configured persona names and descriptions in the tool schema.
Resolve a persona model through the catalogue in a scratch configuration.
Preserve user-set `api_url` and `max_tokens`; replace only derived values.

A directly delegated sub-agent has a terminal of its own. It renders there
as this program renders to any terminal. The parent interprets that terminal
and shows it on a surface, behind the session margin. There is no second
transcript renderer for a sub-agent. A direct sub-agent's screen includes its
tool results. Grandchildren have compact progress in that screen; deeper work
contributes descendant counts. Do not allocate a terminal for every descendant.
Explicit inspection subscribes to bounded document source through the sink.
`cfg->agent_pty` says that a child has a terminal. It lets `sink_may_present()`
present, and it stops the child from sending progress that the parent showed
already.

## OAuth and JSON-RPC

Implement OAuth as a state machine on the borrowed event loop. The start
function must arm the flow and return. Keep the synchronous wait function as a
wrapper over the same state machine.

Bind loopback redirects to `127.0.0.1`. Allow concurrent browser connections.
Return 404 for unrelated paths and continue to wait. Keep issuer, client ID,
scopes, authorization query, token exchange, and credential storage in the
provider-specific caller.

Configured MCP clients survive logout. Remove dynamic clients on logout
because they are tied to one redirect URI. Release discovery state on every
terminal path. Explicit login or logout cancels active browser, discovery, and
refresh work.

A JSON-RPC standard-I/O connection owns its reader, writer, and buffers. Match
responses by request ID. Remove a completed request from both connection lists
before you release its storage.

Every path that ends a connection settles each outstanding request, in
`pending` and in `flush_wait`. A request with no timeout has no other way to
complete. An invalid frame is such a path: the peer can hold the connection
open indefinitely. Destruction also clears the connection pointer on each
request it settles. One frame has a hard size limit, so a peer that sends no newline
cannot grow the receive buffer without end.

## Diagnostics

Collect diagnostics in `cfg->diag`. Drain them only at turn, verb, and slash
command boundaries. Do not print diagnostics during streaming output.

Define `FYAI_MODULE` once in each source file. Let the module add the message
prefix. Do not type the prefix into each message.

The first error is the cause. Demote subsequent errors to debug until the sink
is drained or reset. A path that stops must report the cause at that location.
Do not report only that an operation failed. That report gives no cause.
`fyai_run_turn()` reports a cause on each path that returns no result. It also
supplies a fallback cause.

The component that holds an identity owns it. A sub-agent raises the cause.
Only the caller knows which parallel sub-agent raised it, so the caller adds
the name. Copy a name from generic storage before you run the sub-agent. The
operation reopens the arena, and a cast pointer does not remain valid. Use
`fyai_diag_reset()` after a caller recovers from an error.

A forked tool child exits through `_exit()`. No component drains its sink. The
child must give its collected diagnostics to the parent.
`fyai_diag_take_generic()` puts them in the `tool/run` response. The parent
uses `fyai_diag_adopt()` when it collects the job. Keep the severity and module
that the child recorded. The parent adds a marker because only the parent knows
which child raised the diagnostic. For example:
`[main/agent:greeter] the reason`. Keep an existing marker. During nested
delegation, the innermost path identifies the source.

Do not remove diagnostics when you quote them. The model reads a reason in a
tool result. The user must also receive the same reason. Use
`fyai_diag_string()`. This function renders the diagnostics but does not remove
them.

A child that terminates can have no diagnostic to send. The parent must state
how the job ended. Report the signal or the absence of a result. Put all
details for one failure in one diagnostic. Use a lower severity for a nonfatal
report.

The diagnostic sink owns its own builder and its own output descriptor. Do not
use the durable builder or `transient_gb`, and do not route it through the
rendering sink: diagnostics must report before that sink is created and after
it is destroyed. It is the one output owner beside the sink.

Parse provider JSON with `FYOPPF_COLLECT_DIAG`. The parser then attaches its
report to the failed value instead of writing it to standard error, where it
names the anonymous input buffer by address, sits outside the sink, and cannot
be attributed to the request that produced it. Report it with
`parse_diag_text()` from a caller that has a context, in the one diagnostic
that says what was being read.

### Suppressed results

`(void)` on a call that can fail hides the cause of the failure. Use it only
where there is nothing to report:

- A cleanup, destroy, or release path, which cannot act on a failure.
- A callee that reported the failure itself.
- A function whose result is not a status, such as a count or a predicate.

On a content path, a failure must reach the user. Report it, and stop the
operation that cannot go on. On a display path, report it as a warning: the
page is wrong, but the turn continues. State that policy one time for a
component with a wrapper macro, such as `browser_warn_check()`, instead of a
warning at each site. Do not report each row of a loop: stop at the first
failure and report one time.

Input the user typed is content. A key or a line that a component drops must
be reported.

### Resource failures

Report a resource that cannot be acquired with `fyai_error_check()`. This
applies to `malloc()`, `calloc()`, `realloc()`, `strdup()`, `open_memstream()`,
`asprintf()`, a builder, a surface, a request, and a connection. The message
must name what could not be made. A path that returns a null pointer without a
diagnostic gives the caller no cause.

### The trace log

The diagnostic sink presents required user actions at a turn boundary. It does
this in one process. The trace log records each raise when it occurs. It records
raises from all processes and keeps the original severity. It also records
raises that the mask removes. The trace remains available if a forked child
terminates. Use it to examine a stress run.

`$FYAI_TRACE` turns it on: `1` or `on` writes `~/.fyai/trace.log`, any other
value is the path to write. `$FYAI_TRACE_LEVEL` sets the lowest severity
recorded and defaults to debug.

Write each record as one line with one `write(2)` operation. Use an append-only,
close-on-exec descriptor. The parent and its forked children share the file.
The single write prevents incomplete concurrent lines. Do not use a low
descriptor number. A tool child uses descriptors 3 and 4 for its control
channel. A child that closes inherited descriptors must call
`fyai_diag_trace_reopen()`.

`fyai_diag_trace_tag()` adds the sub-agent branch to process records.
`fyai_diag_tracef()` records events that are not diagnostics. These events
include process start and child termination. Tracing must fail silently. A
trace-write failure must not raise a diagnostic.

Expand formatted text before you intern it. Diagnostic raises are lock-free.
A drain resets storage. Drain only when all raisers are inactive. A null sink
prints immediately.

Keep normal output out of the diagnostic sink. The banner, spinner, shell echo,
approval prompt, stats, usage, and successful command results are presented
content: send them to the rendering sink on the status or notice stream.

## Build and test

Configure and build:

```sh
cmake -S . -B build -G Ninja
ninja -C build
./build/fyai -m gpt-4o-mini "hello"
./build/fyai dump state
```

Run the full test suite through the parallel test target. Use `ctest` only for
targeted test runs:

```sh
ninja -C build parallel-test
./build/fyai_test --list
./build/fyai_test oauth/plain_redirect
ctest --test-dir build -R fyai/unit/oauth
```

The unit-test binary links production sources except `src/main.c`. Do not use
test stubs. Declare tests with `FYAI_TEST_ENTRY(suite, name, entry)` after the
includes in a `tests/fyai_*_test.c` file.

Functional cases run the real binary with the scenario-driven mock provider.
Keep them hermetic: localhost only, with private `HOME` and `XDG_*` paths. A
case that looks for a process must scope the search to its own run - its
process tree, or its scratch directory in the command line. A search of the
whole machine finds another run's children. Never `pkill` a global pattern.

A test driver must read its pseudo-terminal for the full run. A driver that
sleeps without reading fills the terminal buffer. The program under test then
blocks in its write until the driver reads again. The buffer is 1 KiB on macOS
and 12 KiB on Linux, thus a case that stops reading can pass on Linux and fail
on macOS. Wait by reading with a deadline. Do not sleep.

Do not pace a test with a delay. A delay that is sufficient on one runner is
too short on a slower runner, and every run spends its full time. Wait for the
state that the next step needs. The PTY driver does not pause after an
action. A PTY case waits on the terminal state that `tests/screen.py` models:
`wait-screen` for text on the screen, or on a row that scrolled off it or that
an erase of the display removed since the last action, `wait-gone` for text
that left the screen, `wait-copy` for an OSC 52 copy, and `frame` for a key
that must be acted on. Do not wait on the raw capture bytes with `wait` or
`wait-frame`: a frame paints only the cells that changed, so the bytes do not
say what the screen shows. Do not use
`drain` or `settle` to wait for a state. A program in a tile that reports a
changing value, such as its size, prints it on a short interval, and the case
waits for the expected value on the screen. A key that must change nothing
has no state to wait for: send it in one write with the key that follows it,
which keeps their order.

A case that stops a run does not leave its processes behind. The driver sends
SIGTERM before SIGKILL, and a case that kills fyai on purpose ends the programs
it recorded if the run left them.

### Sanitizers

Use ASAN for parser, storage, tool, event, and YAML changes:

```sh
cmake -S . -B build-asan -G Ninja -DENABLE_ASAN=ON -DCMAKE_BUILD_TYPE=Debug
ninja -C build-asan
ninja -C build-asan parallel-test
```

The complete functional suite must also pass under ASAN.

Every deadline in the suite is multiplied by `FYAI_TIMEOUT_SCALE`, which CMake
sets to 3 for a sanitized tree and to 1 otherwise. A run raises it without
reconfiguring:

```sh
FYAI_TIMEOUT_SCALE=4 ninja -C build parallel-test
```

Scale a deadline only. A value that paces a case, such as how long the PTY
driver waits before it types, is a step of the case: scaling it changes what
the case does.

### Static builds

- `FYAI_MOSTLY_STATIC`: statically link dependencies, but keep glibc, libm,
  and the loader dynamic. This mode includes the glibc C23 compatibility
  wrappers.
- `FYAI_MUSL_STATIC`: build a fully static musl binary with the Docker static
  builder.
- `FYAI_STATIC`: use the legacy fully static host-libc mode only for controlled
  same-host tests.

Static dependency modes require `libfyaml.a`. Glibc modes also require static
OpenSSL and zlib libraries. A mostly-static binary must list only libc, libm,
and the loader in `ldd`. A musl-static binary must report that it is not a
dynamic executable.

### Terminal rendering tests

Use libfyvterm when correctness depends on terminal cells, wrapping, cursor
position, background fill, or SGR state. A PTY byte capture cannot prove these
properties.

Use two `struct fyvt_screen` screens. Feed direct libfymd4c output to one
screen. Feed the same content through the libfytimui path to the other.
Compare the complete stable row and adjacent blank rows. Inspect cells after
the final glyph. Normalize colors with `fyvt_screen_convert_color_to_rgb()`
and compare them with `fyvt_color_is_equal()`. Do not compare
`union fyvt_color` with `memcmp()`.

Make the new test fail before the fix. Register it as an individual CTest test.
After the fix, run both the libfytimui and fyai suites. If renderer bytes are
correct but cells differ, trace transport writes and later compositor cleanup.

## C style

Use Linux kernel C style:

- Use hard tabs with 8-column stops.
- Use kernel braces and spacing.
- Declare local variables at the start of the function.
- Do not declare variables inside branches or loops.
- Use `lower_snake_case` for C names.
- Use uppercase names for CMake options.
- Use four spaces in CMake files.
- Compile as GNU C2x with `-Wall -Wextra` and
  `-Wdeclaration-after-statement`.
- Add SPDX headers to new source files.
- Avoid whitespace-only alignment changes.
- State ownership and non-obvious arena, mmap, atomic, durability, and
  filesystem assumptions near the code.

Do not put an operation inside an error-check predicate. Run the operation,
store its result, and then test the result:

```c
	rc = epoll_ctl(el->backend_fd, op, fd, &ee);
	fyai_event_error_check(el, !rc, err_out,
			       "epoll_ctl: %s", strerror(errno));
```

Do not write `fyai_event_error_check(el, !epoll_ctl(...), ...)`. Apply this
rule to all error-check macros. If a subsystem reports through another handle,
define one wrapper macro instead of repeating `->ctx`.

Use ASD-STE100 Simplified Technical English for changed documentation,
retained comments, and commit messages.

## Commits and patch series

Use an imperative commit subject with a subsystem prefix, for example:

```text
cli: add interactive prompt mode
```

Use two or three short body lines. State what changed and why. Wrap at 80
columns. End with exactly this trailer:

```text
Signed-off-by: Pantelis Antoniou <pantelis.antoniou@konsulko.com>
```

Do not add another attribution trailer.

Make each patch one logical change. Build each intermediate patch. Keep these
changes in separate patches and in this order:

1. implementation;
2. tests; and
3. documentation.

Fold a fix into the patch that introduced the defect. Do not add a later fixup
patch. Remove temporary notes and revision history from the final series. Run
the applicable tests after each test patch. Run normal and ASAN suites on the
final patch. Run `git diff --check` on each patch.

## Patch review workflow

Review `master..devel` as a mail series. Before the first review, tag the tip as
`start-of-review` and generate the series:

```sh
git format-patch -o x master..devel
```

The reviewer writes notes that start with `panto>>` into numbered patch files.
Before you rewrite commits, find every annotated file:

```sh
rg -l '^panto>>' x/[0-9][0-9][0-9][0-9]-*.patch
```

Move every annotated numbered file to `_ANNOTATED-NNNN-...patch`. Preserve all
of them before you regenerate any numbered file. Never edit, replace, or
remove an `_ANNOTATED` file.

Fold each requested change into the commit represented by its patch. Carry
interface and semantic changes through all later commits. Keep every
intermediate commit buildable. Resolve commits again from the current series
order after each history rewrite.

When a patch is accepted, rename its preserved file to
`_REVIEWED-NNNN-...patch`. Never edit or replace a `_REVIEWED` file. Remove
only numbered patch files before you regenerate the series. Confirm that new
numbered files contain no `panto>>` notes and that preserved files still
contain their notes.

Keep branch `reviewed` based on `master`. Apply accepted rewritten commits to
it in order. Confirm that its tree matches `devel` at the last accepted patch.
Then switch back to `devel`. Do not apply annotated mail files.

Before regeneration, run `git diff --check master..devel`, build the normal
tree, and run relevant tests. Run the complete ASAN suite after semantic or
structural changes. Remove temporary checkpoint commits and safety tags when
the rewrite is complete. Leave `devel` checked out. Do not change untracked
review data except for the requested files under `x/`.
