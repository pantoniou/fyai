#!/bin/bash
# SPDX-License-Identifier: MIT
# A terminal as large as the side layout of the page document asks for stands
# the work pane in a column beside the transcript: under /page review the
# transcript and the head of a bang tile share the first row, the tile at the
# right. display/work_panels=on takes the column at a smaller size, with the
# width of display/work_panel_cols; off keeps the pane in its band, as
# display/page_layout=band does. {layout} of the header names the layout.
set -eu
. "$(dirname "$0")/../harness.sh"

# $1 columns, $2 the column the tile starts at or "band"; further arguments
# go to fyai.
side()
{
    cols=$1
    want=$2
    shift 2
    fyai_test_setup
    driver=0
    FYAI_TRACE="$TEST_DIR/trace.log" \
    FYAI_PTY_ROWS=40 FYAI_PTY_COLS=$cols \
    FYAI_PTY_INPUT="!sh -c 'printf %s%s SIDE OUT; sleep 60'" \
    FYAI_PTY_NEEDLE="bang-1" FYAI_PTY_TIMEOUT=20 \
    FYAI_PTY_AFTER="wait-screen:SIDEOUT|raw:1d|wait-gone:Ctrl-]|send:/page review on|wait-screen:status.row|wait-screen:head:1" \
    FYAI_PTY_AFTER_PAUSE=0 FYAI_PTY_AFTER_TIMEOUT=10 \
    "$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
        "$FYAI_BIN" -k test-key --theme dark \
        --set display/markdown=true \
        --set display/screen=fullscreen \
        --set "display/prompt_bottom= L={layout}" "$@" -m mock-model -i ||
        driver=$?
    if grep -a -q "needs a libfytimui" "$TEST_DIR/pty.out" \
            "$TEST_DIR/trace.log" 2>/dev/null; then
        skip "this build has no page support"
    fi
    if [ "$driver" -ne 0 ]; then
        tail -c 2000 "$TEST_DIR/pty.out" >&2
        fail "the layout at $cols columns was not reviewed"
    fi
    "$PYTHON" - "$TEST_DIR/pty.out" "$TESTS_DIR" "$cols" "$want" <<'PY' ||
import sys

sys.path.insert(0, sys.argv[2])
from screen import Screen

cols = int(sys.argv[3])
want = sys.argv[4]
END = b"\x1b[?2026l"
data = open(sys.argv[1], "rb").read()
screen = Screen(40, cols)
pos = 0
rows = None
while True:
    i = data.find(END, pos)
    if i < 0:
        break
    screen.feed(data[pos:i + len(END)])
    pos = i + len(END)
    shown = screen.display()
    if any(" head:1" in r for r in shown) and \
            any("status.row" in r for r in shown):
        rows = shown
if rows is None:
    raise SystemExit("no frame named the areas of the page")
top = rows[0]
name = "band" if want == "band" else "side"
if not any(("L=" + name) in r for r in rows):
    raise SystemExit("the header does not name the %s layout" % name)
if want == "band":
    # The band: the head of the tile stands under the transcript.
    if any(" head:1" in r for r in rows[:1]):
        raise SystemExit("the tile stands beside the transcript: %r" % top)
    raise SystemExit(0)
edge = int(want)
t = top.find(" transcript ")
h = top.find(" head:1")
if t < 0 or h < 0 or not t < edge <= h:
    raise SystemExit("the transcript and the tile are not side by side: %r"
                     % top)
if not any("SIDEOUT" in r[edge:] for r in rows):
    raise SystemExit("the output of the tile is not in the side column")
PY
        fail "the layout at $cols columns is not the $want layout"
}

side 180 98
side 120 60 --set display/work_panels=on --set display/work_panel_cols=60
side 180 band --set display/work_panels=off
side 180 band --set display/page_layout=band
pass
