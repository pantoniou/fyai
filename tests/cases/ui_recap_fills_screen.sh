#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that initial replay fills the screen with complete recent exchanges.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

common_args=()
set_args() {
	common_args=(-b main -k test-key --theme dark
	    --set display/markdown=true --set display/stream=true
	    --set api=chat-completions
	    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i)
}

# The exchanges only have to be stored, so the stream comes at once.
"$PYTHON" - "$SCENARIOS_DIR/ui_reflow.json" "$TEST_DIR/reflow-fast.json" <<'PY'
import json
import sys

scenario = json.load(open(sys.argv[1]))
for step in scenario["steps"]:
    step.pop("chunk_split", None)
    step.pop("chunk_delay", None)
with open(sys.argv[2], "w") as out:
    json.dump(scenario, out)
PY

# Store more exchanges than one screen can hold. The session recaps the stored
# exchanges, which hold the same reply: each run waits for one more copy of it.
for _ in 1 2 3; do
	mock_start "$TEST_DIR/reflow-fast.json"
	set_args
	pty_run "$TEST_DIR/build.out" \
	    "$FYAI_BIN" "${common_args[@]}" <<'PTY'
timeout: 40
steps:
  - type: say it
  - wait-count: Reflow streaming done.
    more: 1
    timeout: session
PTY
	mock_stop 1
done

# Open a session without submitting another turn.
mock_start ui_reflow.json
set_args
pty_run "$TEST_DIR/opened.out" \
    "$FYAI_BIN" "${common_args[@]}" <<'PTY'
timeout: 40
steps:
  - type: "   "
  - wait: mock-model
    timeout: session
PTY
mock_stop 0

"$PYTHON" - "$TEST_DIR/opened.out" <<'PY' || fail "the opening screen was not filled"
import os
import sys

sys.path.insert(0, os.environ["TESTS_DIR"])
from screen import Screen

ROWS = 30
screen = Screen(ROWS, 100)
data = open(sys.argv[1], "rb").read()
# Read the opening screen: the exit removes the chrome under it.
end = data.find(b"/exit")
screen.feed(data if end < 0 else data[:end])
used = sum(1 for r in screen.display() if r.strip())
# Allow rows for prompt and status chrome.
if used < ROWS // 2:
    raise SystemExit("only %d of %d rows were used" % (used, ROWS))
PY

pass
