#!/bin/bash
# SPDX-License-Identifier: MIT
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start chat_stream.json

"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=true \
    --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i

grep -qF "Streaming hello from the mock." "$TEST_DIR/pty.out" || \
    fail "assistant reply was not rendered in the PTY"
assert_request 0 'r["body"]["messages"][-1]["content"] == "hello"'
mock_stop 1
pass
