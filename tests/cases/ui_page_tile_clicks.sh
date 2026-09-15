#!/bin/bash
# SPDX-License-Identifier: MIT
# With display/work_controls the head of a tile takes clicks: its name gives
# the tile the keys, and its marks zoom and close it. Under
# display/renderer=page the head is drawn by the page and a click is an act of
# the page, so both renderers must draw the head alike and act alike.
set -eu
. "$(dirname "$0")/../harness.sh"

CAPTURES=$(mktemp -d)
trap 'rm -rf "$CAPTURES"' EXIT

# A press and a release of the first button at 1-based column $1, row $2.
click()
{
    printf '\033[<0;%d;%dM\033[<0;%d;%dm' "$1" "$2" "$1" "$2" |
        od -An -tx1 | tr -d ' \n'
}

NAME=$(click 7 1)
ZOOM=$(click 99 1)
CLOSE=$(click 100 1)

run_with()
{
    renderer=$1
    fyai_test_setup
    driver=0
    FYAI_TRACE="$TEST_DIR/trace.log" \
    FYAI_PTY_COLS=100 \
    FYAI_PTY_INPUT="!sh -c 'while :; do printf \"\\r\\033[KCLICK %s \" \"\$(stty size)\"; sleep 1; done'" \
    FYAI_PTY_NEEDLE="CLICK" FYAI_PTY_TIMEOUT=20 \
    FYAI_PTY_AFTER="wait-frame:Ctrl-]|raw:1d|wait-gone:Ctrl-]|drain:1.5|"\
"raw:$NAME|wait-frame:Ctrl-]|raw:1d|wait-gone:Ctrl-]|"\
"raw:$ZOOM|frame:2|drain:1|"\
"raw:$CLOSE|wait-gone:⤢" \
    FYAI_PTY_AFTER_PAUSE=0.5 FYAI_PTY_AFTER_TIMEOUT=10 \
    "$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
        "$FYAI_BIN" -k test-key --theme dark \
        --set display/markdown=true --set display/work_controls=zoom \
        --set "display/renderer=$renderer" -m mock-model -i || driver=$?
    cp "$TEST_DIR/pty.out" "$CAPTURES/$renderer.out"
    cp "$TEST_DIR/trace.log" "$CAPTURES/$renderer.trace"
    if [ "$driver" -ne 0 ]; then
        tail -c 2000 "$CAPTURES/$renderer.out" >&2
        fail "a click on the head of a tile did not act under $renderer"
    fi
    # The zoom mark zoomed the tile.
    grep -a -q "workpane: .*zoomed=0x" "$CAPTURES/$renderer.trace" ||
        fail "the zoom mark did not zoom the tile under $renderer"
}

run_with stack
run_with page

if grep -a -q "needs a libfytimui" "$CAPTURES/page.out" "$CAPTURES/page.trace"; then
    skip "this build has no page support"
fi
grep -a -q "page: .*tiles=1" "$CAPTURES/page.trace" ||
    fail "the page did not draw the tile"

"$PYTHON" - "$CAPTURES/stack.out" "$CAPTURES/page.out" "$TESTS_DIR" \
    <<'PY' || fail "the page did not draw the head of the tile as the band stack does"
import re
import sys

sys.path.insert(0, sys.argv[3])
from screen import Screen

END = b"\x1b[?2026l"

def head(path):
    """The head row of the tile at rest, before the first click."""
    data = open(path, "rb").read()
    screen = Screen(30, 100)
    pos = 0
    found = None
    while True:
        i = data.find(END, pos)
        if i < 0:
            break
        screen.feed(data[pos:i + len(END)])
        pos = i + len(END)
        rows = screen.display()
        text = "\n".join(rows)
        if "Ctrl-]" in text and found is not None:
            break
        if re.search(r"CLICK \d+ \d+", text) and "Ctrl-]" not in text:
            found = rows[0].ljust(100)
    if found is None:
        raise SystemExit("%s: the tile never stood at rest" % path)
    return found

stack = head(sys.argv[1])
page = head(sys.argv[2])
for name, row in (("stack", stack), ("page", page)):
    # The clicks go to these cells: the name, then the two marks.
    if row[4:18] != "shell [bang-1]" or row[98] != "⤢" or row[99] != "×":
        raise SystemExit("%s head is not where the clicks go: %r" % (name, row))
# The running mark in the gutter blinks; the rest of the row is the same.
if stack[3:] != page[3:]:
    raise SystemExit("head differs:\n stack|%s\n page |%s" % (stack, page))
PY

pass
