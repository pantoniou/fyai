#!/bin/bash
# SPDX-License-Identifier: MIT
# The panel at the right of the input header counts the live shells of the
# user and has a button that hides and shows the work pane. The pane goes and
# comes back with its program running, under both renderers. The panel goes
# when the last shell ends, also on the fullscreen page, where the result of
# /kill is a note and no tile keeps the pane.
set -eu
. "$(dirname "$0")/../harness.sh"

# The program prints a line its command does not hold, so the line leaves the
# screen with the pane. The button is found on the screen: ▣ while the pane
# shows, ▢ while it is hidden.
run_with()
{
    renderer=$1
    screen=inline
    [ "$renderer" = page ] && screen=fullscreen
    fyai_test_setup
    FYAI_PTY_COLS=100 FYAI_PTY_ROWS=30 \
    FYAI_PTY_INPUT="!sh -c 'printf \"%s-%s\\n\" PANE OUT; exec cat'" \
    FYAI_PTY_NEEDLE="bang-1" FYAI_PTY_TIMEOUT=20 \
    FYAI_PTY_AFTER="wait-screen:PANE-OUT|raw:1d|wait-gone:Ctrl-]|"\
"wait-screen:▣ !1|click:▣|wait-gone:PANE-OUT|wait-screen:▢ !1|"\
"click:▢|wait-screen:PANE-OUT|wait-screen:▣ !1|"\
"send:/kill bang-1|wait-screen:stopping shell bang-1|wait-gone:!1" \
    FYAI_PTY_AFTER_PAUSE=0 FYAI_PTY_AFTER_TIMEOUT=10 \
    "$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
        "$FYAI_BIN" -k test-key --theme dark \
        --set display/markdown=true \
        --set display/work_controls=zoom \
        --set "display/renderer=$renderer" \
        --set "display/screen=$screen" \
        --set api=chat-completions \
        --set "api_url=http://127.0.0.1:9/v1/chat/completions" \
        -m mock-model -i ||
        fail "$renderer: the panel did not hide and show the pane"
    if [ "$renderer" = page ] &&
       grep -a -q "needs a libfytimui" "$TEST_DIR/pty.out"; then
        skip "this build has no page support"
    fi
}

run_with stack
run_with page

pass
