#!/bin/bash
# SPDX-License-Identifier: MIT
# A column of tiles that opens beside the transcript makes the transcript
# again at the columns it has left: a line made for the whole terminal wraps
# in the transcript and is not cut by the column.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start chat_long_line.json
driver=0
FYAI_TRACE="$TEST_DIR/trace.log" \
FYAI_PTY_ROWS=40 FYAI_PTY_COLS=180 FYAI_PTY_INPUT="say it long" \
FYAI_PTY_NEEDLE="TAILWORD" FYAI_PTY_TIMEOUT=20 \
FYAI_PTY_AFTER="wait-screen:TAILWORD|send:!sh -c 'printf %s%s SIDE OUT; sleep 60'|wait-screen:SIDEOUT|raw:1d|wait-gone:Ctrl-]|wait-screen:TAILWORD" \
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
    fail "the transcript was not seen beside the column of tiles"
fi

"$PYTHON" - "$TEST_DIR/pty.out" "$TESTS_DIR" <<'PY' ||
import sys

sys.path.insert(0, sys.argv[2])
from screen import Screen

END = b"\x1b[?2026l"
data = open(sys.argv[1], "rb").read()
screen = Screen(40, 180)
pos = 0
rows = None
while True:
    i = data.find(END, pos)
    if i < 0:
        break
    screen.feed(data[pos:i + len(END)])
    pos = i + len(END)
    shown = screen.display()
    if any("SIDEOUT" in r for r in shown):
        rows = shown
if rows is None:
    raise SystemExit("no frame showed the column of tiles")
# The column starts where the tile does; the transcript is left of it.
edge = min(r.find("SIDEOUT") for r in rows if "SIDEOUT" in r)
if not any("TAILWORD" in r[:edge] for r in rows):
    raise SystemExit("the long line was cut by the column: %r"
                     % [r for r in rows if "word00" in r])
PY
    fail "the transcript was not made again beside the column"
pass
