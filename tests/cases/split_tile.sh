#!/bin/bash
# SPDX-License-Identifier: MIT
# `/split` opens a session of its own on the branch in a tile of the work
# pane, and the session that split takes the policy that forks the branch at
# the first commit of both.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start split_tile.json
export MOCK_KEY=test-key
set_common=(--set api=chat-completions --set display/stream=false
	    --set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model
	    --set 'api_key={type: env, value: MOCK_KEY}')
"$FYAI_BIN" "${set_common[@]}" >/dev/null 2>&1

pty_run "$TEST_DIR/pty.out" "$FYAI_BIN" -b main -i <<'PTY'
exit:
  clear: true
steps:
  - type: hello
  - wait-screen: "Heard it."
    timeout: session
  - type: /split
  - wait-screen: "split main"
    timeout: session
  - wait-screen: "split: a tile runs main"
    timeout: session
  - write: "\x1d"
PTY
pass
