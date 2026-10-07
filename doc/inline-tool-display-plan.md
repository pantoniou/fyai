# Inline tool display for the fullscreen page

## Context

Today a `shell` or `agent` call that runs for longer than
`display/work_open_delay_ms` (500 ms) opens a tile of the work pane
(`fyai_tool_job_band_open()`, `src/fyai_tools.c:5209`, armed at :5527). The
progress is drawn there, away from the transcript position of the call, and
the pane opening/closing moves the page. The delay exists only to hide that
jump for fast commands.

Goal: an alternative renderer, gated by configuration, that draws the live
output of the call **in the transcript itself**, at the rows the call occupies
in the Markdown scrollback. First release: fullscreen page only
(`display/screen: fullscreen`, `src/fyai_transcript_view.c`), where the
transcript is a view we own, so nothing jumps. Two kinds of live block:

1. **Text block** (no PTY): the existing progressive fenced render
   (`fyai_fenced_stream_*`, `display/tool_history_lines` /
   `tool_preview_lines`) made at the transcript width.
2. **Terminal block** (PTY: named sessions, `tty: true`, sub-agent screens):
   a `fyai_terminal_view` grid materialized at that point in the transcript.

## Decisions (confirmed)

- Scope: only model `shell` calls and sub-agents go inline. Bang commands,
  `/btw` and editors stay in the work pane.
- PTY input: `Ctrl-T` focus cycles through the inline blocks, and `Ctrl-]`
  returns to the prompt.
- On finish: the block is replaced by the stored exchange, which is the same
  set of rows that replay draws.
- Height: a text block grows to fit, up to `display/tool_preview_lines` /
  history. A terminal block has the fixed `display/inline_terminal_rows`.

## Configuration

- New key `display/tool_display: pane | inline` (default `pane`),
  `x-fyai-scope: session`, in `data/config.schema.yaml`; field
  `cfg->tool_display` set in `apply_config` (`src/fyai_config.c` near :752).
- `display/inline_terminal_rows` (integer, default 12): height of a terminal
  block. Width is the transcript width less the tool-output indent.
- `inline` takes effect only when the UI is fullscreen; otherwise the pane
  path runs unchanged. One predicate, `fyai_ui_tools_inline(ctx)`, decides.
- `work_open_delay_ms` does not apply to inline blocks: an inline block grows
  in place and there is no jump to hide.

## Design

### 1. Live blocks in the transcript view (`src/fyai_transcript_view.c`)

The view now has stored rows, committed live rows, and one flat tail
(`fyai_transcript_view_set_tail()`). Add an ordered list of **live blocks**
after the committed live rows and before the tail:

```c
struct fyai_transcript_block;	/* opaque, owned by the view */
struct fyai_transcript_block *fyai_transcript_view_block_open(v, uintptr_t owner);
int  fyai_transcript_view_block_set(b, const char *rows, size_t len); /* replace rows */
void fyai_transcript_view_block_close(b);	/* rows leave with the stored exchange */
```

- A block is a run of rendered rows (SGR strings, like every other row), so
  `_window()`, `_copy()` (selection), `_scroll()` and the "scrolled-back keeps
  its top row" rule work unchanged.
- Block order is the order of the calls (registration order), matching the
  order in which the stored document will hold the tool exchanges.
- `fyai_transcript_view_rows()` and the top-row anchor count block rows.
  A block that grows above the anchor of a scrolled-back view shifts the
  anchor so the visible rows do not move.
- On commit the existing `fyai_transcript_view_replace_live()` path drops the
  blocks with the live rows when the exchange is stored.

### 2. Sink placement for text blocks (`src/fyai_sink.c`, `src/fyai_ui.c`)

Producers keep the band API (`fyai_sink_band_open/paint/close`); the terminal
backend decides placement. When `fyai_ui_tools_inline()` is true:

- `band_open` creates a transcript block instead of a work-pane tile
  (`fyai_ui_work_tile_create()` is not called).
- `band_paint` renders into the block via `fyai_transcript_view_block_set()`
  and sets `frame_pending`.
- `fyai_sink_band_cols()` returns transcript width minus
  `markdown_tool_output_indent()` (through `fyai_width_reserve_begin()`), and
  a width change calls the band `repaint` hook, as a tile grant does now.
- `band_close` closes the block. The committed rows are the stored exchange
  (`tool_head` + preview), drawn by the normal renderer, so the final rows
  are the ones replay draws.
- Flow: the block is a tool unit; it calls `fyai_sink_unit()` at open, so
  `fyai_flow.c` gives it the same separation as the stored exchange.

In `fyai_tools.c` the inline path opens the band at once (no
`band_delay` timer) and uses the same title row via
`markdown_render_tool_head()`.

### 3. Terminal blocks (`src/fyai_terminal_view.c`, `src/fyai_tools.c`, `src/fyai_agent.c`)

- The terminal session gets its size from the block, not from a tile grant:
  `tool/run` carries `size = {inline_terminal_rows, block cols}`;
  `fyai_ui_surface_granted_rows/cols()` answer from the block.
- New `fyai_terminal_view_rows_sgr(view, struct response_buffer *out)`:
  serializes the grid (`fyai_terminal_view_cell()`) to SGR rows, with the
  cursor reversed and OSC 8 links kept. Called only when
  `fyai_terminal_view_take_damage()` reports damage; the result goes to
  `fyai_transcript_view_block_set()`. The head (title row + command) is
  rendered above it in the same block.
- Width change: the block width follows the transcript width, so a resize
  sends `tty/resize` through the existing barrier
  (`tty/resize` → `tty/resized`).
- Alternate-screen programs are drawn as the grid while running; at end the
  stored exchange is the stripped log, as today.
- Sub-agents: `fyai_agent_view_open()` uses a terminal block when inline.

### 4. Keys and focus for terminal blocks

- A terminal block is focusable: `Ctrl-T`/`Ctrl-Tab` cycle includes live
  blocks in transcript order, `Ctrl-]` returns to the prompt. The focused
  block gets the focus ground (`fyai_chrome_focus()`) and the view scrolls so
  it is visible. Key delivery reuses `fyai_workpane_keys_deliver()` semantics
  by looking the owner up by block.
- Mouse: click in block rows focuses it (`ui_click_off_tiles()` extended to
  resolve the transcript region rows to a block).

### 5. Chrome

`fyai_chrome_items()` gets a kind `INLINE`: no buttons, no scroll bar (the
transcript scroll is the scroll). Kill via `/kill`.

## Phasing (patch series: implementation, tests, docs)

1. config key + predicate.
2. transcript view blocks (+ unit tests in `tests/fyai_transcript_view_test.c`).
3. sink/UI placement for text bands; tools path skips the delay.
4. terminal view SGR serialization; terminal block for shell sessions.
5. sub-agent terminal block.
6. focus/keys for terminal blocks.
7. PTY cases; docs (`doc/`, CLAUDE.md UI section).

## Verification

- Unit: block insertion/growth/anchor in `fyai_transcript_view` tests;
  `fyai_terminal_view_rows_sgr()` against a libfyvterm screen.
- Page golden unaffected (blocks are view rows, not page source).
- PTY cases (`tests/cases/ui_inline_tool_*.sh`) with `wait-screen`:
  a `seq` shell call appears under its title row inside the transcript and
  the committed rows equal the replay; a `tty: true` session shows its grid
  inline and accepts keys after `Ctrl-T`; a resize re-wraps the text block
  and resizes the terminal block; scrolled-back view does not move while a
  block grows.
- `ninja -C build parallel-test`, ASAN suite, and
  `ctest -j96 --repeat until-fail:5` for the PTY cases.
