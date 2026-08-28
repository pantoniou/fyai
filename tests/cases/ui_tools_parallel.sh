#!/bin/bash
# SPDX-License-Identifier: MIT
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start ui_tools_parallel.json

FYAI_PTY_INPUT="run both" \
FYAI_PTY_PROGRESS_NEEDLE="parallel-early-a" \
FYAI_PTY_PROGRESS_TIMEOUT="3" \
FYAI_PTY_MID_NEEDLE=$'\033[32m●' \
FYAI_PTY_MID_TIMEOUT="5" \
FYAI_PTY_NEEDLE="Parallel interactive tools done." \
FYAI_PTY_AFTER="send:/status|wait:Usage / total" \
FYAI_PTY_SNAPSHOT="$TEST_DIR/snapshot.out" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
	"$FYAI_BIN" -k test-key --theme dark \
	--set display/markdown=true --set display/stream=false \
	--set display/tool_preview_lines=5 --set tools=true \
	--set api=chat-completions \
	--set "api_url=$MOCK_URL/v1/chat/completions" \
	-m mock-model -i

"$PYTHON" - "$TEST_DIR/pty.out" <<'EOF' || \
	fail "parallel shell workbands did not update progressively"
import re
import sys

data = open(sys.argv[1], "rb").read()
plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", data)
for marker in (b"parallel-early-a", b"parallel-early-b", b"parallel-still-b",
               b"parallel-late-a", b"parallel-late-b"):
    if marker not in plain:
        raise SystemExit("missing parallel progress marker: %r" % marker)
EOF

"$PYTHON" - "$TEST_DIR/pty.out" <<'EOF' || \
	fail "the first completed parallel shell was not retired promptly"
import os
import sys

sys.path.insert(0, os.environ["TESTS_DIR"])
from tile_assert import frames

data = open(sys.argv[1], "rb").read()
# Require a frame with committed A and live B alone in the pane.
retired = False
for disp in frames(data, 30, 100):
    shown = "\n".join(disp)
    if "parallel-still-b" not in shown:
        continue
    fast_rows = [i for i, line in enumerate(disp) if "fast counter" in line]
    slow_rows = [i for i, line in enumerate(disp) if "slow counter" in line]
    if not fast_rows or not slow_rows:
        continue
    # Reject A remaining as B's sibling tile after commit.
    separate = not set(fast_rows) & set(slow_rows)
    fast_output = any("parallel-late-a" in line for line in disp)
    if separate and fast_output:
        retired = True
if not retired:
    raise SystemExit("fast shell was not committed while the slow shell ran")
EOF

"$PYTHON" "$TESTS_DIR/assert_work_retired.py" \
    "$TEST_DIR/snapshot.out" "● shell" || fail "completed parallel shells remained in the work pane"

assert_request 0 'r["body"]["parallel_tool_calls"] is True'
assert_request 1 \
	'all(any(m.get("tool_call_id") == call for m in '\
'r["body"]["messages"]) for call in '\
'("call_ui_parallel_a", "call_ui_parallel_b"))'
mock_stop 2
pass
