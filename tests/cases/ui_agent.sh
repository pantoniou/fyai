#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify the delegated-agent label and the terminal it renders into.
#
# A sub-agent has a terminal of its own and draws its work there: its tool
# call and what the tool answered. The parent interprets that terminal and
# shows it in the band, behind the margin - the same way it shows a shell
# session, and not a rendering of its own.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_tool_responses.json

FYAI_PTY_INPUT="delegate a greeting to a sub-agent" \
FYAI_PTY_MID_NEEDLE="printf" \
FYAI_PTY_MID_TIMEOUT="8" \
FYAI_PTY_NEEDLE="Delegated and done." \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set tools=true --set api=responses --set builtin_shell=true \
    --set "api_url=$MOCK_URL/v1/responses" -m mock-model -i

"$PYTHON" - "$TEST_DIR/pty.out" <<'EOF' || \
    fail "agent presentation is wrong"
import os
import re
import sys

sys.path.insert(0, os.environ["TESTS_DIR"])
from screen import Screen

data = open(sys.argv[1], "rb").read()
# The sub-agent's terminal is drawn cell by cell, and the compositor writes
# only the cells that changed: a word can reach the capture in pieces, from
# several frames. What was on screen is the reading, not the byte stream.
# A sub-agent has a screen, and a screen is drawn over: what it showed while
# it worked is not all still there at the end. The reading is what was on it,
# so the capture is replayed and every state of it is read.
screen = Screen(30, 100)
seen = []
for i in range(0, len(data), 256):
    screen.feed(data[i:i + 256])
    seen.extend(screen.lines())
seen.extend(screen.lines())
shown = "\n".join(seen)

# The delegation is labelled by its name and short description, not the task.
if "[greeter]" not in shown:
    raise SystemExit("agent name annotation missing from the label")
if "greet and report" not in shown:
    raise SystemExit("agent description missing from the label")
if "print a greeting with the shell" in shown:
    raise SystemExit("agent task prose leaked into the transcript")

# The sub-agent renders to a terminal of its own, and that terminal is what
# the band shows: its tool call, and what the tool answered. Every row of it
# carries the margin that says whose screen it is.
if "printf" not in shown:
    raise SystemExit("the sub-agent's tool call was not shown")
if "agent-was-here" not in shown:
    raise SystemExit("the sub-agent's own screen was not shown")
if not re.search(r"\u2502 .*printf", shown):
    raise SystemExit("the sub-agent screen carries no margin")

if "Delegated and done." not in shown:
    raise SystemExit("parent final answer missing")
EOF

mock_stop 4
pass
