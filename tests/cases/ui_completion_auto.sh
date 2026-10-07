#!/bin/bash
# SPDX-License-Identifier: MIT
# With display/completion=auto the popup opens as a slash command is typed,
# without a Tab. Enter then takes the selection, and Escape closes the popup
# until the line changes.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start chat_basic.json
driver=0
FYAI_PTY_ROWS=24 FYAI_PTY_COLS=100 FYAI_PTY_INPUT="first question" \
FYAI_PTY_NEEDLE="Hello" FYAI_PTY_TIMEOUT=20 \
FYAI_PTY_AFTER="wait-screen:Hello from the mock provider.|raw:2f627261|wait-screen:open the branch browser|wait-screen:list, create, and manage branches|raw:1b5b42|raw:0d|wait-gone:open the branch browser|wait-screen:/branches|raw:7f7f7f7f7f7f7f7f7f|wait-gone:/branches|raw:2f627261|wait-screen:open the branch browser|raw:1b5b323775|wait-gone:open the branch browser|raw:6e|wait-screen:open the branch browser|raw:7f7f7f7f7f|wait-gone:open the branch browser" \
FYAI_PTY_AFTER_PAUSE=0 FYAI_PTY_AFTER_TIMEOUT=10 \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set display/screen=fullscreen \
    --set display/completion=auto \
    --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i ||
    driver=$?
if [ "$driver" -ne 0 ]; then
    tail -c 2000 "$TEST_DIR/pty.out" >&2
    fail "the automatic completion popup did not go as the case says"
fi
mock_stop 1
pass
