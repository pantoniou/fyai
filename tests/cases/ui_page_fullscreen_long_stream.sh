#!/bin/bash
# SPDX-License-Identifier: MIT
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start chat_fullscreen_long_stream.json
FYAI_PTY_ROWS=20 FYAI_PTY_COLS=100 FYAI_PTY_INPUT="first question" \
FYAI_PTY_NEEDLE="stream" \
FYAI_PTY_AFTER="wait-screen:stream line 10|release:$TEST_DIR/resume-stream|wait-screen:stream line 20" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=true \
    --set display/renderer=page --set display/screen=fullscreen \
    --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i

if ! "$PYTHON" - "$TEST_DIR/pty.out" "$TESTS_DIR" <<'PY'
import sys

sys.path.insert(0, sys.argv[2])
from screen import Screen

data = open(sys.argv[1], "rb").read()
screen = Screen(20, 100)
end = b"\x1b[?2026l"
pos = 0
partial = final = False
while True:
    at = data.find(end, pos)
    if at < 0:
        break
    screen.feed(data[pos:at + len(end)])
    pos = at + len(end)
    rows = screen.display()
    first = sum(f"stream line {i:02}" in " ".join(rows)
                for i in range(1, 11))
    last = sum(f"stream line {i:02}" in " ".join(rows)
               for i in range(11, 21))
    if first >= 6 and last == 0:
        partial = True
    if last >= 6 and "stream line 20" in " ".join(rows):
        final = True
if not (partial and final):
    raise SystemExit(f"partial viewport {partial}, final viewport {final}")
PY
then
    fail "the fullscreen stream did not scroll through its transcript"
fi

mock_stop 1
pass
