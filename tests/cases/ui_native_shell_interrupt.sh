#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that interruption retains the native-shell result and partial output.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start native_shell_interrupt.json

start=$(date +%s)
FYAI_PTY_INPUT="run it" \
FYAI_PTY_PROGRESS_NEEDLE="nsiRAN" \
FYAI_PTY_PROGRESS_TIMEOUT="6" \
FYAI_PTY_INTERRUPT_AFTER_PROGRESS="1" \
FYAI_PTY_NEEDLE="interrupted" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set tools=true --set builtin_shell=true --set api=responses \
    --set "api_url=$MOCK_URL/v1/responses" -m mock-model -i
elapsed=$(($(date +%s) - start))

[ "$elapsed" -lt 8 ] || fail "ESC did not interrupt the builtin shell"

"$PYTHON" - "$TEST_DIR/pty.out" <<'EOF' || \
    fail "the interrupted shell was not reported"
import re
import sys

data = open(sys.argv[1], "rb").read()
plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", data)
# The command's own output reached the band before the interrupt.
if b"nsiRAN" not in plain:
    raise SystemExit("output collected before the interrupt was lost")
if b"interrupted" not in plain:
    raise SystemExit("the interrupt was not reported")
EOF

mock_stop 1

# Verify the signal outcome and the output marker in the retained result.
run_fyai history --raw
assert_status 0
assert_stdout_contains "signal"
assert_stdout_contains "nsiRAN"
pass
