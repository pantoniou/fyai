#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify live-region reflow after a width change.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start ui_reflow.json

FYAI_PTY_INPUT="say it" \
FYAI_PTY_ROWS=30 FYAI_PTY_COLS=100 \
FYAI_PTY_MID_NEEDLE="The paragraph is long enough" \
FYAI_PTY_MID_TIMEOUT="8" \
FYAI_PTY_RESIZE_COLS="48" \
FYAI_PTY_NEEDLE="Reflow streaming done." \
FYAI_PTY_AFTER="raw:0c|wait:Reflow streaming done." \
FYAI_PTY_AFTER_TIMEOUT="8" \
FYAI_PTY_TIMEOUT=40 \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=true \
    --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i

"$PYTHON" - "$TEST_DIR/pty.out" <<'PY' || fail "the live region kept its old width"
import os
import sys

sys.path.insert(0, os.environ["TESTS_DIR"])
from tile_assert import frames

COLS = 48
# Account for the two-column document margin.
MARGIN = 4

# Inspect the last screen that contains the reflowed answer.
last = None
for disp in frames(open(sys.argv[1], "rb").read(), 30, 100):
    if any("the window changes" in r for r in disp):
        last = disp
if last is None:
    raise SystemExit("the answer never reached the screen")
for row in last:
    if len(row.rstrip()) > COLS + MARGIN:
        raise SystemExit("a row of %d columns in a window of %d: %r" %
                         (len(row.rstrip()), COLS, row))
PY

pass
