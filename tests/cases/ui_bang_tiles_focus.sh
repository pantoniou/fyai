#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify focus cycling across bang-shell tiles and the prompt.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

FYAI_PTY_INPUT="!sh -c 'printf FIRST; sleep 5'" \
FYAI_PTY_NEEDLE="FIRST" FYAI_PTY_TIMEOUT=20 \
FYAI_PTY_AFTER="send:!sh -c 'printf SECOND; sleep 5'|wait:SECOND|raw:07|wait:focused|raw:07|raw:07|send:/status|wait:Usage / total" \
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
    raise SystemExit("Ctrl-G never focused a tile")
if not any("Usage / total" in row for frame in shown for row in frame):
    raise SystemExit("Ctrl-G did not round-trip back to the prompt")
PYEOF

pass
