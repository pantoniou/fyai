#!/bin/bash
# SPDX-License-Identifier: MIT
# The browser can cancel a live grandchild while its parent is running.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
# The grandchild answers when the case lets it, after the cancel.
mock_start agent_recursive_held.json

pty_run "$TEST_DIR/pty.out" \
	"$FYAI_BIN" -k test-key --theme dark \
	--set display/markdown=true --set display/stream=false \
	--set tools=true --set api=responses \
	--set "api_url=$MOCK_URL/v1/responses" -m mock-model -i <<'PTY'
steps:
  - type: delegate recursively
  - wait: Agent progress
    timeout: 8
  - write: "/branches\n"
  - wait: Agent progress
    timeout: 8
  - wait: Branches
    timeout: session
  - wait-screen: "agent:grandchild"
  - write: "jjK"
  - wait-screen: Recursive delegation complete.
  - touch: "${TEST_DIR}/release-nested"
  - write: "\x1d"
PTY

assert_request 3 'not any(i.get("output") == "NESTED_REPORT" for i in r["body"].get("input", []))'
mock_stop 5
pass
