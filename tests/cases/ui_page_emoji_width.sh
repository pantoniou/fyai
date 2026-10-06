#!/bin/bash
# SPDX-License-Identifier: MIT
# A terminal that draws an emoji base with U+FE0F in one column, as VTE does,
# is measured the first time such a glyph is written: the table that holds it
# is made again at the measured width, and its columns stand level. The
# reply to the measure reaches fyai before /layout, so the screen that shows
# the result of /layout shows the table made again.
set -eu
. "$(dirname "$0")/../harness.sh"

export FYAI_PTY_EMOJI_VS=narrow
fyai_test_setup
mock_start chat_emoji_table.json
driver=0
FYAI_TRACE="$TEST_DIR/trace.log" \
FYAI_PTY_ROWS=30 FYAI_PTY_COLS=100 FYAI_PTY_INPUT="a table" \
FYAI_PTY_NEEDLE="heart" FYAI_PTY_TIMEOUT=20 \
FYAI_PTY_AFTER="wait-screen:heart|send:/layout|wait-screen:layout: auto" \
FYAI_PTY_AFTER_PAUSE=0 FYAI_PTY_AFTER_TIMEOUT=10 \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set display/screen=fullscreen \
    --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i ||
    driver=$?
if grep -a -q "needs a libfytimui" "$TEST_DIR/pty.out" \
        "$TEST_DIR/trace.log" 2>/dev/null; then
    skip "this build has no page support"
fi
if [ "$driver" -ne 0 ]; then
    tail -c 2000 "$TEST_DIR/pty.out" >&2
    fail "the table with an emoji was not shown"
fi

"$PYTHON" - "$TEST_DIR/pty.out" "$TESTS_DIR" <<'PY' ||
import sys

sys.path.insert(0, sys.argv[2])
from screen import Screen

END = b"\x1b[?2026l"
data = open(sys.argv[1], "rb").read()
screen = Screen(30, 100)
pos = 0
grid = None
while True:
    i = data.find(END, pos)
    if i < 0:
        break
    screen.feed(data[pos:i + len(END)])
    pos = i + len(END)
    if any("heart" in r for r in screen.display()):
        grid = [list(r) for r in screen.grid]


def cell_of(word):
    """The cell column where @word starts, in the row that holds it."""
    for cells in grid:
        for c in range(len(cells)):
            if "".join(cells[c:c + len(word)]) == word:
                return c
    raise SystemExit("%r is not on the screen" % word)


if grid is None:
    raise SystemExit("no frame showed the table")
heart, text = cell_of("heart"), cell_of("text")
if heart != text:
    raise SystemExit("the second column stands at %d beside the emoji and "
                     "at %d below it" % (heart, text))
PY
    fail "the table was not made again at the measured width"
pass
