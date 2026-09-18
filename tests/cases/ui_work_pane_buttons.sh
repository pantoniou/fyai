#!/bin/bash
# SPDX-License-Identifier: MIT
# The buttons at the right of the head of a tile. With the mouse grabbed, the
# minimize button makes a tile a head under the screens, a click on that head
# shows it again, and the maximize button gives a tile the pane and gives it
# back. Both renderers draw the buttons from the same head source.
set -eu
. "$(dirname "$0")/../harness.sh"

# An SGR press and release at one-based column $1 and row $2, as hex.
click()
{
    printf '\033[<0;%d;%dM\033[<0;%d;%dm' "$1" "$2" "$1" "$2" |
        od -An -tx1 | tr -d ' \n'
}

# Each program prints a line its command does not hold, so the line leaves
# the screen with the screen of its tile. In a 100-column terminal the minimize
# button of the first tile is column 45 and the maximize button of the second
# is column 98, on row 1. A minimized tile is shown again by a click on its
# head, which is found on the screen. The command of the second tile is whole
# only while that tile has the whole width, which says the layout moved.
run_with()
{
    renderer=$1
    fyai_test_setup
    FYAI_PTY_COLS=100 FYAI_PTY_ROWS=30 \
    FYAI_PTY_INPUT="!sh -c 'printf \"%s-%s\\n\" LEFT OUT; exec cat'" \
    FYAI_PTY_NEEDLE="bang-1" FYAI_PTY_TIMEOUT=20 \
    FYAI_PTY_AFTER="wait-screen:LEFT-OUT|raw:1d|wait-gone:Ctrl-]|"\
"send:!sh -c 'printf \"%s-%s\\n\" RIGHT OUT; exec cat'|wait-screen:RIGHT-OUT|"\
"raw:$(click 45 1)|wait-gone:LEFT-OUT|wait-screen:RIGHT OUT; exec cat'|"\
"click:LEFT OUT; exec cat'|wait-screen:LEFT-OUT|wait-gone:RIGHT OUT; exec cat'|"\
"raw:$(click 98 1)|wait-gone:bang-1|"\
"raw:$(click 98 1)|wait-screen:bang-1|"\
"raw:1d|wait-gone:Ctrl-]|"\
"send:/kill bang-1|wait-screen:stopping shell bang-1|"\
"send:/kill bang-2|wait-screen:stopping shell bang-2" \
    FYAI_PTY_AFTER_PAUSE=0 FYAI_PTY_AFTER_TIMEOUT=10 \
    "$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
        "$FYAI_BIN" -k test-key --theme dark \
        --set display/markdown=true \
        --set display/work_controls=zoom \
        --set "display/renderer=$renderer" \
        --set retry/max_attempts=1 \
        --set api=chat-completions \
        --set "api_url=http://127.0.0.1:9/v1/chat/completions" \
        -m mock-model -i ||
        fail "$renderer: a button of a tile did not act"
    if [ "$renderer" = page ] &&
       grep -a -q "needs a libfytimui" "$TEST_DIR/pty.out"; then
        skip "this build has no page support"
    fi
    grep -a -q "▁" "$TEST_DIR/pty.out" ||
        fail "$renderer: the head has no buttons"
}

run_with stack
run_with page

pass
