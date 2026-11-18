#!/bin/bash
# SPDX-License-Identifier: MIT
# /session new in a session that runs a tool leaves that session running at a
# server and starts a new one in the same terminal; an idle session just ends.
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
  - type: /session new
  - wait: "continues in the background"
    timeout: session
  - wait-gone: "tool.fifo"
  - wait-count: "\x1b[>1u"
    more: 1
    timeout: session
  - type: /session list
  - wait-screen: "main"
    timeout: session
  - wait-screen: "running"
  - type: /session kill main
  - wait-screen: "ended main"
    timeout: session
PTY

run_fyai session list
assert_stdout_contains "no live sessions"
pass
