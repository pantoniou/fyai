# Plan: the screen as one UI Markdown page

This plan adds a second renderer to fyai. It describes the screen as one UI
Markdown page. A page holds slots. A component draws a slot, and a component
can be a page again. The live transcript, the work pane, every tile and the
prompt are slots.

The current renderer stays. `display/renderer` selects one of them, and the
current renderer is the default until the page renderer is equal to it or
better.

The page renderer has two screen modes:

- **inline**: the page is the live region under the native scrollback, as
  the current renderer does it. Committed rows go to the terminal scrollback.
- **fullscreen**: the page is the whole alternate screen. The transcript is a
  slot that scrolls inside the program. Nothing is committed to the terminal
  scrollback.

## 1. The state now

The live region has three owners of geometry:

- `draw_band()` in `libfytimui.c` places a fixed stack: the tail, the work
  bands, the header, two rules, the prompt, two status rows, and a pane
  below the prompt. `fytim_layout_compute_ex()` sheds rows in a fixed order.
- `draw_tile()` and `draw_pane_grid()` place a tile head, the tile marks, the
  scroll bar and the screen. The grid solver is `tracks_solve()`.
- fyai writes chrome as SGR strings: `fytim_set_header()`,
  `fytim_set_status_row()`, `fytim_set_marker()`, `fytim_*_set_top()`.

UI Markdown is used in two places: the tile head label
(`<fy-act id="tile:focus">`) and the pane cap row. Both are strings in a
chrome slot of the library. The library does not know the regions: fyai keeps
them with the tile and maps a click back itself.

libfymd4c already has what a page needs: `fy-fill`, `fy-act`, `fy-role`,
`fy-glyph`, `fy-columns`, `fy-vfill`, `fy-scroll`, and inline and block
`fy-slot` with weights and minimums, a page height, and a region list.

`docs/decisions/0003` of libfytimui describes an alt-screen design that the
library does not implement. The library draws inline. The design replaces that
decision with one that records both modes.

## 2. The model

```text
page (height = screen rows or live rows, width = terminal)
 ├─ slot transcript    inline: the progressive tail; fullscreen: a viewport
 ├─ slot pane          work pane, elastic
 │   └─ tile page      head Markdown + fy-act controls
 │       └─ slot screen   a surface, or a band of styled rows
 ├─ chrome rows        header, rules, status: Markdown of fyai
 ├─ slot prompt        the editor, elastic with min 1
 └─ slot completion    the ribbon, while completion is active
```

Rules:

- **One source per frame.** fyai builds the page source after
  `fyai_workpane_reconcile()` and before the frame. It renders the source at
  the screen size with `FYMD_RF_UI` and a page height. Section 4 decides how
  much of it is rendered again for each frame.
- **The host writes the page; the library composes it.** fyai owns the
  source and the render. libfytimui receives the rendered rows and the region
  list, draws the rows, and draws each slot region with the component that
  the slot id names. The library keeps the editor, key routing, grounds, the
  retained surfaces and the diff to the terminal.
- **A slot region is a grant.** The rows and columns of a slot region are what
  its component was given. They go to the owner through `apply_grant`, as
  tile grants do now. A grant never becomes the next request.
- **Preference is explicit.** A slot height in the source comes from the
  state of its component: the rows of the tail, the preference that
  `fyai_workpane_register()` recorded, the prompt lines. Content that was
  drawn does not change the preference.
- **Recursion is by renderer.** libfymd4c rejects a render from inside a slot
  renderer of the same renderer. Each nesting level has its own renderer.
  Inner regions are offset by the slot region into page coordinates, and the
  id is prefixed with the path of the slot (`pane/t3/tile:focus`).
- **Escape what fyai did not write.** A title, a command, a branch name, a
  model name: each goes through `markdown_ui_escape()`. The transcript slot is
  rendered without `FYMD_RF_UI`. Model output never places a slot or an act.
- **An act is an event.** The library reports a click on an `fy-act` region
  as `FYTIM_EVENT_ACT` with the full id. It acts on nothing itself. fyai
  dispatches by id prefix to the component that owns the work. Each act also
  has a key, so a mouse grab stays optional.
- **The current renderer is not changed.** The page renderer adds API to
  libfytimui and a new path in fyai. It does not replace a call of the
  current path. Both renderers go through the sink and the flow manager.

## 3. The page configuration

The page is display configuration: `display/page`, validated against the
schema, and changed for a session with the display slash commands. It is
never taken from a model.

The layout is dynamic. The tiles of the pane change when a shell, a
sub-agent or a bang session starts or ends, and the prompt lines change while
the user types. Thus the configuration does not list tiles. It states the
skeleton, and fyai generates the parts that follow the state:

```markdown
<fy-slot id="transcript" height="*" min="1"/>

<fy-slot id="pane" drop="2"/>

<fy-role name="chrome">{branch} · {model}</fy-role><fy-fill/>{elapsed}

<fy-slot id="prompt" min="1"/>

<fy-role name="chrome">{status}</fy-role>
```

- A `{name}` variable is plain text. It is expanded as in
  `fyai_prompt_expand()` and escaped with `markdown_ui_escape()`.
- A generated slot (`pane`, `prompt`, `completion`, `transcript`) is expanded
  by its owner for each change of state. The workpane manager writes the grid
  of the `pane` slot and one tile page for each tile, from the tiles that are
  registered now and from `display/work_layout`. A configured page places the
  pane; it does not arrange the tiles.
- `display/page` has a default for each mode, embedded as `data/page.md` and
  `data/page-fullscreen.md`. A page that does not render, or that has no
  `prompt` slot while a prompt is enabled, is rejected with the render
  diagnostic, and the default page is used.
- `display/prompt_top` and `display/prompt_bottom` stay valid. The default
  pages place them.

## 4. Immediate or retained

libfytimui draws in immediate mode: each frame draws every cell, and the core
diffs the cell buffer against the terminal. A page render is more expensive
than the draw calls that `draw_band()` makes now. Decide from measurements,
not from preference.

Candidates:

- **Immediate.** Render the whole page source for each frame, tile pages
  included. Simplest; no invalidation.
- **Retained page.** Keep the rendered rows and regions of each page node
  (the root, each tile page, each generated fragment) with the key
  (source hash, width, height). Render a node again only when its key
  changes. Draw the retained rows in the immediate frame. Slot contents
  (surfaces, the tail, the editor) are drawn in immediate mode in both
  candidates.
- **Retained cells.** Keep the cells of a node in the retained cell form of
  libfytimui decision `0005`, and copy them into the frame.

The measurement:

- A benchmark in `tests/` that drives a page with 0, 1, 4, 8 and 16 tiles, a
  streaming transcript, and a typing prompt, at 80x24, 200x60 and 400x120.
- For each candidate: render time per frame, draw time per frame, frames per
  second under a 60 Hz output stream from every tile, allocations per frame,
  and the time from a resize to a stable screen.
- The same numbers for the current renderer as the reference.
- The rule: pick immediate if its 99th percentile frame time at 200x60 with
  8 tiles is below 4 ms; else the retained candidate with the lower time.
  Record the numbers in the libfytimui decision.

Result (libfytimui decision `0007`): **immediate**. At 200x60 with 8 tiles
the 99th percentile frame is 0.33 ms; at 400x120 with 16 tiles it is below
1 ms. The Markdown render is at most 0.16 ms of a frame; the compose, diff and
write of the library is the larger part, and a retained page does not change
it. Measure again for `fy-grid` tile pages, a palette, and the full-screen
viewport.

## 5. The two screen modes

### inline

The page height is the live rows, as the layout solves them now. The
transcript slot holds the progressive tail; frozen rows are committed with
`fytim_commit()`. A width change repaints committed rows from storage through
`fyai_display_repaint()`, as it does now.

### fullscreen

The page takes the alternate screen. The transcript slot is a viewport over
the stored transcript:

- The viewport draws from the stored `display_outputs` through the one
  fragment walk, `struct fyai_fragment_ops`, and from the open document for
  the live part. It is not a second transcript renderer: it renders the same
  source with the same renderer, at the width of the slot.
- A width change renders the visible exchanges again. There is no committed
  row to repaint and no screen clear.
- Scroll, selection and copy belong to the program: the mouse wheel and
  PageUp/PageDown scroll the viewport, a drag selects, and OSC 52 copies.
  OSC 52 is the only copy path; there is no key that releases the mouse to
  the terminal. A new row keeps the view at the bottom unless the user
  scrolled up.
- The viewport is the scrollback. It reaches back through the whole stored
  transcript, not only the rows of this invocation.
- The scrollback reflows. Rows are made at the width of the slot, so a width
  change makes every row again, above the view included. The view keeps its
  position by an anchor: the exchange, the fragment and the row offset in the
  fragment at the top of the view. After the reflow, the view starts at the
  row that the anchor names at the new width. A row count from the top of
  the transcript is not an anchor: it names other content after a reflow.
- The viewport does not render the whole transcript for a reflow. It renders
  the exchanges that the view shows and measures the others through the
  measuring pass of `struct fyai_fragment_ops`, with a row count cached per
  exchange and width. A scroll into an exchange that has no count at the
  current width measures it then.
- Separation is the flow policy of `src/fyai_flow.c` without change. The top
  edge of the view clips rows; it is not a unit boundary and takes no
  separation of its own.
- Search is an act of the page (`transcript:search`).
- When the program ends, it leaves the alternate screen. The primary screen
  then shows what it showed before. Print the last exchange to the primary
  screen through the sink, so that the answer stays after the exit.
- A terminal session tile in fullscreen mode is the same surface as in inline
  mode. The pane owns its grid in both modes.

`display/screen` selects `inline` or `fullscreen`. It applies only when
`display/renderer` is `page`.

### Sub-agents

The page is recursive through the process tree. A directly delegated
sub-agent renders its own page on its own terminal, and the parent shows that
terminal in a tile, as it does now. The child uses the page renderer when the
parent does. The screen of the child is its grant: the rows and columns of
its tile.

A tile is smaller than a terminal, so the child condenses what it shows. It
does not show a smaller copy of the parent page:

- The child selects a page for its size. `display/page` holds a ladder of
  pages with the least size of each (`full`, `compact`, `summary`, `mark`).
  The child uses the largest page that fits its grant, and selects again at
  each `tty/resize`.
- `full` is the page of a terminal: transcript, pane and status. `compact`
  drops the chrome rows and shows the transcript and a one-row pane summary.
  `summary` shows the last rows of the answer and the state of the current
  tool call. `mark` is one row: the name, the state mark and the elapsed
  time.
- Within one page, `drop` removes blocks in order before the page changes.
- The grandchildren of the child are tiles of the pane of the child. On a
  `compact` page they are the summary row; deeper work is a descendant count,
  as it is now.
- The state of the current tool call is a generated slot, `call`, with a
  generator of its own in the child. It writes the title row through
  `markdown_render_tool_head()`, the state mark, the elapsed time, and the
  last output rows that fit. Page variables do not carry it: a variable is
  plain text and cannot hold a mark, a role or rows.

### The agent summary

When there is no room for a tile of each agent, all agents collapse into one
display: the `agents` slot. It is the last rung of the parent ladder, below
the tiles of the sub-agents, and it takes one row:

```text
agents  3 active · 2 waiting · 7 total   184.2k tokens · $1.37
```

- The counts cover the whole tree of delegation, not only direct children.
  `active` is an agent with a turn in flight, `waiting` is an agent blocked on
  approval, input or a sibling, and `total` includes agents that finished in
  this turn.
- Tokens and cost are the sum over the tree, with the pricing that `stats`
  uses. The child reports its usage and its descendant totals in its
  progress notifications, so the parent sums what it received and does not
  read child branches.
- A generator in the parent writes the slot. Each count is an `fy-act`
  (`agents:active`, `agents:waiting`) that zooms the pane back to the agents
  in that state.
- The layout collapses to the summary when the pane cannot give every agent
  tile the least rung of its ladder. A sub-agent that needs the user, such as
  a pending approval, is not collapsed: it keeps a `mark` row above the
  summary.
- A child with grandchildren uses the same slot on its own `summary` page.
- The child has no prompt slot. It has no user at its keyboard unless the
  user zooms its tile, and then the child has a prompt only if it accepts
  input.
- The parent tile ladder of `fyai_workpane_tile_set_ladder()` still decides
  whether the tile shows a screen. The child ladder decides what the screen
  holds.
- A sub-agent in fullscreen mode uses the alternate screen of its own
  terminal. The terminal view of the parent already enables that buffer.

## 6. Gaps

### libfymd4c

1. **Shed order.** `drop="N"` on a block: lower values go first when the page
   is too short. The prompt has no `drop` and goes last.
2. **Grid with spans.** `<fy-grid rows="..." cols="...">` with
   `<fy-cell row col rowspan colspan>`, and the track sizes that
   `tracks_solve()` supports, `fit` included. `fy-columns` cannot express
   `main-left`.
3. **Ground.** `ground="reverse|#rrggbb"` on `fy-cell` and on a block slot,
   so that the page states focus.
4. **Region paths.** Regions of a nested render in page coordinates, with the
   slot path, or a region sink for the slot renderer.
5. **A viewport render.** Render a document from a row offset for a row
   count, for the fullscreen transcript slot, without rendering the rows
   above the offset each frame.

### libfytimui

All additions. The current API keeps its behaviour.

1. **Page API.** `fytim_page_set(ft, rows, len, regions, count)`: the
   rendered rows under the commit contract, and the slot and act regions.
   When a page is set, the frame draws the page and not the band stack.
2. **Slot binding.** `fytim_surface_bind(sf, id)`,
   `fytim_workband_bind(wb, id)`, and the built-in ids `tail`, `prompt` and
   `completion`. An unbound slot is blank.
3. **The compositor.** Draw the rows, then each slot region clipped to it.
4. **Hit test.** One region table for the frame, for acts and slots.
5. **Fullscreen.** `fytim_cfg.screen = FYTIM_SCREEN_ALT`: enter the alternate
   screen, grab the mouse for the viewport, draw a selection, and copy with
   OSC 52. Restore the primary screen on destroy and on suspend.

### fyai

1. `src/fyai_page.c`: the one owner of the page source. It expands the
   configured page, asks each component for its fragment and preference,
   renders or reuses (section 4), publishes, and hands out the grants.
   `$FYAI_TRACE` records one line for each render.
2. The workpane manager writes the `pane` fragment when the page renderer is
   active, and uses the tile request functions when it is not. The placement
   decision, `fyai_workpane_place()`, is shared.
3. `src/fyai_transcript_view.c`: the fullscreen viewport over the fragment
   walk.
4. The sink backend selects the path: `fyai_ui_*` for the current renderer,
   `fyai_page_*` for the page. A producer does not know which one is active.

## 7. Remaining work

Keep the band stack available. Select a new default only after the page
renderer passes the complete functional suite and the cell comparisons in
both screen modes.

Recursive tile documents still need a page ladder selected by the tile grant,
selection on resize, a call generator, and an agent summary with descendant
usage totals. Test a child and grandchild across each ladder boundary.

## 8. Decisions

- OSC 52 is the copy path in fullscreen mode. No mouse release key.
- The fullscreen viewport uses the flow policy unchanged, and its scrollback
  reflows at every width change.
- A sub-agent renders a page of its own at the size of its tile, and the size
  selects how much the page shows.

- The state of the current tool call is the `call` slot, written by its own
  generator in the child.
- All agents collapse into one `agents` summary row: active, waiting, total,
  tokens and cost over the tree.

## 9. Open questions

None now.
