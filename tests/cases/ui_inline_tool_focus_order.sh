#!/bin/bash
# SPDX-License-Identifier: MIT
# Ctrl-T gives the keys to the places that take them in the order the screen
# shows them, from the top down and from left to right: a terminal block of
# the transcript above the work pane comes before a tile of the pane, and
# after the last the prompt has the keys again. The pane takes a quarter of the
# terminal, so the block stays on the screen. The page is
# $FYAI_INLINE_SCREEN, fullscreen by default.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start inline_tool_focus.json

FYAI_PTY_INPUT="start a program" FYAI_PTY_NEEDLE="ready." FYAI_PTY_TIMEOUT=30 \
FYAI_PTY_AFTER="wait-screen:ready.|"\
"send:!sh -c 'echo TILE-OUT; exec cat'|wait-screen:▌ TILE-OUT|"\
"raw:1d|wait-screen:▌❯|"\
"raw:14|wait-gone:▌❯|send:to-block|wait-row:▌   to-block|"\
"raw:14|wait-screen:▌ TILE-OUT|"\
"raw:14|wait-screen:▌❯" \
FYAI_PTY_AFTER_PAUSE=0 FYAI_PTY_AFTER_TIMEOUT=10 \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -b main -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set "display/screen=${FYAI_INLINE_SCREEN:-fullscreen}" \
    --set display/tool_display=inline --set display/work_zoom_rows=quarter \
    --set tools=true --set api=chat-completions \
    --set shell/input_poll_ms=0 \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i ||
    fail "Ctrl-T did not follow the order of the screen"

mock_stop 2
pass
