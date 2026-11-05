# The UI model

The UI model is the state that every renderer reads. The engine writes it. The
terminal renderer reads it at frame time. A remote client, an HTML renderer or
a GPU renderer reads the same state as a stream. `src/fyai_uimodel.h` is the
interface.

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
| `session`   | `header`: the values of the header and the status templates; `turn`: `{busy, started}`, the start in milliseconds of the real-time clock | `fyai_session_banner_update()`, `fyai_session_turn_set()` |
| `panels`    | `todo`: the rows of the todo list of the branch               | `fyai_ctx_branch_prev_set()`, at each adoption of a branch entry |
| `questions` | `open`: the sets of questions, each `{qid, from, questions}`  | `fyai_ask_open()`, `fyai_ask_close()`, `fyai_ask_settle()` |
| `log`       | `entries`: the output of each command, `{id, title, error, level, parts}` | `fyai_uilog_begin()`, `fyai_uilog_end()` |
| `work`      | `items`: the live work, `{kind, name}`: a `user-shell` or `shell` that runs, an `agent` that runs; `bands`: the bands of the work pane | `fyai_tools_work_update()`, at each start and end of a session or a job; `src/fyai_uiwork.c` |
| `transcript` | `live`: the open output document, `{tag, source, length, fragments, recorded}`, or null; `last`: `{tag, state}` of the document that ended last | `src/fyai_uitranscript.c`, from `fyai_output_begin()`, `fyai_output_append()`, `fyai_output_append_recorded()`, the fragments and `fyai_output_finalize()` |

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
each record it reports as `{stream: drain, item}`. `fyai_diag_format_item()`
writes such a record as the drain does. The span ends with one entry, or none
when the command gave no output. The store keeps the last `FYAI_UILOG_MAX`
entries. The span starts with the pane span of the terminal renderer, and
records whether a terminal UI is open or not.

A band enters the work store at its input. The sink gives each band that it
opens, paints, closes, commits or destroys to `src/fyai_uiwork.c`, and a fenced
stream gives each push of source. A band item is
`{id, shared, title, command, format, ...}`:

- `markdown` (the quoted prose of a sub-agent) and `code` (a stream with a
  `language`) are source. A renderer renders them at its own width.
- `terminal` is the bytes a program wrote on a pipe. They can move a cursor
  and erase a row, as a program that draws for a terminal does, so a renderer
  replays them through a terminal model and does not read them as text.
- `rows` is a body that its producer painted in its final form, in `text`.
  The tool-call and patch views paint such bodies; their Markdown source is
  not tapped yet.

The source of a stream is an append-only text of the model, named by
`source` with the `length` it had at the write. An append copies nothing that
is in a store: a renderer reads the text with `fyai_uimodel_text()`, and a
remote client takes the bytes after the length it has. A stream that is not
the owner of the source of a band starts it again. A terminal session is not
a band: its screen goes as the deltas of the cast protocol.

The transcript store takes the live document at the input of the output
document. Its source is an append-only text of the model, as the source of a
band is. `recorded` holds the ranges that `fyai_output_append_recorded()`
added: another path, such as the band of a tool call, presented them already,
and a renderer that draws the bands does not draw them again. The document
ends finalized, aborted, or discarded when it ends without a record. The
stored exchanges stay in the arena, where a renderer reads them from the head
of the branch.

A copy of a context that renders stored output offscreen, such as the
transcript view and the branch browser, is detached from the model with
`fyai_uimodel_view_detach()`. It writes nothing, and it does not release what
the live context holds.

A set of questions belongs to the engine. `fyai_ask_open()` keeps its callback
and writes the set to the store. The input area builds its queue from the
store, keeps the cursor and the choices as view state, and gives the answer to
`fyai_ask_settle()`, which settles the callback and removes the set. When the
renderer goes, `fyai_ask_settle_all()` settles every set with no answer.

## Parallel construction

While a part of the screen moves to the model, the legacy path that made it
stays as its oracle. The renderer presents what it makes from the model, and
`fyai_uimodel_check()` compares it with what the legacy path makes:

- the header and the status, against the legacy builder of the context;
- the busy state of a turn, against `ui->busy`;
- the todo panel, against the reading of the branch entry;
- the counts of the header panel, against `fyai_tools_counts()`;
- a set of questions, against the copy that `fyai_ui_ask_oracle()` keeps.

The log has no oracle: the store and the pane read the same input.

A difference goes to the trace and, when `$FYAI_UI_MODEL_CHECK` names a file,
to that file with both texts. The test harness names a file in the sandbox of
each case and fails the case when the file is not empty, as it does for a
sanitizer report. An oracle is removed when its part of the screen no longer
has a legacy path.

## Remaining work

- The Markdown source of the tool-call and patch views, which are `rows`.
- The screens of terminal sessions as cast deltas.
- The stored exchanges for a client without the arena: a page of the records
  of each exchange, and the head that moves.
- The transcript store: the stored exchanges by key and the live document.
- A page case for a question that a sub-agent forwards: the `from` of a set
  has no case under the page.
