#!/bin/bash
# SPDX-License-Identifier: MIT
# The scroll bar of a tile moves its view through the history of its program.
# With display/work_controls=full a tile has a bar under both renderers: the
# band stack draws it in the library and the page draws it on its canvas. The
# wheel over the tile moves three rows, the arrow at the top of the bar moves
# one, and what the user types shows the live screen again.
set -eu
. "$(dirname "$0")/../harness.sh"

# An SGR mouse report at one-based column $2 and row $3, as hex: button $1 is
# pressed and released, or a wheel notch that has no release.
mouse()
{
    if [ "$1" -ge 64 ]; then
        printf '\033[<%d;%d;%dM' "$1" "$2" "$3"
    else
        printf '\033[<%d;%d;%dM\033[<%d;%d;%dm' "$1" "$2" "$3" "$1" "$2" "$3"
    fi | od -An -tx1 | tr -d ' \n'
}

# The tile is the whole width of a 100-column terminal, under the blank row
# above the pane: its head takes rows 2 and 3, the screen starts on row 4,
# and the bar is the last column. The
# last row of the screen is the row of the cursor, under L200.
run_with()
{
    renderer=$1
    fyai_test_setup
    FYAI_PTY_COLS=100 FYAI_PTY_ROWS=30 \
    FYAI_PTY_INPUT="!sh -c 'seq -f L%03g 1 200; exec cat'" \
    FYAI_PTY_NEEDLE="bang-1" FYAI_PTY_TIMEOUT=20 \
    FYAI_PTY_AFTER="wait-screen:L200|wait-screen:▴|"\
"raw:$(mouse 64 20 10)|wait-gone:L200|wait-screen:L198|"\
"raw:$(mouse 0 100 4)|wait-gone:L198|wait-screen:L197|"\
"raw:78|wait-screen:L200|"\
"raw:1d|wait-gone:Ctrl-]|"\
"send:/kill bang-1|wait-screen:stopping shell bang-1" \
    FYAI_PTY_AFTER_PAUSE=0 FYAI_PTY_AFTER_TIMEOUT=10 \
    "$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
        "$FYAI_BIN" -k test-key --theme dark \
        --set display/work_controls=full \
        --set "display/renderer=$renderer" \
        --set retry/max_attempts=1 \
        --set api=chat-completions \
        --set "api_url=http://127.0.0.1:9/v1/chat/completions" \
        -m mock-model -i ||
        fail "$renderer: the scroll bar did not move the view"
    if [ "$renderer" = page ] &&
       grep -a -q "needs a libfytimui" "$TEST_DIR/pty.out"; then
        skip "this build has no page support"
    fi
}

run_with stack
run_with page

pass
