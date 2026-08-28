#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify persistent shell-band chrome and scrolling output.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
mock_start shell_band_head.json

FYAI_PTY_INPUT="count" FYAI_PTY_NEEDLE="done." FYAI_PTY_TIMEOUT=40 \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/stream=false \
    --set tools=true --set api=chat-completions \
    --set "api_url=$MOCK_URL/v1/chat/completions" -m mock-model -i

"$PYTHON" - "$TEST_DIR/pty.out" <<'PY' || fail "the head of the shell band is wrong"
import os
import sys

sys.path.insert(0, os.environ["TESTS_DIR"])
from tile_assert import frames

data = open(sys.argv[1], "rb").read()

live = 0
elided = 0
for disp in frames(data, 30, 100):
    # Stop at the completed model answer.
    if any("done." in r for r in disp):
        continue
    # Select frames that contain shell output.
    if not any(r.strip().isdigit() for r in disp):
        continue
    for i, row in enumerate(disp):
        if "⎿" not in row or i + 2 >= len(disp):
            continue
        live += 1
        # Require output directly below the command.
        if not disp[i + 1].strip():
            raise SystemExit("a blank row stands between the command and "
                             "its output: %r" % disp[i + 1])
        # Require persistent title chrome above the command.
        if "shell [Count to forty]" not in disp[i - 1]:
            raise SystemExit("the title of the call is not over its "
                             "command: %r" % disp[i - 1])
        if any("⋯" in r for r in disp[i:]):
            elided += 1
if not live:
    raise SystemExit("the shell band was never on screen")
if not elided:
    raise SystemExit("the output never outgrew the rows of the band")
PY

mock_stop 2
pass
