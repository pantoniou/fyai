#!/bin/bash
# SPDX-License-Identifier: MIT
# A continued interactive session replays recent history and reports the
# session token, cache, and cost figures in the status row.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start chat_stream.json

# The mock binds a new port on each start, so build the arguments each time.
set_args() {
	common_args=(-k test-key --theme catppuccin:dark
	    --set display/markdown=true --set display/stream=true
	    --set api=chat-completions
	    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i)
}
set_args

"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/first.out" \
    "$FYAI_BIN" "${common_args[@]}"
mock_stop 1

# The second session opens on the stored conversation.
mock_start recap_second.json
set_args
FYAI_PTY_INPUT="second" \
FYAI_PTY_NEEDLE="Second reply from the mock." \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/second.out" \
    "$FYAI_BIN" "${common_args[@]}"
mock_stop 1

"$PYTHON" - "$TEST_DIR/second.out" <<'EOF' || \
    fail "the interactive session did not replay recent history"
import re
import sys

data = open(sys.argv[1], "rb").read()
plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", data)
if b"hello" not in plain:
    raise SystemExit("the stored user turn was not replayed")
if b"Streaming hello from the mock." not in plain:
    raise SystemExit("the stored assistant reply was not replayed")
EOF

# Recap off keeps the bare opening.
mock_start recap_second.json
set_args
FYAI_PTY_INPUT="third" \
FYAI_PTY_NEEDLE="Second reply from the mock." \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/third.out" \
    "$FYAI_BIN" "${common_args[@]}" --set display/recap_exchanges=0
mock_stop 1

"$PYTHON" - "$TEST_DIR/third.out" <<'EOF' || \
    fail "display/recap_exchanges=0 still replayed history"
import re
import sys

data = open(sys.argv[1], "rb").read()
plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", data)
if b"Streaming hello from the mock." in plain:
    raise SystemExit("history was replayed with the recap disabled")
EOF

"$PYTHON" - "$TEST_DIR/second.out" <<'EOF' || \
    fail "the status row did not report the session usage"
import re
import sys

data = open(sys.argv[1], "rb").read()
plain = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", data)
# The mock model has no catalogue context window or price, so the context
# and cost fields do not apply here.
if not re.search(rb"cache \S+ \(\d+%\)", plain):
    raise SystemExit("the cache figure is missing")
EOF

exit 0
