#!/bin/bash
# SPDX-License-Identifier: MIT
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start chat_stream.json

"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme catppuccin:dark \
    --set display/markdown=true --set display/stream=true \
    --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i

grep -qF "Streaming hello from the mock." "$TEST_DIR/pty.out" || \
    fail "assistant reply was not rendered in the PTY"
"$PYTHON" - "$TEST_DIR/pty.out" <<'EOF' || \
    fail "submitted user turn was not committed to the transcript"
import re
import sys

data = open(sys.argv[1], "rb").read()
if b"\x1b[38;2;127;132;156m" not in data:
    raise SystemExit("catppuccin status chrome did not reach libfytimui")
plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", data)
if "│ hello".encode() not in plain:
    raise SystemExit(1)
EOF
assert_request 0 'r["body"]["messages"][-1]["content"] == "hello"'
mock_stop 1
pass
