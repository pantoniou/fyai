#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that a remote session client persists sent lines to history so a
# subsequent client can recall them with the Up arrow.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start fleet_history.json
export MOCK_KEY=test-key
set_common=(--set api=chat-completions --set display/stream=false
	    --set tools=true
	    --set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model
	    --set 'api_key={type: env, value: MOCK_KEY}')
"$FYAI_BIN" "${set_common[@]}" >/dev/null 2>&1
"$FYAI_BIN" serve --daemon >"$TEST_DIR/serve.out" 2>&1 ||
	fail "the root server did not start: $(cat "$TEST_DIR/serve.out")"
fyai_test_on_exit '"$FYAI_BIN" serve stop >/dev/null 2>&1 || true'

pty_run "$TEST_DIR/pty.out" "$FYAI_BIN" -b main -i <<'PTY'
exit:
  clear: true
steps:
  - type: Say it
  - wait-screen: "Heard it."
    timeout: session
PTY

# Reconnect with a new client and recall the previous line via Up arrow.
pty_run "$TEST_DIR/pty2.out" "$FYAI_BIN" -b main -i <<'PTY'
exit:
  clear: true
steps:
  - wait-screen: "Heard it."
    timeout: session
  - write: "\e[A\e[A"
  - wait-input: "Say it"
PTY
pass
