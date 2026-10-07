#!/bin/bash
# SPDX-License-Identifier: MIT
# With display/tool_display=inline, Ctrl-T gives the keys to a terminal block
# of the transcript: the prompt loses its edge and each row of the block takes
# it, what is typed reaches its program, and Ctrl-] gives the keys back to
# the prompt. The page is
# $FYAI_INLINE_SCREEN, fullscreen by default.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start inline_tool_focus.json

FYAI_TRACE="$TEST_DIR/trace.log" \
FYAI_PTY_INPUT="start a program" FYAI_PTY_NEEDLE="ready." FYAI_PTY_TIMEOUT=30 \
FYAI_PTY_AFTER="wait-screen:ready.|raw:14|wait-screen:Ctrl-] returns|"\
"wait-gone:▌❯|wait-row:▌|send:typed-into-cat|wait-row:▌   typed-into-cat|"\
"raw:1d|wait-gone:Ctrl-] returns|wait-screen:▌❯" \
FYAI_PTY_AFTER_PAUSE=0 FYAI_PTY_AFTER_TIMEOUT=10 \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -b main -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set "display/screen=${FYAI_INLINE_SCREEN:-fullscreen}" \
    --set display/tool_display=inline \
    --set tools=true --set api=chat-completions \
    --set shell/input_poll_ms=0 \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i

# The program read what was typed: the terminal echoes it, and cat writes it
# back.
"$PYTHON" - "$TEST_DIR/pty.out" "$TESTS_DIR" <<'PY' || fail "the typed line did not reach the program"
import sys

sys.path.insert(0, sys.argv[2])
from screen import Screen

data = open(sys.argv[1], "rb").read()
best = 0
end = data.find(b"\x1b[?2026l")
while end >= 0:
    s = Screen(30, 100)
    s.feed(data[:end + 8])
    best = max(best, sum(1 for r in s.display()
                         if r.strip().lstrip("▌").strip() ==
                         "typed-into-cat"))
    end = data.find(b"\x1b[?2026l", end + 1)
if best < 2:
    raise SystemExit("the program echoed the line %d times" % best)
PY

mock_stop 2
pass
