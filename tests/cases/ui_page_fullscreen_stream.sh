#!/bin/bash
# SPDX-License-Identifier: MIT
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start chat_fullscreen_stream.json
FYAI_PTY_ROWS=20 FYAI_PTY_COLS=100 FYAI_PTY_INPUT="first question" \
FYAI_PTY_NEEDLE="Streaming" \
FYAI_PTY_AFTER="wait-screen:Streaming begins here.|release:$TEST_DIR/resume-stream|wait-screen:continues" \
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
live = final = None
while True:
    at = data.find(end, pos)
    if at < 0:
        break
    screen.feed(data[pos:at + len(end)])
    pos = at + len(end)
    rows = screen.display()
    complete = any("And continues." in row for row in rows)
    for y, row in enumerate(rows):
        if "Streaming begins here." not in row:
            continue
        if complete:
            final = y
        else:
            live = y
if live is None or final is None or live != final:
    raise SystemExit(f"live row {live}, committed row {final}")
PY
then
    fail "the live fullscreen response moved when it committed"
fi

mock_stop 1
pass
