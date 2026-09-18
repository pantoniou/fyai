#!/bin/bash
# SPDX-License-Identifier: MIT
# Both renderers mark keyboard focus with a themed edge. Tiles use the first
# margin column; the prompt uses the column before its marker. The inactive
# location remains blank so changing focus does not move content.
set -eu
. "$(dirname "$0")/../harness.sh"

run_with()
{
    renderer=$1
    fyai_test_setup
    FYAI_PTY_COLS=100 FYAI_PTY_ROWS=30 \
    FYAI_PTY_INPUT="!sh -c 'printf \"%s-%s\\n\" EDGE OUT; exec cat'" \
    FYAI_PTY_NEEDLE="bang-1" FYAI_PTY_TIMEOUT=20 \
    FYAI_PTY_AFTER="wait-screen:▌ EDGE-OUT|wait-gone:▌❯|raw:1d|"\
"wait-gone:▌ EDGE-OUT|wait-screen:  EDGE-OUT|wait-screen:▌❯|"\
"send:/kill bang-1|wait-screen:stopping shell bang-1" \
    FYAI_PTY_AFTER_PAUSE=0 FYAI_PTY_AFTER_TIMEOUT=10 \
    "$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
        "$FYAI_BIN" -k test-key --theme dark \
        --set display/markdown=true \
        --set "display/renderer=$renderer" \
        --set api=chat-completions \
        --set "api_url=http://127.0.0.1:9/v1/chat/completions" \
        -m mock-model -i ||
        fail "$renderer: the edge did not follow the keys"
    if [ "$renderer" = page ] &&
       grep -a -q "needs a libfytimui" "$TEST_DIR/pty.out"; then
        skip "this build has no page support"
    fi
}

run_with stack
run_with page

pass
