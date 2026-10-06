#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that focus does not change work-pane geometry, and that a fractional
# pane height follows the terminal it is a fraction of.
set -eu
. "$(dirname "$0")/../harness.sh"

# The shell reports its size and a count every 0.2 seconds. The count changes
# the screen at each report, so frames go on while the geometry holds.
SHELL_SIZE="!sh -c 'n=0; while :; do n=\$((n+1)); echo \"SIZE \$(stty size) #\$n\"; sleep 0.2; done'"

# Two bang shells report the rows their pseudo-terminals were given, then
# focus is cycled over both tiles and back to the prompt. A tile must keep the
# height it had in every frame after both stand, and reports go on after the
# cycle.
focus_keeps_size()
{
    fyai_test_setup
    FYAI_PTY_ROWS=32 FYAI_PTY_COLS=120 \
    FYAI_PTY_INPUT="$SHELL_SIZE" \
    FYAI_PTY_NEEDLE="bang-1" FYAI_PTY_TIMEOUT=30 \
    FYAI_PTY_AFTER="wait-screen:Ctrl-]|wait-screen:SIZE 14 118|raw:1d|wait-gone:Ctrl-]|"\
"send:$SHELL_SIZE|wait-screen:Ctrl-]|wait-screen:SIZE 14 57|wait-screen:SIZE 14 56|"\
"raw:14|frame:2|raw:14|frame:2|raw:1d|wait-gone:Ctrl-]|frame:6" \
    "$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/focus.out" \
        "$FYAI_BIN" -k test-key --theme dark \
        --set display/markdown=true --set display/work_min_tile_cols=30 \
        --set display/work_zoom_rows=half -m mock-model -i ||
        fail "the two shells did not stand at their sizes around a focus cycle"
    "$PYTHON" - "$TEST_DIR/focus.out" "$TESTS_DIR" <<'PYEOF' || \
        fail "focus cycling changed the size of a tile"
import re
import sys

sys.path.insert(0, sys.argv[2])
from screen import Screen

END = b"\x1b[?2026l"
SIZE = re.compile(r"SIZE (\d+) (\d+) #(\d+)")
data = open(sys.argv[1], "rb").read()
screen = Screen(32, 120)
pos = 0
both = False
seen = set()
while True:
    i = data.find(END, pos)
    if i < 0:
        break
    screen.feed(data[pos:i + len(END)])
    pos = i + len(END)
    # The newest report of each tile is the size its shell has now. Older
    # reports stay on the screen of the tile, the first of them printed before
    # the tile joined the layout.
    newest = {}
    for line in screen.display():
        for m in SIZE.finditer(line):
            side = m.start() >= 60
            count = int(m.group(3))
            if side not in newest or count > newest[side][0]:
                newest[side] = (count, int(m.group(1)), int(m.group(2)))
    sizes = {(r, c) for _, r, c in newest.values()}
    # From the frame in which both tiles stand at their share of the pane.
    both = both or sizes == {(14, 57), (14, 56)}
    if both:
        seen |= sizes
if not both:
    raise SystemExit("the two tiles never stood side by side")
if {r for r, _ in seen} != {14}:
    raise SystemExit("a tile changed its height: %r" % sorted(seen))
PYEOF
}

# A named pane height is a fraction of the current terminal, so a resize of
# the outer window changes it: half of 32 rows, then half of 64.
resize_recalculates()
{
    fyai_test_setup
    FYAI_PTY_ROWS=32 FYAI_PTY_COLS=100 \
    FYAI_PTY_INPUT="$SHELL_SIZE" \
    FYAI_PTY_NEEDLE="bang-1" FYAI_PTY_TIMEOUT=30 \
    FYAI_PTY_AFTER="wait-screen:Ctrl-]|wait-screen:SIZE 14 98|resize:64x100|"\
"wait-screen:SIZE 30 98|raw:1d" \
    "$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/resize-half.out" \
        "$FYAI_BIN" -k test-key --theme dark \
        --set display/markdown=true \
        --set display/work_zoom_rows=half -m mock-model -i ||
        fail "a half pane did not follow the terminal size"
}

focus_keeps_size
# The cap includes the surrounding UI and shell chrome, as the pane's own
# height policy does. The prompt block stands whether or not the tile holds
# the keys, so these do not change when focus does.
resize_recalculates
pass
