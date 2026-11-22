#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify `/root` outside a `/jump` reports an error and leaves the current
# session attached and functional.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start split_tile.json
export MOCK_KEY=test-key
set_common=(--set api=chat-completions --set display/stream=false
	    --set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model
	    --set 'api_key={type: env, value: MOCK_KEY}')

pty_run "$TEST_DIR/pty.out" "$FYAI_BIN" "${set_common[@]}" -b main -i <<'PTY'
exit:
  clear: true
steps:
  - type: /root
  - wait-screen: "no /jump reached this session"
    timeout: session
  - type: hello
  - wait-screen: "Heard it."
    timeout: session
PTY
pass
