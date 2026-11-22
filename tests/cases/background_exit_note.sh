#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that a disconnecting client reports sessions still running on the
# server, both newly backgrounded and already hosted.
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

# Second session exits idle; only the first remains hosted on the server.
pty_run "$TEST_DIR/pty2.out" "$FYAI_BIN" -b other -i <<'PTY'
steps:
  - type: /exit
  - wait: "1 session keeps running at the server: main (idle)"
    timeout: session
PTY
"$FYAI_BIN" serve stop >/dev/null 2>&1 || fail "serve stop failed"
pass
