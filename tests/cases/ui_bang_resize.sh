#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that resizing a live bang session resizes its child PTY.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

# The shell prints its columns at SIGWINCH; the width it was given must reach
# the screen. It writes READY in two parts once its trap stands, so only its
# output holds the word and not the command in the head: a resize before the
# trap would be lost.
FYAI_PTY_ROWS=30 FYAI_PTY_COLS=100 \
FYAI_PTY_INPUT="!sh -c 'trap \"stty size | sed s/.*[[:space:]]/COLS:/\" WINCH; printf %s%s REA DY; sleep 30 & p=\$!; until wait \$p; do :; done'" \
FYAI_PTY_NEEDLE="bang-1" \
FYAI_PTY_AFTER="wait-screen:READY|wait-screen:Ctrl-] returns to the prompt|raw:14|wait-gone:Ctrl-] returns to the prompt|resize:72|wait-screen:COLS:70|raw:1d" \
FYAI_PTY_AFTER_TIMEOUT=20 \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true -m mock-model -i ||
    fail "resized bang terminal did not receive its inner work-pane size"


pass
