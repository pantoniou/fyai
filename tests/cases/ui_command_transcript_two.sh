#!/bin/bash
# SPDX-License-Identifier: MIT
# Send two commands before the first reply. Each stored turn must contain
# the output for its own command.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start ui_card_separation.json
pty_run "$TEST_DIR/pty.out" \
	"$FYAI_BIN" -k test-key --theme dark \
	--set display/markdown=true --set display/stream=true \
	--set display/screen=fullscreen --set display/command_output=transcript \
	--set api=chat-completions \
	--set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -b main -i <<'PTY'
rows: 60
cols: 100
steps:
  - type: first question
  - wait: banana
    timeout: session
  - wait-screen: "❯"
  # One write: both lines are in the queue before the worker answers.
  - write: "/stats\n/context\n"
  - wait-screen: "Context"
  - wait-screen: "Calls"
  - wait-screen: "❯"
  - type: "yellow?"
  - wait-screen: second
PTY
mock_stop 2

# Check that the output for each command appears under its own heading.
run_fyai --color off -b main transcript
assert_status 0
"$PYTHON" - "$TEST_DIR/stdout" <<'PY' || fail "a command was stored with the output of the other"
import sys

text = open(sys.argv[1]).read()
stats = text.index("> /stats")
context = text.index("> /context")
if not stats < text.index("Cache write tokens") < context:
    raise SystemExit("the output of /stats is not under /stats")
if not context < text.index("Prompt estimated"):
    raise SystemExit("the output of /context is not under /context")
PY
pass
