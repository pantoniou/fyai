# Terminal UI rules

This document holds the detailed rules of the output separation, the transcript documents, the page, the work pane, the tiles and the themes. `CLAUDE.md` gives the summary.

## Separation

`src/front/fyai_flow.c` decides what goes between two adjacent units of output: the
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
- A medium that replaces its rows sets the tail of the flow again. The
  transcript view does this when the stored exchange replaces the live rows.
  A separation must go on the same path as its unit, or it lands after it.
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

## Documents

`src/front/fyai_output.c` owns the durable part of a transcript document. This part
contains the Markdown source and its fragments. Keep one tagged document open
for each system, user, or assistant output. Keep an assistant document open
during the complete model and tool loop. Store the final document as
`display_outputs`. Replay these documents for history. Reconstruct a legacy
arena from message and provider data only as a fallback.

An interactive user line queued during a turn enters before the next model
request, after active tool calls finish. Close the assistant document, record
each consecutive queued user line, and open a new assistant document for the
continuation. A queued slash or shell command waits until the turn ends and
keeps later lines behind it. A final model response ends the turn before
pending user lines run.

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

## A window that changes width

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
- A delegated sub-agent draws no header row: the head of its tile in the
  parent names it already, and its screen is the work.
- The size is read in the pump, and a turn waits on the provider rather than on
  the terminal, so an interactive session keeps a SIGWINCH source to wake the
  display for it.

## The page

The page states the live screen as one UI Markdown page. `src/front/fyai_page.c` owns
the page source; the terminal library draws its slots.

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
- The chrome is the stack's: a blank row and the header row, the prompt on its
  card (`fytim_prompt_card()`, a slot two rows taller) or between two rules,
  and two status rows - the focus hint or the keys of the completion popup,
  then the status. The band stack reserves the blank row with `fytim_set_header_rows()`
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
- A margin at the start of a row is `<fy-space n="N"/>` (`space: N` in the
  document): Markdown removes a plain blank there, and a non-breaking space
  reaches the terminal as a different character.
- The page is one `fy-tight`: a blank row between two parts of the chrome is
  not part of it. A blank row of the chrome is `blank: true`, which is
  `<fy-space/>` on a line of its own.
- Text that the configuration or a program wrote goes into the source through
  `page_append_text()`: it is escaped, loses its SGR and its line breaks, and
  loses its leading blanks at the start of a row, where four of them make an
  indented code block.
- The renderer keeps a right margin of `markdown_gutter_cols()` columns. The
  page renderer is made that much wider, so rows and slots take the terminal
  width as the theme changes its margin. Recreate the renderer when that
  margin changes at a settled terminal width.
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
- The keys of the active cases are the keys of the library mode `page`, which
  inherits `prompt`. They are bound with `fytim_mode_bind()` when they change,
  the mode is selected while there are any, and both are cleared when the page
  renderer stops. A bound key reaches fyai as `FYTIM_EVENT_KEY`, whose action
  is the name of the key, and not the editor. A new name of the state goes
  into `data/page-state.schema.yaml`, which its test checks.
- `display/keys` binds the keys of the three modes of the library - `prompt`,
  `completion` and `surface` - to the built-in actions of libfytimui
  (`fytim_action_name()` lists them), or to "" to unbind a key. A key is a
  name, or a chord of up to four keys such as `"Escape Escape"`.
  `ui_keys_configure()` resets each mode to its defaults and binds the keys
  over them, on every configuration change; a mode that the library rejects,
  or an action it does not know, is reported and keeps its defaults.
  `display/chord_timeout_ms` is the time a chord waits. Do not hard-code a key
  of the prompt in C: the library has it as data. The keys that leave a tile
  are keys of the `surface` mode: `ui_keys_configure()` binds `Ctrl-T` to
  `fytim.focus.next` and `Ctrl-]` to the action `fyai.focus.prompt`
  (`fyai_tools_keys_to_prompt()`) before `display/keys`, so the user can move
  them. Do not scan the bytes of a surface for them. The library reports them
  in order with the bytes around them, so what is typed after one reaches the
  new owner.
- Questions to the user are a mode of the input area. `src/tools/fyai_ask.c` owns
  their model: `fyai_ask_normalize()` checks the arguments of `ask_user` and
  makes the one normalized sequence that the UI, the `user/ask` forward of a
  sub-agent, and the text fallbacks all read; `fyai_ask_result()` builds the
  structured result. Do not parse the arguments or build the result anywhere
  else. `fyai_ask_open()` puts the questions in the queue of the UI and calls its
  `done` function with one `struct fyai_ask_reply` for each question, or with
  NULL for none; the caller waits in the event loop, not in a nested loop of
  its own. `ask_user` and the questions of a sub-agent take this path when
  `fyai_ui_ask_available()` says so and no `--answer` remains. The mode is
  `ask` while nothing is typed, where the number keys choose or toggle, and
  `ask_text` once text is typed, where they type. Escape and `^C` answer
  nothing. A caller that goes away withdraws its questions with
  `fyai_ask_close()`. Agents put one set of questions at a time, so the
  page adds `fyai_agents_questions_waiting()` to the sets it says wait. The UI
  copies the text of the questions it shows; the page gets them as
  `struct fyai_page_ask` and does not read generics. A preview is rendered
  with `markdown_render()` at the width the indent leaves, kept until the
  option, the question or the width changes, and drawn in the `preview` slot.
  The option under the cursor is drawn as the completion popup draws its
  selected row: the roles `select.row` (the ground of each of its rows, to the
  edge) and `select.bar` (the bar in its first column, the glyph `select.bar`,
  shared by the label and the description of the option) of the palette theme.
  The page document names the roles; do not put a colour for them in C. A
  theme without them leaves the bar uncoloured and the row bare, so the
  selection is still marked.
- A question given `background` or a `name` is an object of the run, as a
  named wait is. `src/tools/fyai_asks.c` keeps them in `ctx->asks` with a builder of
  their own, because the turn that asked ends before the user answers. `list`
  shows them (kind `questions`), `cancel` withdraws one with
  `fyai_ask_close()`, and `wait` resolves a name to it. When the user
  answers, the registry queues `[question 'NAME' answered]` and the JSON as an
  event; a cancel by the model queues nothing, because the model has the
  result of that call. `ask_user` is an instant tool only for such a call
  (`in_parent` on its entry says so). A forked child drops the registry in
  `fyai_ctx_fork_disown()` without withdrawing the questions of the parent.
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
- A review names the areas of the page. The transcriber starts each row with
  `<fy-mark id="review:AREA"/>`, which takes no column: AREA is the flag
  that shows the row, else its page, with the list of an `each` before it
  and `/blank` after a blank row. `fyai_page_review_paint()` gives each
  mark and each slot the next colour of the palette series and its name
  at the top right, where it covers no text. `/page review`
  paints the live page; `fyai page review SAMPLE` draws a sample state
  and lists the areas. A mark is not a region of the terminal library.
- A page document states its layouts in `layouts`: the first that the
  terminal is large enough for (`min_cols`, `min_rows`) is used, and a node
  can switch on `layout.name`. The thresholds are the document's; C only
  evaluates them, in `fyai_page_layout()`, once per frame before the fit,
  because the layout decides where the pane stands before any tile is
  granted rows. A `side` pane is a column of `cols` beside the transcript,
  each tile a panel of `tile_rows`: the pane takes no rows of the page, and
  `fyai_page_side_place()` and `fyai_page_grid_lead()` write one fy-grid of
  the transcript and the tiles, because grids do not nest. A side column
  needs the fullscreen page. `display/work_panels` says when a side
  layout is taken: `auto` at the size of the document, `on` at any size
  where its column leaves the transcript room, `off` never.
  `display/work_panel_cols` and `display/work_panel_rows` replace the
  `cols` and `tile_rows` of the document when they are not 0.
  `display/page_layout` (`/layout`) names a layout that is taken at any
  size, or `auto`; a side layout that cannot stand falls back to `auto`.
  The header templates name the layout of the last frame as `{layout}`.
  The UI makes the header again when the layout changes, before the frame
  reads it: the rows it replaces are the header of that frame. While a
  column of tiles stands beside it, the transcript takes the render width
  of its own column (`side_cols` of the UI), else that of the terminal, and
  every writer of `cfg->render_width` in the UI honours it. A change of that
  width makes the rows already made again, as a resize does: a column that
  opens beside the transcript must not clip it.
- `display/screen: fullscreen`, the default, puts the page on the alternate
  screen when the session starts (`FYTIM_SCREEN_ALT`). There is no scrollback
  there, so the transcript is a view of the page, `src/front/fyai_transcript_view.c`, drawn into
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
- With `display/command_output: transcript`, the default, a slash command is
  a part of the transcript: its card is drawn, its output is drawn under the card, and
  after it runs `fyai_interactive_record_command()` appends a turn with no
  messages and one `command` display record: the line, which replays as its
  card, and the rows its output drew. The card names the command, so no
  heading stands over them. The model never sees the turn. A command that
  moved the head or changed the branch is recorded by that operation, and a
  command beside a turn, or in a session that holds no exchange yet, is not
  stored. A command turn goes with the exchange before it in a merge or a
  rebase, and `~N` counts it.
- The output stands on a ground of its own, `markdown_command_ground()`:
  `display/command_bg` as given, or with `theme` `display/command_bg_mix`
  percent of the `pane.focus` wash over the ground under the text - the
  theme's on a fullscreen page with a theme ground, else the background the
  probe measured. `markdown_ground_rows()` puts the rows on it when they are
  presented, live and on replay; the record keeps them without it, so a
  replay takes the settings of its own session.
- The test harness selects `pane`, which most cases read; a case of the
  transcript selects `transcript`.
- `config reset` stores an empty configuration and drops the session layer;
  `config undo N` stores the configuration of N changes ago, walking the ref
  log and counting only entries whose configuration differs. Both leave the
  replaced configuration in the ref log.
- With `pane`, the transcript keeps nothing of a slash command:
  no card is drawn and nothing is stored. Its output, and every notice and
  diagnostic, goes to the pane. `fyai_ui_pane_end()` shows a
  result of two rows or less above the status, until the input changes or
  Escape clears it. A longer one opens the log small: the `log_panel` page
  above the status, a heading and at most `UI_LOG_ROWS` rows, which the fit
  holds to a third of the page and never takes from the input. A click on its
  heading or on `[expand]` (`popup.expand`) expands it to a popup that covers
  the page - the transcript, the tiles and the chrome - under a heading that
  names it; `[x]` and Escape hide the small log (`popup.hide`). The popup is
  the `open` case of the `popup.mode` switch of the page, and its keys are the
  page's: Escape, Enter and `q` collapse it to the small log (`popup.close`),
  Up and Down scroll it a row (`popup.scroll`), `l` raises the lowest level it
  shows (`popup.level`) and `x` clears the log (`popup.clear`). PageUp,
  PageDown and the wheel scroll it too.
- The popup shows the log of the session. Each result, notice and
  diagnostic that goes to the pane is an entry with a level - info, notice,
  warning or error, from the highest severity that the drain reported - and
  the log keeps the last `UI_LOG_MAX` entries. The popup is a transcript view
  model made again from the entries at or above the level, with the newest
  from its heading; a drag over it copies the text of its rows. Hiding the
  popup keeps the log. The header panel shows its count with the colour of the
  worst level, and a click on it, or a key bound to `fyai.messages.toggle` in
  `display/keys`, steps the log from hidden to small, to expanded and back to
  hidden. A diagnostic that the pane shows
  has no module prefix: `fyai_diag_set_bare()` holds for the span of
  `fyai_ui_pane_begin()` to `fyai_ui_pane_end()`, and is process-wide because
  a configuration change replaces the sink in the middle of a command.
- A popup hides the tiles, so the page gives them no grant while it is open:
  a grant of no rows would size every program to nothing.
- A cell of the page keeps the OSC 8 link it was drawn with, so a URL of
  the transcript stays a link on the alternate screen. Draw page text with
  `page_cells_text()`, which uses the link table of the canvas; the page
  clears that table when it draws every row again. A cell copied from a tile
  surface names the table of that surface: `page_cells_relink()` gives it the
  id of the canvas.
- A drag over the transcript view is a selection of its text region. Its
  text is the text of the rows in its cells, without styles, links and the
  blanks that end a row (`fyai_transcript_view_copy()`), and fyai copies it
  with `fytim_copy()`. When a fullscreen session ends it closes the UI,
  which gives the terminal its own screen back, and prints nothing there.
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
- Tab completion opens a popup of libfytimui above the prompt. It is a layer:
  it covers the rows above the prompt and moves nothing. Each row is the word
  a candidate puts in the line and its title, from `fyai_session_completion()`,
  which anchors the popup at the word with `fytim_completion_set_anchor()`.
  The popup takes the page ground (`FYTIM_CHROME_POPUP`). Its other styles
  come from roles of the theme through `ui_popup_style()`: the frame from
  `popup.border`, else `chrome.rule`; the selected row from `popup.selected`,
  else `pane.focus`, else the card; the typed part of a label from
  `popup.match`, else `prompt`; and the `pane.edge` mark before the selected
  label. Do not put a colour for them in C. Up, Down, Tab and Shift-Tab
  select, and Tab wraps; Enter takes the selection. `display/completion: auto` opens the popup as
  the line is typed, with no Tab (`fytim_set_completion_auto()`).
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
  the band stack renders it, so a long row is cut in the same place.
- The title row of a tile is one row: a title that does not fit beside the
  buttons and the right-hand text loses its end to an ellipsis, or it would
  wrap and take the buttons to a row of their own.
- `tests/cases/ui_page_tiles.sh` compares two tiles side by side under both
  renderers.
- Under the page renderer the head of a shell or agent tile is a tile page
  (`fytim_surface_set_page()`): the rendered head, then a `screen` slot, built
  by `fyai_page_tile()`. `FYTIM_EVENT_ACT` routes its acts, `tile:focus`
  and the buttons, as the band stack routes a click on its head.
- The ladder of a tile selects the view of its page, where the manager
  reports the presentation: the whole page, the screen alone
  (`FYAI_WORKPANE_PRESENT_OUTPUT`) or the head alone. A view changes what is
  drawn, never what a tile asks for or is granted: the presentation is chosen
  from the grant.
- A page that cannot be built, rendered or accepted is a display failure: the
  session reports it, clears the page and the band stack draws the screen. A
  build without page support says so when `page` is asked for.
- `$FYAI_TRACE` records one `page:` line for each render.

## Work bands

A work band is a sink object. Ask `fyai_sink_bands_available()` before you
choose a banded presentation, open with `fyai_sink_band_open()`, repaint with
`fyai_sink_band_paint()`, and commit the shared band with
`fyai_sink_band_close()`. Do not call `fyai_ui_*` band functions from a
producer; the terminal backend owns that.

## Tile chrome

`src/front/fyai_chrome.c` decides the chrome of every tile of the work pane. An
owner states the content of the head with `struct fyai_chrome_spec` and calls
`fyai_chrome_update()`. It does not build a head, a button, or a column of its
own.

- `fyai_chrome_items()` is the one policy: the tile kind, its phase (running,
  kept, committed), and `display/work_controls` give a mask of
  `FYAI_CHROME_BUTTONS`, `FYAI_CHROME_SCROLL`, and `FYAI_CHROME_WHEEL`. Do not
  read `display/work_controls` anywhere else; use `fyai_chrome_level()`.
- A committed tile has no buttons: a button there acts on nothing. Its head is
  also the top of the rows that the surface commit writes, so the transcript
  keeps the title row.
- A tile of text scrolls too. The manager keeps `scrolled` and `scroll_top` for
  it (`fyai_workpane_band_scroll()`): a tile scrolled back keeps the first row
  it shows while the content grows, until it is back at the end, or until the
  user types (`fyai_workpane_bands_follow()`, called when the line being typed
  changes). The page draws the bar only when the content outgrows the rows, and
  `fyai_chrome_bar_part()` is the one geometry of a bar of text and of a screen.
  The live stream of a tool call renders `display/tool_history_lines` rows and
  the band shows `display/tool_preview_lines` of them. The stored exchange keeps
  the preview, so history is a property of the live view only.
- `fyai_chrome_frame()` divides a row between margin, scroll bar, and body.
  The head, the screen, and the scroll-bar acts of the page all use it, so
  they act on the same columns. The page reads `items` from the tile, not a
  page-wide flag.
- `fyai_chrome_body()` gives a tile its unfocused look when an owner opens
  it, and `fyai_chrome_focus()` is the one place that changes it when the keys
  move: the ground, the edge marker or reversed margin, and the status hint.
  The theme primitives stay in `fyai_ui.c`; the policy of which tile gets
  which hint is the chrome's.
- `fyai_chrome_panel()` builds the panel of the input header: the pane button
  and the counts of live user shells, model shells, and sub-agents.
  `fyai_ui_panel_update()` only stores it. A new kind of live work adds a row to
  the table of that function.
- `fyai_chrome_cap_source()` writes the cap row from a summary that the
  manager gathers: height, tiles, hidden tiles, and zoom. The manager owns the
  facts; the chrome owns the text and the glyphs.
- A new kind of tile gets its chrome by a line in the policy, not by a draw
  path. `tests/fyai_chrome_test.c` states the table.
- The display advances the activity mark for each live shell or agent. A
  remote worker sends work state, not animation frames. The client draws
  the mark even when Markdown is disabled.

## The work pane

Every live shell session and sub-agent screen is a tile of the one work pane,
not a band of its own. A stack of full-width screens pushes the oldest off the
top and says nothing about which screen belongs to which call; one region that
tiles them says both.

`src/front/fyai_workpane.c` owns the pane. It is the one component that writes a
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
- A band has no width before the frame that lays it out. The sink band holds
  a paint made before that frame and presents it when the first grant
  arrives, or when the band commits. The frame after registration is then
  the same whether or not the first output line arrived before it. The
  manager reports each new width to the owner of a band through
  `repaint_head`. A producer that wraps its own rows sets
  `fyai_sink_band_set_repaint()` and renders them again there.
- The cap counts the tiles of the reconciled state. `fyai_workpane_reconcile()`
  writes it, so a frame drawn after a tile registers counts that tile.
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
- The pane never makes the page taller than the terminal. The page first
  gives the pane the rows that the chrome leaves (`fyai_workpane_set_room()`).
  The tiles then take the highest level of detail that fits them all: the
  size each asks for; else their head over `display/tile_live_rows` of live
  output; else one line each in the Agents or the Shells panel, where only
  the tile with the focus stays a tile. Do not squeeze each tile to fewer
  rows to keep it in the grid: a row of output says less than a line in a
  panel.
- The calls that `display/tool_display: inline` draws in blocks of the
  transcript follow the same levels. `ui_live_level()` keeps a record of
  each running block and sizes it to the rows the chrome of the last frame
  leaves the transcript: its whole output, else its head over
  `display/tile_live_rows`, else the block leaves the transcript and takes a
  line in the panel. A block that ends is presented whole.
- A tile that leaves the grid overflows. Its panel line is its state mark,
  the first line of the title of its head
  (`fyai_workpane_tile_set_summary()`, called by `fyai_chrome_update()`),
  and the last line of its program (`last_line` of its tile operations), cut
  to one row. The panels stand after todo and fleet, take at most a quarter
  of the page, and count the lines they leave out. The cap counts only the
  tiles that the user hid.
- `display/focus_bg` is the ground of what holds the keys - the prompt, a
  picker and a focused tile - and the one place focus is said. `theme`, the
  default, is the focus wash of the palette theme: the `pane.focus` role of
  libfypalette, resolved by `markdown_focus_ground()`. The theme makes it for
  the variant of the terminal, so its own text reads on it on a light
  terminal and a dark one. Do not put that colour in C. Without a palette,
  `theme` is `reverse`. `reverse` is the ground the terminal draws text in,
  named rather than given: it asks for no colour the terminal may not have,
  but reverse video turns text of a colour into a ground of that colour. A
  `#rrggbb` is a ground of your own, which asks for no detection and is thus
  the one display setting that can leave the input area unreadable:
  `fyai_config_focus_bg_check()` reports a literal the text of the resolved
  variant cannot be read on, one time for each value, and the value stands. The prompt stands on the same ground, because that is where
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
- `display/focus_mark: edge`, the default, drops the wash: `ui_focus_ground()` gives the
  prompt, a picker and a focused tile the ground of the terminal, and the
  edge marker alone says where the keys are. The margin is not reversed.
- Keyboard focus also uses a one-column edge marker because the background
  wash can be hard to see. Use the palette's `pane.edge` role, or the theme's
  strong style (`ui_edge()`). For a tile, replace the first blank margin
  column (`ui_focus_margin()`). For the prompt, use the reserved column before
  its marker (`fytim_set_prompt_edge()`). Keep that column blank when a tile
  is focused so changing focus does not move content.
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
- The head of a tile is UI Markdown. `fyai_chrome_update()`
  escapes the title with `markdown_ui_escape()` - it holds what a model or a
  program wrote - and makes it the `tile:focus` label, so a click on the name
  gives the tile the keys. The manager keeps the regions of the head with the
  tile (`fyai_workpane_tile_set_regions()`) and frees them with it. A click
  reaches fyai only when the mouse is grabbed, which `display/work_controls`
  decides. Never render chrome with UI Markdown without escaping text that
  fyai did not write.
- `display/work_controls` draws mouse affordances on a tile. `full` is the
  default. Anything but `none` grabs the mouse for the whole session: selection
  and copy are then fyai's, which copies through the terminal, and paste stays
  the terminal's. The library reports a control as
  an event and acts on nothing itself: `fyai_tools_surface_request()` asks the
  component that started the program, because only it can end it.
- Clicking a tile outside its controls focuses it
  (`FYTIM_EVENT_SURFACE_FOCUS`). Clicking outside every tile focuses the
  prompt (`FYTIM_EVENT_FOCUS_PROMPT`). The
  page draws its tiles itself, so a click on its `head:N` or `screen:N` slot
  arrives as the second event with the slot named, and
  `ui_click_off_tiles()` resolves the tile from that slot. Clicking changes
  only focus and preserves zoom, as does `Ctrl-T`.
- `display/work_controls: full` gives each screen tile a scroll bar. The
  terminal view keeps the rows that left the top of the screen with their
  cells, a history of `display/work_history_rows`, and an offset into it; the
  program is not told. `fyai_tools_config_changed()` gives a changed size to
  the views that are open.
  `fyai_ui_surface_publish()` states the extent on the surface, and the bar
  is drawn from it: by the library under the band stack, and by the page
  with `fytim_cells_draw_scroll_bar()`, whose `tile:N:scroll-up`,
  `scroll-down`, `page-up` and `page-down` actions cover its cells. Scroll
  events, including the wheel over a tile, reach the owner through
  `fyai_tools_surface_request()`. A view scrolled back stays on its rows
  while the program writes, and what the user types shows the live screen
  again. A program on the alternate screen has no history.
- What the user acts on in the chrome of a tile - the arrows and the thumb of
  the bar, and the panel button of the header - has the colour of the
  buttons of the head, the text colour. It takes the `text` role of a
  palette theme, else the strong style of the theme, through
  `FYTIM_CHROME_CONTROL` under the band stack and `control_chrome` on the
  page. The track stays dim chrome.
- The head of a tile ends in three buttons when the mouse is grabbed:
  `tile:minimize`, `tile:maximize` and `tile:close`. They are acts of the
  head source that `ui_tile_buttons()` writes, so both renderers draw and
  route them from one source: a click reaches `ui_tile_act()` through
  `FYTIM_EVENT_SURFACE_CLICK` under the band stack and `FYTIM_EVENT_ACT` on
  the page. Their regions are regions of the head, so a head is made again
  at every new width (`repaint_head`), or a click finds a button where the
  old width put it.
- Maximize toggles zoom. In a fullscreen session it also gives the tile the
  whole page (`pane.full` of the page state, `ui->tile_full`): the
  `tile_full` page draws the grid of the pane alone, the fit gives it every
  row but a hint row, and the tile has the keys. The page is back when the
  maximize button is used again, on `Ctrl-]`, or when the tile is no longer
  zoomed. The expand, collapse and close buttons of the log use the glyphs of
  these tile buttons (`tile.maximize`, `tile.minimize`, `tile.close`), through
  `glyphs` of the page state. Minimize is manager state
  (`fyai_workpane_set_minimized()`). A minimized tile renders only its header
  in a fitted row below the screens through `fytim_surface_set_collapsed()`.
  It cannot be focused or zoomed. Its program keeps its current size because
  the manager does not apply a new grant. Clicking its header restores and
  focuses it.
- The input header ends in a panel when the pane holds live work: a button
  that hides and shows the whole pane, then the live shells of the user,
  the live shells of the model and the running sub-agents, each only when
  there is one. `fyai_ui_panel_update()` writes it after every reconcile,
  and when the last tile takes the pane with it: a reconcile with no pane
  does nothing else, and a panel left from before counts a shell that ended.
  The band stack draws it with `fytim_set_header_right()`, and the page
  draws it in the `header` slot with the button as an act of the page; both
  route `panel:pane` through `ui_act()`. Its glyphs are glyphs of the theme
  and its colours are roles of the palette, else styles of the theme of the
  chrome: never a colour escape in C.
- A hidden pane (`fyai_workpane_set_hidden()`) keeps its tiles, grants
  nothing new and takes no keys. A tile given the keys shows the pane again.
- `Ctrl-T` and `Ctrl-Tab` cycle through what takes the keys in the order the
  screen shows it, from the top row down and from left to right in a row,
  then the prompt: tiles and the terminal blocks of the transcript alike.
  `fyai_tools_focus_next()` sorts them by where the last frame drew them
  (`fyai_ui_tile_pos()`, `fyai_ui_inline_pos()`); a place the page did not
  draw goes last. `fyai_workpane_screen_order()` orders the tiles from the
  placement, so a policy of the user's decides it too. A hidden tile takes no keys, and a
  zoomed tile is the whole cycle.
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
  this program keeps for itself - `Ctrl-Tab`, `Ctrl-T`, and `Ctrl-]` - is a
  key of the `surface` mode, which the library reports as an event between the
  bytes before it and the bytes after it, so the bytes after it go to whoever
  holds the keys once the key is acted on. A handler that stops taking keys
  for another reason - a closed browser, a finished action - gives the rest of
  its chunk back with `fyai_ui_keys_return()`, which puts it in front of the
  input the terminal has sent since, to be read again and routed then.
  Dropping it loses what the user typed, and a paste or a fast typist makes
  one chunk of the key and the line under it.
- Look a tile's owner up by surface. A program that ended must not leave a
  pointer behind for the next keystroke to follow.

## A live terminal that is resized

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

## Inline tool display

`display/tool_display: inline`, the default, draws the live output of a call
in the transcript, at the call, in place of a tile of the work pane. The test
harness selects `pane`, which most cases read.
`fyai_ui_tools_inline()` is the one decision: it applies to a page, and a
display without one uses the pane.
`doc/inline-tool-display-plan.md` gives the design and the steps that remain.

- A live call is a block of the transcript view
  (`fyai_transcript_view_block_set()`): after the live rows, before the
  tail, in the order the calls opened. A view scrolled back keeps its top
  row while a block changes.
- Producers keep the band interface. The terminal backend of the sink puts
  an independent band in a block, and the UI puts the shared tool band
  there. Both render the rows that a tile shows.
- A live block takes the separation that the flow would draw, and changes
  no flow state. Its commit drops the block and presents the band as a tile
  commit does, so the stored exchange replaces the same rows.
- A block shows the last `display/tool_preview_lines` rows. The transcript
  scrolls it, so the stream keeps no history for it, and the band opens at
  once: `display/work_open_delay_ms` hides a jump that a block does not make.
- `fyai_transcript_view_show_block()` scrolls a block into the view.
- An inline page has no transcript view: the terminal keeps the committed
  rows. The UI keeps its blocks in a view of their own (`ui_blocks()`), and
  the page draws them under the tail rows, which count them. When they do
  not fit, the last rows stay. A row committed while a block runs goes into
  the scrollback above it.
- A terminal block, a session or a sub-agent, has
  `display/inline_terminal_rows` rows and the width of the transcript less
  the indent of tool output, and follows that width. Its mark blinks on the
  timer of a tile.
- A block of text is rendered again at a new width through the repaint of its
  band: the sink keeps its bands drawn in the transcript, and the UI calls
  `fyai_sink_bands_reflow()` when the live rows reflow.
- `Ctrl-T` takes the terminal blocks in the order of the screen with the
  tiles. A block has no surface, so the canvas of the page takes the keys
  for it (`fyai_ui_inline_keys()`), with a callback of its own: the pane
  clears the callback of a tile when it moves the keys. The block draws the
  edge in its first indent column, a fullscreen view scrolls to it, `^C` goes
  to its program, and `Ctrl-]` gives the keys back. A block that commits gives
  them back too.

## Bang commands

A line that starts with `!` is a command of the user. It is not a model turn.
`fyai_tools_bang()` starts it as a terminal session in the work pane and gives
that tile the keys. The prompt returns when the program ends. A bang line needs
an interactive terminal UI; without one, report that and do not run the
command.

- A bang session is user-owned. The model cannot close it and cannot read its
  output as a tool result. A tool call keeps the output limit of a shell tool.
  A user-owned session has no such limit: the user reads its screen. It is
  not watched for input waits: the model is not told that it waits, because
  that would give the model its output and start a turn.
- A bang command is not a part of the conversation. Its line is not drawn as
  a card, and its session records nothing and commits nothing: not
  its screen, not its output, whether or not a turn runs beside it. When the
  program ends, its tile stays with the outcome, its buttons and the keys it
  held, and the status row says that Escape closes it, until the user
  dismisses it with Escape or `q` in the tile, its close button, or `/kill`.
  The close button of a running bang tile ends its program and the tile with
  it; `/kill` ends the program and leaves the tile to read. A full-screen program - one that entered the
  alternate screen - leaves nothing to read, and its tile goes at once.
- A program that prints once and ends, such as `ls`, reads the size of its
  terminal when it starts; `ls` takes the terminal size over `COLUMNS`. The
  tile of a terminal session is registered before its program starts, so
  `fyai_ui_layout_now()` solves the page then, and `tool/run` carries the
  grant as `size`: the terminal opens at the size of its tile. A size the
  call asks for wins. `COLUMNS` and `LINES` stay unset, because a resize
  cannot change the environment of a running program.
- A user-owned tile can take more rows than the shared pane granted it. A tile
  of a tool call keeps the grant.
- `display/work_zoom_rows` is the height of the work pane. `full` uses the
  terminal height, `half`, the default, and `quarter` use that fraction of it,
  and an integer is a direct row count. It is the pane's disposition, not a mode that
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

## Palette themes

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
- fyai asks the terminal what it supports one time, with the libfypalette
  probe, through `fyai_terminal_probe()`. The configuration keeps the result
  (`cfg->terminal`) and the keys typed during the probe. A copy of the
  configuration keeps the result and does not probe. The probe sends all its
  queries in one write, with DA1 last, and waits for the DA1 reply. Every
  terminal answers DA1, and answers in order, so a query with no reply before
  it is not supported. Do not query the terminal anywhere else - libfytimui
  measures a glyph, below, and is the one exception - and do not decide
  anything from how long a reply takes. The time limit is 1000 ms;
  `$FYPAL_PROBE_TIMEOUT_MS` changes it. The test harness sets 30000, which a
  driver that answers never reaches.
- Terminals draw some glyphs at different widths: VTE draws an emoji base
  with U+FE0F in one column, most terminals in two. libfytimui measures such
  a glyph while the program runs, not in the probe: the first time it writes
  one, it asks for the cursor after it, places the next cell absolutely, and
  records the advance the reply gives. A session that never shows one never
  asks. A narrower measure repaints the cells of the library and raises
  `FYTIM_EVENT_GLYPH_WIDTH`. The UI gives libfymd4c the same measures with
  `fymd_set_glyph_width()`, so Markdown lays out a table as the cells draw
  it, and on the event it makes its rows again as a width change does,
  forgetting the rows of the fullscreen view
  (`fyai_transcript_view_invalidate()`). A PTY case plays VTE with
  `emoji_vs: narrow` in its script; the driver answers the cursor query from
  its terminal model.
- `fyai_ui_open()` gives the result to the UI. The capabilities that the
  terminal reported replace the guess from the environment
  (`fytim_set_caps()`), and the keys typed during the probe go to
  `fytim_keys_return()`, before anything the terminal sends later. A reply
  that arrives after the time limit is dropped by the libfytimui input
  parser. A sub-agent and the branch browser preview do not probe.
- `display/theme_ground=terminal`, the default, makes the background of the
  terminal the ground of the palette with `fypal_ctx_set_ground()`, so the neutral ramp of
  the theme keeps its steps over that background. The theme names the ground
  and decides what follows it; fyai holds no colour. The background comes
  from the probe. A background of the other variant is not applied. A PTY
  case answers the query with `background:` in its script.
- A theme of the `auto` variant takes its variant from the terminal's measured
  background when available. Without a background reply, it uses
  `fypal_term_variant()`: the scheme the terminal reported, then
  `$COLORFGBG`, else dark. The probe runs one
  time, so a configuration change during the session gets the same variant,
  and the colours of the conversation on the screen do not change. A PTY case
  stops answering OSC 11 when the session is started
  (`background_while_starting:` in its script).
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


## Streams

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

## Backends

A backend supplies the presentation policy for `struct fyai_sink_ops`. The
terminal backend repaints in place. It owns the only progressive Markdown
renderer in the process. The capture backend keeps the requested content. A
document backend, such as HTML, uses the capture backend. Tests also use it to
read the output of a run. Each entry point can be NULL. If a backend cannot
present content, the sink discards the content. A producer does not examine the
destination.

While the terminal UI is active, the terminal backend presents the transcript
and notice streams through `fyai_ui_present()`, never through standard output.
The UI keeps what a pane captures in a buffer of its own. Standard output is
spooled only to catch a stray writer. The scrollback takes whole rows: the UI
holds a row that has not ended until its newline arrives, or until the next
drain.

## Tables

Render every Markdown table with `fyai_generic_to_markdown()`. Pass a
`renderopts` generic for titles, selected keys, names, alignment, and formats.
Build `renderopts` in the transient builder or in the same stack frame as the
render call. Do not return a builder-less `fy_mapping()` from a helper because
it uses stack storage. `stats` keeps its own table builder: its cells carry
currency and percent formatting that `renderopts` does not express.

Export is deliberately outside the sink. `fyai_export_view()` serializes
canonical messages to a `FILE *` in the `format: 1` textual grammar that
`src/store/fyai_import.c` reads back, and it must keep working when a conversation
has no stored `display_outputs`.

## Interactive rules

The interactive reader uses the shared event loop. Test terminal input,
signals, and resize behavior under a PTY. Non-TTY functional tests use the
blocking fallback and do not cover the event-driven path.

In the REPL, `/` starts a slash command and `//` sends a literal slash. Ctrl-C
discards nonempty input, ends idle input, or cancels an active turn. Escape and
SIGINT call `fyai_ui_interrupt()`. Keep `ISIG` enabled.

`/resume`, and its alias `/switch`, select another session for the running
invocation: by name, or through the resume picker. HEAD does not move. In a
session, Escape in the picker returns to the session; only the picker of the
`resume` verb ends the invocation.

`/checkout <branch>` selects an existing branch and moves stored HEAD.
`/checkout <branch>@{N}` and other historical references create a new
`session/` branch at that state and check it out. `/checkout -b <name> <ref>`
uses an explicit new branch name. `/reset <ref>` and `/rewind <ref>` move the
current branch head to a reference; its previous entry remains in the reflog.

`/btw <question>` asks a side question while a turn runs. It forks a
sub-agent from the current head onto a child branch `agent:btw-N` and does not
change the conversation of the parent. Its screen is a tile of the work pane.
The finished answer stays in that tile until the user closes it with Escape,
the close button, or `/kill`. In the focused tile, arrow keys and PgUp and PgDn
scroll the answer; one chunk of input can hold several of these keys.

Request-shaping slash settings persist through the common commit path. Display
settings remain session-only. `--new` has the same state effect as `/clear`.
`/compact` makes one tools-disabled summary request and stores the previous
head as `compacted_from`.

