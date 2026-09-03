#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that focus does not change work-pane geometry, and that a fractional
# pane height follows the terminal it is a fraction of.
set -eu
. "$(dirname "$0")/../harness.sh"

# Two bang shells report the rows their pseudo-terminals were given, then
# focus is cycled over both tiles and back to the prompt. Each shell reports
# its size again afterwards: a tile must keep the height it had.
focus_keeps_size()
{
    fyai_test_setup
    FYAI_PTY_ROWS=32 FYAI_PTY_COLS=120 \
    FYAI_PTY_INPUT="!sh -c 'sleep 1; stty size; sleep 4; stty size; sleep 4'" \
    FYAI_PTY_NEEDLE="bang-1" FYAI_PTY_TIMEOUT=30 \
    FYAI_PTY_AFTER="wait:Ctrl-]|raw:1d|"\
"send:!sh -c 'sleep 1; stty size; sleep 4; stty size; sleep 4'|"\
"wait:bang-2|wait:Ctrl-]|raw:14|raw:14|raw:1d|drain:5|raw:1d" \
    "$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/focus.out" \
        "$FYAI_BIN" -k test-key --theme dark \
        --set display/markdown=true --set display/work_min_tile_cols=30 \
        --set display/work_zoom_rows=half -m mock-model -i
    "$PYTHON" - "$TEST_DIR/focus.out" <<'PYEOF' || \
        fail "focus cycling changed the size of a tile"
import re
import sys

# Every "<rows> <cols>" a shell printed, in the order the terminal saw them.
data = open(sys.argv[1], "rb").read()
sizes = [int(m[0]) for m in re.findall(rb"\b(\d+) (\d+)\b", data)]
if len(sizes) < 2:
    raise SystemExit("the bang shells did not report their sizes")
# A tile may not collapse: no report is a fraction of the first one, and no
# report is the two or three rows a short program would have forced.
first = sizes[0]
if first < 3:
    raise SystemExit("the first tile opened at %d rows" % first)
for rows in sizes:
    if rows < 3:
        raise SystemExit("a tile collapsed to %d rows" % rows)
    if rows > first:
        raise SystemExit("focus grew a tile from %d to %d rows" %
                         (first, rows))
PYEOF
}

# A named pane height is a fraction of the current terminal, so a resize of
# the outer window changes it. The shell reports its size across the barrier.
resize_recalculates()
{
    policy=$1
    before=$2
    after=$3
    fyai_test_setup
    FYAI_PTY_ROWS=32 FYAI_PTY_COLS=100 \
    FYAI_PTY_INPUT="!sh -c 'sleep 1; stty size; sleep 3; stty size; sleep 4'" \
    FYAI_PTY_NEEDLE="bang-1" FYAI_PTY_TIMEOUT=30 \
    FYAI_PTY_AFTER="wait:Ctrl-]|wait: 98|drain:.5|resize:64x100|drain:5|raw:1d" \
    "$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/resize-$policy.out" \
        "$FYAI_BIN" -k test-key --theme dark \
        --set display/markdown=true \
        --set "display/work_zoom_rows=$policy" -m mock-model -i
    "$PYTHON" - "$TEST_DIR/resize-$policy.out" "$before" "$after" <<'PYEOF' || \
        fail "a $policy pane did not follow the terminal size"
import re
import sys

data = open(sys.argv[1], "rb").read()
sizes = [int(m[0]) for m in re.findall(rb"\b(\d+) (\d+)\b", data)]
want_before, want_after = int(sys.argv[2]), int(sys.argv[3])
if len(sizes) < 2:
    raise SystemExit("the shell reported fewer than two sizes")
if sizes[0] != want_before:
    raise SystemExit("before the resize the shell had %d rows; expected %d" %
                     (sizes[0], want_before))
if sizes[-1] != want_after:
    raise SystemExit("after the resize the shell had %d rows; expected %d" %
                     (sizes[-1], want_after))
PYEOF
}

focus_keeps_size
# The cap includes the surrounding UI and shell chrome, as the pane's own
# height policy does: half of 32 rows, then half of 64. The prompt block
# stands whether or not the tile holds the keys, so these do not change when
# focus does.
resize_recalculates half 14 30
pass
