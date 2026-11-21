#!/bin/bash
# SPDX-License-Identifier: MIT
# `/jump --split NAME` shows the session NAME of the fleet in a tile, and this
# session stays: the server lists the tile as a watcher of the worker.
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
"$FYAI_BIN" config set fleet/root true >/dev/null 2>&1 ||
	fail "cannot make the project a fleet root"
"$FYAI_BIN" serve --daemon >"$TEST_DIR/serve.out" 2>&1 ||
	fail "the server did not start: $(cat "$TEST_DIR/serve.out")"
fyai_test_on_exit '"$FYAI_BIN" serve stop >/dev/null 2>&1 || true'

pty_run "$TEST_DIR/pty.out" "$FYAI_BIN" -b main -i <<'PTY'
exit:
  clear: true
steps:
  - type: /jump --split main
  - wait-screen: "split main"
    timeout: session
  - wait-file: "${TEST_DIR}/.fyai/logs/serve.log"
    contains: "watches the worker"
    timeout: session
  - write: "\x1d"
PTY
pass
