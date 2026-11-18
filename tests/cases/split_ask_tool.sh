#!/bin/bash
# SPDX-License-Identifier: MIT
# The model of a split session is offered ask_user: the client has the input
# area that answers it. A protocol client that draws nothing is not offered it.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start chat_basic.json
pty_run "$TEST_DIR/pty.out" \
	"$FYAI_BIN" -k test-key --split -m mock-model \
	--set api=chat-completions --set display/stream=false \
	--set tools=true --set "api_url=$MOCK_URL/v1/chat/completions" -i <<'PTY'
steps:
  - type: say hello
  - wait-screen: Hello from the mock provider.
PTY
mock_stop 1
assert_request 0 'any(t["function"]["name"] == "ask_user" for t in r["body"]["tools"])'
pass
