#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify parallel full-screen shells across successive resize barriers. Each
# program paints its size, so after each resize both must stand on the screen
# at the rows the grid gives them, which are the same for both.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

CMD_A="!$PYTHON $TESTS_DIR/resize_tui.py A"
CMD_B="!$PYTHON $TESTS_DIR/resize_tui.py B"

# The steps that wait for both programs at @1 rows after a resize to @2 rows.
at()
{
    printf 'resize:%sx80|wait-screen:A SIZE %sx37|wait-screen:B SIZE %sx36|' \
        "$2" "$1" "$1"
}

AFTER="wait-screen:Ctrl-]|raw:1d|wait-gone:Ctrl-]|send:$CMD_B|"
AFTER="${AFTER}wait-screen:A SIZE 21x47|wait-screen:B SIZE 21x46|"
AFTER="${AFTER}$(at 15 24)$(at 19 28)$(at 17 26)$(at 23 32)raw:1d"
FYAI_PTY_ROWS=30 FYAI_PTY_COLS=100 \
FYAI_PTY_INPUT="$CMD_A" FYAI_PTY_NEEDLE="A SIZE 21x98" \
FYAI_PTY_AFTER="$AFTER" FYAI_PTY_AFTER_TIMEOUT=10 \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true -m mock-model -i ||
    fail "parallel bang shells did not take the same rows after each resize"

pass
