#!/bin/bash
# SPDX-License-Identifier: MIT
# A side question runs during the parent turn and keeps its own branch.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start btw_midturn.json

FYAI_PTY_INPUT="main question" \
FYAI_PTY_DURING_INPUT="/btw side question" \
FYAI_PTY_PROGRESS_NEEDLE="main question" \
FYAI_PTY_NEEDLE="SIDE-ANSWER" \
FYAI_PTY_AFTER="wait-screen:Esc closes|raw:1b5b357e1b5b357e1b5b357e|wait-screen:SIDE-START|snapshot|raw:1b|wait-gone:SIDE-START|release:$TEST_DIR/release-main|wait-screen:MAIN-ANSWER" \
FYAI_PTY_AFTER_TIMEOUT="10" \
FYAI_PTY_SNAPSHOT="$TEST_DIR/btw-panel.out" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set api=responses --set "api_url=$MOCK_URL/v1/responses" \
    -m mock-model -i

"$PYTHON" - "$TEST_DIR/btw-panel.out" <<'EOF' || \
    fail "the completed side answer did not stay in its panel"
import os
import sys
sys.path.insert(0, os.environ["TESTS_DIR"])
from screen import Screen

screen = Screen(30, 100)
screen.feed(open(sys.argv[1], "rb").read())
if "SIDE-START" not in "\n".join(screen.lines()):
    raise SystemExit("the completed side answer did not scroll")
EOF

branch="$($PYTHON - "$TEST_DIR/pty.out" <<'EOF'
import re
import sys
data = open(sys.argv[1], "rb").read()
match = re.search(rb"btw: (session/[A-Za-z0-9./:-]+/agent:btw-1)", data)
if not match:
    raise SystemExit("side branch was not shown")
print(match.group(1).decode())
EOF
)"
parent="${branch%/agent:btw-1}"

run_fyai --branch "$branch" transcript
assert_status 0
assert_stdout_contains "side question"
assert_stdout_contains "SIDE-ANSWER"

run_fyai --branch "$parent" transcript
assert_status 0
assert_stdout_contains "MAIN-ANSWER"
assert_stdout_not_contains "SIDE-ANSWER"

[ ! -f "$TEST_DIR/stream-wait-failed" ] ||
    fail "a held mock reply was never released"

mock_stop 2
pass
