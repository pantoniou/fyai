#!/bin/bash
# SPDX-License-Identifier: MIT
# Rows are hard-wrapped when they are made. A window that changes width while
# an answer is arriving therefore keeps rows made for the old width: too wide
# for a narrower window, which clips them. The live region is made again from
# the source of the document, and the rows it draws fit the window it is read
# in.
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
# The two-column document margin the renderer draws beside every row.
MARGIN = 4

# Every finished screen is modelled at the width the window started at, so a
# row made for that width is seen at its full length instead of being clipped
# at the new one. The reading is the last screen that still shows the answer:
# by then the window is 48 columns and every row on it was made for that.
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
