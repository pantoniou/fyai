#!/bin/bash
# SPDX-License-Identifier: MIT
# Under display/renderer=page the pane is an fy-grid the page writes, with one
# slot for each tile. Two tiles side by side must stand as the pane of the band
# stack places them: the same columns, the same rule between them, the same
# rows for their screens. Each shell prints the size it was given, and the
# tile that holds the keys stands on the same reversed ground.
set -eu
. "$(dirname "$0")/../harness.sh"

CAPTURES=$(mktemp -d)
trap 'rm -rf "$CAPTURES"' EXIT

run_with()
{
    renderer=$1
    fyai_test_setup
    FYAI_TRACE="$TEST_DIR/trace.log" \
    FYAI_PTY_COLS=100 \
    FYAI_PTY_INPUT="!sh -c 'while :; do printf \"\\r\\033[KFIRST %s \" \"\$(stty size)\"; sleep 1; done'" \
    FYAI_PTY_NEEDLE="FIRST" FYAI_PTY_TIMEOUT=20 \
    FYAI_PTY_AFTER="wait:Ctrl-]|raw:1d|wait-gone:Ctrl-]|"\
"send:!sh -c 'while :; do printf \"\\r\\033[KSECOND %s \" \"\$(stty size)\"; sleep 1; done'|wait-frame:SECOND|"\
"frame:2|raw:1d|wait-gone:Ctrl-]|"\
"send:/kill bang-1|wait:stopping shell bang-1|"\
"send:/kill bang-2|wait:stopping shell bang-2" \
    FYAI_PTY_AFTER_PAUSE=0.5 FYAI_PTY_AFTER_TIMEOUT=10 \
    "$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
        "$FYAI_BIN" -k test-key --theme dark \
        --set display/markdown=true --set display/work_min_tile_cols=30 \
        --set "display/renderer=$renderer" -m mock-model -i
    cp "$TEST_DIR/pty.out" "$CAPTURES/$renderer.out"
    cp "$TEST_DIR/trace.log" "$CAPTURES/$renderer.trace"
}

run_with stack
run_with page

if grep -a -q "needs a libfytimui" "$CAPTURES/page.out" "$CAPTURES/page.trace"; then
    skip "this build has no page support"
fi

# The band stack draws the same screen: only the trace says that the page
# placed the two tiles in slots of their own, beside the prompt slot.
grep -a -q "page: .*tiles=2" "$CAPTURES/page.trace" ||
    fail "the page did not place the tiles in slots"

"$PYTHON" - "$CAPTURES/stack.out" "$CAPTURES/page.out" "$TESTS_DIR" \
    <<'PY' || fail "the page did not place the tiles as the band stack does"
import re
import sys

sys.path.insert(0, sys.argv[3])
from screen import Screen

END = b"\x1b[?2026l"

def side_by_side(path):
    """The first frame in which the two tiles stand on one row with the sizes
    they print, and where it is reversed."""
    data = open(path, "rb").read()
    screen = Screen(30, 100)
    pos = 0
    while True:
        i = data.find(END, pos)
        if i < 0:
            raise SystemExit("the two tiles never stood side by side in %s"
                             % path)
        screen.feed(data[pos:i + len(END)])
        pos = i + len(END)
        rows = [r.rstrip() for r in screen.display()]
        if any(re.search(r"FIRST \d+ \d+ .*SECOND \d+ \d+", r) for r in rows):
            return rows, screen.reversed_display()

def normal(row):
    # The session row names its run, and a running mark blinks.
    row = re.sub(r"session/\S+ · \S+", "session", row)
    return re.sub(r"[^\x00-\x7f┃│⎿…─]", " ", row)

stack, stack_rev = side_by_side(sys.argv[1])
page, page_rev = side_by_side(sys.argv[2])
differ = [(i, a, b) for i, (a, b) in enumerate(zip(stack, page))
          if normal(a) != normal(b)]
differ += [(i, "rev " + a, "rev " + b)
           for i, (a, b) in enumerate(zip(stack_rev, page_rev)) if a != b]
if differ:
    raise SystemExit("\n".join("%2d stack|%s\n   page |%s" % d
                               for d in differ))
PY

pass
