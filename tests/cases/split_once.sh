#!/bin/bash
# SPDX-License-Identifier: MIT
# `/split` of a session that no server holds (a plain `fyai` run) starts a
# server for it, and the tile joins it as a watcher, in a root project too.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
# The confined tools run in a user namespace that cannot reach the build tree.
cp "$FYAI_BIN" "$TEST_DIR/fyai"
FYAI_BIN="$TEST_DIR/fyai"
mock_start split_tile.json
export MOCK_KEY=test-key
set_common=(--set api=chat-completions --set display/stream=false
	    --set tools=true
	    --set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model
	    --set 'api_key={type: env, value: MOCK_KEY}'
	    --set sandbox_profile=t
	    --set "sandbox_profiles={t: {extends: dev, deny: [$TEST_DIR/.fyai/unix]}}")
"$FYAI_BIN" "${set_common[@]}" >/dev/null 2>&1
"$FYAI_BIN" config set fleet/root true >/dev/null 2>&1 || fail root

pty_run "$TEST_DIR/pty.out" "$FYAI_BIN" -b main -i <<'PTY'
exit:
  clear: true
steps:
  - type: /split
  - wait-screen: "remote connect --watch"
    timeout: session
  - wait-file: "${TEST_DIR}/.fyai/logs/serve.log"
    contains: "watches the worker"
    timeout: session
  - write: "\x1d"
PTY
pass
