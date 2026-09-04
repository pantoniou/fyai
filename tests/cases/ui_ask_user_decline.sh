#!/bin/bash
# SPDX-License-Identifier: MIT
# Escape declines a pending ask_user question instead of the turn or the
# session: the model gets a "declined" tool result, the prompt returns to
# its normal shape, and the session keeps running.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start ask_user.json

FYAI_PTY_INPUT="ask me something" \
FYAI_PTY_NEEDLE="Proceed with the mock plan?" \
FYAI_PTY_AFTER="settle:0.4|raw:1b|wait:User said yes, proceeding." \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set tools=true --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i

# The driver itself already proves the session did not hang: it requires a
# clean /exit at the end, which used to never come back once Escape hit a
# pending question (the marker just spun forever).
grep -q "the user declined to answer" "$TEST_DIR/pty.out" ||
	fail "the decline was never shown to the model"

"$PYTHON" - "$TEST_DIR/pty.out" "$TESTS_DIR" <<'EOF' || fail "the prompt did not return to its normal shape after declining"
import sys

sys.path.insert(0, sys.argv[2])
from screen import Screen

data = open(sys.argv[1], "rb").read()


def banner_restored(rows):
    return "❯" in rows and any("mock-model" in r and "·" in r for r in rows)


screen = Screen(30, 100)
restored_ok = False
pos = 0
while pos < len(data):
    screen.feed(data[pos:pos + 1])
    pos += 1
    if banner_restored([r for r in screen.display() if r.strip()]):
        restored_ok = True
        break

if not restored_ok:
    raise SystemExit("prompt marker/banner were never restored after "
                      "declining")
EOF

assert_request 1 'any(m.get("role") == "tool" and m.get("tool_call_id") == "call_ask_1" and "declined" in m.get("content", "") for m in r["body"]["messages"])'

mock_stop 2
pass
