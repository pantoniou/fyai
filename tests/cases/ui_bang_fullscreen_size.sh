#!/bin/bash
# SPDX-License-Identifier: MIT
# Verify that a full-screen bang program uses its granted surface grid.
set -eu
. "$(dirname "$0")/../harness.sh"

fyai_test_setup

FYAI_PTY_ROWS=30 FYAI_PTY_COLS=100 \
FYAI_PTY_INPUT="!sh -c 'sleep .2; stty size; printf \"ENV:%s:%s\\n\" \"\${LINES-unset}\" \"\${COLUMNS-unset}\"; sleep 8'" \
FYAI_PTY_NEEDLE=" 98" \
FYAI_PTY_AFTER="drain:.5|raw:0c|drain:1|raw:1d" \
FYAI_PTY_SNAPSHOT="$TEST_DIR/full.out" \
"$PYTHON" "$TESTS_DIR/pty_driver.py" "$TEST_DIR/pty.out" \
    "$FYAI_BIN" -k test-key --theme dark \
    --set display/markdown=true -m mock-model -i

"$PYTHON" - "$TEST_DIR/full.out" "$TESTS_DIR" <<'PYEOF' || \
    fail "bang terminal was not sized to the inner surface grid"
import re
import sys

sys.path.insert(0, sys.argv[2])
from screen import Screen

# Read the reported size from terminal screen cells.
data = open(sys.argv[1], "rb").read()
clear = b"\x1b[0m\x1b[H\x1b[2J"
if clear in data:
    data = data[data.rfind(clear):]
s = Screen(30, 100)
s.feed(data)
lines = s.lines()
sizes = [m for line in lines for m in re.findall(r"\b(\d+) (\d+)\b", line)]
if not sizes:
    raise SystemExit("the full-screen terminal did not report its size")
rows, cols = map(int, sizes[-1])
if cols != 98:
    raise SystemExit("terminal has %d columns; expected 98 after 2-column margin"
                     % cols)
# The prompt block stands whether the tile holds the keys or not, so this is
# the whole band less the fixed chrome (header, two rules, prompt, two status
# rows) and the two rows of the tile's own head.
if rows != 22:
    raise SystemExit("terminal has %d rows; expected 22 under the chrome "
                     "that does not move when focus does" %
                     rows)
if not any("ENV:unset:unset" in line for line in lines):
    raise SystemExit("LINES/COLUMNS override the live PTY size")
PYEOF

pass
