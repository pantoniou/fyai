#!/bin/bash
# SPDX-License-Identifier: MIT
# The head of a live shell band: what the call is, and what it was asked to
# run. It is chrome, and its output is the content under it.
#
# Two things follow from that, and both were once wrong. The output starts on
# the row under the command, with no blank row between them. And when the
# output grows past the rows the band was given, the output is what scrolls:
# the head stays, because a reader who cannot see it cannot tell whose output
# this is.
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
    # The turn is over once the model has answered.
    if any("done." in r for r in disp):
        continue
    # Only once the program has written something: before that the band is
    # its head alone, and there is no output row to stand under it.
    if not any(r.strip().isdigit() for r in disp):
        continue
    for i, row in enumerate(disp):
        if "⎿" not in row or i + 2 >= len(disp):
            continue
        live += 1
        # The output starts on the row under the command.
        if not disp[i + 1].strip():
            raise SystemExit("a blank row stands between the command and "
                             "its output: %r" % disp[i + 1])
        # The title of the call is the row above it, and stays there while
        # the output scrolls under it.
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
