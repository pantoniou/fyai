#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that a shell command finished before detachment does not reopen a
# dead command tile when a client later reattaches to the parked session.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start split_fork.json
export MOCK_KEY=test-key
set_common=(--set api=chat-completions --set display/stream=false
	    --set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model
	    --set 'api_key={type: env, value: MOCK_KEY}')
"$FYAI_BIN" "${set_common[@]}" >/dev/null 2>&1
"$FYAI_BIN" serve --daemon >"$TEST_DIR/serve.out" 2>&1 ||
	fail "the server did not start: $(cat "$TEST_DIR/serve.out")"
fyai_test_on_exit '"$FYAI_BIN" serve stop >/dev/null 2>&1 || true'

pty_run "$TEST_DIR/pty1.out" "$FYAI_BIN" "${set_common[@]}" -b main -i <<'PTY'
exit:
  clear: true
steps:
  - type: hello
  - wait-screen: "Heard it."
    timeout: session
  - type: "!echo ranhere"
  - wait-screen: "ranhere"
    timeout: session
  - write: "\x1d"
  - type: /background
  - wait: "parked at a server"
    timeout: session
PTY

pty_run "$TEST_DIR/pty2.out" "$FYAI_BIN" "${set_common[@]}" resume --last <<'PTY'
exit:
  clear: true
steps:
  - wait-screen: "Heard it."
    timeout: session
  - checkpoint: "${TEST_DIR}/cp.txt"
PTY
! grep -q "bang-1" "$TEST_DIR/cp.txt" || fail "the joined client shows the ended command"
pass
