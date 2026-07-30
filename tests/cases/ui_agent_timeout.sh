#!/bin/bash
# SPDX-License-Identifier: MIT
# Commit a sub-agent timeout as a failure.
#
# The UI selects the mark before collection identifies an expired deadline.
# The child can report success immediately before the parent stops it. The
# committed mark must use the timeout state from the parent.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agent_timeout.json

FYAI_PTY_INPUT="delegate a task that does not end" \
FYAI_PTY_NEEDLE="The sub-agent was stopped." \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set tools=true --set api=responses --set builtin_shell=true \
    --set agent/timeout_ms=1500 \
    --set "api_url=$MOCK_URL/v1/responses" -m mock-model -i

"$PYTHON" - "$TEST_DIR/pty.out" <<'EOF' || fail "agent failure is not marked"
import re
import sys

data = open(sys.argv[1], "rb").read()

# The committed delegation row contains the failure mark.
rows = re.findall(rb"\x1b\[3([12])m\xe2\x97\x8f[^\n]*?\x1b\[1magent", data)
if not rows:
    raise SystemExit("no committed agent row found")
if b"2" in rows:
    raise SystemExit("a stopped sub-agent was marked as a success")
EOF

mock_stop 3
pass
