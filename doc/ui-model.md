# The UI model

The UI model is the state that every renderer reads. The engine writes it. The
terminal renderer reads it at frame time. A remote client, an HTML renderer or
a GPU renderer reads the same state as a stream. `src/fyai_uimodel.h` is the
interface. `doc/ui-architecture.md` gives how the parts work together.

## Rules

- The model holds data and Markdown source. It holds no SGR, no rendered rows,
  no cells, and nothing that depends on a width or a theme. A renderer
  formats, colours and wraps.
- View state is not in the model. Focus, zoom, scroll positions, the popup,
  the layout, the theme, the prompt text and the cursor of a question belong
  to each renderer.
- A store is one generic root. A write replaces the root, or one member of a
  mapping root with `fyai_uimodel_set_member()`. A root equal to the current
  one changes nothing and counts no change.
- Each store has a sequence number. A renderer that draws in a frame compares
  it with the number it drew, and reads the root again when it moved.
- A consumer that is told of each change subscribes with
  `fyai_uimodel_subscribe()`. It borrows the root for the call.

## Memory

A store has two builders. A write resets the builder of the root before the
current one, copies the new root into it, and makes it current. A store thus
holds at most two roots, whatever the number of writes. A root that
`fyai_uimodel_get()` returned is valid until the next write of its store.

A writer builds a new root between `fyai_uimodel_begin()` and
`fyai_uimodel_end()`, in the scratch builder that begin returns. The outermost
begin resets it, so a writer that runs during another write, such as the
adoption of a branch, keeps what the outer writer built. Do not build a root
in the transient builder: a writer that runs for each chunk of a stream would
grow it until the turn ends.

A forked child drops the model with `fyai_uimodel_abandon()`. Its values
describe the display of the parent, such as a turn that runs there.

## Stores

| Store       | Root                                                          | Writer                                   |
| ----------- | ------------------------------------------------------------- | ---------------------------------------- |
| `session`   | `header`: the values of the header and the status templates; `turn`: `{busy, started}`, the start in milliseconds of the real-time clock; `head`: `{updated, op}` of the branch entry that the context adopted, or null | `fyai_session_banner_update()`, `fyai_session_turn_set()`, `fyai_session_head_set()` |
| `panels`    | `todo`: the rows of the todo list of the branch               | `fyai_ctx_branch_prev_set()`, at each adoption of a branch entry |
| `questions` | `open`: the sets of questions, each `{qid, from, questions}`  | `fyai_ask_open()`, `fyai_ask_close()`, `fyai_ask_settle()` |
| `log`       | `entries`: the output of each command, `{id, title, error, level, parts}` | `fyai_uilog_begin()`, `fyai_uilog_end()` |
| `work`      | `items`: the live work, `{kind, name}`: a `user-shell` or `shell` that runs, an `agent` that runs; `bands`: the bands of the work pane | `fyai_tools_work_update()`, at each start and end of a session or a job; `src/fyai_uiwork.c` |
| `transcript` | `live`: the open output document, `{tag, source, length, fragments, recorded, paused}`, or null; `last`: `{tag, state}` of the document that ended last | `src/fyai_uitranscript.c`, from `fyai_output_begin()`, `fyai_output_append()`, `fyai_output_append_recorded()`, the fragments and `fyai_output_finalize()` |
| `state`     | `display`: the stored display configuration of the branch | `fyai_uimodel_state_update()`, at each derivation of the configuration, for a consumer only |

The `header` member holds values, not template expansions: the model, the
provider, the grammar, the reasoning settings, the temperature when the model
takes one, `context: {used, window}`, `cost: {amount, estimated}`,
`switch_delta`, `cache: {input, cached}`, the branch, an attached agent and
its state, the directory as the header names it, the isolation, the view and
the sandbox profile. `fyai_session_header_format()` expands the configured
templates from them.

The log taps the input of the display. While the span of a command is open,
`fyai_sink_markdown()` and `fyai_sink_write()` give each call to
`fyai_uilog_sink()`, as `{stream, md}` for Markdown source and
`{stream, text}` for bytes in their final form, and the diagnostic drain gives
each record it reports as `{stream: drain, item}`.
`fyai_sink_markdown_source()` gives `{stream, md, source: true}`: the medium
got the Markdown as written, because display/markdown is off. `fyai_diag_format_item()`
writes such a record as the drain does. The span ends with one entry, or none
when the command gave no output. The store keeps the last `FYAI_UILOG_MAX`
entries. The span starts with the pane span of the terminal renderer, and
records whether a terminal UI is open or not.

A band enters the work store at its input. The sink gives each band that it
opens, paints, closes, commits or destroys to `src/fyai_uiwork.c`, and a fenced
stream gives each push of source. A band item is
`{id, shared, title, command, format, ...}`:

- `markdown` (the quoted prose of a sub-agent) and `code` (a stream with a
  `language`) are source. A renderer renders them at its own width. A stream
  keeps the last `lines` rows, and `indent` says that it stands under the
  indent of tool output, as the stream of its producer does.
- `terminal` is the bytes a program wrote on a pipe. They can move a cursor
  and erase a row, as a program that draws for a terminal does, so a renderer
  replays them through a terminal model and does not read them as text.
- `tool-view` is the view of a tool call: its Markdown in `text`, the code
  `blocks` it holds and the `preview` rows of each, which
  `fyai_tool_view_render()` renders as the view of the worker was rendered.
- `rows` is a body that its producer painted in its final form, in `text`.

A terminal session is an item of `items`: `{kind, name, title, command, keep,
pane, terminal}`, where `keep` says that its screen stays after its program
and `pane` that it takes the pane and the keys, as an editor does. A session
that ended stays in the store while it is kept, with `exited`, `exit_code`,
`signal`, `closing` and `timed_out`, so a consumer ends its screen as the
outcome says.

The source of a stream is an append-only text of the model, named by
`source` with the `length` it had at the write. An append copies nothing that
is in a store: a renderer reads the text with `fyai_uimodel_text()`, and a
remote client takes the bytes after the length it has. A stream that is not
the owner of the source of a band starts it again. A terminal session is not
a band: the item of the session in the `items` member names a text of the
bytes of its program, `terminal: {source, rows, cols}`, which starts with a
snapshot of the screen, as the cast protocol records output. The item of a
sub-agent with a terminal names the text of its screen the same way.

A band that ends stays in the store for one write with `state` (committed,
destroyed or closed), `ok` and `cause`, so a consumer ends its own band the
same way, and then goes. A process with no display has bands while a
consumer reads its model: the terminal sink gives it bands that present
nothing, so the producers use their banded path and the store takes it. A
fenced stream with no file paints its band, as a stream under a display does.

The transcript store takes the live document at the input of the output
document. Its source is an append-only text of the model, as the source of a
band is. `recorded` holds the ranges that `fyai_output_append_recorded()`
added: another path, such as the band of a tool call, presented them already,
and a renderer that draws the bands does not draw them again. `paused` says
that the live render stopped at a checkpoint, so the rows of a tool exchange
go under the prose before it. The document ends finalized, aborted, or
discarded when it ends without a record. The stored exchanges stay in the
arena, where a renderer reads them from the head of the branch: the `head`
member of the session store changes when the head moves.

A copy of a context that renders stored output offscreen, such as the
transcript view and the branch browser, is detached from the model with
`fyai_uimodel_view_detach()`. It writes nothing, and it does not release what
the live context holds.

A set of questions belongs to the engine. `fyai_ask_open()` keeps its callback
and writes the set to the store. The input area builds its queue from the
store, keeps the cursor and the choices as view state, and gives the answer to
`fyai_ask_settle()`, which settles the callback and removes the set. When the
renderer goes, `fyai_ask_settle_all()` settles every set with no answer.

## The renderer

`src/fyai_uidraw.c` draws the stores with the sink of the context, from the
stores alone. The model gives it each change of a store when the change is
made, before a consumer gets it, so it draws in the order of the writes. A
local session and the client of a remote session use the same renderer:

- The live document. It opens a document of the sink when `live` names a new
  text, gives the sink the bytes up to `length` less the `recorded` ranges,
  pauses and resumes the render as `paused` changes, and ends the document as
  `last` says. `src/fyai_output.c` does not call the sink.
- The log. It replays the parts of each new entry into the capture of the
  pane, each as a notice, because the span captured every stream as the
  output of the command, and gives the rows to `fyai_ui_log_show()`, which
  places them as the output of a command is placed. A display that is not
  open shows nothing again. A command that a mirror runs itself writes no
  store, so the pane shows its capture.
- The bands. It opens a band of the display for each band of the work store
  and draws it from its item: a stream for the source of `markdown`, `code`
  and `terminal`, `fyai_tool_view_render()` for `tool-view`, and the rows of
  `rows`. It ends the band as the store says. The renderer calls the sink with
  `drawing` set, so its bands are bands of the display and the work store
  does not take them.

While the renderer draws the bands, the band that a producer opens is a
shadow: it presents nothing, keeps its last paint, and has the width of the
band of the display that draws it (`fyai_uidraw_band_cols()`). The shared
band of the producers is a second shared band of the sink, so the renderer and
the producers never replace the band of the other.

## Consumers and mirrors

A consumer of the model subscribes to the changes of the stores with
`fyai_uimodel_subscribe()` and to the appends of the texts with
`fyai_uimodel_text_subscribe()`. `fyai_uimodel_text_foreach()` gives a new
consumer each open text whole. A writer that a local display gates, such as
the header and the questions, also writes when `fyai_uimodel_consumed()` says
that a consumer reads the model: a remote worker has no display.

A client of a remote session makes its model a mirror with
`fyai_uimodel_mirror()`. The frames of the worker write it through
`fyai_uimodel_mirror_store()` and `fyai_uimodel_mirror_text()`; the writers of
the client write nothing, and no oracle runs, because the oracle of a mirror
is in the worker. What the renderer asks of the worker, such as the answer to
a set of questions, goes through `fyai_uimodel_intent()`. The tiles of a mirror
have the hooks that `fyai_uimodel_mirror_tiles()` sets: the key router of the
work pane gives them their keys first, and the chrome asks whether the screen
of a tile is kept to read, for the hint of the status row. The entries of the
log that the snapshot holds were shown before the client attached: the client
calls `fyai_uidraw_attached()` when its snapshot is complete.

The `state` store holds what a local renderer reads from the arena: the
client has no arena. The stored display configuration of the branch is the
configuration that the client starts from, under the settings that the client
changes itself. The worker sends the state first, and the client opens its
display with the first stored turns, as a local session opens on its
conversation; until then it holds the other frames in their order.

A terminal session of the worker is a mirror session of the client
(`fyai_tools_mirror_open()`): a session of `src/fyai_tools.c` whose program
runs in the worker. It has the tile or the block, the head, the keys and the
end of a local session, from the same code. Its keys, its size and its end go
to the worker through `struct fyai_tools_remote`. A line that the user types
in the client is echoed there, as a local session echoes it, and the card of
its document from the worker is not drawn again (`fyai_uidraw_echoed()`).

## Parallel construction

While a part of the screen moves to the model, the legacy path that made it
stays as its oracle. The renderer presents what it makes from the model, and
`fyai_uimodel_check()` compares it with what the legacy path makes:

- the header and the status, against the legacy builder of the context;
- the busy state of a turn, against `ui->busy`;
- the todo panel, against the reading of the branch entry;
- the counts of the header panel, against `fyai_tools_counts()`;
- a set of questions, against the copy that `fyai_ui_ask_oracle()` keeps;
- the live document, against the operations that `src/fyai_output.c` records
  with `fyai_uidraw_oracle()` where it called the sink: what the renderer gave
  the sink, and in which order, when the document ends;
- the output of a command, with its title and level, against what the pane
  captured during the span;
- the body of a band, against the last paint of its shadow, when the band
  ends.

A difference goes to the trace and, when `$FYAI_UI_MODEL_CHECK` names a file,
to that file with both texts. The test harness names a file in the sandbox of
each case and fails the case when the file is not empty, as it does for a
sanitizer report. An oracle is removed when its part of the screen no longer
has a legacy path.

A client of a remote session has its own oracle: the local session. An oracle
case (`tests/oracle-harness.sh`) runs one script in a local session and
through `--remote .`, from the same sandbox and the same replies, and fails
when a checkpoint of the screens differs (`doc/pty-driver.md`).

## Remaining work

- The producers still render what their shadows and spans keep for the
  oracles. When the oracles go, a producer writes only the model.
- The stored exchanges for a client without the arena: a page of the records
  of each exchange. The client reads the last turns again when the head moves.
- The screens of sub-agents in a client are tiles of `src/fyai_remote_ui.c`,
  not mirrors of the agent screens of `src/fyai_tools.c`: their head and
  their end differ from a local session.
- The branch browser, the resume picker and the completion of a client read
  no state of the worker: the branch names go in the `state` store next.
- A page case for a question that a sub-agent forwards: the `from` of a set
  has no case under the page.
