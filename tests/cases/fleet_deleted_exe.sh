#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that fleet scanning still works after the running binary is unlinked,
# by re-execing the live image rather than the deleted path.
set -eu
# Requires Linux /proc/self/exe to exec an unlinked binary.
[ "$(uname -s)" = Linux ] || exit 77
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start fleet_jump.json
export MOCK_KEY=test-key
set_common=(--set api=chat-completions --set display/stream=false
	    --set "api_url=$MOCK_URL/v1/chat/completions" --set model=mock-model
	    --set 'api_key={type: env, value: MOCK_KEY}')
"$FYAI_BIN" "${set_common[@]}" >/dev/null 2>&1
mkdir -p a/.git bin
(cd a && "$FYAI_BIN" init >/dev/null 2>&1 &&
	"$FYAI_BIN" "${set_common[@]}" >/dev/null 2>&1) || fail "cannot make the project"
cp "$FYAI_BIN" bin/fyai
SYNC="$HOME/sync"
mkdir -p "$SYNC"
mkfifo "$SYNC/ready"
pty_run "$TEST_DIR/pty.out" "$TEST_DIR/bin/fyai" -b main -i <<PTY &
steps:
  - touch: "$SYNC/ready"
    timeout: session
  - wait-file: "$SYNC/gone"
    contains: go
    timeout: session
  - type: /fleet list
  - wait-screen: "Sessions of the fleet"
    timeout: session
PTY
driver=$!
read -r _ <"$SYNC/ready" || true
rm "$TEST_DIR/bin/fyai"
echo go >"$SYNC/gone"
wait "$driver" || fail "the scan failed with the program file gone: $(tail -c 1500 "$TEST_DIR/pty.out")"
pass
