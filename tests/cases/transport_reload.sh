#!/bin/bash
# SPDX-License-Identifier: MIT
# A reload executes the program again. With credential isolation the transport
# must live through it, with its channels and the key that came from --api-key,
# so that the session can send requests after one reload and after a second.
set -eu
. "$(dirname "$0")/../harness.sh"

case "$(uname -s)" in
Linux) ;;
*) skip "the transport needs Linux" ;;
esac

fyai_test_setup
mock_start transport_reload.json

FYAI_TRANSPORT_ISOLATION=level-b \
pty_run "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark -b main --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" \
    --set display/stream=false -m mock-model -i <<'PTY'
steps:
  - type: first question
  - wait: Reply one.
    timeout: session
  - wait-screen: Reply one.
  - type: "/reload"
  - wait-screen: "↳ Reply one."
  - type: second question
  - wait-screen: Reply two.
  - type: "/reload"
  - wait-screen: "↳ Reply two."
  - type: third question
  - wait-screen: Reply three.
PTY

# The key came from --api-key, and it reached the provider after each reload.
assert_request 0 'r["auth"] == "Bearer test-key"'
assert_request 1 'r["auth"] == "Bearer test-key"'
assert_request 2 'r["auth"] == "Bearer test-key"'
mock_stop 3
pass
