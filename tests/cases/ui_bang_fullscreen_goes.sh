#!/bin/bash
# SPDX-License-Identifier: MIT
# A full-screen bang program draws on the alternate screen and leaves nothing
# on the primary one: when it ends its tile goes at once, with no dismissal.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
driver=0
FYAI_TRACE="$TEST_DIR/trace.log" \
FYAI_PTY_ROWS=30 FYAI_PTY_COLS=100 \
FYAI_PTY_INPUT="!sh -c 'printf \"\\033[?1049h\"; printf %s%s ALT OUT; read x; printf \"\\033[?1049l\"'" \
FYAI_PTY_NEEDLE="bang-1" FYAI_PTY_TIMEOUT=20 \
FYAI_PTY_AFTER="wait-screen:ALTOUT|raw:0d|wait-gone:bang-1" \
FYAI_PTY_AFTER_PAUSE=0 FYAI_PTY_AFTER_TIMEOUT=10 \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true \
    -m mock-model -i || driver=$?
if [ "$driver" -ne 0 ]; then
    tail -c 2000 "$TEST_DIR/pty.out" >&2
    fail "the tile of a full-screen program did not go when it ended"
fi
pass
