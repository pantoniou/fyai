#!/bin/bash
# SPDX-License-Identifier: MIT
# fyai indents rendered content itself: a shell command sits under its marker
# and the output of the call is indented under that. The content must be made
# that much narrower, or a row runs past the right edge and the terminal wraps
# it - which is what turned a live band into interleaved fragments after a
# window was made narrower.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start shell_band_head.json

FYAI_PTY_INPUT="count" \
FYAI_PTY_ROWS=30 FYAI_PTY_COLS=100 \
FYAI_PTY_MID_NEEDLE="⎿" FYAI_PTY_MID_TIMEOUT="8" \
FYAI_PTY_RESIZE_COLS="60" \
FYAI_PTY_NEEDLE="done." FYAI_PTY_TIMEOUT=60 \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set tools=true --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i

"$PYTHON" - "$TEST_DIR/pty.out" <<'PY' || fail "a row ran past the right edge"
import os
import sys

sys.path.insert(0, os.environ["TESTS_DIR"])
from tile_assert import frames

COLS = 60

# Every finished screen is modelled at the width the window started at, so a
# row too wide for the window it is read in is seen whole rather than clipped
# at it. A screen that shows the answer is a screen from after the resize:
# the window changed while the shell was still counting.
seen = False
for disp in frames(open(sys.argv[1], "rb").read(), 30, 100):
    if not any("done." in r for r in disp):
        continue
    seen = True
    widest = max((len(r.rstrip()) for r in disp), default=0)
    if widest > COLS:
        raise SystemExit("a row of %d columns in a window of %d: %r" %
                         (widest, COLS,
                          max((r.rstrip() for r in disp), key=len)))
if not seen:
    raise SystemExit("the turn never finished on the screen")
PY

pass
