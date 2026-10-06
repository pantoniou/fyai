#!/bin/bash
# SPDX-License-Identifier: MIT
# The header is one row, cut at the edge. A working directory with a long path
# runs the header past the terminal: a shell that takes the pane and the keys
# must leave the status row that says how to give the keys back, and the header
# must not wrap onto a second row.
set -eu
. "$(dirname "$0")/../harness.sh"

CAPTURES=$(mktemp -d)
trap 'rm -rf "$CAPTURES"' EXIT
LONG="$CAPTURES/a-directory-with-a-path-long-enough-to-run-the-header-past-its-edge"
mkdir -p "$LONG"

run_with()
{
    renderer=$1
    FYAI_TMPDIR_BASE="$LONG" fyai_test_setup
    driver=0
    FYAI_TRACE="$TEST_DIR/trace.log" \
    FYAI_PTY_COLS=100 \
    FYAI_PTY_INPUT="!sh -c 'while :; do printf \"\\r\\033[K%s%s \" HO LD; sleep 0.2; done'" \
    FYAI_PTY_NEEDLE="bang-1" FYAI_PTY_TIMEOUT=20 \
    FYAI_PTY_AFTER="wait-screen:HOLD|wait-screen:Ctrl-]|raw:1d|wait-gone:Ctrl-]" \
    FYAI_PTY_AFTER_PAUSE=0 FYAI_PTY_AFTER_TIMEOUT=10 \
    "$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
        "$FYAI_BIN" -k test-key --theme dark \
        --set display/markdown=true \
        -m mock-model -i || driver=$?
    cp "$TEST_DIR/pty.out" "$CAPTURES/$renderer.out"
    cp "$TEST_DIR/trace.log" "$CAPTURES/$renderer.trace" 2>/dev/null || :
    if [ "$driver" -ne 0 ]; then
        if grep -a -q "needs a libfytimui" "$CAPTURES/$renderer.out" \
                "$CAPTURES/$renderer.trace" 2>/dev/null; then
            skip "this build has no page support"
        fi
        fail "a shell under a long header did not hold the keys under $renderer"
    fi
}

run_with page

"$PYTHON" - "$CAPTURES/page.out" "$TESTS_DIR" \
    <<'PY' || fail "the page did not draw the long header"
import re
import sys

sys.path.insert(0, sys.argv[2])
from screen import Screen

END = b"\x1b[?2026l"

def held(path):
    """The rows of the last frame in which the shell holds the keys."""
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
        if any("HOLD" in r for r in rows) and any("Ctrl-]" in r for r in rows):
            found = rows
    if found is None:
        raise SystemExit("%s: the shell never held the keys" % path)
    return found

rows = held(sys.argv[1])
at = next((y for y, r in enumerate(rows) if "fyai: session/" in r), None)
if at is None:
    raise SystemExit("no header row: %r" % rows)
# The long directory is drawn on the header row alone: another row that holds
# it means the header wrapped.
if any("a-directory" in r for r in rows[:at] + rows[at + 1:]):
    raise SystemExit("the header is not one row: %r" % rows)
PY

pass
