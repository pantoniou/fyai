#!/bin/bash
# SPDX-License-Identifier: MIT
# Read and write tool calls use the live activity and completion indicators.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start ui_file_tool_marks.json
printf 'read marker\n' > input.txt

FYAI_PTY_INPUT="read and write files" \
FYAI_PTY_NEEDLE="File tool marks done." \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set tools=true --set api=responses \
    --set "api_url=$MOCK_URL/v1/responses" -m mock-model -i

"$PYTHON" - "$TEST_DIR/pty.out" <<'EOF' || \
    fail "file tool activity marks are missing"
import re
import sys

data = open(sys.argv[1], "rb").read()
plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", data)
if not re.search(rb"\xe2\x97\x8f read\s+input\.txt", plain):
    raise SystemExit("read tool has no activity or completion mark")
if not re.search(rb"\xe2\x97\x8f write\s+output\.txt", plain):
    raise SystemExit("write tool has no activity or completion mark")
if not re.search(rb"\x1b\[32m\xe2\x97\x8f[^\n]*read", data):
    raise SystemExit("read tool has no success mark")
if not re.search(rb"\x1b\[32m\xe2\x97\x8f[^\n]*write", data):
    raise SystemExit("write tool has no success mark")
EOF

grep -qF "written marker" output.txt || fail "write tool did not run"
mock_stop 2
pass
