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

shell()
{
    printf '%s' "!sh -c 'while :; do printf \"\\r\\033[K$1 %s \" \"\$(stty size)\"; sleep 0.2; done'"
}

# The case waits on the screen for the size each shell is granted beside the
# other, while the second holds the keys; then for each head to be made again
# at the width of its grant, which leaves no head cut with an ellipsis.
run_with()
{
    renderer=$1
    fyai_test_setup
    FYAI_TRACE="$TEST_DIR/trace.log" \
    FYAI_PTY_COLS=100 \
    FYAI_PTY_INPUT="$(shell FIRST)" \
    FYAI_PTY_NEEDLE="FIRST" FYAI_PTY_TIMEOUT=20 \
    FYAI_PTY_AFTER="wait-screen:Ctrl-]|raw:1d|wait-gone:Ctrl-]|"\
"send:$(shell SECOND)|wait-screen:Ctrl-]|"\
"wait-screen:FIRST 21 47|wait-screen:SECOND 21 46|"\
"raw:1d|wait-gone:Ctrl-]|wait-gone:…|"\
"send:/kill bang-1|wait-screen:stopping shell bang-1|"\
"send:/kill bang-2|wait-screen:stopping shell bang-2" \
    FYAI_PTY_AFTER_PAUSE=0 FYAI_PTY_AFTER_TIMEOUT=10 \
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
SIZES = re.compile(r"FIRST 21 47 .*SECOND 21 46")

def side_by_side(path):
    """The reversed cells of the last frame in which the second tile holds the
    keys beside the first with their granted sizes, and the rows and reversed
    cells of the last such frame at rest, before the first stop."""
    data = open(path, "rb").read()
    screen = Screen(30, 100)
    pos = 0
    focused = rest = None
    while True:
        i = data.find(END, pos)
        if i < 0:
            break
        screen.feed(data[pos:i + len(END)])
        pos = i + len(END)
        rows = [r.rstrip() for r in screen.display()]
        text = "\n".join(rows)
        if "/kill" in text:
            break
        if not any(SIZES.search(r) for r in rows):
            continue
        if "Ctrl-]" in text:
            focused = screen.reversed_display()
        else:
            rest = (rows, screen.reversed_display())
    if focused is None or rest is None:
        raise SystemExit("the two tiles never stood side by side in %s" % path)
    return focused, rest

def normal(row):
    # The session row names its run, and a running mark blinks.
    row = re.sub(r"session/\S+ · \S+", "session", row)
    return re.sub(r"[^\x00-\x7f┃│⎿…─]", " ", row)

stack_focus, (stack, stack_rev) = side_by_side(sys.argv[1])
page_focus, (page, page_rev) = side_by_side(sys.argv[2])
differ = [(i, a, b) for i, (a, b) in enumerate(zip(stack, page))
          if normal(a) != normal(b)]
differ += [(i, "rev " + a, "rev " + b)
           for i, (a, b) in enumerate(zip(stack_rev, page_rev)) if a != b]
differ += [(i, "focus " + a, "focus " + b)
           for i, (a, b) in enumerate(zip(stack_focus, page_focus)) if a != b]
if differ:
    raise SystemExit("\n".join("%2d stack|%s\n   page |%s" % d
                               for d in differ))
PY

pass
