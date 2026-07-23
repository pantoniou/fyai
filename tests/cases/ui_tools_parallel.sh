#!/bin/bash
# SPDX-License-Identifier: MIT
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start ui_tools_parallel.json

FYAI_PTY_INPUT="run both" \
FYAI_PTY_PROGRESS_NEEDLE="parallel-early-a" \
FYAI_PTY_PROGRESS_TIMEOUT="0.3" \
FYAI_PTY_MID_NEEDLE=$'\033[32m●\033[0m' \
FYAI_PTY_MID_TIMEOUT="2.5" \
FYAI_PTY_NEEDLE="Parallel interactive tools done." \
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
for marker in (b"parallel-early-a", b"parallel-early-b",
               b"parallel-late-a", b"parallel-late-b"):
    if marker not in plain:
        raise SystemExit("missing parallel progress marker: %r" % marker)
EOF

assert_request 0 'r["body"]["parallel_tool_calls"] is True'
assert_request 1 \
	'all(any(m.get("tool_call_id") == call for m in '\
'r["body"]["messages"]) for call in '\
'("call_ui_parallel_a", "call_ui_parallel_b"))'
mock_stop 2
pass
