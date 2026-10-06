#!/bin/bash
# SPDX-License-Identifier: MIT
# A bang program that prints once and ends reads the size of its terminal when
# it starts: its terminal is opened at the size of its tile, which the layout
# gives before the program starts, so what it prints fits the tile. The
# program prints SIZEDONE in two parts, so only its output holds the word, and
# the case closes the finished tile with Escape before it leaves.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
driver=0
FYAI_TRACE="$TEST_DIR/trace.log" \
FYAI_PTY_ROWS=30 FYAI_PTY_COLS=100 \
FYAI_PTY_INPUT="!sh -c 'set -- \$(stty size); printf SIZE%sX%sEND \$1 \$2; printf %s%s SIZE DONE'" \
FYAI_PTY_NEEDLE="bang-1" FYAI_PTY_TIMEOUT=20 \
FYAI_PTY_AFTER="wait-screen:SIZEDONE|wait-screen:Esc closes|raw:1b|wait-gone:bang-1" \
FYAI_PTY_AFTER_PAUSE=0 FYAI_PTY_AFTER_TIMEOUT=10 \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true \
    -m mock-model -i || driver=$?
if [ "$driver" -ne 0 ]; then
    tail -c 2000 "$TEST_DIR/pty.out" >&2
    fail "the bang program did not report its size"
fi
"$PYTHON" - "$TEST_DIR/pty.out" "$TESTS_DIR" "$TEST_DIR/trace.log" <<'PY' ||
import re
import sys

sys.path.insert(0, sys.argv[2])
from screen import Screen

END = b"\x1b[?2026l"
data = open(sys.argv[1], "rb").read()
screen = Screen(30, 100)
sizes = []
pos = 0
# The tile is not in the transcript: read the frames it was drawn in.
while True:
    i = data.find(END, pos)
    if i < 0:
        break
    screen.feed(data[pos:i + len(END)])
    pos = i + len(END)
    sizes += re.findall(r"SIZE(\d+)X(\d+)END", "\n".join(screen.display()))
if not sizes:
    raise SystemExit("no size was printed")
rows, cols = (int(n) for n in sizes[-1])
# The size the terminal was opened with, before any resize could reach it.
opened = re.findall(r"shell: terminal (\d+)x(\d+)", open(sys.argv[3]).read())
if not opened:
    raise SystemExit("the size the terminal opened at was not recorded")
opened = (int(opened[0][0]), int(opened[0][1]))
# The size the tile was granted, as the trace records the layout.
grants = re.findall(r"workpane: tile=\S+ kind=\d+ preferred=\S+ "
                    r"grant=(\d+)x(\d+)", open(sys.argv[3]).read())
grants = [(int(r), int(c)) for r, c in grants if int(r) and int(c)]
if not grants:
    raise SystemExit("the tile was never granted a size")
if opened != grants[0] or (rows, cols) != grants[0]:
    raise SystemExit("the terminal opened at %dx%d and the program saw "
                     "%dx%d; the tile was granted %dx%d" %
                     (opened + (rows, cols) + grants[0]))
PY
    fail "the bang program did not start at the size of its tile"
pass
