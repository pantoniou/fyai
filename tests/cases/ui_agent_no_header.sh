#!/bin/bash
# SPDX-License-Identifier: MIT
# A delegated sub-agent draws its work on a terminal of its own, which the
# parent shows in a tile. The head of the tile names the agent, so the screen
# of the agent has no header row of its own: no frame shows a header that
# names the branch of the agent.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_tool_responses.json
driver=0
FYAI_PTY_INPUT="delegate a greeting to a sub-agent" \
FYAI_PTY_NEEDLE="Delegated and done." \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set tools=true --set api=responses --set builtin_shell=true \
    --set "api_url=$MOCK_URL/v1/responses" -m mock-model -i || driver=$?
if [ "$driver" -ne 0 ]; then
    tail -c 2000 "$TEST_DIR/pty.out" >&2
    fail "the delegation did not finish"
fi
"$PYTHON" - "$TEST_DIR/pty.out" "$TESTS_DIR" <<'PY' ||
import sys

sys.path.insert(0, sys.argv[2])
from screen import Screen

END = b"\x1b[?2026l"
data = open(sys.argv[1], "rb").read()
screen = Screen(30, 100)
pos = 0
tile = False
while True:
    i = data.find(END, pos)
    if i < 0:
        break
    screen.feed(data[pos:i + len(END)])
    pos = i + len(END)
    for row in screen.display():
        # The head of the tile names the agent and its task.
        if "agent [greeter]" in row:
            tile = True
        if "fyai:" in row and "agent:greeter" in row:
            raise SystemExit("the agent drew a header: %r" % row)
if not tile:
    raise SystemExit("no frame showed the tile of the agent")
PY
    fail "a delegated sub-agent drew a header row"
pass
