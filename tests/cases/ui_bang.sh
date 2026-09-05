#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that a completed bang shell retires after committing its output.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

# The shell exits at once, and the keys are the prompt's again when the tile
# it opened has retired. The way back is on the status row while a tile holds
# them, thus a row without it is the prompt holding them: type only then. A
# pause instead of a wait types into the tile on a loaded runner, and the
# status command never reaches the session.
FYAI_PTY_INPUT="!sh -c 'printf BANG-OUTPUT'" \
FYAI_PTY_NEEDLE="BANG-OUTPUT" \
FYAI_PTY_AFTER="resize:100|wait-gone:Ctrl-]|send:/status|wait:Usage / total" \
FYAI_PTY_AFTER_PAUSE=1 \
FYAI_PTY_SNAPSHOT="$TEST_DIR/snapshot.out" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true -m mock-model -i

"$PYTHON" - "$TEST_DIR/snapshot.out" "$TESTS_DIR" <<'PYEOF' || \
    fail "completed bang shell remained in the work pane"
import sys

sys.path.insert(0, sys.argv[2])
from screen import Screen

s = Screen(30, 100)
s.feed(open(sys.argv[1], "rb").read())
seen = s.lines()

# Require one committed shell block; exclude the separate input card.
heads = [line for line in seen if line.strip().startswith("● shell [bang-")]
if len(heads) != 1:
    raise SystemExit("bang shell drew %d headings: %r" % (len(heads), heads))
commands = [line for line in seen if "⎿  sh -c" in line]
if len(commands) != 1:
    raise SystemExit("bang shell drew %d command rows: %r" %
                     (len(commands), commands))
outputs = [line for line in seen if "BANG-OUTPUT" in line and
           "sh -c" not in line and "!sh -c" not in line]
if len(outputs) != 1:
    raise SystemExit("bang shell drew %d output rows: %r" %
                     (len(outputs), outputs))
PYEOF

pass
