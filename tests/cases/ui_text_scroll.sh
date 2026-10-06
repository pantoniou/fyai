#!/bin/bash
# SPDX-License-Identifier: MIT
# A tool exchange that holds more rows than it shows has a scroll bar, and the
# bar scrolls it. With display/work_controls=full the tile of text takes a
# column for the bar, draws the arrows and the thumb, and the arrows move the
# rows it shows: the last rows follow the output, an arrow up moves back one
# row, an arrow down returns to the end, and the wheel over the rows moves
# three, and a key that is typed shows the end again. The rows are on rows 7 to 11 of a 30-row terminal.
set -eu
. "$(dirname "$0")/../harness.sh"

# An SGR mouse report of a wheel notch at one-based column $2 and row $3, as
# hex: button 64 is a notch up and 65 a notch down.
wheel()
{
    printf '\033[<%d;%d;%dM' "$1" "$2" "$3" | od -An -tx1 | tr -d ' \n'
}

fyai_test_setup
mock_start ui_text_scroll.json

FYAI_PTY_COLS=100 FYAI_PTY_ROWS=30 \
FYAI_PTY_INPUT="print the lines" FYAI_PTY_NEEDLE="Print sixty lines" \
FYAI_PTY_TIMEOUT=30 \
FYAI_PTY_AFTER="wait-screen:T060|wait-screen:▴|"\
"click:▴|wait-gone:T060|wait-screen:T055|"\
"click:▾|wait-screen:T060|"\
"raw:$(wheel 64 10 9)|wait-gone:T060|wait-screen:T053|"\
"raw:$(wheel 65 10 9)|wait-screen:T060|"\
"raw:$(wheel 64 10 9)|wait-gone:T060|wait-screen:T053|raw:78|wait-screen:T060|raw:7f|"\
"release:$PWD/band-release|wait-screen:done." \
FYAI_PTY_AFTER_PAUSE=0 FYAI_PTY_AFTER_TIMEOUT=10 \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set display/work_controls=full \
    --set tools=true --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i ||
    fail "the scroll bar of a tile of text did not move its rows"

mock_stop 2

# With no history the band keeps the rows it shows, and has nothing to scroll.
fyai_test_setup
mock_start ui_text_scroll.json

FYAI_PTY_COLS=100 FYAI_PTY_ROWS=30 \
FYAI_PTY_INPUT="print the lines" FYAI_PTY_NEEDLE="Print sixty lines" \
FYAI_PTY_TIMEOUT=30 \
FYAI_PTY_AFTER="wait-screen:T060|release:$PWD/band-release|wait-screen:done." \
FYAI_PTY_AFTER_PAUSE=0 FYAI_PTY_AFTER_TIMEOUT=10 \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/none.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set display/work_controls=full --set display/tool_history_lines=0 \
    --set tools=true --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i ||
    fail "a band with no history did not finish"
if grep -a -q "▴" "$TEST_DIR/none.out"; then
    fail "a band with no history drew a scroll bar"
fi

mock_stop 2
pass
