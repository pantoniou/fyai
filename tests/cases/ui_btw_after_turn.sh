#!/bin/bash
# SPDX-License-Identifier: MIT
# A side question keeps running after the main turn frees its scratch state.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start btw_after_turn.json

FYAI_PTY_INPUT="main question" \
FYAI_PTY_DURING_INPUT="/btw side question" \
FYAI_PTY_PROGRESS_NEEDLE="main question" \
FYAI_PTY_NEEDLE="btw: session/" \
FYAI_PTY_AFTER="release:$TEST_DIR/release-main|wait-screen:MAIN-ANSWER|release:$TEST_DIR/release-side|wait-screen:Esc closes|raw:1b|wait-gone:SIDE-ANSWER" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set api=responses --set "api_url=$MOCK_URL/v1/responses" \
    -m mock-model -i

[ ! -f "$TEST_DIR/stream-wait-failed" ] ||
    fail "a held mock reply was never released"

mock_stop 2
pass
