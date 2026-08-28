#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that initial replay fills the screen with complete recent exchanges.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

common_args=()
set_args() {
	common_args=(-k test-key --theme dark
	    --set display/markdown=true --set display/stream=true
	    --set api=chat-completions
	    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i)
}

# Store more exchanges than one screen can hold.
for _ in 1 2 3; do
	mock_start ui_reflow.json
	set_args
	FYAI_PTY_INPUT="say it" \
	FYAI_PTY_NEEDLE="Reflow streaming done." \
	FYAI_PTY_TIMEOUT=40 \
	"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/build.out" \
	    "$FYAI_BIN" "${common_args[@]}"
	mock_stop 1
done

# Open a session without submitting another turn.
mock_start ui_reflow.json
set_args
FYAI_PTY_INPUT="   " \
FYAI_PTY_NEEDLE="mock-model" \
FYAI_PTY_TIMEOUT=40 \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/opened.out" \
    "$FYAI_BIN" "${common_args[@]}"
mock_stop 0

"$PYTHON" - "$TEST_DIR/opened.out" <<'PY' || fail "the opening screen was not filled"
import os
import sys

sys.path.insert(0, os.environ["TESTS_DIR"])
from screen import Screen

ROWS = 30
screen = Screen(ROWS, 100)
screen.feed(open(sys.argv[1], "rb").read())
used = sum(1 for r in screen.display() if r.strip())
# Allow rows for prompt and status chrome.
if used < ROWS // 2:
    raise SystemExit("only %d of %d rows were used" % (used, ROWS))
PY

pass
