#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that a tool child has no terminal on stdin and no controlling terminal.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start tool_child_stdin.json

FYAI_PTY_INPUT="run it" \
FYAI_PTY_NEEDLE="stdin probe done." \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set builtin_shell=true --set api=responses \
    --set "api_url=$MOCK_URL/v1/responses" -m mock-model -i

# The tool must not read from the terminal.
out='r["body"]["input"][-1]["output"][0]["stdout"]'
assert_request 1 "'STDIN=eof' in $out"
assert_request 1 "'CTTY=no' in $out"

mock_stop 2
pass
