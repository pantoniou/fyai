#!/bin/bash
# SPDX-License-Identifier: MIT
# With the tiles in a column beside the transcript, a tile at the top of the
# column stands above a terminal block of the transcript, so Ctrl-T gives it
# the keys first: the order is that of the screen, not blocks before tiles.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start inline_tool_focus.json

FYAI_PTY_COLS=160 FYAI_PTY_INPUT="start a program" FYAI_PTY_NEEDLE="ready." \
FYAI_PTY_TIMEOUT=30 \
FYAI_PTY_AFTER="wait-screen:ready.|"\
"send:!sh -c 'echo TILE-OUT; exec cat'|wait-screen:▌ TILE-OUT|"\
"raw:1d|wait-screen:▌❯|"\
"raw:14|wait-screen:▌ TILE-OUT|wait-gone:▌❯|"\
"raw:14|wait-gone:▌ TILE-OUT|send:to-block|wait-screen:▌   to-block|"\
"raw:14|wait-screen:▌❯" \
FYAI_PTY_AFTER_PAUSE=0 FYAI_PTY_AFTER_TIMEOUT=10 \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -b main -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set display/screen=fullscreen --set display/work_panels=on \
    --set display/tool_display=inline \
    --set tools=true --set api=chat-completions \
    --set shell/input_poll_ms=0 \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i ||
    fail "Ctrl-T did not follow the order of the screen"

mock_stop 2
pass
