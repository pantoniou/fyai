#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify focus cycling across bang-shell tiles and the prompt.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

FYAI_PTY_INPUT="!sh -c 'printf FIRST; sleep 5'" \
FYAI_PTY_NEEDLE="FIRST" FYAI_PTY_TIMEOUT=20 \
FYAI_PTY_AFTER="wait:focused|raw:1d|"\
"send:!sh -c 'printf SECOND; sleep 5'|wait:SECOND|wait:focused|"\
"raw:1b5b393b3575|raw:14|send:/sessions|wait:Active sessions|"\
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
if not any("focused" in row for frame in shown for row in frame):
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
