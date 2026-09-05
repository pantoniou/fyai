#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify focus cycling across bang-shell tiles and the prompt. The pane holds
# its tiles oldest first, so the cycle runs bang-1, bang-2, prompt: Ctrl-Tab
# leaves the tile that has the keys and each Ctrl-T takes the next one.
#
# One key to a frame: a frame reads what has arrived and acts on one focus
# key, so two keys sent with nothing between them can share a frame and the
# window keeps one of them. Each key waits for the frames that read it, and
# the last waits for the way back to leave the status row: the keys are the
# prompt's again, which is where the commands below must land.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

FYAI_PTY_INPUT="!sh -c 'printf FIRST; while :; do sleep 1; done'" \
FYAI_PTY_NEEDLE="FIRST" FYAI_PTY_TIMEOUT=20 \
FYAI_PTY_AFTER="wait:Ctrl-]|raw:1d|"\
"send:!sh -c 'printf SECOND; while :; do sleep 1; done'|wait-frame:SECOND|"\
"raw:1b5b393b3575|frame:2|raw:14|frame:2|raw:14|frame:2|raw:14|"\
"wait-gone:Ctrl-]|send:/sessions|wait:Active sessions|"\
"send:/kill bang-1|wait:stopping shell bang-1|"\
"send:/kill bang-2|wait:stopping shell bang-2" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/work_min_tile_cols=30 \
    -m mock-model -i

"$PYTHON" - "$TEST_DIR/pty.out" "$TESTS_DIR" <<'PYEOF' || \
    fail "bang shells did not tile and cycle focus"
import sys

sys.path.insert(0, sys.argv[2])
from tile_assert import frames

shown = frames(open(sys.argv[1], "rb").read(), 30, 100)
if not any(any("FIRST" in row for row in frame) and
           any("SECOND" in row for row in frame) for frame in shown):
    raise SystemExit("the two bang screens never shared one frame")
# The way back is on the status row while a tile holds the keys; the tile
# itself gains no row, so nothing moves when focus does.
if not any("Ctrl-]" in row for frame in shown for row in frame):
    raise SystemExit("Ctrl-T never focused a tile")
if not any("Active sessions" in row for frame in shown for row in frame):
    raise SystemExit("/sessions did not list the live shells")
if not any("bang-1" in "\n".join(frame) and
           "bang-2" in "\n".join(frame) and
           "shell" in "\n".join(frame) for frame in shown):
    raise SystemExit("/sessions did not include both live shells")
if not any("stopping shell bang-1" in row for frame in shown for row in frame):
    raise SystemExit("/kill did not stop the named shell")
PYEOF

pass
