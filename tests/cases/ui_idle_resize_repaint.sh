#!/bin/bash
# SPDX-License-Identifier: MIT
# A window changed while the session sat at the prompt. Nothing is arriving,
# nothing is typed, and the size is read on a frame: only the signal can ask
# for that frame. With the display open the standard descriptors are pipes of
# its own, so the size must be read from the terminal the display holds.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start ui_reflow.json

FYAI_PTY_INPUT="say it" \
FYAI_PTY_ROWS=30 FYAI_PTY_COLS=100 \
FYAI_PTY_NEEDLE="Reflow streaming done." \
FYAI_PTY_AFTER="resize:52|wait:Reflow streaming done." \
FYAI_PTY_AFTER_PAUSE="1.0" \
FYAI_PTY_TIMEOUT=40 \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=true \
    --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i

"$PYTHON" - "$TEST_DIR/pty.out" <<'PY' || fail "an idle resize was never acted on"
import os
import sys

sys.path.insert(0, os.environ["TESTS_DIR"])
from tile_assert import frames

COLS = 52

# The last screen that still shows the answer is the repaint the resize asked
# for. Without it the answer keeps the rows it was given at 100 columns.
last = None
for disp in frames(open(sys.argv[1], "rb").read(), 30, 100):
    if any("the window changes" in r for r in disp):
        last = disp
if last is None:
    raise SystemExit("the answer never reached the screen")
widest = max((len(r.rstrip()) for r in last), default=0)
if widest > COLS:
    raise SystemExit("a row of %d columns in a window of %d" % (widest, COLS))
PY

pass
