#!/bin/bash
# SPDX-License-Identifier: MIT
# The viewport pans over the drawing to keep the selected branch on the pane.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup
for n in 01 02 03 04 05 06 07 08 09 10 11 12 13 14 15 16 17 18; do
	run_fyai branch create "main/b$n"
	assert_status 0
done

# Twelve moves down take the selection past the foot of the pane. The drawing
# is one window, so the rows above pan off instead of the page turning.
FYAI_PTY_ROWS=20 FYAI_PTY_COLS=60 \
FYAI_PTY_INPUT="/branches" \
FYAI_PTY_NEEDLE="Branches" \
FYAI_PTY_SNAPSHOT="$TEST_DIR/pan.snap" \
FYAI_PTY_AFTER="raw:1b5b48|raw:6a6a6a6a6a6a6a6a6a6a6a6a|wait-frame:b12|snapshot|raw:1d|drain:0.2" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pan.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true --set display/diagram_charset=unicode \
    -m mock-model -i

# The selected branch is on the pane, with the row of context under it that
# says the drawing carries on.
"$PYTHON" - "$TEST_DIR/pan.snap" "$TESTS_DIR" <<'PY' || fail "the viewport did not pan to the selection"
import sys

sys.path.insert(0, sys.argv[2])
from screen import Screen

screen = Screen(20, 60)
screen.feed(open(sys.argv[1], "rb").read())
rows = [r for r in screen.display() if r.strip()]
if not any("b12" in r for r in rows):
    raise SystemExit("the selection is off the pane:\n" + "\n".join(rows))
if not any("b13" in r for r in rows):
    raise SystemExit("the selection has no row under it:\n" + "\n".join(rows))
if any("b01" in r for r in rows):
    raise SystemExit("the drawing did not pan:\n" + "\n".join(rows))
PY

pass
