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

EDITOR=false \
FYAI_PTY_INPUT="/config edit" \
FYAI_PTY_NEEDLE="editor exited unsuccessfully" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/error-pane.out" \
    "$FYAI_BIN" -k test-key --theme catppuccin:dark \
    --set display/markdown=true -m mock-model -i

"$PYTHON" - "$TEST_DIR/error-pane.out" <<'EOF' || \
    fail "config editor failure was not rendered in a non-modal error pane"
import re
import sys

data = open(sys.argv[1], "rb").read()
plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", data)
if "● config".encode() not in plain:
    raise SystemExit("error pane heading missing")
if b"editor exited unsuccessfully" not in plain:
    raise SystemExit("error pane detail missing")
EOF

FYAI_PTY_INPUT="/status" \
FYAI_PTY_NEEDLE="● status" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/status-pane.out" \
    "$FYAI_BIN" -k test-key --theme catppuccin:dark \
    --set display/markdown=true -m mock-model -i

"$PYTHON" - "$TEST_DIR/status-pane.out" <<'EOF' || \
    fail "slash-command status was not rendered in a non-modal pane"
import re
import sys

data = open(sys.argv[1], "rb").read()
plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", data)
if "● status".encode() not in plain or b"Usage / total" not in plain:
    raise SystemExit("status pane content missing")
EOF

FYAI_PTY_INPUT="/config describe display" \
FYAI_PTY_NEEDLE="tool_detail" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/config-scroll.out" \
    "$FYAI_BIN" -k test-key --theme catppuccin:dark \
    --set display/markdown=true -m mock-model -i

"$PYTHON" - "$TEST_DIR/config-scroll.out" <<'EOF' || \
    fail "config report was incorrectly rendered as a status pane"
import re
import sys

data = open(sys.argv[1], "rb").read()
plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", data)
if "● config".encode() in plain:
    raise SystemExit("config report received status-pane chrome")
if b"tool_detail" not in plain:
    raise SystemExit("config report did not reach terminal scrollback")
EOF

mock_start chat_stream_queued_input.json
FYAI_PTY_INPUT="first prompt" \
FYAI_PTY_DURING_INPUT="queued prompt" \
FYAI_PTY_DURING_DELAY="0.2" \
FYAI_PTY_NEEDLE="Queued input completed." \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/queued-input.out" \
    "$FYAI_BIN" -k test-key --theme catppuccin:dark \
    --set display/markdown=true --set display/stream=true \
    --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i
assert_request 0 \
    'r["body"]["messages"][-1]["content"] == "first prompt"'
assert_request 1 \
    'r["body"]["messages"][-1]["content"] == "queued prompt"'
mock_stop 2

FYAI_PTY_INPUT="/transcript all" \
FYAI_PTY_NEEDLE="Queued input completed." \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/transcript-order.out" \
    "$FYAI_BIN" -k test-key --theme catppuccin:dark \
    --set display/markdown=true -m mock-model -i

"$PYTHON" - "$TEST_DIR/transcript-order.out" <<'EOF' || \
    fail "slash transcript did not preserve turn order"
import re
import sys

data = open(sys.argv[1], "rb").read()
plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", data)
needles = [
    b"first prompt",
    b"First streamed reply.",
    b"queued prompt",
    b"Queued input completed.",
]
position = 0
for needle in needles:
    position = plain.find(needle, position)
    if position < 0:
        raise SystemExit("slash transcript omitted or reordered %r" % needle)
    position += len(needle)
EOF

mock_start chat_stream_queued_input.json
FYAI_PTY_INPUT="first prompt" \
FYAI_PTY_DURING_INPUT="unfinished typing" \
FYAI_PTY_DURING_SUBMIT="0" \
FYAI_PTY_CLEAR_BEFORE_EXIT="1" \
FYAI_PTY_DURING_DELAY="0.2" \
FYAI_PTY_NEEDLE="First streamed reply." \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/typing-input.out" \
    "$FYAI_BIN" -k test-key --theme catppuccin:dark \
    --set display/markdown=true --set display/stream=true \
    --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i
assert_request 0 \
    'r["body"]["messages"][-1]["content"] == "first prompt"'
mock_stop 1

mock_start chat_stream_queued_input.json
FYAI_PTY_INPUT="first prompt" \
FYAI_PTY_DURING_INPUT="recalled prompt" \
FYAI_PTY_DURING_DELAY="0.2" \
FYAI_PTY_INTERRUPT_AFTER_DURING="1" \
FYAI_PTY_SUBMIT_RECALLED="1" \
FYAI_PTY_NEEDLE="Queued input completed." \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/interrupt-recall.out" \
    "$FYAI_BIN" -k test-key --theme catppuccin:dark \
    --set display/markdown=true --set display/stream=true \
    --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i
assert_request 0 \
    'r["body"]["messages"][-1]["content"] == "first prompt"'
assert_request 1 \
    'r["body"]["messages"][-1]["content"] == "recalled prompt"'
mock_stop 2

mock_start chat_stream.json
FYAI_PTY_INPUT="   " \
FYAI_PTY_NEEDLE="interactive" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/blank-input.out" \
    "$FYAI_BIN" -k test-key --theme catppuccin:dark \
    --set display/markdown=true --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i
mock_stop 0

pass
