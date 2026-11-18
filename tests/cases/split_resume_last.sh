#!/bin/bash
# SPDX-License-Identifier: MIT
# fyai --split resume --last starts the worker on the session that a local
# resume --last takes.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
run_fyai branch create older
assert_status 0
mock_start chat_basic.json
run_fyai -b newer --set api=chat-completions \
	--set "api_url=$MOCK_URL/v1/chat/completions" --set display/stream=false
assert_status 0

pty_run "$TEST_DIR/pty.out" \
	"$FYAI_BIN" --split -k test-key -m mock-model resume --last <<'PTY'
steps:
  - wait-screen: "\u276f"
  - type: say hello
  - wait-screen: Hello from the mock provider.
PTY
mock_stop 1
assert_state_contains "Hello from the mock provider." -b newer dump state
assert_state_absent "Hello from the mock provider." -b older dump state
pass
