#!/bin/bash
# SPDX-License-Identifier: MIT
# The keys that leave a tile are keys of the surface mode: with display/keys
# Ctrl-] is unbound, so a program in the tile receives it, and F9 gives the
# keys back to the prompt.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

# The terminal of the tile echoes the code of Ctrl-] as ^], which shows that
# the program got it. F9 is "ESC [ 2 0 ~".
FYAI_PTY_INPUT="!cat" \
FYAI_PTY_NEEDLE="Ctrl-] returns to the prompt" FYAI_PTY_TIMEOUT=30 \
FYAI_PTY_AFTER="raw:1d|wait-screen:^]|wait-screen:Ctrl-] returns to the prompt|raw:1b5b32307e|wait-gone:Ctrl-] returns to the prompt" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key -m mock-model -i \
    --set 'display/keys={surface: {"Ctrl-]": "", F9: fyai.focus.prompt}}' ||
    fail "the keys of the surface mode did not move"
pass
