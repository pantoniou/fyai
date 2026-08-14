#!/bin/bash
# SPDX-License-Identifier: MIT
# Each call of a parallel group shows its own result under its own band.
#
# A band commits directly to the scrollback, while a tool result goes to the
# output spool. Without a drain between the two calls, the group showed both
# invocations first and both results after them.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start ui_parallel_result_order.json

FYAI_PTY_INPUT="run both" \
FYAI_PTY_NEEDLE="Both parallel sleep commands" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
	"$FYAI_BIN" -k test-key --theme dark \
	--set display/markdown=true --set display/stream=false \
	--set display/tool_preview_lines=5 --set tools=true \
	--set api=responses --set builtin_shell=true \
	--set "api_url=$MOCK_URL/v1/responses" \
	-m mock-model -i

"$PYTHON" - "$TEST_DIR/pty.out" "$TESTS_DIR" <<'EOF' || fail "parallel results were not shown under their own band"
import sys
sys.path.insert(0, sys.argv[2])
from screen import Screen

screen = Screen()
screen.feed(open(sys.argv[1], "rb").read())
order = []
for line in screen.lines():
    if "sleep 10" in line:
        order.append("call-a")
    elif "sleep 5" in line:
        order.append("call-b")
    elif "timed out after 1000 ms; request" in line:
        order.append("result")
# A band repaints while it runs, so keep the first of each repeated row.
seen = [tag for i, tag in enumerate(order) if i == 0 or order[i - 1] != tag]
want = ["call-a", "result", "call-b", "result"]
if seen != want:
    raise SystemExit("rows out of order: %r" % (seen,))
EOF

mock_stop 2
pass
