#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify every cell after an ncurses differential resize repaint.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

# Wait for the requested width and the completed differential repaint.
FYAI_PTY_ROWS=30 FYAI_PTY_COLS=100 \
FYAI_PTY_INPUT="!env LINES=30 COLUMNS=98 $PYTHON $TESTS_DIR/resize_curses.py" \
FYAI_PTY_NEEDLE="E29G1:" FYAI_PTY_TIMEOUT=20 \
FYAI_PTY_AFTER="resize:52|wait:50G|drain:2|raw:1d" \
FYAI_PTY_AFTER_PAUSE=1 FYAI_PTY_SNAPSHOT="$TEST_DIR/curses.out" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true -m mock-model -i

"$PYTHON" - "$TEST_DIR/curses.out" "$TESTS_DIR" <<'PYEOF' || \
    fail "the resized ncurses pattern was corrupted"
import sys

sys.path.insert(0, sys.argv[2])
from screen import Screen

data = open(sys.argv[1], "rb").read()
clear = b"\x1b[0m\x1b[H\x1b[2J"
if clear in data:
    data = data[data.rfind(clear):]
s = Screen(30, 52)
s.feed(data)
lines = s.lines()
if not any("SIZE 24x50 GEN" in line for line in lines):
    raise SystemExit("the resized ncurses generation is not visible")
for row in range(2, 20):
    token = "R%02d:" % row
    matches = [line.strip() for line in lines if token in line]
    if not matches:
        raise SystemExit("ncurses pattern row %d is missing" % row)
    body = matches[-1]
    fill = chr(ord("A") + row % 26)
    if body[len(token):].replace(fill, ""):
        raise SystemExit("ncurses row %d contains foreign cells: %r" %
                         (row, body))
PYEOF

pass
