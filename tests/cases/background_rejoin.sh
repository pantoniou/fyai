#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that rejoining a backgrounded session mid-tool and issuing /background
# again leaves the tool running and completes the turn cleanly on the server.
set -eu
. "$(dirname "$0")/../harness.sh"
fyai_test_setup
mock_start background_tool.json
export MOCK_KEY=test-key
"$FYAI_BIN" --set api=chat-completions --set display/stream=false \
    --set tools=true \
    --set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model \
    --set 'api_key={type: env, value: MOCK_KEY}' >/dev/null 2>&1
mkfifo tool.fifo

pty_run "$TEST_DIR/pty.out" "$FYAI_BIN" -b main -i <<'PTY'
steps:
  - type: Run the held tool
  - wait-screen: "tool.fifo"
    timeout: session
  - type: /background
  - wait: "continues in the background"
    timeout: session
PTY

pty_run "$TEST_DIR/pty2.out" "$FYAI_BIN" resume <<'PTY'
ready: toggle foreign sessions
steps:
  - wait-screen: "[live · running]"
    timeout: session
  - write: "\r"
  - wait-gone: "toggle foreign sessions"
  - wait-screen: "tool.fifo"
  - type: /background
  - wait: "continues in the background"
    timeout: session
PTY

# Closing the FIFO unblocks the tool so the server can complete the turn.
exec 9<>tool.fifo
exec 9>&-
log=.fyai/logs/serve.log
pid="$(sed -n 's/.*serve\[\([0-9]*\)\]: serving.*/\1/p' "$log" | head -1)"
"$PYTHON" "$(dirname "$0")/../wait_exit.py" "$pid" "$((20 * FYAI_TIMEOUT_SCALE))" ||
    fail "the server did not stop with the turn"
mock_stop 2
assert_request 1 '[m["role"] for m in r["body"]["messages"]][-3:] == ["user", "assistant", "tool"]'
"$FYAI_BIN" -b main export -o saved.md >/dev/null 2>&1 || fail "no export"
grep -q 'Tool done at the server.' saved.md || fail "the turn did not end well"
pass
