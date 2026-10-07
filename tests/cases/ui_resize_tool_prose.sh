#!/bin/bash
# SPDX-License-Identifier: MIT
# The prose before a tool call stays above the call when the window changes
# width while the call runs: a reflow renders only the prose that follows
# the finished rows, and never draws the rows above again under the call.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start ui_resize_tool_prose.json

FYAI_PTY_INPUT="delegate a greeting to a sub-agent" \
FYAI_PTY_NEEDLE="delegate a" FYAI_PTY_TIMEOUT=30 \
FYAI_PTY_AFTER="wait-screen:agent [greeter]|resize:80|wait-screen:@1|snapshot|release:$TEST_DIR/release-child|wait-screen:Delegated and done." \
FYAI_PTY_SNAPSHOT="$TEST_DIR/live.out" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=true \
    --set display/screen=fullscreen --set display/tool_display=inline \
    --set tools=true --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i

"$PYTHON" - "$TEST_DIR/live.out" "$TESTS_DIR" <<'PY' || fail "the prose before the call was drawn again under it"
import sys

sys.path.insert(0, sys.argv[2])
from screen import Screen

s = Screen(30, 80)
s.feed(open(sys.argv[1], "rb").read())
rows = [r.rstrip() for r in s.display() if r.strip()]
text = "\n".join(rows)
head = next((i for i, r in enumerate(rows) if "agent [greeter]" in r), None)
if head is None:
    raise SystemExit("no head of the call on screen:\n" + text)
for line in ("I will look at the diff first.",
             "Starting a review agent on the last commit."):
    n = sum(line in r for r in rows)
    if n != 1:
        raise SystemExit("%r is on screen %d times:\n%s" % (line, n, text))
    at = next(i for i, r in enumerate(rows) if line in r)
    if at > head:
        raise SystemExit("%r is under the call:\n%s" % (line, text))
PY

mock_stop 3
pass
