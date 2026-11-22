#!/bin/bash
# SPDX-License-Identifier: MIT
# The close button of the tile of a side question, clicked while the side
# question and the turn that it was asked in are still running. The tile
# belongs to the client of a split session, which has no shell session or
# job to close: the click must not read one, and the session goes on and
# finishes the turn.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start btw_close.json

pty_run "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set display/work_controls=zoom \
    --set api=responses --set "api_url=$MOCK_URL/v1/responses" \
    -m mock-model -i <<'PTY'
wait_timeout: 10
steps:
  - type: main question
  - wait-screen: "│ main question"
  - write: "/btw side question\n"
  - click: "×"
  - touch: "${TEST_DIR}/release-side"
  - touch: "${TEST_DIR}/release-main"
  - wait-screen: MAIN-ANSWER
    timeout: session
  - write: "\e"
  - wait-gone: Esc closes
  - type: /quit
PTY
pass
