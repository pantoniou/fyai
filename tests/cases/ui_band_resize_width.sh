#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that indented band content remains within the resized width.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start shell_band_resize.json

pty_run "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set tools=true --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i <<'PTY'
rows: 30
cols: 100
timeout: 60
steps:
  - type: count
  - wait: "⎿"
    timeout: 8
  - resize: "30x60"
  # The command waits for this file, so that its end comes after the resize.
  - touch: "${TEST_DIR}/resized"
  - wait: FYAI_TEST_END_SHELL_BAND_HEAD_6f2c91a8
    timeout: session
PTY

"$PYTHON" - "$TEST_DIR/pty.out" <<'PY' || fail "a row ran past the right edge"
import os
import sys

sys.path.insert(0, os.environ["TESTS_DIR"])
from tile_assert import frames

COLS = 60

# Inspect complete post-resize screens without clipping rows.
seen = False
for disp in frames(open(sys.argv[1], "rb").read(), 30, 100):
    if not any("FYAI_TEST_END_SHELL_BAND_HEAD_6f2c91a8" in r for r in disp):
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
