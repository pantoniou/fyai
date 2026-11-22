#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that /continue resumes the model from an incomplete turn ending in a
# tool result without injecting a new user message.
set -eu
. "$(dirname "$0")/../harness.sh"
fyai_test_setup
mock_start continue_session.json
export MOCK_KEY=test-key
"$FYAI_BIN" --set api=chat-completions --set display/stream=false \
    --set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model \
    --set max_tool_iterations=1 \
    --set 'api_key={type: env, value: MOCK_KEY}' >/dev/null 2>&1
printf 'one line\n' >data.txt

pty_run "$TEST_DIR/pty.out" "$FYAI_BIN" -b main -i <<'PTY'
steps:
  - type: Start the long job
  - wait-screen: "limit of 1"
    timeout: session
  - type: /continue
  - wait-screen: "Continued without a message."
    timeout: session
  - type: /continue
  - wait-screen: "does not end with a tool result"
PTY
mock_stop 2
assert_request 1 "[m['role'] for m in r['body']['messages']][-2:] == ['assistant', 'tool']"
pass
