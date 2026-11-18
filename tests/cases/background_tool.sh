#!/bin/bash
# SPDX-License-Identifier: MIT
# /background with a tool running: the tool goes on, and its result reaches the
# model at the server, which stores the end of the turn.
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
  - touch: "${TEST_DIR}/tool.fifo"
PTY

# The tool ended when its fifo closed.

log=.fyai/logs/serve.log
pid="$(sed -n 's/.*serve\[\([0-9]*\)\]: serving.*/\1/p' "$log" | head -1)"
"$PYTHON" "$(dirname "$0")/../wait_exit.py" "$pid" "$((20 * FYAI_TIMEOUT_SCALE))" ||
    fail "the server did not stop with the turn"
mock_stop 2
"$FYAI_BIN" -b main export -o saved.md >/dev/null 2>&1 || fail "no export"
grep -q 'Tool done at the server.' saved.md || fail "the server did not finish the turn"
pass
