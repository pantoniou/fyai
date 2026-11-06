#!/bin/bash
# SPDX-License-Identifier: MIT
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start ui_tool_interrupt.json

start=$(date +%s)
pty_run "$TEST_DIR/pty.out" \
	"$FYAI_BIN" -k test-key --theme dark \
	--set display/markdown=true --set display/stream=false \
	--set tools=true --set api=chat-completions \
	--set "api_url=$MOCK_URL/v1/chat/completions" \
	-m mock-model -i <<'PTY'
steps:
  - type: run it
  - wait: interrupt-early
    timeout: 2
  - write: "\x03"
  - wait: interrupted
    timeout: session
PTY
elapsed=$(($(date +%s) - start))

[ "$elapsed" -lt $((30 * FYAI_TIMEOUT_SCALE)) ] || \
	fail "Ctrl-C did not interrupt the active tool"
[ ! -f interrupt-late.marker ] || fail "tool continued after Ctrl-C"

mock_stop 1
pass
