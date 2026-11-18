#!/bin/bash
# SPDX-License-Identifier: MIT
# A client that leaves says which sessions keep running at the server: the one
# that /background gave to it, and the others that it holds.
set -eu
. "$(dirname "$0")/../harness.sh"
fyai_test_setup
mock_start remote_park.json
export MOCK_KEY=test-key
"$FYAI_BIN" --set api=chat-completions --set display/stream=false \
    --set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model \
    --set 'api_key={type: env, value: MOCK_KEY}' >/dev/null 2>&1

pty_run "$TEST_DIR/pty1.out" "$FYAI_BIN" -b main -i <<'PTY'
steps:
  - type: Say done
  - wait-screen: "All done."
    timeout: session
  - type: /background
  - wait: "is parked at a server"
    timeout: session
PTY

# A second session leaves with no turn: the first one is the one that stays.
pty_run "$TEST_DIR/pty2.out" "$FYAI_BIN" -b other -i <<'PTY'
steps:
  - type: /exit
  - wait: "1 session keeps running at the server: main (idle)"
    timeout: session
PTY
"$FYAI_BIN" serve stop >/dev/null 2>&1 || fail "serve stop failed"
pass
