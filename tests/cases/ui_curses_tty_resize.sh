#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify every cell after an ncurses differential resize repaint.
set -eu
. "$(dirname "$0")/../harness.sh"

# The repaint of the resized screen is not reliably whole on the hosted runners.
[ -z "${GITHUB_ACTIONS:-}" ] || skip "hosted CI runner"

fyai_test_setup

# Wait for the last row of the resized generation on the screen: the program
# paints its rows in order, so that row stands once the differential repaint
# is whole. The pane height depends on the footer, so the rows are read from
# the size that the program reports.
export PYTHON
pty_run "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true -m mock-model -i <<'PTY'
rows: 30
cols: 100
timeout: 40
wait_timeout: 20
steps:
  - type: "!env LINES=30 COLUMNS=98 ${PYTHON} ${TESTS_DIR}/resize_curses.py"
  - wait: "ENDG1:"
    timeout: session
  - resize: 52
  - wait-screen: "ENDG2:"
  - snapshot: "${TEST_DIR}/curses.out"
  - write: "\x1d"
PTY

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
import re
rows = 0
for line in lines:
    m = re.search(r"SIZE (\d+)x50 GEN 2", line)
    if m:
        rows = int(m.group(1))
if not rows:
    raise SystemExit("the resized ncurses generation is not visible")
for row in range(2, rows - 1):
    token = "R%02d:" % row
    matches = [line.strip() for line in lines if token in line]
    if not matches:
        raise SystemExit("ncurses pattern row %d is missing" % row)
    # The focused tile uses its margin for chrome; program cells start at the
    # token after the focus marker.
    body = matches[-1]
    body = body[body.index(token):]
    fill = chr(ord("A") + row % 26)
    if body[len(token):].replace(fill, ""):
        raise SystemExit("ncurses row %d contains foreign cells: %r" %
                         (row, body))
PYEOF

pass
