#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify the delegated-agent label and hidden child tools.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_tool_responses.json

FYAI_PTY_INPUT="delegate a greeting to a sub-agent" \
FYAI_PTY_MID_NEEDLE="printf" \
FYAI_PTY_MID_TIMEOUT="1" \
FYAI_PTY_NEEDLE="Delegated and done." \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set tools=true --set api=responses --set builtin_shell=true \
    --set "api_url=$MOCK_URL/v1/responses" -m mock-model -i

"$PYTHON" - "$TEST_DIR/pty.out" <<'EOF' || \
    fail "agent presentation is wrong"
import re
import sys

data = open(sys.argv[1], "rb").read()
plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", data)
# The delegation is labelled by its name and short description, not the task.
if b"[greeter]" not in plain:
    raise SystemExit("agent name annotation missing from the label")
if b"greet and report" not in plain:
    raise SystemExit("agent description missing from the label")
if b"print a greeting with the shell" in plain:
    raise SystemExit("agent task prose leaked into the transcript")
# Show the agent tool call in the quoted progress display.
if b"printf" not in plain:
    raise SystemExit("agent tool call missing from the progress display")
# Do not show the result from the agent tool.
if b"agent-was-here" in plain:
    raise SystemExit("agent tool result leaked into the progress display")
if b"Delegated and done." not in plain:
    raise SystemExit("parent final answer missing")
EOF

mock_stop 4
pass
