#!/bin/bash
# SPDX-License-Identifier: MIT
# The title row of a tile is one row. A title that does not fit beside the
# buttons, such as the long description of a shell of the model, loses its
# end to an ellipsis, and the buttons stay on the row of the title instead of
# wrapping under it.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start shell_long_title.json
driver=0
FYAI_TRACE="$TEST_DIR/trace.log" \
FYAI_PTY_ROWS=24 FYAI_PTY_COLS=60 FYAI_PTY_INPUT="open a session" \
FYAI_PTY_NEEDLE="CUTOUT" FYAI_PTY_TIMEOUT=20 \
FYAI_PTY_AFTER="wait-screen:CUTOUT|wait-screen:×" \
FYAI_PTY_AFTER_PAUSE=0 FYAI_PTY_AFTER_TIMEOUT=10 \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true \
    --set display/work_controls=full --set display/stream=false \
    --set tools=true --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i ||
    driver=$?
if [ "$driver" -ne 0 ]; then
    tail -c 2000 "$TEST_DIR/pty.out" >&2
    fail "the tile of the session was not drawn"
fi
"$PYTHON" - "$TEST_DIR/pty.out" "$TESTS_DIR" <<'PY' ||
import sys

sys.path.insert(0, sys.argv[2])
from screen import Screen

END = b"\x1b[?2026l"
data = open(sys.argv[1], "rb").read()
screen = Screen(24, 60)
pos = 0
rows = None
while True:
    i = data.find(END, pos)
    if i < 0:
        break
    screen.feed(data[pos:i + len(END)])
    pos = i + len(END)
    # A tile that ended is committed without buttons: read the live one.
    shown = screen.display()
    if any("CUTOUT" in r for r in shown) and any("×" in r for r in shown):
        rows = shown
if rows is None:
    raise SystemExit("no frame showed the live tile")
head = [r for r in rows if "×" in r]
if len(head) != 1:
    raise SystemExit("the buttons are on %d rows: %r" % (len(head), rows))
if "…" not in head[0] or "a terminal session" not in head[0]:
    raise SystemExit("the title was not cut beside the buttons: %r" % head[0])
PY
    fail "the title of a narrow tile wrapped its buttons"
pass
