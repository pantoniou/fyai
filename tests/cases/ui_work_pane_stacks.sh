#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify stack fallback when columns cannot satisfy the minimum tile width.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start agents_parallel.json

pty_run "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set display/work_min_tile_cols=60 \
    --set tools=true --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i <<'PTY'
steps:
  - type: delegate both halves
  - wait-screen: "[alpha] first half"
    timeout: session
  - wait-screen: "[beta] second half"
    timeout: session
  - frame: 2
  - touch: "${TEST_DIR}/release-alpha"
  - wait-screen: "REPORT-ALPHA"
  - touch: "${TEST_DIR}/release-beta"
  - wait: Both sub-agents reported.
    timeout: session
PTY

"$PYTHON" - "$TEST_DIR/pty.out" <<'PYEOF' || \
    fail "the work pane did not stack when a tile could not keep its width"
import os
import sys

sys.path.insert(0, os.environ["TESTS_DIR"])
from tile_assert import frames_with_both

data = open(sys.argv[1], "rb").read()

# Both tiles are live and held while their labels are on screen: that is where
# the pane has to stack.
frames = frames_with_both(data, "[alpha]", "[beta]", 30, 100)
if not frames:
    raise SystemExit("the two sub-agent screens were never both on screen")
for row_a, row_b, row in frames:
    if row_a == row_b:
        raise SystemExit(
            "a hundred columns cannot hold two sixty-column tiles side by "
            "side, but the screens shared row %d" % row_a)
    # A one-column stack has no column separator.
    if "\u2503" in row[20:]:
        raise SystemExit("a column rule was drawn with only one column")
PYEOF

mock_stop 4
pass
