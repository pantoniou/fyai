#!/bin/bash
# SPDX-License-Identifier: MIT
# --no-split keeps an interactive session in the process that has the
# terminal; such a session cannot go to a server.
set -eu
. "$(dirname "$0")/../harness.sh"
fyai_test_setup
mock_start remote_park.json
export MOCK_KEY=test-key
"$FYAI_BIN" --set api=chat-completions --set display/stream=false \
    --set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model \
    --set 'api_key={type: env, value: MOCK_KEY}' >/dev/null 2>&1

pty_run "$TEST_DIR/pty.out" "$FYAI_BIN" --no-split -b main -i <<'PTY'
steps:
  - type: Say done
  - wait-screen: "All done."
    timeout: session
  - type: /background
  - wait-screen: "runs in the terminal process"
    timeout: session
PTY
[ ! -e .fyai/unix ] || fail "a server was started for a session in the terminal process"
pass
