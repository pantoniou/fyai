#!/bin/bash
# SPDX-License-Identifier: MIT
# With display/tool_display=inline, a delegated sub-agent draws its screen in
# the transcript of the page ($FYAI_INLINE_SCREEN, fullscreen by default),
# under the head of its call, and not in a tile of the work pane.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_tool_responses.json

FYAI_TRACE="$TEST_DIR/trace.log" \
FYAI_PTY_INPUT="delegate a greeting to a sub-agent" \
FYAI_PTY_NEEDLE="agent-was-here" FYAI_PTY_TIMEOUT=30 \
FYAI_PTY_AFTER="wait-screen:agent-was-here|snapshot|wait-screen:Delegated and done." \
FYAI_PTY_SNAPSHOT="$TEST_DIR/live.out" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set "display/screen=${FYAI_INLINE_SCREEN:-fullscreen}" --set display/tool_display=inline \
    --set tools=true --set api=responses --set builtin_shell=true \
    --set "api_url=$MOCK_URL/v1/responses" -m mock-model -i

"$PYTHON" - "$TEST_DIR/live.out" "$TESTS_DIR" <<'PY' || fail "the sub-agent screen is not in the transcript"
import sys

sys.path.insert(0, sys.argv[2])
from screen import Screen

s = Screen(30, 100)
s.feed(open(sys.argv[1], "rb").read())
rows = [r.rstrip() for r in s.display() if r.strip()]


def index(pred, what, start=0):
    for i in range(start, len(rows)):
        if pred(rows[i]):
            return i
    raise SystemExit("no %s on screen:\n%s" % (what, "\n".join(rows)))


# The rows above the call are not read: a terminal library built with
# DEBUG reports the sequences it does not handle on standard error, which
# scrolls the screen of the test.
head = index(lambda r: "agent [greeter]" in r, "head of the call")
index(lambda r: r.strip() == "agent-was-here", "screen of the sub-agent", head)
PY

grep -q " page: " "$TEST_DIR/trace.log" || fail "the trace has no page"
if grep -E " page: .* tiles=[1-9]" "$TEST_DIR/trace.log" >/dev/null; then
    fail "the sub-agent opened a tile of the work pane"
fi

mock_stop 4
pass
