#!/bin/bash
# SPDX-License-Identifier: MIT
# The default top row of the prompt names the sandbox profile that confines the
# tools, and shows the new one when /session profile changes it. A session with
# no sandbox says so.
set -eu
[ "$(uname -s)" = Linux ] || exit 77
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start chat_basic.json

session() {
	# $1 the profile to start with, $2 the steps after the start.
	export profile="$1"
	pty_run "$TEST_DIR/pty.out" \
	    "$FYAI_BIN" -k test-key --theme dark -b main --set api=chat-completions \
	    --set "api_url=$MOCK_URL/v1/chat/completions" \
	    --set display/stream=false --set display/markdown=true \
	    --set display/screen=fullscreen --set "sandbox_profile=${profile}" \
	    -m mock-model -i <<'PTY'
rows: 30
cols: 100
steps:
  - wait-screen: "sandbox ${profile}"
  - type: "/session profile build"
  - wait-screen: "sandbox build"
  - type: "/exit"
PTY
}

session dev

# A session with the sandbox off says so.
pty_run "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark -b main --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" \
    --set display/stream=false --set display/markdown=true \
    --set display/screen=fullscreen --set sandbox_profile=none \
    -m mock-model -i <<'PTY'
rows: 30
cols: 100
steps:
  - wait-screen: "no sandbox"
  - type: "/exit"
PTY
mock_stop 0
pass
