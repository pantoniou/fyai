#!/bin/bash
# SPDX-License-Identifier: MIT
# Under display/renderer=page the page places the tiles of every
# display/work_layout. Three shells and a notice must stand as the band stack
# places them in each layout: the same rows, the same size given to each shell,
# and the same reversed cells.
set -eu
. "$(dirname "$0")/../harness.sh"

CAPTURES=$(mktemp -d)
trap 'rm -rf "$CAPTURES"' EXIT

shell()
{
    printf '%s' "!sh -c 'while :; do printf \"\\r\\033[K$1 %s \" \"\$(stty size)\"; sleep 0.2; done'"
}

# The size each shell is granted once the notice stands under the screens.
# The case waits for these on the screen, and for each head to be made again
# at the width of its grant, which leaves no head cut with an ellipsis: both
# renderers are then at rest.
sizes()
{
    case $1 in
    auto|columns) echo "FIRST 9 47|SECOND 9 46|THIRD 8 98" ;;
    stack) echo "FIRST 5 98|SECOND 5 98|THIRD 5 98" ;;
    main-top) echo "FIRST 9 98|SECOND 8 47|THIRD 8 46" ;;
    main-left) echo "FIRST 19 47|SECOND 9 46|THIRD 8 46" ;;
    esac
}

run_with()
{
    renderer=$1
    layout=$2
    settled=$(sizes "$layout" | sed 's/^/wait-screen:/; s/|/|wait-screen:/g')
    fyai_test_setup
    FYAI_TRACE="$TEST_DIR/trace.log" \
    FYAI_PTY_COLS=100 \
    FYAI_PTY_INPUT="$(shell FIRST)" \
    FYAI_PTY_NEEDLE="FIRST" FYAI_PTY_TIMEOUT=20 \
    FYAI_PTY_AFTER="wait-screen:Ctrl-]|raw:1d|wait-gone:Ctrl-]|"\
"send:$(shell SECOND)|wait-screen:Ctrl-]|raw:1d|wait-gone:Ctrl-]|"\
"send:$(shell THIRD)|wait-screen:Ctrl-]|raw:1d|wait-gone:Ctrl-]|"\
"send:/nosuch|wait-screen:unknown|$settled|frame:4|"\
"send:/kill bang-1|wait-screen:stopping shell bang-1|"\
"send:/kill bang-2|wait-screen:stopping shell bang-2|"\
"send:/kill bang-3|wait-screen:stopping shell bang-3" \
    FYAI_PTY_AFTER_PAUSE=0 FYAI_PTY_AFTER_TIMEOUT=10 \
    "$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
        "$FYAI_BIN" -k test-key --theme dark \
        --set display/markdown=true --set display/work_min_tile_cols=30 \
        --set "display/work_layout=$layout" --set display/work_columns=2 \
        --set "display/renderer=$renderer" -m mock-model -i
    cp "$TEST_DIR/pty.out" "$CAPTURES/$renderer-$layout.out"
    cp "$TEST_DIR/trace.log" "$CAPTURES/$renderer-$layout.trace"
}

LAYOUTS="auto columns stack main-top main-left"
for layout in $LAYOUTS; do
    run_with stack "$layout"
    run_with page "$layout"
    if grep -a -q "needs a libfytimui" "$CAPTURES/page-$layout.out" \
        "$CAPTURES/page-$layout.trace"; then
        skip "this build has no page support"
    fi
    grep -a -q "page: .*tiles=4" "$CAPTURES/page-$layout.trace" ||
        fail "the page did not place three shells and a notice in $layout"
done

"$PYTHON" - "$CAPTURES" "$TESTS_DIR" $LAYOUTS <<'PY' ||
import re
import sys

sys.path.insert(0, sys.argv[2])
from screen import Screen

END = b"\x1b[?2026l"
SIZES = re.compile(r"(FIRST|SECOND|THIRD) \d+ \d+")

def before_kill(path):
    """The last frame that shows the three shells with their sizes and the
    notice while the session is idle: before the first stop is entered, so
    both renderers are compared at rest."""
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
        rows = [r.rstrip() for r in screen.display()]
        text = "\n".join(rows)
        if "/kill" in text:
            break
        if len(set(SIZES.findall(text))) == 3 and "unknown" in text:
            found = (rows, screen.reversed_display())
    if found is None:
        raise SystemExit("%s: the shells and the notice never stood together"
                         % path)
    return found

def normal(row):
    # The session row names its run, and a running mark blinks.
    row = re.sub(r"session/\S+ ·\s+\S+", "session", row)
    return re.sub(r"[^\x00-\x7f┃│⎿…─]", " ", row)

failed = []
for layout in sys.argv[3:]:
    stack, stack_rev = before_kill("%s/stack-%s.out" % (sys.argv[1], layout))
    page, page_rev = before_kill("%s/page-%s.out" % (sys.argv[1], layout))
    differ = [(i, a, b) for i, (a, b) in enumerate(zip(stack, page))
              if normal(a) != normal(b)]
    differ += [(i, "rev " + a, "rev " + b)
               for i, (a, b) in enumerate(zip(stack_rev, page_rev)) if a != b]
    if differ:
        failed.append("layout %s:\n%s" % (layout, "\n".join(
            "%2d stack|%s\n   page |%s" % d for d in differ)))
if failed:
    raise SystemExit("\n".join(failed))
PY
    fail "the page did not place the tiles as the band stack does"

pass
